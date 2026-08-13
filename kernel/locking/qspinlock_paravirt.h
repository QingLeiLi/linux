/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GEN_PV_LOCK_SLOWPATH
#error "do not include this file"
#endif
/*
 * 本头只允许由 qspinlock.c 的 PV 第二生成轮次包含；缺少生成标记时主动报错，
 * 防止独立包含产生缺失 qnodes、锁字 helper 或 callback 宏契约的错误编译单元。
 */

/* 哈希、早期内存分配和锁调试接口分别服务于等待者登记、启动期建表和损坏诊断。 */
#include <linux/hash.h>
#include <linux/memblock.h>
#include <linux/debug_locks.h>

/*
 * Implement paravirt qspinlocks; the general idea is to halt the vcpus instead
 * of spinning them.
 *
 * This relies on the architecture to provide two paravirt hypercalls:
 *
 *   pv_wait(u8 *ptr, u8 val) -- suspends the vcpu if *ptr == val
 *   pv_kick(cpu)             -- wakes a suspended vcpu
 *
 * Using these we implement __pv_queued_spin_lock_slowpath() and
 * __pv_queued_spin_unlock() to replace native_queued_spin_lock_slowpath() and
 * native_queued_spin_unlock().
 */
/*
 * 实现半虚拟化 qspinlock：总体思路是让等待 vCPU 停止运行，而非持续占用物理 CPU
 * 自旋。体系结构必须提供两个 hypercall 包装：pv_wait(ptr, val) 在 *ptr 仍等于
 * val 时挂起当前 vCPU，pv_kick(cpu) 唤醒指定 vCPU。
 *
 * 基于它们生成 __pv_queued_spin_lock_slowpath() 和 __pv_queued_spin_unlock()，
 * 分别替代原生 lock 慢路径与 unlock。锁与节点内存仍由公共 qspinlock 协议拥有；
 * 本头额外维护可从锁地址找到休眠队头的哈希表和 vCPU 状态机。
 */

/* locked 字节的特殊值 3：表示锁仍被持有，且 unlock 必须走查哈希/唤醒慢路径。 */
#define _Q_SLOW_VAL	(3U << _Q_LOCKED_OFFSET)

/*
 * Queue Node Adaptive Spinning
 *
 * A queue node vCPU will stop spinning if the vCPU in the previous node is
 * not running. The one lock stealing attempt allowed at slowpath entry
 * mitigates the slight slowdown for non-overcommitted guest with this
 * aggressive wait-early mechanism.
 *
 * The status of the previous node will be checked at fixed interval
 * controlled by PV_PREV_CHECK_MASK. This is to ensure that we won't
 * pound on the cacheline of the previous node too heavily.
 */
/*
 * 队列节点自适应自旋
 *
 * 若前驱节点所在 vCPU 不在运行，当前 vCPU 提前停止自旋。慢路径入口仍允许一次
 * 偷锁，缓解未超分配 guest 因激进提前等待产生的小幅退化。前驱状态只按
 * PV_PREV_CHECK_MASK 控制的固定间隔抽查，避免频繁读取前驱 cacheline。
 */
/* 每 256 次循环检查一次前驱状态；掩码命中零时才读取 prev->state。 */
#define PV_PREV_CHECK_MASK	0xff

/*
 * Queue node uses: VCPU_RUNNING & VCPU_HALTED.
 * Queue head uses: VCPU_RUNNING & VCPU_HASHED.
 */
/*
 * 普通队列节点只使用 RUNNING/HALTED；队头使用 RUNNING/HASHED。HASHED 同时表示
 * 已登记进锁地址哈希且处于可挂起状态，使交接者和解锁者能找到并唤醒它。
 */
enum vcpu_state {
	/* vCPU 正在执行或自旋，kick 方无需替它登记锁。 */
	VCPU_RUNNING = 0,
	VCPU_HALTED,		/* Used only in pv_wait_node */
	/* 仅供 pv_wait_node 使用：节点已宣布将挂起，但尚未由前驱推进。 */
	VCPU_HASHED,		/* = pv_hash'ed + VCPU_HALTED */
	/* 等于“已 pv_hash 登记 + 已挂起”；队头据此避免重复插入哈希。 */
};

/*
 * PV 节点复用 qnode 的预留空间；mcs 必须是首字段以支持公共代码双向转换。
 * cpu 是 hypervisor kick 目标，state 由等待者与前驱并发推进。对象存储仍来自
 * qspinlock.c 的静态 percpu qnodes，生命周期覆盖整次慢路径。
 */
struct pv_node {
	/* 公共 MCS 链接、交接位与嵌套 count。 */
	struct mcs_spinlock	mcs;
	/* 节点所属逻辑 CPU，在初始化后保持不变。 */
	int			cpu;
	/* vcpu_state 的并发状态字节，通过 READ/WRITE_ONCE 或原子 RMW 访问。 */
	u8			state;
};

/*
 * Hybrid PV queued/unfair lock
 *
 * By replacing the regular queued_spin_trylock() with the function below,
 * it will be called once when a lock waiter enter the PV slowpath before
 * being queued.
 *
 * The pending bit is set by the queue head vCPU of the MCS wait queue in
 * pv_wait_head_or_lock() to signal that it is ready to spin on the lock.
 * When that bit becomes visible to the incoming waiters, no lock stealing
 * is allowed. The function will return immediately to make the waiters
 * enter the MCS wait queue. So lock starvation shouldn't happen as long
 * as the queued mode vCPUs are actively running to set the pending bit
 * and hence disabling lock stealing.
 *
 * When the pending bit isn't set, the lock waiters will stay in the unfair
 * mode spinning on the lock unless the MCS wait queue is empty. In this
 * case, the lock waiters will enter the queued mode slowpath trying to
 * become the queue head and set the pending bit.
 *
 * This hybrid PV queued/unfair lock combines the best attributes of a
 * queued lock (no lock starvation) and an unfair lock (good performance
 * on not heavily contended locks).
 */
