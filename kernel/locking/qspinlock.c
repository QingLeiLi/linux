// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Queued spinlock
 *
 * (C) Copyright 2013-2015 Hewlett-Packard Development Company, L.P.
 * (C) Copyright 2013-2014,2018 Red Hat, Inc.
 * (C) Copyright 2015 Intel Corp.
 * (C) Copyright 2015 Hewlett-Packard Enterprise Development LP
 *
 * Authors: Waiman Long <longman@redhat.com>
 *          Peter Zijlstra <peterz@infradead.org>
 */
/*
 * 排队自旋锁实现
 *
 * 本文件用一个 32 位锁字承载 locked、pending 和压缩队尾，并在竞争严重时借助
 * percpu MCS 节点排队。启用半虚拟化后，同一核心慢路径会通过受控自包含分别
 * 生成原生版本和 PV 版本；因此顶部一次性实体必须受 _GEN_PV_LOCK_SLOWPATH
 * 保护，而核心函数本身必须位于保护区之外。
 */

#ifndef _GEN_PV_LOCK_SLOWPATH

/*
 * 首次包含才引入公共依赖并定义静态存储。调用路径需要 SMP/percpu/中断上下文
 * 接口、原子锁字布局、预取和锁争用 trace；第二次 PV 生成复用首次包含的声明，
 * 避免重复定义 qnodes 等实体。
 */
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/mutex.h>
#include <linux/prefetch.h>
#include <asm/byteorder.h>
#include <asm/qspinlock.h>
#include <trace/events/lock.h>

/*
 * Include queued spinlock definitions and statistics code
 */
/* 引入排队锁共享定义与统计代码；二者分别提供状态 helper 和低开销事件更新。 */
#include "qspinlock.h"
#include "qspinlock_stat.h"

/*
 * The basic principle of a queue-based spinlock can best be understood
 * by studying a classic queue-based spinlock implementation called the
 * MCS lock. A copy of the original MCS lock paper ("Algorithms for Scalable
 * Synchronization on Shared-Memory Multiprocessors by Mellor-Crummey and
 * Scott") is available at
 *
 * https://bugzilla.kernel.org/show_bug.cgi?id=206115
 *
 * This queued spinlock implementation is based on the MCS lock, however to
 * make it fit the 4 bytes we assume spinlock_t to be, and preserve its
 * existing API, we must modify it somehow.
 *
 * In particular; where the traditional MCS lock consists of a tail pointer
 * (8 bytes) and needs the next pointer (another 8 bytes) of its own node to
 * unlock the next pending (next->locked), we compress both these: {tail,
 * next->locked} into a single u32 value.
 *
 * Since a spinlock disables recursion of its own context and there is a limit
 * to the contexts that can nest; namely: task, softirq, hardirq, nmi. As there
 * are at most 4 nesting levels, it can be encoded by a 2-bit number. Now
 * we can encode the tail by combining the 2-bit nesting level with the cpu
 * number. With one byte for the lock value and 3 bytes for the tail, only a
 * 32-bit word is now needed. Even though we only need 1 bit for the lock,
 * we extend it to a full byte to achieve better performance for architectures
 * that support atomic byte write.
 *
 * We also change the first spinner to spin on the lock bit instead of its
 * node; whereby avoiding the need to carry a node from lock to unlock, and
 * preserving existing lock API. This also makes the unlock code simpler and
 * faster.
 *
 * N.B. The current implementation only supports architectures that allow
 *      atomic operations on smaller 8-bit and 16-bit data types.
 *
 */
/*
 * 理解排队自旋锁的基本原理，最好先研究经典的 MCS 排队锁。原始 MCS 论文
 * “Algorithms for Scalable Synchronization on Shared-Memory Multiprocessors”
 * 可从上方链接获取。
 *
 * 本实现以 MCS 为基础，但为了保持 spinlock_t 既有 API 并装进约定的 4 字节，
 * 必须压缩其表示。传统 MCS 锁需要 8 字节尾指针，持有者还要携带自己节点的
 * 8 字节 next 指针，才能解锁后继的 next->locked；qspinlock 把 tail 与队头
 * 交接状态压进同一个 u32 锁字。
 *
 * 自旋锁会阻止同一上下文递归，而可嵌套上下文最多为 task、softirq、hardirq、
 * NMI 四层，所以节点索引可用 2 位编码，再与 CPU 号组合成队尾。锁值占一字节、
 * 队尾占三字节，总计只需 32 位；locked 实际只需一位，但扩成完整字节可让支持
 * 原子 byte 写的体系结构获得更好性能。
 *
 * 首个排队者改为轮询共享 locked 位而非自己的节点，从而不必把节点从 lock API
 * 携带到 unlock API，也让解锁保持简单快速。当前算法因此要求体系结构支持对
 * 较小的 8 位和 16 位数据作符合协议的原子访问；具体锁字布局见通用类型头。
 */

