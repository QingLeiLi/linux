// SPDX-License-Identifier: GPL-2.0+
/*
 * 中文学习注释：OpenAI GPT-5 Codex，2026-07-28。
 *
 * 【文件职责】
 * 本文件实现 Tree SRCU 的更新侧、宽限期状态机和回调调度。SRCU 与普通
 * Tree RCU 的核心差异是：读侧允许睡眠，且每个 struct srcu_struct 都是
 * 一个相互独立的保护域；因此更新侧不能依赖 CPU quiescent state，而是
 * 通过两组 per-CPU lock/unlock 计数器识别调用前已存在的读者。
 *
 * 【主调用链】
 * srcu_read_lock/unlock()
 *   → __srcu_read_lock/unlock()：在当前计数槽登记读者进出
 * call_srcu()/synchronize_srcu()
 *   → srcu_gp_start_if_needed()：登记目标序号并启动宽限期
 *   → process_srcu() → srcu_advance_state()
 *   → SCAN1 清空旧槽 → srcu_flip() → SCAN2 清空另一槽
 *   → srcu_gp_end() → srcu_invoke_callbacks()。
 *
 * 【核心对象与生命周期】
 * - srcu_struct：调用者拥有的保护域入口；动态对象必须 init/cleanup 配对。
 * - srcu_usage：域级共享状态，保存 GP 序号、状态机、互斥量和工作项。
 * - srcu_data：per-CPU 计数器与分段回调队列；回调可在提交 CPU 之外执行。
 * - srcu_node：大系统上的组合树节点，把大量 CPU 的 GP 请求逐层汇聚。
 *
 * 【并发不变量】
 * 读者不被锁阻塞；更新者通过计数槽翻转、全内存屏障和两轮扫描保证只等待
 * 旧读者。srcu_usage/srcu_node/srcu_data 各有自己的锁域，GP 串行状态转换
 * 还由 srcu_gp_mutex 约束；回调发布与回收则依赖分段回调队列。SRCU 保证
 * 对象的延迟回收时机，不自动冻结受保护对象字段，字段一致性仍需调用者协议。
 *
 * 【方案权衡】
 * per-CPU 读侧计数使读路径扩展性好且可睡眠，代价是更新侧需要扫描所有
 * possible CPU、维护复杂内存序，并在大机器上按需分配组合树。SMALL 模式
 * 节省小系统内存，BIG 模式以更多节点状态换取高并发请求下的可扩展性。
 */
/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion.
 *
 * Copyright (C) IBM Corporation, 2006
 * Copyright (C) Fujitsu, 2012
 *
 * Authors: Paul McKenney <paulmck@linux.ibm.com>
 *	   Lai Jiangshan <laijs@cn.fujitsu.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		Documentation/RCU/ *.txt
 *
 */
/*
 * 本文件提供“可睡眠的读-复制-更新”互斥机制。版权与作者信息保持原样；
 * 机制背景可继续参阅 Documentation/RCU/。这里的“互斥”主要指更新者在
 * 回收旧版本前等待旧读者离开，并不表示 SRCU 读者之间互相排斥。
 */

/* 本文件所有 pr_* 日志统一加 "rcu: " 前缀，便于从启动/告警日志识别子系统。 */
#define pr_fmt(fmt) "rcu: " fmt

#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/irq_work.h>
#include <linux/rcupdate_wait.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/srcu.h>

#include "rcu.h"
#include "rcu_segcblist.h"

/* Holdoff in nanoseconds for auto-expediting. */
/*
 * 自动加速的抑制时间，单位纳秒。刚结束一次 GP 后在该窗口内不因“看似空闲”
 * 而再次主动加速，避免短时间请求抖动把正常 GP 全部推入高开销路径。
 */
#define DEFAULT_SRCU_EXP_HOLDOFF (25 * 1000)
/* 只读模块参数；启动后作为所有 SRCU 域的启发式阈值使用。 */
static ulong exp_holdoff = DEFAULT_SRCU_EXP_HOLDOFF;
module_param(exp_holdoff, ulong, 0444);

/* Overflow-check frequency.  N bits roughly says every 2**N grace periods. */
/*
 * 控制多久校正一次 per-CPU“所需 GP 序号”，以避免长时间运行后的无符号计数
 * 回绕。位掩码越稀疏，检查频率越低；它不改变每个 GP 的正确性判定。
 */
static ulong counter_wrap_check = (ULONG_MAX >> 2);
module_param(counter_wrap_check, ulong, 0444);

/*
 * Control conversion to SRCU_SIZE_BIG:
 *    0: Don't convert at all.
 *    1: Convert at init_srcu_struct() time.
 *    2: Convert when rcutorture invokes srcu_torture_stats_print().
 *    3: Decide at boot time based on system shape (default).
 * 0x1x: Convert when excessive contention encountered.
 */
/*
 * SMALL→BIG 转换策略：0 永不转换；1 初始化时转换；2 由 rcutorture 统计路径
 * 触发；3 启动时按 CPU 数决定；高半字节的 0x10 位允许锁竞争触发。转换是
 * 单向且幂等的，避免并发请求反复分配/拆除组合树。
 */
#define SRCU_SIZING_NONE	0
#define SRCU_SIZING_INIT	1
#define SRCU_SIZING_TORTURE	2
#define SRCU_SIZING_AUTO	3
#define SRCU_SIZING_CONTEND	0x10
#define SRCU_SIZING_IS(x) ((convert_to_big & ~SRCU_SIZING_CONTEND) == x)
#define SRCU_SIZING_IS_NONE() (SRCU_SIZING_IS(SRCU_SIZING_NONE))
#define SRCU_SIZING_IS_INIT() (SRCU_SIZING_IS(SRCU_SIZING_INIT))
#define SRCU_SIZING_IS_TORTURE() (SRCU_SIZING_IS(SRCU_SIZING_TORTURE))
#define SRCU_SIZING_IS_CONTEND() (convert_to_big & SRCU_SIZING_CONTEND)
/*
 * 上述 SRCU_SIZING_* 是策略编码而非运行状态：低位选择触发时机，CONTEND 位
 * 可与其组合；IS 宏屏蔽附加位后比较基础策略，避免调用点重复位运算。
 */
static int convert_to_big = SRCU_SIZING_AUTO;
module_param(convert_to_big, int, 0444);

/* Number of CPUs to trigger init_srcu_struct()-time transition to big. */
/* 自动策略下的 CPU 数阈值；只读频繁，故标为 __read_mostly。 */
static int big_cpu_lim __read_mostly = 128;
module_param(big_cpu_lim, int, 0444);

/* Contention events per jiffy to initiate transition to big. */
/* SMALL 模式在一个 jiffy 内超过此锁重试次数后请求转为 BIG。 */
static int small_contention_lim __read_mostly = 100;
module_param(small_contention_lim, int, 0444);

/* Early-boot callback-management, so early that no lock is required! */
/*
 * RCU 工作队列建立前，待启动的 srcu_usage 暂挂在此链表。此阶段只有启动 CPU
 * 且中断关闭，所以链表操作无需锁；srcu_init_done 发布后再走正常 irq_work。
 */
static LIST_HEAD(srcu_boot_list);
/* false→true 是全局一次性发布边界，之后早期链表不再接收新项。 */
static bool __read_mostly srcu_init_done;

/*
 * 前置声明按异步执行载体分组：per-CPU work 负责回调，域级 delayed_work
 * 推进 GP，irq_work 负责解开锁依赖，timer 把延迟转换为 work 投递。
 */
static void srcu_invoke_callbacks(struct work_struct *work);
static void srcu_reschedule(struct srcu_struct *ssp, unsigned long delay);
static void process_srcu(struct work_struct *work);
static void srcu_irq_work(struct irq_work *work);
static void srcu_delay_timer(struct timer_list *t);

/*
 * Initialize SRCU per-CPU data.  Note that statically allocated
 * srcu_struct structures might already have srcu_read_lock() and
 * srcu_read_unlock() running against them.  So if the is_static
 * parameter is set, don't initialize ->srcu_ctrs[].srcu_locks and
 * ->srcu_ctrs[].srcu_unlocks.
 */
/*
 * init_srcu_struct_data() - 初始化一个 SRCU 域的全部 per-CPU 运行状态
 *
 * 调用者：init_srcu_struct_fields()；@ssp 是调用者仍拥有、尚未对更新侧完全
 * 发布的 SRCU 域，且 sda 已分配。入参为借用指针，不转移所有权。
 * 返回：无直接返回值；为每个 possible CPU 初始化锁、分段回调队列、定时器、
 * work、反向 ssp 指针及 GP 需求基线。此初始化路径可睡眠与否取决于上层，
 * 本函数本身不分配内存。
 *
 * 静态 srcu_struct 的读侧计数可能在首次更新侧初始化前已经工作，因此这里只
 * 初始化管理元数据，绝不能清零编译期已接线并可能变化的读者计数。
 */
static void init_srcu_struct_data(struct srcu_struct *ssp)
{
	/*
	 * 变量地图：
	 * @cpu 遍历 possible CPU，而不只是 online CPU，因为离线 CPU 以后可上线；
	 * @sdp 是该 CPU 的借用 per-CPU 状态，只在当前循环迭代内使用。
	 */
	int cpu;
	struct srcu_data *sdp;

	/*
	 * Initialize the per-CPU srcu_data array, which feeds into the
	 * leaves of the srcu_node tree.
	 */
	/*
	 * 每个 per-CPU srcu_data 最终连接到组合树叶子。先把回调队列和执行载体
	 * 建好，再由 init_srcu_struct_nodes()（若进入 BIG）补 mynode/grpmask。
	 */
	for_each_possible_cpu(cpu) {
		sdp = per_cpu_ptr(ssp->sda, cpu);
		raw_spin_lock_init(&ACCESS_PRIVATE(sdp, lock));
		rcu_segcblist_init(&sdp->srcu_cblist);
		sdp->srcu_cblist_invoking = false;
		sdp->srcu_gp_seq_needed = ssp->srcu_sup->srcu_gp_seq;
		sdp->srcu_gp_seq_needed_exp = ssp->srcu_sup->srcu_gp_seq;
		sdp->srcu_barrier_head.next = &sdp->srcu_barrier_head;
		sdp->mynode = NULL;
		sdp->cpu = cpu;
		INIT_WORK(&sdp->work, srcu_invoke_callbacks);
		timer_setup(&sdp->delay_work, srcu_delay_timer, 0);
		sdp->ssp = ssp;
	}
}

/* Invalid seq state, used during snp node initialization */
/* 组合树节点尚未接收有效 GP 序号时的哨兵，不能与正常 rcu_seq 值混用。 */
#define SRCU_SNP_INIT_SEQ		0x2

/*
 * Check whether sequence number corresponding to snp node,
 * is invalid.
 */
/*
 * srcu_invl_snp_seq() - 判断节点序号是否仍为初始化哨兵
 * @s：纯输入序号快照，无单位、无所有权。
 * 返回：仅在等于 SRCU_SNP_INIT_SEQ 时为 true；无副作用且不可睡眠。
 */
static inline bool srcu_invl_snp_seq(unsigned long s)
{
	return s == SRCU_SNP_INIT_SEQ;
}

/*
 * Allocated and initialize SRCU combining tree.  Returns @true if
 * allocation succeeded and @false otherwise.
 */
/*
 * init_srcu_struct_nodes() - 分配并接线 BIG 模式的 SRCU 组合树
 *
 * @ssp：待扩容域的借用指针；其 srcu_usage 和 sda 已初始化。
 * @gfp_flags：树节点数组的分配上下文，决定调用者是否允许睡眠。
 * 返回 true 表示 node/level/parent/CPU 映射均建立并以 release 发布到
 * WAIT_BARRIER；false 仅表示节点数组分配失败，调用者仍拥有原 SMALL 状态。
 *
 * 本函数建立两类不变量：每个 srcu_node 知道父节点及 CPU 范围；每个 sdp
 * 知道叶节点及其中代表本 CPU 的 bit。发布前读者不得遍历半初始化树。
 */
static bool init_srcu_struct_nodes(struct srcu_struct *ssp, gfp_t gfp_flags)
{
	/* levelspread 描述各层一个父节点覆盖多少子节点；其余指针均为树内借用。 */
	int cpu;
	int i;
	int level = 0;
	int levelspread[RCU_NUM_LVLS];
	struct srcu_data *sdp;
	struct srcu_node *snp;
	struct srcu_node *snp_first;

	/* Initialize geometry if it has not already been initialized. */
	/* 全局 RCU 几何只初始化一次；随后按当前几何一次性分配连续节点数组。 */
	rcu_init_geometry();
	ssp->srcu_sup->node = kzalloc_objs(*ssp->srcu_sup->node, rcu_num_nodes,
					   gfp_flags);
	if (!ssp->srcu_sup->node)
		return false;

	/* Work out the overall tree geometry. */
	/* level[i] 指向连续数组中第 i 层首节点，不产生额外所有权。 */
	ssp->srcu_sup->level[0] = &ssp->srcu_sup->node[0];
	for (i = 1; i < rcu_num_lvls; i++)
		ssp->srcu_sup->level[i] = ssp->srcu_sup->level[i - 1] + num_rcu_lvl[i - 1];
	rcu_init_levelspread(levelspread, num_rcu_lvl);

	/* Each pass through this loop initializes one srcu_node structure. */
	/*
	 * 广度优先初始化使父层位置已可计算。回调槽先写无效序号，防止并发路径把
	 * 未发布节点误判成已记录某个 GP；grplo/grphi 的 -1 表示尚未归属 CPU。
	 */
	srcu_for_each_node_breadth_first(ssp, snp) {
		raw_spin_lock_init(&ACCESS_PRIVATE(snp, lock));
		BUILD_BUG_ON(ARRAY_SIZE(snp->srcu_have_cbs) !=
			     ARRAY_SIZE(snp->srcu_data_have_cbs));
		for (i = 0; i < ARRAY_SIZE(snp->srcu_have_cbs); i++) {
			snp->srcu_have_cbs[i] = SRCU_SNP_INIT_SEQ;
			snp->srcu_data_have_cbs[i] = 0;
		}
		snp->srcu_gp_seq_needed_exp = SRCU_SNP_INIT_SEQ;
		snp->grplo = -1;
		snp->grphi = -1;
		if (snp == &ssp->srcu_sup->node[0]) {
			/* Root node, special case. */
			/* 根没有父节点；其余公共字段已经在上方初始化。 */
			snp->srcu_parent = NULL;
			continue;
		}

		/* Non-root node. */
		/* 根据本层偏移和扇出定位父节点，形成自叶向根的漏斗路径。 */
		if (snp == ssp->srcu_sup->level[level + 1])
			level++;
		snp->srcu_parent = ssp->srcu_sup->level[level - 1] +
				   (snp - ssp->srcu_sup->level[level]) /
				   levelspread[level - 1];
	}

	/*
	 * Initialize the per-CPU srcu_data array, which feeds into the
	 * leaves of the srcu_node tree.
	 */
	/*
	 * 第二阶段把每个 possible CPU 接到叶节点，并沿祖先链扩大节点覆盖范围。
	 * grpmask 是 CPU 在叶节点局部位图中的一位，后续用于精确调度有回调的 CPU。
	 */
	level = rcu_num_lvls - 1;
	snp_first = ssp->srcu_sup->level[level];
	for_each_possible_cpu(cpu) {
		sdp = per_cpu_ptr(ssp->sda, cpu);
		sdp->mynode = &snp_first[cpu / levelspread[level]];
		for (snp = sdp->mynode; snp != NULL; snp = snp->srcu_parent) {
			if (snp->grplo < 0)
				snp->grplo = cpu;
			snp->grphi = cpu;
		}
		sdp->grpmask = 1UL << (cpu - sdp->mynode->grplo);
	}
	/*
	 * release 发布此前所有 node/sdp 写入；看到 WAIT_BARRIER 及以上状态的并发
	 * 路径用 acquire 后才可遍历树，避免观察到 NULL parent 或未初始化位图。
	 */
	smp_store_release(&ssp->srcu_sup->srcu_size_state, SRCU_SIZE_WAIT_BARRIER);
	return true;
}

/*
 * Initialize non-compile-time initialized fields, including the
 * associated srcu_node and srcu_data structures.  The is_static parameter
 * tells us that ->sda has already been wired up to srcu_data.
 */
/*
 * init_srcu_struct_fields() - 构造 SRCU 域所有不能靠静态初值完成的字段
 *
 * @ssp：输入输出对象；动态模式由调用者提供零散外壳，本函数取得 sup/sda
 * 分配责任并成功后交给 ssp；静态模式借用编译期 sup/sda，不释放它们。
 * @is_static：true 表示首次使用静态域，现有读者计数和静态锁不可重置。
 * 返回 0 表示以 release 写 srcu_gp_seq_needed 完成初始化发布；-ENOMEM 表示
 * 动态分配失败，并逆序清理本函数新取得的对象。
 *
 * 可能睡眠：动态 percpu/kzalloc 与 GFP_KERNEL 建树路径可以睡眠；若当前不可
 * 抢占而 INIT 策略要求 BIG，只标记 ALLOC，把真实分配延后到工作队列。
 */
