/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sysctl.h: General linux system control interface
 *
 * Begun 24 March 1995, Stephen Tweedie
 *
 ****************************************************************
 ****************************************************************
 **
 **  WARNING:
 **  The values in this file are exported to user space via
 **  the sysctl() binary interface.  Do *NOT* change the
 **  numbering of any existing values here, and do not change
 **  any numbers within any one set of values.  If you have to
 **  redefine an existing interface, use a new number for it.
 **  The kernel will then return -ENOTDIR to any application using
 **  the old binary interface.
 **
 ****************************************************************
 ****************************************************************
 */
/*
 * ============================================================================
 * 【sysctl（系统控制）接口概述】
 *
 * 【什么是 sysctl？】
 * sysctl 是 Linux/Unix 系统的运行时配置接口，允许用户和程序在系统运行时
 * 查询和修改内核参数，无需重新编译内核或重启系统。
 *
 * 【两种访问方式】
 * 1. /proc/sys 文件系统接口（推荐）
 *    - 读取：cat /proc/sys/kernel/hostname
 *    - 写入：echo "newhost" > /proc/sys/kernel/hostname
 *    - 工具：sysctl 命令（sysctl -w kernel.hostname=newhost）
 *
 * 2. sysctl() 系统调用（已废弃）
 *    - 二进制接口，通过数字 ID 访问
 *    - 不推荐使用，本头文件的编号保持兼容性
 *
 * 【sysctl 的层次结构】
 * /proc/sys/
 *   ├── kernel/      内核核心参数（hostname, panic, pid_max 等）
 *   ├── vm/          虚拟内存管理（swappiness, dirty_ratio 等）
 *   ├── fs/          文件系统（file-max, inode-max 等）
 *   ├── net/         网络协议栈（ipv4, ipv6 等）
 *   ├── dev/         设备驱动参数
 *   └── user/        用户命名空间限制
 *
 * 【典型使用场景】
 * - 性能调优：vm.swappiness, net.core.rmem_max
 * - 安全加固：kernel.dmesg_restrict, kernel.kptr_restrict
 * - 调试开关：kernel.panic_on_oops, vm.drop_caches
 * - 资源限制：fs.file-max, kernel.pid_max
 * - 网络配置：net.ipv4.ip_forward, net.ipv4.tcp_syncookies
 *
 * 【注意事项】
 * ⚠️ 本头文件中的数字编号（CTL_KERN, CTL_VM 等）用于旧的二进制接口，
 *    必须保持向后兼容。新增接口应使用新编号，不要修改现有编号！
 * ⚠️ 修改错误的 sysctl 参数可能导致系统不稳定或性能下降
 * ⚠️ 某些参数修改需要 root 权限（CAP_SYS_ADMIN）
 * ============================================================================
 */
#ifndef _LINUX_SYSCTL_H
#define _LINUX_SYSCTL_H

#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/wait.h>
#include <linux/rbtree.h>
#include <linux/uidgid.h>
#include <uapi/linux/sysctl.h>

/* For the /proc/sys support */
struct completion;
struct ctl_table;
struct nsproxy;
struct ctl_table_root;
struct ctl_table_header;
struct ctl_dir;

/* Keep the same order as in fs/proc/proc_sysctl.c */
/*
 * 【预定义的常用整数值】
 *
 * 这些全局常量用于 sysctl 条目的 extra1/extra2 字段，
 * 定义参数的取值范围（最小值/最大值）。
 *
 * 【为什么使用全局常量？】
 * 避免每个 sysctl 条目都分配独立的内存存储常用值（如 0, 1, 100），
 * 节省内存并提高缓存命中率。
 *
 * 【顺序要求】
 * 注释说明必须与 fs/proc/proc_sysctl.c 中的 sysctl_vals[] 数组顺序一致！
 */
#define SYSCTL_ZERO			((void *)&sysctl_vals[0])
/* 整数 0
 * 用途：作为最小值（extra1），如禁止负数的参数
 * 示例：vm.overcommit_memory 的最小值为 0
 */

#define SYSCTL_ONE			((void *)&sysctl_vals[1])
/* 整数 1
 * 用途：
 * - 作为最小值（extra1），如至少为 1 的计数器
 * - 作为布尔值的 true（某些参数）
 * 示例：kernel.pid_max 的最小值为 1
 */

#define SYSCTL_TWO			((void *)&sysctl_vals[2])
/* 整数 2
 * 用途：某些枚举类型参数的上限
 */

#define SYSCTL_THREE			((void *)&sysctl_vals[3])
/* 整数 3
 * 用途：某些枚举类型参数的上限
 */

#define SYSCTL_FOUR			((void *)&sysctl_vals[4])
/* 整数 4
 * 用途：某些枚举类型参数的上限
 * 示例：某些调试级别参数（0-4）
 */

#define SYSCTL_ONE_HUNDRED		((void *)&sysctl_vals[5])
/* 整数 100
 * 用途：百分比参数的最大值
 * 示例：vm.swappiness 的最大值为 100（代表 100%）
 */

#define SYSCTL_TWO_HUNDRED		((void *)&sysctl_vals[6])
/* 整数 200
 * 用途：某些百分比参数允许超过 100%
 * 示例：vm.dirty_ratio 在某些情况下可以达到 200
 */

#define SYSCTL_ONE_THOUSAND		((void *)&sysctl_vals[7])
/* 整数 1000
 * 用途：
 * - 以毫秒为单位的时间参数上限（1 秒）
 * - 某些计数器的上限
 */

