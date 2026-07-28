// SPDX-License-Identifier: GPL-2.0-only
/*
 * softirq 与 tasklet 学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5 Codex。
 *
 * 文件职责：
 *   本文件实现软中断的通用调度框架：子系统把某个 softirq 向量登记到
 *   softirq_vec[]，产生事件时只在本 CPU 的 pending 位图中置位，硬中断
 *   返回、local_bh_enable() 或每 CPU ksoftirqd 随后取走位图并调用回调。
 *   文件后半还实现了建立在 TASKLET/HI softirq 之上的旧 tasklet API，
 *   以及强制线程化中断配置下专门承接定时器 softirq 的 ktimers/%u。
 *
 * 职责边界：
 *   这里决定“何时、在哪种上下文分派哪个向量”，但不实现网络、定时器、
 *   RCU 等向量的业务逻辑；各子系统通过 open_softirq() 提供 action，
 *   并自行负责回调内部对象的锁、引用和跨 CPU 一致性。
 *
 * 主调用链：
 *   硬中断处理器/子系统
 *     -> raise_softirq[_irqoff]()：设置本 CPU pending 位
 *     -> irq_exit()/local_bh_enable()：条件允许时就地进入 __do_softirq()
 *     -> handle_softirqs()：清空快照、开中断、逐位调用 softirq_vec[].action
 *     -> 时间/重启次数/调度需求超限：唤醒 ksoftirqd/%u 继续处理。
 *
 * 核心状态与生命周期：
 *   softirq_vec[] 在启动期登记后长期存在；irq_stat 中的 pending 位图和
 *   tasklet 链表均为 per-CPU 状态。置位只是发布“有工作”，执行器会先
 *   清空已领取的快照，使回调运行期间新产生的事件进入下一轮快照。
 *   tasklet 的 SCHED 位防止重复入链，RUN 位保证同一个 tasklet 不会在
 *   多个 CPU 同时执行，count 非零则把已调度对象留到以后重试。
 *
 * 并发模型：
 *   本 CPU pending 位图和队列主要依赖本地关中断完成“硬中断与当前 CPU”
 *   的互斥；不同 CPU 可以并行执行同一 softirq 类型，所以具体 action
 *   必须自行同步共享数据。PREEMPT_RT 把 BH 禁用状态拆成 task 与 per-CPU
 *   计数，并用 local lock/迁移禁止维持 CPU 归属，从而允许相关任务被抢占。
 *
 * 方案权衡：
 *   就地执行可降低事件延迟，但无限重启会饿死普通任务；因此框架设置时间
 *   与次数预算，并在需要调度时交给 ksoftirqd。per-CPU 状态减少全局锁和
 *   cacheline 争用，代价是 CPU 热拔插时必须迁移遗留 tasklet，而且各
 *   softirq action 仍需处理自身的跨 CPU 共享状态。
 *
 * 本次分析基于源码提交：2e80aeff1ff8。
 */
/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 *	Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 */

/* 本文件 pr_* 日志统一增加模块名前缀，不改变调用点的格式参数或严重级别。 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/export.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/local_lock.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/ftrace.h>
#include <linux/smp.h>
#include <linux/smpboot.h>
#include <linux/tick.h>
#include <linux/irq.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>

#include <asm/softirq_stack.h>

/*
 * 本编译单元负责实例化 irq/softirq/tasklet tracepoint；其他文件只看到声明。
 * 必须在包含 trace/events/irq.h 前定义，且只能有一个实例化单元。
 */
#define CREATE_TRACE_POINTS
#include <trace/events/irq.h>

/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
 */
/*
 * 这段上游说明给出了 softirq 的基本并发契约：
 *
 * - 通用框架不为不同 CPU 共享一份调度队列；pending 状态和 tasklet 队列
 *   都按 CPU 保存。因此两个 CPU 可以同时执行同一种 softirq。
 * - 某个 action 若访问设备、协议栈或其他共享对象，必须在子系统内部用
 *   自己的锁完成串行化，softirq 核心不会替它提供全局互斥。
 * - 事件只会在产生它的本地 CPU 上置 pending，这形成“弱 CPU 绑定”：
 *   通常有利于复用本 CPU cache，但不是对象永久固定到某 CPU 的承诺。
 *
 * 示例中 NET_RX 可以按 CPU 并行；NET_TX 的设备级串行化由网络代码负责；
 * tasklet 则额外以自身 RUN 状态保证“同一实例不并发”，不同实例仍可并行。
 */

#ifndef __ARCH_IRQ_STAT
/*
 * irq_stat 是每 CPU 的中断统计/状态容器，softirq pending 位图也位于其中。
 * 若体系结构没有提供定制的 __ARCH_IRQ_STAT，就在此建立并导出通用版本。
 * 对它的本地读改写必须遵守关本地中断协议，不能把 per-CPU 误解成天然无竞态。
 */
DEFINE_PER_CPU_ALIGNED(irq_cpustat_t, irq_stat);
EXPORT_PER_CPU_SYMBOL(irq_stat);
#endif

/*
 * softirq_vec[nr] 保存每个固定 softirq 编号的无参 action。数组在启动期由
 * 各子系统通过 open_softirq() 填充，之后由所有 CPU 的 handle_softirqs()
 * 只读分派；SMP cacheline 对齐减少不同 CPU 读取热点向量时的伪共享。
 */
static struct softirq_action softirq_vec[NR_SOFTIRQS] __cacheline_aligned_in_smp;

/*
 * ksoftirqd 是每 CPU 守护线程指针。smpboot 框架创建、热插拔管理线程；
 * softirq 快速路径超出预算或当前上下文不适合就地执行时，只借用该指针
 * 唤醒对应 CPU 的线程，不取得 task_struct 的额外引用。
 */
DEFINE_PER_CPU(struct task_struct *, ksoftirqd);

/*
 * 编号到可读名称的静态映射，供诊断日志与跟踪展示。顺序必须与
 * include/linux/interrupt.h 的 softirq 编号严格一致；它不控制分派行为。
 */
const char * const softirq_to_name[NR_SOFTIRQS] = {
	"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL",
	"TASKLET", "SCHED", "HRTIMER", "RCU"
};

/*
 * we cannot loop indefinitely here to avoid userspace starvation,
 * but we also don't want to introduce a worst case 1/HZ latency
 * to the pending events, so lets the scheduler to balance
 * the softirq load for us.
 */
/*
 * 上游注释的含义是：不能在当前路径无限循环处理 softirq，否则用户态会
 * 饥饿；但也不能简单推迟到下一次周期 tick，否则最坏会引入 1/HZ 延迟。
 * 因而超出就地处理预算后唤醒 ksoftirqd，让普通调度器在 softirq 工作与
 * 其他可运行任务之间作公平选择。
 */
/*
 * wakeup_softirqd() - 唤醒当前 CPU 的 softirq 守护线程。
 *
 * 调用位置：raise 路径、handle_softirqs() 超预算路径及 RT 的 BH 恢复路径。
 * 入参：无；函数隐式使用当前 CPU 的 ksoftirqd 借用指针。
 * 入口：本地中断必须关闭，所以读取 per-CPU 指针期间不会迁移；不要求调用者
 *       持有 task 引用。wake_up_process() 可在原子上下文调用，不睡眠。
 * 返回：无直接返回值。线程尚未创建时无副作用；存在时把它置为可运行。
 */
static void wakeup_softirqd(void)
{
	/* Interrupts are disabled: no need to stop preemption */
	/*
	 * 此时中断关闭，因此无需另行 preempt_disable() 来固定 CPU。指针只是
	 * smpboot 框架维护的借用值；NULL 表示启动早期或该 CPU 尚无线程。
	 */
	struct task_struct *tsk = __this_cpu_read(ksoftirqd);

	/* 唤醒是幂等提示；线程已经可运行时不会重复创建工作或取得引用。 */
	if (tsk)
		wake_up_process(tsk);
}

#ifdef CONFIG_TRACE_IRQFLAGS
/*
 * lockdep/trace 使用的每 CPU IRQ 上下文镜像。它们记录逻辑上的硬中断使能
 * 和嵌套状态，供跟踪器校验锁上下文；不替代硬件中断状态或 preempt_count。
 */
DEFINE_PER_CPU(int, hardirqs_enabled);
DEFINE_PER_CPU(int, hardirq_context);
EXPORT_PER_CPU_SYMBOL_GPL(hardirqs_enabled);
EXPORT_PER_CPU_SYMBOL_GPL(hardirq_context);
#endif

/*
 * SOFTIRQ_OFFSET usage:
 *
 * On !RT kernels 'count' is the preempt counter, on RT kernels this applies
 * to a per CPU counter and to task::softirqs_disabled_cnt.
 *
 * - count is changed by SOFTIRQ_OFFSET on entering or leaving softirq
 *   processing.
 *
 * - count is changed by SOFTIRQ_DISABLE_OFFSET (= 2 * SOFTIRQ_OFFSET)
 *   on local_bh_disable or local_bh_enable.
 *
 * This lets us distinguish between whether we are currently processing
 * softirq and whether we just have bh disabled.
 */
/*
 * 上游注释描述了 softirq_count 的编码约定：一次正在服务 softirq 使用
 * SOFTIRQ_OFFSET，而 local_bh_disable() 使用它的两倍。这样位级判断能区分
 * “正在执行 action”与“任务仅禁止 bottom half”。非 RT 把值放进
 * preempt_count；RT 因允许 BH-disabled 任务被抢占，还必须同时维护 task
 * 与 per-CPU 状态。所有加减必须配对，否则 in_interrupt()/lockdep 会误判。
 */
#ifdef CONFIG_PREEMPT_RT

/*
 * RT accounts for BH disabled sections in task::softirqs_disabled_cnt and
 * also in per CPU softirq_ctrl::cnt. This is necessary to allow tasks in a
 * softirq disabled section to be preempted.
 *
 * The per task counter is used for softirq_count(), in_softirq() and
 * in_serving_softirqs() because these counts are only valid when the task
 * holding softirq_ctrl::lock is running.
 *
 * The per CPU counter prevents pointless wakeups of ksoftirqd in case that
 * the task which is in a softirq disabled section is preempted or blocks.
 */
/*
 * RT 版本的 BH 禁用区不能简单等同于“关抢占”。task 计数回答当前任务是否
 * 禁止/正在服务 softirq；per-CPU cnt 则表明本 CPU 仍有某个禁用区占用，
 * 避免该任务被抢占或阻塞时无意义地唤醒 ksoftirqd。两者只在持有下面的
 * local lock 或迁移禁止协议时同步，不能脱离该保护比较。
 */
/*
 * struct softirq_ctrl - PREEMPT_RT 上每 CPU 的 BH 串行化状态。
 *
 * lock：把本 CPU 的 BH-disabled 临界区与 softirq 执行串行化；在 RT 上
 *       local_lock_t 可映射为允许调度语义的本地锁，而非普通裸自旋锁。
 * cnt：该 CPU 累积的 SOFTIRQ_OFFSET 计数；零表示 ksoftirqd 可安全接管。
 * 对象静态创建、与 CPU 同寿命，不存在引用转移。
 */
struct softirq_ctrl {
	local_lock_t	lock;
	int		cnt;
};

/* 每 CPU 控制块初始 cnt 为零，local lock 在静态初始化时即可供 RT 路径使用。 */
static DEFINE_PER_CPU(struct softirq_ctrl, softirq_ctrl) = {
	.lock	= INIT_LOCAL_LOCK(softirq_ctrl.lock),
};

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * bh_lock_map 是 local_bh_disable/enable 的 lockdep 虚拟锁。它不提供真实
 * 互斥，只把 API 的嵌套和等待类型告知 lockdep。
 */
static struct lock_class_key bh_lock_key;
struct lockdep_map bh_lock_map = {
	.name			= "local_bh",
	.key			= &bh_lock_key,
	.wait_type_outer	= LD_WAIT_FREE,
	.wait_type_inner	= LD_WAIT_CONFIG, /* PREEMPT_RT makes BH preemptible. */
	/* PREEMPT_RT 使 BH 禁用区可被抢占，因此 lockdep 按配置相关等待处理。 */
	.lock_type		= LD_LOCK_PERCPU,
};
EXPORT_SYMBOL_GPL(bh_lock_map);
#endif