/*
 * 混合 PV 排队/非公平锁
 *
 * 以本函数替换普通 queued_spin_trylock() 后，每个等待者在真正排队前调用一次。
 * MCS 队头在 pv_wait_head_or_lock() 中设置 pending，表示它正主动等锁；新等待者
 * 一旦看到 pending 就不得偷锁而立即排队，只要队头能运行并设置该位便不会饥饿。
 * pending 未设置时，等待者在已有 MCS 队列期间保持非公平模式尝试偷锁；队列为空
 * 则停止偷锁并进入排队，争取成为新队头。这样结合排队锁的抗饥饿与低竞争时
 * 非公平锁的性能。
 */
/* 公共 PV 慢路径内的 queued_spin_trylock 调用经宏改为下方混合实现。 */
#define queued_spin_trylock(l)	pv_hybrid_queued_unfair_trylock(l)
/*
 * pv_hybrid_queued_unfair_trylock() - 在入队前尝试一次受 pending 约束的偷锁
 * @lock: 调用期间有效的共享 qspinlock 借用指针。
 *
 * 返回 true 表示以 acquire cmpxchg 已取得锁；false 表示调用者必须继续 MCS
 * 排队。函数不分配节点、不睡眠；有 tail 且无 pending 时会继续短暂自旋偷锁，
 * 队列消失或队头设置 pending 时退出，避免越过已活跃的公平队头。
 */
static inline bool pv_hybrid_queued_unfair_trylock(struct qspinlock *lock)
{
	/*
	 * Stay in unfair lock mode as long as queued mode waiters are
	 * present in the MCS wait queue but the pending bit isn't set.
	 */
	/* 只要 MCS 等待者存在但 pending 未置位，就保持非公平偷锁模式。 */
	for (;;) {
		/* val 是整锁字快照；old 是 locked 字节 cmpxchg 的期望零值兼失败回填。 */
		int val = atomic_read(&lock->val);
		u8 old = 0;

		if (!(val & _Q_LOCKED_PENDING_MASK) &&
		    try_cmpxchg_acquire(&lock->locked, &old, _Q_LOCKED_VAL)) {
			lockevent_inc(pv_lock_stealing);
			return true;
		}
		if (!(val & _Q_TAIL_MASK) || (val & _Q_PENDING_MASK))
			break;

		/* 仍有队列但队头尚未宣布 active，给 owner/队头和本次偷锁重试机会。 */
		cpu_relax();
	}

	/* 没有偷到锁；返回公共 PV 慢路径分配并发布节点。 */
	return false;
}

/*
 * The pending bit is used by the queue head vCPU to indicate that it
 * is actively spinning on the lock and no lock stealing is allowed.
 */
/*
 * pending 由 MCS 队头设置，表示它正在主动轮询锁并禁止偷锁。下列两个配置分支
 * 都把 pending->locked 转换作为 acquire 操作，但根据字段是否占完整字节选择
 * halfword 或整字原子更新。
 */
#if _Q_PENDING_BITS == 8
/*
 * set_pending() - 在 8 位布局中发布队头 active 状态
 * @lock: 共享 qspinlock 借用指针；调用者是当前 MCS 队头。
 *
 * 单次写 pending 字节为 1，保留 locked/tail；无返回，本身不取得锁。
 */
static __always_inline void set_pending(struct qspinlock *lock)
{
	WRITE_ONCE(lock->pending, 1);
}

/*
 * The pending bit check in pv_queued_spin_steal_lock() isn't a memory
 * barrier. Therefore, an atomic cmpxchg_acquire() is used to acquire the
 * lock just to be sure that it will get it.
 */
/*
 * pv_queued_spin_steal_lock() 中对 pending 的检查不是内存屏障，所以这里必须用
 * atomic cmpxchg_acquire() 真正取得锁，确保临界区获取顺序。
 */
/*
 * trylock_clear_pending() - 8 位布局下把 pending 原子转换为 locked
 * @lock: 共享 qspinlock 借用指针；进入时调用者已设置 pending。
 *
 * 先快速确认 locked 为零，再尝试把 16 位 locked_pending 从 (1,0) 换成 (0,1)。
 * 成功返回 true 且持锁；竞争失败返回 false，old 的回填不向调用者暴露。tail
 * 不受影响，acquire 由成功 cmpxchg 提供。
 */
static __always_inline bool trylock_clear_pending(struct qspinlock *lock)
{
	/* 16 位旧值编码 pending=1、locked=0。 */
	u16 old = _Q_PENDING_VAL;

	return !READ_ONCE(lock->locked) &&
	       try_cmpxchg_acquire(&lock->locked_pending, &old, _Q_LOCKED_VAL);
}
#else /* _Q_PENDING_BITS == 8 */
/*
 * set_pending() - 1 位布局下以整字 RMW 设置 pending
 * @lock: 共享 qspinlock 借用指针；调用者是当前 MCS 队头。
 *
 * atomic_or 保留相邻 locked/tail，无返回且不单独取得锁。
 */
static __always_inline void set_pending(struct qspinlock *lock)
{
	atomic_or(_Q_PENDING_VAL, &lock->val);
}

