/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Context Tracking 状态布局学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件定义 Context Tracking 的“数据面”：上下文枚举、每 CPU 状态对象、
 * state 原子量的位布局，以及供热路径读取这些状态的内联接口。
 * 状态转换策略
 * 位于 kernel/context_tracking.c，面向调用者的入口位于 context_tracking.h。
 *
 * 每个 possible CPU 都有一个长期存在的 context_tracking 实例，不需要引用
 * 计数或释放。低位 ctx_state 描述 KERNEL/IDLE/USER/GUEST，高位 watching
 * 子变量既用奇偶表示 RCU 是否观察该 CPU，又作为转换序列供
 * 远端采样者判断
 * CPU 是否跨越过扩展静止态。当前 CPU 的读者依靠禁止抢占/中断保持 CPU
 * 身份；远端读者根据用途选择普通原子读或 acquire 读。
 *
 * static key 让功能未启用时的入口判断退化为近乎零成本分支；每 CPU active
 * 再表示该 CPU 是否实际参加 full dynticks。收益是降低普通配置热路径成本，
 * 代价是状态位复用和严格的 enter/exit 配对增加了理解与维护复杂度。
 */
#ifndef _LINUX_CONTEXT_TRACKING_STATE_H
#define _LINUX_CONTEXT_TRACKING_STATE_H

#include <linux/percpu.h>
#include <linux/static_key.h>
#include <linux/context_tracking_irq.h>

/* Offset to allow distinguishing irq vs. task-based idle entry/exit. */
/*
 * 该大偏移用于区分 IRQ/NMI 嵌套记账与任务/idle 嵌套记账。选择半个 LONG_MAX
 * 以上的区域，使两类计数在正常嵌套深度下不会混淆；
 * 它是状态编码，不是时间。
 */
#define CT_NESTING_IRQ_NONIDLE	((LONG_MAX / 2) + 1)

/*
 * ctx_state 描述当前 CPU 在 Context Tracking 视角下执行哪类上下文。
 *
 * DISABLED 是查询接口无法给出状态时的返回哨兵；KERNEL 为零，使许多“无需
 * 恢复”路径可返回 0；IDLE/USER/GUEST 是可被 RCU 视为扩展静止态的来源。
 * MAX 只用于计算位宽，不是可发布的运行状态。
 */
enum ctx_state {
	CT_STATE_DISABLED	= -1,	/* returned by ct_state() if unknown */
	/* ct_state() 无法确定状态时返回该值，不能当作真实 CPU 上下文。 */
	CT_STATE_KERNEL		= 0,
	CT_STATE_IDLE		= 1,
	CT_STATE_USER		= 2,
	CT_STATE_GUEST		= 3,
	CT_STATE_MAX		= 4,
};

/*
 * struct context_tracking - 一个 CPU 的 Context Tracking 状态容器。
 *
 * 对象由 DEFINE_PER_CPU() 静态创建，CPU 本地入口直接借用，不转移所有权，
 * 生命周期覆盖整个内核运行期。active/recursion 服务 USER/GUEST 跟踪；
 * state 同时承载上下文与 RCU watching 序列；nesting/nmi_nesting 维护任务
 * 和 IRQ/NMI 两套嵌套协议。字段主要由本 CPU 在禁抢占或关中断环境写入，
 * state 使用原子操作与远端 RCU 采样者同步。
 */
