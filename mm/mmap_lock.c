// SPDX-License-Identifier: GPL-2.0
#define CREATE_TRACE_POINTS
#include <trace/events/mmap_lock.h>

#include <linux/mm.h>
#include <linux/cgroup.h>
#include <linux/memcontrol.h>
#include <linux/mmap_lock.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
/* RCU 保护无引用的树查找阶段，引用计数随后接管 VMA 生命周期。 */
#include <linux/rcupdate.h>
#include <linux/smp.h>
#include <linux/trace_events.h>
#include <linux/local_lock.h>

/*
 * 学习提示：mmap_lock 与 per-VMA lock 共同稳定地址空间；无锁查找还依赖 RCU。
 * VMA 交给读者前必须取得受限引用，写者则用序号和高位标志排斥新读者。
 */
EXPORT_TRACEPOINT_SYMBOL(mmap_lock_start_locking);
EXPORT_TRACEPOINT_SYMBOL(mmap_lock_acquire_returned);
EXPORT_TRACEPOINT_SYMBOL(mmap_lock_released);

#ifdef CONFIG_TRACING
/*
 * Trace calls must be in a separate file, as otherwise there's a circular
 * dependency between linux/mmap_lock.h and trace/events/mmap_lock.h.
 */
/* 译注：trace 调用必须放在独立文件，否则 mmap_lock.h 与 mmap_lock trace 事件头会形成循环依赖。 */

/*
 * 业务背景：mmap 锁 wrapper 在真正尝试前发出统一 tracepoint，供延迟/竞争观测配对。
 * 入参：mm 是借用地址空间；write 区分写锁与读锁请求。
 * 出参/返回：void；只产生 trace 事件，不取得锁或 mm 引用。
 * 注意事项：可在尚未持锁时调用，事件不证明后续获取成功。
 */
void __mmap_lock_do_trace_start_locking(struct mm_struct *mm, bool write)
{
	/* 这是尝试取锁事件，不代表锁已经取得。 */
	trace_mmap_lock_start_locking(mm, write);
}
EXPORT_SYMBOL(__mmap_lock_do_trace_start_locking);

/*
 * 业务背景：锁 API 返回后记录获取结果，使观测者闭合 start→returned 尝试区间。
 * 入参：mm/write 与开始事件一致；success 是真实 API 结果。
 * 出参/返回：void；只发 trace，不改变锁或引用状态。
 * 注意事项：success=false 也必须上报，调用者不能据事件替代持锁判断。
 */
void __mmap_lock_do_trace_acquire_returned(struct mm_struct *mm, bool write,
					   bool success)
{
	/* success 让观测端区分成功与 trylock/可杀等待失败。 */
	trace_mmap_lock_acquire_returned(mm, write, success);
}
EXPORT_SYMBOL(__mmap_lock_do_trace_acquire_returned);

/*
 * 业务背景：读/写锁真实释放边界发出事件，供持锁时长统计完成配对。
 * 入参：mm 借用；write 指明刚释放的锁模式。
 * 出参/返回：void；只记录事件，不执行实际 unlock。
 * 注意事项：必须由已完成解锁语义的 wrapper 调用，模式错误会污染观测。
 */
void __mmap_lock_do_trace_released(struct mm_struct *mm, bool write)
{
	/* write 区分共享和独占持有，事件位于真实释放边界。 */
	trace_mmap_lock_released(mm, write);
}
EXPORT_SYMBOL(__mmap_lock_do_trace_released);
#endif /* CONFIG_TRACING */

#ifdef CONFIG_MMU
#ifdef CONFIG_PER_VMA_LOCK

/* State shared across __vma_[start, end]_exclude_readers. */
struct vma_exclude_readers_state {
	/* 该状态对象把一次排斥操作的输入和清理责任集中传递。 */
	/* Input parameters. */
	/* vma 是本次排斥目标借用指针；state 是 rcuwait 睡眠模式。 */
	struct vm_area_struct *vma;
	int state; /* TASK_KILLABLE or TASK_UNINTERRUPTIBLE. */
	/* detaching 决定目标 refcnt 是 attached 基准 1 还是最终 0。 */
	bool detaching;

	/* Output parameters. */
	/* detached 输出表示计数已归零、VMA 已不可再被读者取得。 */
	bool detached;
	/* exclusive=true 才要求调用者结束排斥状态。 */
	bool exclusive; /* Are we exclusively locked? */
};

/*
 * Now that all readers have been evicted, mark the VMA as being out of the
 * 'exclude readers' state.
 */