/*
 * trylock_clear_pending() - 1 位布局下把 pending 原子转换为 locked
 * @lock: 共享 qspinlock 借用指针；进入时调用者已设置 pending。
 *
 * 循环保留最新 tail、清 pending 并置 locked；若观察到 locked 已被占用则返回
 * false，成功 acquire cmpxchg 返回 true。失败重试时 old 由原子原语更新，避免
 * 覆盖并发队尾变化；函数不管理节点生命周期。
 */
static __always_inline bool trylock_clear_pending(struct qspinlock *lock)
{
	/* old 是比较值兼失败快照，new 是保留 tail 后的新 locked/pending 状态。 */
	int old, new;

	old = atomic_read(&lock->val);
	do {
		if (old & _Q_LOCKED_MASK)
			return false;
		/*
		 * Try to clear pending bit & set locked bit
		 */
		/* 尝试清除 pending 并设置 locked，同时保留 old 中其他字段。 */
		new = (old & ~_Q_PENDING_MASK) | _Q_LOCKED_VAL;
	} while (!atomic_try_cmpxchg_acquire (&lock->val, &old, new));

	return true;
}
#endif /* _Q_PENDING_BITS == 8 */

/*
 * Lock and MCS node addresses hash table for fast lookup
 *
 * Hashing is done on a per-cacheline basis to minimize the need to access
 * more than one cacheline.
 *
 * Dynamically allocate a hash table big enough to hold at least 4X the
 * number of possible cpus in the system. Allocation is done on page
 * granularity. So the minimum number of hash buckets should be at least
 * 256 (64-bit) or 512 (32-bit) to fully utilize a 4k page.
 *
 * Since we should not be holding locks from NMI context (very rare indeed) the
 * max load factor is 0.75, which is around the point where open addressing
 * breaks down.
 *
 */
/*
 * 锁地址到 MCS 节点地址的快速查找哈希表
 *
 * 探测以 cacheline 为单位对齐起点，尽量让常见命中只访问一条 cacheline。启动
 * 时动态分配至少 possible CPU 数四倍的表，并按页粒度取整；为了充分利用 4K
 * 页，最少需要 64 位 256 桶或 32 位 512 桶。正常不应在 NMI 持锁，因此每 CPU
 * 最多三个阻塞层级，相对四倍容量的最大负载约 0.75，正是开放寻址开始明显退化
 * 的边界。
 *
 * 只有休眠队头占条目，lock 是发布/匹配键，node 是解锁者要 kick 的 percpu
 * 节点。owner 必须先 unhash 再 release，保证同一锁不会同时留下多个有效条目。
 */
struct pv_hash_entry {
	/* 非 NULL 表示槽已占用，并作为并发认领与查找的键。 */
	struct qspinlock *lock;
	/* 与该锁当前休眠队头对应的借用节点，由 lock 的发布协议保护。 */
	struct pv_node   *node;
};

/* 一个 cacheline 可容纳的条目数，也是探测起点对齐和容量取整单位。 */
#define PV_HE_PER_LINE	(SMP_CACHE_BYTES / sizeof(struct pv_hash_entry))
/* 一页至少容纳的条目数，保证最小分配完整利用 PAGE_SIZE。 */
#define PV_HE_MIN	(PAGE_SIZE / sizeof(struct pv_hash_entry))

/* 早期启动一次分配的永久哈希基址；之后只复用槽，不释放整张表。 */
static struct pv_hash_entry *pv_lock_hash;
/* 表容量为 1 << bits；初始化后只读，供 hash_ptr 和环形掩码共同使用。 */
static unsigned int pv_lock_hash_bits __read_mostly;

/*
 * Allocate memory for the PV qspinlock hash buckets
 *
 * This function should be called from the paravirt spinlock initialization
 * routine.
 */
/*
 * __pv_init_lock_hash() - 为 PV qspinlock 哈希桶分配内存
 * 本函数应由体系结构 paravirt spinlock 初始化流程在早期启动期调用一次。
 *
 * 无参数和返回值；成功后发布全局 pv_lock_hash 与 pv_lock_hash_bits。分配使用
 * HASH_EARLY|HASH_ZERO，得到按页、因而也按 cacheline 对齐并清零的表；底层早期
 * 分配失败按内核启动分配器策略处理，本函数没有局部回滚或运行期释放路径。
 */
void __init __pv_init_lock_hash(void)
{
	/* 目标容量先取 4 * possible CPU，再向上对齐到整条 cacheline 的桶数。 */
	int pv_hash_size = ALIGN(4 * num_possible_cpus(), PV_HE_PER_LINE);

	/* 小系统也至少分配一页条目，维持探测布局和较低初始负载。 */
	if (pv_hash_size < PV_HE_MIN)
		pv_hash_size = PV_HE_MIN;

	/*
	 * Allocate space from bootmem which should be page-size aligned
	 * and hence cacheline aligned.
	 */
	/*
	 * 从 bootmem/早期内存分配按页对齐空间，因此也满足 cacheline 对齐；传入相同
	 * low/high limit 固定目标规模，分配器回填实际 2 次幂表的 shift。
	 */
	pv_lock_hash = alloc_large_system_hash("PV qspinlock",
					       sizeof(struct pv_hash_entry),
					       pv_hash_size, 0,
					       HASH_EARLY | HASH_ZERO,
					       &pv_lock_hash_bits, NULL,
					       pv_hash_size, pv_hash_size);
}

/*
 * 从 lock 哈希值所在 cacheline 的首槽开始开放寻址，最多环形扫描整张表。
 * @he/@offset 是调用者局部变量；@hash 会先被就地清低位再用于掩码环绕，参数
 * 会多次求值，只能传无副作用左值。表容量必须是 2 次幂且已完成早期初始化。
 */