struct context_tracking {
#ifdef CONFIG_CONTEXT_TRACKING_USER
	/*
	 * When active is false, probes are unset in order
	 * to minimize overhead: TIF flags are cleared
	 * and calls to user_enter/exit are ignored. This
	 * may be further optimized using static keys.
	 */
	/*
	 * active 为 false 时会撤除探针以降低开销，清除 TIF 标志，
	 * user_enter/exit 的昂贵工作也会跳过，并可继续由 static key 优化。
	 * active 是“本 CPU 是否 full-dynticks 跟踪”的策略位；recursion 是本 CPU
	 * 防止 tracing/记账重入状态机的深度计数，
	 * 只有最外层调用可真正转换状态。
	 */
	bool active;
	int recursion;
#endif
#ifdef CONFIG_CONTEXT_TRACKING
	/*
	 * state 是跨 NMI/RCU 采样共享的组合原子量。
	 * 任何修改都必须保持下面的位域协议，
	 * 不能把它拆成两个无序字段，
	 * 否则远端可能看到不一致快照。
	 */
	atomic_t state;
#endif
#ifdef CONFIG_CONTEXT_TRACKING_IDLE
	long nesting;		/* Track process nesting level. */
	long nmi_nesting;	/* Track irq/NMI nesting level. */
	/*
	 * nesting 跟踪任务/idle 边界，nmi_nesting 跟踪 IRQ/NMI 边界。
	 * 两个原英文行尾注释分别意为“进程嵌套层级”和
	 * “IRQ/NMI 嵌套层级”；
	 * 特殊值 CT_NESTING_IRQ_NONIDLE 表示 CPU 原本不在 RCU idle。
	 */
#endif
};

/*
 * We cram two different things within the same atomic variable:
 *
 *                     CT_RCU_WATCHING_START  CT_STATE_START
 *                                |                |
 *                                v                v
 *     MSB [ RCU watching counter ][ context_state ] LSB
 *         ^                       ^
 *         |                       |
 * CT_RCU_WATCHING_END        CT_STATE_END
 *
 * Bits are used from the LSB upwards, so unused bits (if any) will always be in
 * upper bits of the variable.
 */
/*
 * 原图说明 state 中压入了两类信息：从最低位向上先是 context_state，
 * 再是 RCU watching 计数；若原子类型还有剩余位，它们永远位于最高端。
 *
 * 这样一次原子 read-modify-write 就能同时发布“进入哪个上下文”和“RCU
 * watching 序列跨过一个边界”，避免两个字段被远端观察成不同步状态。
 * watching 区不是简单布尔值：最低 watching 位的奇偶给出当前真假，完整
 * 子变量还让 RCU 判断采样期间是否发生过一次或多次 EQS 往返。
 */
#ifdef CONFIG_CONTEXT_TRACKING
/* CT_SIZE 是 state 原子量的总位数，后续所有区间都以位为单位。 */
#define CT_SIZE (sizeof(((struct context_tracking *)0)->state) * BITS_PER_BYTE)

/* ctx_state 从 bit 0 起，占下容纳最大真实状态所需的最小位宽。 */
#define CT_STATE_WIDTH bits_per(CT_STATE_MAX - 1)
#define CT_STATE_START 0
#define CT_STATE_END   (CT_STATE_START + CT_STATE_WIDTH - 1)

#define CT_RCU_WATCHING_MAX_WIDTH (CT_SIZE - CT_STATE_WIDTH)
/*
 * torture 配置故意把 watching 区压缩为两位，以频繁制造计数回绕；
 * 正常配置使用所有剩余位，降低远端采样跨整圈而误判的概率。
 */
#define CT_RCU_WATCHING_WIDTH     (IS_ENABLED(CONFIG_RCU_DYNTICKS_TORTURE) ? 2 : CT_RCU_WATCHING_MAX_WIDTH)
#define CT_RCU_WATCHING_START     (CT_STATE_END + 1)
#define CT_RCU_WATCHING_END       (CT_RCU_WATCHING_START + CT_RCU_WATCHING_WIDTH - 1)
#define CT_RCU_WATCHING           BIT(CT_RCU_WATCHING_START)

/* 两个掩码分别提取低位上下文和高位 watching 子变量。 */
#define CT_STATE_MASK        GENMASK(CT_STATE_END,        CT_STATE_START)
#define CT_RCU_WATCHING_MASK GENMASK(CT_RCU_WATCHING_END, CT_RCU_WATCHING_START)

#define CT_UNUSED_WIDTH (CT_RCU_WATCHING_MAX_WIDTH - CT_RCU_WATCHING_WIDTH)

/*
 * 编译期断言证明三个区段恰好覆盖 atomic_t：布局变化若造成重叠或遗漏，
 * 构建立即失败，而不是把错误推迟到难以复现的 RCU 竞态。
 */