#include "mcs_spinlock.h"

/*
 * Per-CPU queue node structures; we can never have more than 4 nested
 * contexts: task, softirq, hardirq, nmi.
 *
 * Exactly fits one 64-byte cacheline on a 64-bit architecture.
 *
 * PV doubles the storage and uses the second cacheline for PV state.
 */
/*
 * 每 CPU 排队节点；同一 CPU 最多只有 task、softirq、hardirq、NMI 四种嵌套
 * 上下文。64 位原生构建的四节点恰好占一个 64 字节 cacheline；PV 把单节点
 * 扩大一倍，并用第二个 cacheline 保存 PV 状态。
 *
 * 数组具有静态/percpu 生命周期并按 cacheline 对齐。首节点 mcs.count 充当该
 * CPU 的嵌套深度分配器，其余槽由索引定位；慢路径只借用节点，退出时归还槽，
 * 不进行动态分配。
 */
static DEFINE_PER_CPU_ALIGNED(struct qnode, qnodes[_Q_MAX_NODES]);

/*
 * Generate the native code for queued_spin_unlock_slowpath(); provide NOPs for
 * all the PV callbacks.
 */
/*
 * 为 queued_spin_lock_slowpath() 生成原生代码：四个 PV 回调先映射为空操作，
 * 队头等待回调返回 0，表示调用者继续执行原生原子等待。参数均为借用指针，
 * stub 不访问对象、不改变 ownership，也不会睡眠或失败。
 * 原文称 queued_spin_unlock_slowpath()，但紧随其后的 callback 和宏重命名实际
 * 服务于 lock slowpath；这里保留原文并明确当前代码行为。
 */

/*
 * __pv_init_node() - 原生构建的节点初始化适配器
 * @node: 已由公共慢路径初始化的借用节点；原生模式无需附加 PV 状态。
 *
 * 无返回、无副作用；后续仍由公共路径发布并使用该节点。
 */
static __always_inline void __pv_init_node(struct mcs_spinlock *node) { }
/*
 * __pv_wait_node() - 原生构建的前驱等待前置适配器
 * @node: 当前等待者的有效借用节点。
 * @prev: 已解码的前驱借用节点。
 *
 * 原生模式不在这里休眠；返回后公共路径以 MCS acquire 自旋等待 @node。
 */
static __always_inline void __pv_wait_node(struct mcs_spinlock *node,
					   struct mcs_spinlock *prev) { }
/*
 * __pv_kick_node() - 原生构建的后继唤醒适配器
 * @lock: 当前已取得的共享锁借用指针。
 * @node: 已收到 MCS 交接的后继借用节点。
 *
 * 原生等待者始终自旋观察节点，无需虚拟 CPU kick；函数无返回、无副作用。
 */
static __always_inline void __pv_kick_node(struct qspinlock *lock,
					   struct mcs_spinlock *node) { }
/*
 * __pv_wait_head_or_lock() - 原生构建的队头等待适配器
 * @lock: 共享 qspinlock 借用指针。
 * @node: 当前队头的借用节点。
 *
 * 固定返回 0，表示尚未代替调用者获取锁；公共路径随后以 acquire 轮询锁字。
 */
static __always_inline u32  __pv_wait_head_or_lock(struct qspinlock *lock,
						   struct mcs_spinlock *node)
						   { return 0; }

/*
 * 首次生成默认关闭 PV 分支，并把统一 callback 名称绑定到上述 stub。启用
 * CONFIG_PARAVIRT_SPINLOCKS 时，还先把核心函数名改成 native_...，为文件末尾
 * 第二次生成的 __pv_... 实现让出公共语义名称。
 */
#define pv_enabled()		false

