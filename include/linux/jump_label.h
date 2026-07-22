/* SPDX-License-Identifier: GPL-2.0 */
/*
 * static key / jump label 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。该标识只说明新增中文注释来源。
 *
 * 【背景与核心矛盾】
 * tracepoint、调试开关、网络特性等条件常驻极热路径，却通常长期关闭。普通 if 即使
 * 预测准确仍要读取变量、比较并占用分支预测资源。jump label 把“每次执行时判断状态”
 * 改成“状态切换时修改指令”：稳定期热路径只执行一条 NOP 或无条件跳转。
 *
 *   DEFINE_STATIC_KEY_FALSE(feature)
 *             │ 编译器/汇编器生成 __jump_table 元数据
 *             ▼
 *   if (static_branch_unlikely(&feature)) { slow_work(); }
 *             │ 默认关闭：patch site 是 NOP
 *             │ static_branch_enable()：全机同步后 NOP -> JMP
 *             └ static_branch_disable()：JMP -> NOP
 *
 * 收益是读取近乎零成本；代价是切换属于绝对慢路径，需要串行化、CPU/模块生命周期协调、
 * 修改只读可执行文本并维护 I-cache 一致性。因此适合“读极频繁、切换极少”的状态，
 * 不适合用户高频抖动开关。
 *
 * 【对象关系】
 *   static_key.enabled   逻辑引用计数：>0 表示 enabled，0 表示 disabled，负值供切换协议
 *   static_key.type/...  初始真假、是否链接模块元数据，以及该 key 的 jump entries 链
 *   jump_entry           一个补丁点：代码地址、目标地址、key 地址及低位编码标志
 *   static_key_true/false
 *                        类型包装；让宏在编译期知道默认方向并选择最佳指令布局
 *
 * 【生命周期与并发】
 * jump_label_init() 在启动时整理 __jump_table 并把初始指令调整到正确形态；模块装卸时
 * 还会把模块内 entry 链接/摘除。切换 API 在 kernel/jump_label.c 中使用 jump_label_mutex
 * 和 CPU hotplug 读锁，架构层负责真正改写指令。enabled 的 release 发布保证观察到正
 * 值的 fast increment 同时观察到此前文本修改。
 *
 * 【配置回退】
 * CONFIG_JUMP_LABEL=n 时不生成/修改代码，接口仍保留：enabled 使用原子计数，读取退化
 * 为 likely/unlikely 普通条件分支。功能语义一致，但不再拥有 NOP/JMP 的热路径优势。
 */
#ifndef _LINUX_JUMP_LABEL_H
#define _LINUX_JUMP_LABEL_H

/*
 * Jump label support
 *
 * Copyright (C) 2009-2012 Jason Baron <jbaron@redhat.com>
 * Copyright (C) 2011-2012 Red Hat, Inc., Peter Zijlstra
 *
 * DEPRECATED API:
 *
 * The use of 'struct static_key' directly, is now DEPRECATED. In addition
 * static_key_{true,false}() is also DEPRECATED. IE DO NOT use the following:
 *
 * struct static_key false = STATIC_KEY_INIT_FALSE;
 * struct static_key true = STATIC_KEY_INIT_TRUE;
 * static_key_true()
 * static_key_false()
 *
 * The updated API replacements are:
 *
 * DEFINE_STATIC_KEY_TRUE(key);
 * DEFINE_STATIC_KEY_FALSE(key);
 * DEFINE_STATIC_KEY_ARRAY_TRUE(keys, count);
 * DEFINE_STATIC_KEY_ARRAY_FALSE(keys, count);
 * static_branch_likely()
 * static_branch_unlikely()
 *
 * Jump labels provide an interface to generate dynamic branches using
 * self-modifying code. Assuming toolchain and architecture support, if we
 * define a "key" that is initially false via "DEFINE_STATIC_KEY_FALSE(key)",
 * an "if (static_branch_unlikely(&key))" statement is an unconditional branch
 * (which defaults to false - and the true block is placed out of line).
 * Similarly, we can define an initially true key via
 * "DEFINE_STATIC_KEY_TRUE(key)", and use it in the same
 * "if (static_branch_unlikely(&key))", in which case we will generate an
 * unconditional branch to the out-of-line true branch. Keys that are
 * initially true or false can be using in both static_branch_unlikely()
 * and static_branch_likely() statements.
 *
 * At runtime we can change the branch target by setting the key
 * to true via a call to static_branch_enable(), or false using
 * static_branch_disable(). If the direction of the branch is switched by
 * these calls then we run-time modify the branch target via a
 * no-op -> jump or jump -> no-op conversion. For example, for an
 * initially false key that is used in an "if (static_branch_unlikely(&key))"
 * statement, setting the key to true requires us to patch in a jump
 * to the out-of-line of true branch.
 *
 * In addition to static_branch_{enable,disable}, we can also reference count
 * the key or branch direction via static_branch_{inc,dec}. Thus,
 * static_branch_inc() can be thought of as a 'make more true' and
 * static_branch_dec() as a 'make more false'.
 *
 * Since this relies on modifying code, the branch modifying functions
 * must be considered absolute slow paths (machine wide synchronization etc.).
 * OTOH, since the affected branches are unconditional, their runtime overhead
 * will be absolutely minimal, esp. in the default (off) case where the total
 * effect is a single NOP of appropriate size. The on case will patch in a jump
 * to the out-of-line block.
 *
 * When the control is directly exposed to userspace, it is prudent to delay the
 * decrement to avoid high frequency code modifications which can (and do)
 * cause significant performance degradation. Struct static_key_deferred and
 * static_key_slow_dec_deferred() provide for this.
 *
 * Lacking toolchain and or architecture support, static keys fall back to a
 * simple conditional branch.
 *
 * Additional babbling in: Documentation/staging/static-keys.rst
 */