/*
 * 业务背景：写者排空既有读引用后移除 EXCLUDE 高位标志，结束排斥或完成 detach。
 * 入参：ves 记录借用 VMA且调用者当前持 exclusive 状态。
 * 出参/返回：void；减去标志并写 ves->detached，释放 lockdep exclusive 状态。
 * 注意事项：只能调用一次；已 detached 再调用会下溢，普通写路径预期仍 attached。
 */
static void __vma_end_exclude_readers(struct vma_exclude_readers_state *ves)
{
	/* vma 是 ves 内借用目标的局部别名。 */
	struct vm_area_struct *vma = ves->vma;

	/* detached 对象计数已归零，不能再次移除排斥标志。 */
	VM_WARN_ON_ONCE(ves->detached);

	/* 去掉高位标志；结果为零表示 detach 路径完成最终脱链。 */
	ves->detached = refcount_sub_and_test(VM_REFCNT_EXCLUDE_READERS_FLAG,
					      &vma->vm_refcnt);
	__vma_lockdep_release_exclusive(vma);
}

/*
 * 业务背景：等待条件随普通写/最终 detach 不同，需要计算“读者全排空”目标编码。
 * 入参：ves 借用，只读取 detaching。
 * 出参/返回：返回 EXCLUDE 标志加 attached 基准 1或 detach 基准0；无副作用。
 * 注意事项：该值仅在已设置排斥标志期间解释有效。
 */
static unsigned int get_target_refcnt(struct vma_exclude_readers_state *ves)
{
	/* 普通写者保留 attached 基准引用，detach 写者等待纯标志值。 */
	/* tgt 是去掉 EXCLUDE 标志前读者应下降到的基准引用数。 */
	const unsigned int tgt = ves->detaching ? 0 : 1;

	return tgt | VM_REFCNT_EXCLUDE_READERS_FLAG;
}

/*
 * Mark the VMA as being in a state of excluding readers, check to see if any
 * VMA read locks are indeed held, and if so wait for them to be released.
 *
 * Note that this function pairs with vma_refcount_put() which will wake up this
 * thread when it detects that the last reader has released its lock.
 *
 * The ves->state parameter ought to be set to TASK_UNINTERRUPTIBLE in cases
 * where we wish the thread to sleep uninterruptibly or TASK_KILLABLE if a fatal
 * signal is permitted to kill it.
 *
 * The function sets the ves->exclusive parameter to true if readers were
 * excluded, or false if the VMA was detached or an error arose on wait.
 *
 * If the function indicates an exclusive lock was acquired via ves->exclusive
 * the caller is required to invoke __vma_end_exclude_readers() once the
 * exclusive state is no longer required.
 *
 * If ves->state is set to something other than TASK_UNINTERRUPTIBLE, the
 * function may also return -EINTR to indicate a fatal signal was received while
 * waiting.  Otherwise, the function returns 0.
 */
/*
 * 译注：函数给 VMA 设置排斥读者状态并等待既存读锁释放；vma_refcount_put() 在最后读者退出时唤醒。
 * state 选择不可中断或可杀等待。若成功排斥则 exclusive=true，调用者结束独占时必须调用 end；
 * 若 VMA 已 detach 或等待出错则 exclusive=false。非不可中断等待收到致命信号可返回 -EINTR，否则返回0。
 */
/*
 * 业务背景：持 mmap 写锁的 VMA 写者先设置高位门禁，再等待所有既存 per-VMA 读引用退出。
 * 入参：ves 输入 vma/state/detaching，输出 detached/exclusive；对象由调用者栈拥有。
 * 出参/返回：成功 0；可杀等待被致命信号打断返回 -EINTR并撤销门禁；detached 也以0报告。
 * 注意事项：调用者必须持 mmap 写锁；exclusive=true 时必须配对 end，detached 时不得复活 VMA。
 */