#define pv_init_node		__pv_init_node
#define pv_wait_node		__pv_wait_node
#define pv_kick_node		__pv_kick_node
#define pv_wait_head_or_lock	__pv_wait_head_or_lock

#ifdef CONFIG_PARAVIRT_SPINLOCKS
#define queued_spin_lock_slowpath	native_queued_spin_lock_slowpath
#endif

#endif /* _GEN_PV_LOCK_SLOWPATH */

/**
 * queued_spin_lock_slowpath - acquire the queued spinlock
 * @lock: Pointer to queued spinlock structure
 * @val: Current value of the queued spinlock 32-bit word
 *
 * (queue tail, pending bit, lock value)
 *
 *              fast     :    slow                                  :    unlock
 *                       :                                          :
 * uncontended  (0,0,0) -:--> (0,0,1) ------------------------------:--> (*,*,0)
 *                       :       | ^--------.------.             /  :
 *                       :       v           \      \            |  :
 * pending               :    (0,1,1) +--> (0,1,0)   \           |  :
 *                       :       | ^--'              |           |  :
 *                       :       v                   |           |  :
 * uncontended           :    (n,x,y) +--> (n,0,0) --'           |  :
 *   queue               :       | ^--'                          |  :
 *                       :       v                               |  :
 * contended             :    (*,x,y) +--> (*,0,0) ---> (*,0,1) -'  :
 *   queue               :         ^--'                             :
 */
/*
 * queued_spin_lock_slowpath() - 获取排队自旋锁的慢路径
 * @lock: 调用期间持续有效的共享 qspinlock 借用指针；调用者尚未取得该锁。
 * @val: 公共快路径失败时观察到的 32 位锁字快照，可能在进入后立即过期。
 *
 * 状态三元组依次为 (queue tail, pending, locked)。无竞争快路径已由调用者处理；
 * 本函数先尝试 pending 优化，再按需使用当前 CPU 的嵌套 MCS 节点排队。返回时
 * 调用者已经持锁，函数无失败返回，也不会睡眠；PV 版本的回调可能让 vCPU 阻塞，
 * 但仍保持自旋锁 API 的非任务调度语义。调用环境必须使当前 CPU 稳定，并保证
 * 自身上下文不会递归获取同一把锁。
 *
 * pending 路径不预留节点；MCS 路径递增 percpu count 后，所有成功出口都经
 * release 标签配对结束 contention trace 并递减 count。函数只借用静态 percpu
 * 节点和 @lock，不分配资源、不把节点暴露给返回后的调用者；解锁由外层 API 完成。
 */
