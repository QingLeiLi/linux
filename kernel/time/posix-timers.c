// SPDX-License-Identifier: GPL-2.0+
/*
 * 2002-10-15  Posix Clocks & timers
 *                           by George Anzinger george@mvista.com
 *			     Copyright (C) 2002 2003 by MontaVista Software.
 *
 * 2004-06-01  Fix CLOCK_REALTIME clock/timer TIMER_ABSTIME bug.
 *			     Copyright (C) 2004 Boris Hu
 *
 * These are all the functions necessary to implement POSIX clocks & timers
 */
/*
 * 学习总览：本文件是 POSIX clock/timer 通用核心。静态 clockid 经 k_clock 表分派，负 id 则路由到 CPU 或
 * fd 动态时钟；每个进程的 timer 以 signal_struct+非负 timer id 哈希，RCU 提供查找生命周期，bucket 锁
 * 管哈希成员，it_lock 管单 timer 状态，sighand siglock 管进程链表与 sigqueue 交接。周期 timer 不在到期
 * callback 立即重装，而在信号实际递送时按 sequence 校验后推进，避免标准信号合并造成触发风暴。
 * timer 对象由 rcuref、预分配 sigqueue 引用和最终 kfree_rcu 共同收尾；原作者、修复历史与许可见上方保留块。
 */
#include <linux/compat.h>
#include <linux/compiler.h>
#include <linux/init.h>
#include <linux/jhash.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/nospec.h>
#include <linux/posix-clock.h>
#include <linux/posix-timers.h>
#include <linux/prctl.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/time.h>
#include <linux/time_namespace.h>
#include <linux/uaccess.h>

#include "timekeeping.h"
#include "posix-timers.h"

/*
 * Timers are managed in a hash table for lockless lookup. The hash key is
 * constructed from current::signal and the timer ID and the timer is
 * matched against current::signal and the timer ID when walking the hash
 * bucket list.
 *
 * This allows checkpoint/restore to reconstruct the exact timer IDs for
 * a process.
 */
/*
 * timer_hash_bucket 的 lock 保护 head 增删与 ID 唯一性复核；RCU 读者可无锁遍历。__timer_data 在 core init
 * 后只读，保存 2 的幂 bucket 数组、mask 与 k_itimer slab cache，并显式按 4*sizeof(long) 边界布置。
 * 哈希同时混入 signal_struct 指针和 timer id，使相同数字 ID 可在不同线程组独立存在，并支持 CRIU 精确恢复。
 */
struct timer_hash_bucket {
	spinlock_t		lock;
	struct hlist_head	head;
};

static struct {
	struct timer_hash_bucket	*buckets;
	unsigned long			mask;
	struct kmem_cache		*cache;
} __timer_data __ro_after_init __aligned(4*sizeof(long));

#define timer_buckets		(__timer_data.buckets)
#define timer_hashmask		(__timer_data.mask)
#define posix_timers_cache	(__timer_data.cache)

static const struct k_clock * const posix_clocks[];
static const struct k_clock *clockid_to_kclock(const clockid_t id);
static const struct k_clock clock_realtime, clock_monotonic;

#define TIMER_ANY_ID		INT_MIN
/* TIMER_ANY_ID 仅是内核“自动分配”哨兵；可见 timer_t 合法范围固定为 0..INT_MAX。 */

/* SIGEV_THREAD_ID cannot share a bit with the other SIGEV values. */
/* 编译期保证 THREAD_ID 扩展位不与 SIGNAL/NONE/THREAD 基本取值重叠，否则 switch 连续分支无法可靠解码。 */
#if SIGEV_THREAD_ID != (SIGEV_THREAD_ID & \
			~(SIGEV_SIGNAL | SIGEV_NONE | SIGEV_THREAD))
#error "SIGEV_THREAD_ID must not share bit with other SIGEV values!"
#endif

static struct k_itimer *lock_timer(timer_t timer_id);
/* unlock_timer() 是 cleanup class 的释放器；成功 lookup 返回时持 irq-disabled it_lock，空指针无需动作。 */
static inline void unlock_timer(struct k_itimer *timr)
{
	if (likely((timr)))
		spin_unlock_irq(&timr->it_lock);
}

#define scoped_timer_get_or_fail(_id)					\
	scoped_cond_guard(lock_timer, return -EINVAL, _id)

#define scoped_timer				(scope)

/* cleanup class 把“按 ID 查找并持 it_lock”封装为作用域资源；失败统一从当前 int syscall 返回 -EINVAL。 */
DEFINE_CLASS(lock_timer, struct k_itimer *, unlock_timer(_T), lock_timer(id), timer_t id);
DEFINE_CLASS_IS_COND_GUARD(lock_timer);

/*
 * hash_bucket() - 由线程组 @sig 地址和无符号 timer 编号 @nr 选 bucket。
 * jhash2 覆盖指针字并以 nr 为 seed，再用启动期 power-of-two mask 截断；返回静态数组借用指针，无锁要求。
 */
static struct timer_hash_bucket *hash_bucket(struct signal_struct *sig, unsigned int nr)
{
	return &timer_buckets[jhash2((u32 *)&sig, sizeof(sig) / sizeof(u32), nr) & timer_hashmask];
}

/*
 * posix_timer_by_id() - 在 current->signal 命名空间无锁查找 @id。
 * 调用者必须处于 RCU read-side；遍历对应 bucket，READ_ONCE it_signal 与当前 signal 精确相等且 id 相等才
 * 返回借用 timer。初始化/删除阶段 it_signal 低位置 1，故不会匹配；返回对象尚未加 it_lock，需二次校验。
 */
static struct k_itimer *posix_timer_by_id(timer_t id)
{
	struct signal_struct *sig = current->signal;
	struct timer_hash_bucket *bucket = hash_bucket(sig, id);
	struct k_itimer *timer;

	hlist_for_each_entry_rcu(timer, &bucket->head, t_hash) {
		/* timer->it_signal can be set concurrently */
		/* it_signal 可由创建发布或删除失效并发改写，单次原子读避免编译器拆分/重载。 */
		if ((READ_ONCE(timer->it_signal) == sig) && (timer->it_id == id))
			return timer;
	}
	return NULL;
}

/* posix_sig_owner() 清除 it_signal 低位 invalid 标志，得到哈希归属的真实 signal_struct 借用指针。 */
static inline struct signal_struct *posix_sig_owner(const struct k_itimer *timer)
{
	unsigned long val = (unsigned long)timer->it_signal;

	/*
	 * Mask out bit 0, which acts as invalid marker to prevent
	 * posix_timer_by_id() detecting it as valid.
	 */
	/* 低位 1 只作为 syscall 不可见标志；signal_struct 至少按 2 字节对齐，可安全编码。 */
	return (struct signal_struct *)(val & ~1UL);
}

/*
 * posix_timer_hashed() - 检查 @bucket 是否已含 owner=@sig/id=@id。
 * 创建复核路径持 bucket->lock；RCU iterator 的 lockdep 条件声明该锁保护。用 posix_sig_owner 让尚未完全发布
 * 的预留节点也参与冲突检测。命中 true，否则 false；只读对象，不取得引用。
 */
static bool posix_timer_hashed(struct timer_hash_bucket *bucket, struct signal_struct *sig,
			       timer_t id)
{
	struct hlist_head *head = &bucket->head;
	struct k_itimer *timer;

	hlist_for_each_entry_rcu(timer, head, t_hash, lockdep_is_held(&bucket->lock)) {
		if ((posix_sig_owner(timer) == sig) && (timer->it_id == id))
			return true;
	}
	return false;
}

/*
 * posix_timer_add_at() - 原子预留指定 @sig/@id 并把新 @timer 插入哈希。
 * 在目标 bucket 自旋锁下二次查重；空闲时写 id，把 it_signal 设为 owner|1 的 invalid 状态后 hlist_add_head_rcu，
 * 返回 true。冲突返回 false且 timer 未插入。invalid 节点阻止 syscall lookup，却阻止其他创建取得重复 ID。
 */
static bool posix_timer_add_at(struct k_itimer *timer, struct signal_struct *sig, unsigned int id)
{
	struct timer_hash_bucket *bucket = hash_bucket(sig, id);

	scoped_guard (spinlock, &bucket->lock) {
		/*
		 * Validate under the lock as this could have raced against
		 * another thread ending up with the same ID, which is
		 * highly unlikely, but possible.
		 */
		/* 锁内复核封闭多个线程从同一 next_id 或 CRIU 指定相同 ID 的竞争。 */
		if (!posix_timer_hashed(bucket, sig, id)) {
			/*
			 * Set the timer ID and the signal pointer to make
			 * it identifiable in the hash table. The signal
			 * pointer has bit 0 set to indicate that it is not
			 * yet fully initialized. posix_timer_hashed()
			 * masks this bit out, but the syscall lookup fails
			 * to match due to it being set. This guarantees
			 * that there can't be duplicate timer IDs handed
			 * out.
			 */
			/* 先发布 invalid 哈希预留；其余字段完成并成功 copyout 后，it_lock 下清低位正式生效。 */
			timer->it_id = (timer_t)id;
			timer->it_signal = (struct signal_struct *)((unsigned long)sig | 1UL);
			hlist_add_head_rcu(&timer->t_hash, &bucket->head);
			return true;
		}
	}
	return false;
}

/*
 * posix_timer_add() - 为 @timer 分配并预留当前线程组的 timer id。
 * @req_id!=TIMER_ANY_ID 是 CRIU 精确恢复：冲突 -EBUSY，成功把 next counter 移到 req_id+1 并返回该 id。
 * 自动模式最多尝试整个 0..INT_MAX 空间：atomic fetch/inc 后屏蔽符号位，逐个 add_at，冲突间 cond_resched；
 * 成功返回非负 id，空间全满返回 POSIX 要求的 -EAGAIN。函数可调度，对象 ownership 仍归调用者。
 */
static int posix_timer_add(struct k_itimer *timer, int req_id)
{
	struct signal_struct *sig = current->signal;

	if (unlikely(req_id != TIMER_ANY_ID)) {
		if (!posix_timer_add_at(timer, sig, req_id))
			return -EBUSY;

		/*
		 * Move the ID counter past the requested ID, so that after
		 * switching back to normal mode the IDs are outside of the
		 * exact allocated region. That avoids ID collisions on the
		 * next regular timer_create() invocations.
		 */
		/* 跳过精确恢复区，降低随后普通 timer_create 与恢复 ID 碰撞的概率。 */
		atomic_set(&sig->next_posix_timer_id, req_id + 1);
		return req_id;
	}

	for (unsigned int cnt = 0; cnt <= INT_MAX; cnt++) {
		/* Get the next timer ID and clamp it to positive space */
		/* counter 可回绕；屏蔽符号位后始终得到合法非负 timer_t 候选。 */
		unsigned int id = atomic_fetch_inc(&sig->next_posix_timer_id) & INT_MAX;

		if (posix_timer_add_at(timer, sig, id))
			return id;
		cond_resched();
	}
	/* POSIX return code when no timer ID could be allocated */
	/* 完整扫描仍无空位按 POSIX 返回资源暂不可用。 */
	return -EAGAIN;
}