static int init_srcu_struct_fields(struct srcu_struct *ssp, bool is_static)
{
	/* 阶段一：取得域级 sup，并初始化 GP、barrier、work 和 irq_work 状态机。 */
	if (!is_static)
		ssp->srcu_sup = kzalloc_obj(*ssp->srcu_sup);
	if (!ssp->srcu_sup)
		return -ENOMEM;
	if (!is_static)
		raw_spin_lock_init(&ACCESS_PRIVATE(ssp->srcu_sup, lock));
	ssp->srcu_sup->srcu_size_state = SRCU_SIZE_SMALL;
	ssp->srcu_sup->node = NULL;
	mutex_init(&ssp->srcu_sup->srcu_cb_mutex);
	mutex_init(&ssp->srcu_sup->srcu_gp_mutex);
	ssp->srcu_sup->srcu_gp_seq = SRCU_GP_SEQ_INITIAL_VAL;
	ssp->srcu_sup->srcu_barrier_seq = 0;
	mutex_init(&ssp->srcu_sup->srcu_barrier_mutex);
	atomic_set(&ssp->srcu_sup->srcu_barrier_cpu_cnt, 0);
	INIT_DELAYED_WORK(&ssp->srcu_sup->work, process_srcu);
	init_irq_work(&ssp->srcu_sup->irq_work, srcu_irq_work);
	ssp->srcu_sup->sda_is_static = is_static;
	if (!is_static) {
		/* 动态域同时拥有 per-CPU 数组；srcu_ctrp 初始选择计数槽 0。 */
		ssp->sda = alloc_percpu(struct srcu_data);
		ssp->srcu_ctrp = &ssp->sda->srcu_ctrs[0];
	}
	if (!ssp->sda)
		goto err_free_sup;
	/* 阶段二：初始化每 CPU 回调/计数管理元数据并记录启发式时间基线。 */
	init_srcu_struct_data(ssp);
	ssp->srcu_sup->srcu_gp_seq_needed_exp = SRCU_GP_SEQ_INITIAL_VAL;
	ssp->srcu_sup->srcu_last_gp_end = ktime_get_mono_fast_ns();
	if (READ_ONCE(ssp->srcu_sup->srcu_size_state) == SRCU_SIZE_SMALL && SRCU_SIZING_IS_INIT()) {
		/*
		 * INIT 策略尽早进入 BIG。不可抢占上下文不做可睡眠分配，只发布 ALLOC
		 * 请求；可睡眠时立即建树，失败则动态域完整回滚。
		 */
		if (!preemptible())
			WRITE_ONCE(ssp->srcu_sup->srcu_size_state, SRCU_SIZE_ALLOC);
		else if (init_srcu_struct_nodes(ssp, GFP_KERNEL))
			WRITE_ONCE(ssp->srcu_sup->srcu_size_state, SRCU_SIZE_BIG);
		else
			goto err_free_sda;
	}
	ssp->srcu_sup->srcu_ssp = ssp;
	/*
	 * 这是初始化提交点。check_init_srcu_struct() 的 acquire 读取一旦看到
	 * INITIAL_VAL 的 idle 状态，就同时看到此前所有字段，不会重复初始化。
	 */
	smp_store_release(&ssp->srcu_sup->srcu_gp_seq_needed,
			SRCU_GP_SEQ_INITIAL_VAL); /* Init done. */
	/* “初始化完成”：release 写是整个构造过程的发布点，而不只是普通赋初值。 */
	return 0;

err_free_sda:
	/* 到此 sup 与动态 sda 均已取得，但尚未发布；按取得顺序逆序撤销。 */
	if (!is_static) {
		free_percpu(ssp->sda);
		ssp->sda = NULL;
	}
err_free_sup:
	/* 静态对象由链接期所有，不在失败路径释放；动态 sup 的责任仍在本函数。 */
	if (!is_static) {
		kfree(ssp->srcu_sup);
		ssp->srcu_sup = NULL;
	}
	return -ENOMEM;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC

/*
 * __init_srcu_struct_common() - DEBUG_LOCK_ALLOC 下初始化动态域及 lockdep 身份
 * @ssp：输入输出动态域；@name/@key：lockdep 类名与稳定类键，均为借用。
 * 返回 init_srcu_struct_fields() 的 0/-ENOMEM；失败时不留下动态分配。
 * 初始化可能睡眠，且调用前不得仍持有该对象范围内的旧锁实例。
 */
static int
__init_srcu_struct_common(struct srcu_struct *ssp, const char *name, struct lock_class_key *key)
{
	/* Don't re-initialize a lock while it is held. */
	/* 先让 lockdep 检查旧对象没有活跃锁，再把此域登记成独立锁类。 */
	debug_check_no_locks_freed((void *)ssp, sizeof(*ssp));
	lockdep_init_map(&ssp->dep_map, name, key, 0);
	return init_srcu_struct_fields(ssp, false);
}

/*
 * __init_srcu_struct() - 初始化普通读者风格的动态 SRCU 域
 * @ssp/@name/@key 语义同 common；返回 0 或 -ENOMEM，成功后可进入普通读侧。
 */
int __init_srcu_struct(struct srcu_struct *ssp, const char *name, struct lock_class_key *key)
{
	ssp->srcu_reader_flavor = 0;
	return __init_srcu_struct_common(ssp, name, key);
}
EXPORT_SYMBOL_GPL(__init_srcu_struct);

/*
 * __init_srcu_struct_fast() - 初始化 FAST 读者专用动态域
 * 参数、上下文、返回和 ownership 同 __init_srcu_struct()；额外固定 reader
 * flavor，后续混用普通/NMI 等风格会由调试检查告警。
 */
int __init_srcu_struct_fast(struct srcu_struct *ssp, const char *name, struct lock_class_key *key)
{
	ssp->srcu_reader_flavor = SRCU_READ_FLAVOR_FAST;
	return __init_srcu_struct_common(ssp, name, key);
}
EXPORT_SYMBOL_GPL(__init_srcu_struct_fast);

/*
 * __init_srcu_struct_fast_updown() - 初始化 FAST_UPDOWN 专用动态域
 * @ssp/@name/@key 均为输入借用；成功返回 0，失败 -ENOMEM；不产生额外出参。
 */
int __init_srcu_struct_fast_updown(struct srcu_struct *ssp, const char *name,
				   struct lock_class_key *key)
{
	ssp->srcu_reader_flavor = SRCU_READ_FLAVOR_FAST_UPDOWN;
	return __init_srcu_struct_common(ssp, name, key);
}
EXPORT_SYMBOL_GPL(__init_srcu_struct_fast_updown);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */
/* 未启用 DEBUG_LOCK_ALLOC 时省略显式 name/key，但初始化及 ownership 契约不变。 */

/**
 * init_srcu_struct - initialize a sleep-RCU structure
 * @ssp: structure to initialize.
 *
 * Use this in place of DEFINE_SRCU() and DEFINE_STATIC_SRCU()
 * for non-static srcu_struct structures that are to be passed to
 * srcu_read_lock(), srcu_read_lock_nmisafe(), and friends.  It is necessary
 * to invoke this on a given srcu_struct before passing that srcu_struct
 * to any other function.  Each srcu_struct represents a separate domain
 * of SRCU protection.
 */
/*
 * init_srcu_struct() - 初始化普通可睡眠读者使用的独立动态 SRCU 域
 * @ssp：输入输出、非空、调用者拥有；成功后必须以 cleanup_srcu_struct() 配对。
 * 返回 0 或 -ENOMEM。可睡眠；调用前该域不可被任何其他 SRCU API 使用。
 */
int init_srcu_struct(struct srcu_struct *ssp)
{
	ssp->srcu_reader_flavor = 0;
	return init_srcu_struct_fields(ssp, false);
}
EXPORT_SYMBOL_GPL(init_srcu_struct);

/**
 * init_srcu_struct_fast - initialize a fast-reader sleep-RCU structure
 * @ssp: structure to initialize.
 *
 * Use this in place of DEFINE_SRCU_FAST() and DEFINE_STATIC_SRCU_FAST()
 * for non-static srcu_struct structures that are to be passed to
 * srcu_read_lock_fast() and friends.  It is necessary to invoke this on a
 * given srcu_struct before passing that srcu_struct to any other function.
 * Each srcu_struct represents a separate domain of SRCU protection.
 */
/*
 * init_srcu_struct_fast() - 初始化仅供 fast-reader API 使用的动态域
 * @ssp ownership、睡眠上下文和 0/-ENOMEM 返回契约同 init_srcu_struct()；
 * 成功副作用是固定 FAST flavor，禁止与其他读侧风格混用。
 */
int init_srcu_struct_fast(struct srcu_struct *ssp)
{
	ssp->srcu_reader_flavor = SRCU_READ_FLAVOR_FAST;
	return init_srcu_struct_fields(ssp, false);
}
EXPORT_SYMBOL_GPL(init_srcu_struct_fast);

/**
 * init_srcu_struct_fast_updown - initialize a fast-reader up/down sleep-RCU structure
 * @ssp: structure to initialize.
 *
 * Use this function in place of DEFINE_SRCU_FAST_UPDOWN() and
 * DEFINE_STATIC_SRCU_FAST_UPDOWN() for non-static srcu_struct
 * structures that are to be passed to srcu_read_lock_fast_updown(),
 * srcu_down_read_fast(), and friends.  It is necessary to invoke this on a
 * given srcu_struct before passing that srcu_struct to any other function.
 * Each srcu_struct represents a separate domain of SRCU protection.
 */
/*
 * init_srcu_struct_fast_updown() - 初始化 fast-up/down API 使用的动态域
 * @ssp 为调用者拥有的输入输出对象；成功返回 0 并需 cleanup 配对，失败返回
 * -ENOMEM 且不转移任何分配所有权。
 */
int init_srcu_struct_fast_updown(struct srcu_struct *ssp)
{
	ssp->srcu_reader_flavor = SRCU_READ_FLAVOR_FAST_UPDOWN;
	return init_srcu_struct_fields(ssp, false);
}
EXPORT_SYMBOL_GPL(init_srcu_struct_fast_updown);

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/*
 * Initiate a transition to SRCU_SIZE_BIG with lock held.
 */
/*
 * __srcu_transition_to_big() - 在域锁内提交 SMALL→ALLOC 转换请求
 * @ssp：借用 SRCU 域；调用者必须持有 sup->lock，函数用 lockdep 验证。
 * 返回：无直接返回值；release 写 size_state，使后续 acquire 读者看到转换前
 * 的全部状态。只改变状态，不在自旋锁内分配内存，因此不可睡眠。
 */
static void __srcu_transition_to_big(struct srcu_struct *ssp)
{
	lockdep_assert_held(&ACCESS_PRIVATE(ssp->srcu_sup, lock));
	/* release 与遍历者的 size_state acquire 配对，发布 ALLOC 前的域状态。 */
	smp_store_release(&ssp->srcu_sup->srcu_size_state, SRCU_SIZE_ALLOC);
}

/*
 * Initiate an idempotent transition to SRCU_SIZE_BIG.
 */
/*
 * srcu_transition_to_big() - 无论多少并发请求都只提交一次扩容
 * @ssp 为借用输入输出域；返回无值，已转换/正在转换时无副作用。
 * 先无锁 acquire 快查，再以 sup->lock 二次确认，避免常见已转换路径争锁；
 * 真正分配由 GP 完成路径在可睡眠上下文执行。
 */
static void srcu_transition_to_big(struct srcu_struct *ssp)
{
	unsigned long flags;

	/* Double-checked locking on ->srcu_size-state. */
	/* 第一次检查优化 fast path，锁内第二次检查负责并发正确性。 */
	/* acquire 取得转换者在 release 状态写之前完成的所有初始化。 */
	if (smp_load_acquire(&ssp->srcu_sup->srcu_size_state) != SRCU_SIZE_SMALL)
		return;
	raw_spin_lock_irqsave_rcu_node(ssp->srcu_sup, flags);
	/* 锁内 acquire 二次确认并取得可能由竞争者刚发布的转换状态。 */
	if (smp_load_acquire(&ssp->srcu_sup->srcu_size_state) != SRCU_SIZE_SMALL) {
		raw_spin_unlock_irqrestore_rcu_node(ssp->srcu_sup, flags);
		return;
	}
	__srcu_transition_to_big(ssp);
	raw_spin_unlock_irqrestore_rcu_node(ssp->srcu_sup, flags);
}

/*
 * Check to see if the just-encountered contention event justifies
 * a transition to SRCU_SIZE_BIG.
 */
/*
 * raw_spin_lock_irqsave_check_contention() - 在 sup->lock 内累计 SMALL 竞争
 * @ssp：借用域；入口必须已经持有 sup->lock 且中断状态由外层保存。
 * 每个 jiffy 重新计数，超过阈值就请求 ALLOC。返回无值，不睡眠；策略未允许
 * 或状态已离开 SMALL 时立即返回。
 */
static void raw_spin_lock_irqsave_check_contention(struct srcu_struct *ssp)
{
	unsigned long j;

	if (!SRCU_SIZING_IS_CONTEND() || ssp->srcu_sup->srcu_size_state)
		return;
	j = jiffies;
	if (ssp->srcu_sup->srcu_size_jiffies != j) {
		ssp->srcu_sup->srcu_size_jiffies = j;
		ssp->srcu_sup->srcu_n_lock_retries = 0;
	}
	if (++ssp->srcu_sup->srcu_n_lock_retries <= small_contention_lim)
		return;
	__srcu_transition_to_big(ssp);
}

/*
 * Acquire the specified srcu_data structure's ->lock, but check for
 * excessive contention, which results in initiation of a transition
 * to SRCU_SIZE_BIG.  But only if the srcutree.convert_to_big module
 * parameter permits this.
 */
/*
 * raw_spin_lock_irqsave_sdp_contention() - 获取 per-CPU 队列锁并采样竞争
 * @sdp：借用 per-CPU 状态；@flags：输出原中断标志，调用者解锁时原样恢复。
 * 返回时始终持有 sdp->lock。trylock 失败时短暂取得 sup->lock 记录竞争，再
 * 获取目标锁；不睡眠。锁次序不同时持有 sup 与 sdp，避免形成嵌套环。
 */
static void raw_spin_lock_irqsave_sdp_contention(struct srcu_data *sdp, unsigned long *flags)
{
	struct srcu_struct *ssp = sdp->ssp;

	if (raw_spin_trylock_irqsave_rcu_node(sdp, *flags))
		return;
	raw_spin_lock_irqsave_rcu_node(ssp->srcu_sup, *flags);
	raw_spin_lock_irqsave_check_contention(ssp);
	raw_spin_unlock_irqrestore_rcu_node(ssp->srcu_sup, *flags);
	raw_spin_lock_irqsave_rcu_node(sdp, *flags);
}

/*
 * Acquire the specified srcu_struct structure's ->lock, but check for
 * excessive contention, which results in initiation of a transition
 * to SRCU_SIZE_BIG.  But only if the srcutree.convert_to_big module
 * parameter permits this.
 */
/*
 * raw_spin_lock_irqsave_ssp_contention() - 获取域锁并在慢路径统计竞争
 * @ssp：借用域；@flags：输出 IRQ 保存值。返回时持有 sup->lock；调用者必须
 * 用配对 irqrestore 解锁。trylock 成功不增加统计，失败路径在取得锁后判断扩容。
 */
static void raw_spin_lock_irqsave_ssp_contention(struct srcu_struct *ssp, unsigned long *flags)
{
	if (raw_spin_trylock_irqsave_rcu_node(ssp->srcu_sup, *flags))
		return;
	raw_spin_lock_irqsave_rcu_node(ssp->srcu_sup, *flags);
	raw_spin_lock_irqsave_check_contention(ssp);
}

/*
 * First-use initialization of statically allocated srcu_struct
 * structure.  Wiring up the combining tree is more than can be
 * done with compile-time initialization, so this check is added
 * to each update-side SRCU primitive.  Use ssp->lock, which -is-
 * compile-time initialized, to resolve races involving multiple
 * CPUs trying to garner first-use privileges.
 */
/*
 * check_init_srcu_struct() - 静态 SRCU 域更新侧首次使用的并发一次性初始化
 * @ssp：借用、可能只有编译期初值的静态域。返回无值；成功后所有运行字段可见。
 * 快路径 acquire 与 init 的 release 配对；慢路径用编译期已初始化的 sup->lock
 * 选出唯一初始化者。动态内存分配可能睡眠的场景不应在不允许睡眠的首次使用
 * 上触发 INIT 直接建树策略，代码会在不可抢占时延后建树。
 */
static void check_init_srcu_struct(struct srcu_struct *ssp)
{
	unsigned long flags;

	/* The smp_load_acquire() pairs with the smp_store_release(). */
	/* acquire 既判断“已初始化”，也取得初始化者发布的 sda/work/mutex 等字段。 */
	if (!rcu_seq_state(smp_load_acquire(&ssp->srcu_sup->srcu_gp_seq_needed))) /*^^^*/
		return; /* Already initialized. */
	/* 已初始化 fast path 不再取得锁；上方 acquire 已保证字段可见。 */
	raw_spin_lock_irqsave_rcu_node(ssp->srcu_sup, flags);
	if (!rcu_seq_state(ssp->srcu_sup->srcu_gp_seq_needed)) {
		raw_spin_unlock_irqrestore_rcu_node(ssp->srcu_sup, flags);
		return;
	}
	init_srcu_struct_fields(ssp, true);
	raw_spin_unlock_irqrestore_rcu_node(ssp->srcu_sup, flags);
}

/*
 * Is the current or any upcoming grace period to be expedited?
 */
/*
 * srcu_gp_is_expedited() - 判断当前请求水位是否包含尚未完成的加速 GP
 * @ssp：借用域；返回近似时刻的布尔快照，无锁读取允许值随后变化。
 * 比较使用可回绕无符号序号规则；无副作用、不可睡眠。
 */
static bool srcu_gp_is_expedited(struct srcu_struct *ssp)
{
	struct srcu_usage *sup = ssp->srcu_sup;

	return ULONG_CMP_LT(READ_ONCE(sup->srcu_gp_seq), READ_ONCE(sup->srcu_gp_seq_needed_exp));
}

/*
 * Computes approximate total of the readers' ->srcu_ctrs[].srcu_locks
 * values for the rank of per-CPU counters specified by idx, and returns
 * true if the caller did the proper barrier (gp), and if the count of
 * the locks matches that of the unlocks passed in.
 */
/*
 * srcu_readers_lock_idx() - 汇总指定槽的进入计数并与退出快照比较
 * @ssp：借用域；@idx：0/1 计数槽；@gp：调用者是否已执行普通 RCU GP；
 * @unlocks：此前汇总的退出计数。返回 true 仅表示在屏障协议下两者相等。
 *
 * possible CPU 全扫描是无锁近似快照，正确性来自调用者的屏障与重复扫描，而
 * 不是单次原子快照。rd flavor 掩码用于发现同一域混用不兼容读侧 API；
 * SLOWGP 读者要求真正经过普通 RCU GP，不能只靠本地 smp_mb。
 */
static bool srcu_readers_lock_idx(struct srcu_struct *ssp, int idx, bool gp, unsigned long unlocks)
{
	/* sum 是所有 CPU 的累计进入次数；mask 汇总观察到的 reader flavor 位。 */
	int cpu;
	unsigned long mask = 0;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct srcu_data *sdp = per_cpu_ptr(ssp->sda, cpu);

		sum += atomic_long_read(&sdp->srcu_ctrs[idx].srcu_locks);
		if (IS_ENABLED(CONFIG_PROVE_RCU))
			mask = mask | READ_ONCE(sdp->srcu_reader_flavor);
	}
	WARN_ONCE(IS_ENABLED(CONFIG_PROVE_RCU) && (mask & (mask - 1)),
		  "Mixed reader flavors for srcu_struct at %ps.\n", ssp);
	if (mask & SRCU_READ_FLAVOR_SLOWGP && !gp)
		return false;
	return sum == unlocks;
}

/*
 * Returns approximate total of the readers' ->srcu_ctrs[].srcu_unlocks
 * values for the rank of per-CPU counters specified by idx.
 */
/*
 * srcu_readers_unlock_idx() - 汇总指定槽的退出次数及读者风格
 * @ssp/@idx 为输入借用与 0/1 槽；@rdm 为必填输出位图，调用前内容被覆盖。
 * 返回累计退出计数。无锁扫描允许并发增量，因此结果只在后续屏障+进入扫描
 * 协议中有意义；函数本身不睡眠、不转移所有权。
 */
static unsigned long srcu_readers_unlock_idx(struct srcu_struct *ssp, int idx, unsigned long *rdm)
{
	int cpu;
	unsigned long mask = ssp->srcu_reader_flavor;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct srcu_data *sdp = per_cpu_ptr(ssp->sda, cpu);

		sum += atomic_long_read(&sdp->srcu_ctrs[idx].srcu_unlocks);
		mask = mask | READ_ONCE(sdp->srcu_reader_flavor);
	}
	WARN_ONCE(IS_ENABLED(CONFIG_PROVE_RCU) && (mask & (mask - 1)),
		  "Mixed reader flavors for srcu_struct at %ps.\n", ssp);
	*rdm = mask;
	return sum;
}

/*
 * Return true if the number of pre-existing readers is determined to
 * be zero.
 */
