// SPDX-License-Identifier: GPL-2.0-only
/* Kernel thread helper functions.
 *   Copyright (C) 2004 IBM Corporation, Rusty Russell.
 *   Copyright (C) 2009 Red Hat, Inc.
 *
 * Creation is done via kthreadd, so that we get a clean environment
 * even if we're invoked from userspace (think modprobe, hotplug cpu,
 * etc.).
 */
/*
 * 本文件提供内核线程的完整生命周期基础设施：请求者把创建请求交给 kthreadd，
 * 新线程先以不可运行状态返回，调用者再选择绑定、唤醒、停驻或停止；后半部分
 * 在同一套生命周期之上实现串行 kthread_worker。由 kthreadd 代为 fork，可避免
 * 从 modprobe、CPU hotplug 等用户进程继承地址空间、信号处理和调度属性等环境。
 */
#include <uapi/linux/sched/types.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>
#include <linux/unistd.h>
#include <linux/file.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/freezer.h>
#include <linux/ptrace.h>
#include <linux/uaccess.h>
#include <linux/numa.h>
#include <linux/sched/isolation.h>
#include <trace/events/sched.h>


/*
 * 创建队列是多生产者、kthreadd 单消费者：spinlock 只保护链表结构，分配、clone 和
 * completion 等待均在锁外。kthreadd_task 是启动代码发布的长期 task 借用，用于唤醒
 * 消费者；本文件不额外持有/释放其引用。
 */
static DEFINE_SPINLOCK(kthread_create_lock);
static LIST_HEAD(kthread_create_list);
struct task_struct *kthreadd_task;

/*
 * affinity_list 收纳需随 housekeeping/CPU hotplug 重算的控制块；mutex 保护链表、
 * preferred_affinity 和退出摘链。相关路径会分配掩码或迁移 task，故选择可睡眠 mutex。
 */
static LIST_HEAD(kthread_affinity_list);
static DEFINE_MUTEX(kthread_affinity_lock);

/*
 * kthread_create_info 是一次跨线程创建事务：请求者分配并入队，kthreadd 摘取后 fork，
 * 新线程或失败的 kthreadd 填写结果。done 既是完成通知，也是双方用 xchg 争夺最终清理
 * 责任的令牌；成功交付或放弃路径都必须保证只有一方释放 create/full_name。
 */
struct kthread_create_info
{
	/* Information passed to kthread() from kthreadd. */
	/* 请求者写、消费侧读：full_name 成功时转移给 kthread，否则由清理责任方释放。 */
	char *full_name;
	/* threadfn 是线程入口，data 是借用实参；调用者保证二者覆盖线程使用期。 */
	int (*threadfn)(void *data);
	void *data;
	/* task/内核栈的 NUMA 分配偏好；NUMA_NO_NODE 表示不指定。 */
	int node;

	/* Result passed back to kthread_create() from kthreadd. */
	/* result 是返回槽；done 指向请求者栈上的 completion，也是双方争夺清理责任的令牌。 */
	struct task_struct *result;
	struct completion *done;

	/* 仅在 kthread_create_lock 下连接全局请求队列；摘链后由消费侧独占。 */
	struct list_head list;
};

/*
 * struct kthread 是每个 PF_KTHREAD task 的私有控制块，挂在 task->worker_private。
 * set_kthread_struct() 在 task 发布前创建，最终 task 释放路径清理；控制位用 bitops
 * 交换请求，亲和性字段由 kthread_affinity_lock 串行，completion 建立生命周期边界。
 */
struct kthread {
	/* flags 保存 KTHREAD_* 位；cpu 是 per-CPU 线程需恢复的目标编号。 */
	unsigned long flags;
	unsigned int cpu;
	/* 默认 NUMA 亲和偏好；NUMA_NO_NODE 表示只依据 housekeeping。 */
	unsigned int node;
	/* started 标记已越过首次创建睡眠；result 是交给 kthread_stop() 的结果。 */
	int started;
	int result;
	/* 创建时保存的回调契约；控制块借用 data，不管理其存储。 */
	int (*threadfn)(void *);
	void *data;
	/* parked 确认即将停驻；exited 确认 result 已稳定且线程退出。 */
	struct completion parked;
	struct completion exited;
#ifdef CONFIG_BLK_CGROUP
	/* 持有 css_get 引用，由 associate(NULL/新值) 释放或替换。 */
	struct cgroup_subsys_state *blkcg_css;
#endif
	/* To store the full name if task comm is truncated. */
	/* task->comm 被截断时保存完整名称；该字符串随本对象最终释放。 */
	char *full_name;
	/* 所属 task 的反向借用；控制块本身不额外持有 task 引用。 */
	struct task_struct *task;
	/* mutex 保护的全局链节点，以及控制块拥有的可空偏好掩码。 */
	struct list_head affinity_node;
	struct cpumask *preferred_affinity;
};

enum KTHREAD_BITS {
	KTHREAD_IS_PER_CPU = 0,
	KTHREAD_SHOULD_STOP,
	KTHREAD_SHOULD_PARK,
};

/* 位编号而非位掩码；由 bitops 原子访问，使控制者与目标线程可无锁交换请求。 */

/*
 * to_kthread() - 从已知内核线程取得其私有控制块。
 *
 * 调用位置：本文件所有需要访问 stop/park、回调数据、亲和性或 completion 的路径；
 * 它只是 task->worker_private 的受检包装，不稳定 task 生命周期或控制块内容。
 *
 * @k: 纯输入借用，不可为 NULL；调用者必须通过 task 引用、current 身份或其他协议保证
 * 可解引用。预期设置 PF_KTHREAD，违约只 WARN。任意上下文可调用，不取锁、不睡眠。
 * 返回控制块借用指针；set_kthread_struct() 分配失败的早期清理状态可返回 NULL。
 * 返回值不增加引用，也不保证字段不并发变化；调用者仍须遵守各字段的锁/bitops 协议。
 */

static inline struct kthread *to_kthread(struct task_struct *k)
{
	WARN_ON(!(k->flags & PF_KTHREAD));
	return k->worker_private;
}

/*
 * get_kthread_comm() - 复制内核线程的完整名称，必要时回退 task->comm。
 *
 * 调用位置：proc、trace 或诊断代码需要避开 TASK_COMM_LEN 截断时；若创建路径未保存
 * full_name，则与普通 get_task_comm 类似地读取 task->comm。
 *
 * @buf: 纯输出缓冲区，由调用者拥有且至少有 @buf_size 字节。@buf_size: 缓冲区容量，
 * 单位字节；0 时不写。@tsk: 纯输入借用的 kthread task，不可为 NULL，生命周期由
 * 调用者稳定。任意可执行普通内存复制的上下文可调用，不取锁、不睡眠。
 * 返回：无直接返回值。full_name 缺失时 strscpy() 写截断且 NUL 终止的 comm；存在时
 * strscpy_pad() 复制完整名称并填零。所有指针 ownership 均不改变。
 */

void get_kthread_comm(char *buf, size_t buf_size, struct task_struct *tsk)
{
	/* kthread 是 @tsk 控制块的调用期借用，仅用来选择 full_name/comm 分支。 */
	struct kthread *kthread = to_kthread(tsk);

	/* 快速路径：没有额外长名称时直接使用固定长度 comm，并由 strscpy 保证结尾。 */
	if (!kthread || !kthread->full_name) {
		strscpy(buf, tsk->comm, buf_size);
		return;
	}

	/* 慢速路径：复制创建时保存的完整名称，并把目标缓冲区未用部分填零。 */
	strscpy_pad(buf, kthread->full_name, buf_size);
}

/*
 * set_kthread_struct() - 为 kernel_clone 正在构造的 task 建立 kthread 控制块。
 *
 * 调用位置：kernel_clone() 创建 PF_KTHREAD task 的构造阶段；新 task 尚未运行，成功后
 * kthread()、stop/park 和最终 free_kthread_struct() 通过 worker_private 访问它。
 *
 * @p: 输入输出借用的新 task，不可为 NULL；入口要求尚未安装控制块。函数以 GFP_KERNEL
 * 分配并可睡眠，入口不持本文件锁。成功返回 true：completion、亲和性链节点、task/node
 * 已初始化，p->vfork_done 指向 exited，最后写 worker_private 作为发布点，控制块
 * ownership 转给 task。已有控制块或 -ENOMEM 返回 false；前者告警，后者无残留资源，
 * @p 的创建者仍负责终止/回滚 task。
 */

bool set_kthread_struct(struct task_struct *p)
{
	/* kthread 是本函数新建的拥有指针，发布到 worker_private 后 ownership 转给 @p。 */
	struct kthread *kthread;

	/* 阶段 1：拒绝重复安装；旧控制块一旦发布就不能被第二个对象覆盖。 */
	if (WARN_ON_ONCE(to_kthread(p)))
		return false;

	/* 阶段 2：分配并初始化所有会被 stop/park/亲和性路径并发观察的基础设施。 */
	kthread = kzalloc_obj(*kthread);
	if (!kthread)
		return false;

	init_completion(&kthread->exited);
	init_completion(&kthread->parked);
	INIT_LIST_HEAD(&kthread->affinity_node);
	p->vfork_done = &kthread->exited;

	/* 阶段 3：填写反向关系和 NUMA 偏好，最后发布 worker_private。 */
	kthread->task = p;
	kthread->node = tsk_fork_get_node(current);
	p->worker_private = kthread;
	return true;
}

/*
 * free_kthread_struct() - task 最终销毁时释放其 kthread 私有状态。
 *
 * 调用位置：task_struct 最终释放路径；此时 stop/park、亲和性遍历和数据查询均必须已
 * 结束，故无需本文件锁。与 set_kthread_struct() 构成 ownership 的最终回收端。
 *
 * @k: 输入输出借用的 PF_KTHREAD task，不可为 NULL，调用者保证独占最终清理阶段。
 * 不睡眠。返回：无直接返回值；控制块缺失是创建分配失败后的合法空清理。正常路径先
 * 清 worker_private 摘除可发现性，再释放拥有的 full_name 和控制块。CONFIG_BLK_CGROUP
 * 下 blkcg_css 非 NULL 只告警，表示退出前遗漏 associate(NULL)，不能在此静默掩盖引用错账。
 */

void free_kthread_struct(struct task_struct *k)
{
	/* kthread 是从 @k 摘取的最终清理借用，清除 worker_private 后由本函数释放。 */
	struct kthread *kthread;

	/*
	 * Can be NULL if kmalloc() in set_kthread_struct() failed.
	 */
	/* set_kthread_struct() 的分配失败路径尚未安装控制块，因此 NULL 是合法清理状态。 */
	kthread = to_kthread(k);
	if (!kthread)
		return;

	/* 阶段 1：最终回收前核对所有显式外部引用协议都已结束。 */
#ifdef CONFIG_BLK_CGROUP
	WARN_ON_ONCE(kthread->blkcg_css);
#endif
	/* 阶段 2：先摘除 task 上的可发现指针，再释放控制块拥有的动态内存。 */
	k->worker_private = NULL;
	kfree(kthread->full_name);
	kfree(kthread);
}

/**
 * kthread_should_stop - should this kthread return now?
 *
 * When someone calls kthread_stop() on your kthread, it will be woken
 * and this will return true.  You should then return, and your return
 * value will be passed through to kthread_stop().
 */
/*
 * kthread_should_stop() - 目标线程轮询控制者发布的停止请求。
 *
 * 调用位置：长期运行的 threadfn 循环或阻塞点；写侧是 kthread_stop() 的原子置位，
 * 随后 stop 会 unpark、设置通知位并唤醒 task。
 * 入参：无，只能由拥有有效控制块的 current kthread 调用。任意上下文、不取锁、不睡眠。
 * 返回 true 表示查询时 stop 位已发布，回调应尽快返回；false 表示尚无请求。返回值是
 * 并发快照，不清位。回调结果经 kthread_do_exit()/exited completion 传给 stop 调用者。
 * 无引用或 ownership 变化。
 */
bool kthread_should_stop(void)
{
	return test_bit(KTHREAD_SHOULD_STOP, &to_kthread(current)->flags);
}
EXPORT_SYMBOL(kthread_should_stop);

/*
 * __kthread_should_park() - 查询指定 kthread 的停驻请求位。
 * @k: 纯输入借用且不可为 NULL；调用者保证 task/控制块生命周期。由 current 包装和
 * 控制协议内部使用；不取锁、不睡眠。返回 park 位的并发快照，不清位、无副作用，
 * ownership 不变。写侧为 kthread_park()/kthread_unpark() 的 bitops。
 */
static bool __kthread_should_park(struct task_struct *k)
{
	return test_bit(KTHREAD_SHOULD_PARK, &to_kthread(k)->flags);
}

/**
 * kthread_should_park - should this kthread park now?
 *
 * When someone calls kthread_park() on your kthread, it will be woken
 * and this will return true.  You should then do the necessary
 * cleanup and call kthread_parkme()
 *
 * Similar to kthread_should_stop(), but this keeps the thread alive
 * and in a park position. kthread_unpark() "restarts" the thread and
 * calls the thread function again.
 */
/*
 * kthread_should_park() - 当前 kthread 是否应清理临时状态并进入 kthread_parkme()。
 *
 * 调用位置：支持 CPU hotplug/暂停协议的 threadfn 循环；写侧 kthread_park() 置位并
 * 唤醒，回调看到 true 后先清理不可跨停驻保存的资源，再调用 kthread_parkme()。
 * 入参：无，只能由 current kthread 调用；不取锁、不睡眠。返回 park 位快照，不表示
 * 已经进入 TASK_PARKED。与 stop 不同，park 保留 task 和回调生命周期，unpark 后继续。
 * 无引用、字段清理或 ownership 变化。
 */
bool kthread_should_park(void)
{
	return __kthread_should_park(current);
}
EXPORT_SYMBOL_GPL(kthread_should_park);

/*
 * kthread_should_stop_or_park() - 安全查询 current 是否收到停止或停驻请求。
 *
 * 调用位置：可由不确定 current 类型的通用循环作为退出/让出条件；内部先通过
 * tsk_is_kthread() 验证 PF_KTHREAD 及私有控制块，再读取 stop/park 位。
 *
 * 入参：无。运行于调用者当前上下文，不取锁、不睡眠；bitops 写侧可能来自其他 CPU，
 * 返回值只是查询时快照。返回 true 表示 current 是 kthread 且至少一个请求已发布；
 * 普通 task、控制块不可用或无请求均返回 false。无引用和 ownership 变化。
 * 调用者随后通常根据具体协议返回 threadfn，或进入 kthread_parkme()。
 */
bool kthread_should_stop_or_park(void)
{
	/* kthread 是 current 控制块的瞬时借用；NULL 兼容普通 task。 */
	struct kthread *kthread = tsk_is_kthread(current);

	if (!kthread)
		return false;

	return kthread->flags & (BIT(KTHREAD_SHOULD_STOP) | BIT(KTHREAD_SHOULD_PARK));
}

/**
 * kthread_freezable_should_stop - should this freezable kthread return now?
 * @was_frozen: optional out parameter, indicates whether %current was frozen
 *
 * kthread_should_stop() for freezable kthreads, which will enter
 * refrigerator if necessary.  This function is safe from kthread_stop() /
 * freezer deadlock and freezable kthreads should use this function instead
 * of calling try_to_freeze() directly.
 */
/*
 * kthread_freezable_should_stop() - 在安全冻结点处理 freezer 后再报告 stop。
 *
 * 调用位置：设置 freezable 的 kthread 循环，替代直接 try_to_freeze() + should_stop；
 * 组合顺序避免 freezer 等线程冻结、kthread_stop() 又等线程退出的死锁。
 * @was_frozen: 可空纯输出 bool；非 NULL 时入口值无要求，返回时表示本次是否真正进入
 * refrigerator，不转移存储 ownership。只能由 current kthread 在可睡眠上下文调用，
 * might_sleep() 明确这一契约。返回 true/false 为 stop 位快照；即使经历冻结也不代表
 * stop，一定在 thaw 后重新查询。除输出参数外无持久副作用。
 */
