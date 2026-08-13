// SPDX-License-Identifier: GPL-2.0
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/osq_lock.h>

/*
 * An MCS like lock especially tailored for optimistic spinning for sleeping
 * lock implementations (mutex, rwsem, etc).
 *
 * Using a single mcs node per CPU is safe because sleeping locks should not be
 * called from interrupt context and we have preemption disabled while
 * spinning.
 */
/*
 * 这是为 mutex、rwsem 等睡眠锁的乐观自旋定制的 MCS 风格锁。每 CPU 只设一个节点仍然安全，
 * 因为睡眠锁不能在中断上下文获取，而且自旋期间抢占保持关闭，所以同一 CPU 不会嵌套复用该节点。
 */

/*
 * optimistic_spin_node - OSQ 中由一个 CPU 独占复用的排队节点
 * @next: 后继发布的链接；前驱释放或取消时读取并可能原子清空
 * @prev: 当前前驱；并发取消的 Step-C 可以把它改接到更早节点
 * @locked: 前驱写 1 表示 ownership 已交接，本 CPU 只在本节点上等待
 * @cpu: 本节点所属 CPU 的编号加一编码，0 保留给空队列
 *
 * 节点来自永久 percpu 存储而非调用栈，因此并发前驱在链接竞态中可安全 cmpxchg 其字段；但只有
 * 抢占关闭期间的本 CPU 调用者拥有本轮初始化和取消责任。
 */
struct optimistic_spin_node {
	struct optimistic_spin_node *next, *prev;
	int locked; /* 1 if lock acquired */
	/* locked 为 1 表示已经取得锁。 */
	int cpu; /* encoded CPU # + 1 value */
	/* cpu 保存 CPU 编号加一后的编码值。 */
};

/* 每 CPU 一个共享缓存行对齐节点；永久存储避免相邻取消路径遇到栈节点失效。 */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_node);

/*
 * We use the value 0 to represent "no CPU", thus the encoded value
 * will be the CPU number incremented by 1.
 */
/* 0 表示“无 CPU”，所以有效 CPU 编号统一加一后存入 tail/node。 */
/*
 * encode_cpu - 把真实 CPU 编号转换为 OSQ 非零编码
 * @cpu_nr: 当前可用 CPU 编号，调用者通常已关闭抢占
 *
 * 返回 @cpu_nr + 1；无失败与状态修改，结果可安全与 OSQ_UNLOCKED_VAL=0 区分。
 */
static inline int encode_cpu(int cpu_nr)
{
	return cpu_nr + 1;
}

/*
 * node_cpu - 从 percpu 节点取回真实 CPU 编号
 * @node: 已初始化、保持存活的 OSQ 节点
 *
 * 返回 node->cpu - 1，供 vcpu_is_preempted() 等观测；不验证编码，也不取得节点所有权。
 */
static inline int node_cpu(struct optimistic_spin_node *node)
{
	return node->cpu - 1;
}

/*
 * decode_cpu - 由非零 CPU 编码定位永久 percpu OSQ 节点
 * @encoded_cpu_val: tail 或 node 中保存的 CPU+1，必须不是 OSQ_UNLOCKED_VAL
 *
 * 返回对应 osq_node 的借用地址；调用者依赖 OSQ 协议和抢占关闭保证该 CPU 本轮节点状态有效，函数
 * 不增加引用，也不能用于空队列编码。
 */
static inline struct optimistic_spin_node *decode_cpu(int encoded_cpu_val)
{
	int cpu_nr = encoded_cpu_val - 1;

	return per_cpu_ptr(&osq_node, cpu_nr);
}

/*
 * Get a stable @node->next pointer, either for unlock() or unqueue() purposes.
 * Can return NULL in case we were the last queued and we updated @lock instead.
 *
 * If osq_lock() is being cancelled there must be a previous node
 * and 'old_cpu' is its CPU #.
 * For osq_unlock() there is never a previous node and old_cpu is
 * set to OSQ_UNLOCKED_VAL.
 */
/*
 * 为 unlock 或取消取得稳定的 @node->next；若当前是尾节点并已把 @lock 回退，则返回 NULL。
 * osq_lock() 取消时必有前驱，@old_cpu 是其 CPU 编码；osq_unlock() 没有前驱，传空队列值。
 *
 * osq_wait_next - 等待稳定后继，或原子把 tail 从当前节点回退
 * @lock: 当前由调用者拥有的 OSQ
 * @node: 本 CPU 保持有效的节点
 * @old_cpu: 取消时的稳定前驱编码，解锁时为 OSQ_UNLOCKED_VAL
 *
 * 循环先尝试证明自己仍是尾节点并以 acquire cmpxchg 回退 tail；成功返回 NULL，让前驱观察 tail
 * 完成自己的 unlock/unqueue。否则用 xchg 把 next 清空并返回抢到的稳定后继；清空是 Step-A/C
 * 协议的一部分，防止并发后继仍把本节点当有效 prev。函数不睡眠，调用者必须关闭抢占。
 */