/*
 * 原注释给出完整使用契约：新代码应通过 static_key_true/false 类型包装和
 * static_branch_likely/unlikely 访问；enable/disable 表达布尔状态，inc/dec 表达多用户
 * 引用。默认关闭时一条 NOP 是最便宜路径，打开后改写为跳转；代码修改非常昂贵，暴露
 * 给用户空间的频繁 decrement 应使用 deferred 机制合并抖动。架构或工具链不支持时
 * 自动退化为普通分支，而不是失去功能。
 */

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/cleanup.h>

extern bool static_key_initialized;

/*
 * static_key_initialized 由 jump_label_init() 在元数据和初始文本准备完成后发布。它是
 * 全局启动阶段标志，不是每个 key 的 enabled 状态；初始化前调用切换 API 可能面对尚未
 * 建好的 entry 链，因此 STATIC_KEY_CHECK_USE 用 WARN 暴露错误调用顺序。
 */

#define STATIC_KEY_CHECK_USE(key) WARN(!static_key_initialized,		      \
				    "%s(): static key '%pS' used before call to jump_label_init()", \
				    __func__, (key))

/*
 * STATIC_KEY_CHECK_USE(@key) 保留表达式形式：若全局尚未初始化，打印当前函数及 key 的
 * 符号地址；返回 WARN 的布尔值通常只用于诊断，不替代调用者控制流。@key 是借用指针。
 */

struct static_key {
	atomic_t enabled;
#ifdef CONFIG_JUMP_LABEL
/*
 * bit 0 => 1 if key is initially true
 *	    0 if initially false
 * bit 1 => 1 if points to struct static_key_mod
 *	    0 if points to struct jump_entry
 */
	union {
		unsigned long type;
		struct jump_entry *entries;
		struct static_key_mod *next;
	};
#endif	/* CONFIG_JUMP_LABEL */
};

/*
 * static_key 字段地图：
 *   enabled  原子逻辑计数。0 为 false，正数为 true；引用式 API 允许多个用户。启用首个
 *            用户或移除最后用户才需补丁。实现还短暂使用负值表示 0->1 正在提交。
 *   type     entries/next 联合体的整数视图；低位存放 JUMP_TYPE_TRUE/LINKED，地址因对齐
 *            留出这些位。不得把完整 type 直接当指针。
 *   entries  未链接模块链时指向该 key 排序后的首个 jump_entry；借用静态/模块元数据。
 *   next     key 涉及模块时指向 static_key_mod 链，管理不同模块中的补丁点生命周期。
 * 联合体表示三者共享同一机器字；CONFIG_JUMP_LABEL=n 时只需 enabled，避免无用空间。
 */

#endif /* __ASSEMBLY__ */

#ifdef CONFIG_JUMP_LABEL
#include <asm/jump_label.h>

#ifndef __ASSEMBLY__
#ifdef CONFIG_HAVE_ARCH_JUMP_LABEL_RELATIVE

struct jump_entry {
	s32 code;
	s32 target;
	long key;	// key may be far away from the core kernel under KASLR
};