#define SYSCTL_THREE_THOUSAND		((void *)&sysctl_vals[8])
/* 整数 3000
 * 用途：某些超时参数的上限（3 秒）
 */

#define SYSCTL_INT_MAX			((void *)&sysctl_vals[9])
/* 整数 INT_MAX (2147483647)
 * 用途：整数参数的最大值（无实际限制）
 * 示例：fs.file-max 可以设置到 INT_MAX
 */

/* this is needed for the proc_dointvec_minmax for [fs_]overflow UID and GID */
#define SYSCTL_MAXOLDUID		((void *)&sysctl_vals[10])
/* 整数 65535
 * 用途：旧的 16 位 UID/GID 系统的最大值
 * 背景：早期 Linux 使用 16 位 UID（0-65535），现在使用 32 位
 * 示例：fs.overflowuid, fs.overflowgid 的最大值
 */

#define SYSCTL_NEG_ONE			((void *)&sysctl_vals[11])
/* 整数 -1
 * 用途：表示"无限制"或"禁用"的特殊值
 * 示例：某些资源限制参数，-1 表示不限制
 */

extern const int sysctl_vals[];
/* 全局数组：存储上述所有预定义整数值
 * 声明为 const，编译器可以将其放入只读内存段（.rodata）
 */

/*
 * 【预定义的长整数值】
 *
 * 类似 sysctl_vals，但用于 long 类型的参数。
 */
#define SYSCTL_LONG_ZERO	((void *)&sysctl_long_vals[0])
/* 长整数 0 */

#define SYSCTL_LONG_ONE		((void *)&sysctl_long_vals[1])
/* 长整数 1 */

#define SYSCTL_LONG_MAX		((void *)&sysctl_long_vals[2])
/* 长整数 LONG_MAX
 * 32 位系统：2147483647
 * 64 位系统：9223372036854775807
 */

/**
 *
 * "dir" originates from read_iter (dir = 0) or write_iter (dir = 1)
 * in the file_operations struct at proc/proc_sysctl.c. Its value means
 * one of two things for sysctl:
 * 1. SYSCTL_USER_TO_KERN(dir) Writing to an internal kernel variable from user
 *                             space (dir > 0)
 * 2. SYSCTL_KERN_TO_USER(dir) Writing to a user space buffer from a kernel
 *                             variable (dir == 0).
 */
/*
 * 【数据传输方向宏】
 *
 * 在 sysctl 处理函数中，dir 参数表示数据传输方向：
 * - dir = 0: 读操作（内核 → 用户空间）
 * - dir = 1: 写操作（用户空间 → 内核）
 *
 * 【为什么需要这些宏？】
 * 提高代码可读性，避免直接比较 dir 的数字值。
 */
#define SYSCTL_USER_TO_KERN(dir) (!!(dir))
/* 判断是否是写操作（用户 → 内核）
 * 返回值：true (非零) = 写操作，false (0) = 读操作
 *
 * !! 双重取反：
 * - 第一次取反：非零变0，0变非零
 * - 第二次取反：转回布尔值（0或1）
 * 效果：将任意非零值标准化为 1
 */

#define SYSCTL_KERN_TO_USER(dir) (!dir)
/* 判断是否是读操作（内核 → 用户）
 * 返回值：true (非零) = 读操作，false (0) = 写操作
 */

extern const unsigned long sysctl_long_vals[];
/* 全局数组：存储预定义的长整数值 */

/*
 * 【函数指针类型】proc_handler - sysctl 参数处理函数
 *
 * 这是所有 sysctl 处理函数的通用签名。
 */