/**
 * local_bh_blocked() - Check for idle whether BH processing is blocked
 *
 * Returns false if the per CPU softirq::cnt is 0 otherwise true.
 *
 * This is invoked from the idle task to guard against false positive
 * softirq pending warnings, which would happen when the task which holds
 * softirq_ctrl::lock was the only running task on the CPU and blocks on
 * some other lock.
 */
/*
 * 上游 kernel-doc：idle 路径用该函数判断本 CPU 的 BH 是否被阻塞。cnt 为
 * 零返回 false，否则返回 true。若持有 softirq_ctrl.lock 的唯一运行任务
 * 阻塞在别的锁上，idle 可能暂时被调度；此时 pending softirq 不代表遗漏
 * 执行，用 cnt 可以避免“idle 时仍有 softirq”这种假阳性告警。
 *
 * 入参：无。必须在当前 CPU 上读取，返回值只是瞬时快照。
 * 返回：true 表示存在 BH 禁用占用；false 表示没有。无 ownership 或副作用。
 */
bool local_bh_blocked(void)
{
	return __this_cpu_read(softirq_ctrl.cnt) != 0;
}

/*
 * __local_bh_disable_ip() - PREEMPT_RT 上进入一层本地 BH 禁用区。
 *
 * 调用关系：local_bh_disable() 等包装器传入调用点与计数增量；softirq
 *           线程入口也用 SOFTIRQ_OFFSET 表示“正在服务”。
 * @ip：发起禁用的指令地址，仅供 lockdep/trace 归因，不被持有。
 * @cnt：要增加的 softirq 计数，通常是 SOFTIRQ_DISABLE_OFFSET 或
 *       SOFTIRQ_OFFSET；必须由匹配的 enable 路径原值减回。
 * 入口：不得从 hardirq 调用。最外层且可抢占时会取得 per-CPU local lock
 *       或禁止迁移，并进入 RCU-bh 读侧；嵌套层只增加计数。
 * 返回：无直接返回值。当前任务与本 CPU 记账已更新，调用者在恢复前不得
 *       假设 softirq 会在本 CPU 并发执行。
 */
void __local_bh_disable_ip(unsigned long ip, unsigned int cnt)
{
	/*
	 * 变量地图：
	 *   flags  短暂关闭硬中断以原子更新 lockdep 逻辑状态，退出前恢复。
	 *   newcnt 加上本层后的 per-CPU 总计数，同时镜像到 current。
	 */
	unsigned long flags;
	int newcnt;

	/* hardirq 已天然延后 softirq；从该上下文再禁 BH 通常表示调用协议错误。 */
	WARN_ON_ONCE(in_hardirq());

	/* 先发布虚拟锁获取，匹配 enable 或 ksoftirqd_run_end() 的 release。 */
	lock_map_acquire_read(&bh_lock_map);

	/* First entry of a task into a BH disabled section? */
	/*
	 * 只有 current 从零进入非零时才建立 CPU 归属和 RCU-bh 读侧边界；
	 * 嵌套调用复用外层保护，避免递归取得同一 local lock。
	 */
	if (!current->softirq_disable_cnt) {
		if (preemptible()) {
			/*
			 * NEEDS_BH_LOCK 配置用真实 per-CPU local lock 串行化；另一
			 * RT 变体只禁止迁移。二者都保证后续 this_cpu 操作不因
			 * 任务迁移而落到另一 CPU。
			 */
			if (IS_ENABLED(CONFIG_PREEMPT_RT_NEEDS_BH_LOCK))
				local_lock(&softirq_ctrl.lock);
			else
				migrate_disable();

			/* Required to meet the RCU bottomhalf requirements. */
			/*
			 * 与外层最终 enable 的 rcu_read_unlock() 配对，使
			 * local_bh_disable() 保持传统 RCU-bh 读侧契约。
			 */
			rcu_read_lock();
		} else {
			/* 不可抢占入口必须尚无其他 per-CPU BH 占用，否则协议失配。 */
			DEBUG_LOCKS_WARN_ON(this_cpu_read(softirq_ctrl.cnt));
		}
	}

	/*
	 * Track the per CPU softirq disabled state. On RT this is per CPU
	 * state to allow preemption of bottom half disabled sections.
	 */
	/*
	 * 计数同时存在于 CPU 和 task：前者抑制错误唤醒，后者让
	 * in_softirq()/in_serving_softirq() 在任务被抢占后仍描述当前任务。
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT_NEEDS_BH_LOCK)) {
		newcnt = this_cpu_add_return(softirq_ctrl.cnt, cnt);
		/*
		 * Reflect the result in the task state to prevent recursion on the
		 * local lock and to make softirq_count() & al work.
		 */
		/*
		 * task 镜像使用总计数而非单层 cnt，既防止嵌套时再次取 local
		 * lock，也让上下文查询看到与 CPU 状态一致的深度。
		 */
		current->softirq_disable_cnt = newcnt;

		/* 仅从零到非零的最外层转换需要通知 lockdep “softirq off”。 */
		if (IS_ENABLED(CONFIG_TRACE_IRQFLAGS) && newcnt == cnt) {
			raw_local_irq_save(flags);
			lockdep_softirqs_off(ip);
			raw_local_irq_restore(flags);
		}
	} else {
		/* 无 BH local lock 的 RT 变体独立累加 task 与 CPU 计数。 */
		bool sirq_dis = false;

		if (!current->softirq_disable_cnt)
			sirq_dis = true;

		this_cpu_add(softirq_ctrl.cnt, cnt);
		current->softirq_disable_cnt += cnt;
		WARN_ON_ONCE(current->softirq_disable_cnt < 0);

		/* 同样只为第一次禁用生成一次 trace 状态转换。 */
		if (IS_ENABLED(CONFIG_TRACE_IRQFLAGS) && sirq_dis) {
			raw_local_irq_save(flags);
			lockdep_softirqs_off(ip);
			raw_local_irq_restore(flags);
		}
	}
}
EXPORT_SYMBOL(__local_bh_disable_ip);

/*
 * __local_bh_enable() - PREEMPT_RT 内部计数回退与最外层保护释放。
 *
 * @cnt：与进入层匹配的计数增量。
 * @unlock：计数归零时是否同时结束 RCU-bh 读侧并释放 local lock/迁移禁止。
 *          handle_softirqs() 前的中间转换会传 false，以保持串行化边界。
 * 入口：调用任务必须拥有相应 BH 禁用层；NEEDS_BH_LOCK 下 task 与 CPU 计数
 *       必须一致。函数只改变当前 task/CPU 状态，不运行 pending softirq。
 * 返回：无直接返回值；最外层且 unlock=true 时恢复迁移/锁与 RCU 状态。
 */
static void __local_bh_enable(unsigned int cnt, bool unlock)
{
	unsigned long flags;
	bool sirq_en = false;
	int newcnt;

	/* 先判断本次是否完成“禁用 -> 启用”的最外层转换，供 lockdep 记账。 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT_NEEDS_BH_LOCK)) {
		DEBUG_LOCKS_WARN_ON(current->softirq_disable_cnt !=
				    this_cpu_read(softirq_ctrl.cnt));
		if (softirq_count() == cnt)
			sirq_en = true;
	} else {
		if (current->softirq_disable_cnt == cnt)
			sirq_en = true;
	}

	if (IS_ENABLED(CONFIG_TRACE_IRQFLAGS) && sirq_en) {
		raw_local_irq_save(flags);
		lockdep_softirqs_on(_RET_IP_);
		raw_local_irq_restore(flags);
	}

	/*
	 * 再实际减计数。释放 RCU/local lock 必须晚于 task/CPU 计数归零，
	 * 否则并发 softirq 可能观察到仍被禁用的旧状态。
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT_NEEDS_BH_LOCK)) {
		newcnt = this_cpu_sub_return(softirq_ctrl.cnt, cnt);
		current->softirq_disable_cnt = newcnt;

		if (!newcnt && unlock) {
			rcu_read_unlock();
			local_unlock(&softirq_ctrl.lock);
		}
	} else {
		current->softirq_disable_cnt -= cnt;
		this_cpu_sub(softirq_ctrl.cnt, cnt);
		if (unlock && !current->softirq_disable_cnt) {
			/* 与最外层 disable 的迁移禁止和 RCU 读锁逆序配对。 */
			migrate_enable();
			rcu_read_unlock();
		} else {
			WARN_ON_ONCE(current->softirq_disable_cnt < 0);
		}
	}
}

/*
 * __local_bh_enable_ip() - PREEMPT_RT 上退出 BH 禁用区并按条件处理 pending。
 *
 * @ip：enable 调用点；本实现仅随接口携带，不延长其生命周期。
 * @cnt：与 disable 时匹配的层计数。
 * 入口：不得处于 hardirq，硬中断必须开启，当前任务持有相应虚拟 BH 锁。
 * 核心过程：关本地中断读取一致的计数/pending；若只是退出嵌套层则仅减计数；
 *           若最外层在可抢占任务上下文且有 pending，则暂时把状态改成
 *           “正在服务 softirq”并同步执行；不可抢占时改为唤醒 ksoftirqd。
 * 返回：无直接返回值；恢复原中断状态和外层进入前的迁移/RCU/锁状态。
 */