#define for_each_hash_entry(he, offset, hash)						\
	for (hash &= ~(PV_HE_PER_LINE - 1), he = &pv_lock_hash[hash], offset = 0;	\
	     offset < (1 << pv_lock_hash_bits);						\
	     offset++, he = &pv_lock_hash[(hash + offset) & ((1 << pv_lock_hash_bits) - 1)])

/*
 * pv_hash() - 登记阻塞锁到其 PV 队头节点的映射
 * @lock: 仍被持有且生命周期至少覆盖后续 unhash 的 qspinlock 借用指针。
 * @node: 当前队头的静态 percpu PV 节点借用指针，必须持续到 owner 解锁并 kick。
 *
 * 返回被认领条目中 lock 字段的地址，供队头在“设置 SLOW 时恰好取得空锁”路径
 * 直接清槽；正常路径由 unlock 通过 pv_unhash() 清理。函数以 cmpxchg 原子认领
 * 空槽，再写 node；后续发布 _Q_SLOW_VAL 的屏障保证解锁者看到完整映射。无空槽
 * 表示容量/单条目不变量被破坏，BUG() 终止而非返回 NULL。
 */
static struct qspinlock **pv_hash(struct qspinlock *lock, struct pv_node *node)
{
	/* hash 是初始桶，offset 为探测距离；二者在遍历宏内环绕全表。 */
	unsigned long offset, hash = hash_ptr(lock, pv_lock_hash_bits);
	/* he 指向当前候选槽；hopcnt 记录统计意义上的探测次数。 */
	struct pv_hash_entry *he;
	int hopcnt = 0;

	for_each_hash_entry(he, offset, hash) {
		/* cmpxchg 只接受 lock==NULL 的空槽；失败会把实际键回填到 old。 */
		struct qspinlock *old = NULL;
		hopcnt++;
		if (try_cmpxchg(&he->lock, &old, lock)) {
			WRITE_ONCE(he->node, node);
			lockevent_pv_hop(hopcnt);
			return &he->lock;
		}
	}
	/*
	 * Hard assume there is a free entry for us.
	 *
	 * This is guaranteed by ensuring every blocked lock only ever consumes
	 * a single entry, and since we only have 4 nesting levels per CPU
	 * and allocated 4*nr_possible_cpus(), this must be so.
	 *
	 * The single entry is guaranteed by having the lock owner unhash
	 * before it releases.
	 */
	/*
	 * 硬性假定一定存在空槽。表按每 CPU 四层的理论上界预留，前述“NMI 通常不
	 * 持锁”又使实际占用低于饱和；每把阻塞锁只能消耗一个条目。单条目性质来自
	 * owner 在 release 前先 unhash，防止下一任 owner 为同一地址重复登记。
	 */
	BUG();
}

/*
 * pv_unhash() - 按锁地址查找、移除映射并取得待唤醒节点
 * @lock: 当前值为 _Q_SLOW_VAL 的 qspinlock 借用指针。
 *
 * 返回此前 pv_hash() 登记的 pv_node 借用指针；先读取 node，再把条目 lock 清为
 * NULL 供后续复用。调用者已用屏障保证 SLOW 观察先于本次查表，并在 release 锁
 * 后仍可依赖 percpu 节点生命周期执行 kick。找不到条目说明 SLOW/哈希协议损坏，
 * BUG() 终止而不返回 NULL。
 */
static struct pv_node *pv_unhash(struct qspinlock *lock)
{
	/* 与 pv_hash 使用相同起点/探测序；node 保存清槽前取得的结果。 */
	unsigned long offset, hash = hash_ptr(lock, pv_lock_hash_bits);
	struct pv_hash_entry *he;
	struct pv_node *node;

	for_each_hash_entry(he, offset, hash) {
		if (READ_ONCE(he->lock) == lock) {
			node = READ_ONCE(he->node);
			WRITE_ONCE(he->lock, NULL);
			return node;
		}
	}
	/*
	 * Hard assume we'll find an entry.
	 *
	 * This guarantees a limited lookup time and is itself guaranteed by
	 * having the lock owner do the unhash -- IFF the unlock sees the
	 * SLOW flag, there MUST be a hash entry.
	 */
	/*
	 * 硬性假定必能找到条目，从而把查找限制在一张表内。锁 owner 负责 unhash；
	 * 当且仅当 unlock 看到 SLOW 标记时，发布协议保证对应条目必须存在。
	 */
	BUG();
}

/*
 * Return true if when it is time to check the previous node which is not
 * in a running state.
 */
/*
 * 当到达固定抽查周期且前驱不处于 RUNNING 时返回 true。
 */
/*
 * pv_wait_early() - 判断是否应因前驱未运行而提前停止本轮自旋
 * @prev: 有效前驱 PV 节点借用指针，排队关系保证读取期间存活。
 * @loop: 当前倒计时；只有低 PV_PREV_CHECK_MASK 位全零才读取前驱状态。
 *
 * 返回 true 表示应转入 halt 准备，false 表示继续自旋。只作 READ_ONCE 快照，
 * 不改变前驱、不提供同步获取语义，也不保证返回后状态仍不变化。
 */
static inline bool
pv_wait_early(struct pv_node *prev, int loop)
{
	if ((loop & PV_PREV_CHECK_MASK) != 0)
		return false;

	return READ_ONCE(prev->state) != VCPU_RUNNING;
}

/*
 * Initialize the PV part of the mcs_spinlock node.
 */
/* 初始化 mcs_spinlock 节点中由 PV 扩展占用的部分。 */
/*
 * pv_init_node() - 初始化当前排队节点的 PV 字段
 * @node: 已初始化公共 mcs 字段、尚未发布的新节点借用指针。
 *
 * 把公共节点转换为 pv_node，记录稳定的当前 CPU 并置 RUNNING。函数无返回，
 * 不发布节点、不分配资源；调用者随后以 smp_wmb() 和尾码发布整个节点。
 */
