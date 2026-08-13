// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/lockdep.c
 *
 * Runtime locking correctness validator
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2006,2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra
 *
 * this code maps all the lock dependencies as they occur in a live kernel
 * and will warn about the following classes of locking bugs:
 *
 * - lock inversion scenarios
 * - circular lock dependencies
 * - hardirq/softirq safe/unsafe locking bugs
 *
 * Bugs are reported even if the current locking scenario does not cause
 * any deadlock at this point.
 *
 * I.e. if anytime in the past two locks were taken in a different order,
 * even if it happened for another task, even if those were different
 * locks (but of the same class as this lock), this code will detect it.
 *
 * Thanks to Arjan van de Ven for coming up with the initial idea of
 * mapping lock dependencies runtime.
 */
/*
 * 本文件实现运行期锁正确性验证器。它在真实内核运行过程中记录已经发生的锁依赖，并报告锁顺序
 * 反转、环形依赖，以及 hardirq/softirq 上下文中同一锁类既安全又不安全的用法。
 *
 * 报警依据是全局累积的“锁类”关系，而不只是当前任务此刻是否已经死锁：只要历史上任一任务曾按
 * 相反顺序获取两个锁类，即使当时使用的是同类的不同锁实例，后来的观测也能把这条路径闭合成潜在
 * 环路。因此阅读本文件时要区分锁实例、锁类和跨任务保留的依赖图。
 *
 * 这套运行期绘制锁依赖图的思路由 Ingo Molnar 等人实现，最初的依赖映射构想来自
 * Arjan van de Ven；上面的署名和版权原文保持不变。
 */
/* lockdep 会进入大量低层锁路径，关闭分支插桩可避免观测代码递归回到自身并污染热路径。 */
#define DISABLE_BRANCH_PROFILING
/*
 * 调度、IRQ、RCU、栈回溯和锁类型头文件提供被观测上下文；bitmap/hash/random 等头文件支撑
 * lockdep 的固定容量池与索引。这里的 include 只建立编译依赖，不代表 lockdep 拥有这些对象。
 */
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/kallsyms.h>
#include <linux/interrupt.h>
#include <linux/stacktrace.h>
#include <linux/debug_locks.h>
#include <linux/irqflags.h>
#include <linux/utsname.h>
#include <linux/hash.h>
#include <linux/ftrace.h>
#include <linux/stringify.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/gfp.h>
#include <linux/random.h>
#include <linux/jhash.h>
#include <linux/nmi.h>
#include <linux/rcupdate.h>
#include <linux/kprobes.h>
#include <linux/lockdep.h>
#include <linux/context_tracking.h>
#include <linux/console.h>
#include <linux/kasan.h>

#include <asm/sections.h>

#include "lockdep_internals.h"
#include "lock_events.h"

#include <trace/events/lock.h>

#ifdef CONFIG_PROVE_LOCKING
/*
 * prove_locking 是依赖证明热路径的运行开关：默认开启，模块参数和后面的 sysctl 可修改它；置零后
 * 基础锁跟踪仍可能存在，但昂贵的依赖图证明路径会由调用点跳过。0644 允许特权用户在运行期写入。
 */
static int prove_locking = 1;
module_param(prove_locking, int, 0644);
#else
/* 未编译依赖证明时用常量零裁剪对应分支，不能在运行期重新打开。 */
#define prove_locking 0
#endif

#ifdef CONFIG_LOCK_STAT
/* lock_stat 控制等待/持有时间等统计采样；它默认开启，并可通过模块参数或 sysctl 动态关闭。 */
static int lock_stat = 1;
module_param(lock_stat, int, 0644);
#else
/* 未编译 LOCK_STAT 时将判断折叠为假，也不分配或更新相应统计路径。 */
#define lock_stat 0
#endif

#ifdef CONFIG_SYSCTL
/*
 * 该只读表描述 /proc/sys/kernel 下的 lockdep 运行开关。每个条目把名字映射到一个 int，限制访问
 * 长度为 sizeof(int)，并复用 proc_dointvec 完成文本与整数转换；条件编译保证表中不会引用不存在
 * 的配置变量。表本身为静态常量，注册后由 sysctl 核心读取，lockdep 不负责释放其存储。
 */
static const struct ctl_table kern_lockdep_table[] = {
#ifdef CONFIG_PROVE_LOCKING
	{
		.procname       = "prove_locking",
		.data           = &prove_locking,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec,
	},
#endif /* CONFIG_PROVE_LOCKING */
#ifdef CONFIG_LOCK_STAT
	{
		.procname       = "lock_stat",
		.data           = &lock_stat,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec,
	},
#endif /* CONFIG_LOCK_STAT */
};

/**
 * kernel_lockdep_sysctls_init - 在后期初始化阶段发布 lockdep 的 sysctl 表
 *
 * 输入：无；使用静态 kern_lockdep_table，调用时普通内核初始化环境已经可用。
 * 返回：固定返回 0，使 late_initcall 不因注册接口无返回句柄而阻断启动。
 * 副作用：把表注册到 /proc/sys/kernel；表与参数均为静态生命周期，没有调用方接管的资源。
 * 失败边界：register_sysctl_init() 在此接口中不返回可传播的错误，故本函数不能向启动框架报告
 * 单项发布失败。
 */
static __init int kernel_lockdep_sysctls_init(void)
{
	/* 发布动作只做一次；后续读写由 sysctl 核心直接访问表中 data 指向的运行开关。 */
	register_sysctl_init("kernel", kern_lockdep_table);
	return 0;
}
/* 把上述初始化函数排到 late initcall 阶段，避免早期尚未准备好的 sysctl 基础设施。 */
late_initcall(kernel_lockdep_sysctls_init);
#endif /* CONFIG_SYSCTL */

/*
 * 每 CPU 递归计数标记“本 CPU 正在执行 lockdep 内部路径”。公开导出是为了让其他 lockdep 组件使用
 * 同一隔离状态；更新者必须遵守关 IRQ/本 CPU 访问约束，非零时新的锁事件要快速退出。
 */
DEFINE_PER_CPU(unsigned int, lockdep_recursion);
EXPORT_PER_CPU_SYMBOL_GPL(lockdep_recursion);

/**
 * lockdep_enabled - 判断当前执行点是否可以进入 lockdep
 *
 * 输入：无；读取全局 debug_locks、本 CPU 的内部递归计数和当前任务的递归计数。
 * 返回：三个禁用条件均不存在时为 true，否则为 false。
 * 并发边界：本 CPU 计数只用于当前 CPU 的递归隔离；调用方仍须按各入口契约固定 CPU/IRQ 状态。
 * 副作用与所有权：纯判定，不修改状态，也不获取任何对象所有权。
 */
static __always_inline bool lockdep_enabled(void)
{
	/* 一旦严重一致性错误永久关闭 debug_locks，就不再触碰可能已不可信的依赖图。 */
	if (!debug_locks)
		return false;

	/* 防止 lockdep 自己获取内部锁时再次被锁插桩递归观测。 */
	if (this_cpu_read(lockdep_recursion))
		return false;

	/* 任务级计数覆盖跨辅助路径的递归情形，与上面的 CPU 级内部图锁隔离互补。 */
	if (current->lockdep_recursion)
		return false;

	/* 所有保护门均通过，调用方可以继续记录本次锁事件。 */
	return true;
}

/*
 * lockdep_lock: protects the lockdep graph, the hashes and the
 *               class/list/hash allocators.
 *
 * This is one of the rare exceptions where it's justified
 * to use a raw spinlock - we really dont want the spinlock
 * code to recurse back into the lockdep code...
 */
/*
 * lockdep_lock 保护依赖图、各类 hash，以及 class/list/hash 的静态分配器。这里使用 raw spinlock 是
 * 少数有意绕过普通自旋锁封装的场景：若内部锁本身再次触发 lockdep，验证器会无限递归。
 *
 * 所有调用者必须先关闭本地 IRQ；获取前先增加本 CPU 的 lockdep_recursion，使等待内部锁期间发生
 * 的任何锁插桩也立即退出。__owner 只在持有 __lock 时读写，用于验证解锁者和断言调用者确为当前
 * 任务，不承担锁本身的同步职责。
 */
/* 全局内部 raw spinlock 串行修改依赖图和固定容量池，静态初始化为未锁状态。 */
static arch_spinlock_t __lock = (arch_spinlock_t)__ARCH_SPIN_LOCK_UNLOCKED;
/* 记录当前内部图锁持有任务；NULL 表示空闲，仅作调试一致性校验，不拥有 task_struct 引用。 */
static struct task_struct *__owner;

/**
 * lockdep_lock - 获取 lockdep 内部图锁并屏蔽自身递归观测
 *
 * 输入：无；调用者必须已经关闭本地 IRQ，且之后必须由同一任务调用 lockdep_unlock() 配对。
 * 返回：无；成功后当前任务持有 __lock，__owner 指向 current。
 * 副作用：先增加本 CPU lockdep_recursion，再自旋获取全局 raw lock；等待期间也保持递归屏蔽。
 * 失败边界：IRQ 状态不合约只触发 DEBUG_LOCKS_WARN_ON，函数仍继续执行；该锁没有可返回的失败值。
 */
static inline void lockdep_lock(void)
{
	/* 内部锁不可在本地 IRQ 可重入的状态下获取，否则同 CPU 中断可能再次争用它而自锁。 */
	DEBUG_LOCKS_WARN_ON(!irqs_disabled());

	/* 必须先置递归门，再触碰 raw lock，封住自旋等待和持锁区间内的插桩入口。 */
	__this_cpu_inc(lockdep_recursion);
	arch_spin_lock(&__lock);
	/* 锁已提供互斥，最后发布调试 owner 供配对解锁和断言检查。 */
	__owner = current;
}

/**
 * lockdep_unlock - 释放当前任务持有的 lockdep 内部图锁
 *
 * 输入：无；要求本地 IRQ 仍关闭、__lock 由 current 持有，并与 lockdep_lock() 成对。
 * 返回：无；正常返回时图锁已释放，本 CPU 递归计数已减一。
 * 副作用：按“清 owner、释放 raw lock、撤销递归门”的逆序结束临界区。
 * 失败边界：debug_locks 仍开启且 owner 不匹配时报警并直接返回，避免错误任务释放别人的 raw lock；
 * 此时状态故意保持不变，严重错误的上层路径负责关闭或停止继续使用 lockdep。
 */
static inline void lockdep_unlock(void)
{
	/* 与获取侧相同，解锁完成前也不能允许本地 IRQ 重入内部图操作。 */
	DEBUG_LOCKS_WARN_ON(!irqs_disabled());

	/* owner 校验只在调试仍有效时作为硬边界；不匹配时绝不能继续执行真正的 unlock。 */
	if (debug_locks && DEBUG_LOCKS_WARN_ON(__owner != current))
		return;

	/* 先撤销 owner，再释放锁，避免下一持有者看到上一任务残留。 */
	__owner = NULL;
	arch_spin_unlock(&__lock);
	/* raw lock 已经不可访问后才重新允许本 CPU 的后续锁事件进入 lockdep。 */
	__this_cpu_dec(lockdep_recursion);
}

#ifdef CONFIG_PROVE_LOCKING
/**
 * lockdep_assert_locked - 断言当前任务持有 lockdep 内部图锁
 *
 * 输入：无；通常在需要图锁保护的内部 helper 入口调用。
 * 返回：owner 不等于 current 时返回 true 并触发调试告警，匹配时返回 false。
 * 副作用与并发：不改变锁状态；调用点应已处在图锁临界区，__owner 的读取依赖该约束。
 */
static inline bool lockdep_assert_locked(void)
{
	return DEBUG_LOCKS_WARN_ON(__owner != current);
}
#endif

/*
 * 自测期间可把统计和诊断归属到指定任务；这里只保存借用指针，不增加 task_struct 引用，设置者必须
 * 保证对象覆盖整个自测窗口。NULL 表示使用正常的 current 路径。
 */
static struct task_struct *lockdep_selftest_task_struct;


/**
 * graph_lock - 获取依赖图锁，并确认依赖图仍允许修改
 *
 * 输入：无；调用者必须已关闭本地 IRQ，并在返回 1 后用 graph_unlock() 或关闭调试的组合 helper
 * 释放锁。
 * 返回：1 表示仍持有图锁且 debug_locks 有效；0 表示发现全局调试已关闭，并已自行释放图锁。
 * 副作用：进入内部图锁临界区并累计 lockdep_lock 事件；失败返回不会把锁所有权交给调用方。
 * 并发边界：另一个 CPU 可在发现错误后先关闭 debug_locks、再放开图锁去打印；本函数拿到锁后重新
 * 检查开关，避免在该诊断窗口继续修改已经冻结的图。
 */
static int graph_lock(void)
{
	/* 先串行化图状态，再记一次内部图锁获取事件。 */
	lockdep_lock();
	lockevent_inc(lockdep_lock);
	/*
	 * Make sure that if another CPU detected a bug while
	 * walking the graph we dont change it (while the other
	 * CPU is busy printing out stuff with the graph lock
	 * dropped already)
	 */
	/*
	 * 若另一 CPU 在遍历图时发现错误并已关闭调试，就不要在它释放图锁进行打印期间继续改变图。
	 * 重新检查必须发生在拿到图锁之后，才能与关闭者的状态变更建立确定顺序。
	 */
	if (!debug_locks) {
		/* 失败路径不泄漏图锁，调用者见到 0 时也不得再次解锁。 */
		lockdep_unlock();
		return 0;
	}
	/* 图仍有效，锁所有权留给调用者。 */
	return 1;
}

/**
 * graph_unlock - 释放由 graph_lock() 成功返回后持有的依赖图锁
 *
 * 输入：无；仅可对应 graph_lock() 的返回值 1，且本地 IRQ 仍须关闭。
 * 返回与副作用：无返回值；转交给 lockdep_unlock() 清 owner、放锁并撤销递归隔离。
 */
static inline void graph_unlock(void)
{
	lockdep_unlock();
}

/*
 * Turn lock debugging off and return with 0 if it was off already,
 * and also release the graph lock:
 */
/*
 * 关闭锁调试；若此前已经关闭则返回 0，同时无条件释放当前持有的图锁。把两个动作放在同一 helper
 * 中，保证所有诊断失败路径都先冻结后续图修改，再离开临界区进行可能递归进锁代码的打印。
 */
/**
 * debug_locks_off_graph_unlock - 永久关闭锁调试并释放依赖图锁
 *
 * 输入：无；调用者必须持有图锁且本地 IRQ 关闭。
 * 返回：debug_locks_off() 的结果；非零表示本次调用完成从开启到关闭的转换，0 表示此前已关闭。
 * 副作用：全局关闭后续锁调试，然后释放图锁；返回时调用者不再拥有该锁。
 * 失败边界：即使调试早已关闭也必须执行 unlock，调用方据返回值决定是否作为首个报告者打印。
 */
static inline int debug_locks_off_graph_unlock(void)
{
	/* ret 必须跨越解锁保存，因为解锁之后只允许依据它决定是否输出一次诊断。 */
	int ret = debug_locks_off();

	/* 打印或返回上层之前先离开内部锁，避免控制台路径反向进入 lockdep。 */
	lockdep_unlock();

	return ret;
}

/* 当前从直接依赖池占用的条目数；在分配时递增、zap 清边时递减，无锁诊断读取只能视为近似值。 */
unsigned long nr_list_entries;
/* 固定容量的直接依赖边存储，避免 lockdep 在锁路径中进行可能递归或失败方式复杂的动态分配。 */
static struct lock_list list_entries[MAX_LOCKDEP_ENTRIES];
/* 位 i 表示 list_entries[i] 已占用；分配、释放和计数必须与图锁临界区保持一致。 */
static DECLARE_BITMAP(list_entries_in_use, MAX_LOCKDEP_ENTRIES);

/*
 * All data structures here are protected by the global debug_lock.
 *
 * nr_lock_classes is the number of elements of lock_classes[] that is
 * in use.
 */
/*
 * 下列数据结构由全局 lockdep 图锁保护（原文称 global debug_lock，本文件实际对象名为 __lock）。
 * nr_lock_classes 表示 lock_classes[] 中当前在用的元素数量；它不同于历史最大索引，也不同于因
 * 注销而进入 RCU 延迟回收阶段的 zapped 数量。无锁诊断读取只能视为近似值。
 */
/* key hash 使用 class 最大位数减一作为桶位数，以链表解决不同 key 落入同桶的冲突。 */
#define KEYHASH_BITS		(MAX_LOCKDEP_KEYS_BITS - 1)
/* 桶数是 2 的幂，供 keyhashentry() 用掩码友好的 hash_64() 定位。 */
#define KEYHASH_SIZE		(1UL << KEYHASH_BITS)
/* 从 lock_class_key 映射到已注册 lock_class 的 RCU hlist 桶；数组静态存活。 */
static struct hlist_head lock_keys_hash[KEYHASH_SIZE];
/* 当前已占用的 class 槽数量，由图锁保护并供容量/诊断输出。 */
unsigned long nr_lock_classes;
/* 已从公开索引摘除、等待 RCU grace period 后归还的 class 数量。 */
unsigned long nr_zapped_classes;
/* 已登记的动态 key 数量；模块卸载或显式注销时必须同步撤销。 */
unsigned long nr_dynamic_keys;
/* class 数组的扫描上界；分配更高槽时上调、回收恰好位于上界的槽时下调，中间可含空洞。 */
unsigned long max_lock_class_idx;
/* 全局固定容量锁类池；held_lock 只保存其数组索引，不直接拥有 class 生命周期。 */
struct lock_class lock_classes[MAX_LOCKDEP_KEYS];
/* 位 i 表示 lock_classes[i] 当前可被索引；清位后的槽要经过 RCU 回收流程才能复用。 */
DECLARE_BITMAP(lock_classes_in_use, MAX_LOCKDEP_KEYS);

/**
 * hlock_class - 把 held_lock 中保存的 class 索引转换为锁类指针
 * @hlock: 当前任务 held-lock 栈中的记录；调用者保留其所有权并保证记录在检查期间有效
 *
 * 返回：索引对应且仍在用的 lock_classes[] 元素；检测到未占用索引时报警并返回 NULL。
 * 并发边界：调用方必须处在能稳定 class 生命周期的 lockdep 上下文；本函数只校验 in-use 位，
 * 不获取图锁或 RCU 引用，也不能修复被破坏的 bitfield。
 * 副作用：有效路径无修改；无效索引路径触发 DEBUG_LOCKS_WARN_ON。
 */
static inline struct lock_class *hlock_class(struct held_lock *hlock)
{
	/* 先把 bitfield 快照为普通整数，后续所有校验和寻址都只使用这一次读取。 */
	unsigned int class_idx = hlock->class_idx;

	/* Don't re-read hlock->class_idx, can't use READ_ONCE() on bitfield */
	/*
	 * 不要重新读取 hlock->class_idx；位字段不能直接套用 READ_ONCE()。编译器屏障把上面的快照与
	 * 后续使用隔开，避免编译器为了寻址再次从可能并发变化或已损坏的 held_lock 位字段取值。
	 */
	barrier();

	/* 未占用槽不具备可解引用的 lock_class 生命周期，立即把损坏限制在本 helper。 */
	if (!test_bit(class_idx, lock_classes_in_use)) {
		/*
		 * Someone passed in garbage, we give up.
		 */
		/* 调用者传入了无效索引；报警后放弃转换，不能返回静态数组中的任意槽。 */
		DEBUG_LOCKS_WARN_ON(1);
		return NULL;
	}

	/*
	 * At this point, if the passed hlock->class_idx is still garbage,
	 * we just have to live with it
	 */
	/*
	 * 到这里如果最初取得的 hlock->class_idx 仍是恰好落在有效槽上的垃圾值，本函数已无额外身份信息
	 * 可区分，只能接受该结果；更高层的数据结构一致性检查负责发现后续矛盾。
	 */
	/* 数组基址加已验证索引得到借用指针，调用者不得释放或长期保存它。 */
	return lock_classes + class_idx;
}

#ifdef CONFIG_LOCK_STAT
/*
 * 每 CPU、每锁类的统计槽：索引用 lock_class 在全局数组中的下标，更新当前 CPU 槽可避免全局原子
 * 热点；读取者跨 possible CPU 汇总，所以结果是并发更新中的近似观测而非一致快照。
 */
static DEFINE_PER_CPU(struct lock_class_stats[MAX_LOCKDEP_KEYS], cpu_lock_stats);

/**
 * lockstat_clock - 获取 lockstat 时间戳
 *
 * 输入：无；由等待/持锁路径在区间起止处配对记录。
 * 返回：local_clock() 的 64 位时间值，供后续相减得到耗时；它不是墙上时间。
 * 副作用与所有权：只读时钟，不分配资源；调用者保存数值而非任何时钟对象。
 */
static inline u64 lockstat_clock(void)
{
	return local_clock();
}

/**
 * lock_point - 在固定槽数组中查找或登记一个调用点
 * @points: 长度为 LOCKSTAT_POINTS 的地址数组；零槽表示尚未使用
 * @ip: 要登记的指令地址，同一地址只占一个槽
 *
 * 返回：已有或新分配槽的下标；数组已满时返回 LOCKSTAT_POINTS，调用者据此跳过计数。
 * 副作用：首次看到 ip 时写入第一个零槽；已有或已满时不改数组。
 * 并发边界：这是低开销近似统计，函数本身不加锁；并发登记可能丢失去重精度，但不能越界写入。
 */
static int lock_point(unsigned long points[], unsigned long ip)
{
	/* i 同时是线性探测游标和返回值；走到上界即表达“没有可用槽”。 */
	int i;

	/* 按稳定槽序寻找首个空位或已登记的同一地址。 */
	for (i = 0; i < LOCKSTAT_POINTS; i++) {
		if (points[i] == 0) {
			/* 首个空槽归当前地址，随后立即停止，避免同一调用写入多个槽。 */
			points[i] = ip;
			break;
		}
		/* 已存在时复用该槽，使 per-CPU 计数能按同一数组下标聚合。 */
		if (points[i] == ip)
			break;
	}

	return i;
}

/**
 * lock_time_inc - 把一次耗时样本并入统计
 * @lt: 要原地更新的 min/max/total/nr 聚合器，由调用者拥有
 * @time: 本次等待或持锁耗时
 *
 * 返回：无；更新最大值、最小值、总和与样本数。
 * 并发边界：调用者应传入当前 CPU 的统计槽，函数不提供跨 CPU 同步；nr 为零时旧 min 没有意义。
 * 溢出边界：沿用字段的定宽算术，不做饱和处理；它服务于调试统计而非计费级精确计量。
 */
static void lock_time_inc(struct lock_time *lt, u64 time)
{
	/* 新样本超过历史上界时推进 max。 */
	if (time > lt->max)
		lt->max = time;

	/* 第一条样本无条件建立 min；之后只接受更小值。 */
	if (time < lt->min || !lt->nr)
		lt->min = time;

	/* 最后累计总耗时和样本数，使读取者可计算平均值。 */
	lt->total += time;
	lt->nr++;
}

/**
 * lock_time_add - 把一组时长聚合值合并到目标聚合器
 * @src: 只借用的源统计；函数不修改它
 * @dst: 调用者拥有的目标统计，将被原地累加
 *
 * 返回：无；空源直接返回，否则合并 min/max 并累加 total/nr。
 * 并发与溢出边界：不提供同步或饱和算术；典型调用者用它把各 CPU 近似快照汇入已清零的输出。
 */
static inline void lock_time_add(struct lock_time *src, struct lock_time *dst)
{
	/* 没有样本时 src 的 min/max 没有定义，不得参与目标边界比较。 */
	if (!src->nr)
		return;

	/* 目标最大值取两组样本上界中的较大者。 */
	if (src->max > dst->max)
		dst->max = src->max;

	/* 目标为空时直接采用源最小值，否则保留两者较小者。 */
	if (src->min < dst->min || !dst->nr)
		dst->min = src->min;

	/* 总耗时与样本数具备可加性，供最终读取者计算整体平均值。 */
	dst->total += src->total;
	dst->nr += src->nr;
}

/**
 * lock_stats - 汇总一个锁类在所有 possible CPU 上的统计
 * @class: lock_classes[] 中仍有效的锁类；仅借用，不修改其生命周期
 * @stats: 调用者提供的输出缓冲，返回时被完整覆盖
 *
 * 返回：无；输出包含各调用点计数、读写等待/持有时间以及 bounce 计数之和。
 * 并发边界：更新侧不会为本次读取停顿，因此不同 CPU、不同字段可能来自略有差异的时刻；结果用于
 * 调试展示，不是事务性快照。调用者必须保证 class 在整个数组下标换算期间有效。
 */
void lock_stats(struct lock_class *class, struct lock_class_stats *stats)
{
	/* cpu 遍历所有可能 CPU，i 在三个定长计数数组中复用。 */
	int cpu, i;

	/* 先建立空聚合器，保证首次 time merge 可用 nr==0 识别未初始化 min。 */
	memset(stats, 0, sizeof(struct lock_class_stats));
	/* 离线但可能存在历史样本的 CPU 也要纳入，因此使用 possible 而不是 online 集合。 */
	for_each_possible_cpu(cpu) {
		/* 用 class 的数组偏移定位该 CPU 上同一锁类的统计槽；pcs 只是循环内借用指针。 */
		struct lock_class_stats *pcs =
			&per_cpu(cpu_lock_stats, cpu)[class - lock_classes];

		/* IP 地址保存在 class 中，这里按对应槽汇总“发生争用的位置”次数。 */
		for (i = 0; i < ARRAY_SIZE(stats->contention_point); i++)
			stats->contention_point[i] += pcs->contention_point[i];

		/* 同理汇总“导致别人争用的获取位置”次数。 */
		for (i = 0; i < ARRAY_SIZE(stats->contending_point); i++)
			stats->contending_point[i] += pcs->contending_point[i];

		/* 等待时间按读、写语义分开，合并时保留各自全局 min/max。 */
		lock_time_add(&pcs->read_waittime, &stats->read_waittime);
		lock_time_add(&pcs->write_waittime, &stats->write_waittime);

		/* 持有时间也按读写分流，不能与等待时间混在同一分布中。 */
		lock_time_add(&pcs->read_holdtime, &stats->read_holdtime);
		lock_time_add(&pcs->write_holdtime, &stats->write_holdtime);

		/* bounce 类型已经编码获取/争用和读/写组合，逐槽求和即可。 */
		for (i = 0; i < ARRAY_SIZE(stats->bounces); i++)
			stats->bounces[i] += pcs->bounces[i];
	}
}

/**
 * clear_lock_stats - 清除一个锁类的全部 lockstat 数据
 * @class: lock_classes[] 中仍有效且由调用者稳定生命周期的锁类
 *
 * 返回：无；所有 possible CPU 的该 class 统计槽及 class 上的调用点地址数组被清零。
 * 并发边界：不与更新侧建立全局停机屏障；并发新样本可能在清零前后交错，所以语义是重新起算的
 * 尽力操作，而不是精确切割时间线。
 */
void clear_lock_stats(struct lock_class *class)
{
	/* cpu 用于把同一 class 下标投影到每个 possible CPU 的独立数组。 */
	int cpu;

	/* 逐 CPU 清除数值聚合；离线 CPU 的旧样本也不能遗留。 */
	for_each_possible_cpu(cpu) {
		/* cpu_stats 只借用当前循环对应槽，memset 不改变 class 对象本身。 */
		struct lock_class_stats *cpu_stats =
			&per_cpu(cpu_lock_stats, cpu)[class - lock_classes];

		memset(cpu_stats, 0, sizeof(struct lock_class_stats));
	}
	/* 数值槽已清后同步忘记两类 IP 映射，后续样本可从第一个空槽重新登记地址。 */
	memset(class->contention_point, 0, sizeof(class->contention_point));
	memset(class->contending_point, 0, sizeof(class->contending_point));
}

/**
 * get_lock_stats - 定位当前 CPU 上指定锁类的统计槽
 * @class: lock_classes[] 中有效的锁类
 *
 * 返回：当前 CPU 的 cpu_lock_stats[] 中与 class 下标相同的借用指针。
 * 并发边界：函数不禁止迁移；调用者必须已处于能稳定当前 CPU 的 lockdep 入口上下文，并在该上下文
 * 内完成读写。返回槽为静态 percpu 存储，调用者不得释放或跨上下文长期保存。
 */
static struct lock_class_stats *get_lock_stats(struct lock_class *class)
{
	return &this_cpu_ptr(cpu_lock_stats)[class - lock_classes];
}

/**
 * lock_release_holdtime - 在释放 held lock 时登记本次持有时长
 * @hlock: 当前任务即将释放的 held-lock 记录，须含有效 class_idx 和获取时的时间戳
 *
 * 返回：无；统计关闭时不做任何事，开启时更新当前 CPU 对应锁类的读或写持有时间。
 * 副作用：读取本地时钟并修改 percpu 统计；不改变 hlock，也不接管 class/stats 所有权。
 * 失败边界：依赖调用者保证 hlock_class() 可成功，时间差沿用无饱和的 64 位算术。
 */
static void lock_release_holdtime(struct held_lock *hlock)
{
	/* stats 是当前 CPU 槽的借用指针，holdtime 保存本次区间差值。 */
	struct lock_class_stats *stats;
	u64 holdtime;

	/* 运行开关关闭时连时钟和 class 查找成本也跳过。 */
	if (!lock_stat)
		return;

	/* 获取时保存的时间戳到当前时刻之差即本次持锁时长。 */
	holdtime = lockstat_clock() - hlock->holdtime_stamp;

	/* held_lock 的 class_idx 选择统计槽；调用方已保证记录仍合法。 */
	stats = get_lock_stats(hlock_class(hlock));
	/* read 非零表示共享/读模式，否则归入独占/写模式。 */
	if (hlock->read)
		lock_time_inc(&stats->read_holdtime, holdtime);
	else
		lock_time_inc(&stats->write_holdtime, holdtime);
}
#else
/**
 * lock_release_holdtime - 未编译 LOCK_STAT 时的零开销占位实现
 * @hlock: 为保持调用点一致而保留的参数；本配置下不会读取
 *
 * 返回与副作用：立即返回，不记录时钟、class 或 percpu 统计，也不取得任何所有权。
 */
static inline void lock_release_holdtime(struct held_lock *hlock)
{
}
#endif

/*
 * We keep a global list of all lock classes. The list is only accessed with
 * the lockdep spinlock lock held. free_lock_classes is a list with free
 * elements. These elements are linked together by the lock_entry member in
 * struct lock_class.
 */
/*
 * 所有锁类同时按状态挂入全局链表，链表只能在持有 lockdep 内部图锁时访问。free_lock_classes 保存
 * 可分配元素；两条链都复用 struct lock_class.lock_entry，故一个 class 在同一时刻只能属于其中一条
 * 状态链，移动节点就是所有权状态转换而不是复制对象。
 */
/* 已发布并在用的锁类链表；节点存储来自静态 lock_classes[] 池。 */
static LIST_HEAD(all_lock_classes);
/* 已完成初始化或 RCU 回收、可供 register_lock_class() 再分配的锁类链表。 */
static LIST_HEAD(free_lock_classes);

/**
 * struct pending_free - information about data structures about to be freed
 * @zapped: Head of a list with struct lock_class elements.
 * @lock_chains_being_freed: Bitmap that indicates which lock_chains[] elements
 *	are about to be freed.
 */
/*
 * pending_free 汇集即将回收的数据：zapped 是已从公开 class 索引摘除的 lock_class 链表头；
 * lock_chains_being_freed 的置位项表示相应 lock_chains[] 槽已摘除、但仍可能被 RCU 读者看到。
 * 两类对象只有经过 grace period 后才能重新初始化并放回空闲池。
 */
struct pending_free {
	/* 待回收 class 的私有链表头，节点仍使用 lock_entry。 */
	struct list_head zapped;
	/* 待回收 chain 下标集合，位图避免在回调前复用静态槽。 */
	DECLARE_BITMAP(lock_chains_being_freed, MAX_LOCKDEP_CHAINS);
};

/**
 * struct delayed_free - data structures used for delayed freeing
 *
 * A data structure for delayed freeing of data structures that may be
 * accessed by RCU readers at the time these were freed.
 *
 * @rcu_head:  Used to schedule an RCU callback for freeing data structures.
 * @index:     Index of @pf to which freed data structures are added.
 * @scheduled: Whether or not an RCU callback has been scheduled.
 * @pf:        Array with information about data structures about to be freed.
 */
/*
 * delayed_free 为仍可能被 RCU 读者访问的对象提供延迟回收。rcu_head 用来排队回调；index 选择当前
 * 接收新 zap 的 pf 缓冲；scheduled 记录是否已有回调在途；pf[2] 允许提交一批给 RCU 后切到另一批
 * 继续收集，而不覆盖回调尚未处理的集合。该全局实例及其两个缓冲由图锁保护。
 */
static struct delayed_free {
	/* 嵌入式回调节点，生命周期与全局 delayed_free 相同。 */
	struct rcu_head		rcu_head;
	/* 当前收集缓冲下标，只能为 0 或 1。 */
	int			index;
	/* 非零表示回收回调已经调度，避免为同一在途批次重复 call_rcu()。 */
	int			scheduled;
	/* 双缓冲的待回收 class/chain 元数据，本身不动态分配。 */
	struct pending_free	pf[2];
} delayed_free;

/*
 * The lockdep classes are in a hash-table as well, for fast lookup:
 */
/* 锁类另存入 hash 表以快速查找，避免每次 acquire 都线性扫描 all_lock_classes。 */
/* 桶位数比最大 key 索引位数少一，通过 hlist 冲突链在空间和查找长度间折中。 */
#define CLASSHASH_BITS		(MAX_LOCKDEP_KEYS_BITS - 1)
/* class hash 桶数固定为 2 的 CLASSHASH_BITS 次方。 */
#define CLASSHASH_SIZE		(1UL << CLASSHASH_BITS)
/* 将 class key 指针值散列为桶下标；相等 key 必须得到相同结果。 */
#define __classhashfn(key)	hash_long((unsigned long)key, CLASSHASH_BITS)
/* 返回给定 key 对应桶头，结果只是静态数组中的借用指针。 */
#define classhashentry(key)	(classhash_table + __classhashfn((key)))

/* 由 class key 定位已注册 lock_class 的固定桶数组；发布/删除采用 RCU hlist 操作。 */
static struct hlist_head classhash_table[CLASSHASH_SIZE];

/*
 * We put the lock dependency chains into a hash-table as well, to cache
 * their existence:
 */
/*
 * 已验证过的锁依赖链同样放进 hash 表作为存在性缓存：相同 chain key 命中后，可跳过再次执行昂贵的
 * 全图依赖证明；hash 冲突仍需后续字段校验，不能仅凭桶命中认定同一条链。
 */
/* chain 桶位数从最大 chain 数量位数减一取得。 */
#define CHAINHASH_BITS		(MAX_LOCKDEP_CHAINS_BITS-1)
/* 固定 chain hash 桶数。 */
#define CHAINHASH_SIZE		(1UL << CHAINHASH_BITS)
/* 把 64 位 chain key 折叠到桶下标。 */
#define __chainhashfn(chain)	hash_long(chain, CHAINHASH_BITS)
/* 返回 chain key 所在桶头的借用指针。 */
#define chainhashentry(chain)	(chainhash_table + __chainhashfn((chain)))

/* 已发布 lock_chain 的 RCU hlist 桶数组，元素本体仍来自 lock_chains[] 静态池。 */
static struct hlist_head chainhash_table[CHAINHASH_SIZE];

/*
 * the id of held_lock
 */
/* held_lock 的紧凑 ID 同时编码锁类数组下标和 read 模式，供 chain_hlocks[] 节省静态池空间。 */
/**
 * hlock_id - 将 held_lock 压缩为 chain 使用的 16 位标识
 * @hlock: 含有效 class_idx/read 位字段的 held-lock 记录，仅借用
 *
 * 返回：低 MAX_LOCKDEP_KEYS_BITS 位为 class 索引，其上的两位保留 read 模式。
 * 编译边界：若 class 位数加 read 的两位超过 u16，BUILD_BUG_ON 会阻止该配置构建。
 * 副作用与所有权：纯编码，不修改 hlock，也不为返回整数附加对象生命周期。
 */
static inline u16 hlock_id(struct held_lock *hlock)
{
	/* 静态证明两段位域可无损装进 chain_hlocks[] 的 u16 元素。 */
	BUILD_BUG_ON(MAX_LOCKDEP_KEYS_BITS + 2 > 16);

	/* class 占低位，read 左移到其后；两者的布局由上面的容量断言保证不重叠。 */
	return (hlock->class_idx | (hlock->read << MAX_LOCKDEP_KEYS_BITS));
}

/**
 * chain_hlock_class_idx - 从紧凑 held-lock ID 中取回 class 索引
 * @hlock_id: hlock_id() 生成或从 chain_hlocks[] 读取的 16 位标识
 *
 * 返回：清除高位 read 模式后的 lock_classes[] 下标。
 * 副作用：纯位运算；__maybe_unused 允许不需要反解的配置裁剪调用点。
 */
static inline __maybe_unused unsigned int chain_hlock_class_idx(u16 hlock_id)
{
	/* MAX_LOCKDEP_KEYS 为 2 的幂，减一形成覆盖所有 class 索引低位的掩码。 */
	return hlock_id & (MAX_LOCKDEP_KEYS - 1);
}

/*
 * The hash key of the lock dependency chains is a hash itself too:
 * it's a hash of all locks taken up to that lock, including that lock.
 * It's a 64-bit hash, because it's important for the keys to be
 * unique.
 */
/*
 * 锁依赖链的 key 本身也是 hash：它按获取顺序累计从链首到当前锁（包含当前锁）的所有紧凑 ID。
 * 采用 64 位是为了降低不同链得到相同 key 的概率；它仍不是数学上的唯一证明，调试构建会另做
 * collision 校验。
 */
/**
 * iterate_chain_key - 将一个锁 ID 滚动混入已有 64 位 chain key
 * @key: 前一链前缀的 64 位 key
 * @idx: 本次追加的紧凑 held-lock ID，按 u32 参与 jhash mix
 *
 * 返回：包含新锁后的 64 位 chain key；相同起点和有序 ID 序列得到相同结果。
 * 副作用：仅修改局部的两个 32 位半部，不访问或发布 chain cache。
 */
static inline u64 iterate_chain_key(u64 key, u32 idx)
{
	/* k0/k1 分别承载 key 的低/高 32 位，供 jhash 的双字混合宏原地更新。 */
	u32 k0 = key, k1 = key >> 32;

	__jhash_mix(idx, k0, k1); /* Macro that modifies arguments! */
	/* __jhash_mix 是会改写实参的宏；这里 idx、k0、k1 都是局部值，不会篡改调用者的输入对象。 */

	/* 把混合后的两个半部重新拼成 u64，k1 明确提升后再左移以免在 32 位宽度内截断。 */
	return k0 | (u64)k1 << 32;
}

/**
 * lockdep_init_task - 初始化一个任务的 lockdep 运行状态
 * @task: 正在创建或重置、尚未由本函数之外并发修改 lockdep 字段的任务
 *
 * 返回：无；把 held-lock 深度、当前 chain key 和任务级递归计数恢复到空状态。
 * 所有权与边界：只修改 task 内嵌字段，不取得 task_struct 引用，也不清理 held_locks[] 的旧字节；
 * depth 归零后那些槽即不再属于有效栈。
 */
void lockdep_init_task(struct task_struct *task)
{
	task->lockdep_depth = 0; /* no locks held yet */
	/* 当前尚未持有任何锁，因此有效 held-lock 栈深度为零。 */
	/* 空链从约定的种子开始，第一次 acquire 会在此基础上混入 hlock ID。 */
	task->curr_chain_key = INITIAL_CHAIN_KEY;
	/* 新任务不应继承调用者的任务级 lockdep 递归隔离状态。 */
	task->lockdep_recursion = 0;
}

/**
 * lockdep_recursion_inc - 屏蔽本 CPU 上嵌套进入 lockdep
 *
 * 输入与返回：无；调用方须保证当前 CPU 稳定，并最终调用 lockdep_recursion_finish() 配对。
 * 副作用：增加本 CPU lockdep_recursion；计数而非布尔值允许内部辅助路径合法嵌套。
 */
static __always_inline void lockdep_recursion_inc(void)
{
	__this_cpu_inc(lockdep_recursion);
}

/**
 * lockdep_recursion_finish - 结束一次本 CPU lockdep 递归隔离
 *
 * 输入与返回：无；应与先前的 lockdep_recursion_inc() 严格配对。
 * 副作用：递减本 CPU 计数；若结果仍非零则一次性报警并强制清零，防止失配让该 CPU 永久停止跟踪。
 * 失败边界：强制清零优先恢复可用性，但会掩盖更深层尚未退出的错误嵌套，因此告警是重要诊断证据。
 */
static __always_inline void lockdep_recursion_finish(void)
{
	/* 正常最外层配对应降到零；任何非零结果都说明进入/退出数量失配。 */
	if (WARN_ON_ONCE(__this_cpu_dec_return(lockdep_recursion)))
		/* 报警后恢复门控，避免后续所有锁事件都被 lockdep_enabled() 拒绝。 */
		__this_cpu_write(lockdep_recursion, 0);
}

/**
 * lockdep_set_selftest_task - 设置 lockdep 自测期间使用的任务身份
 * @task: 自测任务，或用 NULL 清除覆盖；调用方保证非 NULL 对象覆盖整个使用期
 *
 * 返回：无；仅替换全局借用指针，不增加或释放 task_struct 引用。
 * 并发边界：供受控自测设置，不为任意并发写提供同步；正常运行路径不应竞态修改它。
 */
void lockdep_set_selftest_task(struct task_struct *task)
{
	lockdep_selftest_task_struct = task;
}

/*
 * Debugging switches:
 */
/* 以下是编译期详细诊断开关，默认全部关闭；打开会增加 printk/栈回溯等调试噪声和热路径开销。 */

/* 控制通用锁类详细输出；设为 1 时 verbose() 还会经过 class_filter()。 */
#define VERBOSE			0
/* 为更细粒度的深层诊断保留的总开关，当前文件版本默认不启用。 */
#define VERY_VERBOSE		0

#if VERBOSE
/* 通用 verbose 打开时同步打开 hardirq 与 softirq 状态迁移的详细输出。 */
# define HARDIRQ_VERBOSE	1
# define SOFTIRQ_VERBOSE	1
#else
/* 默认构建把两类 IRQ 详细路径都编译为关闭。 */
# define HARDIRQ_VERBOSE	0
# define SOFTIRQ_VERBOSE	0
#endif

#if VERBOSE || HARDIRQ_VERBOSE || SOFTIRQ_VERBOSE
/*
 * Quick filtering for interesting events:
 */
/* 快速筛选值得输出的锁类，避免启用详细模式后为所有锁事件刷屏。 */
/**
 * class_filter - 判断一个锁类是否进入详细诊断输出
 * @class: 待筛选的有效 lock_class，仅借用
 *
 * 返回：非零表示允许输出；当前有效实现固定返回 0，示例条件被 #if 0 排除。
 * 副作用：无；开发者临时启用示例时可按 name/name_version 精确挑选目标锁类。
 */
static int class_filter(struct lock_class *class)
{
#if 0
	/* Example */
	/* 示例：仅匹配名称版本为 1 的指定锁名；该块当前不参与编译。 */
	if (class->name_version == 1 &&
			!strcmp(class->name, "lockname"))
		return 1;
	if (class->name_version == 1 &&
			!strcmp(class->name, "&struct->lockfield"))
		return 1;
#endif
	/* Filter everything else. 1 would be to allow everything else */
	/* 过滤其余所有锁类；改为 1 则会放行其余所有事件。 */
	return 0;
}
#endif

/**
 * verbose - 判断指定锁类是否需要通用详细输出
 * @class: 待判断的 lock_class；VERBOSE 关闭时参数不会被读取
 *
 * 返回：VERBOSE 打开时返回 class_filter() 的结果，否则固定为 0。
 * 副作用与所有权：纯编译期开关包装，不修改 class，也不持有其引用。
 */
static int verbose(struct lock_class *class)
{
#if VERBOSE
	/* 详细模式仍经精确过滤，便于聚焦单个锁类。 */
	return class_filter(class);
#endif
	/* 默认构建完全关闭详细输出，编译器可消除相应调用分支。 */
	return 0;
}

/**
 * print_lockdep_off - 输出 lockdep 已关闭的公共诊断尾部
 * @bug_msg: 调用者提供的首行错误原因字符串；仅在当前同步打印期间借用
 *
 * 返回：无；向内核日志打印原因和“关闭锁正确性验证器”提示，LOCK_STAT 构建还请求附带
 * /proc/lock_stat。调用者通常已释放图锁并进入控制台紧急区，以免 printk 递归回 lockdep。
 */
static void print_lockdep_off(const char *bug_msg)
{
	/* 先保留最具体的调用方错误原因。 */
	printk(KERN_DEBUG "%s\n", bug_msg);
	/* 告知后续不再继续验证，避免用户把缺少新报告误判为问题消失。 */
	printk(KERN_DEBUG "turning off the locking correctness validator.\n");
	/* 锁正确性验证器正在关闭。 */
#ifdef CONFIG_LOCK_STAT
	/* 若统计可用，请把 /proc/lock_stat 一并附到缺陷报告，辅助还原争用与持锁现场。 */
	printk(KERN_DEBUG "Please attach the output of /proc/lock_stat to the bug report\n");
#endif
}

/* stack_trace[] 当前已消耗的 unsigned long 槽数；由图锁保护，用于容量检查和诊断统计。 */
unsigned long nr_stack_trace_entries;

#ifdef CONFIG_PROVE_LOCKING
/**
 * struct lock_trace - single stack backtrace
 * @hash_entry:	Entry in a stack_trace_hash[] list.
 * @hash:	jhash() of @entries.
 * @nr_entries:	Number of entries in @entries.
 * @entries:	Actual stack backtrace.
 */
/*
 * lock_trace 表示一条去重后的调用栈：hash_entry 把对象挂入 stack_trace_hash[] 的冲突链；hash 是
 * entries 字节序列的 jhash；nr_entries 给出柔性数组中的有效地址数；entries[] 紧随头部保存真实
 * 回溯地址。对象不是独立分配的，而是按 unsigned long 对齐顺序铺在 stack_trace[] 静态池中。
 */
struct lock_trace {
	/* hash 桶链节点，只有提交新 trace 时才链接。 */
	struct hlist_node	hash_entry;
	/* 调用栈地址序列的 32 位快速筛选值，完整相等仍需比较 entries。 */
	u32			hash;
	/* 柔性数组内有效 unsigned long 地址的数量。 */
	u32			nr_entries;
	/* 实际返回地址序列；对齐保证把全局 unsigned long 池转换为本结构时布局合法。 */
	unsigned long		entries[] __aligned(sizeof(unsigned long));
};
/* 一个不含柔性数组内容的 lock_trace 头部占用多少个全局 unsigned long 槽。 */
#define LOCK_TRACE_SIZE_IN_LONGS				\
	(sizeof(struct lock_trace) / sizeof(unsigned long))
/*
 * Stack-trace: sequence of lock_trace structures. Protected by the graph_lock.
 */
/*
 * stack_trace 是连续排列的 lock_trace 记录序列，正常写入由 graph_lock 保护；终止报告在先冻结
 * lockdep 后使用。nr_stack_trace_entries 是下一候选起始槽，hash 链只索引池内记录且不拥有它们。
 */
/* 固定容量 backing store；池满会关闭 lockdep，而不是在锁验证路径中动态扩容。 */
static unsigned long stack_trace[MAX_STACK_TRACE_ENTRIES];
/* 按 trace hash 低位索引的冲突桶，所有元素均指向 stack_trace[] 内已提交记录。 */
static struct hlist_head stack_trace_hash[STACK_TRACE_HASH_SIZE];

/**
 * traces_identical - 判断两条锁依赖调用栈是否完全相同
 * @t1: 第一条有效 lock_trace，仅借用
 * @t2: 第二条有效 lock_trace，仅借用
 *
 * 返回：hash、条目数和全部返回地址均相同才为 true，否则为 false。
 * 副作用与并发：纯比较；调用者以 graph_lock 或已永久冻结 lockdep 的终止诊断状态稳定池和 hash
 * 链，函数不取得任何引用。
 */
static bool traces_identical(struct lock_trace *t1, struct lock_trace *t2)
{
	/* hash 与长度先作廉价拒绝，memcmp 负责排除 hash 碰撞以及同长不同栈。 */
	return t1->hash == t2->hash && t1->nr_entries == t2->nr_entries &&
		memcmp(t1->entries, t2->entries,
		       t1->nr_entries * sizeof(t1->entries[0])) == 0;
}

/**
 * save_trace - 捕获、去重并保存当前 lockdep 调用栈
 *
 * 输入：无；常规调用者持有 graph_lock；首个环路报告路径会先永久关闭 lockdep 并释放图锁，再由
 * 唯一报告者调用。本函数从当前调用路径跳过前三层内部栈帧开始回溯。
 * 返回：已有相同 trace 或新提交 trace 的池内借用指针；容量耗尽或已由别处关闭调试时返回 NULL。
 * 所有权：记录永久驻留在静态 stack_trace[] 池中，不单独释放；返回指针不得由调用者释放。
 * 失败边界：扣除记录头后已没有可保存的返回地址槽时会走关闭调试并解锁的容量错误分支；常规
 * 图锁调用者见到 NULL 后不得再次解锁。终止诊断调用依赖 lockdep 已冻结、不会再有正常图写者。
 */
static struct lock_trace *save_trace(void)
{
	/* trace 指向尚未提交的池尾候选，t2 遍历同 hash 桶，hash_head 是对应桶头。 */
	struct lock_trace *trace, *t2;
	struct hlist_head *hash_head;
	/* hash 保存候选栈的 jhash；max_entries 是扣除头部后本次最多可捕获的地址数。 */
	u32 hash;
	int max_entries;

	/* 低位掩码选桶要求桶数为 2 的幂。 */
	BUILD_BUG_ON_NOT_POWER_OF_2(STACK_TRACE_HASH_SIZE);
	/* 至少要让静态池能够容纳一个 trace 头部，否则任何记录都无法构造。 */
	BUILD_BUG_ON(LOCK_TRACE_SIZE_IN_LONGS >= MAX_STACK_TRACE_ENTRIES);

	/* 在尚未推进全局游标的位置原地构造候选；重复栈可直接丢弃这些临时写入。 */
	trace = (struct lock_trace *)(stack_trace + nr_stack_trace_entries);
	/* 剩余总槽先扣掉固定头部，得到柔性 entries[] 的安全写入上限。 */
	max_entries = MAX_STACK_TRACE_ENTRIES - nr_stack_trace_entries -
		LOCK_TRACE_SIZE_IN_LONGS;

	/* 扣除头部后已无地址槽时，继续验证将丢失依赖证据，故永久关闭 lockdep。 */
	if (max_entries <= 0) {
		/* 组合 helper 同时关闭调试并释放图锁；只有首次关闭者负责打印。 */
		if (!debug_locks_off_graph_unlock())
			return NULL;

		/* 图锁已释放，使用 nbcon 紧急区输出，降低诊断期间控制台重入造成的信息损坏。 */
		nbcon_cpu_emergency_enter();
		print_lockdep_off("BUG: MAX_STACK_TRACE_ENTRIES too low!");
		dump_stack();
		nbcon_cpu_emergency_exit();

		return NULL;
	}
	/* 跳过 save_trace() 及其近邻内部帧，尽量让证据从有意义的 lockdep 调用点开始。 */
	trace->nr_entries = stack_trace_save(trace->entries, max_entries, 3);

	/* 对实际捕获的地址字节计算 jhash，并缓存到候选头部。 */
	hash = jhash(trace->entries, trace->nr_entries *
		     sizeof(trace->entries[0]), 0);
	trace->hash = hash;
	/* 桶数已由编译断言保证为 2 的幂，故低位与运算可代替取模。 */
	hash_head = stack_trace_hash + (hash & (STACK_TRACE_HASH_SIZE - 1));
	/* hash 命中后仍逐地址比较；重复时返回旧对象且不推进池游标。 */
	hlist_for_each_entry(t2, hash_head, hash_entry) {
		if (traces_identical(trace, t2))
			return t2;
	}
	/* 新栈正式占用头部和柔性数组的全部槽，下一候选从其后开始。 */
	nr_stack_trace_entries += LOCK_TRACE_SIZE_IN_LONGS + trace->nr_entries;
	/* 在图锁保护下把新对象发布到桶头，后续相同栈即可复用它。 */
	hlist_add_head(&trace->hash_entry, hash_head);

	/* 返回静态池内已提交对象的借用指针。 */
	return trace;
}

/* Return the number of stack traces in the stack_trace[] array. */
/* 返回 stack_trace[] 静态池中已经提交的调用栈对象数量。 */
/**
 * lockdep_stack_trace_count - 统计所有 stack trace hash 桶中的记录数
 *
 * 输入：无。
 * 返回：逐桶遍历得到的 lock_trace 对象总数，而不是占用的 unsigned long 槽数。
 * 并发边界：面向 /proc/lockdep_stats 的诊断统计，不获取 graph_lock；即使 trace 提交后不删除，
 * 并发新增仍意味着结果只能作为近似观测，调用者不能据此作正确性控制决策。
 */
u64 lockdep_stack_trace_count(void)
{
	/* trace 是桶遍历游标，c 用 64 位承载总对象数，i 遍历全部固定桶。 */
	struct lock_trace *trace;
	u64 c = 0;
	int i;

	/* 每遇到一个已链接记录便累加一次；不按 nr_entries 计数。 */
	for (i = 0; i < ARRAY_SIZE(stack_trace_hash); i++) {
		hlist_for_each_entry(trace, &stack_trace_hash[i], hash_entry) {
			c++;
		}
	}

	return c;
}

/* Return the number of stack hash chains that have at least one stack trace. */
/* 返回至少包含一条 stack trace 的 hash 冲突链数量。 */
/**
 * lockdep_stack_hash_count - 统计非空 stack trace hash 桶
 *
 * 输入：无。
 * 返回：stack_trace_hash[] 中非空桶的个数；它反映 hash 分布，不等于 trace 总数。
 * 并发边界：与 lockdep_stack_trace_count() 相同，这是不加图锁的近似诊断读取；函数不修改桶。
 */
u64 lockdep_stack_hash_count(void)
{
	/* c 记录非空桶数，i 顺序扫描整个固定数组。 */
	u64 c = 0;
	int i;

	/* 一个桶无论冲突链上有多少记录都只计一次。 */
	for (i = 0; i < ARRAY_SIZE(stack_trace_hash); i++)
		if (!hlist_empty(&stack_trace_hash[i]))
			c++;

	return c;
}
#endif

/* 当前 chain cache 中归类为 hardirq 上下文的链数量，由 chain 发布/回收路径成对增减。 */
unsigned int nr_hardirq_chains;
/* 当前 chain cache 中归类为 softirq 上下文的链数量；hardirq 位优先于 softirq 位分类。 */
unsigned int nr_softirq_chains;
/* 当前 chain cache 中不带 hardirq/softirq 上下文位的进程上下文链数量。 */
unsigned int nr_process_chains;
/* 运行以来观测到的单任务 held-lock 栈最大深度，只增不减，供诊断展示。 */
unsigned int max_lockdep_depth;

#ifdef CONFIG_DEBUG_LOCKDEP
/*
 * Various lockdep statistics:
 */
/* 各类 lockdep 内部统计；按 CPU 存放以降低热路径竞争，读取时通常跨 CPU 近似汇总。 */
/* 每 CPU lockdep_stats 的字段布局和 debug_atomic helper 由 lockdep_internals.h 统一定义。 */
DEFINE_PER_CPU(struct lockdep_stats, lockdep_stats);
#endif

#ifdef CONFIG_PROVE_LOCKING
/*
 * Locking printouts:
 */
/* 以下数据把内部 usage bit 翻译为诊断字符串，供锁依赖报警和状态打印复用。 */

/*
 * 对每个 IRQ 状态生成四个指定下标的字符串：在该上下文写持有、IRQ 开启时写持有、在该上下文
 * 读持有、IRQ 开启时读持有。指定下标初始化保证字符串表与 enum lock_usage_bit 的稀疏编号一致。
 */
#define __USAGE(__STATE)						\
	[LOCK_USED_IN_##__STATE] = "IN-"__stringify(__STATE)"-W",	\
	[LOCK_ENABLED_##__STATE] = __stringify(__STATE)"-ON-W",		\
	[LOCK_USED_IN_##__STATE##_READ] = "IN-"__stringify(__STATE)"-R",\
	[LOCK_ENABLED_##__STATE##_READ] = __stringify(__STATE)"-ON-R",

/* usage bit 到人类可读标签的静态查找表；字符串常量由本文件持有，调用者只借用。 */
static const char *usage_str[] =
{
/* 重定义 LOCKDEP_STATE 后包含无 include guard 的状态表，为每个状态展开上述四个字符串。 */
#define LOCKDEP_STATE(__STATE) __USAGE(__STATE)
#include "lockdep_states.h"
/* 展开结束立即撤销临时宏，防止污染后续代码。 */
#undef LOCKDEP_STATE
	/* 两个初始使用位不带 IRQ 方向，分别描述第一次写/读使用。 */
	[LOCK_USED] = "INITIAL USE",
	[LOCK_USED_READ] = "INITIAL READ USE",
	/* abused as string storage for verify_lock_unused() */
	/* 该越过正常 usage 位的槽被 verify_lock_unused() 借作固定字符串存储，而非真实状态索引。 */
	[LOCK_USAGE_STATES] = "IN-NMI",
};
#endif

/**
 * __get_key_name - 尝试把 lockdep subclass key 地址解析为内核符号名
 * @key: 要解析的 subclass key，仅把其地址值交给 kallsyms
 * @str: 调用者提供的 KSYM_NAME_LEN 级缓冲，供 kallsyms 写入格式化名称
 *
 * 返回：kallsyms_lookup() 返回的名称指针，可能指向 @str，也可能在无法解析时为 NULL。
 * 副作用与所有权：不修改 key；返回名称由 kallsyms/调用方缓冲持有，调用者不得释放。
 */
const char *__get_key_name(const struct lockdep_subclass_key *key, char *str)
{
	/* key 的地址就是要符号化的内核地址，其余偏移/大小输出在此场景不需要。 */
	return kallsyms_lookup((unsigned long)key, NULL, NULL, NULL, str);
}

/**
 * lock_flag - 把 usage bit 编号转换为单比特掩码
 * @bit: 小于 unsigned long 位宽的 enum lock_usage_bit 值
 *
 * 返回：仅第 @bit 位为 1 的 unsigned long，供 usage_mask 测试或合并。
 * 副作用：纯位运算；bit 范围由 lockdep_states/lockdep_types 的编译期布局契约保证。
 */
static inline unsigned long lock_flag(enum lock_usage_bit bit)
{
	return 1UL << bit;
}

/**
 * get_usage_char - 把一个 IRQ 状态的写或读 usage 对转换为单字符
 * @class: 要读取 usage_mask 的有效锁类，仅借用
 * @bit: 该状态“在 IRQ 上下文使用”方向的基础 usage bit
 *
 * 返回：'.' 表示 IRQ 关闭且不在该 IRQ 上下文，'+' 表示 IRQ 开启且不在该上下文，'-' 表示位于
 * IRQ 上下文且 IRQ 关闭，'?' 表示两种冲突属性同时出现。
 * 副作用与并发：纯诊断读取，不修改 class；并发 usage 更新时结果是当下可见掩码的近似解释。
 */
static char get_usage_char(struct lock_class *class, enum lock_usage_bit bit)
{
	/*
	 * The usage character defaults to '.' (i.e., irqs disabled and not in
	 * irq context), which is the safest usage category.
	 */
	/* 默认 '.'：IRQ 已关闭且不在对应 IRQ 上下文，是四种组合中最安全的使用类别。 */
	char c = '.';

	/*
	 * The order of the following usage checks matters, which will
	 * result in the outcome character as follows:
	 *
	 * - '+': irq is enabled and not in irq context
	 * - '-': in irq context and irq is disabled
	 * - '?': in irq context and irq is enabled
	 */
	/*
	 * 下列检查顺序会决定最终字符：'+' 表示 IRQ 开启且不在 IRQ 上下文；'-' 表示位于 IRQ 上下文
	 * 且 IRQ 关闭；'?' 表示位于 IRQ 上下文时又记录到 IRQ 开启，是需要关注的冲突组合。
	 */
	/* 方向偏移后的配对位表示“IRQ 开启时使用”，先把候选字符设为 '+'。 */
	if (class->usage_mask & lock_flag(bit + LOCK_USAGE_DIR_MASK)) {
		c = '+';
		/* 基础位也存在说明同时记录了 IRQ 上下文使用，升级为冲突字符 '?'。 */
		if (class->usage_mask & lock_flag(bit))
			c = '?';
	/* 未见 IRQ 开启属性但基础上下文位存在，表示 IRQ 上下文内的关闭状态。 */
	} else if (class->usage_mask & lock_flag(bit))
		c = '-';

	return c;
}

/**
 * get_usage_chars - 生成一个锁类的完整 IRQ usage 字符串
 * @class: 要读取 usage_mask 的有效锁类，仅借用
 * @usage: 至少 LOCK_USAGE_CHARS 字节的调用者缓冲，返回时写入字符串和结尾 NUL
 *
 * 返回：无；按 lockdep_states.h 的状态顺序为每个状态依次写入写模式、读模式两个字符。
 * 不变量：LOCK_USAGE_CHARS 必须覆盖两倍状态数再加终止符，状态表顺序决定用户可见列顺序。
 */
void get_usage_chars(struct lock_class *class, char usage[LOCK_USAGE_CHARS])
{
	/* i 是输出游标；宏每展开一个状态推进两次。 */
	int i = 0;

/* 为每个状态生成写 usage 字符和读 usage 字符，二者分别使用对应基础 bit。 */
#define LOCKDEP_STATE(__STATE) 						\
	usage[i++] = get_usage_char(class, LOCK_USED_IN_##__STATE);	\
	usage[i++] = get_usage_char(class, LOCK_USED_IN_##__STATE##_READ);
#include "lockdep_states.h"
#undef LOCKDEP_STATE

	/* 把固定宽度字符序列终止为可交给 printk/seq_file 的 C 字符串。 */
	usage[i] = '\0';
}

/**
 * __print_lock_name - 续写一个锁类的可区分名称
 * @hlock: 可选 held-lock 记录；非 NULL 时供锁类自定义 print_fn 查看具体实例
 * @class: 要打印的有效锁类，仅在同步打印期间借用
 *
 * 返回：无；使用 KERN_CONT 续写符号名或显式名称，并在需要时追加 name_version、subclass 和实例
 * 自定义信息。函数不打印 usage、wait type 或换行。
 * 并发边界：诊断路径应已稳定 class/hlock；print_fn 由锁类提供，必须适合当前 printk 上下文。
 */
static void __print_lock_name(struct held_lock *hlock, struct lock_class *class)
{
	/* str 为 kallsyms 回退缓冲，name 指向 class 名、该缓冲或符号表返回的名称。 */
	char str[KSYM_NAME_LEN];
	const char *name;

	/* 显式名字优先；没有名字时用 class key 的地址尝试符号化。 */
	name = class->name;
	if (!name) {
		name = __get_key_name(class->key, str);
		printk(KERN_CONT "%s", name);
	} else {
		/* 同名锁类用版本号区分，首个版本省略 #1 以保持输出简洁。 */
		printk(KERN_CONT "%s", name);
		if (class->name_version > 1)
			printk(KERN_CONT "#%d", class->name_version);
		/* subclass 0 是基础类，非零嵌套子类以 /N 明示。 */
		if (class->subclass)
			printk(KERN_CONT "/%d", class->subclass);
		/* 只有具体 held instance 可用时才调用锁类型自定义的实例打印回调。 */
		if (hlock && class->print_fn)
			class->print_fn(hlock->instance);
	}
}

/**
 * print_lock_name - 续写锁类名称、usage 字符和 wait-type 约束
 * @hlock: 可选 held-lock 实例，传给 __print_lock_name()
 * @class: 要打印的有效锁类
 *
 * 返回：无；输出形如“(name){usage}-{outer:inner}”，不负责打印实例地址或结尾换行。
 * 语义：outer 为零时回退到 inner，表示没有更宽松的外层等待类型覆盖。
 */
static void print_lock_name(struct held_lock *hlock, struct lock_class *class)
{
	/* usage 缓冲由 get_usage_chars() 完整填充并 NUL 终止。 */
	char usage[LOCK_USAGE_CHARS];

	/* 先快照 usage 字符串，避免在多次 printk 间重复解释掩码。 */
	get_usage_chars(class, usage);

	/* 名称和属性使用 KERN_CONT 拼接到调用者已经开始的同一日志行。 */
	printk(KERN_CONT " (");
	__print_lock_name(hlock, class);
	printk(KERN_CONT "){%s}-{%d:%d}", usage,
			class->wait_type_outer ?: class->wait_type_inner,
			class->wait_type_inner);
}

/**
 * print_lockdep_cache - 续写一个 lockdep_map 的缓存名称
 * @lock: 已初始化的 lockdep_map；名称缺失时要求 key/subkeys 可用于 kallsyms 回退
 *
 * 返回：无；优先打印 lock->name，否则打印基础 subclass key 的符号名，不追加属性或换行。
 * 所有权：只在调用期间借用 map、key 和名称；局部 str 承接符号化文本。
 */
static void print_lockdep_cache(struct lockdep_map *lock)
{
	/* name 最终指向 map 名或局部 kallsyms 缓冲；str 只活到本次 printk 完成。 */
	const char *name;
	char str[KSYM_NAME_LEN];

	/* 动态/显式初始化通常提供 name，匿名静态 map 则退回 key 地址的符号。 */
	name = lock->name;
	if (!name)
		name = __get_key_name(lock->key->subkeys, str);

	/* 作为更大诊断行的一部分续写，换行由调用者统一控制。 */
	printk(KERN_CONT "%s", name);
}

/**
 * print_lock - 打印一条 held-lock 记录
 * @hlock: 任务 held_locks[] 中的记录；可能在无锁诊断期间被所属任务并发释放和清零
 *
 * 返回：无；有效时打印实例地址、锁类名称/属性和获取点，无法解析有效 class 时打印 <RELEASED>。
 * 并发边界：debug_show_all_locks() 可无锁调用，故输出只用于尽力诊断，不能假定 hlock 各字段来自
 * 同一时刻；函数不取得任务、hlock 或 class 的所有权。
 */
static void print_lock(struct held_lock *hlock)
{
	/*
	 * We can be called locklessly through debug_show_all_locks() so be
	 * extra careful, the hlock might have been released and cleared.
	 *
	 * If this indeed happens, lets pretend it does not hurt to continue
	 * to print the lock unless the hlock class_idx does not point to a
	 * registered class. The rationale here is: since we don't attempt
	 * to distinguish whether we are in this situation, if it just
	 * happened we can't count on class_idx to tell either.
	 */
	/*
	 * debug_show_all_locks() 可能无锁调用本函数，因此必须格外谨慎：目标 hlock 可能已被释放并清零。
	 * 若确实发生这种竞态，只要 class_idx 仍指向已注册锁类，就继续尽力打印；只有它不再指向有效类
	 * 才停止。原因是这里不会也无法可靠区分“刚刚并发释放”和普通稳定记录，class_idx 本身也只能
	 * 充当最低限度的有效性门槛，不能证明其余字段是一致快照。
	 */
	/* 先通过静态 class 池的 in-use 位验证索引；失败时 hlock_class() 返回 NULL。 */
	struct lock_class *lock = hlock_class(hlock);

	/* 无有效 class 可借用时，把记录标为已释放，避免继续解引用实例或获取点。 */
	if (!lock) {
		printk(KERN_CONT "<RELEASED>\n");
		return;
	}

	/* 先打印具体锁实例地址，再拼接 class 的 usage/wait 属性和获取调用点符号。 */
	printk(KERN_CONT "%px", hlock->instance);
	print_lock_name(hlock, lock);
	printk(KERN_CONT ", at: %pS\n", (void *)hlock->acquire_ip);
}

/**
 * lockdep_print_held_locks - 打印一个任务当前可可靠读取的 held-lock 栈
 * @p: 目标任务，仅在调用期间借用；调用者负责保证 task_struct 生命周期
 *
 * 返回：无；总会先打印按 READ_ONCE 取得的深度摘要，随后仅对 current 或非运行状态的其他任务
 * 逐项打印 held_locks[]。
 * 并发边界：不锁住目标任务；正在其他 CPU 运行的非 current 任务可能随时改栈，因此只给摘要并
 * 提前返回。睡眠任务或 current 的逐项输出仍是诊断视角，不转移任何 hlock 所有权。
 */
static void lockdep_print_held_locks(struct task_struct *p)
{
	/* depth 只读取一次，作为摘要与后续循环的共同上界，避免编译器重复取变化中的字段。 */
	int i, depth = READ_ONCE(p->lockdep_depth);

	/* 先打印数量和任务身份；str_plural() 仅负责英文单复数后缀。 */
	if (!depth)
		printk("no locks held by %s/%d.\n", p->comm, task_pid_nr(p));
	else
		printk("%d lock%s held by %s/%d:\n", depth,
		       str_plural(depth), p->comm, task_pid_nr(p));
	/*
	 * It's not reliable to print a task's held locks if it's not sleeping
	 * and it's not the current task.
	 */
	/* 若目标不是 current 且仍在运行，其 held-lock 栈可能并发变化，逐项打印并不可靠。 */
	if (p != current && task_is_running(p))
		return;
	/* 对当前任务或已停止改栈的睡眠任务，按获取顺序打印快照深度内的每条记录。 */
	for (i = 0; i < depth; i++) {
		printk(" #%d: ", i);
		print_lock(p->held_locks + i);
	}
}

/**
 * print_kernel_ident - 打印当前内核发布版本、构建版本首段和 taint 状态
 *
 * 输入与返回：无。
 * 副作用：向日志写一行环境标识，帮助把 lockdep 报告对应到准确内核和污染状态；不修改 utsname。
 */
static void print_kernel_ident(void)
{
	/* version 只打印第一个空格前的片段，避免冗长构建字符串淹没诊断标题。 */
	printk("%s %.*s %s\n", init_utsname()->release,
		(int)strcspn(init_utsname()->version, " "),
		init_utsname()->version,
		print_tainted());
}

/**
 * very_verbose - 判断锁类是否进入最高详细级别输出
 * @class: 待筛选的有效锁类；VERY_VERBOSE 关闭时不读取
 *
 * 返回：VERY_VERBOSE 打开时为 class_filter() 结果，否则固定返回 0。
 * 副作用：无；编译期常量为零时调用分支可被优化掉。
 */
static int very_verbose(struct lock_class *class)
{
#if VERY_VERBOSE
	/* 即使打开最高详细级别，也可通过 class_filter() 限定关注对象。 */
	return class_filter(class);
#endif
	/* 默认构建禁止最高详细输出。 */
	return 0;
}

/*
 * Is this the address of a static object:
 */
/* 判断给定地址是否属于能为 lockdep key 提供足够稳定生命周期的静态内核对象。 */
#ifdef __KERNEL__
/**
 * static_obj - 判断对象地址是否位于 lockdep 认可的静态存储区域
 * @obj: 要分类的任意内核地址，不会被解引用
 *
 * 返回：核心 data/rodata、仍有效的 initdata、内核 percpu、模块普通或 percpu 区域返回 1，否则 0。
 * 生命周期：返回 1 不代表对象永不消失；initdata 仅在释放前有效，模块地址则依赖卸载路径及时注销
 * key。函数只按地址区间分类，不取得模块或对象引用。
 */
static int static_obj(const void *obj)
{
	/* 后续 helper 都按 unsigned long 地址判断所属链接区间。 */
	unsigned long addr = (unsigned long) obj;

	/* 核心内核的可写静态数据覆盖整个运行期，可直接作为稳定 key。 */
	if (is_kernel_core_data(addr))
		return 1;

	/*
	 * keys are allowed in the __ro_after_init section.
	 */
	/* key 也允许位于包含 __ro_after_init 的内核只读数据区域。 */
	if (is_kernel_rodata(addr))
		return 1;

	/*
	 * in initdata section and used during bootup only?
	 * NOTE: On some platforms the initdata section is
	 * outside of the _stext ... _end range.
	 */
	/*
	 * 地址是否位于仅启动期使用的 initdata？某些平台的 initdata 位于 _stext.._end 之外，所以必须
	 * 用专用区间 helper；并且只在系统尚未进入释放 initmem 阶段时承认其生命周期。
	 */
	if (system_state < SYSTEM_FREEING_INITMEM &&
		init_section_contains((void *)addr, 1))
		return 1;

	/*
	 * in-kernel percpu var?
	 */
	/* 内核自身的 percpu 变量也属静态对象；调用链会另行把各 CPU 实例规范化到统一 key。 */
	if (is_kernel_percpu_address(addr))
		return 1;

	/*
	 * module static or percpu var?
	 */
	/* 模块普通静态区或 percpu 区在模块存活期间有效，卸载时必须走 lockdep key 清理流程。 */
	return is_module_address(addr) || is_module_percpu_address(addr);
}
#endif

/*
 * To make lock name printouts unique, we calculate a unique
 * class->name_version generation counter. The caller must hold the graph
 * lock.
 */
/*
 * 为使锁名输出可区分，为同名但不同基础 key 的 class 计算 name_version。调用者必须持有 graph
 * lock；同一基础 key 的不同 subclass 继承已有版本号，不会因 /subclass 再占一个同名版本。
 */
/**
 * count_matching_names - 为新锁类选择稳定的同名版本号
 * @new_class: 已填入 key/subclass/name、尚待发布的锁类；仅借用
 *
 * 返回：无显式名称时为 0；同一基础 key 已存在时复用其 name_version；否则返回所有同名类最大
 * 版本加一，首次同名即为 1。
 * 并发边界：调用者必须持有 graph_lock，以稳定 all_lock_classes 和并发注册顺序；函数不修改链表。
 */
static int count_matching_names(struct lock_class *new_class)
{
	/* class 是已发布类的遍历游标，count 保存目前看到的同名最大版本。 */
	struct lock_class *class;
	int count = 0;

	/* 匿名类靠 key 符号化，不需要 name#N 消歧。 */
	if (!new_class->name)
		return 0;

	/* 图锁保护下遍历全部在用 class，分别处理同基础 key 与纯同名两种关系。 */
	list_for_each_entry(class, &all_lock_classes, lock_entry) {
		/* key 减 subclass 回到 subkeys[0]；同一基础 map 的其他子类沿用同一名称版本。 */
		if (new_class->key - new_class->subclass == class->key)
			return class->name_version;
		/* 不同基础 key 但字符串相同则记录最大已用版本，避免输出重名。 */
		if (class->name && !strcmp(class->name, new_class->name))
			count = max(count, class->name_version);
	}

	/* 没有同基础 key 可继承时，为该名字分配下一个单调版本。 */
	return count + 1;
}

/* used from NMI context -- must be lockless */
/* 可从 NMI 上下文调用，因此查询过程不能获取 graph_lock，必须依赖 IRQ-off 的 RCU-sched 读侧。 */
/**
 * look_up_lock_class - 无锁查找 lockdep_map 指定 subclass 的已注册锁类
 * @lock: 已初始化或尚未首次使用的 lockdep_map，仅借用
 * @subclass: 要查找的子类下标，必须小于 MAX_LOCKDEP_SUBCLASSES
 *
 * 返回：匹配 key 的已发布 lock_class 借用指针；map 尚无 key、桶中未注册或输入非法时返回 NULL。
 * 上下文：noinstr 且可从 NMI 调用；调用者必须关闭本地 IRQ，以形成无锁 RCU-sched 遍历所需边界。
 * 失败边界：subclass 越界说明调用契约损坏，会永久关闭 debug_locks、在显式 instrumentation 区间
 * 打印错误和栈；函数不注册缺失 class，注册由更高层慢路径完成。
 */
static noinstr struct lock_class *
look_up_lock_class(const struct lockdep_map *lock, unsigned int subclass)
{
	/* key 是 map 的具体 subclass key；hash_head/class 用于无锁遍历对应 RCU 桶。 */
	struct lockdep_subclass_key *key;
	struct hlist_head *hash_head;
	struct lock_class *class;

	/* 越界会使 subkeys 指针运算逃出对象，必须在任何寻址前终止 lockdep。 */
	if (unlikely(subclass >= MAX_LOCKDEP_SUBCLASSES)) {
		/* noinstr 函数只有包在 instrumentation_begin/end 内才能安全执行诊断插桩路径。 */
		instrumentation_begin();
		debug_locks_off();
		/* NMI/异常环境使用控制台紧急区尽力保全错误文本。 */
		nbcon_cpu_emergency_enter();
		printk(KERN_ERR
			"BUG: looking up invalid subclass: %u\n", subclass);
		printk(KERN_ERR
			"turning off the locking correctness validator.\n");
		dump_stack();
		nbcon_cpu_emergency_exit();
		instrumentation_end();
		return NULL;
	}

	/*
	 * If it is not initialised then it has never been locked,
	 * so it won't be present in the hash table.
	 */
	/* map 尚未初始化 key，说明它从未进入注册路径，因此 hash 表中不可能存在对应 class。 */
	if (unlikely(!lock->key))
		return NULL;

	/*
	 * NOTE: the class-key must be unique. For dynamic locks, a static
	 * lock_class_key variable is passed in through the mutex_init()
	 * (or spin_lock_init()) call - which acts as the key. For static
	 * locks we use the lock object itself as the key.
	 */
	/*
	 * class key 必须唯一。动态锁由 mutex_init()/spin_lock_init() 等传入静态 lock_class_key；静态锁
	 * 直接以锁对象自身地址作为 key。若两个不同语义对象复用 key，整个依赖图会把它们错误合并。
	 */
	/* 静态锁会在 lockdep_map 存储范围内借用 key 形状，编译期保证 key 不比 map 大。 */
	BUILD_BUG_ON(sizeof(struct lock_class_key) >
			sizeof(struct lockdep_map));

	/* 基础 key 加 subclass 得到本次查找的精确、稳定身份。 */
	key = lock->key->subkeys + subclass;

	/* 相同 key 必然落入同一 class hash 桶，冲突项再逐个比较指针。 */
	hash_head = classhashentry(key);

	/*
	 * We do an RCU walk of the hash, see lockdep_free_key_range().
	 */
	/*
	 * 这里对 hash 做 RCU 遍历；删除与 grace-period 契约见 lockdep_free_key_range()。本路径不显式
	 * rcu_read_lock()，而依赖调用者关闭 IRQ 所提供的 RCU-sched 读侧临界区。
	 */
	/* IRQ 未关闭时既不满足 NMI/本 CPU 稳定约束，也不能安全借用可能正在延迟回收的 class。 */
	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return NULL;

	/* notrace 版本避免遍历 helper 的插桩重新进入 lockdep。 */
	hlist_for_each_entry_rcu_notrace(class, hash_head, hash_entry) {
		if (class->key == key) {
			/*
			 * Huh! same key, different name? Did someone trample
			 * on some memory? We're most confused.
			 */
			/*
			 * key 相同但名称不同通常意味着 key 唯一性被破坏或内存遭踩踏；除专用 no_validate map 外
			 * 只报警一次。即使名称异常，key 仍是身份依据，所以返回找到的 class 供上层继续诊断。
			 */
			WARN_ONCE(class->name != lock->name &&
				  lock->key != &__lockdep_no_validate__,
				  "Looking for class \"%s\" with key %ps, but found a different class \"%s\" with the same key\n",
				  lock->name, lock->key, class->name);
			return class;
		}
	}

	/* 桶中不存在相等 key；上层注册慢路径会在图锁下二次查找后决定是否新建 class。 */
	return NULL;
}

/*
 * Static locks do not have their class-keys yet - for them the key is
 * the lock object itself. If the lock is in the per cpu area, the
 * canonical address of the lock (per cpu offset removed) is used.
 */
/*
 * 静态锁初始没有单独 class key，因而以锁对象自身地址作为身份；若对象位于 percpu 区，必须去掉
 * CPU 偏移并使用规范地址，使各 CPU 副本映射到同一锁类，而不是错误地产生每 CPU 独立 class。
 */
/**
 * assign_lock_key - 为尚无 key 的静态 lockdep_map 推导持久 class key
 * @lock: 要原地填写 key 的 lockdep_map；调用者拥有对象并保证尚未并发注册
 *
 * 返回：内核/模块 percpu 或其他静态地址成功赋 key 时为 true；对象不在持久区域时为 false。
 * 副作用：成功时 lock->key 指向规范 percpu 地址或 map 自身；失败时永久关闭 debug_locks，并在
 * 控制台紧急区提示对象可能缺初始化或需要显式 lockdep 注解。
 * 所有权：key 只是借用静态对象地址，不分配内存；模块卸载仍须由 key 注销路径清理关联 class。
 */
static bool assign_lock_key(struct lockdep_map *lock)
{
	/* addr 是当前 map 地址；can_addr 接收去除 percpu 实例偏移后的统一地址。 */
	unsigned long can_addr, addr = (unsigned long)lock;

#ifdef __KERNEL__
	/*
	 * lockdep_free_key_range() assumes that struct lock_class_key
	 * objects do not overlap. Since we use the address of lock
	 * objects as class key for static objects, check whether the
	 * size of lock_class_key objects does not exceed the size of
	 * the smallest lock object.
	 */
	/*
	 * lockdep_free_key_range() 假定 struct lock_class_key 对象彼此不重叠。静态对象直接用锁地址充当
	 * key，因此编译期保证 key 的覆盖尺寸不超过最小锁对象 raw_spinlock_t，避免相邻锁身份重叠。
	 */
	BUILD_BUG_ON(sizeof(struct lock_class_key) > sizeof(raw_spinlock_t));
#endif

	/* 核心 percpu map：将任意 CPU 实例地址规范化为无 CPU 偏移的共同 key。 */
	if (__is_kernel_percpu_address(addr, &can_addr))
		lock->key = (void *)can_addr;
	/* 模块 percpu map 采用相同规范化原则，但生命周期由模块卸载路径约束。 */
	else if (__is_module_percpu_address(addr, &can_addr))
		lock->key = (void *)can_addr;
	/* 其他认可的核心/模块静态区域直接使用 map 自身地址。 */
	else if (static_obj(lock))
		lock->key = (void *)lock;
	else {
		/* Debug-check: all keys must be persistent! */
		/* 调试不变量：所有 key 都必须覆盖 class 使用期，普通临时/堆对象地址不能隐式充当 key。 */
		debug_locks_off();
		/* 关闭后再尽力输出常见原因；不继续注册一个生命周期不可信的 class。 */
		nbcon_cpu_emergency_enter();
		pr_err("INFO: trying to register non-static key.\n");
		pr_err("The code is fine but needs lockdep annotation, or maybe\n");
		pr_err("you didn't initialize this object before use?\n");
		pr_err("turning off the locking correctness validator.\n");
		dump_stack();
		nbcon_cpu_emergency_exit();
		return false;
	}

	/* map 已持有可稳定复用的借用 key，注册慢路径可以据此查找或创建 class。 */
	return true;
}

#ifdef CONFIG_DEBUG_LOCKDEP

/* Check whether element @e occurs in list @h */
/* 检查链表节点 @e 是否出现在以 @h 为头的双向链表中。 */
/**
 * in_list - 按节点地址检查 list_head 成员关系
 * @e: 要寻找的嵌入式 list_head 节点
 * @h: 要遍历的链表头
 *
 * 返回：遇到同一节点地址时为 true，遍历结束未找到时为 false。
 * 并发边界：仅用于 DEBUG_LOCKDEP 自一致性检查，调用者负责用 graph_lock 稳定链表；函数不改链。
 */
static bool in_list(struct list_head *e, struct list_head *h)
{
	/* f 是裸 list_head 遍历游标，因为这里只比较节点身份，不需要外层容器。 */
	struct list_head *f;

	/* 指针相等即证明目标节点已链接到该表。 */
	list_for_each(f, h) {
		if (e == f)
			return true;
	}

	return false;
}

/*
 * Check whether entry @e occurs in any of the locks_after or locks_before
 * lists.
 */
/* 检查条目 @e 是否出现在任意锁类的 locks_after 或 locks_before 依赖链中。 */
/**
 * in_any_class_list - 全池搜索一个依赖边节点是否被任意 class 链表引用
 * @e: 通常为 list_entries[] 某元素的 entry 节点
 *
 * 返回：任一 lock_class 的正向或反向依赖链包含 @e 时为 true，否则为 false。
 * 代价与并发：扫描全部 class 槽并在各链上线性查找，只供显式一致性检查；调用者须持有图锁。
 */
static bool in_any_class_list(struct list_head *e)
{
	/* class/i 顺序覆盖整个静态池，包括空闲槽，以发现任何残留错误链接。 */
	struct lock_class *class;
	int i;

	/* 任一方向找到即提前成功，不需要统计重复出现次数。 */
	for (i = 0; i < ARRAY_SIZE(lock_classes); i++) {
		class = &lock_classes[i];
		if (in_list(e, &class->locks_after) ||
		    in_list(e, &class->locks_before))
			return true;
	}
	return false;
}

/**
 * class_lock_list_valid - 验证一条 class 依赖链的反向 owner 字段
 * @c: 拥有 @h 链表头的锁类
 * @h: @c 的 locks_before 或 locks_after 链表头
 *
 * 返回：每个 lock_list.links_to 都指回 @c 时为 true；首个不匹配项打印诊断并返回 false。
 * 不变量：依赖边的两份方向记录都把 links_to 设为链表 owner，class 则指向另一端；调用者持有图锁。
 */
static bool class_lock_list_valid(struct lock_class *c, struct list_head *h)
{
	/* e 逐项借用静态 list_entries[] 中已链接的依赖记录。 */
	struct lock_list *e;

	/* 一旦 links_to 与链表 owner 不符，说明边记录挂错链或字段已损坏。 */
	list_for_each_entry(e, h, entry) {
		if (e->links_to != c) {
			/* 输出 owner、池下标和边的两端名称，便于定位哪条镜像依赖失配。 */
			printk(KERN_INFO "class %s: mismatch for lock entry %ld; class %s <> %s",
			       c->name ? : "(?)",
			       (unsigned long)(e - list_entries),
			       e->links_to && e->links_to->name ?
			       e->links_to->name : "(?)",
			       e->class && e->class->name ? e->class->name :
			       "(?)");
			return false;
		}
	}
	return true;
}

#ifdef CONFIG_PROVE_LOCKING
/*
 * 所有已缓存 chain 的紧凑 held-lock ID backing store；lock_chain.base/depth 切出各自连续区间。
 * DEBUG_LOCKDEP 在此处提前声明供 key 重算，实际分配/回收同样受 graph_lock 保护。
 */
static u16 chain_hlocks[MAX_LOCKDEP_CHAIN_HLOCKS];
#endif

/**
 * check_lock_chain_key - 重算并验证一条缓存依赖链的 64 位 key
 * @chain: 已发布的 lock_chain，仅借用；base/depth 必须指向有效 chain_hlocks[] 区间
 *
 * 返回：重算 key 与 chain->chain_key 相同为 true，不同则打印池下标和两值后返回 false；未编译
 * PROVE_LOCKING 时没有 chain hlock 数据可核对，固定返回 true。
 * 并发边界：由 DEBUG_LOCKDEP 全局一致性检查在图锁保护下调用，不修改 chain 或 backing store。
 */
static bool check_lock_chain_key(struct lock_chain *chain)
{
#ifdef CONFIG_PROVE_LOCKING
	/* 从空链种子开始，i 顺序覆盖该 chain 在紧凑数组中的全部 ID。 */
	u64 chain_key = INITIAL_CHAIN_KEY;
	int i;

	/* 必须保持保存时的获取顺序，逐个滚动混入才能复现 chain key。 */
	for (i = chain->base; i < chain->base + chain->depth; i++)
		chain_key = iterate_chain_key(chain_key, chain_hlocks[i]);
	/*
	 * The 'unsigned long long' casts avoid that a compiler warning
	 * is reported when building tools/lib/lockdep.
	 */
	/* 转成 unsigned long long 是为避免构建 tools/lib/lockdep 时因 u64 平台类型差异产生格式告警。 */
	/* 不一致说明 key、base/depth 或 backing store 至少一项已经损坏。 */
	if (chain->chain_key != chain_key) {
		printk(KERN_INFO "chain %lld: key %#llx <> %#llx\n",
		       (unsigned long long)(chain - lock_chains),
		       (unsigned long long)chain->chain_key,
		       (unsigned long long)chain_key);
		return false;
	}
#endif
	return true;
}

/**
 * in_any_zapped_class_list - 判断锁类是否正在任一延迟回收批次中
 * @class: lock_classes[] 中的目标槽
 *
 * 返回：class->lock_entry 位于 delayed_free.pf[0] 或 pf[1] 的 zapped 链时为 true，否则 false。
 * 并发边界：调用者须持有 graph_lock，以稳定双缓冲 index 和两条链；函数不改变回收状态。
 */
static bool in_any_zapped_class_list(struct lock_class *class)
{
	/* pf 顺序遍历两个缓冲，i 作为明确数组边界。 */
	struct pending_free *pf;
	int i;

	/* zapped class 复用 lock_entry，因此可直接按节点身份检查。 */
	for (i = 0, pf = delayed_free.pf; i < ARRAY_SIZE(delayed_free.pf); i++, pf++) {
		if (in_list(&class->lock_entry, &pf->zapped))
			return true;
	}

	return false;
}

/**
 * __check_data_structures - 全量核对 lockdep 静态池、链表和 chain key 的基本一致性
 *
 * 输入：无；调用者必须持有 graph_lock，使 class/edge/chain 与 delayed-free 元数据在扫描期间稳定。
 * 返回：五组检查全部通过时为 true；发现首个矛盾时打印定位信息并返回 false。
 * 检查范围：class 至少属于 active/free/zapped 状态链之一；每条 class 依赖链的 links_to 正确；
 * 已发布 chain key 可重算；in-use 依赖槽出现在某条 class 链；未使用槽不出现在任何 class 链。
 * 边界：该函数不修复损坏，也不额外证明节点只出现一次或双向边必然成对，不能把通过视为完整形式
 * 验证；高代价全池扫描只在显式一致性调试开关下触发。
 */
static bool __check_data_structures(void)
{
	/* class/chain/e 分别遍历三类静态池对象，head 是 chain hash 桶头，i 复用为数组/位图游标。 */
	struct lock_class *class;
	struct lock_chain *chain;
	struct hlist_head *head;
	struct lock_list *e;
	int i;

	/* Check whether all classes occur in a lock list. */
	/* 检查每个 class 槽至少出现在 active、free 或两个 zapped 状态链之一。 */
	for (i = 0; i < ARRAY_SIZE(lock_classes); i++) {
		class = &lock_classes[i];
		if (!in_list(&class->lock_entry, &all_lock_classes) &&
		    !in_list(&class->lock_entry, &free_lock_classes) &&
		    !in_any_zapped_class_list(class)) {
			printk(KERN_INFO "class %px/%s is not in any class list\n",
			       class, class->name ? : "(?)");
			return false;
		}
	}

	/* Check whether all classes have valid lock lists. */
	/* 检查所有 class 的正反依赖链中，每个条目的 links_to 都指回该链 owner。 */
	for (i = 0; i < ARRAY_SIZE(lock_classes); i++) {
		class = &lock_classes[i];
		if (!class_lock_list_valid(class, &class->locks_before))
			return false;
		if (!class_lock_list_valid(class, &class->locks_after))
			return false;
	}

	/* Check the chain_key of all lock chains. */
	/* 对每个已发布 chain 逐 ID 重算 chain_key，hash 桶本身只决定遍历入口。 */
	for (i = 0; i < ARRAY_SIZE(chainhash_table); i++) {
		head = chainhash_table + i;
		/* 图锁已持有；沿用 RCU 遍历形式以匹配该表的发布/删除原语。 */
		hlist_for_each_entry_rcu(chain, head, entry) {
			if (!check_lock_chain_key(chain))
				return false;
		}
	}

	/*
	 * Check whether all list entries that are in use occur in a class
	 * lock list.
	 */
	/* 检查所有置位的依赖池槽确实挂在至少一条 class 正向或反向依赖链中。 */
	for_each_set_bit(i, list_entries_in_use, ARRAY_SIZE(list_entries)) {
		/* 位图下标与静态数组一一对应，e 是当前占用槽。 */
		e = list_entries + i;
		if (!in_any_class_list(&e->entry)) {
			printk(KERN_INFO "list entry %d is not in any class list; class %s <> %s\n",
			       (unsigned int)(e - list_entries),
			       e->class->name ? : "(?)",
			       e->links_to->name ? : "(?)");
			return false;
		}
	}

	/*
	 * Check whether all list entries that are not in use do not occur in
	 * a class lock list.
	 */
	/* 反向检查所有清位槽不应仍残留在任何 class 依赖链，否则后续复用会形成双重链接。 */
	for_each_clear_bit(i, list_entries_in_use, ARRAY_SIZE(list_entries)) {
		e = list_entries + i;
		if (in_any_class_list(&e->entry)) {
			printk(KERN_INFO "list entry %d occurs in a class list; class %s <> %s\n",
			       (unsigned int)(e - list_entries),
			       e->class && e->class->name ? e->class->name :
			       "(?)",
			       e->links_to && e->links_to->name ?
			       e->links_to->name : "(?)");
			return false;
		}
	}

	return true;
}

/*
 * 运行期一致性扫描开关，默认关闭以避免全池遍历开销；0644 允许特权用户通过模块参数接口开启。
 * 非零只决定是否检查，不代表上一次检查结果。
 */
int check_consistency = 0;
module_param(check_consistency, int, 0644);

/**
 * check_data_structures - 按运行开关触发一次或多次 lockdep 全局一致性扫描
 *
 * 输入与返回：无；调用者必须持有 graph_lock。
 * 副作用：check_consistency 非零且尚未失败时调用 __check_data_structures()；首个失败把静态 once 置位
 * 并 WARN，之后永久跳过重复高噪声检查。成功不会置 once，因此后续调用仍会再次验证最新状态。
 */
static void check_data_structures(void)
{
	/* once 只记“已经报告过失败”，不是“已经成功检查过”。 */
	static bool once = false;

	/* 默认开关为零时热路径只付出一个分支；失败报告后也不再扫描。 */
	if (check_consistency && !once) {
		if (!__check_data_structures()) {
			/* 先置位再 WARN，使后续重入或调用不重复遍历已知损坏结构。 */
			once = true;
			WARN_ON(once);
		}
	}
}

#else /* CONFIG_DEBUG_LOCKDEP */

/**
 * check_data_structures - 未编译 DEBUG_LOCKDEP 时的空实现
 *
 * 输入、返回与副作用：无；保留统一调用点，编译器会完全消除该函数。
 */
static inline void check_data_structures(void) { }

#endif /* CONFIG_DEBUG_LOCKDEP */

/**
 * init_chain_block_buckets - 初始化 chain_hlocks 空闲块分配器
 *
 * 输入与返回：无；实际定义位于 chain allocator 区域，统一建立所有 bucket 和初始大空闲块。
 * 调用约束：仅由 lockdep 数据结构一次性初始化路径调用；不分配动态内存。
 */
static void init_chain_block_buckets(void);

/*
 * Initialize the lock_classes[] array elements, the free_lock_classes list
 * and also the delayed_free structure.
 */
/* 初始化 lock_classes[] 各槽、free_lock_classes 链表以及 delayed_free 双缓冲结构。 */
/**
 * init_data_structures_once - 分阶段完成 lockdep 静态数据结构的一次性初始化
 *
 * 输入与返回：无。
 * 第一阶段：初始化两个 zapped 链表，把全部 class 槽挂入 free 链，初始化每个 class 的正反依赖链，
 * 并建立 chain_hlocks 空闲块桶；ds_initialized 防止重复链接同一静态节点。
 * 第二阶段：只有系统进入 SYSTEM_SCHEDULING 后才初始化 delayed_free.rcu_head；早期调用完成第一阶段
 * 后会在稍晚调用补做此步骤，rcu_head_initialized 为 true 后即可快速返回。
 * 并发边界：两个 bool 是一次性阶段标志而非通用锁；正常调用链依赖 lockdep 初始化/图锁序列化首次
 * 结构建立。函数只组织静态存储，不转移动态资源所有权。
 */
static void init_data_structures_once(void)
{
	/* ds_initialized 跟踪结构池，rcu_head_initialized 跟踪必须等待调度阶段的回调头。 */
	static bool __read_mostly ds_initialized, rcu_head_initialized;
	/* i 遍历全部 lock_classes[] 静态槽。 */
	int i;

	/* RCU 头能初始化意味着普通结构阶段也已经在同次或更早调用完成，直接走热路径返回。 */
	if (likely(rcu_head_initialized))
		return;

	/* 早期启动不能初始化 RCU 回调头；进入调度阶段后的首次调用再补齐并发布阶段标志。 */
	if (system_state >= SYSTEM_SCHEDULING) {
		init_rcu_head(&delayed_free.rcu_head);
		rcu_head_initialized = true;
	}

	/* 普通池结构可能已在早期调用完成，此时只需上面的 RCU 补初始化。 */
	if (ds_initialized)
		return;

	/* 在链接静态节点前标记阶段，保证受控重入不会把同一节点重复加入链表。 */
	ds_initialized = true;

	/* 两个 delayed-free 缓冲都从空 zapped class 链开始。 */
	INIT_LIST_HEAD(&delayed_free.pf[0].zapped);
	INIT_LIST_HEAD(&delayed_free.pf[1].zapped);

	/* 每个 class 初始都可分配，并拥有两条独立的空依赖链。 */
	for (i = 0; i < ARRAY_SIZE(lock_classes); i++) {
		list_add_tail(&lock_classes[i].lock_entry, &free_lock_classes);
		INIT_LIST_HEAD(&lock_classes[i].locks_after);
		INIT_LIST_HEAD(&lock_classes[i].locks_before);
	}
	/* 最后把完整 chain_hlocks backing store 登记成空闲块，供 chain cache 分配。 */
	init_chain_block_buckets();
}

/**
 * keyhashentry - 定位动态 lock_class_key 的注册桶
 * @key: 用地址身份参与散列的 key；函数不解引用其内容
 *
 * 返回：lock_keys_hash[] 中对应 hlist 桶头的借用指针。
 * 副作用：无；hash_long() 已按 KEYHASH_BITS 把结果限制到固定数组范围。
 */
static inline struct hlist_head *keyhashentry(const struct lock_class_key *key)
{
	/* 动态 key 以对象地址唯一标识，uintptr_t 保留指针的整数位模式。 */
	unsigned long hash = hash_long((uintptr_t)key, KEYHASH_BITS);

	return lock_keys_hash + hash;
}

/* Register a dynamically allocated key. */
/* 注册一个动态分配、其生命周期由调用者显式管理的 lockdep key。 */
/**
 * lockdep_register_key - 把动态 lock_class_key 发布到全局注册 hash
 * @key: 调用者拥有且尚未注册的动态 key；在注销和 RCU 安全回收前必须持续有效
 *
 * 返回：无；成功时以 RCU hlist 发布 key 并增加 nr_dynamic_keys。
 * 上下文：保存并关闭本地 IRQ后获取 graph_lock；重复注册或误传静态地址只 WARN 一次并不修改表。
 * 失败边界：若 graph_lock() 发现调试已关闭则仅恢复 IRQ；函数不接管或释放 key 内存。
 */
void lockdep_register_key(struct lock_class_key *key)
{
	/* hash_head 是目标桶，k 用于重复检查，flags 保存调用者原始 IRQ 状态。 */
	struct hlist_head *hash_head;
	struct lock_class_key *k;
	unsigned long flags;

	/* 静态 key 无需也不允许进入动态注册表，否则注销所有权会含混。 */
	if (WARN_ON_ONCE(static_obj(key)))
		return;
	/* 桶可在拿锁前按不可变 key 地址计算，实际遍历/发布仍在图锁下完成。 */
	hash_head = keyhashentry(key);

	/* 图锁 helper 要求 IRQ-off；保存式接口允许调用者原先开或关 IRQ。 */
	raw_local_irq_save(flags);
	if (!graph_lock())
		goto restore_irqs;
	/* 图锁内检查同一指针是否已经发布，避免一个 hash_entry 被重复链接。 */
	hlist_for_each_entry_rcu(k, hash_head, hash_entry) {
		if (WARN_ON_ONCE(k == key))
			goto out_unlock;
	}
	/* RCU 发布使无锁/读锁查询者安全看到新 key；计数与发布同处图锁临界区。 */
	hlist_add_head_rcu(&key->hash_entry, hash_head);
	nr_dynamic_keys++;
out_unlock:
	/* 成功和重复路径都在这里释放图锁。 */
	graph_unlock();
restore_irqs:
	/* 恢复进入本 API 前的 IRQ 状态。 */
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lockdep_register_key);

/* Check whether a key has been registered as a dynamic key. */
/* 检查一个非静态 key 地址是否已经登记为动态 key。 */
/**
 * is_dynamic_key - 在 RCU 保护下查询动态 key 注册表
 * @key: 要按指针身份查找的候选 key
 *
 * 返回：找到同一 key 指针时为 true；正常未找到或误传静态地址时为 false。debug_locks 已关闭时
 * 特意返回 true，不再遍历可能含已释放内存的 hash，以安全放弃后续验证。
 * 并发边界：内部获取普通 RCU 读锁；返回值不授予 key 生命周期引用，调用方仍须遵守注册/注销契约。
 */
static bool is_dynamic_key(const struct lock_class_key *key)
{
	/* hash_head 是候选桶，k 遍历已注册 key，found 保存读侧结果。 */
	struct hlist_head *hash_head;
	struct lock_class_key *k;
	bool found = false;

	/* 静态地址应由 static_obj() 分支处理，不应拿动态表查询结果为其背书。 */
	if (WARN_ON_ONCE(static_obj(key)))
		return false;

	/*
	 * If lock debugging is disabled lock_keys_hash[] may contain
	 * pointers to memory that has already been freed. Avoid triggering
	 * a use-after-free in that case by returning early.
	 */
	/*
	 * 锁调试关闭后，lock_keys_hash[] 可能仍含已经释放内存的指针；此时提前返回，避免查询本身触发
	 * use-after-free。返回 true 是“停止质疑 key”的保守放行，不表示表中确认存在该 key。
	 */
	if (!debug_locks)
		return true;

	/* 调试仍有效时才计算并遍历真实桶。 */
	hash_head = keyhashentry(key);

	/* 注销方使用 RCU 删除和 grace period，读锁覆盖完整指针比较过程。 */
	rcu_read_lock();
	hlist_for_each_entry_rcu(k, hash_head, hash_entry) {
		if (k == key) {
			/* 只比较对象身份；命中后无需继续扫描 hash 冲突项。 */
			found = true;
			break;
		}
	}
	rcu_read_unlock();

	return found;
}

/*
 * Register a lock's class in the hash-table, if the class is not present
 * yet. Otherwise we look it up. We cache the result in the lock object
 * itself, so actual lookup of the hash should be once per lock object.
 */
/*
 * 若锁类尚未出现则注册到 hash，否则复用已有 class；结果缓存回 lockdep_map，使同一锁对象通常只需
 * 做一次全局 hash 查询。hash/class 对象属于 lockdep 静态池，map 只保存借用指针。
 */
/**
 * register_lock_class - 查找或注册 lockdep_map 的指定锁子类
 * @lock: 要建立 class 映射并更新 class_cache 的 lockdep_map
 * @subclass: 目标子类编号，范围由 look_up_lock_class() 验证
 * @force: 非零时即使 subclass 非零也把结果写入 class_cache[0]
 *
 * 返回：已存在或新发布的 lock_class 借用指针；key 无效、图调试关闭、静态池耗尽或一致性异常时
 * 返回 NULL。
 * 上下文：调用者必须关闭本地 IRQ。函数先无锁 RCU 查找；创建路径获取 graph_lock 并二次查重，
 * 初始化静态槽后以 RCU 发布。正常返回不持有图锁。
 * 失败边界：class 池耗尽会关闭 debug_locks 并由 helper 释放图锁后打印；其他失败路径也不会把图锁
 * 所有权留给调用者。成功后 class 生命周期由 lockdep/RCU 管理，lock 只缓存指针。
 */
static struct lock_class *
register_lock_class(struct lockdep_map *lock, unsigned int subclass, int force)
{
	/* key 是具体 subclass 身份，hash_head 是其桶，class 保存命中/新槽，idx 是静态池下标。 */
	struct lockdep_subclass_key *key;
	struct hlist_head *hash_head;
	struct lock_class *class;
	int idx;

	/* 无锁查找与后续 percpu/RCU 契约都依赖 IRQ-off；告警不替代调用方修正。 */
	DEBUG_LOCKS_WARN_ON(!irqs_disabled());

	/* 常见路径先做 NMI 安全的无图锁 RCU 查找，命中便只更新 map cache。 */
	class = look_up_lock_class(lock, subclass);
	if (likely(class))
		goto out_set_class_cache;

	/* 尚无 key 的静态 map 尝试由地址推导；已有 key 则必须是认可的静态或已登记动态 key。 */
	if (!lock->key) {
		if (!assign_lock_key(lock))
			return NULL;
	} else if (!static_obj(lock->key) && !is_dynamic_key(lock->key)) {
		return NULL;
	}

	/* key 已稳定后，subkeys[subclass] 是本次 class 的精确身份并决定 hash 桶。 */
	key = lock->key->subkeys + subclass;
	hash_head = classhashentry(key);

	/* 创建或全局发布必须串行；0 表示其他 CPU 已关闭调试且 helper 已自行解锁。 */
	if (!graph_lock()) {
		return NULL;
	}
	/*
	 * We have to do the hash-walk again, to avoid races
	 * with another CPU:
	 */
	/* 拿图锁前另一 CPU 可能已经注册同一 key，因此必须在锁内重走桶，避免重复 class。 */
	hlist_for_each_entry_rcu(class, hash_head, hash_entry) {
		if (class->key == key)
			goto out_unlock_set;
	}

	/* 首次实际分配前建立 free class 链、依赖链、延迟回收缓冲和 chain allocator。 */
	init_data_structures_once();

	/* Allocate a new lock class and add it to the hash. */
	/* 从 free_lock_classes 取一个静态槽；不进行可能递归进入锁路径的动态内存分配。 */
	class = list_first_entry_or_null(&free_lock_classes, typeof(*class),
					 lock_entry);
	if (!class) {
		/* 池耗尽使后续证明不完整，永久关闭调试并释放图锁；非首个关闭者静默返回。 */
		if (!debug_locks_off_graph_unlock()) {
			return NULL;
		}

		/* 图锁已释放，首个报告者在控制台紧急区输出容量诊断和现场栈。 */
		nbcon_cpu_emergency_enter();
		print_lockdep_off("BUG: MAX_LOCKDEP_KEYS too low!");
		dump_stack();
		nbcon_cpu_emergency_exit();
		return NULL;
	}
	/* 槽从此对索引有效；计数、in-use 位和“尚未使用”调试统计同步更新。 */
	nr_lock_classes++;
	__set_bit(class - lock_classes, lock_classes_in_use);
	debug_atomic_inc(nr_unused_locks);
	/* 从 map 复制身份和用户可见名称；key/name 均为借用指针，生命周期由初始化/注销契约保证。 */
	class->key = key;
	class->name = lock->name;
	class->subclass = subclass;
	/* 回收后的 free 槽必须已经清空两条依赖链，否则复用会把旧图边带入新 class。 */
	WARN_ON_ONCE(!list_empty(&class->locks_before));
	WARN_ON_ONCE(!list_empty(&class->locks_after));
	/* 在图锁下分配同名版本，并复制等待上下文与锁类型约束。 */
	class->name_version = count_matching_names(class);
	class->wait_type_inner = lock->wait_type_inner;
	class->wait_type_outer = lock->wait_type_outer;
	class->lock_type = lock->lock_type;
	/*
	 * We use RCU's safe list-add method to make
	 * parallel walking of the hash-list safe:
	 */
	/* 使用 RCU 安全的 hlist 插入，允许 IRQ-off/NMI 查询者与发布并行遍历。 */
	hlist_add_head_rcu(&class->hash_entry, hash_head);
	/*
	 * Remove the class from the free list and add it to the global list
	 * of classes.
	 */
	/* 同一 lock_entry 从 free 状态链移动到全局 active 链，槽对象本身不复制。 */
	list_move_tail(&class->lock_entry, &all_lock_classes);
	/* 记录数组扫描上界；较低空洞由 in-use 位过滤。 */
	idx = class - lock_classes;
	if (idx > max_lock_class_idx)
		max_lock_class_idx = idx;

	/* 可选详细模式必须先释放图锁再打印，避免控制台锁反向进入 lockdep。 */
	if (verbose(class)) {
		graph_unlock();

		nbcon_cpu_emergency_enter();
		printk("\nnew class %px: %s", class->key, class->name);
		if (class->name_version > 1)
			printk(KERN_CONT "#%d", class->name_version);
		printk(KERN_CONT "\n");
		dump_stack();
		nbcon_cpu_emergency_exit();

		/* 打印后重新获取图锁，以汇合下方统一解锁标签；失败时 class 已发布但本次返回 NULL。 */
		if (!graph_lock()) {
			return NULL;
		}
	}
out_unlock_set:
	/* 新建路径和图锁内二次查找命中路径在这里释放图锁。 */
	graph_unlock();

out_set_class_cache:
	/* 基础 subclass 或 force 覆盖主缓存槽；可缓存范围内的其他 subclass 使用同下标槽。 */
	if (!subclass || force)
		lock->class_cache[0] = class;
	else if (subclass < NR_LOCKDEP_CACHING_CLASSES)
		lock->class_cache[subclass] = class;

	/*
	 * Hash collision, did we smoke some? We found a class with a matching
	 * hash but the subclass -- which is hashed in -- didn't match.
	 */
	/*
	 * 若精确 key 命中却得到不同 subclass，说明 hash/key 身份或内存已损坏；subclass 本已编码进 key，
	 * 正常的 hash 冲突不会通过上面的 key 指针相等检查。报警并拒绝把该 class 交给调用者。
	 */
	if (DEBUG_LOCKS_WARN_ON(class->subclass != subclass))
		return NULL;

	/* 返回已有或新建 class 的借用指针，map cache 已按规则更新。 */
	return class;
}

#ifdef CONFIG_PROVE_LOCKING
/*
 * Allocate a lockdep entry. (assumes the graph_lock held, returns
 * with NULL on failure)
 */
/* 分配一个 lockdep 依赖条目；进入时假定持有 graph_lock，失败时返回 NULL。 */
/**
 * alloc_list_entry - 从固定 list_entries[] 池取得一个直接依赖边槽
 *
 * 输入：无；调用者必须持有 graph_lock。
 * 返回：成功时返回仍由图锁保护的空闲槽借用指针；池耗尽时返回 NULL。
 * 副作用：成功会置 in-use 位并增加 nr_list_entries；耗尽会永久关闭 debug_locks、释放 graph_lock，
 * 首个报告者在锁外打印容量错误。因此 NULL 返回后调用者不得再解锁或继续改图。
 */
static struct lock_list *alloc_list_entry(void)
{
	/* 位图中第一个零位就是可复用的静态池下标；线性位扫描不分配内存。 */
	int idx = find_first_zero_bit(list_entries_in_use,
				      ARRAY_SIZE(list_entries));

	/* 没有零位即固定容量耗尽，缺失依赖边会破坏证明完整性，必须关闭 lockdep。 */
	if (idx >= ARRAY_SIZE(list_entries)) {
		/* 组合 helper 关闭调试并释放调用者持有的图锁；只有首个关闭者继续打印。 */
		if (!debug_locks_off_graph_unlock())
			return NULL;

		/* 此处已不持有图锁，可安全进入控制台紧急诊断路径。 */
		nbcon_cpu_emergency_enter();
		print_lockdep_off("BUG: MAX_LOCKDEP_ENTRIES too low!");
		dump_stack();
		nbcon_cpu_emergency_exit();
		return NULL;
	}
	/* 在把指针交给调用者前同步更新容量计数与所有权位。 */
	nr_list_entries++;
	__set_bit(idx, list_entries_in_use);
	/* 槽内容由紧随其后的 add_lock_to_list() 完整覆盖，调用者不得释放指针本身。 */
	return list_entries + idx;
}

/*
 * Add a new dependency to the head of the list:
 */
/* 向以 @head 为表头的依赖链登记一条新直接依赖。 */
/**
 * add_lock_to_list - 分配、填充并以 RCU 方式链接一条直接依赖记录
 * @this: 依赖边另一端的 lock_class，写入 entry->class
 * @links_to: 拥有 @head 的 lock_class，写入 entry->links_to
 * @head: links_to 的 locks_after 或 locks_before 链表头
 * @distance: 两个 held-lock 在任务栈中的距离
 * @dep: 编码读写方向强弱关系的依赖位
 * @trace: 证明该边来源的永久 lock_trace 借用指针
 *
 * 返回：成功发布为 1；静态池耗尽为 0，此时 alloc_list_entry() 已释放 graph_lock。
 * 并发边界：调用者进入时持有 graph_lock；分配/删除均在图锁下，读者则可在 RCU-sched 下遍历。
 */
static int add_lock_to_list(struct lock_class *this,
			    struct lock_class *links_to, struct list_head *head,
			    u16 distance, u8 dep,
			    const struct lock_trace *trace)
{
	/* entry 是本次从固定依赖池取得并最终挂入 class 链的槽。 */
	struct lock_list *entry;
	/*
	 * Lock not present yet - get a new dependency struct and
	 * add it to the list:
	 */
	/* 该依赖尚不存在：先取新记录，再把两端、语义、距离和证据一次填全。 */
	entry = alloc_list_entry();
	if (!entry)
		return 0;

	entry->class = this;
	entry->links_to = links_to;
	entry->dep = dep;
	entry->distance = distance;
	entry->trace = trace;
	/*
	 * Both allocation and removal are done under the graph lock; but
	 * iteration is under RCU-sched; see look_up_lock_class() and
	 * lockdep_free_key_range().
	 */
	/*
	 * 分配和删除均持有 graph_lock，但遍历可发生在 RCU-sched 读侧；发布使用 list_add_tail_rcu()，
	 * 对应删除和 grace period 见 look_up_lock_class()/lockdep_free_key_range() 生命周期说明。
	 */
	/* 加到依赖链尾部；head 是链表头名称，不表示使用 list_add() 的头插语义。 */
	list_add_tail_rcu(&entry->entry, head);

	/* 记录已发布且图锁仍由调用者持有。 */
	return 1;
}

/*
 * For good efficiency of modular, we use power of 2
 */
/* 队列尺寸取 2 的幂，使下标环绕可用按位与替代取模；实际最多容纳 size-1 个元素。 */
#define MAX_CIRCULAR_QUEUE_SIZE		(1UL << CONFIG_LOCKDEP_CIRCULAR_QUEUE_BITS)
/* 低位掩码把递增的 front/rear 映射回环形数组范围。 */
#define CQ_MASK				(MAX_CIRCULAR_QUEUE_SIZE-1)

/*
 * The circular_queue and helpers are used to implement graph
 * breadth-first search (BFS) algorithm, by which we can determine
 * whether there is a path from a lock to another. In deadlock checks,
 * a path from the next lock to be acquired to a previous held lock
 * indicates that adding the <prev> -> <next> lock dependency will
 * produce a circle in the graph. Breadth-first search instead of
 * depth-first search is used in order to find the shortest (circular)
 * path.
 */
/*
 * circular_queue 及 helper 为依赖图实现广度优先搜索，用于判断两个锁类之间是否已有路径。死锁检查
 * 中，若从将要获取的 next 能走到已持有的 prev，再加入 prev->next 就会闭合环路。选择 BFS 而非
 * DFS 是为了得到最短的环路证据，便于诊断输出。
 */
struct circular_queue {
	/* 待访问 lock_list 指针的固定环形存储；队列不拥有依赖记录。 */
	struct lock_list *element[MAX_CIRCULAR_QUEUE_SIZE];
	/* front 指向下一出队槽，rear 指向下一入队槽；相等为空，预留一槽区分满状态。 */
	unsigned int  front, rear;
};

/* 全局复用的 BFS 队列；依赖图搜索由 graph_lock 串行化，因此无需每次分配或每 CPU 复制。 */
static struct circular_queue lock_cq;

/* 历史观测到的 BFS 队列最大在队元素数，供容量调优和 /proc 诊断。 */
unsigned int max_bfs_queue_depth;

/* 每次 BFS 递增的遍历代号；class->dep_gen_id 与它相等表示本轮已经访问。 */
static unsigned int lockdep_dependency_gen_id;

/**
 * __cq_init - 清空环形队列并开始新一代 BFS 访问标记
 * @cq: 要重置的队列，通常为全局 lock_cq
 *
 * 返回：无；front/rear 归零并递增全局 dependency generation。
 * 并发边界：调用者持有 graph_lock；generation 技巧避免逐个清空所有 class 的 visited 状态。
 */
static inline void __cq_init(struct circular_queue *cq)
{
	/* 两个游标相等定义空队列，旧 element 指针无需清零，因为不会越过 rear 读取。 */
	cq->front = cq->rear = 0;
	/* 新代号使上一轮 class->dep_gen_id 自动失效。 */
	lockdep_dependency_gen_id++;
}

/**
 * __cq_empty - 判断环形 BFS 队列是否为空
 * @cq: 已初始化队列
 *
 * 返回：front 与 rear 相等时为非零，否则为零；纯读取，不修改队列。
 */
static inline int __cq_empty(struct circular_queue *cq)
{
	return (cq->front == cq->rear);
}

/**
 * __cq_full - 判断环形 BFS 队列是否已无可用槽
 * @cq: 已初始化队列
 *
 * 返回：rear 的下一环绕位置等于 front 时为非零。设计上保留一个空槽区分满与空。
 */
static inline int __cq_full(struct circular_queue *cq)
{
	return ((cq->rear + 1) & CQ_MASK) == cq->front;
}

/**
 * __cq_enqueue - 把依赖记录指针加入 BFS 队尾
 * @cq: 已初始化且由调用者独占的环形队列
 * @elem: 要排队的 lock_list 借用指针，生命周期由依赖图/RCU 保证
 *
 * 返回：成功为 0；队列已满为 -1，且不修改游标或数组有效内容。
 */
static inline int __cq_enqueue(struct circular_queue *cq, struct lock_list *elem)
{
	/* 先保留满状态，避免覆盖 front 指向的尚未访问元素。 */
	if (__cq_full(cq))
		return -1;

	/* 写入当前 rear 后再按掩码推进，使新元素进入有效 [front,rear) 区间。 */
	cq->element[cq->rear] = elem;
	cq->rear = (cq->rear + 1) & CQ_MASK;
	return 0;
}

/*
 * Dequeue an element from the circular_queue, return a lock_list if
 * the queue is not empty, or NULL if otherwise.
 */
/* 从环形队列取出一个元素：非空时返回 lock_list，空队列返回 NULL。 */
/**
 * __cq_dequeue - 从 BFS 队首弹出一条依赖记录
 * @cq: 已初始化且由调用者独占的环形队列
 *
 * 返回：非空时为下一 lock_list 借用指针；空时为 NULL 且不修改队列。
 * 副作用：成功时按 CQ_MASK 推进 front；不清除旧数组槽，因为有效性完全由游标界定。
 */
static inline struct lock_list * __cq_dequeue(struct circular_queue *cq)
{
	/* lock 保存弹出元素，所有权仍属于依赖图。 */
	struct lock_list * lock;

	/* 空队列没有可读槽。 */
	if (__cq_empty(cq))
		return NULL;

	/* 先读取当前 front，再环绕推进到下一个元素。 */
	lock = cq->element[cq->front];
	cq->front = (cq->front + 1) & CQ_MASK;

	return lock;
}

/**
 * __cq_get_elem_count - 计算环形队列当前在队元素数
 * @cq: 已初始化队列
 *
 * 返回：(rear-front) 按环形掩码归一化后的元素数，范围 0..MAX_CIRCULAR_QUEUE_SIZE-1。
 * 副作用：纯读取；调用者在独占队列时用于更新最大深度统计。
 */
static inline unsigned int  __cq_get_elem_count(struct circular_queue *cq)
{
	return (cq->rear - cq->front) & CQ_MASK;
}

/**
 * mark_lock_accessed - 把依赖记录指向的锁类标记为本轮 BFS 已访问
 * @lock: 有效 lock_list；其 class 字段必须可写且由图锁稳定
 *
 * 返回：无；把 class->dep_gen_id 设为当前全局 generation。
 * 语义：访问标记按锁类而非具体边去重，同一 class 的其他入边不会重复入队。
 */
static inline void mark_lock_accessed(struct lock_list *lock)
{
	lock->class->dep_gen_id = lockdep_dependency_gen_id;
}

/**
 * visit_lock_entry - 记录 BFS 首次到达依赖边时的父边
 * @lock: 当前新访问的 lock_list，将原地写 parent
 * @parent: 导致本次到达的上一条边；根或首层可按调用点约定传入父记录
 *
 * 返回：无；parent 链仅服务本轮最短路径回溯，不改变图的依赖关系或对象所有权。
 */
static inline void visit_lock_entry(struct lock_list *lock,
				    struct lock_list *parent)
{
	lock->parent = parent;
}

/**
 * lock_accessed - 查询依赖记录指向的锁类是否已在本轮 BFS 访问
 * @lock: 有效 lock_list，仅借用
 *
 * 返回：class->dep_gen_id 等于当前 generation 时为非零，否则为零；纯读取。
 */
static inline unsigned long lock_accessed(struct lock_list *lock)
{
	return lock->class->dep_gen_id == lockdep_dependency_gen_id;
}

/**
 * get_lock_parent - 取得 BFS 路径中的上一条依赖边
 * @child: 当前 lock_list 节点
 *
 * 返回：visit_lock_entry() 记录的 parent 借用指针；根节点返回 NULL。纯读取，不修改路径。
 */
static inline struct lock_list *get_lock_parent(struct lock_list *child)
{
	return child->parent;
}

/**
 * get_lock_depth - 计算 BFS 父链从当前节点回到根的边数
 * @child: 目标 lock_list 节点
 *
 * 返回：沿 parent 指针走到 NULL 的步数；根节点深度为 0。
 * 边界：假定本轮 BFS 构造的 parent 链无环且在图锁下稳定；函数不检测损坏造成的 parent 环。
 */
static inline int get_lock_depth(struct lock_list *child)
{
	/* depth 累计父跳数，parent 保存每轮读取结果。 */
	int depth = 0;
	struct lock_list *parent;

	/* 每次上移一层，直到 __bfs_init_root() 设置的 NULL 哨兵。 */
	while ((parent = get_lock_parent(child))) {
		child = parent;
		depth++;
	}
	return depth;
}

/*
 * Return the forward or backward dependency list.
 *
 * @lock:   the lock_list to get its class's dependency list
 * @offset: the offset to struct lock_class to determine whether it is
 *          locks_after or locks_before
 */
/*
 * 返回正向或反向依赖链表：@lock 是待取其 class 依赖链的边记录；@offset 是 struct lock_class 内
 * locks_after 或 locks_before 字段偏移，用同一 BFS 核心选择遍历方向。
 */
/**
 * get_dep_list - 通过字段偏移选择 lock_list.class 的依赖链头
 * @lock: 含有效 class 指针的依赖记录
 * @offset: offsetof(struct lock_class, locks_after) 或 locks_before
 *
 * 返回：对应 list_head 的借用指针。
 * 边界：依赖内核支持的 void 指针字节偏移；offset 只由受控包装函数传入，任意值会产生无效地址。
 */
static inline struct list_head *get_dep_list(struct lock_list *lock, int offset)
{
	/* 先保留无类型 class 基址，再按字节字段偏移定位目标链表头。 */
	void *lock_class = lock->class;

	return lock_class + offset;
}
/*
 * Return values of a bfs search:
 *
 * BFS_E* indicates an error
 * BFS_R* indicates a result (match or not)
 *
 * BFS_EINVALIDNODE: Find a invalid node in the graph.
 *
 * BFS_EQUEUEFULL: The queue is full while doing the bfs.
 *
 * BFS_RMATCH: Find the matched node in the graph, and put that node into
 *             *@target_entry.
 *
 * BFS_RNOMATCH: Haven't found the matched node and keep *@target_entry
 *               _unchanged_.
 */
/*
 * BFS 搜索返回值：BFS_E* 为错误，BFS_R* 为正常结果。EINVALIDNODE 表示图中遇到无效节点；
 * EQUEUEFULL 表示遍历队列容量耗尽；RMATCH 表示找到目标并写入 *@target_entry；RNOMATCH 表示没有
 * 找到且保持 *@target_entry 原值不变。负数/非负数划分供 bfs_error() 统一判断。
 */
enum bfs_result {
	/* 图节点不满足有效性约束。 */
	BFS_EINVALIDNODE = -2,
	/* 环形队列无法再容纳待访问节点。 */
	BFS_EQUEUEFULL = -1,
	/* 成功找到匹配节点。 */
	BFS_RMATCH = 0,
	/* 正常完成搜索但没有匹配。 */
	BFS_RNOMATCH = 1,
};

/*
 * bfs_result < 0 means error
 */
/* bfs_result 小于零表示搜索基础设施或图状态错误。 */
/**
 * bfs_error - 判断 BFS 返回值是否属于错误类别
 * @res: enum bfs_result 结果
 *
 * 返回：负值为 true，RMATCH/RNOMATCH 为 false；纯分类，不修改搜索状态。
 */
static inline bool bfs_error(enum bfs_result res)
{
	return res < 0;
}

/*
 * DEP_*_BIT in lock_list::dep
 *
 * For dependency @prev -> @next:
 *
 *   SR: @prev is shared reader (->read != 0) and @next is recursive reader
 *       (->read == 2)
 *   ER: @prev is exclusive locker (->read == 0) and @next is recursive reader
 *   SN: @prev is shared reader and @next is non-recursive locker (->read != 2)
 *   EN: @prev is exclusive locker and @next is non-recursive locker
 *
 * Note that we define the value of DEP_*_BITs so that:
 *   bit0 is prev->read == 0
 *   bit1 is next->read != 2
 */
/*
 * lock_list.dep 中的 DEP_* 位描述依赖 prev->next 的读写组合：SR 为 prev 共享读且 next 递归读；ER
 * 为 prev 独占且 next 递归读；SN 为 prev 共享读且 next 非递归获取；EN 为 prev 独占且 next 非递归
 * 获取。编号特意让 bit0 表示 prev->read==0，bit1 表示 next->read!=2，从两个布尔量直接组成 0..3。
 */
/* 四种组合在 dep 掩码中的位编号。 */
#define DEP_SR_BIT (0 + (0 << 1)) /* 0 */
#define DEP_ER_BIT (1 + (0 << 1)) /* 1 */
#define DEP_SN_BIT (0 + (1 << 1)) /* 2 */
#define DEP_EN_BIT (1 + (1 << 1)) /* 3 */

/* 把组合编号转换为可并存的单比特依赖掩码。 */
#define DEP_SR_MASK (1U << (DEP_SR_BIT))
#define DEP_ER_MASK (1U << (DEP_ER_BIT))
#define DEP_SN_MASK (1U << (DEP_SN_BIT))
#define DEP_EN_MASK (1U << (DEP_EN_BIT))

/**
 * __calc_dep_bit - 计算正向 prev->next 依赖的组合位编号
 * @prev: 已持有的前驱锁记录
 * @next: 将获取的后继锁记录
 *
 * 返回：以 prev 独占为低位、next 非递归为高位组成的 DEP_*_BIT，范围 0..3。
 */
static inline unsigned int
__calc_dep_bit(struct held_lock *prev, struct held_lock *next)
{
	return (prev->read == 0) + ((next->read != 2) << 1);
}

/**
 * calc_dep - 生成正向依赖记录使用的单比特 dep 掩码
 * @prev: 前驱 held lock
 * @next: 后继 held lock
 *
 * 返回：1 左移 __calc_dep_bit() 位，类型收窄为可存入 lock_list.dep 的 u8。
 */
static inline u8 calc_dep(struct held_lock *prev, struct held_lock *next)
{
	return 1U << __calc_dep_bit(prev, next);
}

/*
 * calculate the dep_bit for backwards edges. We care about whether @prev is
 * shared and whether @next is recursive.
 */
/* 为反向边计算 dep 位；反向遍历仍需保留原关系中 @prev 是否共享、@next 是否递归这两个事实。 */
/**
 * __calc_dep_bitb - 把原 prev->next 关系编码到反向边的组合位编号
 * @prev: 原正向关系的前驱 held lock
 * @next: 原正向关系的后继 held lock
 *
 * 返回：以 next 非递归为低位、prev 独占为高位组成的反向 DEP 位编号，范围 0..3。
 * 语义：交换两布尔量的位位置，使从 next 朝 prev 遍历时仍可用统一强路径规则解释方向。
 */
static inline unsigned int
__calc_dep_bitb(struct held_lock *prev, struct held_lock *next)
{
	return (next->read != 2) + ((prev->read == 0) << 1);
}

/**
 * calc_depb - 生成反向依赖镜像记录使用的单比特 dep 掩码
 * @prev: 原正向关系的前驱 held lock
 * @next: 原正向关系的后继 held lock
 *
 * 返回：1 左移 __calc_dep_bitb() 位后的 u8 掩码。
 */
static inline u8 calc_depb(struct held_lock *prev, struct held_lock *next)
{
	return 1U << __calc_dep_bitb(prev, next);
}

/*
 * Initialize a lock_list entry @lock belonging to @class as the root for a BFS
 * search.
 */
/* 将属于 @class 的临时 lock_list @lock 初始化为 BFS 根节点。 */
/**
 * __bfs_init_root - 建立不带读模式约束的 BFS 临时根
 * @lock: 调用者提供的临时 lock_list，将被原地初始化
 * @class: 搜索起始锁类，仅借用
 *
 * 返回：无；设置 class、以 NULL parent 标记根，并清除 only_xr。
 * 所有权：根记录通常位于调用者栈上，不会链接进依赖图；class 生命周期由调用者/图锁保证。
 */
static inline void __bfs_init_root(struct lock_list *lock,
				   struct lock_class *class)
{
	/* 根只携带起始 class 和路径状态，不代表真实已发布依赖边。 */
	lock->class = class;
	lock->parent = NULL;
	lock->only_xr = 0;
}

/*
 * Initialize a lock_list entry @lock based on a lock acquisition @hlock as the
 * root for a BFS search.
 *
 * ->only_xr of the initial lock node is set to @hlock->read == 2, to make sure
 * that <prev> -> @hlock and @hlock -> <whatever __bfs() found> is not -(*R)->
 * and -(S*)->.
 */
/*
 * 按一次 @hlock 获取初始化正向 BFS 根。根的 only_xr 设为 hlock->read==2，使合成的
 * <prev>->hlock 与 BFS 找到的 hlock->... 不会形成相邻 -(*R)-> -(S*)-> 弱路径。
 */
/**
 * bfs_init_root - 建立正向强依赖搜索根并注入当前获取的读模式
 * @lock: 调用者提供的临时根记录
 * @hlock: 起始 held lock；class_idx 必须有效
 *
 * 返回：无；根 class 来自 hlock_class()，递归读模式时 only_xr 为 true。
 */
static inline void bfs_init_root(struct lock_list *lock,
				 struct held_lock *hlock)
{
	__bfs_init_root(lock, hlock_class(hlock));
	lock->only_xr = (hlock->read == 2);
}

/*
 * Similar to bfs_init_root() but initialize the root for backwards BFS.
 *
 * ->only_xr of the initial lock node is set to @hlock->read != 0, to make sure
 * that <next> -> @hlock and @hlock -> <whatever backwards BFS found> is not
 * -(*S)-> and -(R*)-> (reverse order of -(*R)-> and -(S*)->).
 */
/*
 * 与 bfs_init_root() 类似，但用于反向 BFS。初始 only_xr 设为 hlock->read!=0，避免合成的
 * <next>->hlock 与反向找到的 hlock->... 形成 -(*S)-> -(R*)->；这是正向禁配序列的逆序表达。
 */
/**
 * bfs_init_rootb - 建立反向强依赖搜索根并注入当前获取的读模式
 * @lock: 调用者提供的临时根记录
 * @hlock: 起始 held lock；任意共享读模式都会令 only_xr 为 true
 *
 * 返回：无；其余根字段由 __bfs_init_root() 初始化。
 */
static inline void bfs_init_rootb(struct lock_list *lock,
				  struct held_lock *hlock)
{
	__bfs_init_root(lock, hlock_class(hlock));
	lock->only_xr = (hlock->read != 0);
}

/**
 * __bfs_next - 取得当前 BFS 边在同一父 class 依赖链中的下一兄弟边
 * @lock: 当前边；NULL 或根节点没有可推导的兄弟
 * @offset: 选择父 class 的 locks_after/locks_before 字段偏移
 *
 * 返回：同一 RCU 链中的下一 lock_list 借用指针；到链尾、输入 NULL 或根节点时为 NULL。
 * 作用：BFS 只把每条依赖链首项入队，其余兄弟通过本函数顺序发现，从而节省队列槽。
 */
static inline struct lock_list *__bfs_next(struct lock_list *lock, int offset)
{
	/* 根没有 parent 对应的依赖链，NULL 也不能继续做容器寻址。 */
	if (!lock || !lock->parent)
		return NULL;

	/* parent 的 class 决定兄弟链，当前 entry 是查找下一节点的位置锚点。 */
	return list_next_or_null_rcu(get_dep_list(lock->parent, offset),
				     &lock->entry, struct lock_list, entry);
}

/*
 * Breadth-First Search to find a strong path in the dependency graph.
 *
 * @source_entry: the source of the path we are searching for.
 * @data: data used for the second parameter of @match function
 * @match: match function for the search
 * @target_entry: pointer to the target of a matched path
 * @offset: the offset to struct lock_class to determine whether it is
 *          locks_after or locks_before
 *
 * We may have multiple edges (considering different kinds of dependencies,
 * e.g. ER and SN) between two nodes in the dependency graph. But
 * only the strong dependency path in the graph is relevant to deadlocks. A
 * strong dependency path is a dependency path that doesn't have two adjacent
 * dependencies as -(*R)-> -(S*)->, please see:
 *
 *         Documentation/locking/lockdep-design.rst
 *
 * for more explanation of the definition of strong dependency paths
 *
 * In __bfs(), we only traverse in the strong dependency path:
 *
 *     In lock_list::only_xr, we record whether the previous dependency only
 *     has -(*R)-> in the search, and if it does (prev only has -(*R)->), we
 *     filter out any -(S*)-> in the current dependency and after that, the
 *     ->only_xr is set according to whether we only have -(*R)-> left.
 */
/*
 * 在依赖图中用 BFS 查找强路径。@source_entry 是起点；@data 作为第二参数传给 @match/@skip；
 * @match 判定目标；可选 @skip 判定是否剪掉当前节点及其后继路径；命中节点写入 *@target_entry；
 * @offset 选择 locks_after 正向链或 locks_before 反向链。
 *
 * 同一对节点之间可因 ER、SN 等读写关系存在多种边，但死锁只关心强依赖路径。强路径不能含相邻的
 * -(*R)-> -(S*)->，完整定义见 Documentation/locking/lockdep-design.rst。only_xr 记录前一步过滤后
 * 是否只剩 -(*R)->；若是，本步先去掉全部 -(S*)-> 组合，再根据剩余组合更新 only_xr 传给下一步。
 */
/**
 * __bfs - 在正向或反向依赖图中查找满足回调的最短强路径
 * @source_entry: 调用者构造的临时根节点
 * @data: 原样传给 match/skip 的不透明上下文
 * @match: 必选匹配谓词，返回 true 即结束搜索
 * @skip: 可选剪枝谓词，返回 true 时不匹配也不展开该节点
 * @target_entry: 命中时接收图内 lock_list 借用指针；未命中和错误时保持不变
 * @offset: lock_class 中 locks_after 或 locks_before 的字段偏移
 *
 * 返回：BFS_RMATCH、BFS_RNOMATCH、BFS_EINVALIDNODE 或 BFS_EQUEUEFULL。
 * 上下文：调用者必须持有 graph_lock；函数复用全局 lock_cq 和 class generation 标记，不可并发执行。
 * 所有权：队列、parent 和 only_xr 只是搜索暂态；不会分配、删除或接管任何依赖图对象。
 */
static enum bfs_result __bfs(struct lock_list *source_entry,
			     void *data,
			     bool (*match)(struct lock_list *entry, void *data),
			     bool (*skip)(struct lock_list *entry, void *data),
			     struct lock_list **target_entry,
			     int offset)
{
	/* cq 指向全局压缩 BFS 队列；lock 是当前边，entry 遍历其 class 的邻接边。 */
	struct circular_queue *cq = &lock_cq;
	struct lock_list *lock = NULL;
	struct lock_list *entry;
	/* head 是当前方向的邻接链，cq_depth 用于历史峰值，first 选择每条链唯一入队项。 */
	struct list_head *head;
	unsigned int cq_depth;
	bool first;

	/* 搜索会写 class generation、edge parent/only_xr 和全局队列，必须由图锁串行化。 */
	lockdep_assert_locked();

	/* 新 generation 逻辑清空 visited 状态；空队列必能容纳临时根，故无需处理 enqueue 失败。 */
	__cq_init(cq);
	__cq_enqueue(cq, source_entry);

	/* 优先沿当前父链的下一兄弟边推进；兄弟耗尽后才从队列取下一条邻接链的首项。 */
	while ((lock = __bfs_next(lock, offset)) || (lock = __cq_dequeue(cq))) {
		/* 每个真实/临时节点都必须指向 class；缺失说明图或根构造已损坏。 */
		if (!lock->class)
			return BFS_EINVALIDNODE;

		/*
		 * Step 1: check whether we already finish on this one.
		 *
		 * If we have visited all the dependencies from this @lock to
		 * others (iow, if we have visited all lock_list entries in
		 * @lock->class->locks_{after,before}) we skip, otherwise go
		 * and visit all the dependencies in the list and mark this
		 * list accessed.
		 */
		/*
		 * 第 1 步：判断该 class 的目标方向邻接链是否已在本轮完整处理。已访问则跳过；否则立即按
		 * class 标记，随后只会有首条到达路径负责检查并展开其全部 locks_{after,before} 条目。
		 */
		if (lock_accessed(lock))
			continue;
		else
			mark_lock_accessed(lock);

		/*
		 * Step 2: check whether prev dependency and this form a strong
		 *         dependency path.
		 */
		/* 第 2 步：把父路径状态与当前 dep 组合，确认仍存在强依赖关系。 */
		if (lock->parent) { /* Parent exists, check prev dependency */
			/* 非根节点才有真实前一依赖；dep 是可过滤副本，prev_only_xr 来自父节点路径状态。 */
			u8 dep = lock->dep;
			bool prev_only_xr = lock->parent->only_xr;

			/*
			 * Mask out all -(S*)-> if we only have *R in previous
			 * step, because -(*R)-> -(S*)-> don't make up a strong
			 * dependency.
			 */
			/* 前一步只剩 -(*R)-> 时，去掉当前所有 -(S*)-> 位，因为二者相邻不构成强依赖。 */
			if (prev_only_xr)
				dep &= ~(DEP_SR_MASK | DEP_SN_MASK);

			/* If nothing left, we skip */
			/* 所有组合都被过滤后，该到达路径为弱路径，既不匹配也不向后扩展。 */
			if (!dep)
				continue;

			/* If there are only -(*R)-> left, set that for the next step */
			/* 剩余集合不含 SN/EN 即只剩 -(*R)->，把状态传给下一条边继续执行禁配规则。 */
			lock->only_xr = !(dep & (DEP_SN_MASK | DEP_EN_MASK));
		}

		/*
		 * Step 3: we haven't visited this and there is a strong
		 *         dependency path to this, so check with @match.
		 *         If @skip is provide and returns true, we skip this
		 *         lock (and any path this lock is in).
		 */
		/*
		 * 第 3 步：节点首次到达且路径仍强，先执行可选剪枝；skip 为 true 会连同经过该节点的所有后继
		 * 路径一起略过。未剪枝再调用 match，命中时发布目标边并立即返回最短层次结果。
		 */
		if (skip && skip(lock, data))
			continue;

		if (match(lock, data)) {
			/* 只在 RMATCH 路径写输出参数，RNOMATCH 和错误保持调用者原值。 */
			*target_entry = lock;
			return BFS_RMATCH;
		}

		/*
		 * Step 4: if not match, expand the path by adding the
		 *         forward or backwards dependencies in the search
		 *
		 */
		/* 第 4 步：未命中则选择当前方向邻接链，为所有边记录 parent，并把该链首项加入 BFS 队列。 */
		first = true;
		head = get_dep_list(lock, offset);
		list_for_each_entry_rcu(entry, head, entry) {
			/* 每个兄弟都指回当前节点，以便命中后重建路径及 __bfs_next() 找同链下一项。 */
			visit_lock_entry(entry, lock);

			/*
			 * Note we only enqueue the first of the list into the
			 * queue, because we can always find a sibling
			 * dependency from one (see __bfs_next()), as a result
			 * the space of queue is saved.
			 */
			/*
			 * 每条邻接链只把第一项入队；其余兄弟可由 __bfs_next() 从第一项顺序找到，从而显著节省
			 * 固定队列空间，同时仍保持按父节点层次扩展的 BFS 顺序。
			 */
			if (!first)
				continue;

			first = false;

			/* 首项入队失败说明固定环形队列满，无法保证搜索完整，立即返回显式错误。 */
			if (__cq_enqueue(cq, entry))
				return BFS_EQUEUEFULL;

			/* 成功入队后更新历史最大占用，帮助判断配置的队列位数是否充足。 */
			cq_depth = __cq_get_elem_count(cq);
			if (max_bfs_queue_depth < cq_depth)
				max_bfs_queue_depth = cq_depth;
		}
	}

	/* 队列及所有兄弟链均耗尽，未调用 match 成功；target_entry 按契约保持不变。 */
	return BFS_RNOMATCH;
}

/**
 * __bfs_forwards - 沿 locks_after 方向执行强依赖 BFS
 * @src_entry: 临时搜索根
 * @data: 传给 match/skip 的上下文
 * @match: 必选目标谓词
 * @skip: 可选剪枝谓词
 * @target_entry: 命中输出，其他结果保持原值
 *
 * 返回：原样传播 __bfs() 的 enum bfs_result；调用者须持有 graph_lock。
 */
static inline enum bfs_result
__bfs_forwards(struct lock_list *src_entry,
	       void *data,
	       bool (*match)(struct lock_list *entry, void *data),
	       bool (*skip)(struct lock_list *entry, void *data),
	       struct lock_list **target_entry)
{
	/* locks_after 表示从当前 class 沿已知“之后获取”关系前进。 */
	return __bfs(src_entry, data, match, skip, target_entry,
		     offsetof(struct lock_class, locks_after));

}

/**
 * __bfs_backwards - 沿 locks_before 方向执行强依赖 BFS
 * @src_entry: 临时搜索根
 * @data: 传给 match/skip 的上下文
 * @match: 必选目标谓词
 * @skip: 可选剪枝谓词
 * @target_entry: 命中输出，其他结果保持原值
 *
 * 返回：原样传播 __bfs() 的 enum bfs_result；调用者须持有 graph_lock。
 */
static inline enum bfs_result
__bfs_backwards(struct lock_list *src_entry,
		void *data,
		bool (*match)(struct lock_list *entry, void *data),
	       bool (*skip)(struct lock_list *entry, void *data),
		struct lock_list **target_entry)
{
	/* locks_before 表示从当前 class 逆着已知获取顺序寻找前驱。 */
	return __bfs(src_entry, data, match, skip, target_entry,
		     offsetof(struct lock_class, locks_before));

}

/**
 * print_lock_trace - 打印一条已保存的 lockdep 调用栈
 * @trace: stack_trace[] 池内有效 lock_trace，仅借用
 * @spaces: 每行符号前的缩进空格数
 *
 * 返回：无；把 entries/nr_entries 转交通用 stack_trace_print()，不修改或释放记录。
 */
static void print_lock_trace(const struct lock_trace *trace,
			     unsigned int spaces)
{
	stack_trace_print(trace->entries, trace->nr_entries, spaces);
}

/*
 * Print a dependency chain entry (this is only done when a deadlock
 * has been detected):
 */
/* 打印依赖链中的一个节点；仅在已经确认死锁风险后调用。 */
/**
 * print_circular_bug_entry - 打印环路证据中的一个锁类及其来源栈
 * @target: 路径中的有效 lock_list，须含 class 和 trace
 * @depth: 该节点在诊断链中的编号
 *
 * 返回：无；debug_locks_silent 时不输出，否则打印名称和保存的依赖 trace。
 * 上下文：noinline 保留清晰诊断栈；调用方已冻结 lockdep 并进入适合 printk 的报告阶段。
 */
static noinline void
print_circular_bug_entry(struct lock_list *target, int depth)
{
	/* 静默模式保留检测/关闭行为，但抑制全部详细报告。 */
	if (debug_locks_silent)
		return;
	/* 名称不绑定具体 held instance，随后以六空格缩进打印建立该依赖边的调用栈。 */
	printk("\n-> #%u", depth);
	print_lock_name(NULL, target->class);
	printk(KERN_CONT ":\n");
	print_lock_trace(target->trace, 6);
}

/**
 * print_circular_lock_scenario - 用两 CPU 交错顺序解释已检测到的环路
 * @src: 当前任务正尝试获取的 A 锁记录
 * @tgt: 当前任务已持有的 B 锁记录
 * @prt: 既有 A->...->B 路径中 B 的父边，必须非 NULL
 *
 * 返回：无；必要时先打印中间依赖链摘要，再按 src/tgt 读写与 sync 模式画出潜在死锁顺序。
 * 所有权：只读取 held-lock、父边和 class；调用者已冻结图并负责控制台报告上下文。
 */
static void
print_circular_lock_scenario(struct held_lock *src,
			     struct held_lock *tgt,
			     struct lock_list *prt)
{
	/* source 是当前尝试获取的 A，target 是已持有的 B，parent 是既有 A->...->B 路径上 B 的前驱。 */
	struct lock_class *source = hlock_class(src);
	struct lock_class *target = hlock_class(tgt);
	struct lock_class *parent = prt->class;
	int src_read = src->read;
	int tgt_read = tgt->read;

	/*
	 * A direct locking problem where unsafe_class lock is taken
	 * directly by safe_class lock, then all we need to show
	 * is the deadlock scenario, as it is obvious that the
	 * unsafe lock is taken under the safe lock.
	 *
	 * But if there is a chain instead, where the safe lock takes
	 * an intermediate lock (middle_class) where this lock is
	 * not the same as the safe lock, then the lock chain is
	 * used to describe the problem. Otherwise we would need
	 * to show a different CPU case for each link in the chain
	 * from the safe_class lock to the unsafe_class lock.
	 */
	/*
	 * 若 unsafe_class 直接在 safe_class 下获取，只需展示两 CPU 死锁场景，关系已很直观。若中间还
	 * 经过 middle_class，先打印 safe->middle->unsafe 的链摘要；否则就得为 safe_class 到
	 * unsafe_class 的每一条中间边分别画一个 CPU 场景，反而掩盖主因。
	 */
	/* parent 等于 source 表示直接边；不同则存在至少一个值得展示的中间关系。 */
	if (parent != source) {
		printk("Chain exists of:\n  ");
		__print_lock_name(src, source);
		printk(KERN_CONT " --> ");
		__print_lock_name(NULL, parent);
		printk(KERN_CONT " --> ");
		__print_lock_name(tgt, target);
		printk(KERN_CONT "\n\n");
	}

	/* 用 CPU0/CPU1 的交错获取顺序把抽象环路还原成潜在等待闭环。 */
	printk(" Possible unsafe locking scenario:\n\n");
	printk("       CPU0                    CPU1\n");
	printk("       ----                    ----\n");
	/* CPU0 先持有 target；按实际 read 模式显示 rlock 或 lock。 */
	if (tgt_read != 0)
		printk("  rlock(");
	else
		printk("  lock(");
	__print_lock_name(tgt, target);
	printk(KERN_CONT ");\n");
	/* CPU1 依次获取 target 的路径前驱和 target，表示既有依赖方向。 */
	printk("                               lock(");
	__print_lock_name(NULL, parent);
	printk(KERN_CONT ");\n");
	printk("                               lock(");
	__print_lock_name(tgt, target);
	printk(KERN_CONT ");\n");
	/* CPU0 再尝试当前 source；读、sync 注解和普通独占模式分别标示。 */
	if (src_read != 0)
		printk("  rlock(");
	else if (src->sync)
		printk("  sync(");
	else
		printk("  lock(");
	__print_lock_name(src, source);
	printk(KERN_CONT ");\n");
	printk("\n *** DEADLOCK ***\n\n");
}

/*
 * When a circular dependency is detected, print the
 * header first:
 */
/* 检测到环形依赖时先打印报告头、当前获取和已持有锁，再开始逆序展示既有依赖链。 */
/**
 * print_circular_bug_header - 输出环路报告标题和依赖链首项
 * @entry: BFS 命中的路径末端锁记录
 * @depth: 从临时根到 entry 的父链深度
 * @check_src: 当前任务正尝试获取的 held-lock 描述
 * @check_tgt: 当前任务已经持有、将形成新边的 held-lock 描述
 *
 * 返回：无；静默模式不输出。函数只打印报告前半部，余下父链由 print_circular_bug() 继续。
 */
static noinline void
print_circular_bug_header(struct lock_list *entry, unsigned int depth,
			struct held_lock *check_src,
			struct held_lock *check_tgt)
{
	/* 报告归属当前任务；这里只借用 current。 */
	struct task_struct *curr = current;

	/* 静默模式下检测状态仍已处理，但日志正文省略。 */
	if (debug_locks_silent)
		return;

	pr_warn("\n");
	pr_warn("======================================================\n");
	pr_warn("WARNING: possible circular locking dependency detected\n");
	print_kernel_ident();
	pr_warn("------------------------------------------------------\n");
	pr_warn("%s/%d is trying to acquire lock:\n",
		curr->comm, task_pid_nr(curr));
	print_lock(check_src);

	pr_warn("\nbut task is already holding lock:\n");

	print_lock(check_tgt);
	pr_warn("\nwhich lock already depends on the new lock.\n\n");
	pr_warn("\nthe existing dependency chain (in reverse order) is:\n");

	/* 先打印 BFS 命中端，调用者随后沿 parent 逆序走回根。 */
	print_circular_bug_entry(entry, depth);
}

/*
 * We are about to add B -> A into the dependency graph, and in __bfs() a
 * strong dependency path A -> .. -> B is found: hlock_class equals
 * entry->class.
 *
 * We will have a deadlock case (conflict) if A -> .. -> B -> A is a strong
 * dependency cycle, that means:
 *
 * Either
 *
 *     a) B -> A is -(E*)->
 *
 * or
 *
 *     b) A -> .. -> B is -(*N)-> (i.e. A -> .. -(*N)-> B)
 *
 * as then we don't have -(*R)-> -(S*)-> in the cycle.
 */
/*
 * 即将向图加入 B->A，而 BFS 已找到强路径 A->...->B（hlock 的 class 等于 entry->class）。只有闭合
 * 后的 A->...->B->A 仍是强环才算冲突：或者新边 B->A 属于 -(E*)->，或者既有路径末端属于
 * -(*N)->；这两种情况都不会在环中形成会削弱依赖的相邻 -(*R)-> -(S*)->。
 */
/**
 * hlock_conflict - 判断 BFS 到达的同类节点能否与待加边组成强环
 * @entry: BFS 当前路径节点，only_xr 描述既有 A->...->B 的末端约束
 * @data: 指向代表 B 的 held_lock，由 check_path() 传入
 *
 * 返回：class 匹配且 B->A 为独占前驱，或既有路径不只含 *R 末端时为 true，否则 false。
 * 副作用：纯谓词；hlock_class() 只借用已注册 class。
 */
static inline bool hlock_conflict(struct lock_list *entry, void *data)
{
	/* data 的动态类型由 check_noncircular() 与 check_path() 调用契约固定。 */
	struct held_lock *hlock = (struct held_lock *)data;

	/* 三个行尾原文分别表达：找到 B、待加边为 E*、既有路径为 *N。 */
	return hlock_class(hlock) == entry->class && /* Found A -> .. -> B */
	       /* 已找到 A->...->B 的末端 B。 */
	       (hlock->read == 0 || /* B -> A is -(E*)-> */
		/* 新边 B->A 的前驱是独占获取，即 -(E*)->。 */
		!entry->only_xr); /* A -> .. -> B is -(*N)-> */
		/* 或既有 A->...->B 路径末端不是 only_xr，即保留 -(*N)->。 */
}

/**
 * print_circular_bug - 冻结 lockdep 并输出完整环形依赖报告
 * @this: 本次搜索的临时根记录 A，将补存当前报告 trace
 * @target: BFS 命中的 B 路径记录，parent 链可逆序回到 @this
 * @check_src: 当前正尝试获取的 A held-lock
 * @check_tgt: 当前已持有、将产生 B->A 新边的 B held-lock
 *
 * 返回：无；进入时调用者持有 graph_lock。函数首先永久关闭 debug_locks 并释放图锁；只有首个且
 * 非静默报告者继续保存 trace、进入控制台紧急区并打印标题、既有路径、两 CPU 场景、held locks
 * 与当前栈。任何返回路径都不再持有图锁。
 */
static noinline void print_circular_bug(struct lock_list *this,
				struct lock_list *target,
				struct held_lock *check_src,
				struct held_lock *check_tgt)
{
	/* curr 归属报告任务；parent/first_parent 回溯 BFS 路径，depth 是尚待打印的层号。 */
	struct task_struct *curr = current;
	struct lock_list *parent;
	struct lock_list *first_parent;
	int depth;

	/* 即使静默也先关闭验证并释放图锁；只有首次关闭且允许输出的调用者成为报告者。 */
	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return;

	/* 图已冻结后为临时根记录当前触发现场；失败则无法安全组成完整报告，直接结束。 */
	this->trace = save_trace();
	if (!this->trace)
		return;

	/* target 的 parent 链长度决定报告编号，根深度为零。 */
	depth = get_lock_depth(target);

	/* 后续大量 printk/dump_stack 在控制台紧急区内连续输出。 */
	nbcon_cpu_emergency_enter();

	/* 标题先打印命中端 target。 */
	print_circular_bug_header(target, depth, check_src, check_tgt);

	/* 保存第一父边供两 CPU 场景使用，再沿 parent 逐级逆序打印到根。 */
	parent = get_lock_parent(target);
	first_parent = parent;

	while (parent) {
		/* 向根移动一层便递减显示深度。 */
		print_circular_bug_entry(parent, --depth);
		parent = get_lock_parent(parent);
	}

	printk("\nother info that might help us debug this:\n\n");
	print_circular_lock_scenario(check_src, check_tgt,
				     first_parent);

	/* 同一报告附上当前任务仍持有的全部锁，帮助关联新边来源。 */
	lockdep_print_held_locks(curr);

	/* 最后打印检测现场调用栈，而前面的每条边 trace 说明历史依赖来源。 */
	printk("\nstack backtrace:\n");
	dump_stack();

	nbcon_cpu_emergency_exit();
}

/**
 * print_bfs_bug - 关闭 lockdep 并报告 BFS 基础设施错误
 * @ret: 负值 enum bfs_result，通常为无效节点或队列已满
 *
 * 返回：无；进入时持有 graph_lock。首个报告者关闭调试并释放图锁，队列满时额外建议增大
 * LOCKDEP_CIRCULAR_QUEUE_BITS，随后 WARN 输出错误码；非首个关闭者静默返回。
 */
static noinline void print_bfs_bug(int ret)
{
	/* 组合 helper 保证报告前离开图锁，避免 WARN/printk 路径递归死锁。 */
	if (!debug_locks_off_graph_unlock())
		return;

	/*
	 * Breadth-first-search failed, graph got corrupted?
	 */
	/* 广度优先搜索失败：可能是队列配置不足，也可能是依赖图节点已经损坏。 */
	/* 队列满属于可调容量问题，先给出明确的 Kconfig 调优方向。 */
	if (ret == BFS_EQUEUEFULL)
		pr_warn("Increase LOCKDEP_CIRCULAR_QUEUE_BITS to avoid this warning:\n");

	WARN(1, "lockdep bfs error:%d\n", ret);
}

/**
 * noop_count - BFS 匹配回调：只计数、永不命中
 * @entry: 当前访问的依赖记录；计数逻辑无需读取
 * @data: 指向 unsigned long 计数器
 *
 * 返回：固定 false，使 BFS 遍历完整个可达强依赖子图；副作用是把计数器加一。
 */
static bool noop_count(struct lock_list *entry, void *data)
{
	(*(unsigned long *)data)++;
	return false;
}

/**
 * __lockdep_count_forward_deps - 在已持图锁时统计正向强依赖可达节点
 * @this: 已初始化的 BFS 根记录
 *
 * 返回：noop_count() 被调用的次数；由于根也进入匹配阶段，计数包含根 class 本身。
 * 边界：忽略 __bfs_forwards() 的返回码，队列错误会得到已遍历前缀计数；仅供诊断统计。
 */
static unsigned long __lockdep_count_forward_deps(struct lock_list *this)
{
	/* count 由回调累加；target_entry 因回调永不命中而不会被读取。 */
	unsigned long  count = 0;
	struct lock_list *target_entry;

	/* 无 skip，沿 locks_after 遍历全部强依赖可达 class。 */
	__bfs_forwards(this, (void *)&count, noop_count, NULL, &target_entry);

	return count;
}
/**
 * lockdep_count_forward_deps - 线程安全地统计一个锁类的正向强依赖可达节点
 * @class: 搜索起点 lock_class，仅借用
 *
 * 返回：包含起点本身的诊断计数。
 * 上下文：保存并关闭本地 IRQ，直接获取内部 lockdep_lock 稳定图，统计后按逆序恢复；不永久缓存结果。
 */
unsigned long lockdep_count_forward_deps(struct lock_class *class)
{
	/* ret 保存锁内统计值，flags 保存 IRQ，this 是不链接进图的临时根。 */
	unsigned long ret, flags;
	struct lock_list this;

	/* 根只需 class 身份，不注入 held-lock 读模式。 */
	__bfs_init_root(&this, class);

	/* BFS 复用全局队列和 generation，必须在 IRQ-off 的内部图锁下执行。 */
	raw_local_irq_save(flags);
	lockdep_lock();
	ret = __lockdep_count_forward_deps(&this);
	lockdep_unlock();
	raw_local_irq_restore(flags);

	return ret;
}

/**
 * __lockdep_count_backward_deps - 在已持图锁时统计反向强依赖可达节点
 * @this: 已初始化的 BFS 根记录
 *
 * 返回：沿 locks_before 访问并由 noop_count() 累加的节点数，包含根本身。
 * 边界：与正向版本相同，错误时返回已完成前缀的诊断计数。
 */
static unsigned long __lockdep_count_backward_deps(struct lock_list *this)
{
	/* target_entry 仅满足 BFS 接口，noop_count 永不写入它。 */
	unsigned long  count = 0;
	struct lock_list *target_entry;

	__bfs_backwards(this, (void *)&count, noop_count, NULL, &target_entry);

	return count;
}

/**
 * lockdep_count_backward_deps - 线程安全地统计一个锁类的反向强依赖可达节点
 * @class: 搜索起点 lock_class，仅借用
 *
 * 返回：包含起点本身的反向可达诊断计数。
 * 上下文：以 raw_local_irq_save()+lockdep_lock() 串行全局 BFS，返回前恢复锁与 IRQ 状态。
 */
unsigned long lockdep_count_backward_deps(struct lock_class *class)
{
	/* ret/flags/this 分别保存结果、IRQ 状态和临时 BFS 根。 */
	unsigned long ret, flags;
	struct lock_list this;

	__bfs_init_root(&this, class);

	raw_local_irq_save(flags);
	lockdep_lock();
	ret = __lockdep_count_backward_deps(&this);
	lockdep_unlock();
	raw_local_irq_restore(flags);

	return ret;
}

/*
 * Check that the dependency graph starting at <src> can lead to
 * <target> or not.
 */
/* 检查从 <src> 出发的依赖图是否能到达 <target>。 */
/**
 * check_path - 用给定匹配/剪枝谓词执行正向强路径检查
 * @target: 作为 data 传给 match/skip 的目标 held-lock
 * @src_entry: 已初始化的 BFS 根
 * @match: 必选匹配谓词
 * @skip: 可选剪枝谓词
 * @target_entry: 命中时接收路径末端
 *
 * 返回：__bfs_forwards() 的结果。RMATCH/RNOMATCH 返回时仍持有调用者的 graph_lock；错误结果会先
 * 调用 print_bfs_bug()，其关闭 lockdep 并释放图锁，因此调用者不得再按正常路径解锁。
 */
static noinline enum bfs_result
check_path(struct held_lock *target, struct lock_list *src_entry,
	   bool (*match)(struct lock_list *entry, void *data),
	   bool (*skip)(struct lock_list *entry, void *data),
	   struct lock_list **target_entry)
{
	/* ret 同时携带正常匹配状态和需要终止验证的 BFS 错误。 */
	enum bfs_result ret;

	/* 正向遍历时 opaque data 固定为 target held-lock。 */
	ret = __bfs_forwards(src_entry, target, match, skip, target_entry);

	/* BFS 基础错误不是普通“无路径”，必须关闭验证并在锁外报告。 */
	if (unlikely(bfs_error(ret)))
		print_bfs_bug(ret);

	return ret;
}

/**
 * print_deadlock_bug - 输出同一锁类直接递归死锁报告（前置声明）
 * @curr: 当前任务
 * @prev: 已持有的同类冲突锁记录
 * @next: 当前尝试获取的新锁记录
 *
 * 实际定义位于后文；终止报告路径会关闭 lockdep 并释放 graph_lock。
 */
static void print_deadlock_bug(struct task_struct *, struct held_lock *, struct held_lock *);

/*
 * Prove that the dependency graph starting at <src> can not
 * lead to <target>. If it can, there is a circle when adding
 * <target> -> <src> dependency.
 *
 * Print an error and return BFS_RMATCH if it does.
 */
/*
 * 证明从 <src> 出发的依赖图不能到达 <target>；若能到达，再加入 <target>-><src> 就会闭环。
 * 检出时打印错误并返回 BFS_RMATCH。
 */
/**
 * check_noncircular - 在提交新依赖前证明它不会形成强环
 * @src: 将作为新边后继的 held lock A，也是正向 BFS 起点
 * @target: 已持有的前驱 held lock B；拟新增 B->A
 * @trace: 调用者持有的 trace 指针槽；NULL 时在冲突报告前尝试保存现场
 *
 * 返回：无强路径为 BFS_RNOMATCH；确认环路为 BFS_RMATCH；基础设施错误返回负值。
 * 锁状态：进入时持有 graph_lock。RNOMATCH 保持图锁；错误由 check_path() 释放；RMATCH 进入直接
 * deadlock 或 circular 终止报告并释放/冻结图。调用者必须按返回类别传播这一所有权差异。
 */
static noinline enum bfs_result
check_noncircular(struct held_lock *src, struct held_lock *target,
		  struct lock_trace **const trace)
{
	/* ret 是 BFS 结果，target_entry 接收 B 节点，src_entry 是不发布的 A 临时根。 */
	enum bfs_result ret;
	struct lock_list *target_entry;
	struct lock_list src_entry;

	/* 把 A 的 class 和递归读属性注入正向强路径根。 */
	bfs_init_root(&src_entry, src);

	/* 统计实际执行的环路证明次数。 */
	debug_atomic_inc(nr_cyclic_checks);

	/* 查找 A->...->B 且与拟加 B->A 一起仍构成强环的路径。 */
	ret = check_path(target, &src_entry, hlock_conflict, NULL, &target_entry);

	/* 只有真正强冲突才生成环路正文；错误已由 check_path() 报告。 */
	if (unlikely(ret == BFS_RMATCH)) {
		if (!*trace) {
			/*
			 * If save_trace fails here, the printing might
			 * trigger a WARN but because of the !nr_entries it
			 * should not do bad things.
			 */
			/*
			 * 若此处 save_trace 失败，后续打印可能再触发 WARN；但候选记录没有 nr_entries，原实现预期
			 * 不会继续做有害的越界栈处理。保留报告流程以尽量输出已知环路信息。
			 */
			*trace = save_trace();
		}

		/* 同一 class 的自依赖走直接死锁报告；不同 class 才需打印完整 BFS 环路链。 */
		if (src->class_idx == target->class_idx)
			print_deadlock_bug(current, src, target);
		else
			print_circular_bug(&src_entry, target_entry, src, target);
	}

	return ret;
}

#ifdef CONFIG_TRACE_IRQFLAGS

/*
 * Forwards and backwards subgraph searching, for the purposes of
 * proving that two subgraphs can be connected by a new dependency
 * without creating any illegal irq-safe -> irq-unsafe lock dependency.
 *
 * A irq safe->unsafe deadlock happens with the following conditions:
 *
 * 1) We have a strong dependency path A -> ... -> B
 *
 * 2) and we have ENABLED_IRQ usage of B and USED_IN_IRQ usage of A, therefore
 *    irq can create a new dependency B -> A (consider the case that a holder
 *    of B gets interrupted by an irq whose handler will try to acquire A).
 *
 * 3) the dependency circle A -> ... -> B -> A we get from 1) and 2) is a
 *    strong circle:
 *
 *      For the usage bits of B:
 *        a) if A -> B is -(*N)->, then B -> A could be any type, so any
 *           ENABLED_IRQ usage suffices.
 *        b) if A -> B is -(*R)->, then B -> A must be -(E*)->, so only
 *           ENABLED_IRQ_*_READ usage suffices.
 *
 *      For the usage bits of A:
 *        c) if A -> B is -(E*)->, then B -> A could be any type, so any
 *           USED_IN_IRQ usage suffices.
 *        d) if A -> B is -(S*)->, then B -> A must be -(*N)->, so only
 *           USED_IN_IRQ_*_READ usage suffices.
 */
/*
 * 正反向子图搜索用于证明：给两个子图增加新依赖后，不会连出非法的 irq-safe->irq-unsafe 关系。
 * 发生此类死锁需要同时满足：一，已有强路径 A->...->B；二，B 曾在 IRQ 开启时使用而 A 曾在 IRQ
 * 上下文使用，于是持 B 时被 IRQ 打断并在处理程序获取 A，可隐式形成 B->A；三，闭合后的环仍为
 * 强环。对 B 而言，A->B 为 *N 时任意 ENABLED_IRQ 使用都够，为 *R 时反向边须为 E*，需更严格的
 * ENABLED_IRQ_*_READ 配对；对 A 而言，A->B 为 E* 时任意 USED_IN_IRQ 使用都够，为 S* 时反向边
 * 须为 *N，需对应的 USED_IN_IRQ_*_READ 配对。后续 helper 用 only_xr 和 usage 掩码落实这些筛选。
 */

/*
 * There is a strong dependency path in the dependency graph: A -> B, and now
 * we need to decide which usage bit of A should be accumulated to detect
 * safe->unsafe bugs.
 *
 * Note that usage_accumulate() is used in backwards search, so ->only_xr
 * stands for whether A -> B only has -(S*)-> (in this case ->only_xr is true).
 *
 * As above, if only_xr is false, which means A -> B has -(E*)-> dependency
 * path, any usage of A should be considered. Otherwise, we should only
 * consider _READ usage.
 */
/*
 * 已有强路径 A->B，需要决定累计 A 的哪些 usage 位来检测 safe->unsafe。usage_accumulate() 用于反向
 * 搜索，此时 only_xr 表示 A->B 是否只含 -(S*)->。原文称 only_xr 时“只考虑 _READ usage”；当前
 * 实际代码用 LOCKF_IRQ 掩码去掉带 `_READ` 后缀的位，学习和排障必须以该可执行行为为准。
 */
/**
 * usage_accumulate - 在反向 BFS 中累计可参与 IRQ 反转的起点 usage
 * @entry: 当前可达 lock_list，其 only_xr 描述反向强路径约束
 * @mask: 指向调用者的 unsigned long 累计掩码
 *
 * 返回：固定 false，使 BFS 继续遍历；副作用是 OR 入当前 class 的 usage 位。
 * 筛选：only_xr 为 false 时累计全部 usage_mask；为 true 时按当前实现仅保留 LOCKF_IRQ 非 READ 集合。
 */
static inline bool usage_accumulate(struct lock_list *entry, void *mask)
{
	/* 存在 E* 路径时，当前 class 的全部 usage 都可能参与冲突。 */
	if (!entry->only_xr)
		*(unsigned long *)mask |= entry->class->usage_mask;
	else /* Mask out _READ usage bits */
		/* 去掉 `_READ` usage 位，只累计 LOCKF_ENABLED_IRQ/LOCKF_USED_IN_IRQ。 */
		*(unsigned long *)mask |= (entry->class->usage_mask & LOCKF_IRQ);

	return false;
}

/*
 * There is a strong dependency path in the dependency graph: A -> B, and now
 * we need to decide which usage bit of B conflicts with the usage bits of A,
 * i.e. which usage bit of B may introduce safe->unsafe deadlocks.
 *
 * As above, if only_xr is false, which means A -> B has -(*N)-> dependency
 * path, any usage of B should be considered. Otherwise, we should only
 * consider _READ usage.
 */
/*
 * 已有强路径 A->B，需要判断 B 的哪些 usage 与 A 的累计掩码冲突。原文称 only_xr 时“只考虑
 * _READ usage”；与上一个 helper 一样，当前代码实际通过 LOCKF_IRQ 去掉 `_READ` 后缀位。
 */
/**
 * usage_match - 判断正/反向 BFS 当前 class 是否命中目标 IRQ usage 掩码
 * @entry: 当前 lock_list，only_xr 描述路径末端是否只剩受限关系
 * @mask: 指向待匹配 unsigned long usage 掩码
 *
 * 返回：允许考虑的 class usage 与目标掩码有交集时为 true；无副作用。
 */
static inline bool usage_match(struct lock_list *entry, void *mask)
{
	/* 非 only_xr 路径允许 class 的全部 usage 位参与匹配。 */
	if (!entry->only_xr)
		return !!(entry->class->usage_mask & *(unsigned long *)mask);
	else /* Mask out _READ usage bits */
		/* only_xr 路径按当前实现先去掉 `_READ` usage 位再求交集。 */
		return !!((entry->class->usage_mask & LOCKF_IRQ) & *(unsigned long *)mask);
}

/**
 * usage_skip - 决定 IRQ 反转 BFS 是否剪掉特殊 lockdep 节点
 * @entry: 当前 lock_list
 * @mask: 为适配通用 skip 回调保留，本函数不读取
 *
 * 返回：普通锁为 false；合法 local_lock/percpu 和 WAIT_OVERRIDE 等非普通节点为 true；若 percpu 锁
 * 的 wait_type_inner 违反预期则 WARN 并返回 false，让异常节点保留在搜索中。
 */
static inline bool usage_skip(struct lock_list *entry, void *mask)
{
	/* 普通锁承载真实依赖，不得剪枝。 */
	if (entry->class->lock_type == LD_LOCK_NORMAL)
		return false;

	/*
	 * Skip local_lock() for irq inversion detection.
	 *
	 * For !RT, local_lock() is not a real lock, so it won't carry any
	 * dependency.
	 *
	 * For RT, an irq inversion happens when we have lock A and B, and on
	 * some CPU we can have:
	 *
	 *	lock(A);
	 *	<interrupted>
	 *	  lock(B);
	 *
	 * where lock(B) cannot sleep, and we have a dependency B -> ... -> A.
	 *
	 * Now we prove local_lock() cannot exist in that dependency. First we
	 * have the observation for any lock chain L1 -> ... -> Ln, for any
	 * 1 <= i <= n, Li.inner_wait_type <= L1.inner_wait_type, otherwise
	 * wait context check will complain. And since B is not a sleep lock,
	 * therefore B.inner_wait_type >= 2, and since the inner_wait_type of
	 * local_lock() is 3, which is greater than 2, therefore there is no
	 * way the local_lock() exists in the dependency B -> ... -> A.
	 *
	 * As a result, we will skip local_lock(), when we search for irq
	 * inversion bugs.
	 */
	/*
	 * IRQ 反转检测跳过 local_lock()。非 RT 下它不是真实锁，不承载依赖。RT 下反转要求某 CPU 先
	 * lock(A)，被中断后获取不可睡眠 B，并已有 B->...->A。任意链 L1->...->Ln 都必须满足每个
	 * Li.inner_wait_type <= L1.inner_wait_type，否则 wait-context 检查会先报警；B 不可睡眠故其 inner
	 * wait type 至少为 2，而 local_lock() 为 3，不可能出现在 B->...->A 中。因此搜索 IRQ 反转时可
	 * 安全剪掉它。
	 */
	/* percpu/local_lock 的等待类型若低于 LD_WAIT_CONFIG，先报警并不要基于上述证明剪枝。 */
	if (entry->class->lock_type == LD_LOCK_PERCPU &&
	    DEBUG_LOCKS_WARN_ON(entry->class->wait_type_inner < LD_WAIT_CONFIG))
		return false;

	/*
	 * Skip WAIT_OVERRIDE for irq inversion detection -- it's not actually
	 * a lock and only used to override the wait_type.
	 */
	/* WAIT_OVERRIDE 也跳过：它不是真锁，只用于暂时覆盖 wait_type，不应成为 IRQ 依赖图节点。 */

	/* 到此的合法非普通节点均可连同经过它的路径从 IRQ 反转搜索中剪掉。 */
	return true;
}

/*
 * Find a node in the forwards-direction dependency sub-graph starting
 * at @root->class that matches @bit.
 *
 * Return BFS_MATCH if such a node exists in the subgraph, and put that node
 * into *@target_entry.
 */
/*
 * 从 @root->class 开始在正向依赖子图寻找 usage 与给定掩码匹配的节点。存在时实际返回枚举名
 * BFS_RMATCH（原文 BFS_MATCH 为旧称）并把节点写入 *@target_entry。
 */
/**
 * find_usage_forwards - 沿 locks_after 查找首个冲突 usage 节点
 * @root: 已初始化 BFS 根
 * @usage_mask: 目标 usage 位集合，函数取局部副本供回调读取
 * @target_entry: 命中时接收 lock_list 借用指针
 *
 * 返回：__bfs_forwards() 的完整 enum bfs_result；不在此打印 BFS 错误。
 * 上下文：调用者持有 graph_lock；搜索使用 usage_match() 并由 usage_skip() 剪去特殊节点。
 */
static enum bfs_result
find_usage_forwards(struct lock_list *root, unsigned long usage_mask,
			struct lock_list **target_entry)
{
	/* result 保存匹配、未匹配或基础设施错误。 */
	enum bfs_result result;

	/* 统计实际发起的正向 usage 子图检查次数。 */
	debug_atomic_inc(nr_find_usage_forwards_checks);

	/* usage_mask 地址只在同步 BFS 回调期间借用。 */
	result = __bfs_forwards(root, &usage_mask, usage_match, usage_skip, target_entry);

	return result;
}

/*
 * Find a node in the backwards-direction dependency sub-graph starting
 * at @root->class that matches @bit.
 */
/* 从 @root->class 开始在反向依赖子图寻找 usage 与给定掩码匹配的节点。 */
/**
 * find_usage_backwards - 沿 locks_before 查找首个冲突 usage 节点
 * @root: 已初始化 BFS 根
 * @usage_mask: 目标 usage 位集合
 * @target_entry: 命中时接收 lock_list 借用指针
 *
 * 返回：__bfs_backwards() 的 enum bfs_result；错误报告和图锁状态处理交给调用者。
 */
static enum bfs_result
find_usage_backwards(struct lock_list *root, unsigned long usage_mask,
			struct lock_list **target_entry)
{
	/* result 保存反向 BFS 的分类结果。 */
	enum bfs_result result;

	/* 统计反向 usage 搜索次数，供 DEBUG_LOCKDEP 观测。 */
	debug_atomic_inc(nr_find_usage_backwards_checks);

	/* 使用与正向相同的 match/skip，只改变依赖链字段偏移。 */
	result = __bfs_backwards(root, &usage_mask, usage_match, usage_skip, target_entry);

	return result;
}

/**
 * print_lock_class_header - 打印锁类身份、usage 来源和 key
 * @class: 要诊断的有效 lock_class
 * @depth: 左侧缩进宽度，也作为路径层级的视觉提示
 *
 * 返回：无；打印类名、usage/wait 属性、可选 DEBUG_LOCKDEP 操作次数、每个已记录 usage 的来源栈，
 * 以及 key 地址和符号。只借用 class/trace，不修改图。
 */
static void print_lock_class_header(struct lock_class *class, int depth)
{
	/* bit 遍历具备 usage_traces[] 证据的状态位。 */
	int bit;

	/* 先打印缩进、箭头和统一锁名/属性。 */
	printk("%*s->", depth, "");
	print_lock_name(NULL, class);
#ifdef CONFIG_DEBUG_LOCKDEP
	/* 调试构建附带该 class 经历的操作次数。 */
	printk(KERN_CONT " ops: %lu", debug_class_ops_read(class));
#endif
	printk(KERN_CONT " {\n");

	/* 每个置位 usage 都打印标签和首次记录该状态时保存的调用栈。 */
	for (bit = 0; bit < LOCK_TRACE_STATES; bit++) {
		if (class->usage_mask & (1 << bit)) {
			/* len 累计当前标签行宽，作为栈回溯的对齐缩进。 */
			int len = depth;

			len += printk("%*s   %s", depth, "", usage_str[bit]);
			len += printk(KERN_CONT " at:\n");
			print_lock_trace(class->usage_traces[bit], len);
		}
	}
	/* 闭合 usage 块后打印 key 的裸地址与符号化位置。 */
	printk("%*s }\n", depth, "");

	printk("%*s ... key      at: [<%px>] %pS\n",
		depth, "", class->key, class->key);
}

/*
 * Dependency path printing:
 *
 * After BFS we get a lock dependency path (linked via ->parent of lock_list),
 * printing out each lock in the dependency path will help on understanding how
 * the deadlock could happen. Here are some details about dependency path
 * printing:
 *
 * 1)	A lock_list can be either forwards or backwards for a lock dependency,
 * 	for a lock dependency A -> B, there are two lock_lists:
 *
 * 	a)	lock_list in the ->locks_after list of A, whose ->class is B and
 * 		->links_to is A. In this case, we can say the lock_list is
 * 		"A -> B" (forwards case).
 *
 * 	b)	lock_list in the ->locks_before list of B, whose ->class is A
 * 		and ->links_to is B. In this case, we can say the lock_list is
 * 		"B <- A" (bacwards case).
 *
 * 	The ->trace of both a) and b) point to the call trace where B was
 * 	acquired with A held.
 *
 * 2)	A "helper" lock_list is introduced during BFS, this lock_list doesn't
 * 	represent a certain lock dependency, it only provides an initial entry
 * 	for BFS. For example, BFS may introduce a "helper" lock_list whose
 * 	->class is A, as a result BFS will search all dependencies starting with
 * 	A, e.g. A -> B or A -> C.
 *
 * 	The notation of a forwards helper lock_list is like "-> A", which means
 * 	we should search the forwards dependencies starting with "A", e.g A -> B
 * 	or A -> C.
 *
 * 	The notation of a bacwards helper lock_list is like "<- B", which means
 * 	we should search the backwards dependencies ending with "B", e.g.
 * 	B <- A or B <- C.
 */
/*
 * BFS 完成后，lock_list.parent 串成最短依赖路径，逐锁打印可帮助理解死锁如何发生。每条真实依赖
 * A->B 有两份镜像记录：A.locks_after 中的记录 class=B、links_to=A，记作“A -> B”；B.locks_before
 * 中的记录 class=A、links_to=B，记作“B <- A”。两份记录的 trace 都指向“持有 A 时获取 B”的现场。
 *
 * BFS 还会引入不代表真实边的 helper 根，只提供搜索起点。正向 helper“-> A”表示搜索 A->B、A->C
 * 等从 A 出发的依赖；反向 helper“<- B”表示搜索 B<-A、B<-C 等以 B 结束的依赖。打印函数必须按
 * 搜索方向解释 class/trace，不能把 helper 当成已经存在的图边。
 */

/*
 * printk the shortest lock dependencies from @root to @leaf in reverse order.
 *
 * We have a lock dependency path as follow:
 *
 *    @root                                                                 @leaf
 *      |                                                                     |
 *      V                                                                     V
 *	          ->parent                                   ->parent
 * | lock_list | <--------- | lock_list | ... | lock_list  | <--------- | lock_list |
 * |    -> L1  |            | L1 -> L2  | ... |Ln-2 -> Ln-1|            | Ln-1 -> Ln|
 *
 * , so it's natural that we start from @leaf and print every ->class and
 * ->trace until we reach the @root.
 */
/*
 * 逆序打印从 @root 到 @leaf 的最短正向依赖。BFS parent 指针从 leaf 指回 root：helper“->L1”是根，
 * 中间记录依次为 L1->L2 ... Ln-1->Ln。因此从 leaf 开始逐项打印 class/trace，天然得到逆序证据链。
 */
/**
 * print_shortest_lock_dependencies - 逆序输出正向 BFS 的最短依赖路径
 * @leaf: BFS 命中节点
 * @root: 该 BFS 使用的临时 helper 根
 *
 * 返回：无；沿 parent 从 leaf 走到 root，为每层打印 class 头和该边 acquisition trace。
 * 一致性边界：若 depth 已到零却未抵达 root，打印 bad path 并停止，避免继续信任损坏父链。
 */
static void __used
print_shortest_lock_dependencies(struct lock_list *leaf,
				 struct lock_list *root)
{
	/* entry 从命中端逆向移动，depth 与 parent 跳数同步递减。 */
	struct lock_list *entry = leaf;
	int depth;

	/*compute depth from generated tree by BFS*/
	/* 从 BFS 生成树计算 leaf 到 helper 根的深度。 */
	depth = get_lock_depth(leaf);

	/* do/while 保证 leaf 本身至少打印一次。 */
	do {
		/* class 的 usage/key 信息和建立当前依赖的 trace 分两段输出。 */
		print_lock_class_header(entry->class, depth);
		printk("%*s ... acquired at:\n", depth, "");
		print_lock_trace(entry->trace, 2);
		printk("\n");

		/* 深度计数已耗尽却不是指定 root，说明 parent 树与计算结果/调用参数不一致。 */
		if (depth == 0 && (entry != root)) {
			printk("lockdep:%s bad path found in chain graph\n", __func__);
			break;
		}

		/* 沿父边向 helper 根移动，并同步降低显示层级。 */
		entry = get_lock_parent(entry);
		depth--;
	} while (entry && (depth >= 0));
}

/*
 * printk the shortest lock dependencies from @leaf to @root.
 *
 * We have a lock dependency path (from a backwards search) as follow:
 *
 *    @leaf                                                                 @root
 *      |                                                                     |
 *      V                                                                     V
 *	          ->parent                                   ->parent
 * | lock_list | ---------> | lock_list | ... | lock_list  | ---------> | lock_list |
 * | L2 <- L1  |            | L3 <- L2  | ... | Ln <- Ln-1 |            |    <- Ln  |
 *
 * , so when we iterate from @leaf to @root, we actually print the lock
 * dependency path L1 -> L2 -> .. -> Ln in the non-reverse order.
 *
 * Another thing to notice here is that ->class of L2 <- L1 is L1, while the
 * ->trace of L2 <- L1 is the call trace of L2, in fact we don't have the call
 * trace of L1 in the dependency path, which is alright, because most of the
 * time we can figure out where L1 is held from the call trace of L2.
 */
/*
 * 按正常依赖顺序打印反向 BFS 的 leaf 到 root 路径。反向记录链形如 L2<-L1、L3<-L2 ... helper<-Ln；
 * 虽然沿 parent 从 leaf 走向 root，显示的 class 顺序实际是 L1->L2->...->Ln。
 *
 * 注意 L2<-L1 记录的 class 是 L1，而 trace 却是获取 L2 的现场；路径中没有单独保存 L1 的获取栈，
 * 通常可从 L2 的栈推断 L1 在哪里被持有。因此实现把当前 entry->trace 延迟到下一父项打印，让 trace
 * 与“被获取的后继锁”对齐，leaf 第一项没有可打印的前一 trace。
 */
/**
 * print_shortest_lock_dependencies_backwards - 正序输出反向 BFS 找到的最短依赖路径
 * @leaf: 反向 BFS 命中端
 * @root: 反向搜索 helper 根
 *
 * 返回：无；沿 parent 移动并错后一项使用 trace，使输出呈现原始 L1->...->Ln 获取顺序。
 * 一致性边界：depth 为零但 entry 不是 root 时报告 bad path 并停止。
 */
static void __used
print_shortest_lock_dependencies_backwards(struct lock_list *leaf,
					   struct lock_list *root)
{
	/* entry 从 leaf 向 helper root 走；trace 保存上一条子记录的 acquisition 证据。 */
	struct lock_list *entry = leaf;
	const struct lock_trace *trace = NULL;
	int depth;

	/*compute depth from generated tree by BFS*/
	/* parent 链给出固定显示深度。 */
	depth = get_lock_depth(leaf);

	do {
		/* 每层都打印 class；只有已有错后一项的 trace 时才打印获取现场。 */
		print_lock_class_header(entry->class, depth);
		if (trace) {
			printk("%*s ... acquired at:\n", depth, "");
			print_lock_trace(trace, 2);
			printk("\n");
		}

		/*
		 * Record the pointer to the trace for the next lock_list
		 * entry, see the comments for the function.
		 */
		/* 保存当前记录 trace，供下一次 parent 对应的“被获取锁”位置使用，详见函数总说明。 */
		trace = entry->trace;

		/* 父链深度不再允许移动却未到指定根，说明 BFS 路径元数据损坏。 */
		if (depth == 0 && (entry != root)) {
			printk("lockdep:%s bad path found in chain graph\n", __func__);
			break;
		}

		/* 继续向 helper root 移动。 */
		entry = get_lock_parent(entry);
		depth--;
	} while (entry && (depth >= 0));
}

/**
 * print_irq_lock_scenario - 用两 CPU 时序解释 irq-safe 到 irq-unsafe 的潜在死锁
 * @safe_entry: 反向 BFS 找到的 irq-safe 锁节点
 * @unsafe_entry: 正向 BFS 找到的 irq-unsafe 锁节点
 * @prev_class: 拟新增 prev->next 边的前驱 class
 * @next_class: 拟新增边的后继 class
 *
 * 返回：无；若 safe 与 unsafe 间有中间 class，先打印三点链摘要，再展示 CPU0 被 IRQ 打断与 CPU1
 * 在关 IRQ 区间按依赖顺序取锁所形成的闭环。只用于已确认冲突后的诊断。
 */
static void
print_irq_lock_scenario(struct lock_list *safe_entry,
			struct lock_list *unsafe_entry,
			struct lock_class *prev_class,
			struct lock_class *next_class)
{
	/* safe/unsafe 来自两侧 BFS 命中；middle 选择新边上位于二者之间、最适合展示的 class。 */
	struct lock_class *safe_class = safe_entry->class;
	struct lock_class *unsafe_class = unsafe_entry->class;
	struct lock_class *middle_class = prev_class;

	/* 若 prev 本身就是 safe，新边另一端 next 才是链中的中间/unsafe 候选。 */
	if (middle_class == safe_class)
		middle_class = next_class;

	/*
	 * A direct locking problem where unsafe_class lock is taken
	 * directly by safe_class lock, then all we need to show
	 * is the deadlock scenario, as it is obvious that the
	 * unsafe lock is taken under the safe lock.
	 *
	 * But if there is a chain instead, where the safe lock takes
	 * an intermediate lock (middle_class) where this lock is
	 * not the same as the safe lock, then the lock chain is
	 * used to describe the problem. Otherwise we would need
	 * to show a different CPU case for each link in the chain
	 * from the safe_class lock to the unsafe_class lock.
	 */
	/*
	 * 若 unsafe_class 直接在 safe_class 下获取，只画死锁场景即可。若二者之间经过 middle_class，先
	 * 打印 safe->middle->unsafe 摘要；否则需要为整条链每一跳单独画 CPU，反而难以看清主关系。
	 */
	/* middle 已是 unsafe 表示直接关系，无需重复链摘要。 */
	if (middle_class != unsafe_class) {
		printk("Chain exists of:\n  ");
		__print_lock_name(NULL, safe_class);
		printk(KERN_CONT " --> ");
		__print_lock_name(NULL, middle_class);
		printk(KERN_CONT " --> ");
		__print_lock_name(NULL, unsafe_class);
		printk(KERN_CONT "\n\n");
	}

	/* CPU0 持 unsafe 后被 IRQ 打断并尝试 safe；CPU1 在 IRQ-off 下持 safe/middle，构成互等。 */
	printk(" Possible interrupt unsafe locking scenario:\n\n");
	printk("       CPU0                    CPU1\n");
	printk("       ----                    ----\n");
	printk("  lock(");
	__print_lock_name(NULL, unsafe_class);
	printk(KERN_CONT ");\n");
	printk("                               local_irq_disable();\n");
	/* CPU1 关闭 IRQ 后沿 safe->middle 方向持锁，最终可能等待 CPU0 的 unsafe。 */
	printk("                               lock(");
	__print_lock_name(NULL, safe_class);
	printk(KERN_CONT ");\n");
	printk("                               lock(");
	__print_lock_name(NULL, middle_class);
	printk(KERN_CONT ");\n");
	/* CPU0 的中断处理程序再获取 safe，可能等待 CPU1，闭合环路。 */
	printk("  <Interrupt>\n");
	printk("    lock(");
	__print_lock_name(NULL, safe_class);
	printk(KERN_CONT ");\n");
	printk("\n *** DEADLOCK ***\n\n");
}

/**
 * print_bad_irq_dependency - 冻结 lockdep 并输出完整 IRQ safe/unsafe 反转报告
 * @curr: 触发新依赖的任务
 * @prev_root: 从 prev 向前驱方向搜索使用的临时根
 * @next_root: 从 next 向后继方向搜索使用的临时根
 * @backwards_entry: 反向 BFS 命中的 irq-safe 节点
 * @forwards_entry: 正向 BFS 命中的 irq-unsafe 节点
 * @prev: 当前已持有、拟成为新边前驱的 held lock
 * @next: 当前正获取、拟成为新边后继的 held lock
 * @bit1: safe class 中与冲突配对的 usage bit
 * @bit2: unsafe class 中对应的 usage bit
 * @irqclass: 人类可读 IRQ 状态名，如 HARDIRQ 或 SOFTIRQ
 *
 * 返回：无；进入时持有 graph_lock。首个非静默报告者关闭调试并释放图锁，打印当前 IRQ 上下文、
 * 新边、safe/unsafe usage 来源、两 CPU 场景、held locks 和两侧最短路径；任何返回均不再持图锁。
 */
static void
print_bad_irq_dependency(struct task_struct *curr,
			 struct lock_list *prev_root,
			 struct lock_list *next_root,
			 struct lock_list *backwards_entry,
			 struct lock_list *forwards_entry,
			 struct held_lock *prev,
			 struct held_lock *next,
			 enum lock_usage_bit bit1,
			 enum lock_usage_bit bit2,
			 const char *irqclass)
{
	/* 先冻结图并解锁，避免后续大量 printk/栈回溯递归进入 lockdep。 */
	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return;

	/* 报告期间使用控制台紧急区尽量保持多段输出连续。 */
	nbcon_cpu_emergency_enter();

	/* 标题包含 IRQ 类别、内核身份以及触发任务的 hard/soft IRQ 深度和 enable 状态。 */
	pr_warn("\n");
	pr_warn("=====================================================\n");
	pr_warn("WARNING: %s-safe -> %s-unsafe lock order detected\n",
		irqclass, irqclass);
	print_kernel_ident();
	pr_warn("-----------------------------------------------------\n");
	pr_warn("%s/%d [HC%u[%lu]:SC%u[%lu]:HE%u:SE%u] is trying to acquire:\n",
		curr->comm, task_pid_nr(curr),
		lockdep_hardirq_context(), hardirq_count() >> HARDIRQ_SHIFT,
		curr->softirq_context, softirq_count() >> SOFTIRQ_SHIFT,
		lockdep_hardirqs_enabled(),
		curr->softirqs_enabled);
	/* 首先显示任务正尝试获取的 next。 */
	print_lock(next);

	/* 再显示已持有 prev，并明确本次将新增 prev->next。 */
	pr_warn("\nand this task is already holding:\n");
	print_lock(prev);
	pr_warn("which would create a new lock dependency:\n");
	print_lock_name(prev, hlock_class(prev));
	pr_cont(" ->");
	print_lock_name(next, hlock_class(next));
	pr_cont("\n");

	/* 展示反向子图命中的 safe class，以及 bit1 首次置位时保存的 usage trace。 */
	pr_warn("\nbut this new dependency connects a %s-irq-safe lock:\n",
		irqclass);
	print_lock_name(NULL, backwards_entry->class);
	pr_warn("\n... which became %s-irq-safe at:\n", irqclass);

	print_lock_trace(backwards_entry->class->usage_traces[bit1], 1);

	/* 展示正向子图命中的 unsafe class，以及 bit2 对应的启用 IRQ 使用现场。 */
	pr_warn("\nto a %s-irq-unsafe lock:\n", irqclass);
	print_lock_name(NULL, forwards_entry->class);
	pr_warn("\n... which became %s-irq-unsafe at:\n", irqclass);
	pr_warn("...");

	print_lock_trace(forwards_entry->class->usage_traces[bit2], 1);

	/* 用简化时序图解释隐式中断边如何闭合 safe->...->unsafe 路径。 */
	pr_warn("\nother info that might help us debug this:\n\n");
	print_irq_lock_scenario(backwards_entry, forwards_entry,
				hlock_class(prev), hlock_class(next));

	/* 附上触发任务完整 held-lock 栈。 */
	lockdep_print_held_locks(curr);

	/* 反向 BFS 路径按依赖正序说明 safe class 如何到达当前持有的 prev。 */
	pr_warn("\nthe dependencies between %s-irq-safe lock and the holding lock:\n", irqclass);
	print_shortest_lock_dependencies_backwards(backwards_entry, prev_root);

	pr_warn("\nthe dependencies between the lock to be acquired");
	pr_warn(" and %s-irq-unsafe lock:\n", irqclass);
	/* 图已冻结后给 next helper 根补当前现场，供正向路径末端打印；失败则只省略余下部分。 */
	next_root->trace = save_trace();
	if (!next_root->trace)
		goto out;
	print_shortest_lock_dependencies(forwards_entry, next_root);

	/* 最后给出当前检测栈，与历史 usage/依赖 trace 互补。 */
	pr_warn("\nstack backtrace:\n");
	dump_stack();
out:
	/* 包括 save_trace 失败路径在内，统一退出控制台紧急区。 */
	nbcon_cpu_emergency_exit();
}

/* 按 lockdep_states.h 顺序生成不带 READ 后缀的 IRQ 状态名称表。 */
static const char *state_names[] = {
/* 临时把每个状态展开为其标识符字符串。 */
#define LOCKDEP_STATE(__STATE) \
	__stringify(__STATE),
#include "lockdep_states.h"
#undef LOCKDEP_STATE
};

/* 与 state_names[] 同序，为 read usage 生成“STATE-READ”诊断名称。 */
static const char *state_rnames[] = {
#define LOCKDEP_STATE(__STATE) \
	__stringify(__STATE)"-READ",
#include "lockdep_states.h"
#undef LOCKDEP_STATE
};

/**
 * state_name - 把一个 IRQ usage bit 转换为状态类别名称
 * @bit: 按 read/direction/state 三段编码的 enum lock_usage_bit
 *
 * 返回：带 READ 后缀或普通状态名称的静态字符串借用指针。
 * 边界：右移 LOCK_USAGE_DIR_MASK 位同时去掉 read 和 direction 两个低位，得到状态表下标。
 */
static inline const char *state_name(enum lock_usage_bit bit)
{
	/* bit0 置位选择 read 名称表，否则选择普通名称表。 */
	if (bit & LOCK_USAGE_READ_MASK)
		return state_rnames[bit >> LOCK_USAGE_DIR_MASK];
	else
		return state_names[bit >> LOCK_USAGE_DIR_MASK];
}

/*
 * The bit number is encoded like:
 *
 *  bit0: 0 exclusive, 1 read lock
 *  bit1: 0 used in irq, 1 irq enabled
 *  bit2-n: state
 */
/*
 * usage 位编号的编码：bit0 为 0 表示独占、1 表示 read lock；bit1 为 0 表示在 IRQ 中使用、1 表示
 * 使用时 IRQ 开启；bit2 及以上选择 hardirq/softirq 等状态。这种布局允许用移位批量翻转相邻维度。
 */
/**
 * exclusive_bit - 求一个 usage 位在相同状态下的反方向独占配对位
 * @new_bit: 原 usage bit 编号
 *
 * 返回：保留高位 state、翻转 USED_IN/ENABLED direction，并清除 read 位后的编号。
 * 例：某状态的 USED_IN_*_READ 会映射为 ENABLED_* 非 READ，供冲突配对搜索。
 */
static int exclusive_bit(int new_bit)
{
	/* state 保留 bit2-n，dir 只保留 bit1；刻意不保留 bit0 read。 */
	int state = new_bit & LOCK_USAGE_STATE_MASK;
	int dir = new_bit & LOCK_USAGE_DIR_MASK;

	/*
	 * keep state, bit flip the direction and strip read.
	 */
	/* 保持状态，翻转方向位并去掉 read 位。 */
	return state | (dir ^ LOCK_USAGE_DIR_MASK);
}

/*
 * Observe that when given a bitmask where each bitnr is encoded as above, a
 * right shift of the mask transforms the individual bitnrs as -1 and
 * conversely, a left shift transforms into +1 for the individual bitnrs.
 *
 * So for all bits whose number have LOCK_ENABLED_* set (bitnr1 == 1), we can
 * create the mask with those bit numbers using LOCK_USED_IN_* (bitnr1 == 0)
 * instead by subtracting the bit number by 2, or shifting the mask right by 2.
 *
 * Similarly, bitnr1 == 0 becomes bitnr1 == 1 by adding 2, or shifting left 2.
 *
 * So split the mask (note that LOCKF_ENABLED_IRQ_ALL|LOCKF_USED_IN_IRQ_ALL is
 * all bits set) and recompose with bitnr1 flipped.
 */
/*
 * 对按上述方式编码的 bit 集合，右移会让每个编号减一，左移会加一。所有 ENABLED 位的 direction
 * bit1 为 1，可把掩码右移 2 转成同状态 USED_IN；反向则左移 2。把 mask 按 ENABLED/USED_IN 集合
 * 拆开后分别移位再合并，就能批量翻转 bit1，同时保留 read 位与 state 高位。
 */
/**
 * invert_dir_mask - 批量交换 usage 掩码中的 USED_IN 与 ENABLED 方向
 * @mask: 任意 usage 位集合
 *
 * 返回：仅保留 IRQ usage 位，并把 ENABLED_* 映射到 USED_IN_*、USED_IN_* 映射到 ENABLED_*；
 * read 与 IRQ state 不变。
 * 副作用：纯位运算，不修改输入。
 */
static unsigned long invert_dir_mask(unsigned long mask)
{
	/* excl 从空集合开始接收两个方向的移位结果。 */
	unsigned long excl = 0;

	/* Invert dir */
	/* 翻转 direction：ENABLED 集合右移到 USED_IN，USED_IN 集合左移到 ENABLED。 */
	excl |= (mask & LOCKF_ENABLED_IRQ_ALL) >> LOCK_USAGE_DIR_MASK;
	excl |= (mask & LOCKF_USED_IN_IRQ_ALL) << LOCK_USAGE_DIR_MASK;

	return excl;
}

/*
 * Note that a LOCK_ENABLED_IRQ_*_READ usage and a LOCK_USED_IN_IRQ_*_READ
 * usage may cause deadlock too, for example:
 *
 * P1				P2
 * <irq disabled>
 * write_lock(l1);		<irq enabled>
 *				read_lock(l2);
 * write_lock(l2);
 * 				<in irq>
 * 				read_lock(l1);
 *
 * , in above case, l1 will be marked as LOCK_USED_IN_IRQ_HARDIRQ_READ and l2
 * will marked as LOCK_ENABLE_IRQ_HARDIRQ_READ, and this is a possible
 * deadlock.
 *
 * In fact, all of the following cases may cause deadlocks:
 *
 * 	 LOCK_USED_IN_IRQ_* -> LOCK_ENABLED_IRQ_*
 * 	 LOCK_USED_IN_IRQ_*_READ -> LOCK_ENABLED_IRQ_*
 * 	 LOCK_USED_IN_IRQ_* -> LOCK_ENABLED_IRQ_*_READ
 * 	 LOCK_USED_IN_IRQ_*_READ -> LOCK_ENABLED_IRQ_*_READ
 *
 * As a result, to calculate the "exclusive mask", first we invert the
 * direction (USED_IN/ENABLED) of the original mask, and 1) for all bits with
 * bitnr0 set (LOCK_*_READ), add those with bitnr0 cleared (LOCK_*). 2) for all
 * bits with bitnr0 cleared (LOCK_*_READ), add those with bitnr0 set (LOCK_*).
 */
/*
 * 带 READ 后缀的 ENABLED 与 USED_IN 也可能形成死锁。例如 P1 关 IRQ 后写锁 l1、再写锁 l2；P2 在
 * IRQ 开启时读锁 l2，随后中断中读锁 l1，l1/l2 分别留下 USED_IN_*_READ 与 ENABLED_*_READ，仍可
 * 互等。实际上 USED_IN/ENABLED 两端的 read 与非-read 四种组合都可能冲突。
 *
 * 因而 exclusive mask 先翻转 USED_IN/ENABLED 方向，再为每个 read 位加入其非-read 伙伴，也为每个
 * 非-read 位加入 read 伙伴。当前代码通过 LOCKF_IRQ_READ 右移 bit0、LOCKF_IRQ 左移 bit0 完成扩展。
 */
/**
 * exclusive_mask - 生成与原 usage 集合可能互斥的全部 IRQ usage 位
 * @mask: 原始 USED_IN/ENABLED usage 位集合
 *
 * 返回：方向已翻转，且每个状态同时包含 read 与非-read 两种候选的冲突掩码。
 */
static unsigned long exclusive_mask(unsigned long mask)
{
	/* 第一步保留 state/read 并交换 USED_IN 与 ENABLED。 */
	unsigned long excl = invert_dir_mask(mask);

	/* 第二步双向补齐 bit0 伙伴，使四种 read/写配对均可命中。 */
	excl |= (excl & LOCKF_IRQ_READ) >> LOCK_USAGE_READ_MASK;
	excl |= (excl & LOCKF_IRQ) << LOCK_USAGE_READ_MASK;

	return excl;
}

/*
 * Retrieve the _possible_ original mask to which @mask is
 * exclusive. Ie: this is the opposite of exclusive_mask().
 * Note that 2 possible original bits can match an exclusive
 * bit: one has LOCK_USAGE_READ_MASK set, the other has it
 * cleared. So both are returned for each exclusive bit.
 */
/*
 * 取回与 @mask 互斥的所有“可能原始”usage，概念上是 exclusive_mask() 的反向关系。由于一个冲突位
 * 可对应 read 位已置或已清的两个原始位，变换不是一一映射，必须把两种候选都返回。
 */
/**
 * original_mask - 从冲突 usage 集合反求所有可能的原 usage 位
 * @mask: 已知一侧的冲突 usage 掩码
 *
 * 返回：翻转 direction 并同时包含 read/非-read 伙伴的可能原集合。
 * 注意：结果是关系的逆像而非精确逆函数，调用者仍需用真实 class->usage_mask 二次匹配。
 */
static unsigned long original_mask(unsigned long mask)
{
	/* 与 exclusive_mask 相同的位扩展，解释方向不同：这里枚举所有可能来源。 */
	unsigned long excl = invert_dir_mask(mask);

	/* Include read in existing usages */
	/* 为现有 usage 同时加入 read 与非-read 伙伴。 */
	excl |= (excl & LOCKF_IRQ_READ) >> LOCK_USAGE_READ_MASK;
	excl |= (excl & LOCKF_IRQ) << LOCK_USAGE_READ_MASK;

	return excl;
}

/*
 * Find the first pair of bit match between an original
 * usage mask and an exclusive usage mask.
 */
/* 在原始 usage mask 与互斥 usage mask 之间寻找第一对具体 bit。 */
/**
 * find_exclusive_match - 解析用于报告的第一对原始/冲突 usage 位
 * @mask: 原始 class 的 usage 位集合
 * @excl_mask: 另一 class 的候选互斥位集合
 * @bitp: 成功时写入原始 bit
 * @excl_bitp: 成功时写入匹配的冲突 bit
 *
 * 返回：找到配对为 0，否则为 -1 且不承诺输出参数内容。
 * 顺序：按原 bit 递增扫描，优先选择 exclusive_bit() 的非-read 配对，再尝试同状态 read 配对。
 */
static int find_exclusive_match(unsigned long mask,
				unsigned long excl_mask,
				enum lock_usage_bit *bitp,
				enum lock_usage_bit *excl_bitp)
{
	/* bit 扫描原集合，excl 是方向翻转且去 read 的伙伴，excl_read 再补 read 位。 */
	int bit, excl, excl_read;

	/* LOCK_USED 之前覆盖 IRQ trace states，排除后面的 INITIAL USE 等非 IRQ 槽。 */
	for_each_set_bit(bit, &mask, LOCK_USED) {
		/*
		 * exclusive_bit() strips the read bit, however,
		 * LOCK_ENABLED_IRQ_*_READ may cause deadlocks too, so we need
		 * to search excl | LOCK_USAGE_READ_MASK as well.
		 */
		/* exclusive_bit() 会清 read 位，但 ENABLED_IRQ_*_READ 同样可能死锁，故两种伙伴都查。 */
		excl = exclusive_bit(bit);
		excl_read = excl | LOCK_USAGE_READ_MASK;
		/* 优先返回非-read 冲突位，保证同一输入下报告选择稳定。 */
		if (excl_mask & lock_flag(excl)) {
			*bitp = bit;
			*excl_bitp = excl;
			return 0;
		} else if (excl_mask & lock_flag(excl_read)) {
			/* 非-read 不存在时再接受 read 伙伴。 */
			*bitp = bit;
			*excl_bitp = excl_read;
			return 0;
		}
	}
	return -1;
}

/*
 * Prove that the new dependency does not connect a hardirq-safe(-read)
 * lock with a hardirq-unsafe lock - to achieve this we search
 * the backwards-subgraph starting at <prev>, and the
 * forwards-subgraph starting at <next>:
 */
/*
 * 证明新依赖不会把 hardirq-safe（含 read）锁连到 hardirq-unsafe 锁：从 <prev> 向前驱子图搜索 safe
 * usage，从 <next> 向后继子图搜索对应 unsafe usage；同一流程也覆盖 softirq 状态。
 */
/**
 * check_irq_usage - 验证拟新增 prev->next 不会连接 IRQ-safe 与 IRQ-unsafe 子图
 * @curr: 触发依赖的当前任务，冲突报告使用
 * @prev: 当前已持有的新边前驱
 * @next: 当前正获取的新边后继
 *
 * 返回：1 表示未发现 IRQ 反转且调用者仍持有 graph_lock；0 表示 BFS 错误或确认冲突，报告路径已
 * 关闭 lockdep 并释放图锁。
 * 算法：反向累计 prev 侧 USED_IN usage，正向找 next 侧互斥 ENABLED usage，再回查精确 safe 节点，
 * 最后选出一对具体 bit 交给 print_bad_irq_dependency()。
 */
static int check_irq_usage(struct task_struct *curr, struct held_lock *prev,
			   struct held_lock *next)
{
	/* usage_mask 汇集 safe 侧，forward/backward_mask 分别驱动两次定位搜索。 */
	unsigned long usage_mask = 0, forward_mask, backward_mask;
	/* 两个 bit 最终索引 safe/unsafe usage_traces[]，先置零便于异常诊断。 */
	enum lock_usage_bit forward_bit = 0, backward_bit = 0;
	/* target_entry1 是正向 unsafe 命中，target_entry 是反向 safe 命中。 */
	struct lock_list *target_entry1;
	struct lock_list *target_entry;
	/* this/that 是 prev 反向根和 next 正向根，ret 保存每步 BFS/匹配结果。 */
	struct lock_list this, that;
	enum bfs_result ret;

	/*
	 * Step 1: gather all hard/soft IRQs usages backward in an
	 * accumulated usage mask.
	 */
	/* 第 1 步：沿 prev 的反向子图累计全部 hard/soft IRQ 使用位。 */
	bfs_init_rootb(&this, prev);

	/* usage_accumulate 永不命中，故 target_entry 参数无需提供。 */
	ret = __bfs_backwards(&this, &usage_mask, usage_accumulate, usage_skip, NULL);
	if (bfs_error(ret)) {
		/* BFS 错误由报告 helper 关闭并解锁，返回 0 阻止调用者继续改图。 */
		print_bfs_bug(ret);
		return 0;
	}

	/* safe 端只关心“曾在 IRQ 中使用”方向，丢弃 ENABLED 和非 IRQ 位。 */
	usage_mask &= LOCKF_USED_IN_IRQ_ALL;
	/* 前驱子图没有任何 IRQ-safe usage，不可能形成 safe->unsafe 反转。 */
	if (!usage_mask)
		return 1;

	/*
	 * Step 2: find exclusive uses forward that match the previous
	 * backward accumulated mask.
	 */
	/* 第 2 步：把 safe usage 转为其互斥集合，在 next 的正向子图查找 unsafe 端。 */
	forward_mask = exclusive_mask(usage_mask);

	/* next 的实际读模式影响正向根 only_xr，从而筛掉非强路径。 */
	bfs_init_root(&that, next);

	ret = find_usage_forwards(&that, forward_mask, &target_entry1);
	if (bfs_error(ret)) {
		/* 错误路径已关闭并释放图锁。 */
		print_bfs_bug(ret);
		return 0;
	}
	/* 没有 unsafe 匹配，新增边在 IRQ usage 维度可接受。 */
	if (ret == BFS_RNOMATCH)
		return 1;

	/*
	 * Step 3: we found a bad match! Now retrieve a lock from the backward
	 * list whose usage mask matches the exclusive usage mask from the
	 * lock found on the forward list.
	 *
	 * Note, we should only keep the LOCKF_ENABLED_IRQ_ALL bits, considering
	 * the follow case:
	 *
	 * When trying to add A -> B to the graph, we find that there is a
	 * hardirq-safe L, that L -> ... -> A, and another hardirq-unsafe M,
	 * that B -> ... -> M. However M is **softirq-safe**, if we use exact
	 * invert bits of M's usage_mask, we will find another lock N that is
	 * **softirq-unsafe** and N -> ... -> A, however N -> .. -> M will not
	 * cause a inversion deadlock.
	 */
	/*
	 * 第 3 步：已经找到 unsafe 端，再从反向列表取回与它精确配对的 safe 节点。这里只保留 unsafe
	 * class 的 LOCKF_ENABLED_IRQ_ALL 位再做 original_mask()。例如新增 A->B 时，反向有 hardirq-safe
	 * L->...->A，正向有 hardirq-unsafe M 且 B->...->M；即便 M 同时 softirq-safe，若直接反转其全部
	 * usage，可能误找 softirq-unsafe N 且 N->...->A，但 N->...->M 并不会构成对应反转死锁。
	 */
	backward_mask = original_mask(target_entry1->class->usage_mask & LOCKF_ENABLED_IRQ_ALL);

	/* 用收窄后的可能原集合在同一 prev 反向子图定位具体 safe entry。 */
	ret = find_usage_backwards(&this, backward_mask, &target_entry);
	if (bfs_error(ret)) {
		print_bfs_bug(ret);
		return 0;
	}
	/* 正向已命中却无法回找 safe 源属于内部逻辑异常；WARN 后按“未证实冲突”返回 1。 */
	if (DEBUG_LOCKS_WARN_ON(ret == BFS_RNOMATCH))
		return 1;

	/*
	 * Step 4: narrow down to a pair of incompatible usage bits
	 * and report it.
	 */
	/* 第 4 步：从两端真实 usage_mask 中收窄出一对不兼容 bit，并生成最终报告。 */
	ret = find_exclusive_match(target_entry->class->usage_mask,
				   target_entry1->class->usage_mask,
				   &backward_bit, &forward_bit);
	/* 子图匹配后仍找不到具体 bit 属于一致性异常，WARN 后不关闭图。 */
	if (DEBUG_LOCKS_WARN_ON(ret == -1))
		return 1;

	print_bad_irq_dependency(curr, &this, &that,
				 target_entry, target_entry1,
				 prev, next,
				 backward_bit, forward_bit,
				 state_name(backward_bit));

	/* 冲突报告已经冻结 lockdep 并释放图锁。 */
	return 0;
}

#else

/**
 * check_irq_usage - 未编译 TRACE_IRQFLAGS 时的放行 stub
 * @curr: 未使用
 * @prev: 未使用
 * @next: 未使用
 *
 * 返回：固定 1，表示此配置不做 IRQ safe/unsafe 子图证明，调用者仍持有原图锁。
 */
static inline int check_irq_usage(struct task_struct *curr,
				  struct held_lock *prev, struct held_lock *next)
{
	return 1;
}

/**
 * usage_skip - 未编译 TRACE_IRQFLAGS 时的 BFS 剪枝 stub
 * @entry: 未使用
 * @mask: 未使用
 *
 * 返回：固定 false，不因 IRQ 专属规则跳过任何依赖节点。
 */
static inline bool usage_skip(struct lock_list *entry, void *mask)
{
	return false;
}

#endif /* CONFIG_TRACE_IRQFLAGS */

#ifdef CONFIG_LOCKDEP_SMALL
/*
 * We are about to add A -> B into the dependency graph, and in __bfs() a
 * strong dependency path A -> .. -> B is found: hlock_class equals
 * entry->class.
 *
 * If A -> .. -> B can replace A -> B in any __bfs() search (means the former
 * is _stronger_ than or equal to the latter), we consider A -> B as redundant.
 * For example if A -> .. -> B is -(EN)-> (i.e. A -(E*)-> .. -(*N)-> B), and A
 * -> B is -(ER)-> or -(EN)->, then we don't need to add A -> B into the
 * dependency graph, as any strong path ..-> A -> B ->.. we can get with
 * having dependency A -> B, we could already get a equivalent path ..-> A ->
 * .. -> B -> .. with A -> .. -> B. Therefore A -> B is redundant.
 *
 * We need to make sure both the start and the end of A -> .. -> B is not
 * weaker than A -> B. For the start part, please see the comment in
 * check_redundant(). For the end part, we need:
 *
 * Either
 *
 *     a) A -> B is -(*R)-> (everything is not weaker than that)
 *
 * or
 *
 *     b) A -> .. -> B is -(*N)-> (nothing is stronger than this)
 *
 */
/*
 * 即将加入 A->B，而 BFS 已找到同终点的强路径 A->...->B。只有现有路径能在任意 BFS 中替代直接边，
 * 即强度大于等于直接边时，才把 A->B 判为冗余。例如现有路径为 EN（起点 E*、终点 *N），拟加边
 * 为 ER 或 EN，则任何经直接 A->B 得到的强路径都可用 A->...->B 等价替代。
 *
 * 因此既要检查路径起点不弱（由 check_redundant() 的根 only_xr 设置保证），也要检查终点不弱：
 * 要么拟加 A->B 本身以 *R 结束，任何终点都不弱于它；要么现有 A->...->B 以 *N 结束，已是最强。
 */
/**
 * hlock_equal - 判断 BFS 节点是否以足够强的路径到达拟新增边终点
 * @entry: 当前 BFS 节点，only_xr 描述现有 A->...->B 的末端强度
 * @data: 指向拟新增边终点 B 的 held_lock
 *
 * 返回：class 到达 B，且直接边为 *R 或现有路径为 *N 时为 true。
 * 副作用：纯匹配谓词；路径起点强度已由调用者初始化根状态约束。
 */
static inline bool hlock_equal(struct lock_list *entry, void *data)
{
	/* data 动态类型由 check_redundant()->check_path() 固定。 */
	struct held_lock *hlock = (struct held_lock *)data;

	return hlock_class(hlock) == entry->class && /* Found A -> .. -> B */
	       /* 已找到现有 A->...->B 的终点 B。 */
	       (hlock->read == 2 ||  /* A -> B is -(*R)-> */
		/* 拟加直接边以 *R 结束，现有任意强终点都不更弱。 */
		!entry->only_xr); /* A -> .. -> B is -(*N)-> */
		/* 或现有路径以 *N 结束，足以替代直接边。 */
}

/*
 * Check that the dependency graph starting at <src> can lead to
 * <target> or not. If it can, <src> -> <target> dependency is already
 * in the graph.
 *
 * Return BFS_RMATCH if it does, or BFS_RNOMATCH if it does not, return BFS_E* if
 * any error appears in the bfs search.
 */
/*
 * 检查从 <src> 出发是否已有能替代 <src>-><target> 的路径。存在返回 BFS_RMATCH，不存在返回
 * BFS_RNOMATCH，搜索错误返回 BFS_E*；“可达”还必须满足上面的路径强度条件。
 */
/**
 * check_redundant - 判断拟新增直接依赖是否已被不弱的强路径覆盖
 * @src: 拟新增 A->B 的起点 held lock
 * @target: 拟新增边终点 held lock
 *
 * 返回：找到可替代路径为 BFS_RMATCH，无匹配为 BFS_RNOMATCH，错误为负值。
 * 上下文：调用者持有 graph_lock；错误会由 check_path() 关闭调试并释放图锁，正常结果保持持锁。
 */
static noinline enum bfs_result
check_redundant(struct held_lock *src, struct held_lock *target)
{
	/* ret 保存搜索分类，target_entry 接收但无需后续使用，src_entry 是临时 A 根。 */
	enum bfs_result ret;
	struct lock_list *target_entry;
	struct lock_list src_entry;

	/* 先按 held-lock 读模式建立普通正向根。 */
	bfs_init_root(&src_entry, src);
	/*
	 * Special setup for check_redundant().
	 *
	 * To report redundant, we need to find a strong dependency path that
	 * is equal to or stronger than <src> -> <target>. So if <src> is E,
	 * we need to let __bfs() only search for a path starting at a -(E*)->,
	 * we achieve this by setting the initial node's ->only_xr to true in
	 * that case. And if <prev> is S, we set initial ->only_xr to false
	 * because both -(S*)-> (equal) and -(E*)-> (stronger) are redundant.
	 */
	/*
	 * 冗余检查的特殊根设置：要判冗余，现有路径必须等强或更强。若直接边起点 A 为 E，就令初始
	 * only_xr=true，迫使 __bfs() 只接受以 E* 开始的路径；若 A 为 S，则设 false，同时允许等强 S*
	 * 与更强 E* 起点。
	 */
	src_entry.only_xr = src->read == 0;

	/* 统计真正执行的冗余证明次数。 */
	debug_atomic_inc(nr_redundant_checks);

	/*
	 * Note: we skip local_lock() for redundant check, because as the
	 * comment in usage_skip(), A -> local_lock() -> B and A -> B are not
	 * the same.
	 */
	/*
	 * 冗余搜索跳过 local_lock()：如 usage_skip() 所述，A->local_lock()->B 与直接 A->B 并非等价，
	 * 不能据前者省掉后者。
	 */
	ret = check_path(target, &src_entry, hlock_equal, usage_skip, &target_entry);

	/* 只有完整匹配才累计被省略的冗余边数。 */
	if (ret == BFS_RMATCH)
		debug_atomic_inc(nr_redundant);

	return ret;
}

#else

/**
 * check_redundant - 非 LOCKDEP_SMALL 构建的关闭实现
 * @src: 未使用
 * @target: 未使用
 *
 * 返回：固定 BFS_RNOMATCH，表示不为节省静态池而省略任何已验证直接边。
 */
static inline enum bfs_result
check_redundant(struct held_lock *src, struct held_lock *target)
{
	return BFS_RNOMATCH;
}

#endif

/**
 * inc_chains - 按 IRQ 上下文增加已缓存 chain 分类计数
 * @irq_context: LOCK_CHAIN_HARDIRQ_CONTEXT/SOFTIRQ_CONTEXT 位集合
 *
 * 返回：无；hardirq 位优先，其次 softirq，否则归入 process。即使两位同时存在也只计 hardirq 一类。
 * 并发边界：由 chain 发布路径在 graph_lock 下调用，须与 dec_chains() 使用同一未变分类值配对。
 */
static void inc_chains(int irq_context)
{
	/* 三个计数互斥分类，一条 chain 只增加其中之一。 */
	if (irq_context & LOCK_CHAIN_HARDIRQ_CONTEXT)
		nr_hardirq_chains++;
	else if (irq_context & LOCK_CHAIN_SOFTIRQ_CONTEXT)
		nr_softirq_chains++;
	else
		nr_process_chains++;
}

/**
 * dec_chains - 按 IRQ 上下文减少已缓存 chain 分类计数
 * @irq_context: chain 发布时保存的上下文位集合
 *
 * 返回：无；使用与 inc_chains() 完全相同的 hardirq→softirq→process 优先级。
 * 边界：不做下溢保护，调用者必须保证每条已计数 chain 只回收一次并持有 graph_lock。
 */
static void dec_chains(int irq_context)
{
	/* 分类必须与增加侧一致，否则全局诊断计数会漂移。 */
	if (irq_context & LOCK_CHAIN_HARDIRQ_CONTEXT)
		nr_hardirq_chains--;
	else if (irq_context & LOCK_CHAIN_SOFTIRQ_CONTEXT)
		nr_softirq_chains--;
	else
		nr_process_chains--;
}

/**
 * print_deadlock_scenario - 打印同类递归获取的单 CPU 时序图
 * @nxt: 当前正尝试获取的新 held lock
 * @prv: 当前任务已经持有的同类 held lock
 *
 * 返回：无；先显示持有 prv，再显示获取 nxt，并提示可能缺少锁嵌套标注。
 * 所有权：只借用 held-lock 与 class；仅在已确认冲突、图已冻结的报告阶段调用。
 */
static void
print_deadlock_scenario(struct held_lock *nxt, struct held_lock *prv)
{
	/* next/prev 是对应静态 class 的借用指针，名称打印仍可附带具体实例信息。 */
	struct lock_class *next = hlock_class(nxt);
	struct lock_class *prev = hlock_class(prv);

	/* 单 CPU 上的两次同类获取已经足以展示潜在自等待。 */
	printk(" Possible unsafe locking scenario:\n\n");
	printk("       CPU0\n");
	printk("       ----\n");
	printk("  lock(");
	__print_lock_name(prv, prev);
	printk(KERN_CONT ");\n");
	printk("  lock(");
	__print_lock_name(nxt, next);
	printk(KERN_CONT ");\n");
	printk("\n *** DEADLOCK ***\n\n");
	printk(" May be due to missing lock nesting notation\n\n");
}

/**
 * print_deadlock_bug - 冻结 lockdep 并输出同类递归锁报告
 * @curr: 触发获取的任务
 * @prev: curr 已持有的同类锁记录
 * @next: curr 当前正尝试获取的新锁记录
 *
 * 返回：无；进入时持有 graph_lock。首个非静默报告者关闭 debug_locks 并释放图锁，打印两把锁、
 * 可选实例比较结果、单 CPU 场景、任务 held-lock 栈和当前调用栈；返回时不持有图锁。
 */
static void
print_deadlock_bug(struct task_struct *curr, struct held_lock *prev,
		   struct held_lock *next)
{
	/* cmp_fn 属于该共同 class，因此从已持有 prev 解析一次即可。 */
	struct lock_class *class = hlock_class(prev);

	/* 静默模式也会完成关闭和解锁，只跳过正文；非首个报告者直接返回。 */
	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return;

	/* 报告由多段 printk 和 dump_stack 组成，放进控制台紧急区。 */
	nbcon_cpu_emergency_enter();

	/* 标题和内核身份之后先显示正在获取的 next，再显示已经持有的 prev。 */
	pr_warn("\n");
	pr_warn("============================================\n");
	pr_warn("WARNING: possible recursive locking detected\n");
	print_kernel_ident();
	pr_warn("--------------------------------------------\n");
	pr_warn("%s/%d is trying to acquire lock:\n",
		curr->comm, task_pid_nr(curr));
	print_lock(next);
	pr_warn("\nbut task is already holding lock:\n");
	print_lock(prev);

	/* 自定义实例比较器存在时，附上它对 prev/next 的实际排序结果。 */
	if (class->cmp_fn) {
		pr_warn("and the lock comparison function returns %i:\n",
			class->cmp_fn(prev->instance, next->instance));
	}

	/* 场景图解释同类自等待，held-lock 列表和栈回溯补充完整现场。 */
	pr_warn("\nother info that might help us debug this:\n");
	print_deadlock_scenario(next, prev);
	lockdep_print_held_locks(curr);

	pr_warn("\nstack backtrace:\n");
	dump_stack();

	/* 统一结束控制台紧急报告。 */
	nbcon_cpu_emergency_exit();
}

/*
 * Check whether we are holding such a class already.
 *
 * (Note that this has to be done separately, because the graph cannot
 * detect such classes of deadlocks.)
 *
 * Returns: 0 on deadlock detected, 1 on OK, 2 if another lock with the same
 * lock class is held but nest_lock is also held, i.e. we rely on the
 * nest_lock to avoid the deadlock.
 */
/*
 * 检查当前任务是否已经持有同一锁类。该检查必须独立完成，因为依赖图把同类实例折叠为一个节点，
 * 无法自行识别这类递归死锁。返回 0 表示检测到死锁；1 表示正常；2 表示虽已持有同类锁，但同时
 * 持有声明的 nest_lock，可依赖它串行嵌套行为。
 */
/**
 * check_deadlock - 检查新 held-lock 是否与当前任务栈中的同类锁直接冲突
 * @curr: 当前任务及其稳定的 held_locks[0..lockdep_depth) 栈
 * @next: 尚待提交的新 held-lock，含 class、read、instance、nest_lock
 *
 * 返回：0 为已报告冲突，1 为无同类冲突，2 为由已持有 nest_lock 保护的同类嵌套。
 * 锁状态：进入时持有 graph_lock。返回 1/2 时仍持锁；返回 0 时 print_deadlock_bug() 已冻结 lockdep
 * 并释放图锁。
 */
static int
check_deadlock(struct task_struct *curr, struct held_lock *next)
{
	/* class 保存命中的共同锁类，prev 遍历现有栈，nest 记录已见到的嵌套串行锁。 */
	struct lock_class *class;
	struct held_lock *prev;
	struct held_lock *nest = NULL;
	int i;

	/* 按获取顺序扫描全部已持有记录。 */
	for (i = 0; i < curr->lockdep_depth; i++) {
		prev = curr->held_locks + i;

		/* 实例地址等于 next 声明的 nest_lock 时，后续同类冲突可依赖它获得顺序。 */
		if (prev->instance == next->nest_lock)
			nest = prev;

		if (hlock_class(prev) != hlock_class(next))
			continue;

		/*
		 * Allow read-after-read recursion of the same
		 * lock class (i.e. read_lock(lock)+read_lock(lock)):
		 */
		/* 允许同一锁类的 read-after-read 递归：next 标为递归读且 prev 也是任意读模式。 */
		if ((next->read == 2) && prev->read)
			continue;

		class = hlock_class(prev);

		/* class 自定义比较器返回负值表示 prev 实例严格排在 next 前，按定义该嵌套顺序合法。 */
		if (class->cmp_fn &&
		    class->cmp_fn(prev->instance, next->instance) < 0)
			continue;

		/*
		 * We're holding the nest_lock, which serializes this lock's
		 * nesting behaviour.
		 */
		/* 已持有 nest_lock 会串行同类实例的嵌套行为，返回 2 让上层记录这一特殊成功。 */
		if (nest)
			return 2;

		/* 没有合法豁免的同类重入：终止并报告，报告 helper 负责图锁。 */
		print_deadlock_bug(curr, prev, next);
		return 0;
	}
	/* 整个 held 栈没有冲突 class。 */
	return 1;
}

/*
 * There was a chain-cache miss, and we are about to add a new dependency
 * to a previous lock. We validate the following rules:
 *
 *  - would the adding of the <prev> -> <next> dependency create a
 *    circular dependency in the graph? [== circular deadlock]
 *
 *  - does the new prev->next dependency connect any hardirq-safe lock
 *    (in the full backwards-subgraph starting at <prev>) with any
 *    hardirq-unsafe lock (in the full forwards-subgraph starting at
 *    <next>)? [== illegal lock inversion with hardirq contexts]
 *
 *  - does the new prev->next dependency connect any softirq-safe lock
 *    (in the full backwards-subgraph starting at <prev>) with any
 *    softirq-unsafe lock (in the full forwards-subgraph starting at
 *    <next>)? [== illegal lock inversion with softirq contexts]
 *
 * any of these scenarios could lead to a deadlock.
 *
 * Then if all the validations pass, we add the forwards and backwards
 * dependency.
 */
/*
 * chain cache 未命中后，即将把前一把相关锁 <prev> 指向新锁 <next>。加入边以前依次证明：不会由
 * next 重新走回 prev 形成环；prev 的完整反向子图与 next 的完整正向子图之间不会连接 hardirq
 * safe/unsafe 或 softirq safe/unsafe 状态。全部验证通过后，同时建立正向与反向依赖记录。
 */
/**
 * check_prev_add - 验证并记录一个相关前驱到当前锁的依赖
 * @curr: 当前任务，供 IRQ 反转报告读取持锁上下文
 * @prev: 候选前驱 held lock
 * @next: 当前正获取的 held lock
 * @distance: 两者在 held_locks 栈中的距离
 * @trace: 本轮 acquisition 共用的依赖来源栈；需要新边时才惰性分配
 *
 * 返回：0 表示验证失败、资源不足或图内部不一致，1 表示已有双向边并完成属性合并，2 表示无需
 * 建边或成功新建双向边。正常的 1/2 返回仍持有 graph_lock。多数 0 路径已由 BFS、报告或分配
 * helper 释放图锁；已有正向边却缺少反向镜像的内部不一致分支是例外，它直接返回且仍持锁。
 * 副作用：可能更新已有边的 distance/dep，惰性保存 trace，或向 locks_after/locks_before 各加一项。
 */
static int
check_prev_add(struct task_struct *curr, struct held_lock *prev,
	       struct held_lock *next, u16 distance,
	       struct lock_trace **const trace)
{
	/* entry 遍历已有依赖，ret 承接环路、冗余与分配 helper 的结果。 */
	struct lock_list *entry;
	enum bfs_result ret;

	/* class key 消失说明动态 key 的生命周期可能早于仍在使用的 class。 */
	if (!hlock_class(prev)->key || !hlock_class(next)->key) {
		/*
		 * The warning statements below may trigger a use-after-free
		 * of the class name. It is better to trigger a use-after free
		 * and to have the class name most of the time instead of not
		 * having the class name available.
		 */
		/*
		 * 下列告警为尽量给出 class 名称，会冒着解引用已释放名称的风险；维护者在诊断价值与完全避开
		 * UAF 之间选择了前者，因为大多数现场仍能得到关键类名。
		 */
		/* 分别报告失效的前驱或后继 key；WARN_ONCE 避免同类错误持续刷屏。 */
		WARN_ONCE(!debug_locks_silent && !hlock_class(prev)->key,
			  "Detected use-after-free of lock class %px/%s\n",
			  hlock_class(prev),
			  hlock_class(prev)->name);
		WARN_ONCE(!debug_locks_silent && !hlock_class(next)->key,
			  "Detected use-after-free of lock class %px/%s\n",
			  hlock_class(next),
			  hlock_class(next)->name);
		/* 生命周期已不可信，不再碰图；2 让调用者把它当作“跳过此边”继续收尾。 */
		return 2;
	}

	/* 同一 class 可由实例比较器证明 prev 严格先于 next，从而合法省略自边。 */
	if (prev->class_idx == next->class_idx) {
		/* class 在 key 检查后有效，用其 cmp_fn 表达子类/地址等外部次序。 */
		struct lock_class *class = hlock_class(prev);

		if (class->cmp_fn &&
		    class->cmp_fn(prev->instance, next->instance) < 0)
			/* 有序同类嵌套无需加入可能被误认为递归的 class 自边。 */
			return 2;
	}

	/*
	 * Prove that the new <prev> -> <next> dependency would not
	 * create a circular dependency in the graph. (We do this by
	 * a breadth-first search into the graph starting at <next>,
	 * and check whether we can reach <prev>.)
	 *
	 * The search is limited by the size of the circular queue (i.e.,
	 * MAX_CIRCULAR_QUEUE_SIZE) which keeps track of a breadth of nodes
	 * in the graph whose neighbours are to be checked.
	 */
	/*
	 * 先证明新增 prev->next 不会成环：从 next 广度优先遍历，若可达 prev，则补上新边后闭环。搜索
	 * 宽度受 MAX_CIRCULAR_QUEUE_SIZE 限制；命中与搜索错误都由下层完成终止报告/解锁。
	 */
	ret = check_noncircular(next, prev, trace);
	if (unlikely(bfs_error(ret) || ret == BFS_RMATCH))
		return 0;

	/* 再证明这条边不会把 IRQ-safe 反向子图接到 IRQ-unsafe 正向子图。 */
	if (!check_irq_usage(curr, prev, next))
		return 0;

	/*
	 * Is the <prev> -> <next> dependency already present?
	 *
	 * (this may occur even though this is a new chain: consider
	 *  e.g. the L1 -> L2 -> L3 -> L4 and the L5 -> L1 -> L2 -> L3
	 *  chains - the second one will be new, but L1 already has
	 *  L2 added to its dependency list, due to the first chain.)
	 */
	/*
	 * 即便整个 chain 新出现，prev->next 直接边也可能早已存在。例如已有 L1->L2->L3->L4，随后
	 * 首见 L5->L1->L2->L3 时，L1->L2 不能重复分配，只需合并新观测到的边属性。
	 */
	/* 正向表以 prev 为头；找到同一 next class 就转入原地更新与镜像核对。 */
	list_for_each_entry(entry, &hlock_class(prev)->locks_after, entry) {
		if (entry->class == hlock_class(next)) {
			/* 任一相邻观测都足以把历史长距离边提升为直接边。 */
			if (distance == 1)
				entry->distance = 1;
			/* 同一 class 对可由不同读写模式出现，按位并入正向依赖类型。 */
			entry->dep |= calc_dep(prev, next);

			/*
			 * Also, update the reverse dependency in @next's
			 * ->locks_before list.
			 *
			 *  Here we reuse @entry as the cursor, which is fine
			 *  because we won't go to the next iteration of the
			 *  outer loop:
			 *
			 *  For normal cases, we return in the inner loop.
			 *
			 *  If we fail to return, we have inconsistency, i.e.
			 *  <prev>::locks_after contains <next> while
			 *  <next>::locks_before doesn't contain <prev>. In
			 *  that case, we return after the inner and indicate
			 *  something is wrong.
			 */
			/*
			 * 同步更新 next->locks_before 中的反向镜像。这里复用 entry 游标是安全的：正常情况会在
			 * 内层直接返回；若内层找不到镜像，图的双向表示已经不一致，也会立刻在外层继续前返回。
			 */
			list_for_each_entry(entry, &hlock_class(next)->locks_before, entry) {
				if (entry->class == hlock_class(prev)) {
					/* 两个方向必须携带一致的“是否直接”信息。 */
					if (distance == 1)
						entry->distance = 1;
					/* 反向表采用 calc_depb() 编码对应的读写端点。 */
					entry->dep |= calc_depb(prev, next);
					/* 已有边的两侧均更新完成，不消费新的 trace/list 节点。 */
					return 1;
				}
			}

			/* <prev> is not found in <next>::locks_before */
			/* next 的反向表缺少 prev，说明正反索引损坏；此罕见分支仍持有 graph_lock。 */
			return 0;
		}
	}

	/*
	 * Is the <prev> -> <next> link redundant?
	 */
	/* 尚无直接边时，检查是否已有等强或更强的间接路径可完整替代它。 */
	ret = check_redundant(prev, next);
	if (bfs_error(ret))
		/* BFS 错误路径已经关闭 lockdep 并释放图锁。 */
		return 0;
	else if (ret == BFS_RMATCH)
		/* 冗余边不落图；2 仍表示验证成功且 graph_lock 仍由调用者持有。 */
		return 2;

	/* 同一轮多个 prev 新边共享一份调用栈，直到确实要建第一条边才分配。 */
	if (!*trace) {
		*trace = save_trace();
		if (!*trace)
			/* trace 池耗尽时 save_trace() 已关闭调试并释放图锁。 */
			return 0;
	}

	/*
	 * Ok, all validations passed, add the new lock
	 * to the previous lock's dependency list:
	 */
	/* 全部证明完成，先在 prev->locks_after 中加入指向 next 的正向记录。 */
	ret = add_lock_to_list(hlock_class(next), hlock_class(prev),
			       &hlock_class(prev)->locks_after, distance,
			       calc_dep(prev, next), *trace);

	if (!ret)
		/* 节点不足时 helper 关闭并释放图锁，图中尚未加入本边。 */
		return 0;

	/* 再在 next->locks_before 建立 prev 镜像；两侧共享 distance 与 trace。 */
	ret = add_lock_to_list(hlock_class(prev), hlock_class(next),
			       &hlock_class(next)->locks_before, distance,
			       calc_depb(prev, next), *trace);
	if (!ret)
		/* 此时正向项已经写入；反向分配失败由 helper 解锁并终止 lockdep，不能在此回滚。 */
		return 0;

	/* 新双向边完成；2 与“合法省略”共用成功类别，调用者仅按真假判断。 */
	return 2;
}

/*
 * Add the dependency to all directly-previous locks that are 'relevant'.
 * The ones that are relevant are (in increasing distance from curr):
 * all consecutive trylock entries and the final non-trylock entry - or
 * the end of this context's lock-chain - whichever comes first.
 */
/*
 * 为当前锁加入所有“相关”的直接前驱。由近到远包括连续的 trylock 项，以及随后第一项非 trylock；
 * 若先到当前 IRQ context 的 chain 起点则在那里停止。非 trylock 以前的锁已通过它自己的直接边间接
 * 连到当前锁，无需再次建立跨越边。
 */
/**
 * check_prevs_add - 从当前任务持锁栈选择相关前驱并验证其到 next 的边
 * @curr: 当前任务及其 held_locks 栈
 * @next: 尚未提交到栈深度中的当前新锁
 *
 * 返回：1 表示所有相关前驱均验证/记录成功，调用者仍持有 graph_lock；0 表示某条边失败或栈状态
 * 不一致。trace 在整轮前驱间共享，避免同一次 acquisition 的边重复保存调用栈。
 */
static int
check_prevs_add(struct task_struct *curr, struct held_lock *next)
{
	/* trace 首次需要新边时由 check_prev_add() 填入，后续前驱复用。 */
	struct lock_trace *trace = NULL;
	/* depth 从 next 尚未提交时的 lockdep_depth 向栈底回退。 */
	int depth = curr->lockdep_depth;
	/* hlock 指向本轮选中的候选前驱。 */
	struct held_lock *hlock;

	/*
	 * Debugging checks.
	 *
	 * Depth must not be zero for a non-head lock:
	 */
	/* 非 chain head 调用必须至少已有一把锁，否则无法形成前驱边。 */
	if (!depth)
		goto out_bug;
	/*
	 * At least two relevant locks must exist for this
	 * to be a head:
	 */
	/* next 暂存在 held_locks[depth]；它必须与最后一项已提交锁属于同一 IRQ context。 */
	if (curr->held_locks[depth].irq_context !=
			curr->held_locks[depth-1].irq_context)
		goto out_bug;

	/* 从最近前驱开始，逐项覆盖连续 trylock 与第一项非 trylock。 */
	for (;;) {
		/* 栈下标差转为从 1 开始的依赖距离，最近前驱 distance=1。 */
		u16 distance = curr->lockdep_depth - depth + 1;
		/* depth 表示 next 的插入位置，前驱位于 depth-1。 */
		hlock = curr->held_locks + depth - 1;

		/* check==0 的 held lock 不参与依赖验证，但仍影响回退边界。 */
		if (hlock->check) {
			/* 0 可能伴随下层已解锁；调用者据失败结果立即停止。 */
			int ret = check_prev_add(curr, hlock, next, distance, &trace);
			if (!ret)
				return 0;

			/*
			 * Stop after the first non-trylock entry,
			 * as non-trylock entries have added their
			 * own direct dependencies already, so this
			 * lock is connected to them indirectly:
			 */
			/*
			 * 遇到第一项非 trylock 即停止：它在自己被获取时已连接更早的相关锁，所以当前 next 通过
			 * hlock 的新边可间接到达那些锁。trylock 可能未获取成功，不能承担这种传递连接保证。
			 */
			if (!hlock->trylock)
				break;
		}

		/* 当前候选处理完，向更早一层移动。 */
		depth--;
		/*
		 * End of lock-stack?
		 */
		/* 到栈底说明当前 context 中没有更早候选。 */
		if (!depth)
			break;
		/*
		 * Stop the search if we cross into another context:
		 */
		/* IRQ context 边界切断 chain；不能跨上下文补一条普通持锁顺序边。 */
		if (curr->held_locks[depth].irq_context !=
				curr->held_locks[depth-1].irq_context)
			break;
	}
	/* 所有相关前驱已处理，graph_lock 的所有权保持给 validate_chain()。 */
	return 1;
out_bug:
	/* 首次发现状态损坏时原子关闭 lockdep 并释放 graph_lock。 */
	if (!debug_locks_off_graph_unlock())
		/* 已有其他 CPU 关闭调试时避免重复告警。 */
		return 0;

	/*
	 * Clearly we all shouldn't be here, but since we made it we
	 * can reliable say we messed up our state. See the above two
	 * gotos for reasons why we could possibly end up here.
	 */
	/*
	 * 正常状态不应抵达此处；既然上述 depth/context 断言失败，就明确告警 held_locks 状态已被破坏。
	 */
	WARN_ON(1);

	/* 图锁已释放且 lockdep 已关闭，通知上层终止本次验证。 */
	return 0;
}

/* 全局 chain 描述符池；每项通过 base 指向 chain_hlocks 中连续的压缩 held-lock ID。 */
struct lock_chain lock_chains[MAX_LOCKDEP_CHAINS];
/* 位 i 为 1 表示 lock_chains[i] 已分配，清零项可被新 chain 复用；由 graph_lock 保护。 */
static DECLARE_BITMAP(lock_chains_in_use, MAX_LOCKDEP_CHAINS);
/* 紧凑存放 chain_hlock ID；空闲区自身前几个 u16 被复用为 freelist 元数据。 */
static u16 chain_hlocks[MAX_LOCKDEP_CHAIN_HLOCKS];
/* 被 zapping 回收的 chain 描述符累计数，供 lockdep_stats 观察回收活动。 */
unsigned long nr_zapped_lock_chains;
/* 各分桶当前可重新分配的 chain_hlocks 项总数。 */
unsigned int nr_free_chain_hlocks;	/* Free chain_hlocks in buckets */
/* 分桶中可用的空闲 chain_hlocks 数量。 */
/* 小于两个 u16、无法编码 freelist 指针而永久丢失的项数。 */
unsigned int nr_lost_chain_hlocks;	/* Lost chain_hlocks */
/* 已丢失、不能再进入 chain_hlocks 空闲分桶的项数。 */
/* bucket-0 中尺寸超过固定桶上限的可变长空闲块数。 */
unsigned int nr_large_chain_blocks;	/* size > MAX_CHAIN_BUCKETS */
/* 尺寸大于 MAX_CHAIN_BUCKETS 的空闲 chain block 数。 */

/*
 * The first 2 chain_hlocks entries in the chain block in the bucket
 * list contains the following meta data:
 *
 *   entry[0]:
 *     Bit    15 - always set to 1 (it is not a class index)
 *     Bits 0-14 - upper 15 bits of the next block index
 *   entry[1]    - lower 16 bits of next block index
 *
 * A next block index of all 1 bits means it is the end of the list.
 *
 * On the unsized bucket (bucket-0), the 3rd and 4th entries contain
 * the chain block size:
 *
 *   entry[2] - upper 16 bits of the chain block size
 *   entry[3] - lower 16 bits of the chain block size
 */
/*
 * 空闲 chain block 的前两个 u16 就地编码链表元数据：entry[0] 的 bit15 固定为 1，既表明这不是
 * 普通 class index，也与其余 15 位共同保存 next block index 的高半部；entry[1] 保存低 16 位。
 * 全 1 的 next 表示链尾。无固定尺寸的 bucket-0 还用 entry[2..3] 保存 32 位 block size。
 */
/* 固定尺寸桶覆盖 2..16 项；0 号桶保存尺寸大于上限的可变长块。 */
#define MAX_CHAIN_BUCKETS	16
/* 复用 class_idx 不会使用的最高位，区分空闲块头与正常 chain_hlock。 */
#define CHAIN_BLK_FLAG		(1U << 15)
/* 编码后的全 1 next index，chain_block_next() 将其转换为 -1。 */
#define CHAIN_BLK_LIST_END	0xFFFFU

/* 每个元素保存对应空闲链表的首块 offset，-1 表示空桶；由 graph_lock 串行访问。 */
static int chain_block_buckets[MAX_CHAIN_BUCKETS];

/**
 * size_to_bucket - 把空闲块尺寸映射到固定尺寸桶或可变尺寸桶
 * @size: 以 u16 项计的 block 大小，调用者应传正数
 *
 * 返回：size 为 1..MAX_CHAIN_BUCKETS 时返回 size-1；更大尺寸统一进入 bucket 0。
 * 注意：size==1 与大块都映射到 0，但单项块不会真正加入 freelist。
 */
static inline int size_to_bucket(int size)
{
	/* 超过固定桶覆盖范围的块统一交给按尺寸降序组织的可变长链表。 */
	if (size > MAX_CHAIN_BUCKETS)
		return 0;

	/* 固定尺寸使用零基桶号；size=1 的冲突由 add_chain_block() 先行过滤。 */
	return size - 1;
}

/*
 * Iterate all the chain blocks in a bucket.
 */
/* 遍历一个桶中的全部 chain block。 */
/*
 * 从桶头开始同时维护前驱与当前 offset；循环尾通过块头编码的 next 前进，-1 结束。调用体可在
 * curr 处停止，并用 prev 判断它是桶头还是链中节点。
 */
#define for_each_chain_block(bucket, prev, curr)		\
	for ((prev) = -1, (curr) = chain_block_buckets[bucket];	\
	     (curr) >= 0;					\
	     (prev) = (curr), (curr) = chain_block_next(curr))

/*
 * next block or -1
 */
/* 返回下一个空闲块 offset；当前块是链尾时返回 -1。 */
/**
 * chain_block_next - 解码空闲块头中的 next block index
 * @offset: chain_hlocks 中空闲块首项的下标
 *
 * 返回：后继块 offset，CHAIN_BLK_LIST_END 则为 -1。若首项没有 CHAIN_BLK_FLAG 会告警一次，
 * 但仍按既定格式继续解码，便于暴露 freelist 损坏。
 */
static inline int chain_block_next(int offset)
{
	/* next 暂存 entry[0]：包含标志位和 index 高 15 位。 */
	int next = chain_hlocks[offset];

	/* 空闲块头必须可由最高位区别于合法 class index。 */
	WARN_ON_ONCE(!(next & CHAIN_BLK_FLAG));

	/* 0xffff 是链尾哨兵，无需再组合 entry[1]。 */
	if (next == CHAIN_BLK_LIST_END)
		return -1;

	/* 清标志后恢复高 15 位，再拼接 entry[1] 的低 16 位。 */
	next &= ~CHAIN_BLK_FLAG;
	next <<= 16;
	next |= chain_hlocks[offset + 1];

	/* 返回 chain_hlocks 数组内的下一个空闲块起点。 */
	return next;
}

/*
 * bucket-0 only
 */
/* 仅供 bucket-0 使用。 */
/**
 * chain_block_size - 解码可变长空闲块保存的 32 位尺寸
 * @offset: bucket-0 中空闲块的首项下标
 *
 * 返回：entry[2] 高 16 位与 entry[3] 低 16 位拼成的 u16 项数；无副作用。
 */
static inline int chain_block_size(int offset)
{
	/* 固定尺寸桶由桶号隐含 size，只有 0 号桶需要这两个额外元数据项。 */
	return (chain_hlocks[offset + 2] << 16) | chain_hlocks[offset + 3];
}

/**
 * init_chain_block - 在空闲块头编码后继、桶类别及可选尺寸
 * @offset: 要初始化的空闲块首项
 * @next: 后继块 offset，-1 会自然编码为 CHAIN_BLK_LIST_END
 * @bucket: 所属桶；0 表示可变尺寸桶
 * @size: 块尺寸；仅在非零且 bucket==0 时写入 entry[2..3]
 *
 * 副作用：覆盖 chain_hlocks[offset] 起的两个或四个 u16 元数据项；调用者负责范围与图锁。
 */
static inline void init_chain_block(int offset, int next, int bucket, int size)
{
	/* next 的高部与 freelist 标志同存 entry[0]，低部截断写入 entry[1]。 */
	chain_hlocks[offset] = (next >> 16) | CHAIN_BLK_FLAG;
	chain_hlocks[offset + 1] = (u16)next;

	/* 可变长块必须显式保存 size；更新已有链指针时 size==0 可保留原尺寸。 */
	if (size && !bucket) {
		chain_hlocks[offset + 2] = size >> 16;
		chain_hlocks[offset + 3] = (u16)size;
	}
}

/**
 * add_chain_block - 把一段 chain_hlocks 空闲区加入合适分桶
 * @offset: 空闲区首项下标
 * @size: 以 u16 项计的空闲区长度
 *
 * 固定尺寸块头插对应桶；大块进入 bucket-0 并按尺寸从大到小排序，支持 worst-fit。小于两个 u16
 * 无法保存 next 指针而被舍弃。调用者持有 graph_lock；函数更新空闲/丢失/大块统计。
 */
static inline void add_chain_block(int offset, int size)
{
	/* bucket 是目标分桶，next 是原桶头；prev/curr 用于大块有序插入。 */
	int bucket = size_to_bucket(size);
	int next = chain_block_buckets[bucket];
	int prev, curr;

	/* freelist 指针至少占两个 u16；零长度忽略，单项长度计为永久碎片。 */
	if (unlikely(size < 2)) {
		/*
		 * We can't store single entries on the freelist. Leak them.
		 *
		 * One possible way out would be to uniquely mark them, other
		 * than with CHAIN_BLK_FLAG, such that we can recover them when
		 * the block before it is re-added.
		 */
		/*
		 * 单个 entry 无法编码 freelist 指针，只能泄漏。可能的改进是用区别于 CHAIN_BLK_FLAG 的唯一
		 * 标记，使其前一块日后重新加入时能识别并合并该单项碎片。
		 */
		if (size)
			/* 只统计确实丢失的一个 entry，不把空区间算入。 */
			nr_lost_chain_hlocks++;
		return;
	}

	/* 从现在起整块可由分配器再次使用。 */
	nr_free_chain_hlocks += size;
	if (!bucket) {
		/* bucket-0 专门统计大块数量，便于观察碎片化。 */
		nr_large_chain_blocks++;

		/*
		 * Variable sized, sort large to small.
		 */
		/* 可变尺寸块按从大到小排列，首个能服务请求的就是 worst-fit 候选。 */
		for_each_chain_block(0, prev, curr) {
			if (size >= chain_block_size(curr))
				break;
		}
		/* 新块指向首个不比它大的 curr，并完整写入自身尺寸。 */
		init_chain_block(offset, curr, 0, size);
		if (prev < 0)
			/* 没有前驱说明成为新的最大块和桶头。 */
			chain_block_buckets[0] = offset;
		else
			/* 链中插入时只改 prev 的 next，size=0 保留 prev 原有尺寸元数据。 */
			init_chain_block(prev, offset, 0, 0);
		return;
	}
	/*
	 * Fixed size, add to head.
	 */
	/* 固定尺寸桶无需排序，直接把新块压到原桶头之前。 */
	init_chain_block(offset, next, bucket, size);
	chain_block_buckets[bucket] = offset;
}

/*
 * Only the first block in the list can be deleted.
 *
 * For the variable size bucket[0], the first block (the largest one) is
 * returned, broken up and put back into the pool. So if a chain block of
 * length > MAX_CHAIN_BUCKETS is ever used and zapped, it will just be
 * queued up after the primordial chain block and never be used until the
 * hlock entries in the primordial chain block is almost used up. That
 * causes fragmentation and reduce allocation efficiency. That can be
 * monitored by looking at the "large chain blocks" number in lockdep_stats.
 */
/*
 * 只能删除链表首块。对可变尺寸 bucket[0]，分配器取出最大的首块、拆分，并把余块放回池中。因此
 * 某个长度超过 MAX_CHAIN_BUCKETS 的 chain 被回收后，它会排在初始大块之后；直到初始块几乎耗尽
 * 才会再被采用，造成碎片并降低分配效率。lockdep_stats 的 large chain blocks 可用于监控此现象。
 */
/**
 * del_chain_block - 从指定分桶摘除已知的首块
 * @bucket: 要修改的桶号
 * @size: 被摘首块的 u16 项数
 * @next: 已解码的后继块 offset，或 -1
 *
 * 前置条件：目标必须是 chain_block_buckets[bucket] 当前首块。副作用：推进桶头并扣减空闲统计；
 * bucket-0 还扣减大块计数。块内容不清零，随后会被分配方覆盖。
 */
static inline void del_chain_block(int bucket, int size, int next)
{
	/* 这批 u16 从空闲池转为已分配或待拆分状态。 */
	nr_free_chain_hlocks -= size;
	/* 删除仅支持桶头，所以直接将桶入口推进到调用者预读的 next。 */
	chain_block_buckets[bucket] = next;

	/* 只有可变尺寸桶的节点计入 large chain blocks。 */
	if (!bucket)
		nr_large_chain_blocks--;
}

/**
 * init_chain_block_buckets - 初始化 chain_hlocks 空闲分配器
 *
 * 将所有桶置空，再把完整 chain_hlocks 数组作为一个可变长空闲块加入 bucket-0。应仅在一次性
 * lockdep 数据结构初始化期间、图状态尚未并发可见时调用。
 */
static void init_chain_block_buckets(void)
{
	/* i 逐个覆盖固定大小的桶头数组。 */
	int i;

	/* -1 与 chain_block_next() 的链尾返回约定一致。 */
	for (i = 0; i < MAX_CHAIN_BUCKETS; i++)
		chain_block_buckets[i] = -1;

	/* 初始只有一个覆盖整个存储区的大块，同时建立各项空闲统计。 */
	add_chain_block(0, ARRAY_SIZE(chain_hlocks));
}

/*
 * Return offset of a chain block of the right size or -1 if not found.
 *
 * Fairly simple worst-fit allocator with the addition of a number of size
 * specific free lists.
 */
/*
 * 返回尺寸合适的 chain block offset，找不到时返回 -1。它是带多个精确尺寸 freelist 的简易
 * worst-fit 分配器：优先精确桶，其次取 bucket-0 最大块，最后拆更大的固定尺寸块。
 */
/**
 * alloc_chain_hlocks - 从分桶空闲池分配连续的 chain_hlock 项
 * @req: 请求的 u16 项数
 *
 * 返回：成功时为 chain_hlocks 内起始 offset，失败为 -1。实际 freelist 编码要求最少占两项，
 * 因此 req==1 也消耗两项。调用者持有 graph_lock；函数可能拆块并更新全部空闲统计。
 */
static int alloc_chain_hlocks(int req)
{
	/* bucket 为搜索桶号，curr 为候选首块，size 为候选/扫描尺寸。 */
	int bucket, curr, size;

	/*
	 * We rely on the MSB to act as an escape bit to denote freelist
	 * pointers. Make sure this bit isn't set in 'normal' class_idx usage.
	 */
	/*
	 * freelist 用最高位作为逃逸标志，所以正常 class_idx 的最大合法值必须保证该位恒为 0；编译期
	 * 断言防止调整 MAX_LOCKDEP_KEYS 后两种编码相撞。
	 */
	BUILD_BUG_ON((MAX_LOCKDEP_KEYS-1) & CHAIN_BLK_FLAG);

	/* 确保桶头和初始整块只初始化一次，然后才能读取空闲统计。 */
	init_data_structures_once();

	/* 先按调用者真正需求快速拒绝，避免无意义扫描。 */
	if (nr_free_chain_hlocks < req)
		return -1;

	/*
	 * We require a minimum of 2 (u16) entries to encode a freelist
	 * 'pointer'.
	 */
	/* 即便只存一个 hlock，也要保留两个 u16，日后释放时才能就地编码 next。 */
	req = max(req, 2);
	/* 2..16 映射精确桶，更大的请求映射可变尺寸桶。 */
	bucket = size_to_bucket(req);
	curr = chain_block_buckets[bucket];

	/* 固定尺寸请求优先命中完全等大的块，避免产生碎片。 */
	if (bucket) {
		if (curr >= 0) {
			/* 读取 next 后删除桶头，整块正好交给调用者。 */
			del_chain_block(bucket, req, chain_block_next(curr));
			return curr;
		}
		/* Try bucket 0 */
		/* 精确桶为空，再尝试从可变长大块切割。 */
		curr = chain_block_buckets[0];
	}

	/*
	 * The variable sized freelist is sorted by size; the first entry is
	 * the largest. Use it if it fits.
	 */
	/* bucket-0 按降序排列，首项就是 worst-fit 的最大候选；足够大才可使用。 */
	if (curr >= 0) {
		size = chain_block_size(curr);
		if (likely(size >= req)) {
			/* 先整块摘除，避免链表在拆分期间暴露重叠区间。 */
			del_chain_block(0, size, chain_block_next(curr));
			if (size > req)
				/* 尾部余量以新 offset/size 重新插回恰当分桶。 */
				add_chain_block(curr + req, size - req);
			/* 前 req 项归本次 chain 所有。 */
			return curr;
		}
	}

	/*
	 * Last resort, split a block in a larger sized bucket.
	 */
	/*
	 * 最后从最大的固定尺寸桶向下找比 req 大的首块。这里不查等大桶（前面已查）也不查较小桶。
	 */
	for (size = MAX_CHAIN_BUCKETS; size > req; size--) {
		bucket = size_to_bucket(size);
		curr = chain_block_buckets[bucket];
		if (curr < 0)
			continue;

		/* 摘下较大整块，将 req 后的尾部按新尺寸重新分类。 */
		del_chain_block(bucket, size, chain_block_next(curr));
		add_chain_block(curr + req, size - req);
		return curr;
	}

	/* 总空闲量虽可能足够，但没有任何单个连续块能够满足请求。 */
	return -1;
}

/**
 * free_chain_hlocks - 把 chain 的连续 hlock 区间归还空闲池
 * @base: chain_hlocks 中区间起点
 * @size: chain 实际保存的 hlock 数
 *
 * 单元素 chain 分配时占了两个 u16，释放时同样按至少两项归还；不做相邻块合并。
 */
static inline void free_chain_hlocks(int base, int size)
{
	/* 与 alloc_chain_hlocks() 的最小分配单位保持对称。 */
	add_chain_block(base, max(size, 2));
}

/**
 * lock_chain_get_class - 取得 chain 中第 i 个压缩 hlock 对应的 lock class
 * @chain: 已分配且 base/深度有效的 chain
 * @i: chain 内零基下标，调用者保证不越界
 *
 * 返回：全局 lock_classes 池中的 class 指针；只读取压缩 ID，不改变引用或统计。
 */
struct lock_class *lock_chain_get_class(struct lock_chain *chain, int i)
{
	/* chain_hlock 同时编码 class_idx 和读模式；先按 base+i 取原始值。 */
	u16 chain_hlock = chain_hlocks[chain->base + i];
	/* 去除读模式位，恢复 lock_classes 的数组下标。 */
	unsigned int class_idx = chain_hlock_class_idx(chain_hlock);

	/* class 池为静态数组，索引生命周期由 chain 有效性保证。 */
	return lock_classes + class_idx;
}

/*
 * Returns the index of the first held_lock of the current chain
 */
/* 返回当前 chain 在 held_locks 栈中的第一项下标。 */
/**
 * get_first_held_lock - 向前定位与目标锁同一 IRQ context 的持锁链起点
 * @curr: 当前任务及其已提交 held_locks 栈
 * @hlock: 用于确定目标 irq_context 的新锁或链中锁
 *
 * 返回：当前相同 irq_context 连续后缀的首下标；若全部已持锁都同 context 则为 0。只读任务栈。
 */
static inline int get_first_held_lock(struct task_struct *curr,
					struct held_lock *hlock)
{
	/* i 反向扫描下标，hlock_curr 指向每个已提交 held lock。 */
	int i;
	struct held_lock *hlock_curr;

	/* 从栈顶向前走，直到越过栈底或遇到另一 IRQ context。 */
	for (i = curr->lockdep_depth - 1; i >= 0; i--) {
		hlock_curr = curr->held_locks + i;
		if (hlock_curr->irq_context != hlock->irq_context)
			break;

	}

	/* 循环停在边界项上，前移一位得到当前 chain 的第一项。 */
	return ++i;
}

#ifdef CONFIG_DEBUG_LOCKDEP
/*
 * Returns the next chain_key iteration
 */
/* 打印并返回 chain_key 的下一次迭代结果。 */
/**
 * print_chain_key_iteration - 展示一个 hlock_id 如何推进 chain_key
 * @hlock_id: 本步加入哈希的压缩 held-lock 标识
 * @chain_key: 加入本项以前的累计 key
 *
 * 返回：iterate_chain_key() 计算的新 key；同时向控制台输出输入 ID 与新值，仅供 DEBUG_LOCKDEP
 * 的碰撞诊断路径使用。
 */
static u64 print_chain_key_iteration(u16 hlock_id, u64 chain_key)
{
	/* 与正常建链完全相同的迭代公式，保证诊断可逐步重放。 */
	u64 new_chain_key = iterate_chain_key(chain_key, hlock_id);

	/* 固定 16 位十六进制宽度便于比较每一步 hash 状态。 */
	printk(" hlock_id:%d -> chain_key:%016Lx",
		(unsigned int)hlock_id,
		(unsigned long long)new_chain_key);
	/* 下一轮以本轮输出作为累计输入。 */
	return new_chain_key;
}

/**
 * print_chain_keys_held_locks - 重放当前任务链及待加入锁的 key 迭代
 * @curr: 当前任务及已提交持锁栈
 * @hlock_next: 本次将加入 chain 的新 held lock
 *
 * 从相同 IRQ context 的第一项开始，打印深度、每个压缩 ID、累计 key 与锁详情，最后打印新锁。
 * 仅用于碰撞报告，调用者已进入紧急控制台上下文。
 */
static void
print_chain_keys_held_locks(struct task_struct *curr, struct held_lock *hlock_next)
{
	/* hlock 遍历已提交项；chain_key 从协议初值重算；depth 固定扫描上界。 */
	struct held_lock *hlock;
	u64 chain_key = INITIAL_CHAIN_KEY;
	int depth = curr->lockdep_depth;
	/* i 跳过其他 IRQ context 中更早的 held locks。 */
	int i = get_first_held_lock(curr, hlock_next);

	/* 深度包含现有同 context 后缀以及尚未入栈的 hlock_next。 */
	printk("depth: %u (irq_context %u)\n", depth - i + 1,
		hlock_next->irq_context);
	/* 依次重放已提交后缀，使输出可与缓存 chain 的逐项结果对照。 */
	for (; i < depth; i++) {
		hlock = curr->held_locks + i;
		chain_key = print_chain_key_iteration(hlock_id(hlock), chain_key);

		/* 紧跟 hash 步骤打印实例/class 信息。 */
		print_lock(hlock);
	}

	/* 新锁是当前 chain 最后一项；无需再保存返回 key。 */
	print_chain_key_iteration(hlock_id(hlock_next), chain_key);
	print_lock(hlock_next);
}

/**
 * print_chain_keys_chain - 重放一个已缓存 chain 的 key 迭代
 * @chain: 与当前计算 key 相撞的缓存 chain
 *
 * 打印缓存深度、逐项压缩 ID、累计 key 及 class 名，用于同当前 held-lock 序列逐行比较。
 */
static void print_chain_keys_chain(struct lock_chain *chain)
{
	/* i 遍历缓存区；chain_key 重置为协议初值；hlock_id 保存当前压缩项。 */
	int i;
	u64 chain_key = INITIAL_CHAIN_KEY;
	u16 hlock_id;

	/* 缓存深度决定从 base 起读取的有效项数。 */
	printk("depth: %u\n", chain->depth);
	for (i = 0; i < chain->depth; i++) {
		hlock_id = chain_hlocks[chain->base + i];
		chain_key = print_chain_key_iteration(hlock_id, chain_key);

		/* 缓存不保留实例 held_lock，只能由压缩 class_idx 打印类名。 */
		print_lock_name(NULL, lock_classes + chain_hlock_class_idx(hlock_id));
		printk("\n");
	}
}

/**
 * print_collision - 输出当前链与同 key 缓存链的完整碰撞证据
 * @curr: 当前任务及 held-lock 序列
 * @hlock_next: 计算 chain_key 时尚未提交的新锁
 * @chain: key 相同但内容不一致的缓存 chain
 *
 * 副作用：进入 nbcon CPU 紧急区，打印内核身份、任务、两条逐步 key 序列及栈回溯，然后退出。
 */
static void print_collision(struct task_struct *curr,
			struct held_lock *hlock_next,
			struct lock_chain *chain)
{
	/* 碰撞通常发生在敏感锁路径，紧急区保证控制台输出可推进。 */
	nbcon_cpu_emergency_enter();

	/* 先给出醒目的报告头和触发任务身份。 */
	pr_warn("\n");
	pr_warn("============================\n");
	pr_warn("WARNING: chain_key collision\n");
	print_kernel_ident();
	pr_warn("----------------------------\n");
	pr_warn("%s/%d: ", current->comm, task_pid_nr(current));
	pr_warn("Hash chain already cached but the contents don't match!\n");

	/* 第一组重放当前正在形成的真实 held-lock chain。 */
	pr_warn("Held locks:");
	print_chain_keys_held_locks(curr, hlock_next);

	/* 第二组重放 hash 表中使用同一 key 的已有 chain。 */
	pr_warn("Locks in cached chain:");
	print_chain_keys_chain(chain);

	/* 最后保留调用栈，定位哪条 acquire 路径触发碰撞。 */
	pr_warn("\nstack backtrace:\n");
	dump_stack();

	/* 报告结束，恢复普通控制台状态。 */
	nbcon_cpu_emergency_exit();
}
#endif

/*
 * Checks whether the chain and the current held locks are consistent
 * in depth and also in content. If they are not it most likely means
 * that there was a collision during the calculation of the chain_key.
 * Returns: 0 not passed, 1 passed
 */
/*
 * 检查缓存 chain 与当前 held locks 的深度和内容是否一致。不一致通常意味着 chain_key 计算发生
 * 哈希碰撞。返回 0 表示未通过，1 表示通过；非 DEBUG_LOCKDEP 构建不做逐项验证而固定通过。
 */
/**
 * check_no_collision - 对同 key 的缓存 chain 做深度与已提交前缀确认
 * @curr: 当前任务及已提交持锁栈
 * @hlock: 尚未提交、但属于待查 chain 最后一项的新锁
 * @chain: chain cache 命中的候选
 *
 * 返回：DEBUG_LOCKDEP 下深度和已提交前缀一致为 1，发现差异为 0 并打印碰撞；其他配置为 1。
 * 新 hlock 本身已参与调用者命中的 chain_key，这里显式比较的是缓存中它之前的 held-lock 前缀。
 */
static int check_no_collision(struct task_struct *curr,
			struct held_lock *hlock,
			struct lock_chain *chain)
{
#ifdef CONFIG_DEBUG_LOCKDEP
	/* i 是任务栈起点/游标，j 是缓存 chain 下标，id 是当前任务侧压缩 ID。 */
	int i, j, id;

	/* 只比较与新锁相同 IRQ context 的连续 held-lock 后缀。 */
	i = get_first_held_lock(curr, hlock);

	/* 缓存深度应等于已有后缀长度加 1 个新锁。 */
	if (DEBUG_LOCKS_WARN_ON(chain->depth != curr->lockdep_depth - (i - 1))) {
		print_collision(curr, hlock, chain);
		return 0;
	}

	/* 缓存最后一项是参数 hlock；先逐项核对之前的已提交部分。 */
	for (j = 0; j < chain->depth - 1; j++, i++) {
		id = hlock_id(&curr->held_locks[i]);

		if (DEBUG_LOCKS_WARN_ON(chain_hlocks[chain->base + j] != id)) {
			/* 首个内容差异足以确认哈希碰撞，输出两条完整序列。 */
			print_collision(curr, hlock, chain);
			return 0;
		}
	}
#endif
	/* 调用者已用 chain_key 命中；调试核验未发现反证即可接受缓存。 */
	return 1;
}

/*
 * Given an index that is >= -1, return the index of the next lock chain.
 * Return -2 if there is no next lock chain.
 */
/* 给定一个不小于 -1 的索引，返回下一条已用 lock chain 的索引；不存在则返回 -2。 */
/**
 * lockdep_next_lockchain - 枚举 lock_chains_in_use 中下一项已分配 chain
 * @i: 上一次返回的索引；传 -1 可从第 0 项开始
 *
 * 返回：下一已置位索引，越过数组末尾时为 -2。该接口不加 graph_lock，调用者需接受快照变化或
 * 在外层提供同步，主要供诊断枚举使用。
 */
long lockdep_next_lockchain(long i)
{
	/* 从 i+1 开始找位，确保不会重复返回调用者刚处理的 chain。 */
	i = find_next_bit(lock_chains_in_use, ARRAY_SIZE(lock_chains), i + 1);
	/* bitmap helper 用 size 表示未命中，外部 API 将其转换为专用 -2 哨兵。 */
	return i < ARRAY_SIZE(lock_chains) ? i : -2;
}

/**
 * lock_chain_count - 统计当前已分配的 lock chain 数
 *
 * 返回：lock_chains_in_use 中置位数；只读 bitmap，适用于状态/统计展示。
 */
unsigned long lock_chain_count(void)
{
	/* bitmap 是 chain 描述符分配状态的唯一真值，避免遍历 hash 表。 */
	return bitmap_weight(lock_chains_in_use, ARRAY_SIZE(lock_chains));
}

/* Must be called with the graph lock held. */
/* 必须在持有 graph lock 时调用。 */
/**
 * alloc_lock_chain - 从静态 lock_chain 描述符池分配一项
 *
 * 返回：首个空闲描述符，池满则为 NULL。成功会先设置对应 in-use 位；调用者随后初始化字段并挂入
 * hash。graph_lock 串行保护 find/set 组合。
 */
static struct lock_chain *alloc_lock_chain(void)
{
	/* idx 是 bitmap 中首个零位，也是 lock_chains 数组下标。 */
	int idx = find_first_zero_bit(lock_chains_in_use,
				      ARRAY_SIZE(lock_chains));

	/* 找不到零位时 helper 返回 bitmap 大小。 */
	if (unlikely(idx >= ARRAY_SIZE(lock_chains)))
		return NULL;
	/* 在返回指针前占用槽位，防止同一图锁临界区后的其他分配重复选择。 */
	__set_bit(idx, lock_chains_in_use);
	return lock_chains + idx;
}

/*
 * Adds a dependency chain into chain hashtable. And must be called with
 * graph_lock held.
 *
 * Return 0 if fail, and graph_lock is released.
 * Return 1 if succeed, with graph_lock held.
 */
/*
 * 把依赖 chain 加入 chain hash，且必须持有 graph_lock。失败返回 0 并释放 graph_lock；成功返回 1
 * 并继续持锁。函数同时分配描述符与压缩 hlock 区间，复制同 IRQ context 的当前链。
 */
/**
 * add_chain_cache - 创建并缓存当前 acquisition 对应的完整依赖链
 * @curr: 当前任务及已提交 held-lock 栈
 * @hlock: 尚未提交、作为新 chain 最后一项的锁
 * @chain_key: 已把 hlock 迭代进去的最终 chain hash key
 *
 * 返回：1 表示缓存成功且 graph_lock 仍持有；0 表示锁断言或容量失败，容量报告路径释放图锁并
 * 关闭 lockdep。成功会更新 miss 和 IRQ-context chain 统计。
 */
static inline int add_chain_cache(struct task_struct *curr,
				  struct held_lock *hlock,
				  u64 chain_key)
{
	/* hash_head 是目标桶；chain 是新描述符；i/j 分别遍历任务栈与压缩数组。 */
	struct hlist_head *hash_head = chainhashentry(chain_key);
	struct lock_chain *chain;
	int i, j;

	/*
	 * The caller must hold the graph lock, ensure we've got IRQs
	 * disabled to make this an IRQ-safe lock.. for recursion reasons
	 * lockdep won't complain about its own locking errors.
	 */
	/*
	 * 调用者必须持图锁；这里同时确认 IRQ 已关闭，令内部图锁对中断安全。lockdep 为避免递归不会报告
	 * 自己的锁错误，因此必须显式断言这项入口契约。
	 */
	if (lockdep_assert_locked())
		return 0;

	/* 先占用一个 chain 描述符；耗尽会永久关闭本轮 lockdep。 */
	chain = alloc_lock_chain();
	if (!chain) {
		/* 只有首次关闭者负责输出容量诊断；helper 同时释放 graph_lock。 */
		if (!debug_locks_off_graph_unlock())
			return 0;

		/* 紧急控制台区保证严重容量错误和栈回溯尽量完整输出。 */
		nbcon_cpu_emergency_enter();
		print_lockdep_off("BUG: MAX_LOCKDEP_CHAINS too low!");
		dump_stack();
		nbcon_cpu_emergency_exit();
		return 0;
	}
	/* 描述符先记录查找 key 与新锁所在 IRQ context。 */
	chain->chain_key = chain_key;
	chain->irq_context = hlock->irq_context;
	/* i 定位同 context 连续后缀，depth 再包含尚未入栈的 hlock。 */
	i = get_first_held_lock(curr, hlock);
	chain->depth = curr->lockdep_depth + 1 - i;

	/* base/depth/class_idx 的位域宽度必须覆盖对应静态池的最大下标。 */
	BUILD_BUG_ON((1UL << 24) <= ARRAY_SIZE(chain_hlocks));
	BUILD_BUG_ON((1UL << 6)  <= ARRAY_SIZE(curr->held_locks));
	BUILD_BUG_ON((1UL << 8*sizeof(chain_hlocks[0])) <= ARRAY_SIZE(lock_classes));

	/* 为整个 chain 分配连续压缩 ID 存储；单项也按分配器规则占至少两格。 */
	j = alloc_chain_hlocks(chain->depth);
	if (j < 0) {
		/* 描述符位已占用，但 lockdep 即将关闭；现有路径不尝试回滚该槽位。 */
		if (!debug_locks_off_graph_unlock())
			return 0;

		/* 报告 chain_hlocks 容量不足这一独立上限。 */
		nbcon_cpu_emergency_enter();
		print_lockdep_off("BUG: MAX_LOCKDEP_CHAIN_HLOCKS too low!");
		dump_stack();
		nbcon_cpu_emergency_exit();
		return 0;
	}

	/* base 固化连续区间起点；先复制新锁之前的所有同 context held locks。 */
	chain->base = j;
	for (j = 0; j < chain->depth - 1; j++, i++) {
		/* lock_id 压缩 class_idx 与读模式，足以重建依赖 chain 内容。 */
		int lock_id = hlock_id(curr->held_locks + i);

		chain_hlocks[chain->base + j] = lock_id;
	}
	/* 最后一项来自尚未写入 curr->held_locks 的 hlock 参数。 */
	chain_hlocks[chain->base + j] = hlock_id(hlock);
	/* 完整初始化后再以 RCU 可见方式发布到 chain hash 桶头。 */
	hlist_add_head_rcu(&chain->entry, hash_head);
	/* 新增缓存意味着此前 lookup miss，并按 IRQ context 更新 chain 总数。 */
	debug_atomic_inc(chain_lookup_misses);
	inc_chains(chain->irq_context);

	/* 发布完成，graph_lock 仍交还给 validate_chain() 的后续阶段。 */
	return 1;
}

/*
 * Look up a dependency chain. Must be called with either the graph lock or
 * the RCU read lock held.
 */
/* 查找依赖 chain；调用者必须持有 graph_lock 或 RCU read lock。 */
/**
 * lookup_chain_cache - 按 chain_key 查询已发布的依赖链
 * @chain_key: 完整持锁链的累计 key
 *
 * 返回：首个 key 相等的缓存 chain，未命中为 NULL。这里只比较哈希值；DEBUG_LOCKDEP 调用者可再用
 * check_no_collision() 核对深度和内容。命中会增加 chain_lookup_hits。
 */
static inline struct lock_chain *lookup_chain_cache(u64 chain_key)
{
	/* hash_head 由 key 低位映射；chain 是 RCU 安全遍历游标。 */
	struct hlist_head *hash_head = chainhashentry(chain_key);
	struct lock_chain *chain;

	/* 发布/删除与 RCU 协调，READ_ONCE 避免 key 读取被编译器撕裂或合并。 */
	hlist_for_each_entry_rcu(chain, hash_head, entry) {
		if (READ_ONCE(chain->chain_key) == chain_key) {
			/* hit 表示无需再次验证和构造这条常见链。 */
			debug_atomic_inc(chain_lookup_hits);
			return chain;
		}
	}
	/* 桶内无同 key 项，调用方可能升级到 graph_lock 并新增缓存。 */
	return NULL;
}

/*
 * If the key is not present yet in dependency chain cache then
 * add it and return 1 - in this case the new dependency chain is
 * validated. If the key is already hashed, return 0.
 * (On return with 1 graph_lock is held.)
 */
/*
 * dependency chain cache 尚无该 key 时加入并返回 1，此时新链还需验证；key 已存在则返回 0。只有
 * 返回 1 时 graph_lock 由本函数替调用者保持，0 返回均不把图锁所有权交给调用者。
 */
/**
 * lookup_chain_cache_add - 以双重检查方式命中或创建 chain cache 项
 * @curr: 当前任务及已提交 held-lock 栈
 * @hlock: 本次新获取、作为 chain 尾项的 held lock
 * @chain_key: 包含 hlock 后的完整累计 key
 *
 * 返回：已有缓存为 0；成功新增为 1 且 graph_lock 保持持有；加锁或容量失败为 0。不取图锁的首查优化
 * 常见 hit，miss 后获取图锁并二次查询以避免并发重复插入。
 */
static inline int lookup_chain_cache_add(struct task_struct *curr,
					 struct held_lock *hlock,
					 u64 chain_key)
{
	/* class 只用于 verbose 输出，chain 保存两阶段 lookup 的候选。 */
	struct lock_class *class = hlock_class(hlock);
	struct lock_chain *chain = lookup_chain_cache(chain_key);

	/* 第一阶段命中不需要获取 graph_lock，直接核验调试期碰撞。 */
	if (chain) {
cache_hit:
		/* 深度/前缀不一致表示同 key 内容碰撞，报告后终止本次验证。 */
		if (!check_no_collision(curr, hlock, chain))
			return 0;

		/* 极详细模式记录为何这条 acquisition 没有重复做依赖图证明。 */
		if (very_verbose(class)) {
			printk("\nhash chain already cached, key: "
					"%016Lx tail class: [%px] %s\n",
					(unsigned long long)chain_key,
					class->key, class->name);
		}

		/* cache hit 用 0 表示“不需要验证新 chain”，不是错误。 */
		return 0;
	}

	/* 首查 miss 可按类过滤输出即将创建的新 key。 */
	if (very_verbose(class)) {
		printk("\nnew hash chain, key: %016Lx tail class: [%px] %s\n",
			(unsigned long long)chain_key, class->key, class->name);
	}

	/* 修改 hash/分配池前取得全局图锁；调试已关闭时获取失败并直接跳过。 */
	if (!graph_lock())
		return 0;

	/*
	 * We have to walk the chain again locked - to avoid duplicates:
	 */
	/* 拿锁后必须二次查询，覆盖首查与加锁之间由另一 CPU 发布同 key 的竞态。 */
	chain = lookup_chain_cache(chain_key);
	if (chain) {
		/* cache_hit 路径按无图锁契约运行，所以先释放后复用统一逻辑。 */
		graph_unlock();
		goto cache_hit;
	}

	/* 仍未命中时在持锁状态创建；失败路径由 add_chain_cache() 负责释放图锁。 */
	if (!add_chain_cache(curr, hlock, chain_key))
		return 0;

	/* 新 cache 项已发布，调用者必须继续验证依赖并最终释放 graph_lock。 */
	return 1;
}

/**
 * validate_chain - 对新持锁链执行同类重入、前驱边及跨图死锁验证
 * @curr: 当前任务及其已提交 held_locks
 * @hlock: 本次将加入栈的新 held lock
 * @chain_head: 非零表示 hlock 是当前 IRQ context chain 的首项
 * @chain_key: 已迭代 hlock 后的完整 chain key
 *
 * 返回：1 表示无需新验证或验证通过；0 表示死锁/碰撞、资源失败或 lockdep 已停止。trylock 与
 * check==0 仍维护持锁栈但不新增依赖。cache miss 路径由 lookup helper 取得 graph_lock，本函数正常
 * 完成时释放它；失败通常由报告/容量 helper 解锁，check_prev_add() 已记录的镜像不一致例外仍持锁。
 */
static int validate_chain(struct task_struct *curr,
			  struct held_lock *hlock,
			  int chain_head, u64 chain_key)
{
	/*
	 * Trylock needs to maintain the stack of held locks, but it
	 * does not add new dependencies, because trylock can be done
	 * in any order.
	 *
	 * We look up the chain_key and do the O(N^2) check and update of
	 * the dependencies only if this is a new dependency chain.
	 * (If lookup_chain_cache_add() return with 1 it acquires
	 * graph_lock for us)
	 */
	/*
	 * trylock 可按任意顺序尝试，因此只维护 held-lock 栈而不建立依赖。普通且启用 check 的锁先查
	 * chain cache；只有新 chain 才做 O(N^2) 验证/更新，lookup 返回 1 时已替本函数持有图锁。
	 */
	if (!hlock->trylock && hlock->check &&
	    lookup_chain_cache_add(curr, hlock, chain_key)) {
		/*
		 * Check whether last held lock:
		 *
		 * - is irq-safe, if this lock is irq-unsafe
		 * - is softirq-safe, if this lock is hardirq-unsafe
		 *
		 * And check whether the new lock's dependency graph
		 * could lead back to the previous lock:
		 *
		 * - within the current held-lock stack
		 * - across our accumulated lock dependency records
		 *
		 * any of these scenarios could lead to a deadlock.
		 */
		/*
		 * 新链需要检查：当前栈是否已持有相同 class；IRQ safe/unsafe 是否反转；以及新锁从栈内或
		 * 累积依赖图能否回到前驱形成环。任何一种都可能死锁。
		 */
		/*
		 * The simple case: does the current hold the same lock
		 * already?
		 */
		/* 简单第一关先扫 held_locks，识别无豁免的同 class 递归获取。 */
		int ret = check_deadlock(curr, hlock);

		/* 0 表示终止报告已经处理图锁，立即停止提交新锁。 */
		if (!ret)
			return 0;
		/*
		 * Add dependency only if this lock is not the head
		 * of the chain, and if the new lock introduces no more
		 * lock dependency (because we already hold a lock with the
		 * same lock class) nor deadlock (because the nest_lock
		 * serializes nesting locks), see the comments for
		 * check_deadlock().
		 */
		/*
		 * chain head 没有前驱；ret==2 表示同 class 已由 cmp_fn/nest_lock 等规则合法处理，新增普通
		 * class 依赖既无必要也可能制造自边。其余情况才把所有相关前驱接到 hlock。
		 */
		if (!chain_head && ret != 2) {
			/* 任一前驱验证失败就终止；具体图锁状态由 check_prev_add() 的失败分支决定。 */
			if (!check_prevs_add(curr, hlock))
				return 0;
		}

		/* 新 chain 的所有验证/依赖更新完成，释放 lookup 阶段取得的 graph_lock。 */
		graph_unlock();
	} else {
		/* after lookup_chain_cache_add(): */
		/* lookup 后若全局调试已被错误路径关闭，本次 acquisition 不能再视为验证成功。 */
		if (unlikely(!debug_locks))
			return 0;
	}

	/* cache hit、策略跳过或新链完整通过，允许上层提交 hlock。 */
	return 1;
}
#else
/**
 * validate_chain - 未编译 PROVE_LOCKING 时的依赖链验证 stub
 * @curr: 未使用
 * @hlock: 未使用
 * @chain_head: 未使用
 * @chain_key: 未使用
 *
 * 返回：固定 1；此配置仍可执行外围持锁记账，但不构建/验证依赖图。
 */
static inline int validate_chain(struct task_struct *curr,
				 struct held_lock *hlock,
				 int chain_head, u64 chain_key)
{
	return 1;
}

/**
 * init_chain_block_buckets - 未编译 PROVE_LOCKING 时的初始化 stub
 *
 * 无输入、无返回值、无副作用，因为该配置不分配 chain_hlocks。
 */
static void init_chain_block_buckets(void)	{ }
#endif /* CONFIG_PROVE_LOCKING */

/*
 * We are building curr_chain_key incrementally, so double-check
 * it from scratch, to make sure that it's done correctly:
 */
/* curr_chain_key 平时增量构造；这里从头重算一次，确认每步保存和最终结果都正确。 */
/**
 * check_chain_key - 调试构建中重放任务 held-lock 栈并校验增量 chain key
 * @curr: 要核验 lockdep_depth、held_locks 与 curr_chain_key 的任务
 *
 * 逐项先检查 hlock->prev_chain_key，再验证 class_idx 已注册；跨 IRQ context 时把本段累计值重置为
 * INITIAL_CHAIN_KEY 后继续迭代。任一不一致会关闭 lockdep 并告警。非 DEBUG_LOCKDEP 时为空操作。
 */
static void check_chain_key(struct task_struct *curr)
{
#ifdef CONFIG_DEBUG_LOCKDEP
	/* hlock/prev_hlock 表示当前与前一栈项；i 遍历深度；chain_key 是从头重算值。 */
	struct held_lock *hlock, *prev_hlock = NULL;
	unsigned int i;
	u64 chain_key = INITIAL_CHAIN_KEY;

	/* 按实际 acquisition 顺序重放每个已提交 held lock。 */
	for (i = 0; i < curr->lockdep_depth; i++) {
		hlock = curr->held_locks + i;
		/* 每个栈项保存“加入自己之前”的 key，必须等于当前重放状态。 */
		if (chain_key != hlock->prev_chain_key) {
			/* 状态已不可信，先冻结后报告首个失配位置和两个 key。 */
			debug_locks_off();
			/*
			 * We got mighty confused, our chain keys don't match
			 * with what we expect, someone trample on our task state?
			 */
			/* chain key 与预期不符，很可能有代码踩坏了该任务的 lockdep 栈状态。 */
			WARN(1, "hm#1, depth: %u [%u], %016Lx != %016Lx\n",
				curr->lockdep_depth, i,
				(unsigned long long)chain_key,
				(unsigned long long)hlock->prev_chain_key);
			return;
		}

		/*
		 * hlock->class_idx can't go beyond MAX_LOCKDEP_KEYS, but is
		 * it registered lock class index?
		 */
		/* class_idx 位宽保证不越 MAX_LOCKDEP_KEYS，但还必须确认该槽当前确实已注册。 */
		if (DEBUG_LOCKS_WARN_ON(!test_bit(hlock->class_idx, lock_classes_in_use)))
			return;

		/* 新 IRQ context 开始独立 chain；prev_chain_key 的核对已在重置前完成。 */
		if (prev_hlock && (prev_hlock->irq_context !=
							hlock->irq_context))
			chain_key = INITIAL_CHAIN_KEY;
		/* 把当前 class/read ID 纳入本 context 的累计 key。 */
		chain_key = iterate_chain_key(chain_key, hlock_id(hlock));
		prev_hlock = hlock;
	}
	/* 重放最终值必须等于任务快速路径维护的 curr_chain_key。 */
	if (chain_key != curr->curr_chain_key) {
		debug_locks_off();
		/*
		 * More smoking hash instead of calculating it, damn see these
		 * numbers float.. I bet that a pink elephant stepped on my memory.
		 */
		/* 最终 hash 飘移说明增量计算或任务内存遭破坏，输出深度及期望/实际值。 */
		WARN(1, "hm#2, depth: %u [%u], %016Lx != %016Lx\n",
			curr->lockdep_depth, i,
			(unsigned long long)chain_key,
			(unsigned long long)curr->curr_chain_key);
	}
#endif
}

#ifdef CONFIG_PROVE_LOCKING
/* usage 状态转换的核心更新函数；前置声明供下面报告/检查路径互相组织调用。 */
static int mark_lock(struct task_struct *curr, struct held_lock *this,
		     enum lock_usage_bit new_bit);

/**
 * print_usage_bug_scenario - 打印同一锁在任务与中断间自死锁的最小示意
 * @lock: 发生互斥 usage 状态转换的 held lock
 *
 * 只输出 CPU0 获取锁、被中断、处理器再次获取同锁的概念序列；不改变 lockdep 或图锁状态。
 */
static void print_usage_bug_scenario(struct held_lock *lock)
{
	/* class 提供稳定名称，lock 提供 subclass/实例等打印上下文。 */
	struct lock_class *class = hlock_class(lock);

	/* 用紧凑 ASCII 时序突出“持锁时被中断、同 CPU 再取同锁”的闭环。 */
	printk(" Possible unsafe locking scenario:\n\n");
	printk("       CPU0\n");
	printk("       ----\n");
	printk("  lock(");
	__print_lock_name(lock, class);
	printk(KERN_CONT ");\n");
	printk("  <Interrupt>\n");
	printk("    lock(");
	__print_lock_name(lock, class);
	printk(KERN_CONT ");\n");
	printk("\n *** DEADLOCK ***\n\n");
}

/**
 * print_usage_bug - 报告同一 class 已登记 usage 与新 usage 的直接冲突
 * @curr: 触发状态转换的当前任务
 * @this: 本次获取/状态标记对应的 held lock
 * @prev_bit: class 上已经存在、与新状态互斥的 usage bit
 * @new_bit: 当前试图登记的 usage bit
 *
 * 首次报告会关闭 lockdep，进入紧急控制台上下文，输出两种状态、旧状态 trace、IRQ 事件、持锁栈
 * 与调用栈。若调试已关闭或 silent，则不重复输出。
 */
static void
print_usage_bug(struct task_struct *curr, struct held_lock *this,
		enum lock_usage_bit prev_bit, enum lock_usage_bit new_bit)
{
	/* debug_locks_off() 只有首个关闭者返回真；silent 模式同样压制正文。 */
	if (!debug_locks_off() || debug_locks_silent)
		return;

	/* 锁错误可能发生在控制台敏感上下文，使用 nbcon 紧急区包住完整报告。 */
	nbcon_cpu_emergency_enter();

	pr_warn("\n");
	pr_warn("================================\n");
	pr_warn("WARNING: inconsistent lock state\n");
	print_kernel_ident();
	pr_warn("--------------------------------\n");

	pr_warn("inconsistent {%s} -> {%s} usage.\n",
		usage_str[prev_bit], usage_str[new_bit]);

	/* 同时给出 hard/soft IRQ 嵌套计数与当前 enable 状态，解释转换发生的执行环境。 */
	pr_warn("%s/%d [HC%u[%lu]:SC%u[%lu]:HE%u:SE%u] takes:\n",
		curr->comm, task_pid_nr(curr),
		lockdep_hardirq_context(), hardirq_count() >> HARDIRQ_SHIFT,
		lockdep_softirq_context(curr), softirq_count() >> SOFTIRQ_SHIFT,
		lockdep_hardirqs_enabled(),
		lockdep_softirqs_enabled(curr));
	print_lock(this);

	/* class 在首次登记 prev_bit 时保存的 trace 是冲突的历史一侧。 */
	pr_warn("{%s} state was registered at:\n", usage_str[prev_bit]);
	print_lock_trace(hlock_class(this)->usage_traces[prev_bit], 1);

	/* 补充最近 IRQ 开关位置、概念死锁图和任务当前全部持锁。 */
	print_irqtrace_events(curr);
	pr_warn("\nother info that might help us debug this:\n");
	print_usage_bug_scenario(this);

	lockdep_print_held_locks(curr);

	/* 当前栈是冲突的新 usage 一侧。 */
	pr_warn("\nstack backtrace:\n");
	dump_stack();

	nbcon_cpu_emergency_exit();
}

/*
 * Print out an error if an invalid bit is set:
 */
/* 若 class 已设置与新状态互斥的 bit，则输出错误。 */
/**
 * valid_state - 验证 class 上不存在指定的冲突 usage bit
 * @curr: 当前任务，供冲突报告使用
 * @this: 正在更新 usage 的 held lock
 * @new_bit: 拟设置的新 usage bit
 * @bad_bit: 与 new_bit 互斥、必须保持清零的旧 bit
 *
 * 返回：未见 bad_bit 为 1；冲突为 0。冲突时先释放 graph_lock，再由 print_usage_bug() 关闭
 * lockdep 和输出报告，避免持内部图锁进入较长打印路径。
 */
static inline int
valid_state(struct task_struct *curr, struct held_lock *this,
	    enum lock_usage_bit new_bit, enum lock_usage_bit bad_bit)
{
	/* usage_mask 按 enum lock_usage_bit 的位号保存 class 历史状态。 */
	if (unlikely(hlock_class(this)->usage_mask & (1 << bad_bit))) {
		/* 报告不要求图锁，且可能打印大量状态，先缩短临界区。 */
		graph_unlock();
		print_usage_bug(curr, this, bad_bit, new_bit);
		return 0;
	}
	/* 这里只证明一个 bad_bit；调用者会按状态机逐个检查所有互斥位。 */
	return 1;
}


/*
 * print irq inversion bug:
 */
/* 打印 IRQ 上下文引入的依赖反转错误。 */
/**
 * print_irq_inversion_bug - 报告 usage 变化使已有依赖路径变成 IRQ 反转
 * @curr: 触发 usage 变化的任务
 * @root: 以 this class 构造的 BFS 临时根
 * @other: 搜索命中的不兼容 usage 节点
 * @this: 本次 usage 状态发生变化的 held lock
 * @forwards: 非零表示从 this 正向找到 unsafe 锁，零表示反向找到 safe 锁
 * @irqclass: hardirq/softirq 及读写状态名称
 *
 * 首次报告会关闭 lockdep 并释放 graph_lock，重建搜索路径中的 middle 节点，输出两 CPU 反转场景、
 * 当前持锁、最短依赖与栈回溯。silent 或已经关闭时直接返回。
 */
static void
print_irq_inversion_bug(struct task_struct *curr,
			struct lock_list *root, struct lock_list *other,
			struct held_lock *this, int forwards,
			const char *irqclass)
{
	/* entry 从命中端沿 parent 回溯，middle 保存靠近 root/other 的场景连接点。 */
	struct lock_list *entry = other;
	struct lock_list *middle = NULL;
	/* depth 是 parent 链剩余长度，也用于防御损坏路径。 */
	int depth;

	/* 原子关闭 lockdep 并释放图锁；只有首个且非 silent 报告者继续。 */
	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return;

	/* 保证控制台在 IRQ/锁异常现场仍能推进整份诊断。 */
	nbcon_cpu_emergency_enter();

	pr_warn("\n");
	pr_warn("========================================================\n");
	pr_warn("WARNING: possible irq lock inversion dependency detected\n");
	print_kernel_ident();
	pr_warn("--------------------------------------------------------\n");
	pr_warn("%s/%d just changed the state of lock:\n",
		curr->comm, task_pid_nr(curr));
	print_lock(this);
	/* 搜索方向决定历史关系是 this 曾获取 unsafe 锁，还是曾被 safe 锁获取。 */
	if (forwards)
		pr_warn("but this lock took another, %s-unsafe lock in the past:\n", irqclass);
	else
		pr_warn("but this lock was taken by another, %s-safe lock in the past:\n", irqclass);
	print_lock_name(NULL, other->class);
	pr_warn("\n\nand interrupts could create inverse lock ordering between them.\n\n");

	pr_warn("\nother info that might help us debug this:\n");

	/* Find a middle lock (if one exists) */
	/* 如路径不止一条边，沿 BFS parent 链找一个中间 class 供紧凑两 CPU 场景展示。 */
	depth = get_lock_depth(other);
	do {
		/* depth 用尽却尚未回到 root，说明 parent 链与先前计算不一致。 */
		if (depth == 0 && (entry != root)) {
			pr_warn("lockdep:%s bad path found in chain graph\n", __func__);
			break;
		}
		middle = entry;
		entry = get_lock_parent(entry);
		depth--;
	} while (entry && entry != root && (depth >= 0));
	/* 按搜索方向调整两端顺序，保证示意图始终表达实际反向关系。 */
	if (forwards)
		print_irq_lock_scenario(root, other,
			middle ? middle->class : root->class, other->class);
	else
		print_irq_lock_scenario(other, root,
			middle ? middle->class : other->class, root->class);

	lockdep_print_held_locks(curr);

	/* 为临时 root 保存当前侧 trace，随后打印命中端到 root 的最短依赖路径。 */
	pr_warn("\nthe shortest dependencies between 2nd lock and 1st lock:\n");
	root->trace = save_trace();
	if (!root->trace)
		goto out;
	print_shortest_lock_dependencies(other, root);

	/* 当前调用栈定位引入不兼容 usage 的代码位置。 */
	pr_warn("\nstack backtrace:\n");
	dump_stack();
out:
	/* trace 分配失败也必须退出紧急控制台区。 */
	nbcon_cpu_emergency_exit();
}

/*
 * Prove that in the forwards-direction subgraph starting at <this>
 * there is no lock matching <mask>:
 */
/* 证明从 <this> 出发的正向子图中不存在匹配 <mask> 的锁。 */
/**
 * check_usage_forwards - 检查 this 的后继子图没有指定 IRQ usage
 * @curr: 当前任务，供反转报告使用
 * @this: usage 正在变化的搜索根 held lock
 * @bit: 要排除的写 usage bit；函数同时检查对应 read bit
 *
 * 返回：无匹配为 1 且 graph_lock 仍持有；匹配或 BFS 错误为 0，报告 helper 关闭 lockdep 并释放
 * 图锁。匹配时根据目标 class 实际置位选择写或读状态名称。
 */
static int
check_usage_forwards(struct task_struct *curr, struct held_lock *this,
		     enum lock_usage_bit bit)
{
	/* ret 是 BFS 结果；root 是 this 临时根；target_entry 接收首次匹配节点。 */
	enum bfs_result ret;
	struct lock_list root;
	struct lock_list *target_entry;
	/* read_bit 是同状态的读变体，usage_mask 允许任一模式命中。 */
	enum lock_usage_bit read_bit = bit + LOCK_USAGE_READ_MASK;
	unsigned usage_mask = lock_flag(bit) | lock_flag(read_bit);

	/* 正向根保留 this 的读模式，强路径规则由 find_usage_forwards() 统一执行。 */
	bfs_init_root(&root, this);
	ret = find_usage_forwards(&root, usage_mask, &target_entry);
	if (bfs_error(ret)) {
		/* BFS 错误报告负责关闭 lockdep 和释放图锁。 */
		print_bfs_bug(ret);
		return 0;
	}
	/* 完整后继子图没有不兼容 usage，证明通过。 */
	if (ret == BFS_RNOMATCH)
		return 1;

	/* Check whether write or read usage is the match */
	/* 检查命中的是写 usage 还是其读 usage，以便报告精确 irqclass。 */
	if (target_entry->class->usage_mask & lock_flag(bit)) {
		print_irq_inversion_bug(curr, &root, target_entry,
					this, 1, state_name(bit));
	} else {
		print_irq_inversion_bug(curr, &root, target_entry,
					this, 1, state_name(read_bit));
	}

	/* 命中路径已经由反转报告终止并解锁。 */
	return 0;
}

/*
 * Prove that in the backwards-direction subgraph starting at <this>
 * there is no lock matching <mask>:
 */
/* 证明从 <this> 出发的反向子图中不存在匹配 <mask> 的锁。 */
/**
 * check_usage_backwards - 检查 this 的前驱子图没有指定 IRQ usage
 * @curr: 当前任务，供反转报告使用
 * @this: usage 正在变化的反向搜索根 held lock
 * @bit: 要排除的写 usage bit；函数同时检查对应 read bit
 *
 * 返回：无匹配为 1 并保持 graph_lock；匹配或 BFS 错误为 0，报告路径负责关闭 lockdep 并解锁。
 */
static int
check_usage_backwards(struct task_struct *curr, struct held_lock *this,
		      enum lock_usage_bit bit)
{
	/* 与正向版本对应，只是根和遍历方向改为 locks_before。 */
	enum bfs_result ret;
	struct lock_list root;
	struct lock_list *target_entry;
	/* 同时覆盖写/读两种 usage，避免只查一半状态空间。 */
	enum lock_usage_bit read_bit = bit + LOCK_USAGE_READ_MASK;
	unsigned usage_mask = lock_flag(bit) | lock_flag(read_bit);

	/* bfs_init_rootb() 依据 this 的读模式设置反向强路径根。 */
	bfs_init_rootb(&root, this);
	ret = find_usage_backwards(&root, usage_mask, &target_entry);
	if (bfs_error(ret)) {
		print_bfs_bug(ret);
		return 0;
	}
	/* 未找到不兼容前驱，允许登记新 usage。 */
	if (ret == BFS_RNOMATCH)
		return 1;

	/* Check whether write or read usage is the match */
	/* 根据目标真实置位选择写状态或 read 状态名称。 */
	if (target_entry->class->usage_mask & lock_flag(bit)) {
		print_irq_inversion_bug(curr, &root, target_entry,
					this, 0, state_name(bit));
	} else {
		print_irq_inversion_bug(curr, &root, target_entry,
					this, 0, state_name(read_bit));
	}

	/* 命中已生成 IRQ inversion 报告并释放图锁。 */
	return 0;
}

/**
 * print_irqtrace_events - 打印任务最近的 hardirq/softirq 开关事件
 * @curr: 提供 irqtrace 快照的任务
 *
 * 输出事件序号以及 hardirq/softirq 最近 enable/disable 的事件号、地址和符号。函数自行进入并退出
 * nbcon CPU 紧急区，可被其他 lockdep 错误报告嵌套调用。
 */
void print_irqtrace_events(struct task_struct *curr)
{
	/* trace 借用 curr 内嵌快照，报告期间只读。 */
	const struct irqtrace_events *trace = &curr->irqtrace;

	/* IRQ 错误现场可能限制普通控制台推进，使用紧急上下文。 */
	nbcon_cpu_emergency_enter();

	/* 总事件戳帮助判断下列四个最近事件的相对先后。 */
	printk("irq event stamp: %u\n", trace->irq_events);
	printk("hardirqs last  enabled at (%u): [<%px>] %pS\n",
		trace->hardirq_enable_event, (void *)trace->hardirq_enable_ip,
		(void *)trace->hardirq_enable_ip);
	printk("hardirqs last disabled at (%u): [<%px>] %pS\n",
		trace->hardirq_disable_event, (void *)trace->hardirq_disable_ip,
		(void *)trace->hardirq_disable_ip);
	printk("softirqs last  enabled at (%u): [<%px>] %pS\n",
		trace->softirq_enable_event, (void *)trace->softirq_enable_ip,
		(void *)trace->softirq_enable_ip);
	printk("softirqs last disabled at (%u): [<%px>] %pS\n",
		trace->softirq_disable_event, (void *)trace->softirq_disable_ip,
		(void *)trace->softirq_disable_ip);

	/* 所有地址和符号输出完成后恢复调用前控制台状态。 */
	nbcon_cpu_emergency_exit();
}

/**
 * HARDIRQ_verbose - 判断是否为指定 class 输出 hardirq 状态细节
 * @class: 候选 lock class
 *
 * 返回：HARDIRQ_VERBOSE 编译开关开启时采用 class_filter()，否则固定 0。
 */
static int HARDIRQ_verbose(struct lock_class *class)
{
#if HARDIRQ_VERBOSE
	/* 详细级别开启后仍受全局 class 过滤条件约束。 */
	return class_filter(class);
#endif
	/* 编译期关闭 hardirq 详细输出。 */
	return 0;
}

/**
 * SOFTIRQ_verbose - 判断是否为指定 class 输出 softirq 状态细节
 * @class: 候选 lock class
 *
 * 返回：SOFTIRQ_VERBOSE 开启时为 class_filter() 结果，否则固定 0。
 */
static int SOFTIRQ_verbose(struct lock_class *class)
{
#if SOFTIRQ_VERBOSE
	/* 与 hardirq 版本共享同一 class 过滤策略。 */
	return class_filter(class);
#endif
	/* 编译期关闭 softirq 详细输出。 */
	return 0;
}

/* 按 lockdep 状态枚举顺序保存各状态的 verbose 判定函数。 */
static int (*state_verbose_f[])(struct lock_class *class) = {
/* 展开为 HARDIRQ_verbose、SOFTIRQ_verbose 等函数项，顺序必须与 lockdep_states.h 一致。 */
#define LOCKDEP_STATE(__STATE) \
	__STATE##_verbose,
#include "lockdep_states.h"
#undef LOCKDEP_STATE
};

/**
 * state_verbose - 分派某个 usage bit 所属 IRQ 状态的 verbose 策略
 * @bit: 包含 state、方向与读写维度的 lock usage bit
 * @class: 要交给状态过滤器判断的 lock class
 *
 * 返回：对应 state 的 HARDIRQ_verbose()/SOFTIRQ_verbose() 等结果；只读分发表。
 */
static inline int state_verbose(enum lock_usage_bit bit,
				struct lock_class *class)
{
	/* 去掉低位方向/读写编码后得到 lockdep state 索引。 */
	return state_verbose_f[bit >> LOCK_USAGE_DIR_MASK](class);
}

/* usage 子检查的统一签名：任务、held lock、bit 和可读名称，返回真假状态。 */
typedef int (*check_usage_f)(struct task_struct *, struct held_lock *,
			     enum lock_usage_bit bit, const char *name);

/**
 * mark_lock_irq - 验证一个 IRQ usage bit 与 class 自身及依赖子图相容
 * @curr: 当前任务，供冲突报告读取上下文
 * @this: 要登记 usage 的 held lock
 * @new_bit: 拟设置的 IRQ usage bit，含 state/方向/读写维度
 *
 * 返回：0 表示直接状态或依赖图冲突并已走终止路径；1 表示验证通过；2 表示通过且该 state/class
 * 命中 verbose 过滤。入口持有 graph_lock；成功保持持锁，失败报告路径释放。
 */
static int
mark_lock_irq(struct task_struct *curr, struct held_lock *this,
		enum lock_usage_bit new_bit)
{
	/* excl_bit 是互斥方向，read/dir 分别提取读写与 ENABLED/USED_IN 维度。 */
	int excl_bit = exclusive_bit(new_bit);
	int read = new_bit & LOCK_USAGE_READ_MASK;
	int dir = new_bit & LOCK_USAGE_DIR_MASK;

	/*
	 * Validate that this particular lock does not have conflicting
	 * usage states.
	 */
	/* 第一层：同一 class 不能已经登记 new_bit 的互斥状态。 */
	if (!valid_state(curr, this, new_bit, excl_bit))
		return 0;

	/*
	 * Check for read in write conflicts
	 */
	/* 新状态是写模式时，还要排除互斥方向的 read usage；反之读状态不排斥历史写以外的额外项。 */
	if (!read && !valid_state(curr, this, new_bit,
				  excl_bit + LOCK_USAGE_READ_MASK))
		return 0;


	/*
	 * Validate that the lock dependencies don't have conflicting usage
	 * states.
	 */
	/* 第二层：class 自身无冲突后，还要证明已有依赖子图不会把两种互斥 usage 连起来。 */
	if (dir) {
		/*
		 * mark ENABLED has to look backwards -- to ensure no dependee
		 * has USED_IN state, which, again, would allow  recursion deadlocks.
		 */
		/* 标记 ENABLED 必须向前驱回查，确保没有 dependee 已处于 USED_IN，否则中断可递归取锁。 */
		if (!check_usage_backwards(curr, this, excl_bit))
			return 0;
	} else {
		/*
		 * mark USED_IN has to look forwards -- to ensure no dependency
		 * has ENABLED state, which would allow recursion deadlocks.
		 */
		/* 标记 USED_IN 必须向后继正查，确保没有 dependency 已处于 ENABLED。 */
		if (!check_usage_forwards(curr, this, excl_bit))
			return 0;
	}

	/* 验证通过后，verbose 命中用返回 2 通知 mark_lock() 输出状态变化详情。 */
	if (state_verbose(new_bit, hlock_class(this)))
		return 2;

	/* 普通成功，无需额外打印。 */
	return 1;
}

/*
 * Mark all held locks with a usage bit:
 */
/* 给当前任务持有的全部锁标记一个 usage bit。 */
/**
 * mark_held_locks - 把同一 IRQ enable usage 应用到任务全部可检查 held locks
 * @curr: 要扫描 held_locks[0..lockdep_depth) 的任务
 * @base_bit: 写锁形态的基础 ENABLED usage bit
 *
 * 每个读锁把 bit 平移到对应 READ 状态，check==0 项跳过。返回 1 表示全部标记成功，任一
 * mark_lock() 失败则返回 0；此前已成功项不会回滚。
 */
static int
mark_held_locks(struct task_struct *curr, enum lock_usage_bit base_bit)
{
	/* hlock 指向当前栈项，i 按 acquisition 顺序遍历。 */
	struct held_lock *hlock;
	int i;

	for (i = 0; i < curr->lockdep_depth; i++) {
		/* 每项从调用者给定的写 usage 开始，再按实际 read 模式调整。 */
		enum lock_usage_bit hlock_bit = base_bit;
		hlock = curr->held_locks + i;

		/* 任意非零 read 编码都登记到 usage 状态的 READ 象限。 */
		if (hlock->read)
			hlock_bit += LOCK_USAGE_READ_MASK;

		/* state/方向/read 组合必须落在生成的 usage 枚举范围内。 */
		BUG_ON(hlock_bit >= LOCK_USAGE_STATES);

		/* 不参与依赖验证的锁也不影响 IRQ usage 状态机。 */
		if (!hlock->check)
			continue;

		/* mark_lock() 自行获取/释放图锁并保存首次 usage trace。 */
		if (!mark_lock(curr, hlock, hlock_bit))
			return 0;
	}

	/* 整个持锁栈已更新，或没有需检查项。 */
	return 1;
}

/*
 * Hardirqs will be enabled:
 */
/* hardirq 即将开启。 */
/**
 * __trace_hardirqs_on_caller - 在 hardirq 开启前给全部 held locks 登记 ENABLED usage
 *
 * 先登记 LOCK_ENABLED_HARDIRQ；若任务 softirq 也处于 enabled，再补记 LOCK_ENABLED_SOFTIRQ，
 * 因为此前 hardirq disabled 阶段无法安全记录后者。任一步失败即停止；调用者负责递归门与入口校验。
 */
static void __trace_hardirqs_on_caller(void)
{
	/* IRQ 状态跟踪只针对当前执行任务。 */
	struct task_struct *curr = current;

	/*
	 * We are going to turn hardirqs on, so set the
	 * usage bit for all held locks:
	 */
	/* 开 hardirq 以前，当前持有的每把锁都必须成为 hardirq-enabled usage。 */
	if (!mark_held_locks(curr, LOCK_ENABLED_HARDIRQ))
		return;
	/*
	 * If we have softirqs enabled, then set the usage
	 * bit for all held locks. (disabled hardirqs prevented
	 * this bit from being set before)
	 */
	/*
	 * 若 softirq 逻辑上也已开启，同批锁还需登记 softirq-enabled；先前 hardirq 关闭阻止了该位及时
	 * 写入，所以在这里补齐。
	 */
	if (curr->softirqs_enabled)
		mark_held_locks(curr, LOCK_ENABLED_SOFTIRQ);
}

/**
 * lockdep_hardirqs_on_prepare - Prepare for enabling interrupts
 *
 * Invoked before a possible transition to RCU idle from exit to user or
 * guest mode. This ensures that all RCU operations are done before RCU
 * stops watching. After the RCU transition lockdep_hardirqs_on() has to be
 * invoked to set the final state.
 */
/*
 * 在可能从用户/guest 退出路径进入 RCU idle 前调用，确保所有需要 RCU 的 lockdep 工作在 RCU 停止
 * 观察前完成；RCU 转换以后还必须调用 lockdep_hardirqs_on() 提交最终软件状态。
 */
/**
 * lockdep_hardirqs_on_prepare - 为即将开启 hardirq 预做 usage 与链一致性处理
 *
 * 跳过 lockdep 已关闭、NMI、递归或冗余开启；确认硬件 IRQ 当前关闭、启动期状态已结束且不在
 * hardirq handler。随后保存 chain key，进入递归门并给全部 held locks 标记 ENABLED usage。
 */
void lockdep_hardirqs_on_prepare(void)
{
	/* 全局验证已停止时不得再读取/更新 usage 图。 */
	if (unlikely(!debug_locks))
		return;

	/*
	 * NMIs do not (and cannot) track lock dependencies, nothing to do.
	 */
	/* NMI 不跟踪也无法安全跟踪锁依赖，prepare 阶段无工作可做。 */
	if (unlikely(in_nmi()))
		return;

	/* 正处在 lockdep 内部操作时避免递归进入 usage 更新。 */
	if (unlikely(this_cpu_read(lockdep_recursion)))
		return;

	/* 软件状态已经是 enabled，视作冗余事件并只记统计。 */
	if (unlikely(lockdep_hardirqs_enabled())) {
		/*
		 * Neither irq nor preemption are disabled here
		 * so this is racy by nature but losing one hit
		 * in a stat is not a big deal.
		 */
		/* 此处 IRQ/抢占均未关闭，统计递增天然有竞争；偶失一次仅影响诊断计数。 */
		__debug_atomic_inc(redundant_hardirqs_on);
		return;
	}

	/*
	 * We're enabling irqs and according to our state above irqs weren't
	 * already enabled, yet we find the hardware thinks they are in fact
	 * enabled.. someone messed up their IRQ state tracing.
	 */
	/* 软件认为正从 OFF 开启，但硬件若已经 enabled，说明 IRQ 状态跟踪次序被破坏。 */
	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;

	/*
	 * See the fine text that goes along with this variable definition.
	 */
	/* 启动早期的特殊 IRQ-disabled 窗口不应走普通 on prepare，详见变量定义处约束。 */
	if (DEBUG_LOCKS_WARN_ON(early_boot_irqs_disabled))
		return;

	/*
	 * Can't allow enabling interrupts while in an interrupt handler,
	 * that's general bad form and such. Recursion, limited stack etc..
	 */
	/* 中断 handler 内重新开启 hardirq 会引入递归和有限栈风险，作为状态错误拒绝。 */
	if (DEBUG_LOCKS_WARN_ON(lockdep_hardirq_context()))
		return;

	/* 最终 on 阶段将据此确认 prepare 与提交之间 held-lock chain 未变化。 */
	current->hardirq_chain_key = current->curr_chain_key;

	/* usage 标记自身会触发 lockdep 逻辑，使用 per-CPU 递归门隔离。 */
	lockdep_recursion_inc();
	__trace_hardirqs_on_caller();
	lockdep_recursion_finish();
}
EXPORT_SYMBOL_GPL(lockdep_hardirqs_on_prepare);

/**
 * lockdep_hardirqs_on - 提交 hardirq OFF 到 ON 的软件跟踪状态
 * @ip: 实际开启 IRQ 的调用点地址
 *
 * 普通上下文校验递归、旧状态、硬件 IRQ 状态及 prepare 保存的 chain key；受支持的 NMI 路径跳过
 * 这些不同步检查。成功设置 per-CPU hardirqs_enabled，并记录 enable IP/事件戳和统计。
 */
void noinstr lockdep_hardirqs_on(unsigned long ip)
{
	/* trace 是当前任务内嵌的最近 IRQ 事件记录。 */
	struct irqtrace_events *trace = &current->irqtrace;

	/* noinstr 路径只做最小早退检查，调试关闭时不触碰其余状态。 */
	if (unlikely(!debug_locks))
		return;

	/*
	 * NMIs can happen in the middle of local_irq_{en,dis}able() where the
	 * tracking state and hardware state are out of sync.
	 *
	 * NMIs must save lockdep_hardirqs_enabled() to restore IRQ state from,
	 * and not rely on hardware state like normal interrupts.
	 */
	/*
	 * NMI 可能打断 local_irq_{en,dis}able() 的中间窗口，此时软件与硬件状态暂时不同步。NMI 必须
	 * 保存/恢复 lockdep_hardirqs_enabled()，不能像普通中断那样依赖硬件标志。
	 */
	if (unlikely(in_nmi())) {
		/* 未启用 NMI IRQFLAGS 跟踪时完全忽略此事件。 */
		if (!IS_ENABLED(CONFIG_TRACE_IRQFLAGS_NMI))
			return;

		/*
		 * Skip:
		 *  - recursion check, because NMI can hit lockdep;
		 *  - hardware state check, because above;
		 *  - chain_key check, see lockdep_hardirqs_on_prepare().
		 */
		/*
		 * NMI 可击中 lockdep 本身，故跳过递归检查；硬件状态可能不同步，故跳过硬件检查；它也不经过
		 * 常规 prepare 配对，故跳过 chain_key 检查。
		 */
		goto skip_checks;
	}

	/* 普通上下文不得递归提交 lockdep IRQ 状态。 */
	if (unlikely(this_cpu_read(lockdep_recursion)))
		return;

	if (lockdep_hardirqs_enabled()) {
		/*
		 * Neither irq nor preemption are disabled here
		 * so this is racy by nature but losing one hit
		 * in a stat is not a big deal.
		 */
		/* 与 prepare 相同，此无锁统计允许竞争导致少量计数损失。 */
		__debug_atomic_inc(redundant_hardirqs_on);
		return;
	}

	/*
	 * We're enabling irqs and according to our state above irqs weren't
	 * already enabled, yet we find the hardware thinks they are in fact
	 * enabled.. someone messed up their IRQ state tracing.
	 */
	/* 真正执行开启前硬件应仍处于 disabled，否则调用顺序不可信。 */
	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;

	/*
	 * Ensure the lock stack remained unchanged between
	 * lockdep_hardirqs_on_prepare() and lockdep_hardirqs_on().
	 */
	/* prepare 到最终提交之间不得获取/释放锁；累计 key 必须保持原值。 */
	DEBUG_LOCKS_WARN_ON(current->hardirq_chain_key !=
			    current->curr_chain_key);

skip_checks:
	/* we'll do an OFF -> ON transition: */
	/* 提交 OFF->ON，并把该事件的 IP 与单调任务事件戳一起保存。 */
	__this_cpu_write(hardirqs_enabled, 1);
	trace->hardirq_enable_ip = ip;
	trace->hardirq_enable_event = ++trace->irq_events;
	debug_atomic_inc(hardirqs_on_events);
}
EXPORT_SYMBOL_GPL(lockdep_hardirqs_on);

/*
 * Hardirqs were disabled:
 */
/* hardirq 已被关闭。 */
/**
 * lockdep_hardirqs_off - 记录 hardirq ON 到 OFF 的软件状态转换
 * @ip: 实际关闭 IRQ 的调用点地址
 *
 * 要求调用时硬件 IRQ 已关闭。普通上下文受递归门保护；配置支持时 NMI 仍更新软件状态以便成对
 * 恢复。真实转换记录 disable IP/事件戳，重复 OFF 只增加冗余统计。
 */
void noinstr lockdep_hardirqs_off(unsigned long ip)
{
	/* debug_locks 关闭后保持 noinstr 快速早退。 */
	if (unlikely(!debug_locks))
		return;

	/*
	 * Matching lockdep_hardirqs_on(), allow NMIs in the middle of lockdep;
	 * they will restore the software state. This ensures the software
	 * state is consistent inside NMIs as well.
	 */
	/*
	 * 与 on 路径配对，允许 NMI 打断 lockdep 并更新/恢复其软件 IRQ 状态，使 NMI 内部看到一致值；
	 * 普通上下文则仍拒绝 lockdep_recursion 非零的重入。
	 */
	if (in_nmi()) {
		if (!IS_ENABLED(CONFIG_TRACE_IRQFLAGS_NMI))
			return;
	} else if (__this_cpu_read(lockdep_recursion))
		return;

	/*
	 * So we're supposed to get called after you mask local IRQs, but for
	 * some reason the hardware doesn't quite think you did a proper job.
	 */
	/* off hook 应在屏蔽本地 IRQ 后调用；否则软件转换不能代表硬件事实。 */
	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;

	if (lockdep_hardirqs_enabled()) {
		/* 只有真实 ON->OFF 才需要访问并更新时间线。 */
		struct irqtrace_events *trace = &current->irqtrace;

		/*
		 * We have done an ON -> OFF transition:
		 */
		/* 先提交 per-CPU 状态，再记录位置、事件序号和有效转换计数。 */
		__this_cpu_write(hardirqs_enabled, 0);
		trace->hardirq_disable_ip = ip;
		trace->hardirq_disable_event = ++trace->irq_events;
		debug_atomic_inc(hardirqs_off_events);
	} else {
		/* 软件状态本来就是 OFF，只统计重复通知。 */
		debug_atomic_inc(redundant_hardirqs_off);
	}
}
EXPORT_SYMBOL_GPL(lockdep_hardirqs_off);

/*
 * Softirqs will be enabled:
 */
/* softirq 即将开启。 */
/**
 * lockdep_softirqs_on - 记录 softirq OFF 到 ON 并更新 held-lock usage
 * @ip: 开启 softirq 的调用点地址
 *
 * 要求 lockdep 可进入且硬件 IRQ 关闭。真实转换在递归门内提交任务 softirqs_enabled、事件戳与
 * 统计；若 hardirq 同时 enabled，再把所有 held locks 标为 softirq-enabled。
 */
void lockdep_softirqs_on(unsigned long ip)
{
	/* softirq 跟踪存于当前任务的 irqtrace。 */
	struct irqtrace_events *trace = &current->irqtrace;

	/* lockdep_enabled() 同时覆盖 debug_locks 与 per-CPU 递归门。 */
	if (unlikely(!lockdep_enabled()))
		return;

	/*
	 * We fancy IRQs being disabled here, see softirq.c, avoids
	 * funny state and nesting things.
	 */
	/* softirq.c 按硬件 IRQ 已关闭调用，可避免状态与嵌套关系出现歧义。 */
	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;

	if (current->softirqs_enabled) {
		/* 已是 ON，仅累计冗余通知。 */
		debug_atomic_inc(redundant_softirqs_on);
		return;
	}

	/* 状态提交与 held-lock usage 更新整体禁止递归进入 lockdep。 */
	lockdep_recursion_inc();
	/*
	 * We'll do an OFF -> ON transition:
	 */
	/* 提交任务级 OFF->ON 并记录最近开启位置与事件序号。 */
	current->softirqs_enabled = 1;
	trace->softirq_enable_ip = ip;
	trace->softirq_enable_event = ++trace->irq_events;
	debug_atomic_inc(softirqs_on_events);
	/*
	 * We are going to turn softirqs on, so set the
	 * usage bit for all held locks, if hardirqs are
	 * enabled too:
	 */
	/* 只有 hardirq 也开启时，softirq 才能打断当前临界区，因而需要登记 ENABLED usage。 */
	if (lockdep_hardirqs_enabled())
		mark_held_locks(current, LOCK_ENABLED_SOFTIRQ);
	lockdep_recursion_finish();
}

/*
 * Softirqs were disabled:
 */
/* softirq 已被关闭。 */
/**
 * lockdep_softirqs_off - 记录 softirq ON 到 OFF 的软件状态转换
 * @ip: 关闭 softirq 的调用点地址
 *
 * 要求 lockdep 可进入且硬件 IRQ 已关闭。真实转换清任务状态并记录 disable 事件；若本来已 OFF，
 * 只累计冗余计数。转换后还确认 softirq_count() 反映禁用/嵌套上下文。
 */
void lockdep_softirqs_off(unsigned long ip)
{
	/* 包含递归与全局开关的统一入口门。 */
	if (unlikely(!lockdep_enabled()))
		return;

	/*
	 * We fancy IRQs being disabled here, see softirq.c
	 */
	/* 与 on 路径相同，softirq.c 应在硬件 IRQ 关闭区调用。 */
	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;

	if (current->softirqs_enabled) {
		/* 仅真实转换需要写最近事件快照。 */
		struct irqtrace_events *trace = &current->irqtrace;

		/*
		 * We have done an ON -> OFF transition:
		 */
		/* 清软件状态后记录位置、事件戳和有效 off 次数。 */
		current->softirqs_enabled = 0;
		trace->softirq_disable_ip = ip;
		trace->softirq_disable_event = ++trace->irq_events;
		debug_atomic_inc(softirqs_off_events);
		/*
		 * Whoops, we wanted softirqs off, so why aren't they?
		 */
		/* 调用方声称 softirq 已关闭，计数却为零表示跟踪 hook 次序异常。 */
		DEBUG_LOCKS_WARN_ON(!softirq_count());
	} else
		/* 已处于 OFF 的重复通知不改事件快照。 */
		debug_atomic_inc(redundant_softirqs_off);
}

/**
 * lockdep_cleanup_dead_cpu - Ensure CPU lockdep state is cleanly stopped
 *
 * @cpu: index of offlined CPU
 * @idle: task pointer for offlined CPU's idle thread
 *
 * Invoked after the CPU is dead. Ensures that the tracing infrastructure
 * is left in a suitable state for the CPU to be subsequently brought
 * online again.
 */
/*
 * CPU 已离线后调用，保证其 lockdep 跟踪状态干净停止，以便该 CPU 后续重新 online 时能从一致状态
 * 开始。@cpu 是离线 CPU 索引，@idle 是其 idle 线程，可为 NULL。
 */
/**
 * lockdep_cleanup_dead_cpu - 清理离线 CPU 遗留的 hardirq 软件状态
 * @cpu: 已离线 CPU 的索引
 * @idle: 该 CPU 的 idle 任务，用于打印 IRQ trace；可为 NULL
 *
 * 若 hardirqs_enabled 仍置位则告警、可选打印最后 IRQ 事件，并强制清零 per-CPU 状态。lockdep 已
 * 关闭时不处理。
 */
void lockdep_cleanup_dead_cpu(unsigned int cpu, struct task_struct *idle)
{
	/* 停止验证后不再诊断离线 CPU 的跟踪残留。 */
	if (unlikely(!debug_locks))
		return;

	/* 正常 CPU teardown 应已让软件 hardirq 状态处于 OFF。 */
	if (unlikely(per_cpu(hardirqs_enabled, cpu))) {
		pr_warn("CPU %u left hardirqs enabled!", cpu);
		/* idle 任务存在时给出该 CPU 最近 enable/disable 的调用点。 */
		if (idle)
			print_irqtrace_events(idle);
		/* Clean it up for when the CPU comes online again. */
		/* 为该 CPU 下次上线清理遗留状态。 */
		per_cpu(hardirqs_enabled, cpu) = 0;
	}
}

/**
 * mark_usage - 根据获取环境给新 held lock 登记 USED_IN/ENABLED/USED 状态
 * @curr: 当前任务及 IRQ/softirq 状态
 * @hlock: 已构造、尚待提交的 held lock
 * @check: 非零执行完整 IRQ usage 标记；为零只登记一般 USED
 *
 * 返回：全部 mark_lock() 成功为 1，任一状态冲突或资源失败为 0。trylock 不登记 USED_IN；sync
 * 获取不形成临界区，故不登记 ENABLED；读锁使用对应 READ usage bit。
 */
static int
mark_usage(struct task_struct *curr, struct held_lock *hlock, int check)
{
	/* 关闭完整检查时仍需保留 class 曾被使用的基本事实。 */
	if (!check)
		goto lock_used;

	/*
	 * If non-trylock use in a hardirq or softirq context, then
	 * mark the lock as used in these contexts:
	 */
	/* 非 trylock 的成功获取在当前 hardirq/softirq context 中形成真实临界区，登记 USED_IN。 */
	if (!hlock->trylock) {
		/* 读获取使用 READ 象限，使多个 reader 的兼容性可被状态机区分。 */
		if (hlock->read) {
			if (lockdep_hardirq_context())
				if (!mark_lock(curr, hlock,
						LOCK_USED_IN_HARDIRQ_READ))
					return 0;
			if (curr->softirq_context)
				if (!mark_lock(curr, hlock,
						LOCK_USED_IN_SOFTIRQ_READ))
					return 0;
		} else {
			/* 写获取登记普通 USED_IN bit。 */
			if (lockdep_hardirq_context())
				if (!mark_lock(curr, hlock, LOCK_USED_IN_HARDIRQ))
					return 0;
			if (curr->softirq_context)
				if (!mark_lock(curr, hlock, LOCK_USED_IN_SOFTIRQ))
					return 0;
		}
	}

	/*
	 * For lock_sync(), don't mark the ENABLED usage, since lock_sync()
	 * creates no critical section and no extra dependency can be introduced
	 * by interrupts
	 */
	/*
	 * lock_sync() 不建立临界区，中断无法借它引入额外依赖，因此 sync 获取不登记 ENABLED。否则，
	 * 获取点 hardirq 未关闭时登记 hardirq-enabled；softirq 状态为 on 时再登记 softirq-enabled。
	 */
	if (!hlock->hardirqs_off && !hlock->sync) {
		if (hlock->read) {
			if (!mark_lock(curr, hlock,
					LOCK_ENABLED_HARDIRQ_READ))
				return 0;
			if (curr->softirqs_enabled)
				if (!mark_lock(curr, hlock,
						LOCK_ENABLED_SOFTIRQ_READ))
					return 0;
		} else {
			if (!mark_lock(curr, hlock,
					LOCK_ENABLED_HARDIRQ))
				return 0;
			if (curr->softirqs_enabled)
				if (!mark_lock(curr, hlock,
						LOCK_ENABLED_SOFTIRQ))
					return 0;
		}
	}

lock_used:
	/* mark it as used: */
	/* 无论是否做 IRQ 证明，class 至少登记为一般 USED；mark_lock() 会按 read 自动转换。 */
	if (!mark_lock(curr, hlock, LOCK_USED))
		return 0;

	return 1;
}

/**
 * task_irq_context - 编码当前执行点所属的 lock chain IRQ context
 * @task: 提供 softirq_context 计数的任务，通常为 current
 *
 * 返回：hardirq 与 softirq context 常量按各自布尔状态加权后的编码，可区分普通、softirq、hardirq
 * 及可能的组合状态；只读当前 CPU/task 状态。
 */
static inline unsigned int task_irq_context(struct task_struct *task)
{
	/* 双重取反把嵌套计数归一为 0/1，再乘对应 chain context 标识。 */
	return LOCK_CHAIN_HARDIRQ_CONTEXT * !!lockdep_hardirq_context() +
	       LOCK_CHAIN_SOFTIRQ_CONTEXT * !!task->softirq_context;
}

/**
 * separate_irq_context - 判断新锁是否跨入另一条 IRQ-context chain
 * @curr: 当前任务及已提交 held-lock 栈
 * @hlock: 本次新锁，irq_context 已初始化
 *
 * 返回：栈非空且前一项 context 与新锁不同为 1，否则为 0。返回 1 会使上层重置 chain hash，并阻止
 * 新锁与跨 context 的 prev 建立普通依赖。
 */
static int separate_irq_context(struct task_struct *curr,
		struct held_lock *hlock)
{
	/* depth 也是新锁即将写入的下标。 */
	unsigned int depth = curr->lockdep_depth;

	/*
	 * Keep track of points where we cross into an interrupt context:
	 */
	/* 空栈天然是 chain head；非空时才有可比较的前一项。 */
	if (depth) {
		/* prev_hlock 指向当前栈顶。 */
		struct held_lock *prev_hlock;

		prev_hlock = curr->held_locks + depth-1;
		/*
		 * If we cross into another context, reset the
		 * hash key (this also prevents the checking and the
		 * adding of the dependency to 'prev'):
		 */
		/* context 改变时开启新 chain；上层据此重置 key，也不会验证/添加到 prev 的边。 */
		if (prev_hlock->irq_context != hlock->irq_context)
			return 1;
	}
	return 0;
}

/*
 * Mark a lock with a usage bit, and validate the state transition:
 */
/* 给锁设置 usage bit，并验证这次状态转换。 */
/**
 * mark_lock - 原子登记一个 class usage bit 并执行所需 IRQ 冲突证明
 * @curr: 当前任务，供图验证与报告使用
 * @this: 要更新其 class 的 held lock
 * @new_bit: 拟登记的 usage 状态
 *
 * 返回：0 表示非法 bit、图锁失败、trace 耗尽或冲突；1 表示已存在/普通成功；2 表示成功且已输出
 * verbose 详情。函数用无锁快查加 graph_lock 下复查避免重复写 cacheline；首次状态保存 trace。
 */
static int mark_lock(struct task_struct *curr, struct held_lock *this,
			     enum lock_usage_bit new_bit)
{
	/* new_mask 是目标位；ret 由 IRQ 验证返回 1 或 verbose 成功 2。 */
	unsigned int new_mask, ret = 1;

	/* 防止调用者提供越过生成枚举的 bit，告警并拒绝移位。 */
	if (new_bit >= LOCK_USAGE_STATES) {
		DEBUG_LOCKS_WARN_ON(1);
		return 0;
	}

	/* 一般 USED 对读获取转换为 LOCK_USED_READ，保留读写语义。 */
	if (new_bit == LOCK_USED && this->read)
		new_bit = LOCK_USED_READ;

	new_mask = 1 << new_bit;

	/*
	 * If already set then do not dirty the cacheline,
	 * nor do any checks:
	 */
	/* 常见已置位路径既不争图锁，也不重复写共享 usage_mask cacheline。 */
	if (likely(hlock_class(this)->usage_mask & new_mask))
		return 1;

	if (!graph_lock())
		return 0;
	/*
	 * Make sure we didn't race:
	 */
	/* 首查到拿锁之间可能由另一 CPU 置位；锁内复查后直接走统一解锁。 */
	if (unlikely(hlock_class(this)->usage_mask & new_mask))
		goto unlock;

	/* class 从完全未使用转为首个 usage 时，减少 unused class 统计。 */
	if (!hlock_class(this)->usage_mask)
		debug_atomic_dec(nr_unused_locks);

	/* 先发布 usage 位，使后续冲突搜索看到正在登记的状态。 */
	hlock_class(this)->usage_mask |= new_mask;

	/* 仅可追踪的状态保存首次现场；save_trace() 失败会关闭并释放图锁。 */
	if (new_bit < LOCK_TRACE_STATES) {
		if (!(hlock_class(this)->usage_traces[new_bit] = save_trace()))
			return 0;
	}

	/* IRQ 专属状态还需验证 class 自身和整个依赖子图；USED/USED_READ 无此步骤。 */
	if (new_bit < LOCK_USED) {
		ret = mark_lock_irq(curr, this, new_bit);
		if (!ret)
			return 0;
	}

unlock:
	/* 普通成功或锁内竞态命中都在这里归还 graph_lock。 */
	graph_unlock();

	/*
	 * We must printk outside of the graph_lock:
	 */
	/* verbose 输出可能很慢且递归触发控制台锁，必须放在 graph_lock 临界区之外。 */
	if (ret == 2) {
		/* 用紧急控制台包裹状态、IRQ 历史和调用栈。 */
		nbcon_cpu_emergency_enter();
		printk("\nmarked lock as {%s}:\n", usage_str[new_bit]);
		print_lock(this);
		print_irqtrace_events(curr);
		dump_stack();
		nbcon_cpu_emergency_exit();
	}

	/* 保留 2 让调用层知道 verbose 分支已执行，真假语义仍表示成功。 */
	return ret;
}

/**
 * task_wait_context - 计算当前执行环境允许的最宽松内部 wait type
 * @curr: 当前任务及 IRQ threading/config 状态
 *
 * 返回：非线程化 hardirq 为 LD_WAIT_SPIN；会线程化的 hardirq 或 softirq 为 LD_WAIT_CONFIG；普通
 * 任务上下文为 LD_WAIT_MAX。数值越小表示环境越严格。
 */
static inline short task_wait_context(struct task_struct *curr)
{
	/*
	 * Set appropriate wait type for the context; for IRQs we have to take
	 * into account force_irqthread as that is implied by PREEMPT_RT.
	 */
	/* 按执行 context 设定 wait 上限；IRQ 还需考虑 PREEMPT_RT 隐含的 force_irqthread。 */
	if (lockdep_hardirq_context()) {
		/*
		 * Check if force_irqthreads will run us threaded.
		 */
		/* threaded hardirq 或配置型 IRQ 可使用 CONFIG 级等待约束。 */
		if (curr->hardirq_threaded || curr->irq_config)
			return LD_WAIT_CONFIG;

		/* 真正硬中断不能睡眠，只允许 spin 级原语。 */
		return LD_WAIT_SPIN;
	} else if (curr->softirq_context) {
		/*
		 * Softirqs are always threaded.
		 */
		/* 此 wait-type 模型把 softirq 视作线程化环境。 */
		return LD_WAIT_CONFIG;
	}

	/* 普通任务上下文从最宽松上界开始，再由已持锁收紧。 */
	return LD_WAIT_MAX;
}

/**
 * print_lock_invalid_wait_context - 报告在过严环境中获取较宽松 wait-type 锁
 * @curr: 触发获取的当前任务
 * @hlock: wait_type_outer 超出当前约束的新锁
 *
 * 返回：固定 0。首次调用关闭 lockdep；非 silent 时输出新锁、当前执行 context、全部 held locks 与
 * 栈回溯。函数不持 graph_lock，由 wait-context 入口在建依赖前调用。
 */
static int
print_lock_invalid_wait_context(struct task_struct *curr,
				struct held_lock *hlock)
{
	/* curr_inner 用于把任务当前环境显示为 context-{inner:inner}。 */
	short curr_inner;

	/* 仅首次关闭者输出，避免多个 CPU 重复报告同一失效状态。 */
	if (!debug_locks_off())
		return 0;
	/* silent 模式仍保留关闭副作用，但压制控制台正文。 */
	if (debug_locks_silent)
		return 0;

	/* wait-context 错误也可能发生在锁敏感路径，进入紧急控制台区。 */
	nbcon_cpu_emergency_enter();

	pr_warn("\n");
	pr_warn("=============================\n");
	pr_warn("[ BUG: Invalid wait context ]\n");
	print_kernel_ident();
	pr_warn("-----------------------------\n");

	pr_warn("%s/%d is trying to lock:\n", curr->comm, task_pid_nr(curr));
	print_lock(hlock);

	pr_warn("other info that might help us debug this:\n");

	/* 重新计算当前执行环境的基础约束；更严格的 held lock 会在持锁列表中体现。 */
	curr_inner = task_wait_context(curr);
	pr_warn("context-{%d:%d}\n", curr_inner, curr_inner);

	/* 当前 held stack 解释是哪把锁把环境收紧。 */
	lockdep_print_held_locks(curr);

	pr_warn("stack backtrace:\n");
	dump_stack();

	nbcon_cpu_emergency_exit();

	/* 调用协议以关闭 debug_locks 表示失败，整数结果保持历史上的 0。 */
	return 0;
}

/*
 * Verify the wait_type context.
 *
 * This check validates we take locks in the right wait-type order; that is it
 * ensures that we do not take mutexes inside spinlocks and do not attempt to
 * acquire spinlocks inside raw_spinlocks and the sort.
 *
 * The entire thing is slightly more complex because of RCU, RCU is a lock that
 * can be taken from (pretty much) any context but also has constraints.
 * However when taken in a stricter environment the RCU lock does not loosen
 * the constraints.
 *
 * Therefore we must look for the strictest environment in the lock stack and
 * compare that to the lock we're trying to acquire.
 */
/*
 * 验证 wait_type 获取顺序：禁止在 spinlock 内取 mutex，也禁止在 raw_spinlock 内取普通 spinlock 等。
 * RCU 几乎可从任何 context 获取但自身也带约束；在更严格环境中获取 RCU 不会放宽已有约束。因此
 * 必须找出当前 IRQ-context 持锁栈里最严格的环境，再与新锁的 outer wait type 比较。
 */
/**
 * check_wait_context - 验证新锁 outer wait type 不宽于当前最严格环境
 * @curr: 当前任务及 held-lock 栈
 * @next: 要获取的新 held lock
 *
 * 返回：当前实现始终为 0；无 wait type 或 trylock 直接跳过，非法组合通过
 * print_lock_invalid_wait_context() 关闭并报告 lockdep。副作用而非返回码承载违规结果。
 */
static int check_wait_context(struct task_struct *curr, struct held_lock *next)
{
	/* next_inner/outer 来自 class；curr_inner 累计当前最严格值；depth 遍历同 context 后缀。 */
	u8 next_inner = hlock_class(next)->wait_type_inner;
	u8 next_outer = hlock_class(next)->wait_type_outer;
	u8 curr_inner;
	int depth;

	/* 未参与 wait-type 模型的锁以及可能失败的 trylock 不收紧/验证顺序。 */
	if (!next_inner || next->trylock)
		return 0;

	/* 未显式给 outer 时用 inner，表示锁的内外约束相同。 */
	if (!next_outer)
		next_outer = next_inner;

	/*
	 * Find start of current irq_context..
	 */
	/* 反向找到当前 IRQ-context chain 的起点，不让其他 context 的锁约束混入。 */
	for (depth = curr->lockdep_depth - 1; depth >= 0; depth--) {
		/* prev 只在本次循环迭代内使用。 */
		struct held_lock *prev = curr->held_locks + depth;
		if (prev->irq_context != next->irq_context)
			break;
	}
	depth++;

	/* 先以 hardirq/softirq/任务本身的等待能力作为环境上界。 */
	curr_inner = task_wait_context(curr);

	/* 再让当前 chain 中每把已持锁的 inner wait type 逐步收紧它。 */
	for (; depth < curr->lockdep_depth; depth++) {
		struct held_lock *prev = curr->held_locks + depth;
		struct lock_class *class = hlock_class(prev);
		u8 prev_inner = class->wait_type_inner;

		/* wait_type_inner==0 的 class 不参与此模型。 */
		if (prev_inner) {
			/*
			 * We can have a bigger inner than a previous one
			 * when outer is smaller than inner, as with RCU.
			 *
			 * Also due to trylocks.
			 */
			/*
			 * RCU 等锁可有 outer < inner，因此它被获取时允许较宽的 inner，但持有后仍以更严格值约束
			 * 后继；trylock 的特殊性也允许前后 inner 表面上不单调。
			 */
			curr_inner = min(curr_inner, prev_inner);

			/*
			 * Allow override for annotations -- this is typically
			 * only valid/needed for code that only exists when
			 * CONFIG_PREEMPT_RT=n.
			 */
			/* 显式 annotation 可覆盖累计约束，通常只用于 !PREEMPT_RT 专属代码。 */
			if (unlikely(class->lock_type == LD_LOCK_WAIT_OVERRIDE))
				curr_inner = prev_inner;
		}
	}

	/* 新锁 outer 数值更大、比当前允许范围更宽松时报告，例如 spin 内取 mutex。 */
	if (next_outer > curr_inner)
		return print_lock_invalid_wait_context(curr, next);

	/* 合法与已报告路径都按历史接口返回 0，调用者另由 debug_locks 观察终止。 */
	return 0;
}

#else /* CONFIG_PROVE_LOCKING */

/**
 * mark_usage - 未编译 PROVE_LOCKING 时的 usage 标记 stub
 * @curr: 未使用
 * @hlock: 未使用
 * @check: 未使用
 *
 * 返回：固定 1，允许外围 acquisition 继续。
 */
static inline int
mark_usage(struct task_struct *curr, struct held_lock *hlock, int check)
{
	return 1;
}

/**
 * task_irq_context - 未编译 PROVE_LOCKING 时的 IRQ-context 编码 stub
 * @task: 未使用
 *
 * 返回：固定 0，把全部获取视为普通 chain context。
 */
static inline unsigned int task_irq_context(struct task_struct *task)
{
	return 0;
}

/**
 * separate_irq_context - 未编译 PROVE_LOCKING 时的 chain 分隔 stub
 * @curr: 未使用
 * @hlock: 未使用
 *
 * 返回：固定 0，不因 IRQ context 开启新依赖 chain。
 */
static inline int separate_irq_context(struct task_struct *curr,
		struct held_lock *hlock)
{
	return 0;
}

/**
 * check_wait_context - 未编译 PROVE_LOCKING 时的 wait-type 校验 stub
 * @curr: 未使用
 * @next: 未使用
 *
 * 返回：固定 0，不执行 wait context 状态验证。
 */
static inline int check_wait_context(struct task_struct *curr,
				     struct held_lock *next)
{
	return 0;
}

#endif /* CONFIG_PROVE_LOCKING */

/*
 * Initialize a lock instance's lock-class mapping info:
 */
/* 初始化一个锁实例到 lock class 的映射信息。 */
/**
 * lockdep_init_map_type - 初始化 lockdep_map 的身份、wait type 与可选 subclass
 * @lock: 待初始化的 map
 * @name: 非 NULL 的持久名称字符串
 * @key: 静态对象内 key 或已注册 dynamic key
 * @subclass: 非零时立即注册的 subclass
 * @inner: 持有该锁后施加给内部获取的 wait-type 上限
 * @outer: 获取该锁所需的外部 wait-type 能力，0 表示沿用 inner
 * @lock_type: 普通或 wait override 等锁类型
 *
 * 清空 class cache，记录 lockstat CPU，校验名称/key 生命周期并写基本字段。非零 subclass 在关闭
 * 本地 IRQ 和递归门内预注册；subclass 0 留给首次 acquisition 的惰性注册。
 */
void lockdep_init_map_type(struct lockdep_map *lock, const char *name,
			    struct lock_class_key *key, int subclass,
			    u8 inner, u8 outer, u8 lock_type)
{
	/* i 清理有限个热 subclass cache 槽。 */
	int i;

	/* map 可能复用，初始化时不得保留旧 class 指针。 */
	for (i = 0; i < NR_LOCKDEP_CACHING_CLASSES; i++)
		lock->class_cache[i] = NULL;

#ifdef CONFIG_LOCK_STAT
	/* lockstat 用初始化 CPU 关联每 CPU 统计信息。 */
	lock->cpu = raw_smp_processor_id();
#endif

	/*
	 * Can't be having no nameless bastards around this place!
	 */
	/* 名称是所有诊断的基本身份；NULL 时用字面占位并停止继续初始化。 */
	if (DEBUG_LOCKS_WARN_ON(!name)) {
		lock->name = "NULL";
		return;
	}

	/* 名称通过校验后再保存调用者提供的持久指针。 */
	lock->name = name;

	/* wait-type 三元组随后会复制进注册出的每个 lock_class。 */
	lock->wait_type_outer = outer;
	lock->wait_type_inner = inner;
	lock->lock_type = lock_type;

	/*
	 * No key, no joy, we need to hash something.
	 */
	/* class hash 没有 key 无法建立稳定身份，告警并保留未注册状态。 */
	if (DEBUG_LOCKS_WARN_ON(!key))
		return;
	/*
	 * Sanity check, the lock-class key must either have been allocated
	 * statically or must have been registered as a dynamic key.
	 */
	/* key 必须位于静态对象，或已显式加入 dynamic key 注册表，确保生命周期可验证。 */
	if (!static_obj(key) && !is_dynamic_key(key)) {
		if (debug_locks)
			printk(KERN_ERR "BUG: key %px has not been registered!\n", key);
		DEBUG_LOCKS_WARN_ON(1);
		return;
	}
	/* 只有通过来源校验的 key 才发布到 map。 */
	lock->key = key;

	/* 即便 debug 已关闭，以上 map 基本字段仍完成；class 注册则不再尝试。 */
	if (unlikely(!debug_locks))
		return;

	if (subclass) {
		/* 保存本地 IRQ 状态，覆盖注册临界区。 */
		unsigned long flags;

		/* 此时尚未进入递归门，先确认 lockdep 入口可用。 */
		if (DEBUG_LOCKS_WARN_ON(!lockdep_enabled()))
			return;

		/* register_lock_class() 依赖 IRQ 关闭，并用递归门防止内部事件重入。 */
		raw_local_irq_save(flags);
		lockdep_recursion_inc();
		register_lock_class(lock, subclass, 1);
		lockdep_recursion_finish();
		raw_local_irq_restore(flags);
	}
}
EXPORT_SYMBOL_GPL(lockdep_init_map_type);

/* 特殊 key：仍跟踪 held-lock 栈和基本 usage，但 acquisition 会关闭依赖证明。 */
struct lock_class_key __lockdep_no_validate__;
EXPORT_SYMBOL_GPL(__lockdep_no_validate__);

/* 特殊 key：acquisition 在最早入口直接返回，完全不跟踪该 map。 */
struct lock_class_key __lockdep_no_track__;
EXPORT_SYMBOL_GPL(__lockdep_no_track__);

#ifdef CONFIG_PROVE_LOCKING
/**
 * lockdep_set_lock_cmp_fn - 为 map 的基础 class 安装实例排序与诊断打印回调
 * @lock: 目标 lockdep_map
 * @cmp_fn: 判断同 class 两实例合法顺序的比较函数
 * @print_fn: 打印实例附加信息的函数
 *
 * 在本地 IRQ 关闭和递归门内取得/注册 subclass 0。已有不同回调时告警，但仍以新值覆盖；注册失败
 * 则不写。该能力让同 class 的多实例按外部稳定次序嵌套而不产生伪死锁。
 */
void lockdep_set_lock_cmp_fn(struct lockdep_map *lock, lock_cmp_fn cmp_fn,
			     lock_print_fn print_fn)
{
	/* 优先读基础 class cache；flags 保存调用前 IRQ 状态。 */
	struct lock_class *class = lock->class_cache[0];
	unsigned long flags;

	/* class 查找/注册与字段更新均隔离本地 IRQ 和 lockdep 递归。 */
	raw_local_irq_save(flags);
	lockdep_recursion_inc();

	/* 尚未缓存时允许创建 subclass 0 class。 */
	if (!class)
		class = register_lock_class(lock, 0, 0);

	if (class) {
		/* 同一 class 不应被不同组件反复赋予互相冲突的比较/打印语义。 */
		WARN_ON(class->cmp_fn	&& class->cmp_fn != cmp_fn);
		WARN_ON(class->print_fn && class->print_fn != print_fn);

		/* WARN 后仍采用最新注册者提供的函数，保持调用 API 的既有行为。 */
		class->cmp_fn	= cmp_fn;
		class->print_fn = print_fn;
	}

	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lockdep_set_lock_cmp_fn);
#endif

/**
 * print_lock_nested_lock_not_held - 报告 acquisition 声明的 nest_lock 并未被当前任务持有
 * @curr: 当前任务及 held-lock 栈
 * @hlock: 带 nest_lock 指针的新 held lock
 *
 * 首次报告关闭 lockdep；非 silent 时打印新锁、期望持有的 nest_lock 名称、当前 held locks 和栈
 * 回溯。无图锁输入，返回 void。
 */
static void
print_lock_nested_lock_not_held(struct task_struct *curr,
				struct held_lock *hlock)
{
	/* 仅首次关闭者负责正文。 */
	if (!debug_locks_off())
		return;
	/* silent 仍关闭验证但不进行控制台输出。 */
	if (debug_locks_silent)
		return;

	/* nested 获取错误可能发生在锁密集路径，用紧急控制台保证报告推进。 */
	nbcon_cpu_emergency_enter();

	pr_warn("\n");
	pr_warn("==================================\n");
	pr_warn("WARNING: Nested lock was not taken\n");
	print_kernel_ident();
	pr_warn("----------------------------------\n");

	pr_warn("%s/%d is trying to lock:\n", curr->comm, task_pid_nr(curr));
	print_lock(hlock);

	/* 先显示正在获取的锁，再显示调用者声称用于串行嵌套的锁。 */
	pr_warn("\nbut this task is not holding:\n");
	pr_warn("%s\n", hlock->nest_lock->name);

	pr_warn("\nstack backtrace:\n");
	dump_stack();

	pr_warn("\nother info that might help us debug this:\n");
	/* 保留当前任务实际持锁列表，用于确认 nest_lock 确实缺席。 */
	lockdep_print_held_locks(curr);

	pr_warn("\nstack backtrace:\n");
	dump_stack();

	/* 完整报告结束后恢复控制台上下文。 */
	nbcon_cpu_emergency_exit();
}

/* 查询当前任务是否持有指定 map 的内部 helper，定义位于 release/held 查询区域。 */
static int __lock_is_held(const struct lockdep_map *lock, int read);

/*
 * This gets called for every mutex_lock*()/spin_lock*() operation.
 * We maintain the dependency maps and validate the locking attempt:
 *
 * The callers must make sure that IRQs are disabled before calling it,
 * otherwise we could get an interrupt which would want to take locks,
 * which would end up in lockdep again.
 */
/*
 * 每次 mutex_lock*()/spin_lock*() 都会进入这里，以维护依赖 map 并验证获取。调用者必须先关闭 IRQ，
 * 否则中断可能在本函数中途再次取锁并递归进入 lockdep。
 */
/**
 * __lock_acquire - 构造、验证并提交一次锁获取到当前任务持锁栈
 * @lock: 被获取实例的 lockdep_map
 * @subclass: 本次 acquisition 使用的 subclass
 * @trylock: 非零表示尝试获取，不建立普通顺序依赖
 * @read: 0 为写，1/2 为读语义
 * @check: 非零启用 usage/依赖验证
 * @hardirqs_off: 获取点 hardirq 是否关闭
 * @nest_lock: 声明串行嵌套的外层 map；可为 NULL
 * @ip: 获取调用点
 * @references: nested 引用计数增量
 * @pin_count: 初始 pin 计数
 * @sync: 非零表示 lock_sync()，只同步依赖而不形成实际临界区
 *
 * 返回：1 为正常验证/提交或 sync 完成；2 为合并到栈顶同 class nested 引用；0 为跳过、错误或验证
 * 失败。阶段依次为 class 解析、临时 hlock 构造、wait/usage、chain key、nest/依赖验证和最终入栈。
 */
static int __lock_acquire(struct lockdep_map *lock, unsigned int subclass,
			  int trylock, int read, int check, int hardirqs_off,
			  struct lockdep_map *nest_lock, unsigned long ip,
			  int references, int pin_count, int sync)
{
	/* curr 是唯一被修改的任务；class/hlock 分别保存类与待提交栈项。 */
	struct task_struct *curr = current;
	struct lock_class *class = NULL;
	struct held_lock *hlock;
	/* depth 是新项下标；chain_head 标识本 IRQ context 的依赖链起点。 */
	unsigned int depth;
	int chain_head = 0;
	/* class_idx 是静态 class 池下标，chain_key 是验证通过前的候选累计值。 */
	int class_idx;
	u64 chain_key;

	/* 全局调试停止后不再更新任何 acquisition 状态。 */
	if (unlikely(!debug_locks))
		return 0;

	/* no_track 比 no_validate 更强：连 held-lock 栈和基础 usage 都完全跳过。 */
	if (unlikely(lock->key == &__lockdep_no_track__))
		return 0;

	/* 所有实际进入跟踪的获取先累计事件。 */
	lockevent_inc(lockdep_acquire);

	/* 全局 prove 关闭或特殊 key 只关闭图验证，仍保留 class/held stack 记账。 */
	if (!prove_locking || lock->key == &__lockdep_no_validate__) {
		check = 0;
		lockevent_inc(lockdep_nocheck);
	}

	/* subclass 会参与 key 偏移，必须在编译上限内。 */
	if (DEBUG_LOCKS_WARN_ON(subclass >= MAX_LOCKDEP_SUBCLASSES))
		return 0;

	/* 热门低 subclass 优先从 map 内 cache 直接取得 class。 */
	if (subclass < NR_LOCKDEP_CACHING_CLASSES)
		class = lock->class_cache[subclass];
	/*
	 * Not cached?
	 */
	/* cache 未命中或 subclass 超出 cache 范围时查找/注册 class。 */
	if (unlikely(!class)) {
		class = register_lock_class(lock, subclass, 0);
		if (!class)
			return 0;
	}

	/* 记录该 class 被 acquisition 路径操作的次数。 */
	debug_class_ops_inc(class);

	/* 类级 verbose 过滤命中时输出 class 身份、同名版本和调用栈。 */
	if (very_verbose(class)) {
		nbcon_cpu_emergency_enter();
		printk("\nacquire class [%px] %s", class->key, class->name);
		if (class->name_version > 1)
			printk(KERN_CONT "#%d", class->name_version);
		printk(KERN_CONT "\n");
		dump_stack();
		nbcon_cpu_emergency_exit();
	}

	/*
	 * Add the lock to the list of currently held locks.
	 * (we dont increase the depth just yet, up until the
	 * dependency checks are done)
	 */
	/*
	 * 先在 held_locks[depth] 构造候选，但依赖检查完成以前不递增 lockdep_depth，使失败不会暴露半提交
	 * 栈项，同时 check_prevs_add() 仍可读取这个临时候选。
	 */
	depth = curr->lockdep_depth;
	/*
	 * Ran out of static storage for our per-task lock stack again have we?
	 */
	/* 每任务持锁栈是固定数组；没有候选槽时告警并拒绝继续。 */
	if (DEBUG_LOCKS_WARN_ON(depth >= MAX_LOCK_DEPTH))
		return 0;

	/* 指针差得到可压缩进 hlock_id 的 class 池下标。 */
	class_idx = class - lock_classes;

	/* sync 不形成持有关系，不能与已有栈顶 nested 引用合并。 */
	if (depth && !sync) {
		/* we're holding locks and the new held lock is not a sync */
		/* 已持锁且新操作不是 sync。 */
		hlock = curr->held_locks + depth - 1;
		/* 带 nest_lock 的同 class 栈顶获取用引用计数压缩为一个 held_lock。 */
		if (hlock->class_idx == class_idx && nest_lock) {
			/* 调用者未给显式增量时，至少表示新增一个引用。 */
			if (!references)
				references++;

			/* 旧项从普通持有转换到 ref-count 模式时，先为原持有补一个引用。 */
			if (!hlock->references)
				hlock->references++;

			/* 合并本次引用，release 路径将对称递减而不立刻出栈。 */
			hlock->references += references;

			/* Overflow */
			/* 加法回绕后结果会小于增量，关闭调试以避免失配释放。 */
			if (DEBUG_LOCKS_WARN_ON(hlock->references < references))
				return 0;

			/* 2 告诉 release 重放逻辑这次 acquisition 被合并。 */
			return 2;
		}
	}

	/* 使用尚未提交的尾后槽构造完整 held_lock。 */
	hlock = curr->held_locks + depth;
	/*
	 * Plain impossible, we just registered it and checked it weren't no
	 * NULL like.. I bet this mushroom I ate was good!
	 */
	/* class 刚经过 cache/注册成功检查，NULL 只可能意味着内部状态遭破坏。 */
	if (DEBUG_LOCKS_WARN_ON(!class))
		return 0;
	hlock->class_idx = class_idx;
	hlock->acquire_ip = ip;
	hlock->instance = lock;
	hlock->nest_lock = nest_lock;
	hlock->irq_context = task_irq_context(curr);
	hlock->trylock = trylock;
	hlock->read = read;
	hlock->check = check;
	hlock->sync = !!sync;
	hlock->hardirqs_off = !!hardirqs_off;
	hlock->references = references;
#ifdef CONFIG_LOCK_STAT
	/* 新持有尚无等待时间戳；hold 起点取当前 lockstat clock。 */
	hlock->waittime_stamp = 0;
	hlock->holdtime_stamp = lockstat_clock();
#endif
	hlock->pin_count = pin_count;

	/* wait-type 违规通过关闭 debug_locks 报告；保留接口返回检查。 */
	if (check_wait_context(curr, hlock))
		return 0;

	/* Initialize the lock usage bit */
	/* 按 acquisition context 登记 USED_IN/ENABLED/USED usage。 */
	if (!mark_usage(curr, hlock, check))
		return 0;

	/*
	 * Calculate the chain hash: it's the combined hash of all the
	 * lock keys along the dependency chain. We save the hash value
	 * at every step so that we can get the current hash easily
	 * after unlock. The chain hash is then used to cache dependency
	 * results.
	 *
	 * The 'key ID' is what is the most compact key value to drive
	 * the hash, not class->key.
	 */
	/*
	 * chain hash 是依赖链上紧凑 lock ID 的组合 hash。每个栈项保存加入自己前的 key，使 unlock 后能
	 * O(1) 恢复。最终 key 用于缓存依赖验证；驱动 hash 的是紧凑 ID，不是 class->key 指针。
	 */
	/*
	 * Whoops, we did it again.. class_idx is invalid.
	 */
	/* class_idx 虽来自指针差，仍确认对应静态槽已登记为 in-use。 */
	if (DEBUG_LOCKS_WARN_ON(!test_bit(class_idx, lock_classes_in_use)))
		return 0;

	/* 从当前 context chain 已提交的累计 key 开始。 */
	chain_key = curr->curr_chain_key;
	if (!depth) {
		/*
		 * How can we have a chain hash when we ain't got no keys?!
		 */
		/* 空 held stack 不可能携带非初始 key，否则上次 release 恢复失败。 */
		if (DEBUG_LOCKS_WARN_ON(chain_key != INITIAL_CHAIN_KEY))
			return 0;
		/* 第一把锁天然是 chain head，不需要前驱依赖。 */
		chain_head = 1;
	}

	/* 无论是否跨 context，先保存 unlock 时应恢复的全局前值。 */
	hlock->prev_chain_key = chain_key;
	/* IRQ context 切换开启独立 chain，hash 从初值重算且不连接上一栈项。 */
	if (separate_irq_context(curr, hlock)) {
		chain_key = INITIAL_CHAIN_KEY;
		chain_head = 1;
	}
	/* 把新锁紧凑 ID 纳入候选 chain key。 */
	chain_key = iterate_chain_key(chain_key, hlock_id(hlock));

	/* nest_lock 声明只有在当前任务确实持有该 map 时才有效。 */
	if (nest_lock && !__lock_is_held(nest_lock, -1)) {
		print_lock_nested_lock_not_held(curr, hlock);
		return 0;
	}

	/* 非 silent 调试下再次捕获动态 key 过早释放造成的失效 class。 */
	if (!debug_locks_silent) {
		WARN_ON_ONCE(depth && !hlock_class(hlock - 1)->key);
		WARN_ON_ONCE(!hlock_class(hlock)->key);
	}

	/* 对新 chain 做 cache、递归、IRQ 反转和前驱环路证明。 */
	if (!validate_chain(curr, hlock, chain_head, chain_key))
		return 0;

	/* For lock_sync(), we are done here since no actual critical section */
	/* lock_sync() 只让依赖验证观察同步点，不提交 held stack 或 curr_chain_key。 */
	if (hlock->sync)
		return 1;

	/* 所有验证通过后提交累计 key 和栈深度。 */
	curr->curr_chain_key = chain_key;
	curr->lockdep_depth++;
	/* 调试构建立刻从栈重放 key，捕捉提交过程中的状态破坏。 */
	check_chain_key(curr);
#ifdef CONFIG_DEBUG_LOCKDEP
	/* check_chain_key() 可能关闭 debug_locks，必须把该结果传播为失败。 */
	if (unlikely(!debug_locks))
		return 0;
#endif
	/*
	 * 提交后仍保留一个尾后临时槽给下一 acquisition；达到数组上限即关闭 lockdep 并输出全局现场。
	 */
	if (unlikely(curr->lockdep_depth >= MAX_LOCK_DEPTH)) {
		debug_locks_off();
		nbcon_cpu_emergency_enter();
		print_lockdep_off("BUG: MAX_LOCK_DEPTH too low!");
		printk(KERN_DEBUG "depth: %i  max: %lu!\n",
		       curr->lockdep_depth, MAX_LOCK_DEPTH);

		lockdep_print_held_locks(current);
		debug_show_all_locks();
		dump_stack();
		nbcon_cpu_emergency_exit();

		return 0;
	}

	/* 只在出现历史新高时更新最大观测持锁深度。 */
	if (unlikely(curr->lockdep_depth > max_lockdep_depth))
		max_lockdep_depth = curr->lockdep_depth;

	/* 新 held_lock 已完整提交。 */
	return 1;
}

/**
 * print_unlock_imbalance_bug - 报告释放一个当前任务并未持有的 lock map
 * @curr: 执行 release 的任务
 * @lock: 找不到匹配 held_lock 的 map
 * @ip: release 调用点
 *
 * 首次报告关闭 lockdep；非 silent 时输出 map cache 身份、release 符号、当前 held locks 与栈回溯。
 */
static void print_unlock_imbalance_bug(struct task_struct *curr,
				       struct lockdep_map *lock,
				       unsigned long ip)
{
	/* 关闭竞态中仅首个 CPU 输出正文。 */
	if (!debug_locks_off())
		return;
	/* silent 模式保留关闭结果但不打印。 */
	if (debug_locks_silent)
		return;

	/* 用紧急控制台避免坏 unlock 现场因其他锁依赖而卡住。 */
	nbcon_cpu_emergency_enter();

	pr_warn("\n");
	pr_warn("=====================================\n");
	pr_warn("WARNING: bad unlock balance detected!\n");
	print_kernel_ident();
	pr_warn("-------------------------------------\n");
	pr_warn("%s/%d is trying to release lock (",
		curr->comm, task_pid_nr(curr));
	print_lockdep_cache(lock);
	pr_cont(") at:\n");
	print_ip_sym(KERN_WARNING, ip);
	/* map cache 与 IP 一起定位释放对象和调用代码。 */
	pr_warn("but there are no more locks to release!\n");
	pr_warn("\nother info that might help us debug this:\n");
	/* 展示尚存持锁，判断是重复释放、错对象还是跨 context。 */
	lockdep_print_held_locks(curr);

	/* 当前调用栈补足 release 的动态路径。 */
	pr_warn("\nstack backtrace:\n");
	dump_stack();

	/* 报告结束，退出 nbcon 紧急区。 */
	nbcon_cpu_emergency_exit();
}

/**
 * match_held_lock - 判断一个 held_lock 是否代表给定 map 的持有
 * @hlock: 候选任务栈项
 * @lock: 要查询/释放的 lockdep_map
 *
 * 返回：实例指针完全相同为 1；ref-count nested 项还允许基础 class 相同为 1；否则 0。后者支持
 * 多个同 class 实例合并到一个栈项，但会验证 class 已注册且 references 必须伴随 nest_lock。
 */
static noinstr int match_held_lock(const struct held_lock *hlock,
				   const struct lockdep_map *lock)
{
	/* 普通持有以 map 实例身份精确匹配。 */
	if (hlock->instance == lock)
		return 1;

	/* 只有启用引用合并的 nested 栈项才允许退化到 class 身份。 */
	if (hlock->references) {
		/* 查询目标 map 的基础 class，优先使用无需加锁的 cache。 */
		const struct lock_class *class = lock->class_cache[0];

		if (!class)
			class = look_up_lock_class(lock, 0);

		/*
		 * If look_up_lock_class() failed to find a class, we're trying
		 * to test if we hold a lock that has never yet been acquired.
		 * Clearly if the lock hasn't been acquired _ever_, we're not
		 * holding it either, so report failure.
		 */
		/* 查不到 class 表明此 map 从未成功 acquire，自然不可能正被持有。 */
		if (!class)
			return 0;

		/*
		 * References, but not a lock we're actually ref-counting?
		 * State got messed up, follow the sites that change ->references
		 * and try to make sense of it.
		 */
		/* references 非零却没有 nest_lock 违反合并协议，状态可能已被破坏。 */
		if (DEBUG_LOCKS_WARN_ON(!hlock->nest_lock))
			return 0;

		/* 合并项以 class_idx 接受同 class 的另一实例。 */
		if (hlock->class_idx == class - lock_classes)
			return 1;
	}

	/* 既非同实例，也不是合法同 class 引用项。 */
	return 0;
}

/* @depth must not be zero */
/* @depth 不得为零。 */
/**
 * find_held_lock - 在当前 IRQ-context 栈后缀中寻找指定 map 的 held_lock
 * @curr: 要搜索的任务
 * @lock: 目标 lockdep_map
 * @depth: 搜索起始栈深度，必须大于 0
 * @idx: 输出命中下标；未命中时保存停止位置
 *
 * 返回：优先匹配栈顶，否则向下搜索但不跨 irq_context 边界；无匹配为 NULL。匹配规则包含 nested
 * references 的同 class 合并语义。
 */
static struct held_lock *find_held_lock(struct task_struct *curr,
					struct lockdep_map *lock,
					unsigned int depth, int *idx)
{
	/* ret 是最终结果，hlock 当前项，prev_hlock 是较新一项；i 为当前下标。 */
	struct held_lock *ret, *hlock, *prev_hlock;
	int i;

	/* 栈顶是绝大多数正常 LIFO release 的快速路径。 */
	i = depth - 1;
	hlock = curr->held_locks + i;
	ret = hlock;
	if (match_held_lock(hlock, lock))
		goto out;

	/* 未命中栈顶后向更早项扫描。 */
	ret = NULL;
	for (i--, prev_hlock = hlock--;
	     i >= 0;
	     i--, prev_hlock = hlock--) {
		/*
		 * We must not cross into another context:
		 */
		/* IRQ context 切换是 chain 边界，release 查找不能跨越它匹配更早同 map。 */
		if (prev_hlock->irq_context != hlock->irq_context) {
			ret = NULL;
			break;
		}
		if (match_held_lock(hlock, lock)) {
			ret = hlock;
			break;
		}
	}

out:
	/* 即使未命中也返回最终扫描下标，调用者可用于诊断/重放边界。 */
	*idx = i;
	return ret;
}

/**
 * reacquire_held_locks - 用保存的 held_lock 元数据重放被临时弹出的栈后缀
 * @curr: 要恢复 held stack 的任务
 * @depth: 原后缀结束深度
 * @idx: 首个要重放的下标
 * @merged: 累计首项重放时发生 nested 引用合并的次数
 *
 * 返回：1 表示 __lock_acquire() 返回失败；正常全部重放为 0。IRQ 前置断言或意外返回码告警后也沿
 * 历史接口返回 0，调用者依赖 debug_locks 状态识别异常。每项沿用原元数据，但 sync 固定为 0。
 */
static int reacquire_held_locks(struct task_struct *curr, unsigned int depth,
				int idx, unsigned int *merged)
{
	/* hlock 遍历原数组快照位置，first_idx 用于识别仅首项可与保留栈顶合并。 */
	struct held_lock *hlock;
	int first_idx = idx;

	/* 重放调用 __lock_acquire()，沿用其 IRQ-disabled 入口契约。 */
	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return 0;

	/* 从 idx 到原 depth-1 依次恢复，保持原 acquisition 次序。 */
	for (hlock = curr->held_locks + idx; idx < depth; idx++, hlock++) {
		switch (__lock_acquire(hlock->instance,
				    hlock_class(hlock)->subclass,
				    hlock->trylock,
				    hlock->read, hlock->check,
				    hlock->hardirqs_off,
				    hlock->nest_lock, hlock->acquire_ip,
				    hlock->references, hlock->pin_count, 0)) {
		case 0:
			/* acquisition 失败；此 API 以 1 向上层表示重放失败。 */
			return 1;
		case 1:
			/* 正常重新压栈，无额外修正。 */
			break;
		case 2:
			/* 只有首个重放项可能并入仍保留的栈顶，记录供 release 下标校正。 */
			*merged += (idx == first_idx);
			break;
		default:
			/* __lock_acquire() 的返回域只能是 0/1/2。 */
			WARN_ON(1);
			return 0;
		}
	}
	/* 全部后缀恢复完成。 */
	return 0;
}

/**
 * __lock_set_class - 重设一把已持有锁的 map 身份/subclass 并重建其后栈链
 * @lock: 必须已出现在当前 held stack 的 map
 * @name: 新 class 名称
 * @key: 新 class key
 * @subclass: 新 subclass
 * @ip: 操作调用点，供失衡报告使用
 *
 * 返回：重建并通过深度校验为 1，否则 0。函数找到目标后重初始化 map/class，临时截断目标及其后
 * 栈项，再按原 acquisition 元数据重放；新 class 可能让首项与前驱合并，故用 merged 修正期望深度。
 */
static int
__lock_set_class(struct lockdep_map *lock, const char *name,
		 struct lock_class_key *key, unsigned int subclass,
		 unsigned long ip)
{
	/* curr 是待重建任务；depth 保存原深度，merged 统计重放时首项合并。 */
	struct task_struct *curr = current;
	unsigned int depth, merged = 0;
	/* hlock 是目标旧栈项，class 是新 class，i 是目标下标。 */
	struct held_lock *hlock;
	struct lock_class *class;
	int i;

	/* lockdep 已关闭时不能安全重写 class/chain。 */
	if (unlikely(!debug_locks))
		return 0;

	/* 保存原栈深度，重放后用于一致性检查。 */
	depth = curr->lockdep_depth;
	/*
	 * This function is about (re)setting the class of a held lock,
	 * yet we're not actually holding any locks. Naughty user!
	 */
	/* set_class 只适用于当前已持锁；空栈调用明显错误。 */
	if (DEBUG_LOCKS_WARN_ON(!depth))
		return 0;

	/* 搜索不跨 IRQ context，找不到按错误 release 同样报告失衡。 */
	hlock = find_held_lock(curr, lock, depth, &i);
	if (!hlock) {
		print_unlock_imbalance_bug(curr, lock, ip);
		return 0;
	}

	/* 保留 map 原 wait/lock type，仅替换名称、key，并注册请求 subclass。 */
	lockdep_init_map_type(lock, name, key, 0,
			      lock->wait_type_inner,
			      lock->wait_type_outer,
			      lock->lock_type);
	class = register_lock_class(lock, subclass, 0);
	/* 目标栈项改指新 class；后续重放将据此重新计算依赖和 chain key。 */
	hlock->class_idx = class - lock_classes;

	/* 回退到目标之前，并恢复它加入前保存的累计 key。 */
	curr->lockdep_depth = i;
	curr->curr_chain_key = hlock->prev_chain_key;

	/* 从目标自身开始依次重放原后缀。 */
	if (reacquire_held_locks(curr, depth, i, &merged))
		return 0;

	/*
	 * I took it apart and put it back together again, except now I have
	 * these 'spare' parts.. where shall I put them.
	 */
	/* 拆开再组装后，除允许的首项 class 合并外不得多出或丢失栈项。 */
	if (DEBUG_LOCKS_WARN_ON(curr->lockdep_depth != depth - merged))
		return 0;
	/* class 切换后的链已重建。 */
	return 1;
}

/**
 * __lock_downgrade - 把一把已持有写锁降级为读锁并重建其后栈链
 * @lock: 当前任务持有的目标 map
 * @ip: downgrade 调用点，也替换目标 acquire_ip
 *
 * 返回：成功为 1，空栈、找不到锁、重放或深度异常为 0。函数从目标处截断 held stack，把 read
 * 改为 1 后重放；class 未改变，因此任何 nested merge 都视作错误。
 */
static int __lock_downgrade(struct lockdep_map *lock, unsigned long ip)
{
	/* depth 保存原栈深度；merged 理论上必须保持 0。 */
	struct task_struct *curr = current;
	unsigned int depth, merged = 0;
	struct held_lock *hlock;
	int i;

	/* 调试关闭时不再改任务 lockdep 栈。 */
	if (unlikely(!debug_locks))
		return 0;

	/* 保存原深度，最终必须原样恢复。 */
	depth = curr->lockdep_depth;
	/*
	 * This function is about (re)setting the class of a held lock,
	 * yet we're not actually holding any locks. Naughty user!
	 */
	/* downgrade 同样要求至少持有一把锁。 */
	if (DEBUG_LOCKS_WARN_ON(!depth))
		return 0;

	/* 只在当前 IRQ-context chain 内寻找目标。 */
	hlock = find_held_lock(curr, lock, depth, &i);
	if (!hlock) {
		print_unlock_imbalance_bug(curr, lock, ip);
		return 0;
	}

	/* 临时弹出目标及更内层锁，恢复目标获取前的 chain key。 */
	curr->lockdep_depth = i;
	curr->curr_chain_key = hlock->prev_chain_key;

	/* 已是读锁仍被 downgrade 会告警；随后固定为普通读语义并更新调用点。 */
	WARN(hlock->read, "downgrading a read lock");
	hlock->read = 1;
	hlock->acquire_ip = ip;

	/* 用修改后的目标元数据开始重建后缀。 */
	if (reacquire_held_locks(curr, depth, i, &merged))
		return 0;

	/* Merging can't happen with unchanged classes.. */
	/* class 全部未变，重放不应产生新的同 class nested 合并。 */
	if (DEBUG_LOCKS_WARN_ON(merged))
		return 0;

	/*
	 * I took it apart and put it back together again, except now I have
	 * these 'spare' parts.. where shall I put them.
	 */
	/* 降级只改变读写模式，最终 held-lock 项数必须与原栈一致。 */
	if (DEBUG_LOCKS_WARN_ON(curr->lockdep_depth != depth))
		return 0;

	/* 目标及其后依赖已按读模式重新验证。 */
	return 1;
}

/*
 * Remove the lock from the list of currently held locks - this gets
 * called on mutex_unlock()/spin_unlock*() (or on a failed
 * mutex_lock_interruptible()).
 */
/*
 * 从当前 held-lock 列表移除锁；由 mutex_unlock()/spin_unlock*() 或失败的
 * mutex_lock_interruptible() 调用。
 */
/**
 * __lock_release - 释放匹配 held_lock，并在非 LIFO 情况下重建其后栈项
 * @lock: 要释放的 lockdep_map
 * @ip: release 调用点
 *
 * 返回：栈顶移除或仅减少仍非零 references 时为 1；非 LIFO 成功重放后为 0；错误也为 0，需结合
 * debug_locks 判断。精确实例释放会更新 holdtime；pin_count 非零会告警但继续。
 */
static int
__lock_release(struct lockdep_map *lock, unsigned long ip)
{
	/* depth 是原栈深度；merged 初始 1 代表被移除目标本身。 */
	struct task_struct *curr = current;
	unsigned int depth, merged = 1;
	struct held_lock *hlock;
	int i;

	/* 已停止验证时 release 不再触碰栈。 */
	if (unlikely(!debug_locks))
		return 0;

	/* 固定原深度供查找和重放范围使用。 */
	depth = curr->lockdep_depth;
	/*
	 * So we're all set to release this lock.. wait what lock? We don't
	 * own any locks, you've been drinking again?
	 */
	/* 空栈释放必然失衡，打印对象与调用点。 */
	if (depth <= 0) {
		print_unlock_imbalance_bug(curr, lock, ip);
		return 0;
	}

	/*
	 * Check whether the lock exists in the current stack
	 * of held locks:
	 */
	/* 在当前 IRQ-context 栈后缀查找实例或合法 nested 同 class 引用。 */
	hlock = find_held_lock(curr, lock, depth, &i);
	if (!hlock) {
		print_unlock_imbalance_bug(curr, lock, ip);
		return 0;
	}

	/* 只有精确 map 实例拥有可归属的 lockstat holdtime。 */
	if (hlock->instance == lock)
		lock_release_holdtime(hlock);

	/* pinned 锁按约定不应释放；告警用于发现生命周期破坏。 */
	WARN(hlock->pin_count, "releasing a pinned lock\n");

	/* 引用合并项先只减引用；仍有引用时逻辑持有关系未结束，无需改栈。 */
	if (hlock->references) {
		hlock->references--;
		if (hlock->references) {
			/*
			 * We had, and after removing one, still have
			 * references, the current lock stack is still
			 * valid. We're done!
			 */
			/* 移除一个引用后仍非零，当前 stack/chain key 保持有效。 */
			return 1;
		}
	}

	/*
	 * We have the right lock to unlock, 'hlock' points to it.
	 * Now we remove it from the stack, and add back the other
	 * entries (if any), recalculating the hash along the way:
	 */
	/*
	 * hlock 已指向正确目标。将栈截到目标之前，并从目标保存的 prev_chain_key 恢复累计状态；若目标
	 * 不是栈顶，再把更内层项按原顺序重新 acquire，以重新计算中间依赖和 hash。
	 */

	curr->lockdep_depth = i;
	curr->curr_chain_key = hlock->prev_chain_key;

	/*
	 * The most likely case is when the unlock is on the innermost
	 * lock. In this case, we are done!
	 */
	/* 常见严格 LIFO release 无后缀可重放，截栈即完成。 */
	if (i == depth-1)
		return 1;

	/* 非 LIFO release 跳过目标，从 i+1 开始恢复更内层持有。 */
	if (reacquire_held_locks(curr, depth, i + 1, &merged))
		return 0;

	/*
	 * We had N bottles of beer on the wall, we drank one, but now
	 * there's not N-1 bottles of beer left on the wall...
	 * Pouring two of the bottles together is acceptable.
	 */
	/* 期望少一项；若首个重放项与保留栈顶合并，则允许再少一项并由 merged 计入。 */
	DEBUG_LOCKS_WARN_ON(curr->lockdep_depth != depth - merged);

	/*
	 * Since reacquire_held_locks() would have called check_chain_key()
	 * indirectly via __lock_acquire(), we don't need to do it again
	 * on return.
	 */
	/* __lock_acquire() 已在每次重放提交后间接调用 check_chain_key()，无需再次全栈重算。 */
	/* 返回 0 表示走过非 LIFO 慢路径，调用者不应把它简单解释为失败。 */
	return 0;
}

/**
 * __lock_is_held - 查询当前任务是否持有 map，并可约束读写模式
 * @lock: 目标 lockdep_map
 * @read: -1 接受任意模式，0 要求写，1 要求任意非零 read
 *
 * 返回：找到匹配且模式符合为 LOCK_STATE_HELD，否则为 LOCK_STATE_NOT_HELD。找到 map 但模式不符
 * 会立即返回 not-held；匹配包含 nested references 的同 class 语义。
 */
static __always_inline
int __lock_is_held(const struct lockdep_map *lock, int read)
{
	/* curr 提供 held stack，i 正向扫描 acquisition 顺序。 */
	struct task_struct *curr = current;
	int i;

	/* 遍历全部 context；这是“任务是否持有”查询，不受 release 的 context 边界限制。 */
	for (i = 0; i < curr->lockdep_depth; i++) {
		/* hlock 只在当前迭代有效。 */
		struct held_lock *hlock = curr->held_locks + i;

		/* 先按实例/nested class 匹配，再核对可选读写条件。 */
		if (match_held_lock(hlock, lock)) {
			/* -1 是不关心模式；否则把 read 编码归一成布尔值。 */
			if (read == -1 || !!hlock->read == read)
				return LOCK_STATE_HELD;

			/* 同 map 已找到但模式不符，不再寻找另一栈项。 */
			return LOCK_STATE_NOT_HELD;
		}
	}

	/* 栈中没有代表该 map 的项。 */
	return LOCK_STATE_NOT_HELD;
}

/**
 * __lock_pin_lock - 给当前持有的锁增加不可释放 pin 并返回配对 cookie
 * @lock: 必须由当前任务持有的 map
 *
 * 返回：成功为非零随机化 cookie，lockdep 关闭或未持有为 NIL_COOKIE；未持有还会告警。cookie 值
 * 累加进 u32 pin_count，使错误 cookie/嵌套 pin 更容易被 unpin/release 检出。
 */
static struct pin_cookie __lock_pin_lock(struct lockdep_map *lock)
{
	/* cookie 默认无效；curr/i 用于扫描 held stack。 */
	struct pin_cookie cookie = NIL_COOKIE;
	struct task_struct *curr = current;
	int i;

	/* 调试停止时保持 NIL_COOKIE。 */
	if (unlikely(!debug_locks))
		return cookie;

	/* 找到第一个实例或 nested-class 匹配项。 */
	for (i = 0; i < curr->lockdep_depth; i++) {
		struct held_lock *hlock = curr->held_locks + i;

		if (match_held_lock(hlock, lock)) {
			/*
			 * Grab 16bits of randomness; this is sufficient to not
			 * be guessable and still allows some pin nesting in
			 * our u32 pin_count.
			 */
			/* 取 16 位不可轻易猜测的时钟低位并加 1，避免与 NIL_COOKIE 的零值冲突。 */
			cookie.val = 1 + (sched_clock() & 0xffff);
			/* 累加而非覆盖，允许有限层嵌套 pin，配对 unpin 再减同值。 */
			hlock->pin_count += cookie.val;
			return cookie;
		}
	}

	/* pin 未持有锁是 API 误用，返回初始无效 cookie。 */
	WARN(1, "pinning an unheld lock\n");
	return cookie;
}

/**
 * __lock_repin_lock - 用既有 cookie 恢复一次暂时解除的 pin
 * @lock: 当前任务重新持有的目标 map
 * @cookie: 原 __lock_pin_lock() 返回的 cookie
 *
 * 找到 held_lock 后把 cookie 值重新加回 pin_count；未持有则告警。用于需要临时 unpin 后保持同一
 * pin 身份的路径，不生成新随机值。
 */
static void __lock_repin_lock(struct lockdep_map *lock, struct pin_cookie cookie)
{
	/* curr/i 扫描当前持锁栈。 */
	struct task_struct *curr = current;
	int i;

	/* lockdep 关闭时 pin 协议也停止验证。 */
	if (unlikely(!debug_locks))
		return;

	for (i = 0; i < curr->lockdep_depth; i++) {
		struct held_lock *hlock = curr->held_locks + i;

		/* 匹配后恢复原 cookie 对 pin_count 的贡献。 */
		if (match_held_lock(hlock, lock)) {
			hlock->pin_count += cookie.val;
			return;
		}
	}

	/* repin 目标必须已重新 acquire。 */
	WARN(1, "pinning an unheld lock\n");
}

/**
 * __lock_unpin_lock - 用配对 cookie 从 held_lock 扣除一次 pin
 * @lock: 当前任务持有的目标 map
 * @cookie: 要撤销的 pin cookie
 *
 * 未持有、从未 pin 或扣减下溢都会告警；下溢时把 pin_count 修复为 0，避免后续释放持续误报。
 */
static void __lock_unpin_lock(struct lockdep_map *lock, struct pin_cookie cookie)
{
	/* curr/i 定位目标 held_lock。 */
	struct task_struct *curr = current;
	int i;

	/* 调试关闭后不再维护 pin_count。 */
	if (unlikely(!debug_locks))
		return;

	for (i = 0; i < curr->lockdep_depth; i++) {
		struct held_lock *hlock = curr->held_locks + i;

		/* 找到目标后先确认至少存在某个 pin，再按 cookie 精确扣减。 */
		if (match_held_lock(hlock, lock)) {
			/* 零计数无法合法 unpin，告警后避免无符号下溢。 */
			if (WARN(!hlock->pin_count, "unpinning an unpinned lock\n"))
				return;

			hlock->pin_count -= cookie.val;

			/* cookie 不配对或重复 unpin 可让 u32 回绕；转 signed 检测并自愈为零。 */
			if (WARN((int)hlock->pin_count < 0, "pin count corrupted\n"))
				hlock->pin_count = 0;

			return;
		}
	}

	/* 栈中没有目标仍请求 unpin，属于持有生命周期错误。 */
	WARN(1, "unpinning an unheld lock\n");
}

/*
 * Check whether we follow the irq-flags state precisely:
 */
/* 检查 lockdep 软件状态是否精确跟随真实 irq flags。 */
/**
 * check_flags - 对照保存的硬件 IRQ flags 与 lockdep hard/softirq 软件状态
 * @flags: 关闭本地 IRQ 前保存的调用点硬件标志
 *
 * 仅 PROVE_LOCKING+DEBUG_LOCKDEP 生效。函数为 noinstr，显式打开 instrumentation 后检查 hardirq；
 * 非 PREEMPT_RT 且不在 hardirq 时再检查 softirq。告警若关闭 lockdep，则打印最近 IRQ trace。
 */
static noinstr void check_flags(unsigned long flags)
{
#if defined(CONFIG_PROVE_LOCKING) && defined(CONFIG_DEBUG_LOCKDEP)
	/* 已经关闭后无需再比较，避免重复诊断。 */
	if (!debug_locks)
		return;

	/* Get the warning out..  */
	/* 下面的 WARN/printk 需要允许插桩，离开前必须成对 instrumentation_end()。 */
	instrumentation_begin();

	/* flags 描述 hook 入口前真实 hardirq 状态，应与软件 hardirqs_enabled 恰好互补。 */
	if (irqs_disabled_flags(flags)) {
		if (DEBUG_LOCKS_WARN_ON(lockdep_hardirqs_enabled())) {
			/* 硬件已关而软件仍 on，通常缺少 irq-off annotation。 */
			printk("possible reason: unannotated irqs-off.\n");
		}
	} else {
		if (DEBUG_LOCKS_WARN_ON(!lockdep_hardirqs_enabled())) {
			/* 硬件已开而软件仍 off，通常缺少 irq-on annotation。 */
			printk("possible reason: unannotated irqs-on.\n");
		}
	}

#ifndef CONFIG_PREEMPT_RT
	/*
	 * We dont accurately track softirq state in e.g.
	 * hardirq contexts (such as on 4KSTACKS), so only
	 * check if not in hardirq contexts:
	 */
	/* 非 RT 下 hardirq context 中 softirq 跟踪并不精确（如 4KSTACKS），因此只在非 hardirq 校验。 */
	if (!hardirq_count()) {
		if (softirq_count()) {
			/* like the above, but with softirqs */
			/* 实际 softirq 计数非零表示 disabled/in-context，软件 enabled 应为假。 */
			DEBUG_LOCKS_WARN_ON(current->softirqs_enabled);
		} else {
			/* lick the above, does it taste good? */
			/* 实际无 softirq 禁用计数时，软件状态应为 enabled。 */
			DEBUG_LOCKS_WARN_ON(!current->softirqs_enabled);
		}
	}
#endif

	/* 任一 WARN 关闭了 lockdep 时，补充最近四类 IRQ 开关事件帮助定位漏标。 */
	if (!debug_locks)
		print_irqtrace_events(current);

	/* 恢复 noinstr 区域的插桩禁止状态。 */
	instrumentation_end();
#endif
}

/**
 * lock_set_class - 在安全入口包装下重设当前已持锁的 class
 * @lock: 已持有目标 map
 * @name: 新名称
 * @key: 新 key
 * @subclass: 新 subclass
 * @ip: 调用点
 *
 * 若 lockdep 可进入，则保存并关闭本地 IRQ、开启递归门、校验 flags，调用 __lock_set_class()；成功
 * 后再全栈核验 chain key，最后恢复递归与 IRQ 状态。
 */
void lock_set_class(struct lockdep_map *lock, const char *name,
		    struct lock_class_key *key, unsigned int subclass,
		    unsigned long ip)
{
	/* flags 保存 API 入口时的真实 IRQ 状态，供 check_flags() 对照。 */
	unsigned long flags;

	/* 同时避开全局关闭和 lockdep 内部递归。 */
	if (unlikely(!lockdep_enabled()))
		return;

	/* class 重建需要稳定的本 CPU held stack。 */
	raw_local_irq_save(flags);
	lockdep_recursion_inc();
	check_flags(flags);
	/* 成功重放后再独立重算 key，覆盖最终任务状态。 */
	if (__lock_set_class(lock, name, key, subclass, ip))
		check_chain_key(current);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_set_class);

/**
 * lock_downgrade - 在安全入口包装下把当前已持有写锁降级为读锁
 * @lock: 已持有目标 map
 * @ip: downgrade 调用点
 *
 * 关闭本地 IRQ并进入递归门后调用 __lock_downgrade() 重建目标及后缀；成功时校验最终 chain key，
 * 然后恢复入口状态。
 */
void lock_downgrade(struct lockdep_map *lock, unsigned long ip)
{
	/* flags 用于 IRQ 状态核对和恢复。 */
	unsigned long flags;

	if (unlikely(!lockdep_enabled()))
		return;

	/* held stack 改写全程在本地 IRQ 关闭和递归保护下。 */
	raw_local_irq_save(flags);
	lockdep_recursion_inc();
	check_flags(flags);
	if (__lock_downgrade(lock, ip))
		check_chain_key(current);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_downgrade);

/* NMI context !!! */
/* 仅在 NMI context 调用。 */
/**
 * verify_lock_unused - 在无法正常跟踪的 NMI 获取中确认目标 class 从未被使用
 * @lock: NMI 正在获取的 map
 * @hlock: 临时构造、携带本次读写模式的 held lock
 * @subclass: 要查询的 subclass
 *
 * PROVE_LOCKING 下，无 class 或无冲突 usage 直接通过；写获取与 USED/USED_READ 冲突，读获取只与
 * USED 冲突。命中时补 class_idx 并以 usage bug 路径报告。其他配置为空操作。
 */
static void verify_lock_unused(struct lockdep_map *lock, struct held_lock *hlock, int subclass)
{
#ifdef CONFIG_PROVE_LOCKING
	/* class 是既有注册项；mask 初始只排除曾有写使用。 */
	struct lock_class *class = look_up_lock_class(lock, subclass);
	unsigned long mask = LOCKF_USED;

	/* if it doesn't have a class (yet), it certainly hasn't been used yet */
	/* 尚无 class 表示该 map/subclass 从未成功 acquire，自然也未使用。 */
	if (!class)
		return;

	/*
	 * READ locks only conflict with USED, such that if we only ever use
	 * READ locks, there is no deadlock possible -- RCU.
	 */
	/* 读锁只与历史写 USED 冲突；若始终只有 reader，则像 RCU 一样不会因读读形成死锁。 */
	if (!hlock->read)
		mask |= LOCKF_USED_READ;

	/* class 没有任何会与本次模式冲突的历史 USED 位。 */
	if (!(class->usage_mask & mask))
		return;

	/* 报告前让临时 hlock 能被普通 class/trace 打印 helper 解析。 */
	hlock->class_idx = class - lock_classes;

	/* LOCK_USAGE_STATES 作为无正常名称的新状态哨兵，表示 NMI 中不应再次使用的冲突。 */
	print_usage_bug(current, hlock, LOCK_USED, LOCK_USAGE_STATES);
#endif
}

/**
 * lockdep_nmi - 判断当前是否是可执行 NMI 特殊验证的入口
 *
 * 返回：当前 CPU 未处在 lockdep_recursion 且当前确为 NMI 时为 true，否则 false。raw_cpu_read()
 * 避免在 NMI 判断中引入普通 per-CPU 访问检查。
 */
static bool lockdep_nmi(void)
{
	/* NMI 若打断 lockdep 自身，不能递归使用其共享状态。 */
	if (raw_cpu_read(lockdep_recursion))
		return false;

	/* 非 NMI 不需要特殊 verify_lock_unused 路径。 */
	if (!in_nmi())
		return false;

	/* 两项条件均满足。 */
	return true;
}

/*
 * read_lock() is recursive if:
 * 1. We force lockdep think this way in selftests or
 * 2. The implementation is not queued read/write lock or
 * 3. The locker is at an in_interrupt() context.
 */
/*
 * read_lock() 在以下任一条件下视为可递归：自测强制该语义；实现不是 queued rwlock；或获取者位于
 * in_interrupt() context。
 */
/**
 * read_lock_is_recursive - 判断当前构建/上下文中的读锁是否允许递归获取
 *
 * 返回：强制自测标志、非 QUEUED_RWLOCKS 实现或中断上下文任一成立即 true，否则 false。
 */
bool read_lock_is_recursive(void)
{
	/* 三个条件分别覆盖测试、实现能力和中断路径的递归语义。 */
	return force_read_lock_recursive ||
	       !IS_ENABLED(CONFIG_QUEUED_RWLOCKS) ||
	       in_interrupt();
}
EXPORT_SYMBOL_GPL(read_lock_is_recursive);

/*
 * We are not always called with irqs disabled - do that here,
 * and also avoid lockdep recursion:
 */
/* 调用时不保证 IRQ 已关闭，因此在这里关闭，并同时避免 lockdep 递归。 */
/**
 * lock_acquire - 对外记录一次锁获取并进入内部依赖验证
 * @lock: 被获取 map
 * @subclass: 获取 subclass
 * @trylock: 是否为 trylock
 * @read: 读写模式
 * @check: 是否启用完整依赖检查
 * @nest_lock: 可选嵌套串行锁
 * @ip: 获取调用点
 *
 * 始终先发 tracepoint。正常路径做 KASAN 探测、保存并关闭 IRQ、核对 flags、进入递归门后调用
 * __lock_acquire()；NMI 递归受阻时，对非 trylock 退化为“class 必须未使用”验证。
 */
void lock_acquire(struct lockdep_map *lock, unsigned int subclass,
			  int trylock, int read, int check,
			  struct lockdep_map *nest_lock, unsigned long ip)
{
	/* flags 保存 lock_acquire() 入口的真实 IRQ 状态。 */
	unsigned long flags;

	/* trace 事件独立于 debug_locks 是否继续做图验证。 */
	trace_lock_acquire(lock, subclass, trylock, read, check, nest_lock, ip);

	/* 全局关闭后不再访问 map 或 held stack。 */
	if (!debug_locks)
		return;

	/*
	 * As KASAN instrumentation is disabled and lock_acquire() is usually
	 * the first lockdep call when a task tries to acquire a lock, add
	 * kasan_check_byte() here to check for use-after-free and other
	 * memory errors.
	 */
	/*
	 * KASAN 插桩在相关低级路径被关闭，而 lock_acquire() 常是任务首次触碰 map 的 lockdep 入口；显式
	 * 读一个字节可尽早发现 map 的 UAF 或其他地址错误。
	 */
	kasan_check_byte(lock);

	/* 常规 lockdep 不可进入时，仅保留 NMI 的保守安全检查。 */
	if (unlikely(!lockdep_enabled())) {
		/* XXX allow trylock from NMI ?!? */
		/* 原实现仍留有 NMI trylock 是否应允许的设计问题；当前只检查非 trylock。 */
		if (lockdep_nmi() && !trylock) {
			/* 临时 hlock 只初始化 verify/report 会读取的字段，不提交到任务栈。 */
			struct held_lock hlock;

			hlock.acquire_ip = ip;
			hlock.instance = lock;
			hlock.nest_lock = nest_lock;
			hlock.irq_context = 2; // XXX
			/* irq_context 的常量 2 同样保留上游待澄清标记，不在注释任务中改代码。 */
			hlock.trylock = trylock;
			hlock.read = read;
			hlock.check = check;
			hlock.hardirqs_off = true;
			hlock.references = 0;

			/* NMI 中不能建图，只能确认该 class 从未在可冲突模式使用。 */
			verify_lock_unused(lock, &hlock, subclass);
		}
		return;
	}

	/* 正常路径关闭本地 IRQ，先对照入口 flags 与软件跟踪状态。 */
	raw_local_irq_save(flags);
	check_flags(flags);

	lockdep_recursion_inc();
	__lock_acquire(lock, subclass, trylock, read, check,
		       irqs_disabled_flags(flags), nest_lock, ip, 0, 0, 0);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_acquire);

/**
 * lock_release - 对外记录一次锁释放并更新当前任务 held stack
 * @lock: 被释放 map
 * @ip: release 调用点
 *
 * 先发 release tracepoint；lockdep 可用且非 no_track 时，关闭 IRQ、校验 flags、进入递归门调用
 * __lock_release()。其返回 1 的快速路径需显式重算 chain key，非 LIFO 慢路径已在重放中核验。
 */
void lock_release(struct lockdep_map *lock, unsigned long ip)
{
	/* flags 保存 release 入口 IRQ 状态。 */
	unsigned long flags;

	/* tracing 不受 lockdep 跳过条件影响。 */
	trace_lock_release(lock, ip);

	/* no_track map 从未入栈，不能尝试匹配释放。 */
	if (unlikely(!lockdep_enabled() ||
		     lock->key == &__lockdep_no_track__))
		return;

	/* 稳定本 CPU held stack，并核对调用前硬件/软件 IRQ 状态。 */
	raw_local_irq_save(flags);
	check_flags(flags);

	lockdep_recursion_inc();
	/* 返回 1 的快速路径未在重放中调用 check_chain_key()，此处补做。 */
	if (__lock_release(lock, ip))
		check_chain_key(current);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_release);

/*
 * lock_sync() - A special annotation for synchronize_{s,}rcu()-like API.
 *
 * No actual critical section is created by the APIs annotated with this: these
 * APIs are used to wait for one or multiple critical sections (on other CPUs
 * or threads), and it means that calling these APIs inside these critical
 * sections is potential deadlock.
 */
/*
 * lock_sync() 是 synchronize_{s,}rcu() 类 API 的特殊 annotation。它不创建真实临界区，而是等待
 * 其他 CPU/线程上的一个或多个临界区结束；在那些临界区内部调用该同步 API 可能形成死锁。
 */
/**
 * lock_sync - 验证一个“等待其他临界区”的同步依赖而不压入 held stack
 * @lock: 表示同步域的 map
 * @subclass: subclass
 * @read: 同步关系的读写语义
 * @check: 是否做依赖验证
 * @nest_lock: 可选嵌套 map
 * @ip: 调用点
 *
 * 在 IRQ 关闭和递归门内以 sync=1 调用 __lock_acquire()；内部完成依赖验证后不提交临界区，随后
 * 核验当前真实 held stack 的 chain key。
 */
void lock_sync(struct lockdep_map *lock, unsigned subclass, int read,
	       int check, struct lockdep_map *nest_lock, unsigned long ip)
{
	/* flags 保存同步 annotation 入口 IRQ 状态。 */
	unsigned long flags;

	if (unlikely(!lockdep_enabled()))
		return;

	/* 与 acquire/release 公共包装一致地稳定状态并检查 flags。 */
	raw_local_irq_save(flags);
	check_flags(flags);

	lockdep_recursion_inc();
	/* trylock=0、references/pin=0、sync=1 是该特殊 acquisition 的固定属性。 */
	__lock_acquire(lock, subclass, 0, read, check,
		       irqs_disabled_flags(flags), nest_lock, ip, 0, 0, 1);
	check_chain_key(current);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_sync);

/**
 * lock_is_held_type - 查询当前任务对 map 的持有及读写类型
 * @lock: 目标 map
 * @read: -1 任意，0 写，1 读
 *
 * 返回：HELD、NOT_HELD 或 lockdep 不可用时的 UNKNOWN。UNKNOWN 避免 lockdep_assert_held() 与
 * lockdep_assert_not_held() 在验证关闭/递归期间产生假阴性。函数为 noinstr。
 */
noinstr int lock_is_held_type(const struct lockdep_map *lock, int read)
{
	/* flags 保存 IRQ 状态；ret 默认 not-held，正常查询会覆盖。 */
	unsigned long flags;
	int ret = LOCK_STATE_NOT_HELD;

	/*
	 * Avoid false negative lockdep_assert_held() and
	 * lockdep_assert_not_held().
	 */
	/* 无法可靠查询时返回 UNKNOWN，让正反两种 assertion 都避免误判。 */
	if (unlikely(!lockdep_enabled()))
		return LOCK_STATE_UNKNOWN;

	/* noinstr 包装同样在本地 IRQ 关闭与递归门内扫描 held stack。 */
	raw_local_irq_save(flags);
	check_flags(flags);

	lockdep_recursion_inc();
	ret = __lock_is_held(lock, read);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);

	/* 返回内部实例/class 与模式匹配结果。 */
	return ret;
}
EXPORT_SYMBOL_GPL(lock_is_held_type);
NOKPROBE_SYMBOL(lock_is_held_type);

/**
 * lock_pin_lock - 在公共入口保护下 pin 一把当前持有锁
 * @lock: 当前任务持有的目标 map
 *
 * 返回：成功 cookie 或不可验证/未持有时 NIL_COOKIE。包装负责 lockdep_enabled 门、IRQ 保存、flags
 * 核验与递归隔离，实际计数由 __lock_pin_lock() 完成。
 */
struct pin_cookie lock_pin_lock(struct lockdep_map *lock)
{
	/* cookie 默认无效，flags 保存入口 IRQ 状态。 */
	struct pin_cookie cookie = NIL_COOKIE;
	unsigned long flags;

	if (unlikely(!lockdep_enabled()))
		return cookie;

	raw_local_irq_save(flags);
	check_flags(flags);

	lockdep_recursion_inc();
	cookie = __lock_pin_lock(lock);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);

	/* 透传内部生成的随机化 cookie。 */
	return cookie;
}
EXPORT_SYMBOL_GPL(lock_pin_lock);

/**
 * lock_repin_lock - 在公共入口保护下恢复既有 pin cookie
 * @lock: 已重新持有的目标 map
 * @cookie: 原 pin 身份
 *
 * lockdep 可进入时关闭 IRQ并隔离递归，再由 __lock_repin_lock() 把 cookie 值加回 held_lock。
 */
void lock_repin_lock(struct lockdep_map *lock, struct pin_cookie cookie)
{
	/* flags 保存入口 IRQ 状态。 */
	unsigned long flags;

	if (unlikely(!lockdep_enabled()))
		return;

	raw_local_irq_save(flags);
	check_flags(flags);

	lockdep_recursion_inc();
	__lock_repin_lock(lock, cookie);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_repin_lock);

/**
 * lock_unpin_lock - 在公共入口保护下撤销一个 pin cookie
 * @lock: 当前持有目标 map
 * @cookie: 要扣除的配对 cookie
 *
 * 关闭本地 IRQ并进入递归门后调用 __lock_unpin_lock()；内部负责未持有、未 pin 与计数损坏告警。
 */
void lock_unpin_lock(struct lockdep_map *lock, struct pin_cookie cookie)
{
	/* flags 保存入口 IRQ 状态。 */
	unsigned long flags;

	if (unlikely(!lockdep_enabled()))
		return;

	raw_local_irq_save(flags);
	check_flags(flags);

	lockdep_recursion_inc();
	__lock_unpin_lock(lock, cookie);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_unpin_lock);

#ifdef CONFIG_LOCK_STAT
/**
 * print_lock_contention_bug - 报告 contended/acquired 回调找不到对应 held lock
 * @curr: 当前任务
 * @lock: 发生回调的 map
 * @ip: contention/acquired 调用点
 *
 * 首次报告关闭 lockdep；非 silent 时打印 map、调用点、当前 held locks 和栈回溯。
 */
static void print_lock_contention_bug(struct task_struct *curr,
				      struct lockdep_map *lock,
				      unsigned long ip)
{
	/* 只允许首次关闭者打印，避免统计 hook 错误刷屏。 */
	if (!debug_locks_off())
		return;
	if (debug_locks_silent)
		return;

	nbcon_cpu_emergency_enter();

	pr_warn("\n");
	pr_warn("=================================\n");
	pr_warn("WARNING: bad contention detected!\n");
	print_kernel_ident();
	pr_warn("---------------------------------\n");
	pr_warn("%s/%d is trying to contend lock (",
		curr->comm, task_pid_nr(curr));
	print_lockdep_cache(lock);
	pr_cont(") at:\n");
	print_ip_sym(KERN_WARNING, ip);
	pr_warn("but there are no locks held!\n");
	pr_warn("\nother info that might help us debug this:\n");
	lockdep_print_held_locks(curr);

	pr_warn("\nstack backtrace:\n");
	dump_stack();

	/* 完成报告后退出紧急区。 */
	nbcon_cpu_emergency_exit();
}

/**
 * __lock_contended - 记录一次已跟踪锁的竞争起点和热点位置
 * @lock: 正在等待的 map
 * @ip: 观察到 contention 的代码位置
 *
 * 要求公共包装已关闭 IRQ并隔离递归。找到精确实例后记录 waittime 起点，更新 class 的 contention
 * point、contending point 与跨 CPU bounce 统计；no_track 或 nested class-only 匹配不计实例统计。
 */
static void
__lock_contended(struct lockdep_map *lock, unsigned long ip)
{
	/* curr/hlock 定位栈项，stats 是当前 CPU 的 class 统计。 */
	struct task_struct *curr = current;
	struct held_lock *hlock;
	struct lock_class_stats *stats;
	unsigned int depth;
	int i, contention_point, contending_point;

	depth = curr->lockdep_depth;
	/*
	 * Whee, we contended on this lock, except it seems we're not
	 * actually trying to acquire anything much at all..
	 */
	/* contention 回调应发生在 lock_acquire 已建立临时/逻辑持有记录之后。 */
	if (DEBUG_LOCKS_WARN_ON(!depth))
		return;

	if (unlikely(lock->key == &__lockdep_no_track__))
		return;

	hlock = find_held_lock(curr, lock, depth, &i);
	if (!hlock) {
		print_lock_contention_bug(curr, lock, ip);
		return;
	}

	if (hlock->instance != lock)
		return;

	hlock->waittime_stamp = lockstat_clock();

	contention_point = lock_point(hlock_class(hlock)->contention_point, ip);
	contending_point = lock_point(hlock_class(hlock)->contending_point,
				      lock->ip);

	stats = get_lock_stats(hlock_class(hlock));
	if (contention_point < LOCKSTAT_POINTS)
		stats->contention_point[contention_point]++;
	if (contending_point < LOCKSTAT_POINTS)
		stats->contending_point[contending_point]++;
	if (lock->cpu != smp_processor_id())
		stats->bounces[bounce_contended + !!hlock->read]++;
}

/**
 * __lock_acquired - 在竞争结束取得锁时结算等待时间和迁移统计
 * @lock: 已取得所有权的 map
 * @ip: 本次实际取得锁的位置
 *
 * 找到精确 held-lock 实例后，若存在 waittime_stamp 则计算等待时长并把 hold 起点移到真正 acquired
 * 时刻；更新读/写 waittime、跨 CPU acquired bounce，并保存 map 最近 CPU/IP。
 */
static void
__lock_acquired(struct lockdep_map *lock, unsigned long ip)
{
	/* curr/hlock/stats 定位任务栈和 class 统计。 */
	struct task_struct *curr = current;
	struct held_lock *hlock;
	struct lock_class_stats *stats;
	unsigned int depth;
	u64 now, waittime = 0;
	int i, cpu;

	depth = curr->lockdep_depth;
	/*
	 * Yay, we acquired ownership of this lock we didn't try to
	 * acquire, how the heck did that happen?
	 */
	/* acquired 回调前应已有 lock_acquire 记录，空栈说明调用协议破坏。 */
	if (DEBUG_LOCKS_WARN_ON(!depth))
		return;

	if (unlikely(lock->key == &__lockdep_no_track__))
		return;

	hlock = find_held_lock(curr, lock, depth, &i);
	if (!hlock) {
		print_lock_contention_bug(curr, lock, _RET_IP_);
		return;
	}

	if (hlock->instance != lock)
		return;

	cpu = smp_processor_id();
	if (hlock->waittime_stamp) {
		now = lockstat_clock();
		waittime = now - hlock->waittime_stamp;
		hlock->holdtime_stamp = now;
	}

	stats = get_lock_stats(hlock_class(hlock));
	if (waittime) {
		if (hlock->read)
			lock_time_inc(&stats->read_waittime, waittime);
		else
			lock_time_inc(&stats->write_waittime, waittime);
	}
	if (lock->cpu != cpu)
		stats->bounces[bounce_acquired + !!hlock->read]++;

	lock->cpu = cpu;
	lock->ip = ip;
}

/**
 * lock_contended - 对外记录锁竞争事件并进入 LOCK_STAT 内部统计
 * @lock: 发生竞争的 map
 * @ip: contention 点
 *
 * 始终先发 tracepoint；仅 lock_stat 开启且 lockdep 可进入时，在 IRQ/递归保护下调用
 * __lock_contended()。
 */
void lock_contended(struct lockdep_map *lock, unsigned long ip)
{
	/* flags 保存入口 IRQ 状态。 */
	unsigned long flags;

	trace_lock_contended(lock, ip);

	if (unlikely(!lock_stat || !lockdep_enabled()))
		return;

	raw_local_irq_save(flags);
	check_flags(flags);
	lockdep_recursion_inc();
	__lock_contended(lock, ip);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_contended);

/**
 * lock_acquired - 对外记录竞争后真正取得锁的事件
 * @lock: 已取得的 map
 * @ip: acquired 点
 *
 * 始终发 tracepoint；LOCK_STAT 运行时启用且 lockdep 可进入时，在 IRQ/递归保护下结算等待统计。
 */
void lock_acquired(struct lockdep_map *lock, unsigned long ip)
{
	/* flags 保存入口 IRQ 状态。 */
	unsigned long flags;

	trace_lock_acquired(lock, ip);

	if (unlikely(!lock_stat || !lockdep_enabled()))
		return;

	raw_local_irq_save(flags);
	check_flags(flags);
	lockdep_recursion_inc();
	__lock_acquired(lock, ip);
	lockdep_recursion_finish();
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_acquired);
#endif

/*
 * Used by the testsuite, sanitize the validator state
 * after a simulated failure:
 */
/* 测试套件在模拟失败后用它清理 validator 状态。 */

/**
 * lockdep_reset - 为自测重置当前任务与 dependency chain cache
 *
 * 在本地 IRQ 关闭期间重新初始化 current、清零完整 held-lock 数组和三类 chain 计数，重新启用
 * debug_locks，并把所有 chain hash 桶置空。它面向受控 testsuite，不是并发全局完全重置接口。
 */
void lockdep_reset(void)
{
	/* flags 保存入口 IRQ 状态，i 遍历 chain hash 桶。 */
	unsigned long flags;
	int i;

	raw_local_irq_save(flags);
	lockdep_init_task(current);
	memset(current->held_locks, 0, MAX_LOCK_DEPTH*sizeof(struct held_lock));
	nr_hardirq_chains = 0;
	nr_softirq_chains = 0;
	nr_process_chains = 0;
	debug_locks = 1;
	for (i = 0; i < CHAINHASH_SIZE; i++)
		INIT_HLIST_HEAD(chainhash_table + i);
	raw_local_irq_restore(flags);
}

/* Remove a class from a lock chain. Must be called with the graph lock held. */
/* 从一条 lock chain 移除指定 class；必须持有 graph lock。 */
/**
 * remove_class_from_lock_chain - 若 chain 包含 class，则回收整条 chain
 * @pf: 当前 pending-free 批次，记录等待 RCU 后复用的描述符
 * @chain: 要扫描的缓存 chain
 * @class: 正在注销的 class
 *
 * PROVE_LOCKING 下扫描压缩 class_idx；命中即归还 hlock 区、使 key 对 RCU reader 失效、从 hash
 * 摘除并登记描述符延迟释放。每条 chain 中同一 class 最多一次。其他配置为空操作。
 */
static void remove_class_from_lock_chain(struct pending_free *pf,
					 struct lock_chain *chain,
					 struct lock_class *class)
{
#ifdef CONFIG_PROVE_LOCKING
	/* i 遍历 chain_hlocks[base, base+depth)。 */
	int i;

	/* 只比较压缩项的 class_idx，忽略读模式位。 */
	for (i = chain->base; i < chain->base + chain->depth; i++) {
		if (chain_hlock_class_idx(chain_hlocks[i]) != class - lock_classes)
			continue;
		/*
		 * Each lock class occurs at most once in a lock chain so once
		 * we found a match we can break out of this loop.
		 */
		/* 同一 lock class 在合法 chain 最多出现一次，命中后无需继续扫描。 */
		goto free_lock_chain;
	}
	/* Since the chain has not been modified, return. */
	/* 未含目标 class，保持 chain 完全不变。 */
	return;

free_lock_chain:
	/* 压缩 ID 区先归还分桶分配器，并同步减少 context chain 统计。 */
	free_chain_hlocks(chain->base, chain->depth);
	/* Overwrite the chain key for concurrent RCU readers. */
	/* RCU reader 可能仍持 chain 指针；先用初始 key 让其不再把此项当作原缓存命中。 */
	WRITE_ONCE(chain->chain_key, INITIAL_CHAIN_KEY);
	dec_chains(chain->irq_context);

	/*
	 * Note: calling hlist_del_rcu() from inside a
	 * hlist_for_each_entry_rcu() loop is safe.
	 */
	/* hlist_del_rcu() 允许在外层 RCU 迭代中删除当前项。 */
	hlist_del_rcu(&chain->entry);
	__set_bit(chain - lock_chains, pf->lock_chains_being_freed);
	nr_zapped_lock_chains++;
#endif
}

/* Must be called with the graph lock held. */
/* 必须持有 graph lock。 */
/**
 * remove_class_from_lock_chains - 从所有 chain hash 桶清除含指定 class 的 chain
 * @pf: 当前 pending-free 批次
 * @class: 正在注销的 class
 *
 * 在 graph_lock 下遍历每个桶，并把每条 chain 交给单链 helper；RCU 删除允许当前迭代继续。
 */
static void remove_class_from_lock_chains(struct pending_free *pf,
					  struct lock_class *class)
{
	/* chain 是桶内游标，head 是当前桶，i 遍历全部 hash 槽。 */
	struct lock_chain *chain;
	struct hlist_head *head;
	int i;

	for (i = 0; i < ARRAY_SIZE(chainhash_table); i++) {
		head = chainhash_table + i;
		hlist_for_each_entry_rcu(chain, head, entry) {
			remove_class_from_lock_chain(pf, chain, class);
		}
	}
}

/*
 * Remove all references to a lock class. The caller must hold the graph lock.
 */
/* 删除对一个 lock class 的全部引用；调用者必须持有 graph lock。 */
/**
 * zap_class - 从依赖边、class 索引和 chain cache 中逻辑注销一个 class
 * @pf: 当前 RCU pending-free 批次
 * @class: key 当前非 NULL 的目标 class
 *
 * 先删除所有涉及该 class 的 lock_list 边；确认正反邻接表都空后，将 class 移到 zapped 列表、从
 * hash 删除、清 key/name 和 in-use 位并更新统计，最后移除所有包含它的 chain。内存复位延迟到 RCU。
 */
static void zap_class(struct pending_free *pf, struct lock_class *class)
{
	/* entry/i 扫描静态依赖边池。 */
	struct lock_list *entry;
	int i;

	WARN_ON_ONCE(!class->key);

	/*
	 * Remove all dependencies this lock is
	 * involved in:
	 */
	/* 删除 class 作为源或目标参与的全部依赖边，并归还对应静态槽。 */
	for_each_set_bit(i, list_entries_in_use, ARRAY_SIZE(list_entries)) {
		entry = list_entries + i;
		if (entry->class != class && entry->links_to != class)
			continue;
		__clear_bit(i, list_entries_in_use);
		nr_list_entries--;
		list_del_rcu(&entry->entry);
	}
	if (list_empty(&class->locks_after) &&
	    list_empty(&class->locks_before)) {
		list_move_tail(&class->lock_entry, &pf->zapped);
		hlist_del_rcu(&class->hash_entry);
		WRITE_ONCE(class->key, NULL);
		WRITE_ONCE(class->name, NULL);
		/* Class allocated but not used, -1 in nr_unused_locks */
		/* class 若从未设置 usage，注销时抵消注册阶段计入的 unused 统计。 */
		if (class->usage_mask == 0)
			debug_atomic_dec(nr_unused_locks);
		nr_lock_classes--;
		__clear_bit(class - lock_classes, lock_classes_in_use);
		if (class - lock_classes == max_lock_class_idx)
			max_lock_class_idx--;
	} else {
		/* 邻接边仍残留意味着边池/链表不一致，保留 class 并报告。 */
		WARN_ONCE(true, "%s() failed for class %s\n", __func__,
			  class->name);
	}

	remove_class_from_lock_chains(pf, class);
	nr_zapped_classes++;
}

/**
 * reinit_class - 在 RCU 宽限期后清零一个已 zapped class 的可复用字段
 * @class: 已从 active 索引移除且邻接表为空的 class
 *
 * 前后各做链表一致性 WARN；memset_startat(..., key) 保留用于 free/zapped 链表管理的前置字段，
 * 清零从 key 开始的身份、usage、trace 和统计状态。
 */
static void reinit_class(struct lock_class *class)
{
	/* 回收前 lock_entry 必须仍链接，正反依赖均已清空。 */
	WARN_ON_ONCE(!class->lock_entry.next);
	WARN_ON_ONCE(!list_empty(&class->locks_after));
	WARN_ON_ONCE(!list_empty(&class->locks_before));
	memset_startat(class, 0, key);
	WARN_ON_ONCE(!class->lock_entry.next);
	WARN_ON_ONCE(!list_empty(&class->locks_after));
	WARN_ON_ONCE(!list_empty(&class->locks_before));
}

/**
 * within - 判断地址是否落在一个半开内存区间
 * @addr: 待检查地址
 * @start: 区间起点
 * @size: 区间字节数
 *
 * 返回：start <= addr < start+size 为真。用于按模块地址同时匹配 class key 和 name。
 */
static inline int within(const void *addr, void *start, unsigned long size)
{
	/* 内核 GNU C 允许 void 指针按字节做区间加法。 */
	return addr >= start && addr < start + size;
}

/**
 * inside_selftest - 判断当前是否是 lockdep 指定的自测任务
 *
 * 返回：current 与 lockdep_selftest_task_struct 借用指针相同为 true。自测采用同步立即回收路径。
 */
static bool inside_selftest(void)
{
	/* 指针身份由 selftest 入口在受控生命周期内设置。 */
	return current == lockdep_selftest_task_struct;
}

/* The caller must hold the graph lock. */
/* 调用者必须持有 graph lock。 */
/**
 * get_pending_free - 取得当前开放的延迟回收缓冲区
 *
 * 返回：delayed_free.pf[index]；双缓冲 index 由 prepare_call_rcu_zapped() 在封口时翻转。
 */
static struct pending_free *get_pending_free(void)
{
	/* graph_lock 保证 index 与生产者写入同一视图。 */
	return delayed_free.pf + delayed_free.index;
}

/* RCU callback 前置声明，供 prepare 的调用者排队同一回调函数。 */
static void free_zapped_rcu(struct rcu_head *cb);

/*
* See if we need to queue an RCU callback, must called with
* the lockdep lock held, returns false if either we don't have
* any pending free or the callback is already scheduled.
* Otherwise, a call_rcu() must follow this function call.
*/
/*
 * 判断是否需要排队 RCU callback。必须持有 lockdep 图锁；没有 pending free 或 callback 已排队时
 * 返回 false，否则封闭当前缓冲并返回 true，调用者随后必须调用 call_rcu()。
 */
/**
 * prepare_call_rcu_zapped - 封闭当前 pending-free 批次并决定是否启动 RCU 宽限期
 * @pf: 调用者刚填充的当前开放缓冲
 *
 * 返回：true 表示 scheduled 已置位且 index 已翻到另一缓冲，调用者必须 call_rcu()；false 表示
 * 无待回收项或已有 callback 会串联处理。自测不应使用异步路径。
 */
static bool prepare_call_rcu_zapped(struct pending_free *pf)
{
	/* 自测要求立即释放，误入异步调度路径需告警。 */
	WARN_ON_ONCE(inside_selftest());

	/* 没有 zapped class 就无需宽限期。 */
	if (list_empty(&pf->zapped))
		return false;

	if (delayed_free.scheduled)
		return false;

	delayed_free.scheduled = true;

	WARN_ON_ONCE(delayed_free.pf + delayed_free.index != pf);
	delayed_free.index ^= 1;

	/* 调用者现在负责在解图锁后排队 callback。 */
	return true;
}

/* The caller must hold the graph lock. May be called from RCU context. */
/* 调用者必须持有 graph lock；可从 RCU callback context 调用。 */
/**
 * __free_zapped_classes - 完成一个已过宽限期批次的物理复位与池归还
 * @pf: 已封闭且 RCU reader 不再引用的 pending-free 批次
 *
 * 先核验全局结构，逐个 reinit class，再把 zapped 列表整体拼到 free_lock_classes。PROVE_LOCKING
 * 下同时清除延迟的 chain in-use 位，并重置批次 bitmap。
 */
static void __free_zapped_classes(struct pending_free *pf)
{
	/* class 遍历待回收列表，链表本身到最后统一 splice。 */
	struct lock_class *class;

	/* 宽限期后复用前再做一次池/链表一致性自检。 */
	check_data_structures();

	/* 清除每个 class 的身份与统计字段，但保留链表节点用于 splice。 */
	list_for_each_entry(class, &pf->zapped, lock_entry)
		reinit_class(class);

	list_splice_init(&pf->zapped, &free_lock_classes);

#ifdef CONFIG_PROVE_LOCKING
	/* 只有现在才允许被删除 chain 的描述符槽重新分配。 */
	bitmap_andnot(lock_chains_in_use, lock_chains_in_use,
		      pf->lock_chains_being_freed, ARRAY_SIZE(lock_chains));
	bitmap_clear(pf->lock_chains_being_freed, 0, ARRAY_SIZE(lock_chains));
#endif
}

/**
 * free_zapped_rcu - RCU 宽限期结束后回收封闭批次并串联下一批
 * @ch: 必须是 delayed_free.rcu_head
 *
 * 在本地 IRQ 关闭和 raw lockdep 图锁下，回收 index^1 的封闭缓冲、清 scheduled，再检查当前开放
 * 缓冲是否已积累新项。若需要下一轮，解锁后再次 call_rcu()。
 */
static void free_zapped_rcu(struct rcu_head *ch)
{
	/* pf 是本次封闭批次；flags 保存 IRQ；need_callback 决定串联下一宽限期。 */
	struct pending_free *pf;
	unsigned long flags;
	bool need_callback;

	if (WARN_ON_ONCE(ch != &delayed_free.rcu_head))
		return;

	raw_local_irq_save(flags);
	lockdep_lock();

	/* closed head */
	/* 当前 index 指向生产者开放缓冲，异或 1 得到触发本 callback 的封闭缓冲。 */
	pf = delayed_free.pf + (delayed_free.index ^ 1);
	__free_zapped_classes(pf);
	delayed_free.scheduled = false;
	need_callback =
		prepare_call_rcu_zapped(delayed_free.pf + delayed_free.index);
	lockdep_unlock();
	raw_local_irq_restore(flags);

	/*
	* If there's pending free and its callback has not been scheduled,
	* queue an RCU callback.
	*/
	/* 若开放批次已被 prepare 封口且尚未排队，现在串联同一 callback。 */
	if (need_callback)
		call_rcu(&delayed_free.rcu_head, free_zapped_rcu);

}

/*
 * Remove all lock classes from the class hash table and from the
 * all_lock_classes list whose key or name is in the address range [start,
 * start + size). Move these lock classes to the zapped_classes list. Must
 * be called with the graph lock held.
 */
/*
 * 从 class hash 与 all_lock_classes 中移除 key 或 name 落在 [start,start+size) 的全部 class，并把
 * 它们移入 zapped 列表。必须持有 graph lock。
 */
/**
 * __lockdep_free_key_range - 逻辑注销归属于指定地址区间的所有 lock class
 * @pf: 当前 pending-free 批次
 * @start: 待释放模块/对象地址起点
 * @size: 半开区间长度
 *
 * 扫描全部 class hash 桶；key 和 name 任一位于区间即 zap_class()，从而连带清依赖与 chain。
 */
static void __lockdep_free_key_range(struct pending_free *pf, void *start,
				     unsigned long size)
{
	/* class/head 是 RCU hash 游标，i 遍历桶。 */
	struct lock_class *class;
	struct hlist_head *head;
	int i;

	/* Unhash all classes that were created by a module. */
	/* 模块创建的 class 可能按任意 key hash 分布，必须扫描整表。 */
	for (i = 0; i < CLASSHASH_SIZE; i++) {
		head = classhash_table + i;
		hlist_for_each_entry_rcu(class, head, hash_entry) {
			if (!within(class->key, start, size) &&
			    !within(class->name, start, size))
				continue;
			zap_class(pf, class);
		}
	}
}

/*
 * Used in module.c to remove lock classes from memory that is going to be
 * freed; and possibly re-used by other modules.
 *
 * We will have had one synchronize_rcu() before getting here, so we're
 * guaranteed nobody will look up these exact classes -- they're properly dead
 * but still allocated.
 */
/*
 * module.c 在模块内存即将释放并可能被其他模块复用前调用。到达这里以前已有一次 synchronize_rcu()，
 * 因而不会再有新 lookup 命中这些确已死亡但描述符仍分配的 class。
 */
/**
 * lockdep_free_key_range_reg - 常规模块卸载的异步双缓冲回收路径
 * @start: 即将释放的地址区间起点
 * @size: 区间长度
 *
 * 在图锁下把匹配 class 加入当前 pending 批次并按需排队 RCU callback；随后再 synchronize_rcu()，
 * 等待已经在 look_up_lock_class() 中持有旧 key/name 指针的迭代者退出。本函数会睡眠。
 */
static void lockdep_free_key_range_reg(void *start, unsigned long size)
{
	/* pf 是当前生产批次；flags 保护本 CPU；need_callback 延后到解锁后处理。 */
	struct pending_free *pf;
	unsigned long flags;
	bool need_callback;

	init_data_structures_once();

	raw_local_irq_save(flags);
	lockdep_lock();
	pf = get_pending_free();
	__lockdep_free_key_range(pf, start, size);
	need_callback = prepare_call_rcu_zapped(pf);
	lockdep_unlock();
	raw_local_irq_restore(flags);
	if (need_callback)
		call_rcu(&delayed_free.rcu_head, free_zapped_rcu);
	/*
	 * Wait for any possible iterators from look_up_lock_class() to pass
	 * before continuing to free the memory they refer to.
	 */
	/* 等所有可能仍在 class lookup 中引用模块 key/name 的 reader 离开后，调用方才可继续释放内存。 */
	synchronize_rcu();
}

/*
 * Free all lockdep keys in the range [start, start+size). Does not sleep.
 * Ignores debug_locks. Must only be used by the lockdep selftests.
 */
/* 立即释放 [start,start+size) 内全部 lockdep key；不睡眠且忽略 debug_locks，仅供 selftest。 */
/**
 * lockdep_free_key_range_imm - 自测环境中同步注销并立即复用 class
 * @start: 自测对象区间起点
 * @size: 区间长度
 *
 * 在 IRQ 关闭和图锁内使用固定 pending 缓冲，逻辑 zap 后直接 __free_zapped_classes()，不经过 RCU
 * callback；只因 selftest 已保证无并发 reader 才安全。
 */
static void lockdep_free_key_range_imm(void *start, unsigned long size)
{
	/* 自测直接借双缓冲首项，flags 保存 IRQ。 */
	struct pending_free *pf = delayed_free.pf;
	unsigned long flags;

	init_data_structures_once();

	raw_local_irq_save(flags);
	lockdep_lock();
	__lockdep_free_key_range(pf, start, size);
	__free_zapped_classes(pf);
	lockdep_unlock();
	raw_local_irq_restore(flags);
}

/**
 * lockdep_free_key_range - 按当前是否 selftest 分派 key 地址区间回收策略
 * @start: 待释放地址起点
 * @size: 半开区间长度
 *
 * 自测走不睡眠的立即路径，正常模块生命周期走 RCU 延迟路径；入口确保一次性数据结构已初始化。
 */
void lockdep_free_key_range(void *start, unsigned long size)
{
	/* 两种分支都依赖 class hash/free list/pending 缓冲。 */
	init_data_structures_once();

	if (inside_selftest())
		lockdep_free_key_range_imm(start, size);
	else
		lockdep_free_key_range_reg(start, size);
}

/*
 * Check whether any element of the @lock->class_cache[] array refers to a
 * registered lock class. The caller must hold either the graph lock or the
 * RCU read lock.
 */
/*
 * 检查 @lock->class_cache[] 是否有任一元素仍指向已注册 class。调用者必须持 graph lock 或 RCU
 * read lock。
 */
/**
 * lock_class_cache_is_registered - 验证 map cache 中是否仍引用活动 class
 * @lock: 要检查全部快速 cache 槽的 map
 *
 * 返回：任一 cache 指针与当前 class hash 中的活动项相同为 true，否则 false。全表扫描避免信任
 * 可能已经成为 stale pointer 的 cache 内容。
 */
static bool lock_class_cache_is_registered(struct lockdep_map *lock)
{
	/* class/head 遍历注册 hash，i/j 分别是桶和 map cache 下标。 */
	struct lock_class *class;
	struct hlist_head *head;
	int i, j;

	for (i = 0; i < CLASSHASH_SIZE; i++) {
		head = classhash_table + i;
		hlist_for_each_entry_rcu(class, head, hash_entry) {
			for (j = 0; j < NR_LOCKDEP_CACHING_CLASSES; j++)
				if (lock->class_cache[j] == class)
					return true;
		}
	}
	/* 没有 cache 槽仍对应活动 class。 */
	return false;
}

/* The caller must hold the graph lock. Does not sleep. */
/* 调用者必须持有 graph lock；函数不睡眠。 */
/**
 * __lockdep_reset_lock - 注销一个 map 的全部 subclass class
 * @pf: 当前 pending-free 批次
 * @lock: 要清除 lockdep 全部已知信息的 map
 *
 * 扫描全部 MAX_LOCKDEP_SUBCLASSES，查到 class 就 zap；完成后确认 map 的快速 cache 不再指向任何
 * 已注册 class，违反则关闭 lockdep。
 */
static void __lockdep_reset_lock(struct pending_free *pf,
				 struct lockdep_map *lock)
{
	/* class 保存每个 subclass 查询结果，j 遍历完整上限而非仅 cache 范围。 */
	struct lock_class *class;
	int j;

	/*
	 * Remove all classes this lock might have:
	 */
	/* 同一 map 可能按不同 subclass 注册多个独立 class，必须全部移除。 */
	for (j = 0; j < MAX_LOCKDEP_SUBCLASSES; j++) {
		/*
		 * If the class exists we look it up and zap it:
		 */
		/* 未注册 subclass 无需动作；活动项进入本批 RCU zapped 列表。 */
		class = look_up_lock_class(lock, j);
		if (class)
			zap_class(pf, class);
	}
	/*
	 * Debug check: in the end all mapped classes should
	 * be gone.
	 */
	/* reset 后 cache 即使保留旧地址，也绝不能再命中活动 class。 */
	if (WARN_ON_ONCE(lock_class_cache_is_registered(lock)))
		debug_locks_off();
}

/*
 * Remove all information lockdep has about a lock if debug_locks == 1. Free
 * released data structures from RCU context.
 */
/* debug_locks==1 时移除 map 的全部 lockdep 信息，并从 RCU context 释放已注销结构。 */
/**
 * lockdep_reset_lock_reg - 生产环境中按 RCU 延迟回收一个 map 的 class
 * @lock: 要重置的 map
 *
 * 关闭本地 IRQ后尝试 graph_lock；成功则 zap 全部 subclass 并封闭 pending 批次，解锁后按需
 * call_rcu()。debug_locks 已关闭导致取锁失败时只恢复 IRQ，不做回收。
 */
static void lockdep_reset_lock_reg(struct lockdep_map *lock)
{
	/* pf 是开放批次；flags 保存 IRQ；locked/need_callback 跟踪两阶段状态。 */
	struct pending_free *pf;
	unsigned long flags;
	int locked;
	bool need_callback = false;

	raw_local_irq_save(flags);
	locked = graph_lock();
	if (!locked)
		goto out_irq;

	pf = get_pending_free();
	__lockdep_reset_lock(pf, lock);
	need_callback = prepare_call_rcu_zapped(pf);

	graph_unlock();
out_irq:
	raw_local_irq_restore(flags);
	if (need_callback)
		call_rcu(&delayed_free.rcu_head, free_zapped_rcu);
}

/*
 * Reset a lock. Does not sleep. Ignores debug_locks. Must only be used by the
 * lockdep selftests.
 */
/* 重置一个 lock；不睡眠且忽略 debug_locks，仅允许 lockdep selftest 使用。 */
/**
 * lockdep_reset_lock_imm - 自测环境同步注销并立即复用 map 的 class
 * @lock: 要重置的 map
 *
 * 在 raw lockdep 图锁内用固定 pending 缓冲完成 zap 与 __free_zapped_classes()，不等待 RCU。
 */
static void lockdep_reset_lock_imm(struct lockdep_map *lock)
{
	/* 自测固定使用首个 pending 缓冲，flags 保存 IRQ。 */
	struct pending_free *pf = delayed_free.pf;
	unsigned long flags;

	raw_local_irq_save(flags);
	lockdep_lock();
	__lockdep_reset_lock(pf, lock);
	__free_zapped_classes(pf);
	lockdep_unlock();
	raw_local_irq_restore(flags);
}

/**
 * lockdep_reset_lock - 按 selftest/生产环境分派单 map 重置策略
 * @lock: 要移除全部 class 与依赖的 map
 *
 * 入口确保全局池已初始化；自测立即回收，正常调用采用 RCU 延迟回收。
 */
void lockdep_reset_lock(struct lockdep_map *lock)
{
	/* 两条路径都依赖 pending/free 全局结构。 */
	init_data_structures_once();

	if (inside_selftest())
		lockdep_reset_lock_imm(lock);
	else
		lockdep_reset_lock_reg(lock);
}

/*
 * Unregister a dynamically allocated key.
 *
 * Unlike lockdep_register_key(), a search is always done to find a matching
 * key irrespective of debug_locks to avoid potential invalid access to freed
 * memory in lock_class entry.
 */
/*
 * 注销动态分配 key。与 lockdep_register_key() 不同，无论 debug_locks 状态如何都必须搜索匹配项，
 * 避免 lock_class 继续保留指向即将释放 key 内存的无效引用。
 */
/**
 * lockdep_unregister_key - 从 dynamic-key hash 注销 key 并延迟回收关联 class
 * @key: 先前注册的动态 key；静态对象非法
 *
 * 本函数可睡眠。图锁内从 key hash 删除精确项，逻辑注销 key 地址对应 class、按需封闭 RCU 批次并
 * 减少动态 key 统计；解锁后排 callback，最后 expedited grace period 等 is_dynamic_key() reader。
 */
void lockdep_unregister_key(struct lock_class_key *key)
{
	/* hash_head 是目标 key 桶，k 是 RCU 游标，pf 是回收批次。 */
	struct hlist_head *hash_head = keyhashentry(key);
	struct lock_class_key *k;
	struct pending_free *pf;
	unsigned long flags;
	bool found = false;
	bool need_callback = false;

	might_sleep();

	if (WARN_ON_ONCE(static_obj(key)))
		return;

	raw_local_irq_save(flags);
	lockdep_lock();

	hlist_for_each_entry_rcu(k, hash_head, hash_entry) {
		if (k == key) {
			hlist_del_rcu(&k->hash_entry);
			found = true;
			break;
		}
	}
	WARN_ON_ONCE(!found && debug_locks);
	if (found) {
		pf = get_pending_free();
		__lockdep_free_key_range(pf, key, 1);
		need_callback = prepare_call_rcu_zapped(pf);
		nr_dynamic_keys--;
	}
	lockdep_unlock();
	raw_local_irq_restore(flags);

	if (need_callback)
		call_rcu(&delayed_free.rcu_head, free_zapped_rcu);

	/*
	 * Wait until is_dynamic_key() has finished accessing k->hash_entry.
	 *
	 * Some operations like __qdisc_destroy() will call this in a debug
	 * kernel, and the network traffic is disabled while waiting, hence
	 * the delay of the wait matters in debugging cases. Currently use a
	 * synchronize_rcu_expedited() to speed up the wait at the cost of
	 * system IPIs. TODO: Replace RCU with hazptr for this.
	 */
	/*
	 * 等 is_dynamic_key() 不再访问 k->hash_entry。__qdisc_destroy() 等调试内核路径会在网络流量停用时
	 * 等待，延迟很敏感，所以当前用 expedited RCU，以系统 IPI 成本换取更短等待；未来拟用 hazptr。
	 */
	synchronize_rcu_expedited();
}
EXPORT_SYMBOL_GPL(lockdep_unregister_key);

/**
 * lockdep_init - 启动时打印 lockdep 静态容量与内存占用
 *
 * 输出 subclass/depth/key/各 hash 与 pool 上限，汇总全局依赖结构、可选 PROVE_LOCKING BFS/chain
 * 存储、可选 IRQ trace 存储，以及每个 task_struct 的 held_locks 字节数。仅 __init 阶段调用。
 */
void __init lockdep_init(void)
{
	/* 先给出 validator 身份和所有会导致运行时容量关闭的主要编译上限。 */
	pr_info("Lock dependency validator: Copyright (c) 2006 Red Hat, Inc., Ingo Molnar\n");

	pr_info("... MAX_LOCKDEP_SUBCLASSES:  %lu\n", MAX_LOCKDEP_SUBCLASSES);
	pr_info("... MAX_LOCK_DEPTH:          %lu\n", MAX_LOCK_DEPTH);
	pr_info("... MAX_LOCKDEP_KEYS:        %lu\n", MAX_LOCKDEP_KEYS);
	pr_info("... CLASSHASH_SIZE:          %lu\n", CLASSHASH_SIZE);
	pr_info("... MAX_LOCKDEP_ENTRIES:     %lu\n", MAX_LOCKDEP_ENTRIES);
	pr_info("... MAX_LOCKDEP_CHAINS:      %lu\n", MAX_LOCKDEP_CHAINS);
	pr_info("... CHAINHASH_SIZE:          %lu\n", CHAINHASH_SIZE);

	pr_info(" memory used by lock dependency info: %zu kB\n",
	       (sizeof(lock_classes) +
		sizeof(lock_classes_in_use) +
		sizeof(classhash_table) +
		sizeof(list_entries) +
		sizeof(list_entries_in_use) +
		sizeof(chainhash_table) +
		sizeof(delayed_free)
#ifdef CONFIG_PROVE_LOCKING
		/* 依赖证明构建额外包含 BFS 队列、chain 描述符/bitmap 和压缩 ID 池。 */
		+ sizeof(lock_cq)
		+ sizeof(lock_chains)
		+ sizeof(lock_chains_in_use)
		+ sizeof(chain_hlocks)
#endif
		) / 1024
		);

#if defined(CONFIG_TRACE_IRQFLAGS) && defined(CONFIG_PROVE_LOCKING)
	/* 两项功能同时开启时另报 usage stack trace 池及其 hash。 */
	pr_info(" memory used for stack traces: %zu kB\n",
	       (sizeof(stack_trace) + sizeof(stack_trace_hash)) / 1024
	       );
#endif

	pr_info(" per task-struct memory footprint: %zu bytes\n",
	       sizeof(((struct task_struct *)NULL)->held_locks));
}

/**
 * print_freed_lock_bug - 报告正在释放的内存范围仍包含当前持有 lock map
 * @curr: 执行内存释放的当前任务
 * @mem_from: 释放区间起点
 * @mem_to: 释放区间尾后地址
 * @hlock: instance 与该区间重叠的 held lock
 *
 * 首次报告关闭 lockdep；非 silent 时打印半开范围对应的含尾地址、冲突锁、全部持锁与栈回溯。
 */
static void
print_freed_lock_bug(struct task_struct *curr, const void *mem_from,
		     const void *mem_to, struct held_lock *hlock)
{
	/* 只让首个发现者输出。 */
	if (!debug_locks_off())
		return;
	if (debug_locks_silent)
		return;

	nbcon_cpu_emergency_enter();

	pr_warn("\n");
	pr_warn("=========================\n");
	pr_warn("WARNING: held lock freed!\n");
	print_kernel_ident();
	pr_warn("-------------------------\n");
	pr_warn("%s/%d is freeing memory %px-%px, with a lock still held there!\n",
		curr->comm, task_pid_nr(curr), mem_from, mem_to-1);
	print_lock(hlock);
	lockdep_print_held_locks(curr);

	pr_warn("\nstack backtrace:\n");
	dump_stack();

	/* 完成报告。 */
	nbcon_cpu_emergency_exit();
}

/**
 * not_in_range - 判断两个半开内存区间是否完全不重叠
 * @mem_from: 第一区间起点
 * @mem_len: 第一区间长度
 * @lock_from: 第二区间起点
 * @lock_len: 第二区间长度
 *
 * 返回：任一区间尾地址不大于另一区间起点时为 true；相邻但不重叠也返回 true。
 */
static inline int not_in_range(const void* mem_from, unsigned long mem_len,
				const void* lock_from, unsigned long lock_len)
{
	/* 两个方向的“完全位于之前”条件覆盖所有不相交排列。 */
	return lock_from + lock_len <= mem_from ||
		mem_from + mem_len <= lock_from;
}

/*
 * Called when kernel memory is freed (or unmapped), or if a lock
 * is destroyed or reinitialized - this code checks whether there is
 * any held lock in the memory range of <from> to <to>:
 */
/*
 * 内核内存被释放/解除映射，或锁被销毁/重新初始化时调用；检查 [from,to) 范围内是否仍有 held lock。
 */
/**
 * debug_check_no_locks_freed - 防止释放包含当前任务仍持有 lockdep_map 的内存
 * @mem_from: 待释放半开区间起点
 * @mem_len: 区间字节数
 *
 * 在本地 IRQ 关闭下扫描 current held stack，以每个 hlock->instance 的完整 map 大小做重叠判断；
 * 首次命中即报告并停止。仅检查当前任务的持有关系。
 */
void debug_check_no_locks_freed(const void *mem_from, unsigned long mem_len)
{
	/* curr/hlock 是扫描对象，flags 保护任务栈，i 遍历深度。 */
	struct task_struct *curr = current;
	struct held_lock *hlock;
	unsigned long flags;
	int i;

	/* 调试关闭后无需做生命周期诊断。 */
	if (unlikely(!debug_locks))
		return;

	/* 稳定本 CPU 的 held stack，逐项检查 map 对象区间。 */
	raw_local_irq_save(flags);
	for (i = 0; i < curr->lockdep_depth; i++) {
		hlock = curr->held_locks + i;

		/* 当前 held-lock 的 map 与释放区间不重叠则继续。 */
		if (not_in_range(mem_from, mem_len, hlock->instance,
					sizeof(*hlock->instance)))
			continue;

		/* 第一处重叠足以关闭 lockdep 并给出完整报告。 */
		print_freed_lock_bug(curr, mem_from, mem_from + mem_len, hlock);
		break;
	}
	/* 恢复入口 IRQ 状态。 */
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(debug_check_no_locks_freed);

/**
 * print_held_locks_bug - 报告当前任务在要求空栈的检查点仍持锁
 *
 * 首次报告关闭 lockdep；非 silent 时打印任务身份、全部 held locks 与当前栈回溯。
 */
static void print_held_locks_bug(void)
{
	/* 只由首个关闭者输出。 */
	if (!debug_locks_off())
		return;
	if (debug_locks_silent)
		return;

	nbcon_cpu_emergency_enter();

	pr_warn("\n");
	pr_warn("====================================\n");
	pr_warn("WARNING: %s/%d still has locks held!\n",
	       current->comm, task_pid_nr(current));
	print_kernel_ident();
	pr_warn("------------------------------------\n");
	lockdep_print_held_locks(current);
	pr_warn("\nstack backtrace:\n");
	dump_stack();

	/* 报告结束。 */
	nbcon_cpu_emergency_exit();
}

/**
 * debug_check_no_locks_held - 断言当前任务 held-lock 栈为空
 *
 * lockdep_depth 大于 0 时进入终止报告；空栈无副作用。常用于任务/路径生命周期边界。
 */
void debug_check_no_locks_held(void)
{
	/* unlikely 保持正常空栈检查的快速路径。 */
	if (unlikely(current->lockdep_depth > 0))
		print_held_locks_bug();
}
EXPORT_SYMBOL_GPL(debug_check_no_locks_held);

#ifdef __KERNEL__
/**
 * debug_show_all_locks - 打印系统所有任务当前记录的 held locks
 *
 * lockdep 已关闭时只输出提示。否则在 RCU read-side 临界区遍历所有进程线程，跳过空栈任务，并在
 * 长扫描中触碰 NMI/softlockup watchdog；最后打印分隔尾。
 */
void debug_show_all_locks(void)
{
	/* g/p 是 for_each_process_thread() 的进程与线程游标。 */
	struct task_struct *g, *p;

	if (unlikely(!debug_locks)) {
		pr_warn("INFO: lockdep is turned off.\n");
		return;
	}
	pr_warn("\nShowing all locks held in the system:\n");

	rcu_read_lock();
	for_each_process_thread(g, p) {
		if (!p->lockdep_depth)
			continue;
		lockdep_print_held_locks(p);
		touch_nmi_watchdog();
		touch_all_softlockup_watchdogs();
	}
	rcu_read_unlock();

	pr_warn("\n");
	pr_warn("=============================================\n\n");
}
EXPORT_SYMBOL_GPL(debug_show_all_locks);
#endif

/*
 * Careful: only use this function if you are sure that
 * the task cannot run in parallel!
 */
/* 小心：只有能保证目标任务不会并行运行时才可调用。 */
/**
 * debug_show_held_locks - 打印一个静止任务的 held-lock 栈
 * @task: 调用者保证不会并行修改 lockdep 栈的目标任务
 *
 * lockdep 关闭时只打印提示，否则直接调用 lockdep_print_held_locks()；函数自身不提供任务同步。
 */
void debug_show_held_locks(struct task_struct *task)
{
	/* debug_locks 关闭意味着栈可能已不完整，避免伪装成可靠快照。 */
	if (unlikely(!debug_locks)) {
		printk("INFO: lockdep is turned off.\n");
		return;
	}
	lockdep_print_held_locks(task);
}
EXPORT_SYMBOL_GPL(debug_show_held_locks);

/**
 * lockdep_sys_exit - 在返回用户态时检查锁泄漏并重置 syscall 间 invariant 历史
 *
 * 若 current 仍有 held locks，首次发现会关闭 lockdep 并在紧急控制台打印任务及持锁列表。无论
 * 是否泄漏，最后都以 false 重置 invariant state，使每个 syscall 的锁历史彼此独立。
 */
asmlinkage __visible void lockdep_sys_exit(void)
{
	/* curr 是正在离开内核的任务。 */
	struct task_struct *curr = current;

	if (unlikely(curr->lockdep_depth)) {
		if (!debug_locks_off())
			return;
		nbcon_cpu_emergency_enter();
		pr_warn("\n");
		pr_warn("================================================\n");
		pr_warn("WARNING: lock held when returning to user space!\n");
		print_kernel_ident();
		pr_warn("------------------------------------------------\n");
		pr_warn("%s/%d is leaving the kernel with locks still held!\n",
				curr->comm, curr->pid);
		lockdep_print_held_locks(curr);
		nbcon_cpu_emergency_exit();
	}

	/*
	 * The lock history for each syscall should be independent. So wipe the
	 * slate clean on return to userspace.
	 */
	/* 每个 syscall 的 lock history 应独立，返回用户态时清空上一调用的 invariant 账本。 */
	lockdep_invariant_state(false);
}

/**
 * lockdep_rcu_suspicious - 报告一次不满足 RCU 使用条件的可疑操作
 * @file: 检测点源码文件
 * @line: 检测点行号
 * @s: 违规情形描述
 *
 * 保存 debug_locks 快照并通过 warn_rcu_enter()/exit() 建立报告所需 RCU 状态；在紧急控制台中输出
 * 位置、CPU online/RCU scheduler/debug 状态、extended quiescent state、当前持锁和栈回溯。
 */
void lockdep_rcu_suspicious(const char *file, const int line, const char *s)
{
	/* curr 用于持锁报告；dl 保留入口状态；rcu 是 warn_rcu_enter() 的配对令牌。 */
	struct task_struct *curr = current;
	int dl = READ_ONCE(debug_locks);
	bool rcu = warn_rcu_enter();

	/* Note: the following can be executed concurrently, so be careful. */
	/* 下列报告可能由多个 CPU 并发执行，不能依赖独占的全局打印状态。 */
	/* 紧急控制台保证 RCU/锁异常上下文中的输出进度。 */
	nbcon_cpu_emergency_enter();
	pr_warn("\n");
	pr_warn("=============================\n");
	pr_warn("WARNING: suspicious RCU usage\n");
	print_kernel_ident();
	pr_warn("-----------------------------\n");
	pr_warn("%s:%d %s!\n", file, line, s);
	pr_warn("\nother info that might help us debug this:\n\n");
	/* 同时指出 offline CPU 的非法使用，以及 lockdep 已关闭可能造成的假阳性。 */
	pr_warn("\n%srcu_scheduler_active = %d, debug_locks = %d\n%s",
	       !rcu_lockdep_current_cpu_online()
			? "RCU used illegally from offline CPU!\n"
			: "",
	       rcu_scheduler_active, dl,
	       dl ? "" : "Possible false positive due to lockdep disabling via debug_locks = 0\n");

	/*
	 * If a CPU is in the RCU-free window in idle (ie: in the section
	 * between ct_idle_enter() and ct_idle_exit(), then RCU
	 * considers that CPU to be in an "extended quiescent state",
	 * which means that RCU will be completely ignoring that CPU.
	 * Therefore, rcu_read_lock() and friends have absolutely no
	 * effect on a CPU running in that state. In other words, even if
	 * such an RCU-idle CPU has called rcu_read_lock(), RCU might well
	 * delete data structures out from under it.  RCU really has no
	 * choice here: we need to keep an RCU-free window in idle where
	 * the CPU may possibly enter into low power mode. This way we can
	 * notice an extended quiescent state to other CPUs that started a grace
	 * period. Otherwise we would delay any grace period as long as we run
	 * in the idle task.
	 *
	 * So complain bitterly if someone does call rcu_read_lock(),
	 * rcu_read_lock_bh() and so on from extended quiescent states.
	 */
	/*
	 * CPU 位于 idle 的 RCU-free 窗口（ct_idle_enter() 到 ct_idle_exit()）时，RCU 把它视为 extended
	 * quiescent state 并完全忽略。此时 rcu_read_lock() 等没有保护作用；即便 idle CPU 调用了它，RCU
	 * 仍可能在其脚下删除数据结构。RCU 必须保留这个可进入低功耗的无 RCU 窗口，才能向其他开始
	 * grace period 的 CPU 报告扩展静止状态，否则 idle task 会无限拖延宽限期。因此，在 extended
	 * quiescent state 调用 rcu_read_lock()/rcu_read_lock_bh() 等必须强烈告警。
	 */
	/* rcu_is_watching()==false 精确表明当前 CPU 正落在上述无保护窗口。 */
	if (!rcu_is_watching())
		pr_warn("RCU used illegally from extended quiescent state!\n");

	/* 当前 held locks 和调用栈帮助关联哪条锁路径违反 RCU 约束。 */
	lockdep_print_held_locks(curr);
	pr_warn("\nstack backtrace:\n");
	dump_stack();
	/* 先结束控制台紧急区，再用入口令牌恢复 warn RCU 状态。 */
	nbcon_cpu_emergency_exit();
	warn_rcu_exit(rcu);
}
EXPORT_SYMBOL_GPL(lockdep_rcu_suspicious);