/*
 * srcu_readers_active_idx_check() - 判定某槽旧读者是否曾经全部退出
 * @ssp：借用域；@idx：待检查的旧槽。返回 true 表示屏障两侧的 unlock/lock
 * 汇总相等，因而本次扫描区间内存在“零旧读者”时刻；false 需以后重试。
 * SLOWGP flavor 会执行 synchronize_rcu[_expedited]()，因此本函数可能睡眠；
 * 普通 flavor 只执行全屏障。
 */
static bool srcu_readers_active_idx_check(struct srcu_struct *ssp, int idx)
{
	bool did_gp;
	unsigned long rdm;
	unsigned long unlocks;

	unlocks = srcu_readers_unlock_idx(ssp, idx, &rdm);
	did_gp = !!(rdm & SRCU_READ_FLAVOR_SLOWGP);

	/*
	 * Make sure that a lock is always counted if the corresponding
	 * unlock is counted. Needs to be a smp_mb() as the read side may
	 * contain a read from a variable that is written to before the
	 * synchronize_srcu() in the write side. In this case smp_mb()s
	 * A and B (or X and Y) act like the store buffering pattern.
	 *
	 * This smp_mb() also pairs with smp_mb() C (or, in the case of X,
	 * Z) to prevent accesses after the synchronize_srcu() from being
	 * executed before the grace period ends.
	 */
	/*
	 * 先读 unlock，再执行 A/X：保证随后读到某个 lock 时，对应 unlock 不会被
	 * 错排到扫描之前，同时与读侧 B/C 及后续屏障构成 store-buffering 禁止环。
	 * _lite/SLOWGP 读者缺少所需本地屏障时，用一次 Tree RCU GP 补足保证。
	 */
	if (!did_gp)
		smp_mb(); /* A */
	/* A：普通 flavor 的本地全屏障，配对读侧 B/C 与后续扫描。 */
	else if (srcu_gp_is_expedited(ssp))
		synchronize_rcu_expedited(); /* X */
	/* X：SLOWGP 且当前需加速时，用 expedited Tree RCU GP 补足跨 CPU 屏障。 */
	else
		synchronize_rcu(); /* X */
	/* X：SLOWGP 普通路径完成一次 Tree RCU GP，不能以单 CPU 屏障替代。 */

	/*
	 * If the locks are the same as the unlocks, then there must have
	 * been no readers on this index at some point in this function.
	 * But there might be more readers, as a task might have read
	 * the current ->srcu_ctrp but not yet have incremented its CPU's
	 * ->srcu_ctrs[idx].srcu_locks counter.  In fact, it is possible
	 * that most of the tasks have been preempted between fetching
	 * ->srcu_ctrp and incrementing ->srcu_ctrs[idx].srcu_locks.  And
	 * there could be almost (ULONG_MAX / sizeof(struct task_struct))
	 * tasks in a system whose address space was fully populated
	 * with memory.  Call this quantity Nt.
	 *
	 * So suppose that the updater is preempted at this
	 * point in the code for a long time.  That now-preempted
	 * updater has already flipped ->srcu_ctrp (possibly during
	 * the preceding grace period), done an smp_mb() (again,
	 * possibly during the preceding grace period), and summed up
	 * the ->srcu_ctrs[idx].srcu_unlocks counters.  How many times
	 * can a given one of the aforementioned Nt tasks increment the
	 * old ->srcu_ctrp value's ->srcu_ctrs[idx].srcu_locks counter,
	 * in the absence of nesting?
	 *
	 * It can clearly do so once, given that it has already fetched
	 * the old value of ->srcu_ctrp and is just about to use that
	 * value to index its increment of ->srcu_ctrs[idx].srcu_locks.
	 * But as soon as it leaves that SRCU read-side critical section,
	 * it will increment ->srcu_ctrs[idx].srcu_unlocks, which must
	 * follow the updater's above read from that same value.  Thus,
	   as soon the reading task does an smp_mb() and a later fetch from
	 * ->srcu_ctrp, that task will be guaranteed to get the new index.
	 * Except that the increment of ->srcu_ctrs[idx].srcu_unlocks
	 * in __srcu_read_unlock() is after the smp_mb(), and the fetch
	 * from ->srcu_ctrp in __srcu_read_lock() is before the smp_mb().
	 * Thus, that task might not see the new value of ->srcu_ctrp until
	 * the -second- __srcu_read_lock(), which in turn means that this
	 * task might well increment ->srcu_ctrs[idx].srcu_locks for the
	 * old value of ->srcu_ctrp twice, not just once.
	 *
	 * However, it is important to note that a given smp_mb() takes
	 * effect not just for the task executing it, but also for any
	 * later task running on that same CPU.
	 *
	 * That is, there can be almost Nt + Nc further increments
	 * of ->srcu_ctrs[idx].srcu_locks for the old index, where Nc
	 * is the number of CPUs.  But this is OK because the size of
	 * the task_struct structure limits the value of Nt and current
	 * systems limit Nc to a few thousand.
	 *
	 * OK, but what about nesting?  This does impose a limit on
	 * nesting of half of the size of the task_struct structure
	 * (measured in bytes), which should be sufficient.  A late 2022
	 * TREE01 rcutorture run reported this size to be no less than
	 * 9408 bytes, allowing up to 4704 levels of nesting, which is
	 * comfortably beyond excessive.  Especially on 64-bit systems,
	 * which are unlikely to be configured with an address space fully
	 * populated with memory, at least not anytime soon.
	 */
	/*
	 * 上述长注释解释了为什么“计数相等”仍允许少量已经取到旧 ctrp、尚未递增
	 * lock 的迟到读者：每个任务至多带来受 task 数与 CPU 数约束的有限迟到
	 * 增量，且下一轮屏障/翻转会把它们收敛。嵌套深度上界远低于计数回绕空间，
	 * 所以无符号累计仍可安全用于这一相等性协议。
	 */
	return srcu_readers_lock_idx(ssp, idx, did_gp, unlocks);
}

/**
 * srcu_readers_active - returns true if there are readers. and false
 *                       otherwise
 * @ssp: which srcu_struct to count active readers (holding srcu_read_lock).
 *
 * Note that this is not an atomic primitive, and can therefore suffer
 * severe errors when invoked on an active srcu_struct.  That said, it
 * can be useful as an error check at cleanup time.
 */
/*
 * srcu_readers_active() - 清理阶段粗略检测两个槽是否仍有读者
 * @ssp：借用域。返回 lock 总和减 unlock 总和，非零表示观察到活动读者。
 * 这不是同步原语：并发读者会令跨 CPU 多次读取互不一致，只适合 teardown
 * 告警，不能据此安全释放一个仍在使用的域；无所有权变化且不可睡眠。
 */
static bool srcu_readers_active(struct srcu_struct *ssp)
{
	int cpu;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct srcu_data *sdp = per_cpu_ptr(ssp->sda, cpu);

		sum += atomic_long_read(&sdp->srcu_ctrs[0].srcu_locks);
		sum += atomic_long_read(&sdp->srcu_ctrs[1].srcu_locks);
		sum -= atomic_long_read(&sdp->srcu_ctrs[0].srcu_unlocks);
		sum -= atomic_long_read(&sdp->srcu_ctrs[1].srcu_unlocks);
	}
	return sum;
}

/*
 * We use an adaptive strategy for synchronize_srcu() and especially for
 * synchronize_srcu_expedited().  We spin for a fixed time period
 * (defined below, boot time configurable) to allow SRCU readers to exit
 * their read-side critical sections.  If there are still some readers
 * after one jiffy, we repeatedly block for one jiffy time periods.
 * The blocking time is increased as the grace-period age increases,
 * with max blocking time capped at 10 jiffies.
 */
/*
 * synchronize 路径采用自适应等待：先以微秒级短轮询争取低延迟；若一个
 * jiffy 后仍有慢读者，则转为工作队列延时并随 GP 年龄增加间隔，最大 10
 * jiffies。这样在短读侧上减少调度延迟，同时避免长读侧造成忙等占满 CPU。
 */
#define SRCU_DEFAULT_RETRY_CHECK_DELAY		5

static ulong srcu_retry_check_delay = SRCU_DEFAULT_RETRY_CHECK_DELAY;
module_param(srcu_retry_check_delay, ulong, 0444);
/* 读者计数短轮询的单次间隔，单位微秒；运行期只读。 */

#define SRCU_INTERVAL		1		// Base delay if no expedited GPs pending.
#define SRCU_MAX_INTERVAL	10		// Maximum incremental delay from slow readers.
/* 普通 GP 基础延迟为 1 jiffy；慢读者导致的递增延迟最多 10 jiffies。 */

#define SRCU_DEFAULT_MAX_NODELAY_PHASE_LO	3UL	// Lowmark on default per-GP-phase
							// no-delay instances.
#define SRCU_DEFAULT_MAX_NODELAY_PHASE_HI	1000UL	// Highmark on default per-GP-phase
							// no-delay instances.
/* 每个 GP 阶段连续零延迟调度次数的下/上界，限制加速模式的 CPU 占用。 */

#define SRCU_UL_CLAMP_LO(val, low)	((val) > (low) ? (val) : (low))
#define SRCU_UL_CLAMP_HI(val, high)	((val) < (high) ? (val) : (high))
#define SRCU_UL_CLAMP(val, low, high)	SRCU_UL_CLAMP_HI(SRCU_UL_CLAMP_LO((val), (low)), (high))
/* 三个纯算术宏依次执行下限、上限和双边夹取，用于约束自动推导的调度预算。 */
// per-GP-phase no-delay instances adjusted to allow non-sleeping poll upto
// one jiffies time duration. Mult by 2 is done to factor in the srcu_get_delay()
// called from process_srcu().
/*
 * 按 HZ 和微秒轮询间隔推导：每个阶段允许无睡眠轮询约一个 jiffy；乘二是因为
 * process_srcu() 一轮内会两次计算延迟。随后再夹在上述经验上下界之间。
 */
#define SRCU_DEFAULT_MAX_NODELAY_PHASE_ADJUSTED	\
	(2UL * USEC_PER_SEC / HZ / SRCU_DEFAULT_RETRY_CHECK_DELAY)

// Maximum per-GP-phase consecutive no-delay instances.
/* 单个 SCAN 阶段允许的最大连续零延迟次数。 */
#define SRCU_DEFAULT_MAX_NODELAY_PHASE	\
	SRCU_UL_CLAMP(SRCU_DEFAULT_MAX_NODELAY_PHASE_ADJUSTED,	\
		      SRCU_DEFAULT_MAX_NODELAY_PHASE_LO,	\
		      SRCU_DEFAULT_MAX_NODELAY_PHASE_HI)

static ulong srcu_max_nodelay_phase = SRCU_DEFAULT_MAX_NODELAY_PHASE;
module_param(srcu_max_nodelay_phase, ulong, 0444);
/* 单个 SCAN 阶段的立即重排预算，启动后只读。 */

// Maximum consecutive no-delay instances.
/* 跨阶段总零延迟次数至少 100，防止频繁阶段切换绕过节流。 */
#define SRCU_DEFAULT_MAX_NODELAY	(SRCU_DEFAULT_MAX_NODELAY_PHASE > 100 ?	\
					 SRCU_DEFAULT_MAX_NODELAY_PHASE : 100)

static ulong srcu_max_nodelay = SRCU_DEFAULT_MAX_NODELAY;
module_param(srcu_max_nodelay, ulong, 0444);
/* 整个连续处理过程的立即重排预算，防止跨阶段累计占满 CPU。 */

/*
 * Return grace-period delay, zero if there are expedited grace
 * periods pending, SRCU_INTERVAL otherwise.
 */
/*
 * srcu_get_delay() - 在域锁内计算下一次 GP 工作的延迟 jiffy 数
 * @ssp：借用域；入口必须持有 sup->lock。返回 0 表示立即重排，1..10 表示
 * 延迟 jiffy 数。会更新加速阶段零延迟计数，无内存分配且不可睡眠。
 * GP 越老，普通路径退避越长；加速路径先给零延迟预算，耗尽后至少让出 1 jiffy。
 */
static unsigned long srcu_get_delay(struct srcu_struct *ssp)
{
	/* gpstart 是当前 GP 的 jiffies 起点；jbase 最终被夹到 SRCU_MAX_INTERVAL。 */
	unsigned long gpstart;
	unsigned long j;
	unsigned long jbase = SRCU_INTERVAL;
	struct srcu_usage *sup = ssp->srcu_sup;

	lockdep_assert_held(&ACCESS_PRIVATE(ssp->srcu_sup, lock));
	if (srcu_gp_is_expedited(ssp))
		jbase = 0;
	if (rcu_seq_state(READ_ONCE(sup->srcu_gp_seq))) {
		j = jiffies - 1;
		gpstart = READ_ONCE(sup->srcu_gp_start);
		if (time_after(j, gpstart))
			jbase += j - gpstart;
		if (!jbase) {
			ASSERT_EXCLUSIVE_WRITER(sup->srcu_n_exp_nodelay);
			WRITE_ONCE(sup->srcu_n_exp_nodelay, READ_ONCE(sup->srcu_n_exp_nodelay) + 1);
			if (READ_ONCE(sup->srcu_n_exp_nodelay) > srcu_max_nodelay_phase)
				jbase = 1;
		}
	}
	return jbase > SRCU_MAX_INTERVAL ? SRCU_MAX_INTERVAL : jbase;
}

/**
 * cleanup_srcu_struct - deconstruct a sleep-RCU structure
 * @ssp: structure to clean up.
 *
 * Must invoke this after you are finished using a given srcu_struct that
 * was initialized via init_srcu_struct(), else you leak memory.
 */
/*
 * cleanup_srcu_struct() - 停止并释放动态 SRCU 域的运行资源
 * @ssp：调用者独占的输入输出域；必须先停止新读者、call_srcu 和 poll 请求，
 * 并先调用 srcu_barrier() 等完所有回调。返回无值。
 *
 * 本函数可睡眠：同步 irq_work、timer、work。任何活动迹象都选择 WARN 后泄漏，
 * 而不是冒险释放造成 UAF。静态域只拆动态组合树和运行状态，不释放链接期 sda；
 * 动态域最终清空 sda/sup 指针，把 ownership 归还分配器。
 */
void cleanup_srcu_struct(struct srcu_struct *ssp)
{
	/* delay 必须非零：零表示仍有 expedited 工作，清理会与立即重排竞争。 */
	int cpu;
	unsigned long delay;
	struct srcu_usage *sup = ssp->srcu_sup;

	raw_spin_lock_irq_rcu_node(ssp->srcu_sup);
	delay = srcu_get_delay(ssp);
	raw_spin_unlock_irq_rcu_node(ssp->srcu_sup);
	if (WARN_ON(!delay))
		return; /* Just leak it! */
	/* 仍需零延迟推进时“直接泄漏”，避免与飞行中的工作竞争释放。 */
	if (WARN_ON(srcu_readers_active(ssp)))
		return; /* Just leak it! */
	/* 仍有读者时同样选择泄漏；告警后释放会把诊断错误升级成 UAF。 */
	/* Wait for irq_work to finish first as it may queue a new work. */
	/*
	 * 必须先同步 irq_work，因为它能新建 delayed_work；再清域级 work，最后逐
	 * CPU 删除 timer/flush callback work，按“生产者→消费者”次序封住新工作。
	 */
	irq_work_sync(&sup->irq_work);
	flush_delayed_work(&sup->work);
	for_each_possible_cpu(cpu) {
		struct srcu_data *sdp = per_cpu_ptr(ssp->sda, cpu);

		timer_delete_sync(&sdp->delay_work);
		flush_work(&sdp->work);
		if (WARN_ON(rcu_segcblist_n_cbs(&sdp->srcu_cblist)))
			return; /* Forgot srcu_barrier(), so just leak it! */
		/* 队列非空说明调用者漏了 barrier；宁可保留整个域也不能丢回调或 UAF。 */
	}
	if (WARN_ON(rcu_seq_state(READ_ONCE(sup->srcu_gp_seq)) != SRCU_STATE_IDLE) ||
	    WARN_ON(rcu_seq_current(&sup->srcu_gp_seq) != sup->srcu_gp_seq_needed) ||
	    WARN_ON(srcu_readers_active(ssp))) {
		pr_info("%s: Active srcu_struct %p read state: %d gp state: %lu/%lu\n",
			__func__, ssp, rcu_seq_state(READ_ONCE(sup->srcu_gp_seq)),
			rcu_seq_current(&sup->srcu_gp_seq), sup->srcu_gp_seq_needed);
		return; // Caller forgot to stop doing call_srcu()?
			// Or caller invoked start_poll_synchronize_srcu()
			// and then cleanup_srcu_struct() before that grace
			// period ended?
	}
	/*
	 * 这里的原注释提出两个可能原因：调用者仍在 call_srcu，或 poll 启动的 GP
	 * 尚未结束。两者都破坏 teardown 前“无新请求且 GP idle”的前置条件。
	 */
	/* 所有异步活动已停止，先释放可选树，再按静态/动态 ownership 拆 sda/sup。 */
	kfree(sup->node);
	sup->node = NULL;
	sup->srcu_size_state = SRCU_SIZE_SMALL;
	if (!sup->sda_is_static) {
		free_percpu(ssp->sda);
		ssp->sda = NULL;
		kfree(sup);
		ssp->srcu_sup = NULL;
	}
}
EXPORT_SYMBOL_GPL(cleanup_srcu_struct);

/*
 * Check for consistent reader flavor.
 */
/*
 * __srcu_check_read_flavor() - 调试检查一个域/CPU 是否混用读侧风格
 * @ssp：借用域；@read_flavor：单一 flavor 位，不能是多 bit 组合。
 * 返回无值；首次使用以 cmpxchg 原子记录本 CPU 风格，冲突只 WARN 不修复。
 * 可在读侧调用，不睡眠；raw_cpu_ptr 要求外层读侧协议保证 CPU 选择有效。
 */
void __srcu_check_read_flavor(struct srcu_struct *ssp, int read_flavor)
{
	int old_read_flavor;
	struct srcu_data *sdp;

	/* NMI-unsafe use in NMI is a bad sign, as is multi-bit read_flavor values. */
	/* NMI 中只能使用 NMI-safe 风格；位图必须恰有一个策略位，混合值无契约。 */
	WARN_ON_ONCE(read_flavor != SRCU_READ_FLAVOR_NMI &&
		     read_flavor != SRCU_READ_FLAVOR_FAST && in_nmi());
	WARN_ON_ONCE(read_flavor & (read_flavor - 1));

	sdp = raw_cpu_ptr(ssp->sda);
	old_read_flavor = READ_ONCE(sdp->srcu_reader_flavor);
	WARN_ON_ONCE(ssp->srcu_reader_flavor && read_flavor != ssp->srcu_reader_flavor);
	WARN_ON_ONCE(old_read_flavor && ssp->srcu_reader_flavor &&
		     old_read_flavor != ssp->srcu_reader_flavor);
	WARN_ON_ONCE(read_flavor == SRCU_READ_FLAVOR_FAST && !ssp->srcu_reader_flavor);
	if (!old_read_flavor) {
		/*
		 * 多个上下文可能首次记录同一 CPU；cmpxchg 只允许一个把 0 改为当前
		 * flavor，失败者拿到胜者值并在下方核对，防止静默混用。
		 */
		old_read_flavor = cmpxchg(&sdp->srcu_reader_flavor, 0, read_flavor);
		if (!old_read_flavor)
			return;
	}
	WARN_ONCE(old_read_flavor != read_flavor, "CPU %d old state %d new state %d\n", sdp->cpu, old_read_flavor, read_flavor);
}
EXPORT_SYMBOL_GPL(__srcu_check_read_flavor);