/*
 * 相对 jump_entry 字段均相对“字段自身地址”编码，以 32 位位移覆盖附近 code/target，key
 * 使用 long 是因为 KASLR 下模块与核心 key 可能相距很远：
 *   code   补丁指令地址相对 &entry->code 的 s32 位移；
 *   target 启用分支目标相对 &entry->target 的 s32 位移；
 *   key    static_key 相对 &entry->key 的位移，低两位复用为 branch/init 标志。
 * entry 是链接器生成的只读元数据记录，不拥有代码或 key。
 */

/* jump_entry_code - 解码补丁点绝对虚拟地址；@entry 为借用元数据，返回 unsigned long。 */
static inline unsigned long jump_entry_code(const struct jump_entry *entry)
{
	return (unsigned long)&entry->code + entry->code;
}

/* jump_entry_target - 解码该补丁点启用时的跳转目标，不修改 entry。 */
static inline unsigned long jump_entry_target(const struct jump_entry *entry)
{
	return (unsigned long)&entry->target + entry->target;
}

/*
 * jump_entry_key - 解码相对 key 指针
 *
 * offset 是 entry->key 清除低两位标志后的有符号相对位移；加到字段自身地址恢复
 * static_key 借用指针。保留 long 的符号可正确表达 key 位于 entry 前方的情况。
 */
static inline struct static_key *jump_entry_key(const struct jump_entry *entry)
{
	long offset = entry->key & ~3L;

	return (struct static_key *)((unsigned long)&entry->key + offset);
}

#else

/*
 * 非相对格式由体系结构 struct jump_entry 直接保存绝对 code/target/key 值。三个 helper
 * 维持与相对格式相同的调用接口；key 仍须清低两位，因为标志编码与地址格式无关。
 */

static inline unsigned long jump_entry_code(const struct jump_entry *entry)
{
	return entry->code;
}

static inline unsigned long jump_entry_target(const struct jump_entry *entry)
{
	return entry->target;
}

static inline struct static_key *jump_entry_key(const struct jump_entry *entry)
{
	return (struct static_key *)((unsigned long)entry->key & ~3UL);
}

#endif

/* jump_entry_is_branch - 读取 key 编码 bit0；true 表示该 site 使用 likely 分支布局。 */
static inline bool jump_entry_is_branch(const struct jump_entry *entry)
{
	return (unsigned long)entry->key & 1UL;
}

/* jump_entry_is_init - 读取 key 编码 bit1；true 表示该 entry 属于 init text 生命周期。 */
static inline bool jump_entry_is_init(const struct jump_entry *entry)
{
	return (unsigned long)entry->key & 2UL;
}

/*
 * jump_entry_set_init - 原地设置或清除 entry 的 init-text 标志
 * @entry 是写侧持有的元数据；@set 决定 bit1，其他地址位和 branch bit 必须保持不变。
 */
static inline void jump_entry_set_init(struct jump_entry *entry, bool set)
{
	if (set)
		entry->key |= 2;
	else
		entry->key &= ~2;
}

/*
 * jump_entry_size - 返回补丁点指令槽长度，单位字节
 * 固定宽度架构直接使用 JUMP_LABEL_NOP_SIZE；需要按 entry 判断的架构调用自己的 helper。
 */
static inline int jump_entry_size(struct jump_entry *entry)
{
#ifdef JUMP_LABEL_NOP_SIZE
	return JUMP_LABEL_NOP_SIZE;
#else
	return arch_jump_entry_size(entry);
#endif
}

#endif
#endif

#ifndef __ASSEMBLY__

enum jump_label_type {
	JUMP_LABEL_NOP = 0,
	JUMP_LABEL_JMP,
};

/*
 * jump_label_type 是架构文本转换层的目标指令形态：NOP 表示顺序落下，JMP 表示跳到
 * entry target。它描述物理补丁结果，不直接等同 key 的逻辑真假；likely/unlikely 布局
 * 不同时，同一个 enabled 状态可能需要相反指令形态。
 */

struct module;

/* module 是不完整类型前置声明；实现层用它把动态模块的 jump table 生命周期与 key 关联。 */

#ifdef CONFIG_JUMP_LABEL

#define JUMP_TYPE_FALSE		0UL
#define JUMP_TYPE_TRUE		1UL
#define JUMP_TYPE_LINKED	2UL
#define JUMP_TYPE_MASK		3UL

/*
 * static_key 联合体低两位编码：FALSE/TRUE 是 bit0 的初始逻辑值；LINKED(bit1) 表示联合体
 * 当前通过 static_key_mod 链管理模块 entries；MASK 用于从地址中剥离两位标志。地址对象
 * 至少四字节对齐，因此复用低位不会丢失真实指针信息。
 */