static int __vma_start_exclude_readers(struct vma_exclude_readers_state *ves)
{
	/* vma/tgt_refcnt 为稳定别名与等待目标；err 保存 rcuwait 中断结果。 */
	struct vm_area_struct *vma = ves->vma;
	unsigned int tgt_refcnt = get_target_refcnt(ves);
	int err = 0;

	/* mmap 写锁串行化 attach/detach，是排斥协议的外层所有权。 */
	mmap_assert_write_locked(vma->vm_mm);

	/*
	 * If vma is detached then only vma_mark_attached() can raise the
	 * vm_refcnt. mmap_write_lock prevents racing with vma_mark_attached().
	 *
	 * See the comment describing the vm_area_struct->vm_refcnt field for
	 * details of possible refcnt values.
	 */
	/* 译注：detached VMA 只有 vma_mark_attached() 能从零增加引用，而当前 mmap 写锁排斥该路径。 */
	/* 从零加失败说明 VMA 已脱离，绝不能通过本路径复活。 */
	if (!refcount_add_not_zero(VM_REFCNT_EXCLUDE_READERS_FLAG, &vma->vm_refcnt)) {
		ves->detached = true;
		return 0;
	}

	/* 标志阻止新读者，rcuwait 再等待既存引用降到目标值。 */
	__vma_lockdep_acquire_exclusive(vma);
	err = rcuwait_wait_event(&vma->vm_mm->vma_writer_wait,
		   refcount_read(&vma->vm_refcnt) == tgt_refcnt,
		   ves->state);
	if (err) {
		/* 等待被信号打断时必须撤销排斥标志。 */
		__vma_end_exclude_readers(ves);
		return err;
	}

	/* 达到目标值后才发布 exclusive，明确交给调用者配对释放。 */
	__vma_lockdep_stat_mark_acquired(vma);
	ves->exclusive = true;
	return 0;
}

/*
 * 业务背景：修改 VMA 字段前把当前 mm 写序号发布到 VMA，并在发布期间排斥 per-VMA 读者。
 * 入参：vma 借用且调用者持 mmap 写锁；state 指定不可中断或可杀等待。
 * 出参/返回：成功 0；等待中断返回负 errno；返回时排斥门已解除且 VMA 应仍 attached。
 * 注意事项：WRITE_ONCE 仅处理无锁预检数据竞争，真正同步来自 refcnt/序号 release-acquire 协议。
 */