/*
 * Counts the new reader in the appropriate per-CPU element of the
 * srcu_struct.
 * Returns a guaranteed non-negative index that must be passed to the
 * matching __srcu_read_unlock().
 */
/*
 * __srcu_read_lock() - 在当前活动槽登记一个普通 SRCU 读者
 * @ssp：借用保护域；必须已初始化。返回非负槽索引，调用者必须原样传给匹配的
 * __srcu_read_unlock()，可跨 CPU 解锁。无失败返回、无引用转移且不可睡眠。
 *
 * READ_ONCE 固定本次使用的 per-CPU 槽；先递增进入计数，再执行 B 屏障，保证
 * 临界区访问不能“泄漏”到登记之前，并与更新侧 A/E 等屏障共同判定旧读者。
 */
int __srcu_read_lock(struct srcu_struct *ssp)
{
	struct srcu_ctr __percpu *scp = READ_ONCE(ssp->srcu_ctrp);

	this_cpu_inc(scp->srcu_locks.counter);
	smp_mb(); /* B */  /* Avoid leaking the critical section. */
	/* B：禁止受保护数据访问越过读者登记，防止更新者过早认定本读者不存在。 */
	return __srcu_ptr_to_ctr(ssp, scp);
}
EXPORT_SYMBOL_GPL(__srcu_read_lock);

/*
 * Removes the count for the old reader from the appropriate per-CPU
 * element of the srcu_struct.  Note that this may well be a different
 * CPU than that which was incremented by the corresponding srcu_read_lock().
 */
/*
 * __srcu_read_unlock() - 为 @idx 对应读者登记退出
 * @ssp：借用同一保护域；@idx：配对 lock 返回的 0/1 索引，可在迁移后的 CPU
 * 上使用。返回无值、无失败；屏障 C 保证临界区访问先于退出计数可见。
 * lock/unlock 可落在不同 CPU，因此算法汇总所有 possible CPU 的累计数。
 */
void __srcu_read_unlock(struct srcu_struct *ssp, int idx)
{
	smp_mb(); /* C */  /* Avoid leaking the critical section. */
	/* C：若先让 unlock 可见，更新者可能回收临界区仍在访问的旧对象。 */
	this_cpu_inc(__srcu_ctr_to_ptr(ssp, idx)->srcu_unlocks.counter);
}
EXPORT_SYMBOL_GPL(__srcu_read_unlock);

#ifdef CONFIG_NEED_SRCU_NMI_SAFE
/* 该配置提供使用原子 RMW 的 NMI-safe 读侧；普通 this_cpu_inc 不足以防 NMI 嵌套。 */

/*
 * Counts the new reader in the appropriate per-CPU element of the
 * srcu_struct, but in an NMI-safe manner using RMW atomics.
 * Returns an index that must be passed to the matching srcu_read_unlock().
 */
/*
 * __srcu_read_lock_nmisafe() - 用原子 RMW 登记可在 NMI 中嵌套的读者
 * @ssp 为借用域；返回必须配对传回的槽索引，无失败。raw_cpu_ptr 取得当前 CPU
 * 槽，atomic_long_inc 防同 CPU 的 NMI 打断普通更新丢计数；after_atomic 屏障
 * 提供与普通 B 相同的临界区入口顺序。
 */
int __srcu_read_lock_nmisafe(struct srcu_struct *ssp)
{
	struct srcu_ctr __percpu *scpp = READ_ONCE(ssp->srcu_ctrp);
	struct srcu_ctr *scp = raw_cpu_ptr(scpp);

	atomic_long_inc(&scp->srcu_locks);
	smp_mb__after_atomic(); /* B */  /* Avoid leaking the critical section. */
	/* 原子操作后的 B 屏障禁止临界区访问越过进入计数。 */
	return __srcu_ptr_to_ctr(ssp, scpp);
}
EXPORT_SYMBOL_GPL(__srcu_read_lock_nmisafe);

/*
 * Removes the count for the old reader from the appropriate per-CPU
 * element of the srcu_struct.  Note that this may well be a different
 * CPU than that which was incremented by the corresponding srcu_read_lock().
 */
/*
 * __srcu_read_unlock_nmisafe() - 原子登记 NMI-safe 读者退出
 * @ssp/@idx 与 lock 配对；返回无值。before_atomic 屏障承担 C 的角色；即使
 * 任务迁移，也只需全局 lock/unlock 总数相等，不要求同一 CPU 上成对。
 */
void __srcu_read_unlock_nmisafe(struct srcu_struct *ssp, int idx)
{
	smp_mb__before_atomic(); /* C */  /* Avoid leaking the critical section. */
	/* 原子操作前的 C 屏障保证临界区访问完成后才发布退出计数。 */
	atomic_long_inc(&raw_cpu_ptr(__srcu_ctr_to_ptr(ssp, idx))->srcu_unlocks);
}
EXPORT_SYMBOL_GPL(__srcu_read_unlock_nmisafe);

#endif // CONFIG_NEED_SRCU_NMI_SAFE
/* CONFIG_NEED_SRCU_NMI_SAFE 关闭时由头文件/其他实现提供适合该架构的路径。 */

/*
 * Start an SRCU grace period.
 */
/*
 * srcu_gp_start() - 把已登记的需求推进到 SCAN1
 * @ssp：借用域；入口持有 sup->lock，且 needed 严格领先 current。
 * 返回无值；记录起始 jiffies、清零无延迟预算并通过 rcu_seq_start 发布活动
 * GP。不可睡眠。起始前全屏障把先前 needed 写与状态启动严格排序。
 */
static void srcu_gp_start(struct srcu_struct *ssp)
{
	int state;

	lockdep_assert_held(&ACCESS_PRIVATE(ssp->srcu_sup, lock));
	WARN_ON_ONCE(ULONG_CMP_GE(ssp->srcu_sup->srcu_gp_seq, ssp->srcu_sup->srcu_gp_seq_needed));
	WRITE_ONCE(ssp->srcu_sup->srcu_gp_start, jiffies);
	WRITE_ONCE(ssp->srcu_sup->srcu_n_exp_nodelay, 0);
	smp_mb(); /* Order prior store to ->srcu_gp_seq_needed vs. GP start. */
	/* 不能先让观察者看到 SCAN1、后看到旧 needed，否则可能错误进入 idle。 */
	rcu_seq_start(&ssp->srcu_sup->srcu_gp_seq);
	state = rcu_seq_state(ssp->srcu_sup->srcu_gp_seq);
	WARN_ON_ONCE(state != SRCU_STATE_SCAN1);
}


/*
 * srcu_delay_timer() - per-CPU 延时到期后把回调执行工作投递到目标 CPU
 * @t：嵌在 srcu_data 中的借用 timer；container_of 恢复所属 sdp。
 * 返回无值；定时器上下文不可睡眠，仅 queue_work，不直接执行回调。
 */
static void srcu_delay_timer(struct timer_list *t)
{
	struct srcu_data *sdp = container_of(t, struct srcu_data, delay_work);

	queue_work_on(sdp->cpu, rcu_gp_wq, &sdp->work);
}

/*
 * srcu_queue_delayed_work_on() - 立即或定时调度一个 CPU 的回调 work
 * @sdp：借用 per-CPU 状态；@delay：jiffy 数，0 表示立即 queue_work。
 * 返回无值；timer_reduce 只会把已有期限提前，不会把更早唤醒推迟。
 */
static void srcu_queue_delayed_work_on(struct srcu_data *sdp,
				       unsigned long delay)
{
	if (!delay) {
		queue_work_on(sdp->cpu, rcu_gp_wq, &sdp->work);
		return;
	}

	timer_reduce(&sdp->delay_work, jiffies + delay);
}

/*
 * Schedule callback invocation for the specified srcu_data structure,
 * if possible, on the corresponding CPU.
 */
/*
 * srcu_schedule_cbs_sdp() - per-CPU 回调调度薄包装
 * @sdp 为借用目标；@delay 为 jiffy。返回无值，副作用是投递 work/timer；
 * 不保证一定在该 CPU 执行，只向 queue_work_on 表达亲和目标。
 */
static void srcu_schedule_cbs_sdp(struct srcu_data *sdp, unsigned long delay)
{
	srcu_queue_delayed_work_on(sdp, delay);
}

/*
 * Schedule callback invocation for all srcu_data structures associated
 * with the specified srcu_node structure that have callbacks for the
 * just-completed grace period, the one corresponding to idx.  If possible,
 * schedule this invocation on the corresponding CPUs.
 */
/*
 * srcu_schedule_cbs_snp() - 按叶节点位图唤醒有完成回调的 CPU
 * @ssp/@snp：借用域和节点；@mask：以 snp->grplo 为 bit0 的 CPU 位图；
 * @delay：jiffy。只为 fully-online CPU 投递，返回无值且不转移队列 ownership。
 */
static void srcu_schedule_cbs_snp(struct srcu_struct *ssp, struct srcu_node *snp,
				  unsigned long mask, unsigned long delay)
{
	int cpu;

	for (cpu = snp->grplo; cpu <= snp->grphi; cpu++)
		if ((mask & (1UL << (cpu - snp->grplo))) && rcu_cpu_beenfullyonline(cpu))
			srcu_schedule_cbs_sdp(per_cpu_ptr(ssp->sda, cpu), delay);
}

/*
 * Note the end of an SRCU grace period.  Initiates callback invocation
 * and starts a new grace period if needed.
 *
 * The ->srcu_cb_mutex acquisition does not protect any data, but
 * instead prevents more than one grace period from starting while we
 * are initiating callback invocation.  This allows the ->srcu_have_cbs[]
 * array to have a finite number of elements.
 */
/*
 * srcu_gp_end() - 提交 SCAN2 完成、唤醒回调并按需衔接下一 GP
 *
 * @ssp：借用域。入口由 srcu_advance_state() 持有 srcu_gp_mutex，当前状态必须
 * 是 SCAN2；函数可睡眠（srcu_cb_mutex、可能 GFP_KERNEL 建树）。返回无值，
 * 并负责释放 srcu_gp_mutex。
 *
 * srcu_cb_mutex 不保护普通字段，而是限制“完成回调分发期间”最多再启动一个
 * GP，使有限个 srcu_have_cbs 槽不会被快速连续 GP 覆盖。完成点先发布序号，
 * 再按 SMALL/BIG 模式找出需执行回调的 CPU，最后检查下一需求和扩容状态机。
 */
static void srcu_gp_end(struct srcu_struct *ssp)
{
	/*
	 * 变量地图：gpseq 为刚完成序号；idx 为环形 have_cbs 槽；mask 为叶节点
	 * CPU 位图；ss_state 是 acquire 的扩容阶段快照；cbdelay 决定是否立即回调。
	 */
	unsigned long cbdelay = 1;
	bool cbs;
	bool last_lvl;
	int cpu;
	unsigned long gpseq;
	int idx;
	unsigned long mask;
	struct srcu_data *sdp;
	unsigned long sgsne;
	struct srcu_node *snp;
	int ss_state;
	struct srcu_usage *sup = ssp->srcu_sup;

	/* Prevent more than one additional grace period. */
	/* 串行化“结束本 GP→标记回调→允许后继 GP”，而非保护单一数据对象。 */
	mutex_lock(&sup->srcu_cb_mutex);

	/* End the current grace period. */
	/*
	 * 域锁下把 SCAN2 结束为 idle/下一序号，并把 expedited needed 水位追到完成
	 * 序号；解锁后释放 gp_mutex，至此其他线程可启动下一 GP，但受 cb_mutex 限制。
	 */
	raw_spin_lock_irq_rcu_node(sup);
	idx = rcu_seq_state(sup->srcu_gp_seq);
	WARN_ON_ONCE(idx != SRCU_STATE_SCAN2);
	if (srcu_gp_is_expedited(ssp))
		cbdelay = 0;

	WRITE_ONCE(sup->srcu_last_gp_end, ktime_get_mono_fast_ns());
	rcu_seq_end(&sup->srcu_gp_seq);
	gpseq = rcu_seq_current(&sup->srcu_gp_seq);
	if (ULONG_CMP_LT(sup->srcu_gp_seq_needed_exp, gpseq))
		WRITE_ONCE(sup->srcu_gp_seq_needed_exp, gpseq);
	raw_spin_unlock_irq_rcu_node(sup);
	mutex_unlock(&sup->srcu_gp_mutex);
	/* A new grace period can start at this point.  But only one. */
	/* 新 GP 可以开始，但 cb_mutex 阻止它再完成回调分发前无限向前滚动。 */

	/* Initiate callback invocation as needed. */
	/*
	 * SMALL/树未达可用阶段时，所有回调集中在 boot CPU；树可用后逐节点把当前
	 * gpseq 写入环形槽，取走并清零有回调 CPU 位图，再在锁外投递 work。
	 */
	ss_state = smp_load_acquire(&sup->srcu_size_state);
	if (ss_state < SRCU_SIZE_WAIT_BARRIER) {
		srcu_schedule_cbs_sdp(per_cpu_ptr(ssp->sda, get_boot_cpu_id()),
					cbdelay);
	} else {
		idx = rcu_seq_ctr(gpseq) % ARRAY_SIZE(snp->srcu_have_cbs);
		srcu_for_each_node_breadth_first(ssp, snp) {
			raw_spin_lock_irq_rcu_node(snp);
			cbs = false;
			last_lvl = snp >= sup->level[rcu_num_lvls - 1];
			if (last_lvl)
				cbs = ss_state < SRCU_SIZE_BIG || snp->srcu_have_cbs[idx] == gpseq;
			snp->srcu_have_cbs[idx] = gpseq;
			rcu_seq_set_state(&snp->srcu_have_cbs[idx], 1);
			sgsne = snp->srcu_gp_seq_needed_exp;
			if (srcu_invl_snp_seq(sgsne) || ULONG_CMP_LT(sgsne, gpseq))
				WRITE_ONCE(snp->srcu_gp_seq_needed_exp, gpseq);
			if (ss_state < SRCU_SIZE_BIG)
				mask = ~0;
			else
				mask = snp->srcu_data_have_cbs[idx];
			snp->srcu_data_have_cbs[idx] = 0;
			raw_spin_unlock_irq_rcu_node(snp);
			if (cbs)
				srcu_schedule_cbs_snp(ssp, snp, mask, cbdelay);
		}
	}

	/* Occasionally prevent srcu_data counter wrap. */
	/*
	 * 低频把落后很远的 per-CPU needed 水位追到当前序号。只有相差 100 以上才
	 * 修正，避免干扰正常未完成请求；每个 sdp 锁保护本 CPU 队列元数据。
	 */
	if (!(gpseq & counter_wrap_check))
		for_each_possible_cpu(cpu) {
			sdp = per_cpu_ptr(ssp->sda, cpu);
			raw_spin_lock_irq_rcu_node(sdp);
			if (ULONG_CMP_GE(gpseq, sdp->srcu_gp_seq_needed + 100))
				sdp->srcu_gp_seq_needed = gpseq;
			if (ULONG_CMP_GE(gpseq, sdp->srcu_gp_seq_needed_exp + 100))
				sdp->srcu_gp_seq_needed_exp = gpseq;
			raw_spin_unlock_irq_rcu_node(sdp);
		}

	/* Callback initiation done, allow grace periods after next. */
	/* have_cbs 槽和调度决定已稳定，允许后续 GP 穿过这道有限槽保护门。 */
	mutex_unlock(&sup->srcu_cb_mutex);

	/* Start a new grace period if needed. */
	/* 域锁内重新读取当前序号；若 idle 且 needed 领先，原子启动并立即重排工作。 */
	raw_spin_lock_irq_rcu_node(sup);
	gpseq = rcu_seq_current(&sup->srcu_gp_seq);
	if (!rcu_seq_state(gpseq) &&
	    ULONG_CMP_LT(gpseq, sup->srcu_gp_seq_needed)) {
		srcu_gp_start(ssp);
		raw_spin_unlock_irq_rcu_node(sup);
		srcu_reschedule(ssp, 0);
	} else {
		raw_spin_unlock_irq_rcu_node(sup);
	}

	/* Transition to big if needed. */
	/*
	 * 扩容状态机在可睡眠 GP work 上逐步推进：ALLOC 真正分配树，其余等待阶段
	 * 每完成一轮 GP/回调协议递增，最终到 BIG。release 让树遍历者取得完整节点。
	 */
	if (ss_state != SRCU_SIZE_SMALL && ss_state != SRCU_SIZE_BIG) {
		if (ss_state == SRCU_SIZE_ALLOC)
			init_srcu_struct_nodes(ssp, GFP_KERNEL);
		else
			/* release 发布本阶段完成，后继 acquire 才可使用对应树/回调状态。 */
			smp_store_release(&sup->srcu_size_state, ss_state + 1);
	}
}

/*
 * Funnel-locking scheme to scalably mediate many concurrent expedited
 * grace-period requests.  This function is invoked for the first known
 * expedited request for a grace period that has already been requested,
 * but without expediting.  To start a completely new grace period,
 * whether expedited or not, use srcu_funnel_gp_start() instead.
 */
/*
 * srcu_funnel_exp_start() - 把既有普通 GP 的“需加速”请求逐层汇聚到根
 * @ssp：借用域；@snp：可空起始叶/祖先，NULL 表示无可用树；@s：目标 GP
 * 序号快照。返回无值；每层锁只在需要提高水位时写入，发现同等/更新请求即
 * 提前返回。不可睡眠，最后更新域级 expedited 水位。
 */
static void srcu_funnel_exp_start(struct srcu_struct *ssp, struct srcu_node *snp,
				  unsigned long s)
{
	unsigned long flags;
	unsigned long sgsne;

	if (snp)
		for (; snp != NULL; snp = snp->srcu_parent) {
			sgsne = READ_ONCE(snp->srcu_gp_seq_needed_exp);
			if (WARN_ON_ONCE(rcu_seq_done(&ssp->srcu_sup->srcu_gp_seq, s)) ||
			    (!srcu_invl_snp_seq(sgsne) && ULONG_CMP_GE(sgsne, s)))
				return;
			raw_spin_lock_irqsave_rcu_node(snp, flags);
			sgsne = snp->srcu_gp_seq_needed_exp;
			if (!srcu_invl_snp_seq(sgsne) && ULONG_CMP_GE(sgsne, s)) {
				raw_spin_unlock_irqrestore_rcu_node(snp, flags);
				return;
			}
			WRITE_ONCE(snp->srcu_gp_seq_needed_exp, s);
			raw_spin_unlock_irqrestore_rcu_node(snp, flags);
		}
	raw_spin_lock_irqsave_ssp_contention(ssp, &flags);
	if (ULONG_CMP_LT(ssp->srcu_sup->srcu_gp_seq_needed_exp, s))
		WRITE_ONCE(ssp->srcu_sup->srcu_gp_seq_needed_exp, s);
	raw_spin_unlock_irqrestore_rcu_node(ssp->srcu_sup, flags);
}

