// SPDX-License-Identifier: GPL-2.0-or-later
/* delayacct.c - per-task delay accounting
 *
 * Copyright (C) Shailabh Nagar, IBM Corp. 2006
 */

#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/cputime.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/taskstats.h>
#include <linux/sysctl.h>
#include <linux/delayacct.h>
#include <linux/module.h>

/*
 * 延迟记账以成对的 start/end 钩子记录任务等待块 I/O、换入、直接回收、抖动、
 * 内存规整、写保护复制和 IRQ 的耗时。静态键让禁用时的调用点成本接近一个
 * 不跳转分支；task_delay_info 按任务可选分配，内部 raw spinlock 保护每组
 * count/total/min/max/timestamp 快照，taskstats 再把这些值导出给用户空间。
 */

#define UPDATE_DELAY(type) \
do { \
	d->type##_delay_max = tsk->delays->type##_delay_max; \
	d->type##_delay_min = tsk->delays->type##_delay_min; \
	d->type##_delay_max_ts.tv_sec = tsk->delays->type##_delay_max_ts.tv_sec; \
	d->type##_delay_max_ts.tv_nsec = tsk->delays->type##_delay_max_ts.tv_nsec; \
	tmp = d->type##_delay_total + tsk->delays->type##_delay; \
	d->type##_delay_total = (tmp < d->type##_delay_total) ? 0 : tmp; \
	d->type##_count += tsk->delays->type##_count; \
} while (0)

DEFINE_STATIC_KEY_FALSE(delayacct_key);
int delayacct_on __read_mostly;	/* Delay accounting turned on/off */
/* delayacct_on 是管理状态：0 关闭，1 开启。 */
struct kmem_cache *delayacct_cache;

static void set_delayacct(bool enabled)
{
	/* 先后顺序确保打开时先修补静态分支，关闭时先阻止新记账再撤销分支。 */
	if (enabled) {
		static_branch_enable(&delayacct_key);
		delayacct_on = 1;
	} else {
		delayacct_on = 0;
		static_branch_disable(&delayacct_key);
	}
}

static int __init delayacct_setup_enable(char *str)
{
	/* 启动参数 delayacct 仅记录初值，真正启用静态键在 delayacct_init()。 */
	delayacct_on = 1;
	return 1;
}
__setup("delayacct", delayacct_setup_enable);

void delayacct_init(void)
{
	/* 缓存分配失败即 panic；init_task 也必须有与普通任务一致的可选记账对象。 */
	delayacct_cache = KMEM_CACHE(task_delay_info, SLAB_PANIC|SLAB_ACCOUNT);
	delayacct_tsk_init(&init_task);
	set_delayacct(delayacct_on);
}

#ifdef CONFIG_PROC_SYSCTL
static int sysctl_delayacct(const struct ctl_table *table, int write, void *buffer,
		     size_t *lenp, loff_t *ppos)
{
	/*
	 * 写操作只允许 CAP_SYS_ADMIN。复制 ctl_table 并把 data 指向栈上 state，可先由
	 * 通用处理器完成 0/1 校验，再一次性更新静态键，避免暴露中间非法值。
	 */
	int state = delayacct_on;
	struct ctl_table t;
	int err;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;
	if (write)
		set_delayacct(state);
	return err;
}

static const struct ctl_table kern_delayacct_table[] = {
	{
		.procname       = "task_delayacct",
		.data           = NULL,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = sysctl_delayacct,
		.extra1         = SYSCTL_ZERO,
		.extra2         = SYSCTL_ONE,
	},
};

static __init int kernel_delayacct_sysctls_init(void)
{
	/* 暴露 /proc/sys/kernel/task_delayacct，失败不阻断 late init。 */
	register_sysctl_init("kernel", kern_delayacct_table);
	return 0;
}
late_initcall(kernel_delayacct_sysctls_init);
#endif

void __delayacct_tsk_init(struct task_struct *tsk)
{
	/* fork 路径按需零分配；内存不足时允许 delays=NULL，任务仍可正常运行。 */
	tsk->delays = kmem_cache_zalloc(delayacct_cache, GFP_KERNEL);
	if (tsk->delays)
		raw_spin_lock_init(&tsk->delays->lock);
}

/*
 * Finish delay accounting for a statistic using its timestamps (@start),
 * accumulator (@total) and @count
 * 用 local_clock 差值结束一次区间；只有正差值才在 raw spinlock 下同时更新
 * 总量、次数和极值。最大值时间戳使用墙钟，便于用户定位真实发生时刻。
 */
static void delayacct_end(raw_spinlock_t *lock, u64 *start, u64 *total, u32 *count,
							 u64 *max, u64 *min, struct timespec64 *ts)
{
	s64 ns = local_clock() - *start;
	unsigned long flags;

	if (ns > 0) {
		raw_spin_lock_irqsave(lock, flags);
		*total += ns;
		(*count)++;
		if (ns > *max) {
			*max = ns;
			ktime_get_real_ts64(ts);
		}
		if (*min == 0 || ns < *min)
			*min = ns;
		raw_spin_unlock_irqrestore(lock, flags);
	}
}