/* static_key_false - 旧式默认 false 查询；@key 为借用指针，返回架构生成分支的逻辑值。 */
static __always_inline bool static_key_false(struct static_key *key)
{
	return arch_static_branch(key, false);
}

/* static_key_true - 旧式默认 true 查询；取反用于把架构分支原语还原成 key 逻辑值。 */
static __always_inline bool static_key_true(struct static_key *key)
{
	return !arch_static_branch(key, true);
}

/*
 * 下列声明是实现层操作面：jump table 边界供初始化扫描；init/init_ro 管理启动生命周期；
 * lock/unlock 管理全局更新事务；arch transform 系列改写指令；text_reserved 保护仍被引用
 * 的代码区；static_key_* 管理引用或布尔状态；jump_label_init_type 计算初始指令形态。
 * 逐项参数和锁约束由紧随声明后的分组说明给出。
 */
extern struct jump_entry __start___jump_table[];
extern struct jump_entry __stop___jump_table[];

extern void jump_label_init(void);
extern void jump_label_init_ro(void);
extern void jump_label_lock(void);
extern void jump_label_unlock(void);
extern void arch_jump_label_transform(struct jump_entry *entry,
				      enum jump_label_type type);
extern bool arch_jump_label_transform_queue(struct jump_entry *entry,
					    enum jump_label_type type);
extern void arch_jump_label_transform_apply(void);
extern int jump_label_text_reserved(void *start, void *end);
extern bool static_key_slow_inc(struct static_key *key);
extern bool static_key_fast_inc_not_disabled(struct static_key *key);
extern void static_key_slow_dec(struct static_key *key);
extern bool static_key_slow_inc_cpuslocked(struct static_key *key);
extern void static_key_slow_dec_cpuslocked(struct static_key *key);
extern int static_key_count(struct static_key *key);
extern void static_key_enable(struct static_key *key);
extern void static_key_disable(struct static_key *key);
extern void static_key_enable_cpuslocked(struct static_key *key);
extern void static_key_disable_cpuslocked(struct static_key *key);
extern enum jump_label_type jump_label_init_type(struct jump_entry *entry);

/*
 * CONFIG_JUMP_LABEL=y 的外部接口分组：
 *   __start/__stop___jump_table 链接器给出的核心内核 entry 半开区间；
 *   jump_label_init/init_ro 启动期整理/补丁元数据，并在适当阶段收紧只读属性；
 *   jump_label_lock/unlock 串行化 key 更新与模块 entry 链变化；
 *   arch_jump_label_transform[_queue/_apply] 由体系结构执行单点或批量文本改写；
 *   jump_label_text_reserved 判断卸载/替换的文本区间是否仍含受管理补丁点；
 *   static_key_* inc/dec/enable/disable 修改逻辑计数，仅在跨越 0 边界时更新文本；
 *   *_cpuslocked 变体要求调用者已持 CPU hotplug 读锁，避免嵌套获取；
 *   static_key_count 读取逻辑计数并隐藏内部 -1 过渡态；
 *   jump_label_init_type 根据初始 key 类型和 site 布局算出初始 NOP/JMP。
 * 所有 key/entry 参数均为借用指针；文本更新 API 属于可睡眠慢路径。
 */

#define STATIC_KEY_INIT_TRUE					\
	{ .enabled = ATOMIC_INIT(1),				\
	  .type = JUMP_TYPE_TRUE }
#define STATIC_KEY_INIT_FALSE					\
	{ .enabled = ATOMIC_INIT(0),				\
	  .type = JUMP_TYPE_FALSE }

/*
 * STATIC_KEY_INIT_TRUE/FALSE 同时初始化逻辑计数与初始类型位：true 从 1 个用户开始，
 * false 从 0 开始；entries 链会由 jump_label_init() 根据 __jump_table 建立。它们是静态
 * 初始化器，只能放在对象定义上下文，不执行运行期注册。
 */

#else  /* !CONFIG_JUMP_LABEL */

#include <linux/atomic.h>
#include <linux/bug.h>

static __always_inline int static_key_count(struct static_key *key)
{
	/* @key 是借用对象；raw_atomic_read 返回当前逻辑计数，无锁且不提供额外内存顺序。 */
	return raw_atomic_read(&key->enabled);
}