/*
 * Funnel-locking scheme to scalably mediate many concurrent grace-period
 * requests.  The winner has to do the work of actually starting grace
 * period s.  Losers must either ensure that their desired grace-period
 * number is recorded on at least their leaf srcu_node structure, or they
 * must take steps to invoke their own callbacks.
 *
 * Note that this function also does the work of srcu_funnel_exp_start(),
 * in some cases by directly invoking it.
 *
 * The srcu read lock should be hold around this function. And s is a seq snap
 * after holding that lock.
 */
/*
 * srcu_funnel_gp_start() - 可扩展地登记 GP/回调需求并选出启动者
 *
 * @ssp：借用域；@sdp：当前请求所归属的 per-CPU 状态；@s：在 SRCU 读锁内
 * 获取的目标序号，避免回绕；@do_norm：true 普通、false expedited。
 * 返回无值；不睡眠。调用者仍持有保护序号生命周期的 SRCU 读锁。
 *
 * 叶到根逐层更新 needed/have_cbs；若看到已有相同或更高水位，本请求作为
 * loser 停止上行，但必须确保本 CPU 位已登记或自行调度回调。到达根的 winner
 * 在 sup->lock 下发布全局水位，并在 idle 时真正启动 GP。
 */
static void srcu_funnel_gp_start(struct srcu_struct *ssp, struct srcu_data *sdp,
				 unsigned long s, bool do_norm)
{
	unsigned long flags;
	int idx = rcu_seq_ctr(s) % ARRAY_SIZE(sdp->mynode->srcu_have_cbs);
	unsigned long sgsne;
	struct srcu_node *snp;
	struct srcu_node *snp_leaf;
	unsigned long snp_seq;
	struct srcu_usage *sup = ssp->srcu_sup;

	/* Ensure that snp node tree is fully initialized before traversing it */
	/* acquire 看到 WAIT_BARRIER 后才可读取 init_nodes 发布的 parent/mynode。 */
	if (smp_load_acquire(&sup->srcu_size_state) < SRCU_SIZE_WAIT_BARRIER)
		snp_leaf = NULL;
	else
		snp_leaf = sdp->mynode;

	if (snp_leaf)
		/* Each pass through the loop does one level of the srcu_node tree. */
		/*
		 * 每层先无条件取锁再比较环形槽，避免并发完成路径覆盖。若叶节点已有
		 * 同一 s，就 OR 入本 CPU 位；若槽已属于更新 GP，本回调不再能并入，
		 * 因而主动调度本地队列以免遗漏。
		 */
		for (snp = snp_leaf; snp != NULL; snp = snp->srcu_parent) {
			if (WARN_ON_ONCE(rcu_seq_done(&sup->srcu_gp_seq, s)) && snp != snp_leaf)
				return; /* GP already done and CBs recorded. */
			/* GP 已完成且回调位已记录；越过叶层后无需继续向祖先重复登记。 */
			raw_spin_lock_irqsave_rcu_node(snp, flags);
			snp_seq = snp->srcu_have_cbs[idx];
			if (!srcu_invl_snp_seq(snp_seq) && ULONG_CMP_GE(snp_seq, s)) {
				if (snp == snp_leaf && snp_seq == s)
					snp->srcu_data_have_cbs[idx] |= sdp->grpmask;
				raw_spin_unlock_irqrestore_rcu_node(snp, flags);
				if (snp == snp_leaf && snp_seq != s) {
					srcu_schedule_cbs_sdp(sdp, do_norm ? SRCU_INTERVAL : 0);
					return;
				}
				if (!do_norm)
					srcu_funnel_exp_start(ssp, snp, s);
				return;
			}
			snp->srcu_have_cbs[idx] = s;
			if (snp == snp_leaf)
				snp->srcu_data_have_cbs[idx] |= sdp->grpmask;
			sgsne = snp->srcu_gp_seq_needed_exp;
			if (!do_norm && (srcu_invl_snp_seq(sgsne) || ULONG_CMP_LT(sgsne, s)))
				WRITE_ONCE(snp->srcu_gp_seq_needed_exp, s);
			raw_spin_unlock_irqrestore_rcu_node(snp, flags);
		}

	/* Top of tree, must ensure the grace period will be started. */
	/* 到达根的请求负责把局部水位提交到域级全局 needed。 */
	raw_spin_lock_irqsave_ssp_contention(ssp, &flags);
	if (ULONG_CMP_LT(sup->srcu_gp_seq_needed, s)) {
		/*
		 * Record need for grace period s.  Pair with load
		 * acquire setting up for initialization.
		 */
		/* release 同静态首次初始化 acquire 共用该字段，兼具“初始化完成”发布。 */
		smp_store_release(&sup->srcu_gp_seq_needed, s); /*^^^*/
	}
	if (!do_norm && ULONG_CMP_LT(sup->srcu_gp_seq_needed_exp, s))
		WRITE_ONCE(sup->srcu_gp_seq_needed_exp, s);

	/* If grace period not already in progress, start it. */
	/* 只有 idle 且目标尚未完成时启动；sup->lock 使多个 winner 中仅一人成功。 */
	if (!WARN_ON_ONCE(rcu_seq_done(&sup->srcu_gp_seq, s)) &&
	    rcu_seq_state(sup->srcu_gp_seq) == SRCU_STATE_IDLE) {
		srcu_gp_start(ssp);

		// And how can that list_add() in the "else" clause
		// possibly be safe for concurrent execution?  Well,
		// it isn't.  And it does not have to be.  After all, it
		// can only be executed during early boot when there is only
		// the one boot CPU running with interrupts still disabled.
		//
		// Use an irq_work here to avoid acquiring runqueue lock with
		// srcu rcu_node::lock held. BPF instrument could introduce the
		// opposite dependency, hence we need to break the possible
		// locking dependency here.
		/*
		 * 原注释说明早期 list_add 并非并发安全，但当时仅 boot CPU、中断关闭，
		 * 所以无需锁。正常运行改用 irq_work，是为了先脱离 srcu node 锁再去
		 * 获取 workqueue/runqueue 相关锁，避免 BPF 插桩造成反向锁依赖。
		 */
		if (likely(srcu_init_done))
			irq_work_queue(&sup->irq_work);
		else if (list_empty(&sup->work.work.entry))
			list_add(&sup->work.work.entry, &srcu_boot_list);
	}
	raw_spin_unlock_irqrestore_rcu_node(sup, flags);
}

/*
 * Wait until all readers counted by array index idx complete, but
 * loop an additional time if there is an expedited grace period pending.
 * The caller must ensure that ->srcu_ctrp is not changed while checking.
 */
/*
 * try_check_zero() - 短轮询等待指定计数槽暂时归零
 * @ssp：借用域；@idx：旧槽；@trycount：最多检查次数，expedited 可借 curdelay
 * 多一次机会。返回 true 表示归零，false 表示应由工作队列稍后重试。
 * 调用者以 srcu_gp_mutex 保证检查期间 ctrp 不再翻转；udelay 忙等，不睡眠。
 */
static bool try_check_zero(struct srcu_struct *ssp, int idx, int trycount)
{
	unsigned long curdelay;

	raw_spin_lock_irq_rcu_node(ssp->srcu_sup);
	curdelay = !srcu_get_delay(ssp);
	raw_spin_unlock_irq_rcu_node(ssp->srcu_sup);

	for (;;) {
		if (srcu_readers_active_idx_check(ssp, idx))
			return true;
		if ((--trycount + curdelay) <= 0)
			return false;
		udelay(srcu_retry_check_delay);
	}
}

/*
 * Increment the ->srcu_ctrp counter so that future SRCU readers will
 * use the other rank of the ->srcu_(un)lock_count[] arrays.  This allows
 * us to wait for pre-existing readers in a starvation-free manner.
 */
/*
 * srcu_flip() - 把新读者切换到另一 per-CPU 计数槽
 * @ssp：借用域；调用者持有 srcu_gp_mutex 且已确认目标旧槽归零。
 * 返回无值；WRITE_ONCE 是读侧观察到的发布点。前后全屏障与 B/C 配对，使
 * 旧读者不会跨翻转逃逸、新读者不会被错误纳入本 GP；不睡眠。
 */
static void srcu_flip(struct srcu_struct *ssp)
{
	/*
	 * Because the flip of ->srcu_ctrp is executed only if the
	 * preceding call to srcu_readers_active_idx_check() found that
	 * the ->srcu_ctrs[].srcu_unlocks and ->srcu_ctrs[].srcu_locks sums
	 * matched and because that summing uses atomic_long_read(),
	 * there is ordering due to a control dependency between that
	 * summing and the WRITE_ONCE() in this call to srcu_flip().
	 * This ordering ensures that if this updater saw a given reader's
	 * increment from __srcu_read_lock(), that reader was using a value
	 * of ->srcu_ctrp from before the previous call to srcu_flip(),
	 * which should be quite rare.  This ordering thus helps forward
	 * progress because the grace period could otherwise be delayed
	 * by additional calls to __srcu_read_lock() using that old (soon
	 * to be new) value of ->srcu_ctrp.
	 *
	 * This sum-equality check and ordering also ensures that if
	 * a given call to __srcu_read_lock() uses the new value of
	 * ->srcu_ctrp, this updater's earlier scans cannot have seen
	 * that reader's increments, which is all to the good, because
	 * this grace period need not wait on that reader.  After all,
	 * if those earlier scans had seen that reader, there would have
	 * been a sum mismatch and this code would not be reached.
	 *
	 * This means that the following smp_mb() is redundant, but
	 * it stays until either (1) Compilers learn about this sort of
	 * control dependency or (2) Some production workload running on
	 * a production system is unduly delayed by this slowpath smp_mb().
	 * Except for _lite() readers, where it is inoperative, which
	 * means that it is a good thing that it is redundant.
	 */
	/*
	 * 控制依赖理论上已把先前计数相等检查排在翻转前，但编译器不完整理解这类
	 * 跨函数依赖，因此保留 E。它是慢路径成本，用明确屏障换取可审计的顺序。
	 */
	smp_mb(); /* E */  /* Pairs with B and C. */
	/* E 与读侧 B/C 配对，封住旧槽观察和新 ctrp 发布之间的重排。 */

	WRITE_ONCE(ssp->srcu_ctrp,
		   &ssp->sda->srcu_ctrs[!(ssp->srcu_ctrp - &ssp->sda->srcu_ctrs[0])]);

	/*
	 * Ensure that if the updater misses an __srcu_read_unlock()
	 * increment, that task's __srcu_read_lock() following its next
	 * __srcu_read_lock() or __srcu_read_unlock() will see the above
	 * counter update.  Note that both this memory barrier and the
	 * one in srcu_readers_active_idx_check() provide the guarantee
	 * for __srcu_read_lock().
	 *
	 * Note that this is a performance optimization, in which we spend
	 * an otherwise unnecessary smp_mb() in order to reduce the number
	 * of full per-CPU-variable scans in srcu_readers_lock_idx() and
	 * srcu_readers_unlock_idx().  But this performance optimization
	 * is not so optimal for SRCU-fast, where we would be spending
	 * not smp_mb(), but rather synchronize_rcu().  At the same time,
	 * the overhead of the smp_mb() is in the noise, so there is no
	 * point in omitting it in the SRCU-fast case.  So the same code
	 * is executed either way.
	 */
	/*
	 * D 主要减少以后全 CPU 扫描中的迟到旧槽增量；对 FAST flavor 即使替代
	 * 同步成本更高，保留统一实现更简单且该慢路径屏障相对可忽略。
	 */
	smp_mb(); /* D */  /* Pairs with C. */
	/* D 与退出屏障 C 配对，保证后续进入最终观察到已翻转 ctrp。 */
}

/*
 * If SRCU is likely idle, in other words, the next SRCU grace period
 * should be expedited, return true, otherwise return false.  Except that
 * in the presence of _lite() readers, always return false.
 *
 * Note that it is OK for several current from-idle requests for a new
 * grace period from idle to specify expediting because they will all end
 * up requesting the same grace period anyhow.  So no loss.
 *
 * Note also that if any CPU (including the current one) is still invoking
 * callbacks, this function will nevertheless say "idle".  This is not
 * ideal, but the overhead of checking all CPUs' callback lists is even
 * less ideal, especially on large systems.  Furthermore, the wakeup
 * can happen before the callback is fully removed, so we have no choice
 * but to accept this type of error.
 *
 * This function is also subject to counter-wrap errors, but let's face
 * it, if this function was preempted for enough time for the counters
 * to wrap, it really doesn't matter whether or not we expedite the grace
 * period.  The extra overhead of a needlessly expedited grace period is
 * negligible when amortized over that time period, and the extra latency
 * of a needlessly non-expedited grace period is similarly negligible.
 */
/*
 * srcu_should_expedite() - 启发式判断“从空闲启动”的首个 GP 是否应加速
 * @ssp：借用域。返回 true 仅是高概率空闲判断，允许假阳性/假阴性；不提供
 * 状态同步保证。首次静态域会初始化；读取本地回调队列时短暂持 sdp->lock。
 *
 * _lite/SLOWGP 读者禁止非请求式加速；本 CPU 已有回调也说明域并非空闲。
 * 为可扩展性不扫描所有 CPU，只用末次 GP 时间与两次 gp_seq 夹住 needed
 * 快照。即使计数回绕或远端正执行回调而误判，也只影响延迟/开销，不影响 GP
 * 正确性，这正是原英文注释所述的有意近似。
 */
static bool srcu_should_expedite(struct srcu_struct *ssp)
{
	unsigned long curseq;
	unsigned long flags;
	struct srcu_data *sdp;
	unsigned long t;
	unsigned long tlast;

	check_init_srcu_struct(ssp);
	/* If _lite() readers, don't do unsolicited expediting. */
	/* SLOWGP 风格需要真正 RCU GP，主动加速的短轮询并不能提供同等语义。 */
	if (this_cpu_read(ssp->sda->srcu_reader_flavor) & SRCU_READ_FLAVOR_SLOWGP)
		return false;
	/* If the local srcu_data structure has callbacks, not idle.  */
	/* 只查本 CPU 是概率探测；完整全 CPU 锁扫描的成本会破坏大系统扩展性。 */
	sdp = raw_cpu_ptr(ssp->sda);
	raw_spin_lock_irqsave_rcu_node(sdp, flags);
	if (rcu_segcblist_pend_cbs(&sdp->srcu_cblist)) {
		raw_spin_unlock_irqrestore_rcu_node(sdp, flags);
		return false; /* Callbacks already present, so not idle. */
		/* 已有本地待处理回调，域显然不满足“从空闲启动”的启发式条件。 */
	}
	raw_spin_unlock_irqrestore_rcu_node(sdp, flags);

	/*
	 * No local callbacks, so probabilistically probe global state.
	 * Exact information would require acquiring locks, which would
	 * kill scalability, hence the probabilistic nature of the probe.
	 */
	/* 无本地回调后用无锁全局快照继续判断，接受精度换扩展性。 */

	/* First, see if enough time has passed since the last GP. */
	/* 抑制窗口尚未过去时保持普通 GP，避免连续完成后立刻反复 expedited。 */
	t = ktime_get_mono_fast_ns();
	tlast = READ_ONCE(ssp->srcu_sup->srcu_last_gp_end);
	if (exp_holdoff == 0 ||
	    time_in_range_open(t, tlast, tlast + exp_holdoff))
		return false; /* Too soon after last GP. */
	/* 距上次 GP 结束过近，保持普通模式以抑制连续加速。 */

	/* Next, check for probable idleness. */
	/*
	 * 两次 current 读取夹住 needed；中间全屏障禁止采样重排。只有 needed 未
	 * 领先且序号保持不变，才认为观察窗口内没有 GP 启停。
	 */
	curseq = rcu_seq_current(&ssp->srcu_sup->srcu_gp_seq);
	smp_mb(); /* Order ->srcu_gp_seq with ->srcu_gp_seq_needed. */
	if (ULONG_CMP_LT(curseq, READ_ONCE(ssp->srcu_sup->srcu_gp_seq_needed)))
		return false; /* Grace period in progress, so not idle. */
	/* needed 领先 current 表示已有 GP 欠账，不能称为 idle。 */
	smp_mb(); /* Order ->srcu_gp_seq with prior access. */
	if (curseq != rcu_seq_current(&ssp->srcu_sup->srcu_gp_seq))
		return false; /* GP # changed, so not idle. */
	/* 两次采样之间 GP 编号变化，概率探测窗口不稳定，保守不加速。 */
	return true; /* With reasonable probability, idle! */
	/* 这里只是“以合理概率空闲”，不是可供正确性依赖的线性化结论。 */
}

/*
 * SRCU callback function to leak a callback.
 */
/*
 * srcu_leak_callback() - 重复入队检测后的安全占位回调
 * @rhp：借用但故意不处理的 callback head。返回无值且无副作用。
 * 与其让同一 head 双重排队破坏链表，调试路径把函数替换为空操作并泄漏请求；
 * WARN 已在提交端报告根因。
 */
static void srcu_leak_callback(struct rcu_head *rhp)
{
}

/*
 * Start an SRCU grace period, and also queue the callback if non-NULL.
 */
/*
 * srcu_gp_start_if_needed() - 将回调并入分段队列并确保目标 GP 被登记
 *
 * @ssp：借用域；@rhp：可空，非空时 ownership 转移给 SRCU 回调队列直到执行；
 * @do_norm：true 普通 GP，false 加速 GP。返回目标序号 cookie，无错误返回。
 * 可在中断受限的早期路径运行，不睡眠；通过 NMI-safe SRCU 读锁防序号在快照
 * 与漏斗登记期间回绕。
 *
 * 阶段：选择安全 sdp → 锁内入队、先 snap 后 advance/accelerate → 更新局部
 * needed → 锁外沿组合树汇聚 → 释放序号保护。失败不是资源错误；并发 loser
 * 复用 winner 已登记的 GP。
 */
static unsigned long srcu_gp_start_if_needed(struct srcu_struct *ssp,
					     struct rcu_head *rhp, bool do_norm)
{
	unsigned long flags;
	int idx;
	bool needexp = false;
	bool needgp = false;
	unsigned long s;
	struct srcu_data *sdp;
	struct srcu_node *sdp_mynode;
	int ss_state;

