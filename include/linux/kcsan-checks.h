/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KCSAN access checks and modifiers. These can be used to explicitly check
 * uninstrumented accesses, or change KCSAN checking behaviour of accesses.
 *
 * Copyright (C) 2019, Google LLC.
 */
/*
 * KCSAN 显式访问检查与行为修饰接口。它们用于检查未被编译器插桩的访问，或
 * 临时改变当前上下文对 atomic、scoped、masked access 的解释。KCSAN 是动态
 * 采样检测器：断言提高发现概率，不建立真实同步，也不延长被检查对象生命周期。
 */

#ifndef _LINUX_KCSAN_CHECKS_H
#define _LINUX_KCSAN_CHECKS_H

/* Note: Only include what is already included by compiler.h. */
/* 本头会被 compiler.h 间接使用，只能包含 compiler.h 已经包含的基础依赖，避免循环。 */
#include <linux/compiler_attributes.h>
#include <linux/types.h>

/* Access types -- if KCSAN_ACCESS_WRITE is not set, the access is a read. */
/* type 位图：未置 WRITE 即读；COMPOUND 表示复合读改写，ATOMIC 表示原子访问。 */
#define KCSAN_ACCESS_WRITE	(1 << 0) /* Access is a write. */
#define KCSAN_ACCESS_COMPOUND	(1 << 1) /* Compounded read-write instrumentation. */
#define KCSAN_ACCESS_ATOMIC	(1 << 2) /* Access is atomic. */
/* The following are special, and never due to compiler instrumentation. */
/* ASSERT 与 SCOPED 只由显式 API 产生，编译器自动插桩不会设置。 */
#define KCSAN_ACCESS_ASSERT	(1 << 3) /* Access is an assertion. */
#define KCSAN_ACCESS_SCOPED	(1 << 4) /* Access is a scoped access. */

/*
 * __kcsan_*: Always calls into the runtime when KCSAN is enabled. This may be used
 * even in compilation units that selectively disable KCSAN, but must use KCSAN
 * to validate access to an address. Never use these in header files!
 */
/*
 * __kcsan_* 在 CONFIG_KCSAN=y 时始终进入 runtime，即使当前翻译单元关闭自动
 * 插桩；因此适合必须验证地址的实现文件，禁止放进头文件以免强迫未插桩调用者
 * 产生 runtime 依赖。kcsan_* 则只在当前单元启用插桩时有效，可安全用于头文件。
 */
#ifdef CONFIG_KCSAN
/**
 * __kcsan_check_access - check generic access for races
 *
 * @ptr: address of access
 * @size: size of access
 * @type: access type modifier
 */
/*
 * __kcsan_check_access() 检查 [@ptr,@ptr+@size) 的一次访问；@type 描述读写、
 * atomic/assert/scoped 属性。指针只借用，size 以字节计，不执行真实内存访问；
 * runtime 可记录/比较 watchpoint，不睡眠要求由任意内核上下文调用决定。
 */
void __kcsan_check_access(const volatile void *ptr, size_t size, int type);

/*
 * See definition of __tsan_atomic_signal_fence() in kernel/kcsan/core.c.
 * Note: The mappings are arbitrary, and do not reflect any real mappings of C11
 * memory orders to the LKMM memory orders and vice-versa!
 */
/*
 * signal_fence 数值映射只用来让 KCSAN runtime 区分 mb/wmb/rmb/release，完全
 * 不代表 C11 memory order 与 LKMM 的真实对应关系；不得拿这些常量推导硬件顺序。
 */
#define __KCSAN_BARRIER_TO_SIGNAL_FENCE_mb	__ATOMIC_SEQ_CST
#define __KCSAN_BARRIER_TO_SIGNAL_FENCE_wmb	__ATOMIC_ACQ_REL
#define __KCSAN_BARRIER_TO_SIGNAL_FENCE_rmb	__ATOMIC_ACQUIRE
#define __KCSAN_BARRIER_TO_SIGNAL_FENCE_release	__ATOMIC_RELEASE

