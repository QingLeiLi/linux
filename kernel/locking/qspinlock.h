/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Queued spinlock defines
 *
 * This file contains macro definitions and functions shared between different
 * qspinlock slow path implementations.
 */
/*
 * 排队自旋锁定义
 *
 * 本文件集中放置不同 qspinlock 慢路径实现共享的宏、队列节点包装和短小 helper。
 * 它不实现完整慢路径，而是约定 32 位锁字与 percpu MCS 节点之间如何编码、定位
 * 和转换；原生与半虚拟化实现因而可以复用同一套状态表示。
 */
#ifndef __LINUX_QSPINLOCK_H
#define __LINUX_QSPINLOCK_H

/*
 * percpu 接口提供按 CPU 定位节点数组的能力；通用 qspinlock 头提供 32 位锁字
 * 布局与原子类型，通用 MCS 头提供嵌入节点。include guard 只约束定义次数，
 * 实际节点存储由包含者 qspinlock.c 定义并管理。
 */
#include <asm-generic/percpu.h>
#include <linux/percpu-defs.h>
#include <asm-generic/qspinlock.h>
#include <asm-generic/mcs_spinlock.h>

/*
 * 同一 CPU 最多为 task、softirq、hardirq、NMI 四种嵌套上下文各占一个节点。
 * 该常量同时受尾码中 2 位索引宽度约束；慢路径若罕见地耗尽四槽，会退化为
 * 直接轮询锁字，而不是越界访问 percpu 数组。
 */
#define _Q_MAX_NODES	4

/*
 * The pending bit spinning loop count.
 * This heuristic is used to limit the number of lockword accesses
 * made by atomic_cond_read_relaxed when waiting for the lock to
 * transition out of the "== _Q_PENDING_VAL" state. We don't spin
 * indefinitely because there's no guarantee that we'll make forward
 * progress.
 */
/*
 * pending 位自旋循环次数。
 * 这个启发式上限约束 atomic_cond_read_relaxed() 等待锁字离开
 * “== _Q_PENDING_VAL”状态时的访问次数。这里不无限自旋，因为不能保证该等待
 * 一定取得前进；超过上限后慢路径会重新检查并转入能够排队的流程。
 *
 * 体系结构可在包含本文件前覆盖该值；默认只尝试一次，x86 当前覆盖为 512 次。
 */
#ifndef _Q_PENDING_LOOPS
#define _Q_PENDING_LOOPS	1
#endif

/*
 * On 64-bit architectures, the mcs_spinlock structure will be 16 bytes in
 * size and four of them will fit nicely in one 64-byte cacheline. For
 * pvqspinlock, however, we need more space for extra data. To accommodate
 * that, we insert two more long words to pad it up to 32 bytes. IOW, only
 * two of them can fit in a cacheline in this case. That is OK as it is rare
 * to have more than 2 levels of slowpath nesting in actual use. We don't
 * want to penalize pvqspinlocks to optimize for a rare case in native
 * qspinlocks.
 */
/*
 * 在 64 位体系结构上，mcs_spinlock 通常占 16 字节，四个节点恰好装入一个
 * 64 字节 cacheline。pvqspinlock 还需要额外状态空间，因此这里增加两个 long
 * 填充到 32 字节，每个 cacheline 只能容纳两个。实际运行中慢路径嵌套超过两层
 * 很少发生，故接受 PV 多占一个 cacheline，避免为了原生 qspinlock 的罕见情形
 * 让所有 PV 节点承担更差布局。
 *
 * mcs 必须位于首字段：调用点把 `&qnodes[0].mcs` 当作基址，再由
 * grab_mcs_node() 转回 qnode 做带正确步长的索引。reserved 只提供 PV 私有存储；
 * 本头不解释其内容，也不单独分配或释放节点。
 */
struct qnode {
	/* 排队、链接和本地等待所需的基础 MCS 节点，也是对外计算节点地址的锚点。 */
	struct mcs_spinlock mcs;
#ifdef CONFIG_PARAVIRT_SPINLOCKS
	/* 半虚拟化慢路径专用的两个机器字；原生构建不为它们付出空间。 */
	long reserved[2];
#endif
};

/*
 * We must be able to distinguish between no-tail and the tail at 0:0,
 * therefore increment the cpu number by one.
 */
/*
 * 必须区分“没有队尾”和 CPU 0、索引 0 的队尾，因此编码时把 CPU 号加一。
 * 全零尾字段由此只表示空队列；解码时必须对 CPU 字段减一恢复原编号。
 */