	check_init_srcu_struct(ssp);
	/*
	 * While starting a new grace period, make sure we are in an
	 * SRCU read-side critical section so that the grace-period
	 * sequence number cannot wrap around in the meantime.
	 */
	/* 临时读锁不是保护业务数据，而是给有限位宽 gp_seq 快照提供生命周期窗口。 */
	idx = __srcu_read_lock_nmisafe(ssp);
	ss_state = smp_load_acquire(&ssp->srcu_sup->srcu_size_state);
	// If !rcu_cpu_beenfullyonline(), interrupts are still disabled,
	// so no migration is possible in either direction from this CPU.
	/*
	 * CPU 尚未 fully online 时中断关闭、不会迁移，且早期回调统一归 boot CPU；
	 * 树/调用阶段未到 WAIT_CALL 也必须沿该集中路径，避免访问未就绪 per-CPU 队列。
	 */
	if (ss_state < SRCU_SIZE_WAIT_CALL || !rcu_cpu_beenfullyonline(raw_smp_processor_id()))
		sdp = per_cpu_ptr(ssp->sda, get_boot_cpu_id());
	else
		sdp = raw_cpu_ptr(ssp->sda);
	raw_spin_lock_irqsave_sdp_contention(sdp, &flags);
	if (rhp)
		rcu_segcblist_enqueue(&sdp->srcu_cblist, rhp);
	/*
	 * It's crucial to capture the snapshot 's' for acceleration before
	 * reading the current gp_seq that is used for advancing. This is
	 * essential because if the acceleration snapshot is taken after a
	 * failed advancement attempt, there's a risk that a grace period may
	 * conclude and a new one may start in the interim. If the snapshot is
	 * captured after this sequence of events, the acceleration snapshot 's'
	 * could be excessively advanced, leading to acceleration failure.
	 * In such a scenario, an 'acceleration leak' can occur, where new
	 * callbacks become indefinitely stuck in the RCU_NEXT_TAIL segment.
	 * Also note that encountering advancing failures is a normal
	 * occurrence when the grace period for RCU_WAIT_TAIL is in progress.
	 *
	 * To see this, consider the following events which occur if
	 * rcu_seq_snap() were to be called after advance:
	 *
	 *  1) The RCU_WAIT_TAIL segment has callbacks (gp_num = X + 4) and the
	 *     RCU_NEXT_READY_TAIL also has callbacks (gp_num = X + 8).
	 *
	 *  2) The grace period for RCU_WAIT_TAIL is seen as started but not
	 *     completed so rcu_seq_current() returns X + SRCU_STATE_SCAN1.
	 *
	 *  3) This value is passed to rcu_segcblist_advance() which can't move
	 *     any segment forward and fails.
	 *
	 *  4) srcu_gp_start_if_needed() still proceeds with callback acceleration.
	 *     But then the call to rcu_seq_snap() observes the grace period for the
	 *     RCU_WAIT_TAIL segment as completed and the subsequent one for the
	 *     RCU_NEXT_READY_TAIL segment as started (ie: X + 4 + SRCU_STATE_SCAN1)
	 *     so it returns a snapshot of the next grace period, which is X + 12.
	 *
	 *  5) The value of X + 12 is passed to rcu_segcblist_accelerate() but the
	 *     freshly enqueued callback in RCU_NEXT_TAIL can't move to
	 *     RCU_NEXT_READY_TAIL which already has callbacks for a previous grace
	 *     period (gp_num = X + 8). So acceleration fails.
	 */
	/*
	 * 核心顺序是“先 snap 加速目标，再 advance 已完成段”。若反过来，两个 GP
	 * 可在锁外观察间隙完成/启动，snap 会跳到 X+12，而 NEXT_READY 已占 X+8，
	 * 新回调无法后移，永久卡在 NEXT_TAIL，形成 acceleration leak。
	 */
	s = rcu_seq_snap(&ssp->srcu_sup->srcu_gp_seq);
	if (rhp) {
		rcu_segcblist_advance(&sdp->srcu_cblist,
				      rcu_seq_current(&ssp->srcu_sup->srcu_gp_seq));
		/*
		 * Acceleration can never fail because the base current gp_seq
		 * used for acceleration is <= the value of gp_seq used for
		 * advancing. This means that RCU_NEXT_TAIL segment will
		 * always be able to be emptied by the acceleration into the
		 * RCU_NEXT_READY_TAIL or RCU_WAIT_TAIL segments.
		 */
		/*
		 * 由于 acceleration 的基准 s 早于等于 advance 所用 current，分段队列
		 * 必有可容纳位置；WARN 表示内部序号不变量已被破坏，而非可恢复失败。
		 */
		WARN_ON_ONCE(!rcu_segcblist_accelerate(&sdp->srcu_cblist, s));
	}
	if (ULONG_CMP_LT(sdp->srcu_gp_seq_needed, s)) {
		sdp->srcu_gp_seq_needed = s;
		needgp = true;
	}
	if (!do_norm && ULONG_CMP_LT(sdp->srcu_gp_seq_needed_exp, s)) {
		sdp->srcu_gp_seq_needed_exp = s;
		needexp = true;
	}
	raw_spin_unlock_irqrestore_rcu_node(sdp, flags);

	/* Ensure that snp node tree is fully initialized before traversing it */
	/* 与 init_nodes 的 release 配对；树未发布时从 NULL 起点直接更新域级水位。 */
	if (ss_state < SRCU_SIZE_WAIT_BARRIER)
		sdp_mynode = NULL;
	else
		sdp_mynode = sdp->mynode;

	if (needgp)
		/* 新普通需求逐层汇聚；漏斗 winner 负责启动。 */
		srcu_funnel_gp_start(ssp, sdp, s, do_norm);
	else if (needexp)
		/* GP 已登记但加速水位落后，只补 expedited 请求，无需重复登记回调。 */
		srcu_funnel_exp_start(ssp, sdp_mynode, s);
	__srcu_read_unlock_nmisafe(ssp, idx);
	return s;
}

/*
 * Enqueue an SRCU callback on the srcu_data structure associated with
 * the current CPU and the specified srcu_struct structure, initiating
 * grace-period processing if it is not already running.
 *
 * Note that all CPUs must agree that the grace period extended beyond
 * all pre-existing SRCU read-side critical section.  On systems with
 * more than one CPU, this means that when "func()" is invoked, each CPU
 * is guaranteed to have executed a full memory barrier since the end of
 * its last corresponding SRCU read-side critical section whose beginning
 * preceded the call to call_srcu().  It also means that each CPU executing
 * an SRCU read-side critical section that continues beyond the start of
 * "func()" must have executed a memory barrier after the call_srcu()
 * but before the beginning of that SRCU read-side critical section.
 * Note that these guarantees include CPUs that are offline, idle, or
 * executing in user mode, as well as CPUs that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked call_srcu() and CPU B invoked the
 * resulting SRCU callback function "func()", then both CPU A and CPU
 * B are guaranteed to execute a full memory barrier during the time
 * interval between the call to call_srcu() and the invocation of "func()".
 * This guarantee applies even if CPU A and CPU B are the same CPU (but
 * again only if the system has more than one CPU).
 *
 * Of course, these guarantees apply only for invocations of call_srcu(),
 * srcu_read_lock(), and srcu_read_unlock() that are all passed the same
 * srcu_struct structure.
 */
/*
 * __call_srcu() - call_srcu 公共语义的内部提交点
 * @ssp：借用保护域；@rhp：调用者提供且提交后由 SRCU 暂时拥有的链表节点；
 * @func：GP 后调用的非阻塞回调；@do_norm：普通或 expedited。
 * 返回无值、无分配失败。debug 检测到同一 head 重复提交时不再入队，而把其
 * 回调替换为泄漏占位并告警，避免链表损坏。
 *
 * 原英文内存序说明的要点：同一 ssp 上，回调执行前所有调用前已存在的读侧
 * 均结束；跨 CPU 提交/执行以及读侧边界之间具备完整屏障保证。但回调可与
 * call_srcu 之后才开始的新读者并发，不能把 GP 理解成“域内没有任何读者”。
 */
static void __call_srcu(struct srcu_struct *ssp, struct rcu_head *rhp,
			rcu_callback_t func, bool do_norm)
{
	if (debug_rcu_head_queue(rhp)) {
		/* Probable double call_srcu(), so leak the callback. */
		/* 疑似双重入队时故意不触碰队列；泄漏优于 next 指针被两条链同时修改。 */
		WRITE_ONCE(rhp->func, srcu_leak_callback);
		WARN_ONCE(1, "call_srcu(): Leaked duplicate callback\n");
		return;
	}
	rhp->func = func;
	(void)srcu_gp_start_if_needed(ssp, rhp, do_norm);
}

/**
 * call_srcu() - Queue a callback for invocation after an SRCU grace period
 * @ssp: srcu_struct in queue the callback
 * @rhp: structure to be used for queueing the SRCU callback.
 * @func: function to be invoked after the SRCU grace period
 *
 * The callback function will be invoked some time after a full SRCU
 * grace period elapses, in other words after all pre-existing SRCU
 * read-side critical sections have completed.  However, the callback
 * function might well execute concurrently with other SRCU read-side
 * critical sections that started after call_srcu() was invoked.  SRCU
 * read-side critical sections are delimited by srcu_read_lock() and
 * srcu_read_unlock(), and may be nested.
 *
 * The callback will be invoked from process context, but with bh
 * disabled.  The callback function must therefore be fast and must
 * not block.
 *
 * See the description of call_rcu() for more detailed information on
 * memory ordering guarantees.
 */
/*
 * call_srcu() - 异步等待一个 SRCU GP 后执行回调
 * @ssp：借用、已初始化保护域；@rhp：输入输出 callback head，提交到执行完成前
 * 调用者不得复用/释放；@func：进程上下文、BH disabled 下调用，必须快速且
 * 不阻塞。返回无直接值，副作用是入队并按需启动普通 GP。
 *
 * 成功提交不代表立即开始新 GP，可能并入已有目标。回调只等待提交前已有
 * 读者，允许与后来的读者并发；这与对象版本化回收模型相匹配。
 */
void call_srcu(struct srcu_struct *ssp, struct rcu_head *rhp,
	       rcu_callback_t func)
{
	__call_srcu(ssp, rhp, func, true);
}
EXPORT_SYMBOL_GPL(call_srcu);

/*
 * Helper function for synchronize_srcu() and synchronize_srcu_expedited().
 */
/*
 * __synchronize_srcu() - 用栈上回调把异步 GP 转成同步等待
 * @ssp：借用域；@do_norm：true 普通，false 请求 expedited。
 * 返回无值；可能睡眠，必须在进程上下文且不能位于同一 SRCU/RCU 读侧临界区，
 * 否则等待自身退出形成死锁。调度器未活动的极早期直接返回。
 *
 * 栈上 rcu_synchronize 的 ownership 仅在本函数内；提交 wakeme_after_rcu 后
 * 等 completion，回调完成才可销毁 head。末尾 smp_mb 把返回后的访问排在 GP
 * 之后，弥补当前 CPU 可能完全未参与扫描/回调锁路径的情况。
 */
static void __synchronize_srcu(struct srcu_struct *ssp, bool do_norm)
{
	struct rcu_synchronize rcu;

	srcu_lock_sync(&ssp->dep_map);

	RCU_LOCKDEP_WARN(lockdep_is_held(ssp) ||
			 lock_is_held(&rcu_bh_lock_map) ||
			 lock_is_held(&rcu_lock_map) ||
			 lock_is_held(&rcu_sched_lock_map),
			 "Illegal synchronize_srcu() in same-type SRCU (or in RCU) read-side critical section");

	if (rcu_scheduler_active == RCU_SCHEDULER_INACTIVE)
		/* 单启动上下文尚无并发 SRCU 读者，无需建立异步等待对象。 */
		return;
	might_sleep();
	check_init_srcu_struct(ssp);
	init_completion(&rcu.completion);
	init_rcu_head_on_stack(&rcu.head);
	__call_srcu(ssp, &rcu.head, wakeme_after_rcu, do_norm);
	wait_for_completion(&rcu.completion);
	destroy_rcu_head_on_stack(&rcu.head);

	/*
	 * Make sure that later code is ordered after the SRCU grace
	 * period.  This pairs with the raw_spin_lock_irq_rcu_node()
	 * in srcu_invoke_callbacks().  Unlike Tree RCU, this is needed
	 * because the current CPU might have been totally uninvolved with
	 * (and thus unordered against) that grace period.
	 */
	/*
	 * 与 srcu_invoke_callbacks() 的 sdp 锁路径共同建立完成顺序；Tree RCU 的
	 * 当前 CPU 通常天然参与 GP，SRCU 不保证这一点，所以这里必须显式补屏障。
	 */
	smp_mb();
}

/**
 * synchronize_srcu_expedited - Brute-force SRCU grace period
 * @ssp: srcu_struct with which to synchronize.
 *
 * Wait for an SRCU grace period to elapse, but be more aggressive about
 * spinning rather than blocking when waiting.
 *
 * Note that synchronize_srcu_expedited() has the same deadlock and
 * memory-ordering properties as does synchronize_srcu().
 */
/*
 * synchronize_srcu_expedited() - 更积极轮询地同步等待 SRCU GP
 * @ssp：借用域。返回无值；能睡眠且死锁/内存序契约与 synchronize_srcu 相同。
 * rcu_gp_is_normal() 可由全局策略把“显式 expedited”降为普通处理。
 */
void synchronize_srcu_expedited(struct srcu_struct *ssp)
{
	__synchronize_srcu(ssp, rcu_gp_is_normal());
}
EXPORT_SYMBOL_GPL(synchronize_srcu_expedited);

/**
 * synchronize_srcu - wait for prior SRCU read-side critical-section completion
 * @ssp: srcu_struct with which to synchronize.
 *
 * Wait for the count to drain to zero of both indexes. To avoid the
 * possible starvation of synchronize_srcu(), it waits for the count of
 * the index=!(ssp->srcu_ctrp - &ssp->sda->srcu_ctrs[0]) to drain to zero
 * at first, and then flip the ->srcu_ctrp and wait for the count of the
 * other index.
 *
 * Can block; must be called from process context.
 *
 * Note that it is illegal to call synchronize_srcu() from the corresponding
 * SRCU read-side critical section; doing so will result in deadlock.
 * However, it is perfectly legal to call synchronize_srcu() on one
 * srcu_struct from some other srcu_struct's read-side critical section,
 * as long as the resulting graph of srcu_structs is acyclic.
 *
 * There are memory-ordering constraints implied by synchronize_srcu().
 * On systems with more than one CPU, when synchronize_srcu() returns,
 * each CPU is guaranteed to have executed a full memory barrier since
 * the end of its last corresponding SRCU read-side critical section
 * whose beginning preceded the call to synchronize_srcu().  In addition,
 * each CPU having an SRCU read-side critical section that extends beyond
 * the return from synchronize_srcu() is guaranteed to have executed a
 * full memory barrier after the beginning of synchronize_srcu() and before
 * the beginning of that SRCU read-side critical section.  Note that these
 * guarantees include CPUs that are offline, idle, or executing in user mode,
 * as well as CPUs that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked synchronize_srcu(), which returned
 * to its caller on CPU B, then both CPU A and CPU B are guaranteed
 * to have executed a full memory barrier during the execution of
 * synchronize_srcu().  This guarantee applies even if CPU A and CPU B
 * are the same CPU, but again only if the system has more than one CPU.
 *
 * Of course, these memory-ordering guarantees apply only when
 * synchronize_srcu(), srcu_read_lock(), and srcu_read_unlock() are
 * passed the same srcu_struct structure.
 *
 * Implementation of these memory-ordering guarantees is similar to
 * that of synchronize_rcu().
 *
 * If SRCU is likely idle as determined by srcu_should_expedite(),
 * expedite the first request.  This semantic was provided by Classic SRCU,
 * and is relied upon by its users, so TREE SRCU must also provide it.
 * Note that detecting idleness is heuristic and subject to both false
 * positives and negatives.
 */
/*
 * synchronize_srcu() - 等待调用前同域 SRCU 读侧全部结束
 * @ssp：借用、已初始化域。返回无值；可阻塞，仅限进程上下文，禁止在同一域
 * 读侧内调用。不同 SRCU 域间可嵌套等待，但依赖图必须无环。
 *
 * 两槽算法先等当前新读者未使用的旧槽归零，再翻转 ctrp，最后等另一槽归零，
 * 从而新读者不断到来也不会饿死更新者。若启发式判断域空闲或全局要求加速，
 * 首个请求走 expedited；误判只改变性能。原英文列出的跨 CPU 全屏障保证仅
 * 对传入同一个 ssp 的 lock/unlock/synchronize 操作成立。
 */
void synchronize_srcu(struct srcu_struct *ssp)
{
	if (srcu_should_expedite(ssp) || rcu_gp_is_expedited())
		synchronize_srcu_expedited(ssp);
	else
		__synchronize_srcu(ssp, true);
}
EXPORT_SYMBOL_GPL(synchronize_srcu);

/**
 * get_state_synchronize_srcu - Provide an end-of-grace-period cookie
 * @ssp: srcu_struct to provide cookie for.
 *
 * This function returns a cookie that can be passed to
 * poll_state_synchronize_srcu(), which will return true if a full grace
 * period has elapsed in the meantime.  It is the caller's responsibility
 * to make sure that grace period happens, for example, by invoking
 * call_srcu() after return from get_state_synchronize_srcu().
 */
/*
 * get_state_synchronize_srcu() - 只获取“未来某 GP 完成”的轮询 cookie
 * @ssp：借用域。返回有限位宽序号 cookie，不启动 GP、无错误返回。
 * 调用者必须另行保证 GP 发生，例如随后 call_srcu；前置 smp_mb 把此前对
 * SRCU 保护数据的更新排在 cookie 采样之前。不可用 cookie 延长对象寿命。
 */
unsigned long get_state_synchronize_srcu(struct srcu_struct *ssp)
{
	// Any prior manipulation of SRCU-protected data must happen
	// before the load from ->srcu_gp_seq.
	/* 先发布受保护数据修改，再采样 gp_seq，防轮询成功却未覆盖这些写入。 */
	smp_mb();
	return rcu_seq_snap(&ssp->srcu_sup->srcu_gp_seq);
}
EXPORT_SYMBOL_GPL(get_state_synchronize_srcu);

/**
 * start_poll_synchronize_srcu - Provide cookie and start grace period
 * @ssp: srcu_struct to provide cookie for.
 *
 * This function returns a cookie that can be passed to
 * poll_state_synchronize_srcu(), which will return true if a full grace
 * period has elapsed in the meantime.  Unlike get_state_synchronize_srcu(),
 * this function also ensures that any needed SRCU grace period will be
 * started.  This convenience does come at a cost in terms of CPU overhead.
 */
/*
 * start_poll_synchronize_srcu() - 获取 cookie 并确保对应普通 GP 已登记
 * @ssp：借用域。返回目标序号，无失败；相较 get_state 会触发漏斗、work 和
 * 计数操作，CPU 开销更高。调用者以后用 poll 查询，cleanup 前必须确保其结束。
 */
unsigned long start_poll_synchronize_srcu(struct srcu_struct *ssp)
{
	return srcu_gp_start_if_needed(ssp, NULL, true);
}
EXPORT_SYMBOL_GPL(start_poll_synchronize_srcu);

/**
 * poll_state_synchronize_srcu - Has cookie's grace period ended?
 * @ssp: srcu_struct to provide cookie for.
 * @cookie: Return value from get_state_synchronize_srcu() or start_poll_synchronize_srcu().
 *
 * This function takes the cookie that was returned from either
 * get_state_synchronize_srcu() or start_poll_synchronize_srcu(), and
 * returns @true if an SRCU grace period elapsed since the time that the
 * cookie was created.
 *
 * Because cookies are finite in size, wrapping/overflow is possible.
 * This is more pronounced on 32-bit systems where cookies are 32 bits,
 * where in theory wrapping could happen in about 14 hours assuming
 * 25-microsecond expedited SRCU grace periods.  However, a more likely
 * overflow lower bound is on the order of 24 days in the case of
 * one-millisecond SRCU grace periods.  Of course, wrapping in a 64-bit
 * system requires geologic timespans, as in more than seven million years
 * even for expedited SRCU grace periods.
 *
 * Wrapping/overflow is much more of an issue for CONFIG_SMP=n systems
 * that also have CONFIG_PREEMPTION=n, which selects Tiny SRCU.  This uses
 * a 16-bit cookie, which rcutorture routinely wraps in a matter of a
 * few minutes.  If this proves to be a problem, this counter will be
 * expanded to the same size as for Tree SRCU.
 */
