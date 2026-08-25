// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/schedstat implementation
 */
/*
 * 本文件同时承担两类工作：在调度器热路径中维护实体的等待、睡眠和阻塞统计，
 * 以及通过 /proc/schedstat 导出每 CPU 和调度域计数器。前者由各调度类在持有
 * runqueue 锁时调用；后者是诊断接口，只提供可能跨字段变化的观测快照。
 */
#include "sched.h"

/*
 * 记录实体开始在 runqueue 上等待 CPU 的时刻。rq 是当前队列且调用者持有其锁；
 * stats 既可能属于任务，也可能属于组调度实体，p 为 NULL 表示后者。
 *
 * 普通入队直接保存 rq_clock()。迁移任务则由 __update_stats_wait_end() 把迁移前
 * 已等待时长暂存在 wait_start 中；新队列时钟晚于该值时，这里以“当前时钟减去
 * 已等待时长”重建起点。条件同时避免时钟域偏差造成无符号下溢；极端偏差下保留
 * 当前时刻，宁可少计也不制造巨大的等待值。函数不睡眠，也不取得额外锁。
 */
void __update_stats_wait_start(struct rq *rq, struct task_struct *p,
			       struct sched_statistics *stats)
{
	/* wait_start 是当前队列时钟；prev_wait_start 可能是迁移携带的等待时长。 */
	u64 wait_start, prev_wait_start;

	wait_start = rq_clock(rq);
	prev_wait_start = schedstat_val(stats->wait_start);

	if (p && likely(wait_start > prev_wait_start))
		wait_start -= prev_wait_start;

	/* 调用者已通过 schedstat_enabled() 门控，因此使用无分支的内部写宏。 */
	__schedstat_set(stats->wait_start, wait_start);
}

/*
 * 结束一次 runqueue 等待并累计最大值、次数和总时长。调用上下文和所有权约束与
 * __update_stats_wait_start() 相同；调用者还保证 wait_start 已初始化，避免运行时
 * 开启 schedstats 后把零起点误算成一次超长等待。
 */
void __update_stats_wait_end(struct rq *rq, struct task_struct *p,
			     struct sched_statistics *stats)
{
	/* 同一 rq 时钟域内相减得到本段等待时间；迁移分支会把它带到目标 rq。 */
	u64 delta = rq_clock(rq) - schedstat_val(stats->wait_start);

	if (p) {
		if (task_on_rq_migrating(p)) {
			/*
			 * Preserve migrating task's wait time so wait_start
			 * time stamp can be adjusted to accumulate wait time
			 * prior to migration.
			 */
			/*
			 * 保留迁移任务已经等待的时长，使目标队列能够调整 wait_start
			 * 时间戳，并把迁移前的等待时间继续累计进去。
			 *
			 * 此处不能按普通离队结算：任务仍然可运行，只是换了 rq，提前增加
			 * wait_count 会把一次连续等待拆成两次。返回时 wait_start 的临时
			 * 含义由“时间戳”变为“已等待时长”，在目标 rq 入队时再恢复。
			 */
			__schedstat_set(stats->wait_start, delta);

			return;
		}

		/* 仅真实任务有 tracepoint；组实体仍会更新自身的聚合统计。 */
		trace_sched_stat_wait(p, delta);
	}

	/* 最大值、样本数和总量属于同一次结算，rq 锁保证更新路径的原子性。 */
	__schedstat_set(stats->wait_max,
			max(schedstat_val(stats->wait_max), delta));
	__schedstat_inc(stats->wait_count);
	__schedstat_add(stats->wait_sum, delta);
	__schedstat_set(stats->wait_start, 0);
}

/*
 * 实体从睡眠或阻塞状态重新入队时结算离队时间。sleep_start 表示可中断睡眠，
 * block_start 表示不可中断阻塞；任务指针为空时只维护组实体统计，不触发任务级
 * 延迟记账和 tracepoint。调用者持有 rq 锁，函数不睡眠。
 */
void __update_stats_enqueue_sleeper(struct rq *rq, struct task_struct *p,
				    struct sched_statistics *stats)
{
	/* 先取快照；对应分支结算后会清零起点，防止同一段时间重复累计。 */
	u64 sleep_start, block_start;

	sleep_start = schedstat_val(stats->sleep_start);
	block_start = schedstat_val(stats->block_start);