bool kthread_freezable_should_stop(bool *was_frozen)
{
	/* frozen 只记录本次调用是否真正经过 refrigerator，并按需复制到输出参数。 */
	bool frozen = false;

	might_sleep();

	/* 阶段 1：只在 freezer 已发请求时进入 refrigerator，避免无条件调度开销。 */
	if (unlikely(freezing(current)))
		frozen = __refrigerator(true);

	/* 阶段 2：先向调用者报告本次冻结事实，再读取可能在冻结期间到达的 stop 位。 */
	if (was_frozen)
		*was_frozen = frozen;

	return kthread_should_stop();
}
EXPORT_SYMBOL_GPL(kthread_freezable_should_stop);

/**
 * kthread_func - return the function specified on kthread creation
 * @task: kthread task in question
 *
 * Returns NULL if the task is not a kthread.
 */
/*
 * kthread_func() - 容错取得创建 kthread 时保存的回调函数。
 * @task: 纯输入借用，可为普通 task 但不可为 NULL；调用者保证 task 生命周期。
 * 诊断/识别路径使用，不取锁、不睡眠。返回创建时 threadfn 的借用函数指针；非 kthread、
 * 控制块缺失返回 NULL。返回不固定模块代码生命周期，调用者若要调用仍须保证提供者有效。
 * 无字段修改和 ownership 变化。
 */
void *kthread_func(struct task_struct *task)
{
	/* kthread 是容错识别得到的控制块借用，可能为 NULL。 */
	struct kthread *kthread = tsk_is_kthread(task);

	/* 简单探测函数仅有“识别后返回/失败返回 NULL”两个分支，无额外资源阶段。 */
	if (kthread)
		return kthread->threadfn;
	return NULL;
}
EXPORT_SYMBOL_GPL(kthread_func);

/**
 * kthread_data - return data value specified on kthread creation
 * @task: kthread task in question
 *
 * Return the data value specified when kthread @task was created.
 * The caller is responsible for ensuring the validity of @task when
 * calling this function.
 */
/*
 * kthread_data() - 取得创建时传入的私有数据借用指针。
 * @task: 纯输入借用且必须是带有效控制块的 kthread，不可为 NULL；调用者保证 task
 * 生命周期及与 threadfn/data 更新的同步。任意上下文，不取锁、不睡眠。
 * 返回创建时 @data 的原始借用值，可为 NULL；不增加引用、不验证目标内存，也不延长
 * data 生命周期。与容错 probe 版本不同，错误 task 会触发 to_kthread() 告警/无效访问。
 */
void *kthread_data(struct task_struct *task)
{
	return to_kthread(task)->data;
}
EXPORT_SYMBOL_GPL(kthread_data);

/**
 * kthread_probe_data - speculative version of kthread_data()
 * @task: possible kthread task in question
 *
 * @task could be a kthread task.  Return the data value specified when it
 * was created if accessible.  If @task isn't a kthread task or its data is
 * inaccessible for any reason, %NULL is returned.  This function requires
 * that @task itself is safe to dereference.
 */
/*
 * kthread_probe_data() - 在 task 已可安全解引用的前提下容错探测 data。
 * @task: 纯输入借用，可为普通 task 但不可为 NULL；调用者必须先保证 task 自身安全
 * 解引用，本函数不取得 task 引用。用于调试/探测可能正在变化的 kthread 控制块。
 * 不取锁、不睡眠；copy_from_kernel_nofault 只避免读取 kthread->data 时产生 fault。
 * 成功返回创建 data 的借用快照；非 kthread、控制块不可访问或读取失败返回 NULL。
 * NULL 也可能是合法 data，故不能区分“不存在”和“值为 NULL”。不延长任何生命周期。
 */
void *kthread_probe_data(struct task_struct *task)
{
	/* kthread 是可空控制块借用；data 是 nofault 读取的统一返回槽。 */
	struct kthread *kthread = tsk_is_kthread(task);
	void *data = NULL;

	/* 先过滤普通 task；nofault 读取失败时保留初始化的 NULL，统一形成容错返回。 */
	if (kthread)
		copy_from_kernel_nofault(&data, &kthread->data, sizeof(data));
	return data;
}

/*
 * __kthread_parkme() - 由目标线程完成 park/unpark 的睡眠握手。
 *
 * 调用位置：kthread() 首次运行 threadfn 前，以及 threadfn 响应
 * kthread_should_park() 后；控制侧由 kthread_park()/kthread_unpark() 配对。
 *
 * @self: current 的私有控制块，纯输入借用，必须覆盖整个停驻期；只能代表 current。
 * 入口不持锁，可睡眠。循环先以特殊状态序列化并发唤醒，再检查 SHOULD_PARK；仍需
 * 停驻时完成 parked completion 并主动调度。返回：无直接返回值；仅在控制侧清位后
 * 返回，并保证 current 已恢复 TASK_RUNNING。函数不取得或转移任何引用。
 */
static void __kthread_parkme(struct kthread *self)
{
	for (;;) {
		/*
		 * TASK_PARKED is a special state; we must serialize against
		 * possible pending wakeups to avoid store-store collisions on
		 * task->state.
		 *
		 * Such a collision might possibly result in the task state
		 * changin from TASK_PARKED and us failing the
		 * wait_task_inactive() in kthread_park().
		 */
		/*
		 * TASK_PARKED 必须用 set_special_state() 与并发 wakeup 串行化；普通状态写可能
		 * 与唤醒侧同时覆盖 task->__state，使 kthread_park() 无法观察稳定停驻状态。
		 */
		set_special_state(TASK_PARKED);
		if (!test_bit(KTHREAD_SHOULD_PARK, &self->flags))
			break;

		/*
		 * Thread is going to call schedule(), do not preempt it,
		 * or the caller of kthread_park() may spend more time in
		 * wait_task_inactive().
		 */
		/*
		 * 在禁止抢占窗口中先 complete 再主动 schedule，保证等待者获知“马上睡眠”后，
		 * 当前线程不会被抢占并长期停在 complete 与 schedule 之间。
		 */
		preempt_disable();
		complete(&self->parked);
		schedule_preempt_disabled();
		preempt_enable();
	}
	__set_current_state(TASK_RUNNING);
}

/*
 * kthread_parkme() - 让 current 按已发布的 park 请求进入可恢复停驻。
 *
 * 调用位置：kthread 回调在 kthread_should_park() 为 true 并完成自身资源清理后调用；
 * 本包装把 current 的私有控制块交给 __kthread_parkme()。
 *
 * 入参：无。只能由带有效控制块的 current kthread 调用；入口不持本文件锁，可能睡眠。
 * 返回：无直接返回值；kthread_unpark() 清位并唤醒后返回，线程仍存活且可继续工作。
 * 不改变引用 ownership，停驻期间由 task 生命周期的外部协议保证对象有效。
 */
void kthread_parkme(void)
{
	__kthread_parkme(to_kthread(current));
}

EXPORT_SYMBOL_GPL(kthread_parkme);

/*
 * kthread_do_exit() - 保存线程结果并撤销退出线程的动态亲和性登记。
 *
 * 调用位置：do_exit() 识别 PF_KTHREAD 后最先调用；本函数返回后，do_exit() 的
 * exit_mm() → exit_mm_release() → mm_release() 会通过 task->vfork_done 完成 exited，
 * kthread_stop() 随后才能读取稳定的 result。控制块本身直到 free_task() 才最终释放。
 *
 * @kthread: 当前线程私有控制块，输入输出借用；函数写 result、affinity_node 和
 * preferred_affinity，不取得长期引用。@result: do_exit() 传入的 long；赋给控制块
 * 的 int result 后由 stop 返回，常规 threadfn int/-errno 值保持不变；API 不应传入
 * 超出 int 表示范围的结果，否则会发生实现定义的有符号整数转换。入口不持 affinity
 * mutex，可能睡眠。
 * 返回：无直接返回值。若已登记，成功出口保证链表摘除且偏好掩码释放；未登记时只
 * 保存结果。mutex 与 CPU hotplug/cpuset 遍历串行，防止遍历者访问已退出控制块。
 */
void kthread_do_exit(struct kthread *kthread, long result)
{
	/* 阶段 1：先发布最终结果；exited completion 稍后使 stop 侧安全读取。 */
	kthread->result = result;
	if (!list_empty(&kthread->affinity_node)) {
		/* 阶段 2：与 hotplug/cpuset 遍历串行摘链，阻止新的亲和性访问。 */
		mutex_lock(&kthread_affinity_lock);
		list_del(&kthread->affinity_node);
		mutex_unlock(&kthread_affinity_lock);

		/* 摘链后不再有遍历者借用偏好掩码，可以释放控制块拥有的副本。 */
		if (kthread->preferred_affinity) {
			kfree(kthread->preferred_affinity);
			kthread->preferred_affinity = NULL;
		}
	}
}

/**
 * kthread_complete_and_exit - Exit the current kthread.
 * @comp: Completion to complete
 * @code: The integer value to return to kthread_stop().
 *
 * If present, complete @comp and then return code to kthread_stop().
 *
 * A kernel thread whose module may be removed after the completion of
 * @comp can use this function to exit safely.
 *
 * Does not return.
 */
/*
 * kthread_complete_and_exit() - 可选通知外部后以 @code 永久退出当前 kthread。
 *
 * 调用位置：模块 kthread 完成最后一项工作，需要先通知卸载者再退出；complete 后模块
 * 可能立即卸载，因此此函数必须是调用模块代码的最后一步。
 * @comp: 可空输入借用 completion；非 NULL 时只发完成事件，不取得 ownership。
 * @code: 交给 kthread_exit() 的 long；退出侧存入 int result，故常规 int/-errno 值由
 * kthread_stop() 保持返回；API 不应传入超出 int 表示范围的值。
 * current 必须是 kthread；complete 不睡眠，但退出路径可能调度。函数 __noreturn：通知
 * 后立即退出，绝不再访问调用模块的数据或指令。NULL comp 时仅执行退出。
 */
void __noreturn kthread_complete_and_exit(struct completion *comp, long code)
{
	/* 可选发布完成事件后立即进入不可返回的退出路径，中间不得再调用模块代码。 */
	if (comp)
		complete(comp);

	kthread_exit(code);
}
EXPORT_SYMBOL(kthread_complete_and_exit);

/*
 * kthread_fetch_affinity() - 计算动态 kthread 当前可用的 CPU 掩码。
 *
 * 调用位置：首次建立节点亲和性、显式安装 preferred mask，以及 CPU/cpuset 变化后的
 * 全局重算路径。调用者持 kthread_affinity_lock，函数内部以 RCU guard 读取当前
 * housekeeping 掩码。
 *
 * @kthread: 纯输入借用；preferred_affinity/node 在 mutex 下稳定。@cpumask: 调用者
 * 分配的输出缓冲，入口内容无要求，返回时被完全改写；ownership 不变。函数不睡眠。
 * 返回：无直接返回值。优先取显式偏好，否则取 NUMA 节点或全部 housekeeper，再与
 * HK_TYPE_DOMAIN 求交；交集为空时保证回退到 housekeeper，避免产生空的允许掩码。
 */
static void kthread_fetch_affinity(struct kthread *kthread, struct cpumask *cpumask)
{
	/* pref 是 RCU 读侧临界区内选出的显式、节点或 housekeeping 掩码借用。 */
	const struct cpumask *pref;

	/* 阶段 1：RCU 读侧稳定 housekeeping 掩码指针，再选择显式或节点默认偏好。 */
	guard(rcu)();

	if (kthread->preferred_affinity) {
		pref = kthread->preferred_affinity;
	} else {
		if (kthread->node == NUMA_NO_NODE)
			pref = housekeeping_cpumask(HK_TYPE_DOMAIN);
		else
			pref = cpumask_of_node(kthread->node);
	}

	/* 阶段 2：仅允许 domain housekeeper；空交集回退保证输出至少是非空允许集合。 */
	cpumask_and(cpumask, pref, housekeeping_cpumask(HK_TYPE_DOMAIN));
	if (cpumask_empty(cpumask))
		cpumask_copy(cpumask, housekeeping_cpumask(HK_TYPE_DOMAIN));
}

/*
 * kthread_affine_node() - 将 current 纳入 NUMA/housekeeping 动态亲和性管理。
 *
 * 调用位置：kthreadd 初始化，以及普通 kthread 首次被唤醒且尚未硬绑定/显式偏好时。
 * 后续 CPU online 或 housekeeping 改变由 kthreads_update_affinity() 重算。
 *
 * 入参：无。current 必须是非 per-CPU kthread；入口不持 affinity mutex。函数以
 * GFP_KERNEL 分配临时 cpumask、取得 mutex 并调用调度器修改亲和性，因此可能睡眠。
 * 返回：无直接返回值；分配失败或错误类型只告警并保留原亲和性。完成后 affinity_node
 * 被发布到全局链表，并尝试把有效掩码设为偏好与 housekeeper 的非空交集；调度器设置
 * 的返回值未被本函数上报，CPU 竞态失败依赖调度器 fallback/后续 hotplug 重算收敛。
 */
static void kthread_affine_node(void)
{
	/* kthread 借用 current 控制块；affinity 是本次计算拥有的临时 cpumask。 */
	struct kthread *kthread = to_kthread(current);
	cpumask_var_t affinity;

	/* 阶段 1：per-CPU 线程走硬绑定协议，不能混入会被全局重算的动态链表。 */
	if (WARN_ON_ONCE(kthread_is_per_cpu(current)))
		return;

	/* 临时掩码仅服务本次计算，分配失败保持旧亲和性且不发布链表节点。 */
	if (!zalloc_cpumask_var(&affinity, GFP_KERNEL)) {
		WARN_ON_ONCE(1);
		return;
	}

	/* 阶段 2：在同一 mutex 临界区内先发布成员资格，再计算并尝试安装亲和性。 */
	mutex_lock(&kthread_affinity_lock);
	WARN_ON_ONCE(!list_empty(&kthread->affinity_node));
	list_add_tail(&kthread->affinity_node, &kthread_affinity_list);
	/*
	 * The node cpumask is racy when read from kthread() but:
	 * - a racing CPU going down will either fail on the subsequent
	 *   call to set_cpus_allowed_ptr() or be migrated to housekeepers
	 *   afterwards by the scheduler.
	 * - a racing CPU going up will be handled by kthreads_online_cpu()
	 */
	/*
	 * node 掩码与 CPU 上下线并发读取是刻意允许的：下线由调度器回退，上线由
	 * hotplug 回调重算。因此这里要求的是最终收敛，而非一次快照绝对稳定。
	 */
	kthread_fetch_affinity(kthread, affinity);
	set_cpus_allowed_ptr(current, affinity);
	mutex_unlock(&kthread_affinity_lock);

	/* 阶段 3：实际掩码已复制进调度器状态，临时缓冲可在解锁后释放。 */
	free_cpumask_var(affinity);
}

/*
 * kthread() - 新内核线程的首入口，完成创建握手后运行 threadfn 并退出。
 *
 * 调用链：kthreadd → kernel_thread() → kthread() → __kthread_parkme() → threadfn() →
 * kthread_exit()。创建者在 __kthread_create_on_node() 等待 done，并在首次唤醒前绑定。
 *
 * @_create: kthread_create_info 输入输出对象；入口时由创建协议共享，done 的 xchg 决定
 * 本线程是继续交付结果还是接管 create/full_name 的释放。threadfn/data 被复制到局部
 * 与 self，full_name 成功时 ownership 转移到 self。运行于新 kthread 进程上下文，
 * 不持 create spinlock，允许睡眠。
 *
 * 返回：源码类型为 int，但所有路径都经 kthread_exit()，不会返回调用者。创建者已放弃
 * 时以 -EINTR 退出；正常首次唤醒后执行回调并传递其结果；stop 在首次唤醒前到达则不
 * 调 threadfn，结果为 -EINTR。create->result 后的 complete(done) 是 task 交付边界。
 */
