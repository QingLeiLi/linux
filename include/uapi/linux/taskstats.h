/* SPDX-License-Identifier: LGPL-2.1 WITH Linux-syscall-note */
/*
 * taskstats 用户 ABI 学习导读
 *
 * 本头文件同时被内核和用户程序包含，定义 Generic Netlink taskstats
 * family 的稳定二进制布局。kernel/taskstats.c 负责填充与发送，getdelays
 * 等工具按这里的版本号、属性类型和单位解析；
 * 任何字段换序、缩小或改变
 * 含义都会破坏已编译用户程序，因此扩展只能追加到 struct taskstats 尾部。
 *
 * 一个载荷用 AGGR_PID/AGGR_TGID 嵌套属性把标识与同一 struct taskstats
 * 绑定。结构既可表示单 task 快照，也可表示线程组中可累加字段的总和；
 * 用户必须结合外层属性和 AGROUP 标志判断记录语义，不能只看结构本身。
 */
/* taskstats.h - exporting per-task statistics
 *
 * Copyright (C) Shailabh Nagar, IBM Corp. 2006
 *           (C) Balbir Singh,   IBM Corp. 2006
 *           (C) Jay Lan,        SGI, 2006
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef _LINUX_TASKSTATS_H
#define _LINUX_TASKSTATS_H

#include <linux/types.h>
#include <linux/time_types.h>

/* Format for per-task data returned to userland when
 *	- a task exits
 *	- listener requests stats for a task
 *
 * The struct is versioned. Newer versions should only add fields to
 * the bottom of the struct to maintain backward compatibility.
 *
 *
 * To add new fields
 *	a) bump up TASKSTATS_VERSION
 *	b) add comment indicating new version number at end of struct
 *	c) add new fields after version comment; maintain 64-bit alignment
 */
/*
 * 该结构用于任务退出通知和用户主动查询。版本只允许向尾部增长，使旧
 * 程序仍能读取自己认识的前缀。新增字段必须同时提升 TASKSTATS_VERSION、
 * 标出新版本边界并保持 64 位成员对齐；不能把插入中部当作普通 C 结构
 * 重构，否则 Netlink 中的既有字段偏移会改变。
 */


#define TASKSTATS_VERSION	17
#define TS_COMM_LEN		32	/* should be >= TASK_COMM_LEN
					 * in linux/sched.h */
/*
 * 当前 ABI 版本为 17。TS_COMM_LEN 至少覆盖内核 task comm，额外空间保证
 * 导出命令名不会因 ABI 缓冲区比内核源字段短而产生不可预期截断。
 */

/*
 * struct taskstats 是一次记账快照，不拥有任何内核对象或用户指针；所有
 * 成员均是按 UAPI 固定宽度编码的值。每 PID 记录由一个 task 填充；每
 * TGID 记录只对定义为可累加的字段求和，
 * 身份类字段不应被用户假定有效。
 * 计数器通常允许自然溢出，跨版本读取前必须先检查 @version。
 */
struct taskstats {

	/* The version number of this struct. This field is always set to
	 * TAKSTATS_VERSION, which is defined in <linux/taskstats.h>.
	 * Each time the struct is changed, the value should be incremented.
	 */
	/*
	 * version 总是由内核写为 TASKSTATS_VERSION，结构变化时必须递增；
	 * ac_exitcode 是 wait/acct 语义的任务退出状态。查询仍存活任务时，
	 * 退出码尚未形成，用户不能把零机械理解为“正常退出”。
	 */
	__u16	version;
	__u32	ac_exitcode;		/* Exit status */

	/* The accounting flags of a task as defined in <linux/acct.h>
	 * Defined values are AFORK, ASU, ACOMPAT, ACORE, AXSIG, and AGROUP.
	 * (AGROUP since version 12).
	 */
	/*
	 * ac_flag 是 <linux/acct.h> 的 AFORK/ASU/ACOMPAT/ACORE/AXSIG 位集合；
	 * v12 起 AGROUP 表示该 PID 退出记录来自线程组最后一个任务。ac_nice
	 * 保存 task_nice() 的记账值，不是调度策略本身。
	 */
	__u8	ac_flag;		/* Record flags */
	__u8	ac_nice;		/* task_nice */