/**
 * __kcsan_mb - full memory barrier instrumentation
 */
/* __kcsan_mb() 报告完整屏障；无入参/返回，不替代真实 fence。 */
void __kcsan_mb(void);

/**
 * __kcsan_wmb - write memory barrier instrumentation
 */
/* __kcsan_wmb() 报告写屏障；无入参/返回。 */
void __kcsan_wmb(void);

/**
 * __kcsan_rmb - read memory barrier instrumentation
 */
/* __kcsan_rmb() 报告读屏障；无入参/返回。 */
void __kcsan_rmb(void);

/**
 * __kcsan_release - release barrier instrumentation
 */
/* __kcsan_release() 报告 release 屏障；无入参/返回。 */
void __kcsan_release(void);

/**
 * kcsan_disable_current - disable KCSAN for the current context
 *
 * Supports nesting.
 */
/* kcsan_disable_current() 增加当前执行上下文禁用深度；可嵌套，必须成对 enable。 */
void kcsan_disable_current(void);

/**
 * kcsan_enable_current - re-enable KCSAN for the current context
 *
 * Supports nesting.
 */
/* enable 递减禁用深度；nowarn 版本可安全用于 uaccess 区域，避免状态恢复告警。 */
void kcsan_enable_current(void);
/* kcsan_enable_current_nowarn() 同样递减深度，但可在 uaccess 区域无告警恢复。 */
void kcsan_enable_current_nowarn(void); /* Safe in uaccess regions. */
/* nowarn 变体可在 uaccess 区域安全恢复，不触发普通 enable 的上下文告警。 */

/**
 * kcsan_nestable_atomic_begin - begin nestable atomic region
 *
 * Accesses within the atomic region may appear to race with other accesses but
 * should be considered atomic.
 */
/* nestable_atomic_begin/end 把范围内看似竞态的访问视为一个可嵌套原子区域；不提供真实互斥。 */
void kcsan_nestable_atomic_begin(void);

/**
 * kcsan_nestable_atomic_end - end nestable atomic region
 */
/* 结束最近一层 nestable atomic；缺失配对会污染后续当前上下文的检测语义。 */
void kcsan_nestable_atomic_end(void);

/**
 * kcsan_flat_atomic_begin - begin flat atomic region
 *
 * Accesses within the atomic region may appear to race with other accesses but
 * should be considered atomic.
 */
/* flat_atomic_begin/end 提供非嵌套原子区语义，适合已知不会递归的范围，开销更直接。 */
void kcsan_flat_atomic_begin(void);

/**
 * kcsan_flat_atomic_end - end flat atomic region
 */
/* 结束 flat atomic 区；必须与 begin 成对，返回无直接值。 */
void kcsan_flat_atomic_end(void);

/**
 * kcsan_atomic_next - consider following accesses as atomic
 *
 * Force treating the next n memory accesses for the current context as atomic
 * operations.
 *
 * @n: number of following memory accesses to treat as atomic.
 */
/* kcsan_atomic_next(@n) 令当前上下文接下来的 @n 次内存访问按原子处理；@n 必须非负且范围精确。 */
void kcsan_atomic_next(int n);

/**
 * kcsan_set_access_mask - set access mask
 *
 * Set the access mask for all accesses for the current context if non-zero.
 * Only value changes to bits set in the mask will be reported.
 *
 * @mask: bitmask
 */
/*
 * kcsan_set_access_mask(@mask) 设置当前上下文访问位掩码；非零时只报告 mask 位
 * 的变化，传 0 恢复全位检查。它是上下文状态，所有出口都必须恢复。
 */
void kcsan_set_access_mask(unsigned long mask);

/* Scoped access information. */
/*
 * kcsan_scoped_access 描述持续检查区间。union 在已入当前上下文链表时保存 list
 * 节点，否则保存初始化栈深度；ptr/size/type 描述借用内存范围，ip 记录建立点。
 * 结构通常位于调用者栈上，其生命周期必须覆盖整个 scoped access。
 */