void __lockfunc queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	/* prev 是解码出的前驱，next 是已观察到的后继，node 是本次占用的 percpu 槽。 */
	struct mcs_spinlock *prev, *next, *node;
	/* old 保存尾发布前锁字，tail 是本 CPU 与嵌套层编码后的队尾字段。 */
	u32 old, tail;
	/* idx 是递增前的嵌套深度，也是 qnodes[] 槽号。 */
	int idx;

	/* 编译期拒绝 CPU 数量超出尾码 CPU 字段可编码范围的配置。 */
	BUILD_BUG_ON(CONFIG_NR_CPUS >= (1U << _Q_TAIL_CPU_BITS));

	/* PV 生成版本跳过原生 pending/virt 优化，直接进入带 PV 回调的节点路径。 */
	if (pv_enabled())
		goto pv_queue;

	/* 体系结构虚拟锁实现若已取得锁便直接返回；false 才继续通用路径。 */
	if (virt_spin_lock(lock))
		return;

	/*
	 * Wait for in-progress pending->locked hand-overs with a bounded
	 * number of spins so that we guarantee forward progress.
	 *
	 * 0,1,0 -> 0,0,1
	 */
	/*
	 * 对正在进行的 pending->locked 交接只作有限次数自旋，以保证算法能够转入
	 * 具有前进路径的后续状态。状态 0,1,0 可能很快变成 0,0,1；若锁字仍等于
	 * 单独的 _Q_PENDING_VAL，则 cnt 从体系结构启发值递减到零后也停止等待。
	 */
	if (val == _Q_PENDING_VAL) {
		/* cnt 只限制本次锁字轮询，不表示时间或调度次数。 */
		int cnt = _Q_PENDING_LOOPS;
		val = atomic_cond_read_relaxed(&lock->val,
					       (VAL != _Q_PENDING_VAL) || !cnt--);
	}

	/*
	 * If we observe any contention; queue.
	 */
	/* 若除 locked 外已有 pending 或 tail，说明存在竞争者，直接进入公平排队。 */
	if (val & ~_Q_LOCKED_MASK)
		goto queue;

	/*
	 * trylock || pending
	 *
	 * 0,0,* -> 0,1,* -> 0,0,1 pending, trylock
	 */
	/*
	 * 尝试设置 pending：从 (0,0,*) 变为 (0,1,*)，随后等待/接管为 (0,0,1)。
	 * acquire 原子 OR 返回更新前的完整锁字，供下一步识别并发竞争。
	 */
	val = queued_fetch_set_pending_acquire(lock);

	/*
	 * If we observe contention, there is a concurrent locker.
	 *
	 * Undo and queue; our setting of PENDING might have made the
	 * n,0,0 -> 0,0,0 transition fail and it will now be waiting
	 * on @next to become !NULL.
	 */
	/*
	 * 若旧值已含 pending 或 tail，就存在并发加锁者。撤销后转入队列；本次设置
	 * PENDING 可能让另一队头的 n,0,0 -> 0,0,0 无竞争摘尾比较交换失败，那个
	 * 队头因此会保留队列并等待 @next 非 NULL，所以我们必须继续完成入队链接。
	 */
	if (unlikely(val & ~_Q_LOCKED_MASK)) {

		/* Undo PENDING if we set it. */
		/* 仅当旧值原先没有 pending 时，本线程才拥有并需要撤销刚设置的位。 */
		if (!(val & _Q_PENDING_MASK))
			clear_pending(lock);

		goto queue;
	}

	/*
	 * We're pending, wait for the owner to go away.
	 *
	 * 0,1,1 -> *,1,0
	 *
	 * this wait loop must be a load-acquire such that we match the
	 * store-release that clears the locked bit and create lock
	 * sequentiality; this is because not all
	 * clear_pending_set_locked() implementations imply full
	 * barriers.
	 */
	/*
	 * 当前线程是 pending 持有者，等待 owner 离开：(0,1,1) 到 (*,1,0)。
	 * 等待必须使用 load-acquire，与解锁清 locked 的 store-release 配对并建立锁
	 * 顺序；不能依赖 clear_pending_set_locked()，因为并非所有实现都含完整屏障。
	 * 若取回的旧值已是 unlocked，则前面的 acquire RMW 已提供所需顺序，无需再等。
	 */
	if (val & _Q_LOCKED_MASK)
		smp_cond_load_acquire(&lock->locked, !VAL);

	/*
	 * take ownership and clear the pending bit.
	 *
	 * 0,1,0 -> 0,0,1
	 */
	/* 把 pending ownership 转成 locked，记录命中 pending 快路径后直接持锁返回。 */
	clear_pending_set_locked(lock);
	lockevent_inc(lock_pending);
	return;

	/*
	 * End of pending bit optimistic spinning and beginning of MCS
	 * queuing.
	 */
	/* pending 位乐观自旋到此结束，下面开始 MCS 节点排队。 */
queue:
	/* 原生竞争路径在这里计一次 slowpath；PV 版本已在其专用队头路径统计。 */
	lockevent_inc(lock_slowpath);
