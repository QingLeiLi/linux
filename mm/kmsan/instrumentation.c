// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN compiler API.
 *
 * This file implements __msan_XXX hooks that Clang inserts into the code
 * compiled with -fsanitize=kernel-memory.
 * See Documentation/dev-tools/kmsan.rst for more information on how KMSAN
 * instrumentation works.
 *
 * Copyright (C) 2017-2022 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */
/*
 * KMSAN 编译器接口层。
 *
 * Clang 以 -fsanitize=kernel-memory 编译内核代码时，会把 metadata 查询、
 * 内存 intrinsic、栈变量生灭和未初始化值使用改写为本文件的 __msan_* 调用。
 * 本层先完成真实数据访问，再在 KMSAN runtime 递归门禁内同步 shadow/origin；
 * 它不拥有被检测内存，metadata backing 的生命周期由 KMSAN shadow 层管理。
 * 详细插桩模型参见 Documentation/dev-tools/kmsan.rst。
 */

#include "kmsan.h"
#include <linux/gfp.h>
#include <linux/kmsan.h>
#include <linux/kmsan_string.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

/*
 * is_bad_asm_addr() - 判断 inline asm 输出是否能安全更新 KMSAN metadata。
 * @addr/@size 描述借用的写入区间，@is_store 保留编译器 ABI 语义，本实现不据此分支。
 * 返回 true 表示用户地址或无 shadow backing，调用者应跳过；false 才可继续反毒。
 * 无锁、不可睡眠，也不取得地址或 metadata ownership。
 */
static inline bool is_bad_asm_addr(void *addr, uintptr_t size, bool is_store)
{
	/* 独立地址空间架构上，低于 TASK_SIZE 的指针属于用户地址，不能按内核地址处理。 */
	if (IS_ENABLED(CONFIG_ARCH_HAS_NON_OVERLAPPING_ADDRESS_SPACE) &&
	    (u64)addr < TASK_SIZE)
		return true;
	if (!kmsan_get_metadata(addr, KMSAN_META_SHADOW))
		return true;
	return false;
}

/*
 * get_shadow_origin_ptr() - 在临时开放用户访问的窗口取得一次访存的 metadata 对。
 * @addr/@size 是编译器即将访问的借用区间，@store 区分读 metadata 与写 metadata。
 * 返回的 shadow/origin 均为借用指针，可能指向 dummy page，调用者不得释放。
 * 保存并恢复当前访问状态，避免 metadata 查询被 SMAP/PAN 一类机制误拦截。
 */
static inline struct shadow_origin_ptr
get_shadow_origin_ptr(void *addr, u64 size, bool store)
{
	/* ua_flags 只在本调用内有效，必须与 restore 严格配对。 */
	unsigned long ua_flags = user_access_save();
	struct shadow_origin_ptr ret;

	ret = kmsan_get_shadow_origin_ptr(addr, size, store);
	user_access_restore(ua_flags);
	return ret;
}

/*
 * KMSAN instrumentation functions follow. They are not declared elsewhere in
 * the kernel code, so they are preceded by prototypes, to silence
 * -Wmissing-prototypes warnings.
 */
/*
 * 以下是 KMSAN 插桩函数。它们不是由普通内核源码调用并统一声明的 API，
 * 因而每个导出定义前保留显式原型，以满足 -Wmissing-prototypes。
 */

/* Get shadow and origin pointers for a memory load with non-standard size. */
/* 为非标准长度 load 取得 shadow/origin 指针。 */
struct shadow_origin_ptr __msan_metadata_ptr_for_load_n(void *addr,
							uintptr_t size);
/*
 * __msan_metadata_ptr_for_load_n() - 服务变长内存读取插桩。
 * @addr/@size 描述借用读取区间；返回其可读 metadata 对，不转移 ownership。
 * 不报告错误；不可跟踪范围由底层折叠为 load dummy metadata。
 */
struct shadow_origin_ptr __msan_metadata_ptr_for_load_n(void *addr,
							uintptr_t size)
{
	return get_shadow_origin_ptr(addr, size, /*store*/ false);
}
EXPORT_SYMBOL(__msan_metadata_ptr_for_load_n);

/* Get shadow and origin pointers for a memory store with non-standard size. */
/* 为非标准长度 store 取得 shadow/origin 指针。 */
struct shadow_origin_ptr __msan_metadata_ptr_for_store_n(void *addr,
							 uintptr_t size);