	/* Delay accounting fields start
	 *
	 * All values, until comment "Delay accounting fields end" are
	 * available only if delay accounting is enabled, even though the last
	 * few fields are not delays
	 *
	 * xxx_count is the number of delay values recorded
	 * xxx_delay_total is the corresponding cumulative delay in nanoseconds
	 *
	 * xxx_delay_total wraps around to zero on overflow
	 * xxx_count incremented regardless of overflow
	 */
	/*
	 * 从这里到 “Delay accounting fields end” 的字段只有启用 delayacct 时
	 * 才有采集意义，
	 * 其中末尾若干运行时间虽不是“等待延迟”也沿用该配置。
	 * xxx_count 是样本次数，xxx_delay_total 是对应纳秒累计值；total 溢出
	 * 会回绕为零，而 count 仍继续增长，ABI 不提供饱和或溢出标志。
	 */

	/* Delay waiting for cpu, while runnable
	 * count, delay_total NOT updated atomically
	 */
	/*
	 * cpu_count/cpu_delay_total 记录任务可运行但等待 CPU 的次数与总纳秒数。
	 * 两字段不是作为一个原子快照更新，
	 * 主动查询可能观察到一次更新的中间
	 * 组合；退出后的最终快照不再与任务运行并发。
	 */
	__u64	cpu_count __attribute__((aligned(8)));
	__u64	cpu_delay_total;

	/* Following four fields atomically updated using task->delays->lock */
	/*
	 * 接下来的 blkio 与 swapin 两组在 task->delays->lock 下更新；该锁让
	 * 各组计数与累计值保持采集协议一致，但不意味着整个 taskstats 结构
	 * 能与其他子系统字段形成同一时刻的原子快照。
	 */

	/* Delay waiting for synchronous block I/O to complete
	 * does not account for delays in I/O submission
	 */
	/*
	 * blkio_count/blkio_delay_total 只统计同步块 I/O 提交后等待完成的次数
	 * 与纳秒，不包含构造和提交请求本身花费的时间。
	 */
	__u64	blkio_count;
	__u64	blkio_delay_total;

	/* Delay waiting for page fault I/O (swap in only) */
	/*
	 * swapin_* 统计缺页触发 swap-in I/O 的等待次数与纳秒，
	 * 不覆盖普通文件缺页。
	 */
	__u64	swapin_count;
	__u64	swapin_delay_total;

	/* cpu "wall-clock" running time
	 * On some architectures, value will adjust for cpu time stolen
	 * from the kernel in involuntary waits due to virtualization.
	 * Value is cumulative, in nanoseconds, without a corresponding count
	 * and wraps around to zero silently on overflow
	 */
	/*
	 * cpu_run_real_total 是任务被计为运行时的墙钟纳秒；
	 * 部分体系结构会扣除
	 * 虚拟化导致的 stolen time。它没有对应 count，溢出时静默回绕。
	 */
	__u64	cpu_run_real_total;

	/* cpu "virtual" running time
	 * Uses time intervals seen by the kernel i.e. no adjustment
	 * for kernel's involuntary waits due to virtualization.
	 * Value is cumulative, in nanoseconds, without a corresponding count
	 * and wraps around to zero silently on overflow
	 */
	/*
	 * cpu_run_virtual_total 使用内核看到的运行区间，不校正虚拟机被宿主
	 * 抢占的时间；同样以纳秒累计、没有 count，并在溢出时静默回绕。
	 */
	__u64	cpu_run_virtual_total;
	/* Delay accounting fields end */
	/* version 1 ends here */
	/*
	 * 上述标记结束 delayacct 字段，
	 * 同时也是 taskstats v1 的兼容前缀边界。
	 */

