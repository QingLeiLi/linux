/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Authors: Waiman Long <longman@redhat.com>
 */

/*
 * 本文件是锁事件的 X-macro 目录，故意不设置 include guard：
 * lock_events.h 以默认展开生成 enum lock_events，lock_events.c 再把
 * LOCK_EVENT() 重定义为指定初始化器并重复包含，从同一顺序生成
 * debugfs 文件名。调用点最终用 LOCKEVENT_<name> 索引每 CPU 计数器；
 * 因而条目的名字、顺序和条件编译共同构成跨包含点的编号契约。
 *
 * 这里仅声明统计槽，不取得锁、引用或对象所有权，也不直接更新计数。
 * CONFIG_LOCK_EVENT_COUNTS 关闭时，更新宏在 lock_events.h 中退化为空操作；
 * 某类锁未编译时，本文件的同一配置条件会连同其枚举槽一起移除。
 */

#ifndef LOCK_EVENT
#define LOCK_EVENT(name)	LOCKEVENT_ ## name,
#endif

/*
 * 包含者没有自定义 LOCK_EVENT() 时，默认把 name 通过 ## 与前缀拼接，
 * 生成一个 LOCKEVENT_name 枚举项。已经自定义时则保留调用方的展开方式；
 * 本文件不在结尾撤销该宏，宏的定义域由包含者负责管理。
 */

#ifdef CONFIG_QUEUED_SPINLOCKS
#ifdef CONFIG_PARAVIRT_SPINLOCKS
/*
 * Locking events for PV qspinlock.
 */
/*
 * 以下是半虚拟化排队自旋锁的事件。只有排队自旋锁和 PV 自旋锁同时
 * 启用时才分配这些编号，避免原生锁构建产生永远不会更新的 PV 槽。
 */
LOCK_EVENT(pv_hash_hops)	/* Average # of hops per hashing operation */
/* 读取时呈现每次哈希操作的平均探测跳数；槽内先累计总跳数，再除以 pv_kick_unlock。 */
LOCK_EVENT(pv_kick_unlock)	/* # of vCPU kicks issued at unlock time   */
/* 统计解锁路径为交接锁而发出的 vCPU kick 次数，也是 kick 延迟和哈希跳数的分母。 */
LOCK_EVENT(pv_kick_wake)	/* # of vCPU kicks for pv_latency_wake	   */
/* 统计确实为等待者留下时间戳的 kick-to-wakeup 样本数，作为唤醒延迟的分母。 */
LOCK_EVENT(pv_latency_kick)	/* Average latency (ns) of vCPU kick	   */
/* 读取时呈现一次 vCPU kick 的平均延迟（纳秒）；槽内累计 pv_kick() 调用耗时。 */
LOCK_EVENT(pv_latency_wake)	/* Average latency (ns) of kick-to-wakeup  */
/* 读取时呈现从 kick 到目标 vCPU 返回等待路径的平均延迟（纳秒）；槽内累计样本总耗时。 */
LOCK_EVENT(pv_lock_stealing)	/* # of lock stealing operations	   */
/* 统计队头声明 pending 前，后来者通过原子操作直接取得锁的成功次数。 */
LOCK_EVENT(pv_spurious_wakeup)	/* # of spurious wakeups in non-head vCPUs */
/* 统计非队头 vCPU 从 pv_wait() 返回后仍未收到 MCS 交接的次数，反映无效唤醒。 */
LOCK_EVENT(pv_wait_again)	/* # of wait's after queue head vCPU kick  */
/* 统计队头 vCPU 被 kick 过后仍再次进入休眠等待的次数，而不是所有队头等待次数。 */
LOCK_EVENT(pv_wait_early)	/* # of early vCPU wait's		   */
/* 统计非队头节点因前驱 vCPU 已不运行而提前停止自旋、转入 PV 等待的次数。 */
LOCK_EVENT(pv_wait_head)	/* # of vCPU wait's at the queue head	   */
/* 统计队头 vCPU 在锁仍忙时进入 PV 等待的次数；一次竞争可能因重复等待而累计多次。 */
LOCK_EVENT(pv_wait_node)	/* # of vCPU wait's at non-head queue node */
/* 统计非队头 MCS 节点进入 PV 等待的次数，用于观察队列中部的休眠压力。 */
#endif /* CONFIG_PARAVIRT_SPINLOCKS */

/*
 * Locking events for qspinlock
 *
 * Subtracting lock_use_node[234] from lock_slowpath will give you
 * lock_use_node1.
 */
/*
 * 以下是原生与 PV qspinlock 共用的排队事件。
 * lock_use_node1 没有独立槽：用 lock_slowpath 减去 node2、node3、node4
 * 的次数即可得到，这是上面英文注释表达的原意。
 *
 * 修正说明：当前 qspinlock.c 与 kernel/bpf/rqspinlock.c 都先递增
 * lock_slowpath，随后才检查 idx 是否超过可用节点；节点耗尽时还会递增
 * lock_no_node。因此精确的 node1 次数还应减去 lock_no_node。该路径极少
 * 发生，但不能在解释编号关系时把它当成已经选择了第一个 MCS 节点。
 */