/* posix_get_realtime_timespec() 忽略 @which_clock，向 @tp 写 host REALTIME；REALTIME 不受 time namespace 偏移。 */
static int posix_get_realtime_timespec(clockid_t which_clock, struct timespec64 *tp)
{
	ktime_get_real_ts64(tp);
	return 0;
}

/* posix_get_realtime_ktime() 返回 host/root REALTIME ktime，供 timer 绝对值与 remaining 计算。 */
static ktime_t posix_get_realtime_ktime(clockid_t which_clock)
{
	return ktime_get_real();
}

/* posix_clock_realtime_set() 把已复制的 @tp 交 timekeeping setter；权限、合法性和通知均由其处理。 */
static int posix_clock_realtime_set(const clockid_t which_clock,
				    const struct timespec64 *tp)
{
	return do_sys_settimeofday64(tp, NULL);
}

/* posix_clock_realtime_adj() 把内核 timex 交 do_adjtimex；返回状态/错误原样传给 clock_adjtime syscall。 */
static int posix_clock_realtime_adj(const clockid_t which_clock,
				    struct __kernel_timex *t)
{
	return do_adjtimex(t);
}

/*
 * posix_get_monotonic_timespec() - 输出当前任务 time namespace 中的 MONOTONIC。
 * 先读取 host monotonic，再叠加 namespace monotonic offset；@which_clock 未使用，成功固定返回 0。
 */
static int posix_get_monotonic_timespec(clockid_t which_clock, struct timespec64 *tp)
{
	ktime_get_ts64(tp);
	timens_add_monotonic(tp);
	return 0;
}

/* posix_get_monotonic_ktime() 返回 host/root MONOTONIC，不叠加 namespace offset，供内核 timer 运算。 */
static ktime_t posix_get_monotonic_ktime(clockid_t which_clock)
{
	return ktime_get();
}

/* posix_get_monotonic_raw() 输出 RAW 读数并叠加当前 namespace monotonic offset；固定返回 0。 */
static int posix_get_monotonic_raw(clockid_t which_clock, struct timespec64 *tp)
{
	ktime_get_raw_ts64(tp);
	timens_add_monotonic(tp);
	return 0;
}

/* posix_get_realtime_coarse() 输出低成本 host REALTIME_COARSE；不受 time namespace 虚拟化。 */
static int posix_get_realtime_coarse(clockid_t which_clock, struct timespec64 *tp)
{
	ktime_get_coarse_real_ts64(tp);
	return 0;
}

/* posix_get_monotonic_coarse() 输出 coarse monotonic 并叠加当前 time namespace monotonic offset。 */
static int posix_get_monotonic_coarse(clockid_t which_clock,
						struct timespec64 *tp)
{
	ktime_get_coarse_ts64(tp);
	timens_add_monotonic(tp);
	return 0;
}

/* posix_get_coarse_res() 忽略 clock id，把编译/运行低分辨率常量 KTIME_LOW_RES 转 timespec64 返回。 */
static int posix_get_coarse_res(const clockid_t which_clock, struct timespec64 *tp)
{
	*tp = ktime_to_timespec64(KTIME_LOW_RES);
	return 0;
}

/* posix_get_boottime_timespec() 输出 host BOOTTIME 加当前 namespace boottime offset，固定返回 0。 */
static int posix_get_boottime_timespec(const clockid_t which_clock, struct timespec64 *tp)
{
	ktime_get_boottime_ts64(tp);
	timens_add_boottime(tp);
	return 0;
}

/* posix_get_boottime_ktime() 返回 host/root BOOTTIME，供绝对 timer 内核坐标计算。 */
static ktime_t posix_get_boottime_ktime(const clockid_t which_clock)
{
	return ktime_get_boottime();
}

/* posix_get_tai_timespec() 输出 host CLOCK_TAI timespec；当前 time namespace 不虚拟化 TAI。 */
static int posix_get_tai_timespec(clockid_t which_clock, struct timespec64 *tp)
{
	ktime_get_clocktai_ts64(tp);
	return 0;
}

/* posix_get_tai_ktime() 返回 host/root CLOCK_TAI ktime。 */
static ktime_t posix_get_tai_ktime(clockid_t which_clock)
{
	return ktime_get_clocktai();
}

/* posix_get_hrtimer_res() 报告 POSIX 高精度 clock 的软件分辨率：0 秒和全局 hrtimer_resolution 纳秒。 */
static int posix_get_hrtimer_res(clockid_t which_clock, struct timespec64 *tp)
{
	tp->tv_sec = 0;
	tp->tv_nsec = hrtimer_resolution;
	return 0;
}

/*
 * The siginfo si_overrun field and the return value of timer_getoverrun(2)
 * are of type int. Clamp the overrun value to INT_MAX
 */
/*
 * timer_overrun_to_int() - 把 @timr->it_overrun_last 缓存窄化为用户 ABI int。
 * 只在超过 INT_MAX 时饱和，其他值直接转换；调用者持 it_lock 稳定字段。返回的是最近一次实际递送信号时
 * 固化的 overrun 快照，不是此刻 timer 的实时落后量。
 */
static inline int timer_overrun_to_int(struct k_itimer *timr)
{
	if (timr->it_overrun_last > (s64)INT_MAX)
		return INT_MAX;

	return (int)timr->it_overrun_last;
}

/*
 * common_hrtimer_rearm() - 信号递送路径为通用 hrtimer 周期 timer 推进并重装。
 * 调用者持 @timr->it_lock 且 interval>0；hrtimer_forward_now 跨过的到期次数累加 it_overrun，然后按已更新
 * 绝对 expires 用 user-start 启动。返回 true 表示已排队，false 表示期限仍已过，由上层再次排信号。
 */
static bool common_hrtimer_rearm(struct k_itimer *timr)
{
	struct hrtimer *timer = &timr->it.real.timer;

	timr->it_overrun += hrtimer_forward_now(timer, timr->it_interval);
	return hrtimer_start_expires_user(timer, HRTIMER_MODE_ABS);
}

/*
 * __posixtimer_deliver_signal() - 在一个预分配 timer sigqueue 实际出队时决定丢弃或递送，并延迟重装周期 timer。
 * @info 是将交用户的 siginfo，@timr 由 signal 引用稳定；函数取得 it_lock。若 timer 已 set/delete 导致
 * signal_seq 与排队快照不等，或 timer 已 invalid，返回 false 丢弃旧信号。oneshot 返回 true直接递送。
 *
 * 周期 timer 必须处于 REQUEUE_PENDING：timer_rearm 推进期限/累计 overrun，随后缓存 last、重置当前计数
 * 为 -1、递增 sequence，并把饱和 overrun 写 @info。重装成功置 ARMED；期限又过则立即 queue_signal。
 * 最终 true 表示当前信号仍应递送。所有状态在 it_lock 下串行，函数本身不取得/释放 timer 引用。
 */
static bool __posixtimer_deliver_signal(struct kernel_siginfo *info, struct k_itimer *timr)
{
	bool queued;

	guard(spinlock)(&timr->it_lock);

	/*
	 * Check if the timer is still alive or whether it got modified
	 * since the signal was queued. In either case, don't rearm and
	 * drop the signal.
	 */
	/* sequence 不匹配说明排队后发生 set/delete；旧 sigqueue 不能重装或递送新一代 timer。 */
	if (timr->it_signal_seq != timr->it_sigqueue_seq || WARN_ON_ONCE(!posixtimer_valid(timr)))
		return false;

	if (!timr->it_interval || WARN_ON_ONCE(timr->it_status != POSIX_TIMER_REQUEUE_PENDING))
		return true;

	/* timer_rearm() updates timr::it_overrun */
	/* clock-specific rearm 同时更新 it_overrun，core 随后固定本次交付的用户可见快照。 */
	queued = timr->kclock->timer_rearm(timr);

	timr->it_overrun_last = timr->it_overrun;
	timr->it_overrun = -1LL;
	++timr->it_signal_seq;
	info->si_overrun = timer_overrun_to_int(timr);

	if (queued)
		timr->it_status = POSIX_TIMER_ARMED;
	else
		posix_timer_queue_signal(timr);
	return true;
}

/*
 * This function is called from the signal delivery code. It decides
 * whether the signal should be dropped and rearms interval timers.  The
 * timer can be unconditionally accessed as there is a reference held on
 * it.
 */
/*
 * posixtimer_deliver_signal() - signal core 持 current sighand siglock 调用的 timer 信号递送桥。
 * @timer_sigq 内嵌于已有 signal 引用的 k_itimer。为遵守 siglock -> it_lock 的反向冲突，先释放 siglock但保持
 * IRQ disabled，调用内部状态机，再归还排队时取得的 timer 引用，最后重取 siglock。返回 true 递送，false
 * 丢弃；归还引用后 timr 可能进入 RCU 释放，不能再访问。
 */
bool posixtimer_deliver_signal(struct kernel_siginfo *info, struct sigqueue *timer_sigq)
{
	struct k_itimer *timr = container_of(timer_sigq, struct k_itimer, sigq);
	bool ret;

	/*
	 * Release siglock to ensure proper locking order versus
	 * timr::it_lock. Keep interrupts disabled.
	 */
	/* 保持 IRQ 关闭，仅暂退 siglock，建立 it_lock 在 siglock 之前的全局锁序。 */
	spin_unlock(&current->sighand->siglock);

	ret = __posixtimer_deliver_signal(info, timr);

	/* Drop the reference which was acquired when the signal was queued */
	/* pending/ignored 节点持有的引用在本次出队处理后归还。 */
	posixtimer_putref(timr);

	spin_lock(&current->sighand->siglock);
	return ret;
}

/*
 * posix_timer_queue_signal() - 在 timer 到期或已过期启动路径发布一次 POSIX timer 信号。
 * 要求持 @timr->it_lock；invalid timer 直接返回。interval 非零置 REQUEUE_PENDING，oneshot 置 DISARMED，
 * 再交 signal 层记录 sequence、合并/忽略/排队并管理额外引用。无返回值；周期重装留到实际递送路径。
 */
void posix_timer_queue_signal(struct k_itimer *timr)
{
	lockdep_assert_held(&timr->it_lock);

	if (!posixtimer_valid(timr))
		return;

	timr->it_status = timr->it_interval ? POSIX_TIMER_REQUEUE_PENDING : POSIX_TIMER_DISARMED;
	posixtimer_send_sigqueue(timr);
}