	if (sleep_start) {
		/* 起点来自任务离队时的 rq_clock()，正常情况下与当前 rq 同域。 */
		u64 delta = rq_clock(rq) - sleep_start;

		/* 跨 CPU 时钟偏差表现为有符号负值时夹到零，避免无符号巨值污染统计。 */
		if ((s64)delta < 0)
			delta = 0;

		if (unlikely(delta > schedstat_val(stats->sleep_max)))
			__schedstat_set(stats->sleep_max, delta);

		__schedstat_set(stats->sleep_start, 0);
		__schedstat_add(stats->sum_sleep_runtime, delta);

		if (p) {
			/* 延迟记账接口使用微秒，第三个参数 1 标识可中断睡眠。 */
			account_scheduler_latency(p, delta >> 10, 1);
			trace_sched_stat_sleep(p, delta);
		}
	}

	if (block_start) {
		/* 不可中断阻塞使用独立起点，但采用相同的时钟偏差防护。 */
		u64 delta = rq_clock(rq) - block_start;

		if ((s64)delta < 0)
			delta = 0;

		if (unlikely(delta > schedstat_val(stats->block_max)))
			__schedstat_set(stats->block_max, delta);

		__schedstat_set(stats->block_start, 0);
		/* 总睡眠时间包含阻塞时间，sum_block_runtime 只记录阻塞子集。 */
		__schedstat_add(stats->sum_sleep_runtime, delta);
		__schedstat_add(stats->sum_block_runtime, delta);

		if (p) {
			if (p->in_iowait) {
				/* in_iowait 只把本次阻塞进一步归入 I/O 等待子集。 */
				__schedstat_add(stats->iowait_sum, delta);
				__schedstat_inc(stats->iowait_count);
				trace_sched_stat_iowait(p, delta);
			}

			trace_sched_stat_blocked(p, delta);

			/* 第三个参数 0 标识不可中断阻塞；右移 10 是纳秒到近似微秒。 */
			account_scheduler_latency(p, delta >> 10, 0);
		}
	}
}

/*
 * Current schedstat API version.
 *
 * Bump this up when changing the output format or the meaning of an existing
 * format, so that tools can adapt (or abort)
 */
/*
 * 当前 schedstat API 版本。修改输出字段、顺序或既有字段语义时必须递增，
 * 让用户态工具选择适配或拒绝解析；它是文本 ABI 的版本，不是内核内部版本。
 */
#define SCHEDSTAT_VERSION 17

/*
 * seq_file 的 show 回调。v==1 是头部哨兵；其余 token 以 cpu+2 编码。
 * CPU/rq 计数器在调度热路径更新，这里不取 rq 锁，因此一次输出中的字段可能来自
 * 略有差异的时刻。该接口用于趋势和差值观测，不能作为同步或一致性判据。
 */
static int show_schedstat(struct seq_file *seq, void *v)
{
	/* cpu 只在非头部 token 分支赋值。 */
	int cpu;

	if (v == (void *)1) {
		/* 每次从文件起点读取时先给出解析版本和当前 jiffies 时间戳。 */
		seq_printf(seq, "version %d\n", SCHEDSTAT_VERSION);
		seq_printf(seq, "timestamp %lu\n", jiffies);
	} else {
		struct rq *rq;
		struct sched_domain *sd;
		/* 同一 CPU 的调度域按从内到外的遍历次序编号。 */
		int dcount = 0;
		/* seq token 的 2 偏移为头部保留 1，并让 CPU 0 不等于 NULL。 */
		cpu = (unsigned long)(v - 2);
		rq = cpu_rq(cpu);

		/* runqueue-specific stats */
		/*
		 * 输出 runqueue 级统计。第二个数值字段固定为 0，是旧 O(1)
		 * 调度器 array_exp 字段的 ABI 占位；其余字段顺序见 sched-stats.rst。
		 */
		seq_printf(seq,
		    "cpu%d %u 0 %u %u %u %u %llu %llu %lu",
		    cpu, rq->yld_count,
		    rq->sched_count, rq->sched_goidle,
		    rq->ttwu_count, rq->ttwu_local,
		    rq->rq_cpu_time,
		    rq->rq_sched_info.run_delay, rq->rq_sched_info.pcount);

		seq_printf(seq, "\n");

		/* domain-specific stats */
		/*
		 * 输出调度域级统计。RCU 只保护 sched_domain 链及其 cpumask 生命周期，
		 * 不会把这些持续变化的计数器冻结成原子快照。
		 */
		rcu_read_lock();
		for_each_domain(cpu, sd) {
			/* busy、idle、newly-idle 三类各输出同样的 11 个负载均衡字段。 */
			enum cpu_idle_type itype;

			seq_printf(seq, "domain%d %s %*pb", dcount++, sd->name,
				   cpumask_pr_args(sched_domain_span(sd)));
			for (itype = 0; itype < CPU_MAX_IDLE_TYPES; itype++) {
				seq_printf(seq, " %u %u %u %u %u %u %u %u %u %u %u",
				    sd->lb_count[itype],
				    sd->lb_balanced[itype],
				    sd->lb_failed[itype],
				    /*
				     * 前三项记录尝试、已均衡和失败次数；下面四项分别累计
				     * load/util/task/misfit 失衡量，最后四项记录拉取结果与失败原因。
				     */
				    sd->lb_imbalance_load[itype],
				    sd->lb_imbalance_util[itype],
				    sd->lb_imbalance_task[itype],
				    sd->lb_imbalance_misfit[itype],
				    sd->lb_gained[itype],
				    sd->lb_hot_gained[itype],
				    sd->lb_nobusyq[itype],
				    sd->lb_nobusyg[itype]);
			}
			/* 最后 12 项依次为 active balance、exec/fork 占位和唤醒迁移统计。 */
			seq_printf(seq,
				   " %u %u %u %u %u %u %u %u %u %u %u %u\n",
			    sd->alb_count, sd->alb_failed, sd->alb_pushed,
			    sd->sbe_count, sd->sbe_balanced, sd->sbe_pushed,
			    sd->sbf_count, sd->sbf_balanced, sd->sbf_pushed,
			    sd->ttwu_wake_remote, sd->ttwu_move_affine,
			    sd->ttwu_move_balance);
		}
		rcu_read_unlock();
	}
	/* seq_printf 自行在 seq_file 中记录溢出；show 协议以 0 表示该项处理完成。 */
	return 0;
}

