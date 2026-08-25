// SPDX-License-Identifier: GPL-2.0-only

#include <linux/sched/debug.h>
#include "sched.h"

/*
 * The implementation of the wait_bit*() and related waiting APIs:
 */
/*
 * 本文件实现按“地址 + bit”或按变量地址等待的共享哈希等待队列。调用者不为每个
 * 对象嵌入 waitqueue，而是把 key 映射到 256 个全局桶；哈希碰撞只增加回调扫描，
 * wake 回调还会比较完整 key，绝不会把别的位/变量当成目标条件。等待者栈上的 entry
 * 由 prepare/finish 或自动移除回调管理，实际条件仍必须由调用者重新检查。
 */

/* 8 位哈希固定提供 256 个桶；这是共享池容量，不限制可等待对象数量。 */
#define WAIT_TABLE_BITS 8
#define WAIT_TABLE_SIZE (1 << WAIT_TABLE_BITS)

/* 所有 bit/var 等待共享此表；cacheline 对齐降低表首部的无关共享。 */
static wait_queue_head_t bit_wait_table[WAIT_TABLE_SIZE] __cacheline_aligned;

/*
 * 根据 @word 地址和 @bit 生成稳定桶地址。两个输入只用于构造哈希 key，返回借用的
 * 全局 waitqueue，调用者不得释放。地址先按 word 位数左移再混入 bit，减少相邻位碰撞；
 * 哈希不保证唯一，精确过滤由 wake 回调完成。函数无锁、无分配且不能失败。
 */
wait_queue_head_t *bit_waitqueue(unsigned long *word, int bit)
{
	/* shift 等于 unsigned long 的位编号宽度对数：32 位为 5，64 位为 6。 */
	const int shift = BITS_PER_LONG == 32 ? 5 : 6;
	/* val 把对象地址与位号组合成 hash_long() 的输入。 */
	unsigned long val = (unsigned long)word << shift | bit;

	return bit_wait_table + hash_long(val, WAIT_TABLE_BITS);
}
EXPORT_SYMBOL(bit_waitqueue);

/*
 * bit waitqueue 的唤醒过滤回调。@wq_entry 是等待者内嵌节点，@arg 是唤醒方栈上的
 * wait_bit_key；mode/sync 原样传给通用回调。地址、bit 不匹配或目标位仍置位时返回 0，
 * 让共享桶继续扫描而不唤醒此任务；精确匹配且位已清除时调用 autoremove，成功唤醒
 * 会把栈节点从队列摘除。回调在 waitqueue 锁内运行，不得睡眠。
 */
int wake_bit_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *arg)
{
	/* key 属于本次 __wake_up 调用；wait_bit 通过 container_of 恢复完整等待描述符。 */
	struct wait_bit_key *key = arg;
	struct wait_bit_queue_entry *wait_bit = container_of(wq_entry, struct wait_bit_queue_entry, wq_entry);

	if (wait_bit->key.flags != key->flags ||
			wait_bit->key.bit_nr != key->bit_nr ||
			test_bit(key->bit_nr, key->flags))
		return 0;

	/* 通用函数设置任务可运行并在成功时安全摘除 entry。 */
	return autoremove_wake_function(wq_entry, mode, sync, key);
}
EXPORT_SYMBOL(wake_bit_function);

/*
 * To allow interruptible waiting and asynchronous (i.e. non-blocking)
 * waiting, the actions of __wait_on_bit() and __wait_on_bit_lock() are
 * permitted return codes. Nonzero return codes halt waiting and return.
 */
/*
 * 为支持可中断等待与异步（不阻塞）动作，__wait_on_bit() 和
 * __wait_on_bit_lock() 把 action 返回值作为协议：非零立即停止并返回。
 */
/*
 * 在指定桶上等待 bit 清零。@wq_head/@wbq_entry 由 wrapper 构造且仅借用；@action
 * 可睡眠并返回 0 继续或非零终止；@mode 是 prepare_to_wait 使用的 task state。
 * 每轮先入队并设置状态，再检查 bit、必要时执行 action，随后用 acquire 语义复查，
 * 避免“检查后、入队前”的丢唤醒。退出总会 finish_wait 恢复 TASK_RUNNING 并摘除节点。
 * 返回 0 表示观察到 bit 清零，其他值原样来自 action；函数允许睡眠。
 */