/*
 * This function gets called when a POSIX.1b interval timer expires from
 * the HRTIMER interrupt (soft interrupt on RT kernels).
 *
 * Handles CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME and CLOCK_TAI
 * based timers.
 */
/*
 * posix_timer_fn() - 通用 wall-clock k_itimer 内嵌 hrtimer 的到期 callback。
 * @timer 反推 k_itimer，持 irqsave it_lock 调 queue_signal，固定返回 HRTIMER_NORESTART；REALTIME、MONOTONIC、
 * BOOTTIME、TAI 共用。运行于 hrtimer hardirq（RT 为 softirq）上下文，周期性由信号递送后另行重装。
 */
static enum hrtimer_restart posix_timer_fn(struct hrtimer *timer)
{
	struct k_itimer *timr = container_of(timer, struct k_itimer, it.real.timer);

	guard(spinlock_irqsave)(&timr->it_lock);
	posix_timer_queue_signal(timr);
	return HRTIMER_NORESTART;
}

/*
 * posixtimer_create_prctl() - 控制当前线程组的 CRIU timer ID 恢复模式。
 * OFF/ON 分别清/置 signal->timer_create_restore_ids 并返回 0，GET 返回当前 0/1，其他 @ctrl 返回 -EINVAL。
 * 模式开启后 timer_create 从 created_timer_id 用户地址读取期望 ID；调用者需自行避免与并发创建的策略竞争。
 */
long posixtimer_create_prctl(unsigned long ctrl)
{
	switch (ctrl) {
	case PR_TIMER_CREATE_RESTORE_IDS_OFF:
		current->signal->timer_create_restore_ids = 0;
		return 0;
	case PR_TIMER_CREATE_RESTORE_IDS_ON:
		current->signal->timer_create_restore_ids = 1;
		return 0;
	case PR_TIMER_CREATE_RESTORE_IDS_GET:
		return current->signal->timer_create_restore_ids;
	}
	return -EINVAL;
}

/*
 * good_sigevent() - 校验 timer_create 的内核 sigevent 并选择目标 pid。
 * 默认借用 current TGID。SIGEV_THREAD_ID 查指定 vpid，要求目标存在且同线程组，再继续按后续分支校验 signo；
 * SIGEV_SIGNAL/SIGEV_THREAD 要求 1..SIGRTMAX，SIGEV_NONE 不要求有效 signo。成功返回 RCU 保护下的借用 pid，
 * 非法组合/目标返回 NULL；调用者须在同一 RCU read-side 用 get_pid 转为持有引用。
 */
static struct pid *good_sigevent(sigevent_t * event)
{
	struct pid *pid = task_tgid(current);
	struct task_struct *rtn;

	switch (event->sigev_notify) {
	case SIGEV_SIGNAL | SIGEV_THREAD_ID:
		pid = find_vpid(event->sigev_notify_thread_id);
		rtn = pid_task(pid, PIDTYPE_PID);
		if (!rtn || !same_thread_group(rtn, current))
			return NULL;
		fallthrough;
	case SIGEV_SIGNAL:
	case SIGEV_THREAD:
		if (event->sigev_signo <= 0 || event->sigev_signo > SIGRTMAX)
			return NULL;
		fallthrough;
	case SIGEV_NONE:
		return pid;
	default:
		return NULL;
	}
}

/*
 * alloc_posix_timer() - 从启动期 slab 分配清零 k_itimer，并预留 sigqueue 配额。
 * cache 未建立、GFP_KERNEL 分配失败或 posixtimer_init_sigqueue 不能取得 RLIMIT_SIGPENDING ucounts 时返回 NULL；
 * sigqueue 失败会归还 slab。成功把 rcuref 初始化为 1 并返回 owned 对象，pid/hash/clock 尚未初始化。
 */
static struct k_itimer *alloc_posix_timer(void)
{
	struct k_itimer *tmr;

	if (unlikely(!posix_timers_cache))
		return NULL;

	tmr = kmem_cache_zalloc(posix_timers_cache, GFP_KERNEL);
	if (!tmr)
		return tmr;

	if (unlikely(!posixtimer_init_sigqueue(&tmr->sigq))) {
		kmem_cache_free(posix_timers_cache, tmr);
		return NULL;
	}
	rcuref_init(&tmr->rcuref, 1);
	return tmr;
}

/*
 * posixtimer_free_timer() - rcuref 最后引用归零后的最终释放器。
 * 归还可空 it_pid 引用与预分配 sigqueue 的 ucounts 配额，再 kfree_rcu 延迟释放 k_itimer；调用者不能再访问。
 */
void posixtimer_free_timer(struct k_itimer *tmr)
{
	put_pid(tmr->it_pid);
	if (tmr->sigq.ucounts)
		dec_rlimit_put_ucounts(tmr->sigq.ucounts, UCOUNT_RLIMIT_SIGPENDING);
	kfree_rcu(tmr, rcu);
}

/*
 * posix_timer_unhash_and_free() - 撤销 timer ID 哈希预留并归还基础 rcuref。
 * 从带/不带 invalid 位的 owner 计算 bucket，在 bucket 锁下 hlist_del_rcu，随后 posixtimer_putref；若仍有
 * pending/ignored signal 引用则延迟最终释放，否则由 kfree_rcu 收尾。调用者须已阻止新的有效 lookup。
 */
static void posix_timer_unhash_and_free(struct k_itimer *tmr)
{
	struct timer_hash_bucket *bucket = hash_bucket(posix_sig_owner(tmr), tmr->it_id);

	scoped_guard (spinlock, &bucket->lock)
		hlist_del_rcu(&tmr->t_hash);
	posixtimer_putref(tmr);
}

/*
 * common_timer_create() - 为普通 wall-clock k_itimer 初始化内嵌 hrtimer。
 * 使用已设置的 it_clock、posix_timer_fn 与默认模式 0；不启动 timer、无失败路径，返回 0。
 */
static int common_timer_create(struct k_itimer *new_timer)
{
	hrtimer_setup(&new_timer->it.real.timer, posix_timer_fn, new_timer->it_clock, 0);
	return 0;
}

/* Create a POSIX.1b interval timer. */
/*
 * do_timer_create() - timer_create 的内核主事务。
 * 解析 @which_clock 操作表（无效 -EINVAL、无 create -EOPNOTSUPP）；CRIU 模式从输出地址反向读取期望 ID，
 * copy fault/-范围分别 -EFAULT/-EINVAL。随后分配 timer、初始化 it_lock，并以 invalid it_signal 插入哈希
 * 预留唯一 ID；失败归还所有资源。
 *
 * 填 clock/kclock/overrun 后，显式 @event 在 RCU 下经 good_sigevent 校验并 get_pid，保存 notify/signo/value；
 * NULL event 默认向 current TGID 发 SIGALRM 且 sival_int=id。设置 PID/TGID 类型与 SI_TIMER 信息后先把 ID
 * copy_to_user；再调用 clock-specific create。任一后续错误会 unhash/put，用户虽可能看过 ID但 syscall
 * 返回失败且该 ID永不生效，clock create 回调必须自行清理其失败前的局部资源。
 *
 * 成功时按 it_lock -> sighand siglock 顺序清除 it_signal invalid 位并接入 signal->posix_timers；解锁后对象
 * 可被并发删除，函数不可再解引用。返回 0 或上述错误/clock create 错误。
 */
static int do_timer_create(clockid_t which_clock, struct sigevent *event,
			   timer_t __user *created_timer_id)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	timer_t req_id = TIMER_ANY_ID;
	struct k_itimer *new_timer;
	int error, new_timer_id;

	if (!kc)
		return -EINVAL;
	if (!kc->timer_create)
		return -EOPNOTSUPP;

	/* Special case for CRIU to restore timers with a given timer ID. */
	/* 恢复模式复用 created_timer_id 作为输入/输出地址，正常模式只在成功预留后写输出。 */
	if (unlikely(current->signal->timer_create_restore_ids)) {
		if (copy_from_user(&req_id, created_timer_id, sizeof(req_id)))
			return -EFAULT;
		/* Valid IDs are 0..INT_MAX */
		if ((unsigned int)req_id > INT_MAX)
			return -EINVAL;
	}

	new_timer = alloc_posix_timer();
	if (unlikely(!new_timer))
		return -EAGAIN;

	spin_lock_init(&new_timer->it_lock);

	/*
	 * Add the timer to the hash table. The timer is not yet valid
	 * after insertion, but has a unique ID allocated.
	 */
	/* 哈希中的 invalid 节点只占住 ID，不允许其他 syscall 观察半初始化字段。 */
	new_timer_id = posix_timer_add(new_timer, req_id);
	if (new_timer_id < 0) {
		posixtimer_free_timer(new_timer);
		return new_timer_id;
	}

	new_timer->it_clock = which_clock;
	new_timer->kclock = kc;
	new_timer->it_overrun = -1LL;

	if (event) {
		scoped_guard (rcu)
			new_timer->it_pid = get_pid(good_sigevent(event));
		if (!new_timer->it_pid) {
			error = -EINVAL;
			goto out;
		}
		new_timer->it_sigev_notify     = event->sigev_notify;
		new_timer->sigq.info.si_signo = event->sigev_signo;
		new_timer->sigq.info.si_value = event->sigev_value;
	} else {
		new_timer->it_sigev_notify     = SIGEV_SIGNAL;
		new_timer->sigq.info.si_signo = SIGALRM;
		new_timer->sigq.info.si_value.sival_int = new_timer->it_id;
		new_timer->it_pid = get_pid(task_tgid(current));
	}

	if (new_timer->it_sigev_notify & SIGEV_THREAD_ID)
		new_timer->it_pid_type = PIDTYPE_PID;
	else
		new_timer->it_pid_type = PIDTYPE_TGID;

	new_timer->sigq.info.si_tid = new_timer->it_id;
	new_timer->sigq.info.si_code = SI_TIMER;

	if (copy_to_user(created_timer_id, &new_timer_id, sizeof (new_timer_id))) {
		error = -EFAULT;
		goto out;
	}
	/*
	 * After successful copy out, the timer ID is visible to user space
	 * now but not yet valid because new_timer::signal low order bit is 1.
	 *
	 * Complete the initialization with the clock specific create
	 * callback.
	 */
	/* 用户已看到数值 ID，但低位标记仍阻止 lookup；clock-specific 初始化成功后才正式发布。 */
	error = kc->timer_create(new_timer);
	if (error)
		goto out;

	/*
	 * timer::it_lock ensures that __lock_timer() observes a fully
	 * initialized timer when it observes a valid timer::it_signal.
	 *
	 * sighand::siglock is required to protect signal::posix_timers.
	 */
	/* it_lock 发布所有 timer 字段，siglock 串行线程组 timer 链表与信号退出/清理。 */
	scoped_guard (spinlock_irq, &new_timer->it_lock) {
		guard(spinlock)(&current->sighand->siglock);
		/*
		 * new_timer::it_signal contains the signal pointer with
		 * bit 0 set, which makes it invalid for syscall operations.
		 * Store the unmodified signal pointer to make it valid.
		 */
		WRITE_ONCE(new_timer->it_signal, current->signal);
		hlist_add_head_rcu(&new_timer->list, &current->signal->posix_timers);
	}
	/*
	 * After unlocking @new_timer is subject to concurrent removal and
	 * cannot be touched anymore
	 */
	/* 清 invalid 位与接链后删除者可立即取得对象，因此这里是最后一次无条件访问。 */
	return 0;
