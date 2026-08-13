/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kernel/lockdep_internals.h
 *
 * Runtime locking correctness validator
 *
 * lockdep subsystem internal functions and variables.
 */
/*
 * 内核运行时锁正确性验证器的内部契约：本头文件集中定义锁类 usage 位编码、
 * 依赖图静态池容量、跨文件统计接口和调试统计 helper。它不是公开 API；
 * `lockdep.c` 负责状态更新，`lockdep_proc.c` 等观察者按这里的同一布局读取。
 * 文件本身没有 include guard，当前两个使用者都在各自翻译单元中只包含一次；
 * 新增包含点也必须保持这一约束，避免重复定义枚举、结构体和内联函数。
 */

/*
 * Lock-class usage-state bits:
 */
/*
 * 锁类 usage 状态的“位编号”。每个 lockdep IRQ 状态连续生成四项：
 * 在该上下文写持有、在该上下文读持有、该上下文开启时写持有、该上下文
 * 开启时读持有。顺序使低两位天然编码 read 和方向，供后面的掩码位移使用。
 */
enum lock_usage_bit {
#define LOCKDEP_STATE(__STATE)		\
	LOCK_USED_IN_##__STATE,		\
	LOCK_USED_IN_##__STATE##_READ,	\
	LOCK_ENABLED_##__STATE,		\
	LOCK_ENABLED_##__STATE##_READ,
#include "lockdep_states.h"
#undef LOCKDEP_STATE
	LOCK_USED,
	LOCK_USED_READ,
	LOCK_USAGE_STATES,
};

/*
 * X-macro 的 __STATE 是 HARDIRQ/SOFTIRQ 标识符，只在预处理期参与拼接；
 * `LOCK_USED`/`LOCK_USED_READ` 记录不依赖 IRQ 状态的首次写/读使用，最后的
 * `LOCK_USAGE_STATES` 是位数哨兵，不代表可设置的 usage 位。
 */

/* states after LOCK_USED_READ are not traced and printed */
/*
 * `LOCK_USED_READ` 之后的项不保存 trace、也不打印；当前其后只有数量哨兵。
 * 公共 `lock_class.usage_traces[]` 容量必须与这里真实可跟踪的位数完全相等，
 * 静态断言可在 `lockdep_states.h` 行数与 XXX 常量失配时阻止构建。
 */
static_assert(LOCK_TRACE_STATES == LOCK_USAGE_STATES);

#define LOCK_USAGE_READ_MASK 1
#define LOCK_USAGE_DIR_MASK  2
#define LOCK_USAGE_STATE_MASK (~(LOCK_USAGE_READ_MASK | LOCK_USAGE_DIR_MASK))

/*
 * 位编号编码：bit0=0 表示写/独占、1 表示读；bit1=0 表示在 IRQ 上下文使用、
 * 1 表示 IRQ 开启时使用；其余高位选择具体 IRQ 状态。这里的常量是位编号
 * 偏移/掩码，不是 usage_mask 中的单 bit 标志；左右移 1 或 2 正是切换相邻
 * read/方向伙伴，`LOCK_USAGE_STATE_MASK` 则保留状态编号部分。
 */

/*
 * Usage-state bitmasks:
 */
/* 把上一枚举中的“位编号”变成可与 lock_class.usage_mask 运算的单 bit 掩码。 */
#define __LOCKF(__STATE)	LOCKF_##__STATE = (1 << LOCK_##__STATE),

