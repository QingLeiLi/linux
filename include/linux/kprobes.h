/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_KPROBES_H
#define _LINUX_KPROBES_H
/*
 *  Kernel Probes (KProbes)
 *  内核探针（KProbes）
 *
 * Copyright (C) IBM Corporation, 2002, 2004
 *
 * 2002-Oct	Created by Vamsi Krishna S <vamsi_krishna@in.ibm.com> Kernel
 *		Probes initial implementation ( includes suggestions from
 *		Rusty Russell).
 * 2002-10月  由 Vamsi Krishna S 创建，内核探针初始实现
 *
 * 2004-July	Suparna Bhattacharya <suparna@in.ibm.com> added jumper probes
 *		interface to access function arguments.
 * 2004-7月   Suparna Bhattacharya 添加了跳转探针接口以访问函数参数
 *
 * 2005-May	Hien Nguyen <hien@us.ibm.com> and Jim Keniston
 *		<jkenisto@us.ibm.com>  and Prasanna S Panchamukhi
 *		<prasanna@in.ibm.com> added function-return probes.
 * 2005-5月   Hien Nguyen 等人添加了函数返回探针
 *
 * =========================================================
 * KProbes 整体设计说明:
 *
 * KProbes 允许在运行中的内核中动态插入探针，无需修改内核源码或重新编译。
 * 工作原理:
 *   1. 将目标地址的指令替换为断点指令（x86 上是 int3）
 *   2. 断点触发时，内核单步执行原始指令的副本
 *   3. 执行用户提供的 pre_handler / post_handler 回调
 *
 * 三种探针类型:
 *   - kprobe:    在任意内核地址插入探针（函数入口、中间、返回）
 *   - kretprobe: 专门跟踪函数返回值
 *   - jprobe:    （已废弃）用于访问函数参数
 *
 * KProbes 与 ftrace 的集成:
 *   当目标函数在入口处有 ftrace 桩时，可复用 ftrace 机制
 *   避免双重断点开销，性能更好（KPROBE_FLAG_FTRACE）
 * =========================================================
 */
#include <linux/compiler.h>
#include <linux/linkage.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/mutex.h>
#include <linux/ftrace.h>
#include <linux/objpool.h>
#include <linux/rethook.h>
#include <asm/kprobes.h>

#ifdef CONFIG_KPROBES

/* kprobe_status settings */
/* kprobe 状态标志位 —— 描述单步执行过程中探针所处的阶段 */
/*
 * KPROBE_HIT_ACTIVE:  探针已被命中，pre_handler 正在或即将执行
 * KPROBE_HIT_SS:      正在单步执行被替换的原始指令
 * KPROBE_REENTER:     在处理一个 kprobe 时又触发了另一个 kprobe（重入）
 *                     重入时必须小心保存/恢复状态，避免破坏外层探针上下文
 * KPROBE_HIT_SSDONE:  单步执行已完成，post_handler 正在或即将执行
 *
 * 状态转换流程:
 *   断点触发 → HIT_ACTIVE（执行 pre_handler）
 *            → HIT_SS（单步执行原始指令）
 *            → HIT_SSDONE（执行 post_handler）
 *            → 恢复正常执行
 */
#define KPROBE_HIT_ACTIVE	0x00000001
#define KPROBE_HIT_SS		0x00000002
#define KPROBE_REENTER		0x00000004
#define KPROBE_HIT_SSDONE	0x00000008

#else /* !CONFIG_KPROBES */
#include <asm-generic/kprobes.h>
typedef int kprobe_opcode_t;
struct arch_specific_insn {
	int dummy;
};
#endif /* CONFIG_KPROBES */

/*
 * 函数指针类型定义 —— kprobe 回调函数的原型
 *
 * kprobe_pre_handler_t: 在探测地址指令执行前调用
 *   @p:    触发的 kprobe 结构指针
 *   @regs: CPU 寄存器状态（可读写，修改会影响执行流）
 *   返回值: 0 表示继续正常执行；非0表示跳过单步执行（谨慎使用）
 *
 * kprobe_post_handler_t: 在探测地址指令执行后调用
 *   @p:     触发的 kprobe 结构指针
 *   @regs:  CPU 寄存器状态
 *   @flags: 保留字段，当前未使用
 *   返回值: void，无返回值
 *
 * kretprobe_handler_t: 函数返回探针的回调
 *   @ri:   当前 kretprobe 实例（包含用户自定义数据 data[]）
 *   @regs: CPU 寄存器状态（regs->ax 等含返回值）
 *   返回值: 0 表示正常；非0目前无特殊意义
 */
struct kprobe;
struct pt_regs;
struct kretprobe;
struct kretprobe_instance;
typedef int (*kprobe_pre_handler_t) (struct kprobe *, struct pt_regs *);
typedef void (*kprobe_post_handler_t) (struct kprobe *, struct pt_regs *,
				       unsigned long flags);
typedef int (*kretprobe_handler_t) (struct kretprobe_instance *,
				    struct pt_regs *);

/*
 * struct kprobe - 描述一个内核探针的核心结构
 *
 * 使用方法:
 *   1. 分配并清零一个 struct kprobe
 *   2. 设置 symbol_name（或 addr）+ offset
 *   3. 设置 pre_handler 和/或 post_handler
 *   4. 调用 register_kprobe() 注册
 *   5. 使用完毕后调用 unregister_kprobe()
 *
 * 注意事项:
 *   - addr 和 symbol_name 二选一，symbol_name 更常用也更安全
 *   - pre_handler 运行在禁中断上下文，不能睡眠
 *   - 不能对以下位置插入探针：kprobe 自身代码、不可屏蔽中断处理程序
 */
struct kprobe {
	struct hlist_node hlist;  // 哈希表节点，用于 kprobe 地址哈希表的快速查找

	/* list of kprobes for multi-handler support */
	/* 多处理器支持的 kprobe 链表 —— 同一地址可以注册多个 kprobe */
	struct list_head list;

	/*count the number of times this probe was temporarily disarmed */
	/* 记录此探针被临时撤销（disarm）的次数，通常因重入或递归触发 */
	unsigned long nmissed;

	/* location of the probe point */
	/* 探针插入的内核地址；与 symbol_name 二选一 */
	kprobe_opcode_t *addr;

	/* Allow user to indicate symbol name of the probe point */
	/* 用符号名指定探测位置，注册时由内核解析为地址（推荐方式）*/
	const char *symbol_name;

	/* Offset into the symbol */
	/* 相对于符号起始地址的字节偏移，常用于探测函数内部特定位置 */
	unsigned int offset;

	/* Called before addr is executed. */
	/* 在目标地址指令执行前调用的回调函数 */
	kprobe_pre_handler_t pre_handler;