struct kcsan_scoped_access {
	union {
		struct list_head list; /* scoped_accesses list */
		/* list 是挂入当前上下文 scoped_accesses 链表的节点。 */
		/*
		 * Not an entry in scoped_accesses list; stack depth from where
		 * the access was initialized.
		 */
		/* 尚未入链时，该 union 保存建立访问位置的栈深度，而不是链表节点。 */
		int stack_depth;
	};

	/* Access information. */
	/* 以下字段保存被持续检查的地址、字节数和 access type。 */
	const volatile void *ptr;
	size_t size;
	int type;
	/* Location where scoped access was set up. */
	/* ip 记录建立 scoped access 的指令地址，供报告定位。 */
	unsigned long ip;
};
/*
 * Automatically call kcsan_end_scoped_access() when kcsan_scoped_access goes
 * out of scope; relies on attribute "cleanup", which is supported by all
 * compilers that support KCSAN.
 */
/* cleanup 属性在局部变量离开作用域时自动调用 end；所有支持 KCSAN 的编译器都支持该属性。 */
#define __kcsan_cleanup_scoped                                                 \
	__maybe_unused __attribute__((__cleanup__(kcsan_end_scoped_access)))

/**
 * kcsan_begin_scoped_access - begin scoped access
 *
 * Begin scoped access and initialize @sa, which will cause KCSAN to
 * continuously check the memory range in the current thread until
 * kcsan_end_scoped_access() is called for @sa.
 *
 * Scoped accesses are implemented by appending @sa to an internal list for the
 * current execution context, and then checked on every call into the KCSAN
 * runtime.
 *
 * @ptr: address of access
 * @size: size of access
 * @type: access type modifier
 * @sa: struct kcsan_scoped_access to use for the scope of the access
 */
/*
 * kcsan_begin_scoped_access() 初始化调用者提供的 @sa，并把它加入当前执行上下文
 * scoped 列表；之后每次进入 runtime 都持续检查 @ptr/@size，直到 end。返回
 * @sa 便于 cleanup 声明；所有参数只借用，不分配对象，不得对同一 @sa 重复 begin。
 */
struct kcsan_scoped_access *
kcsan_begin_scoped_access(const volatile void *ptr, size_t size, int type,
			  struct kcsan_scoped_access *sa);

/**
 * kcsan_end_scoped_access - end scoped access
 *
 * End a scoped access, which will stop KCSAN checking the memory range.
 * Requires that kcsan_begin_scoped_access() was previously called once for @sa.
 *
 * @sa: a previously initialized struct kcsan_scoped_access
 */
/* end 从当前上下文列表摘除 @sa 并停止检查；要求此前恰好 begin 一次，返回无直接值。 */
void kcsan_end_scoped_access(struct kcsan_scoped_access *sa);


#else /* CONFIG_KCSAN */

/* CONFIG_KCSAN=n 时所有函数内联为空；begin 原样返回 @sa，保持 cleanup/调用语法而无运行时副作用。 */

static inline void __kcsan_check_access(const volatile void *ptr, size_t size,
					int type) { }