void __local_bh_enable_ip(unsigned long ip, unsigned int cnt)
{
	/*
	 * 变量地图：
	 *   preempt_on 入口是否允许抢占，决定能否在当前 task 栈就地处理。
	 *   flags      保存硬中断状态，保护 per-CPU pending 与计数转换。
	 *   pending    本 CPU待处理向量快照，只用于决定是否进入处理器。
	 *   curcnt     当前总嵌套计数；只有等于 cnt 才说明本次退出最外层。
	 */
	bool preempt_on = preemptible();
	unsigned long flags;
	u32 pending;
	int curcnt;

	WARN_ON_ONCE(in_hardirq());
	lockdep_assert_irqs_enabled();

	lock_map_release(&bh_lock_map);

	/* 固定当前 CPU，并防止取 pending 与状态转换之间插入本地硬中断。 */
	local_irq_save(flags);
	if (IS_ENABLED(CONFIG_PREEMPT_RT_NEEDS_BH_LOCK))
		curcnt = this_cpu_read(softirq_ctrl.cnt);
	else
		curcnt = current->softirq_disable_cnt;

	/*
	 * If this is not reenabling soft interrupts, no point in trying to
	 * run pending ones.
	 */
	/*
	 * 上游注释：若当前只退出一个嵌套层，BH 仍处于禁用状态，此时尝试运行
	 * pending 没有意义；统一跳到 out 只减本层计数，最外层退出者再负责分派。
	 */
	if (curcnt != cnt)
		goto out;

	pending = local_softirq_pending();
	if (!pending)
		goto out;

	/*
	 * If this was called from non preemptible context, wake up the
	 * softirq daemon.
	 */
	/*
	 * 上游注释：不可抢占入口不能安全地在当前调用栈同步执行 softirq，
	 * 因此只唤醒守护线程；pending 位保持不变，直到线程真正领取。
	 */
	if (!preempt_on) {
		/* 当前栈不能安全承担处理，只发布线程唤醒，pending 位仍保持。 */
		wakeup_softirqd();
		goto out;
	}

	/*
	 * Adjust softirq count to SOFTIRQ_OFFSET which makes
	 * in_serving_softirq() become true.
	 */
	/*
	 * 最外层 BH-disable 使用 2*OFFSET；执行 action 必须表现为 1*OFFSET。
	 * 先减到 serving 状态且不解锁，保证 __do_softirq() 不与另一执行者重入。
	 */
	cnt = SOFTIRQ_OFFSET;
	__local_bh_enable(cnt, false);
	__do_softirq();

out:
	/* 普通退出减原 cnt；处理路径则再减剩余 OFFSET，并最终释放外层保护。 */
	__local_bh_enable(cnt, preempt_on);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(__local_bh_enable_ip);

/*
 * Invoked from ksoftirqd_run() outside of the interrupt disabled section
 * to acquire the per CPU local lock for reentrancy protection.
 */
/*
 * 上游注释说明：ksoftirqd_run() 在未关中断区调用这里，先取得每 CPU
 * local lock 防止重入，再关闭中断进入 pending 位图的领取协议。
 *
 * 入参：无。返回时本地中断关闭且 softirq 状态为 serving；必须由
 * ksoftirqd_run_end() 配对恢复。获取 RT local lock 时允许发生调度等待。
 */
static inline void ksoftirqd_run_begin(void)
{
	__local_bh_disable_ip(_RET_IP_, SOFTIRQ_OFFSET);
	local_irq_disable();
}

/* Counterpart to ksoftirqd_run_begin() */
/*
 * ksoftirqd_run_end() - 撤销 RT 守护线程的 BH/CPU 串行化边界。
 *
 * 入参、返回值均无；释放虚拟锁和真实 local lock/RCU 边界，校验已经离开
 * 中断上下文，最后开启本地中断。只能与一次成功的 begin 成对使用。
 */
static inline void ksoftirqd_run_end(void)
{
	/* pairs with the lock_map_acquire_read() in ksoftirqd_run_begin() */
	/* 匹配 begin 中 __local_bh_disable_ip() 建立的 lockdep read acquire。 */
	lock_map_release(&bh_lock_map);
	__local_bh_enable(SOFTIRQ_OFFSET, true);
	WARN_ON_ONCE(in_interrupt());
	local_irq_enable();
}

/*
 * RT 下 begin/end 的实际上下文维护已由 ksoftirqd local lock 完成，因此
 * handle_softirqs() 周围这两个 hook 是空操作。二者均无入参、返回与副作用。
 */
static inline void softirq_handle_begin(void) { }
static inline void softirq_handle_end(void) { }

/*
 * should_wake_ksoftirqd() - 判断 RT 上本 CPU 守护线程是否值得唤醒。
 *
 * cnt 非零表示某任务仍占有 BH 禁用区，即使线程醒来亦不能取得执行权；
 * 因此仅在零时返回 true。返回是瞬时布尔值，无副作用和 ownership。
 */
static inline bool should_wake_ksoftirqd(void)
{
	return !this_cpu_read(softirq_ctrl.cnt);
}

/*
 * invoke_softirq() - RT 的硬中断退出策略：只唤醒线程，不在当前栈执行。
 *
 * 入参、返回值均无；若 BH 未被其他任务阻塞，则唤醒当前 CPU ksoftirqd。
 */
static inline void invoke_softirq(void)
{
	if (should_wake_ksoftirqd())
		wakeup_softirqd();
}

/* RT flush 校验只豁免调度向量；该常量把编号转换为 pending 位图掩码。 */
#define SCHED_SOFTIRQ_MASK	BIT(SCHED_SOFTIRQ)

/*
 * flush_smp_call_function_queue() can raise a soft interrupt in a function
 * call. On RT kernels this is undesired and the only known functionalities
 * are in the block layer which is disabled on RT, and in the scheduler for
 * idle load balancing. If soft interrupts get raised which haven't been
 * raised before the flush, warn if it is not a SCHED_SOFTIRQ so it can be
 * investigated.
 */
/*
 * 上游注释说明：flush_smp_call_function_queue() 执行跨 CPU 回调时可能新置
 * softirq。RT 不希望普通功能从该位置产生 softirq；已知例外是 RT 已禁用的
 * block 路径和调度器 idle load balance。因此如果 flush 后出现此前没有的
 * 位，且新增内容不只是 SCHED_SOFTIRQ，就告警以便调查，随后按 RT 规则唤醒。
 *
 * @was_pending：flush 前本 CPU pending 位图快照，按 softirq 编号逐位编码。
 * 返回：无直接返回值。位图未变时无副作用；变化时可能告警并唤醒 ksoftirqd。
 */
void do_softirq_post_smp_call_flush(unsigned int was_pending)
{
	/* flush 后的新快照与旧值比较，判断跨 CPU 回调是否发布了额外工作。 */
	unsigned int is_pending = local_softirq_pending();

	if (unlikely(was_pending != is_pending)) {
		/*
		 * 允许的新增位只有 SCHED_SOFTIRQ；去掉该位后应仍等于旧值。
		 * WARN 不撤销 pending，invoke_softirq() 仍保证工作获得处理机会。
		 */
		WARN_ON_ONCE(was_pending != (is_pending & ~SCHED_SOFTIRQ_MASK));
		invoke_softirq();
	}
}

#else /* CONFIG_PREEMPT_RT */
/* 以上为 PREEMPT_RT 实现；以下是非 RT 内核的 preempt_count 实现。 */

/*
 * This one is for softirq.c-internal use, where hardirqs are disabled
 * legitimately:
 */
/*
 * 非 RT 内部版本允许调用点合法地关着硬中断；它直接操作 preempt_count，
 * 而不是经过可能递归触发 preempt tracer 的通用包装。
 */
#ifdef CONFIG_TRACE_IRQFLAGS
/*
 * __local_bh_disable_ip() - 非 RT/IRQFLAGS 配置下增加 BH 或 serving 计数。
 *
 * @ip：禁用发生的调用地址，供 lockdep 归因。
 * @cnt：SOFTIRQ_DISABLE_OFFSET 或 SOFTIRQ_OFFSET，必须由 enable 精确减回。
 * 入口：不得在 hardirq；函数会保存并关闭本地中断，故不睡眠。
 * 返回：无直接返回值；preempt_count、lockdep 和 preempt trace 已同步更新。
 */
void __local_bh_disable_ip(unsigned long ip, unsigned int cnt)
{
	/* flags 保存调用前硬中断状态，仅覆盖本次记账的极短窗口。 */
	unsigned long flags;

	WARN_ON_ONCE(in_hardirq());

	raw_local_irq_save(flags);
	/*
	 * The preempt tracer hooks into preempt_count_add and will break
	 * lockdep because it calls back into lockdep after SOFTIRQ_OFFSET
	 * is set and before current->softirq_enabled is cleared.
	 * We must manually increment preempt_count here and manually
	 * call the trace_preempt_off later.
	 */
	/*
	 * 必须绕开 preempt_count_add() 的 tracer hook：若 hook 在 OFFSET 已写入
	 * 而 lockdep 尚未看到 softirq-off 的半更新状态回调 lockdep，会错误
	 * 分类锁上下文。这里先裸加计数，随后按明确顺序补 trace。
	 */
	__preempt_count_add(cnt);
	/*
	 * Were softirqs turned off above:
	 */
	/* 仅第一次进入相应 softirq 禁用层时切换 lockdep 状态。 */
	if (softirq_count() == (cnt & SOFTIRQ_MASK))
		lockdep_softirqs_off(ip);
	raw_local_irq_restore(flags);

	if (preempt_count() == cnt) {
#ifdef CONFIG_DEBUG_PREEMPT
		current->preempt_disable_ip = get_lock_parent_ip();
#endif
		trace_preempt_off(CALLER_ADDR0, get_lock_parent_ip());
	}
}
EXPORT_SYMBOL(__local_bh_disable_ip);
#endif /* CONFIG_TRACE_IRQFLAGS */
/* CONFIG_TRACE_IRQFLAGS 专用的非 RT disable 实现到此结束。 */

/*
 * __local_bh_enable() - 非 RT 内部计数回退，不主动处理 pending softirq。
 *
 * @cnt：与 disable 对应的计数值。
 * 入口：硬中断必须关闭，以保证 preempt_count/lockdep/trace 的转换连续。
 * 返回：无直接返回值；恢复一层 preempt/softirq 状态，调用者负责何时开中断。
 */
static void __local_bh_enable(unsigned int cnt)
{
	lockdep_assert_irqs_disabled();

	if (preempt_count() == cnt)
		trace_preempt_on(CALLER_ADDR0, get_lock_parent_ip());

	if (softirq_count() == (cnt & SOFTIRQ_MASK))
		lockdep_softirqs_on(_RET_IP_);

	/* 所有跟踪状态已经恢复后再减真实计数，避免观察到不一致的中间状态。 */
	__preempt_count_sub(cnt);
}

/*
 * Special-case - softirqs can safely be enabled by __do_softirq(),
 * without processing still-pending softirqs:
 */
/*
 * 上游注释强调这是特殊内部出口：__do_softirq() 可以恢复 BH 状态但不递归
 * 处理执行期间再次产生的 pending；外层 handle_softirqs() 会统一决定重启
 * 或交给 ksoftirqd，避免在 enable 中形成无界递归。
 *
 * 入参：无。入口不得处于 hardirq，必须匹配一层 BH disable。
 * 返回：无直接返回值；只减 SOFTIRQ_DISABLE_OFFSET，不分派新工作。
 */
void _local_bh_enable(void)
{
	WARN_ON_ONCE(in_hardirq());
	__local_bh_enable(SOFTIRQ_DISABLE_OFFSET);
}
EXPORT_SYMBOL(_local_bh_enable);

/*
 * __local_bh_enable_ip() - 非 RT 上退出任务的 BH 禁用区并处理本 CPU pending。
 *
 * @ip：enable 调用地址，供 lockdep 标记 softirq-on。
 * @cnt：与进入匹配的计数，通常为 SOFTIRQ_DISABLE_OFFSET。
 * 入口：hardirq 外且硬中断开启。函数保持最后一个 preempt 禁用单位，直到
 *       可选 softirq 分派完成，防止期间迁移或被普通任务抢占。
 * 返回：无直接返回值；恢复计数/中断跟踪，必要时触发调度检查。
 */
void __local_bh_enable_ip(unsigned long ip, unsigned int cnt)
{
	WARN_ON_ONCE(in_hardirq());
	lockdep_assert_irqs_enabled();
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_disable();
#endif
	/*
	 * Are softirqs going to be turned on now:
	 */
	/* 只有正好退出最外层 disable 时，lockdep 才从 softirq-off 切回 on。 */
	if (softirq_count() == SOFTIRQ_DISABLE_OFFSET)
		lockdep_softirqs_on(ip);
	/*
	 * Keep preemption disabled until we are done with
	 * softirq processing:
	 */
	/*
	 * 先减 cnt-1，刻意留下一个 preempt 禁用单位；这样检查 pending 到
	 * do_softirq() 完成之间 current 不会迁移，per-CPU 状态仍属于本 CPU。
	 */
	__preempt_count_sub(cnt - 1);

	if (unlikely(!in_interrupt() && local_softirq_pending())) {
		/*
		 * Run softirq if any pending. And do it in its own stack
		 * as we may be calling this deep in a task call stack already.
		 */
		/*
		 * 任务调用栈可能很深，因此经体系结构入口切到专用 softirq 栈；
		 * 若仍处于任何中断/BH 上下文则保留 pending 给外层退出路径。
		 */
		do_softirq();
	}

	preempt_count_dec();
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_enable();
#endif
	preempt_check_resched();
}
EXPORT_SYMBOL(__local_bh_enable_ip);

/*
 * softirq_handle_begin/end() - 非 RT 的 action 分派上下文记账对。
 *
 * begin 增加 SOFTIRQ_OFFSET，使 in_serving_softirq() 为真；end 精确减回并
 * 校验已退出中断上下文。二者无入参和直接返回值，必须成对且在关中断处调用。
 */
static inline void softirq_handle_begin(void)
{
	__local_bh_disable_ip(_RET_IP_, SOFTIRQ_OFFSET);
}

static inline void softirq_handle_end(void)
{
	__local_bh_enable(SOFTIRQ_OFFSET);
	WARN_ON_ONCE(in_interrupt());
}

/*
 * 非 RT 的 ksoftirqd 已是固定 CPU 的 per-CPU 线程，不需要 RT local lock；
 * begin/end 仅以关开本地中断保护 pending 位图的领取边界。均无入参/返回值。
 */
static inline void ksoftirqd_run_begin(void)
{
	local_irq_disable();
}

static inline void ksoftirqd_run_end(void)
{
	local_irq_enable();
}

/*
 * should_wake_ksoftirqd() - 非 RT 始终允许发出唤醒提示。
 *
 * 返回恒为 true；是否已有线程以及是否已经可运行由 wakeup_softirqd()/
 * 调度器自行处理。无入参和副作用。
 */
static inline bool should_wake_ksoftirqd(void)
{
	return true;
}

/*
 * invoke_softirq() - 非 RT 硬中断退出时选择就地执行或线程接管。
 *
 * 入参、返回值均无。普通模式优先在当前 CPU 立即处理以降低延迟；强制
 * IRQ 线程化且 ksoftirqd 已存在时改为唤醒线程。入口本地中断关闭。
 */
static inline void invoke_softirq(void)
{
	if (!force_irqthreads() || !__this_cpu_read(ksoftirqd)) {
#ifdef CONFIG_HAVE_IRQ_EXIT_ON_IRQ_STACK
		/*
		 * We can safely execute softirq on the current stack if
		 * it is the irq stack, because it should be near empty
		 * at this stage.
		 */
		/*
		 * 若 irq_exit 本就在独立 IRQ 栈上，此时硬中断处理已接近返回，
		 * 栈较空，可直接复用而不再切栈。
		 */
		__do_softirq();
#else
		/*
		 * Otherwise, irq_exit() is called on the task stack that can
		 * be potentially deep already. So call softirq in its own stack
		 * to prevent from any overrun.
		 */
		/*
		 * 否则 irq_exit 可能运行在很深的 task 栈；切换到体系结构提供的
		 * softirq 栈可避免 action 链继续增长导致栈溢出。
		 */
		do_softirq_own_stack();
#endif
	} else {
		wakeup_softirqd();
	}
}

/*
 * do_softirq() - 从任务上下文显式处理当前 CPU 的 pending softirq。
 *
 * 入参：无。若调用者已经处于 hardirq/softirq/BH-disabled 上下文则直接返回，
 * 因为外层退出路径会负责分派，避免递归。
 * 核心过程：保存并关闭本地中断，稳定读取 per-CPU pending；非零时在专用
 *           softirq 栈调用 __do_softirq()；最后恢复原硬中断状态。
 * 返回：无直接返回值。可能执行任意已登记 action，但不保证清空执行期间
 *       持续新产生的工作，超预算部分会交给 ksoftirqd。
 */
asmlinkage __visible void do_softirq(void)
{
	/* pending 是关中断下取得的本 CPU 位图快照；flags 保存入口 IRQ 状态。 */
	__u32 pending;
	unsigned long flags;

	if (in_interrupt())
		return;

	/* 关中断同时固定 CPU，保证检查与切入 softirq 栈之间 pending 不丢失。 */
	local_irq_save(flags);

	pending = local_softirq_pending();

	if (pending)
		do_softirq_own_stack();

	local_irq_restore(flags);
}

#endif /* !CONFIG_PREEMPT_RT */
/* 非 PREEMPT_RT 的 BH 与显式 do_softirq 实现到此结束。 */

/*
 * We restart softirq processing for at most MAX_SOFTIRQ_RESTART times,
 * but break the loop if need_resched() is set or after 2 ms.
 * The MAX_SOFTIRQ_TIME provides a nice upper bound in most cases, but in
 * certain cases, such as stop_machine(), jiffies may cease to
 * increment and so we need the MAX_SOFTIRQ_RESTART limit as
 * well to make sure we eventually return from this method.
 *
 * These limits have been established via experimentation.
 * The two things to balance is latency against fairness -
 * we want to handle softirqs as soon as possible, but they
 * should not be able to lock up the box.
 */
/*
 * 上游注释给出公平性预算的双保险：一次进入最多在约 2ms 内重启十次。
 * 时间界通常限制延迟，但 stop_machine() 等场景可能令 jiffies 暂停推进，
 * 所以还必须有次数上限。二者都不是实时保证，而是实验得到的“尽快处理”
 * 与“不能锁死 CPU/饿死任务”之间的折中；超出的 pending 由 ksoftirqd 接手。
 */
#define MAX_SOFTIRQ_TIME  msecs_to_jiffies(2)
#define MAX_SOFTIRQ_RESTART 10

#ifdef CONFIG_TRACE_IRQFLAGS
/*
 * When we run softirqs from irq_exit() and thus on the hardirq stack we need
 * to keep the lockdep irq context tracking as tight as possible in order to
 * not miss-qualify lock contexts and miss possible deadlocks.
 */
/*
 * 当 irq_exit() 直接在硬中断栈执行 softirq 时，真实硬中断嵌套计数正在退出，
 * 但 lockdep 仍可能把当前点分类为 hardirq。下面的 begin/end 暂时切换为
 * softirq lockdep 上下文，并在结束时恢复原 hardirq 标记，使 action 中取得
 * 的锁被按真实上下文检查；否则可能漏报 hardirq 与 softirq 间的死锁。
 */

/*
 * lockdep_softirq_start() - 把 lockdep 上下文切换为 softirq。
 *
 * 入参：无；只操作当前 CPU 的 lockdep 记账，不改变硬件 IRQ 或 preempt_count。
 * 返回：true 表示入口曾被 lockdep 视为 hardirq，end 必须恢复该状态；
 *       false 表示无需恢复。无对象 ownership，不能睡眠。
 */
static inline bool lockdep_softirq_start(void)
{
	/* in_hardirq 仅记录是否需要在出口补回 hardirq 上下文。 */
	bool in_hardirq = false;

	if (lockdep_hardirq_context()) {
		/* 先退出硬中断分类，再进入 softirq，避免两种上下文在 lockdep 中重叠。 */
		in_hardirq = true;
		lockdep_hardirq_exit();
	}

	lockdep_softirq_enter();

	return in_hardirq;
}

/*
 * lockdep_softirq_end() - 结束 softirq lockdep 分类并按入口快照恢复 hardirq。
 *
 * @in_hardirq：必须是配对 start 的返回值。
 * 返回：无直接返回值；仅改变 lockdep 记账，不改变真实中断状态。
 */
static inline void lockdep_softirq_end(bool in_hardirq)
{
	lockdep_softirq_exit();

	if (in_hardirq)
		lockdep_hardirq_enter();
}
#else
/*
 * 未启用 IRQFLAGS 跟踪时，lockdep 上下文切换退化为空：start 恒返回 false，
 * end 忽略该值。这样主处理循环无需条件编译分叉，且没有运行时副作用。
 */
static inline bool lockdep_softirq_start(void) { return false; }
static inline void lockdep_softirq_end(bool in_hardirq) { }
#endif

/*
 * handle_softirqs() - 领取并分派当前 CPU 的 softirq pending 位图。
 *
 * 调用关系：__do_softirq() 的核心实现；也由 ksoftirqd 直接调用。各 action
 *           可再次 raise 任意向量，形成下一轮 pending。
 * @ksirqd：true 表示当前由 ksoftirqd 线程执行；false 表示硬中断退出、
 *          BH 恢复或显式 do_softirq 的就地路径。
 * 入口：本地硬中断关闭，当前 CPU 已固定；调用者已选择合适栈。函数执行
 *       action 时开启硬中断但保持 softirq serving 上下文，action 不应睡眠。
 * 核心过程：暂存并清除 pending -> 按最低置位编号逐个调用 action -> 再关
 *           中断收集新 pending -> 在时间、次数和 need_resched 预算内重启，
 *           否则唤醒 ksoftirqd。
 * 返回：无直接返回值。已领取向量至少被调用一次；持续新到达的工作可能仍
 *       pending。current 的 PF_MEMALLOC、记账、lockdep 与上下文状态均恢复。
 */
static void handle_softirqs(bool ksirqd)
{
	/*
	 * 变量地图：
	 *   end         本轮允许就地重启的 jiffies 截止点。
	 *   old_flags   current 原 PF_* 标志快照，只恢复 PF_MEMALLOC 位。
	 *   max_restart 剩余重启次数预算。
	 *   h           指向当前 softirq_vec 槽位，不持有额外引用。
	 *   in_hardirq  lockdep 入口是否需要恢复 hardirq 分类。
	 *   pending     本轮已领取的向量位图；清零全局后由局部快照拥有执行责任。
	 *   softirq_bit ffs() 返回的 1 基最低置位位置，零表示快照耗尽。
	 */
	unsigned long end = jiffies + MAX_SOFTIRQ_TIME;
	unsigned long old_flags = current->flags;
	int max_restart = MAX_SOFTIRQ_RESTART;
	struct softirq_action *h;
	bool in_hardirq;
	__u32 pending;
	int softirq_bit;

	/*
	 * Mask out PF_MEMALLOC as the current task context is borrowed for the
	 * softirq. A softirq handled, such as network RX, might set PF_MEMALLOC
	 * again if the socket is related to swapping.
	 */
	/*
	 * softirq 借用了被中断任务或 ksoftirqd 的 current。不能无条件继承
	 * PF_MEMALLOC，否则普通网络接收可能误用内存回收保留资源；若确实处理
	 * swap 相关 socket，网络路径会按自己的条件重新设置。出口只恢复该位。
	 */
	current->flags &= ~PF_MEMALLOC;

	/* 在关中断状态领取入口时已存在的本 CPU pending 快照。 */
	pending = local_softirq_pending();

	/*
	 * 建立 serving-softirq、lockdep 和 cputime/统计上下文。顺序使 action
	 * 开始前所有观察者都已看到正确分类，出口按逆序撤销。
	 */
	softirq_handle_begin();
	in_hardirq = lockdep_softirq_start();
	account_softirq_enter(current);

restart:
	/* Reset the pending bitmask before enabling irqs */
	/*
	 * 清零只表示本地快照 pending 已领取这些工作，不是丢弃事件。必须在
	 * 开中断前完成：之后 action 或新硬中断再次置位时会留在全局位图，
	 * 循环末尾即可区分“本轮快照”与“执行期间新到达”。
	 */
	set_softirq_pending(0);

	/* action 运行期间允许硬中断抢占，以免 softirq 批处理放大硬件 IRQ 延迟。 */
	local_irq_enable();

	/* 从向量表起点配合位移后的 pending，按编号从低到高扫描置位项。 */
	h = softirq_vec;

	while ((softirq_bit = ffs(pending))) {
		/*
		 * vec_nr 是全局向量编号；prev_count 用来校验 action 是否把自己
		 * 的 preempt/BH 嵌套计数泄漏到框架。h 是静态表借用指针。
		 */
		unsigned int vec_nr;
		int prev_count;

		h += softirq_bit - 1;

		vec_nr = h - softirq_vec;
		prev_count = preempt_count();

		kstat_incr_softirqs_this_cpu(vec_nr);

		/*
		 * trace entry/exit 包围真实函数指针调用。action 无返回值，若有
		 * 新工作通过 raise 发布；框架不替它持有业务对象或处理失败码。
		 */
		trace_softirq_entry(vec_nr);
		h->action();
		trace_softirq_exit(vec_nr);
		if (unlikely(prev_count != preempt_count())) {
			/*
			 * action 未配对 enable/unlock 会污染后续向量的上下文判断。
			 * 这里报告具体向量并强制恢复入口计数，属于损坏隔离措施，
			 * 不能修复 action 已造成的锁或资源错误。
			 */
			pr_err("huh, entered softirq %u %s %p with preempt_count %08x, exited with %08x?\n",
			       vec_nr, softirq_to_name[vec_nr], h->action,
			       prev_count, preempt_count());
			preempt_count_set(prev_count);
		}
		/*
		 * h 越过已处理槽；pending 右移 softirq_bit 位，既删除本次置位，
		 * 也跳过它之前已知为零的位，使下一次 ffs 相对新 h 继续扫描。
		 */
		h++;
		pending >>= softirq_bit;
	}

	if (!IS_ENABLED(CONFIG_PREEMPT_RT) && ksirqd)
		/*
		 * 非 RT 的 ksoftirqd 完成一批工作可作为 RCU quiescent state；
		 * RT 的 RCU/线程语义不同，不能在此沿用该报告。
		 */
		rcu_softirq_qs();

	/* 重新关中断，原子地查看 action/硬中断运行期间积累的新 pending。 */
	local_irq_disable();

	pending = local_softirq_pending();
	if (pending) {
		/*
		 * 只有时间未到、调度器不要求让出 CPU 且仍有次数预算时才重新
		 * 领取。goto restart 会清全局位图并处理这份新快照。
		 */
		if (time_before(jiffies, end) && !need_resched() &&
		    --max_restart)
			goto restart;

		/*
		 * 任一公平性条件失败都保留全局 pending 位，并唤醒线程接管。
		 * 这确保退出当前原子上下文不丢工作，同时给普通任务调度机会。
		 */
		wakeup_softirqd();
	}

	/* 按入口的逆序撤销统计、lockdep 与 serving 状态，并只恢复 PF_MEMALLOC。 */
	account_softirq_exit(current);
	lockdep_softirq_end(in_hardirq);
	softirq_handle_end();
	current_restore_flags(old_flags, PF_MEMALLOC);
}

/*
 * __do_softirq() - softirq 专用栈/体系结构入口的公共分派包装。
 *
 * 入参：无。入口本地中断关闭，调用者已建立合适的栈与 CPU 归属。
 * 返回：无直接返回值；以 ksirqd=false 执行预算循环，余量可能交给线程。
 * __softirq_entry 供体系结构/调试工具标识入口，__visible 防止编译器隐藏符号。
 */
asmlinkage __visible void __softirq_entry __do_softirq(void)
{
	handle_softirqs(false);
}

/**
 * irq_enter_rcu - Enter an interrupt context with RCU watching
 */
/*
 * 上游 kernel-doc：在 RCU 已经 watching 的前提下进入硬中断上下文。
 *
 * 入参：无。调用者已完成体系结构低级入口与必要的 RCU/context-tracking
 *       转换，本函数不取得对象引用。
 * 过程：增加 HARDIRQ_OFFSET/lockdep 状态，修复延迟 hrtimer，按 NO_HZ
 *       条件通知 tick 子系统，最后开始 hardirq 时间记账。
 * 返回：无直接返回值；current CPU 被标记为 hardirq 上下文，必须由
 *       irq_exit_rcu() 或等价路径配对，函数不能睡眠。
 */
void irq_enter_rcu(void)
{
	/*
	 * __irq_enter_raw() 只更新 preempt_count 与 lockdep，刻意不做时间
	 * 记账；记账放到 hrtimer/tick 修复之后，由本函数末尾显式完成。
	 */
	__irq_enter_raw();

	/*
	 * If this is a nested interrupt that hits the exit_to_user_mode_loop
	 * where it has enabled interrupts but before it has hit schedule() we
	 * could have hrtimers in an undefined state. Fix it up here.
	 */
	/*
	 * 嵌套中断可能击中 exit_to_user_mode_loop 已开中断、但尚未 schedule()
	 * 的窗口，那里延迟重挂的 hrtimer 状态尚未收束。先重挂，避免后续 tick/
	 * softirq 路径观察或运行一个未正确 armed 的定时器。
	 */
	hrtimer_rearm_deferred();

	/*
	 * NO_HZ_FULL CPU，或 idle task 的最外层硬中断，需要让 tick 子系统更新
	 * 停 tick 期间的时间状态。irq_count 等于一个 HARDIRQ_OFFSET 用来排除
	 * 嵌套硬中断；嵌套层不重复执行昂贵的 tick 进入处理。
	 */
	if (tick_nohz_full_cpu(smp_processor_id()) ||
	    (is_idle_task(current) && (irq_count() == HARDIRQ_OFFSET)))
		tick_irq_enter();

	account_hardirq_enter(current);
}

/**
 * irq_enter - Enter an interrupt context including RCU update
 */
/*
 * 上游 kernel-doc：这是完整硬中断入口，除 irq_enter_rcu() 的工作外还先
 * 通过 context tracking 通知 RCU 从 EQS/用户态重新 watching。
 *
 * 入参、返回值均无。入口可能来自 idle/用户态或内核态；不可睡眠。
 * ct_irq_enter() 必须先于任何需要 RCU 保护的中断工作，出口由 irq_exit()
 * 以相反顺序配对。
 */
void irq_enter(void)
{
	ct_irq_enter();
	irq_enter_rcu();
}

/*
 * tick_irq_exit() - 硬中断最外层退出时把定时轮/NO_HZ 状态传播给 tick 子系统。
 *
 * 入参、返回值均无。仅 CONFIG_NO_HZ_COMMON 下有实际工作；读取当前 CPU 与
 * 调度状态的瞬时快照，不转移 ownership。必须在硬中断上下文记账退出之后、
 * 真正返回被中断上下文之前调用。
 */
static inline void tick_irq_exit(void)
{
#ifdef CONFIG_NO_HZ_COMMON
	/* cpu 是当前处理器编号；整个 IRQ 退出路径禁止迁移，值持续有效。 */
	int cpu = smp_processor_id();

	/* Make sure that timer wheel updates are propagated */
	/*
	 * 上游注释：确保定时轮更新传播出去。调度核心判断 CPU 将保持 idle 且
	 * 无 resched，或该 CPU 属于 nohz_full 时，才需要 NO_HZ 退出钩子。
	 */
	if ((sched_core_idle_cpu(cpu) && !need_resched()) || tick_nohz_full_cpu(cpu)) {
		/*
		 * 只在已经退出最外层 hardirq 时调用；嵌套层仍由外层负责最终
		 * tick 收束，提前处理会误判 CPU 即将回到 idle/用户态。
		 */
		if (!in_hardirq())
			tick_nohz_irq_exit();
	}
#endif
}

#ifdef CONFIG_IRQ_FORCED_THREADING
/*
 * 强制 IRQ 线程化时，每 CPU ktimerd 指针由 smpboot 管理；pending_timer_softirq
 * 是专门留给定时器线程的向量位图。它与普通 local_softirq_pending 分开，
 * 防止低优先级 ksoftirqd 延迟需要更及时唤醒任务的 timer softirq。
 */
DEFINE_PER_CPU(struct task_struct *, ktimerd);
DEFINE_PER_CPU(unsigned long, pending_timer_softirq);

/*
 * wake_timersd() - 唤醒当前 CPU 的 ktimers/%u 线程。
 *
 * 入参：无。入口在当前 CPU 且不可迁移，借用 smpboot 管理的 task 指针；
 * 返回：无直接返回值。线程尚未建立时无副作用，否则发出幂等唤醒。
 */
static void wake_timersd(void)
{
	struct task_struct *tsk = __this_cpu_read(ktimerd);

	if (tsk)
		wake_up_process(tsk);
}

#else

/*
 * 未启用强制 IRQ 线程化时没有 ktimerd；该空实现保持退出主线统一。
 * 入参、返回值和副作用均无。
 */
static inline void wake_timersd(void) { }

#endif

/*
 * __irq_exit_rcu() - 完成硬中断记账，必要时分派 softirq/timer 线程并更新 tick。
 *
 * 入参：无。入口仍计为 hardirq 上下文；按体系结构约定保证或主动关闭本地
 *       中断。调用者负责随后完成 context tracking 与 lockdep 的最终退出。
 * 返回：无直接返回值；HARDIRQ_OFFSET 已减去，最外层且有普通 pending 时
 *       就地执行或唤醒 ksoftirqd，强制线程化 timer pending 可唤醒 ktimerd。
 */
static inline void __irq_exit_rcu(void)
{
#ifndef __ARCH_IRQ_EXIT_IRQS_DISABLED
	/* 通用体系结构在此关中断，稳定 per-CPU 计数、pending 与线程唤醒决策。 */
	local_irq_disable();
#else
	/* 声明自行保证 IRQ 关闭的体系结构若违反契约，会由 lockdep 立即指出。 */
	lockdep_assert_irqs_disabled();
#endif
	/*
	 * 先结束时间记账，再减 HARDIRQ_OFFSET；减完后 in_interrupt() 才能判断
	 * 是否已经离开最外层，嵌套硬中断不会提前运行 softirq。
	 */
	account_hardirq_exit(current);
	preempt_count_sub(HARDIRQ_OFFSET);
	if (!in_interrupt() && local_softirq_pending()) {
		/*
		 * If we left hrtimers unarmed, make sure to arm them now,
		 * before enabling interrupts to run SoftIRQ.
		 */
		/*
		 * 若入口阶段留下延迟重挂的 hrtimer，必须在允许 softirq 开中断
		 * 之前补齐 armed 状态，否则定时器回调可能面对不完整的时序状态。
		 */
		hrtimer_rearm_deferred();
		/* RT 唤醒线程；非 RT 按 forced-threading 和栈能力选择就地或线程。 */
		invoke_softirq();
	}

	/*
	 * 强制线程化 timer 位图与普通 pending 独立；只有已离开 NMI/hardirq
	 * 才唤醒 ktimerd，避免在嵌套中断中过早调度线程。
	 */
	if (IS_ENABLED(CONFIG_IRQ_FORCED_THREADING) && force_irqthreads() &&
	    local_timers_pending_force_th() && !(in_nmi() | in_hardirq()))
		wake_timersd();

	tick_irq_exit();
}

/**
 * irq_exit_rcu() - Exit an interrupt context without updating RCU
 *
 * Also processes softirqs if needed and possible.
 */
/*
 * 上游 kernel-doc：退出一个“RCU 仍在 watching”的硬中断上下文，并在条件
 * 允许时处理 softirq。本函数不调用 ct_irq_exit()，适合已由调用者管理
 * context tracking 的入口。
 *
 * 入参、返回值均无；入口必须与 irq_enter_rcu() 配对且不可睡眠。返回后
 * hardirq/lockdep 状态均结束，RCU watching 状态保持不变。
 */
void irq_exit_rcu(void)
{
	__irq_exit_rcu();
	 /* must be last! */
	/*
	 * 上游要求它必须最后执行：在 softirq 与 tick 退出工作完成前，lockdep
	 * 仍应知道这是硬中断返回路径；提前退出会错误分类其中取得的锁。
	 */
	lockdep_hardirq_exit();
}

/**
 * irq_exit - Exit an interrupt context, update RCU and lockdep
 *
 * Also processes softirqs if needed and possible.
 */
/*
 * 上游 kernel-doc：完整退出硬中断，同时更新 RCU/context tracking 与
 * lockdep，并尽可能处理 softirq。
 *
 * 入参、返回值均无；与 irq_enter() 配对。__irq_exit_rcu() 先完成所有仍需
 * RCU watching 的工作，ct_irq_exit() 再按被中断上下文恢复状态，lockdep
 * hardirq exit 必须最后发布。
 */
void irq_exit(void)
{
	__irq_exit_rcu();
	ct_irq_exit();
	 /* must be last! */
	/* 与 irq_exit_rcu() 相同，lockdep 的 hardirq 退出必须是最后状态转换。 */
	lockdep_hardirq_exit();
}

/*
 * This function must run with irqs disabled!
 */
/* 上游契约：本函数必须在本地硬中断关闭时运行，以保护 per-CPU pending 位图。 */
/*
 * raise_softirq_irqoff() - 发布本 CPU softirq，并保证任务上下文有执行者。
 *
 * @nr：softirq 向量编号，范围必须为 [0, NR_SOFTIRQS)，调用者保证合法。
 * 入口：本地硬中断关闭；可来自 hardirq、softirq 或普通任务上下文。
 * 返回：无直接返回值。对应 pending 位已设置；若普通上下文且允许唤醒，
 *       ksoftirqd 被唤醒。中断/BH 上下文只置位，外层退出负责实际执行。
 */
inline void raise_softirq_irqoff(unsigned int nr)
{
	/* 先置位再决定唤醒，确保线程一旦运行就能观察到工作。 */
	__raise_softirq_irqoff(nr);

	/*
	 * If we're in an interrupt or softirq, we're done
	 * (this also catches softirq-disabled code). We will
	 * actually run the softirq once we return from
	 * the irq or softirq.
	 *
	 * Otherwise we wake up ksoftirqd to make sure we
	 * schedule the softirq soon.
	 */
	/*
	 * in_interrupt() 也涵盖仅 local_bh_disable 的非 RT 上下文；这些路径
	 * 有明确的 enable/exit 分派点。普通任务上下文没有该保证，所以唤醒线程。
	 */
	if (!in_interrupt() && should_wake_ksoftirqd())
		wakeup_softirqd();
}

/*
 * raise_softirq() - 可在硬中断开启处调用的安全发布包装。
 *
 * @nr：合法 softirq 向量编号。
 * 入口：任意不睡眠内核上下文；函数短暂保存并关闭本地硬中断，以固定 CPU
 *       并与本 CPU pending 的其他更新串行化。
 * 返回：无直接返回值；恢复原 IRQ 状态，发布与唤醒语义同 irqoff 版本。
 */
void raise_softirq(unsigned int nr)
{
	/* flags 仅保存调用前本地硬中断状态，不携带出函数。 */
	unsigned long flags;

	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}

/*
 * __raise_softirq_irqoff() - softirq 发布协议的最小原语。
 *
 * @nr：合法向量编号；1UL << nr 映射为 pending 位。
 * 入口：本地硬中断必须关闭，调用者已固定 CPU。
 * 返回：无直接返回值；先发出 raise trace，再以 OR 保留其他已 pending 位。
 *       本函数不唤醒线程，适合已有明确退出/批处理策略的内部路径。
 */
void __raise_softirq_irqoff(unsigned int nr)
{
	lockdep_assert_irqs_disabled();
	trace_softirq_raise(nr);
	or_softirq_pending(1UL << nr);
}

/*
 * open_softirq() - 为一个固定 softirq 编号登记全局 action。
 *
 * @nr：向量编号，必须位于 [0, NR_SOFTIRQS)。
 * @action：无参、无返回值的回调函数，必须长期有效且不可为 NULL；所有权不
 *          转移，数组只保存函数地址。
 * 入口：启动初始化阶段调用，尚无并发分派；本函数不加锁、不能用于运行时
 *       替换 action。
 * 返回：无直接返回值；之后任一 CPU 处理 nr 时都会调用该 action。
 */
void open_softirq(int nr, void (*action)(void))
{
	softirq_vec[nr].action = action;
}

/*
 * Tasklets
 */
/*
 * 上游标题以下实现旧 tasklet API。tasklet 不是独立线程，而是挂到本 CPU
 * TASKLET_SOFTIRQ 或 HI_SOFTIRQ 链表的回调对象；API 已弃用，新代码通常应
 * 优先选择线程化 IRQ 或 workqueue。这里仍需维护历史驱动的调度与销毁契约。
 */
/*
 * struct tasklet_head - 一个 CPU、一个优先级的 tasklet 单向 FIFO 队列。
 *
 * head：首个待处理 tasklet 的借用指针，NULL 表示空队列。
 * tail：始终指向“最后一个 next 槽”或空队列的 &head，使追加无需遍历。
 * 队列对象为 per-CPU 静态存储；本 CPU 通过关硬中断与硬中断中的调度者互斥。
 * tasklet 自身由调用子系统创建/销毁，入队不会增加独立引用，调用者销毁前
 * 必须用 tasklet_kill() 等协议确保对象既不在队列中也未运行。
 */
struct tasklet_head {
	struct tasklet_struct *head;
	struct tasklet_struct **tail;
};

/* 普通与高优先级 tasklet 各有一套 per-CPU FIFO，分别由两个 softirq 消费。 */
static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec);
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec);

