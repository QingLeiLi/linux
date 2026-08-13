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
 * Authors: Waiman Long <waiman.long@hpe.com>
 */

/*
 * Collect locking event counts
 */
/*
 * 收集并向 debugfs 导出锁事件计数。热路径只更新每 CPU 槽；本文件在用户
 * 读取时才跨 CPU 汇总，从而避免为统计引入全局锁。代价是读取与重置都不是
 * 原子快照，结果只适合性能诊断，不能参与锁协议的正确性判断。
 */
#include <linux/debugfs.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/fs.h>

#include "lock_events.h"

#undef  LOCK_EVENT
#define LOCK_EVENT(name)	[LOCKEVENT_ ## name] = #name,

/*
 * lock_events.h 已用默认展开生成枚举；这里撤销旧定义，再把同一 X-macro
 * 条目展开成“枚举编号 → 字符串名”的指定初始化器。#name 把标识符字符串化，
 * 因而 debugfs 名称与事件编号来自唯一目录，不会靠两份手写表维持同步。
 */

#define LOCK_EVENTS_DIR		"lock_event_counts"

/* debugfs 顶层目录名；初始化成功后所有事件文件和重置入口都位于其下。 */

/*
 * When CONFIG_LOCK_EVENT_COUNTS is enabled, event counts of different
 * types of locks will be reported under the <debugfs>/lock_event_counts/
 * directory. See lock_events_list.h for the list of available locking
 * events.
 *
 * Writing to the special ".reset_counts" file will reset all the above
 * locking event counts. This is a very slow operation and so should not
 * be done frequently.
 *
 * These event counts are implemented as per-cpu variables which are
 * summed and computed whenever the corresponding debugfs files are read. This
 * minimizes added overhead making the counts usable even in a production
 * environment.
 */
/*
 * 启用 CONFIG_LOCK_EVENT_COUNTS 后，不同锁类型的事件计数会显示在
 * <debugfs>/lock_event_counts/ 目录；可用事件以 lock_events_list.h 为准。
 * 向特殊文件 .reset_counts 写入会重置上述全部计数。该操作要遍历所有
 * possible CPU 和全部事件，代价很高，不应频繁执行。
 *
 * 计数采用每 CPU 变量，读取对应 debugfs 文件时才求和和换算。这把更新
 * 开销压到足够低，使统计可用于生产环境，但并发读取不会得到原子快照。
 *
 * 数组比真实事件数多一个元素：前 lockevent_num 项由 X-macro 指定初始化，
 * 末项只保存 .reset_counts 名称。字符串为静态只读存储，debugfs 借用指针，
 * 不发生引用或 ownership 转移。
 */
static const char * const lockevent_names[lockevent_num + 1] = {

#include "lock_events_list.h"

	[LOCKEVENT_reset_cnts] = ".reset_counts",
};

/*
 * Per-cpu counts
 */
/*
 * 为每个 possible CPU 定义 lockevent_num 个 unsigned long 槽。锁热路径写
 * 本 CPU 槽，读取/重置路径跨 CPU 访问；该存储与内核同生命周期，不需要
 * 单独释放。percpu 降低争用，但不保证与并发更新一致的跨 CPU 快照。
 */
DEFINE_PER_CPU(unsigned long, lockevents[lockevent_num]);

/*
 * The lockevent_read() function can be overridden.
 */