out:
	posix_timer_unhash_and_free(new_timer);
	return error;
}

/* native timer_create() 复制可选 sigevent；NULL 使用 SIGALRM 默认值，最终委托 do_timer_create。 */
SYSCALL_DEFINE3(timer_create, const clockid_t, which_clock,
		struct sigevent __user *, timer_event_spec,
		timer_t __user *, created_timer_id)
{
	if (timer_event_spec) {
		sigevent_t event;

		if (copy_from_user(&event, timer_event_spec, sizeof (event)))
			return -EFAULT;
		return do_timer_create(which_clock, &event, created_timer_id);
	}
	return do_timer_create(which_clock, NULL, created_timer_id);
}

#ifdef CONFIG_COMPAT
/* compat timer_create() 用 get_compat_sigevent 转换 32 位布局，ID 输出仍是 timer_t；其余事务与 native 共用。 */
COMPAT_SYSCALL_DEFINE3(timer_create, clockid_t, which_clock,
		       struct compat_sigevent __user *, timer_event_spec,
		       timer_t __user *, created_timer_id)
{
	if (timer_event_spec) {
		sigevent_t event;

		if (get_compat_sigevent(&event, timer_event_spec))
			return -EFAULT;
		return do_timer_create(which_clock, &event, created_timer_id);
	}
	return do_timer_create(which_clock, NULL, created_timer_id);
}
#endif

/*
 * lock_timer() - 按 current 线程组和非负 @timer_id 查找 timer，并返回持 irq-disabled it_lock 的借用指针。
 * 超出 0..INT_MAX 立即 NULL。RCU 覆盖哈希 lookup 和加锁前窗口；找到后 spin_lock_irq，再复核 it_signal
 * 仍精确等于 current->signal，以封闭删除置 invalid 的竞态。成功返回时 cleanup guard 负责 unlock；失败
 * 返回 NULL且无锁。对象由 RCU 保证加锁前不释放，it_lock 保证返回后的字段/有效性。
 */
static struct k_itimer *lock_timer(timer_t timer_id)
{
	struct k_itimer *timr;

	/*
	 * timer_t could be any type >= int and we want to make sure any
	 * @timer_id outside positive int range fails lookup.
	 */
	/* 宽类型 timer_t 也必须拒绝负值或高位非零，避免截断后误命中合法 int ID。 */
	if ((unsigned long long)timer_id > INT_MAX)
		return NULL;

	/*
	 * The hash lookup and the timers are RCU protected.
	 *
	 * Timers are added to the hash in invalid state where
	 * timr::it_signal is marked invalid. timer::it_signal is only set
	 * after the rest of the initialization succeeded.
	 *
	 * Timer destruction happens in steps:
	 *  1) Set timr::it_signal marked invalid with timr::it_lock held
	 *  2) Release timr::it_lock
	 *  3) Remove from the hash under hash_lock
	 *  4) Put the reference count.
	 *
	 * The reference count might not drop to zero if timr::sigq is
	 * queued. In that case the signal delivery or flush will put the
	 * last reference count.
	 *
	 * When the reference count reaches zero, the timer is scheduled
	 * for RCU removal after the grace period.
	 *
	 * Holding rcu_read_lock() across the lookup ensures that
	 * the timer cannot be freed.
	 *
	 * The lookup validates locklessly that timr::it_signal ==
	 * current::it_signal and timr::it_id == @timer_id. timr::it_id
	 * can't change, but timr::it_signal can become invalid during
	 * destruction, which makes the locked check fail.
	 */
	/* 删除采用 invalid -> 解 it_lock -> unhash -> putref -> RCU free，lookup 则 RCU -> it_lock -> 二次校验。 */
	guard(rcu)();
	timr = posix_timer_by_id(timer_id);
	if (timr) {
		spin_lock_irq(&timr->it_lock);
		/*
		 * Validate under timr::it_lock that timr::it_signal is
		 * still valid. Pairs with #1 above.
		 */
		/* 与删除第 1 步配对：拿锁后只有未标记的同一 signal owner 才可交给 syscall。 */
		if (timr->it_signal == current->signal)
			return timr;
		spin_unlock_irq(&timr->it_lock);
	}
	return NULL;
}

/* common_hrtimer_remaining() 以调用者同一 @now 计算调整低分辨率补偿后的 expires-now；结果可为负。 */
static ktime_t common_hrtimer_remaining(struct k_itimer *timr, ktime_t now)
{
	struct hrtimer *timer = &timr->it.real.timer;

	return __hrtimer_expires_remaining_adjusted(timer, now);
}

/* common_hrtimer_forward() 按正 it_interval 将 hrtimer 期限推进到 @now 之后，返回跨过次数；不启动 timer。 */
static s64 common_hrtimer_forward(struct k_itimer *timr, ktime_t now)
{
	struct hrtimer *timer = &timr->it.real.timer;

	return hrtimer_forward(timer, now, timr->it_interval);
}

/*
 * Get the time remaining on a POSIX.1b interval timer.
 *
 * Two issues to handle here:
 *
 *  1) The timer has a requeue pending. The return value must appear as
 *     if the timer has been requeued right now.
 *
 *  2) The timer is a SIGEV_NONE timer. These timers are never enqueued
 *     into the hrtimer queue and therefore never expired. Emulate expiry
 *     here taking #1 into account.
 */
/*
 * common_timer_get() - 在已持 @timr->it_lock 时生成当前 POSIX timer 设置。
 * @cur_setting 必须由调用者预先清零。先快照 interval/SIGEV_NONE；有 interval 写 it_interval，普通已 disarm
 * oneshot 直接保持全零，SIGEV_NONE oneshot 虽恒为 DISARMED 仍继续按保存期限计算。
 *
 * 用 kclock root 时间取一次 @now。周期 timer 若不处于 ARMED（信号待重排或 SIGEV_NONE 从不入队），先
 * timer_forward 到 now 之后并累计 overrun，再以同一 now 算 remaining，保证视图自洽。正剩余转 timespec；
 * 非正时 SIGEV_NONE oneshot 返回 0，真实信号 timer 返回 1ns，表示已到期但信号尚未递送而非已关闭。
 * 函数可能推进软件期限/overrun，但不启动 timer、不访问用户内存。
 */
void common_timer_get(struct k_itimer *timr, struct itimerspec64 *cur_setting)
{
	const struct k_clock *kc = timr->kclock;
	ktime_t now, remaining, iv;
	bool sig_none;

	sig_none = timr->it_sigev_notify == SIGEV_NONE;
	iv = timr->it_interval;

	/* interval timer ? */
	/* interval 字段只在非零时写；调用者的预清零同时构成 disarmed 输出。 */
	if (iv) {
		cur_setting->it_interval = ktime_to_timespec64(iv);
	} else if (timr->it_status == POSIX_TIMER_DISARMED) {
		/*
		 * SIGEV_NONE oneshot timers are never queued and therefore
		 * timr->it_status is always DISARMED. The check below
		 * vs. remaining time will handle this case.
		 *
		 * For all other timers there is nothing to update here, so
		 * return.
		 */
		/* SIGEV_NONE 无真实队列状态，必须继续比较其保存的 expires；其他 disarmed timer 可直接返回全零。 */
		if (!sig_none)
			return;
	}

	now = kc->clock_get_ktime(timr->it_clock);

	/*
	 * If this is an interval timer and either has requeue pending or
	 * is a SIGEV_NONE timer move the expiry time forward by intervals,
	 * so expiry is > now.
	 */
	/* pending/虚拟 timer 在查询时补做周期推进，使用户看到下一期限而非陈旧到期点。 */
	if (iv && timr->it_status != POSIX_TIMER_ARMED)
		timr->it_overrun += kc->timer_forward(timr, now);

	remaining = kc->timer_remaining(timr, now);
	/*
	 * As @now is retrieved before a possible timer_forward() and
	 * cannot be reevaluated by the compiler @remaining is based on the
	 * same @now value. Therefore @remaining is consistent vs. @now.
	 *
	 * Consequently all interval timers, i.e. @iv > 0, cannot have a
	 * remaining time <= 0 because timer_forward() guarantees to move
	 * them forward so that the next timer expiry is > @now.
	 */
	/* forward 与 remaining 共用单次 now；编译器不可重新求值该局部量，避免跨时刻不一致。 */
	if (remaining <= 0) {
		/*
		 * A single shot SIGEV_NONE timer must return 0, when it is
		 * expired! Timers which have a real signal delivery mode
		 * must return a remaining time greater than 0 because the
		 * signal has not yet been delivered.
		 */
		/* 1ns 是“已到期、待信号消费”的可观察哨兵；无信号 oneshot 才按真实已过期返回 0。 */
		if (!sig_none)
			cur_setting->it_value.tv_nsec = 1;
	} else {
		cur_setting->it_value = ktime_to_timespec64(remaining);
	}
}

/*
 * do_timer_gettime() - 查找/锁定 @timer_id，并调用 clock-specific timer_get 填内核 @setting。
 * 先把整个输出清零；lookup 失败由 scoped guard 返回 -EINVAL，成功在 it_lock 下完成并返回 0。
 */
static int do_timer_gettime(timer_t timer_id,  struct itimerspec64 *setting)
{
	memset(setting, 0, sizeof(*setting));
	scoped_timer_get_or_fail(timer_id)
		scoped_timer->kclock->timer_get(scoped_timer, setting);
	return 0;
}

/* Get the time remaining on a POSIX.1b interval timer. */
/* native timer_gettime() 取得内核快照后复制 __kernel_itimerspec；lookup -EINVAL，复制失败 -EFAULT，成功 0。 */
SYSCALL_DEFINE2(timer_gettime, timer_t, timer_id,
		struct __kernel_itimerspec __user *, setting)
{
	struct itimerspec64 cur_setting;

	int ret = do_timer_gettime(timer_id, &cur_setting);
	if (!ret) {
		if (put_itimerspec64(&cur_setting, setting))
			ret = -EFAULT;
	}
	return ret;
}

#ifdef CONFIG_COMPAT_32BIT_TIME