/* 以下 CONFIG_KCSAN=n stubs 均无入参副作用、无返回值且不睡眠，只保持调用 ABI。 */
/* __kcsan_mb()：完整屏障插桩为空。 */
static inline void __kcsan_mb(void)			{ }
/* __kcsan_wmb()：写屏障插桩为空。 */
static inline void __kcsan_wmb(void)			{ }
/* __kcsan_rmb()：读屏障插桩为空。 */
static inline void __kcsan_rmb(void)			{ }
/* __kcsan_release()：release 插桩为空。 */
static inline void __kcsan_release(void)		{ }
/* kcsan_disable_current()：不建立禁用深度。 */
static inline void kcsan_disable_current(void)		{ }
/* kcsan_enable_current()：无禁用深度需要恢复。 */
static inline void kcsan_enable_current(void)		{ }
/* kcsan_enable_current_nowarn()：nowarn 恢复同样为空。 */
static inline void kcsan_enable_current_nowarn(void)	{ }
/* kcsan_nestable_atomic_begin()：不建立嵌套 atomic 区。 */
static inline void kcsan_nestable_atomic_begin(void)	{ }
/* kcsan_nestable_atomic_end()：不结束任何检测区。 */
static inline void kcsan_nestable_atomic_end(void)	{ }
/* kcsan_flat_atomic_begin()：不建立 flat atomic 区。 */
static inline void kcsan_flat_atomic_begin(void)	{ }
/* kcsan_flat_atomic_end()：不结束任何检测区。 */
static inline void kcsan_flat_atomic_end(void)		{ }
/* @n 被忽略；不会把后续真实访问变成原子操作。 */
static inline void kcsan_atomic_next(int n)		{ }
/* @mask 被忽略；不会建立当前上下文检测状态。 */
static inline void kcsan_set_access_mask(unsigned long mask) { }

struct kcsan_scoped_access { };
#define __kcsan_cleanup_scoped __maybe_unused
static inline struct kcsan_scoped_access *
kcsan_begin_scoped_access(const volatile void *ptr, size_t size, int type,
			  struct kcsan_scoped_access *sa) { return sa; }
/* @sa 只借用且不读取；无 KCSAN 构建没有链表需要摘除。 */
static inline void kcsan_end_scoped_access(struct kcsan_scoped_access *sa) { }

#endif /* CONFIG_KCSAN */

#ifdef __SANITIZE_THREAD__
/*
 * Only calls into the runtime when the particular compilation unit has KCSAN
 * instrumentation enabled. May be used in header files.
 */
/* 仅当前翻译单元启用 KCSAN 插桩时才映射到 runtime，因此 kcsan_check_access 可用于头文件。 */
#define kcsan_check_access __kcsan_check_access

/*
 * Only use these to disable KCSAN for accesses in the current compilation unit;
 * calls into libraries may still perform KCSAN checks.
 */
/* __kcsan_disable/enable 只影响当前翻译单元中的访问；被调库仍可能执行自己的 KCSAN 检查。 */
#define __kcsan_disable_current kcsan_disable_current
#define __kcsan_enable_current kcsan_enable_current_nowarn
#else /* __SANITIZE_THREAD__ */
/* 当前翻译单元未插桩时，显式非强制检查与局部 enable/disable 均为空。 */
static inline void kcsan_check_access(const volatile void *ptr, size_t size,
				      int type) { }
static inline void __kcsan_enable_current(void)  { }
static inline void __kcsan_disable_current(void) { }
#endif /* __SANITIZE_THREAD__ */

#if defined(CONFIG_KCSAN_WEAK_MEMORY) && defined(__SANITIZE_THREAD__)
/*
 * Normal barrier instrumentation is not done via explicit calls, but by mapping
 * to a repurposed __atomic_signal_fence(), which normally does not generate any
 * real instructions, but is still intercepted by fsanitize=thread. This means,
 * like any other compile-time instrumentation, barrier instrumentation can be
 * disabled with the __no_kcsan function attribute.
 *
 * Also see definition of __tsan_atomic_signal_fence() in kernel/kcsan/core.c.
 *
 * These are all macros, like <asm/barrier.h>, since some architectures use them
 * in non-static inline functions.
 */
/*
 * 普通屏障插桩不显式调用 runtime，而复用通常不生成指令、但会被
 * fsanitize=thread 截获的 __atomic_signal_fence；前后 barrier() 阻止编译器
 * 穿越。它与其他编译期插桩一样会被 __no_kcsan 禁用。某些体系结构在非 static
 * inline 中使用这些接口，故必须保持宏形式。core.c 的拦截实现见原文。
 */
