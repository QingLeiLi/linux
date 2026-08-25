// SPDX-License-Identifier: GPL-2.0

/*
 * Auto-group scheduling implementation:
 */
/*
 * 自动分组把仍在 root CPU cgroup 的同一 signal_struct 线程组映射到一个 CFS
 * task_group，使交互式会话先按组竞争 CPU。signal->autogroup 持有 kref；siglock
 * 串行化组指针和线程链表，实际任务调度实体由 sched_move_task() 在 rq 锁下迁移。
 */

#include "autogroup.h"
#include "sched.h"

/* 运行时总开关，默认启用；热读冷写，READ_ONCE 消费，因此放入 __read_mostly。 */
unsigned int __read_mostly sysctl_sched_autogroup_enabled = 1;
/* 永久默认组供 init 和创建失败退化使用；静态生命周期，不走普通销毁。 */
static struct autogroup autogroup_default;
/* 为动态 autogroup 分配仅用于展示的单调 id；原子操作允许并发创建。 */
static atomic_t autogroup_seq_nr;

#ifdef CONFIG_SYSCTL
/* 注册 /proc/sys/kernel/sched_autogroup_enabled，只接受 0 或 1。 */
static const struct ctl_table sched_autogroup_sysctls[] = {
	{
		.procname       = "sched_autogroup_enabled",
		.data           = &sysctl_sched_autogroup_enabled,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		/*
		 * data/maxlen/mode 把 proc 文件绑定到全局开关；minmax handler 再用
		 * extra1/extra2 把写入限制为 0 或 1，拒绝值不会发布到热路径读者。
		 */
		.proc_handler   = proc_dointvec_minmax,
		.extra1         = SYSCTL_ZERO,
		.extra2         = SYSCTL_ONE,
	},
};

/* 启动期注册 sysctl 表；无参数/返回值，注册对象由 sysctl core 持有。 */
static void __init sched_autogroup_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_autogroup_sysctls);
}
#else /* !CONFIG_SYSCTL: */
/* 未启用 SYSCTL 时保持调用点配置无关，展开为空操作。 */
#define sched_autogroup_sysctl_init() do { } while (0)
#endif /* !CONFIG_SYSCTL */

/*
 * 启动期初始化默认组并发布给 @init_task->signal。@init_task 不可为 NULL，只借用；
 * root_task_group 与静态 autogroup_default 都是永久对象。函数初始化 kref/rwsem 后
 * 发布指针，并按配置注册 sysctl；无返回值，仅初始化阶段调用，可执行可睡眠注册。
 */
void __init autogroup_init(struct task_struct *init_task)
{
	autogroup_default.tg = &root_task_group;
	kref_init(&autogroup_default.kref);
	init_rwsem(&autogroup_default.lock);
	init_task->signal->autogroup = &autogroup_default;
	sched_autogroup_sysctl_init();
}

/*
 * task_group 最终 RCU 释放路径调用，用于释放 @tg->autogroup 反向容器。
 * @tg 已不可被并发访问且只借用；kfree(NULL) 安全。函数不释放 tg 本身、无返回值。
 */
void autogroup_free(struct task_group *tg)
{
	kfree(tg->autogroup);
}

/*
 * kref 归零回调：@kref 必须嵌在动态 autogroup 中。RT group 配置下先撤销借用 root
 * RT 数组的别名，防止通用销毁释放永久存储；随后从 RCU 组树摘除 tg 并安排延迟销毁。
 * 回调不直接 kfree ag，它最终由 task_group 的 autogroup_free() 释放，不能睡眠。
 */
static inline void autogroup_destroy(struct kref *kref)
{
	struct autogroup *ag = container_of(kref, struct autogroup, kref);

#ifdef CONFIG_RT_GROUP_SCHED
	/* We've redirected RT tasks to the root task group... */
	/* 自动组 RT task 曾重定向到 root；销毁前清空别名，避免释放 root 数组。 */
	ag->tg->rt_se = NULL;
	ag->tg->rt_rq = NULL;
#endif
	sched_release_group(ag->tg);
	sched_destroy_group(ag->tg);
}