static_assert(CT_STATE_WIDTH        +
	      CT_RCU_WATCHING_WIDTH +
	      CT_UNUSED_WIDTH       ==
	      CT_SIZE);

/*
 * 声明每 CPU 对象；存储由 kernel/context_tracking.c 唯一定义并初始化。
 * 使用者得到的是 CPU 本地借用地址，不需要也不能自行释放。
 */
DECLARE_PER_CPU(struct context_tracking, context_tracking);
#endif	/* CONFIG_CONTEXT_TRACKING */
/* 以上布局仅在存在实际 Context Tracking state 时编译。 */

#ifdef CONFIG_CONTEXT_TRACKING_USER
/*
 * __ct_state() - 无同步地读取当前 CPU 的上下文低位。
 *
 * 入参：无。调用者必须已经固定 CPU 身份，通常位于 IRQ-off/noinstr 路径；
 * 返回 KERNEL/IDLE/USER/GUEST 之一。raw 原子读不提供 acquire 顺序，也不
 * 取得引用；该接口面向已经满足入口协议的内部热路径，
 * 不应作为通用调试查询。
 */
static __always_inline int __ct_state(void)
{
	return raw_atomic_read(this_cpu_ptr(&context_tracking.state)) & CT_STATE_MASK;
}
#endif

#ifdef CONFIG_CONTEXT_TRACKING_IDLE
/*
 * ct_rcu_watching() - 读取当前 CPU 的完整 watching 子变量。
 *
 * 入参：无。调用者负责固定 CPU；返回值保留序列位而非规范化 bool，
 * 用于比较/跟踪转换历史。普通 atomic_read 不提供跨 CPU acquire 保证。
 */
static __always_inline int ct_rcu_watching(void)
{
	return atomic_read(this_cpu_ptr(&context_tracking.state)) & CT_RCU_WATCHING_MASK;
}

/*
 * ct_rcu_watching_cpu() - 普通采样指定 CPU 的 watching 子变量。
 *
 * @cpu: 纯输入逻辑 CPU 号，对象是长期 per-CPU 存储，无 ownership 转移。
 * 返回该 CPU 的即时序列快照；不提供 acquire 排序，
 * 适合只判断计数本身的路径。
 */
static __always_inline int ct_rcu_watching_cpu(int cpu)
{
	/* ct 是目标 CPU 长期对象的借用指针，只在本次读取中使用。 */
	struct context_tracking *ct = per_cpu_ptr(&context_tracking, cpu);

	return atomic_read(&ct->state) & CT_RCU_WATCHING_MASK;
}

/*
 * ct_rcu_watching_cpu_acquire() - 以 acquire 语义采样远端 CPU 的 watching。
 *
 * @cpu: 纯输入逻辑 CPU 号，无引用和 ownership 变化。
 * 返回 watching 子变量；acquire 与状态转换侧的有序原子更新配合，使调用者
 * 在确认序列后再观察相关先行操作。它只排序内存访问，
 * 不冻结远端 CPU 状态。
 */
static __always_inline int ct_rcu_watching_cpu_acquire(int cpu)
{
	/*
	 * ct 是目标 per-CPU 对象的借用指针，
	 * 生命周期覆盖整个系统运行期。
	 */
	struct context_tracking *ct = per_cpu_ptr(&context_tracking, cpu);

	return atomic_read_acquire(&ct->state) & CT_RCU_WATCHING_MASK;
}

/*
 * 下列四个 getter 分别读取当前/指定 CPU 的任务嵌套和 NMI 嵌套值。
 * 当前 CPU 版本要求调用者固定 CPU；远端版本只是瞬时无序快照，
 * 调用者必须
 * 接受并发变化。均无参数输出、引用或副作用，不应把返回值当作锁。
 */
static __always_inline long ct_nesting(void)
{
	return __this_cpu_read(context_tracking.nesting);
}

static __always_inline long ct_nesting_cpu(int cpu)
{
	/* @cpu 是纯输入逻辑 CPU 号；ct 为长期 per-CPU 对象的借用指针。 */
	struct context_tracking *ct = per_cpu_ptr(&context_tracking, cpu);

	return ct->nesting;
}