int __vma_start_write(struct vm_area_struct *vma, int state)
{
	/* 排斥读者前采样 mm 序号，稍后写入 VMA 作为本轮写标记。 */
	/* mm_lock_seq 是本轮写代次；ves 是栈上输入输出状态；err 传播等待错误。 */
	const unsigned int mm_lock_seq = __vma_raw_mm_seqnum(vma);
	struct vma_exclude_readers_state ves = {
		.vma = vma,
		.state = state,
	};
	int err;

	/* state 决定等待能否被致命信号终止。 */
	err = __vma_start_exclude_readers(&ves);
	if (err) {
		WARN_ON_ONCE(ves.detached);
		return err;
	}

	/*
	 * We should use WRITE_ONCE() here because we can have concurrent reads
	 * from the early lockless pessimistic check in vma_start_read().
	 * We don't really care about the correctness of that early check, but
	 * we should use WRITE_ONCE() for cleanliness and to keep KCSAN happy.
	 */
	/* WRITE_ONCE 约束无锁预检的数据竞争，正式同步仍靠引用协议。 */
	WRITE_ONCE(vma->vm_lock_seq, mm_lock_seq);

	if (ves.exclusive) {
		/* 序号发布后即可重新放行读者，他们会因相等而回退。 */
		__vma_end_exclude_readers(&ves);
		/* VMA should remain attached. */
		WARN_ON_ONCE(ves.detached);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(__vma_start_write);

/*
 * 业务背景：VMA 从 Maple Tree/链表拆除后必须不可中断地排空竞速读者并把 refcnt 变为零。
 * 入参：vma 借用；调用者持 VMA 写锁与 mm 写锁并已开始 detach。
 * 出参/返回：void；返回时 detached=true，新读者无法增加引用。
 * 注意事项：不允许取消；唯一剩余读者是看到写锁后会自行回退的竞速获取者。
 */
void __vma_exclude_readers_for_detach(struct vm_area_struct *vma)
{
	/* detach 不能半途取消，因此不可中断地等待引用归零。 */
	/* ves 在栈上记录最终 detached 输出；err 仅用于验证不可中断路径。 */
	struct vma_exclude_readers_state ves = {
		.vma = vma,
		.state = TASK_UNINTERRUPTIBLE,
		.detaching = true,
	};
	int err;

	/*
	 * Wait until the VMA is detached with no readers. Since we hold the VMA
	 * write lock, the only read locks that might be present are those from
	 * threads trying to acquire the read lock and incrementing the
	 * reference count before realising the write lock is held and
	 * decrementing it.
	 */
	/* 返回时必须观察到 detached；exclusive 表示还欠最终减标志。 */
	err = __vma_start_exclude_readers(&ves);
	if (!err && ves.exclusive) {
		/*
		 * Once this is complete, no readers can increment the
		 * reference count, and the VMA is marked detached.
		 */
		/* 移除标志后计数归零，从此不能再接受新读引用。 */
		__vma_end_exclude_readers(&ves);
	}
	/* If an error arose but we were detached anyway, we don't care. */
	WARN_ON_ONCE(!ves.detached);
}

/*
 * Try to read-lock a vma. The function is allowed to occasionally yield false
 * locked result to avoid performance overhead, in which case we fall back to
 * using mmap_lock. The function should never yield false unlocked result.
 * False locked result is possible if mm_lock_seq overflows or if vma gets
 * reused and attached to a different mm before we lock it.
 * Returns the vma on success, NULL on failure to lock and EAGAIN if vma got
 * detached.
 *
 * IMPORTANT: RCU lock must be held upon entering the function, but upon error
 *            IT IS RELEASED. The caller must handle this correctly.
 */
/*
 * 译注：该尝试读锁允许偶发“其实已锁却判断失败”并退到 mmap_lock，以避免额外开销，但绝不能把未锁
 * 误判为已锁；序号回绕或 VMA 复用到另一 mm 可造成前者。成功返回 VMA，普通失败 NULL，detach
 * 返回 -EAGAIN。入口必须持 RCU；任何错误返回都已由本函数释放 RCU，调用者必须遵守这一不对称契约。
 */
/*
 * 业务背景：RCU 树查找得到无引用 VMA 后，尝试把生命周期交给受限 refcount 并验证写序号。
 * 入参：mm 借用；vma 仅受入口 RCU 保护，调用者必须已持 rcu_read_lock。
 * 出参/返回：成功返回带 per-VMA 读引用对象且保留 RCU；普通失败 NULL、detached -EAGAIN，错误均代解 RCU。
 * 注意事项：允许假性“已锁”导致回退但绝不假性未锁；对象换 mm 时临时 mmgrab 覆盖 wakeup 窗口。
 */
static inline struct vm_area_struct *vma_start_read(struct mm_struct *mm,
						    struct vm_area_struct *vma)
{
	/* other_mm 跨 VMA put 稳定复用目标 mm；oldcnt 返回加引用失败前的计数编码。 */
	struct mm_struct *other_mm;
	int oldcnt;

	/* 入口 RCU 稳定未引用对象；错误返回会代调用者退出 RCU。 */
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "no rcu lock held");
	/*
	 * Check before locking. A race might cause false locked result.
	 * We can use READ_ONCE() for the mm_lock_seq here, and don't need
	 * ACQUIRE semantics, because this is just a lockless check whose result
	 * we don't rely on for anything - the mm_lock_seq read against which we
	 * need ordering is below.
	 */
	/* 译注：加引用前的序号检查只作悲观预检，允许竞态导致假性回退，所以 READ_ONCE 足够且无需 acquire。 */
	/* 序号相等表示写者覆盖该 VMA，快速回退避免引用竞争。 */
	if (READ_ONCE(vma->vm_lock_seq) == READ_ONCE(mm->mm_lock_seq.sequence)) {
		vma = NULL;
		goto err;
	}

	/*
	 * If VM_REFCNT_EXCLUDE_READERS_FLAG is set,
	 * __refcount_inc_not_zero_limited_acquire() will fail because
	 * VM_REFCNT_LIMIT is less than VM_REFCNT_EXCLUDE_READERS_FLAG.
	 *
	 * Acquire fence is required here to avoid reordering against later
	 * vm_lock_seq check and checks inside lock_vma_under_rcu().
	 */
	/* acquire 加引用同时拒绝零值和带 EXCLUDE_READERS 的高位值。 */
	if (unlikely(!__refcount_inc_not_zero_limited_acquire(&vma->vm_refcnt, &oldcnt,
							      VM_REFCNT_LIMIT))) {
		/* return EAGAIN if vma got detached from under us */
		/* oldcnt=0 表示被隔离，EAGAIN 提示树遍历者重启定位。 */
		vma = oldcnt ? NULL : ERR_PTR(-EAGAIN);
		goto err;
	}

	/* 自此引用计数稳定 VMA，lockdep 同步记录逻辑读锁。 */
	__vma_lockdep_acquire_read(vma);

	/* 对象复用后可能属于另一 mm，必须走特殊的 unstable 路径。 */
	if (unlikely(vma->vm_mm != mm))
		goto err_unstable;

	/*
	 * Overflow of vm_lock_seq/mm_lock_seq might produce false locked result.
	 * False unlocked result is impossible because we modify and check
	 * vma->vm_lock_seq under vma->vm_refcnt protection and mm->mm_lock_seq
	 * modification invalidates all existing locks.
	 *
	 * We must use ACQUIRE semantics for the mm_lock_seq so that if we are
	 * racing with vma_end_write_all(), we only start reading from the VMA
	 * after it has been unlocked.
	 * This pairs with RELEASE semantics in vma_end_write_all().
	 */
	/*
	 * 译注：序号回绕可造成假性“被写锁”结果，但不会假性放行，因为 VMA 序号在 refcnt 保护下修改，
	 * mm 序号变化会使现有锁失效。正式 mm 序号读取必须 acquire，与 vma_end_write_all() 的 release 配对。
	 */
	/* acquire 读取与 write-all 的 release 配对，保证解锁后才读字段。 */
	if (unlikely(vma->vm_lock_seq == raw_read_seqcount(&mm->mm_lock_seq))) {
		vma_refcount_put(vma);
		vma = NULL;
		goto err;
	}

	/* 成功保留 VMA 读引用，RCU 则由上层在稳定后退出。 */
	return vma;
err:
	/* NULL/ERR_PTR 的共同契约：此处已经消费入口 RCU 锁。 */
	rcu_read_unlock();

	return vma;
err_unstable:
	/*
	 * If vma got attached to another mm from under us, that mm is not
	 * stable and can be freed in the narrow window after vma->vm_refcnt
	 * is dropped and before rcuwait_wake_up(mm) is called. Grab it before
	 * releasing vma->vm_refcnt.
	 */
	/* 暂存另一个 mm，覆盖放弃 VMA 引用后的 wakeup 生命周期窗口。 */
	other_mm = vma->vm_mm; /* use a copy as vma can be freed after we drop vm_refcnt */

	/* __mmdrop() is a heavy operation, do it after dropping RCU lock. */
	/* 重型 mmdrop 放在 RCU 外，临时 mm 引用则跨越 vma_refcount_put。 */
	rcu_read_unlock();
	mmgrab(other_mm);
	vma_refcount_put(vma);
	mmdrop(other_mm);

	return NULL;
}

/*
 * Lookup and lock a VMA under RCU protection. Returned VMA is guaranteed to be
 * stable and not isolated. If the VMA is not found or is being modified the
 * function returns NULL.
 */
/* 译注：在 RCU 下查找并锁定 VMA；成功对象稳定且未隔离，未找到或正在修改则返回 NULL。 */
/*
 * 业务背景：page fault 快路径在 RCU 下按地址查 Maple Tree，并把候选转换成稳定 per-VMA 读锁。
 * 入参：mm 借用；address 是目标用户虚拟地址。
 * 出参/返回：成功返回带读引用且已退出 RCU的覆盖 VMA；未找到/写竞争返回 NULL。
 * 注意事项：detached 触发游标重置重试；最终边界复核防止并发收缩返回错误 VMA。
 */
struct vm_area_struct *lock_vma_under_rcu(struct mm_struct *mm,
					  unsigned long address)
{
	/* Maple Tree 游标定位 address，retry 用于 VMA 被替换后的重查。 */
	MA_STATE(mas, &mm->mm_mt, address, address);
	struct vm_area_struct *vma;

retry:
	/* 未取得引用前，树节点和 VMA 只能在 RCU 临界区内解引用。 */
	rcu_read_lock();
	vma = mas_walk(&mas);
	if (!vma) {
		rcu_read_unlock();
		goto inval;
	}

	/* 失败时 vma_start_read 已解 RCU，分支不得重复 unlock。 */
	vma = vma_start_read(mm, vma);
	if (IS_ERR_OR_NULL(vma)) {
		/* Check if the VMA got isolated after we found it */
		if (PTR_ERR(vma) == -EAGAIN) {
			/* 隔离可能伴随替换，重置游标后从原地址重走。 */
			count_vm_vma_lock_event(VMA_LOCK_MISS);
			/* The area was replaced with another one */
			mas_set(&mas, address);
			goto retry;
		}

		/* Failed to lock the VMA */
		goto inval;
	}
	/*
	 * At this point, we have a stable reference to a VMA: The VMA is
	 * locked and we know it hasn't already been isolated.
	 * From here on, we can access the VMA without worrying about which
	 * fields are accessible for RCU readers.
	 */
	/* 引用已稳定对象，可缩短 RCU 临界区再验证地址边界。 */
	rcu_read_unlock();

	/* Check if the vma we locked is the right one. */
	/* 查找到加锁间 VMA 可收缩，最终检查防止返回错误覆盖。 */
	if (unlikely(address < vma->vm_start || address >= vma->vm_end)) {
		vma_end_read(vma);
		goto inval;
	}

	return vma;

inval:
	/* NULL 让调用者退回 mmap_lock 慢路径，同时记录 abort。 */
	count_vm_vma_lock_event(VMA_LOCK_ABORT);
	return NULL;
}

/*
 * 业务背景：RCU/per-VMA 快路径不稳定时，在全局 mmap 读锁下从原位置重查并取得独立 VMA 引用。
 * 入参：mm/vmi 借用；from_addr 是重建迭代器的地址。
 * 出参/返回：返回带读引用 VMA、无后继 NULL，信号/引用溢出返回 ERR_PTR；返回时 mmap 锁已释放。
 * 注意事项：可睡眠且 killable；VMA 引用必须在解全局锁前取得。
 */
static struct vm_area_struct *lock_next_vma_under_mmap_lock(struct mm_struct *mm,
							    struct vma_iterator *vmi,
							    unsigned long from_addr)
{
	/* 慢路径在全局读锁内重查，再转换为独立 VMA 读引用。 */
	struct vm_area_struct *vma;
	int ret;

	/* 信号错误以 ERR_PTR 交还，调用者仍会恢复 RCU 状态。 */
	ret = mmap_read_lock_killable(mm);
	if (ret)
		return ERR_PTR(ret);

	/* Lookup the vma at the last position again under mmap_read_lock */
	/* 必须在锁内从原位置重建迭代器，旧 RCU 游标不再可信。 */
	vma_iter_set(vmi, from_addr);
	vma = vma_next(vmi);
	if (vma) {
		/* Very unlikely vma->vm_refcnt overflow case */
		if (unlikely(!vma_start_read_locked(vma)))
			vma = ERR_PTR(-EAGAIN);
	}

	/* VMA 引用已接管生命期，因此可以释放全局 mmap 锁。 */
	mmap_read_unlock(mm);

	return vma;
}

/*
 * 业务背景：RCU 迭代器逐个取得稳定 VMA，遇到替换/空洞竞态时用写序号验证或退到 mmap_lock。
 * 入参：mm/vmi 借用；from_addr 是不可跳过的上次位置；调用者入口持 RCU。
 * 出参/返回：成功为带读引用 VMA，结束 NULL，慢路径错误 ERR_PTR；除成功快返外保持调用者 RCU 约定。
 * 注意事项：vma_start_read 失败会消费 RCU，所有 retry/fallback 必须精确恢复；候选失败先 vma_end_read。
 */
struct vm_area_struct *lock_next_vma(struct mm_struct *mm,
				     struct vma_iterator *vmi,
				     unsigned long from_addr)
{
	/* 调用者维持 RCU；fallback 会短暂退出并在返回前恢复。 */
	struct vm_area_struct *vma;
	unsigned int mm_wr_seq;
	bool mmap_unlocked;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "no rcu read lock held");
retry:
	/* 推测序号用于判断稍后观察到的地址空洞是否跨越写者。 */
	/* Start mmap_lock speculation in case we need to verify the vma later */
	mmap_unlocked = mmap_lock_speculate_try_begin(mm, &mm_wr_seq);
	vma = vma_next(vmi);
	if (!vma)
		return NULL;

	/* 失败会消费 RCU，重试和 fallback 必须显式重新进入。 */
	vma = vma_start_read(mm, vma);
	if (IS_ERR_OR_NULL(vma)) {
		/*
		 * Retry immediately if the vma gets detached from under us.
		 * Infinite loop should not happen because the vma we find will
		 * have to be constantly knocked out from under us.
		 */
		/* 译注：若候选在脚下被 detach 立即重试；只有它持续被替换才可能无限循环。 */
		if (PTR_ERR(vma) == -EAGAIN) {
			/* reset to search from the last address */
			/* 从 from_addr 重启，不能跳过替换到原位置的新 VMA。 */
			rcu_read_lock();
			vma_iter_set(vmi, from_addr);
			goto retry;
		}

		goto fallback;
	}

	/* Verify the vma is not behind the last search position. */
	/* 候选 VMA 不能完整落在上次搜索位置之前。 */
	if (unlikely(from_addr >= vma->vm_end))
		goto fallback_unlock;

	/*
	 * vma can be ahead of the last search position but we need to verify
	 * it was not shrunk after we found it and another vma has not been
	 * installed ahead of it. Otherwise we might observe a gap that should
	 * not be there.
	 */
	/* 译注：候选可位于搜索位置之后，但必须验证它未在查找后收缩，且前方未插入另一 VMA，否则会观察到虚假空洞。 */
	if (from_addr < vma->vm_start) {
		/* 出现空洞时检查推测序号，排除并发收缩或插入。 */
		/* Verify only if the address space might have changed since vma lookup. */
		if (!mmap_unlocked || mmap_lock_speculate_retry(mm, mm_wr_seq)) {
			vma_iter_set(vmi, from_addr);
			if (vma != vma_next(vmi))
				goto fallback_unlock;
		}
	}

	return vma;

fallback_unlock:
	/* 退出 RCU 并放弃候选引用，再交由全局锁慢路径重查。 */
	rcu_read_unlock();
	vma_end_read(vma);
fallback:
	/* 无论成功还是错误，返回前均恢复调用者要求的 RCU 状态。 */
	vma = lock_next_vma_under_mmap_lock(mm, vmi, from_addr);
	rcu_read_lock();
	/* Reinitialize the iterator after re-entering rcu read section */
	vma_iter_set(vmi, IS_ERR_OR_NULL(vma) ? from_addr : vma->vm_end);

	return vma;
}
#endif /* CONFIG_PER_VMA_LOCK */

