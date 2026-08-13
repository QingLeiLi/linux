/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 * Authors: Waiman Long <longman@redhat.com>
 */
/*
 * qspinlock 统计适配层：在计数启用时把 PV 哈希、等待和唤醒路径映射到 lockevent
 * percpu 槽，并提供带平均值换算的强 lockevent_read()；关闭计数时只保留必要的
 * 零开销 stub。文件依赖包含顺序，用末尾宏包装随后出现的 PV 调用点。
 */

/* 提供事件编号、percpu 数组、更新宏和可被本文件强定义覆盖的读取声明。 */
#include "lock_events.h"

#ifdef CONFIG_LOCK_EVENT_COUNTS
#ifdef CONFIG_PARAVIRT_SPINLOCKS
/*
 * Collect pvqspinlock locking event counts
 */
/* 收集 PV qspinlock 加锁事件计数；只有计数与 PV 两项配置同时启用才编译。 */
/* 调度时钟用于延迟采样，fs 接口用于 debugfs 读取回调和用户缓冲复制。 */
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/fs.h>

/* 在当前 CPU 的 lockevents 数组中取得指定事件槽左值，参数只应是事件名 token。 */
#define EVENT_COUNT(ev)	lockevents[LOCKEVENT_ ## ev]

/*
 * PV specific per-cpu counter
 */
/*
 * PV 专用 percpu 时间戳。kicker 向目标 CPU 槽写 kick 开始时刻，waiter 在自己的
 * 槽上清零并于返回后消费；它只服务近似延迟观测，不参与锁同步或对象生命周期。
 */
static DEFINE_PER_CPU(u64, pv_kick_time);

/*
 * Function to read and return the PV qspinlock counts.
 *
 * The following counters are handled specially:
 * 1. pv_latency_kick
 *    Average kick latency (ns) = pv_latency_kick/pv_kick_unlock
 * 2. pv_latency_wake
 *    Average wake latency (ns) = pv_latency_wake/pv_kick_wake
 * 3. pv_hash_hops
 *    Average hops/hash = pv_hash_hops/pv_kick_unlock
 */
/*
 * 读取并返回 PV qspinlock 计数。
 *
 * 三类计数需特殊处理：pv_latency_kick / pv_kick_unlock 得到平均 kick 延迟(ns)；
 * pv_latency_wake / pv_kick_wake 得到平均 wake 延迟(ns)；pv_hash_hops /
 * pv_kick_unlock 得到每次 hash 的平均探测步数。
 */
/*
 * lockevent_read() - 向用户空间读取一个 lockevent debugfs 计数
 * @file: 已打开的 debugfs 文件借用指针，inode->i_private 保存事件编号。
 * @user_buf: 调用者用户缓冲区，成功时写入 ASCII 数字和换行。
 * @count: 本次最多复制的字节数。
 * @ppos: 文件偏移借用指针，由 simple_read_from_buffer() 更新以支持重复读取。
 *
 * 返回实际复制字节数、EOF 的 0，或 `-EBADF`/用户复制错误。函数在可访问用户
 * 内存的进程上下文运行，不锁住各 CPU 计数，所得值是遍历期间的近似聚合；不
 * 获取 file/inode 所有权，不修改计数，也不分配动态内存。
 */