static int kthread(void *_create)
{
	static const struct sched_param param = { .sched_priority = 0 };
	/*
	 * 变量地图：
	 * create   跨线程请求信封；done 仲裁前由请求者和本线程共享，仲裁后按结果决定归属。
	 * threadfn/data  在通知创建者前复制出的回调契约，避免之后继续依赖 create。
	 * done     请求者栈上的完成量借用；非 NULL 表示请求者仍等待且栈帧有效。
	 * self     current 私有控制块借用，保存完整名称、回调契约和最终结果。
	 * param    不可变的 SCHED_NORMAL/零优先级参数，只在恢复默认调度属性时借用。
	 * ret      退出结果；默认 -EINTR，真正运行 threadfn 后改为其返回值。
	 */
	/* Copy data: it's on kthread's stack */
	/* create 属于跨线程请求；先复制 threadfn/data，避免通知请求者后再访问其生命周期。 */
	struct kthread_create_info *create = _create;
	int (*threadfn)(void *data) = create->threadfn;
	void *data = create->data;
	struct completion *done;
	struct kthread *self;
	int ret;

	self = to_kthread(current);

	/* Release the structure when caller killed by a fatal signal. */
	/* xchg 是 ownership 仲裁点：NULL 表示请求者已放弃等待并把 create 清理权转给本线程。 */
	done = xchg(&create->done, NULL);
	if (!done) {
		kfree(create->full_name);
		kfree(create);
		kthread_exit(-EINTR);
	}

	self->full_name = create->full_name;
	self->threadfn = threadfn;
	self->data = data;

	/*
	 * The new thread inherited kthreadd's priority and CPU mask. Reset
	 * back to default in case they have been changed.
	 */
	/* kthreadd 环境本身可能被管理代码修改；新线程对外可见前恢复普通调度策略。 */
	sched_setscheduler_nocheck(current, SCHED_NORMAL, &param);

	/* OK, tell user we're spawned, wait for stop or wakeup */
	/* 发布 current 到 result 后 complete；此时 task 已构造但保持不可中断睡眠，尚未执行回调。 */
	__set_current_state(TASK_UNINTERRUPTIBLE);
	create->result = current;
	/*
	 * Thread is going to call schedule(), do not preempt it,
	 * or the creator may spend more time in wait_task_inactive().
	 */
	/* 禁止抢占缩短“创建者已被通知、线程尚未 schedule 出去”的不稳定窗口。 */
	preempt_disable();
	complete(done);
	schedule_preempt_disabled();
	preempt_enable();

	self->started = 1;

	/*
	 * Apply default node affinity if no call to kthread_bind[_mask]() nor
	 * kthread_affine_preferred() was issued before the first wake-up.
	 */
	/* 首次唤醒前若调用者未硬绑定或提供偏好，才安装 NUMA/housekeeping 默认策略。 */
	if (!(current->flags & PF_NO_SETAFFINITY) && !self->preferred_affinity)
		kthread_affine_node();

	/* 阶段 4：stop 若抢在首次启动前到达，则跳过回调并保留约定的 -EINTR。 */
	ret = -EINTR;
	if (!test_bit(KTHREAD_SHOULD_STOP, &self->flags)) {
		/* 对外声明 cgroup 已就绪，完成可能的初始 park 握手后才进入用户回调。 */
		cgroup_kthread_ready();
		__kthread_parkme(self);
		ret = threadfn(data);
	}
	/* 不可回滚边界：结果交给退出路径；本函数此后不返回，也不再访问 create。 */
	kthread_exit(ret);
}

/* called from kernel_clone() to get node information for about to be created task */
/* kernel_clone() 在分配新 task/栈前查询 NUMA 节点；仅 kthreadd 子线程继承本次请求偏好。 */
/*
 * tsk_fork_get_node() - 返回即将创建 task 的 NUMA 分配节点。
 *
 * 调用位置：kernel_clone() 在分配 task/内核栈前查询；create_kthread() 已把本次请求
 * node 临时写入 kthreadd->pref_node_fork。
 * @tsk: 纯输入借用的父 task，不可为 NULL；调用者保证生命周期。任意上下文，不取锁、
 * 不睡眠。CONFIG_NUMA 且父正是 kthreadd_task 时返回本次偏好节点；其他父 task 或
 * 关闭 NUMA 均返回 NUMA_NO_NODE。只返回整数快照，无字段/ownership 副作用。
 */
int tsk_fork_get_node(struct task_struct *tsk)
{
#ifdef CONFIG_NUMA
	/* 配置分支：只有 kthreadd 正在代办的 fork 才读取其临时请求节点。 */
	if (tsk == kthreadd_task)
		return tsk->pref_node_fork;
#endif
	/* 普通 fork 或非 NUMA 构建均明确表示“不指定分配节点”。 */
	return NUMA_NO_NODE;
}

/*
 * create_kthread() - 由 kthreadd 把一个已摘队请求转化为新 task。
 *
 * 调用位置：kthreadd() 在释放 kthread_create_lock 后调用；成功时新 task 从
 * kthread() 接管后续交付，失败时本函数直接通知创建者。
 *
 * @create: 输入输出请求，入口由消费侧独占但其 done 指向等待者栈；成功 clone 后
 * ownership 交给新线程。失败时 xchg(done, NULL) 决定由本函数释放整个请求，还是
 * 保留 create 给仍等待的创建者并只转移 full_name 的释放责任。可能睡眠。
 * 返回：无直接返回值。成功结果由新线程完成；失败向仍等待者发布 ERR_PTR(pid)，或在
 * 等待者已退出时静默完成全部清理。CONFIG_NUMA 下 pref_node_fork 仅影响本次 fork。
 */
static void create_kthread(struct kthread_create_info *create)
{
	/* pid 是 kernel_thread() 的结果槽：非负为已移交请求的新 task pid，负值为 errno。 */
	int pid;

#ifdef CONFIG_NUMA
	/* 阶段 1：仅在本次 clone 周围暂存节点偏好；下一请求会覆盖该字段。 */
	current->pref_node_fork = create->node;
#endif
	/* We want our own signal handler (we take no signals by default). */
	/* 不共享 signal handler，使新 kthread 保持默认不接收信号；FS/files 则继承干净 kthreadd 环境。 */
	pid = kernel_thread(kthread, create, create->full_name,
			    CLONE_FS | CLONE_FILES | SIGCHLD);
	/* 成功时 create 已转交新线程，kthreadd 不再访问；仅 clone 失败由本函数收尾。 */
	if (pid < 0) {
		/* Release the structure when caller killed by a fatal signal. */
		/* clone 失败也用同一 done 令牌决定由 kthreadd 释放，还是向仍等待的请求者回报错误。 */
		struct completion *done = xchg(&create->done, NULL);

		kfree(create->full_name);
		if (!done) {
			kfree(create);
			return;
		}
		/* 等待者仍在：把 errno 写入结果槽，complete 后由等待者释放请求信封。 */
		create->result = ERR_PTR(pid);
		complete(done);
	}
}

/*
 * __kthread_create_on_node() - 分配创建请求、交给 kthreadd 并同步取得未启动 task。
 *
 * 调用位置：所有 kthread/worker 创建包装最终汇聚于此；返回成功 task 后，调用者可先
 * 设置硬绑定或软偏好，再用 wake_up_process() 启动。
 *
 * @threadfn: 新线程回调，纯输入函数指针，不可为 NULL；其代码须覆盖线程生命期。
 * @data: 交给回调的借用指针，可空，调用者保证使用期。@node: task/栈的 NUMA 分配
 * 节点，或 NUMA_NO_NODE。@namefmt: printf 格式字符串借用。@args: 输入 va_list 借用，
 * 仅在格式化名称期间消费。函数在进程上下文运行，可分配、唤醒和 killable 睡眠。
 *
 * 成功返回已构造、TASK_UNINTERRUPTIBLE 且尚未运行 threadfn 的 task 借用指针；
 * 请求/名称分配失败返回 ERR_PTR(-ENOMEM)，致命信号抢在完成前到达返回 -EINTR。
 * done 的 xchg 是 create 清理 ownership 的仲裁点，任何出口都不会遗留双方同时释放。
 */
static __printf(4, 0)
struct task_struct *__kthread_create_on_node(int (*threadfn)(void *data),
						    void *data, int node,
						    const char namefmt[],
						    va_list args)
{
	/*
	 * 变量地图：
	 * done    当前栈上的创建完成量；函数返回前必须确认执行侧不再访问它。
	 * create 请求信封的拥有指针；正常由本函数最终释放，致命信号路径可转移给执行侧。
	 * task   统一返回槽，保存成功 task 或 -ENOMEM 错误指针；其他错误由 create->result 来。
	 */
	DECLARE_COMPLETION_ONSTACK(done);
	struct task_struct *task;
	struct kthread_create_info *create = kmalloc_obj(*create);

	/* 阶段 1：请求信封和完整名称都由本调用先拥有，失败统一在 free_create 回收。 */
	if (!create)
		return ERR_PTR(-ENOMEM);
	create->threadfn = threadfn;
	create->data = data;
	create->node = node;
	create->done = &done;
	create->full_name = kvasprintf(GFP_KERNEL, namefmt, args);
	if (!create->full_name) {
		task = ERR_PTR(-ENOMEM);
		goto free_create;
	}

	/* 阶段 2：在短自旋锁临界区发布请求，随后唤醒唯一消费者 kthreadd。 */
	spin_lock(&kthread_create_lock);
	list_add_tail(&create->list, &kthread_create_list);
	spin_unlock(&kthread_create_lock);

	wake_up_process(kthreadd_task);
	/* 阶段 3：killable 等待创建结果；致命信号路径以 xchg 仲裁请求与栈上 done 的归属。 */
	/*
	 * Wait for completion in killable state, for I might be chosen by
	 * the OOM killer while kthreadd is trying to allocate memory for
	 * new kernel thread.
	 */
	/* killable 等待允许 OOM victim 退出，而不会无限占着 kthreadd 正在申请的内存。 */
	if (unlikely(wait_for_completion_killable(&done))) {
		/*
		 * If I was killed by a fatal signal before kthreadd (or new
		 * kernel thread) calls complete(), leave the cleanup of this
		 * structure to that thread.
		 */
		/* 成功把 done 置 NULL 即转移 create/full_name 清理责任，绝不能再解引用 create。 */
		if (xchg(&create->done, NULL))
			return ERR_PTR(-EINTR);
		/*
		 * kthreadd (or new kernel thread) will call complete()
		 * shortly.
		 */
		/* 若执行端已取走令牌，它即将 complete 栈上对象；必须等完才能离开当前栈帧。 */
		wait_for_completion(&done);
	}
	/* 阶段 4：completion 保证 result 已发布；读取后回收仍由请求者拥有的信封。 */
	task = create->result;
free_create:
	/* 此标签只处理请求者仍拥有 create 的路径；full_name 的归属见各失败/成功分支。 */
	kfree(create);
	return task;
}

/**
 * kthread_create_on_node - create a kthread.
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @node: task and thread structures for the thread are allocated on this node
 * @namefmt: printf-style name for the thread.
 *
 * Description: This helper function creates and names a kernel
 * thread.  The thread will be stopped: use wake_up_process() to start
 * it.  See also kthread_run().  The new thread has SCHED_NORMAL policy and
 * is affine to all CPUs.
 *
 * If thread is going to be bound on a particular cpu, give its node
 * in @node, to get NUMA affinity for kthread stack, or else give NUMA_NO_NODE.
 * When woken, the thread will run @threadfn() with @data as its
 * argument. @threadfn() can either return directly if it is a
 * standalone thread for which no one will call kthread_stop(), or
 * return when 'kthread_should_stop()' is true (which means
 * kthread_stop() has been called).  The return value should be zero
 * or a negative error number; it will be passed to kthread_stop().
 *
 * Returns a task_struct or ERR_PTR(-ENOMEM) or ERR_PTR(-EINTR).
 */
/*
 * kthread_create_on_node() - 创建命名 kthread，并以指定 NUMA 节点分配 task/栈。
 * @threadfn: 纯输入回调，不可为 NULL，代码覆盖线程生命期。@data: 传给回调的借用
 * 指针，可为 NULL，存储覆盖回调使用期。@node: NUMA 节点或 NUMA_NO_NODE。
 * @namefmt: printf 格式字符串借用；尾随参数仅在本次格式化期间读取。
 * 可睡眠进程上下文；va_start/va_end 把参数交给核心请求协议。成功返回新 task 指针：
 * threadfn 尚未运行，task 处于 TASK_UNINTERRUPTIBLE，调用者可先 bind/设置偏好再唤醒。
 * 请求或名称分配失败返回 ERR_PTR(-ENOMEM)，致命信号中断等待返回 ERR_PTR(-EINTR)。
 * 回调返回值应为 0/负 errno，stop 时透传；task 生命周期遵循 task 引用协议。
 * 修正说明：英文“affine to all CPUs”只描述创建完成时继承状态；当前 kthread() 在首次
 * 唤醒后、回调执行前会对未硬绑定/未设偏好的线程调用 kthread_affine_node()，最终默认
 * 受 NUMA 偏好和 HK_TYPE_DOMAIN housekeeping 掩码约束，并非始终允许所有 CPU。
 */
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data, int node,
					   const char namefmt[],
					   ...)
{
	/* task 保存核心创建结果；args 只在当前包装调用期间枚举尾随格式参数。 */
	struct task_struct *task;
	va_list args;

	/* 单阶段包装：建立 va_list 生命周期，核心函数返回前已完成全部异步握手。 */
	va_start(args, namefmt);
	task = __kthread_create_on_node(threadfn, data, node, namefmt, args);
	va_end(args);

	return task;
}
EXPORT_SYMBOL(kthread_create_on_node);

/*
 * __kthread_bind_mask() - 在目标稳定 inactive 时强制安装 CPU 掩码。
 *
 * 调用位置：首次唤醒前的 bind API，以及 per-CPU 线程从 TASK_PARKED 恢复绑定时。
 *
 * @p: 输入输出借用 task；调用者保证其为活 kthread。@mask: 纯输入借用 CPU 集合，
 * 必须含 possible CPU，可暂时没有 online CPU。@state: wait_task_inactive() 期望的
 * TASK_UNINTERRUPTIBLE 或 TASK_PARKED。入口不持 p->pi_lock；等待目标 inactive 可能
 * 调度，随后 scoped_guard 自动 irqsave 加锁并强制写亲和性。
 * 返回：无直接返回值；inactive 检查失败只告警且不承诺绑定。成功后 mask 生效并设置
 * PF_NO_SETAFFINITY，阻止普通亲和性路径破坏专用绑定；参数 ownership 均不改变。
 */
static void __kthread_bind_mask(struct task_struct *p, const struct cpumask *mask, unsigned int state)
{
	/* 阶段 1：先确认目标已按调用协议调度出 CPU，失败时保持原掩码和标志。 */
	if (!wait_task_inactive(p, state)) {
		WARN_ON(1);
		return;
	}

	/* 阶段 2：pi_lock 下强制写入允许掩码；scoped_guard 离开语句后自动恢复中断并解锁。 */
	scoped_guard (raw_spinlock_irqsave, &p->pi_lock)
		set_cpus_allowed_force(p, mask);

	/* It's safe because the task is inactive. */
	/* task 已确认不在 CPU 上，故设置禁止后续普通亲和性修改的标志不会与执行侧竞争。 */
	p->flags |= PF_NO_SETAFFINITY;
}

/*
 * __kthread_bind() - 把单个 CPU 转为 cpumask 后复用核心绑定协议。
 * @p: 输入输出借用 kthread。@cpu: possible CPU 编号，可 offline。@state: 目标必须达到
 * 的 inactive 状态。入口锁、睡眠和失败语义同 __kthread_bind_mask()。
 * 返回：无直接返回值；成功后 @p 硬绑定到 @cpu，ownership 不变。
 */
static void __kthread_bind(struct task_struct *p, unsigned int cpu, unsigned int state)
{
	__kthread_bind_mask(p, cpumask_of(cpu), state);
}

/*
 * kthread_bind_mask() - 首次唤醒前把刚创建的 kthread 硬绑定到 CPU 集合。
 *
 * 调用位置：设备、worker 等创建者在 kthread_create() 成功后、wake_up_process() 前。
 *
 * @p: 输入输出借用的未启动 kthread，必须处于 TASK_UNINTERRUPTIBLE。@mask: 纯输入
 * 借用的 possible CPU 集合，调用后无需保持存储。入口不持 pi_lock，可能等待目标
 * inactive。返回：无直接返回值；核心绑定失败仅告警。成功后设置硬亲和性和
 * PF_NO_SETAFFINITY；started 告警用于发现调用顺序违约，不转移 task 引用。
 */