	/* Called after addr is executed, unless... */
	/* 在目标地址指令执行后调用（若 pre_handler 返回非0则跳过）*/
	kprobe_post_handler_t post_handler;

	/* Saved opcode (which has been replaced with breakpoint) */
	/* 保存被断点指令替换掉的原始操作码，用于单步执行和撤销探针 */
	kprobe_opcode_t opcode;

	/* copy of the original instruction */
	/* 原始指令的完整副本（含解码信息），单步执行时在此处执行 */
	struct arch_specific_insn ainsn;

	/*
	 * Indicates various status flags.
	 * Protected by kprobe_mutex after this kprobe is registered.
	 */
	/*
	 * 探针状态标志位（见下方 KPROBE_FLAG_* 定义）
	 * 注册后由 kprobe_mutex 保护，注册前可直接设置
	 */
	u32 flags;
};

/* Kprobe status flags */
/* kprobe 标志位 —— 描述探针当前状态和特性 */
/*
 * KPROBE_FLAG_GONE:          探针已被移除但结构尚未释放（延迟清理）
 * KPROBE_FLAG_DISABLED:      探针被临时禁用，命中时不执行回调
 * KPROBE_FLAG_OPTIMIZED:     探针已被优化（用直接跳转替代断点），性能更好
 *                            注意：此标志仅对 optimized_kprobe 结构有效
 * KPROBE_FLAG_FTRACE:        探针通过 ftrace 机制实现，而非断点机制
 *                            当目标函数入口有 ftrace 桩时自动选择此路径
 * KPROBE_FLAG_ON_FUNC_ENTRY: 探针位于函数入口处
 *                            允许某些架构使用更高效的实现路径
 */
#define KPROBE_FLAG_GONE	1 /* breakpoint has already gone */
                                  /* 断点已经消失 */
#define KPROBE_FLAG_DISABLED	2 /* probe is temporarily disabled */
                                  /* 探针被临时禁用 */
#define KPROBE_FLAG_OPTIMIZED	4 /*
				   * probe is really optimized.
				   * NOTE:
				   * this flag is only for optimized_kprobe.
				   */
                                  /* 探针已真正被优化（仅用于 optimized_kprobe）*/
#define KPROBE_FLAG_FTRACE	8 /* probe is using ftrace */
                                  /* 探针使用 ftrace 机制 */
#define KPROBE_FLAG_ON_FUNC_ENTRY	16 /* probe is on the function entry */
                                           /* 探针位于函数入口 */

/* Has this kprobe gone ? */
/* 检查探针是否已被移除（gone 状态） */
static inline bool kprobe_gone(struct kprobe *p)
{
	return p->flags & KPROBE_FLAG_GONE;
}

/* Is this kprobe disabled ? */
/*
 * 检查探针是否被禁用
 * 注意: GONE 状态的探针也视为禁用（|= 两个标志同时检查）
 */
static inline bool kprobe_disabled(struct kprobe *p)
{
	return p->flags & (KPROBE_FLAG_DISABLED | KPROBE_FLAG_GONE);
}

/* Is this kprobe really running optimized path ? */
/* 检查探针是否正在使用优化路径（直接跳转而非断点）*/
static inline bool kprobe_optimized(struct kprobe *p)
{
	return p->flags & KPROBE_FLAG_OPTIMIZED;
}

/* Is this kprobe uses ftrace ? */
/* 检查探针是否使用 ftrace 机制实现 */
static inline bool kprobe_ftrace(struct kprobe *p)
{
	return p->flags & KPROBE_FLAG_FTRACE;
}

/*
 * Function-return probe -
 * Note:
 * User needs to provide a handler function, and initialize maxactive.
 * maxactive - The maximum number of instances of the probed function that
 * can be active concurrently.
 * nmissed - tracks the number of times the probed function's return was
 * ignored, due to maxactive being too low.
 *
 */
/*
 * 函数返回探针（kretprobe）相关结构
 *
 * kretprobe 的工作原理:
 *   1. 在目标函数入口处设置 kprobe
 *   2. 函数被调用时，将返回地址替换为内核跳板（trampoline）
 *   3. 函数返回时先到达跳板，执行用户的 handler，再跳回真正的返回地址
 *
 * maxactive 的选择:
 *   - 递归函数或被多CPU并发调用的函数需要较大的 maxactive
 *   - 若设为0，内核自动选择合理默认值（通常为 NR_CPUS 或 2*NR_CPUS）
 *   - maxactive 过小会导致 nmissed 增加，某些函数返回被跳过
 *
 * data_size 的使用:
 *   - 设置此字段可在每个 kretprobe_instance 的 data[] 尾部附加用户数据
 *   - entry_handler 中写入，handler 中读取，实现跨越函数执行的数据传递
 */

/*
 * struct kretprobe_holder - kretprobe 的持有者，管理实例对象池
 * @rp:   RCU 保护的 kretprobe 指针（支持并发注销）
 * @pool: 对象池，预分配 maxactive 个 kretprobe_instance
 *
 * 设计原因: 使用对象池避免在中断上下文动态分配内存
 */
struct kretprobe_holder {
	struct kretprobe __rcu *rp;   // RCU 保护，允许并发读取并安全注销
	struct objpool_head	pool; // 预分配的 instance 对象池
};

/*
 * struct kretprobe - 函数返回探针结构
 * @kp:            内嵌的基础 kprobe（用于在函数入口处设断点）
 * @handler:       函数返回时调用的回调，可访问返回值和用户数据
 * @entry_handler: 函数入口时调用的回调（可选），用于采集入口信息
 *                 返回非0表示跳过此次调用的返回探测
 * @maxactive:     允许同时活跃的探测实例数，0表示内核自动选择
 * @nmissed:       因实例不足而错过的返回探测次数（监控是否需增大 maxactive）
 * @data_size:     每个实例附带的用户数据大小（字节），上限 KRETPROBE_MAX_DATA_SIZE
 * @rh/@rph:       内部实现字段，依赖 CONFIG_KRETPROBE_ON_RETHOOK 编译选项
 */
struct kretprobe {
	struct kprobe kp;
	kretprobe_handler_t handler;
	kretprobe_handler_t entry_handler;
	int maxactive;
	int nmissed;
	size_t data_size;
#ifdef CONFIG_KRETPROBE_ON_RETHOOK
	struct rethook *rh;       // 使用 rethook 框架实现时的句柄
#else
	struct kretprobe_holder *rph;  // 使用传统实现时的持有者
#endif
};

/* KRETPROBE_MAX_DATA_SIZE: 每个 kretprobe 实例用户数据的最大字节数 */
#define KRETPROBE_MAX_DATA_SIZE	4096