/* 无 jump-label 配置不需整理元数据，只发布“接口已初始化”，无参数和失败路径。 */
static __always_inline void jump_label_init(void)
{
	static_key_initialized = true;
}

/* 无 jump table/text 元数据需要只读收尾；无参数、返回值或副作用。 */
static __always_inline void jump_label_init_ro(void) { }

/* jump_label_init_ro 在 fallback 中为空：没有 jump table/text 元数据需要改成只读。 */

/* fallback 默认 false 查询：enabled>0 为 true，unlikely_notrace 只提供布局/预测提示。 */
static __always_inline bool static_key_false(struct static_key *key)
{
	if (unlikely_notrace(static_key_count(key) > 0))
		return true;
	return false;
}

/* fallback 默认 true 查询：逻辑仍是 enabled>0，likely_notrace 对常见 true 路径给提示。 */
static __always_inline bool static_key_true(struct static_key *key)
{
	if (likely_notrace(static_key_count(key) > 0))
		return true;
	return false;
}

/*
 * static_key_fast_inc_not_disabled - fallback 中无文本补丁的原子引用增加
 *
 * @key 为借用对象；v 是 cmpxchg 期望值兼失败后的最新值。初始化检查后，以 CAS 循环
 * 把 enabled 加一；负过渡态或整数溢出返回 false，成功返回 true。即使不支持 jump
 * label，也保持 CONFIG=y 对负值/溢出的契约，避免同一调用者因配置不同产生语义分裂。
 */
static inline bool static_key_fast_inc_not_disabled(struct static_key *key)
{
	int v;

	STATIC_KEY_CHECK_USE(key);
	/*
	 * Prevent key->enabled getting negative to follow the same semantics
	 * as for CONFIG_JUMP_LABEL=y, see kernel/jump_label.c comment.
	 */
	/* 原注释说明负 enabled 属于实现保留状态，绝不能被普通引用增加变成有效正计数。 */
	v = atomic_read(&key->enabled);
	do {
		if (v < 0 || (v + 1) < 0)
			return false;
	} while (!likely(atomic_try_cmpxchg(&key->enabled, &v, v + 1)));
	return true;
}
#define static_key_slow_inc(key)	static_key_fast_inc_not_disabled(key)

/* fallback 没有补丁慢路径，slow_inc 直接别名为上述原子增加并保留 bool 返回约定。 */

/* static_key_slow_dec - 原子减少一个用户；@key 必须已有匹配引用，函数不防止下溢。 */
static inline void static_key_slow_dec(struct static_key *key)
{
	STATIC_KEY_CHECK_USE(key);
	atomic_dec(&key->enabled);
}

#define static_key_slow_inc_cpuslocked(key) static_key_slow_inc(key)
#define static_key_slow_dec_cpuslocked(key) static_key_slow_dec(key)

/* fallback 区间查询：@start/@end 为借用边界，固定返回 0 且无副作用。 */
static inline int jump_label_text_reserved(void *start, void *end)
{
	/* @start/@end 在无补丁元数据时无需检查；固定 0 表示区间未被 jump label 保留。 */
	return 0;
}

/* fallback 无全局 entry/module 链；这一对无参、无返回空操作仅保持调用者接口一致。 */
static inline void jump_label_lock(void) {}
static inline void jump_label_unlock(void) {}

/* fallback 无全局 entry/module 链，lock/unlock 是保持调用者结构一致的空操作。 */

/*
 * static_key_enable - 把布尔 key 从 0 置 1
 * @key 为借用对象；若已非 0，只允许合法幂等状态 1，否则告警。无返回值、无文本修改。
 */
static inline void static_key_enable(struct static_key *key)
{
	STATIC_KEY_CHECK_USE(key);

	if (atomic_read(&key->enabled) != 0) {
		WARN_ON_ONCE(atomic_read(&key->enabled) != 1);
		return;
	}
	atomic_set(&key->enabled, 1);
}

/* static_key_disable - 把布尔 key 从 1 置 0；已为 0 时幂等，其他计数表示 API 混用。 */
static inline void static_key_disable(struct static_key *key)
{
	STATIC_KEY_CHECK_USE(key);

	if (atomic_read(&key->enabled) != 1) {
		WARN_ON_ONCE(atomic_read(&key->enabled) != 0);
		return;
	}
	atomic_set(&key->enabled, 0);
}

#define static_key_enable_cpuslocked(k)		static_key_enable((k))
#define static_key_disable_cpuslocked(k)	static_key_disable((k))