void kthread_bind_mask(struct task_struct *p, const struct cpumask *mask)
{
	/* kthread 是 @p 控制块借用，仅用于绑定完成后的 started 契约检查。 */
	struct kthread *kthread = to_kthread(p);

	/* 简单包装先执行绑定，再用 started 检查“首次唤醒前调用”的入口契约。 */
	__kthread_bind_mask(p, mask, TASK_UNINTERRUPTIBLE);
	WARN_ON_ONCE(kthread->started);
}

/**
 * kthread_bind - bind a just-created kthread to a cpu.
 * @p: thread created by kthread_create().
 * @cpu: cpu (might not be online, must be possible) for @k to run on.
 *
 * Description: This function is equivalent to set_cpus_allowed(),
 * except that @cpu doesn't need to be online, and the thread must be
 * stopped (i.e., just returned from kthread_create()).
 */
/*
 * kthread_bind() - 首次唤醒前把刚创建的 kthread 硬绑定到单个 CPU。
 * @p: 输入输出借用的未启动 kthread，必须处于 TASK_UNINTERRUPTIBLE。@cpu: possible
 * CPU 编号，可以暂时 offline。入口不持 pi_lock，可能等待目标 inactive。
 * 返回：无直接返回值；核心绑定失败只告警。成功后 allowed mask 仅含 @cpu，并设置
 * PF_NO_SETAFFINITY；started 告警检测过晚调用。task 引用 ownership 不变。
 * 修正说明：上游 @cpu 描述中的“for @k”是旧参数名；当前目标参数是 @p。
 */
void kthread_bind(struct task_struct *p, unsigned int cpu)
{
	/* kthread 是 @p 控制块借用，仅用于检测过晚绑定。 */
	struct kthread *kthread = to_kthread(p);

	/* 单 CPU 包装与 mask 版本共享 inactive/锁协议，随后检查是否已经启动。 */
	__kthread_bind(p, cpu, TASK_UNINTERRUPTIBLE);
	WARN_ON_ONCE(kthread->started);
}
EXPORT_SYMBOL(kthread_bind);

/**
 * kthread_create_on_cpu - Create a cpu bound kthread
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @cpu: The cpu on which the thread should be bound,
 * @namefmt: printf-style name for the thread. Format is restricted
 *	     to "name.*%u". Code fills in cpu number.
 *
 * Description: This helper function creates and names a kernel thread
 */
/*
 * kthread_create_on_cpu() - 在 @cpu 所属 NUMA 节点创建并硬绑定未启动 kthread。
 * @threadfn: 纯输入回调，不可为 NULL。@data: 可空借用，覆盖线程使用期。@cpu:
 * possible CPU 编号，用于 NUMA 分配、硬绑定和以后 hotplug 恢复。@namefmt: 借用格式，
 * 受限为含 CPU 号的 name.*%u 形式。可睡眠进程上下文。
 * 成功返回未启动 task，已绑定 @cpu 并在私有块记录 cpu；调用者随后唤醒。创建失败
 * 原样返回 ERR_PTR(-ENOMEM/-EINTR)，不执行绑定。CPU offline 造成绑定丢失时，
 * per-CPU 线程的 kthread_unpark() 使用记录值重建；参数 ownership 不变。
 */
struct task_struct *kthread_create_on_cpu(int (*threadfn)(void *data),
					  void *data, unsigned int cpu,
					  const char *namefmt)
{
	/* p 是新建 task 的结果/错误槽，成功后作为返回的借用指针。 */
	struct task_struct *p;

	/* 阶段 1：以目标 CPU 的节点创建未启动线程；错误指针直接保持原 errno 返回。 */
	p = kthread_create_on_node(threadfn, data, cpu_to_node(cpu), namefmt,
				   cpu);
	if (IS_ERR(p))
		return p;
	/* 阶段 2：成功对象尚未运行，可安全硬绑定并记录 hotplug 恢复所需的 CPU。 */
	kthread_bind(p, cpu);
	/* CPU hotplug need to bind once again when unparking the thread. */
	/* CPU 下线可能使调度器丢弃硬绑定，因此保存编号，后续 unpark 在停驻状态重新绑定。 */
	to_kthread(p)->cpu = cpu;
	return p;
}
EXPORT_SYMBOL(kthread_create_on_cpu);

/*
 * kthread_set_per_cpu() - 设置或清除线程的 per-CPU 身份。
 * @k: 输入输出借用且已硬绑定的 kthread，不可为 NULL；调用者稳定生命周期。@cpu:
 * 非负 possible CPU 编号表示设置，负值表示清除身份但不自动撤销已有 allowed mask。
 * 由 per-CPU 子系统在创建/热插拔管理阶段调用；不取锁、不睡眠。返回：无直接返回值。
 * 缺失控制块静默返回；未设置 PF_NO_SETAFFINITY 告警。设置路径先写 cpu 再原子置位，
 * unpark 随后用它恢复绑定；清除只原子清位。无 task 引用 ownership 变化。
 */
void kthread_set_per_cpu(struct task_struct *k, int cpu)
{
	/* kthread 是 @k 控制块的调用期借用，承载身份位和恢复 CPU。 */
	struct kthread *kthread = to_kthread(k);

	/* 容错空控制块路径不修改 task；正常路径要求此前硬绑定已禁止普通改亲和性。 */
	if (!kthread)
		return;

	WARN_ON_ONCE(!(k->flags & PF_NO_SETAFFINITY));

	/* 清除身份不改 cpu 字段或 allowed mask；设置时必须先写编号、再发布可见位。 */
	if (cpu < 0) {
		clear_bit(KTHREAD_IS_PER_CPU, &kthread->flags);
		return;
	}

	kthread->cpu = cpu;
	set_bit(KTHREAD_IS_PER_CPU, &kthread->flags);
}

/*
 * kthread_is_per_cpu() - 容错查询 @p 是否为 per-CPU kthread。
 * @p: 纯输入借用，可为普通 task但不可为 NULL；调用者稳定 task 生命周期。调度/hotplug
 * 与控制代码用于选择专用绑定协议；不取锁、不睡眠。返回 true 仅表示有效 kthread 的
 * KTHREAD_IS_PER_CPU 位在查询时已置位；普通 task、控制块缺失或清位均为 false。
 * 返回是并发快照，无字段、引用或 ownership 副作用。
 */
bool kthread_is_per_cpu(struct task_struct *p)
{
	/* kthread 是容错识别结果；NULL 统一映射为 false。 */
	struct kthread *kthread = tsk_is_kthread(p);

	/* 简单容错查询先排除普通 task，再以原子 bitops 取得身份快照。 */
	if (!kthread)
		return false;

	return test_bit(KTHREAD_IS_PER_CPU, &kthread->flags);
}

/**
 * kthread_unpark - unpark a thread created by kthread_create().
 * @k:		thread created by kthread_create().
 *
 * Sets kthread_should_park() for @k to return false, wakes it, and
 * waits for it to return. If the thread is marked percpu then its
 * bound to the cpu again.
 */
/*
 * kthread_unpark() - 撤销 park 请求并唤醒 TASK_PARKED 线程。
 * @k: 输入输出借用的活 kthread，不可为 NULL；调用者保证 task/control block 覆盖调用。
 * 控制侧在 CPU online 或恢复服务时调用。入口不持本文件锁；per-CPU 重绑可能等待
 * TASK_PARKED inactive，普通路径不睡眠。返回：无直接返回值；未置 park 位幂等返回。
 * per-CPU 线程先恢复可能因 offline 丢失的绑定，再清位并 wake_up_state(TASK_PARKED)。
 * 清位与定向唤醒保证停驻循环至少观察其一，不等待 threadfn 真正恢复执行，ownership 不变。
 * 修正说明：上方英文称“waits for it to return”，但当前实现只有 clear_bit() 与
 * wake_up_state()，没有 completion/inactive 等待；因此当前版本是异步唤醒后立即返回。
 */
void kthread_unpark(struct task_struct *k)
{
	/* kthread 是 @k 控制块借用，提供 park/per-CPU 位与目标 CPU。 */
	struct kthread *kthread = to_kthread(k);

	if (!test_bit(KTHREAD_SHOULD_PARK, &kthread->flags))
		return;
	/*
	 * Newly created kthread was parked when the CPU was offline.
	 * The binding was lost and we need to set it again.
	 */
	/* CPU 离线时调度器可打破绑定；停驻状态允许在目标不执行时安全重装。 */
	if (test_bit(KTHREAD_IS_PER_CPU, &kthread->flags))
		__kthread_bind(k, kthread->cpu, TASK_PARKED);

	clear_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
	/*
	 * __kthread_parkme() will either see !SHOULD_PARK or get the wakeup.
	 */
	/* 清位与 wake 二者至少一个会被停驻循环观察，避免检查与睡眠之间丢失唤醒。 */
	wake_up_state(k, TASK_PARKED);
}
EXPORT_SYMBOL_GPL(kthread_unpark);

/**
 * kthread_park - park a thread created by kthread_create().
 * @k: thread created by kthread_create().
 *
 * Sets kthread_should_park() for @k to return true, wakes it, and
 * waits for it to return. This can also be called after kthread_create()
 * instead of calling wake_up_process(): the thread will park without
 * calling threadfn().
 *
 * Returns 0 if the thread is parked, -ENOSYS if the thread exited.
 * If called by the kthread itself just the park bit is set.
 */
/*
 * kthread_park() - 请求线程进入可恢复的 TASK_PARKED，并可选等待握手完成。
 * @k: 输入输出借用的活 kthread，不可为 NULL；调用者稳定 task/control block。CPU
 * hotplug 和子系统暂停路径调用；入口不持本文件锁。外部调用会唤醒并等待 completion/
 * inactive，可能睡眠；目标自调用只置位，避免等待自己死锁。
 * 返回 0 表示请求建立；外部调用还保证 task 已以 TASK_PARKED 调度出 CPU。PF_EXITING
 * 返回 -ENOSYS，重复 park 返回 -EBUSY，二者伴随告警。置位是请求发布点；parked
 * completion 只证明即将 schedule，随后 wait_task_inactive 完成第二阶段确认。
 */
int kthread_park(struct task_struct *k)
{
	/* kthread 是 @k 控制块借用，parked completion 覆盖整个外部等待。 */
	struct kthread *kthread = to_kthread(k);

	/* 阶段 1：退出中或已有请求都不能开启第二次握手，保持原控制状态返回。 */
	if (WARN_ON(k->flags & PF_EXITING))
		return -ENOSYS;

	if (WARN_ON_ONCE(test_bit(KTHREAD_SHOULD_PARK, &kthread->flags)))
		return -EBUSY;

	/* 阶段 2：原子发布请求；自调用不能等待自身，只把响应责任留给后续回调路径。 */
	set_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
	if (k != current) {
		wake_up_process(k);
		/*
		 * Wait for __kthread_parkme() to complete(), this means we
		 * _will_ have TASK_PARKED and are about to call schedule().
		 */
		/* completion 表示状态已写且马上 schedule，并不单独证明目标已离开 CPU。 */
		wait_for_completion(&kthread->parked);
		/*
		 * Now wait for that schedule() to complete and the task to
		 * get scheduled out.
		 */
		/* 第二阶段以 wait_task_inactive 确认硬绑定/CPU hotplug 可安全操作 task。 */
		WARN_ON_ONCE(!wait_task_inactive(k, TASK_PARKED));
	}

	return 0;
}
EXPORT_SYMBOL_GPL(kthread_park);

/**
 * kthread_stop - stop a thread created by kthread_create().
 * @k: thread created by kthread_create().
 *
 * Sets kthread_should_stop() for @k to return true, wakes it, and
 * waits for it to exit. This can also be called after kthread_create()
 * instead of calling wake_up_process(): the thread will exit without
 * calling threadfn().
 *
 * If threadfn() may call kthread_exit() itself, the caller must ensure
 * task_struct can't go away.
 *
 * Returns the result of threadfn(), or %-EINTR if wake_up_process()
 * was never called.
 */
/*
 * kthread_stop() - 发布永久停止请求，唤醒目标并同步等待退出结果。
 * @k: 输入输出借用的目标 kthread，不可为 NULL/current；调用者保证入口可取得 task
 * 引用，本函数 get_task_struct() 临时稳定到 exited 后。只能在可睡眠进程上下文。
 * 阶段：置 stop 位 → 撤销 park → 设置 TIF_NOTIFY_SIGNAL → 唤醒 → 等 exited → 读取
 * result 并 put 临时引用。多种唤醒覆盖 parked、可中断及普通睡眠。
 * 返回 threadfn/kthread_exit 的结果；若创建后从未首次 wake，返回 -EINTR。completion
 * 保证 result 的写入先于读取。函数不消耗调用者原有 task 引用。
 */
int kthread_stop(struct task_struct *k)
{
	/* kthread 在临时 task 引用下保持有效；ret 保存 completion 后读取的最终结果。 */
	struct kthread *kthread;
	int ret;

	trace_sched_kthread_stop(k);

	/* 阶段 1：跨越目标退出窗口前先取得临时 task 引用，再发布不可撤销 stop 请求。 */
	get_task_struct(k);
	kthread = to_kthread(k);
	set_bit(KTHREAD_SHOULD_STOP, &kthread->flags);
	kthread_unpark(k);
	set_tsk_thread_flag(k, TIF_NOTIFY_SIGNAL);
	wake_up_process(k);
	/* 阶段 2：同步等待退出侧写 result、清理亲和性并完成 exited。 */
	wait_for_completion(&kthread->exited);
	ret = kthread->result;
	put_task_struct(k);

	trace_sched_kthread_stop_ret(ret);
	return ret;
}
EXPORT_SYMBOL(kthread_stop);

/**
 * kthread_stop_put - stop a thread and put its task struct
 * @k: thread created by kthread_create().
 *
 * Stops a thread created by kthread_create() and put its task_struct.
 * Only use when holding an extra task struct reference obtained by
 * calling get_task_struct().
 */
/*
 * kthread_stop_put() - stop 后再释放调用者预先持有的一份 task 引用。
 * @k: 输入拥有指针，必须对应调用者此前 get_task_struct() 的额外引用；不可为 current。
 * 只能在可睡眠进程上下文。先调用 kthread_stop()（它另取临时引用），再 put 调用者的
 * 这份引用，因此成功返回后消耗 @k ownership，调用者不能继续依赖该引用。
 * 返回值原样透传 stop 的 threadfn 结果/-EINTR；无独立错误类别。
 */
int kthread_stop_put(struct task_struct *k)
{
	/* ret 保存 stop 结果，释放调用者引用后仍可安全返回该纯整数值。 */
	int ret;

	ret = kthread_stop(k);
	put_task_struct(k);
	return ret;
}
EXPORT_SYMBOL(kthread_stop_put);

/*
 * kthreadd() - 内核线程创建守护者和请求队列唯一消费者。
 * @unused: 未使用的纯输入占位，可为 NULL。启动代码创建并发布 kthreadd_task；本函数
 * 建立供所有子 kthread 继承的干净 comm、信号、mems、cgroup 和 housekeeping 环境。
 * 永久运行于可睡眠进程上下文，返回路径理论不可达（源码 0 仅满足签名）。
 * 循环先设置 TASK_INTERRUPTIBLE 再检查队列，与生产者入队+wake 配对避免丢唤醒；
 * 持 create spinlock 摘取请求，clone 前释放以免在分配睡眠时阻塞生产者。每个 create
 * 摘链后 ownership 交给 create_kthread()/新线程；无正常失败出口。
 */