static void pv_init_node(struct mcs_spinlock *node)
{
	/* pn 与 @node 指向同一 qnode 存储，PV 字段位于 mcs 之后的预留空间。 */
	struct pv_node *pn = (struct pv_node *)node;

	/* 编译期保证 PV 扩展不会越过 qnode 为 PV 构建预留的容量。 */
	BUILD_BUG_ON(sizeof(struct pv_node) > sizeof(struct qnode));

	/* 自旋锁上下文保证 CPU 稳定；cpu 用于未来 kick，state 从正在运行开始。 */
	pn->cpu = smp_processor_id();
	pn->state = VCPU_RUNNING;
}

/*
 * Wait for node->locked to become true, halt the vcpu after a short spin.
 * pv_kick_node() is used to set _Q_SLOW_VAL and fill in hash table on its
 * behalf.
 */
/*
 * 等待 node->locked 变为真；短暂自旋后挂起 vCPU。若前驱已经停止运行，可在
 * 阈值耗尽前提前挂起。pv_kick_node() 会代表本节点设置推进状态，使它随后在
 * 队头阶段能建立锁地址哈希并使用 _Q_SLOW_VAL 协议。
 */
/*
 * pv_wait_node() - 等待 MCS 前驱把队列 ownership 交给当前节点
 * @node: 当前等待者的有效 percpu MCS 节点借用指针。
 * @prev: 当前节点的有效前驱借用指针，用于周期抽查其运行状态。
 *
 * 函数先自旋，必要时通过 pv_wait() 挂起 vCPU；观察 node->locked 后返回。它不
 * 自行提供 acquire，调用者必须紧接着执行 arch_mcs_spin_lock_contended()，即使
 * 本函数已观察到 1，也由该统一 load-acquire 建立前驱临界区的可见性。节点与
 * 前驱存储均由 percpu 队列拥有，函数不转移 ownership。
 */
static void pv_wait_node(struct mcs_spinlock *node, struct mcs_spinlock *prev)
{
	/* pn/pp 是公共节点对应的 PV 视图；wait_early/loop 控制每轮自旋与提前挂起。 */
	struct pv_node *pn = (struct pv_node *)node;
	struct pv_node *pp = (struct pv_node *)prev;
	bool wait_early;
	int loop;

	/* 虚假唤醒或抢锁竞态可使一轮未完成，外层循环重新短暂自旋后再决定挂起。 */
	for (;;) {
		/* 每轮最多 SPIN_THRESHOLD 次；固定间隔检查前驱是否已不运行。 */
		for (wait_early = false, loop = SPIN_THRESHOLD; loop; loop--) {
			if (READ_ONCE(node->locked))
				return;
			if (pv_wait_early(pp, loop)) {
				wait_early = true;
				break;
			}
			cpu_relax();
		}

		/*
		 * Order pn->state vs pn->locked thusly:
		 *
		 * [S] pn->state = VCPU_HALTED	  [S] next->locked = 1
		 *     MB			      MB
		 * [L] pn->locked		[RmW] pn->state = VCPU_HASHED
		 *
		 * Matches the cmpxchg() from pv_kick_node().
		 */
		/*
		 * pn->state 与 pn->locked 必须形成如下顺序：等待者先 store-mb 宣布 HALTED，
		 * 再检查交接位；前驱先 release 写 next->locked，再以 RMW 把后继 state 推到
		 * HASHED。它与 pv_kick_node() 的 cmpxchg 配对，排除“已经交接却双方都认为
		 * 对方负责唤醒”而永久休眠的丢失唤醒。
		 */
		smp_store_mb(pn->state, VCPU_HALTED);

		/* 宣布 HALTED 后再次检查 locked；仍未交接才记录并条件挂起。 */
		if (!READ_ONCE(node->locked)) {
			lockevent_inc(pv_wait_node);
			lockevent_cond_inc(pv_wait_early, wait_early);
			pv_wait(&pn->state, VCPU_HALTED);
		}

		/*
		 * If pv_kick_node() changed us to VCPU_HASHED, retain that
		 * value so that pv_wait_head_or_lock() knows to not also try
		 * to hash this lock.
		 */
		/*
		 * 若 pv_kick_node() 已把状态改为 HASHED，比较交换因期望 HALTED 不匹配而
		 * 保留 HASHED，使 pv_wait_head_or_lock() 知道无需重复入哈希；若仍为 HALTED，
		 * 则恢复 RUNNING，表示本 vCPU 已继续执行。
		 */
		cmpxchg(&pn->state, VCPU_HALTED, VCPU_RUNNING);

		/*
		 * If the locked flag is still not set after wakeup, it is a
		 * spurious wakeup and the vCPU should wait again. However,
		 * there is a pretty high overhead for CPU halting and kicking.
		 * So it is better to spin for a while in the hope that the
		 * MCS lock will be released soon.
		 */
		/*
		 * 唤醒后 locked 仍未设置就是虚假唤醒，应再次等待。但 halt/kick 成本很高，
		 * 所以下一轮先重新自旋一段时间，期待 MCS ownership 很快到达，并记录事件。
		 */
		lockevent_cond_inc(pv_spurious_wakeup,
				  !READ_ONCE(node->locked));
	}

	/*
	 * By now our node->locked should be 1 and our caller will not actually
	 * spin-wait for it. We do however rely on our caller to do a
	 * load-acquire for us.
	 */
	/*
	 * 到这里按设计 node->locked 已为 1，调用者后续不会长时间自旋；但本函数的
	 * READ_ONCE 不承担 acquire，仍明确依赖调用者执行统一的 load-acquire。
	 * 当前实现的成功检查在循环内直接 return，因此本尾注描述协议上的到达状态。
	 */
}