/*
 * encode_tail() - 把 CPU 与该 CPU 的嵌套节点索引编码为锁字尾字段
 * @cpu: 当前节点所属的有效逻辑 CPU 编号；须能放入 _Q_TAIL_CPU_BITS。
 * @idx: 该 CPU 的节点槽索引，范围为 0..3，须能放入 _Q_TAIL_IDX_BITS。
 *
 * 纯计算函数，不访问共享状态、不取得引用。返回已位移到 qspinlock 锁字正确
 * 位置的非零 u32 尾码；调用者随后把它交给 xchg_tail() 发布。
 */
static inline __pure u32 encode_tail(int cpu, int idx)
{
	/* tail 只承载高位队尾编码，不包含 locked 或 pending 状态。 */
	u32 tail;

	/* CPU 加一后放入高位，保证 CPU0/idx0 不会与空尾码混淆。 */
	tail  = (cpu + 1) << _Q_TAIL_CPU_OFFSET;
	/* 嵌套槽占两位；调用者负责保证 idx 小于四。 */
	tail |= idx << _Q_TAIL_IDX_OFFSET; /* assume < 4 */
	/* 假定小于 4；这是尾码格式约束，函数本身不做运行时范围检查。 */

	return tail;
}

/*
 * decode_tail() - 把非零队尾码还原为对应 CPU 的 MCS 节点
 * @tail: 从锁字取得、包含 CPU+1 与嵌套索引的队尾字段；不得是无尾的零值。
 * @qnodes: 每 CPU qnode 四槽数组的 percpu 基址，生命周期覆盖返回指针的使用。
 *
 * 纯地址计算函数，不验证 CPU/索引、不增加引用。返回由 percpu 存储拥有的节点
 * 借用指针；调用者仅在排队协议保证该槽仍属于前驱时访问它。
 */
static inline __pure struct mcs_spinlock *decode_tail(u32 tail,
						      struct qnode __percpu *qnodes)
{
	/* CPU 字段编码时加过一；零尾码在此会得到 -1，因此必须由调用者先排除。 */
	int cpu = (tail >> _Q_TAIL_CPU_OFFSET) - 1;
	/* 掩掉其他锁字状态后，把两位嵌套索引移回低位。 */
	int idx = (tail &  _Q_TAIL_IDX_MASK) >> _Q_TAIL_IDX_OFFSET;

	/* per_cpu_ptr 选择目标 CPU，再取该槽内嵌的 mcs；返回后存储仍属 percpu 数组。 */
	return per_cpu_ptr(&qnodes[idx].mcs, cpu);
}

/*
 * grab_mcs_node() - 从当前 CPU 的首个 MCS 节点定位指定嵌套槽
 * @base: `qnodes[0].mcs` 的有效借用指针；mcs 必须保持为 qnode 的首字段。
 * @idx: 当前 CPU 的槽索引，正常范围为 0..3，调用者在调用前处理耗尽情形。
 *
 * 返回同一 percpu qnode 数组内第 @idx 个元素的 mcs 地址；不访问字段、不改变
 * count，也不转移 ownership。强制转换的目的，是按完整 qnode 大小而非仅按
 * struct mcs_spinlock 大小前进，尤其要跨过 PV reserved 填充。
 */
static inline __pure
struct mcs_spinlock *grab_mcs_node(struct mcs_spinlock *base, int idx)
{
	return &((struct qnode *)base + idx)->mcs;
}

/* 同时覆盖 locked 与 pending 位，供整字更新时保留这两个低位状态。 */
#define _Q_LOCKED_PENDING_MASK (_Q_LOCKED_MASK | _Q_PENDING_MASK)

/*
 * pending 占完整字节时，locked/pending/tail 分别可用 8/8/16 位访问；本分支
 * 利用不重叠字段避免不必要的 32 位 cmpxchg。下列状态三元组统一写作
 * (tail, pending, locked)，星号表示该 helper 不改变或不约束该部分。
 */
#if _Q_PENDING_BITS == 8
/**
 * clear_pending - clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,* -> *,0,*
 */
/*
 * clear_pending() - 清除 pending 位
 * @lock: 有效 qspinlock 借用指针；调用者必须拥有自己设置的 pending 状态。
 *
 * 状态从 (*,1,*) 变为 (*,0,*)，tail 和 locked 字段保持不变。单次 WRITE_ONCE
 * 只发布字段更新，不提供 acquire/release 临界区语义；该 helper 用于 pending
 * 快路径发现竞争后撤销自己的标记，无返回值、无资源获取，也不改变节点生命期。
 */
static __always_inline void clear_pending(struct qspinlock *lock)
{
	WRITE_ONCE(lock->pending, 0);
}