static inline struct optimistic_spin_node *
osq_wait_next(struct optimistic_spin_queue *lock,
	      struct optimistic_spin_node *node,
	      int old_cpu)
{
	int curr = encode_cpu(smp_processor_id());

	for (;;) {
		if (atomic_read(&lock->tail) == curr &&
		    atomic_cmpxchg_acquire(&lock->tail, curr, old_cpu) == curr) {
			/*
			 * We were the last queued, we moved @lock back. @prev
			 * will now observe @lock and will complete its
			 * unlock()/unqueue().
			 */
				/*
				 * 当前是最后排队者，已把 tail 回退到 @old_cpu；前驱随后会观察 tail 并完成自己的解锁或取消。
				 */
			return NULL;
		}

		/*
		 * We must xchg() the @node->next value, because if we were to
		 * leave it in, a concurrent unlock()/unqueue() from
		 * @node->next might complete Step-A and think its @prev is
		 * still valid.
		 *
		 * If the concurrent unlock()/unqueue() wins the race, we'll
		 * wait for either @lock to point to us, through its Step-B, or
		 * wait for a new @node->next from its Step-C.
		 */
			/*
			 * 必须用 xchg 取走 node->next；若只读后保留它，并发后继完成 Step-A 时会误以为旧 prev 仍有效。
			 * 若并发 unlock/unqueue 先赢，当前循环要么等其 Step-B 让 tail 指向本节点，要么等 Step-C 写入
			 * 新的 node->next。
			 */
		if (node->next) {
			struct optimistic_spin_node *next;

			next = xchg(&node->next, NULL);
			if (next)
				return next;
		}

		cpu_relax();
	}
}

/*
 * osq_lock - 入队取得 OSQ，或在应调度/前驱停跑时安全取消
 * @lock: 已初始化且在调用到配对解锁期间保持存活的 optimistic spin queue
 *
 * 仅限抢占关闭的任务上下文，本 CPU 独占其 percpu node。函数初始化节点并以全序 xchg 交换 tail：
 * 空队列立即返回 true；有前驱则发布双向链接，在本地 locked 上等交接，同时观察 need_resched 和
 * 前驱 vCPU。交接赢得竞态返回 true；取消则按 A 稳定 prev、B 稳定 next/回退 tail、C 重连邻居，
 * 完成后返回 false。true 出口必须配对 osq_unlock()；false 出口已彻底退队，不得解锁。
 */