/*
 * __tasklet_schedule_common() - 把已取得 SCHED 位的 tasklet 追加到本 CPU 队列。
 *
 * 调用关系：tasklet_schedule()/tasklet_hi_schedule() 先以 test_and_set_bit()
 *           赢得 SCHED 位，随后经两个包装器到此；对应 softirq 消费队列。
 * @t：待调度 tasklet 的借用指针；调用者保证对象已初始化、SCHED 位已置位，
 *     且在执行或 kill 完成前保持存活。
 * @headp：普通或高优先级 per-CPU 队列描述符。
 * @softirq_nr：与 headp 匹配的 TASKLET_SOFTIRQ 或 HI_SOFTIRQ。
 * 入口：可从硬中断等原子上下文调用；不睡眠。函数自行保存/关闭本地中断，
 *       从而固定 CPU 并与该 CPU 的队列摘取串行化。
 * 返回：无直接返回值；t 成为 FIFO 尾节点，相应 softirq pending 位已发布。
 */
static void __tasklet_schedule_common(struct tasklet_struct *t,
				      struct tasklet_head __percpu *headp,
				      unsigned int softirq_nr)
{
	/*
	 * head 是关中断后取得的本 CPU 队列借用指针；flags 保存调用前 IRQ 状态。
	 * 二者都只在本函数临界区有效。
	 */
	struct tasklet_head *head;
	unsigned long flags;

	local_irq_save(flags);
	head = this_cpu_ptr(headp);
	/*
	 * tail 指向当前最后一个 next 槽：写入 t 即完成链接，再把 tail 前移到
	 * t->next。SCHED 位保证同一 t 不会被并发追加两次而破坏单链表。
	 */
	t->next = NULL;
	*head->tail = t;
	head->tail = &(t->next);
	raise_softirq_irqoff(softirq_nr);
	local_irq_restore(flags);
}