enum {
#define LOCKDEP_STATE(__STATE)						\
	__LOCKF(USED_IN_##__STATE)					\
	__LOCKF(USED_IN_##__STATE##_READ)				\
	__LOCKF(ENABLED_##__STATE)					\
	__LOCKF(ENABLED_##__STATE##_READ)
#include "lockdep_states.h"
#undef LOCKDEP_STATE
	__LOCKF(USED)
	__LOCKF(USED_READ)
};

/*
 * 第二次包含状态表，为每个 LOCK_* 位编号生成对应 LOCKF_* 掩码；末尾两项
 * 覆盖一般首次使用。当前位数可装入 enum 的 int 表达式，usage_mask 则以
 * unsigned long 保存组合结果。
 */

enum {
#define LOCKDEP_STATE(__STATE)	LOCKF_ENABLED_##__STATE |
	LOCKF_ENABLED_IRQ =
#include "lockdep_states.h"
	0,
#undef LOCKDEP_STATE

#define LOCKDEP_STATE(__STATE)	LOCKF_USED_IN_##__STATE |
	LOCKF_USED_IN_IRQ =
#include "lockdep_states.h"
	0,
#undef LOCKDEP_STATE

#define LOCKDEP_STATE(__STATE)	LOCKF_ENABLED_##__STATE##_READ |
	LOCKF_ENABLED_IRQ_READ =
#include "lockdep_states.h"
	0,
#undef LOCKDEP_STATE

#define LOCKDEP_STATE(__STATE)	LOCKF_USED_IN_##__STATE##_READ |
	LOCKF_USED_IN_IRQ_READ =
#include "lockdep_states.h"
	0,
#undef LOCKDEP_STATE
};

/*
 * 后四次 X-macro 展开分别 OR 出所有 IRQ 状态的 ENABLED-write、USED_IN-write、
 * ENABLED-read 和 USED_IN-read 集合。尾部 `0` 结束跨 include 的 OR 表达式；
 * 这些聚合值让冲突检测一次筛出全部 HARDIRQ/SOFTIRQ 对应位。
 */

#define LOCKF_ENABLED_IRQ_ALL (LOCKF_ENABLED_IRQ | LOCKF_ENABLED_IRQ_READ)
#define LOCKF_USED_IN_IRQ_ALL (LOCKF_USED_IN_IRQ | LOCKF_USED_IN_IRQ_READ)

#define LOCKF_IRQ (LOCKF_ENABLED_IRQ | LOCKF_USED_IN_IRQ)
#define LOCKF_IRQ_READ (LOCKF_ENABLED_IRQ_READ | LOCKF_USED_IN_IRQ_READ)

/*
 * `_ALL` 按方向合并读写，供 invert_dir_mask() 在 USED_IN 与 ENABLED 间移动；
 * LOCKF_IRQ/LOCKF_IRQ_READ 按访问类型合并两个方向，供 exclusive_mask() 在
 * 写与读伙伴之间扩展潜在死锁集合。它们只描述位集合，不改变锁类状态。
 */

/*
 * CONFIG_LOCKDEP_SMALL is defined for sparc. Sparc requires .text,
 * .data and .bss to fit in required 32MB limit for the kernel. With
 * CONFIG_LOCKDEP we could go over this limit and cause system boot-up problems.
 * So, reduce the static allocations for lockdeps related structures so that
 * everything fits in current required size limit.
 */
/*
 * CONFIG_LOCKDEP_SMALL 由 sparc 在启用 LOCKDEP 时选择。sparc 要求内核
 * .text、.data 和 .bss 装入 32MB；常规 lockdep 静态池可能越界并妨碍启动，
 * 因此该配置使用固定的小容量。缩容只降低可记录规模，不改变依赖检查语义；
 * 池耗尽时 lockdep 会报告容量不足并停止可靠验证。
 */
#ifdef CONFIG_LOCKDEP_SMALL
/*
 * MAX_LOCKDEP_ENTRIES is the maximum number of lock dependencies
 * we track.
 *
 * We use the per-lock dependency maps in two ways: we grow it by adding
 * every to-be-taken lock to all currently held lock's own dependency
 * table (if it's not there yet), and we check it for lock order
 * conflicts and deadlocks.
 */
/*
 * MAX_LOCKDEP_ENTRIES 是可跟踪的直接锁依赖边上限。每次准备获取新锁时，
 * lockdep 把它加入当前各持锁类的依赖表（尚不存在时），之后查询这些表以
 * 发现锁序冲突和死锁。small 配置还固定 chain 位数、压缩栈地址池项数及
 * 栈 trace 哈希桶数，限制相应 `.bss` 静态数组占用。
 */
#define MAX_LOCKDEP_ENTRIES	16384UL
#define MAX_LOCKDEP_CHAINS_BITS	15
#define MAX_STACK_TRACE_ENTRIES	262144UL
#define STACK_TRACE_HASH_SIZE	8192
#else
#define MAX_LOCKDEP_ENTRIES	(1UL << CONFIG_LOCKDEP_BITS)

/* 常规构建由 Kconfig 指数决定直接依赖边数量；数值单位是 `struct lock_list` 项。 */

#define MAX_LOCKDEP_CHAINS_BITS	CONFIG_LOCKDEP_CHAINS_BITS

/* 依赖链池大小稍后按 2 的该指数次幂计算，配置调节容量与静态内存占用。 */

/*
 * Stack-trace: tightly packed array of stack backtrace
 * addresses. Protected by the hash_lock.
 */
/*
 * stack-trace 是紧密排列的调用栈地址数组，由 hash_lock 保护分配和去重。
 * 两个 Kconfig 指数分别决定地址池项数与哈希桶数；引用 trace 不能只靠
 * hash_lock 延长生命周期，但这里的静态池随内核同生命周期，不会单独释放。
 */
#define MAX_STACK_TRACE_ENTRIES	(1UL << CONFIG_LOCKDEP_STACK_TRACE_BITS)
#define STACK_TRACE_HASH_SIZE	(1 << CONFIG_LOCKDEP_STACK_TRACE_HASH_BITS)
#endif

/*
 * Bit definitions for lock_chain.irq_context
 */
/*
 * lock_chain.irq_context 的两位编码：bit0 表示链处于 softirq 上下文，bit1
 * 表示 hardirq 上下文；两位同时置位代表链跨越/包含两种中断上下文组合。
 */
#define LOCK_CHAIN_SOFTIRQ_CONTEXT	(1 << 0)
#define LOCK_CHAIN_HARDIRQ_CONTEXT	(1 << 1)

#define MAX_LOCKDEP_CHAINS	(1UL << MAX_LOCKDEP_CHAINS_BITS)

/* lock_chains[] 与其 in-use bitmap 的元素上限，单位为已缓存依赖链。 */

#define AVG_LOCKDEP_CHAIN_DEPTH		5
#define MAX_LOCKDEP_CHAIN_HLOCKS (MAX_LOCKDEP_CHAINS * AVG_LOCKDEP_CHAIN_DEPTH)

/*
 * 以每条链平均 5 个 held-lock 编号估算共享 chain_hlocks[] 池容量。5 是静态
 * 容量系数，不限制单条链深度；可变长块分配器负责复用空间，池耗尽会触发
 * “MAX_LOCKDEP_CHAIN_HLOCKS too low” 并使验证器失去完整覆盖。
 */

extern struct lock_chain lock_chains[];

/* lockdep.c 定义的全局链对象池；调用者借用元素，索引/分配受内部图锁协议保护。 */

#define LOCK_USAGE_CHARS (2*XXX_LOCK_USAGE_STATES + 1)

/* 每种 IRQ 状态输出写/读两个字符，最后额外保留一个 NUL 终止符。 */

/*
 * get_usage_chars() - 把锁类 IRQ usage 位格式化为紧凑字符数组
 *
 * 调用链：lockdep/proc 打印 → 本函数 → 针对每个状态的写/读位调用内部
 * get_usage_char()。@class 是有效锁类的借用输入指针，调用期间必须保持稳定；
 * @usage 是调用者拥有的输出数组，容量必须为 LOCK_USAGE_CHARS。
 * 函数不取得引用、不睡眠，返回：无直接返回值；成功后写满每状态两个字符并
 * 追加 NUL。字符表达 IRQ-safe/unsafe 组合，只读 usage_mask，不修改锁类。
 */
extern void get_usage_chars(struct lock_class *class,
			    char usage[LOCK_USAGE_CHARS]);

/*
 * __get_key_name() - 尝试把锁类 subclass key 地址解析为符号名
 *
 * @key 是借用的输入 key，不可为 NULL；@str 是调用者提供的输出缓冲区，
 * 应具备 KSYM_NAME_LEN 容量。内部调用 kallsyms_lookup()，不转移 ownership；
 * 成功返回指向名称文本的借用指针，查找失败返回 NULL。调用者随后用于诊断
 * 输出，不能把返回指针当作新分配对象释放。
 */
extern const char *__get_key_name(const struct lockdep_subclass_key *key,
				  char *str);

/*
 * lock_chain_get_class() - 取得依赖链第 i 个 held-lock 所属的锁类
 *
 * @chain 是稳定链对象的借用输入；@i 是从 0 开始且必须小于 chain->depth 的
 * 索引。函数从 chain_hlocks[base+i] 解码 class_idx，返回 lock_classes[] 中的
 * 借用指针，不会返回错误或增加引用；调用者必须通过图遍历协议保证链槽未被
 * 并发回收。函数不睡眠、无可观察副作用，通常由 /proc/lockdep_chains 打印继续。
 */
struct lock_class *lock_chain_get_class(struct lock_chain *chain, int i);

/*
 * 以下第一组统计由 lockdep.c 的静态池分配/回收路径维护：
 *   nr_lock_classes        当前占用的锁类数量；
 *   nr_zapped_classes      累计摘除并等待/完成回收的锁类次数；
 *   nr_zapped_lock_chains  累计从哈希表摘除的依赖链次数；
 *   nr_list_entries        当前占用的直接依赖边条目数；
 *   nr_dynamic_keys        当前已注册动态 key 数。
 * 读取者只借用数值作诊断，不能据此取得池元素生命周期；并发读取不保证快照。
 */
extern unsigned long nr_lock_classes;
extern unsigned long nr_zapped_classes;
extern unsigned long nr_zapped_lock_chains;
extern unsigned long nr_list_entries;
extern unsigned long nr_dynamic_keys;

/*
 * lockdep_next_lockchain() - 在 in-use bitmap 中寻找下一条依赖链
 *
 * @i 是起始索引，范围允许 -1；搜索从 i+1 开始。找到时返回 [0,
 * MAX_LOCKDEP_CHAINS) 的索引，没有下一项返回 -2。它不取得链引用，调用者
 * 需按 lockdep proc/图遍历规则保证 bitmap 稳定；不睡眠、无状态副作用。
 */
long lockdep_next_lockchain(long i);

/*
 * lock_chain_count() - 统计当前 in-use 依赖链 bitmap 的置位数
 *
 * 入参：无；返回当前观察到的链数量。函数只扫描 bitmap，不取得引用、不
 * 睡眠；若分配/回收并发发生，返回值是诊断性观察而非事务快照。
 */
unsigned long lock_chain_count(void);

/* 已使用的紧密栈 trace 池项数；只表示池占用，单位是 unsigned long 槽。 */
extern unsigned long nr_stack_trace_entries;

/*
 * 当前缓存链按最深上下文分类的数量：hardirq、softirq 和进程上下文；
 * 创建/删除链时成对增减，proc 读取可能与更新并发，不承担同步职责。
 */
extern unsigned int nr_hardirq_chains;
extern unsigned int nr_softirq_chains;
extern unsigned int nr_process_chains;

/*
 * chain_hlocks 可变长块分配器状态：free 是当前可分配槽数，lost 是因碎片/块
 * 太小而永久无法重挂空闲表的累计槽数，large 是当前超过固定 bucket 范围的
 * 空闲块数量。三者单位/含义不同，不能简单都当作“已使用数”。
 */
extern unsigned int nr_free_chain_hlocks;
extern unsigned int nr_lost_chain_hlocks;
extern unsigned int nr_large_chain_blocks;

/*
 * 运行期历史峰值：任务持锁栈最大深度、依赖 BFS 队列最大占用，以及曾使用
 * 的最大 lock_classes[] 下标。它们用于容量诊断，不给读取者提供对象引用。
 */
extern unsigned int max_lockdep_depth;
extern unsigned int max_bfs_queue_depth;
extern unsigned long max_lock_class_idx;

/*
 * 全局锁类静态池及其占用 bitmap。元素由 lockdep.c 发布、摘除并经 RCU
 * 延迟复用；外部读者只能按图锁/RCU/既有 proc 遍历协议借用，不能释放。
 */
extern struct lock_class lock_classes[MAX_LOCKDEP_KEYS];
extern unsigned long lock_classes_in_use[];

#ifdef CONFIG_PROVE_LOCKING
/*
 * lockdep_count_forward_deps() - 统计从一个锁类可到达的后继依赖
 *
 * @class 是借用的有效起点，不可为 NULL。实现关闭本地 IRQ、取得 lockdep
 * 图锁并执行向前 BFS，不睡眠；返回遍历到的依赖节点数。调用期间不转移
 * ownership，BFS 只读图，但可能在内部容量异常时告警，结果用于 proc 诊断。
 */
extern unsigned long lockdep_count_forward_deps(struct lock_class *);

/*
 * lockdep_count_backward_deps() - 统计可到达一个锁类的前驱依赖
 *
 * @class 契约与 forward 版本相同；实现关闭本地 IRQ并持图锁执行反向 BFS，
 * 返回前驱节点数，无引用转移、不会睡眠，调用者下一步通常只格式化该统计值。
 */
extern unsigned long lockdep_count_backward_deps(struct lock_class *);
#ifdef CONFIG_TRACE_IRQFLAGS
/*
 * lockdep_stack_trace_count() - 返回当前保存的 lock_trace 对象数量
 *
 * 入参：无；返回 u64 诊断计数，不转移 trace ownership，不承诺并发原子快照。
 */
u64 lockdep_stack_trace_count(void);

/*
 * lockdep_stack_hash_count() - 返回 stack trace 哈希链中的对象数量
 *
 * 入参：无；返回 u64 诊断计数，用于与 trace 数/池占用交叉检查；无副作用。
 */
u64 lockdep_stack_hash_count(void);
#endif
#else
/*
 * lockdep_count_forward_deps() - 关闭依赖证明时的零值占位实现
 *
 * @class 是为保持调用接口一致而接收的借用输入，本实现不解引用、不保存；
 * 不持锁、不睡眠，恒返回 0，明确表示此配置没有执行前向依赖证明统计。
 */
static inline unsigned long
lockdep_count_forward_deps(struct lock_class *class)
{
	return 0;
}

/*
 * lockdep_count_backward_deps() - 关闭依赖证明时的零值占位实现
 *
 * @class 同样仅保持接口一致，不解引用且 ownership 不变；不持锁、不睡眠，
 * 恒返回 0，调用者可无条件编译同一 proc 展示逻辑。
 */
static inline unsigned long
lockdep_count_backward_deps(struct lock_class *class)
{
	return 0;
}
#endif

#ifdef CONFIG_DEBUG_LOCKDEP

#include <asm/local.h>
/*
 * Various lockdep statistics.
 * We want them per cpu as they are often accessed in fast path
 * and we want to avoid too much cache bouncing.
 */
/*
 * 各类 lockdep 调试统计。它们在快速路径频繁访问，所以按 CPU 保存以减少
 * cacheline 抖动；proc 展示时才跨 CPU 求和。统计不是验证状态本身，允许
 * 非快照读取和少量丢样本，删除统计也不应改变 lockdep 判错逻辑。
 *
 * 生命周期：lockdep.c 定义每 CPU 实例，随内核同生命周期；写者只改当前
 * CPU 槽，读者借用并汇总，不转移 ownership。
 */
struct lockdep_stats {
	/* 依赖链哈希缓存命中/未命中次数，用于衡量跳过完整验证的效果。 */
	unsigned long  chain_lookup_hits;
	unsigned int   chain_lookup_misses;
	/* hardirq 软件跟踪状态实际 ON/OFF 转换次数，以及重复请求同一状态的次数。 */
	unsigned long  hardirqs_on_events;
	unsigned long  hardirqs_off_events;
	unsigned long  redundant_hardirqs_on;
	unsigned long  redundant_hardirqs_off;
	/* softirq 对应的实际 ON/OFF 转换与冗余转换次数。 */
	unsigned long  softirqs_on_events;
	unsigned long  softirqs_off_events;
	unsigned long  redundant_softirqs_on;
	unsigned long  redundant_softirqs_off;
	/*
	 * nr_unused_locks 是当前已注册但尚未使用的类的净值；其余字段分别记录
	 * 冗余依赖检查、确认冗余、环检测及 usage 正向/反向搜索的累计次数。
	 */
	int            nr_unused_locks;
	unsigned int   nr_redundant_checks;
	unsigned int   nr_redundant;
	unsigned int   nr_cyclic_checks;
	unsigned int   nr_find_usage_forwards_checks;
	unsigned int   nr_find_usage_backwards_checks;

	/*
	 * Per lock class locking operation stat counts
	 */
	/*
	 * 每个锁类的获取操作次数。下标与 lock_classes[] 完全一致；每 CPU 数组
	 * 避免所有锁获取争用同一全局计数，类回收/复用时由既有 lockdep 协议约束。
	 */
	unsigned long lock_class_ops[MAX_LOCKDEP_KEYS];
};

/* 每 CPU 调试统计实例的声明；实际定义位于 lockdep.c。 */
DECLARE_PER_CPU(struct lockdep_stats, lockdep_stats);

#define __debug_atomic_inc(ptr)					\
	this_cpu_inc(lockdep_stats.ptr);

/*
 * `ptr` 名称容易误导：它实际必须是 lockdep_stats 的字段 token，不是 C 指针。
 * __debug_atomic_inc() 用 checked `this_cpu_inc()`，供无法保证 IRQ/抢占已关闭的
 * 少数统计路径使用；只更新当前 CPU，尾随分号已包含在宏展开中。
 */

#define debug_atomic_inc(ptr)			{		\
	WARN_ON_ONCE(!irqs_disabled());				\
	__this_cpu_inc(lockdep_stats.ptr);			\
}

/*
 * debug_atomic_inc(field) 先一次性告警检查本地 IRQ 必须关闭，再用
 * `__this_cpu_inc()` 更新当前 CPU 字段。IRQ 关闭防止同 CPU 中断路径穿插
 * 非原子读改写；宏不加 lockdep 图锁，也不保证跨 CPU 可见性的同步时刻。
 */

#define debug_atomic_dec(ptr)			{		\
	WARN_ON_ONCE(!irqs_disabled());				\
	__this_cpu_dec(lockdep_stats.ptr);			\
}

/* decrement 版本遵守同一 IRQ 前置条件，用于未使用类等可增可减的净值统计。 */

#define debug_atomic_read(ptr)		({				\
	struct lockdep_stats *__cpu_lockdep_stats;			\
	unsigned long long __total = 0;					\
	int __cpu;							\
	for_each_possible_cpu(__cpu) {					\
		__cpu_lockdep_stats = &per_cpu(lockdep_stats, __cpu);	\
		__total += __cpu_lockdep_stats->ptr;			\
	}								\
	__total;							\
})