#define STATIC_KEY_INIT_TRUE	{ .enabled = ATOMIC_INIT(1) }
#define STATIC_KEY_INIT_FALSE	{ .enabled = ATOMIC_INIT(0) }

/* fallback 初始化器只需 enabled；没有 type/entry 指针和初始文本形态。 */

#endif	/* CONFIG_JUMP_LABEL */

DEFINE_LOCK_GUARD_0(jump_label_lock, jump_label_lock(), jump_label_unlock())

/*
 * 生成 cleanup guard 类型：使用 guard(jump_label_lock)() 的作用域退出时自动 unlock，
 * 包括提前 return。该宏不创建全局锁；它包装上面两种配置各自的 lock/unlock 实现。
 */

#define STATIC_KEY_INIT STATIC_KEY_INIT_FALSE
#define jump_label_enabled static_key_enabled

/*
 * STATIC_KEY_INIT 保留旧 API 的默认 false 初始化语义；jump_label_enabled 是历史查询别名。
 * 新代码应优先使用带 true/false 类型的 DEFINE_STATIC_KEY_* 与 static_branch_*。
 */

/* -------------------------------------------------------------------------- */

/*
 * Two type wrappers around static_key, such that we can use compile time
 * type differentiation to emit the right code.
 *
 * All the below code is macros in order to play type games.
 */
/*
 * 原注释解释包装层的关键目的：两个结构体运行时都只含同一个 static_key，却是不同 C
 * 类型。宏借 __builtin_types_compatible_p 在编译期识别初始 true/false，选择能让默认
 * 路径成为 NOP 的架构原语；若退化成普通函数，类型信息会丢失，无法生成最佳布局。
 */

struct static_key_true {
	struct static_key key;
};

struct static_key_false {
	struct static_key key;
};

/*
 * static_key_true/false 代表“默认开启/默认关闭的分支控制对象”。唯一字段 key 承载计数、
 * entry 链和并发状态；包装对象与内嵌 key 地址相同，不增加独立生命周期或所有权。
 */

#define STATIC_KEY_TRUE_INIT  (struct static_key_true) { .key = STATIC_KEY_INIT_TRUE,  }
#define STATIC_KEY_FALSE_INIT (struct static_key_false){ .key = STATIC_KEY_INIT_FALSE, }

/* TRUE/FALSE_INIT 是带类型的复合字面量，把底层初始计数和 wrapper 类型绑定。 */

#define DEFINE_STATIC_KEY_TRUE(name)	\
	struct static_key_true name = STATIC_KEY_TRUE_INIT

#define DEFINE_STATIC_KEY_TRUE_RO(name)	\
	struct static_key_true name __ro_after_init = STATIC_KEY_TRUE_INIT

#define DECLARE_STATIC_KEY_TRUE(name)	\
	extern struct static_key_true name

#define DEFINE_STATIC_KEY_FALSE(name)	\
	struct static_key_false name = STATIC_KEY_FALSE_INIT

#define DEFINE_STATIC_KEY_FALSE_RO(name)	\
	struct static_key_false name __ro_after_init = STATIC_KEY_FALSE_INIT

#define DECLARE_STATIC_KEY_FALSE(name)	\
	extern struct static_key_false name

/*
 * DEFINE_* 创建具有静态存储期的 key；*_RO 额外放入 __ro_after_init，适用于初始化后
 * 不再切换的 key；DECLARE_* 只声明外部对象。@name 是标识符，不是运行时表达式。
 */

#define DEFINE_STATIC_KEY_ARRAY_TRUE(name, count)		\
	struct static_key_true name[count] = {			\
		[0 ... (count) - 1] = STATIC_KEY_TRUE_INIT,	\
	}

#define DEFINE_STATIC_KEY_ARRAY_FALSE(name, count)		\
	struct static_key_false name[count] = {			\
		[0 ... (count) - 1] = STATIC_KEY_FALSE_INIT,	\
	}

/*
 * ARRAY 变体定义 @count 个同类型 key，并用 GNU 范围初始化器逐个建立正确初值；每个
 * 元素仍有独立 enabled/entry 链，数组本身不共享计数。@count 必须是编译期正整数。
 */

#define _DEFINE_STATIC_KEY_1(name)	DEFINE_STATIC_KEY_TRUE(name)
#define _DEFINE_STATIC_KEY_0(name)	DEFINE_STATIC_KEY_FALSE(name)
#define DEFINE_STATIC_KEY_MAYBE(cfg, name)			\
	__PASTE(_DEFINE_STATIC_KEY_, IS_ENABLED(cfg))(name)