/* 放弃一个 autogroup 引用；最后一个 put 同步触发摘除/延迟销毁回调。 */
static inline void autogroup_kref_put(struct autogroup *ag)
{
	kref_put(&ag->kref, autogroup_destroy);
}

/* 获取 @ag 的非空 kref 并返回同一借用地址；调用者必须最终 put，不会睡眠。 */
static inline struct autogroup *autogroup_kref_get(struct autogroup *ag)
{
	kref_get(&ag->kref);
	return ag;
}

/*
 * 稳定取得 @p 所在线程组的 autogroup 引用。lock_task_sighand() 成功时以 siglock
 * 保护 signal 指针读取；若 task 正在释放而无法加锁，退化为永久 default 并取引用。
 * 返回非 NULL 持有引用，调用者必须 put；函数 irqsave 加自旋锁，不能睡眠。
 */
static inline struct autogroup *autogroup_task_get(struct task_struct *p)
{
	/* ag 是返回的持有引用；flags 保存 sighand 锁的本地中断状态。 */
	struct autogroup *ag;
	unsigned long flags;

	if (!lock_task_sighand(p, &flags))
		return autogroup_kref_get(&autogroup_default);

	ag = autogroup_kref_get(p->signal->autogroup);
	unlock_task_sighand(p, &flags);

	return ag;
}

/*
 * 分配并发布一个 root 下的动态 CFS autogroup。成功返回带一份调用者引用的 @ag；
 * kzalloc 或 task_group 分配失败时打印限速告警并返回持有引用的 default，接口因此
 * 永不返回 NULL/ERR_PTR。成功路径初始化引用、锁、id、反向指针并 online tg。
 * CONFIG_RT_GROUP_SCHED 下释放新组 RT 私有存储，改借 root RT 队列。GFP_KERNEL 与
 * sched_create_group 可睡眠，禁止在自旋锁内调用；失败按取得顺序回滚。
 */
static inline struct autogroup *autogroup_create(void)
{
	/* ag 拥有自动组容器；tg 在成功 online 后由组销毁链管理。 */
	struct autogroup *ag = kzalloc_obj(*ag);
	struct task_group *tg;

	if (!ag)
		goto out_fail;

	/*
	 * 第一阶段只取得尚未发布的 ag 容器；随后创建其拥有的 task_group。
	 * ERR_PTR 表示组创建未取得 ownership，故 out_free 只需释放 ag，不能销毁 tg。
	 */
	tg = sched_create_group(&root_task_group);
	if (IS_ERR(tg))
		goto out_free;

	kref_init(&ag->kref);
	init_rwsem(&ag->lock);
	ag->id = atomic_inc_return(&autogroup_seq_nr);
	ag->tg = tg;
#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * Autogroup RT tasks are redirected to the root task group
	 * so we don't have to move tasks around upon policy change,
	 * or flail around trying to allocate bandwidth on the fly.
	 * A bandwidth exception in __sched_setscheduler() allows
	 * the policy change to proceed.
	 */
	/*
	 * 自动组 RT 任务重定向到 root task group，从而无需在策略切换时移动任务，
	 * 也无需动态分配 RT 带宽；__sched_setscheduler() 有配套带宽例外。
	 */
	free_rt_sched_group(tg);
	tg->rt_se = root_task_group.rt_se;
	tg->rt_rq = root_task_group.rt_rq;
#endif /* CONFIG_RT_GROUP_SCHED */
	tg->autogroup = ag;

	/* online 是对全局 RCU task_group 树的发布点；此后失败不可直接 kfree。 */
	sched_online_group(tg, &root_task_group);
	return ag;

out_free:
	/* tg 创建失败尚未发布，只需释放独立 ag 容器。 */
	kfree(ag);
out_fail:
	/* 限速日志避免内存压力下反复创建造成告警洪泛。 */
	if (printk_ratelimit()) {
		printk(KERN_WARNING "autogroup_create: %s failure.\n",
			ag ? "sched_create_group()" : "kzalloc()");
	}

	return autogroup_kref_get(&autogroup_default);
}