/*
 * struct kretprobe_instance - 单次函数调用对应的 kretprobe 实例
 *
 * 每次目标函数被调用时，从对象池取出一个 instance：
 *   - 保存真实返回地址（ret_addr/node.ret_addr）
 *   - 持有帧指针 fp 用于栈展开验证
 *   - data[] 尾部可存储用户自定义数据（大小由 kretprobe.data_size 决定）
 *
 * 两种实现路径:
 *   CONFIG_KRETPROBE_ON_RETHOOK=y: 基于通用 rethook 框架
 *   CONFIG_KRETPROBE_ON_RETHOOK=n: 传统实现，使用 llist + rcu 管理
 */
struct kretprobe_instance {
#ifdef CONFIG_KRETPROBE_ON_RETHOOK
	struct rethook_node node;      // rethook 框架节点，含返回地址
#else
	struct rcu_head rcu;           // RCU 回调头，用于安全延迟释放
	struct llist_node llist;       // 无锁链表节点，归还到空闲池
	struct kretprobe_holder *rph;  // 指回持有者（找到 kretprobe）
	kprobe_opcode_t *ret_addr;     // 保存的真实返回地址
	void *fp;                      // 帧指针，用于栈验证防止误匹配
#endif
	char data[];                   // 用户自定义数据（柔性数组）
};

/*
 * struct kretprobe_blackpoint - 不允许插入 kretprobe 的黑名单条目
 * @name: 函数名字符串
 * @addr: 解析后的函数地址（启动时填充）
 *
 * 某些函数（如调度器关键路径）不能安全地替换返回地址
 */
struct kretprobe_blackpoint {
	const char *name;
	void *addr;
};

/*
 * struct kprobe_blacklist_entry - kprobe 黑名单地址范围条目
 * @list:       链表节点
 * @start_addr: 禁止插入探针的起始地址（含）
 * @end_addr:   禁止插入探针的结束地址（不含）
 *
 * 某些内核代码区域（如 kprobe 自身、某些中断处理）
 * 不允许被探测，以防止递归和系统不稳定
 */
struct kprobe_blacklist_entry {
	struct list_head list;
	unsigned long start_addr;
	unsigned long end_addr;
};

#ifdef CONFIG_KPROBES
/*
 * current_kprobe: per-CPU 变量，指向当前 CPU 上正在执行的 kprobe
 * 用于检测重入（一个 kprobe handler 内触发了另一个 kprobe）
 * 在 kprobe handler 执行期间非 NULL，其他时候为 NULL
 */
DECLARE_PER_CPU(struct kprobe *, current_kprobe);

/*
 * kprobe_ctlblk: per-CPU kprobe 控制块
 * 保存 kprobe 执行期间的 CPU 状态（如之前的 kprobe 状态、标志寄存器等）
 * 结构体在 asm/kprobes.h 中定义，因架构而异
 */
DECLARE_PER_CPU(struct kprobe_ctlblk, kprobe_ctlblk);

/*
 * kprobe_busy_begin / kprobe_busy_end - 标记 kprobe 处于"忙"状态
 *
 * 用途: 在 kretprobe trampoline handler 中设置虚拟 kprobe，
 * 防止 kretprobe 在自身执行过程中被递归触发。
 *
 * 必须成对调用，不能睡眠。
 */
extern void kprobe_busy_begin(void);
extern void kprobe_busy_end(void);

#ifdef CONFIG_KRETPROBES
/* Check whether @p is used for implementing a trampoline. */
/* 检查 @p 是否是用于实现跳板（trampoline）的内部 kprobe */
extern int arch_trampoline_kprobe(struct kprobe *p);

#ifdef CONFIG_KRETPROBE_ON_RETHOOK
/*
 * get_kretprobe - 从 kretprobe_instance 获取对应的 kretprobe 结构
 * @ri: kretprobe 实例指针
 *
 * 返回值: 对应的 kretprobe 指针
 *
 * nokprobe_inline: 此函数不能被 kprobe 探测，防止递归
 * rethook::data 字段在对象生命周期内不变，可安全访问无需 RCU
 */
static nokprobe_inline struct kretprobe *get_kretprobe(struct kretprobe_instance *ri)
{
	/* rethook::data is non-changed field, so that you can access it freely. */
	/* rethook::data 是不变字段，可自由访问 */
	return (struct kretprobe *)ri->node.rethook->data;
}

/*
 * get_kretprobe_retaddr - 获取 kretprobe 实例保存的真实返回地址
 * @ri: kretprobe 实例指针
 * 返回值: 函数调用者的返回地址
 */
static nokprobe_inline unsigned long get_kretprobe_retaddr(struct kretprobe_instance *ri)
{
	return ri->node.ret_addr;
}
#else
/*
 * arch_prepare_kretprobe - 架构相关：准备 kretprobe 实例
 * @ri:   要初始化的 kretprobe 实例
 * @regs: 当前 CPU 寄存器（含返回地址）
 *
 * 将 regs 中的真实返回地址保存到 ri->ret_addr，
 * 并将 regs 中的返回地址替换为跳板地址
 */
extern void arch_prepare_kretprobe(struct kretprobe_instance *ri,
				   struct pt_regs *regs);

/*
 * arch_kretprobe_fixup_return - 修复 kretprobe 的返回地址
 * @regs:            CPU 寄存器
 * @correct_ret_addr: 应该恢复的真实返回地址
 *
 * 在跳板处理完毕后，将寄存器中的返回地址修正为真实返回地址
 */
void arch_kretprobe_fixup_return(struct pt_regs *regs,
				 kprobe_opcode_t *correct_ret_addr);

/* __kretprobe_trampoline: 汇编实现的跳板函数（entry_32/64.S 中定义）*/
void __kretprobe_trampoline(void);

/*
 * kretprobe_trampoline_addr - 获取跳板函数的真实地址
 *
 * 某些架构使用结构化函数指针（如 IA64），需用
 * dereference_kernel_function_descriptor() 解引用得到实际代码地址
 *
 * nokprobe_inline: 此路径上不能插入探针，防止无限递归
 */
static nokprobe_inline void *kretprobe_trampoline_addr(void)
{
	return dereference_kernel_function_descriptor(__kretprobe_trampoline);
}

/*
 * __kretprobe_trampoline_handler - 跳板核心处理函数
 * @regs:          CPU 寄存器（含被替换的返回地址）
 * @frame_pointer: 帧指针，用于在返回栈中定位正确的实例
 *
 * 返回值: 真实的返回地址（调用者用它修复 PC）
 *
 * 从对象池找到匹配的 kretprobe_instance，
 * 执行用户 handler，归还实例，返回真实地址
 */
unsigned long __kretprobe_trampoline_handler(struct pt_regs *regs,
					     void *frame_pointer);