/*
 * __msan_metadata_ptr_for_store_n() - 服务变长内存写入插桩。
 * @addr/@size 描述借用写入区间；返回其可写 metadata 对，不转移 ownership。
 * 不可跟踪范围使用丢弃写入的 store dummy metadata。
 */
struct shadow_origin_ptr __msan_metadata_ptr_for_store_n(void *addr,
							 uintptr_t size)
{
	return get_shadow_origin_ptr(addr, size, /*store*/ true);
}
EXPORT_SYMBOL(__msan_metadata_ptr_for_store_n);

/*
 * Declare functions that obtain shadow/origin pointers for loads and stores
 * with fixed size.
 */
/*
 * 为固定 1/2/4/8 字节 load/store 生成与变长版本相同的 ABI：load 读取
 * metadata，store 更新 metadata；每个生成函数只借用 @addr 并按值返回指针对。
 * 宏同时生成前置原型和导出符号，不能把续行拆开单独注释。
 */
#define DECLARE_METADATA_PTR_GETTER(size)                                  \
	struct shadow_origin_ptr __msan_metadata_ptr_for_load_##size(      \
		void *addr);                                               \
	struct shadow_origin_ptr __msan_metadata_ptr_for_load_##size(      \
		void *addr)                                                \
	{                                                                  \
		return get_shadow_origin_ptr(addr, size, /*store*/ false); \
	}                                                                  \
	/* load 包装结束；下面导出并生成 store 包装。 */                 \
	EXPORT_SYMBOL(__msan_metadata_ptr_for_load_##size);                \
	struct shadow_origin_ptr __msan_metadata_ptr_for_store_##size(     \
		void *addr);                                               \
	struct shadow_origin_ptr __msan_metadata_ptr_for_store_##size(     \
		void *addr)                                                \
	{                                                                  \
		/* store=true 使不可跟踪写落到可丢弃的 dummy metadata。 */ \
		return get_shadow_origin_ptr(addr, size, /*store*/ true);  \
	}                                                                  \
	EXPORT_SYMBOL(__msan_metadata_ptr_for_store_##size)

DECLARE_METADATA_PTR_GETTER(1);
DECLARE_METADATA_PTR_GETTER(2);
DECLARE_METADATA_PTR_GETTER(4);
DECLARE_METADATA_PTR_GETTER(8);

/*
 * Handle a memory store performed by inline assembly. KMSAN conservatively
 * attempts to unpoison the outputs of asm() directives to prevent false
 * positives caused by missed stores.
 *
 * __msan_instrument_asm_store() may be called for inline assembly code when
 * entering or leaving IRQ. We omit the check for kmsan_in_runtime() to ensure
 * the memory written to in these cases is also marked as initialized.
 */
/*
 * 处理 inline asm 执行的内存写入。KMSAN 无法看见汇编内部 store，因此保守地把
 * asm 输出反毒，避免把“漏插桩的真实写入”误报成未初始化。
 * IRQ 进入/退出也可能调用本函数；故意不检查 kmsan_in_runtime()，保证这些写入
 * 同样发布为已初始化状态。
 */
void __msan_instrument_asm_store(void *addr, uintptr_t size);
/*
 * __msan_instrument_asm_store() - 尽力同步 inline asm 输出的 shadow 状态。
 * @addr/@size 是 asm 已写入的借用区间；无返回值，不取得 ownership。
 * KMSAN 关闭、地址不可跟踪时静默跳过；异常大长度告警一次并只处理 8 字节。
 * 可在 IRQ 边界调用，不睡眠；user_access 状态在所有已保存路径上恢复。
 */
void __msan_instrument_asm_store(void *addr, uintptr_t size)
{
	unsigned long ua_flags;

	if (!kmsan_enabled)
		return;

	ua_flags = user_access_save();
	/*
	 * Most of the accesses are below 32 bytes. The exceptions so far are
	 * clwb() (64 bytes), FPU state (512 bytes) and chsc() (4096 bytes).
	 */
	/*
	 * 多数访问小于 32 字节；已知例外是 clwb() 的 64 字节、FPU 状态的
	 * 512 字节和 chsc() 的 4096 字节，故 4096 是本接口的防御上界。
	 */
	if (size > 4096) {
		WARN_ONCE(1, "assembly store size too big: %ld\n", size);
		size = 8;
	}
	if (is_bad_asm_addr(addr, size, /*is_store*/ true)) {
		user_access_restore(ua_flags);
		return;
	}
	/* Unpoisoning the memory on best effort. */
	/* 这里只能尽力反毒：跳过不可跟踪范围不会改变真实 asm 写入结果。 */
	kmsan_internal_unpoison_memory(addr, size, /*checked*/ false);
	user_access_restore(ua_flags);
}
EXPORT_SYMBOL(__msan_instrument_asm_store);

/*
 * KMSAN instrumentation pass replaces LLVM memcpy, memmove and memset
 * intrinsics with calls to respective __msan_ functions. We use
 * get_param0_metadata() and set_retval_metadata() to store the shadow/origin
 * values for the destination argument of these functions and use them for the
 * functions' return values.
 */
/*
 * KMSAN 插桩把 LLVM memcpy/memmove/memset intrinsic 替换为对应 __msan_*
 * 函数。目的指针也是函数返回值，因此先从当前执行上下文的参数 TLS 保存它的
 * shadow/origin，真实复制与 metadata 更新结束后再发布到返回值 TLS。
 */
/*
 * get_param0_metadata() - 读取第 0 个参数（目的指针）的调用边 metadata。
 * @shadow/@origin 是调用者提供的出参，函数从当前 task/per-CPU KMSAN context 借读。
 * 无失败返回、无分配；仅在紧随其后的返回值 metadata 发布前有效。
 */
static inline void get_param0_metadata(u64 *shadow,
				       depot_stack_handle_t *origin)
{
	struct kmsan_ctx *ctx = kmsan_get_context();

	*shadow = *(u64 *)(ctx->cstate.param_tls);
	*origin = ctx->cstate.param_origin_tls[0];
}

/*
 * set_retval_metadata() - 把保存的目的指针 metadata 写入当前返回值 TLS。
 * @shadow/@origin 按值输入；无返回值，不拥有 stack-depot handle。
 * 这是插桩调用者观察到返回指针 taint 的发布点。
 */
static inline void set_retval_metadata(u64 shadow, depot_stack_handle_t origin)
{
	struct kmsan_ctx *ctx = kmsan_get_context();

	*(u64 *)(ctx->cstate.retval_tls) = shadow;
	ctx->cstate.retval_origin_tls = origin;
}

/* Handle llvm.memmove intrinsic. */
/* 处理 llvm.memmove intrinsic。 */
void *__msan_memmove(void *dst, const void *src, uintptr_t n);
/*
 * __msan_memmove() - 完成可重叠字节移动并同步 shadow/origin。
 * @dst 为借用目的区，@src 为借用源区，@n 为长度；返回 @dst，不转移 ownership。
 * 先执行真实 __memmove；零长度或 KMSAN 关闭/递归时保留数据结果并跳过 metadata。
 * 正常路径在 runtime 门禁内按重叠方向复制 metadata，最后发布返回值 metadata。
 */
void *__msan_memmove(void *dst, const void *src, uintptr_t n)
{
	depot_stack_handle_t origin;
	void *result;
	u64 shadow;

	get_param0_metadata(&shadow, &origin);
	result = __memmove(dst, src, n);
	if (!n)
		/* Some people call memmove() with zero length. */
		/* 部分调用者会传入零长度；此时不要求地址可访问，也不触碰 metadata。 */
		return result;
	if (!kmsan_enabled || kmsan_in_runtime())
		return result;

	kmsan_enter_runtime();
	kmsan_internal_memmove_metadata(dst, (void *)src, n);
	kmsan_leave_runtime();

	set_retval_metadata(shadow, origin);
	return result;
}
EXPORT_SYMBOL(__msan_memmove);

/* Handle llvm.memcpy intrinsic. */
/* 处理 llvm.memcpy intrinsic。 */
void *__msan_memcpy(void *dst, const void *src, uintptr_t n);
/*
 * __msan_memcpy() - 完成非重叠复制并传播 KMSAN metadata。
 * @dst/@src 为借用且按 memcpy 约定不得重叠，@n 为长度；返回 @dst。
 * 数据复制始终先完成；零长度、关闭或 runtime 递归路径不再更新 metadata。
 * metadata helper 采用更宽松的 memmove 方向语义，不改变 memcpy 的数据契约。
 */
void *__msan_memcpy(void *dst, const void *src, uintptr_t n)
{
	depot_stack_handle_t origin;
	void *result;
	u64 shadow;

	get_param0_metadata(&shadow, &origin);
	result = __memcpy(dst, src, n);
	if (!n)
		/* Some people call memcpy() with zero length. */
		/* 零长度调用合法；真实 intrinsic 已返回，不能再查询可能无效的地址。 */
		return result;

	if (!kmsan_enabled || kmsan_in_runtime())
		return result;

	kmsan_enter_runtime();
	/* Using memmove instead of memcpy doesn't affect correctness. */
	/* metadata 的逐字节传播允许重叠，使用 memmove 语义不会削弱 memcpy 正确性。 */
	kmsan_internal_memmove_metadata(dst, (void *)src, n);
	kmsan_leave_runtime();

	set_retval_metadata(shadow, origin);
	return result;
}
EXPORT_SYMBOL(__msan_memcpy);

/* Handle llvm.memset intrinsic. */
/* 处理 llvm.memset intrinsic。 */
void *__msan_memset(void *dst, int c, uintptr_t n);
/*
 * __msan_memset() - 填充真实字节并把目的区标记为已初始化。
 * @dst 为借用目的区，@c 是填充值，@n 是长度；返回 @dst，不转移 ownership。
 * 真实 __memset 先执行；KMSAN 关闭/递归时只保留数据结果，否则在门禁内反毒。
 * 返回值沿用第 0 个参数的 metadata，而非整数 @c 的 metadata。
 */
void *__msan_memset(void *dst, int c, uintptr_t n)
{
	depot_stack_handle_t origin;
	void *result;
	u64 shadow;

	get_param0_metadata(&shadow, &origin);
	result = __memset(dst, c, n);
	if (!kmsan_enabled || kmsan_in_runtime())
		return result;

	kmsan_enter_runtime();
	/*
	 * Clang doesn't pass parameter metadata here, so it is impossible to
	 * use shadow of @c to set up the shadow for @dst.
	 */
	/*
	 * Clang 在这里不传 @c 的参数 metadata，无法用 @c 的 shadow 构造 @dst；
	 * 既然 memset 已真实写满目的区，运行时将其视为已初始化。
	 */
	kmsan_internal_unpoison_memory(dst, n, /*checked*/ false);
	kmsan_leave_runtime();

	set_retval_metadata(shadow, origin);
	return result;
}
EXPORT_SYMBOL(__msan_memset);

/*
 * Create a new origin from an old one. This is done when storing an
 * uninitialized value to memory. When reporting an error, KMSAN unrolls and
 * prints the whole chain of stores that preceded the use of this value.
 */
/*
 * 从旧 origin 创建传播节点；每次未初始化值被写入内存时记录一次 store，最终报告
 * 沿链回放该值到达使用点之前的完整传播历史。
 */
depot_stack_handle_t __msan_chain_origin(depot_stack_handle_t origin);
/*
 * __msan_chain_origin() - 为一次未初始化值传播扩展 origin 链。
 * @origin 是借用的旧 stack-depot handle；成功返回新 handle，禁用或递归返回 0。
 * 创建节点可能分配，故临时进入 runtime 并保存/恢复 user_access 状态；返回值
 * 由 metadata 记录长期引用，本函数不显式释放 depot 条目。
 */
depot_stack_handle_t __msan_chain_origin(depot_stack_handle_t origin)
{
	/* 0 既是初始值，也是无法记录新传播节点时的中性 origin。 */
	depot_stack_handle_t ret = 0;
	unsigned long ua_flags;

	if (!kmsan_enabled || kmsan_in_runtime())
		return ret;

	ua_flags = user_access_save();

	/* Creating new origins may allocate memory. */
	/* 创建新 origin 可能分配内存，必须以 runtime 门禁阻止分配路径再次插桩递归。 */
	kmsan_enter_runtime();
	ret = kmsan_internal_chain_origin(origin);
	kmsan_leave_runtime();
	user_access_restore(ua_flags);
	return ret;
}
EXPORT_SYMBOL(__msan_chain_origin);

/* Poison a local variable when entering a function. */
/* 进入函数时把一个局部变量标记为未初始化。 */
void __msan_poison_alloca(void *address, uintptr_t size, char *descr);
/*
 * __msan_poison_alloca() - 建立栈局部变量的初始 poison 与来源描述。
 * @address/@size 是当前栈帧内借用区间，@descr 是编译器提供、只借读的描述字符串。
 * 无返回值；关闭/递归时不改 metadata，成功时保存合成栈并把全区 shadow 置毒。
 * stack depot 可能分配，runtime 与 user_access 状态均严格配对；checked=true 要求
 * 地址确实可跟踪，局部变量退出时由 __msan_unpoison_alloca() 清理状态。
 */
void __msan_poison_alloca(void *address, uintptr_t size, char *descr)
{
	/* handle 被写入 origin metadata；entries 只是在当前栈上的临时编码数组。 */
	depot_stack_handle_t handle;
	unsigned long entries[4];
	unsigned long ua_flags;

	if (!kmsan_enabled || kmsan_in_runtime())
		return;

	ua_flags = user_access_save();
	entries[0] = KMSAN_ALLOCA_MAGIC_ORIGIN;
	/* 描述、直接调用者和可选上一帧共同构成可由报告器识别的 alloca origin。 */
	entries[1] = (u64)descr;
	entries[2] = (u64)__builtin_return_address(0);
	/*
	 * With frame pointers enabled, it is possible to quickly fetch the
	 * second frame of the caller stack without calling the unwinder.
	 * Without them, simply do not bother.
	 */
	/*
	 * 启用 frame pointer 时无需调用 unwinder 就能快速取调用者的第二帧；
	 * 未启用时不冒险猜测，直接把该槽记为 0。
	 */
	if (IS_ENABLED(CONFIG_UNWINDER_FRAME_POINTER))
		entries[3] = (u64)__builtin_return_address(1);
	else
		entries[3] = 0;

	/* stack_depot_save() may allocate memory. */
	/* stack_depot_save() 可能分配，保存期间进入 runtime 以切断 KMSAN 自递归。 */
	kmsan_enter_runtime();
	handle = stack_depot_save(entries, ARRAY_SIZE(entries), __GFP_HIGH);
	kmsan_leave_runtime();

	kmsan_internal_set_shadow_origin(address, size, -1, handle,
					 /*checked*/ true);
	user_access_restore(ua_flags);
}
EXPORT_SYMBOL(__msan_poison_alloca);

/* Unpoison a local variable. */
/* 局部变量生命周期结束时将其标记为已初始化，避免旧栈 poison 泄漏到复用槽位。 */
void __msan_unpoison_alloca(void *address, uintptr_t size);
/*
 * __msan_unpoison_alloca() - 清除离开作用域的栈变量 metadata。
 * @address/@size 是借用栈区间；无返回值，不改变真实字节或 ownership。
 * 关闭/递归时静默跳过；正常路径在 runtime 门禁内以 checked=true 反毒。
 */
void __msan_unpoison_alloca(void *address, uintptr_t size)
{
	if (!kmsan_enabled || kmsan_in_runtime())
		return;

	kmsan_enter_runtime();
	kmsan_internal_unpoison_memory(address, size, /*checked*/ true);
	kmsan_leave_runtime();
}
EXPORT_SYMBOL(__msan_unpoison_alloca);

/*
 * Report that an uninitialized value with the given origin was used in a way
 * that constituted undefined behavior.
 */
/* 报告给定 origin 的未初始化值已被用于构成未定义行为的操作。 */
void __msan_warning(u32 origin);
/*
 * __msan_warning() - 把编译器检测到的标量未初始化值使用交给报告器。
 * @origin 是借用 stack-depot handle；无具体地址/范围，因此其余位置参数均为 0/NULL。
 * 无返回值；报告器负责递归、串行、限流及 panic 策略，本层不改变 metadata。
 */
void __msan_warning(u32 origin)
{
	kmsan_report(origin, /*address*/ NULL, /*size*/ 0,
		     /*off_first*/ 0, /*off_last*/ 0, /*user_addr*/ NULL,
		     REASON_ANY);
}
EXPORT_SYMBOL(__msan_warning);

/*
 * At the beginning of an instrumented function, obtain the pointer to
 * `struct kmsan_context_state` holding the metadata for function parameters.
 */
/* 插桩函数入口取得保存参数 metadata 的 struct kmsan_context_state 指针。 */
struct kmsan_context_state *__msan_get_context_state(void);
/*
 * __msan_get_context_state() - 返回当前执行上下文的调用边 metadata 状态。
 * 无参数；返回 task 或 IRQ per-CPU kmsan_ctx 内部的借用指针，调用者不得释放或缓存
 * 到上下文切换之外。无失败返回、无分配，具体上下文选择由 kmsan_get_context() 完成。
 */
struct kmsan_context_state *__msan_get_context_state(void)
{
	return &kmsan_get_context()->cstate;
}
EXPORT_SYMBOL(__msan_get_context_state);