/*
 * 判断 @p 是否应以 signal autogroup 覆盖当前 @tg。两者只借用；非 root cgroup 优先，
 * 正在退出的 task 也拒绝自动组。返回 bool，无引用/状态副作用且不能睡眠。允许与
 * autogroup_move_group() 短暂竞态，因为移动方在 put 旧组前会重新 sched_move_task()；
 * PF_EXITING 则让已脱离线程链表的退出任务回到真实组，避免再借 signal 指针。
 */
bool task_wants_autogroup(struct task_struct *p, struct task_group *tg)
{
	if (tg != &root_task_group)
		return false;
	/*
	 * If we race with autogroup_move_group() the caller can use the old
	 * value of signal->autogroup but in this case sched_move_task() will
	 * be called again before autogroup_kref_put().
	 *
	 * However, there is no way sched_autogroup_exit_task() could tell us
	 * to avoid autogroup->tg, so we abuse PF_EXITING flag for this case.
	 */
	/*
	 * 与 autogroup_move_group() 竞态时，调用者可能读到旧 signal->autogroup；移动方
	 * 会在释放旧引用前再次迁移所有可见线程，最终状态仍收敛。退出任务无法从
	 * sched_autogroup_exit_task() 额外传递禁用信息，因此复用 PF_EXITING 作门禁。
	 */
	if (p->flags & PF_EXITING)
		return false;

	return true;
}

/*
 * 在线程退出、即将进入 exit_notify() 前，把 @p 的调度实体移出 autogroup。
 * @p 是 current 退出路径上的稳定借用；sched_move_task(..., true) 在 rq 锁下重算组。
 * 无返回值、可能获取调度锁但不分配。PF_EXITING 已置位，使 task_wants_autogroup()
 * 选择真实 cgroup；完成后即使线程从 thread list 消失也不再依赖 signal autogroup。
 */
void sched_autogroup_exit_task(struct task_struct *p)
{
	/*
	 * We are going to call exit_notify() and autogroup_move_group() can't
	 * see this thread after that: we can no longer use signal->autogroup.
	 * See the PF_EXITING check in task_wants_autogroup().
	 */
	/*
	 * 接下来 exit_notify() 后组移动无法再从线程链表看到本线程，因此此刻必须停止
	 * 使用 signal->autogroup；配套判断位于 task_wants_autogroup() 的 PF_EXITING。
	 */
	sched_move_task(p, true);
}

/*
 * 把 @p 所在线程组切换到持有中的 @ag。函数先用 siglock 稳定 signal->autogroup
 * 和线程链表；若 task 正在释放而锁失败只告警返回。相同组快速返回。否则先为新组
 * 取引用并发布 signal 指针，再逐线程调用 sched_move_task() 修复 rq 下派生映射，
 * 解锁后才 put 旧组。无直接返回值；调用者不得持有 siglock，内部 rq 锁操作不睡眠。
 */