int __sched
__wait_on_bit(struct wait_queue_head *wq_head, struct wait_bit_queue_entry *wbq_entry,
	      wait_bit_action_f *action, unsigned mode)
{
	/* ret 是 action 的终止状态；初始 0 表示继续等待。 */
	int ret = 0;

	do {
		/* 发布当前任务状态并把栈 entry 加入共享桶，然后才重新检查条件。 */
		prepare_to_wait(wq_head, &wbq_entry->wq_entry, mode);
		if (test_bit(wbq_entry->key.bit_nr, wbq_entry->key.flags))
			ret = (*action)(&wbq_entry->key, mode);
	} while (test_bit_acquire(wbq_entry->key.bit_nr, wbq_entry->key.flags) && !ret);

	/* 无论清位、信号、超时或自定义失败，都恢复 task 状态并清理队列链接。 */
	finish_wait(wq_head, &wbq_entry->wq_entry);

	return ret;
}
EXPORT_SYMBOL(__wait_on_bit);

/*
 * wait_on_bit* 内联快速路径发现 bit 仍置位后进入的普通慢路径。@word/@bit 标识条件，
 * @action/@mode 决定如何睡眠和能否被信号中断。函数在栈上建立精确 key entry，选择
 * 共享桶并委托 __wait_on_bit()；返回其 0 或 action 错误，不保留 entry ownership。
 */
int __sched out_of_line_wait_on_bit(unsigned long *word, int bit,
				    wait_bit_action_f *action, unsigned mode)
{
	/* wq_head 借用全局桶；DEFINE_WAIT_BIT 把 current 和过滤回调嵌入栈对象。 */
	struct wait_queue_head *wq_head = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wq_entry, word, bit);

	return __wait_on_bit(wq_head, &wq_entry, action, mode);
}
EXPORT_SYMBOL(out_of_line_wait_on_bit);

/*
 * 带相对超时（jiffies）的 bit 等待慢路径。参数和 ownership 同普通 wrapper；@timeout
 * 转换为绝对 jiffies deadline 存入栈 key，bit_wait_timeout() 据此返回 -EAGAIN。
 * jiffies 回绕由 time_after_eq 规则处理。返回 0、-EINTR、-EAGAIN 或自定义 action 值。
 */
int __sched out_of_line_wait_on_bit_timeout(
	unsigned long *word, int bit, wait_bit_action_f *action,
	unsigned mode, unsigned long timeout)
{
	struct wait_queue_head *wq_head = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wq_entry, word, bit);

	/* 固定一次绝对截止点，循环被虚假唤醒时不会重新延长相对超时。 */
	wq_entry.key.timeout = jiffies + timeout;

	return __wait_on_bit(wq_head, &wq_entry, action, mode);
}
EXPORT_SYMBOL_GPL(out_of_line_wait_on_bit_timeout);

/*
 * 排他等待 bit 清零并原子置位，相当于获得由该 bit 表示的锁。参数/返回协议同
 * __wait_on_bit()，但 entry 以 exclusive 方式排到队尾，每次被唤醒后用
 * test_and_set_bit() 竞争所有权。成功返回 0 且 bit 已置位；action 非零且仍未取得
 * bit 时返回该值。函数可睡眠，退出前保证栈 entry 不再挂在队列中。
 */