bool osq_lock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_node);
	struct optimistic_spin_node *prev, *next;
	int curr = encode_cpu(smp_processor_id());
	int old;
	/*
	 * node 是本 CPU 本轮拥有的永久节点；prev/next 是取消协议稳定的邻居借用指针；curr 为本 CPU+1，
	 * old 是 xchg 返回的旧 tail，决定无竞争成功或前驱节点。
	 */

	node->locked = 0;
	node->next = NULL;
	node->cpu = curr;

	/*
	 * We need both ACQUIRE (pairs with corresponding RELEASE in
	 * unlock() uncontended, or fastpath) and RELEASE (to publish
	 * the node fields we just initialised) semantics when updating
	 * the lock tail.
	 */
	/*
	 * 更新 tail 既需 acquire（与无竞争/快速释放的 release 配对），也需 release（发布刚初始化的节点
	 * 字段），所以使用同时具备两侧排序的 atomic_xchg()。
	 */
	old = atomic_xchg(&lock->tail, curr);
	if (old == OSQ_UNLOCKED_VAL)
		return true;

	prev = decode_cpu(old);
	node->prev = prev;

	/*
	 * osq_lock()			unqueue
	 *
	 * node->prev = prev		osq_wait_next()
	 * WMB				MB
	 * prev->next = node		next->prev = prev // unqueue-C
	 *
	 * Here 'node->prev' and 'next->prev' are the same variable and we need
	 * to ensure these stores happen in-order to avoid corrupting the list.
	 */
	/*
	 * osq_lock 写 node->prev 后再发布 prev->next；并发取消 Step-C 会写同一个 next->prev。WMB 与其
	 * MB 配合，强制两个 store 依序可见，避免双向链被交叉写坏。
	 */
	smp_wmb();

	WRITE_ONCE(prev->next, node);

	/*
	 * Normally @prev is untouchable after the above store; because at that
	 * moment unlock can proceed and wipe the node element from stack.
	 *
	 * However, since our nodes are static per-cpu storage, we're
	 * guaranteed their existence -- this allows us to apply
	 * cmpxchg in an attempt to undo our queueing.
	 */
	/*
	 * 一般链入后前驱就可能解锁，若节点在栈上便不能再触碰；OSQ 节点是永久 percpu 存储，因此仍可
	 * 对前驱字段做 cmpxchg，尝试撤销本次排队。
	 */

	/*
	 * Wait to acquire the lock or cancellation. Note that need_resched()
	 * will come with an IPI, which will wake smp_cond_load_relaxed() if it
	 * is implemented with a monitor-wait. vcpu_is_preempted() relies on
	 * polling, be careful.
	 */
	/*
	 * 等待 ownership 或取消条件。need_resched 通常伴随 IPI，可唤醒 monitor-wait 实现；
	 * vcpu_is_preempted() 依赖轮询，不能假设也有事件唤醒。
	 */
	if (smp_cond_load_relaxed(&node->locked, VAL || need_resched() ||
				  vcpu_is_preempted(node_cpu(node->prev))))
		return true;

	/* unqueue */
	/* 以下开始取消排队。 */
	/*
	 * Step - A  -- stabilize @prev
	 *
	 * Undo our @prev->next assignment; this will make @prev's
	 * unlock()/unqueue() wait for a next pointer since @lock points to us
	 * (or later).
	 */
	/*
	 * Step-A 稳定 prev：撤销 prev->next=node，使前驱因 tail 仍指向本节点或更后节点而等待新的 next。
	 */

	for (;;) {
		/*
		 * cpu_relax() below implies a compiler barrier which would
		 * prevent this comparison being optimized away.
		 */
		/* 下方 cpu_relax() 隐含编译器屏障，防止编译器把本轮比较优化为不再重新读取。 */
		if (data_race(prev->next) == node &&
		    cmpxchg(&prev->next, node, NULL) == node)
			break;

		/*
		 * We can only fail the cmpxchg() racing against an unlock(),
		 * in which case we should observe @node->locked becoming
		 * true.
		 */
		/* cmpxchg 只会因并发 unlock 失败；若对方已交接，acquire 读取 locked 后改走成功出口。 */
		if (smp_load_acquire(&node->locked))
			return true;

		cpu_relax();

		/*
		 * Or we race against a concurrent unqueue()'s step-B, in which
		 * case its step-C will write us a new @node->prev pointer.
		 */
		/* 也可能与另一取消者的 Step-B 竞争；其 Step-C 会为本节点写入新的 prev，故每轮重新加载。 */
		prev = READ_ONCE(node->prev);
	}

	/*
	 * Step - B -- stabilize @next
	 *
	 * Similar to unlock(), wait for @node->next or move @lock from @node
	 * back to @prev.
	 */
	/* Step-B 稳定 next：与 unlock 相同，等待 node->next，或把 tail 从本节点回退到 prev。 */

	next = osq_wait_next(lock, node, prev->cpu);
	if (!next)
		return false;

	/*
	 * Step - C -- unlink
	 *
	 * @prev is stable because its still waiting for a new @prev->next
	 * pointer, @next is stable because our @node->next pointer is NULL and
	 * it will wait in Step-A.
	 */
	/*
	 * Step-C 摘链：prev 正等待新的 prev->next，因此稳定；本节点 next 已清空，使 next 在 Step-A 等待，
	 * 因而可先把 next->prev 改为 prev，再让 prev->next 指向 next，完成绕过本节点。
	 */

	WRITE_ONCE(next->prev, prev);
	WRITE_ONCE(prev->next, next);

	return false;
}

/*
 * osq_unlock - 释放 OSQ，并把排队权交给稳定后继
 * @lock: current CPU 已通过 osq_lock() 成功取得且保持存活的队列
 *
 * 调用者保持抢占关闭并使用本 CPU percpu node。无竞争时 release cmpxchg 把 tail 清零；已看到 next
 * 时以 xchg 取走链接并写其 locked 直接交接；后继尚未链接或正在取消时调用 osq_wait_next()，由它
 * 等到稳定后继或完成 tail 清空。无返回值，所有出口本 CPU 均不再拥有 OSQ，节点可供下一轮复用。
 */
void osq_unlock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node, *next;
	int curr = encode_cpu(smp_processor_id());
	/* node 是本 CPU 永久节点，next 是清链接后得到的稳定后继，curr 是本 CPU+1 tail 编码。 */

	/*
	 * Fast path for the uncontended case.
	 */
	/* 无竞争快速路径：以 release 把唯一尾节点改为空队列。 */
	if (atomic_try_cmpxchg_release(&lock->tail, &curr, OSQ_UNLOCKED_VAL))
		return;

	/*
	 * Second most likely case.
	 */
	/* 次常见路径：后继已经发布，直接取走 next 链接并把 locked 交给它。 */
	node = this_cpu_ptr(&osq_node);
	next = xchg(&node->next, NULL);
	if (next) {
		WRITE_ONCE(next->locked, 1);
		return;
	}

	next = osq_wait_next(lock, node, OSQ_UNLOCKED_VAL);
	if (next)
		WRITE_ONCE(next->locked, 1);
}