/*
 * __tasklet_schedule() - 普通优先级 tasklet 的已置位入队包装。
 *
 * @t：已初始化且由外层 tasklet_schedule() 首次置 SCHED 位的借用对象。
 * 返回：无直接返回值；对象进入当前 CPU tasklet_vec 并发布 TASKLET_SOFTIRQ。
 * 该内部接口不重复测试 SCHED，调用者不得直接对已入队对象重复调用。
 */
void __tasklet_schedule(struct tasklet_struct *t)
{
	__tasklet_schedule_common(t, &tasklet_vec,
				  TASKLET_SOFTIRQ);
}
EXPORT_SYMBOL(__tasklet_schedule);

/*
 * __tasklet_hi_schedule() - 高优先级 tasklet 的已置位入队包装。
 *
 * @t：已取得 SCHED 位且在处理完成前保持存活的 tasklet。
 * 返回：无直接返回值；进入 tasklet_hi_vec 并发布 HI_SOFTIRQ。同步与普通
 * 版本相同，区别只是向量优先级和队列。
 */
void __tasklet_hi_schedule(struct tasklet_struct *t)
{
	__tasklet_schedule_common(t, &tasklet_hi_vec,
				  HI_SOFTIRQ);
}
EXPORT_SYMBOL(__tasklet_hi_schedule);