LOCK_EVENT(lock_pending)	/* # of locking ops via pending code	     */
/* 统计通过 pending 位等待并取得锁的操作次数，表示未进入 MCS 队列的中度竞争路径。 */
LOCK_EVENT(lock_slowpath)	/* # of locking ops via MCS lock queue	     */
/* 统计进入 MCS 排队阶段的加锁操作；其中也包括随后因节点耗尽而直接自旋/失败的次数。 */
LOCK_EVENT(lock_use_node2)	/* # of locking ops that use 2nd percpu node */
/* 统计同一 CPU 嵌套加锁时使用第二个 percpu MCS 节点的次数。 */
LOCK_EVENT(lock_use_node3)	/* # of locking ops that use 3rd percpu node */
/* 统计使用第三个 percpu MCS 节点的次数；必须紧随 node2 以支持编号算术。 */
LOCK_EVENT(lock_use_node4)	/* # of locking ops that use 4th percpu node */
/* 统计使用第四个 percpu MCS 节点的次数；node2～node4 的枚举值必须连续且同序。 */
LOCK_EVENT(lock_no_node)	/* # of locking ops w/o using percpu node    */
/* 统计嵌套深度耗尽可用节点、因而不能进入正常 MCS 排队协议的操作次数。 */
#endif /* CONFIG_QUEUED_SPINLOCKS */

/*
 * Locking events for Resilient Queued Spin Lock
 */
/* 以下事件属于 resilient qspinlock；它独立于上面的普通 qspinlock 配置块保留编号。 */
LOCK_EVENT(rqspinlock_lock_timeout)	/* # of locking ops that timeout	*/
/* 统计 resilient qspinlock 加锁等待超过期限的操作次数，调用者据此识别超时恢复路径。 */

/*
 * Locking events for rwsem
 */
/*
 * 以下事件刻画读写信号量慢路径中的休眠、唤醒、乐观自旋、偷锁和强制交接。
 * 它们是观测计数而非同步原语：更新使用允许偶发丢失的 percpu 操作，不参与
 * sem->count、wait_lock 或 waiter 生命周期的正确性保证。
 */
LOCK_EVENT(rwsem_sleep_reader)	/* # of reader sleeps			*/
/* 统计读者慢路径实际调用调度器让出 CPU 的次数；同一读者可循环休眠多次。 */
LOCK_EVENT(rwsem_sleep_writer)	/* # of writer sleeps			*/
/* 统计写者慢路径实际调度休眠的次数；handoff 后立即重试而未休眠不计入。 */
LOCK_EVENT(rwsem_wake_reader)	/* # of reader wakeups			*/
/* 统计非空读者批次被标记唤醒的次数；一次可覆盖多个读者，并非逐 task 计数。 */
LOCK_EVENT(rwsem_wake_writer)	/* # of writer wakeups			*/
/* 统计队首写者被加入 wake_q 的次数；真正唤醒在释放 wait_lock 后由调用者完成。 */
LOCK_EVENT(rwsem_opt_lock)	/* # of opt-acquired write locks	*/
/* 统计写者尚未入等待队列时，通过 cmpxchg 乐观取得写锁的成功次数。 */
LOCK_EVENT(rwsem_opt_fail)	/* # of failed optspins			*/
/* 统计因需调度、owner 不可自旋或最终未取锁而终止乐观自旋的失败次数。 */
LOCK_EVENT(rwsem_opt_nospin)	/* # of disabled optspins		*/
/* 统计写者针对读者持锁阶段自旋超时后，把 rwsem 标记为 NONSPINNABLE 的次数。 */
LOCK_EVENT(rwsem_rlock)		/* # of read locks acquired		*/
/* 统计读者经排队/等待慢路径最终取得读锁的次数；偷锁和队列为空快速返回另有槽。 */
LOCK_EVENT(rwsem_rlock_steal)	/* # of read locks by lock stealing	*/
/* 统计读者在无 writer/handoff 阻挡时从慢路径直接偷取读锁的次数。 */
LOCK_EVENT(rwsem_rlock_fast)	/* # of fast read locks acquired	*/
/* 统计读者发现等待队列为空且无写者持锁，补足 acquire 语义后立即返回的次数。 */
LOCK_EVENT(rwsem_rlock_fail)	/* # of failed read lock acquisitions	*/
/* 统计可中断读等待因信号退出、未取得读锁的次数。 */
LOCK_EVENT(rwsem_rlock_handoff)	/* # of read lock handoffs		*/
/* 统计读者等待超时后首次请求设置 HANDOFF、迫使当前写者交接的次数。 */
LOCK_EVENT(rwsem_wlock)		/* # of write locks acquired		*/
/* 统计写者经排队慢路径最终取得写锁的次数；入口前的乐观成功由 rwsem_opt_lock 记录。 */
LOCK_EVENT(rwsem_wlock_fail)	/* # of failed write lock acquisitions	*/
/* 统计可中断写等待因信号退出、从队列撤销且未取得写锁的次数。 */
LOCK_EVENT(rwsem_wlock_handoff)	/* # of write lock handoffs		*/
/* 统计队首写者因实时优先级或等待过久而设置 HANDOFF、要求禁止继续偷锁的次数。 */