int kthreadd(void *unused)
{
	/*
	 * 变量地图：comm 是写入 current->comm 的固定名称；tsk 是 current 的借用别名。
	 * 循环内 create 是刚从全局链表摘下的请求，解锁后由 create_kthread() 接管处理。
	 */
	static const char comm[TASK_COMM_LEN] = "kthreadd";
	struct task_struct *tsk = current;

	/* Setup a clean context for our children to inherit. */
	/* 子 kthread 会继承此处建立的 comm 之外环境：忽略信号、允许有内存的节点。 */
	set_task_comm(tsk, comm);
	ignore_signals(tsk);
	set_mems_allowed(node_states[N_MEMORY]);

	current->flags |= PF_NOFREEZE;
	cgroup_init_kthreadd();

	kthread_affine_node();

	for (;;) {
		/* 先设睡眠状态再检查队列，与生产者入队后 wake_up_process 配合避免丢失唤醒。 */
		set_current_state(TASK_INTERRUPTIBLE);
		if (list_empty(&kthread_create_list))
			schedule();
		__set_current_state(TASK_RUNNING);

		/* 阶段 2：持锁逐个摘链；每次处理前放锁，使生产者和可能睡眠的 clone 都可推进。 */
		spin_lock(&kthread_create_lock);
		while (!list_empty(&kthread_create_list)) {
			struct kthread_create_info *create;

			create = list_entry(kthread_create_list.next,
					    struct kthread_create_info, list);
			list_del_init(&create->list);
			spin_unlock(&kthread_create_lock);

			/* 请求已私有化；kernel_thread 可能分配/睡眠，必须在自旋锁外调用。 */
			create_kthread(create);

			spin_lock(&kthread_create_lock);
		}
		spin_unlock(&kthread_create_lock);
	}

	return 0;
}

/**
 * kthread_affine_preferred - Define a kthread's preferred affinity
 * @p: thread created by kthread_create().
 * @mask: preferred mask of CPUs (might not be online, must be possible) for @p
 *        to run on.
 *
 * Similar to kthread_bind_mask() except that the affinity is not a requirement
 * but rather a preference that can be constrained by CPU isolation or CPU hotplug.
 * Must be called before the first wakeup of the kthread.
 *
 * Returns 0 if the affinity has been applied.
 */
/*
 * kthread_affine_preferred() - 首次唤醒前安装可被隔离/hotplug 约束的软偏好。
 * @p: 输入输出借用的未启动 kthread，必须 TASK_UNINTERRUPTIBLE 且 started==0。
 * @mask: 纯输入借用的 possible CPU 集合，可包含 offline/isolated CPU；函数复制内容，
 * 返回后调用者可释放原掩码。进程上下文中等待 inactive、GFP_KERNEL 分配并持 affinity
 * mutex，可能睡眠。成功返回 0：私有副本 ownership 交给 kthread，加入动态管理链，
 * 实际 allowed mask 取偏好与 housekeeping 的非空交集。-EINVAL 表示状态/调用顺序错误；
 * -ENOMEM 表示临时或持久掩码分配失败。与硬绑定不同，不置 PF_NO_SETAFFINITY，后续
 * CPU/cpuset 变化可重算；task 引用 ownership 不变。
 */
int kthread_affine_preferred(struct task_struct *p, const struct cpumask *mask)
{
	/*
	 * 变量地图：kthread 是 @p 私有块借用；affinity 是仅在本函数有效的计算输出掩码；
	 * ret 累积 0/-ENOMEM。preferred_affinity 是成功分配后转给控制块长期拥有的副本。
	 */
	struct kthread *kthread = to_kthread(p);
	cpumask_var_t affinity;
	int ret = 0;

	/* 阶段 1：确认 task 尚未首次运行；发布后的回调会与这些字段并发，不能再安装。 */
	if (!wait_task_inactive(p, TASK_UNINTERRUPTIBLE) || kthread->started) {
		WARN_ON(1);
		return -EINVAL;
	}

	WARN_ON_ONCE(kthread->preferred_affinity);

	/* 阶段 2：分别分配本次计算缓冲和控制块长期拥有的偏好副本。 */
	if (!zalloc_cpumask_var(&affinity, GFP_KERNEL))
		return -ENOMEM;

	kthread->preferred_affinity = kzalloc(sizeof(struct cpumask), GFP_KERNEL);
	if (!kthread->preferred_affinity) {
		ret = -ENOMEM;
		goto out;
	}

	/* 阶段 3：task 尚未启动使新指针不可被回调观察；锁内填内容并发布链表成员关系。 */
	mutex_lock(&kthread_affinity_lock);
	cpumask_copy(kthread->preferred_affinity, mask);
	WARN_ON_ONCE(!list_empty(&kthread->affinity_node));
	list_add_tail(&kthread->affinity_node, &kthread_affinity_list);
	kthread_fetch_affinity(kthread, affinity);

	/* 调度器的 allowed mask 由 pi_lock 保护；scoped_guard 在单条语句结束后自动解锁。 */
	scoped_guard (raw_spinlock_irqsave, &p->pi_lock)
		set_cpus_allowed_force(p, affinity);

	mutex_unlock(&kthread_affinity_lock);
out:
	/* 失败时只有临时 affinity 需要释放；已安装偏好仅在完整成功路径存在。 */
	free_cpumask_var(affinity);

	return ret;
}
EXPORT_SYMBOL_GPL(kthread_affine_preferred);

/*
 * kthreads_update_affinity() - 串行重算所有受管理 kthread 的有效 CPU 集合。
 *
 * 调用位置：CPU online 回调以 force=false 调用；cpuset/housekeeping 改变以 true 调用。
 * @force: 纯输入布尔值；true 强制处理所有受管理线程，false 只需恢复显式/NUMA 偏好。
 * 入参无其他 ownership。mutex guard 覆盖完整遍历，与退出摘链及偏好安装串行；函数会
 * GFP_KERNEL 分配掩码并调用 set_cpus_allowed_ptr()，所以只能在可睡眠进程上下文。
 * 返回 0 表示遍历完成或链表为空；-ENOMEM 表示临时掩码分配失败；发现硬绑定/per-CPU
 * 对象错误入链时继续处理其余项并最终返回 -EINVAL。每项都会请求调度器更新，但
 * set_cpus_allowed_ptr() 的返回值未汇入 @ret，因此 0 不额外证明每次迁移均已即时成功。
 */
static int kthreads_update_affinity(bool force)
{
	/*
	 * 变量地图：affinity 是遍历中反复覆写的临时输出掩码；k 是 mutex 保护链表中的借用
	 * 控制块；ret 初始 0，发现错误成员后保留 -EINVAL，同时继续更新其余线程。
	 */
	cpumask_var_t affinity;
	struct kthread *k;
	int ret;

	guard(mutex)(&kthread_affinity_lock);
	/* guard 在所有 return 路径自动解锁，稳定链表、偏好掩码及退出侧摘链。 */

	/* 快速路径：没有受管理线程时不必分配临时 cpumask。 */
	if (list_empty(&kthread_affinity_list))
		return 0;

	if (!zalloc_cpumask_var(&affinity, GFP_KERNEL))
		return -ENOMEM;

	/* 慢速路径：分配一次缓冲供整个遍历复用；单个坏成员不阻断其余线程收敛。 */
	ret = 0;

	list_for_each_entry(k, &kthread_affinity_list, affinity_node) {
		/* 动态链表绝不应包含硬绑定或 per-CPU 线程；发现后记录错误并跳过。 */
		if (WARN_ON_ONCE((k->task->flags & PF_NO_SETAFFINITY) ||
				 kthread_is_per_cpu(k->task))) {
			ret = -EINVAL;
			continue;
		}

		/*
		 * Unbound kthreads without preferred affinity are already affine
		 * to housekeeping, whether those CPUs are online or not. So no need
		 * to handle newly online CPUs for them. However housekeeping changes
		 * have to be applied.
		 *
		 * But kthreads with a preferred affinity or node are different:
		 * if none of their preferred CPUs are online and part of
		 * housekeeping at the same time, they must be affine to housekeeping.
		 * But as soon as one of their preferred CPU becomes online, they must
		 * be affine to them.
		 */
		/*
		 * 无偏好的 unbound 线程一直允许全部 housekeeper，上线事件无需重写；显式偏好或
		 * NUMA 偏好则需在 CPU 上线时从回退集合恢复。force 用于策略本身改变。
		 */
		if (force || k->preferred_affinity || k->node != NUMA_NO_NODE) {
			kthread_fetch_affinity(k, affinity);
			set_cpus_allowed_ptr(k->task, affinity);
		}
	}

	/* 所有 set_cpus_allowed_ptr() 已复制掩码内容，遍历缓冲可在返回前释放。 */
	free_cpumask_var(affinity);

	return ret;
}

/**
 * kthreads_update_housekeeping - Update kthreads affinity on cpuset change
 *
 * When cpuset changes a partition type to/from "isolated" or updates related
 * cpumasks, propagate the housekeeping cpumask change to preferred kthreads
 * affinity.
 *
 * Returns 0 if successful, -ENOMEM if temporary mask couldn't
 * be allocated or -EINVAL in case of internal error.
 */
/*
 * kthreads_update_housekeeping() - cpuset 隔离变化后重算 kthread 亲和性。
 *
 * cpuset 把新 HK_TYPE_DOMAIN 掩码发布并更新 unbound workqueue/timer migration
 * 后调用这里。入参：无。函数在可睡眠进程上下文运行；下层会分配临时 cpumask、
 * 遍历受管理 kthread 并调用 set_cpus_allowed_ptr()。
 *
 * force=true 表示即使 CPU online 集合没有变化，也必须按新 housekeeping 策略
 * 重算。带 preferred_affinity 或 NUMA node 偏好的线程优先保留仍有效的偏好；
 * 偏好与在线 housekeeper 无交集时回退到 housekeeping 集合。
 * 返回 0 成功，临时分配失败返回 -ENOMEM，内部亲和性错误返回 -EINVAL。
 */
int kthreads_update_housekeeping(void)
{
	return kthreads_update_affinity(true);
}

/*
 * Re-affine kthreads according to their preferences
 * and the newly online CPU. The CPU down part is handled
 * by select_fallback_rq() which default re-affines to
 * housekeepers from other nodes in case the preferred
 * affinity doesn't apply anymore.
 */
/*
 * CPU 上线时按线程偏好重新计算亲和性；CPU 下线则由 select_fallback_rq()
 * 把失去有效偏好的线程回退到其他节点的 housekeeper。@cpu 是 hotplug 回调提供
 * 的新上线 CPU 编号，本薄包装不直接使用它，因为下层读取完整 online 集合。
 */
/*
 * kthreads_online_cpu() - CPUHP online 阶段恢复 kthread 的显式/NUMA 偏好。
 * @cpu: 新上线 CPU 编号，纯输入但不直接读取；有效集合由下层统一快照。运行在 CPU
 * hotplug 可睡眠上下文，不持 affinity mutex。返回 0、-ENOMEM 或 -EINVAL，原样透传
 * kthreads_update_affinity(false)。函数不处理 down：调度器 fallback 负责下线迁移。
 */
static int kthreads_online_cpu(unsigned int cpu)
{
	return kthreads_update_affinity(false);
}

/*
 * kthreads_init() - 在启动早期注册 kthread 的 CPU online 回调。
 * 入参：无；early_initcall 进程上下文，可睡眠性由 cpuhp 注册接口决定。
 * 返回 cpuhp_setup_state() 的 0 或负 errno；成功后每次 CPU 上线调用
 * kthreads_online_cpu()，CPU 下线继续由调度器 fallback 路径处理。无 ownership 转移。
 */
static int kthreads_init(void)
{
	return cpuhp_setup_state(CPUHP_AP_KTHREADS_ONLINE, "kthreads:online",
				kthreads_online_cpu, NULL);
}

early_initcall(kthreads_init);

/*
 * __kthread_init_worker() - 初始化调用者提供的 worker 及其 lockdep 类。
 * @worker: 纯输出对象，调用者拥有存储且保证尚无并发访问；返回后仍由调用者拥有。
 * @name: lockdep 类名的借用字符串。@key: lockdep class key 借用，须具有静态生命期。
 * 不取外部锁、不分配、不睡眠；清零旧内容后初始化 raw spinlock、即时队列和延迟队列。
 * 返回：无直接返回值。成功出口的 worker 为空闲状态，尚未关联 task 或 current_work。
 */
void __kthread_init_worker(struct kthread_worker *worker,
				const char *name,
				struct lock_class_key *key)
{
	/* 阶段 1：调用者独占未发布对象，先清零所有历史指针和状态。 */
	memset(worker, 0, sizeof(struct kthread_worker));
	/* 阶段 2：建立 IRQ-safe 队列锁及两个空链表；此后对象才满足入队前置条件。 */
	raw_spin_lock_init(&worker->lock);
	lockdep_set_class_and_name(&worker->lock, key, name);
	INIT_LIST_HEAD(&worker->work_list);
	INIT_LIST_HEAD(&worker->delayed_work_list);
}

EXPORT_SYMBOL_GPL(__kthread_init_worker);

/**
 * kthread_worker_fn - kthread function to process kthread_worker
 * @worker_ptr: pointer to initialized kthread_worker
 *
 * This function implements the main cycle of kthread worker. It processes
 * work_list until it is stopped with kthread_stop(). It sleeps when the queue
 * is empty.
 *
 * The works are not allowed to keep any locks, disable preemption or interrupts
 * when they finish. There is defined a safe point for freezing when one work
 * finishes and before a new one is started.
 *
 * Also the works must not be handled by more than one worker at the same time,
 * see also kthread_queue_work().
 */
/*
 * kthread_worker_fn() - 单线程串行取出并执行 worker 队列。
 * @worker_ptr: 纯输入借用，实际指向已初始化 kthread_worker，不可为 NULL；worker 与
 * 其中 work 队列覆盖 task 生命期。由 create_worker 创建的 kthread 作为 threadfn 调用，
 * 也兼容旧用户直接运行；运行于可睡眠进程上下文。
 * worker->lock 是 IRQ-safe raw lock，保护 work_list、current_work、task 和 canceling；
 * 回调在锁外且 TASK_RUNNING 下串行执行，可睡眠，但返回时不得遗留锁、关闭抢占/IRQ。
 * 空队列进入 TASK_INTERRUPTIBLE；KTW_FREEZABLE 时每个 work 之间是冻结安全点。
 * kthread_stop() 到达后锁内清空 worker->task 并返回 0；该发布阻止入队侧继续唤醒退出
 * task。函数不释放 worker/work ownership，销毁由 kthread_destroy_worker() 完成。
 */
int kthread_worker_fn(void *worker_ptr)
{
	/*
	 * 变量地图：worker 是 @worker_ptr 的类型化借用；work 是本轮从队列唯一领取的任务，
	 * NULL 表示队列空；func 在调用回调前保存，允许回调释放 work 容器后仍完成 trace。
	 */
	struct kthread_worker *worker = worker_ptr;
	struct kthread_work *work;

	/*
	 * FIXME: Update the check and remove the assignment when all kthread
	 * worker users are created using kthread_create_worker*() functions.
	 */
	/* 兼容旧调用者直接运行 worker_fn；迁移完成后应只允许 create_worker 设置 task。 */
	WARN_ON(worker->task && worker->task != current);
	worker->task = current;

	if (worker->flags & KTW_FREEZABLE)
		set_freezable();

repeat:
	/* 阶段 1：先发布可中断睡眠状态，再检查 stop；与 stop 的条件写/唤醒屏障配对。 */
	set_current_state(TASK_INTERRUPTIBLE);	/* mb paired w/ kthread_stop */
	/* 状态写带屏障，与 stop 的条件写+wakeup 配对，防止“看不到 stop 且错过唤醒”。 */

	if (kthread_should_stop()) {
		/* 清空 task 是销毁发布边界：出锁后新的 queue 只入链，不再尝试唤醒已退出线程。 */
		__set_current_state(TASK_RUNNING);
		raw_spin_lock_irq(&worker->lock);
		worker->task = NULL;
		raw_spin_unlock_irq(&worker->lock);
		return 0;
	}

	/* 阶段 2：锁内从 FIFO 唯一领取一个 work，并发布 current_work 给 flush/cancel 观察。 */
	work = NULL;
	/* 在 raw lock 下唯一领取队首并发布 current_work，取消/flush 据此区分排队与执行。 */
	raw_spin_lock_irq(&worker->lock);
	if (!list_empty(&worker->work_list)) {
		work = list_first_entry(&worker->work_list,
					struct kthread_work, node);
		list_del_init(&work->node);
	}
	worker->current_work = work;
	raw_spin_unlock_irq(&worker->lock);

	/* 阶段 3：锁外执行回调；无工作则睡眠，冻结请求则先恢复 RUNNING。 */
	if (work) {
		kthread_work_func_t func = work->func;
		__set_current_state(TASK_RUNNING);
		trace_sched_kthread_work_execute_start(work);
		work->func(work);
		/*
		 * Avoid dereferencing work after this point.  The trace
		 * event only cares about the address.
		 */
		/* 回调可以释放包含 work 的对象；结束 trace 以后只允许使用已保存地址/func 值。 */
		trace_sched_kthread_work_execute_end(work, func);
	} else if (!freezing(current)) {
		/* 空队列保持 TASK_INTERRUPTIBLE 睡眠，queue 路径在首次工作到来时唤醒。 */
		schedule();
	} else {
		/*
		 * Handle the case where the current remains
		 * TASK_INTERRUPTIBLE. try_to_freeze() expects
		 * the current to be TASK_RUNNING.
		 */
		/* refrigerator 要求 RUNNING；冻结请求存在时先修正状态再进入冻结点。 */
		__set_current_state(TASK_RUNNING);
	}

	/* 阶段 4：每个 work 边界处理 freezer、主动让出 CPU，然后开始下一轮。 */
	try_to_freeze();
	cond_resched();
	goto repeat;
}
EXPORT_SYMBOL_GPL(kthread_worker_fn);