pv_queue:
	/*
	 * 取得当前 CPU 首槽，把 count 的旧值作为本次嵌套索引并立即递增，再编码
	 * CPU/idx 尾码。自旋锁调用链保证 CPU 稳定；递增必须在可能发生的 IRQ 嵌套前
	 * 可见，后面的 barrier 还会约束它与实际槽初始化的编译器顺序。
	 */
	node = this_cpu_ptr(&qnodes[0].mcs);
	idx = node->count++;
	tail = encode_tail(smp_processor_id(), idx);

	/* 从占用节点起，所有出口都在 release 标签配对结束这次争用观测。 */
	trace_contention_begin(lock, LCB_F_SPIN);

	/*
	 * 4 nodes are allocated based on the assumption that there will
	 * not be nested NMIs taking spinlocks. That may not be true in
	 * some architectures even though the chance of needing more than
	 * 4 nodes will still be extremely unlikely. When that happens,
	 * we fall back to spinning on the lock directly without using
	 * any MCS node. This is not the most elegant solution, but is
	 * simple enough.
	 */
	/*
	 * 四个节点基于“不嵌套 NMI 获取自旋锁”的假设分配；少数体系结构可能打破
	 * 假设，但用尽四槽仍极罕见。发生时不能越界复用活动节点，故退化为直接
	 * 轮询共享锁的 trylock，不使用 MCS 队列。这个方案不够优雅，却能保持正确且
	 * 简单；成功后仍去 release 归还先前递增的 count 并结束 trace。
	 */
	if (unlikely(idx >= _Q_MAX_NODES)) {
		lockevent_inc(lock_no_node);
		while (!queued_spin_trylock(lock))
			cpu_relax();
		goto release;
	}

	/* 把首槽地址按完整 qnode 步长调整到本次嵌套槽；存储仍归 percpu 数组。 */
	node = grab_mcs_node(node, idx);

	/*
	 * Keep counts of non-zero index values:
	 */
	/* 只在 idx 非零时统计第二至第四节点的使用；事件编号按 idx-1 连续偏移。 */
	lockevent_cond_inc(lock_use_node2 + idx - 1, idx);

	/*
	 * Ensure that we increment the head node->count before initialising
	 * the actual node. If the compiler is kind enough to reorder these
	 * stores, then an IRQ could overwrite our assignments.
	 */
	/*
	 * 必须先递增首节点 count，再初始化实际节点。若编译器把下面的写提前到 count
	 * 之前，恰在其间到来的 IRQ 会选中同一槽并覆盖这些赋值。barrier() 只禁止
	 * 编译器重排；同 CPU 中断嵌套与 count 协议负责互斥选择，不需要硬件屏障。
	 */
	barrier();

	/* 初始化本节点的 MCS 公共字段，再让 PV callback 初始化其扩展字段。 */
	node->locked = 0;
	node->next = NULL;
	pv_init_node(node);

	/*
	 * We touched a (possibly) cold cacheline in the per-cpu queue node;
	 * attempt the trylock once more in the hope someone let go while we
	 * weren't watching.
	 */
	/*
	 * 访问 percpu 队列节点可能带入了冷 cacheline；趁这段时间 owner 也许已解锁，
	 * 因而在发布节点前再 trylock 一次。成功时节点尚未对外可见，可直接统一释放槽。
	 */
	if (queued_spin_trylock(lock))
		goto release;

	/*
	 * Ensure that the initialisation of @node is complete before we
	 * publish the updated tail via xchg_tail() and potentially link
	 * @node into the waitqueue via WRITE_ONCE(prev->next, node) below.
	 */
	/*
	 * 在 xchg_tail() 发布新尾码、以及随后可能通过 prev->next 链接 @node 之前，
	 * 保证 locked/next/PV 字段初始化全部完成。xchg_tail() 本身是 relaxed，故该
	 * smp_wmb() 是其他 CPU 凭尾码找到并访问节点时所依赖的发布顺序。
	 */
	smp_wmb();

	/*
	 * Publish the updated tail.
	 * We have already touched the queueing cacheline; don't bother with
	 * pending stuff.
	 *
	 * p,*,* -> n,*,*
	 */
	/*
	 * 发布更新后的队尾。既然已进入队列并触碰 queue cacheline，就不再尝试
	 * pending 优化；xchg 把 (p,*,*) 变为 (n,*,*) 并返回旧锁字/旧尾码。
	 */
	old = xchg_tail(lock, tail);
	/* 尚未观察后继；等待前驱时后继可能并发写入 node->next。 */
	next = NULL;

	/*
	 * if there was a previous node; link it and wait until reaching the
	 * head of the waitqueue.
	 */
	/*
	 * 若旧锁字含前一队尾，将其解码为前驱，发布 prev->next 链接，并等待 MCS
	 * ownership 逐节点传到自己。旧尾为空则本节点已是队头，不需要本地节点等待。
	 */
	if (old & _Q_TAIL_MASK) {
		prev = decode_tail(old, qnodes);

		/* Link @node into the waitqueue. */
		/* 把本节点链接到唯一前驱；WRITE_ONCE 与前驱等待 next 的并发读取配合。 */
		WRITE_ONCE(prev->next, node);

		/* PV 可先休眠等待；随后统一的 acquire 观察保证前驱交接的内存序。 */
		pv_wait_node(node, prev);
		arch_mcs_spin_lock_contended(&node->locked);

		/*
		 * While waiting for the MCS lock, the next pointer may have
		 * been set by another lock waiter. We optimistically load
		 * the next pointer & prefetch the cacheline for writing
		 * to reduce latency in the upcoming MCS unlock operation.
		 */
		/*
		 * 等待 MCS 锁期间，另一等待者可能已设置本节点的 next。这里乐观读取，
		 * 若存在便预取其 cacheline 为写，降低稍后 MCS 解锁交接的写延迟；快照为空
		 * 不代表永远无后继，真正需要交接时还会再次条件等待。
		 */
		next = READ_ONCE(node->next);
		if (next)
			prefetchw(next);
	}

	/*
	 * we're at the head of the waitqueue, wait for the owner & pending to
	 * go away.
	 *
	 * *,x,y -> *,0,0
	 *
	 * this wait loop must use a load-acquire such that we match the
	 * store-release that clears the locked bit and create lock
	 * sequentiality; this is because the set_locked() function below
	 * does not imply a full barrier.
	 *
	 * The PV pv_wait_head_or_lock function, if active, will acquire
	 * the lock and return a non-zero value. So we have to skip the
	 * atomic_cond_read_acquire() call. As the next PV queue head hasn't
	 * been designated yet, there is no way for the locked value to become
	 * _Q_SLOW_VAL. So both the set_locked() and the
	 * atomic_cmpxchg_relaxed() calls will be safe.
	 *
	 * If PV isn't active, 0 will be returned instead.
	 *
	 */
	/*
	 * 当前已到 MCS 队头，等待 owner 与 pending 都消失：(*,x,y) 到 (*,0,0)。
	 * 该循环必须 load-acquire，与解锁清 locked 的 store-release 配对并建立锁序，
	 * 因为下方 set_locked() 只是普通单次写，不隐含完整屏障。
	 *
	 * 活跃 PV 的 pv_wait_head_or_lock() 可能已经取得锁并返回非零，此时必须跳过
	 * 原生 acquire 轮询。下一 PV 队头尚未指定，所以此刻 locked 不会并发变成
	 * _Q_SLOW_VAL，后续 set_locked()/relaxed cmpxchg 在其 PV 协议下仍安全。
	 * 原生 stub 固定返回 0，继续执行 atomic_cond_read_acquire()。
	 */
	if ((val = pv_wait_head_or_lock(lock, node)))
		goto locked;

	val = atomic_cond_read_acquire(&lock->val, !(VAL & _Q_LOCKED_PENDING_MASK));