/*
 * Called after setting next->locked = 1 when we're the lock owner.
 *
 * Instead of waking the waiters stuck in pv_wait_node() advance their state
 * such that they're waiting in pv_wait_head_or_lock(), this avoids a
 * wake/sleep cycle.
 */
/*
 * 当前锁 owner 写 next->locked=1 后调用。与其唤醒仍停在 pv_wait_node() 的后继，
 * 更高效的做法是把后继状态推进到 pv_wait_head_or_lock() 所期待的 HASHED，避免
 * 一次先 wake、随后又 sleep 的往返。
 */
/*
 * pv_kick_node() - 在 MCS 交接后推进或唤醒后继的 PV 状态
 * @lock: 当前持有且将由后继成为队头的 qspinlock 借用指针。
 * @node: 已收到 locked=1 的后继节点借用指针，生命周期由 percpu 队列保证。
 *
 * 后继若仍为 HALTED，就原子改成 HASHED、把锁登记到哈希并将 locked 写成 SLOW；
 * 若状态不是 HALTED，说明后继正运行并会自行观察交接，函数直接返回。无普通失败
 * 返回和内存分配；哈希无槽会 BUG。成功后 unlock 必须先 unhash 才能 release。
 */
static void pv_kick_node(struct qspinlock *lock, struct mcs_spinlock *node)
{
	/* pn 是后继 PV 视图；old 既是期望 HALTED，也接收失败时的实际状态。 */
	struct pv_node *pn = (struct pv_node *)node;
	u8 old = VCPU_HALTED;
	/*
	 * If the vCPU is indeed halted, advance its state to match that of
	 * pv_wait_node(). If OTOH this fails, the vCPU was running and will
	 * observe its next->locked value and advance itself.
	 *
	 * Matches with smp_store_mb() and cmpxchg() in pv_wait_node()
	 *
	 * The write to next->locked in arch_mcs_spin_unlock_contended()
	 * must be ordered before the read of pn->state in the cmpxchg()
	 * below for the code to work correctly. To guarantee full ordering
	 * irrespective of the success or failure of the cmpxchg(),
	 * a relaxed version with explicit barrier is used. The control
	 * dependency will order the reading of pn->state before any
	 * subsequent writes.
	 */
	/*
	 * 若 vCPU 确已 HALTED，把状态推进到与 pv_wait_node() 协议匹配的 HASHED；若
	 * cmpxchg 失败，vCPU 正运行，会看到 next->locked 后自行前进。
	 *
	 * arch_mcs_spin_unlock_contended() 对 next->locked 的 release 写必须先于本次
	 * cmpxchg 读取 pn->state。为让成功与失败两种 RMW 结果都具备完整前序，使用
	 * relaxed cmpxchg 加显式 smp_mb__before_atomic()；失败后的控制依赖还把 state
	 * 读取排在后续写之前。该图与等待侧 store-mb/cmpxchg 配对。
	 */
	smp_mb__before_atomic();
	if (!try_cmpxchg_relaxed(&pn->state, &old, VCPU_HASHED))
		return;

	/*
	 * Put the lock into the hash table and set the _Q_SLOW_VAL.
	 *
	 * As this is the same vCPU that will check the _Q_SLOW_VAL value and
	 * the hash table later on at unlock time, no atomic instruction is
	 * needed.
	 */
	/*
	 * 把锁放入哈希并设置 _Q_SLOW_VAL。未来检查 SLOW 并查哈希的是同一 vCPU，
	 * 所以这里对 locked 的写无需额外原子 RMW；pv_hash 内的发布再由后续协议保证
	 * unlock 可见。写 SLOW 后，该锁对象在 unhash/release 前必须继续存活。
	 */
	WRITE_ONCE(lock->locked, _Q_SLOW_VAL);
	(void)pv_hash(lock, pn);
}

/*
 * Wait for l->locked to become clear and acquire the lock;
 * halt the vcpu after a short spin.
 * __pv_queued_spin_unlock() will wake us.
 *
 * The current value of the lock will be returned for additional processing.
 */
/*
 * 等待 lock->locked 清零并取得锁；短暂自旋后挂起 vCPU，最终由
 * __pv_queued_spin_unlock() 唤醒。返回当前锁字供公共慢路径继续处理。
 */
/*
 * pv_wait_head_or_lock() - 作为 MCS 队头等待并取得共享 qspinlock
 * @lock: 当前仍由 owner 持有、调用期间持续有效的共享锁借用指针。
 * @node: 当前队头的有效 percpu MCS 节点；PV 扩展已由 pv_init_node() 初始化。
 *
 * 返回非零 u32，表示函数已通过 acquire cmpxchg 或全序 xchg 取得锁；公共慢路径
 * 因此跳过原生 acquire 等待并进入 locked 阶段。函数可能经 pv_wait() 挂起 vCPU，
 * 但不进行任务级睡眠。首次需要挂起时登记哈希并发布 SLOW；取得锁时若由本函数
 * 自己建立条目则负责直接清槽，否则未来 unlock 按 SLOW 协议 unhash。节点和锁
 * 都是借用对象，函数不分配内存；哈希不变量破坏会在 pv_hash() 中 BUG。
 */