/*
 * debug_atomic_read(field) 是 GNU statement expression：声明私有临时变量，
 * 遍历所有 possible CPU，把指定字段累加到 unsigned long long，并以最后的
 * `__total` 表达式作为宏值返回。它不加锁，故与写者并发时只是诊断快照；
 * `field` 仍是字段 token，不会作为有副作用的表达式重复求值。
 */

/*
 * debug_class_ops_inc() - 记录一个锁类发生一次获取操作
 *
 * 调用链：__lock_acquire() 完成 class 查找 → 本函数 → 当前 CPU 的
 * lock_class_ops[idx]。@class 必须是 lock_classes[] 中已发布元素的借用指针，
 * 不可为 NULL；指针相减得到 [0, MAX_LOCKDEP_KEYS) 下标。调用点处于 lockdep
 * 获取热路径，不睡眠、不转移引用；返回：无直接返回值，副作用仅为统计加一。
 */
static inline void debug_class_ops_inc(struct lock_class *class)
{
	int idx;

	/* idx 是 class 在全局静态池中的零基下标，也是每 CPU 统计数组的契约下标。 */
	idx = class - lock_classes;
	__debug_atomic_inc(lock_class_ops[idx]);
}

/*
 * debug_class_ops_read() - 汇总一个锁类在所有 CPU 上的获取次数
 *
 * 调用链：/proc/lockdep 或诊断打印 → 本函数。@class 与 inc 版本相同，是
 * lock_classes[] 中稳定元素的借用输入；函数不取得引用、不睡眠。
 * 返回 unsigned long 汇总值，可能自然回绕；并发更新不会被冻结，因此结果
 * 仅供观察。无其他副作用，调用者随后格式化 OPS 字段。
 */
static inline unsigned long debug_class_ops_read(struct lock_class *class)
{
	int idx, cpu;
	unsigned long ops = 0;

	/* idx 定位每 CPU 数组的同一列；cpu 遍历 possible CPU；ops 保存非快照总和。 */

	idx = class - lock_classes;
	for_each_possible_cpu(cpu)
		ops += per_cpu(lockdep_stats.lock_class_ops[idx], cpu);
	return ops;
}

#else
# define __debug_atomic_inc(ptr)	do { } while (0)
# define debug_atomic_inc(ptr)		do { } while (0)
# define debug_atomic_dec(ptr)		do { } while (0)
# define debug_atomic_read(ptr)		0
# define debug_class_ops_inc(ptr)	do { } while (0)

/*
 * 关闭 CONFIG_DEBUG_LOCKDEP 后，所有更新参数都不求值，读取恒为 0，编译器会
 * 完全移除调试统计开销。debug_class_ops_read() 没有 stub，因为它的调用点
 * 本身受同一配置保护；不能在未保护的新代码中直接调用它。
 */
#endif
