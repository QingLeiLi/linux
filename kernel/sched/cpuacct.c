// SPDX-License-Identifier: GPL-2.0

/*
 * CPU accounting code for task groups.
 *
 * Based on the work by Paul Menage (menage@google.com) and Balbir Singh
 * (balbir@in.ibm.com).
 */
/* cpuacct v1 按 cgroup 层级累计任务执行时间，并提供总量、用户/系统及逐 CPU 文本接口。 */
#include <linux/sched/cputime.h>
#include "sched.h"

/* Time spent by the tasks of the CPU accounting group executing in ... */
/* 统计任务在用户态与内核态消耗的时间；NSTATS 既是数量，也被内部用作“总执行时间”选择器。 */
enum cpuacct_stat_index {
	CPUACCT_STAT_USER,	/* ... user mode */
	/* 任务执行在用户态的时间。 */
	CPUACCT_STAT_SYSTEM,	/* ... kernel mode */
	/* 任务执行在内核态（含 IRQ/softirq 归类）的时间。 */

	CPUACCT_STAT_NSTATS,
};

/* stat 文件字段名，索引必须与 cpuacct_stat_index 对齐。 */
static const char * const cpuacct_stat_desc[] = {
	[CPUACCT_STAT_USER] = "user",
	[CPUACCT_STAT_SYSTEM] = "system",
};

/* track CPU usage of a group of tasks and its child groups */
/*
 * cpuacct 嵌入 cgroup css，并拥有每 CPU 总执行纳秒 cpuusage 与 kernel_cpustat 分类数组。
 * charge 会沿父链累计，所以每个对象包含本组及后代；动态对象由 css_alloc/free 管理。
 */
struct cpuacct {
	struct cgroup_subsys_state	css;
	/* cpuusage holds pointer to a u64-type object on every CPU */
	/* cpuusage 指向每个 CPU 一个 u64 的动态 per-CPU 存储。 */
	u64 __percpu	*cpuusage;
	struct kernel_cpustat __percpu	*cpustat;
};

/* 把可空 css 转回 cpuacct；NULL 保持 NULL，返回借用指针。 */
static inline struct cpuacct *css_ca(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct cpuacct, css) : NULL;
}

/* Return CPU accounting group to which this task belongs */
/* 返回 @tsk 当前 cpuacct css 对应对象；task_css 生命周期由 cgroup/task 协议稳定。 */
static inline struct cpuacct *task_ca(struct task_struct *tsk)
{
	return css_ca(task_css(tsk, cpuacct_cgrp_id));
}

/* 返回 @ca 的父 cpuacct，root 的 css.parent 为 NULL，因而终止层级遍历。 */
static inline struct cpuacct *parent_ca(struct cpuacct *ca)
{
	return css_ca(ca->css.parent);
}

/* root 总执行时间复用静态 per-CPU 槽；分类统计直接别名全局 kernel_cpustat。 */
static DEFINE_PER_CPU(u64, root_cpuacct_cpuusage);
static struct cpuacct root_cpuacct = {
	.cpustat	= &kernel_cpustat,
	.cpuusage	= &root_cpuacct_cpuusage,
};

/* Create a new CPU accounting group */
/*
 * 创建 css：parent_css 为空返回永久 root；普通组依次分配容器、总量和分类 per-CPU
 * 存储，成功返回内嵌 css ownership 给 cgroup core。任一步失败逆序释放并返回
 * ERR_PTR(-ENOMEM)。GFP_KERNEL 可睡眠，未发布半初始化对象。
 */
static struct cgroup_subsys_state *
cpuacct_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct cpuacct *ca;

	if (!parent_css)
		return &root_cpuacct.css;

	ca = kzalloc_obj(*ca);
	if (!ca)
		goto out;

	ca->cpuusage = alloc_percpu(u64);
	if (!ca->cpuusage)
		goto out_free_ca;

	ca->cpustat = alloc_percpu(struct kernel_cpustat);
	if (!ca->cpustat)
		goto out_free_cpuusage;

	return &ca->css;