	/* Basic Accounting Fields start */
	/*
	 * 基础记账字段描述命令名、调度身份、
	 * 命名空间映射后的 UID/GID/PID、
	 * 起止时间和缺页计数。ac_pad 只用于固定 ABI 对齐，不承载语义。
	 */
	char	ac_comm[TS_COMM_LEN];	/* Command name */
	__u8	ac_sched __attribute__((aligned(8)));
					/* Scheduling discipline */
	__u8	ac_pad[3];
	__u32	ac_uid __attribute__((aligned(8)));
					/* User ID */
	__u32	ac_gid;			/* Group ID */
	__u32	ac_pid;			/* Process ID */
	__u32	ac_ppid;		/* Parent process ID */
	/* __u32 range means times from 1970 to 2106 */
	/*
	 * 32 位秒数只能表达 1970—2106；
	 * v10 的 ac_btime64 是长期兼容替代字段。
	 */
	__u32	ac_btime;		/* Begin time [sec since 1970] */
	__u64	ac_etime __attribute__((aligned(8)));
					/* Elapsed time [usec] */
	__u64	ac_utime;		/* User CPU time [usec] */
	__u64	ac_stime;		/* SYstem CPU time [usec] */
	__u64	ac_minflt;		/* Minor Page Fault Count */
	__u64	ac_majflt;		/* Major Page Fault Count */
	/* Basic Accounting Fields end */
	/*
	 * ac_etime/ac_utime/ac_stime 单位均为微秒；minflt 不需要磁盘 I/O，
	 * majflt 通常需要取回页面。以上标记结束基础记账字段组。
	 */

	/* Extended accounting fields start */
	/* 扩展记账字段由 CONFIG_TASK_XACCT 路径填充，关闭时保持为零。 */
	/* Accumulated RSS usage in duration of a task, in MBytes-usecs.
	 * The current rss usage is added to this counter every time
	 * a tick is charged to a task's system time. So, at the end we
	 * will have memory usage multiplied by system time. Thus an
	 * average usage per system time unit can be calculated.
	 */
	/*
	 * coremem 把每个系统时间 tick 观察到的 RSS 与时间相乘后累计，单位
	 * MB-usec；它不是退出瞬间 RSS。
	 * 用它除以相应系统时间可估算平均 RSS。
	 */
	__u64	coremem;		/* accumulated RSS usage in MB-usec */
	/* Accumulated virtual memory usage in duration of a task.
	 * Same as acct_rss_mem1 above except that we keep track of VM usage.
	 */
	/*
	 * virtmem 使用同样的时间加权方法累计虚拟地址空间大小，
	 * 而不是常驻集。
	 */
	__u64	virtmem;		/* accumulated VM  usage in MB-usec */

	/* High watermark of RSS and virtual memory usage in duration of
	 * a task, in KBytes.
	 */
	/*
	 * hiwater_rss/hiwater_vm 分别保存任务生命周期内
	 * RSS/虚拟内存峰值，单位 KB。
	 */
	__u64	hiwater_rss;		/* High-watermark of RSS usage, in KB */
	__u64	hiwater_vm;		/* High-water VM usage, in KB */

	/* The following four fields are I/O statistics of a task. */
	/*
	 * read_char/write_char 是传给读写接口的字节量，read_syscalls/
	 * write_syscalls 是相应系统调用次数；它们不等同于实际存储设备 I/O。
	 */
	__u64	read_char;		/* bytes read */
	__u64	write_char;		/* bytes written */
	__u64	read_syscalls;		/* read syscalls */
	__u64	write_syscalls;		/* write syscalls */
	/* Extended accounting fields end */
	/* 上述标记结束 CONFIG_TASK_XACCT 管理的扩展字段。 */

#define TASKSTATS_HAS_IO_ACCOUNTING
	/* Per-task storage I/O accounting starts */
	/*
	 * 宏通知用户头文件具备存储 I/O 记账字段。read_bytes/write_bytes 是实际
	 * 发往存储层的字节量，cancelled_write_bytes
	 * 统计被截断等路径取消的写入。
	 */
	__u64	read_bytes;		/* bytes of read I/O */
	__u64	write_bytes;		/* bytes of write I/O */
	__u64	cancelled_write_bytes;	/* bytes of cancelled write I/O */

	__u64  nvcsw;			/* voluntary_ctxt_switches */
	__u64  nivcsw;			/* nonvoluntary_ctxt_switches */
	/*
	 * nvcsw/nivcsw 分别是主动阻塞让出
	 * 与被调度器抢占造成的上下文切换次数。
	 */