static u32
pv_wait_head_or_lock(struct qspinlock *lock, struct mcs_spinlock *node)
{
	/* pn 是本节点的 PV 视图；lp 为已登记条目的 lock 字段地址，1 是“已登记”哨兵。 */
	struct pv_node *pn = (struct pv_node *)node;
	struct qspinlock **lp = NULL;
	/* waitcnt 统计重复等待轮数，loop 控制每轮主动自旋预算。 */
	int waitcnt = 0;
	int loop;

	/*
	 * If pv_kick_node() already advanced our state, we don't need to
	 * insert ourselves into the hash table anymore.
	 */
	/*
	 * 若 pv_kick_node() 已把状态推进到 HASHED，它也已代为建立条目；使用非 NULL、
	 * 永不解引用的哨兵跳过本函数的首次 hash 分支，避免同一锁重复登记。
	 */
	if (READ_ONCE(pn->state) == VCPU_HASHED)
		lp = (struct qspinlock **)1;

	/*
	 * Tracking # of slowpath locking operations
	 */
	/* 统计一次 PV 队头锁慢路径，而非每次可能重复的 wait 循环。 */
	lockevent_inc(lock_slowpath);

	/* 偷锁或虚假/竞争唤醒可能让队头重复等待；waitcnt 在每轮末递增。 */
	for (;; waitcnt++) {
		/*
		 * Set correct vCPU state to be used by queue node wait-early
		 * mechanism.
		 */
		/* 每轮先宣布 RUNNING，供后继的 wait-early 判断当前队头正在推进。 */
		WRITE_ONCE(pn->state, VCPU_RUNNING);

		/*
		 * Set the pending bit in the active lock spinning loop to
		 * disable lock stealing before attempting to acquire the lock.
		 */
		/* 主动自旋前设置 pending 禁止新等待者偷锁，再在有限预算内尝试 acquire。 */
		set_pending(lock);
		for (loop = SPIN_THRESHOLD; loop; loop--) {
			if (trylock_clear_pending(lock))
				goto gotlock;
			cpu_relax();
		}
		/* 未在预算内取得锁，撤销 active 标记，随后才允许进入可挂起状态。 */
		clear_pending(lock);


		if (!lp) { /* ONCE */
			/* 仅第一次需要挂起时登记；lp 同时充当“已完成一次”标志。 */
			lp = pv_hash(lock, pn);

			/*
			 * We must hash before setting _Q_SLOW_VAL, such that
			 * when we observe _Q_SLOW_VAL in __pv_queued_spin_unlock()
			 * we'll be sure to be able to observe our hash entry.
			 *
			 *   [S] <hash>                 [Rmw] l->locked == _Q_SLOW_VAL
			 *       MB                           RMB
			 * [RmW] l->locked = _Q_SLOW_VAL  [L] <unhash>
			 *
			 * Matches the smp_rmb() in __pv_queued_spin_unlock().
			 */
			/*
			 * 必须先写哈希条目，再设置 _Q_SLOW_VAL；这样 unlock 一旦观察到
			 * SLOW，经过其 smp_rmb() 就一定能看到条目。等待侧的 <hash> 写先于
			 * xchg RMW，解锁侧先以 RMW/失败读取观察 SLOW，再以 RMB 约束 unhash
			 * 读取，构成图中的发布—查找顺序。
			 */
			if (xchg(&lock->locked, _Q_SLOW_VAL) == 0) {
				/*
				 * The lock was free and now we own the lock.
				 * Change the lock value back to _Q_LOCKED_VAL
				 * and unhash the table.
				 */
				/*
				 * xchg 发现锁原本为空，因此当前队头已取得锁。把特殊 SLOW 恢复成
				 * 普通 LOCKED，并直接清除自己刚建条目的键；无需 unlock 慢路径介入。
				 */
				WRITE_ONCE(lock->locked, _Q_LOCKED_VAL);
				WRITE_ONCE(*lp, NULL);
				goto gotlock;
			}
		}
		/* 条目和 SLOW 均已发布，标记节点 HASHED 后记录等待次数并条件挂起。 */
		WRITE_ONCE(pn->state, VCPU_HASHED);
		lockevent_inc(pv_wait_head);
		lockevent_cond_inc(pv_wait_again, waitcnt);
		pv_wait(&lock->locked, _Q_SLOW_VAL);

		/*
		 * Because of lock stealing, the queue head vCPU may not be
		 * able to acquire the lock before it has to wait again.
		 */
		/* 偷锁可能抢在被唤醒的队头之前取得锁，所以队头可能必须再次循环等待。 */
	}

	/*
	 * The cmpxchg() or xchg() call before coming here provides the
	 * acquire semantics for locking. The dummy ORing of _Q_LOCKED_VAL
	 * here is to indicate to the compiler that the value will always
	 * be nozero to enable better code optimization.
	 */
	/*
	 * 到达这里前的 cmpxchg 或 xchg 已提供获取锁所需的 acquire 语义。与
	 * _Q_LOCKED_VAL 做一次仅供编译器分析的 OR，明确返回值恒非零，便于优化；
	 * 它不再次修改共享锁字。原文 nozero 按当前语义应理解为 nonzero。
	 */
gotlock:
	return (u32)(atomic_read(&lock->val) | _Q_LOCKED_VAL);
}

/*
 * Include the architecture specific callee-save thunk of the
 * __pv_queued_spin_unlock(). This thunk is put together with
 * __pv_queued_spin_unlock() to make the callee-save thunk and the real unlock
 * function close to each other sharing consecutive instruction cachelines.
 * Alternatively, architecture specific version of __pv_queued_spin_unlock()
 * can be defined.
 */