out_free_cpuusage:
	/* 分类数组失败时先释放已取得的总量 per-CPU 存储。 */
	free_percpu(ca->cpuusage);
out_free_ca:
	kfree(ca);
out:
	return ERR_PTR(-ENOMEM);
}

/* Destroy an existing CPU accounting group */
/* cgroup core 在 css 无用户后释放动态组的两份 per-CPU 存储和容器；root 不走此路径。 */
static void cpuacct_css_free(struct cgroup_subsys_state *css)
{
	struct cpuacct *ca = css_ca(css);

	free_percpu(ca->cpustat);
	free_percpu(ca->cpuusage);
	kfree(ca);
}

/*
 * 读取 @ca 在 @cpu 的用户、系统或总执行纳秒。index==NSTATS 合法表示 cpuusage；
 * 越界告警返回 0。32 位平台以目标 rq 锁防止 u64 撕裂，64 位直接读。参数借用，
 * 返回近似统计快照，不承诺跨字段一致性。
 */
static u64 cpuacct_cpuusage_read(struct cpuacct *ca, int cpu,
				 enum cpuacct_stat_index index)
{
	u64 *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);
	u64 *cpustat = per_cpu_ptr(ca->cpustat, cpu)->cpustat;
	u64 data;

	/*
	 * We allow index == CPUACCT_STAT_NSTATS here to read
	 * the sum of usages.
	 */
	/* NSTATS 在此特例读取总执行时间，而不是分类数组下标。 */
	if (WARN_ON_ONCE(index > CPUACCT_STAT_NSTATS))
		return 0;

#ifndef CONFIG_64BIT
	/*
	 * Take rq->lock to make 64-bit read safe on 32-bit platforms.
	 */
	/* 32 位平台取 rq 锁保证 64 位读不撕裂。 */
	raw_spin_rq_lock_irq(cpu_rq(cpu));
#endif

	switch (index) {
	case CPUACCT_STAT_USER:
		data = cpustat[CPUTIME_USER] + cpustat[CPUTIME_NICE];
		break;
	case CPUACCT_STAT_SYSTEM:
		data = cpustat[CPUTIME_SYSTEM] + cpustat[CPUTIME_IRQ] +
			cpustat[CPUTIME_SOFTIRQ];
		break;
	case CPUACCT_STAT_NSTATS:
		data = *cpuusage;
		break;
	}

#ifndef CONFIG_64BIT
	raw_spin_rq_unlock_irq(cpu_rq(cpu));
#endif

	return data;
}

/*
 * 把动态 @ca 的指定 CPU 总量及 user/nice/system/irq/softirq 分类清零；root 全局统计
 * 禁止重置并直接返回。32 位以 rq 锁保证 u64 写原子，函数无返回值、不睡眠。
 */
static void cpuacct_cpuusage_write(struct cpuacct *ca, int cpu)
{
	u64 *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);
	u64 *cpustat = per_cpu_ptr(ca->cpustat, cpu)->cpustat;

	/* Don't allow to reset global kernel_cpustat */
	/* 不允许通过 cgroup 文件重置全局 kernel_cpustat。 */
	if (ca == &root_cpuacct)
		return;

#ifndef CONFIG_64BIT
	/*
	 * Take rq->lock to make 64-bit write safe on 32-bit platforms.
	 */
	/* 32 位平台取 rq 锁防止并发计费看到撕裂值。 */
	raw_spin_rq_lock_irq(cpu_rq(cpu));
#endif
	*cpuusage = 0;
	cpustat[CPUTIME_USER] = cpustat[CPUTIME_NICE] = 0;
	cpustat[CPUTIME_SYSTEM] = cpustat[CPUTIME_IRQ] = 0;
	cpustat[CPUTIME_SOFTIRQ] = 0;

#ifndef CONFIG_64BIT
	raw_spin_rq_unlock_irq(cpu_rq(cpu));
#endif
}