#ifdef CONFIG_LOCK_MM_AND_FIND_VMA
#include <linux/extable.h>

/*
 * 业务背景：fault 入口必须避免内核在已持 mmap 写锁的 bug 路径阻塞自死锁，先 trylock 再审查异常表。
 * 入参：mm 借用；regs 可 NULL，非 NULL 用于区分用户态和可修复内核指令。
 * 出参/返回：成功取得 mmap 读锁返回 true；不安全内核 fault 或可杀等待失败返回 false且不持锁。
 * 注意事项：可能睡眠；内核态仅异常表声明可 fault 的指令允许阻塞等待。
 */
static inline bool get_mmap_lock_carefully(struct mm_struct *mm, struct pt_regs *regs)
{
	/* trylock 避免内核在已持写锁触发故障时自死锁。 */
	if (likely(mmap_read_trylock(mm)))
		return true;

	if (regs && !user_mode(regs)) {
		/* 内核态仅让异常表声明可恢复的指令进入阻塞取锁。 */
		unsigned long ip = exception_ip(regs);
		if (!search_exception_tables(ip))
			return false;
	}

	/* 用户态或可修复故障允许可杀地等待读锁。 */
	return !mmap_read_lock_killable(mm);
}

/*
 * 业务背景：栈扩展理想上原子把 mmap 读锁升级为写锁，避免释放窗口和 VMA 重查。
 * 入参：mm 是当前持读锁的借用地址空间。
 * 出参/返回：当前实现恒 false且不改变锁；为未来 rwsem 原子升级预留。
 * 注意事项：调用者必须按失败路径释放读锁、取得写锁并重新查找，不能假定仍持原候选。
 */