/*
 * __kthread_create_worker_on_node() - 分配 worker 并创建尚未启动的执行 task。
 * @flags: KTW_* 行为位，成功后写入 worker。@node: task/栈 NUMA 节点或
 * NUMA_NO_NODE。@namefmt/@args: 名称格式与 va_list 借用，仅创建期间有效。
 * 运行于可睡眠进程上下文；先分配/初始化 worker，再调用 kthread 核心创建接口。
 * 成功返回由调用者拥有的 worker，worker->task 已设置但 task 尚未首次唤醒；失败返回
 * ERR_PTR(-ENOMEM/-EINTR)，并从 fail_task 逆序释放已分配 worker，不遗留 ownership。
 */
static __printf(3, 0) struct kthread_worker *
__kthread_create_worker_on_node(unsigned int flags, int node,
				const char namefmt[], va_list args)
{
	/* worker 是本函数新分配的拥有指针；task 是核心创建返回的未启动 task 或错误指针。 */
	struct kthread_worker *worker;
	struct task_struct *task;

	/* 阶段 1：分配并建立空队列/锁；此时 worker 尚无异步可见性。 */
	worker = kzalloc_obj(*worker);
	if (!worker)
		return ERR_PTR(-ENOMEM);

	kthread_init_worker(worker);

	/* 阶段 2：创建以 worker 为 data 的未启动 task，成功后两者生命周期绑定。 */
	task = __kthread_create_on_node(kthread_worker_fn, worker,
					node, namefmt, args);
	if (IS_ERR(task))
		goto fail_task;

	/* 成功发布边界：先写行为 flags，再把可唤醒 task 挂入 worker，调用者随后取得对象。 */
	worker->flags = flags;
	worker->task = task;

	return worker;

fail_task:
	/* task 创建失败，没有异步执行者持有 worker，可直接回滚唯一分配。 */
	kfree(worker);
	return ERR_CAST(task);
}

/**
 * kthread_create_worker_on_node - create a kthread worker
 * @flags: flags modifying the default behavior of the worker
 * @node: task structure for the thread is allocated on this node
 * @namefmt: printf-style name for the kthread worker (task).
 *
 * Returns a pointer to the allocated worker on success, ERR_PTR(-ENOMEM)
 * when the needed structures could not get allocated, and ERR_PTR(-EINTR)
 * when the caller was killed by a fatal signal.
 */
/*
 * kthread_create_worker_on_node() - 分配 worker 和未启动 task。
 * @flags: KTW_* 行为位，例如 KTW_FREEZABLE。@node: task/内核栈 NUMA 节点或
 * NUMA_NO_NODE。@namefmt: printf 名称格式借用；尾随参数仅在创建期间读取。
 * 可睡眠进程上下文。成功返回由调用者拥有、最终须 kthread_destroy_worker() 的 worker；
 * worker->task 已创建但 threadfn 尚未运行，调用者可先配置后唤醒。分配失败返回
 * ERR_PTR(-ENOMEM)，致命信号中断创建等待返回 ERR_PTR(-EINTR)；失败不遗留 worker。
 */
struct kthread_worker *
kthread_create_worker_on_node(unsigned int flags, int node, const char namefmt[], ...)
{
	/* worker 是新分配对象的结果/错误槽；args 只在核心创建调用期间有效。 */
	struct kthread_worker *worker;
	va_list args;

	/* 单阶段可变参数包装：核心函数返回后 va_list 即可结束，不被异步 task 保存。 */
	va_start(args, namefmt);
	worker = __kthread_create_worker_on_node(flags, node, namefmt, args);
	va_end(args);

	return worker;
}
EXPORT_SYMBOL(kthread_create_worker_on_node);

/**
 * kthread_create_worker_on_cpu - create a kthread worker and bind it
 *	to a given CPU and the associated NUMA node.
 * @cpu: CPU number
 * @flags: flags modifying the default behavior of the worker
 * @namefmt: printf-style name for the thread. Format is restricted
 *	     to "name.*%u". Code fills in cpu number.
 *
 * Use a valid CPU number if you want to bind the kthread worker
 * to the given CPU and the associated NUMA node.
 *
 * A good practice is to add the cpu number also into the worker name.
 * For example, use kthread_create_worker_on_cpu(cpu, "helper/%d", cpu).
 *
 * CPU hotplug:
 * The kthread worker API is simple and generic. It just provides a way
 * to create, use, and destroy workers.
 *
 * It is up to the API user how to handle CPU hotplug. They have to decide
 * how to handle pending work items, prevent queuing new ones, and
 * restore the functionality when the CPU goes off and on. There are a
 * few catches:
 *
 *    - CPU affinity gets lost when it is scheduled on an offline CPU.
 *
 *    - The worker might not exist when the CPU was off when the user
 *      created the workers.
 *
 * Good practice is to implement two CPU hotplug callbacks and to
 * destroy/create the worker when the CPU goes down/up.
 *
 * Return:
 * The pointer to the allocated worker on success, ERR_PTR(-ENOMEM)
 * when the needed structures could not get allocated, and ERR_PTR(-EINTR)
 * when the caller was killed by a fatal signal.
 */
/*
 * kthread_create_worker_on_cpu() - 创建 worker 并把 task 硬绑定到 @cpu。
 * @cpu: possible CPU 编号，用于 NUMA 分配和硬绑定。@flags: KTW_* 行为位。
 * @namefmt: 借用名称格式，接口会传入 cpu 作为格式参数；建议名称也包含 CPU 编号。
 * 可睡眠进程上下文。成功返回调用者拥有的 worker，task 已绑定但尚未唤醒；失败返回
 * ERR_PTR(-ENOMEM/-EINTR)。通用 worker API 不代管 CPU hotplug：CPU 下线会丢亲和性，
 * 创建时 CPU 已离线也可能使期望的 worker 不存在；用户应在 down/up 回调中停止入队、
 * 处理 pending work 并销毁/重建 worker。参数 ownership 不变。
 * 修正说明：英文示例来自旧参数顺序；当前声明第二项是 @flags，且 CPU 号由实现作为
 * namefmt 的格式参数传入，当前形式应类似 kthread_create_worker_on_cpu(cpu, flags,
 * "helper/%u")，不能把第三个 cpu 实参放到本接口之后。
 */
struct kthread_worker *
kthread_create_worker_on_cpu(int cpu, unsigned int flags,
			     const char namefmt[])
{
	/* worker 是节点创建结果/错误槽，成功时再借用其 task 完成硬绑定。 */
	struct kthread_worker *worker;

	/* 阶段 1：按 CPU 所属节点创建尚未启动的 worker，错误指针保持原样返回。 */
	worker = kthread_create_worker_on_node(flags, cpu_to_node(cpu), namefmt, cpu);
	/* 阶段 2：仅成功对象可在首次唤醒前安全硬绑定；接口仍把启动责任交给调用者。 */
	if (!IS_ERR(worker))
		kthread_bind(worker->task, cpu);

	return worker;
}
EXPORT_SYMBOL(kthread_create_worker_on_cpu);

/*
 * Returns true when the work could not be queued at the moment.
 * It happens when it is already pending in a worker list
 * or when it is being cancelled.
 */
/*
 * queuing_blocked() - 在 worker 锁内判断 work 是否允许再次入队。
 * @worker: 纯输入借用，调用者必须持 worker->lock。@work: 输入借用，node 与 canceling
 * 在同一锁下稳定。不可睡眠、无 ownership 变化。返回 true 表示 work 已在即时/延迟
 * 链表，或同步取消正处于放锁窗口；false 表示调用者可继续发布。
 */
static inline bool queuing_blocked(struct kthread_worker *worker,
				   struct kthread_work *work)
{
	/* lockdep 断言把“node/canceling 必须在同一把 worker 锁下观察”变为可检测契约。 */
	lockdep_assert_held(&worker->lock);

	return !list_empty(&work->node) || work->canceling;
}

/*
 * kthread_insert_work_sanity_check() - 在真正插链前验证 work 的单 worker 不变量。
 * @worker/@work 均为借用；调用者必须持 worker->lock，不可睡眠。返回：无直接返回值；
 * node 非空或历史 worker 不同只触发告警，帮助发现重复入队/跨 worker 复用。函数本身
 * 不修复状态；正确调用的 work 必须已 list_del_init 且未更换归属。
 */
static void kthread_insert_work_sanity_check(struct kthread_worker *worker,
					     struct kthread_work *work)
{
	/* 两个 WARN 都是诊断而非恢复；正确性仍要求调用者只传入空闲且归属一致的 work。 */
	lockdep_assert_held(&worker->lock);
	WARN_ON_ONCE(!list_empty(&work->node));
	/* Do not use a work with >1 worker, see kthread_queue_work() */
	/* 同一 work 不得跨 worker 使用，否则取消方会取得错误的锁并破坏两个队列。 */
	WARN_ON_ONCE(work->worker && work->worker != worker);
}

/* insert @work before @pos in @worker */
/* 在 @worker 中把 @work 插到 @pos 之前；三者都由持锁调用者稳定。 */
/*
 * kthread_insert_work() - 在锁内发布 work 并按需唤醒执行线程。
 * @worker: 输入输出借用，调用者持 worker->lock。@work: 输入输出借用，入口必须空闲，
 * 返回时 node 已入链且 worker 归属已记录。@pos: 该 worker 某条队列中的借用位置。
 * 不可睡眠。返回：无直接返回值；list_add_tail() 是队列发布点。若 worker 当前未执行
 * work 且 task 存活，则 wake_up_process() 使其观察新队列；否则由现有循环自然领取。
 */
static void kthread_insert_work(struct kthread_worker *worker,
				struct kthread_work *work,
				struct list_head *pos)
{
	/* 阶段 1：在任何可观察修改前验证 node 空闲及历史 worker 归属。 */
	kthread_insert_work_sanity_check(worker, work);

	trace_sched_kthread_work_queue_work(worker, work);

	/* 阶段 2：先入链再记录归属；两项都在同一 raw lock 下原子地对竞争者可见。 */
	list_add_tail(&work->node, pos);
	work->worker = worker;
	if (!worker->current_work && likely(worker->task))
		wake_up_process(worker->task);
}

/**
 * kthread_queue_work - queue a kthread_work
 * @worker: target kthread_worker
 * @work: kthread_work to queue
 *
 * Queue @work to work processor @task for async execution.  @task
 * must have been created with kthread_create_worker().  Returns %true
 * if @work was successfully queued, %false if it was already pending.
 *
 * Reinitialize the work if it needs to be used by another worker.
 * For example, when the worker was stopped and started again.
 */
/*
 * kthread_queue_work() - 尝试把一个空闲 work 异步发布给指定 worker。
 * @worker: 输入输出借用，调用者保证 worker 及其 task 覆盖排队/执行期。@work: 输入输出
 * 借用，入口须已初始化且不能同时属于另一 worker；其容器须覆盖完成或同步取消。
 * 可从 IRQ 调用；函数 irqsave 获取 worker->lock，不睡眠，与执行、timer 和取消路径
 * 串行。返回 true 表示本次把 node 插入 work_list、pending 数增加；false 表示已 pending
 * 或 canceling，状态不变。成功后 worker 持有异步使用权，但不取得引用计数 ownership。
 * 修正说明：英文中的“@task”是旧接口措辞；当前参数是 @worker，实际执行 task 来自
 * worker->task，并由 kthread_create_worker*() 或兼容初始化路径建立。
 */
bool kthread_queue_work(struct kthread_worker *worker,
			struct kthread_work *work)
{
	/* ret 记录本次是否完成 idle→pending 转换；flags 保存并恢复调用者 IRQ 状态。 */
	bool ret = false;
	unsigned long flags;

