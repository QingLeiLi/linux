/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KTHREAD_H
#define _LINUX_KTHREAD_H
/* Simple interface for creating and stopping kernel threads without mess. */
/*
 * 本头文件定义 kthread 的公开创建、启动、停驻、停止、worker、临时 mm 与 blkcg 接口。
 * 这里只公开 task/worker 的外部契约；每 task 的 struct kthread 保持不透明，具体状态机
 * 位于 kernel/kthread.c。调用者尤其要区分“创建但未唤醒”、可恢复 park 和永久 stop。
 */
#include <linux/err.h>
#include <linux/sched.h>

struct mm_struct;

/* opaque kthread data */
/* 每个 PF_KTHREAD task 的私有控制块；调用者只能通过本接口查询，不能依赖内部布局。 */
struct kthread;

/*
 * When "(p->flags & PF_KTHREAD)" is set the task is a kthread and will
 * always remain a kthread.  For kthreads p->worker_private always
 * points to a struct kthread.  For tasks that are not kthreads
 * p->worker_private is used to point to other things.
 *
 * Return NULL for any task that is not a kthread.
 */
/*
 * tsk_is_kthread() - 容错识别 task 并取得不透明 kthread 控制块。
 * @p: 纯输入借用，不可为 NULL；调用者须保证 task 可解引用。任意上下文可调用，
 * 不取锁、不睡眠。PF_KTHREAD 一旦设置不会撤销；返回 worker_private 的借用快照，
 * 普通 task 返回 NULL。返回值不增加 task/控制块引用，最终释放并发仍由调用者排除。
 */
static inline struct kthread *tsk_is_kthread(struct task_struct *p)
{
	/* PF_KTHREAD 是判别 worker_private 当前含义的类型标签，不能只凭指针非空推断。 */
	if (p->flags & PF_KTHREAD)
		return p->worker_private;
	return NULL;
}

/*
 * kthread_create_on_node() - 在指定 NUMA 节点创建尚未启动的内核线程。
 * @threadfn: 非空回调借用，返回 int 作为 stop 结果。@data: 可空回调实参借用。
 * @node: NUMA 节点或 NUMA_NO_NODE。@namefmt/...: printf 名称及参数，仅调用期读取。
 * 可睡眠；成功返回未运行 threadfn 的 task 借用，失败返回 ERR_PTR(-ENOMEM/-EINTR)。
 * 调用者通常先设置亲和性，再 wake_up_process()；异步 data/代码须覆盖线程生命期。
 */
__printf(4, 5)
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data,
					   int node,
					   const char namefmt[], ...);

/**
 * kthread_create - create a kthread on the current node
 * @threadfn: the function to run in the thread
 * @data: data pointer for @threadfn()
 * @namefmt: printf-style format string for the thread name
 * @arg: arguments for @namefmt.
 *
 * This macro will create a kthread on the current node, leaving it in
 * the stopped state.  This is just a helper for kthread_create_on_node();
 * see the documentation there for more details.
 */
/*
 * kthread_create() - 以 NUMA_NO_NODE 包装 kthread_create_on_node()。
 * 参数、可睡眠性、错误指针和 ownership 与核心接口相同；宏只补默认节点，不启动线程。
 * GNU 可变参数的 ##arg 允许调用者在格式串没有额外占位参数时省略尾随实参。
 */