/*
 * lockevent_read() - 汇总一个普通锁事件并复制给 debugfs 读者
 *
 * 本函数可以被覆盖：这是弱默认实现；qspinlock_stat.h 在 PV qspinlock
 * 统计构建中提供强定义，对延迟和哈希跳数进一步计算平均值。
 *
 * 【宏观位置】debugfs .read → 本函数 → per_cpu() 汇总 →
 * simple_read_from_buffer()。调用者是读取事件文件的用户进程。
 * 【入口条件】进程上下文，可因用户页故障睡眠；无需持有统计锁。@file、
 * @user_buf、@ppos 都是 VFS 借用指针，调用前后 ownership 不变。
 * 【参数】@file 的 inode private 保存事件编号；@user_buf 是用户态输出区；
 * @count 是最多复制字节数；@ppos 是输入输出文件偏移，成功后由 helper 推进。
 * 这些指针由 VFS 提供，本层不接受主动传入 NULL。
 * 【返回】合法编号返回本次复制字节数，文件末尾可为 0；编号达到
 * lockevent_num（包括 reset 伪编号）返回 -EBADF；用户复制失败可返回
 * -EFAULT。无引用转移；副作用仅为写用户缓冲区和推进文件偏移。
 * 【并发边界】逐个读取 possible CPU 槽期间更新可继续发生，sum 不是同一
 * 时刻的快照；允许的偶发丢样本也不会在这里补偿。
 */