/*
 * poll_state_synchronize_srcu() - 非阻塞检查 cookie 对应 GP 是否完成
 * @ssp：借用同一域；@cookie：由 get/start_poll 返回，不能跨域混用。
 * 返回 false 表示尚未精确完成，true 表示已完成或特殊 COMPLETED cookie。
 * true 前的全屏障把 GP 结束排在调用者后续访问之前。有限位宽会回绕：Tree
 * SRCU 32 位在极端速率下仍有小时/天级理论风险，64 位近似可忽略；Tiny
 * SRCU 的 16 位风险更显著，调用者不应无限期保存 cookie。
 */
bool poll_state_synchronize_srcu(struct srcu_struct *ssp, unsigned long cookie)
{
	if (cookie != SRCU_GET_STATE_COMPLETED &&
	    !rcu_seq_done_exact(&ssp->srcu_sup->srcu_gp_seq, cookie))
		return false;
	// Ensure that the end of the SRCU grace period happens before
	// any subsequent code that the caller might execute.
	/* acquire 式完成边界：轮询成功后的回收/复用不得越过 GP 结束。 */
	smp_mb(); // ^^^
	return true;
}
EXPORT_SYMBOL_GPL(poll_state_synchronize_srcu);

/*
 * Callback function for srcu_barrier() use.
 */
/*
 * srcu_barrier_cb() - 一个 per-CPU barrier 哨兵回调完成时汇总
 * @rhp：嵌在 sdp 中、由 barrier 临时入队的 head。返回无值；把 next 自指作为
 * 已执行标记，并原子递减全域计数，最后一个完成者唤醒等待线程。
 * 回调在 BH disabled 进程上下文运行，不睡眠。
 */
static void srcu_barrier_cb(struct rcu_head *rhp)
{
	struct srcu_data *sdp;
	struct srcu_struct *ssp;

	rhp->next = rhp; // Mark the callback as having been invoked.
	/* 自指恢复“空闲/已调用”哨兵，下一次 barrier 才可安全复用同一 head。 */
	sdp = container_of(rhp, struct srcu_data, srcu_barrier_head);
	ssp = sdp->ssp;
	if (atomic_dec_and_test(&ssp->srcu_sup->srcu_barrier_cpu_cnt))
		complete(&ssp->srcu_sup->srcu_barrier_completion);
}

/*
 * Enqueue an srcu_barrier() callback on the specified srcu_data
 * structure's ->cblist.  but only if that ->cblist already has at least one
 * callback enqueued.  Note that if a CPU already has callbacks enqueue,
 * it must have already registered the need for a future grace period,
 * so all we need do is enqueue a callback that will use the same grace
 * period as the last callback already in the queue.
 */
/*
 * srcu_barrier_one_cpu() - 若某 sdp 有旧回调，在队尾附加 barrier 哨兵
 * @ssp/@sdp：借用域和 per-CPU 队列。返回无值；sdp 锁内先增加待完成计数，
 * entrain 成功后哨兵复用队尾最后回调已登记的 GP，无需启动新 GP；队列为空
 * 则撤销 debug 状态与计数。不可睡眠。
 */
static void srcu_barrier_one_cpu(struct srcu_struct *ssp, struct srcu_data *sdp)
{
	raw_spin_lock_irq_rcu_node(sdp);
	atomic_inc(&ssp->srcu_sup->srcu_barrier_cpu_cnt);
	sdp->srcu_barrier_head.func = srcu_barrier_cb;
	debug_rcu_head_queue(&sdp->srcu_barrier_head);
	if (!rcu_segcblist_entrain(&sdp->srcu_cblist,
				   &sdp->srcu_barrier_head)) {
		debug_rcu_head_unqueue(&sdp->srcu_barrier_head);
		atomic_dec(&ssp->srcu_sup->srcu_barrier_cpu_cnt);
	}
	raw_spin_unlock_irq_rcu_node(sdp);
}

/**
 * srcu_barrier - Wait until all in-flight call_srcu() callbacks complete.
 * @ssp: srcu_struct on which to wait for in-flight callbacks.
 */
/*
 * srcu_barrier() - 等待调用时已经提交的所有 call_srcu 回调完成
 * @ssp：借用域。返回无值；可睡眠。barrier_mutex 串行多个 barrier，后来的
 * 调用可借用先行者结果。函数不阻止并发新 call_srcu；只以每个非空 sdp 队尾
 * 哨兵建立调用时截面。
 *
 * 初始计数 1 防止遍历尚未完成时某 CPU 哨兵立即执行把 completion 提前触发；
 * 全部投递后移除该哨兵，计数归零才真正表示截面内所有队列已越过哨兵。
 */
void srcu_barrier(struct srcu_struct *ssp)
{
	int cpu;
	int idx;
	unsigned long s = rcu_seq_snap(&ssp->srcu_sup->srcu_barrier_seq);

	check_init_srcu_struct(ssp);
	mutex_lock(&ssp->srcu_sup->srcu_barrier_mutex);
	if (rcu_seq_done(&ssp->srcu_sup->srcu_barrier_seq, s)) {
		smp_mb(); /* Force ordering following return. */
		/* 借用先行 barrier 的结果时仍补全屏障，使返回后访问晚于其回调完成。 */
		mutex_unlock(&ssp->srcu_sup->srcu_barrier_mutex);
		return; /* Someone else did our work for us. */
		/* 另一个 barrier 已覆盖本调用截面，无需重复向每 CPU 队列插哨兵。 */
	}
	rcu_seq_start(&ssp->srcu_sup->srcu_barrier_seq);
	init_completion(&ssp->srcu_sup->srcu_barrier_completion);

	/* Initial count prevents reaching zero until all CBs are posted. */
	/* 发布阶段持有虚拟引用，防止早完成者在后续 CPU 尚未 entrain 前唤醒。 */
	atomic_set(&ssp->srcu_sup->srcu_barrier_cpu_cnt, 1);

	idx = __srcu_read_lock_nmisafe(ssp);
	/* acquire 取得 init_nodes 在 WAIT_BARRIER 发布前完成的 mynode/parent 接线。 */
	if (smp_load_acquire(&ssp->srcu_sup->srcu_size_state) < SRCU_SIZE_WAIT_BARRIER)
		srcu_barrier_one_cpu(ssp, per_cpu_ptr(ssp->sda,	get_boot_cpu_id()));
	else
		for_each_possible_cpu(cpu)
			srcu_barrier_one_cpu(ssp, per_cpu_ptr(ssp->sda, cpu));
	__srcu_read_unlock_nmisafe(ssp, idx);

	/* Remove the initial count, at which point reaching zero can happen. */
	/* 所有队列均已检查/附加，此后归零才是完整 barrier 条件。 */
	if (atomic_dec_and_test(&ssp->srcu_sup->srcu_barrier_cpu_cnt))
		complete(&ssp->srcu_sup->srcu_barrier_completion);
	wait_for_completion(&ssp->srcu_sup->srcu_barrier_completion);

	rcu_seq_end(&ssp->srcu_sup->srcu_barrier_seq);
	mutex_unlock(&ssp->srcu_sup->srcu_barrier_mutex);
}
EXPORT_SYMBOL_GPL(srcu_barrier);

/* Callback for srcu_expedite_current() usage. */
/*
 * srcu_expedite_current_cb() - 完成一次加速哨兵并按状态决定是否再投递
 * @rhp：嵌在 sdp 的专用 srcu_ec_head，ownership 正由回调队列归还给 sdp。
 * 返回无值；sdp->lock 保护 IDLE/PENDING/REPOST 状态机。REPOST 表示第一次
 * 回调飞行期间又收到请求，必须转回 PENDING 并在锁外重新 call_srcu。
 */
static void srcu_expedite_current_cb(struct rcu_head *rhp)
{
	unsigned long flags;
	bool needcb = false;
	struct srcu_data *sdp = container_of(rhp, struct srcu_data, srcu_ec_head);

	raw_spin_lock_irqsave_sdp_contention(sdp, &flags);
	if (sdp->srcu_ec_state == SRCU_EC_IDLE) {
		WARN_ON_ONCE(1);
	} else if (sdp->srcu_ec_state == SRCU_EC_PENDING) {
		sdp->srcu_ec_state = SRCU_EC_IDLE;
	} else {
		WARN_ON_ONCE(sdp->srcu_ec_state != SRCU_EC_REPOST);
		sdp->srcu_ec_state = SRCU_EC_PENDING;
		needcb = true;
	}
	raw_spin_unlock_irqrestore_rcu_node(sdp, flags);
	// If needed, requeue ourselves as an expedited SRCU callback.
	/* 锁外重排，避免 __call_srcu 获取同一 sdp 锁造成自锁；false 请求 expedited。 */
	if (needcb)
		__call_srcu(sdp->ssp, &sdp->srcu_ec_head, srcu_expedite_current_cb, false);
}

/**
 * srcu_expedite_current - Expedite the current SRCU grace period
 * @ssp: srcu_struct to expedite.
 *
 * Cause the current SRCU grace period to become expedited.  The grace
 * period following the current one might also be expedited.  If there is
 * no current grace period, one might be created.  If the current grace
 * period is currently sleeping, that sleep will complete before expediting
 * will take effect.
 */
/*
 * srcu_expedite_current() - 请求当前（必要时下一）SRCU GP 进入加速模式
 * @ssp：借用域。返回无值；不等待 GP 完成。migrate_disable 固定 this_cpu_ptr
 * 所选 sdp 到解锁/提交之后，sdp 锁串行 EC 状态机。
 *
 * IDLE 首次投递专用回调；PENDING 改 REPOST 合并第二次请求；REPOST 继续合并。
 * 因回调本身排在 expedited GP 后，它既维持加速水位又提供“飞行中请求”重发
 * 边界。若 GP 正在定时睡眠，本 API 不取消该睡眠，只影响随后处理。
 */
void srcu_expedite_current(struct srcu_struct *ssp)
{
	unsigned long flags;
	bool needcb = false;
	struct srcu_data *sdp;

	migrate_disable();
	sdp = this_cpu_ptr(ssp->sda);
	raw_spin_lock_irqsave_sdp_contention(sdp, &flags);
	if (sdp->srcu_ec_state == SRCU_EC_IDLE) {
		sdp->srcu_ec_state = SRCU_EC_PENDING;
		needcb = true;
	} else if (sdp->srcu_ec_state == SRCU_EC_PENDING) {
		sdp->srcu_ec_state = SRCU_EC_REPOST;
	} else {
		WARN_ON_ONCE(sdp->srcu_ec_state != SRCU_EC_REPOST);
	}
	raw_spin_unlock_irqrestore_rcu_node(sdp, flags);
	// If needed, queue an expedited SRCU callback.
	/* 仅 IDLE→PENDING 的线程实际提交；其余请求压缩进状态位，避免回调风暴。 */
	if (needcb)
		__call_srcu(ssp, &sdp->srcu_ec_head, srcu_expedite_current_cb, false);
	migrate_enable();
}
EXPORT_SYMBOL_GPL(srcu_expedite_current);

/**
 * srcu_batches_completed - return batches completed.
 * @ssp: srcu_struct on which to report batch completion.
 *
 * Report the number of batches, correlated with, but not necessarily
 * precisely the same as, the number of grace periods that have elapsed.
 */
/*
 * srcu_batches_completed() - 返回供统计使用的 GP 序号快照
 * @ssp：借用域。返回 READ_ONCE 的原始序号编码，与完成批次数相关但不保证一一
 * 对应，调用者不得用它替代 poll cookie 或同步原语；无副作用、不睡眠。
 */
unsigned long srcu_batches_completed(struct srcu_struct *ssp)
{
	return READ_ONCE(ssp->srcu_sup->srcu_gp_seq);
}
EXPORT_SYMBOL_GPL(srcu_batches_completed);

/*
 * Core SRCU state machine.  Push state bits of ->srcu_gp_seq
 * to SRCU_STATE_SCAN2, and invoke srcu_gp_end() when scan has
 * completed in that state.
 */
/*
 * srcu_advance_state() - 驱动 SRCU GP 的 IDLE→SCAN1→SCAN2→完成状态机
 * @ssp：借用域。返回无值；可睡眠并持有 srcu_gp_mutex 串行槽翻转和扫描。
 * 若读者尚未退出，会释放 mutex 返回，由 process_srcu 稍后重试；完成 SCAN2
 * 时调用 srcu_gp_end()，由后者负责释放 mutex。
 *
 * 两轮扫描必要性：读者可能在读取 ctrp 后长时间延迟，所以任一时刻两个槽都
 * 可能仍有读者。先清当前“非活动旧槽”，翻转新读者去向，再清刚成为旧槽的
 * 读者，才能覆盖 GP 请求前已存在的所有临界区。
 */
static void srcu_advance_state(struct srcu_struct *ssp)
{
	int idx;

	mutex_lock(&ssp->srcu_sup->srcu_gp_mutex);

	/*
	 * Because readers might be delayed for an extended period after
	 * fetching ->srcu_ctrp for their index, at any point in time there
	 * might well be readers using both idx=0 and idx=1.  We therefore
	 * need to wait for readers to clear from both index values before
	 * invoking a callback.
	 *
	 * The load-acquire ensures that we see the accesses performed
	 * by the prior grace period.
	 */
	/* acquire 同上个 GP 的结束发布配对，开始新扫描前取得其状态/数据访问。 */
	idx = rcu_seq_state(smp_load_acquire(&ssp->srcu_sup->srcu_gp_seq)); /* ^^^ */
	if (idx == SRCU_STATE_IDLE) {
		/*
		 * IDLE 下重新以域锁核对 needed。无欠账立即退出；有欠账则本线程启动，
		 * 若锁外已有别人启动，只释放 mutex，不重复推进对方的 SCAN1。
		 */
		raw_spin_lock_irq_rcu_node(ssp->srcu_sup);
		if (ULONG_CMP_GE(ssp->srcu_sup->srcu_gp_seq, ssp->srcu_sup->srcu_gp_seq_needed)) {
			WARN_ON_ONCE(rcu_seq_state(ssp->srcu_sup->srcu_gp_seq));
			raw_spin_unlock_irq_rcu_node(ssp->srcu_sup);
			mutex_unlock(&ssp->srcu_sup->srcu_gp_mutex);
			return;
		}
		idx = rcu_seq_state(READ_ONCE(ssp->srcu_sup->srcu_gp_seq));
		if (idx == SRCU_STATE_IDLE)
			srcu_gp_start(ssp);
		raw_spin_unlock_irq_rcu_node(ssp->srcu_sup);
		if (idx != SRCU_STATE_IDLE) {
			mutex_unlock(&ssp->srcu_sup->srcu_gp_mutex);
			return; /* Someone else started the grace period. */
			/* 竞争者已把状态推进出 IDLE，本线程不重复扫描其未稳定阶段。 */
		}
	}

	if (rcu_seq_state(READ_ONCE(ssp->srcu_sup->srcu_gp_seq)) == SRCU_STATE_SCAN1) {
		/*
		 * SCAN1 先等待当前 ctrp 未指向的槽归零。成功后 flip，把未来读者导向
		 * 刚清空槽，再发布 SCAN2；失败不忙等过久，交回工作队列退避。
		 */
		idx = !(ssp->srcu_ctrp - &ssp->sda->srcu_ctrs[0]);
		if (!try_check_zero(ssp, idx, 1)) {
			mutex_unlock(&ssp->srcu_sup->srcu_gp_mutex);
			return; /* readers present, retry later. */
			/* 旧槽仍有读者；释放串行锁，由 delayed_work 后续重试。 */
		}
		srcu_flip(ssp);
		raw_spin_lock_irq_rcu_node(ssp->srcu_sup);
		rcu_seq_set_state(&ssp->srcu_sup->srcu_gp_seq, SRCU_STATE_SCAN2);
		ssp->srcu_sup->srcu_n_exp_nodelay = 0;
		raw_spin_unlock_irq_rcu_node(ssp->srcu_sup);
	}

	if (rcu_seq_state(READ_ONCE(ssp->srcu_sup->srcu_gp_seq)) == SRCU_STATE_SCAN2) {

		/*
		 * SRCU read-side critical sections are normally short,
		 * so check at least twice in quick succession after a flip.
		 */
		/*
		 * SRCU 读侧通常很短，翻转后快速检查两次可低延迟完成；仍有旧读者则
		 * 返回重试。归零后 srcu_gp_end 提交完成并释放 gp_mutex，当前函数
		 * 不能再触碰受该 mutex 串行的阶段状态。
		 */
		idx = !(ssp->srcu_ctrp - &ssp->sda->srcu_ctrs[0]);
		if (!try_check_zero(ssp, idx, 2)) {
			mutex_unlock(&ssp->srcu_sup->srcu_gp_mutex);
			return; /* readers present, retry later. */
			/* 翻转后的旧槽仍未清空，本轮不完成 GP，稍后继续 SCAN2。 */
		}
		ssp->srcu_sup->srcu_n_exp_nodelay = 0;
		srcu_gp_end(ssp);  /* Releases ->srcu_gp_mutex. */
		/* gp_end 内部释放 gp_mutex；调用后本函数不再执行需要该锁的操作。 */
	}
}

/*
 * Invoke a limited number of SRCU callbacks that have passed through
 * their grace period.  If there are more to do, SRCU will reschedule
 * the workqueue.  Note that needed memory barriers have been executed
 * in this task's context by srcu_readers_active_idx_check().
 */
/*
 * srcu_invoke_callbacks() - 从一个 sdp 批量摘取并调用已过 GP 的回调
 * @work：嵌在 srcu_data 中的 work；container_of 得到所属 sdp/ssp。
 * 返回无值；进程上下文执行，但每个用户回调周围禁用 BH，回调不得阻塞。
 *
 * sdp 锁内先 advance 分段队列并设置 invoking，锁外执行用户函数，避免用户
 * 回调在自旋锁下运行。再次加锁扣减队列总数、清 invoking；若嵌套提交或
 * barrier 留下 ready 回调，立即重排下一轮。
 */