static inline bool mmap_upgrade_trylock(struct mm_struct *mm)
{
	/* 当前恒失败，接口为未来原子读转写升级预留。 */
	/*
	 * We don't have this operation yet.
	 *
	 * It should be easy enough to do: it's basically a
	 *    atomic_long_try_cmpxchg_acquire()
	 * from RWSEM_READER_BIAS -> RWSEM_WRITER_LOCKED, but
	 * it also needs the proper lockdep magic etc.
	 */
	/* 译注：未来可用 rwsem reader-bias→writer-locked 的原子 cmpxchg 实现，但还必须补齐 lockdep 语义。 */
	return false;
}

/*
 * 业务背景：无原子升级时先释放读锁，再按 fault 安全规则取得 killable 写锁以扩展栈。
 * 入参：mm 当前持读锁；regs 可 NULL并用于异常表检查。
 * 出参/返回：成功持 mmap 写锁返回 true；不安全/等待失败返回 false且不持任何 mmap 锁。
 * 注意事项：释放窗口允许 VMA 树变化，成功后调用者必须完整重查候选与 VM_GROWSDOWN。
 */
static inline bool upgrade_mmap_lock_carefully(struct mm_struct *mm, struct pt_regs *regs)
{
	/* 非原子升级先放读锁，故调用者取得写锁后必须重查 VMA。 */
	mmap_read_unlock(mm);
	if (regs && !user_mode(regs)) {
		unsigned long ip = exception_ip(regs);
		if (!search_exception_tables(ip))
			return false;
	}
	/* 同样检查异常表，避免内核 bug 永久等待写锁。 */
	return !mmap_write_lock_killable(mm);
}