/* timer_gettime32() 与 native 共用锁内快照，再按 old_itimerspec32 布局窄化复制。 */
SYSCALL_DEFINE2(timer_gettime32, timer_t, timer_id,
		struct old_itimerspec32 __user *, setting)
{
	struct itimerspec64 cur_setting;

	int ret = do_timer_gettime(timer_id, &cur_setting);
	if (!ret) {
		if (put_old_itimerspec32(&cur_setting, setting))
			ret = -EFAULT;
	}
	return ret;
}

#endif

/**
 * sys_timer_getoverrun - Get the number of overruns of a POSIX.1b interval timer
 * @timer_id:	The timer ID which identifies the timer
 *
 * The "overrun count" of a timer is one plus the number of expiration
 * intervals which have elapsed between the first expiry, which queues the
 * signal and the actual signal delivery. On signal delivery the "overrun
 * count" is calculated and cached, so it can be returned directly here.
 *
 * As this is relative to the last queued signal the returned overrun count
 * is meaningless outside of the signal delivery path and even there it
 * does not accurately reflect the current state when user space evaluates
 * it.
 *
 * Returns:
 *	-EINVAL		@timer_id is invalid
 *	1..INT_MAX	The number of overruns related to the last delivered signal
 */
/*
 * timer_getoverrun() - 锁定 @timer_id 后返回最近一次已递送信号缓存的饱和 overrun。
 * 无效 ID -EINVAL；代码允许初始/无额外超时值 0，上方旧 Returns 范围写 1..INT_MAX 并不覆盖这一实际边界。
 * 结果不是当前实时到期次数，仅由实际信号递送时更新。
 */
SYSCALL_DEFINE1(timer_getoverrun, timer_t, timer_id)
{
	scoped_timer_get_or_fail(timer_id)
		return timer_overrun_to_int(scoped_timer);
}

/*
 * common_hrtimer_arm() - 为普通 wall-clock POSIX timer 准备并可选启动 hrtimer。
 * @expires 是相对时长或已转换到 host 的绝对期限；@absolute 决定模式，@sigev_none 决定是否真实排队。
 * 相对 CLOCK_REALTIME 按 POSIX 语义不受后续墙钟调整，hrtimer_setup 会落到 MONOTONIC，并把 timr->kclock
 * 临时切为 clock_monotonic 供 get/forward 使用；绝对设置切回 clock_realtime，it_clock 始终保留原 ID。
 *
 * 每次 setup 重建 callback/base；相对值用 callback base 当前时间安全转绝对，再 set_expires。SIGEV_NONE
 * 只保存期限并返回 true；其他用 user-start，true 表示排队，false 表示期限已过且 callback 未执行。
 * 调用者持 it_lock，timer 必须已取消或 inactive。
 */
static bool common_hrtimer_arm(struct k_itimer *timr, ktime_t expires,
			       bool absolute, bool sigev_none)
{
	struct hrtimer *timer = &timr->it.real.timer;
	enum hrtimer_mode mode;

	mode = absolute ? HRTIMER_MODE_ABS : HRTIMER_MODE_REL;
	/*
	 * Posix magic: Relative CLOCK_REALTIME timers are not affected by
	 * clock modifications, so they become CLOCK_MONOTONIC based under the
	 * hood. See hrtimer_setup(). Update timr->kclock, so the generic
	 * functions which use timr->kclock->clock_get_*() work.
	 *
	 * Note: it_clock stays unmodified, because the next timer_set() might
	 * use ABSTIME, so it needs to switch back.
	 */
	/* 相对 REALTIME 的底层 MONOTONIC 切换只影响当前设置；下一次 ABSTIME 仍依 it_clock 切回。 */
	if (timr->it_clock == CLOCK_REALTIME)
		timr->kclock = absolute ? &clock_realtime : &clock_monotonic;

	hrtimer_setup(&timr->it.real.timer, posix_timer_fn, timr->it_clock, mode);

	if (!absolute)
		expires = ktime_add_safe(expires, hrtimer_cb_get_time(timer));
	hrtimer_set_expires(timer, expires);

	/* For sigev_none pretend that the timer is queued */
	/* SIGEV_NONE 由 gettime 按保存期限模拟到期，不消耗 hrtimer 队列或产生信号。 */
	if (sigev_none)
		return true;

	return hrtimer_start_expires_user(timer, HRTIMER_MODE_ABS);
}

/* common_hrtimer_try_to_cancel() 保留 hrtimer 的 1/0/负 callback-running 语义，调用者持 it_lock。 */
static int common_hrtimer_try_to_cancel(struct k_itimer *timr)
{
	return hrtimer_try_to_cancel(&timr->it.real.timer);
}

/* common_timer_wait_running() 为通用 hrtimer 调用 RT-aware wait/relax；POSIX core 已放开 it_lock并持 RCU。 */
static void common_timer_wait_running(struct k_itimer *timer)
{
	hrtimer_cancel_wait_running(&timer->it.real.timer);
}

/*
 * On PREEMPT_RT this prevents priority inversion and a potential livelock
 * against the ksoftirqd thread in case that ksoftirqd gets preempted while
 * executing a hrtimer callback.
 *
 * See the comments in hrtimer_cancel_wait_running(). For PREEMPT_RT=n this
 * just results in a cpu_relax().
 *
 * For POSIX CPU timers with CONFIG_POSIX_CPU_TIMERS_TASK_WORK=n this is
 * just a cpu_relax(). With CONFIG_POSIX_CPU_TIMERS_TASK_WORK=y this
 * prevents spinning on an eventually scheduled out task and a livelock
 * when the task which tries to delete or disarm the timer has preempted
 * the task which runs the expiry in task work context.
 */
/*
 * timer_wait_running() - 调用 clock-specific callback-running 等待钩子。
 * 外层已放开 timer->it_lock 并用 RCU 防释放；hrtimer 在 RT 上等待 softirq，非 RT relax，CPU timer 可等待
 * task-work handler。钩子可能暂退 RCU，返回后 @timer 可能失效，故本函数及调用者都不得再直接解引用，
 * 只能结束 RCU 区并重新按 ID 查找/加锁。
 */
static void timer_wait_running(struct k_itimer *timer)
{
	/*
	 * kc->timer_wait_running() might drop RCU lock. So @timer
	 * cannot be touched anymore after the function returns!
	 */
	/* callback 返回可能经历 RCU 释放窗口；不要缓存或再访问 timer/kclock 字段。 */
	timer->kclock->timer_wait_running(timer);
}

/*
 * Set up the new interval and reset the signal delivery data
 */
/*
 * posix_timer_set_common() - 提交新 interval 并重置 overrun 代际。
 * @new_setting->it_value 为 0 时无论给定 interval 为何都按 disarm 清 interval；armed 时把规范 timespec interval
 * 转 ktime。随后 last=0/current=-1，使第一次到期本身不计为 overrun。调用者持 it_lock，不启动/取消 timer。
 */
void posix_timer_set_common(struct k_itimer *timer, struct itimerspec64 *new_setting)
{
	if (new_setting->it_value.tv_sec || new_setting->it_value.tv_nsec)
		timer->it_interval = timespec64_to_ktime(new_setting->it_interval);
	else
		timer->it_interval = 0;

	/* Reset overrun accounting */
	/* -1 基线与 rearm 返回的至少 1 次相加，使无额外错过周期时用户 overrun 为 0。 */
	timer->it_overrun_last = 0;
	timer->it_overrun = -1LL;
}

/* Set a POSIX.1b interval timer. */
/*
 * common_timer_set() - 在已持 @timr->it_lock 时替换通用 clock timer 设置。
 * 可选 @old_setting 先通过 common_timer_get 补齐旧值。若 clock-specific try_cancel 报 callback 正运行，返回
 * TIMER_RETRY，不提交新状态；外层会解锁等待并重试。成功取消后置 DISARMED、提交 interval/overrun；新 value
 * 为 0 立即返回 0。否则把 value 转 ktime，ABSTIME 经 time namespace 转 host，识别 SIGEV_NONE 后 timer_arm。
 * arm true 时真实信号 timer 置 ARMED；false 说明期限已过，立即 queue_signal。返回 0，无用户访问。
 */
int common_timer_set(struct k_itimer *timr, int flags,
		     struct itimerspec64 *new_setting,
		     struct itimerspec64 *old_setting)
{
	const struct k_clock *kc = timr->kclock;
	bool sigev_none;
	ktime_t expires;

	if (old_setting)
		common_timer_get(timr, old_setting);

	/*
	 * Careful here. On SMP systems the timer expiry function could be
	 * active and spinning on timr->it_lock.
	 */
	/* callback 可能正等待同一 it_lock；不能在锁内同步等待，只通知外层执行 RETRY 协议。 */
	if (kc->timer_try_to_cancel(timr) < 0)
		return TIMER_RETRY;

	timr->it_status = POSIX_TIMER_DISARMED;
	posix_timer_set_common(timr, new_setting);

	/* Keep timer disarmed when it_value is zero */
	/* POSIX disarm 同时忽略用户提供的 interval，已由 set_common 清为 0。 */
	if (!new_setting->it_value.tv_sec && !new_setting->it_value.tv_nsec)
		return 0;

	expires = timespec64_to_ktime(new_setting->it_value);
	if (flags & TIMER_ABSTIME)
		expires = timens_ktime_to_host(timr->it_clock, expires);
	sigev_none = timr->it_sigev_notify == SIGEV_NONE;

	if (kc->timer_arm(timr, expires, flags & TIMER_ABSTIME, sigev_none)) {
		if (!sigev_none)
			timr->it_status = POSIX_TIMER_ARMED;
	} else {
		/* Timer was already expired, queue the signal */
		/* user-start 保证过期时不进 callback；由当前持锁路径同步发布一次到期信号。 */
		posix_timer_queue_signal(timr);
	}
	return 0;
}

/*
 * do_timer_settime() - timer_settime 的锁定、代际失效与 callback-running 重试事务。
 * 校验新 value/interval timespec，非法 -EINVAL；可选 old 输出先清零。每轮 lock_timer，首轮预存旧 interval，
 * 递增 it_signal_seq 使已排队信号无法递送/重装，再调 clock-specific timer_set。非 TIMER_RETRY 直接返回。
 *
 * RETRY 时在仍持 it_lock/RCU 的作用域内额外取得 RCU，退出 guard 解 it_lock 后调用 timer_wait_running；
 * 随后结束 RCU并重新按 ID 查找。重试把 old_spec64 置 NULL，保留第一次操作前的旧快照，不被等待后的状态
 * 覆盖。timer 可在等待时被删除，下一轮相应返回 -EINVAL。
 */