/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,0 -> *,0,1
 *
 * Lock stealing is not allowed if this function is used.
 */
/*
 * clear_pending_set_locked() - 取得锁所有权并同时清除 pending 位
 * @lock: 有效 qspinlock 借用指针；调用者已持有 pending 且已以 acquire 等待
 *        当前 locked 清零。
 *
 * 状态从 (*,1,0) 一步变为 (*,0,1)，16 位 WRITE_ONCE 保留 tail。使用本函数时
 * 不允许偷锁：它按进入条件假定 locked 为 0，并覆盖整个 locked_pending 半字；
 * 并发偷锁会破坏这个状态转换。函数无返回值，本身不提供完整屏障，获取顺序来自
 * 调用者此前的 acquire 操作。
 */
static __always_inline void clear_pending_set_locked(struct qspinlock *lock)
{
	WRITE_ONCE(lock->locked_pending, _Q_LOCKED_VAL);
}

/*
 * xchg_tail - Put in the new queue tail code word & retrieve previous one
 * @lock : Pointer to queued spinlock structure
 * @tail : The new queue tail code word
 * Return: The previous queue tail code word
 *
 * xchg(lock, tail), which heads an address dependency
 *
 * p,*,* -> n,*,* ; prev = xchg(lock, node)
 */
/*
 * xchg_tail() - 写入新队尾码并取回旧队尾码
 * @lock: 共享 qspinlock 的有效借用指针。
 * @tail: encode_tail() 生成、已位移到锁字高位的新队尾码。
 * Return: 更新前的队尾码，仍位于 u32 的 _Q_TAIL_MASK 位置；零表示原队列无尾。
 *
 * 状态从 (p,*,*) 原子变为 (n,*,*)，locked 与 pending 两个字节不受影响。
 * 返回的 p 随后被 decode_tail() 转成前驱地址，因此该 xchg 位于地址依赖链头。
 * 调用者已在发布前完成 MCS 节点初始化和写屏障，所以这里可用 relaxed 语义；
 * helper 不单独取得锁，也不接管节点所有权。
 */
static __always_inline u32 xchg_tail(struct qspinlock *lock, u32 tail)
{
	/*
	 * We can use relaxed semantics since the caller ensures that the
	 * MCS node is properly initialized before updating the tail.
	 */
	/*
	 * 可以使用 relaxed 语义，因为调用者保证在更新队尾之前已经正确初始化
	 * MCS 节点。16 位交换只访问 tail 半字；返回值左移回统一的 u32 锁字位置。
	 */
	return (u32)xchg_relaxed(&lock->tail,
				 tail >> _Q_TAIL_OFFSET) << _Q_TAIL_OFFSET;
}

#else /* _Q_PENDING_BITS == 8 */
/*
 * pending 只有一位时，tail 紧邻其后，不能再用独立 pending 字节或 tail 半字
 * 访问而不碰到其他字段。本分支因此围绕完整 32 位 atomic_t 做原子 RMW，
 * 每次更新都显式保留本 helper 不负责的锁字部分。
 */

/**
 * clear_pending - clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,* -> *,0,*
 */
/*
 * clear_pending() - 清除 pending 位
 * @lock: 有效 qspinlock 借用指针；调用者必须拥有自己设置的 pending 状态。
 *
 * atomic_andnot() 对完整锁字原子清掉 _Q_PENDING_VAL，使 (*,1,*) 变为
 * (*,0,*)，其余位保持原值。这避免与相邻 tail 位的混合宽度访问；函数无返回，
 * 不取得锁和节点，不单独承担 acquire/release 排序。
 */
static __always_inline void clear_pending(struct qspinlock *lock)
{
	atomic_andnot(_Q_PENDING_VAL, &lock->val);
}

/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,0 -> *,0,1
 */
/*
 * clear_pending_set_locked() - 取得锁所有权并同时清除 pending 位
 * @lock: 有效 qspinlock 借用指针；进入时状态须满足 pending=1、locked=0，且
 *        调用者已通过此前 acquire 等待获得访问临界区所需的内存序。
 *
 * 原子加上 (-_Q_PENDING_VAL + _Q_LOCKED_VAL)，把 (*,1,0) 转成 (*,0,1)，
 * 并保持 tail 不变。该算术依赖上述位值前置条件，不能作为任意状态的设置接口；
 * 函数无返回、不管理节点，本身不替代调用者的 acquire 操作。
 */
static __always_inline void clear_pending_set_locked(struct qspinlock *lock)
{
	atomic_add(-_Q_PENDING_VAL + _Q_LOCKED_VAL, &lock->val);
}