locked:
	/*
	 * claim the lock:
	 *
	 * n,0,0 -> 0,0,1 : lock, uncontended
	 * *,*,0 -> *,*,1 : lock, contended
	 *
	 * If the queue head is the only one in the queue (lock value == tail)
	 * and nobody is pending, clear the tail code and grab the lock.
	 * Otherwise, we only need to grab the lock.
	 */
	/*
	 * 取得锁：若队头也是唯一节点（锁字 tail 等于本地 tail）且无人 pending，
	 * 尝试把 n,0,0 原子换成 0,0,1，一步清尾并获锁；否则保留队尾，只需设置
	 * locked，稍后再把 MCS ownership 交给后继。
	 */

	/*
	 * In the PV case we might already have _Q_LOCKED_VAL set, because
	 * of lock stealing; therefore we must also allow:
	 *
	 * n,0,1 -> 0,0,1
	 *
	 * Note: at this point: (val & _Q_PENDING_MASK) == 0, because of the
	 *       above wait condition, therefore any concurrent setting of
	 *       PENDING will make the uncontended transition fail.
	 */
	/*
	 * PV 偷锁可能让进入此处时已经有 _Q_LOCKED_VAL，因此还允许 n,0,1 到
	 * 0,0,1。上方等待条件保证 val 中 pending 为零；若并发线程此时设置 pending，
	 * 无竞争 cmpxchg 会失败并更新 val，当前队头便转入保留尾码的竞争路径。
	 */
	if ((val & _Q_TAIL_MASK) == tail) {
		if (atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCKED_VAL))
			goto release; /* No contention */
		/* 无竞争：已同时清除尾码并持锁，节点无需向后继交接。 */
	}

	/*
	 * Either somebody is queued behind us or _Q_PENDING_VAL got set
	 * which will then detect the remaining tail and queue behind us
	 * ensuring we'll see a @next.
	 */
	/*
	 * 此时要么已有节点排在后面，要么并发设置了 _Q_PENDING_VAL；后者会看到仍
	 * 保留的 tail 并转入本节点之后排队，因此当前持有者最终必能观察到 @next。
	 */
	set_locked(lock);

	/*
	 * contended path; wait for next if not observed yet, release.
	 */
	/*
	 * 竞争路径若尚未预取到 next，就在本地指针上条件自旋到后继完成链接；这里
	 * 只需 relaxed，因为节点初始化发布与锁所有权排序已由前面的屏障/acquire 完成。
	 */
	if (!next)
		next = smp_cond_load_relaxed(&node->next, (VAL));

	arch_mcs_spin_unlock_contended(&next->locked);
	/* release 写 next->locked 完成 MCS 交接；PV 再按需要 kick 可能休眠的后继。 */
	pv_kick_node(lock, next);