/*
 * kretprobe_trampoline_handler - 跳板处理器的对外包装
 * @regs:          CPU 寄存器
 * @frame_pointer: 帧指针
 *
 * 在调用内部处理器前后设置"kprobe busy"标志，
 * 防止跳板自身被 kretprobe 探测导致递归
 *
 * nokprobe_inline: 不可被 kprobe 探测
 */
static nokprobe_inline
unsigned long kretprobe_trampoline_handler(struct pt_regs *regs,
					   void *frame_pointer)
{
	unsigned long ret;
	/*
	 * Set a dummy kprobe for avoiding kretprobe recursion.
	 * Since kretprobe never runs in kprobe handler, no kprobe must
	 * be running at this point.
	 */
	/*
	 * 设置一个虚拟 kprobe 防止 kretprobe 递归。
	 * 由于 kretprobe 永远不会在 kprobe handler 内运行，
	 * 此处不应该有 kprobe 正在执行。
	 */
	kprobe_busy_begin();
	ret = __kretprobe_trampoline_handler(regs, frame_pointer);
	kprobe_busy_end();

	return ret;
}

/*
 * get_kretprobe - 从传统实现的 kretprobe_instance 获取 kretprobe
 * @ri: kretprobe 实例
 * 返回值: 对应的 kretprobe 指针（通过 RCU 读锁保护）
 */
static nokprobe_inline struct kretprobe *get_kretprobe(struct kretprobe_instance *ri)
{
	return rcu_dereference_check(ri->rph->rp, rcu_read_lock_any_held());
}

/*
 * get_kretprobe_retaddr - 从传统实现获取真实返回地址
 * @ri: kretprobe 实例
 * 返回值: 保存的真实函数返回地址
 */
static nokprobe_inline unsigned long get_kretprobe_retaddr(struct kretprobe_instance *ri)
{
	return (unsigned long)ri->ret_addr;
}
#endif /* CONFIG_KRETPROBE_ON_RETHOOK */

#else /* !CONFIG_KRETPROBES */
static inline void arch_prepare_kretprobe(struct kretprobe *rp,
					struct pt_regs *regs)
{
}
static inline int arch_trampoline_kprobe(struct kprobe *p)
{
	return 0;
}
#endif /* CONFIG_KRETPROBES */

/* Markers of '_kprobe_blacklist' section */
/*
 * __start_kprobe_blacklist / __stop_kprobe_blacklist
 * 链接脚本生成的符号，标记 _kprobe_blacklist ELF section 的起止地址
 * 该 section 中存放不允许被 kprobe 探测的地址范围
 * 架构代码通过 NOKPROBE_SYMBOL() 宏将函数加入此 section
 */
extern unsigned long __start_kprobe_blacklist[];
extern unsigned long __stop_kprobe_blacklist[];

/* kretprobe 的函数级黑名单（按名称），启动时解析为地址 */
extern struct kretprobe_blackpoint kretprobe_blacklist[];

/*
 * arch_prepare_kprobe - 架构相关：为插入探针做准备
 * @p: 要准备的 kprobe
 * 返回值: 0 成功，负值表示该地址无法被探测
 *
 * 主要工作：
 * - 检查目标地址是否合法（不在黑名单、不是跳转目标中间等）
 * - 分配并复制原始指令到 ainsn 缓冲区
 * - 对指令进行必要的修正（如相对地址修正）
 */
extern int arch_prepare_kprobe(struct kprobe *p);

/*
 * arch_arm_kprobe - 架构相关：激活探针（写入断点指令）
 * @p: 要激活的 kprobe
 * 将目标地址的原始指令替换为断点指令（如 x86 的 int3）
 * 调用前必须已调用 arch_prepare_kprobe
 */
extern void arch_arm_kprobe(struct kprobe *p);

/*
 * arch_disarm_kprobe - 架构相关：撤销探针（恢复原始指令）
 * @p: 要撤销的 kprobe
 * 将断点指令替换回原始指令，探针不再触发
 */
extern void arch_disarm_kprobe(struct kprobe *p);

/* arch_init_kprobes - 架构相关：初始化 kprobe 子系统 */
extern int arch_init_kprobes(void);

/* kprobes_inc_nmissed_count - 增加探针的 nmissed 计数 */
extern void kprobes_inc_nmissed_count(struct kprobe *p);

/*
 * arch_within_kprobe_blacklist - 检查地址是否在架构定义的黑名单中
 * @addr: 要检查的地址
 * 返回值: true 表示该地址不可被探测
 */
extern bool arch_within_kprobe_blacklist(unsigned long addr);

/* arch_populate_kprobe_blacklist - 将架构特定的黑名单地址填充到全局链表 */
extern int arch_populate_kprobe_blacklist(void);

/*
 * kprobe_on_func_entry - 判断探针是否位于函数入口
 * @addr:   探针地址（与 sym 二选一）
 * @sym:    函数名（与 addr 二选一）
 * @offset: 相对于符号的偏移
 * 返回值: 1 表示是函数入口，0 表示不是，负值表示错误
 */
extern int kprobe_on_func_entry(kprobe_opcode_t *addr, const char *sym, unsigned long offset);

/*
 * within_kprobe_blacklist - 检查地址是否在全局 kprobe 黑名单中
 * @addr: 要检查的内核地址
 * 返回值: true 表示该地址禁止被探测
 */
extern bool within_kprobe_blacklist(unsigned long addr);

/* kprobe_add_ksym_blacklist - 将单个内核符号地址加入黑名单 */
extern int kprobe_add_ksym_blacklist(unsigned long entry);

/* kprobe_add_area_blacklist - 将一段地址区间加入黑名单 */
extern int kprobe_add_area_blacklist(unsigned long start, unsigned long end);

/*
 * struct kprobe_insn_cache - kprobe 指令槽缓存管理器
 *
 * kprobe 需要将原始指令复制到可执行的内存页中执行（单步执行阶段）
 * 此结构管理这些"指令页"的分配和回收
 *
 * @mutex:      保护指令缓存的互斥锁
 * @alloc:      分配一个新指令页的函数（通常分配一个内存页）
 * @free:       释放指令页的函数
 * @sym:        用于 kallsyms 识别的符号名
 * @pages:      已分配的指令页链表
 * @insn_size:  每个指令槽的大小（指令+可能的 INT3 填充）
 * @nr_garbage: 待回收的"脏"槽数量（超过阈值时触发垃圾回收）
 *
 * 设计原因:
 * - 集中管理指令内存，避免频繁页分配
 * - 支持优化探针（optinsn）和普通探针使用不同的缓存
 */