void __delayacct_blkio_start(void)
{
	/* 记录当前任务开始等待同步块 I/O 的本地时钟时间。 */
	current->delays->blkio_start = local_clock();
}

/*
 * We cannot rely on the `current` macro, as we haven't yet switched back to
 * the process being woken.
 * 唤醒路径执行时 current 仍是唤醒者，因此必须显式使用被唤醒任务 p 的状态。
 */
void __delayacct_blkio_end(struct task_struct *p)
{
	delayacct_end(&p->delays->lock,
		      &p->delays->blkio_start,
		      &p->delays->blkio_delay,
		      &p->delays->blkio_count,
		      &p->delays->blkio_delay_max,
		      &p->delays->blkio_delay_min,
		      &p->delays->blkio_delay_max_ts);
}

/*
 * delayacct_add_tsk() - 把任务延迟记账快照累加到 taskstats 输出对象。
 *
 * @d: 调用者拥有的输出/累计缓冲区；函数在原值上累加或覆盖对应极值。
 * @tsk: 被采集任务，借用指针；调用者保证 task 生命周期。
 *
 * 调用位置：fill_stats() 构造每 PID 快照，tgid_stats_add_task() 构造组
 * 汇总。函数先读取无需 tsk->delays 的调度时间，再在 delays 存在时持其
 * raw spinlock 复制 I/O、swap、回收等字段。入口不要求持锁，锁内不睡眠。
 * 返回值当前恒为 0；计数溢出通过“total 为零但 count 非零”的约定表达。
 */
int delayacct_add_tsk(struct taskstats *d, struct task_struct *tsk)
{
	u64 utime, stime, stimescaled, utimescaled;
	unsigned long long t2, t3;
	unsigned long flags, t1;
	s64 tmp;

	/*
	 * CPU real/scaled 累计使用有符号临时量检测越界；溢出时按 ABI 约定
	 * 把 total 置零，而不是饱和到最大值。
	 */
	task_cputime(tsk, &utime, &stime);
	tmp = (s64)d->cpu_run_real_total;
	tmp += utime + stime;
	d->cpu_run_real_total = (tmp < (s64)d->cpu_run_real_total) ? 0 : tmp;

	task_cputime_scaled(tsk, &utimescaled, &stimescaled);
	tmp = (s64)d->cpu_scaled_run_real_total;
	tmp += utimescaled + stimescaled;
	d->cpu_scaled_run_real_total =
		(tmp < (s64)d->cpu_scaled_run_real_total) ? 0 : tmp;

	/*
	 * No locking available for sched_info (and too expensive to add one)
	 * Mitigate by taking snapshot of values
	 */
	/*
	 * sched_info 没有适合此读路径的锁，新增锁又会增加调度热路径成本，
	 * 因此依次抓取三个标量形成尽力而为的快照；
	 * 主动查询可能混合相邻时刻，
	 * 退出任务则不再运行，最终值更稳定。
	 */
	t1 = tsk->sched_info.pcount;
	t2 = tsk->sched_info.run_delay;
	t3 = tsk->se.sum_exec_runtime;

	d->cpu_count += t1;

	d->cpu_delay_max = tsk->sched_info.max_run_delay;
	d->cpu_delay_min = tsk->sched_info.min_run_delay;
	d->cpu_delay_max_ts.tv_sec = tsk->sched_info.max_run_delay_ts.tv_sec;
	d->cpu_delay_max_ts.tv_nsec = tsk->sched_info.max_run_delay_ts.tv_nsec;
	tmp = (s64)d->cpu_delay_total + t2;
	d->cpu_delay_total = (tmp < (s64)d->cpu_delay_total) ? 0 : tmp;
	tmp = (s64)d->cpu_run_virtual_total + t3;

	d->cpu_run_virtual_total =
		(tmp < (s64)d->cpu_run_virtual_total) ?	0 : tmp;

	/*
	 * 没有按需分配 delays 对象时，
	 * 调度字段仍有效，其余延迟字段保持原值。
	 */
	if (!tsk->delays)
		return 0;

	/* zero XXX_total, non-zero XXX_count implies XXX stat overflowed */
	/*
	 * delays->lock 让每类 count/total/max/min/timestamp 作为一组复制；
	 * total 为零且 count 非零表示累计量发生过回绕，
	 * 用户不能当作无延迟。
	 */
	raw_spin_lock_irqsave(&tsk->delays->lock, flags);
	UPDATE_DELAY(blkio);
	UPDATE_DELAY(swapin);
	UPDATE_DELAY(freepages);
	UPDATE_DELAY(thrashing);
	UPDATE_DELAY(compact);
	UPDATE_DELAY(wpcopy);
	UPDATE_DELAY(irq);
	raw_spin_unlock_irqrestore(&tsk->delays->lock, flags);

	return 0;
}