/*
 * tasklet_clear_sched() - 原子清除 tasklet 的 SCHED 位并校验状态协议。
 *
 * @t：当前执行者或 kill 路径独占处理的 tasklet 借用指针。
 * 返回：此前 SCHED 为 1 时清除并返回 true；若已为 0 则告警并返回 false。
 * test_and_clear_wake_up_bit() 还会唤醒等待该位变化的 tasklet_kill() 等路径。
 * 清位后新的 tasklet_schedule() 可以再次入队，所以调用回调前的对象存活
 * 与自重调度必须由 tasklet API 的 RUN/kill 协议共同保证。
 */
static bool tasklet_clear_sched(struct tasklet_struct *t)
{
	if (test_and_clear_wake_up_bit(TASKLET_STATE_SCHED, &t->state))
		return true;

	/* 到达这里表示队列项与状态位失配；日志同时指出新旧回调 ABI 的函数。 */
	WARN_ONCE(1, "tasklet SCHED state not set: %s %pS\n",
		  t->use_callback ? "callback" : "func",
		  t->use_callback ? (void *)t->callback : (void *)t->func);

	return false;
}

#ifdef CONFIG_PREEMPT_RT
/*
 * struct tasklet_sync_callback - RT 上协调 tasklet 回调与同步等待者的 per-CPU 状态。
 *
 * cb_lock：tasklet_action_common() 执行回调批次时持有；RT spinlock 支持优先级
 *          继承，使等待取消/同步的高优先级任务不会被低优先级回调无限阻塞。
 * cb_waiters：正在尝试取得 cb_lock 的取消等待者数量，供回调完成一个 tasklet
 *             后决定是否主动让锁。对象与 CPU 同寿命，字段由原子操作/锁保护。
 */
struct tasklet_sync_callback {
	spinlock_t	cb_lock;
	atomic_t	cb_waiters;
};

/* 每 CPU 回调协调器静态初始化；无等待者且锁未持有。 */
static DEFINE_PER_CPU(struct tasklet_sync_callback, tasklet_sync_callback) = {
	.cb_lock	= __SPIN_LOCK_UNLOCKED(tasklet_sync_callback.cb_lock),
	.cb_waiters	= ATOMIC_INIT(0),
};

/*
 * tasklet_lock_callback() - RT 上取得当前 CPU 的回调批次锁。
 *
 * 入参、返回值均无。由 tasklet_action_common() 在开硬中断后调用；可能因
 * RT mutex 化的 spinlock 等待，成功后直到 unlock 期间同 CPU 同步者不能越过。
 */
static void tasklet_lock_callback(void)
{
	spin_lock(this_cpu_ptr(&tasklet_sync_callback.cb_lock));
}

/*
 * tasklet_unlock_callback() - 释放当前 CPU 的 RT 回调批次锁。
 *
 * 入参、返回值均无；必须与同 CPU 的 tasklet_lock_callback() 配对。
 */
static void tasklet_unlock_callback(void)
{
	spin_unlock(this_cpu_ptr(&tasklet_sync_callback.cb_lock));
}

/*
 * tasklet_callback_cancel_wait_running() - RT 原子等待路径让回调执行者获得推进机会。
 *
 * 入参、返回值均无。等待者先增加 cb_waiters，再取得并释放 cb_lock；若当前
 * tasklet 回调持锁，RT 锁会建立优先级继承并等待其在安全点交锁。计数只覆盖
 * 等锁窗口，防止 tasklet_callback_sync_wait_running() 错过交接请求。
 */
static void tasklet_callback_cancel_wait_running(void)
{
	/* sync_cb 是当前 CPU 静态对象的借用指针，调用期间任务不得迁移。 */
	struct tasklet_sync_callback *sync_cb = this_cpu_ptr(&tasklet_sync_callback);

	atomic_inc(&sync_cb->cb_waiters);
	spin_lock(&sync_cb->cb_lock);
	atomic_dec(&sync_cb->cb_waiters);
	spin_unlock(&sync_cb->cb_lock);
}

/*
 * tasklet_callback_sync_wait_running() - RT 回调完成一个 tasklet 后向等待者交锁。
 *
 * 入参、返回值均无。调用者当前持有本 CPU cb_lock；若观察到等待者，则主动
 * unlock/lock 一次，让取消路径取得锁并推进，避免当前 softirq 连续处理队列
 * 导致等待者与被等待 tasklet 形成活锁。返回时调用者重新持锁。
 */
static void tasklet_callback_sync_wait_running(void)
{
	struct tasklet_sync_callback *sync_cb = this_cpu_ptr(&tasklet_sync_callback);

	if (atomic_read(&sync_cb->cb_waiters)) {
		spin_unlock(&sync_cb->cb_lock);
		spin_lock(&sync_cb->cb_lock);
	}
}

#else /* !CONFIG_PREEMPT_RT: */
/* 以下为空的非 PREEMPT_RT 回调同步钩子。 */

/*
 * 非 RT 不需要回调批次的可调度锁与优先级继承；三个 hook 均为空操作，
 * 入参、返回与副作用均无，保留统一的 tasklet 主循环结构。
 */
static void tasklet_lock_callback(void) { }
static void tasklet_unlock_callback(void) { }
static void tasklet_callback_sync_wait_running(void) { }

#ifdef CONFIG_SMP
/*
 * 非 RT SMP 的 tasklet RUN 位等待使用 cpu_relax()，无需 RT 交锁协议；
 * 此配置占位函数无入参、返回与副作用。
 */
static void tasklet_callback_cancel_wait_running(void) { }
#endif
#endif /* !CONFIG_PREEMPT_RT */
/* PREEMPT_RT 与非 RT 的 tasklet 回调同步辅助实现到此汇合。 */

/*
 * tasklet_action_common() - 摘取一个 per-CPU tasklet 队列并逐个尝试执行。
 *
 * @tl_head：当前 CPU 普通或高优先级队列的借用指针。
 * @softirq_nr：队列对应向量，重排失败项时用它重新置 pending。
 * 入口：处于 softirq serving 上下文，初始硬中断开启；不可睡眠式执行普通
 *       业务。RT 回调锁可能按 RT spinlock 规则阻塞/继承优先级。
 * 过程：关中断整体摘链 -> 开中断逐项获取 RUN 位 -> count 为零时清 SCHED
 *       并调用新/旧 ABI 回调 -> 正在别处运行或被 disable 的对象重新入尾。
 * 返回：无直接返回值。可执行项完成一次；未执行项仍保持 SCHED 并重新 pending。
 */