	/* time accounting for SMT machines */
	/*
	 * scaled CPU 时间按频率、SMT 容量等体系结构尺度校正，
	 * 用于和未缩放的
	 * utime/stime/real_total 区分；具体精度取决于体系结构记账支持。
	 */
	__u64	ac_utimescaled;		/* utime scaled on frequency etc */
	__u64	ac_stimescaled;		/* stime scaled on frequency etc */
	__u64	cpu_scaled_run_real_total; /* scaled cpu_run_real_total */

	/* Delay waiting for memory reclaim */
	/*
	 * freepages_* 记录任务在直接内存回收中
	 * 等待释放页面的次数与纳秒。
	 */
	__u64	freepages_count;
	__u64	freepages_delay_total;


	/* Delay waiting for thrashing page */
	/* thrashing_* 记录 refault/thrashing 相关页面等待次数与纳秒。 */
	__u64	thrashing_count;
	__u64	thrashing_delay_total;

	/* v10: 64-bit btime to avoid overflow */
	/* v10 追加 64 位启动秒数，避免旧 ac_btime 的 2106 年边界。 */
	__u64	ac_btime64;		/* 64-bit begin time */

	/* v11: Delay waiting for memory compact */
	/* compact_* 记录任务等待内存规整的次数与总纳秒。 */
	__u64	compact_count;
	__u64	compact_delay_total;

	/* v12 begin */
	/* v12 从这里开始增加线程组身份、组墙钟时间和可执行文件标识。 */
	__u32   ac_tgid;	/* thread group ID */
	/* Thread group walltime up to now. This is total process walltime if
	 * AGROUP flag is set.
	 */
	/*
	 * ac_tgetime 是截至采样时的线程组墙钟微秒；仅当 AGROUP 置位、即最后
	 * 线程退出时，它才代表完整进程生命周期，否则只是当前累计值。
	 */
	__u64	ac_tgetime __attribute__((aligned(8)));
	/* Lightweight information to identify process binary files.
	 * This leaves userspace to match this to a file system path, using
	 * MAJOR() and MINOR() macros to identify a device and mount point,
	 * the inode to identify the executable file. This is /proc/self/exe
	 * at the end, so matching the most recent exec(). Values are zero
	 * for kernel threads.
	 */
	/*
	 * ac_exe_dev/ac_exe_inode 用设备号与 inode 轻量标识最后一次 exec 的
	 * /proc/self/exe。用户可用 MAJOR/MINOR 找挂载设备，再以 inode 匹配文件；
	 * 内核线程没有用户可执行文件，两字段为零。
	 * 它们不是路径，也不阻止
	 * 文件被重命名或删除。
	 */
	__u64   ac_exe_dev;     /* program binary device ID */
	__u64   ac_exe_inode;   /* program binary inode number */
	/* v12 end */
	/* v12 兼容字段到此结束。 */

	/* v13: Delay waiting for write-protect copy */
	/*
	 * wpcopy_* 记录写保护缺页执行 copy-on-write 时
	 * 等待复制的次数与总纳秒。
	 */
	__u64    wpcopy_count;
	__u64    wpcopy_delay_total;

	/* v14: Delay waiting for IRQ/SOFTIRQ */
	/* irq_* 记录任务因硬中断或软中断工作而延迟的次数与总纳秒。 */
	__u64    irq_count;
	__u64    irq_delay_total;

	/* v15: add Delay max and Delay min */
	/*
	 * v15 首次引入各类延迟极值；
	 * v16 将这些字段移动到结构尾部稳定布局。
	 */

	/* v16: move Delay max and Delay min to the end of taskstat */
	/*
	 * 下列 *_delay_max/min 均以纳秒记录
	 * 对应延迟类别的单次最大值和最小值。
	 * 没有样本时的零值必须结合对应 *_count 判断，
	 * 不能解释成真实最小延迟。
	 */
	__u64	cpu_delay_max;
	__u64	cpu_delay_min;

	__u64	blkio_delay_max;
	__u64	blkio_delay_min;

	__u64	swapin_delay_max;
	__u64	swapin_delay_min;

	__u64	freepages_delay_max;
	__u64	freepages_delay_min;

	__u64	thrashing_delay_max;
	__u64	thrashing_delay_min;

	__u64	compact_delay_max;
	__u64	compact_delay_min;

