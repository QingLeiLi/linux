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
 * 本内部头文件把 lock_events_list.h 的 X-macro 目录展开成稳定的事件编号，
 * 并为锁热路径提供启用/禁用统计时一致的调用接口。计数只用于 debugfs
 * 观测，不参与加锁正确性；为降低热路径成本，它明确允许偶发丢失更新。
 */

#ifndef __LOCKING_LOCK_EVENTS_H
#define __LOCKING_LOCK_EVENTS_H

enum lock_events {

#include "lock_events_list.h"

	lockevent_num,	/* Total number of lock event counts */
	/* 锁事件计数槽总数；它是数组长度和合法可读事件编号的上界。 */
	LOCKEVENT_reset_cnts = lockevent_num,
	/*
	 * 复用“首个越界编号”作为只写的 reset 伪事件。它不占 lockevents[]
	 * 计数槽；lock_events.c 只把它绑定到 .reset_counts debugfs 文件。
	 */
};

/*
 * lock_events_list.h 未设置 include guard，默认的 LOCK_EVENT(name) 在这里
 * 展开为 LOCKEVENT_name 枚举项。条件编译会同步裁剪编号空间；任何使用者
 * 都必须通过同一目录取得编号，不能另行复制一份可能错位的顺序。
 */

#ifdef CONFIG_LOCK_EVENT_COUNTS
/*
 * Per-cpu counters
 */
/*
 * 每个 CPU 持有一组 unsigned long 事件槽，索引范围为
 * [0, lockevent_num)。定义位于 lock_events.c，读取时跨 possible CPU 求和；
 * 计数可能自然回绕，也不提供与被观测锁状态一致的原子快照。
 */
DECLARE_PER_CPU(unsigned long, lockevents[lockevent_num]);

/*
 * Increment the statistical counters. use raw_cpu_inc() because of lower
 * overhead and we don't care if we loose the occasional update.
 */
/*
 * __lockevent_inc() - 按条件递增当前 CPU 的一个锁事件统计槽
 *
 * 【宏观位置】锁实现中的 lockevent_inc()/lockevent_cond_inc() → 本函数
 * → raw_cpu_inc()；之后由 lockevent_read() 跨 CPU 汇总。
 * 【入口条件】@event 必须是 [0, lockevent_num) 内的真实事件编号；@cond
 * 决定本次样本是否计入。调用者不转移任何对象或引用，也不要求持有统计锁。
 * 本函数不会睡眠，适合锁热路径；raw percpu 操作不为统计建立同步快照。
 * 【参数】@event 是输入编号；@cond 是输入布尔条件，false 时不访问计数槽。
 * 【返回与副作用】无直接返回值。条件成立时递增当前 CPU 槽；低开销实现允许
 * 与中断等并发更新偶发互相覆盖，因此计数是诊断近似值，不能作为正确性依据。
 * 原文的 “loose” 应按语义理解为 “lose”，即允许偶发丢失更新。
 */
static inline void __lockevent_inc(enum lock_events event, bool cond)
{
	/* 条件型接口在这里汇合；false 路径完全不触碰 percpu 存储。 */
	if (cond)
		/* 只更新本 CPU 的 event 槽，不加全局锁，也不转移任何所有权。 */
		raw_cpu_inc(lockevents[event]);
}

#define lockevent_inc(ev)	  __lockevent_inc(LOCKEVENT_ ##ev, true)
#define lockevent_cond_inc(ev, c) __lockevent_inc(LOCKEVENT_ ##ev, c)

/*
 * 两个宏用 token pasting 把无前缀事件名转换成枚举常量：普通形式无条件加一，
 * cond 形式把 c 恰好求值一次后再决定是否加一。ev 必须是目录中的标识符，
 * 不是运行时表达式；宏本身不取得锁，也不会改变被观测操作的成功与否。
 */

/*
 * __lockevent_add() - 给当前 CPU 的一个锁事件槽累加任意增量
 *
 * 【宏观位置】lockevent_add() → 本函数 → raw_cpu_add()。它用于一次事件
 * 贡献不止 1 个单位的累计量，例如跳数或时间；当前版本没有目录内直接调用点。
 * 【入口条件】@event 必须是合法真实事件编号；@inc 是本次输入增量，类型为
 * int，函数不检查正负或溢出。无统计锁、引用或 ownership 要求，且不会睡眠。
 * 【返回与副作用】无直接返回值；把 @inc 加到当前 CPU 的 unsigned long 槽。
 * 与递增接口相同，结果允许偶发丢样本和自然回绕，只能用于诊断统计。
 */
static inline void __lockevent_add(enum lock_events event, int inc)
{
	/* 单次 raw percpu 读改写避免全局争用，但不提供跨 CPU 一致性。 */
	raw_cpu_add(lockevents[event], inc);
}

#define lockevent_add(ev, c)	__lockevent_add(LOCKEVENT_ ##ev, c)

/* ev 经 token pasting 选择槽，c 作为 int 增量恰好求值一次并传给内联函数。 */

#else  /* CONFIG_LOCK_EVENT_COUNTS */

#define lockevent_inc(ev)
#define lockevent_add(ev, c)		do { (void)(c); } while (0)
#define lockevent_cond_inc(ev, c)	do { (void)(c); } while (0)

/*
 * 关闭统计后不生成任何计数访问。lockevent_inc() 连 ev 都不求值；add 和
 * cond_inc 仍以 (void)(c) 把第二实参求值一次，保留调用点原有副作用并避免
 * “未使用”问题。do/while(0) 让宏在 if/else 中表现为单条语句。
 */

#endif /* CONFIG_LOCK_EVENT_COUNTS */

/*
 * lockevent_read() - 把一个锁事件的跨 CPU 汇总值读给 debugfs 用户
 *
 * 【宏观位置】debugfs fops_lockevent.read → 本函数；lock_events.c 提供弱
 * 默认实现，启用 PV qspinlock 统计时 qspinlock_stat.h 的强实现额外换算平均值。
 * 【入口条件】由 debugfs 读回调在进程上下文调用，允许用户页故障并可能睡眠；
 * 不要求调用者持锁。file、user_buf、ppos 均为借用指针，调用前后 ownership
 * 不变；各 CPU 计数可并发变化，所以结果不是同一时刻的原子快照。
 * 【参数】@file 是输入文件，其 inode private 保存事件编号；@user_buf 是长度
 * 至少由 @count 限定的用户态输出缓冲区；@count 是最多复制的字节数；@ppos
 * 是输入输出文件偏移，成功复制后由 helper 推进，均不接受由本层主动传入 NULL。
 * 【返回】成功返回复制字节数（文件末尾可为 0）；非法/重置伪编号返回 -EBADF，
 * 用户缓冲区复制失败可返回 -EFAULT。无引用转移；唯一输出是用户缓冲区和偏移。
 */
ssize_t lockevent_read(struct file *file, char __user *user_buf,
		       size_t count, loff_t *ppos);

#endif /* __LOCKING_LOCK_EVENTS_H */