static void tasklet_action_common(struct tasklet_head *tl_head,
				  unsigned int softirq_nr)
{
	/* list 独占持有刚从 per-CPU 队列摘下的链；节点对象仍归各调用者所有。 */
	struct tasklet_struct *list;

	/*
	 * 关本地中断后一次性把共享队列移到局部 list，并把共享头恢复为空队列。
	 * 此后新 schedule 可安全追加到新的共享队列，不必等待整个批次执行完。
	 */
	local_irq_disable();
	list = tl_head->head;
	tl_head->head = NULL;
	tl_head->tail = &tl_head->head;
	local_irq_enable();

	/* RT 上以回调锁包住批次，并在每个回调后按等待者情况提供交接点。 */
	tasklet_lock_callback();
	while (list) {
		/* t 是当前节点；先推进局部 list，允许 t->next 随后用于重新入队。 */
		struct tasklet_struct *t = list;

		list = list->next;

		/*
		 * RUN 位在 SMP/RT 上保证同一实例不会跨 CPU 并发。取得 RUN 后仍
		 * 要检查 count：disable 可能已阻止回调，但调度请求必须保留。
		 */
		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				/*
				 * 清 SCHED 是本次执行真正消费调度请求的边界。清位后
				 * 回调自身或其他 CPU 可再次 schedule，形成未来一次执行。
				 */
				if (tasklet_clear_sched(t)) {
					if (t->use_callback) {
						/* 新 ABI 直接把对象传给 callback，便于 container_of。 */
						trace_tasklet_entry(t, t->callback);
						t->callback(t);
						trace_tasklet_exit(t, t->callback);
					} else {
						/* 旧 ABI 只传初始化时保存的 unsigned long data。 */
						trace_tasklet_entry(t, t->func);
						t->func(t->data);
						trace_tasklet_exit(t, t->func);
					}
				}
				/*
				 * 回调返回或异常状态跳过后释放 RUN 并唤醒等待运行结束者。
				 * RT 随后检查是否应暂时交出批次锁给 kill/disable 等待者。
				 */
				tasklet_unlock(t);
				tasklet_callback_sync_wait_running();
				continue;
			}
			/* count 非零表示 disabled：不能清 SCHED，否则 enable 后会丢请求。 */
			tasklet_unlock(t);
		}

		/*
		 * 未取得 RUN 或 tasklet 被 disable 时，保持 SCHED=1 并重新追加到
		 * 当前 CPU 队尾。关中断保护 tail 指针，并直接置同一 softirq 位；
		 * 预算机制避免持续不可运行项把 CPU 困在无界循环。
		 */
		local_irq_disable();
		t->next = NULL;
		*tl_head->tail = t;
		tl_head->tail = &t->next;
		__raise_softirq_irqoff(softirq_nr);
		local_irq_enable();
	}
	/* 整个局部快照已消费或重新入队，释放 RT 批次锁。 */
	tasklet_unlock_callback();
}

/*
 * tasklet_action() - 普通 TASKLET_SOFTIRQ 的登记 action。
 *
 * 入参、返回值均无。先让 workqueue 处理复用该 softirq 的普通优先级工作，
 * 再消费当前 CPU tasklet_vec；__latent_entropy 允许该控制流贡献熵估计。
 */
static __latent_entropy void tasklet_action(void)
{
	workqueue_softirq_action(false);
	tasklet_action_common(this_cpu_ptr(&tasklet_vec), TASKLET_SOFTIRQ);
}

/*
 * tasklet_hi_action() - 高优先级 HI_SOFTIRQ 的登记 action。
 *
 * 入参、返回值均无。与普通版本相同，但处理 workqueue 高优先级部分和
 * tasklet_hi_vec；优先级只影响 softirq 编号/调度次序，不提供额外互斥。
 */
static __latent_entropy void tasklet_hi_action(void)
{
	workqueue_softirq_action(true);
	tasklet_action_common(this_cpu_ptr(&tasklet_hi_vec), HI_SOFTIRQ);
}

/*
 * tasklet_setup() - 以新 callback(struct tasklet_struct *) ABI 初始化 tasklet。
 *
 * @t：调用者提供的未发布对象，函数原地初始化；对象存储与最终释放始终归
 *     调用者，调用者必须保证当前未入队、未运行。
 * @callback：长期有效且不可为 NULL 的回调函数；只保存函数地址，不转移所有权。
 * 入口：初始化阶段，无并发 schedule/kill；不睡眠。
 * 返回：无直接返回值；t 处于未调度、未运行、enabled 状态，之后可调用
 *       tasklet_schedule()。重复初始化活跃对象会破坏队列与状态协议。
 */
void tasklet_setup(struct tasklet_struct *t,
		   void (*callback)(struct tasklet_struct *))
{
	/*
	 * next=NULL 且 state=0 表示未入队；count=0 表示 enabled。
	 * use_callback 选择 union 中 callback 分支，data 在新 ABI 下清零不用。
	 */
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->callback = callback;
	t->use_callback = true;
	t->data = 0;
}
EXPORT_SYMBOL(tasklet_setup);

/*
 * tasklet_init() - 以旧 func(unsigned long) ABI 初始化 tasklet。
 *
 * @t：调用者拥有的未发布 tasklet 输出对象，调用期间不可并发访问。
 * @func：长期有效且不可为 NULL 的旧式回调函数指针，仅借用保存。
 * @data：回调执行时原样传入的无符号长整型载荷，框架不解释其 ownership；
 *        若编码指针，调用者负责对象寿命与转换正确性。
 * 返回：无直接返回值；t 成为 enabled、未调度对象。该旧 API 仅为兼容保留。
 */
void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	/* 状态初始化与新 ABI 相同，仅 union 选择 func 并保存调用者 data。 */
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->use_callback = false;
	t->data = data;
}
EXPORT_SYMBOL(tasklet_init);

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
/*
 * Do not use in new code. Waiting for tasklets from atomic contexts is
 * error prone and should be avoided.
 */
/*
 * 上游警告：新代码不要从原子上下文忙等 tasklet。等待者可能阻塞负责执行
 * tasklet 的同一 CPU/线程，容易造成活锁；优先改用可睡眠同步或其他异步机制。
 */
/*
 * tasklet_unlock_spin_wait() - 在不能睡眠的旧调用路径等待 RUN 位清除。
 *
 * @t：调用者保证仍存活的 tasklet 借用指针；函数不取得引用。
 * 入口：SMP 或 PREEMPT_RT。非 RT 使用 cpu_relax() 忙等；RT 必须通过回调锁
 *       交接让被抢占的 softirq/ksoftirqd 获得推进机会。
 * 返回：无直接返回值；返回时观察到 RUN=0，但不保证 tasklet 未再次调度，
 *       也不清 SCHED。调用者需用 disable/kill 等更高层协议稳定状态。
 */
void tasklet_unlock_spin_wait(struct tasklet_struct *t)
{
	/* RUN 可能由另一 CPU 清除，test_bit 每轮重新观察共享状态。 */
	while (test_bit(TASKLET_STATE_RUN, &(t)->state)) {
		if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
			/*
			 * Prevent a live lock when current preempted soft
			 * interrupt processing or prevents ksoftirqd from
			 * running.
			 */
			/*
			 * 上游说明：若 current 抢占了 softirq 执行者或阻止
			 * ksoftirqd 运行，纯忙等永远等不到 RUN 清除；RT 回调锁
			 * 协议提供优先级继承和交接点以打破该活锁。
			 */
			tasklet_callback_cancel_wait_running();
		} else {
			/* 非 RT 只给处理器自旋提示；调用者必须保证执行 CPU 能继续前进。 */
			cpu_relax();
		}
	}
}
EXPORT_SYMBOL(tasklet_unlock_spin_wait);
#endif

/*
 * tasklet_kill() - 同步取消一个 tasklet 的已调度/运行实例。
 *
 * @t：调用者拥有且在函数返回前保持存活的 tasklet；函数不释放对象。
 * 入口：应从可睡眠任务上下文调用，不得由该 tasklet 自己调用。中断上下文
 *       只会被提示告警，继续等待可能死锁。
 * 过程：原子等待并取得 SCHED 位，阻止新的 schedule 把对象再次入队；等待
 *       RUN 位清零，确保任何已开始回调完成；最后清 SCHED 并唤醒位等待者。
 * 返回：无直接返回值；返回时本次调度已取消/完成且未运行。调用者若还允许
 *       并发 schedule，返回后仍可能重新发布，销毁前须先从业务源头止住它。
 */
void tasklet_kill(struct tasklet_struct *t)
{
	/* 中断上下文不能可靠睡眠等待执行者，告警指出错误用法但保持旧 API 行为。 */
	if (in_interrupt())
		pr_notice("Attempt to kill tasklet from interrupt\n");

	/*
	 * wait_on_bit_lock() 等待 SCHED 可取得后将其置 1，相当于暂时占有调度权；
	 * tasklet_schedule() 此时只看到已置位，不会再次把 t 链入队列。
	 */
	wait_on_bit_lock(&t->state, TASKLET_STATE_SCHED, TASK_UNINTERRUPTIBLE);

	/* 再等待已越过清 SCHED 边界的回调释放 RUN，两个状态共同封闭竞态窗口。 */
	tasklet_unlock_wait(t);
	/* 释放 kill 临时持有的 SCHED 位，并唤醒其他等待者。 */
	tasklet_clear_sched(t);
}
EXPORT_SYMBOL(tasklet_kill);

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
/*
 * tasklet_unlock() - 发布 tasklet 回调已结束并唤醒 RUN 位等待者。
 *
 * @t：当前执行者持有 RUN 位的 tasklet 借用指针。
 * 返回：无直接返回值；原子清 RUN 并唤醒 tasklet_kill()/disable 等等待者。
 * 必须位于所有回调对象访问之后，形成“回调完成”发布边界。
 */
void tasklet_unlock(struct tasklet_struct *t)
{
	clear_and_wake_up_bit(TASKLET_STATE_RUN, &t->state);
}
EXPORT_SYMBOL_GPL(tasklet_unlock);

/*
 * tasklet_unlock_wait() - 可睡眠地等待 tasklet 不再运行。
 *
 * @t：等待期间保持存活的 tasklet 借用指针。
 * 入口：任务上下文；调用者必须另有 SCHED/count 协议阻止不受控的新执行。
 * 返回：无直接返回值；观察到 RUN=0。它不清 SCHED、也不取得长期锁。
 */