/*
 * Helper for page fault handling.
 *
 * This is kind of equivalent to "mmap_read_lock()" followed
 * by "find_extend_vma()", except it's a lot more careful about
 * the locking (and will drop the lock on failure).
 *
 * For example, if we have a kernel bug that causes a page
 * fault, we don't want to just use mmap_read_lock() to get
 * the mm lock, because that would deadlock if the bug were
 * to happen while we're holding the mm lock for writing.
 *
 * So this checks the exception tables on kernel faults in
 * order to only do this all for instructions that are actually
 * expected to fault.
 *
 * We can also actually take the mm lock for writing if we
 * need to extend the vma, which helps the VM layer a lot.
 */
/*
 * 译注：该 fault helper 类似“mmap_read_lock()+find_extend_vma()”，但失败会主动解锁并避免内核 bug
 * 在已持写锁时死锁；内核 fault 只有异常表允许的指令才阻塞取锁。若需向下扩栈，它会取得写锁完成修改。
 */
/*
 * 业务背景：通用 page fault 安全取得 mmap 锁、查找覆盖 VMA，并在合法 VM_GROWSDOWN 时扩展栈。
 * 入参：mm 借用；addr 是 fault 地址；regs 可 NULL或描述 fault 上下文。
 * 出参/返回：成功返回覆盖 VMA且调用者持 mmap 读锁；失败 NULL且不持锁。
 * 注意事项：读→写非原子升级后必须重查；扩展发布后写锁降级为读锁，所有错误逆序解锁。
 */