#define kthread_create(threadfn, data, namefmt, arg...) \
	kthread_create_on_node(threadfn, data, NUMA_NO_NODE, namefmt, ##arg)


/*
 * kthread_create_on_cpu() - 按 CPU 所属节点创建并硬绑定尚未启动的线程。
 * @threadfn/@data: 回调及其可空借用实参。@cpu: possible CPU 编号，可暂时 offline。
 * @namefmt: 接收 CPU 编号的名称格式。可睡眠；成功返回待唤醒 task，失败返回错误指针。
 * per-CPU 身份和 hotplug 重绑仍需相应管理协议，返回值不自动启动回调。
 */
struct task_struct *kthread_create_on_cpu(int (*threadfn)(void *data),
					  void *data,
					  unsigned int cpu,
					  const char *namefmt);

/*
 * get_kthread_comm() - 把 kthread 完整名称或回退 comm 复制到调用者缓冲区。
 * @buf/@buf_size: 输出缓冲区及字节容量；@tsk: 生命周期稳定的 kthread 借用。
 * 不睡眠、不转移 ownership；返回 void，目标按容量截断并保证字符串终止语义。
 */
void get_kthread_comm(char *buf, size_t buf_size, struct task_struct *tsk);
/*
 * set_kthread_struct() - 为 kernel_clone 正在构造的 PF_KTHREAD task 安装私有控制块。
 * @p: 未运行的新 task 借用；可睡眠分配。true 表示控制块已发布并由 task 拥有，
 * false 表示重复安装或 -ENOMEM，调用者继续负责 fork 回滚。普通驱动不应直接调用。
 */
bool set_kthread_struct(struct task_struct *p);

/*
 * kthread_set_per_cpu() - 发布或清除已硬绑定线程的 per-CPU 身份。
 * @k: kthread 借用；@cpu: 非负 possible CPU 表示设置，负值表示清位。不睡眠、返回 void；
 * 设置时记录 hotplug 恢复 CPU，清除时不自动改变已有 allowed mask。
 */
void kthread_set_per_cpu(struct task_struct *k, int cpu);
/*
 * kthread_is_per_cpu() - 容错查询 task 的 per-CPU 标志。
 * @k: 可为普通 task 的输入借用，不可为 NULL。任意上下文、不睡眠；返回原子状态快照，
 * 普通 task 或控制块缺失为 false，不增加任何引用。
 */
bool kthread_is_per_cpu(struct task_struct *k);

/**
 * kthread_run - create and wake a thread.
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @namefmt: printf-style name for the thread.
 *
 * Description: Convenient wrapper for kthread_create() followed by
 * wake_up_process().  Returns the kthread or ERR_PTR(-ENOMEM).
 */
/*
 * kthread_run() - 创建后立即唤醒普通 kthread 的表达式宏。
 * 参数与 kthread_create() 相同；成功值是已可并发执行 threadfn 的 task 借用，错误指针
 * 原样返回。语句表达式只求值一次内部结果；调用前必须准备好异步可见数据。
 * 补充说明：上游只列 -ENOMEM；当前创建等待还可能被致命信号中断并返回 -EINTR。
 */
#define kthread_run(threadfn, data, namefmt, ...)			   \
({									   \
	struct task_struct *__k						   \
		= kthread_create(threadfn, data, namefmt, ## __VA_ARGS__); \
	if (!IS_ERR(__k))						   \
		wake_up_process(__k);					   \
	__k;								   \
})

/**
 * kthread_run_on_cpu - create and wake a cpu bound thread.
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @cpu: The cpu on which the thread should be bound,
 * @namefmt: printf-style name for the thread. Format is restricted
 *	     to "name.*%u". Code fills in cpu number.
 *
 * Description: Convenient wrapper for kthread_create_on_cpu()
 * followed by wake_up_process().  Returns the kthread or
 * ERR_PTR(-ENOMEM).
 */
/*
 * kthread_run_on_cpu() - 创建、硬绑定并立即唤醒 CPU 线程。
 * @threadfn/@data: 回调契约。@cpu: possible CPU。@namefmt: CPU 名称格式。
 * 可睡眠；错误指针原样返回，成功返回已启动 task 借用。唤醒后不能再调用要求
 * “首次运行前”的接口，且调用者仍须自行管理 CPU hotplug。
 * 补充说明：错误类别除上游列出的 -ENOMEM 外，当前实现也可能返回 -EINTR。
 */
static inline struct task_struct *
kthread_run_on_cpu(int (*threadfn)(void *data), void *data,
			unsigned int cpu, const char *namefmt)
{
	/* p 是创建结果槽；只有非错误 task 才能发布到运行队列。 */
	struct task_struct *p;

	p = kthread_create_on_cpu(threadfn, data, cpu, namefmt);
	if (!IS_ERR(p))
		wake_up_process(p);

	return p;
}

/*
 * free_kthread_struct() - 在 task_struct 最终释放阶段回收 kthread 私有控制块。
 * @k: PF_KTHREAD task 的最终清理借用；不睡眠、返回 void。仅 task 核心释放路径调用，
 * 普通用户不得主动释放；入口要求 stop/park/亲和性及 blkcg 引用协议都已结束。
 */
void free_kthread_struct(struct task_struct *k);
/*
 * kthread_bind() - 首次唤醒前把新 kthread 硬绑定到一个 possible CPU。
 * @k: 未启动 task 借用；@cpu: 可 offline 的 possible CPU。可能等待 task inactive；
 * 返回 void，违约或失败以 WARN 报告，成功后普通亲和性修改被禁止。
 */
void kthread_bind(struct task_struct *k, unsigned int cpu);
/*
 * kthread_bind_mask() - 首次唤醒前把 kthread 硬绑定到 CPU 集合。
 * @k: 未启动 task 借用；@mask: 调用期只读 possible CPU 掩码。睡眠/失败和 ownership
 * 与单 CPU 版本相同；实现复制掩码内容，不保留 @mask 指针。
 */
void kthread_bind_mask(struct task_struct *k, const struct cpumask *mask);
/*
 * kthread_affine_preferred() - 为未启动线程安装可被隔离/hotplug 约束的软偏好。
 * @p: 未启动 task 借用；@mask: 会被复制的 possible CPU 集合。可睡眠；返回 0、
 * -ENOMEM 或 -EINVAL。成功后控制块拥有副本并参加后续全局重算，不消耗 task 引用。
 */
int kthread_affine_preferred(struct task_struct *p, const struct cpumask *mask);
/*
 * kthread_stop() - 发布永久停止请求并同步等待目标退出。
 * @k: 非 current 的活 kthread 借用；调用者须在入口稳定 task。可睡眠；返回 threadfn
 * 的 int 结果，或未首次启动时的 -EINTR。本函数临时取放 task 引用，不消耗调用者引用。
 */
int kthread_stop(struct task_struct *k);
/*
 * kthread_stop_put() - stop 后消费调用者预先持有的一份 task 引用。
 * @k: 由 get_task_struct() 获得的拥有引用；可睡眠。返回值同 kthread_stop()，返回后
 * 调用者不得继续凭该引用访问 @k。
 */
int kthread_stop_put(struct task_struct *k);
/* kthread_should_stop() - current 查询永久停止请求；不睡眠，true 后回调应尽快返回。 */
bool kthread_should_stop(void);
/* kthread_should_park() - current 查询可恢复请求；不睡眠，true 后清理并调用 parkme。 */
bool kthread_should_park(void);
/* kthread_should_stop_or_park() - 容错查询任一请求；普通 task 返回 false，不睡眠。 */
bool kthread_should_stop_or_park(void);
/*
 * kthread_freezable_should_stop() - 在 freezer 安全点冻结后重新查询 stop。
 * @was_frozen: 可空 bool 输出。仅 current kthread 的可睡眠上下文；返回 stop 快照，
 * 避免直接 try_to_freeze() 与同步 stop 形成冻结死锁。
 */
bool kthread_freezable_should_stop(bool *was_frozen);
/* kthread_func() - 容错取得 threadfn 借用；@k 可为普通 task，失败返回 NULL。 */
void *kthread_func(struct task_struct *k);
/* kthread_data() - 取得 data 借用；@k 必须是稳定 kthread，返回可空且不加引用。 */
void *kthread_data(struct task_struct *k);
/* kthread_probe_data() - nofault 探测 @k；不可访问、非 kthread 或空 data 返回 NULL。 */
void *kthread_probe_data(struct task_struct *k);
/* kthread_park() - 请求 @k 进入可恢复 TASK_PARKED；可睡眠，返回 0/-ENOSYS/-EBUSY。 */
int kthread_park(struct task_struct *k);
/* kthread_unpark() - 清除 @k 的 park 请求并异步唤醒；per-CPU 路径先恢复绑定。 */
void kthread_unpark(struct task_struct *k);
/* kthread_parkme() - current 完成 park 握手并睡眠到 unpark；仅 kthread 调用。 */
void kthread_parkme(void);
/*
 * kthread_exit() - 把结果交给 do_exit() 并永久终止 current kthread。
 * @result: long 退出值，stop 最终以 int 保存/返回；宏不返回，调用后不得再访问线程资源。
 */
#define kthread_exit(result) do_exit(result)
/* kthread_complete_and_exit() - 可选 complete 后退出 current；函数不返回。 */
void kthread_complete_and_exit(struct completion *, long) __noreturn;
/* kthreads_update_housekeeping() - 重算动态亲和性；可睡眠，返回 0/-ENOMEM/-EINVAL。 */
int kthreads_update_housekeeping(void);
/* kthread_do_exit() - do_exit 内部保存结果并摘除亲和性；仅退出核心调用。 */
void kthread_do_exit(struct kthread *, long);

/* kthreadd() - 启动期创建守护循环；@unused 未使用，永久消费请求且正常不返回。 */
int kthreadd(void *unused);
/* 启动代码发布的 kthreadd task 借用；用于唤醒创建消费者。 */
extern struct task_struct *kthreadd_task;
/* tsk_fork_get_node() - 借用 @tsk，返回当前请求节点或 NUMA_NO_NODE，不睡眠。 */
extern int tsk_fork_get_node(struct task_struct *tsk);

/*
 * Simple work processor based on kthread.
 *
 * This provides easier way to make use of kthreads.  A kthread_work
 * can be queued and flushed using queue/kthread_flush_work()
 * respectively.  Queued kthread_works are processed by a kthread
 * running kthread_worker_fn().
 */
/*
 * kthread_worker 是建立在单个 kthread 上的串行执行器：入队者可来自 IRQ/进程上下文，
 * worker task 逐个在锁外运行回调。work 容器由调用者拥有，必须覆盖排队、timer 和执行期；
 * flush/cancel 只提供同步边界，不自动释放对象。一个 work 不能并发跨两个 worker 使用。
 */
/* work 的前向声明使回调类型可以引用自身；完整字段在下方定义。 */
struct kthread_work;
/* 回调在 worker task 进程上下文执行，可睡眠，但返回时不得遗留锁、抢占或 IRQ 禁用。 */
typedef void (*kthread_work_func_t)(struct kthread_work *work);
/* kthread_delayed_work_timer_fn() - 到期后迁移 delayed work；@t 为 timer 借用。 */
void kthread_delayed_work_timer_fn(struct timer_list *t);

enum {
	/* 允许 worker 在两个 work 之间进入 freezer；回调自身必须返回到安全点。 */
	KTW_FREEZABLE		= 1 << 0,	/* freeze during suspend */
	/* KTW_FREEZABLE：系统挂起期间允许 freezer 冻结该 worker task。 */
};

/*
 * struct kthread_worker - 串行队列、执行 task 及其同步状态。
 * 初始化后由调用者拥有，destroy 才释放 create_worker 分配的对象。lock 统一保护两条
 * 队列、task/current_work 和 work->canceling；回调执行期间不持锁。
 */
struct kthread_worker {
	/* KTW_* 行为位；创建/初始化后在线程启动前稳定。 */
	unsigned int		flags;
	/* IRQ-safe raw lock，使 timer/硬中断入队与 worker/取消路径共享同一串行域。 */
	raw_spinlock_t		lock;
	/* 可立即执行 FIFO，以及等待 timer 到期的 delayed work 链表。 */
	struct list_head	work_list;
	struct list_head	delayed_work_list;
	/* 执行本队列的 task 借用；stop 出口在锁内清 NULL。 */
	struct task_struct	*task;
	/* 当前锁外执行的 work 借用；flush/cancel 在 lock 下观察，空闲时为 NULL。 */
	struct kthread_work	*current_work;
};

/*
 * struct kthread_work - 可重复排队的单个工作项。
 * 调用者拥有存储；node 非空表示即时或延迟 pending，worker 记录历史唯一归属。
 */
struct kthread_work {
	/* 同一时刻只连接 worker 的一条链；list_del_init 后表示不再 pending。 */
	struct list_head	node;
	/* 借用回调代码，必须覆盖每次执行；worker 在锁外调用。 */
	kthread_work_func_t	func;
	/* 历史/当前 worker 借用，用来选择同步锁；跨 worker 前必须重新初始化 work。 */
	struct kthread_worker	*worker;
	/* Number of canceling calls that are running at the moment. */
	/* 同步取消者计数；非零时封锁并发或回调自重排，允许多个取消窗口嵌套。 */
	int			canceling;
};

/* delayed work 把普通 work 与 irqsafe timer 绑定；两者共享容器和调用者生命周期。 */
struct kthread_delayed_work {
	/* 到期后迁入即时队列并执行的基础 work。 */
	struct kthread_work work;
	/* 非零 delay 的时间轮对象；同步取消后才可释放整个 dwork。 */
	struct timer_list timer;
};

/* KTHREAD_WORK_INIT() - 静态初始化普通 work，建立空 node 并保存回调。 */
#define KTHREAD_WORK_INIT(work, fn)	{				\
	.node = LIST_HEAD_INIT((work).node),				\
	.func = (fn),							\
	}

/* KTHREAD_DELAYED_WORK_INIT() - 静态初始化 work 与 TIMER_IRQSAFE timer。 */
#define KTHREAD_DELAYED_WORK_INIT(dwork, fn) {				\
	.work = KTHREAD_WORK_INIT((dwork).work, (fn)),			\
	.timer = __TIMER_INITIALIZER(kthread_delayed_work_timer_fn,\
				     TIMER_IRQSAFE),			\
	}