int __sched
__wait_on_bit_lock(struct wait_queue_head *wq_head, struct wait_bit_queue_entry *wbq_entry,
			wait_bit_action_f *action, unsigned mode)
{
	/* ret 记录 action 的信号/自定义终止请求，0 表示仍可继续竞争。 */
	int ret = 0;

	for (;;) {
		/* 排他 waiter 位于队尾，一次 wake 通常只交棒给一个竞争者。 */
		prepare_to_wait_exclusive(wq_head, &wbq_entry->wq_entry, mode);
		if (test_bit(wbq_entry->key.bit_nr, wbq_entry->key.flags)) {
			ret = action(&wbq_entry->key, mode);
			/*
			 * See the comment in prepare_to_wait_event().
			 * finish_wait() does not necessarily takes wwq_head->lock,
			 * but test_and_set_bit() implies mb() which pairs with
			 * smp_mb__after_atomic() before wake_up_page().
			 */
			/*
			 * 参见 prepare_to_wait_event()：finish_wait() 不一定取得 wq_head->lock，
			 * 但 test_and_set_bit() 隐含的全屏障与清位唤醒前的
			 * smp_mb__after_atomic() 配对，关闭条件检查与摘队之间的丢唤醒窗口。
			 */
			if (ret)
				/* action 要求退出时先摘除 entry，再作最后一次原子竞争。 */
				finish_wait(wq_head, &wbq_entry->wq_entry);
		}
		/* 0→1 原子转换是 ownership 提交点；成功后调用者拥有该 bit 锁。 */
		if (!test_and_set_bit(wbq_entry->key.bit_nr, wbq_entry->key.flags)) {
			if (!ret)
				finish_wait(wq_head, &wbq_entry->wq_entry);
			return 0;
		} else if (ret) {
			/* 未取得 bit 且 action 已终止，entry 已由上面清理。 */
			return ret;
		}
	}
}
EXPORT_SYMBOL(__wait_on_bit_lock);

/*
 * wait_on_bit_lock* 快速路径竞争失败后的 out-of-line wrapper。@word/@bit 是锁位，
 * @action/@mode 控制阻塞方式；栈 entry 和全局桶均只在调用期间借用。返回 0 表示已
 * 原子置位取得所有权，非零来自 action；wrapper 本身不保留资源。
 */
int __sched out_of_line_wait_on_bit_lock(unsigned long *word, int bit,
					 wait_bit_action_f *action, unsigned mode)
{
	struct wait_queue_head *wq_head = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wq_entry, word, bit);

	return __wait_on_bit_lock(wq_head, &wq_entry, action, mode);
}
EXPORT_SYMBOL(out_of_line_wait_on_bit_lock);

/*
 * 在已知 @wq_head 上发送一次精确 bit 唤醒。@word/@bit 构造栈 key，函数不拥有
 * 任何对象；waitqueue_active() 是避免加锁扫描的提示性快路径，正确性依赖清位方
 * 在调用前按 wake_up_bit() 契约执行全屏障。__wake_up 每次唤醒一个匹配的 normal
 * waiter，过滤回调会跳过哈希碰撞和仍置位的 key。函数原子上下文可用且不睡眠。
 */
void __wake_up_bit(struct wait_queue_head *wq_head, unsigned long *word, int bit)
{
	/* key 只在同步 __wake_up 回调扫描期间存活。 */
	struct wait_bit_key key = __WAIT_BIT_KEY_INITIALIZER(word, bit);

	/* 空桶直接返回；非空时 mode=TASK_NORMAL、nr_exclusive=1。 */
	if (waitqueue_active(wq_head))
		__wake_up(wq_head, TASK_NORMAL, 1, &key);
}
EXPORT_SYMBOL(__wake_up_bit);

/**
 * wake_up_bit - wake up waiters on a bit
 * @word: the address containing the bit being waited on
 * @bit: the bit at that address being waited on
 *
 * Wake up any process waiting in wait_on_bit() or similar for the
 * given bit to be cleared.
 *
 * The wake-up is sent to tasks in a waitqueue selected by hash from a
 * shared pool.  Only those tasks on that queue which have requested
 * wake_up on this specific address and bit will be woken, and only if the
 * bit is clear.
 *
 * In order for this to function properly there must be a full memory
 * barrier after the bit is cleared and before this function is called.
 * If the bit was cleared atomically, such as a by clear_bit() then
 * smb_mb__after_atomic() can be used, othwewise smb_mb() is needed.
 * If the bit was cleared with a fully-ordered operation, no further
 * barrier is required.
 *
 * Normally the bit should be cleared by an operation with RELEASE
 * semantics so that any changes to memory made before the bit is
 * cleared are guaranteed to be visible after the matching wait_on_bit()
 * completes.
 */