struct kprobe_insn_cache {
	struct mutex mutex;
	void *(*alloc)(void);	/* allocate insn page */
	                        /* 分配指令页的函数指针 */
	void (*free)(void *);	/* free insn page */
	                        /* 释放指令页的函数指针 */
	const char *sym;	/* symbol for insn pages */
	                        /* 指令页对应的符号名（用于调试）*/
	struct list_head pages; /* list of kprobe_insn_page */
	                        /* 已分配指令页的链表 */
	size_t insn_size;	/* size of instruction slot */
	                        /* 单个指令槽的大小（字节）*/
	int nr_garbage;         /* 待 GC 的脏槽数量 */
};

#ifdef __ARCH_WANT_KPROBES_INSN_SLOT
/*
 * __get_insn_slot - 从指令缓存中分配一个指令槽
 * @c: 指令缓存管理器
 * 返回值: 指向可执行指令槽的指针，失败返回NULL
 *
 * 从已有页中寻找空闲槽，若无则分配新页
 */
extern kprobe_opcode_t *__get_insn_slot(struct kprobe_insn_cache *c);

/*
 * __free_insn_slot - 释放指令槽
 * @c:     指令缓存管理器
 * @slot:  要释放的槽地址
 * @dirty: 1=标记为脏槽（延迟回收），0=立即回收
 *
 * 脏槽机制用于 RCU 保护：探针注销后不立即回收，
 * 等待所有 CPU 经过静止点后再回收
 */
extern void __free_insn_slot(struct kprobe_insn_cache *c,
			     kprobe_opcode_t *slot, int dirty);

/* sleep-less address checking routine  */
/*
 * __is_insn_slot_addr - 检查地址是否属于指令缓存页（不睡眠版本）
 * @c:    指令缓存管理器
 * @addr: 要检查的地址
 * 返回值: true 表示该地址属于此缓存管理的指令页
 *
 * 用于异常处理路径，判断触发异常的 PC 是否在 kprobe 指令槽中
 */
extern bool __is_insn_slot_addr(struct kprobe_insn_cache *c,
				unsigned long addr);

/*
 * DEFINE_INSN_CACHE_OPS - 定义指令缓存操作函数的宏
 * @__name: 缓存类型名称（insn 或 optinsn）
 *
 * 自动生成以下函数:
 * - get_<name>_slot():    分配指令槽
 * - free_<name>_slot():   释放指令槽
 * - is_kprobe_<name>_slot(): 检查地址是否属于该缓存
 *
 * 设计原因: 用宏生成类型安全的包装函数，避免重复代码
 */
#define DEFINE_INSN_CACHE_OPS(__name)					\
extern struct kprobe_insn_cache kprobe_##__name##_slots;		\
									\