static int do_timer_settime(timer_t timer_id, int tmr_flags, struct itimerspec64 *new_spec64,
			    struct itimerspec64 *old_spec64)
{
	if (!timespec64_valid(&new_spec64->it_interval) ||
	    !timespec64_valid(&new_spec64->it_value))
		return -EINVAL;

	if (old_spec64)
		memset(old_spec64, 0, sizeof(*old_spec64));

	for (; ; old_spec64 = NULL) {
		struct k_itimer *timr;

		scoped_timer_get_or_fail(timer_id) {
			timr = scoped_timer;

			if (old_spec64)
				old_spec64->it_interval = ktime_to_timespec64(timr->it_interval);

			/* Prevent signal delivery and rearming. */
			/* 每次尝试都推进 sequence，旧 pending sigqueue 即使随后出队也会被判为过期代际。 */
			timr->it_signal_seq++;

			int ret = timr->kclock->timer_set(timr, tmr_flags, new_spec64, old_spec64);
			if (ret != TIMER_RETRY)
				return ret;

			/* Protect the timer from being freed when leaving the lock scope */
			/* 先额外进入 RCU，再由 cleanup guard 解 it_lock，封闭 wait callback 取得参数前的释放窗口。 */
			rcu_read_lock();
		}
		timer_wait_running(timr);
		rcu_read_unlock();
	}
}

/* Set a POSIX.1b interval timer */
/*
 * native timer_settime() - 复制并设置 __kernel_itimerspec，可选返回提交前旧值。
 * NULL new -EINVAL，输入/输出 copy fault -EFAULT；do_timer_settime 成功后 old copy 失败不会回滚已经生效的新 timer。
 */
SYSCALL_DEFINE4(timer_settime, timer_t, timer_id, int, flags,
		const struct __kernel_itimerspec __user *, new_setting,
		struct __kernel_itimerspec __user *, old_setting)
{
	struct itimerspec64 new_spec, old_spec, *rtn;
	int error = 0;

	if (!new_setting)
		return -EINVAL;

	if (get_itimerspec64(&new_spec, new_setting))
		return -EFAULT;

	rtn = old_setting ? &old_spec : NULL;
	error = do_timer_settime(timer_id, flags, &new_spec, rtn);
	if (!error && old_setting) {
		if (put_itimerspec64(&old_spec, old_setting))
			error = -EFAULT;
	}
	return error;
}

#ifdef CONFIG_COMPAT_32BIT_TIME
/* timer_settime32() 转换 old_itimerspec32 后复用同一事务；成功提交后的 old 窄化复制失败同样不回滚。 */
SYSCALL_DEFINE4(timer_settime32, timer_t, timer_id, int, flags,
		struct old_itimerspec32 __user *, new,
		struct old_itimerspec32 __user *, old)
{
	struct itimerspec64 new_spec, old_spec;
	struct itimerspec64 *rtn = old ? &old_spec : NULL;
	int error = 0;

	if (!new)
		return -EINVAL;
	if (get_old_itimerspec32(&new_spec, new))
		return -EFAULT;

	error = do_timer_settime(timer_id, flags, &new_spec, rtn);
	if (!error && old) {
		if (put_old_itimerspec32(&old_spec, old))
			error = -EFAULT;
	}
	return error;
}
#endif

/*
 * common_timer_del() - clock core 的通用 hrtimer 删除钩子。
 * callback 正运行返回 TIMER_RETRY；否则 try_cancel 已同步摘除并把状态置 DISARMED，返回 0。调用者持 it_lock。
 */
int common_timer_del(struct k_itimer *timer)
{
	const struct k_clock *kc = timer->kclock;

	if (kc->timer_try_to_cancel(timer) < 0)
		return TIMER_RETRY;
	timer->it_status = POSIX_TIMER_DISARMED;
	return 0;
}

/*
 * If the deleted timer is on the ignored list, remove it and
 * drop the associated reference.
 */
/*
 * posix_timer_cleanup_ignored() - 在 sighand siglock 下幂等移除 timer 的 ignored signal 节点。
 * 若 hashed，hlist_del_init 后归还 ignored 链表持有的 rcuref；未入链不动作。timer 由其他基础引用稳定。
 */
static inline void posix_timer_cleanup_ignored(struct k_itimer *tmr)
{
	if (!hlist_unhashed(&tmr->ignored_list)) {
		hlist_del_init(&tmr->ignored_list);
		posixtimer_putref(tmr);
	}
}

/*
 * posix_timer_delete() - 在已持 @timer->it_lock 时使 timer 永久失效、脱离线程组并停止 clock-specific timer。
 * 先递增 signal_seq；再取 current sighand siglock，把 it_signal 低位置 1、从 signal->posix_timers 摘除并清
 * ignored 引用。该顺序让 signal disposition/lookup 观察 invalid，阻断新排队、递送和周期重装。
 *
 * 随后调用 timer_del；若 callback 正运行，保持 invalid 但临时解 it_lock，在 RCU 下 wait，再重取锁重试。
 * 返回时底层 timer 已停止且 it_lock 仍由调用者持有；对象仍在 ID hash，外层再 unhash/put。函数无错误返回。
 */
static void posix_timer_delete(struct k_itimer *timer)
{
	/*
	 * Invalidate the timer, remove it from the linked list and remove
	 * it from the ignored list if pending.
	 *
	 * The invalidation must be written with siglock held so that the
	 * signal code observes the invalidated timer::it_signal in
	 * do_sigaction(), which prevents it from moving a pending signal
	 * of a deleted timer to the ignore list.
	 *
	 * The invalidation also prevents signal queueing, signal delivery
	 * and therefore rearming from the signal delivery path.
	 *
	 * A concurrent lookup can still find the timer in the hash, but it
	 * will check timer::it_signal with timer::it_lock held and observe
	 * bit 0 set, which invalidates it. That also prevents the timer ID
	 * from being handed out before this timer is completely gone.
	 */
	/* invalid 标记先于 unhash 发布，使并发 RCU lookup 即使找到节点也在 it_lock 二次校验失败。 */
	timer->it_signal_seq++;

	scoped_guard (spinlock, &current->sighand->siglock) {
		unsigned long sig = (unsigned long)timer->it_signal | 1UL;

		WRITE_ONCE(timer->it_signal, (struct signal_struct *)sig);
		hlist_del_rcu(&timer->list);
		posix_timer_cleanup_ignored(timer);
	}

	while (timer->kclock->timer_del(timer) == TIMER_RETRY) {
		guard(rcu)();
		spin_unlock_irq(&timer->it_lock);
		timer_wait_running(timer);
		spin_lock_irq(&timer->it_lock);
	}
}

/* Delete a POSIX.1b interval timer. */
/*
 * timer_delete() - 查找 current 线程组 timer，锁内调用 posix_timer_delete，解锁后从 hash 删除并归还基础引用。
 * 无效/已删除 ID 返回 -EINVAL；成功返回 0并释放 ID。pending signal 引用可能让对象延迟到出队/flush 后释放。
 */
SYSCALL_DEFINE1(timer_delete, timer_t, timer_id)
{
	struct k_itimer *timer;

	scoped_timer_get_or_fail(timer_id) {
		timer = scoped_timer;
		posix_timer_delete(timer);
	}
	/* Remove it from the hash, which frees up the timer ID */
	/* 哈希摘除是 ID 可重新分配的时点，最终内存释放仍受 rcuref+RCU 延迟。 */
	posix_timer_unhash_and_free(timer);
	return 0;
}

/*
 * Invoked from do_exit() when the last thread of a thread group exits.
 * At that point no other task can access the timers of the dying
 * task anymore.
 */
/*
 * exit_itimers() - 线程组最后一个线程退出时批量删除 @tsk->signal 的全部 POSIX timer。
 * 先清 CRIU restore 模式；空链直接返回。持 tsk sighand siglock 把主链整体移到局部 head，阻断 /proc 同时读取，
 * 再逐个持 timer it_lock 调 delete、unhash/put，并 cond_resched。此时无其他任务可通过 dying signal 访问。
 *
 * 正常 delete 应清空 ignored_posix_timers；若 WARN 发现残留，把链移到局部并逐个 cleanup 归还引用。
 * 函数可调度，无返回值；要求 do_exit 最后线程上下文，current 的 sighand 与 @tsk 匹配。
 */
void exit_itimers(struct task_struct *tsk)
{
	struct hlist_head timers;
	struct hlist_node *next;
	struct k_itimer *timer;

	/* Clear restore mode for exec() */
	/* exec/exit 都不能把一次 CRIU 精确 ID 策略泄漏给后续映像。 */
	tsk->signal->timer_create_restore_ids = 0;

	if (hlist_empty(&tsk->signal->posix_timers))
		return;

	/* Protect against concurrent read via /proc/$PID/timers */
	/* 整链搬走让 proc 读者在同一 siglock 下看到删除前或空列表，而非半处理链。 */
	scoped_guard (spinlock_irq, &tsk->sighand->siglock)
		hlist_move_list(&tsk->signal->posix_timers, &timers);

	/* The timers are not longer accessible via tsk::signal */
	/* 主链已私有化；仍按单 timer 标准删除协议失效 hash/signal 与底层 clock 资源。 */
	hlist_for_each_entry_safe(timer, next, &timers, list) {
		scoped_guard (spinlock_irq, &timer->it_lock)
			posix_timer_delete(timer);
		posix_timer_unhash_and_free(timer);
		cond_resched();
	}

	/*
	 * There should be no timers on the ignored list. posix_timer_delete() has
	 * mopped them up.
	 */
	/* ignored 残留仅是防御性修复路径；cleanup 逐项归还链表持有的引用。 */
	if (!WARN_ON_ONCE(!hlist_empty(&tsk->signal->ignored_posix_timers)))
		return;

	hlist_move_list(&tsk->signal->ignored_posix_timers, &timers);
	while (!hlist_empty(&timers)) {
		posix_timer_cleanup_ignored(hlist_entry(timers.first, struct k_itimer,
							ignored_list));
	}
}

/*
 * clock_settime() - native POSIX clock 设置分派。
 * @which_clock 无映射或该 clock 无 setter 均返回 -EINVAL；复制 @tp 失败 -EFAULT。成功取得 timespec64 后把
 * 权限、范围检查和实际提交交 clock-specific clock_set，返回值原样传递；本层不预截断到 getres 粒度。
 */
SYSCALL_DEFINE2(clock_settime, const clockid_t, which_clock,
		const struct __kernel_timespec __user *, tp)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	struct timespec64 new_tp;

	if (!kc || !kc->clock_set)
		return -EINVAL;

	if (get_timespec64(&new_tp, tp))
		return -EFAULT;

	/*
	 * Permission checks have to be done inside the clock specific
	 * setter callback.
	 */
	/* 不同 clock/动态设备权限模型不同，必须在具体 setter 内对 current credentials/fd mode 判定。 */
	return kc->clock_set(which_clock, &new_tp);
}