/*
 * wake_up_bit() - 唤醒等待指定 bit 清零的任务
 *
 * @word 是包含目标位的地址，@bit 是位号；两者只用于选择桶和精确 key，不转移
 * ownership。唤醒从共享哈希池选择队列，只有地址/位匹配且该位已经清零的 waiter
 * 会被唤醒。清位后、调用前必须有全内存屏障：原子 clear 可用
 * smp_mb__after_atomic()，普通写需 smp_mb()，全序操作无需额外屏障。通常清位还应有
 * release 语义，与等待侧 test_bit_acquire() 配对，使临界区写入在返回后可见。
 * 函数无返回值、不保证存在 waiter，也不会睡眠。
 */
void wake_up_bit(unsigned long *word, int bit)
{
	__wake_up_bit(bit_waitqueue(word, bit), word, bit);
}
EXPORT_SYMBOL(wake_up_bit);

/*
 * 根据任意内核地址 @p 选择变量等待桶。返回借用的全局 waitqueue，哈希碰撞由
 * var_wake_function() 比较完整地址过滤；无分配、无锁且不会失败。
 */
wait_queue_head_t *__var_waitqueue(void *p)
{
	return bit_wait_table + hash_ptr(p, WAIT_TABLE_BITS);
}
EXPORT_SYMBOL(__var_waitqueue);

/*
 * 变量等待的精确唤醒回调。@arg 是 wake_up_var() 构造的 key；仅当等待 entry 的
 * flags 地址和 bit_nr=-1 都匹配时，才委托 autoremove 唤醒并摘队。mode/sync 原样
 * 透传；不匹配返回 0 继续扫描共享桶。回调在 waitqueue 锁内运行，不能睡眠。
 */
static int
var_wake_function(struct wait_queue_entry *wq_entry, unsigned int mode,
		  int sync, void *arg)
{
	/* 两个指针都只在当前同步回调期间借用，不取得 task 或 key 引用。 */
	struct wait_bit_key *key = arg;
	struct wait_bit_queue_entry *wbq_entry =
		container_of(wq_entry, struct wait_bit_queue_entry, wq_entry);

	if (wbq_entry->key.flags != key->flags ||
	    wbq_entry->key.bit_nr != key->bit_nr)
		return 0;

	return autoremove_wake_function(wq_entry, mode, sync, key);
}

/*
 * 初始化 wait_var_event 宏使用的栈 entry。@wbq_entry 是纯输出存储，@var 是作为
 * 身份 key 的借用地址，@flags 通常决定是否 WQ_FLAG_EXCLUSIVE。复合字面量一次性
 * 设置 bit_nr=-1 哨兵、private=current、变量过滤回调和自指空链表；函数不入队、
 * 不改变 task state、无返回值且不睡眠。entry 必须活到 finish_wait/自动摘除完成。
 */
void init_wait_var_entry(struct wait_bit_queue_entry *wbq_entry, void *var, int flags)
{
	*wbq_entry = (struct wait_bit_queue_entry){
		.key = {
			.flags	= (var),
			.bit_nr = -1,
		},
		/*
		 * key 先固定完整变量身份；内嵌 wq_entry 再绑定 current、过滤回调和
		 * 自指链表头。初始化结束后仍未入队，后续 prepare_to_wait 才转移队列成员状态。
		 */
		.wq_entry = {
			.flags	 = flags,
			.private = current,
			.func	 = var_wake_function,
			.entry	 = LIST_HEAD_INIT(wbq_entry->wq_entry.entry),
		},
	};
}
EXPORT_SYMBOL(init_wait_var_entry);