/**
 * xchg_tail - Put in the new queue tail code word & retrieve previous one
 * @lock : Pointer to queued spinlock structure
 * @tail : The new queue tail code word
 * Return: The previous queue tail code word
 *
 * xchg(lock, tail)
 *
 * p,*,* -> n,*,* ; prev = xchg(lock, node)
 */
/*
 * xchg_tail() - 写入新队尾码并取回更新前的锁字
 * @lock: 共享 qspinlock 的有效借用指针。
 * @tail: encode_tail() 生成、只包含新 tail 字段的 u32 值。
 * Return: 成功交换前的完整 u32 锁字；调用者以 _Q_TAIL_MASK 解释其中旧尾码。
 *
 * 原文把返回值称为“previous queue tail code word”；本分支因整字 cmpxchg 实际
 * 还可能返回旧 locked/pending 位，而 8 位 pending 分支会把低位清零。公共调用点
 * 只依赖旧 tail，因此两种返回形态兼容。循环把 (p,*,*) 变为 (n,*,*)，每次失败
 * 后以原子原语回填的最新 old 重新保留 locked/pending，防止覆盖并发低位更新。
 */
static __always_inline u32 xchg_tail(struct qspinlock *lock, u32 tail)
{
	/* old 是当前比较值兼失败时的新快照；new 拼接最新低位与调用者的新 tail。 */
	u32 old, new;

	/* 初始快照可以马上失效，真正一致性由下方 try_cmpxchg 循环保证。 */
	old = atomic_read(&lock->val);
	do {
		/* 丢弃旧 tail，只保留本 helper 不负责的 locked/pending 位。 */
		new = (old & _Q_LOCKED_PENDING_MASK) | tail;
		/*
		 * We can use relaxed semantics since the caller ensures that
		 * the MCS node is properly initialized before updating the
		 * tail.
		 */
		/*
		 * 可以使用 relaxed 语义，因为调用者保证在更新队尾之前已经正确初始化
		 * MCS 节点。发布初始化的顺序由调用点在进入本 helper 前的 smp_wmb()
		 * 提供；比较交换这里只负责锁字更新的原子性和失败重试。
		 */
	} while (!atomic_try_cmpxchg_relaxed(&lock->val, &old, new));

	/* 成功时 old 仍是被 new 替换的完整旧锁字，ownership 不随返回值转移。 */
	return old;
}
#endif /* _Q_PENDING_BITS == 8 */

/**
 * queued_fetch_set_pending_acquire - fetch the whole lock value and set pending
 * @lock : Pointer to queued spinlock structure
 * Return: The previous lock value
 *
 * *,*,* -> *,1,*
 */
/*
 * queued_fetch_set_pending_acquire() - 读取整个锁字并设置 pending 位
 * @lock: 共享 qspinlock 的有效借用指针。
 * Return: OR 更新前的完整 32 位锁值，供慢路径判断 locked、pending 与 tail。
 *
 * 状态从 (*,*,*) 原子变为 (*,1,*)，已有 pending 时保持为 1。acquire 语义
 * 约束成功 pending 路径后续访问；函数不保证调用者最终获锁，若返回值显示已有
 * 竞争，调用者须撤销自己新设的 pending（若确由自己从 0 设为 1）并转入队列。
 * 体系结构可预先定义同名宏和实现覆盖此默认原子 OR。
 */
#ifndef queued_fetch_set_pending_acquire
static __always_inline u32 queued_fetch_set_pending_acquire(struct qspinlock *lock)
{
	return atomic_fetch_or_acquire(_Q_PENDING_VAL, &lock->val);
}
#endif

/**
 * set_locked - Set the lock bit and own the lock
 * @lock: Pointer to queued spinlock structure
 *
 * *,*,0 -> *,0,1
 */
/*
 * set_locked() - 设置 locked 位并取得锁
 * @lock: 共享 qspinlock 的有效借用指针；调用者已是队头，并已以 acquire 方式
 *        等到 locked=0 且 pending=0。
 *
 * 在上述前置条件下，状态表现为 (*,*,0) 到 (*,0,1)。代码只写 locked 字节，
 * 并不会主动清除 pending；三元组中的 pending=0 来自调用点等待条件。该写保留
 * tail，使后继仍能排队；无返回、不管理节点，也不提供完整内存屏障。
 */
static __always_inline void set_locked(struct qspinlock *lock)
{
	WRITE_ONCE(lock->locked, _Q_LOCKED_VAL);
}

#endif /* __LINUX_QSPINLOCK_H */