/* DEFINE_KTHREAD_WORK() - 定义并静态初始化 @work；存储期由声明作用域决定。 */
#define DEFINE_KTHREAD_WORK(work, fn)					\
	struct kthread_work work = KTHREAD_WORK_INIT(work, fn)

/* DEFINE_KTHREAD_DELAYED_WORK() - 定义并静态初始化 @dwork 延迟工作对象。 */
#define DEFINE_KTHREAD_DELAYED_WORK(dwork, fn)				\
	struct kthread_delayed_work dwork =				\
		KTHREAD_DELAYED_WORK_INIT(dwork, fn)

/*
 * __kthread_init_worker() - 清零并初始化调用者提供的 worker 及 lockdep 类。
 * @worker: 独占输出存储。@name/@key: 静态生命期的 lockdep 名称与类键借用。
 * 不分配、不睡眠、返回 void；出口 worker 队列为空且尚未关联 task。
 */
extern void __kthread_init_worker(struct kthread_worker *worker,
			const char *name, struct lock_class_key *key);

/*
 * kthread_init_worker() - 为每个宏展开点生成独立静态 lock class 并初始化 worker。
 * @worker 只求值于初始化调用；调用者独占存储且不得在 pending work 上重新初始化。
 */
#define kthread_init_worker(worker)					\
	do {								\
		static struct lock_class_key __key;			\
		__kthread_init_worker((worker), "("#worker")->lock", &__key); \
	} while (0)

/*
 * kthread_init_work() - 运行时把调用者对象重置为空闲普通 work。
 * @work: 独占输出存储；@fn: 覆盖执行期的回调借用。不睡眠；pending/executing 时重置
 * 会破坏链表和 worker 归属，跨 worker 复用前须先同步静止。
 */
#define kthread_init_work(work, fn)					\
	do {								\
		memset((work), 0, sizeof(struct kthread_work));		\
		INIT_LIST_HEAD(&(work)->node);				\
		(work)->func = (fn);					\
	} while (0)

/*
 * kthread_init_delayed_work() - 运行时初始化基础 work 与 irqsafe timer。
 * @dwork: 独占输出存储；@fn: 回调借用。timer/pending/执行均静止时才能重置。
 */
#define kthread_init_delayed_work(dwork, fn)				\
	do {								\
		kthread_init_work(&(dwork)->work, (fn));		\
		timer_setup(&(dwork)->timer,				\
			     kthread_delayed_work_timer_fn,		\
			     TIMER_IRQSAFE);				\
	} while (0)

/*
 * kthread_worker_fn() - 作为 kthread threadfn 串行消费 worker 队列。
 * @worker_ptr: 已初始化 worker 借用，覆盖 task 生命期。可睡眠；正常仅在 stop 后返回 0。
 * 回调在锁外执行，结束时必须恢复可抢占且 IRQ 开启状态。
 */
int kthread_worker_fn(void *worker_ptr);

/*
 * kthread_create_worker_on_node() - 分配 worker 及未启动 task。
 * @flags: KTW_* 位。@node: NUMA 节点或 NUMA_NO_NODE。@namefmt/...: 名称格式参数。
 * 可睡眠；成功返回调用者拥有的 worker，失败为 ERR_PTR(-ENOMEM/-EINTR)，失败无残留。
 */
__printf(3, 4)
struct kthread_worker *kthread_create_worker_on_node(unsigned int flags,
						     int node,
						     const char namefmt[], ...);

/* kthread_create_worker() - 默认节点创建包装；返回 worker 尚未唤醒。 */
#define kthread_create_worker(flags, namefmt, ...) \
	kthread_create_worker_on_node(flags, NUMA_NO_NODE, namefmt, ## __VA_ARGS__);

/**
 * kthread_run_worker - create and wake a kthread worker.
 * @flags: flags modifying the default behavior of the worker
 * @namefmt: printf-style name for the thread.
 *
 * Description: Convenient wrapper for kthread_create_worker() followed by
 * wake_up_process().  Returns the kthread_worker or ERR_PTR(-ENOMEM).
 */
/*
 * kthread_run_worker() - 创建默认节点 worker 后立即唤醒其 task。
 * @flags/@namefmt/...: 创建参数。表达式宏返回已启动且由调用者拥有的 worker，或原错误
 * 指针；成功后 work 可并发执行，销毁前须阻止生产者并处理 delayed work。
 * 补充说明：当前底层创建还可能因致命信号中断等待而返回 ERR_PTR(-EINTR)。
 */
#define kthread_run_worker(flags, namefmt, ...)					\
({										\
	struct kthread_worker *__kw						\
		= kthread_create_worker(flags, namefmt, ## __VA_ARGS__);	\
	if (!IS_ERR(__kw))							\
		wake_up_process(__kw->task);					\
	__kw;									\
})

/*
 * kthread_create_worker_on_cpu() - 创建未启动 worker 并硬绑定 task 到 @cpu。
 * @cpu: possible CPU。@flags: KTW_* 位。@namefmt: 自动接收 CPU 号的名称格式。
 * 可睡眠；成功返回调用者拥有的 worker，失败返回错误指针；hotplug 由用户管理。
 */
struct kthread_worker *
kthread_create_worker_on_cpu(int cpu, unsigned int flags,
			     const char namefmt[]);

/**
 * kthread_run_worker_on_cpu - create and wake a cpu bound kthread worker.
 * @cpu: CPU number
 * @flags: flags modifying the default behavior of the worker
 * @namefmt: printf-style name for the thread. Format is restricted
 *	     to "name.*%u". Code fills in cpu number.
 *
 * Description: Convenient wrapper for kthread_create_worker_on_cpu()
 * followed by wake_up_process().  Returns the kthread_worker or
 * ERR_PTR(-ENOMEM).
 */
/*
 * kthread_run_worker_on_cpu() - 创建 CPU-bound worker 并立即唤醒其 task。
 * 参数/错误与 create_on_cpu 相同；成功返回已启动 worker ownership，调用者最终 destroy。
 * 补充说明：除上游列出的 -ENOMEM 外，底层创建也可能返回 ERR_PTR(-EINTR)。
 */
static inline struct kthread_worker *
kthread_run_worker_on_cpu(int cpu, unsigned int flags,
			  const char namefmt[])
{
	/* kw 是创建结果槽；仅成功对象可解引用 task 并发布唤醒。 */
	struct kthread_worker *kw;

	kw = kthread_create_worker_on_cpu(cpu, flags, namefmt);
	if (!IS_ERR(kw))
		wake_up_process(kw->task);

	return kw;
}

/*
 * kthread_queue_work() - 原子地把空闲 work 发布到 worker 即时 FIFO。
 * @worker/@work: 覆盖排队和执行期的借用。可从 IRQ 调用、不睡眠；true 表示本次入队，
 * false 表示已 pending/canceling。成功不取得容器引用，调用者必须自行稳定存储。
 */
bool kthread_queue_work(struct kthread_worker *worker,
			struct kthread_work *work);

/*
 * kthread_queue_delayed_work() - 首次发布 delayed work，@delay 单位为 jiffies。
 * @worker/@dwork: 异步期稳定借用；delay=0 立即入 FIFO。可从 IRQ 调用；true 表示由 idle
 * 变 pending，false 表示 timer/work 已 pending 或取消中，不转移对象 ownership。
 */
bool kthread_queue_delayed_work(struct kthread_worker *worker,
				struct kthread_delayed_work *dwork,
				unsigned long delay);

/*
 * kthread_mod_delayed_work() - 首次排队或重设 delayed work 的相对延迟。
 * @worker/@dwork: 同一历史 worker 的稳定借用；@delay: jiffies，0 保证立即排队。
 * IRQ-safe；false 仅表示此前 idle，本次新增 pending，true 表示已有 pending 或并发命令获胜。
 */
bool kthread_mod_delayed_work(struct kthread_worker *worker,
			      struct kthread_delayed_work *dwork,
			      unsigned long delay);

/*
 * kthread_flush_work() - 等待 @work 在调用时观察到的 pending/执行轮次完成。
 * @work 及历史 worker 必须覆盖等待；可睡眠、返回 void。不阻止返回后的再次入队。
 */
void kthread_flush_work(struct kthread_work *work);
/*
 * kthread_flush_worker() - 以 FIFO 屏障等待调用时 worker 的即时工作全部越过。
 * @worker: 活 worker 借用；可睡眠。并发在屏障后入队和 delayed timer 不在保证范围。
 */
void kthread_flush_worker(struct kthread_worker *worker);

/*
 * kthread_cancel_work_sync() - 摘除普通 work 并等待正在执行的轮次结束。
 * @work: 容器及历史 worker 稳定借用；可睡眠。true 表示曾摘 pending node；无论返回值
 * 如何，出口均不再 pending/执行。不能用于 delayed_work 的内嵌 work。
 */
bool kthread_cancel_work_sync(struct kthread_work *work);
/* kthread_cancel_delayed_work_sync() - 同步停止 timer/work，返回 pending 摘除事实。 */
bool kthread_cancel_delayed_work_sync(struct kthread_delayed_work *work);

/*
 * kthread_destroy_worker() - flush、stop 并释放 create_worker 返回的 worker。
 * @worker: 输入拥有指针；可睡眠。调用前须封锁新入队并取消全部 delayed work；返回后
 * worker/task 均失效。该接口消费 ownership，无错误返回。
 */
void kthread_destroy_worker(struct kthread_worker *worker);

/* kthread_use_mm() - current 临时安装 @mm 并取得引用；可睡眠，须与 unuse 成对。 */
void kthread_use_mm(struct mm_struct *mm);
/* kthread_unuse_mm() - 撤销同一 @mm 并释放 use 引用；可睡眠，返回后 mm=NULL。 */
void kthread_unuse_mm(struct mm_struct *mm);

/* blkcg css 前向声明：具体布局由 cgroup 子系统拥有。 */
struct cgroup_subsys_state;

#ifdef CONFIG_BLK_CGROUP
/* kthread_associate_blkcg() - 替换/解除 current 的 blkcg；@css 可空并取得引用。 */
void kthread_associate_blkcg(struct cgroup_subsys_state *css);
/* kthread_blkcg() - 返回 current 所持 blkcg css 借用或 NULL；不增加引用。 */
struct cgroup_subsys_state *kthread_blkcg(void);
#else
/* kthread_associate_blkcg() - 未启用 blkcg 时为空操作；@css ownership 不变。 */
static inline void kthread_associate_blkcg(struct cgroup_subsys_state *css) { }
#endif
/* 结束本头文件 include guard；多次包含不会重复声明或定义宏。 */
#endif /* _LINUX_KTHREAD_H */