/*
 * clock_gettime() - native POSIX clock 读取分派。
 * 无效 clock -EINVAL；调用 clock_get_timespec 取得当前 time namespace 视图，成功后复制到用户 @tp，copy fault
 * 改为 -EFAULT。clock-specific 错误原样返回且不复制未定义输出。
 */
SYSCALL_DEFINE2(clock_gettime, const clockid_t, which_clock,
		struct __kernel_timespec __user *, tp)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	struct timespec64 kernel_tp;
	int error;

	if (!kc)
		return -EINVAL;

	error = kc->clock_get_timespec(which_clock, &kernel_tp);

	if (!error && put_timespec64(&kernel_tp, tp))
		error = -EFAULT;

	return error;
}

/*
 * do_clock_adjtime() - 已在内核内存中的 timex 调整/查询分派。
 * 无效 id -EINVAL，无 clock_adj 方法 -EOPNOTSUPP；否则把 @ktx 交具体 clock 并返回其负错误或非负 clock 状态。
 */
int do_clock_adjtime(const clockid_t which_clock, struct __kernel_timex * ktx)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);

	if (!kc)
		return -EINVAL;
	if (!kc->clock_adj)
		return -EOPNOTSUPP;

	return kc->clock_adj(which_clock, ktx);
}

/*
 * clock_adjtime() - native timex 用户 ABI 包装。
 * 先完整 copy_from_user，失败 -EFAULT；do_clock_adjtime 返回非负时把可能更新的查询/状态字段复制回用户，
 * copyout 失败覆盖为 -EFAULT；负错误不复制。非负返回值可能是 TIME_* 状态，不限于 0。
 */
SYSCALL_DEFINE2(clock_adjtime, const clockid_t, which_clock,
		struct __kernel_timex __user *, utx)
{
	struct __kernel_timex ktx;
	int err;

	if (copy_from_user(&ktx, utx, sizeof(ktx)))
		return -EFAULT;

	err = do_clock_adjtime(which_clock, &ktx);

	if (err >= 0 && copy_to_user(utx, &ktx, sizeof(ktx)))
		return -EFAULT;

	return err;
}

/**
 * sys_clock_getres - Get the resolution of a clock
 * @which_clock:	The clock to get the resolution for
 * @tp:			Pointer to a a user space timespec64 for storage
 *
 * POSIX defines:
 *
 * "The clock_getres() function shall return the resolution of any
 * clock. Clock resolutions are implementation-defined and cannot be set by
 * a process. If the argument res is not NULL, the resolution of the
 * specified clock shall be stored in the location pointed to by res. If
 * res is NULL, the clock resolution is not returned. If the time argument
 * of clock_settime() is not a multiple of res, then the value is truncated
 * to a multiple of res."
 *
 * Due to the various hardware constraints the real resolution can vary
 * wildly and even change during runtime when the underlying devices are
 * replaced. The kernel also can use hardware devices with different
 * resolutions for reading the time and for arming timers.
 *
 * The kernel therefore deviates from the POSIX spec in various aspects:
 *
 * 1) The resolution returned to user space
 *
 *    For CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_TAI,
 *    CLOCK_REALTIME_ALARM, CLOCK_BOOTTIME_ALAREM and CLOCK_MONOTONIC_RAW
 *    the kernel differentiates only two cases:
 *
 *    I)  Low resolution mode:
 *
 *	  When high resolution timers are disabled at compile or runtime
 *	  the resolution returned is nanoseconds per tick, which represents
 *	  the precision at which timers expire.
 *
 *    II) High resolution mode:
 *
 *	  When high resolution timers are enabled the resolution returned
 *	  is always one nanosecond independent of the actual resolution of
 *	  the underlying hardware devices.
 *
 *	  For CLOCK_*_ALARM the actual resolution depends on system
 *	  state. When system is running the resolution is the same as the
 *	  resolution of the other clocks. During suspend the actual
 *	  resolution is the resolution of the underlying RTC device which
 *	  might be way less precise than the clockevent device used during
 *	  running state.
 *
 *   For CLOCK_REALTIME_COARSE and CLOCK_MONOTONIC_COARSE the resolution
 *   returned is always nanoseconds per tick.
 *
 *   For CLOCK_PROCESS_CPUTIME and CLOCK_THREAD_CPUTIME the resolution
 *   returned is always one nanosecond under the assumption that the
 *   underlying scheduler clock has a better resolution than nanoseconds
 *   per tick.
 *
 *   For dynamic POSIX clocks (PTP devices) the resolution returned is
 *   always one nanosecond.
 *
 * 2) Affect on sys_clock_settime()
 *
 *    The kernel does not truncate the time which is handed in to
 *    sys_clock_settime(). The kernel internal timekeeping is always using
 *    nanoseconds precision independent of the clocksource device which is
 *    used to read the time from. The resolution of that device only
 *    affects the precision of the time returned by sys_clock_gettime().
 *
 * Returns:
 *	0		Success. @tp contains the resolution
 *	-EINVAL		@which_clock is not a valid clock ID
 *	-EFAULT		Copying the resolution to @tp faulted
 *	-ENODEV		Dynamic POSIX clock is not backed by a device
 *	-EOPNOTSUPP	Dynamic POSIX clock does not support getres()
 */
/*
 * clock_getres() - 返回指定 clock 的内核报告分辨率，可只做能力校验。
 * @tp 可为 NULL；无效 id -EINVAL，clock-specific 可返回 -ENODEV/-EOPNOTSUPP 等。成功且 tp 非空才复制，
 * copy fault -EFAULT。普通高精度 clock 报 hrtimer_resolution，coarse 报 KTIME_LOW_RES，CPU/dynamic 通常报
 * 1ns；这些是软件/API 分辨率，不保证底层读取精度，ALARM suspend 时还受 RTC 实际粒度限制。
 *
 * 内核不会像上方 POSIX 引文那样把 clock_settime 输入截断到该分辨率，timekeeping 始终保存纳秒值；设备
 * 可替换且 read/timer 硬件可不同，故报告值也不是稳定硬件量。上方 `CLOCK_BOOTTIME_ALAREM` 是旧拼写错误，
 * 实指 CLOCK_BOOTTIME_ALARM。所有原规范说明保留供对照。
 */
SYSCALL_DEFINE2(clock_getres, const clockid_t, which_clock,
		struct __kernel_timespec __user *, tp)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	struct timespec64 rtn_tp;
	int error;

	if (!kc)
		return -EINVAL;

	error = kc->clock_getres(which_clock, &rtn_tp);

	if (!error && tp && put_timespec64(&rtn_tp, tp))
		error = -EFAULT;

	return error;
}

#ifdef CONFIG_COMPAT_32BIT_TIME

/* clock_settime32() 把 old_timespec32 扩展为 timespec64 后直接调用具体 setter；错误边界同 native。 */
SYSCALL_DEFINE2(clock_settime32, clockid_t, which_clock,
		struct old_timespec32 __user *, tp)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	struct timespec64 ts;

	if (!kc || !kc->clock_set)
		return -EINVAL;

	if (get_old_timespec32(&ts, tp))
		return -EFAULT;

	return kc->clock_set(which_clock, &ts);
}

/* clock_gettime32() 取得 clock timespec64 后窄化到 old_timespec32；仅成功路径复制，fault -EFAULT。 */
SYSCALL_DEFINE2(clock_gettime32, clockid_t, which_clock,
		struct old_timespec32 __user *, tp)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	struct timespec64 ts;
	int err;

	if (!kc)
		return -EINVAL;

	err = kc->clock_get_timespec(which_clock, &ts);

	if (!err && put_old_timespec32(&ts, tp))
		err = -EFAULT;

	return err;
}

/* clock_adjtime32() 转换 old_timex32，复用 do_clock_adjtime，仅在非负状态时把更新字段转换复制回用户。 */
SYSCALL_DEFINE2(clock_adjtime32, clockid_t, which_clock,
		struct old_timex32 __user *, utp)
{
	struct __kernel_timex ktx;
	int err;

	err = get_old_timex32(&ktx, utp);
	if (err)
		return err;

	err = do_clock_adjtime(which_clock, &ktx);

	if (err >= 0 && put_old_timex32(utp, &ktx))
		return -EFAULT;

	return err;
}

/* clock_getres_time32() 支持 NULL tp，只在成功且非空时按 old_timespec32 复制分辨率。 */
SYSCALL_DEFINE2(clock_getres_time32, clockid_t, which_clock,
		struct old_timespec32 __user *, tp)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	struct timespec64 ts;
	int err;

	if (!kc)
		return -EINVAL;

	err = kc->clock_getres(which_clock, &ts);
	if (!err && tp && put_old_timespec32(&ts, tp))
		return -EFAULT;

	return err;
}

#endif

/*
 * sys_clock_nanosleep() for CLOCK_REALTIME and CLOCK_TAI
 */
/*
 * common_nsleep() - REALTIME/TAI 的通用 nanosleep 适配。
 * 把规范 @rqtp 转 ktime，仅按 TIMER_ABSTIME 位选择 ABS/REL 交 hrtimer_nanosleep；这两类 clock 不做 time
 * namespace offset 转换。返回 0、-EINTR/restart 类错误等由 hrtimer 层定义，restart_block 由外层预置。
 */
static int common_nsleep(const clockid_t which_clock, int flags,
			 const struct timespec64 *rqtp)
{
	ktime_t texp = timespec64_to_ktime(*rqtp);

	return hrtimer_nanosleep(texp, flags & TIMER_ABSTIME ?
				 HRTIMER_MODE_ABS : HRTIMER_MODE_REL,
				 which_clock);
}

/*
 * sys_clock_nanosleep() for CLOCK_MONOTONIC and CLOCK_BOOTTIME
 *
 * Absolute nanosleeps for these clocks are time-namespace adjusted.
 */
/*
 * common_nsleep_timens() - MONOTONIC/BOOTTIME 的 time namespace aware nanosleep。
 * 相对时长不受 offset 影响；绝对用户期限先由 timens_ktime_to_host 转 root 坐标，再按 ABS 模式睡眠。返回值
 * 与 hrtimer_nanosleep 相同，@rqtp 只读。
 */
static int common_nsleep_timens(const clockid_t which_clock, int flags,
				const struct timespec64 *rqtp)
{
	ktime_t texp = timespec64_to_ktime(*rqtp);

	if (flags & TIMER_ABSTIME)
		texp = timens_ktime_to_host(which_clock, texp);

	return hrtimer_nanosleep(texp, flags & TIMER_ABSTIME ?
				 HRTIMER_MODE_ABS : HRTIMER_MODE_REL,
				 which_clock);
}