static void
autogroup_move_group(struct task_struct *p, struct autogroup *ag)
{
	/* prev 在 siglock 下取得并由 signal 旧引用保持；t 遍历稳定线程链表。 */
	struct autogroup *prev;
	struct task_struct *t;
	unsigned long flags;

	if (WARN_ON_ONCE(!lock_task_sighand(p, &flags)))
		return;

	prev = p->signal->autogroup;
	if (prev == ag) {
		/* 没有状态变化，也不调整任何引用。 */
		unlock_task_sighand(p, &flags);
		return;
	}

	p->signal->autogroup = autogroup_kref_get(ag);
	/*
	 * We can't avoid sched_move_task() after we changed signal->autogroup,
	 * this process can already run with task_group() == prev->tg or we can
	 * race with cgroup code which can read autogroup = prev under rq->lock.
	 * In the latter case for_each_thread() can not miss a migrating thread,
	 * cpu_cgroup_attach() must not be possible after cgroup_task_exit()
	 * and it can't be removed from thread list, we hold ->siglock.
	 *
	 * If an exiting thread was already removed from thread list we rely on
	 * sched_autogroup_exit_task().
	 */
	/*
	 * 发布新 signal 指针后不能省略逐 task 迁移：进程可能仍以 prev->tg 运行，且
	 * cgroup 路径可能已在 rq 锁下读到旧值。siglock 防止迁移线程从 thread list
	 * 消失；更早移除的退出线程由 sched_autogroup_exit_task() 收尾。
	 */
	for_each_thread(p, t)
		sched_move_task(t, true);

	unlock_task_sighand(p, &flags);
	/* 所有仍可见线程已重算后才允许旧组引用归零进入 RCU 销毁。 */
	autogroup_kref_put(prev);
}

/* Allocates GFP_KERNEL, cannot be called under any spinlock: */
/*
 * 分配使用 GFP_KERNEL，不能在任何自旋锁内调用。为 @p 创建新组并附着整个线程组；
 * create 返回的临时引用在 move 为 signal 获取独立引用后释放。创建失败会把 default
 * 作为退化组附着，接口无错误返回但可能睡眠。
 */
void sched_autogroup_create_attach(struct task_struct *p)
{
	struct autogroup *ag = autogroup_create();

	autogroup_move_group(p, ag);

	/* Drop extra reference added by autogroup_create(): */
	/* 丢弃 create 的调用者引用，signal->autogroup 仍持有 move 取得的一份。 */
	autogroup_kref_put(ag);
}
EXPORT_SYMBOL(sched_autogroup_create_attach);

/* Cannot be called under siglock. Currently has no users: */
/*
 * 不能在 siglock 下调用；当前没有用户。把 @p 的整个线程组切回永久 default，
 * 无返回值；内部会获取 siglock/rq 锁，不接管 task 引用。
 */
void sched_autogroup_detach(struct task_struct *p)
{
	autogroup_move_group(p, &autogroup_default);
}
EXPORT_SYMBOL(sched_autogroup_detach);

/*
 * fork 新 signal_struct 时继承 current 的 autogroup。@sig 是未发布的纯输出对象；
 * autogroup_task_get() 返回一份引用存入 sig，失败锁定时退化为 default。无返回值。
 */
void sched_autogroup_fork(struct signal_struct *sig)
{
	sig->autogroup = autogroup_task_get(current);
}

/*
 * signal_struct 最终释放时放弃其 autogroup 引用。@sig 已无并发使用者，只借用；
 * 最后一个引用可能触发 task_group 摘除和 RCU 延迟销毁。无返回值。
 */
void sched_autogroup_exit(struct signal_struct *sig)
{
	autogroup_kref_put(sig->autogroup);
}

/*
 * 解析启动参数 noautogroup：@str 未使用，把启动期总开关清零并返回 1 表示参数已处理。
 * __setup 使其仅在早期命令行解析调用，无锁且无失败类别。
 */
static int __init setup_autogroup(char *str)
{
	sysctl_sched_autogroup_enabled = 0;

	return 1;
}
__setup("noautogroup", setup_autogroup);

#ifdef CONFIG_PROC_FS