/*
 * 包含体系结构专用的 __pv_queued_spin_unlock() callee-save thunk。把 thunk 与真实
 * unlock 放在相邻的连续指令 cacheline，可改善调用局部性；体系结构也可以直接
 * 定义优化版 __pv_queued_spin_unlock()。x86-64 当前用汇编合并寄存器保存和普通
 * 解锁快路径，32 位则生成调用下方 C 实现的 thunk。
 */
#include <asm/qspinlock_paravirt.h>

/*
 * PV versions of the unlock fastpath and slowpath functions to be used
 * instead of queued_spin_unlock().
 */
/* 下列 PV 解锁快/慢路径替代 queued_spin_unlock()，并负责唤醒哈希中的队头。 */
/*
 * __pv_queued_spin_unlock_slowpath() - 释放 SLOW 锁并唤醒登记的 PV 队头
 * @lock: 当前持有的 qspinlock 借用指针；进入时尚未执行 release 清零。
 * @locked: 快路径 release cmpxchg 失败后回填的实际 locked 字节。
 *
 * 正常要求 @locked == _Q_SLOW_VAL。函数先以 RMB 确认 SLOW 发布对应的哈希可见，
 * 再查找并移除条目，随后 store-release 清锁，最后用已保存节点的 CPU 号 kick。
 * 无返回值和普通失败码；损坏值按 debug_locks_silent 策略告警后保留锁状态返回，
 * 哈希缺失则 BUG。unhash 后不保留 @lock 引用，release 后只继续使用 percpu node。
 */
__visible __lockfunc void
__pv_queued_spin_unlock_slowpath(struct qspinlock *lock, u8 locked)
{
	/* node 保存 unhash 返回的静态 percpu 节点，跨锁 release 仍然有效。 */
	struct pv_node *node;

	/* 非 SLOW 值说明快路径失败原因不是合法 PV 慢状态，诊断后不能贸然解锁。 */
	if (unlikely(locked != _Q_SLOW_VAL)) {
		WARN(!debug_locks_silent,
		     "pvqspinlock: lock 0x%lx has corrupted value 0x%x!\n",
		     (unsigned long)lock, atomic_read(&lock->val));
		return;
	}

	/*
	 * A failed cmpxchg doesn't provide any memory-ordering guarantees,
	 * so we need a barrier to order the read of the node data in
	 * pv_unhash *after* we've read the lock being _Q_SLOW_VAL.
	 *
	 * Matches the cmpxchg() in pv_wait_head_or_lock() setting _Q_SLOW_VAL.
	 */
	/*
	 * 失败的 cmpxchg 不提供内存排序，所以必须以屏障把 pv_unhash() 的 node 数据
	 * 读取排在已经观察 lock==_Q_SLOW_VAL 之后，与队头“先 hash、再发布 SLOW”
	 * 的 RMW 配对。原文称 pv_wait_head_or_lock() 用 cmpxchg 设置 SLOW，当前代码
	 * 实际使用 xchg()；保留原文并以现实现为准。
	 */
	smp_rmb();

	/*
	 * Since the above failed to release, this must be the SLOW path.
	 * Therefore start by looking up the blocked node and unhashing it.
	 */
	/* 快速 release 已失败且值为 SLOW，先找到阻塞队头并移除唯一哈希条目。 */
	node = pv_unhash(lock);

	/*
	 * Now that we have a reference to the (likely) blocked pv_node,
	 * release the lock.
	 */
	/* 已持有独立 node 引用，现可用 store-release 发布临界区并把 locked 清零。 */
	smp_store_release(&lock->locked, 0);

	/*
	 * At this point the memory pointed at by lock can be freed/reused,
	 * however we can still use the pv_node to kick the CPU.
	 * The other vCPU may not really be halted, but kicking an active
	 * vCPU is harmless other than the additional latency in completing
	 * the unlock.
	 */
	/*
	 * release 后 @lock 指向内存即可被释放或复用，后续严禁再解引用它；percpu
	 * pv_node 仍存活，可安全读取 cpu 并 kick。目标 vCPU 即使实际未挂起，多一次
	 * kick 也只增加解锁延迟，不破坏正确性。
	 */
	lockevent_inc(pv_kick_unlock);
	pv_kick(node->cpu);
}

/* 体系结构未提供优化版/宏标记时，编译下方通用 C 解锁入口。 */
#ifndef __pv_queued_spin_unlock
/*
 * __pv_queued_spin_unlock() - PV qspinlock 解锁快路径
 * @lock: 当前 CPU 持有且调用期间有效的 qspinlock 借用指针。
 *
 * 以 release cmpxchg 尝试把普通 LOCKED 直接清零；成功即返回。失败时原子原语把
 * 实际字节回填到 locked，交给慢路径区分合法 SLOW 与损坏值。函数无返回值；
 * 普通路径不访问哈希，SLOW 路径必须先 unhash 再 release/kick。
 */
__visible __lockfunc void __pv_queued_spin_unlock(struct qspinlock *lock)
{
	/* 期望普通锁值；cmpxchg 失败时更新为实际 SLOW 或损坏值。 */
	u8 locked = _Q_LOCKED_VAL;

	/*
	 * We must not unlock if SLOW, because in that case we must first
	 * unhash. Otherwise it would be possible to have multiple @lock
	 * entries, which would be BAD.
	 */
	/*
	 * SLOW 状态下绝不能直接清锁，因为必须先 unhash；否则下一任 owner 可能为同一
	 * @lock 再插入条目，产生多个映射并让唤醒对象失配。普通值才可直接 release。
	 */
	if (try_cmpxchg_release(&lock->locked, &locked, 0))
		return;

	__pv_queued_spin_unlock_slowpath(lock, locked);
}
#endif /* __pv_queued_spin_unlock */