static __always_inline long ct_nmi_nesting(void)
{
	return __this_cpu_read(context_tracking.nmi_nesting);
}

static __always_inline long ct_nmi_nesting_cpu(int cpu)
{
	/* @cpu 含义同 ct_nesting_cpu()，返回 IRQ/NMI 嵌套的即时快照。 */
	struct context_tracking *ct = per_cpu_ptr(&context_tracking, cpu);

	return ct->nmi_nesting;
}
#endif /* #ifdef CONFIG_CONTEXT_TRACKING_IDLE */
/* 上述接口只在 idle/IRQ 嵌套状态存在时提供。 */

#ifdef CONFIG_CONTEXT_TRACKING_USER
/*
 * context_tracking_key 是全局只读 static-key 门控。初始化代码只增加其启用
 * 计数；热路径通过 jump-label 判断是否至少有一个 CPU 需要 USER 跟踪。
 */
extern struct static_key_false context_tracking_key;

/*
 * context_tracking_enabled() - 判断全局是否启用了 USER Context Tracking。
 *
 * 入参：无。返回 static key 的布尔结果；不代表当前 CPU active，不提供状态
 * 顺序或对象生命周期保证。未启用时分支被修补为低成本直通。
 */
static __always_inline bool context_tracking_enabled(void)
{
	return static_branch_unlikely(&context_tracking_key);
}

/*
 * context_tracking_enabled_cpu() - 同时检查全局 key 和指定 CPU active 位。
 *
 * @cpu: 纯输入逻辑 CPU 号。返回 true 表示该 CPU 实际参与 full-dynticks
 * USER 跟踪；false 仍可能需要维护跨 CPU 可恢复的 context_state。
 */
static __always_inline bool context_tracking_enabled_cpu(int cpu)
{
	return context_tracking_enabled() && per_cpu(context_tracking.active, cpu);
}

/*
 * context_tracking_enabled_this_cpu() - 检查当前 CPU 是否实际 active。
 *
 * 入参：无。调用者应固定 CPU；返回全局 key 与本 CPU active 的合取结果。
 * 只读状态、无 ownership 变化，也不承诺后续迁移后的 CPU 仍有相同策略。
 */
static __always_inline bool context_tracking_enabled_this_cpu(void)
{
	return context_tracking_enabled() && __this_cpu_read(context_tracking.active);
}

/**
 * ct_state() - return the current context tracking state if known
 *
 * Returns the current cpu's context tracking state if context tracking
 * is enabled.  If context tracking is disabled, returns
 * CT_STATE_DISABLED.  This should be used primarily for debugging.
 */
/*
 * 返回当前 CPU 的 Context Tracking 状态；若全局未启用则返回
 * CT_STATE_DISABLED。原文特别强调该接口主要用于调试，不是同步原语。
 *
 * 入参：无。函数临时禁止抢占以保证 __ct_state() 前后仍在同一 CPU；
 * 返回已知 ctx_state 或 DISABLED，不取得引用。恢复抢占时可能触发调度，
 * 因而不适合要求始终不可调度的底层入口。
 */
static __always_inline int ct_state(void)
{
	/* ret 只保存禁抢占窗口内取得的按值状态快照。 */
	int ret;

	if (!context_tracking_enabled())
		return CT_STATE_DISABLED;

	preempt_disable();
	ret = __ct_state();
	preempt_enable();

	return ret;
}

#else
/*
 * CONFIG_CONTEXT_TRACKING_USER=n 时的全局、指定 CPU、本 CPU 三个查询 stub。
 * 各自保持原参数形状，但均返回 false、无副作用，
 * 也不读取 @cpu 对应存储。
 */
static __always_inline bool context_tracking_enabled(void) { return false; }
static __always_inline bool context_tracking_enabled_cpu(int cpu) { return false; }
static __always_inline bool context_tracking_enabled_this_cpu(void) { return false; }
#endif /* CONFIG_CONTEXT_TRACKING_USER */
/* 上述分支把未配置 USER 跟踪的调用点编译为常量 false。 */

#endif