/*
 * clock_nanosleep() - native POSIX nanosleep 分派与 restart 元数据初始化。
 * 无效 clock -EINVAL，无 nsleep -EOPNOTSUPP，输入 copy fault -EFAULT，非规范/负 timespec -EINVAL。绝对请求
 * 按 POSIX 不返回剩余时间，强制 rmtp=NULL；随后把 restart fn 预设为 no-restart，并按 rmtp 是否存在记录
 * TT_NATIVE 与用户指针，最终调用 clock-specific nsleep。具体实现可在中断时改写 restart fn/期限。
 * 本层不统一拒绝未知 flag：通用 hrtimer 适配只观察 TIMER_ABSTIME，ALARM 等实现可自行返回 -EINVAL。
 */
SYSCALL_DEFINE4(clock_nanosleep, const clockid_t, which_clock, int, flags,
		const struct __kernel_timespec __user *, rqtp,
		struct __kernel_timespec __user *, rmtp)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	struct timespec64 t;

	if (!kc)
		return -EINVAL;
	if (!kc->nsleep)
		return -EOPNOTSUPP;

	if (get_timespec64(&t, rqtp))
		return -EFAULT;

	if (!timespec64_valid(&t))
		return -EINVAL;
	if (flags & TIMER_ABSTIME)
		/* 绝对期限重试仍是同一目标，不定义 remaining 输出。 */
		rmtp = NULL;
	current->restart_block.fn = do_no_restart_syscall;
	current->restart_block.nanosleep.type = rmtp ? TT_NATIVE : TT_NONE;
	current->restart_block.nanosleep.rmtp = rmtp;

	return kc->nsleep(which_clock, flags, &t);
}

#ifdef CONFIG_COMPAT_32BIT_TIME

/* clock_nanosleep_time32() 转换 old_timespec32，并以 TT_COMPAT/compat_rmtp 记录剩余时间 ABI；其余同 native。 */
SYSCALL_DEFINE4(clock_nanosleep_time32, clockid_t, which_clock, int, flags,
		struct old_timespec32 __user *, rqtp,
		struct old_timespec32 __user *, rmtp)
{
	const struct k_clock *kc = clockid_to_kclock(which_clock);
	struct timespec64 t;

	if (!kc)
		return -EINVAL;
	if (!kc->nsleep)
		return -EOPNOTSUPP;

	if (get_old_timespec32(&t, rqtp))
		return -EFAULT;

	if (!timespec64_valid(&t))
		return -EINVAL;
	if (flags & TIMER_ABSTIME)
		rmtp = NULL;
	current->restart_block.fn = do_no_restart_syscall;
	current->restart_block.nanosleep.type = rmtp ? TT_COMPAT : TT_NONE;
	current->restart_block.nanosleep.compat_rmtp = rmtp;

	return kc->nsleep(which_clock, flags, &t);
}

#endif

/*
 * 以下 k_clock 静态表声明每种固定 clock 的能力边界：REALTIME 可 set/adj，REALTIME/MONOTONIC/TAI/BOOTTIME
 * 支持通用 timer，RAW/COARSE 只读；MONOTONIC/BOOTTIME 的 sleep 做 time namespace 转换。所有对象静态常驻，
 * timer 调用由 it_lock 串行，clock getter 本身依 timekeeping 并发协议。
 */
static const struct k_clock clock_realtime = {
	.clock_getres		= posix_get_hrtimer_res,
	.clock_get_timespec	= posix_get_realtime_timespec,
	.clock_get_ktime	= posix_get_realtime_ktime,
	.clock_set		= posix_clock_realtime_set,
	.clock_adj		= posix_clock_realtime_adj,
	.nsleep			= common_nsleep,
	.timer_create		= common_timer_create,
	.timer_set		= common_timer_set,
	.timer_get		= common_timer_get,
	.timer_del		= common_timer_del,
	.timer_rearm		= common_hrtimer_rearm,
	.timer_forward		= common_hrtimer_forward,
	.timer_remaining	= common_hrtimer_remaining,
	.timer_try_to_cancel	= common_hrtimer_try_to_cancel,
	.timer_wait_running	= common_timer_wait_running,
	.timer_arm		= common_hrtimer_arm,
};

static const struct k_clock clock_monotonic = {
	.clock_getres		= posix_get_hrtimer_res,
	.clock_get_timespec	= posix_get_monotonic_timespec,
	.clock_get_ktime	= posix_get_monotonic_ktime,
	.nsleep			= common_nsleep_timens,
	.timer_create		= common_timer_create,
	.timer_set		= common_timer_set,
	.timer_get		= common_timer_get,
	.timer_del		= common_timer_del,
	.timer_rearm		= common_hrtimer_rearm,
	.timer_forward		= common_hrtimer_forward,
	.timer_remaining	= common_hrtimer_remaining,
	.timer_try_to_cancel	= common_hrtimer_try_to_cancel,
	.timer_wait_running	= common_timer_wait_running,
	.timer_arm		= common_hrtimer_arm,
};

static const struct k_clock clock_monotonic_raw = {
	.clock_getres		= posix_get_hrtimer_res,
	.clock_get_timespec	= posix_get_monotonic_raw,
};

static const struct k_clock clock_realtime_coarse = {
	.clock_getres		= posix_get_coarse_res,
	.clock_get_timespec	= posix_get_realtime_coarse,
};

static const struct k_clock clock_monotonic_coarse = {
	.clock_getres		= posix_get_coarse_res,
	.clock_get_timespec	= posix_get_monotonic_coarse,
};

static const struct k_clock clock_tai = {
	.clock_getres		= posix_get_hrtimer_res,
	.clock_get_ktime	= posix_get_tai_ktime,
	.clock_get_timespec	= posix_get_tai_timespec,
	.nsleep			= common_nsleep,
	.timer_create		= common_timer_create,
	.timer_set		= common_timer_set,
	.timer_get		= common_timer_get,
	.timer_del		= common_timer_del,
	.timer_rearm		= common_hrtimer_rearm,
	.timer_forward		= common_hrtimer_forward,
	.timer_remaining	= common_hrtimer_remaining,
	.timer_try_to_cancel	= common_hrtimer_try_to_cancel,
	.timer_wait_running	= common_timer_wait_running,
	.timer_arm		= common_hrtimer_arm,
};

static const struct k_clock clock_boottime = {
	.clock_getres		= posix_get_hrtimer_res,
	.clock_get_ktime	= posix_get_boottime_ktime,
	.clock_get_timespec	= posix_get_boottime_timespec,
	.nsleep			= common_nsleep_timens,
	.timer_create		= common_timer_create,
	.timer_set		= common_timer_set,
	.timer_get		= common_timer_get,
	.timer_del		= common_timer_del,
	.timer_rearm		= common_hrtimer_rearm,
	.timer_forward		= common_hrtimer_forward,
	.timer_remaining	= common_hrtimer_remaining,
	.timer_try_to_cancel	= common_hrtimer_try_to_cancel,
	.timer_wait_running	= common_timer_wait_running,
	.timer_arm		= common_hrtimer_arm,
};

/*
 * posix_clocks[] - 非负标准 clockid 到 k_clock 的稀疏静态映射。
 * CPU、ALARM 与可选 AUX 由各自实现表接管；空洞/越界无效。数组只读，索引前由 clockid_to_kclock 做边界与
 * speculation 防护。
 */
static const struct k_clock * const posix_clocks[] = {
	[CLOCK_REALTIME]		= &clock_realtime,
	[CLOCK_MONOTONIC]		= &clock_monotonic,
	[CLOCK_PROCESS_CPUTIME_ID]	= &clock_process,
	[CLOCK_THREAD_CPUTIME_ID]	= &clock_thread,
	[CLOCK_MONOTONIC_RAW]		= &clock_monotonic_raw,
	[CLOCK_REALTIME_COARSE]		= &clock_realtime_coarse,
	[CLOCK_MONOTONIC_COARSE]	= &clock_monotonic_coarse,
	[CLOCK_BOOTTIME]		= &clock_boottime,
	[CLOCK_REALTIME_ALARM]		= &alarm_clock,
	[CLOCK_BOOTTIME_ALARM]		= &alarm_clock,
	[CLOCK_TAI]			= &clock_tai,
#ifdef CONFIG_POSIX_AUX_CLOCKS
	[CLOCK_AUX ... CLOCK_AUX_LAST]	= &clock_aux,
#endif
};

/*
 * clockid_to_kclock() - 把任意 @id 分类为固定、fd 动态或 CPU clock 操作表。
 * 负 id 的低位字段等于 CLOCKFD 时返回 clock_posix_dynamic，否则交 clock_posix_cpu 继续验证 pid/type；
 * 非负越过 posix_clocks 返回 NULL，合法范围用 array_index_nospec 后读取，数组空洞也返回 NULL。
 * 返回静态借用指针，无引用/锁；具体 clock 是否支持某操作由调用者检查相应函数指针。
 */
static const struct k_clock *clockid_to_kclock(const clockid_t id)
{
	clockid_t idx = id;

	if (id < 0) {
		return (id & CLOCKFD_MASK) == CLOCKFD ?
			&clock_posix_dynamic : &clock_posix_cpu;
	}

	if (id >= ARRAY_SIZE(posix_clocks))
		return NULL;

	return posix_clocks[array_index_nospec(idx, ARRAY_SIZE(posix_clocks))];
}

/*
 * posixtimer_init() - core_initcall 阶段建立 k_itimer slab 与全局 ID hash。
 * 创建带 SLAB_ACCOUNT 的对齐 cache；BASE_SMALL 选择 512 bucket，否则以 512*num_possible_cpus 向上取 2 的幂。
 * alloc_large_system_hash 返回数组与 shift，随后发布 size/mask，并逐 bucket 初始化锁/head。成功固定返回 0；
 * cache 若未建立，alloc_posix_timer 会把后续 timer_create 降级为 -EAGAIN，hash 分配遵循早期分配器策略。
 */
static int __init posixtimer_init(void)
{
	unsigned long i, size;
	unsigned int shift;

	posix_timers_cache = kmem_cache_create("posix_timers_cache",
					       sizeof(struct k_itimer),
					       __alignof__(struct k_itimer),
					       SLAB_ACCOUNT, NULL);

	if (IS_ENABLED(CONFIG_BASE_SMALL))
		size = 512;
	else
		size = roundup_pow_of_two(512 * num_possible_cpus());

	timer_buckets = alloc_large_system_hash("posixtimers", sizeof(*timer_buckets),
						size, 0, 0, &shift, NULL, size, size);
	size = 1UL << shift;
	timer_hashmask = size - 1;

	for (i = 0; i < size; i++) {
		spin_lock_init(&timer_buckets[i].lock);
		INIT_HLIST_HEAD(&timer_buckets[i].head);
	}
	return 0;
}
core_initcall(posixtimer_init);