#define _DEFINE_STATIC_KEY_RO_1(name)	DEFINE_STATIC_KEY_TRUE_RO(name)
#define _DEFINE_STATIC_KEY_RO_0(name)	DEFINE_STATIC_KEY_FALSE_RO(name)
#define DEFINE_STATIC_KEY_MAYBE_RO(cfg, name)			\
	__PASTE(_DEFINE_STATIC_KEY_RO_, IS_ENABLED(cfg))(name)

#define _DECLARE_STATIC_KEY_1(name)	DECLARE_STATIC_KEY_TRUE(name)
#define _DECLARE_STATIC_KEY_0(name)	DECLARE_STATIC_KEY_FALSE(name)
#define DECLARE_STATIC_KEY_MAYBE(cfg, name)			\
	__PASTE(_DECLARE_STATIC_KEY_, IS_ENABLED(cfg))(name)

/*
 * *_MAYBE 系列把 Kconfig @cfg 在预处理期折叠为 0/1，再拼接到 true/false 定义宏；
 * 无论配置如何对象都存在，但默认状态与配置一致。RO/DECLARE 版本分别保持只读和声明语义。
 */

extern bool ____wrong_branch_error(void);

/*
 * ____wrong_branch_error 是故意不给正常调用者使用的类型错误落点。宏只在参数不是允许的
 * static_key 类型时引用它，使编译/链接或 objtool 检查暴露 API 误用，而非静默生成错分支。
 */

#define static_key_enabled(x)							\
({										\
	if (!__builtin_types_compatible_p(typeof(*x), struct static_key) &&	\
	    !__builtin_types_compatible_p(typeof(*x), struct static_key_true) &&\
	    !__builtin_types_compatible_p(typeof(*x), struct static_key_false))	\
		____wrong_branch_error();					\
	static_key_count((struct static_key *)x) > 0;				\
})

/*
 * static_key_enabled(@x) 是语句表达式：先在编译期接受底层 static_key 或两种 wrapper，
 * 再统一转换为底层指针并返回 count>0。它只查询逻辑状态，不保证生成 jump-label NOP；
 * 热路径应使用 static_branch_*。@x 必须是对象指针，宏不会取得引用。
 */

#ifdef CONFIG_JUMP_LABEL

/*
 * Combine the right initial value (type) with the right branch order
 * to generate the desired result.
 *
 *
 * type\branch|	likely (1)	      |	unlikely (0)
 * -----------+-----------------------+------------------
 *            |                       |
 *  true (1)  |	   ...		      |	   ...
 *            |    NOP		      |	   JMP L
 *            |    <br-stmts>	      |	1: ...
 *            |	L: ...		      |
 *            |			      |
 *            |			      |	L: <br-stmts>
 *            |			      |	   jmp 1b
 *            |                       |
 * -----------+-----------------------+------------------
 *            |                       |
 *  false (0) |	   ...		      |	   ...
 *            |    JMP L	      |	   NOP
 *            |    <br-stmts>	      |	1: ...
 *            |	L: ...		      |
 *            |			      |
 *            |			      |	L: <br-stmts>
 *            |			      |	   jmp 1b
 *            |                       |
 * -----------+-----------------------+------------------
 *
 * The initial value is encoded in the LSB of static_key::entries,
 * type: 0 = false, 1 = true.
 *
 * The branch type is encoded in the LSB of jump_entry::key,
 * branch: 0 = unlikely, 1 = likely.
 *
 * This gives the following logic table:
 *
 *	enabled	type	branch	  instuction
 * -----------------------------+-----------
 *	0	0	0	| NOP
 *	0	0	1	| JMP
 *	0	1	0	| NOP
 *	0	1	1	| JMP
 *
 *	1	0	0	| JMP
 *	1	0	1	| NOP
 *	1	1	0	| JMP
 *	1	1	1	| NOP
 *
 * Which gives the following functions:
 *
 *   dynamic: instruction = enabled ^ branch
 *   static:  instruction = type ^ branch
 *
 * See jump_label_type() / jump_label_init_type().
 */
/*
 * 原表需要从两个独立维度理解：
 *   type   来自 wrapper 的初始真假，决定链接时 patch site 初始形态；
 *   branch 来自 likely/unlikely 调用形式，决定 true 代码块在线内还是线外。
 * 运行期最终指令只取决于 enabled XOR branch；启动初始化指令取决于 type XOR branch。
 * 因而默认 true 配 likely、默认 false 配 unlikely 都能让常见路径使用 NOP。entry->key
 * bit0 保存 branch，static_key 联合体 bit0 保存 type，二者不是同一个标志位存储位置。
 */