#define __KCSAN_BARRIER_TO_SIGNAL_FENCE(name)					\
	do {									\
		barrier();							\
		__atomic_signal_fence(__KCSAN_BARRIER_TO_SIGNAL_FENCE_##name);	\
		barrier();							\
	} while (0)
#define kcsan_mb()	__KCSAN_BARRIER_TO_SIGNAL_FENCE(mb)
#define kcsan_wmb()	__KCSAN_BARRIER_TO_SIGNAL_FENCE(wmb)
#define kcsan_rmb()	__KCSAN_BARRIER_TO_SIGNAL_FENCE(rmb)
#define kcsan_release()	__KCSAN_BARRIER_TO_SIGNAL_FENCE(release)
#elif defined(CONFIG_KCSAN_WEAK_MEMORY) && defined(__KCSAN_INSTRUMENT_BARRIERS__)
#define kcsan_mb	__kcsan_mb
#define kcsan_wmb	__kcsan_wmb
#define kcsan_rmb	__kcsan_rmb
#define kcsan_release	__kcsan_release
#else /* CONFIG_KCSAN_WEAK_MEMORY && ... */
#define kcsan_mb()	do { } while (0)
#define kcsan_wmb()	do { } while (0)
#define kcsan_rmb()	do { } while (0)
#define kcsan_release()	do { } while (0)
#endif /* CONFIG_KCSAN_WEAK_MEMORY && ... */

/*
 * 三组配置：当前单元自动插桩时用 signal_fence；仅显式 barrier 插桩时映射到
 * __kcsan_* runtime；弱内存检测关闭时为空。无论哪种都只服务检测器，真实硬件
 * 屏障仍由 asm/barrier.h 执行。
 */

/**
 * __kcsan_check_read - check regular read access for races
 *
 * @ptr: address of access
 * @size: size of access
 */
#define __kcsan_check_read(ptr, size) __kcsan_check_access(ptr, size, 0)

/* __kcsan_check_read() 强制检查借用地址 @ptr 上 @size 字节普通读，不执行该读本身。 */

/**
 * __kcsan_check_write - check regular write access for races
 *
 * @ptr: address of access
 * @size: size of access
 */
#define __kcsan_check_write(ptr, size)                                         \
	__kcsan_check_access(absolute_pointer(ptr), size, KCSAN_ACCESS_WRITE)

/* 强制普通写检查；absolute_pointer 隐藏对象关系，避免编译器错误推导检查访问本身。 */

/**
 * __kcsan_check_read_write - check regular read-write access for races
 *
 * @ptr: address of access
 * @size: size of access
 */
#define __kcsan_check_read_write(ptr, size)                                    \
	__kcsan_check_access(ptr, size, KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE)

/* 强制复合读改写检查，同时设置 COMPOUND 与 WRITE。 */

/**
 * kcsan_check_read - check regular read access for races
 *
 * @ptr: address of access
 * @size: size of access
 */
#define kcsan_check_read(ptr, size) kcsan_check_access(ptr, size, 0)

/**
 * kcsan_check_write - check regular write access for races
 *
 * @ptr: address of access
 * @size: size of access
 */
#define kcsan_check_write(ptr, size)                                           \
	kcsan_check_access(absolute_pointer(ptr), size, KCSAN_ACCESS_WRITE)

/**
 * kcsan_check_read_write - check regular read-write access for races
 *
 * @ptr: address of access
 * @size: size of access
 */
#define kcsan_check_read_write(ptr, size)                                      \
	kcsan_check_access(ptr, size, KCSAN_ACCESS_COMPOUND | KCSAN_ACCESS_WRITE)

/* 无双下划线的 read/write/read_write 语义相同，但只在当前单元启用自动插桩时生效。 */

/*
 * Check for atomic accesses: if atomic accesses are not ignored, this simply
 * aliases to kcsan_check_access(), otherwise becomes a no-op.
 */
/*
 * 原子访问检查：CONFIG_KCSAN_IGNORE_ATOMICS=y 时全部为空；否则设置 ATOMIC，
 * 并按 read/write/compound 组合 type。它验证检测器模型，不让非原子机器访问
 * 变成原子，也不替代 atomic_t/锁。
 */
#ifdef CONFIG_KCSAN_IGNORE_ATOMICS
#define kcsan_check_atomic_read(...)		do { } while (0)
#define kcsan_check_atomic_write(...)		do { } while (0)
#define kcsan_check_atomic_read_write(...)	do { } while (0)
#else
#define kcsan_check_atomic_read(ptr, size)                                     \
	kcsan_check_access(ptr, size, KCSAN_ACCESS_ATOMIC)
#define kcsan_check_atomic_write(ptr, size)                                    \
	kcsan_check_access(absolute_pointer(ptr), size, KCSAN_ACCESS_ATOMIC | KCSAN_ACCESS_WRITE)
#define kcsan_check_atomic_read_write(ptr, size)                               \
	kcsan_check_access(ptr, size, KCSAN_ACCESS_ATOMIC | KCSAN_ACCESS_WRITE | KCSAN_ACCESS_COMPOUND)
#endif

/**
 * ASSERT_EXCLUSIVE_WRITER - assert no concurrent writes to @var
 *
 * Assert that there are no concurrent writes to @var; other readers are
 * allowed. This assertion can be used to specify properties of concurrent code,
 * where violation cannot be detected as a normal data race.
 *
 * For example, if we only have a single writer, but multiple concurrent
 * readers, to avoid data races, all these accesses must be marked; even
 * concurrent marked writes racing with the single writer are bugs.
 * Unfortunately, due to being marked, they are no longer data races. For cases
 * like these, we can use the macro as follows:
 *
 * .. code-block:: c
 *
 *	void writer(void) {
 *		spin_lock(&update_foo_lock);
 *		ASSERT_EXCLUSIVE_WRITER(shared_foo);
 *		WRITE_ONCE(shared_foo, ...);
 *		spin_unlock(&update_foo_lock);
 *	}
 *	void reader(void) {
 *		// update_foo_lock does not need to be held!
 *		... = READ_ONCE(shared_foo);
 *	}
 *
 * Note: ASSERT_EXCLUSIVE_WRITER_SCOPED(), if applicable, performs more thorough
 * checking if a clear scope where no concurrent writes are expected exists.
 *
 * @var: variable to assert on
 */
/*
 * ASSERT_EXCLUSIVE_WRITER(@var) 声明此刻不应有并发写者，但允许读者。典型场景
 * 是所有访问都被 READ_ONCE/WRITE_ONCE 标记，普通竞态检测已看不出“单写者”
 * 约束；显式 ASSERT access 可恢复该不变量检查。它只采样一个时点，存在清晰
 * 作用域时 scoped 版本覆盖更完整。@var 不转移 ownership。
 */
#define ASSERT_EXCLUSIVE_WRITER(var)                                           \
	__kcsan_check_access(&(var), sizeof(var), KCSAN_ACCESS_ASSERT)

/*
 * Helper macros for implementation of for ASSERT_EXCLUSIVE_*_SCOPED(). @id is
 * expected to be unique for the scope in which instances of kcsan_scoped_access
 * are declared.
 */
/*
 * scoped helper 用 __COUNTER__ 生成作用域唯一的栈变量名，cleanup 属性保证所有
 * 正常控制流出口自动 end；dummy 指针承接 begin 返回值。@id 只需在当前作用域唯一。
 */
#define __kcsan_scoped_name(c, suffix) __kcsan_scoped_##c##suffix
#define __ASSERT_EXCLUSIVE_SCOPED(var, type, id)                               \
	struct kcsan_scoped_access __kcsan_scoped_name(id, _)                  \
		__kcsan_cleanup_scoped;                                        \
	struct kcsan_scoped_access *__kcsan_scoped_name(id, _dummy_p)          \
		__maybe_unused = kcsan_begin_scoped_access(                    \
			&(var), sizeof(var), KCSAN_ACCESS_SCOPED | (type),     \
			&__kcsan_scoped_name(id, _))

/**
 * ASSERT_EXCLUSIVE_WRITER_SCOPED - assert no concurrent writes to @var in scope
 *
 * Scoped variant of ASSERT_EXCLUSIVE_WRITER().
 *
 * Assert that there are no concurrent writes to @var for the duration of the
 * scope in which it is introduced. This provides a better way to fully cover
 * the enclosing scope, compared to multiple ASSERT_EXCLUSIVE_WRITER(), and
 * increases the likelihood for KCSAN to detect racing accesses.
 *
 * For example, it allows finding race-condition bugs that only occur due to
 * state changes within the scope itself:
 *
 * .. code-block:: c
 *
 *	void writer(void) {
 *		spin_lock(&update_foo_lock);
 *		{
 *			ASSERT_EXCLUSIVE_WRITER_SCOPED(shared_foo);
 *			WRITE_ONCE(shared_foo, 42);
 *			...
 *			// shared_foo should still be 42 here!
 *		}
 *		spin_unlock(&update_foo_lock);
 *	}
 *	void buggy(void) {
 *		if (READ_ONCE(shared_foo) == 42)
 *			WRITE_ONCE(shared_foo, 1); // bug!
 *	}
 *
 * @var: variable to assert on
 */
/*
 * ASSERT_EXCLUSIVE_WRITER_SCOPED(@var) 在声明点至词法作用域结束持续断言没有并发
 * 写，能发现断言后状态又被错误写回的竞态。示例中 buggy() 在值为 42 时改写，
 * 即使单点断言未撞上，持续检查也更可能发现；读者仍被允许。
 */
#define ASSERT_EXCLUSIVE_WRITER_SCOPED(var)                                    \
	__ASSERT_EXCLUSIVE_SCOPED(var, KCSAN_ACCESS_ASSERT, __COUNTER__)

/**
 * ASSERT_EXCLUSIVE_ACCESS - assert no concurrent accesses to @var
 *
 * Assert that there are no concurrent accesses to @var (no readers nor
 * writers). This assertion can be used to specify properties of concurrent
 * code, where violation cannot be detected as a normal data race.
 *
 * For example, where exclusive access is expected after determining no other
 * users of an object are left, but the object is not actually freed. We can
 * check that this property actually holds as follows:
 *
 * .. code-block:: c
 *
 *	if (refcount_dec_and_test(&obj->refcnt)) {
 *		ASSERT_EXCLUSIVE_ACCESS(*obj);
 *		do_some_cleanup(obj);
 *		release_for_reuse(obj);
 *	}
 *
 * Note:
 *
 * 1. ASSERT_EXCLUSIVE_ACCESS_SCOPED(), if applicable, performs more thorough
 *    checking if a clear scope where no concurrent accesses are expected exists.
 *
 * 2. For cases where the object is freed, `KASAN <kasan.html>`_ is a better
 *    fit to detect use-after-free bugs.
 *
 * @var: variable to assert on
 */
/*
 * ASSERT_EXCLUSIVE_ACCESS(@var) 要求既无读者也无写者，适合引用计数归零但对象
 * 尚未释放、准备清理复用的时点。若对象真的被 free，检测 UAF 应交给 KASAN；
 * 有明确清理区间时 scoped 版本更完整。
 */
#define ASSERT_EXCLUSIVE_ACCESS(var)                                           \
	__kcsan_check_access(&(var), sizeof(var), KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ASSERT)

/**
 * ASSERT_EXCLUSIVE_ACCESS_SCOPED - assert no concurrent accesses to @var in scope
 *
 * Scoped variant of ASSERT_EXCLUSIVE_ACCESS().
 *
 * Assert that there are no concurrent accesses to @var (no readers nor writers)
 * for the entire duration of the scope in which it is introduced. This provides
 * a better way to fully cover the enclosing scope, compared to multiple
 * ASSERT_EXCLUSIVE_ACCESS(), and increases the likelihood for KCSAN to detect
 * racing accesses.
 *
 * @var: variable to assert on
 */
/* ASSERT_EXCLUSIVE_ACCESS_SCOPED 在整个词法作用域持续禁止任何并发访问，并由 cleanup 自动结束。 */
#define ASSERT_EXCLUSIVE_ACCESS_SCOPED(var)                                    \
	__ASSERT_EXCLUSIVE_SCOPED(var, KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ASSERT, __COUNTER__)

/**
 * ASSERT_EXCLUSIVE_BITS - assert no concurrent writes to subset of bits in @var
 *
 * Bit-granular variant of ASSERT_EXCLUSIVE_WRITER().
 *
 * Assert that there are no concurrent writes to a subset of bits in @var;
 * concurrent readers are permitted. This assertion captures more detailed
 * bit-level properties, compared to the other (word granularity) assertions.
 * Only the bits set in @mask are checked for concurrent modifications, while
 * ignoring the remaining bits, i.e. concurrent writes (or reads) to ~mask bits
 * are ignored.
 *
 * Use this for variables, where some bits must not be modified concurrently,
 * yet other bits are expected to be modified concurrently.
 *
 * For example, variables where, after initialization, some bits are read-only,
 * but other bits may still be modified concurrently. A reader may wish to
 * assert that this is true as follows:
 *
 * .. code-block:: c
 *
 *	ASSERT_EXCLUSIVE_BITS(flags, READ_ONLY_MASK);
 *	foo = (READ_ONCE(flags) & READ_ONLY_MASK) >> READ_ONLY_SHIFT;
 *
 * Note: The access that immediately follows ASSERT_EXCLUSIVE_BITS() is assumed
 * to access the masked bits only, and KCSAN optimistically assumes it is
 * therefore safe, even in the presence of data races, and marking it with
 * READ_ONCE() is optional from KCSAN's point-of-view. We caution, however, that
 * it may still be advisable to do so, since we cannot reason about all compiler
 * optimizations when it comes to bit manipulations (on the reader and writer
 * side). If you are sure nothing can go wrong, we can write the above simply
 * as:
 *
 * .. code-block:: c
 *
 *	ASSERT_EXCLUSIVE_BITS(flags, READ_ONLY_MASK);
 *	foo = (flags & READ_ONLY_MASK) >> READ_ONLY_SHIFT;
 *
 * Another example, where this may be used, is when certain bits of @var may
 * only be modified when holding the appropriate lock, but other bits may still
 * be modified concurrently. Writers, where other bits may change concurrently,
 * could use the assertion as follows:
 *
 * .. code-block:: c
 *
 *	spin_lock(&foo_lock);
 *	ASSERT_EXCLUSIVE_BITS(flags, FOO_MASK);
 *	old_flags = flags;
 *	new_flags = (old_flags & ~FOO_MASK) | (new_foo << FOO_SHIFT);
 *	if (cmpxchg(&flags, old_flags, new_flags) != old_flags) { ... }
 *	spin_unlock(&foo_lock);
 *
 * @var: variable to assert on
 * @mask: only check for modifications to bits set in @mask
 */
/*
 * ASSERT_EXCLUSIVE_BITS(@var,@mask) 只断言 mask 位没有并发写，允许 ~mask 位变化和
 * 所有读者。宏先安装当前上下文 mask、执行 ASSERT、立即恢复为 0，再把下一次
 * 真实访问视为原子；因此必须紧邻只访问 masked bits 的语句。KCSAN 允许省略
 * READ_ONCE 只是检测器视角，编译器位操作仍可能需要 READ_ONCE。所有出口恢复
 * mask 是硬性不变量；宏不提供锁，写者示例仍用锁+cmpxchg 合并其他位的变化。
 */
#define ASSERT_EXCLUSIVE_BITS(var, mask)                                       \
	do {                                                                   \
		kcsan_set_access_mask(mask);                                   \
		__kcsan_check_access(&(var), sizeof(var), KCSAN_ACCESS_ASSERT);\
		kcsan_set_access_mask(0);                                      \
		kcsan_atomic_next(1);                                          \
	} while (0)

#endif /* _LINUX_KCSAN_CHECKS_H */
