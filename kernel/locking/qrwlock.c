// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Queued read/write locks
 *
 * (C) Copyright 2013-2014 Hewlett-Packard Development Company, L.P.
 *
 * Authors: Waiman Long <waiman.long@hp.com>
 */
/*
 * 排队读写锁慢路径。共享原子 cnts 同时编码 writer 状态和高位 reader 计数，
 * 内部 wait_lock 为普通上下文的 reader/writer 提供公平排队；中断 reader 为避免
 * 与被打断上下文形成自死锁，对仅 WAITING 的 writer 有意采用特殊旁路。
 */
/* SMP/原子与中断接口支撑锁协议，trace 记录读写自旋争用的开始和结束。 */
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/spinlock.h>
#include <trace/events/lock.h>

/**
 * queued_read_lock_slowpath - acquire read lock of a queued rwlock
 * @lock: Pointer to queued rwlock structure
 */
/*
 * queued_read_lock_slowpath() - 获取排队 rwlock 的读锁慢路径
 * @lock: 调用期间有效的 qrwlock 借用指针；公共快路径已经把 reader bias 加入
 *        cnts，但因观察到 writer 状态而未能直接返回。
 *
 * 中断上下文保留已加的 bias，只以 acquire 等实际写持有者离开，不进入内部
 * wait_lock；普通上下文先撤销 bias，再在公平 wait_lock 排队，成为队首后重新
 * 加 bias并等待 writer 解锁。函数不睡眠、无失败返回；返回即持有读锁，内部
 * wait_lock 已释放。它不分配资源或改变 @lock 的 ownership，外层必须配对读解锁。
 */
void __lockfunc queued_read_lock_slowpath(struct qrwlock *lock)
{
	/*
	 * Readers come here when they cannot get the lock without waiting
	 */
	/* reader 无法在公共快路径无等待获锁时进入这里；其读计数此刻仍已加一。 */
	if (unlikely(in_interrupt())) {
		/*
		 * Readers in interrupt context will get the lock immediately
		 * if the writer is just waiting (not holding the lock yet),
		 * so spin with ACQUIRE semantics until the lock is available
		 * without waiting in the queue.
		 */
		/*
		 * 中断 reader 若 writer 只是 WAITING、尚未真正持锁，就应立即保留读锁；
		 * 否则排入 wait_lock 可能阻塞在被本中断打断且正持有/等待内部锁的上下文上。
		 * 因此只以 ACQUIRE 轮询 _QW_LOCKED 清零，不等待 WAITING，也不进入公平队列。
		 * 若 writer 已持锁，acquire 与其 release 解锁配对后再返回。
		 */
		atomic_cond_read_acquire(&lock->cnts, !(VAL & _QW_LOCKED));
		return;
	}
	/* 普通 reader 先撤销公共快路径加过的 bias，避免排队期间阻挡前方 writer。 */
	atomic_sub(_QR_BIAS, &lock->cnts);

	/* 从真正开始等待内部队列起记录一次读自旋争用。 */
	trace_contention_begin(lock, LCB_F_SPIN | LCB_F_READ);

	/*
	 * Put the reader into the wait queue
	 */
	/* 获取公平 wait_lock 成为队首，再重新登记 reader bias，宣告本 reader 将持锁。 */
	arch_spin_lock(&lock->wait_lock);
	atomic_add(_QR_BIAS, &lock->cnts);

	/*
	 * The ACQUIRE semantics of the following spinning code ensure
	 * that accesses can't leak upwards out of our subsequent critical
	 * section in the case that the lock is currently held for write.
	 */
	/*
	 * 下方自旋的 ACQUIRE 语义保证：若当前写锁仍被持有，后续读临界区访问不能
	 * 向上泄漏到写锁释放之前；它与 writer 对 wlocked 的 store-release 配对。
	 * 此时持有 wait_lock，后续普通 reader/writer 不会越过当前队首。
	 */
	atomic_cond_read_acquire(&lock->cnts, !(VAL & _QW_LOCKED));

	/*
	 * Signal the next one in queue to become queue head
	 */
	/* 释放内部锁，通知下一名等待者成为队首；当前 reader 的 bias 保留到外层解锁。 */
	arch_spin_unlock(&lock->wait_lock);

	/* 与本函数的 contention_begin 配对；0 表示已成功取得读锁。 */
	trace_contention_end(lock, 0);
}
/* 供使用通用 qrwlock 快路径的体系结构链接读慢路径。 */
EXPORT_SYMBOL(queued_read_lock_slowpath);

/**
 * queued_write_lock_slowpath - acquire write lock of a queued rwlock
 * @lock : Pointer to queued rwlock structure
 */
/*
 * queued_write_lock_slowpath() - 获取排队 rwlock 的写锁慢路径
 * @lock: 调用期间有效的 qrwlock 借用指针；公共零值 acquire cmpxchg 已失败。
 *
 * 函数先进入公平 wait_lock 队列；成为队首后若 cnts 为空则直接 acquire 获锁，
 * 否则设置 WAITING 阻止后来的普通 reader 插队，等所有 reader/owner 离开后把
 * WAITING 原子换成 LOCKED。无睡眠和失败返回；返回即持有写锁且内部 wait_lock
 * 已释放。锁对象仍归调用者，外层必须配对写解锁。
 */
void __lockfunc queued_write_lock_slowpath(struct qrwlock *lock)
{
	/* cnts 保存锁字快照，也是 try_cmpxchg 失败时的回填值。 */
	int cnts;

	/* writer 进入慢路径即开始记录写自旋争用，所有成功出口在末尾配对结束。 */
	trace_contention_begin(lock, LCB_F_SPIN | LCB_F_WRITE);

	/* Put the writer into the wait queue */
	/* 把 writer 放入公平内部队列；持有后才可宣告 WAITING 并等待前方读者排空。 */
	arch_spin_lock(&lock->wait_lock);

	/* Try to acquire the lock directly if no reader is present */
	/* 若无 reader、writer 状态也为零，直接以 acquire cmpxchg 设置 LOCKED。 */
	if (!(cnts = atomic_read(&lock->cnts)) &&
	    atomic_try_cmpxchg_acquire(&lock->cnts, &cnts, _QW_LOCKED))
		goto unlock;

	/* Set the waiting flag to notify readers that a writer is pending */
	/* 设置 WAITING 通知后来的普通 reader 撤销 bias 并排到本 writer 后面。 */
	atomic_or(_QW_WAITING, &lock->cnts);

	/* When no more readers or writers, set the locked flag */
	/*
	 * relaxed 等到锁字只剩 WAITING，即既无高位 reader 计数也无写持有者；随后
	 * acquire cmpxchg 把 WAITING 换成 LOCKED。若中断 reader 在等待期间临时加入，
	 * cnts 会再次含 reader bias，比较交换失败并回填新值，循环继续直到它 release。
	 */
	do {
		cnts = atomic_cond_read_relaxed(&lock->cnts, VAL == _QW_WAITING);
	} while (!atomic_try_cmpxchg_acquire(&lock->cnts, &cnts, _QW_LOCKED));
unlock:
	/* 两条获取路径都已持有写锁，现释放内部排队锁让后续等待者开始观察。 */
	arch_spin_unlock(&lock->wait_lock);

	/* 与 begin 配对；0 表示写锁获取成功。 */
	trace_contention_end(lock, 0);
}
/* 供使用通用 qrwlock 快路径的体系结构链接写慢路径。 */
EXPORT_SYMBOL(queued_write_lock_slowpath);