static void srcu_invoke_callbacks(struct work_struct *work)
{
	/* ready_cbs 是本轮私有临时链表；摘取后 ownership 从 sdp 队列转给当前 work。 */
	long len;
	bool more;
	struct rcu_cblist ready_cbs;
	struct rcu_head *rhp;
	struct srcu_data *sdp;
	struct srcu_struct *ssp;

	sdp = container_of(work, struct srcu_data, work);

	ssp = sdp->ssp;
	rcu_cblist_init(&ready_cbs);
	raw_spin_lock_irq_rcu_node(sdp);
	WARN_ON_ONCE(!rcu_segcblist_segempty(&sdp->srcu_cblist, RCU_NEXT_TAIL));
	rcu_segcblist_advance(&sdp->srcu_cblist,
			      rcu_seq_current(&ssp->srcu_sup->srcu_gp_seq));
	/*
	 * Although this function is theoretically re-entrant, concurrent
	 * callbacks invocation is disallowed to avoid executing an SRCU barrier
	 * too early.
	 */
	/*
	 * work 理论上可重入，但同一 sdp 同时执行两批会让 barrier 后批越过前批而
	 * 提前完成；invoking 把它串行化。无 ready 回调则不取得执行者身份。
	 */
	if (sdp->srcu_cblist_invoking ||
	    !rcu_segcblist_ready_cbs(&sdp->srcu_cblist)) {
		raw_spin_unlock_irq_rcu_node(sdp);
		return;  /* Someone else on the job or nothing to do. */
		/* 已有执行者或没有 ready 回调，本 work 不取得任何 callback ownership。 */
	}

	/* We are on the job!  Extract and invoke ready callbacks. */
	/*
	 * 锁内原子摘取全部 done 段并记长度，随后释放锁。此后新提交可继续入 sdp，
	 * 但不能接触 ready_cbs；每个 head 调用后 ownership 归还其容器拥有者。
	 */
	sdp->srcu_cblist_invoking = true;
	rcu_segcblist_extract_done_cbs(&sdp->srcu_cblist, &ready_cbs);
	len = ready_cbs.len;
	raw_spin_unlock_irq_rcu_node(sdp);
	rhp = rcu_cblist_dequeue(&ready_cbs);
	for (; rhp != NULL; rhp = rcu_cblist_dequeue(&ready_cbs)) {
		debug_rcu_head_unqueue(rhp);
		debug_rcu_head_callback(rhp);
		local_bh_disable();
		rhp->func(rhp);
		local_bh_enable();
	}
	WARN_ON_ONCE(ready_cbs.len);

	/*
	 * Update counts, accelerate new callbacks, and if needed,
	 * schedule another round of callback invocation.
	 */
	/* 将私有批次数从全队列记账扣除，清执行标志并观察执行期间新出现的 ready。 */
	raw_spin_lock_irq_rcu_node(sdp);
	rcu_segcblist_add_len(&sdp->srcu_cblist, -len);
	sdp->srcu_cblist_invoking = false;
	more = rcu_segcblist_ready_cbs(&sdp->srcu_cblist);
	raw_spin_unlock_irq_rcu_node(sdp);
	/* An SRCU barrier or callbacks from previous nesting work pending */
	/* 嵌套回调可能重新入队；more 为 true 时必须再投递，不能依赖新 GP 唤醒。 */
	if (more)
		srcu_schedule_cbs_sdp(sdp, 0);
}

/*
 * Finished one round of SRCU grace period.  Start another if there are
 * more SRCU callbacks queued, otherwise put SRCU into not-running state.
 */
/*
 * srcu_reschedule() - 根据 needed 水位决定继续 GP work 还是进入 idle
 * @ssp：借用域；@delay：下一次 delayed_work 的 jiffy 延迟。
 * 返回无值，不睡眠。sup->lock 下检查 current/needed：全部完成且状态 idle
 * 时停止；有欠账但无活动 GP 时启动；其余情况按 delay 继续推进。
 */
static void srcu_reschedule(struct srcu_struct *ssp, unsigned long delay)
{
	bool pushgp = true;

	raw_spin_lock_irq_rcu_node(ssp->srcu_sup);
	if (ULONG_CMP_GE(ssp->srcu_sup->srcu_gp_seq, ssp->srcu_sup->srcu_gp_seq_needed)) {
		if (!WARN_ON_ONCE(rcu_seq_state(ssp->srcu_sup->srcu_gp_seq))) {
			/* All requests fulfilled, time to go idle. */
			/* current 已追上 needed 且序号确为 idle，不再排 work。 */
			pushgp = false;
		}
	} else if (!rcu_seq_state(ssp->srcu_sup->srcu_gp_seq)) {
		/* Outstanding request and no GP.  Start one. */
		/* 欠账仍在但状态 idle，本线程在锁内建立 SCAN1 后再排 work。 */
		srcu_gp_start(ssp);
	}
	raw_spin_unlock_irq_rcu_node(ssp->srcu_sup);

	if (pushgp)
		queue_delayed_work(rcu_gp_wq, &ssp->srcu_sup->work, delay);
}

/*
 * This is the work-queue function that handles SRCU grace periods.
 */
/*
 * process_srcu() - 域级 GP 工作队列入口
 * @work：srcu_usage.delayed_work 内嵌 work；返回无值，可在进程上下文睡眠。
 * 先推进一次状态机，再在域锁内计算退避。连续零延迟重排按 jiffy 计数并设总
 * 上限，超过后强制延迟 1 jiffy，避免 expedited/慢读者组合形成 CPU livelock。
 */
static void process_srcu(struct work_struct *work)
{
	unsigned long curdelay;
	unsigned long j;
	struct srcu_struct *ssp;
	struct srcu_usage *sup;

	sup = container_of(work, struct srcu_usage, work.work);
	ssp = sup->srcu_ssp;

	srcu_advance_state(ssp);
	/* advance 可能完成、推进一阶段或因旧读者存在原地返回；统一重新计算延迟。 */
	raw_spin_lock_irq_rcu_node(ssp->srcu_sup);
	curdelay = srcu_get_delay(ssp);
	raw_spin_unlock_irq_rcu_node(ssp->srcu_sup);
	if (curdelay) {
		/* 一旦真正让出 CPU，连续零延迟预算重新开始。 */
		WRITE_ONCE(sup->reschedule_count, 0);
	} else {
		j = jiffies;
		if (READ_ONCE(sup->reschedule_jiffies) == j) {
			/* 同一 jiffy 内持续立即重排，达到总阈值后强制一次调度间隔。 */
			ASSERT_EXCLUSIVE_WRITER(sup->reschedule_count);
			WRITE_ONCE(sup->reschedule_count, READ_ONCE(sup->reschedule_count) + 1);
			if (READ_ONCE(sup->reschedule_count) > srcu_max_nodelay)
				curdelay = 1;
		} else {
			/* 新 jiffy 的第一轮零延迟，重建局部计数基线。 */
			WRITE_ONCE(sup->reschedule_count, 1);
			WRITE_ONCE(sup->reschedule_jiffies, j);
		}
	}
	srcu_reschedule(ssp, curdelay);
}

/*
 * srcu_irq_work() - 脱离 node/sup 锁依赖后启动 GP delayed_work
 * @work：srcu_usage 内嵌 irq_work。返回无值；irq_work 上下文不可睡眠。
 * 在域锁内读取当前延迟，只把“是否非零”映射成 0/1 jiffy，随后投递到
 * rcu_gp_wq；这切断提交路径持锁时直接获取 runqueue/workqueue 锁的依赖。
 */
static void srcu_irq_work(struct irq_work *work)
{
	struct srcu_struct *ssp;
	struct srcu_usage *sup;
	unsigned long delay;
	unsigned long flags;

	sup = container_of(work, struct srcu_usage, irq_work);
	ssp = sup->srcu_ssp;

	raw_spin_lock_irqsave_rcu_node(ssp->srcu_sup, flags);
	delay = srcu_get_delay(ssp);
	raw_spin_unlock_irqrestore_rcu_node(ssp->srcu_sup, flags);

	queue_delayed_work(rcu_gp_wq, &sup->work, !!delay);
}

/*
 * srcutorture_get_gp_data() - 向 rcutorture 导出最小 GP 状态快照
 * @ssp：借用域；@flags：必填输出，Tree SRCU 固定写 0；@gp_seq：必填输出
 * 当前完成序号。返回无值，无同步保证，仅用于测试观测，不可驱动回收决策。
 */
void srcutorture_get_gp_data(struct srcu_struct *ssp, int *flags,
			     unsigned long *gp_seq)
{
	*flags = 0;
	*gp_seq = rcu_seq_current(&ssp->srcu_sup->srcu_gp_seq);
}
EXPORT_SYMBOL_GPL(srcutorture_get_gp_data);

/*
 * 扩容状态到诊断字符串的只读映射。末项是越界/未知兜底；数组不参与状态机，
 * 只用于 rcutorture 输出，因此字符串 lifetime 为整个内核运行期。
 */
static const char * const srcu_size_state_name[] = {
	"SRCU_SIZE_SMALL",
	"SRCU_SIZE_ALLOC",
	"SRCU_SIZE_WAIT_BARRIER",
	"SRCU_SIZE_WAIT_CALL",
	"SRCU_SIZE_WAIT_CBS1",
	"SRCU_SIZE_WAIT_CBS2",
	"SRCU_SIZE_WAIT_CBS3",
	"SRCU_SIZE_WAIT_CBS4",
	"SRCU_SIZE_BIG",
	"SRCU_SIZE_???",
};

/*
 * srcu_torture_stats_print() - 打印一个域的扩容、读者计数与回调概况
 * @ssp：借用域，甚至可能刚 cleanup；@tt/@tf：借用的日志前缀字符串。
 * 返回无值；统计使用 data_race 接受非一致快照，只供诊断。TORTURE 转换策略
 * 下打印末尾还会幂等请求 BIG，故该函数不只是纯 getter。
 */
void srcu_torture_stats_print(struct srcu_struct *ssp, char *tt, char *tf)
{
	int cpu;
	int idx;
	unsigned long s0 = 0, s1 = 0;
	int ss_state = READ_ONCE(ssp->srcu_sup->srcu_size_state);
	int ss_state_idx = ss_state;

	idx = ssp->srcu_ctrp - &ssp->sda->srcu_ctrs[0];
	if (ss_state < 0 || ss_state >= ARRAY_SIZE(srcu_size_state_name))
		ss_state_idx = ARRAY_SIZE(srcu_size_state_name) - 1;
	pr_alert("%s%s Tree SRCU g%ld state %d (%s)",
		 tt, tf, rcu_seq_current(&ssp->srcu_sup->srcu_gp_seq), ss_state,
		 srcu_size_state_name[ss_state_idx]);
	if (!ssp->sda) {
		// Called after cleanup_srcu_struct(), perhaps.
		/* sda 为 NULL 很可能是动态域已 cleanup；此时不能再做 per-CPU 遍历。 */
		pr_cont(" No per-CPU srcu_data structures (->sda == NULL).\n");
	} else {
		pr_cont(" per-CPU(idx=%d):", idx);
		for_each_possible_cpu(cpu) {
			unsigned long l0, l1;
			unsigned long u0, u1;
			long c0, c1;
			struct srcu_data *sdp;

			sdp = per_cpu_ptr(ssp->sda, cpu);
			/* 诊断允许与读者并发，data_race 明示此 unlock 快照不参与正确性。 */
			u0 = data_race(atomic_long_read(&sdp->srcu_ctrs[!idx].srcu_unlocks));
			/* 同上，当前活动槽的 unlock 也只用于近似统计。 */
			u1 = data_race(atomic_long_read(&sdp->srcu_ctrs[idx].srcu_unlocks));

			/*
			 * Make sure that a lock is always counted if the corresponding
			 * unlock is counted.
			 */
			/*
			 * 先采 unlock、rmb、再采 lock，避免诊断中出现“看见退出却没看见
			 * 对应进入”的反直觉负数；data_race 明示这里只求可读快照。
			 */
			smp_rmb();

			/* rmb 后采 lock；data_race 接受值在打印期间继续增长。 */
			l0 = data_race(atomic_long_read(&sdp->srcu_ctrs[!idx].srcu_locks));
			/* 当前活动槽同样是非原子的一致性快照，不用于释放对象。 */
			l1 = data_race(atomic_long_read(&sdp->srcu_ctrs[idx].srcu_locks));

			c0 = l0 - u0;
			c1 = l1 - u1;
			pr_cont(" %d(%ld,%ld %c)",
				cpu, c0, c1,
				"C."[rcu_segcblist_empty(&sdp->srcu_cblist)]);
			s0 += c0;
			s1 += c1;
		}
		pr_cont(" T(%ld,%ld)\n", s0, s1);
	}
	if (SRCU_SIZING_IS_TORTURE())
		srcu_transition_to_big(ssp);
}
EXPORT_SYMBOL_GPL(srcu_torture_stats_print);

/*
 * srcu_bootup_announce() - 启动早期打印 Tree SRCU 策略参数
 * 入参：无。返回固定 0 供 early_initcall 继续；只读模块参数并输出日志，
 * __init 表示启动后代码可回收，不参与运行期同步。
 */
static int __init srcu_bootup_announce(void)
{
	pr_info("Hierarchical SRCU implementation.\n");
	if (exp_holdoff != DEFAULT_SRCU_EXP_HOLDOFF)
		pr_info("\tNon-default auto-expedite holdoff of %lu ns.\n", exp_holdoff);
	if (srcu_retry_check_delay != SRCU_DEFAULT_RETRY_CHECK_DELAY)
		pr_info("\tNon-default retry check delay of %lu us.\n", srcu_retry_check_delay);
	if (srcu_max_nodelay != SRCU_DEFAULT_MAX_NODELAY)
		pr_info("\tNon-default max no-delay of %lu.\n", srcu_max_nodelay);
	pr_info("\tMax phase no-delay instances is %lu.\n", srcu_max_nodelay_phase);
	return 0;
}
early_initcall(srcu_bootup_announce);

/*
 * srcu_init() - RCU workqueue/timer 就绪后切换 SRCU 出早期启动模式
 * 入参：无；返回无值。由 RCU 初始化主线调用，此时仍按启动串行上下文执行。
 * 先确定 SMALL/BIG 策略，再发布 srcu_init_done，最后搬运 srcu_boot_list；
 * 此顺序保证新 call_srcu 走 irq_work 时底层队列已存在。
 */
void __init srcu_init(void)
{
	struct srcu_usage *sup;

	/* Decide on srcu_struct-size strategy. */
	/* AUTO 在大 CPU 系统直接 INIT→BIG，小系统保留 SMALL 并以竞争触发扩容。 */
	if (SRCU_SIZING_IS(SRCU_SIZING_AUTO)) {
		if (nr_cpu_ids >= big_cpu_lim) {
			convert_to_big = SRCU_SIZING_INIT; // Don't bother waiting for contention.
			/* CPU 数已高于阈值，直接扩容比先承受全局锁竞争更合算。 */
			pr_info("%s: Setting srcu_struct sizes to big.\n", __func__);
		} else {
			convert_to_big = SRCU_SIZING_NONE | SRCU_SIZING_CONTEND;
			pr_info("%s: Setting srcu_struct sizes based on contention.\n", __func__);
		}
	}

	/*
	 * Once that is set, call_srcu() can follow the normal path and
	 * queue delayed work. This must follow RCU workqueues creation
	 * and timers initialization.
	 */
	/*
	 * 原注释强调发布必须晚于 RCU workqueue 与 timer 初始化。之后逐项摘下早期
	 * 链表；INIT 策略先标 ALLOC，再把 work 投递到可睡眠上下文完成建树。
	 */
	srcu_init_done = true;
	while (!list_empty(&srcu_boot_list)) {
		sup = list_first_entry(&srcu_boot_list, struct srcu_usage,
				      work.work.entry);
		list_del_init(&sup->work.work.entry);
		if (SRCU_SIZING_IS(SRCU_SIZING_INIT) &&
		    sup->srcu_size_state == SRCU_SIZE_SMALL)
			sup->srcu_size_state = SRCU_SIZE_ALLOC;
		queue_work(rcu_gp_wq, &sup->work.work);
	}
}

#ifdef CONFIG_MODULES
/* 模块含静态 DEFINE_SRCU 时，装载器通过下列 notifier 补分配/清理 per-CPU 数据。 */

/* Initialize any global-scope srcu_struct structures used by this module. */
/*
 * srcu_module_coming() - 模块进入 COMING 时为每个静态 SRCU 域分配 sda
 * @mod：借用、尚未完全发布的模块。返回 0 或 -ENOMEM；成功的 sda ownership
 * 归对应 ssp，模块离开时释放。失败发生在中途时，已分配项由模块装载失败清理
 * 协议接管；本函数可睡眠。
 */
static int srcu_module_coming(struct module *mod)
{
	int i;
	struct srcu_struct *ssp;
	struct srcu_struct **sspp = mod->srcu_struct_ptrs;

	for (i = 0; i < mod->num_srcu_structs; i++) {
		ssp = *(sspp++);
		ssp->sda = alloc_percpu(struct srcu_data);
		if (WARN_ON_ONCE(!ssp->sda))
			return -ENOMEM;
		ssp->srcu_ctrp = &ssp->sda->srcu_ctrs[0];
	}
	return 0;
}

/* Clean up any global-scope srcu_struct structures used by this module. */
/*
 * srcu_module_going() - 模块 GOING 时停止并释放其静态 SRCU 域运行资源
 * @mod：借用、已停止外部新调用的模块；返回无值，可睡眠。
 * 若域曾完成首次初始化，先 cleanup 动态树/work；确认无读者后释放模块装载时
 * 分配的 sda。活动读者只 WARN 并保留，避免模块卸载路径制造 UAF。
 */
static void srcu_module_going(struct module *mod)
{
	int i;
	struct srcu_struct *ssp;
	struct srcu_struct **sspp = mod->srcu_struct_ptrs;

	for (i = 0; i < mod->num_srcu_structs; i++) {
		ssp = *(sspp++);
		/* acquire 与首次初始化 release 配对，决定运行字段是否已完整建立。 */
		if (!rcu_seq_state(smp_load_acquire(&ssp->srcu_sup->srcu_gp_seq_needed)) &&
		    !WARN_ON_ONCE(!ssp->srcu_sup->sda_is_static))
			cleanup_srcu_struct(ssp);
		if (!WARN_ON(srcu_readers_active(ssp)))
			free_percpu(ssp->sda);
	}
}

/* Handle one module, either coming or going. */
/*
 * srcu_module_notify() - 模块状态通知分派器
 * @self：借用 notifier，自身状态不变；@val：MODULE_STATE_*；@data：借用 mod。
 * COMING 返回分配结果，GOING 执行清理后返回 0，其他状态无操作。可睡眠。
 */
static int srcu_module_notify(struct notifier_block *self,
			      unsigned long val, void *data)
{
	struct module *mod = data;
	int ret = 0;

	switch (val) {
	case MODULE_STATE_COMING:
		ret = srcu_module_coming(mod);
		break;
	case MODULE_STATE_GOING:
		srcu_module_going(mod);
		break;
	default:
		break;
	}
	return ret;
}

/* 全局 notifier 对象由 late init 注册，生命周期覆盖所有后续模块装卸。 */
static struct notifier_block srcu_module_nb = {
	.notifier_call = srcu_module_notify,
	.priority = 0,
};

/*
 * init_srcu_module_notifier() - 注册 SRCU 模块生命周期通知
 * 入参：无。返回 register_module_notifier 的 0/errno；失败仅告警，意味着含
 * 静态 SRCU 的模块不能获得本文件的自动 sda 生命周期管理。
 */
static __init int init_srcu_module_notifier(void)
{
	int ret;

	ret = register_module_notifier(&srcu_module_nb);
	if (ret)
		pr_warn("Failed to register srcu module notifier\n");
	return ret;
}
late_initcall(init_srcu_module_notifier);

#endif /* #ifdef CONFIG_MODULES */
/* 无模块配置时静态内核内 SRCU 仍由正常初始化路径管理，不需要装卸通知器。 */