typedef int proc_handler(const struct ctl_table *ctl, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
/* proc_handler - sysctl 参数的读写处理函数
 * @ctl:    sysctl 表项（包含参数的元数据）
 * @write:  操作类型（0=读，1=写）
 * @buffer: 用户空间缓冲区（内核已经处理了 copy_from_user/copy_to_user）
 * @lenp:   输入/输出缓冲区长度（字节）
 * @ppos:   文件位置指针（通常不使用，保持为 0）
 *
 * 返回值：0=成功，负值=错误码
 *
 * 【工作流程】
 * 读操作（write=0）：
 * 1. 从 ctl->data 读取内核变量的值
 * 2. 格式化为字符串（如 "123\n"）
 * 3. 写入 buffer
 * 4. 更新 *lenp 为实际写入的字节数
 *
 * 写操作（write=1）：
 * 1. 从 buffer 读取用户输入的字符串
 * 2. 解析并验证（检查范围、格式）
 * 3. 写入 ctl->data 指向的内核变量
 * 4. 可选：触发副作用（如重新配置子系统）
 *
 * 【常用的处理函数】
 * - proc_dostring:         字符串参数
 * - proc_dointvec:         整数参数
 * - proc_dointvec_minmax:  有范围限制的整数
 * - proc_dobool:           布尔参数（0/1）
 * - proc_doulongvec_minmax: 长整数数组
 */

/*
 * ============================================================================
 * 【预定义的 proc_handler 函数】
 *
 * 内核提供了一系列通用的处理函数，覆盖常见的参数类型。
 * ============================================================================
 */

int proc_dostring(const struct ctl_table *, int, void *, size_t *, loff_t *);
/* 处理字符串参数
 * 示例：kernel.hostname, kernel.domainname
 *
 * 【要求】
 * - ctl->data 指向 char 数组
 * - ctl->maxlen 是数组大小（字节）
 *
 * 【行为】
 * - 读：返回字符串（以 \n 结尾）
 * - 写：复制用户输入，自动添加 \0 终止符，截断超长输入
 */

int proc_dobool(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
/* 处理布尔参数
 * 示例：kernel.panic_on_oops
 *
 * 【要求】
 * - ctl->data 指向 bool 或 int 变量
 *
 * 【行为】
 * - 读：返回 "0" 或 "1"
 * - 写：接受 0/1, Y/N, yes/no, true/false
 */

int proc_dointvec(const struct ctl_table *, int, void *, size_t *, loff_t *);
/* 处理整数参数（无范围限制）
 * 示例：kernel.pid_max (无 extra1/extra2)
 *
 * 【要求】
 * - ctl->data 指向 int 变量
 *
 * 【行为】
 * - 读：返回整数的十进制字符串
 * - 写：解析十进制整数（支持负数）
 */

int proc_dointvec_minmax(const struct ctl_table *table, int dir, void *buffer,
			 size_t *lenp, loff_t *ppos);
/* 处理有范围限制的整数参数
 * 示例：vm.swappiness (范围 0-100)
 *
 * 【要求】
 * - ctl->data 指向 int 变量
 * - ctl->extra1 指向最小值（或 NULL 表示无下限）
 * - ctl->extra2 指向最大值（或 NULL 表示无上限）
 *
 * 【行为】
 * - 写入时验证：min <= value <= max
 * - 超出范围返回 -EINVAL
 *
 * 【典型配置】
 * {
 *     .procname = "swappiness",
 *     .data     = &vm_swappiness,
 *     .maxlen   = sizeof(int),
 *     .mode     = 0644,
 *     .proc_handler = proc_dointvec_minmax,
 *     .extra1   = SYSCTL_ZERO,
 *     .extra2   = SYSCTL_ONE_HUNDRED,
 * }
 */

int proc_dointvec_conv(const struct ctl_table *table, int dir, void *buffer,
		       size_t *lenp, loff_t *ppos,
		       int (*conv)(bool *negp, unsigned long *u_ptr, int *k_ptr,
				   int dir, const struct ctl_table *table));
/* 处理整数参数，支持自定义转换函数
 * @conv: 用户空间 ↔ 内核空间的转换函数
 *
 * 【使用场景】
 * 需要特殊转换逻辑的参数，如：
 * - 单位转换（用户输入秒，内核存储 jiffies）
 * - 枚举映射（用户输入字符串，内核存储枚举值）
 */

int proc_int_k2u_conv_kop(ulong *u_ptr, const int *k_ptr, bool *negp,
			  ulong (*k_ptr_op)(const ulong));
/* 整数：内核 → 用户转换（带操作函数）
 * @u_ptr:     输出：用户空间值
 * @k_ptr:     输入：内核值
 * @negp:      输出：是否为负数
 * @k_ptr_op:  转换操作（如乘以 1000）
 */

int proc_int_u2k_conv_uop(const ulong *u_ptr, int *k_ptr, const bool *negp,
			  ulong (*u_ptr_op)(const ulong));
/* 整数：用户 → 内核转换（带操作函数）
 * @u_ptr:     输入：用户空间值
 * @k_ptr:     输出：内核值
 * @negp:      输入：是否为负数
 * @u_ptr_op:  转换操作（如除以 1000）
 */

int proc_int_conv(bool *negp, ulong *u_ptr, int *k_ptr, int dir,
		  const struct ctl_table *tbl, bool k_ptr_range_check,
		  int (*user_to_kern)(const bool *negp, const ulong *u_ptr, int *k_ptr),
		  int (*kern_to_user)(bool *negp, ulong *u_ptr, const int *k_ptr));
/* 通用整数转换函数（双向）
 * @negp:              是否为负数
 * @u_ptr:             用户空间值
 * @k_ptr:             内核值
 * @dir:               方向（0=读，1=写）
 * @k_ptr_range_check: 是否检查内核值范围
 * @user_to_kern:      用户→内核转换函数
 * @kern_to_user:      内核→用户转换函数
 */

int proc_douintvec(const struct ctl_table *, int, void *, size_t *, loff_t *);
/* 处理无符号整数参数
 * 示例：net.core.somaxconn
 *
 * 【要求】
 * - ctl->data 指向 unsigned int 变量
 *
 * 【行为】
 * - 只接受非负整数
 * - 负数输入返回 -EINVAL
 */

int proc_douintvec_minmax(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
/* 处理有范围限制的无符号整数参数
 * 类似 proc_dointvec_minmax，但用于 unsigned int
 */

int proc_douintvec_conv(const struct ctl_table *table, int write, void *buffer,
			size_t *lenp, loff_t *ppos,
			int (*conv)(unsigned long *lvalp, unsigned int *valp,
				    int write, const struct ctl_table *table));
/* 处理无符号整数参数，支持自定义转换 */

int proc_uint_k2u_conv(ulong *u_ptr, const uint *k_ptr);
/* 无符号整数：内核 → 用户转换 */

int proc_uint_u2k_conv_uop(const ulong *u_ptr, uint *k_ptr,
			   ulong (*u_ptr_op)(const ulong));
/* 无符号整数：用户 → 内核转换（带操作函数） */

int proc_uint_conv(ulong *u_ptr, uint *k_ptr, int dir,
		   const struct ctl_table *tbl, bool k_ptr_range_check,
		   int (*user_to_kern)(const ulong *u_ptr, uint *k_ptr),
		   int (*kern_to_user)(ulong *u_ptr, const uint *k_ptr));
/* 通用无符号整数转换函数 */

int proc_dou8vec_minmax(const struct ctl_table *table, int write, void *buffer,
			size_t *lenp, loff_t *ppos);
/* 处理 8 位无符号整数（u8）参数
 * 用于取值范围 0-255 的参数
 */

int proc_doulongvec_minmax(const struct ctl_table *, int, void *, size_t *, loff_t *);
/* 处理长整数数组参数
 * 示例：kernel.random.boot_id (UUID)
 *
 * 【要求】
 * - ctl->data 指向 unsigned long 数组
 * - ctl->maxlen 是数组总字节数
 *
 * 【行为】
 * - 读：返回空格分隔的数字列表
 * - 写：解析空格分隔的数字
 */

int proc_doulongvec_minmax_conv(const struct ctl_table *table, int dir,
				void *buffer, size_t *lenp, loff_t *ppos,
				unsigned long convmul, unsigned long convdiv);
/* 处理长整数数组，支持乘除转换
 * @convmul: 读取时乘以此值（单位转换）
 * @convdiv: 写入时除以此值
 *
 * 【使用场景】
 * 单位转换，如将内核的页数转换为用户看到的 KB
 */

int proc_do_large_bitmap(const struct ctl_table *, int, void *, size_t *, loff_t *);
/* 处理大型位图参数
 * 示例：用于 CPU 亲和性掩码等
 *
 * 【格式】
 * 读/写：逗号分隔的范围列表（如 "0-3,8,12-15"）
 */

int proc_do_static_key(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
/* 处理静态键（static key）参数
 *
 * 【静态键是什么？】
 * Linux 内核的优化机制，用于高性能的条件分支。
 * 通过运行时代码修改（self-modifying code）实现接近零开销的开关。
 *
 * 【使用场景】
 * 调试开关、性能追踪开关等很少改变但频繁检查的标志
 */

/*
 * Register a set of sysctl names by calling register_sysctl
 * with an initialised array of struct ctl_table's.
 *
 * sysctl names can be mirrored automatically under /proc/sys.  The
 * procname supplied controls /proc naming.
 *
 * The table's mode will be honoured for proc-fs access.
 *
 * Leaf nodes in the sysctl tree will be represented by a single file
 * under /proc; non-leaf nodes will be represented by directories.  A
 * null procname disables /proc mirroring at this node.
 *
 * The data and maxlen fields of the ctl_table
 * struct enable minimal validation of the values being written to be
 * performed, and the mode field allows minimal authentication.
 *
 * There must be a proc_handler routine for any terminal nodes
 * mirrored under /proc/sys (non-terminals are handled by a built-in
 * directory handler).  Several default handlers are available to
 * cover common cases.
 */
/*
 * ============================================================================
 * 【sysctl 注册和使用说明】
 *
 * 【如何注册 sysctl 参数】
 * 1. 定义 struct ctl_table 数组
 * 2. 调用 register_sysctl() 或 register_sysctl_init()
 * 3. 内核自动在 /proc/sys 下创建对应的文件/目录
 *
 * 【/proc/sys 的映射规则】
 * - 叶子节点（有 proc_handler）→ 文件
 * - 非叶子节点（无 proc_handler）→ 目录
 * - procname = NULL → 不在 /proc 中显示
 *
 * 【权限控制】
 * mode 字段控制文件权限（标准 Unix 权限位）：
 * - 0444 (r--r--r--): 所有人可读（敏感信息应谨慎）
 * - 0644 (rw-r--r--): root 可写，其他人可读（常用）
 * - 0600 (rw-------): 仅 root 可读写（安全敏感参数）
 *
 * 【验证机制】
 * - data + maxlen: 基本的缓冲区大小检查
 * - extra1 + extra2: 范围验证（最小值/最大值）
 * - proc_handler: 自定义验证逻辑
 *
 * 【处理函数要求】
 * 叶子节点必须提供 proc_handler，非叶子节点使用内置的目录处理器。
 * 内核提供了多个通用处理函数（proc_dointvec, proc_dostring 等）。
 * ============================================================================
 */

/* Support for userspace poll() to watch for changes */
/*
 * 【结构体】ctl_table_poll - 支持用户空间 poll() 监视 sysctl 变化
 *
 * 某些 sysctl 参数允许用户空间程序通过 poll/select/epoll 等待其值改变。
 */
struct ctl_table_poll {
	atomic_t event;
	/* 事件计数器
	 * 每次参数值改变时递增，用户空间的 poll() 检测到计数器变化后返回。
	 *
	 * 【为什么用 atomic_t？】
	 * 参数可能在中断上下文或多个 CPU 上并发修改，
	 * atomic_t 保证计数器更新的原子性。
	 */

	wait_queue_head_t wait;
	/* 等待队列
	 * 用户空间调用 poll() 时会睡眠在这个队列上，
	 * 参数改变时唤醒所有等待者。
	 *
	 * 【工作流程】
	 * 1. 用户空间：fd = open("/proc/sys/xxx"); poll(fd, ...)
	 * 2. 内核修改参数：proc_sys_poll_notify(poll)
	 * 3. 唤醒等待队列：wake_up(&poll->wait)
	 * 4. 用户空间的 poll() 返回，检测到变化
	 */
};

static inline void *proc_sys_poll_event(struct ctl_table_poll *poll)
{
	return (void *)(unsigned long)atomic_read(&poll->event);
}
/* 获取当前事件计数器的值（作为不透明指针返回）
 * @poll: poll 结构
 *
 * 返回值：事件计数器的值（转换为 void * 指针）
 *
 * 【为什么返回 void *？】
 * 用户空间通过比较两次返回的指针值是否相同来判断参数是否改变，
 * 不需要知道具体的数值，只需要知道"变了"还是"没变"。
 */

#define __CTL_TABLE_POLL_INITIALIZER(name) {				\
	.event = ATOMIC_INIT(0),					\
	.wait = __WAIT_QUEUE_HEAD_INITIALIZER(name.wait) }
/* 静态初始化宏：在编译时初始化 ctl_table_poll 结构
 * @name: 变量名
 *
 * 【使用示例】
 * static struct ctl_table_poll my_poll = __CTL_TABLE_POLL_INITIALIZER(my_poll);
 */

#define DEFINE_CTL_TABLE_POLL(name)					\
	struct ctl_table_poll name = __CTL_TABLE_POLL_INITIALIZER(name)
/* 定义并静态初始化 ctl_table_poll 结构
 * @name: 变量名
 *
 * 【使用示例】
 * DEFINE_CTL_TABLE_POLL(my_poll);
 * // 等价于：
 * // struct ctl_table_poll my_poll = { .event = ATOMIC_INIT(0), ... };
 */

/* A sysctl table is an array of struct ctl_table: */
/*
 * 【核心结构体】ctl_table - sysctl 表项（描述单个参数）
 *
 * 这是 sysctl 系统的核心数据结构，每个 sysctl 参数对应一个 ctl_table。
 */
struct ctl_table {
	const char *procname;		/* Text ID for /proc/sys */
	/* 参数名称（在 /proc/sys 中显示的名字）
	 * 例如："hostname", "pid_max", "swappiness"
	 *
	 * 【命名规则】
	 * - 小写字母和下划线
	 * - 简短且描述性强
	 * - NULL 表示该节点不在 /proc 中显示（内部节点）
	 */

	void *data;
	/* 指向内核变量的指针
	 * 例如：&system_utsname.nodename, &pid_max, &vm_swappiness
	 *
	 * 【类型】
	 * 根据 proc_handler 的不同，data 可以指向：
	 * - int / unsigned int（proc_dointvec）
	 * - bool（proc_dobool）
	 * - char[]（proc_dostring）
	 * - unsigned long[]（proc_doulongvec_minmax）
	 *
	 * 【NULL 的含义】
	 * 目录节点（非叶子节点）的 data 为 NULL
	 */

	int maxlen;
	/* data 指向的缓冲区最大长度（字节）
	 * 例如：sizeof(int), sizeof(system_utsname.nodename)
	 *
	 * 【作用】
	 * - 字符串参数：防止缓冲区溢出
	 * - 数组参数：表示数组总字节数
	 * - 单个整数：sizeof(int) 或 sizeof(long)
	 *
	 * 【注意】
	 * 目录节点的 maxlen 为 0
	 */

	umode_t mode;
	/* 文件权限（标准 Unix 权限位）
	 * 例如：0644, 0444, 0600
	 *
	 * 【常用值】
	 * - 0444: 所有人可读，不可写（只读参数）
	 * - 0644: root 可读写，其他人可读（常用）
	 * - 0600: 仅 root 可读写（安全敏感）
	 * - 0555: 目录（可执行位表示可进入）
	 *
	 * 【位定义】
	 * - S_IRUSR (0400): 用户可读
	 * - S_IWUSR (0200): 用户可写
	 * - S_IRGRP (0040): 组可读
	 * - S_IROTH (0004): 其他人可读
	 */

	proc_handler *proc_handler;	/* Callback for text formatting */
	/* 处理函数指针（读写参数时调用）
	 * 例如：proc_dointvec, proc_dostring, proc_dointvec_minmax
	 *
	 * 【NULL 的含义】
	 * 目录节点的 proc_handler 为 NULL（使用内置的目录处理器）
	 *
	 * 【职责】
	 * - 格式化：将内核变量转换为用户可读的字符串
	 * - 解析：将用户输入的字符串转换为内核变量
	 * - 验证：检查输入值的合法性
	 * - 副作用：修改参数后可能触发的操作（如重新配置）
	 */

	struct ctl_table_poll *poll;
	/* poll 支持（可选）
	 * 如果不为 NULL，用户空间可以用 poll() 等待参数改变。
	 *
	 * 【使用场景】
	 * 需要实时监控的参数，如：
	 * - 系统状态变化（负载、内存压力）
	 * - 配置热加载（检测配置文件是否被管理员修改）
	 *
	 * 【大多数参数】
	 * poll = NULL（不支持 poll，用户只能主动轮询读取）
	 */

	void *extra1;
	/* 额外参数 1（通常是最小值）
	 * 例如：SYSCTL_ZERO, SYSCTL_ONE
	 *
	 * 【proc_dointvec_minmax 的用法】
	 * extra1 = &min_value，写入时检查 value >= min_value
	 *
	 * 【其他用途】
	 * 某些 proc_handler 用 extra1 传递其他信息
	 */

	void *extra2;
	/* 额外参数 2（通常是最大值）
	 * 例如：SYSCTL_INT_MAX, SYSCTL_ONE_HUNDRED
	 *
	 * 【proc_dointvec_minmax 的用法】
	 * extra2 = &max_value，写入时检查 value <= max_value
	 */
} __randomize_layout;
/* __randomize_layout 属性：
 * GCC 插件随机化结构体字段的布局（KASLR 的一部分），
 * 增加攻击者利用内核漏洞的难度。
 *
 * 【为什么随机化 ctl_table？】
 * 如果攻击者知道结构体的确切布局，可以通过内存破坏漏洞
 * 精确覆盖 proc_handler 指针，劫持控制流。
 * 随机化布局使攻击更加困难（需要先泄漏布局信息）。
 */

/*
 * 【结构体】ctl_node - 红黑树节点（将 ctl_table_header 组织成树）
 */
struct ctl_node {
	struct rb_node node;
	/* 红黑树节点
	 * 用于将 sysctl 目录组织成层次结构
	 */

	struct ctl_table_header *header;
	/* 指向对应的 ctl_table_header */
};

/**
 * struct ctl_table_header - maintains dynamic lists of struct ctl_table trees
 * @ctl_table: pointer to the first element in ctl_table array
 * @ctl_table_size: number of elements pointed by @ctl_table
 * @used: The entry will never be touched when equal to 0.
 * @count: Upped every time something is added to @inodes and downed every time
 *         something is removed from inodes
 * @nreg: When nreg drops to 0 the ctl_table_header will be unregistered.
 * @rcu: Delays the freeing of the inode. Introduced with "unfuck proc_sysctl ->d_compare()"
 *
 * @type: Enumeration to differentiate between ctl target types
 * @type.SYSCTL_TABLE_TYPE_DEFAULT: ctl target with no special considerations
 * @type.SYSCTL_TABLE_TYPE_PERMANENTLY_EMPTY: Identifies a permanently empty dir
 *                                            target to serve as a mount point
 */
/*
 * 【结构体】ctl_table_header - sysctl 表的头部（管理注册的 sysctl 表）
 *
 * 每次调用 register_sysctl() 都会创建一个 ctl_table_header，
 * 用于管理注册的 ctl_table 数组的生命周期。
 */
struct ctl_table_header {
	union {
		struct {
			const struct ctl_table *ctl_table;
			/* 指向 ctl_table 数组的第一个元素
			 * 这是注册时传入的表
			 */

			int ctl_table_size;
			/* ctl_table 数组的元素个数
			 * 通过 ARRAY_SIZE(table) 获得
			 */

			int used;
			/* 使用计数
			 * 等于 0 时，该条目永不触碰（已禁用）
			 */

			int count;
			/* inode 引用计数
			 * 每当 /proc/sys 下创建一个 inode，count++
			 * inode 删除时，count--
			 *
			 * 【用途】
			 * 确保在有 inode 引用时不释放 header
			 */

			int nreg;
			/* 注册计数
			 * 降为 0 时，header 被注销（unregister）
			 *
			 * 【正常情况】
			 * nreg = 1（注册时设置）
			 * unregister_sysctl_table() 时减为 0
			 */
		};
		struct rcu_head rcu;
		/* RCU 延迟释放
		 * 与前面的字段共用内存（union）
		 *
		 * 【为什么需要 RCU？】
		 * /proc/sys 的查找可能在 RCU 临界区中无锁进行，
		 * 释放 header 时需要等待所有读者完成。
		 */
	};
	struct completion *unregistering;
	/* 注销完成信号
	 * unregister_sysctl_table() 等待所有引用释放时使用
	 *
	 * 【工作流程】
	 * 1. unregister_sysctl_table() 设置 unregistering
	 * 2. 等待 count 降为 0
	 * 3. complete(unregistering) 通知注销完成
	 */

	const struct ctl_table *ctl_table_arg;
	/* 原始的 ctl_table 参数（用于调试和错误报告） */

	struct ctl_table_root *root;
	/* 指向所属的 sysctl 根节点
	 * 不同的命名空间有不同的 root
	 */

	struct ctl_table_set *set;
	/* 指向所属的 sysctl 集合
	 * 用于权限检查和可见性控制
	 */

	struct ctl_dir *parent;
	/* 指向父目录
	 * 用于构建 /proc/sys 的目录层次
	 */

	struct ctl_node *node;
	/* 指向红黑树节点
	 * 用于将 header 插入目录树
	 */

	struct hlist_head inodes; /* head for proc_inode->sysctl_inodes */
	/* inode 链表头
	 * 链接所有引用该 header 的 proc_inode
	 * 用于注销时批量清理
	 */

	enum {
		SYSCTL_TABLE_TYPE_DEFAULT,
		/* 默认类型：普通的 sysctl 表 */

		SYSCTL_TABLE_TYPE_PERMANENTLY_EMPTY,
		/* 永久空目录：仅作为挂载点
		 * 用于 register_sysctl_mount_point()
		 */
	} type;
};

/*
 * 【结构体】ctl_dir - sysctl 目录节点
 */
struct ctl_dir {
	/* Header must be at the start of ctl_dir */
	struct ctl_table_header header;
	/* 目录的 header（必须在结构体开头）
	 * 这样可以在 ctl_table_header 和 ctl_dir 之间转换
	 */

	struct rb_root root;
	/* 红黑树根节点
	 * 存储该目录下的所有子节点（文件和子目录）
	 *
	 * 【为什么用红黑树？】
	 * - 支持快速查找（O(log n)）
	 * - 保持节点有序（方便遍历）
	 * - /proc/sys 可能有很多参数，哈希表或链表效率低
	 */
};

/*
 * 【结构体】ctl_table_set - sysctl 表集合（用于命名空间隔离）
 */
struct ctl_table_set {
	int (*is_seen)(struct ctl_table_set *);
	/* 可见性检查函数
	 * 返回值：1=当前进程可见该集合，0=不可见
	 *
	 * 【使用场景】
	 * 容器命名空间：每个容器只能看到自己的 sysctl 参数
	 */

	struct ctl_dir dir;
	/* 集合的根目录 */
};

/*
 * 【结构体】ctl_table_root - sysctl 根节点（命名空间的顶层）
 */
struct ctl_table_root {
	struct ctl_table_set default_set;
	/* 默认集合（所有参数的根） */

	struct ctl_table_set *(*lookup)(struct ctl_table_root *root);
	/* 查找函数：根据当前进程确定使用哪个集合
	 *
	 * 【使用场景】
	 * 网络命名空间：每个网络命名空间有独立的 net.* 参数
	 * 不同进程调用时返回不同的 set
	 */

	void (*set_ownership)(struct ctl_table_header *head,
			      kuid_t *uid, kgid_t *gid);
	/* 设置所有权（用户和组 ID）
	 * 某些命名空间的 sysctl 参数需要特殊的所有者
	 */

	int (*permissions)(struct ctl_table_header *head, const struct ctl_table *table);
	/* 权限检查函数
	 * 返回值：0=允许访问，负值=拒绝
	 *
	 * 【默认行为】
	 * 检查 table->mode 和当前进程的权限
	 *
	 * 【自定义行为】
	 * 某些参数可能需要额外的权限检查（如 CAP_NET_ADMIN）
	 */
};

/*
 * 【宏】register_sysctl - 注册 sysctl 表（常用版本）
 * @path:  在 /proc/sys 下的路径（如 "kernel/random"）
 * @table: ctl_table 数组
 *
 * 这是最常用的注册宏，自动计算数组大小。
 *
 * 【展开后】
 * register_sysctl_sz(path, table, ARRAY_SIZE(table))
 *
 * 【使用示例】
 * static struct ctl_table my_table[] = {
 *     {
 *         .procname = "my_param",
 *         .data     = &my_param,
 *         .maxlen   = sizeof(int),
 *         .mode     = 0644,
 *         .proc_handler = proc_dointvec,
 *     },
 *     { }  // 终止符（可选，但推荐）
 * };
 *
 * static int __init my_init(void) {
 *     register_sysctl("kernel", my_table);
 *     return 0;
 * }
 */
#define register_sysctl(path, table)	\
	register_sysctl_sz(path, table, ARRAY_SIZE(table))

#ifdef CONFIG_SYSCTL

void proc_sys_poll_notify(struct ctl_table_poll *poll);
/* 通知 poll 等待者参数已改变
 * @poll: 要通知的 poll 结构
 *
 * 【调用时机】
 * 内核修改某个支持 poll 的 sysctl 参数后调用
 *
 * 【工作流程】
 * 1. 原子递增 poll->event
 * 2. 唤醒 poll->wait 等待队列
 * 3. 用户空间的 poll() 返回 POLLIN
 */

extern void setup_sysctl_set(struct ctl_table_set *p,
	struct ctl_table_root *root,
	int (*is_seen)(struct ctl_table_set *));
/* 初始化 sysctl 集合
 * @p:       要初始化的集合
 * @root:    所属的根节点
 * @is_seen: 可见性检查函数
 *
 * 【使用场景】
 * 创建新的命名空间时初始化其 sysctl 集合
 */

extern void retire_sysctl_set(struct ctl_table_set *set);
/* 退役（销毁）sysctl 集合
 * @set: 要销毁的集合
 *
 * 【调用时机】
 * 命名空间退出时释放其 sysctl 集合
 */

struct ctl_table_header *__register_sysctl_table(
	struct ctl_table_set *set,
	const char *path, const struct ctl_table *table, size_t table_size);
/* 底层注册函数（指定集合）
 * @set:        要注册到的集合（通常是命名空间的集合）
 * @path:       路径
 * @table:      ctl_table 数组
 * @table_size: 数组大小
 *
 * 返回值：成功返回 ctl_table_header *，失败返回 NULL
 *
 * 【使用场景】
 * 命名空间特定的 sysctl 参数（如网络命名空间）
 */

struct ctl_table_header *register_sysctl_sz(const char *path, const struct ctl_table *table,
					    size_t table_size);
/* 注册 sysctl 表（指定大小）
 * @path:       在 /proc/sys 下的路径（如 "kernel", "vm", "net/ipv4"）
 * @table:      ctl_table 数组
 * @table_size: 数组元素个数
 *
 * 返回值：成功返回 ctl_table_header *，失败返回 NULL
 *
 * 【路径规则】
 * - 不以 / 开头
 * - 用 / 分隔层级（如 "net/ipv4/tcp"）
 * - 自动创建不存在的中间目录
 *
 * 【注意事项】
 * - 返回的 header 用于后续注销（unregister_sysctl_table）
 * - 注册后，/proc/sys/<path>/ 下自动创建文件
 * - 如果路径已存在，新参数会添加到该目录下
 *
 * 【生命周期】
 * 1. register_sysctl_sz() 分配 header
 * 2. 创建 /proc/sys 文件
 * 3. 用户空间通过 /proc/sys 访问参数
 * 4. unregister_sysctl_table(header) 注销
 * 5. 删除 /proc/sys 文件，释放 header
 */

void unregister_sysctl_table(struct ctl_table_header * table);
/* 注销 sysctl 表
 * @table: register_sysctl*() 返回的 header
 *
 * 【工作流程】
 * 1. 从目录树中移除该表
 * 2. 删除 /proc/sys 下的文件
 * 3. 等待所有引用释放（等待 header->count 降为 0）
 * 4. 通过 RCU 延迟释放 header
 *
 * 【注意事项】
 * - 可能阻塞（等待 inode 引用释放）
 * - table 为 NULL 时安全（无操作）
 * - 调用后 table 指针失效，不可再使用
 *
 * 【典型模式】
 * static struct ctl_table_header *my_header;
 *
 * static int __init my_init(void) {
 *     my_header = register_sysctl("kernel", my_table);
 *     if (!my_header)
 *         return -ENOMEM;
 *     return 0;
 * }
 *
 * static void __exit my_exit(void) {
 *     unregister_sysctl_table(my_header);
 * }
 */

extern int sysctl_init_bases(void);
/* 初始化 sysctl 基础设施
 * 在内核启动早期调用，设置根目录和默认集合
 */

extern void __register_sysctl_init(const char *path, const struct ctl_table *table,
				 const char *table_name, size_t table_size);
/* 初始化时注册 sysctl 表（底层函数）
 * @path:       路径
 * @table:      ctl_table 数组
 * @table_name: 表的名字（用于调试）
 * @table_size: 数组大小
 *
 * 【与 register_sysctl 的区别】
 * - __register_sysctl_init: 用于内核初始化时（__init 阶段）
 * - register_sysctl: 用于模块或运行时注册
 *
 * 【优势】
 * 初始化时注册的表不分配 header（节省内存），
 * 因为这些表永不注销（内核核心参数）。
 */

/*
 * 【宏】register_sysctl_init - 初始化时注册 sysctl 表
 * @path:  路径
 * @table: ctl_table 数组
 *
 * 用于内核核心参数的注册（永久性参数，不会注销）。
 *
 * 【使用场景】
 * - 内核核心参数（kernel.*, vm.*, fs.*）
 * - 子系统初始化时的参数
 * - 永不注销的参数
 *
 * 【与 register_sysctl 的选择】
 * - 永久参数：用 register_sysctl_init
 * - 模块参数：用 register_sysctl（需要在模块卸载时注销）
 * - 动态参数：用 register_sysctl
 *
 * 【示例】
 * static struct ctl_table kern_table[] = {
 *     { .procname = "pid_max", ... },
 *     { }
 * };
 *
 * static int __init kernel_sysctl_init(void) {
 *     register_sysctl_init("kernel", kern_table);
 *     return 0;
 * }
 */
#define register_sysctl_init(path, table)	\
	__register_sysctl_init(path, table, #table, ARRAY_SIZE(table))

extern struct ctl_table_header *register_sysctl_mount_point(const char *path);
/* 注册 sysctl 挂载点（空目录）
 * @path: 挂载点路径（如 "fs/binfmt_misc"）
 *
 * 返回值：成功返回 ctl_table_header *，失败返回 NULL
 *
 * 【用途】
 * 创建一个永久空目录，作为其他 sysctl 表的挂载点。
 *
 * 【与普通目录的区别】
 * - 普通目录：注册参数时自动创建，最后一个参数注销时自动删除
 * - 挂载点：显式创建，不会自动删除，即使没有参数也存在
 *
 * 【使用场景】
 * 某些子系统希望目录永久存在，即使暂时没有参数：
 * - fs/binfmt_misc: 二进制格式注册器
 * - debug/: 调试接口挂载点
 *
 * 【示例】
 * static int __init binfmt_misc_init(void) {
 *     // 先创建挂载点
 *     binfmt_misc_header = register_sysctl_mount_point("fs/binfmt_misc");
 *     // 后续可以在该目录下注册参数
 *     return 0;
 * }
 */

void do_sysctl_args(void);
/* 处理内核命令行的 sysctl 参数
 * 解析 sysctl.xxx=yyy 形式的启动参数
 *
 * 【使用场景】
 * 在内核启动时通过命令行设置 sysctl 参数：
 * linux ... sysctl.kernel.panic=10 sysctl.vm.swappiness=60
 */

bool sysctl_is_alias(char *param);
/* 检查参数名是否是 sysctl 别名
 * @param: 参数名（如 "kernel.panic"）
 *
 * 返回值：true=是 sysctl 参数，false=不是
 *
 * 【使用场景】
 * 内核命令行解析器判断参数是否是 sysctl
 */

extern int unaligned_enabled;
/* 是否启用非对齐访问（架构相关）
 * 某些架构（如 ARM）可以配置是否允许非对齐内存访问
 */

extern int no_unaligned_warning;
/* 是否禁用非对齐访问警告
 * 某些程序可能频繁触发非对齐访问，警告会刷屏
 */

#else /* CONFIG_SYSCTL */
/* CONFIG_SYSCTL 未启用时的空实现（避免 #ifdef 遍布代码） */

static inline void register_sysctl_init(const char *path, const struct ctl_table *table)
{
}

static inline struct ctl_table_header *register_sysctl_mount_point(const char *path)
{
	return NULL;
}

static inline struct ctl_table_header *register_sysctl_sz(const char *path,
							  const struct ctl_table *table,
							  size_t table_size)
{
	return NULL;
}

static inline void unregister_sysctl_table(struct ctl_table_header * table)
{
}

static inline void setup_sysctl_set(struct ctl_table_set *p,
	struct ctl_table_root *root,
	int (*is_seen)(struct ctl_table_set *))
{
}

static inline void do_sysctl_args(void)
{
}

static inline bool sysctl_is_alias(char *param)
{
	return false;
}
#endif /* CONFIG_SYSCTL */

#endif /* _LINUX_SYSCTL_H */