	__u64	wpcopy_delay_max;
	__u64	wpcopy_delay_min;

	__u64	irq_delay_max;
	__u64	irq_delay_min;

	/*v17: delay max timestamp record*/
	/*
	 * v17 为每类最大延迟增加 __kernel_timespec 时间戳，记录最大样本发生
	 * 的时间。结构使用 UAPI timespec 以固定秒/纳秒布局，字段顺序与上面
	 * max/min 类别一一对应。
	 */
	struct __kernel_timespec cpu_delay_max_ts;
	struct __kernel_timespec blkio_delay_max_ts;
	struct __kernel_timespec swapin_delay_max_ts;
	struct __kernel_timespec freepages_delay_max_ts;
	struct __kernel_timespec thrashing_delay_max_ts;
	struct __kernel_timespec compact_delay_max_ts;
	struct __kernel_timespec wpcopy_delay_max_ts;
	struct __kernel_timespec irq_delay_max_ts;
};


/*
 * Commands sent from userspace
 * Not versioned. New commands should only be inserted at the enum's end
 * prior to __TASKSTATS_CMD_MAX
 */
/*
 * 命令编号不随 struct taskstats 的版本号变化。新命令只能追加在内部 MAX
 * 哨兵之前，保持既有数值不变；TASKSTATS_CMD_MAX 导出最后一个有效编号。
 */

/*
 * TASKSTATS_CMD_UNSPEC 保留为无效/未指定；GET 是用户到内核的查询或订阅
 * 操作，NEW 是内核返回查询结果或主动发送退出事件时使用的消息命令。
 */
enum {
	TASKSTATS_CMD_UNSPEC = 0,	/* Reserved */
	TASKSTATS_CMD_GET,		/* user->kernel request/get-response */
	TASKSTATS_CMD_NEW,		/* kernel->user event */
	__TASKSTATS_CMD_MAX,
};

#define TASKSTATS_CMD_MAX (__TASKSTATS_CMD_MAX - 1)

enum {
	/*
	 * TYPE_PID/TGID 携带 u32 标识，TYPE_STATS 携带 struct taskstats。
	 * AGGR_PID/AGGR_TGID 是无直接业务载荷的嵌套容器，把随后标识和统计
	 * 组成一条记录；TYPE_NULL 仅作为 64 位属性对齐 padding。
	 */
	TASKSTATS_TYPE_UNSPEC = 0,	/* Reserved */
	TASKSTATS_TYPE_PID,		/* Process id */
	TASKSTATS_TYPE_TGID,		/* Thread group id */
	TASKSTATS_TYPE_STATS,		/* taskstats structure */
	TASKSTATS_TYPE_AGGR_PID,	/* contains pid + stats */
	TASKSTATS_TYPE_AGGR_TGID,	/* contains tgid + stats */
	TASKSTATS_TYPE_NULL,		/* contains nothing */
	__TASKSTATS_TYPE_MAX,
};

#define TASKSTATS_TYPE_MAX (__TASKSTATS_TYPE_MAX - 1)

enum {
	/*
	 * CMD_ATTR_PID/TGID 请求即时快照；REGISTER/DEREGISTER_CPUMASK 的载荷
	 * 是 CPU 列表字符串，用请求方 Netlink port ID 增删退出事件订阅。
	 */
	TASKSTATS_CMD_ATTR_UNSPEC = 0,
	TASKSTATS_CMD_ATTR_PID,
	TASKSTATS_CMD_ATTR_TGID,
	TASKSTATS_CMD_ATTR_REGISTER_CPUMASK,
	TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK,
	__TASKSTATS_CMD_ATTR_MAX,
};

#define TASKSTATS_CMD_ATTR_MAX (__TASKSTATS_CMD_ATTR_MAX - 1)

/* NETLINK_GENERIC related info */
/*
 * 用户先按 TASKSTATS_GENL_NAME 向 Generic Netlink controller 查询动态
 * family ID；GENL_VERSION 是消息协议版本，
 * 与结构字段版本 TASKSTATS_VERSION 相互独立。
 */

#define TASKSTATS_GENL_NAME	"TASKSTATS"
#define TASKSTATS_GENL_VERSION	0x1

#endif /* _LINUX_TASKSTATS_H */