/**
 * wake_up_var - wake up waiters on a variable (kernel address)
 * @var: the address of the variable being waited on
 *
 * Wake up any process waiting in wait_var_event() or similar for the
 * given variable to change.  wait_var_event() can be waiting for an
 * arbitrary condition to be true and associates that condition with an
 * address.  Calling wake_up_var() suggests that the condition has been
 * made true, but does not strictly require the condtion to use the
 * address given.
 *
 * The wake-up is sent to tasks in a waitqueue selected by hash from a
 * shared pool.  Only those tasks on that queue which have requested
 * wake_up on this specific address will be woken.
 *
 * In order for this to function properly there must be a full memory
 * barrier after the variable is updated (or more accurately, after the
 * condition waited on has been made to be true) and before this function
 * is called.  If the variable was updated atomically, such as a by
 * atomic_dec() then smb_mb__after_atomic() can be used.  If the
 * variable was updated by a fully ordered operation such as
 * atomic_dec_and_test() then no extra barrier is required.  Otherwise
 * smb_mb() is needed.
 *
 * Normally the variable should be updated (the condition should be made
 * to be true) by an operation with RELEASE semantics such as
 * smp_store_release() so that any changes to memory made before the
 * variable was updated are guaranteed to be visible after the matching
 * wait_var_event() completes.
 */
/*
 * wake_up_var() - 通知等待某变量地址的任务重新检查任意条件
 *
 * @var 只充当哈希与精确匹配身份，条件可以引用别的状态；通知仅表示“条件可能成立”，
 * waiter 必须循环复查。更新条件后、调用前需要全屏障；原子更新可在适用时使用
 * smp_mb__after_atomic()，全序 RMW 无需额外屏障，普通更新需要 smp_mb()。条件发布
 * 通常使用 release，等待条件读取使用 acquire，使此前写入可见。函数唤醒匹配地址
 * 的 waiter，无返回值、不拥有 @var，也不会睡眠。
 */
void wake_up_var(void *var)
{
	__wake_up_bit(__var_waitqueue(var), var, -1);
}
EXPORT_SYMBOL(wake_up_var);

/*
 * 普通 bit wait 的默认 action。@word 只携带 key/timeout，本实现不读取；@mode 决定
 * 哪类 pending signal 可终止等待。schedule() 让出 CPU，醒来后返回 -EINTR 或 0 让
 * 外层复查 bit。函数必须在可睡眠进程上下文调用，不改变 entry ownership。
 */
__sched int bit_wait(struct wait_bit_key *word, int mode)
{
	schedule();
	if (signal_pending_state(mode, current))
		return -EINTR;

	return 0;
}
EXPORT_SYMBOL(bit_wait);

/*
 * I/O 等待版本的默认 action。参数、返回和 signal 语义同 bit_wait()，区别是
 * io_schedule() 将阻塞计入 I/O wait 并向调度/块层提供相应提示；函数可睡眠。
 */
__sched int bit_wait_io(struct wait_bit_key *word, int mode)
{
	io_schedule();
	if (signal_pending_state(mode, current))
		return -EINTR;

	return 0;
}
EXPORT_SYMBOL(bit_wait_io);

/*
 * 带绝对 jiffies deadline 的 action。@word->timeout 由 wrapper 固定，@mode 控制信号
 * 中断；到期返回 -EAGAIN，睡眠后有允许信号返回 -EINTR，否则返回 0 让外层再次检查
 * bit。READ_ONCE 取得一致的当前 jiffies，time_after_eq 处理回绕；函数允许睡眠。
 */
__sched int bit_wait_timeout(struct wait_bit_key *word, int mode)
{
	/* now 是本轮判定基准，避免同一表达式多次读取滚动的 jiffies。 */
	unsigned long now = READ_ONCE(jiffies);

	/* 先检查到期，避免无符号相减在 deadline 已过时形成长睡眠。 */
	if (time_after_eq(now, word->timeout))
		return -EAGAIN;
	schedule_timeout(word->timeout - now);
	if (signal_pending_state(mode, current))
		return -EINTR;

	return 0;
}
EXPORT_SYMBOL_GPL(bit_wait_timeout);

/*
 * 启动阶段初始化全部共享哈希桶。无参数、无返回值，i 遍历固定 256 个槽并建立各自
 * 自旋锁和空链表；只能执行一次，必须早于任何 bit/var waiter 使用表。__init 表示
 * 启动完成后函数代码可回收，初始化过程不分配动态内存。
 */
void __init wait_bit_init(void)
{
	/* i 是 [0, WAIT_TABLE_SIZE) 的桶下标。 */
	int i;

	for (i = 0; i < WAIT_TABLE_SIZE; i++)
		init_waitqueue_head(bit_wait_table + i);
}