	/* 唯一临界区把“检查空闲”和“插链发布”合并，两个并发入队者至多一个成功。 */
	raw_spin_lock_irqsave(&worker->lock, flags);
	if (!queuing_blocked(worker, work)) {
		kthread_insert_work(worker, work, &worker->work_list);
		ret = true;
	}
	raw_spin_unlock_irqrestore(&worker->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(kthread_queue_work);

/**
 * kthread_delayed_work_timer_fn - callback that queues the associated kthread
 *	delayed work when the timer expires.
 * @t: pointer to the expired timer
 *
 * The format of the function is defined by struct timer_list.
 * It should have been called from irqsafe timer with irq already off.
 */
/*
 * kthread_delayed_work_timer_fn() - timer 到期时把 delayed work 迁到即时队列。
 * @t: 已到期的 timer 借用；timer_container_of() 恢复包含它的 dwork。timer 必须以
 * irqsafe 方式初始化，入口已经关闭本地 IRQ；函数仍 irqsave 获取 worker->lock，以便
 * 和进程上下文的 queue/mod/cancel 共用同一协议，不可睡眠。
 * 返回：无直接返回值。正常路径从 delayed_work_list 摘除 node；未处于 canceling 时
 * 再发布到 work_list，否则保持不 pending。worker 为 NULL 表示 pending 期间被错误
 * 重初始化，只告警返回；函数不取得或释放 work 容器 ownership。
 */
void kthread_delayed_work_timer_fn(struct timer_list *t)
{
	/*
	 * 变量地图：dwork/work 依次从 timer 恢复包含对象；worker 是 work 的历史归属借用；
	 * flags 保存本地 IRQ 状态，使回调与进程/IRQ 入队路径使用同一 raw lock 协议。
	 */
	struct kthread_delayed_work *dwork = timer_container_of(dwork, t,
								timer);
	struct kthread_work *work = &dwork->work;
	struct kthread_worker *worker = work->worker;
	unsigned long flags;

	/*
	 * This might happen when a pending work is reinitialized.
	 * It means that it is used a wrong way.
	 */
	/* pending 期间重新初始化会清掉 worker 归属；这是 API 误用，无法安全确定应锁哪一队列。 */
	if (WARN_ON_ONCE(!worker))
		return;

	raw_spin_lock_irqsave(&worker->lock, flags);
	/* Work must not be used with >1 worker, see kthread_queue_work(). */
	/* 锁内复核持久归属，防止从错误 worker 的 delayed_work_list 摘节点。 */
	WARN_ON_ONCE(work->worker != worker);

	/* Move the work from worker->delayed_work_list. */
	/* 先摘延迟链，再依据 canceling 决定是否发布到即时链，node 同时只属于一条链。 */
	WARN_ON_ONCE(list_empty(&work->node));
	list_del_init(&work->node);
	if (!work->canceling)
		kthread_insert_work(worker, work, &worker->work_list);

	raw_spin_unlock_irqrestore(&worker->lock, flags);
}
EXPORT_SYMBOL(kthread_delayed_work_timer_fn);

/*
 * __kthread_queue_delayed_work() - 在锁内立即发布或建立延时 timer。
 * @worker: 输入输出借用，调用者持 worker->lock。@dwork: 输入输出借用且必须空闲；
 * @delay: 从当前 jiffies 起等待的 tick 数，0 具有“立即可运行”语义。不可睡眠。
 * 返回：无直接返回值。delay=0 把 work 发布到即时链；非零时发布到 delayed_work_list、
 * 记录 worker 和 expires，再把 timer 交给 timer 子系统。调用者仍拥有对象存储。
 */
static void __kthread_queue_delayed_work(struct kthread_worker *worker,
					 struct kthread_delayed_work *dwork,
					 unsigned long delay)
{
	/* timer/work 是 @dwork 两个内嵌视图，均只在持 worker 锁的本次调用中借用。 */
	struct timer_list *timer = &dwork->timer;
	struct kthread_work *work = &dwork->work;

	/* 阶段 1：回调指针不匹配意味着对象未按 delayed-work API 初始化，只诊断而不改写。 */
	WARN_ON_ONCE(timer->function != kthread_delayed_work_timer_fn);

	/*
	 * If @delay is 0, queue @dwork->work immediately.  This is for
	 * both optimization and correctness.  The earliest @timer can
	 * expire is on the closest next tick and delayed_work users depend
	 * on that there's no such delay when @delay is 0.
	 */
	/*
	 * delay=0 同时是优化和正确性要求：timer 最早也要到下一 tick，用户则依赖零延迟
	 * 不引入这一额外等待，所以必须绕过 timer 直接进入即时队列。
	 */
	if (!delay) {
		kthread_insert_work(worker, work, &worker->work_list);
		return;
	}

	/* Be paranoid and try to detect possible races already now. */
	/* 启动 timer 前验证 node 空闲，尽早暴露重复入队或跨 worker 复用。 */
	kthread_insert_work_sanity_check(worker, work);

	/* 阶段 2：非零延迟先发布到受锁保护的延迟链，再填写到期值并交给 timer wheel。 */
	list_add(&work->node, &worker->delayed_work_list);
	work->worker = worker;
	timer->expires = jiffies + delay;
	add_timer(timer);
}

/**
 * kthread_queue_delayed_work - queue the associated kthread work
 *	after a delay.
 * @worker: target kthread_worker
 * @dwork: kthread_delayed_work to queue
 * @delay: number of jiffies to wait before queuing
 *
 * If the work has not been pending it starts a timer that will queue
 * the work after the given @delay. If @delay is zero, it queues the
 * work immediately.
 *
 * Return: %false if the @work has already been pending. It means that
 * either the timer was running or the work was queued. It returns %true
 * otherwise.
 */
/*
 * kthread_queue_delayed_work() - 若 delayed work 空闲则按 jiffies 延迟发布。
 * @worker: 输入输出借用，须覆盖 timer/执行期。@dwork: 输入输出借用，其容器须覆盖
 * 完成或同步取消。@delay: 延迟 tick 数，0 表示立即进入 work_list。
 * 可从 IRQ 调用；irqsave raw lock 与 timer、worker、mod/cancel 串行，不睡眠。
 * 返回 true 表示本次从 idle 变为 pending；false 表示 timer/即时 work 已 pending，或
 * canceling 窗口阻止入队。函数不取得引用计数，异步存储生命周期仍由调用者负责。
 */
bool kthread_queue_delayed_work(struct kthread_worker *worker,
				struct kthread_delayed_work *dwork,
				unsigned long delay)
{
	/* work 是 dwork 的内嵌视图；flags/ret 分别保存 IRQ 状态和本次发布结果。 */
	struct kthread_work *work = &dwork->work;
	unsigned long flags;
	bool ret = false;

	/* 检查空闲与发布必须在同一 irqsave 临界区，timer 回调不能插入竞争窗口。 */
	raw_spin_lock_irqsave(&worker->lock, flags);

	if (!queuing_blocked(worker, work)) {
		__kthread_queue_delayed_work(worker, dwork, delay);
		ret = true;
	}

	raw_spin_unlock_irqrestore(&worker->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(kthread_queue_delayed_work);

/*
 * kthread_flush_work 表示一次栈上 FIFO 屏障：work 被插到目标之后，worker 执行它时
 * complete(done)，使等待者确认目标已越过当前执行轮次。对象只在等待者栈帧内存活，
 * completion 保证回调不再使用它后函数才返回。
 */
struct kthread_flush_work {
	struct kthread_work	work;
	struct completion	done;
};

/*
 * kthread_flush_work_fn() - 执行栈上 flush 屏障并唤醒等待者。
 * @work: 嵌在仍存活 kthread_flush_work 中的输入借用；worker 串行调用且不持其 raw lock。
 * 返回：无直接返回值；complete() 是唯一副作用。回调不睡眠、不保留栈上指针。
 */
static void kthread_flush_work_fn(struct kthread_work *work)
{
	/* container_of 只恢复仍在等待者栈上的屏障对象；completion 结束其异步借用期。 */
	struct kthread_flush_work *fwork =
		container_of(work, struct kthread_flush_work, work);
	complete(&fwork->done);
}

/**
 * kthread_flush_work - flush a kthread_work
 * @work: work to flush
 *
 * If @work is queued or executing, wait for it to finish execution.
 */
/*
 * kthread_flush_work() - 等待指定 work 在调用时已观察到的排队或执行轮次完成。
 * @work: 输入借用；其上次 worker 和 work 容器必须覆盖本次等待，worker 不得并发销毁。
 * 只能在可睡眠进程上下文调用。锁内若 work pending，就把栈上屏障插在其后；若正执行，
 * 插到队首；已经 idle 则直接返回。返回：无直接返回值。非空路径在 completion 后保证
 * 目标轮次已结束，但不阻止回调完成后或其他 CPU 随后再次入队。
 */
void kthread_flush_work(struct kthread_work *work)
{
	/*
	 * 变量地图：fwork 是当前栈上的 FIFO 屏障，等待完成前不得离开；worker 是 work 历史
	 * 归属借用；noop 表示目标已 idle，不需要把 fwork 暴露给异步 worker。
	 */
	struct kthread_flush_work fwork = {
		KTHREAD_WORK_INIT(fwork.work, kthread_flush_work_fn),
		COMPLETION_INITIALIZER_ONSTACK(fwork.done),
	};
	struct kthread_worker *worker;
	bool noop = false;

	/* 快速路径：从未关联过 worker 的 work 不可能正在该 API 的队列或回调中。 */
	worker = work->worker;
	if (!worker)
		return;

	/* 阶段 1：锁内取得调用时快照，并把屏障精确插到目标之后或当前队首。 */
	raw_spin_lock_irq(&worker->lock);
	/* Work must not be used with >1 worker, see kthread_queue_work(). */
	/* work 的持久归属必须仍是该 worker，否则这里会在错误锁下检查/插入链表。 */
	WARN_ON_ONCE(work->worker != worker);

	/* pending 插到目标 node 之后；executing 插队首；两者都让屏障在目标轮次之后执行。 */
	if (!list_empty(&work->node))
		kthread_insert_work(worker, &fwork.work, work->node.next);
	else if (worker->current_work == work)
		kthread_insert_work(worker, &fwork.work,
				    worker->work_list.next);
	else
		noop = true;

	raw_spin_unlock_irq(&worker->lock);

	/* 阶段 2：锁外睡眠，直到串行 worker 执行屏障；idle 快速路径无需等待。 */
	if (!noop)
		wait_for_completion(&fwork.done);
}
EXPORT_SYMBOL_GPL(kthread_flush_work);

/*
 * Make sure that the timer is neither set nor running and could
 * not manipulate the work list_head any longer.
 *
 * The function is called under worker->lock. The lock is temporary
 * released but the timer can't be set again in the meantime.
 */
/*
 * kthread_cancel_delayed_work_timer() - 同步保证 delayed timer 不再 pending 或执行。
 * @work: 输入输出借用，必须嵌于 delayed_work 且已关联 worker。@flags: 调用者 irqsave
 * 标志的输入输出借用；入口/出口均持 worker->lock，函数中间恢复 IRQ 并暂时放锁。
 * timer_delete_sync() 必须在放开 worker 锁后等待，否则会与取同一锁的 timer 回调
 * 死锁。timer 为 irqsafe，因此该同步可从 IRQ 路径使用，但可能等待正在运行
 * 的回调，不能持有回调还会获取的其他锁。canceling 在放锁窗口阻止 queue/mod 重装。
 * 返回：无直接返回值；出口重新持锁，timer 已静止，work 的 node 仍由调用者处理。
 */
static void kthread_cancel_delayed_work_timer(struct kthread_work *work,
					      unsigned long *flags)
{
	/* dwork 从内嵌 work 恢复；worker 是历史归属借用，放锁期间由 canceling 协议稳定。 */
	struct kthread_delayed_work *dwork =
		container_of(work, struct kthread_delayed_work, work);
	struct kthread_worker *worker = work->worker;

	/*
	 * timer_delete_sync() must be called to make sure that the timer
	 * callback is not running. The lock must be temporary released
	 * to avoid a deadlock with the callback. In the meantime,
	 * any queuing is blocked by setting the canceling counter.
	 */
	/*
	 * 必须同步删除 timer，才能保证回调已结束；回调也取 worker->lock，因此等待前暂时
	 * 放锁。放锁前增加 canceling，在此期间所有 queue/mod 都会被拒绝，timer 无法重装。
	 */
	/* canceling 是允许嵌套取消者的计数而非 bool；任一非零值都封锁重新入队。 */
	work->canceling++;
	/* 放锁等待 timer 回调前已置 canceling，queue/mod 在窗口内不能重新发布此 work。 */
	raw_spin_unlock_irqrestore(&worker->lock, *flags);
	timer_delete_sync(&dwork->timer);
	raw_spin_lock_irqsave(&worker->lock, *flags);
	work->canceling--;
}

/*
 * This function removes the work from the worker queue.
 *
 * It is called under worker->lock. The caller must make sure that
 * the timer used by delayed work is not running, e.g. by calling
 * kthread_cancel_delayed_work_timer().
 *
 * The work might still be in use when this function finishes. See the
 * current_work proceed by the worker.
 *
 * Return: %true if @work was pending and successfully canceled,
 *	%false if @work was not pending
 */
/*
 * __kthread_cancel_work() - 在 worker 锁内摘除尚未执行的 work。
 * @work: 输入输出借用；调用者持 work->worker->lock。若为 delayed work，调用者必须先
 * 停止 timer，防止回调并发移动同一 node。不可睡眠，不处理 current_work。
 * 返回 true 表示 node 曾位于 work_list/delayed_work_list 且已 list_del_init；false
 * 表示 work 已 idle 或正在执行。返回后容器仍可能被 worker 回调使用。
 */
static bool __kthread_cancel_work(struct kthread_work *work)
{
	/*
	 * Try to remove the work from a worker list. It might either
	 * be from worker->work_list or from worker->delayed_work_list.
	 */
	/* node 非空无法区分两条队列，但二者都受同一 worker 锁保护，可统一摘除并重新初始化。 */
	if (!list_empty(&work->node)) {
		/* node 非空统一代表即时或延迟 pending；摘链并重置为空闲哨兵。 */
		list_del_init(&work->node);
		return true;
	}

	return false;
}

/**
 * kthread_mod_delayed_work - modify delay of or queue a kthread delayed work
 * @worker: kthread worker to use
 * @dwork: kthread delayed work to queue
 * @delay: number of jiffies to wait before queuing
 *
 * If @dwork is idle, equivalent to kthread_queue_delayed_work(). Otherwise,
 * modify @dwork's timer so that it expires after @delay. If @delay is zero,
 * @work is guaranteed to be queued immediately.
 *
 * Return: %false if @dwork was idle and queued, %true otherwise.
 *
 * A special case is when the work is being canceled in parallel.
 * It might be caused either by the real kthread_cancel_delayed_work_sync()
 * or yet another kthread_mod_delayed_work() call. We let the other command
 * win and return %true here. The return value can be used for reference
 * counting and the number of queued works stays the same. Anyway, the caller
 * is supposed to synchronize these operations a reasonable way.
 *
 * This function is safe to call from any context including IRQ handler.
 * See __kthread_cancel_work() and kthread_delayed_work_timer_fn()
 * for details.
 */
/*
 * kthread_mod_delayed_work() - 首次排队或修改 delayed work 的到期时间。
 * @worker: 输入输出借用，必须是 dwork 的唯一归属。@dwork: 输入输出借用且容器覆盖
 * 异步期。@delay: 新延迟 tick 数，0 保证立即进入 work_list。接口可从 IRQ 调用；
 * raw lock 路径不可睡眠，但同步删 timer 会临时恢复 IRQ/放锁并等待回调，调用约束由
 * irqsafe timer 协议保证。返回 false 仅表示此前 idle、本次新增 pending；true 表示
 * 此前已 pending，或并发 cancel/mod 获胜而 pending 数未改变。调用者用该值维护外部
 * 引用时，仍须在更高层合理串行多方命令。
 */
bool kthread_mod_delayed_work(struct kthread_worker *worker,
			      struct kthread_delayed_work *dwork,
			      unsigned long delay)
{
	/*
	 * 变量地图：work 是 dwork 内嵌的即时工作视图；flags 保存 irqsave 状态；ret 表示
	 * 调用前是否已 pending，并在并发 cancel 获胜时保持 true 以维持外部引用账目。
	 */
	struct kthread_work *work = &dwork->work;
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&worker->lock, flags);

	/* Do not bother with canceling when never queued. */
	/* worker==NULL 表示从未发布，不存在 timer/node，直接进入首次排队。 */
	if (!work->worker) {
		ret = false;
		goto fast_queue;
	}

	/* Work must not be used with >1 worker, see kthread_queue_work() */
	/* delayed work 不能跨 worker 修改，否则 timer 回调与本路径会使用不同的锁。 */
	WARN_ON_ONCE(work->worker != worker);

	/*
	 * Temporary cancel the work but do not fight with another command
	 * that is canceling the work as well.
	 *
	 * It is a bit tricky because of possible races with another
	 * mod_delayed_work() and cancel_delayed_work() callers.
	 *
	 * The timer must be canceled first because worker->lock is released
	 * when doing so. But the work can be removed from the queue (list)
	 * only when it can be queued again so that the return value can
	 * be used for reference counting.
	 */
	/*
	 * 这里先停 timer，因为同步等待会放锁；只有确认没有另一 canceler 占有窗口后才摘链
	 * 并立刻按新 delay 重排。这样返回值仍能表示 pending 数是否改变，供外部引用计数使用。
	 */
	kthread_cancel_delayed_work_timer(work, &flags);
	if (work->canceling) {
		/* The number of works in the queue does not change. */
		/* 并发取消/修改获胜，本调用放弃重排，外部引用计数不应改变。 */
		ret = true;
		goto out;
	}
	ret = __kthread_cancel_work(work);

fast_queue:
	__kthread_queue_delayed_work(worker, dwork, delay);
out:
	raw_spin_unlock_irqrestore(&worker->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(kthread_mod_delayed_work);

/*
 * __kthread_cancel_work_sync() - 摘除 pending work 并等待正在执行的轮次结束。
 * @work: 输入输出借用；其历史 worker 必须覆盖整个调用。@is_dwork: true 表示 work
 * 嵌在 delayed_work 中，须先同步删除 timer；false 时调用者保证不存在 timer。
 * 只能在可睡眠上下文。函数先持 raw lock 摘链；若 current_work 命中，增加 canceling、
 * 放锁并用 flush 屏障等待，阻止回调自重排或他方入队穿过等待窗口。
 * 返回 true 仅表示入口时从某链表摘除了 pending node；false 也可能等待过执行中的回调。
 * 返回时 work 不再 pending/执行，但对象及 worker ownership 仍属于调用者。
 */
static bool __kthread_cancel_work_sync(struct kthread_work *work, bool is_dwork)
{
	/*
	 * 变量地图：worker 是 work 的历史归属借用，调用者保证其存活；flags 保存跨越
	 * timer 同步和 flush 放锁窗口的 IRQ 状态；ret 只记录是否从链表摘过 pending node。
	 */
	struct kthread_worker *worker = work->worker;
	unsigned long flags;
	int ret = false;

	/* 快速路径：从未归属 worker 的对象既无 timer/node，也不可能是 current_work。 */
	if (!worker)
		goto out;

	raw_spin_lock_irqsave(&worker->lock, flags);
	/* Work must not be used with >1 worker, see kthread_queue_work(). */
	/* 同步取消必须锁住 work 的历史 worker；跨 worker 复用会破坏 node/current_work 判断。 */
	WARN_ON_ONCE(work->worker != worker);

	/* 阶段 1：delayed work 必须先让 timer 静止，再统一摘除其即时或延迟链节点。 */
	if (is_dwork)
		kthread_cancel_delayed_work_timer(work, &flags);

	ret = __kthread_cancel_work(work);

	/* 阶段 2：未执行即可锁内结束；命中 current_work 才进入放锁等待慢路径。 */
	if (worker->current_work != work)
		goto out_fast;

	/*
	 * The work is in progress and we need to wait with the lock released.
	 * In the meantime, block any queuing by setting the canceling counter.
	 */
	/* work 正执行时必须放锁等待；先增加 canceling，阻止回调自重排或其他 CPU 重新入队。 */
	work->canceling++;
	/* 等待执行不能持 raw lock；计数先封住自重排和其他入队，再用 flush 越过回调。 */
	raw_spin_unlock_irqrestore(&worker->lock, flags);
	kthread_flush_work(work);
	raw_spin_lock_irqsave(&worker->lock, flags);
	work->canceling--;

out_fast:
	raw_spin_unlock_irqrestore(&worker->lock, flags);
out:
	return ret;
}

/**
 * kthread_cancel_work_sync - cancel a kthread work and wait for it to finish
 * @work: the kthread work to cancel
 *
 * Cancel @work and wait for its execution to finish.  This function
 * can be used even if the work re-queues itself. On return from this
 * function, @work is guaranteed to be not pending or executing on any CPU.
 *
 * kthread_cancel_work_sync(&delayed_work->work) must not be used for
 * delayed_work's. Use kthread_cancel_delayed_work_sync() instead.
 *
 * The caller must ensure that the worker on which @work was last
 * queued can't be destroyed before this function returns.
 *
 * Return: %true if @work was pending, %false otherwise.
 */
/*
 * kthread_cancel_work_sync() - 同步取消普通 work 并等待执行结束。
 * @work: 输入输出借用；其历史 worker 和容器须覆盖调用期，worker 不得并发销毁。
 * 不能用于 delayed_work->work，因为它不会同步删除 timer。只能在可睡眠上下文。
 * 返回 true 表示曾从队列摘除 pending node，false 表示当时未排队；无论返回值如何，
 * 出口都保证 work 不再 pending 或执行，包括会自重排的回调。ownership 不变。
 */
bool kthread_cancel_work_sync(struct kthread_work *work)
{
	/* 普通 work 的单阶段包装明确关闭 timer 同步分支。 */
	return __kthread_cancel_work_sync(work, false);
}
EXPORT_SYMBOL_GPL(kthread_cancel_work_sync);

/**
 * kthread_cancel_delayed_work_sync - cancel a kthread delayed work and
 *	wait for it to finish.
 * @dwork: the kthread delayed work to cancel
 *
 * This is kthread_cancel_work_sync() for delayed works.
 *
 * Return: %true if @dwork was pending, %false otherwise.
 */
/*
 * kthread_cancel_delayed_work_sync() - 同步取消 delayed work 的 timer、队列和执行。
 * @dwork: 输入输出借用；其 worker 与容器覆盖调用期。只能在可睡眠进程上下文。
 * 返回 true 表示 work 曾在即时/延迟链 pending，false 表示未从链表摘除；返回时即使
 * 原先正在执行或会自重排，也保证 timer 静止且 work 不再 pending/执行。ownership 不变。
 */
bool kthread_cancel_delayed_work_sync(struct kthread_delayed_work *dwork)
{
	/* delayed 包装把内嵌 work 交给核心，并开启 timer 静止步骤。 */
	return __kthread_cancel_work_sync(&dwork->work, true);
}
EXPORT_SYMBOL_GPL(kthread_cancel_delayed_work_sync);

/**
 * kthread_flush_worker - flush all current works on a kthread_worker
 * @worker: worker to flush
 *
 * Wait until all currently executing or pending works on @worker are
 * finished.
 */
/*
 * kthread_flush_worker() - 等待 worker 在调用时已有的执行中和排队工作全部越过屏障。
 * @worker: 输入借用，worker task 与对象必须覆盖等待期；调用者应阻止销毁。仅可在可
 * 睡眠进程上下文。函数把栈上 flush work 排到即时队尾并等待 completion。
 * 返回：无直接返回值。屏障前工作均已完成；并发在屏障后新入队的工作不在保证范围，
 * 因而要获得静止队列，调用者必须先停止生产者。无 worker ownership 变化。
 */
void kthread_flush_worker(struct kthread_worker *worker)
{
	/* fwork 是仅存活到 completion 的栈上 FIFO 屏障，worker 不取得其持久 ownership。 */
	struct kthread_flush_work fwork = {
		KTHREAD_WORK_INIT(fwork.work, kthread_flush_work_fn),
		COMPLETION_INITIALIZER_ONSTACK(fwork.done),
	};

	/* 栈上屏障排在当前即时队尾；等待保证 worker 不再持有该栈对象后才返回。 */
	kthread_queue_work(worker, &fwork.work);
	wait_for_completion(&fwork.done);
}
EXPORT_SYMBOL_GPL(kthread_flush_worker);

/**
 * kthread_destroy_worker - destroy a kthread worker
 * @worker: worker to be destroyed
 *
 * Flush and destroy @worker.  The simple flush is enough because the kthread
 * worker API is used only in trivial scenarios.  There are no multi-step state
 * machines needed.
 *
 * Note that this function is not responsible for handling delayed work, so
 * caller should be responsible for queuing or canceling all delayed work items
 * before invoke this function.
 */
/*
 * kthread_destroy_worker() - 停止并释放由 create_worker API 返回的 worker。
 * @worker: 输入输出拥有指针；成功路径消耗调用者 ownership。入口前调用者必须阻止新
 * 入队，并已取消或转移全部 delayed_work；本函数只 flush 当前即时队列。
 * 只能在可睡眠进程上下文。阶段为 flush FIFO → kthread_stop(task) → 检查两队列为空
 * → kfree(worker)。返回：无直接返回值；task 为 NULL 时告警并保留 worker ownership；
 * 正常返回后 worker/task 均不可再访问。遗留队列告警意味着调用者违反生命周期协议。
 */
void kthread_destroy_worker(struct kthread_worker *worker)
{
	/* task 是销毁开始时对 worker->task 的借用快照；stop 期间由 kthread_stop 临时引用稳定。 */
	struct task_struct *task;

	/* 阶段 1：没有活执行 task 表示对象不符合 create_worker/destroy 配对，拒绝释放。 */
	task = worker->task;
	if (WARN_ON(!task))
		return;

	/* 阶段 2：先越过所有即时 work，再永久停止 task；生产者必须已由调用者封住。 */
	kthread_flush_worker(worker);
	kthread_stop(task);
	/* 阶段 3：延迟/即时链均应为空；告警后仍释放，违约调用者不能再访问 worker。 */
	WARN_ON(!list_empty(&worker->delayed_work_list));
	WARN_ON(!list_empty(&worker->work_list));
	kfree(worker);
}
EXPORT_SYMBOL(kthread_destroy_worker);

/**
 * kthread_use_mm - make the calling kthread operate on an address space
 * @mm: address space to operate on
 */
/*
 * kthread_use_mm() - 让 current kthread 临时以普通 mm 用户身份访问地址空间。
 * @mm: 输入借用的有效地址空间，不可为 NULL；函数 mmgrab() 取得一份真实 mm 引用，
 * 调用者仍拥有原引用，并须随后以同一指针调用 kthread_unuse_mm()。
 * current 必须 PF_KTHREAD 且 current->mm==NULL；进程上下文中取得 task_lock，短暂关闭
 * 本地 IRQ 并切换页表。返回：无直接返回值。成功后 current->mm/active_mm 指向 @mm，
 * 可访问用户地址；旧 lazy active_mm 引用已释放。mmdrop_lazy_tlb() 同时提供 expedited
 * membarrier 所需发布屏障，保证 mm 可见先于随后的用户内存访问。
 */
void kthread_use_mm(struct mm_struct *mm)
{
	/* active_mm 保存切换前的 lazy-TLB mm 并在末尾释放；tsk 是 current 的借用别名。 */
	struct mm_struct *active_mm;
	struct task_struct *tsk = current;

	/* 阶段 1：验证只能从当前无真实 mm 的 kthread 进入；WARN 仅诊断，不替代契约。 */
	WARN_ON_ONCE(!(tsk->flags & PF_KTHREAD));
	WARN_ON_ONCE(tsk->mm);

	/*
	 * It is possible for mm to be the same as tsk->active_mm, but
	 * we must still mmgrab(mm) and mmdrop_lazy_tlb(active_mm),
	 * because these references are not equivalent.
	 */
	/*
	 * 即使两个指针相同，active_mm 的 lazy-TLB 引用与 mm 的真实用户引用也不是同一
	 * ownership：必须先取得真实引用，并在切换后释放旧 lazy 引用，不能按指针相等跳过。
	 */
	/* 阶段 2：先取得真实 mm 引用，确保页表和 mm_struct 覆盖整个安装窗口。 */
	mmgrab(mm);

	/* 阶段 3：task_lock 与 IRQ-off 窗口共同发布 task 字段并切换架构页表状态。 */
	task_lock(tsk);
	/* Hold off tlb flush IPIs while switching mm's */
	/* 关本地 IRQ，避免页表寄存器与 task 字段尚未一致时插入 TLB flush IPI。 */
	local_irq_disable();
	active_mm = tsk->active_mm;
	tsk->active_mm = mm;
	tsk->mm = mm;
	membarrier_update_current_mm(mm);
	switch_mm_irqs_off(active_mm, mm, tsk);
	local_irq_enable();
	task_unlock(tsk);
#ifdef finish_arch_post_lock_switch
	finish_arch_post_lock_switch();
#endif

	/*
	 * When a kthread starts operating on an address space, the loop
	 * in membarrier_{private,global}_expedited() may not observe
	 * that tsk->mm, and not issue an IPI. Membarrier requires a
	 * memory barrier after storing to tsk->mm, before accessing
	 * user-space memory. A full memory barrier for membarrier
	 * {PRIVATE,GLOBAL}_EXPEDITED is implicitly provided by
	 * mmdrop_lazy_tlb().
	 */
	/*
	 * expedited membarrier 扫描可能在 tsk->mm 发布前漏过 current；这里隐含的全屏障
	 * 保证扫描者若没观察到 mm，则用户访问尚未越过屏障，仍满足 membarrier 契约。
	 */
	/* 阶段 4：释放旧 lazy 引用；其全屏障同时封闭 membarrier 的漏观察窗口。 */
	mmdrop_lazy_tlb(active_mm);
}
EXPORT_SYMBOL_GPL(kthread_use_mm);

/**
 * kthread_unuse_mm - reverse the effect of kthread_use_mm()
 * @mm: address space to operate on
 */
/*
 * kthread_unuse_mm() - 撤销 kthread_use_mm()，恢复无用户 mm 的 lazy-TLB 状态。
 * @mm: 输入借用且必须与 use 时相同；函数把真实引用转换为 active_mm 的 lazy 引用，
 * 最后 mmdrop() 释放 use 取得的真实引用。current 必须为 PF_KTHREAD 且 current->mm
 * 非 NULL。取得 task_lock 并短暂关 IRQ；返回：无直接返回值。成功后 current->mm 为
 * NULL，active_mm 仍暂指 @mm 供 lazy TLB 使用。清空前的全屏障保证先前用户访问不会
 * 被重排到 mm=NULL 之后，从而与 expedited membarrier 扫描正确配对。
 */
void kthread_unuse_mm(struct mm_struct *mm)
{
	/* tsk 是 current 的借用别名；@mm 同时是当前真实 mm 和 active_mm 的 lazy-TLB 对象。 */
	struct task_struct *tsk = current;

	/* 阶段 1：验证 use/unuse 配对；传入 @mm 必须正是当前已安装对象。 */
	WARN_ON_ONCE(!(tsk->flags & PF_KTHREAD));
	WARN_ON_ONCE(!tsk->mm);

	/* 阶段 2：锁内先建立用户访问完成屏障，再把真实 mm 身份撤销为 NULL。 */
	task_lock(tsk);
	/*
	 * When a kthread stops operating on an address space, the loop
	 * in membarrier_{private,global}_expedited() may not observe
	 * that tsk->mm, and not issue an IPI. Membarrier requires a
	 * memory barrier after accessing user-space memory, before
	 * clearing tsk->mm.
	 */
	/* 先完成所有用户内存访问，再发布 mm=NULL；否则 membarrier 可能漏发 IPI。 */
	smp_mb__after_spinlock();
	/* 全屏障保证此前用户访问先于 mm=NULL，避免 expedited membarrier 漏发 IPI。 */
	local_irq_disable();
	tsk->mm = NULL;
	membarrier_update_current_mm(NULL);
	/* 在释放真实引用前取得 lazy-TLB 引用，保证同一 mm 在 ownership 转换中不断档。 */
	mmgrab_lazy_tlb(mm);
	/* active_mm is still 'mm' */
	/* 真实 mm 所有权已撤销，active_mm 仍以 lazy-TLB 角色借用，等待以后调度切换。 */
	enter_lazy_tlb(mm, tsk);
	local_irq_enable();
	task_unlock(tsk);

	/* 阶段 3：字段和架构状态均已切换，最后释放 use_mm() 取得的真实引用。 */
	mmdrop(mm);
}
EXPORT_SYMBOL_GPL(kthread_unuse_mm);

#ifdef CONFIG_BLK_CGROUP
/**
 * kthread_associate_blkcg - associate blkcg to current kthread
 * @css: the cgroup info
 *
 * Current thread must be a kthread. The thread is running jobs on behalf of
 * other threads. In some cases, we expect the jobs attach cgroup info of
 * original threads instead of that of current thread. This function stores
 * original thread's cgroup info in current kthread context for later
 * retrieval.
 */
/*
 * kthread_associate_blkcg() - 为代表其他任务执行 I/O 的 current 保存原任务 blkcg。
 * @css: 纯输入借用的 blkcg cgroup_subsys_state，可为 NULL 表示解除；安装时 css_get()
 * 取得一份由 kthread 私有块持有的引用，替换/解除时 css_put() 释放旧引用。
 * 仅 CONFIG_BLK_CGROUP 存在；普通 task 或控制块缺失时无操作。不取本文件锁、不睡眠，
 * 约定由 current 自身串行调用。返回：无直接返回值；成功后后续 I/O 可经
 * kthread_blkcg() 取得关联 css。退出前必须解除，否则 free_kthread_struct() 会告警。
 */
void kthread_associate_blkcg(struct cgroup_subsys_state *css)
{
	/* kthread 是 current 控制块的可空借用，用来持有被替换 css 的引用。 */
	struct kthread *kthread;

	/* 快速路径：普通 task 或早期控制块缺失时没有保存关联的位置。 */
	if (!(current->flags & PF_KTHREAD))
		return;
	kthread = to_kthread(current);
	if (!kthread)
		return;

	/* 阶段 1：先释放旧 css 引用并清空发布槽，确保替换过程没有重复 ownership。 */
	if (kthread->blkcg_css) {
		css_put(kthread->blkcg_css);
		kthread->blkcg_css = NULL;
	}
	/* 阶段 2：新关联先 css_get 再发布指针；NULL 输入到此即完成解除。 */
	if (css) {
		css_get(css);
		kthread->blkcg_css = css;
	}
}
EXPORT_SYMBOL(kthread_associate_blkcg);

/**
 * kthread_blkcg - get associated blkcg css of current kthread
 *
 * Current thread must be a kthread.
 */
/*
 * kthread_blkcg() - 查询 current 为代理 I/O 保存的 blkcg css。
 * 入参：无。仅 CONFIG_BLK_CGROUP 存在；不取锁、不睡眠。current 不是 kthread、控制块
 * 缺失或未关联时返回 NULL；否则返回私有块所持 css 的借用指针，不增加引用。
 * 返回指针只在 current 未调用 associate(NULL/其他 css) 的串行作用域内稳定；若需跨越
 * 该边界，调用者必须自行 css_get()。无其他可观察副作用。
 */
struct cgroup_subsys_state *kthread_blkcg(void)
{
	/* kthread 是 current 控制块的可空借用，不延长返回 css 的生命周期。 */
	struct kthread *kthread;

	/* 简单查询只在 current 自身串行域读取；普通 task 或空控制块统一返回 NULL。 */
	if (current->flags & PF_KTHREAD) {
		kthread = to_kthread(current);
		if (kthread)
			return kthread->blkcg_css;
	}
	return NULL;
}
#endif