/*
 * This iterator needs some explanation.
 * It returns 1 for the header position.
 * This means 2 is cpu 0.
 * In a hotplugged system some CPUs, including cpu 0, may be missing so we have
 * to use cpumask_* to iterate over the CPUs.
 */
/*
 * 该迭代器需要额外说明：位置 0 返回头部 token 1，所以 token 2 才表示 CPU 0。
 * 热插拔系统可能缺少任意 CPU（包括 CPU 0），不能把位置直接当 CPU 编号，必须
 * 用 cpumask_* 在 cpu_online_mask 中寻找下一项。遍历不持有 CPU hotplug 锁，
 * 因而并发上下线时允许得到一个近似视图，但 token 始终经过 nr_cpu_ids 校验。
 */
static void *schedstat_start(struct seq_file *file, loff_t *offset)
{
	/* n 是 seq_file 请求的逻辑位置；file 无需参与定位。 */
	unsigned long n = *offset;

	/* 位置 0 专用于版本和时间戳头部。 */
	if (n == 0)
		return (void *) 1;

	n--;

	if (n > 0)
		/* 从上次 CPU 的后一项继续，跳过 offline CPU 和编号空洞。 */
		n = cpumask_next(n - 1, cpu_online_mask);
	else
		/* 头部之后从当前第一个 online CPU 开始，不假定 CPU 0 在线。 */
		n = cpumask_first(cpu_online_mask);

	/* 把可能跳跃的 CPU 编号规范化回 seq_file 的下次逻辑位置。 */
	*offset = n + 1;

	if (n < nr_cpu_ids)
		/* 加 2 同时避开 NULL 和头部哨兵；show 中会逆变换。 */
		return (void *)(unsigned long)(n + 2);

	/* cpumask 搜索返回越界哨兵时结束遍历。 */
	return NULL;
}

/* 推进一个逻辑位置后复用 start 的热插拔感知定位规则。 */
static void *schedstat_next(struct seq_file *file, void *data, loff_t *offset)
{
	(*offset)++;

	return schedstat_start(file, offset);
}

/* 迭代期间没有跨回调持有锁或引用，因此 stop 无需释放资源。 */
static void schedstat_stop(struct seq_file *file, void *data)
{
}

/* seq_file 用这张只读操作表完成 seek、分段读取和 EOF 处理。 */
static const struct seq_operations schedstat_sops = {
	.start = schedstat_start,
	.next  = schedstat_next,
	.stop  = schedstat_stop,
	.show  = show_schedstat,
};

/*
 * 在子系统初始化阶段注册只读的 /proc/schedstat。权限参数 0 采用 procfs 默认模式；
 * 父目录为 NULL 表示根目录。这里沿用历史接口，未检查 proc_create_seq() 返回值，
 * 因而即使内存不足导致文件未创建，初始化仍返回成功，失败表现为用户态看不到节点。
 */
static int __init proc_schedstat_init(void)
{
	proc_create_seq("schedstat", 0, NULL, &schedstat_sops);
	return 0;
}
/* procfs 已可用后执行注册；该函数仅初始化期调用，代码随后可回收。 */
subsys_initcall(proc_schedstat_init);