#define static_branch_likely(x)							\
({										\
	bool branch;								\
	if (__builtin_types_compatible_p(typeof(*x), struct static_key_true))	\
		branch = !arch_static_branch(&(x)->key, true);			\
	else if (__builtin_types_compatible_p(typeof(*x), struct static_key_false)) \
		branch = !arch_static_branch_jump(&(x)->key, true);		\
	else									\
		branch = ____wrong_branch_error();				\
	likely_notrace(branch);								\
})

/*
 * static_branch_likely(@x)：局部 branch 保存架构原语计算的逻辑结果。默认 true wrapper
 * 使用 arch_static_branch，默认 false wrapper 使用 arch_static_branch_jump，以便无论
 * 初始类型如何都维持 likely 的代码布局；错误类型走专用错误符号。likely_notrace 避免
 * 为 static-key 自身引入 tracing 递归。
 */

#define static_branch_unlikely(x)						\
({										\
	bool branch;								\
	if (__builtin_types_compatible_p(typeof(*x), struct static_key_true))	\
		branch = arch_static_branch_jump(&(x)->key, false);		\
	else if (__builtin_types_compatible_p(typeof(*x), struct static_key_false)) \
		branch = arch_static_branch(&(x)->key, false);			\
	else									\
		branch = ____wrong_branch_error();				\
	unlikely_notrace(branch);							\
})

/*
 * static_branch_unlikely(@x) 与上式对称：选择使默认 false 路径为 NOP、true 块线外的
 * 原语组合，并用 unlikely_notrace 返回逻辑值。两个宏都要求 @x 指向 wrapper，而不是
 * 已废弃的裸 struct static_key。
 */

#else /* !CONFIG_JUMP_LABEL */

#define static_branch_likely(x)		likely_notrace(static_key_enabled(&(x)->key))
#define static_branch_unlikely(x)	unlikely_notrace(static_key_enabled(&(x)->key))

/*
 * CONFIG_JUMP_LABEL=n 时两个宏直接读取 enabled；likely/unlikely 只给编译器分支预测和
 * 布局提示，不修改代码。@x 仍通过 wrapper 的 key 字段保持同一源代码接口。
 */

#endif /* CONFIG_JUMP_LABEL */

#define static_branch_maybe(config, x)					\
	(IS_ENABLED(config) ? static_branch_likely(x)			\
			    : static_branch_unlikely(x))

/*
 * static_branch_maybe(@config, @x) 用编译期 Kconfig 值选择预测方向：配置默认开启时走
 * likely，否则走 unlikely。它不改变 key 的实际 enabled 状态，只优化代码布局。
 */

/*
 * Advanced usage; refcount, branch is enabled when: count != 0
 */
/*
 * 原注释定义引用式协议：每个 inc 必须有匹配 dec；计数从 0->1 时启用并补丁，从 1->0
 * 时关闭并补丁，中间正数变化只改计数。cpuslocked 版本要求外层已阻止 CPU hotplug。
 */

#define static_branch_inc(x)		static_key_slow_inc(&(x)->key)
#define static_branch_dec(x)		static_key_slow_dec(&(x)->key)
#define static_branch_inc_cpuslocked(x)	static_key_slow_inc_cpuslocked(&(x)->key)
#define static_branch_dec_cpuslocked(x)	static_key_slow_dec_cpuslocked(&(x)->key)

/* 四个宏只把 wrapper 解包为底层 key；返回/失败语义继承对应 static_key_* 实现。 */

/*
 * Normal usage; boolean enable/disable.
 */
/*
 * 布尔协议只允许 0/1，适用于唯一控制者；重复 enable/disable 可幂等，但与 inc/dec 混用
 * 会触发计数告警。它与引用式 API 的选择是所有权模型选择，不只是命名差异。
 */

#define static_branch_enable(x)			static_key_enable(&(x)->key)
#define static_branch_disable(x)		static_key_disable(&(x)->key)
#define static_branch_enable_cpuslocked(x)	static_key_enable_cpuslocked(&(x)->key)
#define static_branch_disable_cpuslocked(x)	static_key_disable_cpuslocked(&(x)->key)

/* enable/disable 宏同样仅解包 wrapper；真正文本修改、锁和内存序在实现函数中完成。 */

#endif /* __ASSEMBLY__ */

#endif	/* _LINUX_JUMP_LABEL_H */