ssize_t lockevent_read(struct file *file, char __user *user_buf,
		       size_t count, loff_t *ppos)
{
	/* buf 保存最终文本；cpu/id/len 分别为遍历 CPU、事件编号和格式化长度。 */
	char buf[64];
	int cpu, id, len;
	/* sum 为目标事件总和，kicks 为特殊平均值的样本分母。 */
	u64 sum = 0, kicks = 0;

	/*
	 * Get the counter ID stored in file->f_inode->i_private
	 */
	/* 从 file 对应 inode 的 i_private 取 debugfs 创建时保存的事件编号。 */
	id = (long)file_inode(file)->i_private;

	/* reset 伪编号及更大值都不是可读事件文件，拒绝越界索引。 */
	if (id >= lockevent_num)
		return -EBADF;

	for_each_possible_cpu(cpu) {
		/* possible CPU 即使当前 offline 也可能保留历史 percpu 计数，故全部汇总。 */
		sum += per_cpu(lockevents[id], cpu);
		/*
		 * Need to sum additional counters for some of them
		 */
		/* 特殊平均值还需按事件选择同一 CPU 上对应的样本计数。 */
		switch (id) {

		case LOCKEVENT_pv_latency_kick:
		case LOCKEVENT_pv_hash_hops:
			kicks += per_cpu(EVENT_COUNT(pv_kick_unlock), cpu);
			break;

		case LOCKEVENT_pv_latency_wake:
			kicks += per_cpu(EVENT_COUNT(pv_kick_wake), cpu);
			break;
		}
	}

	if (id == LOCKEVENT_pv_hash_hops) {
		/* frac 保存整数商之后余数换算出的百分之一位部分。 */
		u64 frac = 0;

		if (kicks) {
			/* do_div 原地把 sum 变为整数商并返回余数，再四舍五入到百分之一。 */
			frac = 100ULL * do_div(sum, kicks);
			frac = DIV_ROUND_CLOSEST_ULL(frac, kicks);
		}

		/*
		 * Return a X.XX decimal number
		 */
		/* 返回 X.XX 十进制文本；无样本时总和与小数部分均保持零。 */
		len = snprintf(buf, sizeof(buf) - 1, "%llu.%02llu\n",
			       sum, frac);
	} else {
		/*
		 * Round to the nearest ns
		 */
		/* kick/wake 累计纳秒按样本数四舍五入；普通事件不作除法。 */
		if ((id == LOCKEVENT_pv_latency_kick) ||
		    (id == LOCKEVENT_pv_latency_wake)) {
			if (kicks)
				sum = DIV_ROUND_CLOSEST_ULL(sum, kicks);
		}
		len = snprintf(buf, sizeof(buf) - 1, "%llu\n", sum);
	}

	/* 按 @count/@ppos 复制栈上文本，helper 负责用户访问错误与 EOF 语义。 */
	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

/*
 * PV hash hop count
 */
/* PV 哈希探测步数：每次成功插入把本次 hopcnt 累加到当前 CPU 事件槽。 */
/*
 * lockevent_pv_hop() - 记录一次 PV 哈希插入的探测次数
 * @hopcnt: 本次从 1 开始的正探测次数。
 *
 * 无返回；只更新当前 CPU 的近似统计，不影响哈希或锁状态。
 */
static inline void lockevent_pv_hop(int hopcnt)
{
	this_cpu_add(EVENT_COUNT(pv_hash_hops), hopcnt);
}

/*
 * Replacement function for pv_kick()
 */
/* pv_kick() 的统计替代函数。 */
/*
 * __pv_kick() - 记录 kick 调用耗时并调用体系结构底层 pv_kick
 * @cpu: 要唤醒的有效目标 CPU 编号。
 *
 * 先把当前 sched_clock 时间写入目标 CPU 的 pv_kick_time 槽，供目标 waiter 计算
 * 端到端 wake 延迟；再调用底层 hypercall，并在发起 CPU 累加调用耗时。无返回，
 * 不保证目标确实处于挂起状态，统计写不参与唤醒正确性。
 */
static inline void __pv_kick(int cpu)
{
	/* start 同时作为目标槽的握手时间戳和本地 hypercall 耗时起点。 */
	u64 start = sched_clock();

	per_cpu(pv_kick_time, cpu) = start;
	pv_kick(cpu);
	this_cpu_add(EVENT_COUNT(pv_latency_kick), sched_clock() - start);
}

/*
 * Replacement function for pv_wait()
 */
/* pv_wait() 的统计替代函数。 */
/*
 * __pv_wait() - 包装条件挂起并记录匹配 kick 到返回的延迟
 * @ptr: 底层条件等待的有效状态字节借用指针。
 * @val: 只有 *ptr 仍等于该值时底层才挂起 vCPU。
 *
 * 进入前清零当前 CPU 时间戳，调用底层 wait；返回后若 kicker 写入了非零开始
 * 时间，就在当前 CPU 累加 wake 延迟并增加样本数。无返回、不取得 @ptr 所有权；
 * 零时间戳表示本次返回未匹配到统计到的 kick，因而不产生 wake 样本。
 */
static inline void __pv_wait(u8 *ptr, u8 val)
{
	/* pkick_time 指向当前 CPU 的握手槽，CPU 稳定性由 PV 锁等待上下文保证。 */
	u64 *pkick_time = this_cpu_ptr(&pv_kick_time);

	*pkick_time = 0;
	pv_wait(ptr, val);
	if (*pkick_time) {
		this_cpu_add(EVENT_COUNT(pv_latency_wake),
			     sched_clock() - *pkick_time);
		lockevent_inc(pv_kick_wake);
	}
}

/*
 * 这些宏在包装函数定义之后才生效：上方函数体内的 pv_kick/pv_wait 已解析为底层
 * 体系结构调用，后续 qspinlock_paravirt.h 的调用则进入包装器，因而不会递归。
 */
#define pv_kick(c)	__pv_kick(c)
#define pv_wait(p, v)	__pv_wait(p, v)

#endif /* CONFIG_PARAVIRT_SPINLOCKS */

#else /* CONFIG_LOCK_EVENT_COUNTS */

/*
 * 关闭锁事件计数时，PV 哈希调用点仍需要同名 helper 才能共享源码；空函数不保存
 * 状态、不访问 percpu 数据。它是 C 函数而非空宏，调用实参仍按 C 规则求值，
 * 因此调用者不应把有副作用表达式误认为会被配置消除。
 */
/*
 * lockevent_pv_hop() - 关闭计数配置下的空操作实现
 * @hopcnt: 调用点传入的探测次数；进入函数后不读取该值。
 *
 * 无返回和副作用，不取得资源；编译器通常会完全消除普通整数实参的调用开销。
 */
static inline void lockevent_pv_hop(int hopcnt)	{ }

#endif /* CONFIG_LOCK_EVENT_COUNTS */