ssize_t __weak lockevent_read(struct file *file, char __user *user_buf,
			      size_t count, loff_t *ppos)
{
	char buf[64];
	int cpu, id, len;
	u64 sum = 0;

	/*
	 * 变量地图：buf 保存带换行的十进制文本；cpu 是 possible CPU 游标；
	 * id 是文件绑定的事件编号；len 是格式化长度；sum 用 u64 承接各 CPU
	 * unsigned long 槽之和，减少 32 位构建上的汇总溢出风险。
	 */

	/*
	 * Get the counter ID stored in file->f_inode->i_private
	 */
	/* debugfs 创建文件时把枚举值编码进 inode private；这里只借用并解码。 */
	id = (long)file_inode(file)->i_private;

	/* reset 伪编号没有计数槽，任何更大编号同样不能用于数组索引。 */
	if (id >= lockevent_num)
		return -EBADF;

	/* 阶段 1：无锁汇总所有 possible CPU，接受并发更新造成的时间切片差异。 */
	for_each_possible_cpu(cpu)
		sum += per_cpu(lockevents[id], cpu);
	/* 阶段 2：转换成一行文本；64 字节足以容纳 u64 十进制值和换行。 */
	len = snprintf(buf, sizeof(buf) - 1, "%llu\n", sum);

	/* 阶段 3：按 @count/@ppos 实现可分段读取，并把复制结果原样返回 VFS。 */
	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

/*
 * Function to handle write request
 *
 * When idx = reset_cnts, reset all the counts.
 */
/*
 * lockevent_write() - 处理 debugfs 写请求并重置全部锁事件槽
 *
 * 本函数处理写请求；当文件绑定的编号是 reset_cnts 时清零所有计数。
 * 【宏观位置】写 .reset_counts → 本函数 → 遍历 possible CPU/事件并
 * WRITE_ONCE(0)。普通事件文件权限是 0400，重置文件权限是 0200；编号检查
 * 仍作为 fops 复用时的防御边界。
 * 【入口条件】由 debugfs 在进程上下文调用，无需持有统计锁。函数不主动
 * 睡眠，但双重循环可能很慢。所有指针均由 VFS 借用，ownership 不变。
 * 【参数】@file 是输入文件，inode private 决定是否为 reset；@user_buf 是
 * 未解析的用户输入缓冲区，内容和可空性不影响重置；@count 是请求字节数并
 * 被当作已消费长度返回；@ppos 是借用偏移指针，本函数不读取也不推进它。
 * 【返回】所有路径都返回 @count，不报告逐槽结果。reset 编号会清零每个槽；
 * 其他编号不修改状态。没有引用转移或资源分配。
 * 【并发边界】WRITE_ONCE 防止单个清零存储被编译器拆分/合并，但不与
 * raw_cpu_inc() 原子配对；竞态样本可能留在重置前值之后，也可能被清零覆盖。
 * 因而“重置完成”只表示遍历结束，不是全局统计纪元的事务边界。
 */
static ssize_t lockevent_write(struct file *file, const char __user *user_buf,
			   size_t count, loff_t *ppos)
{
	int cpu;

	/* cpu 遍历每个 possible CPU；内层 i/ptr 分别是事件游标和借用的 CPU 槽首址。 */

	/*
	 * Get the counter ID stored in file->f_inode->i_private
	 */
	/* 只接受初始化阶段专门绑定给 .reset_counts 的伪编号。 */
	if ((long)file_inode(file)->i_private != LOCKEVENT_reset_cnts)
		return count;

	/*
	 * 按 CPU 再按事件清零。ptr 只在当前 cpu 迭代中有效，不持有引用；远端
	 * CPU 仍可同时更新其槽，所以这里不能承诺清零瞬间的一致快照。
	 */
	for_each_possible_cpu(cpu) {
		int i;
		unsigned long *ptr = per_cpu_ptr(lockevents, cpu);

		for (i = 0 ; i < lockevent_num; i++)
			WRITE_ONCE(ptr[i], 0);
	}
	/* 用户提供的数据无需解析；遍历完成即把全部输入字节报告为已消费。 */
	return count;
}

/*
 * Debugfs data structures
 */
/*
 * debugfs 文件操作表在内核整个运行期保持只读：事件文件借用 .read，reset
 * 文件借用 .write，权限位阻止不适用方向；default_llseek 管理普通文件偏移。
 * debugfs 只保存此静态表的指针，不取得需要本文件释放的引用。
 */
static const struct file_operations fops_lockevent = {
	.read = lockevent_read,
	.write = lockevent_write,
	.llseek = default_llseek,
};

#ifdef CONFIG_PARAVIRT_SPINLOCKS
#include <asm/paravirt.h>

/*
 * skip_lockevent() - 判断初始化时是否应隐藏一个 PV qspinlock 事件
 *
 * 【宏观位置】init_lockevent_counts() 枚举名称 → 本函数 → 决定是否创建
 * 对应 debugfs 文件。只过滤展示项，不改变事件编号或热路径计数代码。
 * 【入口条件】仅在 initcall 的单线程初始化路径调用，函数和缓存都标记
 * __init/__initdata，初始化后可回收；不持锁、不睡眠。
 * 【参数】@name 是输入的 NUL 结尾静态事件名，借用且不可为 NULL；不转移
 * ownership。返回 true 表示当前运行环境不用 PV 解锁，应跳过 pv_ 前缀项；
 * 返回 false 表示保留。无初始化期之外的副作用。
 */
static bool __init skip_lockevent(const char *name)
{
	static int pv_on __initdata = -1;

	/* pv_on 以 -1 表示尚未探测，随后缓存 0/1；initcall 串行调用，无需加锁。 */

	/* 非 native 的 paravirt unlock 表示本机实际启用了 PV qspinlock 操作。 */
	if (pv_on < 0)
		pv_on = !pv_is_native_spin_unlock();
	/*
	 * Skip PV qspinlock events on bare metal.
	 */
	/*
	 * 在裸机原生解锁路径上跳过 PV qspinlock 事件。只比较固定的前三字节
	 * "pv_"；名称来自静态表，长度和生命周期均满足 memcmp() 前置条件。
	 */
	if (!pv_on && !memcmp(name, "pv_", 3))
		return true;
	return false;
}
#else
/*
 * skip_lockevent() - 无 PV 自旋锁构建下的不过滤占位实现
 *
 * 【宏观位置】供 init_lockevent_counts() 使用，与上面的配置实现保持接口一致。
 * 【入口条件】初始化上下文，不持锁、不睡眠。@name 是借用的输入字符串，
 * 本实现不读取内容、不可保存，也不转移 ownership。
 * 【返回】恒为 false，因此为目录中的每个事件创建 debugfs 文件；无副作用。
 */
static inline bool skip_lockevent(const char *name)
{
	return false;
}
#endif

/*
 * Initialize debugfs for the locking event counts.
 */
/*
 * init_lockevent_counts() - 在 fs_initcall 阶段创建锁事件 debugfs 接口
 *
 * 初始化锁事件计数的 debugfs：先创建顶层目录，再为每个未过滤事件创建
 * 只读文件，最后创建只写 reset 文件。它是计数存储对用户可见的发布边界。
 *
 * 【宏观位置】fs_initcall → 本函数 → debugfs_create_dir()/create_file()；
 * 成功后用户通过 fops_lockevent 读汇总值或触发重置。
 * 【入口条件】内核初始化进程上下文，允许睡眠；入参：无。无需预持锁，
 * lockevent_names、fops 和 percpu 存储都已静态初始化。
 * 【主要阶段】1. 创建目录；2. 过滤并创建真实事件文件；3. 创建 reset 文件；
 * 4. 任一文件失败时递归撤销已经发布的整棵子树。
 * 【返回】全部创建成功返回 0；目录或任一文件创建失败统一告警并返回
 * -ENOMEM，原始 debugfs 错误不会透传。无输出参数。
 * 【ownership】d_counts 是 debugfs 返回的创建句柄，本函数负责在后续创建
 * 失败时用 debugfs_remove_recursive() 摘除整棵子树；成功后节点由 debugfs
 * 持续管理，本 built-in 组件没有退出清理。函数代码及 __initdata 缓存在
 * 初始化完成后可回收。
 */
static int __init init_lockevent_counts(void)
{
	struct dentry *d_counts = debugfs_create_dir(LOCK_EVENTS_DIR, NULL);
	int i;

	/* d_counts 标识已发布的目录根；i 是 [0, lockevent_num) 的事件编号游标。 */

	/* 目录尚未建立，没有可递归删除的子树，直接进入统一告警出口。 */
	if (IS_ERR(d_counts))
		goto out;

	/*
	 * Create the debugfs files
	 *
	 * As reading from and writing to the stat files can be slow, only
	 * root is allowed to do the read/write to limit impact to system
	 * performance.
	 */
	/*
	 * 创建各个 debugfs 文件。读写统计文件可能较慢，因此只允许 root 读写，
	 * 以限制对系统性能的影响。真实事件使用 0400；每个 inode private 保存
	 * 转成 void * 的事件编号，read 回调再按同一契约还原。
	 */
	for (i = 0; i < lockevent_num; i++) {
		/* 裸机可跳过不会产生样本的 pv_ 文件，但编号表本身保持完整。 */
		if (skip_lockevent(lockevent_names[i]))
			continue;
		/* 任一部分创建失败都不能留下残缺目录，转入整树回滚。 */
		if (IS_ERR(debugfs_create_file(lockevent_names[i], 0400, d_counts,
					 (void *)(long)i, &fops_lockevent)))
			goto fail_undo;
	}

	/* 最后发布只写重置入口；其伪编号不在 lockevents[] 的合法索引范围内。 */
	if (IS_ERR(debugfs_create_file(lockevent_names[LOCKEVENT_reset_cnts], 0200,
				 d_counts, (void *)(long)LOCKEVENT_reset_cnts,
				 &fops_lockevent)))
		goto fail_undo;

	/* 目录和全部所需文件已经对 debugfs 用户可见，后续不再回滚。 */
	return 0;
fail_undo:
	/* 已拥有一个可能只创建了一部分的目录树；递归摘除，避免暴露残缺接口。 */
	debugfs_remove_recursive(d_counts);
out:
	/* 目录创建失败和子项失败在外部统一表现为告警与 -ENOMEM。 */
	pr_warn("Could not create '%s' debugfs entries\n", LOCK_EVENTS_DIR);
	return -ENOMEM;
}

/* 在文件系统初始化阶段调用一次；本文件为 built-in 组件，不提供模块退出路径。 */
fs_initcall(init_lockevent_counts);