/*
 * 处理 /proc/<pid>/autogroup 的 nice 写入。@p 只用于稳定取得其 autogroup 引用；
 * @nice 必须在 [-20,19]。依次执行范围、LSM、负 nice 权限和非管理员 100ms 全局限速
 * 检查，再把 nice 映射为 CFS shares，在 ag->lock 写侧串行更新 tg 权重与展示值。
 * 返回 0 成功，或 -EINVAL/-EPERM/-EAGAIN、LSM/调度组错误；所有出口释放 ag 引用。
 * sched_group_set_shares() 获取全局 mutex 和各 CPU rq 锁，属于重操作且可以睡眠。
 */
int proc_sched_autogroup_set_nice(struct task_struct *p, int nice)
{
	/* next 是所有非管理员写者共享的下次允许时刻；调用路径串行性不由本变量保证。 */
	static unsigned long next = INITIAL_JIFFIES;
	/* ag 是持有引用；shares 是 scale_load 后的内部权重，idx 是防推测数组下标。 */
	struct autogroup *ag;
	unsigned long shares;
	int err, idx;

	if (nice < MIN_NICE || nice > MAX_NICE)
		/* 越界值不得进入权重表索引。 */
		return -EINVAL;

	/* LSM 先审计/拒绝本次 nice 请求；目标权限语义沿用现有接口的 current。 */
	err = security_task_setnice(current, nice);
	if (err)
		return err;

	if (nice < 0 && !can_nice(current, nice))
		/* 提升 CPU 权重需要资源限制或相应能力。 */
		return -EPERM;

	/* This is a heavy operation, taking global locks.. */
	/* 此操作会取得全局锁和所有 CPU rq 锁，因此限制非管理员调用频率。 */
	if (!capable(CAP_SYS_ADMIN) && time_before(jiffies, next))
		return -EAGAIN;

	next = HZ / 10 + jiffies;
	/* 取得引用后，即使线程组并发换组，当前更新对象也不会被释放。 */
	ag = autogroup_task_get(p);

	/* nospec 把验证过的 nice+20 约束在 40 项权重表内。 */
	idx = array_index_nospec(nice + 20, 40);
	shares = scale_load(sched_prio_to_weight[idx]);

	down_write(&ag->lock);
	/* 先提交 task_group shares；只有成功才同步用户可见 nice，避免两者分裂。 */
	err = sched_group_set_shares(ag->tg, shares);
	if (!err)
		ag->nice = nice;
	up_write(&ag->lock);

	autogroup_kref_put(ag);

	return err;
}

/*
 * 向 @m 输出 @p 当前动态 autogroup 的路径和 nice。函数取得 ag 引用稳定生命周期；
 * default/root 等非自动组不输出。ag->lock 读侧使 nice 与并发 set_nice 串行，seq_file
 * 管理缓冲；无返回值，所有路径在 out 释放引用，读取可能睡眠。
 */
void proc_sched_autogroup_show_task(struct task_struct *p, struct seq_file *m)
{
	struct autogroup *ag = autogroup_task_get(p);

	if (!task_group_is_autogroup(ag->tg))
		/* 默认 root group 没有 tg->autogroup 反向指针。 */
		goto out;

	down_read(&ag->lock);
	seq_printf(m, "/autogroup-%ld nice %d\n", ag->id, ag->nice);
	up_read(&ag->lock);

out:
	/* 与 task_get 配对，允许最后一个用户启动延迟销毁。 */
	autogroup_kref_put(ag);
}
#endif /* CONFIG_PROC_FS */

/*
 * 为 task_group 展示层格式化 autogroup 路径。@tg 必须在调用者的 RCU/锁保护下存活；
 * @buf 是容量 @buflen 字节的调用者输出存储。非自动组返回 0 且不写；自动组返回
 * snprintf 的完整所需长度，可能 >= buflen 表示截断。函数只读不可变 id，不分配、
 * 不取 ag->lock，也不转移任何 ownership。
 */
int autogroup_path(struct task_group *tg, char *buf, int buflen)
{
	if (!task_group_is_autogroup(tg))
		return 0;

	return snprintf(buf, buflen, "%s-%ld", "/autogroup", tg->autogroup->id);
}