/* Return total CPU usage (in nanoseconds) of a group */
/* 汇总所有 possible CPU 的指定分类，返回纳秒；读取是逐 CPU 近似快照。 */
static u64 __cpuusage_read(struct cgroup_subsys_state *css,
			   enum cpuacct_stat_index index)
{
	struct cpuacct *ca = css_ca(css);
	u64 totalcpuusage = 0;
	int i;

	for_each_possible_cpu(i)
		totalcpuusage += cpuacct_cpuusage_read(ca, i, index);

	return totalcpuusage;
}

/* cgroup usage_user 的 read_u64 wrapper；@cft 未使用，返回用户态纳秒。 */
static u64 cpuusage_user_read(struct cgroup_subsys_state *css,
			      struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_USER);
}

/* usage_sys wrapper，返回 system+irq+softirq 纳秒。 */
static u64 cpuusage_sys_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_SYSTEM);
}

/* usage wrapper，返回调度器累计的总执行纳秒。 */
static u64 cpuusage_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_NSTATS);
}

/*
 * usage 写接口只接受 0 作为 reset；非零返回 -EINVAL。逐 possible CPU 清零动态组，
 * root 写入因内部保护成为成功的无操作。操作可跨 CPU 得到非原子重置快照。
 */
static int cpuusage_write(struct cgroup_subsys_state *css, struct cftype *cft,
			  u64 val)
{
	struct cpuacct *ca = css_ca(css);
	int cpu;

	/*
	 * Only allow '0' here to do a reset.
	 */
	/* 仅允许写 0 触发重置。 */
	if (val)
		return -EINVAL;

	for_each_possible_cpu(cpu)
		cpuacct_cpuusage_write(ca, cpu);

	return 0;
}

/* 输出指定分类的逐 possible CPU 数值，以空格分隔并换行；返回 0，seq_file 记录溢出。 */
static int __cpuacct_percpu_seq_show(struct seq_file *m,
				     enum cpuacct_stat_index index)
{
	struct cpuacct *ca = css_ca(seq_css(m));
	u64 percpu;
	int i;

	for_each_possible_cpu(i) {
		percpu = cpuacct_cpuusage_read(ca, i, index);
		seq_printf(m, "%llu ", (unsigned long long) percpu);
	}
	seq_printf(m, "\n");
	return 0;
}

/* usage_percpu_user 展示 wrapper；V 未使用。 */
static int cpuacct_percpu_user_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_USER);
}

/* usage_percpu_sys 展示 wrapper；V 未使用。 */
static int cpuacct_percpu_sys_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_SYSTEM);
}

/* usage_percpu 总执行时间展示 wrapper；V 未使用。 */
static int cpuacct_percpu_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_NSTATS);
}

/* 输出 usage_all 表头以及每个 possible CPU 的 user/system 纳秒行；读取为近似快照。 */
static int cpuacct_all_seq_show(struct seq_file *m, void *V)
{
	struct cpuacct *ca = css_ca(seq_css(m));
	int index;
	int cpu;

	seq_puts(m, "cpu");
	for (index = 0; index < CPUACCT_STAT_NSTATS; index++)
		seq_printf(m, " %s", cpuacct_stat_desc[index]);
	seq_puts(m, "\n");

	for_each_possible_cpu(cpu) {
		seq_printf(m, "%d", cpu);
		for (index = 0; index < CPUACCT_STAT_NSTATS; index++)
			seq_printf(m, " %llu",
				   cpuacct_cpuusage_read(ca, cpu, index));
		seq_puts(m, "\n");
	}
	return 0;
}

/*
 * 输出 legacy stat 的 user/system clock ticks。先跨 CPU 汇总原始分类与 sum_exec_runtime，
 * 再由 cputime_adjust 结合 cgroup prev_cputime 单调调整 user/system，最后纳秒转 clock_t。
 * prev_cputime 是可观察状态更新；返回 0，读取不保证与并发计费原子一致。
 */