__u64 __delayacct_blkio_ticks(struct task_struct *tsk)
{
	/* 在锁内读取完整纳秒累计值，再按 USER_HZ 转为传统 clock_t tick。 */
	__u64 ret;
	unsigned long flags;

	raw_spin_lock_irqsave(&tsk->delays->lock, flags);
	ret = nsec_to_clock_t(tsk->delays->blkio_delay);
	raw_spin_unlock_irqrestore(&tsk->delays->lock, flags);
	return ret;
}

void __delayacct_freepages_start(void)
{
	/* 标记当前任务进入同步直接回收/等待释放页面阶段。 */
	current->delays->freepages_start = local_clock();
}

void __delayacct_freepages_end(void)
{
	/* 与 freepages_start 配对，将本次直接回收等待计入任务统计。 */
	delayacct_end(&current->delays->lock,
		      &current->delays->freepages_start,
		      &current->delays->freepages_delay,
		      &current->delays->freepages_count,
		      &current->delays->freepages_delay_max,
		      &current->delays->freepages_delay_min,
		      &current->delays->freepages_delay_max_ts);
}

void __delayacct_thrashing_start(bool *in_thrashing)
{
	/*
	 * 抖动区间允许嵌套：把旧状态返回给调用者，仅最外层设置起点；内层结束时
	 * 看到 in_thrashing=true 便不重复累计。
	 */
	*in_thrashing = !!current->in_thrashing;
	if (*in_thrashing)
		return;

	current->in_thrashing = 1;
	current->delays->thrashing_start = local_clock();
}

void __delayacct_thrashing_end(bool *in_thrashing)
{
	/* 仅最外层调用清除任务标志并结束计时，与 start 返回的布尔值严格配对。 */
	if (*in_thrashing)
		return;

	current->in_thrashing = 0;
	delayacct_end(&current->delays->lock,
		      &current->delays->thrashing_start,
		      &current->delays->thrashing_delay,
		      &current->delays->thrashing_count,
		      &current->delays->thrashing_delay_max,
		      &current->delays->thrashing_delay_min,
		      &current->delays->thrashing_delay_max_ts);
}

void __delayacct_swapin_start(void)
{
	/* 标记当前任务开始等待缺页换入。 */
	current->delays->swapin_start = local_clock();
}

void __delayacct_swapin_end(void)
{
	/* 结束换入等待区间并原子更新统计组。 */
	delayacct_end(&current->delays->lock,
		      &current->delays->swapin_start,
		      &current->delays->swapin_delay,
		      &current->delays->swapin_count,
		      &current->delays->swapin_delay_max,
		      &current->delays->swapin_delay_min,
		      &current->delays->swapin_delay_max_ts);
}

void __delayacct_compact_start(void)
{
	/* 标记当前任务开始同步内存规整。 */
	current->delays->compact_start = local_clock();
}

void __delayacct_compact_end(void)
{
	/* 结束同步规整区间并更新 total/count/min/max。 */
	delayacct_end(&current->delays->lock,
		      &current->delays->compact_start,
		      &current->delays->compact_delay,
		      &current->delays->compact_count,
		      &current->delays->compact_delay_max,
		      &current->delays->compact_delay_min,
		      &current->delays->compact_delay_max_ts);
}

void __delayacct_wpcopy_start(void)
{
	/* 标记写保护缺页处理中的页面复制开始。 */
	current->delays->wpcopy_start = local_clock();
}

void __delayacct_wpcopy_end(void)
{
	/* 结束写保护复制区间并更新对应统计。 */
	delayacct_end(&current->delays->lock,
		      &current->delays->wpcopy_start,
		      &current->delays->wpcopy_delay,
		      &current->delays->wpcopy_count,
		      &current->delays->wpcopy_delay_max,
		      &current->delays->wpcopy_delay_min,
		      &current->delays->wpcopy_delay_max_ts);
}

void __delayacct_irq(struct task_struct *task, u32 delta)
{
	/*
	 * IRQ 延迟已由调用者计算为 u32 纳秒差值，无需 start 字段；在目标任务锁下
	 * 累加次数/总量/极值。delta=0 计次数但不建立非零最小值。
	 */
	unsigned long flags;

	raw_spin_lock_irqsave(&task->delays->lock, flags);
	task->delays->irq_delay += delta;
	task->delays->irq_count++;
	if (delta > task->delays->irq_delay_max) {
		task->delays->irq_delay_max = delta;
		ktime_get_real_ts64(&task->delays->irq_delay_max_ts);
	}
	if (delta && (!task->delays->irq_delay_min || delta < task->delays->irq_delay_min))
		task->delays->irq_delay_min = delta;
	raw_spin_unlock_irqrestore(&task->delays->lock, flags);
}