static inline kprobe_opcode_t *get_##__name##_slot(void)		\
{									\
	return __get_insn_slot(&kprobe_##__name##_slots);		\
}									\
									\
static inline void free_##__name##_slot(kprobe_opcode_t *slot, int dirty)\
{									\
	__free_insn_slot(&kprobe_##__name##_slots, slot, dirty);	\
}									\
									\
static inline bool is_kprobe_##__name##_slot(unsigned long addr)	\
{									\
	return __is_insn_slot_addr(&kprobe_##__name##_slots, addr);	\
}

/*
 * KPROBE_INSN_PAGE_SYM - 普通指令页的符号名
 * 用于在 /proc/kallsyms 中显示指令页的地址
 */
#define KPROBE_INSN_PAGE_SYM		"kprobe_insn_page"

/*
 * KPROBE_OPTINSN_PAGE_SYM - 优化指令页的符号名
 */
#define KPROBE_OPTINSN_PAGE_SYM		"kprobe_optinsn_page"

/*
 * kprobe_cache_get_kallsym - 为指令缓存生成 kallsyms 符号
 * @c:       指令缓存管理器
 * @symnum:  符号序号（输入输出）
 * @value:   输出符号地址
 * @type:    输出符号类型（'t' 表示代码）
 * @sym:     输出符号名称
 * 返回值: 0=成功，负值=没有更多符号
 *
 * 用途: 使 kprobe 指令页在 kallsyms 中可见，便于调试
 */
int kprobe_cache_get_kallsym(struct kprobe_insn_cache *c, unsigned int *symnum,
			     unsigned long *value, char *type, char *sym);
#else /* !__ARCH_WANT_KPROBES_INSN_SLOT */
/* 架构不需要指令槽时的空实现 */
#define DEFINE_INSN_CACHE_OPS(__name)					\
static inline bool is_kprobe_##__name##_slot(unsigned long addr)	\
{									\
	return 0;							\
}
#endif

/*
 * 为普通 kprobe 定义指令缓存操作函数
 * 生成: get_insn_slot(), free_insn_slot(), is_kprobe_insn_slot()
 */
DEFINE_INSN_CACHE_OPS(insn);

#ifdef CONFIG_OPTPROBES
/*
 * Internal structure for direct jump optimized probe
 */
/*
 * struct optimized_kprobe - 优化探针结构（直接跳转优化）
 *
 * 设计原因:
 * - 普通 kprobe 通过断点中断触发，开销大
 * - 优化探针将断点替换为直接跳转到 handler，减少上下文切换
 * - 需要更复杂的指令重写（替换多条指令为一个长跳转）
 *
 * @kp:      内嵌的基础 kprobe 结构
 * @list:    优化队列链表节点（批量优化用）
 * @optinsn: 架构相关的优化指令信息（跳转目标、detour buffer 等）
 */
struct optimized_kprobe {
	struct kprobe kp;
	struct list_head list;	/* list for optimizing queue */
	                        /* 优化队列链表（批量处理） */
	struct arch_optimized_insn optinsn;  /* 架构相关的优化指令 */
};

/* Architecture dependent functions for direct jump optimization */
/*
 * 以下为架构相关的优化探针函数（由各架构实现）
 */

/*
 * arch_prepared_optinsn - 检查优化指令是否已准备好
 * @optinsn: 架构相关的优化指令结构
 * 返回值: 非0 表示已准备好，0 表示未准备好
 */
extern int arch_prepared_optinsn(struct arch_optimized_insn *optinsn);

/*
 * arch_check_optimized_kprobe - 检查优化探针的状态
 * @op: 优化探针
 * 返回值: 0=可以优化，负值=不可优化
 */
extern int arch_check_optimized_kprobe(struct optimized_kprobe *op);

/*
 * arch_prepare_optimized_kprobe - 为优化探针准备指令
 * @op:   优化探针
 * @orig: 原始 kprobe（用于复制配置）
 * 返回值: 0=成功，负值=失败
 *
 * 主要工作:
 * - 分配 detour buffer（绕行缓冲区）
 * - 生成跳转指令和调用 handler 的代码
 * - 复制被覆盖的原始指令以便单步执行
 */
extern int arch_prepare_optimized_kprobe(struct optimized_kprobe *op,
					 struct kprobe *orig);

/*
 * arch_remove_optimized_kprobe - 移除优化探针的指令
 * @op: 优化探针
 *
 * 释放 detour buffer 等资源
 */
extern void arch_remove_optimized_kprobe(struct optimized_kprobe *op);

/*
 * arch_optimize_kprobes - 批量激活优化探针
 * @oplist: 待优化的探针链表
 *
 * 将链表中所有探针的断点替换为跳转指令
 * 使用 stop_machine 确保修改安全
 */
extern void arch_optimize_kprobes(struct list_head *oplist);

/*
 * arch_unoptimize_kprobes - 批量撤销优化探针
 * @oplist:    待撤销优化的探针链表
 * @done_list: 已完成撤销的探针链表（输出）
 *
 * 将跳转指令替换回断点或原始指令
 */
extern void arch_unoptimize_kprobes(struct list_head *oplist,
				    struct list_head *done_list);

/*
 * arch_unoptimize_kprobe - 撤销单个优化探针
 * @op: 优化探针
 */
extern void arch_unoptimize_kprobe(struct optimized_kprobe *op);

/*
 * arch_within_optimized_kprobe - 检查地址是否在优化探针的跳转指令范围内
 * @op:   优化探针
 * @addr: 要检查的地址
 * 返回值: 非0 表示在范围内
 *
 * 用途: 判断触发断点的地址是否属于此优化探针
 *       （优化跳转可能覆盖多条原始指令）
 */
extern int arch_within_optimized_kprobe(struct optimized_kprobe *op,
					kprobe_opcode_t *addr);

/*
 * opt_pre_handler - 优化探针的前置处理器
 * @p:    kprobe 指针
 * @regs: CPU 寄存器
 *
 * 在优化探针的 detour buffer 中被调用
 * 调用用户注册的 pre_handler，处理返回值
 */
extern void opt_pre_handler(struct kprobe *p, struct pt_regs *regs);

/*
 * 为优化指令定义缓存操作函数
 * 生成: get_optinsn_slot(), free_optinsn_slot(), is_kprobe_optinsn_slot()
 */
DEFINE_INSN_CACHE_OPS(optinsn);

/*
 * wait_for_kprobe_optimizer - 等待优化工作线程完成
 *
 * 优化是异步执行的（在 kprobe_optimizer 工作队列中）
 * 此函数阻塞直到所有待处理的优化/撤销优化操作完成
 */
extern void wait_for_kprobe_optimizer(void);

/*
 * optprobe_queued_unopt - 检查优化探针是否已排队等待撤销优化
 * @op: 优化探针
 * 返回值: true 表示已排队
 */
bool optprobe_queued_unopt(struct optimized_kprobe *op);

/*
 * kprobe_disarmed - 检查探针是否已撤销（未激活）
 * @p: kprobe
 * 返回值: true 表示探针未激活
 */
bool kprobe_disarmed(struct kprobe *p);
#else /* !CONFIG_OPTPROBES */
static inline void wait_for_kprobe_optimizer(void) { }
#endif /* CONFIG_OPTPROBES */

#ifdef CONFIG_KPROBES_ON_FTRACE
/*
 * kprobe_ftrace_handler - 基于 ftrace 的 kprobe 处理器
 * @ip:        被跟踪函数的地址
 * @parent_ip: 调用者地址
 * @ops:       ftrace_ops
 * @fregs:     ftrace 寄存器上下文
 *
 * 设计原因:
 * - 对于函数入口的 kprobe，可利用 ftrace 已有的钩子
 * - 避免断点开销，性能更好
 * - 与优化探针互补（ftrace 用于函数入口，优化探针用于任意位置）
 */
extern void kprobe_ftrace_handler(unsigned long ip, unsigned long parent_ip,
				  struct ftrace_ops *ops, struct ftrace_regs *fregs);

/*
 * arch_prepare_kprobe_ftrace - 为基于 ftrace 的 kprobe 做准备
 * @p: kprobe
 * 返回值: 0=成功，负值=此探针不适合用 ftrace
 *
 * 检查探针是否位于函数入口且 ftrace 可用
 */
extern int arch_prepare_kprobe_ftrace(struct kprobe *p);

/* Set when ftrace has been killed: kprobes on ftrace must be disabled for safety */
/*
 * kprobe_ftrace_disabled - ftrace 已被 kill 的标志
 *
 * 当 ftrace_kill() 被调用时（通常因严重错误），设置此标志
 * 基于 ftrace 的 kprobe 必须禁用以确保安全
 */
extern bool kprobe_ftrace_disabled __read_mostly;

/*
 * kprobe_ftrace_kill - 禁用所有基于 ftrace 的 kprobe
 *
 * 在 ftrace_kill() 时调用，停止通过 ftrace 触发 kprobe
 */
extern void kprobe_ftrace_kill(void);
#else
static inline int arch_prepare_kprobe_ftrace(struct kprobe *p)
{
	return -EINVAL;
}
static inline void kprobe_ftrace_kill(void) {}
#endif /* CONFIG_KPROBES_ON_FTRACE */

/* Get the kprobe at this addr (if any) - called with preemption disabled */
/*
 * get_kprobe - 获取指定地址的 kprobe
 * @addr: 探针地址
 * 返回值: kprobe 指针，无探针返回 NULL
 *
 * 注意: 必须在禁用抢占的上下文调用（哈希表无锁访问）
 */
struct kprobe *get_kprobe(void *addr);

/* kprobe_running() will just return the current_kprobe on this CPU */
/*
 * kprobe_running - 获取当前 CPU 正在执行的 kprobe
 * 返回值: 当前 kprobe 指针，无正在执行的探针返回 NULL
 *
 * 用途: handler 内部判断嵌套（被探测函数内部再次触发探针）
 */
static inline struct kprobe *kprobe_running(void)
{
	return __this_cpu_read(current_kprobe);
}

/*
 * reset_current_kprobe - 清除当前 CPU 的 kprobe 上下文
 *
 * 探针处理完成后调用，恢复嵌套级别
 */
static inline void reset_current_kprobe(void)
{
	__this_cpu_write(current_kprobe, NULL);
}

/*
 * get_kprobe_ctlblk - 获取当前 CPU 的 kprobe 控制块
 * 返回值: 指向 per-CPU kprobe_ctlblk 的指针
 *
 * 控制块包含嵌套状态、保存的 kprobe 指针等
 */
static inline struct kprobe_ctlblk *get_kprobe_ctlblk(void)
{
	return this_cpu_ptr(&kprobe_ctlblk);
}

/*
 * kprobe_lookup_name - 根据符号名查找地址
 * @name:   函数名
 * @offset: 相对符号的偏移
 * 返回值: 地址指针，查找失败返回 NULL
 *
 * 用途: 允许用户通过函数名注册 kprobe，而非硬编码地址
 */
kprobe_opcode_t *kprobe_lookup_name(const char *name, unsigned int offset);

/*
 * arch_adjust_kprobe_addr - 架构相关：调整探针地址
 * @addr:          原始地址
 * @offset:        偏移量
 * @on_func_entry: 输出参数，是否位于函数入口
 * 返回值: 调整后的地址
 *
 * 用途: 某些架构需要调整地址（如跳过函数描述符，ARM64 BTI 指令）
 */
kprobe_opcode_t *arch_adjust_kprobe_addr(unsigned long addr, unsigned long offset, bool *on_func_entry);

/*
 * register_kprobe - 注册单个 kprobe
 * @p: 要注册的 kprobe（用户填充 addr/symbol_name 和 handler）
 * 返回值: 0=成功，负值=失败
 */
int register_kprobe(struct kprobe *p);

/*
 * unregister_kprobe - 注销单个 kprobe
 * @p: 要注销的 kprobe
 */
void unregister_kprobe(struct kprobe *p);

/*
 * register_kprobes - 批量注册 kprobe
 * @kps: kprobe 指针数组
 * @num: 数组大小
 * 返回值: 成功注册的数量，若全部失败返回负值
 */
int register_kprobes(struct kprobe **kps, int num);

/*
 * unregister_kprobes - 批量注销 kprobe
 * @kps: kprobe 指针数组
 * @num: 数组大小
 */
void unregister_kprobes(struct kprobe **kps, int num);

/*
 * register_kretprobe - 注册返回探针
 * @rp: kretprobe 结构（用户填充 kp 和 handler）
 * 返回值: 0=成功，负值=失败
 */
int register_kretprobe(struct kretprobe *rp);

/*
 * unregister_kretprobe - 注销返回探针
 * @rp: 要注销的 kretprobe
 */
void unregister_kretprobe(struct kretprobe *rp);

/*
 * register_kretprobes - 批量注册返回探针
 * @rps: kretprobe 指针数组
 * @num: 数组大小
 * 返回值: 成功注册的数量
 */
int register_kretprobes(struct kretprobe **rps, int num);

/*
 * unregister_kretprobes - 批量注销返回探针
 * @rps: kretprobe 指针数组
 * @num: 数组大小
 */
void unregister_kretprobes(struct kretprobe **rps, int num);

#if defined(CONFIG_KRETPROBE_ON_RETHOOK) || !defined(CONFIG_KRETPROBES)
/*
 * kprobe_flush_task - 清理任务的 kretprobe 状态（rethook 模式下不需要）
 * @tk: 任务
 *
 * 传统实现: 任务退出时需清理其返回栈中残留的 kretprobe_instance
 * rethook 实现: 自动管理，无需手动清理
 */
#define kprobe_flush_task(tk)	do {} while (0)
#else
void kprobe_flush_task(struct task_struct *tk);
#endif

/*
 * kprobe_free_init_mem - 释放 init 段中的 kprobe
 *
 * 内核 init 完成后调用，移除 __init 函数上的探针
 * （__init 函数的内存即将被释放）
 */
void kprobe_free_init_mem(void);

/*
 * disable_kprobe - 禁用（暂停）kprobe
 * @kp: 要禁用的 kprobe
 * 返回值: 0=成功，负值=失败
 *
 * 探针保持注册状态，但不再触发
 * 可通过 enable_kprobe 重新启用
 */
int disable_kprobe(struct kprobe *kp);

/*
 * enable_kprobe - 启用之前禁用的 kprobe
 * @kp: 要启用的 kprobe
 * 返回值: 0=成功，负值=失败
 */
int enable_kprobe(struct kprobe *kp);

/*
 * dump_kprobe - 打印 kprobe 的调试信息
 * @kp: 要打印的 kprobe
 *
 * 用于调试，输出探针地址、符号、flags 等信息
 */
void dump_kprobe(struct kprobe *kp);

/*
 * alloc_insn_page - 分配普通指令页
 * 返回值: 可执行页的地址，失败返回 NULL
 *
 * 由 kprobe_insn_cache 的 alloc 回调调用
 */
void *alloc_insn_page(void);

/*
 * alloc_optinsn_page - 分配优化指令页
 * 返回值: 可执行页的地址
 */
void *alloc_optinsn_page(void);

/*
 * free_optinsn_page - 释放优化指令页
 * @page: 要释放的页地址
 */
void free_optinsn_page(void *page);

/*
 * kprobe_get_kallsym - 为 kprobe 相关地址生成 kallsym 符号
 * @symnum: 符号序号
 * @value:  输出符号地址
 * @type:   输出符号类型
 * @sym:    输出符号名
 * 返回值: 0=成功，负值=没有更多符号
 *
 * 使 kprobe 指令页在 /proc/kallsyms 中可见
 */
int kprobe_get_kallsym(unsigned int symnum, unsigned long *value, char *type,
		       char *sym);

/*
 * arch_kprobe_get_kallsym - 架构相关的 kallsym 生成
 * @symnum: 符号序号（输入输出）
 * @value:  输出符号地址
 * @type:   输出符号类型
 * @sym:    输出符号名
 * 返回值: 0=成功，负值=没有更多符号
 */
int arch_kprobe_get_kallsym(unsigned int *symnum, unsigned long *value,
			    char *type, char *sym);

/*
 * kprobe_exceptions_notify - kprobe 异常通知链处理器
 * @self: notifier_block
 * @val:  异常类型（DIE_TRAP, DIE_PAGE_FAULT 等）
 * @data: 异常数据（通常是 pt_regs）
 * 返回值: NOTIFY_STOP=已处理，NOTIFY_DONE=未处理
 *
 * 注册到内核异常通知链，捕获断点和页错误等异常
 * 判断是否由 kprobe 触发，若是则调用相应处理器
 */
int kprobe_exceptions_notify(struct notifier_block *self,
			     unsigned long val, void *data);

#else /* !CONFIG_KPROBES: */

/* CONFIG_KPROBES 未启用时的空桩实现 */

static inline int kprobe_fault_handler(struct pt_regs *regs, int trapnr)
{
	return 0;
}
static inline struct kprobe *get_kprobe(void *addr)
{
	return NULL;
}
static inline struct kprobe *kprobe_running(void)
{
	return NULL;
}
#define kprobe_busy_begin()	do {} while (0)
#define kprobe_busy_end()	do {} while (0)

static inline int register_kprobe(struct kprobe *p)
{
	return -EOPNOTSUPP;
}
static inline int register_kprobes(struct kprobe **kps, int num)
{
	return -EOPNOTSUPP;
}
static inline void unregister_kprobe(struct kprobe *p)
{
}
static inline void unregister_kprobes(struct kprobe **kps, int num)
{
}
static inline int register_kretprobe(struct kretprobe *rp)
{
	return -EOPNOTSUPP;
}
static inline int register_kretprobes(struct kretprobe **rps, int num)
{
	return -EOPNOTSUPP;
}
static inline void unregister_kretprobe(struct kretprobe *rp)
{
}
static inline void unregister_kretprobes(struct kretprobe **rps, int num)
{
}
static inline void kprobe_flush_task(struct task_struct *tk)
{
}
static inline void kprobe_free_init_mem(void)
{
}
static inline void kprobe_ftrace_kill(void)
{
}
static inline int disable_kprobe(struct kprobe *kp)
{
	return -EOPNOTSUPP;
}
static inline int enable_kprobe(struct kprobe *kp)
{
	return -EOPNOTSUPP;
}

/*
 * within_kprobe_blacklist - 空桩实现（假设所有地址都在黑名单）
 *
 * 返回 true 确保未启用 kprobes 时不会误注册探针
 */
static inline bool within_kprobe_blacklist(unsigned long addr)
{
	return true;
}

/*
 * kprobe_get_kallsym - 空桩实现
 *
 * 返回 -ERANGE 表示无符号可枚举
 */
static inline int kprobe_get_kallsym(unsigned int symnum, unsigned long *value,
				     char *type, char *sym)
{
	return -ERANGE;
}
#endif /* CONFIG_KPROBES */

/*
 * disable_kretprobe - 禁用返回探针的便捷包装
 * @rp: kretprobe
 * 返回值: 0=成功，负值=失败
 *
 * 内部调用 disable_kprobe(&rp->kp)
 */
static inline int disable_kretprobe(struct kretprobe *rp)
{
	return disable_kprobe(&rp->kp);
}

/*
 * enable_kretprobe - 启用返回探针的便捷包装
 * @rp: kretprobe
 * 返回值: 0=成功，负值=失败
 */
static inline int enable_kretprobe(struct kretprobe *rp)
{
	return enable_kprobe(&rp->kp);
}

#ifndef CONFIG_KPROBES
/*
 * is_kprobe_insn_slot - CONFIG_KPROBES 未启用时的空桩
 * @addr: 地址
 * 返回值: 总是 false
 */
static inline bool is_kprobe_insn_slot(unsigned long addr)
{
	return false;
}
#endif /* !CONFIG_KPROBES */

#ifndef CONFIG_OPTPROBES
/*
 * is_kprobe_optinsn_slot - CONFIG_OPTPROBES 未启用时的空桩
 * @addr: 地址
 * 返回值: 总是 false
 */
static inline bool is_kprobe_optinsn_slot(unsigned long addr)
{
	return false;
}
#endif /* !CONFIG_OPTPROBES */

#ifdef CONFIG_KRETPROBES
#ifdef CONFIG_KRETPROBE_ON_RETHOOK
/*
 * is_kretprobe_trampoline - 检查地址是否是 kretprobe 跳板（rethook 实现）
 * @addr: 要检查的地址
 * 返回值: true 表示该地址是 rethook 跳板
 *
 * rethook 实现下，直接调用 rethook 的 API
 */
static nokprobe_inline bool is_kretprobe_trampoline(unsigned long addr)
{
	return is_rethook_trampoline(addr);
}

/*
 * kretprobe_find_ret_addr - 查找真实返回地址（rethook 实现）
 * @tsk: 目标任务
 * @fp:  帧指针
 * @cur: 当前遍历位置（输入输出）
 * 返回值: 真实返回地址，0 表示未找到
 *
 * 遍历任务的 rethook 返回栈，找到与 fp 匹配的条目
 * 用于栈回溯时恢复被替换的返回地址
 */
static nokprobe_inline
unsigned long kretprobe_find_ret_addr(struct task_struct *tsk, void *fp,
				      struct llist_node **cur)
{
	return rethook_find_ret_addr(tsk, (unsigned long)fp, cur);
}
#else
/* 传统 kretprobe 实现（非 rethook） */

/*
 * is_kretprobe_trampoline - 检查地址是否是 kretprobe 跳板（传统实现）
 * @addr: 要检查的地址
 * 返回值: true 表示该地址是 kretprobe_trampoline
 */
static nokprobe_inline bool is_kretprobe_trampoline(unsigned long addr)
{
	return (void *)addr == kretprobe_trampoline_addr();
}

/*
 * kretprobe_find_ret_addr - 查找真实返回地址（传统实现）
 * @tsk: 目标任务
 * @fp:  帧指针
 * @cur: 当前遍历位置（输入输出）
 * 返回值: 真实返回地址
 *
 * 传统实现需在 kprobes.c 中定义
 */
unsigned long kretprobe_find_ret_addr(struct task_struct *tsk, void *fp,
				      struct llist_node **cur);
#endif
#else
/* CONFIG_KRETPROBES 未启用时的空桩 */

static nokprobe_inline bool is_kretprobe_trampoline(unsigned long addr)
{
	return false;
}

static nokprobe_inline
unsigned long kretprobe_find_ret_addr(struct task_struct *tsk, void *fp,
				      struct llist_node **cur)
{
	return 0;
}
#endif

/* Returns true if kprobes handled the fault */
/*
 * kprobe_page_fault - 处理可能由 kprobe 引起的页错误
 * @regs: CPU 寄存器（包含触发错误的地址）
 * @trap: 陷阱编号
 * 返回值: true 表示是 kprobe 相关的错误且已处理
 *
 * 调用时机: 内核页错误处理器发现错误地址可能与 kprobe 有关时
 *
 * 检查逻辑:
 * 1. CONFIG_KPROBES 未启用 -> false
 * 2. 用户态错误 -> false（kprobe 只在内核态）
 * 3. 抢占已启用 -> false（无法安全调用 kprobe_running）
 * 4. 无 kprobe 正在运行 -> false
 * 5. 调用 kprobe_fault_handler 处理
 *
 * 典型场景:
 * - 单步执行复制的指令时访问无效地址
 * - 探针 handler 内部触发页错误
 *
 * nokprobe_inline: 此函数自身不可被探测（避免递归）
 */
static nokprobe_inline bool kprobe_page_fault(struct pt_regs *regs,
					      unsigned int trap)
{
	if (!IS_ENABLED(CONFIG_KPROBES))
		return false;
	if (user_mode(regs))
		return false;
	/*
	 * To be potentially processing a kprobe fault and to be allowed
	 * to call kprobe_running(), we have to be non-preemptible.
	 */
	/*
	 * 要处理 kprobe 错误并允许调用 kprobe_running()，
	 * 必须处于不可抢占状态（保证 per-CPU 变量访问安全）
	 */
	if (preemptible())
		return false;
	if (!kprobe_running())
		return false;
	return kprobe_fault_handler(regs, trap);
}

#endif /* _LINUX_KPROBES_H */