/*
 * Locking events for rtlock_slowlock()
 */
/* 以下事件把 PREEMPT_RT 的 rtlock 慢路径拆成入口、两处成功点、休眠和延迟唤醒。 */
LOCK_EVENT(rtlock_slowlock)	/* # of rtlock_slowlock() calls		*/
/* 统计 rtlock_slowlock_locked() 在持有 wait_lock 时进入慢路径的次数。 */
LOCK_EVENT(rtlock_slow_acq1)	/* # of locks acquired after wait_lock	*/
/* 统计进入后首次 try_to_take_rt_mutex() 就成功、尚未挂入 waiter 的次数。 */
LOCK_EVENT(rtlock_slow_acq2)	/* # of locks acquired in for loop	*/
/* 统计已经建立 waiter 后，在重试循环中成功取得 rtlock 的次数。 */
LOCK_EVENT(rtlock_slow_sleep)	/* # of sleeps				*/
/* 统计无法在 owner 上乐观自旋、因而调用 schedule_rtlock() 的实际休眠次数。 */
LOCK_EVENT(rtlock_slow_wake)	/* # of wakeup's			*/
/* 统计慢路径结束时 wake_q 非空的次数，表示本次操作还携带待调用者执行的延迟唤醒。 */

/*
 * Locking events for rt_mutex_slowlock()
 */
/* 以下事件对应可睡眠 rt_mutex 慢路径，并区分外层准备、内层阻塞循环和错误处理。 */
LOCK_EVENT(rtmutex_slowlock)	/* # of rt_mutex_slowlock() calls	*/
/* 统计 __rt_mutex_slowlock() 在持有 wait_lock 时被调用的次数。 */
LOCK_EVENT(rtmutex_slow_block)	/* # of rt_mutex_slowlock_block() calls	*/
/* 统计进入 __rt_mutex_slowlock_block() 阻塞/重试 helper 的次数。 */
LOCK_EVENT(rtmutex_slow_acq1)	/* # of locks acquired after wait_lock	*/
/* 统计外层慢路径首次重试便取得 rt_mutex、无需建立 waiter 的次数。 */
LOCK_EVENT(rtmutex_slow_acq2)	/* # of locks acquired at the end	*/
/* 统计阻塞 helper 成功返回后，由外层慢路径确认完成获取的次数。 */
LOCK_EVENT(rtmutex_slow_acq3)	/* # of locks acquired in *block()	*/
/* 统计已经建立 waiter 后，在阻塞 helper 的循环内成功取得 rt_mutex 的次数。 */
LOCK_EVENT(rtmutex_slow_sleep)	/* # of sleeps				*/
/* 统计既未取得锁也无法继续在 owner 上自旋，实际调用 rt_mutex_schedule() 的次数。 */
LOCK_EVENT(rtmutex_slow_wake)	/* # of wakeup's			*/
/* 统计慢路径返回时 wake_q 非空的次数；它观测待执行的延迟唤醒，而非当前任务被唤醒次数。 */
LOCK_EVENT(rtmutex_deadlock)	/* # of rt_mutex_handle_deadlock()'s	*/
/*
 * 统计失败出口调用 rt_mutex_handle_deadlock() 的次数。当前调用点在 ret 非零时
 * 统一执行，因此该槽记录处理器调用，不应直接等同于已经确认的死锁环数量。
 */

/*
 * Locking events for lockdep
 */
/*
 * 以下事件观察 lockdep 的获取检查入口、依赖图串行化和跳过证明的子路径。
 * 它们只在 CONFIG_LOCK_EVENT_COUNTS 启用时产生低开销 percpu 统计，不改变
 * lockdep 是否验证一把锁，也不替代 lockdep 自身的递归保护和图锁。
 */
LOCK_EVENT(lockdep_acquire)
/* 统计 __lock_acquire() 接受一个需要跟踪的锁获取事件的次数。 */
LOCK_EVENT(lockdep_lock)
/* 统计 graph_lock() 为检查或修改依赖图而取得内部 lockdep 图锁的次数。 */
LOCK_EVENT(lockdep_nocheck)
/* 统计 acquire 入口因 prove_locking 关闭或锁标记为 no-validate 而跳过依赖证明的次数。 */