static int cpuacct_stats_show(struct seq_file *sf, void *v)
{
	struct cpuacct *ca = css_ca(seq_css(sf));
	struct task_cputime cputime;
	u64 val[CPUACCT_STAT_NSTATS];
	int cpu;
	int stat;

	memset(&cputime, 0, sizeof(cputime));
	for_each_possible_cpu(cpu) {
		u64 *cpustat = per_cpu_ptr(ca->cpustat, cpu)->cpustat;

		cputime.utime += cpustat[CPUTIME_USER];
		cputime.utime += cpustat[CPUTIME_NICE];
		cputime.stime += cpustat[CPUTIME_SYSTEM];
		cputime.stime += cpustat[CPUTIME_IRQ];
		cputime.stime += cpustat[CPUTIME_SOFTIRQ];

		cputime.sum_exec_runtime += *per_cpu_ptr(ca->cpuusage, cpu);
	}

	cputime_adjust(&cputime, &seq_css(sf)->cgroup->prev_cputime,
		&val[CPUACCT_STAT_USER], &val[CPUACCT_STAT_SYSTEM]);

	for (stat = 0; stat < CPUACCT_STAT_NSTATS; stat++) {
		seq_printf(sf, "%s %llu\n", cpuacct_stat_desc[stat],
			nsec_to_clock_t(val[stat]));
	}

	return 0;
}

/* cpuacct v1 文件表；回调 ownership 由 cgroup core 管理，末尾空项终止。 */
static struct cftype files[] = {
	{
		.name = "usage",
		.read_u64 = cpuusage_read,
		.write_u64 = cpuusage_write,
	},
	{
		.name = "usage_user",
		.read_u64 = cpuusage_user_read,
	},
	{
		.name = "usage_sys",
		.read_u64 = cpuusage_sys_read,
	},
	{
		.name = "usage_percpu",
		.seq_show = cpuacct_percpu_seq_show,
	},
	{
		.name = "usage_percpu_user",
		.seq_show = cpuacct_percpu_user_seq_show,
	},
	{
		.name = "usage_percpu_sys",
		.seq_show = cpuacct_percpu_sys_seq_show,
	},
	{
		.name = "usage_all",
		.seq_show = cpuacct_all_seq_show,
	},
	{
		.name = "stat",
		.seq_show = cpuacct_stats_show,
	},
	{ }	/* terminate */
	/* 空描述符终止表扫描。 */
};

/*
 * charge this task's execution time to its accounting group.
 *
 * called with rq->lock held.
 */
/*
 * 在 rq 锁下把 @tsk 本次执行纳秒 @cputime 计入其 cpuacct 及全部祖先的当前 CPU 槽。
 * task/层级借用且不取 css 引用；root 也被累计。无返回值、不可睡眠。
 */
void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
	unsigned int cpu = task_cpu(tsk);
	struct cpuacct *ca;

	lockdep_assert_rq_held(cpu_rq(cpu));

	for (ca = task_ca(tsk); ca; ca = parent_ca(ca))
		*per_cpu_ptr(ca->cpuusage, cpu) += cputime;
}

/*
 * Add user/system time to cpuacct.
 *
 * Note: it's the caller that updates the account of the root cgroup.
 */
/*
 * 把 @val 加到 @tsk 动态 cpuacct 及祖先的当前 CPU 分类 @index；到 root 前停止，因为
 * 调用者另行更新全局 kernel_cpustat。调用者保证当前 CPU 固定及 index 有效；无返回值。
 */
void cpuacct_account_field(struct task_struct *tsk, int index, u64 val)
{
	struct cpuacct *ca;

	for (ca = task_ca(tsk); ca != &root_cpuacct; ca = parent_ca(ca))
		__this_cpu_add(ca->cpustat->cpustat[index], val);
}

/* cpuacct cgroup 子系统：早期初始化，使用上述 css 生命周期与 legacy 文件表。 */
struct cgroup_subsys cpuacct_cgrp_subsys = {
	.css_alloc	= cpuacct_css_alloc,
	.css_free	= cpuacct_css_free,
	.legacy_cftypes	= files,
	.early_init	= true,
};