release:
	/* 所有使用 percpu 槽的成功路径在此关闭 trace，并归还一次嵌套计数。 */
	trace_contention_end(lock, 0);

	/*
	 * release the node
	 */
	/* 释放本次节点槽；节点是静态 percpu 存储，不进行内存回收。 */
	__this_cpu_dec(qnodes[0].mcs.count);
}
/* 导出实际宏展开后的慢路径符号，供体系结构公共 spinlock 快路径调用。 */
EXPORT_SYMBOL(queued_spin_lock_slowpath);

/*
 * Generate the paravirt code for queued_spin_unlock_slowpath().
 */
/*
 * 为 queued_spin_unlock_slowpath() 生成半虚拟化代码。
 *
 * 原文写的是 unlock_slowpath，但下方宏和自包含实际重新生成的是
 * queued_spin_lock_slowpath()；这里保留原文并按当前代码说明。仅在第一次包含且
 * 启用 CONFIG_PARAVIRT_SPINLOCKS 时进入：定义生成标记，替换 callback/函数名，
 * 再包含 PV 实现头和本文件。第二次包含因已定义 _GEN_PV_LOCK_SLOWPATH 而跳过
 * 顶部实体，末尾条件也为假，所以不会无限递归。
 */
#if !defined(_GEN_PV_LOCK_SLOWPATH) && defined(CONFIG_PARAVIRT_SPINLOCKS)
/* 标记接下来的自包含为 PV 生成轮次，并让顶部一次性定义保护生效。 */
#define _GEN_PV_LOCK_SLOWPATH

/* 第二轮核心函数从 pv_queue 起步，并调用 qspinlock_paravirt.h 中的真实实现。 */
#undef  pv_enabled
#define pv_enabled()	true

#undef pv_init_node
#undef pv_wait_node
#undef pv_kick_node
#undef pv_wait_head_or_lock

#undef  queued_spin_lock_slowpath
/* 把同一源码函数体生成成独立的 PV 慢路径符号，避免覆盖先前的 native 版本。 */
#define queued_spin_lock_slowpath	__pv_queued_spin_lock_slowpath

/* 先定义真实 PV callback，再以本文件第二轮展开消费这些宏。 */
#include "qspinlock_paravirt.h"
#include "qspinlock.c"

/*
 * nopvspin 是早期启动阶段写入、体系结构 PV 初始化阶段读取的全局禁用请求。
 * 静态存储期默认 false；本文件不根据它直接切换已生成的函数，而由 x86/KVM、
 * Xen 等消费者决定是否安装 PV spinlock 操作。
 */
bool nopvspin;
/*
 * parse_nopvspin() - 处理无值的 `nopvspin` 内核早期参数
 * @arg: 参数解析框架传入的可选字符串；本开关不解释其内容，也不保存指针。
 *
 * 在单线程早期启动上下文把全局 nopvspin 置为 true，并返回 0 表示参数已接受。
 * 无分配和失败分支；后续体系结构初始化读取该标志，函数本身不立即改写锁实现。
 */
static __init int parse_nopvspin(char *arg)
{
	/* 参数是否出现就是全部语义，@arg 的具体文本无需读取。 */
	nopvspin = true;
	return 0;
}
/* 在早期命令行解析表登记 `nopvspin`，回调代码在 init 生命周期结束后可释放。 */
early_param("nopvspin", parse_nopvspin);
#endif