void tasklet_unlock_wait(struct tasklet_struct *t)
{
	wait_on_bit(&t->state, TASKLET_STATE_RUN, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL_GPL(tasklet_unlock_wait);
#endif

/*
 * softirq_init() - 初始化所有 CPU 的 tasklet 队列并登记两个 tasklet action。
 *
 * 入参：无。启动期在并发调度 tasklet 之前调用一次；可遍历 possible CPU，
 *       包括尚未 online 的 CPU。函数不睡眠且无失败返回。
 * 返回：无直接返回值；每个普通/高优队列建立 head=NULL、tail=&head 不变量，
 *       TASKLET_SOFTIRQ 和 HI_SOFTIRQ 的全局向量回调完成登记。
 */
void __init softirq_init(void)
{
	/* cpu 是 possible CPU 编号，仅在循环迭代内有效。 */
	int cpu;

	for_each_possible_cpu(cpu) {
		/*
		 * BSS 已令 head 为 NULL；这里只把 tail 指向各自 head 槽，建立
		 * O(1) 尾插所需的空队列不变量。
		 */
		per_cpu(tasklet_vec, cpu).tail =
			&per_cpu(tasklet_vec, cpu).head;
		per_cpu(tasklet_hi_vec, cpu).tail =
			&per_cpu(tasklet_hi_vec, cpu).head;
	}

	/* 登记发生在工作发布前，运行期 action 指针只读，无需锁。 */
	open_softirq(TASKLET_SOFTIRQ, tasklet_action);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action);
}

/*
 * ksoftirqd_should_run() - smpboot 线程的“本 CPU 是否有普通 softirq 工作”谓词。
 *
 * @cpu：smpboot 传入的线程所属 CPU 编号；线程已绑到该 CPU，本实现直接读取
 *       local pending，因此参数本身不参与寻址。
 * 返回：位图非零值表示应运行，零表示可继续睡眠；无副作用、无 ownership。
 */
static int ksoftirqd_should_run(unsigned int cpu)
{
	return local_softirq_pending();
}

/*
 * run_ksoftirqd() - ksoftirqd/%u 一次被唤醒后的 softirq 处理迭代。
 *
 * @cpu：线程绑定 CPU 编号，仅满足 smpboot 回调签名；per-CPU 访问用当前 CPU。
 * 入口：任务上下文，线程已绑定且可调度。begin 关闭中断并在 RT 上取得 BH
 *       local lock；必须由 end 配对，即使唤醒后工作已被其他路径取走。
 * 返回：无直接返回值。若仍有 pending，以 ksirqd=true 运行预算循环，恢复
 *       上下文后显式 cond_resched() 给更高/同优先级任务调度机会。
 */
static void run_ksoftirqd(unsigned int cpu)
{
	/* 建立后再次检查 pending，处理“唤醒与真正运行之间工作已被消费”的竞态。 */
	ksoftirqd_run_begin();
	if (local_softirq_pending()) {
		/*
		 * We can safely run softirq on inline stack, as we are not deep
		 * in the task stack here.
		 */
		/*
		 * 上游说明：这是专用守护线程的顶层循环，task 栈很浅，可直接调用
		 * 核心处理器，无需切换 softirq 专用栈。
		 */
		handle_softirqs(true);
		/* action 返回时硬中断再次关闭；end 释放 RT/IRQ 上下文并开中断。 */
		ksoftirqd_run_end();
		/* 即使处理器未触发 need_resched，也在批次边界提供一次自愿调度点。 */
		cond_resched();
		return;
	}
	/* 假唤醒路径也必须释放 begin 建立的状态。 */
	ksoftirqd_run_end();
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * takeover_tasklets() - CPU 下线完成后把其遗留 tasklet 接管到当前 CPU。
 *
 * @cpu：已经死亡的源 CPU 编号，其 tasklet 队列不会再被源 CPU/中断修改。
 * 入口：CPU hotplug teardown 回调，运行于存活的当前 CPU；可调用 workqueue
 *       的对应死亡处理。源队列不需加锁，目标 per-CPU 队列仍以关中断保护。
 * 返回：恒为 0，表示接管完成；源队列变空，节点 ownership 仍属于原调用者，
 *       目标 CPU 的普通/高优 softirq 被置位以执行迁移来的节点。
 */
static int takeover_tasklets(unsigned int cpu)
{
	/* 先让复用 tasklet softirq 的 workqueue 迁移/收束死亡 CPU 状态。 */
	workqueue_softirq_dead(cpu);

	/* CPU is dead, so no lock needed. */
	/*
	 * 上游说明针对源 CPU：它已不再执行，所以读取/清空 per_cpu(..., cpu)
	 * 无需源锁。关闭当前 CPU 中断则保护目标队列免受本地调度者修改。
	 */
	local_irq_disable();

	/* Find end, append list for that CPU. */
	/*
	 * 上游说“找到末尾并追加”：源 tail 已缓存最后 next 槽，因此无需遍历。
	 * 非空判断比较 tail 与 &head；追加后把目标 tail 直接接到源 tail。
	 */
	if (&per_cpu(tasklet_vec, cpu).head != per_cpu(tasklet_vec, cpu).tail) {
		*__this_cpu_read(tasklet_vec.tail) = per_cpu(tasklet_vec, cpu).head;
		__this_cpu_write(tasklet_vec.tail, per_cpu(tasklet_vec, cpu).tail);
		per_cpu(tasklet_vec, cpu).head = NULL;
		per_cpu(tasklet_vec, cpu).tail = &per_cpu(tasklet_vec, cpu).head;
	}
	/*
	 * 即使源普通队列为空也置 pending，产生一次可能的空 action；这简化了
	 * 分支并确保 workqueue_softirq_dead() 转移的相关普通工作得到服务。
	 */
	raise_softirq_irqoff(TASKLET_SOFTIRQ);

	/* 高优队列使用相同 O(1) 拼接与源队列复位协议。 */
	if (&per_cpu(tasklet_hi_vec, cpu).head != per_cpu(tasklet_hi_vec, cpu).tail) {
		*__this_cpu_read(tasklet_hi_vec.tail) = per_cpu(tasklet_hi_vec, cpu).head;
		__this_cpu_write(tasklet_hi_vec.tail, per_cpu(tasklet_hi_vec, cpu).tail);
		per_cpu(tasklet_hi_vec, cpu).head = NULL;
		per_cpu(tasklet_hi_vec, cpu).tail = &per_cpu(tasklet_hi_vec, cpu).head;
	}
	raise_softirq_irqoff(HI_SOFTIRQ);

	/* 恢复当前 CPU 中断；两个 pending 将由退出/线程路径随后处理。 */
	local_irq_enable();
	return 0;
}
#else
/* 无 CPU 热拔插时 smpboot 不需要死亡回调，以 NULL 明确表示无接管动作。 */
#define takeover_tasklets	NULL
#endif /* CONFIG_HOTPLUG_CPU */
/* CPU 热拔插关闭时，takeover_tasklets 退化为空回调。 */

/*
 * softirq_threads 描述 ksoftirqd per-CPU 线程族的 smpboot 生命周期：
 * store 发布各 CPU task 指针；should_run 观察 pending；thread_fn 执行批次；
 * thread_comm 生成 ksoftirqd/<cpu> 名称。结构体静态存活，由 smpboot 借用，
 * 注册成功后框架负责创建、停放、唤醒和 CPU hotplug 协调。
 */
static struct smp_hotplug_thread softirq_threads = {
	.store			= &ksoftirqd,
	.thread_should_run	= ksoftirqd_should_run,
	.thread_fn		= run_ksoftirqd,
	.thread_comm		= "ksoftirqd/%u",
};

#ifdef CONFIG_IRQ_FORCED_THREADING
/*
 * ktimerd_setup() - 初始化当前 ktimers/%u 线程的调度策略。
 *
 * @cpu：线程所属 CPU 编号，本实现不直接使用。
 * 返回：无直接返回值；把 current 设为低优先级 SCHED_FIFO。由 smpboot 在线程
 * 启动阶段调用，允许调度器 helper 修改 current 策略。
 */
static void ktimerd_setup(unsigned int cpu)
{
	/* Above SCHED_NORMAL to handle timers before regular tasks. */
	/*
	 * 上游说明：低优先级 FIFO 仍高于 SCHED_NORMAL，使定时器唤醒先于普通
	 * 任务处理；“low”又避免与高优先级实时业务争夺更高 RT 优先级。
	 */
	sched_set_fifo_low(current);
}

/*
 * ktimerd_should_run() - 判断当前 CPU 是否有强制线程化 timer softirq。
 *
 * @cpu：绑定 CPU 编号，仅用于统一回调签名。
 * 返回：pending_timer_softirq 位图，零表示睡眠、非零表示线程应运行；无副作用。
 */
static int ktimerd_should_run(unsigned int cpu)
{
	return local_timers_pending_force_th();
}

/*
 * raise_ktimers_thread() - 向当前 CPU 的专用 timer 位图发布一个向量。
 *
 * @nr：timer 类 softirq 编号，调用者保证可放入 unsigned long 位图。
 * 入口：硬中断路径且 CPU 固定；该函数不主动唤醒，irq_exit 的 wake_timersd()
 *       统一完成发布后的线程唤醒。
 * 返回：无直接返回值；保留旧位并 OR 入 BIT(nr)，同时产生 softirq raise trace。
 */
void raise_ktimers_thread(unsigned int nr)
{
	trace_softirq_raise(nr);
	__this_cpu_or(pending_timer_softirq, BIT(nr));
}

/*
 * run_ktimerd() - 把专用 timer pending 转交普通分派器并在线程上下文执行。
 *
 * @cpu：线程绑定 CPU 编号，本实现使用当前 CPU per-CPU 状态。
 * 入口：ktimers/%u 任务上下文；begin 建立与 ksoftirqd 相同的 IRQ/BH 边界。
 * 过程：原子窗口内读取专用位图、清零源位图、OR 入普通 softirq pending，
 *       然后由 __do_softirq() 按统一向量表执行。
 * 返回：无直接返回值；已领取 timer 位完成或按普通预算留待后续，begin/end
 *       状态精确配对。该函数本身不返回错误。
 */
static void run_ktimerd(unsigned int cpu)
{
	/* timer_si 是本次从专用位图领取的向量集合。 */
	unsigned int timer_si;

	ksoftirqd_run_begin();

	/*
	 * begin 后本地中断关闭，防止硬中断在“读取 -> 清零 -> 转存”之间置位而
	 * 被清掉。先清专用源，再 OR 到普通 pending，执行责任完成一次性转移。
	 */
	timer_si = local_timers_pending_force_th();
	__this_cpu_write(pending_timer_softirq, 0);
	or_softirq_pending(timer_si);

	__do_softirq();

	/* 恢复 BH/IRQ 状态；__do_softirq() 返回时按约定本地中断关闭。 */
	ksoftirqd_run_end();
}

/*
 * timer_thread 描述 ktimers per-CPU 线程族：除存储、谓词和执行回调外，
 * setup 还配置低优先级 FIFO 策略。只有运行时 force_irqthreads() 为真才注册。
 */
static struct smp_hotplug_thread timer_thread = {
	.store			= &ktimerd,
	.setup			= ktimerd_setup,
	.thread_should_run	= ktimerd_should_run,
	.thread_fn		= run_ktimerd,
	.thread_comm		= "ktimers/%u",
};
#endif

/*
 * spawn_ksoftirqd() - 启动期注册 softirq 线程和 CPU 死亡接管回调。
 *
 * 入参：无。early_initcall 阶段调用一次，smpboot/cpuhp 基础设施已可用。
 * 过程：登记 CPUHP_SOFTIRQ_DEAD 状态 -> 注册 ksoftirqd 线程族 -> 若编译且
 *       运行时强制 IRQ 线程化，再注册 ktimers 线程族。
 * 返回：成功恒为 0。注册失败视为内核无法维持中断后半部执行，BUG_ON 立即
 *       停止而非向 initcall 返回可恢复错误；成功后线程生命周期归框架管理。
 */
static __init int spawn_ksoftirqd(void)
{
	/*
	 * nocalls 表示注册状态时不为已经 online 的 CPU 回放 startup 回调；
	 * 本状态只提供 teardown 的 takeover_tasklets，用于 CPU 死亡阶段。
	 */
	cpuhp_setup_state_nocalls(CPUHP_SOFTIRQ_DEAD, "softirq:dead", NULL,
				  takeover_tasklets);
	BUG_ON(smpboot_register_percpu_thread(&softirq_threads));
#ifdef CONFIG_IRQ_FORCED_THREADING
	/* 配置支持不等于运行时启用；仅 force_irqthreads() 时创建额外 timer 线程。 */
	if (force_irqthreads())
		BUG_ON(smpboot_register_percpu_thread(&timer_thread));
#endif
	return 0;
}
/*
 * early_initcall 把注册安排在常规设备初始化前，确保可能产生 softirq 的
 * 子系统全面启动前，各 CPU 已有可接管超预算工作的守护线程。
 */
early_initcall(spawn_ksoftirqd);

/*
 * [ These __weak aliases are kept in a separate compilation unit, so that
 *   GCC does not inline them incorrectly. ]
 */
/*
 * 上游注释：这些弱别名刻意保留在独立编译单元，避免 GCC 错误内联。体系结构
 * 可提供同名强定义覆盖它们；未覆盖时使用以下通用默认值。弱函数的地址/覆盖
 * 关系依赖链接阶段，因此不能把这里的函数体当作所有架构的最终实现。
 */

/*
 * early_irq_init() - 通用的早期 IRQ 描述符初始化弱钩子。
 *
 * 入参：无。体系结构未覆盖时不做任何工作并返回 0；强定义可返回负 errno。
 * __init 表示代码在启动完成后可回收，调用者不得保存函数地址供运行期使用。
 */
int __init __weak early_irq_init(void)
{
	return 0;
}

/*
 * arch_probe_nr_irqs() - 体系结构探测可用 IRQ 数量的弱默认实现。
 *
 * 入参：无。返回 NR_IRQS_LEGACY，表示仅承诺传统静态 IRQ 范围；体系结构强
 * 定义可根据控制器/固件返回更大范围。无副作用和 ownership。
 */
int __init __weak arch_probe_nr_irqs(void)
{
	return NR_IRQS_LEGACY;
}

/*
 * arch_early_irq_init() - 体系结构专用早期 IRQ 初始化的弱空实现。
 *
 * 入参：无。未覆盖时返回 0 且无副作用；体系结构强定义可执行初始化并返回
 * 错误。函数位于 __init 生命周期，只在启动期有效。
 */
int __init __weak arch_early_irq_init(void)
{
	return 0;
}

/*
 * arch_dynirq_lower_bound() - 让体系结构调整动态 IRQ 分配的最低编号。
 *
 * @from：通用层提出的起始 IRQ 编号，单位为 IRQ number。
 * 返回：弱默认原样返回 from；体系结构可提高下界以避开保留/传统 IRQ。
 * 函数不分配描述符、不修改全局状态，返回值只是后续搜索的策略边界。
 */
unsigned int __weak arch_dynirq_lower_bound(unsigned int from)
{
	return from;
}