struct vm_area_struct *lock_mm_and_find_vma(struct mm_struct *mm,
			unsigned long addr, struct pt_regs *regs)
{
	/* 成功返回持 mmap 读锁；所有失败出口负责释放已取得的锁。 */
	struct vm_area_struct *vma;

	if (!get_mmap_lock_carefully(mm, regs))
		return NULL;

	/* 已覆盖 addr 时无需修改树，直接保留读锁返回。 */
	vma = find_vma(mm, addr);
	if (likely(vma && (vma->vm_start <= addr)))
		return vma;

	/*
	 * Well, dang. We might still be successful, but only
	 * if we can extend a vma to do so.
	 */
	/* 只有紧随地址且允许向下增长的栈 VMA 才可能扩展。 */
	if (!vma || !(vma->vm_flags & VM_GROWSDOWN)) {
		mmap_read_unlock(mm);
		return NULL;
	}

	/*
	 * We can try to upgrade the mmap lock atomically,
	 * in which case we can continue to use the vma
	 * we already looked up.
	 *
	 * Otherwise we'll have to drop the mmap lock and
	 * re-take it, and also look up the vma again,
	 * re-checking it.
	 */
	/* 释放再取写锁存在窗口，所以升级后不能复用旧查找结果。 */
	if (!mmap_upgrade_trylock(mm)) {
		if (!upgrade_mmap_lock_carefully(mm, regs))
			return NULL;

		/* 写锁内重新验证存在性、覆盖关系与增长属性。 */
		vma = find_vma(mm, addr);
		if (!vma)
			goto fail;
		if (vma->vm_start <= addr)
			goto success;
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto fail;
	}

	/* 栈边界只在写锁下改变，错误统一走 fail 解锁。 */
	if (expand_stack_locked(vma, addr))
		goto fail;

success:
	/* 降级兑现成功返回持读锁的接口契约。 */
	mmap_write_downgrade(mm);
	return vma;

fail:
	/* 写锁阶段的所有失败在此收口，防止锁泄漏。 */
	mmap_write_unlock(mm);
	return NULL;
}
#endif /* CONFIG_LOCK_MM_AND_FIND_VMA */

#else /* CONFIG_MMU */

/*
 * At least xtensa ends up having protection faults even with no
 * MMU.. No stack expansion, at least.
 */
/* 译注：至少 Xtensa 在无 MMU 时仍可能产生保护 fault，但此配置不支持栈扩展。 */
/*
 * 业务背景：NOMMU 保护 fault 仍需在 mmap 读锁下查 VMA，但没有页表或向下扩栈路径。
 * 入参：mm 借用；addr 是 fault 地址；regs 未使用。
 * 出参/返回：命中返回 VMA且保留 mmap 读锁；未命中 NULL并已解锁。
 * 注意事项：调用者成功后必须配对 mmap_read_unlock，不会修改 VMA 边界。
 */
struct vm_area_struct *lock_mm_and_find_vma(struct mm_struct *mm,
			unsigned long addr, struct pt_regs *regs)
{
	/* NOMMU 也可能保护故障，但没有向下扩栈的写锁路径。 */
	struct vm_area_struct *vma;

	mmap_read_lock(mm);
	/* 成功保留读锁，失败则在返回 NULL 前释放。 */
	vma = vma_lookup(mm, addr);
	if (!vma)
		mmap_read_unlock(mm);
	return vma;
}

#endif /* CONFIG_MMU */
