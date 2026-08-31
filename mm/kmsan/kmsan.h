/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Functions used by the KMSAN runtime.
 *
 * Copyright (C) 2017-2022 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */
/* 本头文件汇总 KMSAN 编译器插桩与非插桩 runtime 之间的内部契约。 */

#ifndef __MM_KMSAN_KMSAN_H
#define __MM_KMSAN_KMSAN_H
/* 保护宏保证内部类型、常量与 always-inline 递归防护只定义一次。 */

#include <linux/irqflags.h>
#include <linux/kmsan.h>
#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/pgtable.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>

/* alloca origin 的 stack-depot 伪栈首项；报告器据此识别栈对象初始化来源。 */
#define KMSAN_ALLOCA_MAGIC_ORIGIN 0xabcd0100
/* origin 链节点的伪栈首项；其后保存当前复制栈与上一条 origin handle。 */
#define KMSAN_CHAIN_MAGIC_ORIGIN 0xabcd0200

/* 只把内存标毒为未初始化；metadata 不可翻译时不额外告警。 */
#define KMSAN_POISON_NOCHECK 0x0
/* 要求 metadata 可翻译；失败会发 KMSAN 警告，适合应受跟踪的分配。 */
#define KMSAN_POISON_CHECK 0x1
/* 低位之外的 UAF 标志；记录“由释放产生的毒”，可与 CHECK 按位组合。 */
#define KMSAN_POISON_FREE 0x2

/* 一个 origin handle 覆盖连续 4 字节 shadow；地址按此粒度向下对齐。 */
#define KMSAN_ORIGIN_SIZE 4
/* 复制 origin 链最多保留 7 层，避免含未初始化 padding 的重复复制无限增长。 */
#define KMSAN_MAX_ORIGIN_DEPTH 7

/* 采集创建/传播 origin 时最多保存 64 个内核栈地址。 */
#define KMSAN_STACK_DEPTH 64

/* 传给 metadata 查询的布尔选择器：false 选择逐字节 shadow。 */
#define KMSAN_META_SHADOW (false)
/* 传给 metadata 查询的布尔选择器：true 选择 4 字节粒度 origin。 */
#define KMSAN_META_ORIGIN (true)

/*
 * A pair of metadata pointers to be returned by the instrumentation functions.
 */
/*
 * struct shadow_origin_ptr 把同一被访问地址对应的 shadow/origin 借用指针成对返回。
 * @shadow 每字节表示初始化状态，@origin 每 4 字节保存 stack-depot handle；二者
 * 要么指向真实 metadata，要么同时指向 load/store dummy page。结构体按值创建和
 * 返回，不拥有 backing，也不延长物理页或 vmalloc 映射生命周期。
 */
struct shadow_origin_ptr {
	/* 同一声明中的两个指针都是临时借用；调用者按访问方向读写，不负责释放。 */
	void *shadow, *origin;
};

/*
 * kmsan_get_shadow_origin_ptr() - 为一次插桩访存取得 shadow/origin 地址对。
 * 业务背景：编译器 hook 需以原访存地址/长度访问 metadata。@addr 为借用内核地址，
 * @size 为字节数且不得超过 PAGE_SIZE，@store 指示写 metadata 还是读 metadata。
 * 出参/返回：返回真实借用指针；禁用/不可跟踪时，load 得全零 dummy、store 得丢弃页。
 * 注意事项：要求范围 metadata 连续；dummy 指针不代表原地址可被 KMSAN 跟踪。
 */
struct shadow_origin_ptr kmsan_get_shadow_origin_ptr(void *addr, u64 size,
						     bool store);
/*
 * kmsan_init_alloc_meta_for_range() - 为启动早期已存在的直接映射区间分配 metadata。
 * 业务背景：buddy 接管前用 memblock 为内核 data/pgdat 等补齐 shadow 与 origin。
 * @start/@end 是半开字节区间借用地址，函数页对齐后逐页绑定 metadata page。
 * 出参/返回：无直接返回；memblock 分配失败 panic，成功后 backing 归 KMSAN 长期持有。
 * 注意事项：仅 __init 阶段调用、可改变 memblock 保留布局，运行期不得再调用。
 */
void __init kmsan_init_alloc_meta_for_range(void *start, void *end);

/* 每个枚举值描述“为何检查该未初始化范围”，报告器据此选择 bug 类型与泄漏语境。 */
enum kmsan_bug_reason {
	/* REASON_ANY：普通未初始化值使用；例如分支比较，未必涉及用户态地址。 */
	REASON_ANY,
	/* REASON_COPY_TO_USER：内核字节即将/已经泄漏到用户缓冲区。 */
	REASON_COPY_TO_USER,
	/* REASON_SUBMIT_URB：未初始化字节提交给 USB 设备，属于设备侧信息泄漏。 */
	REASON_SUBMIT_URB,
};

/*
 * kmsan_print_origin() - 沿 stack-depot origin 链打印创建与传播栈。
 * 业务背景：最终报告需把 handle 还原为可读因果链。@origin 是 0 或有效 depot handle。
 * 出参/返回：无直接返回；向内核日志输出，不取得/释放 handle ownership。
 * 注意事项：可由报告锁内路径或连续性诊断直接调用；调用者负责日志串行与 runtime guard，
 * 坏/空栈只输出有限诊断。
 */
void kmsan_print_origin(depot_stack_handle_t origin);

/**
 * kmsan_report() - Report a use of uninitialized value.
 * @origin:    Stack ID of the uninitialized value.
 * @address:   Address at which the memory access happens.
 * @size:      Memory access size.
 * @off_first: Offset (from @address) of the first byte to be reported.
 * @off_last:  Offset (from @address) of the last byte to be reported.
 * @user_addr: When non-NULL, denotes the userspace address to which the kernel
 *             is leaking data.
 * @reason:    Error type from enum kmsan_bug_reason.
 *
 * kmsan_report() prints an error message for a consequent group of bytes
 * sharing the same origin. If an uninitialized value is used in a comparison,
 * this function is called once without specifying the addresses. When checking
 * a memory range, KMSAN may call kmsan_report() multiple times with the same
 * @address, @size, @user_addr and @reason, but different @off_first and
 * @off_last corresponding to different @origin values.
 */
/*
 * 报告连续、共享同一 @origin 的未初始化字节组；普通比较可令 @address=NULL、
 * @size=0，范围检查则用 @off_first..@off_last（相对 @address 的包含式字节偏移）
 * 分段多次调用。@user_addr 非 NULL 时标出泄漏目标，@reason 必须是上述枚举值。
 * 函数无直接返回；可能打印栈、加 TAINT_BAD_PAGE，并按 panic_on_kmsan panic。
 * 它会拒绝禁用、递归、无 origin 或嵌套报告；内部加 raw spinlock 且不可睡眠。
 */
void kmsan_report(depot_stack_handle_t origin, void *address, int size,
		  int off_first, int off_last, const void __user *user_addr,
		  enum kmsan_bug_reason reason);

/*
 * 中断/NMI 不能使用被中断任务的 current->kmsan_ctx，因而每个 CPU 常驻一份
 * kmsan_percpu_ctx；非任务上下文在 CPU 固定期间借用它，不取得独立 ownership。
 */
DECLARE_PER_CPU(struct kmsan_ctx, kmsan_percpu_ctx);

/*
 * kmsan_get_context() - 选择当前执行上下文应使用的 KMSAN 递归/参数状态。
 * 业务背景：任务上下文状态随 task 迁移，中断状态则必须按当前 CPU 隔离。
 * 入参：无。出参/返回：in_task() 时返回借用 current->kmsan_ctx，否则返回 raw per-CPU ctx。
 * 注意事项：非任务调用点天然处于 CPU 固定窗口；返回指针不能跨任务或 CPU 保存。
 */
static __always_inline struct kmsan_ctx *kmsan_get_context(void)
{
	return in_task() ? &current->kmsan_ctx : raw_cpu_ptr(&kmsan_percpu_ctx);
}

/*
 * When a compiler hook or KMSAN runtime function is invoked, it may make a
 * call to instrumented code and eventually call itself recursively. To avoid
 * that, we guard the runtime entry regions with
 * kmsan_enter_runtime()/kmsan_leave_runtime() and exit the hook if
 * kmsan_in_runtime() is true.
 *
 * Non-runtime code may occasionally get executed in nested IRQs from the
 * runtime code (e.g. when called via smp_call_function_single()). Because some
 * KMSAN routines may take locks (e.g. for memory allocation), we conservatively
 * bail out instead of calling them. To minimize the effect of this (potentially
 * missing initialization events) kmsan_in_runtime() is not checked in
 * non-blocking runtime functions.
 */
/*
 * runtime 自身可能调用已插桩代码并再次进入 hook，故 enter/leave 用 ctx 计数阻断
 * 递归。runtime 被嵌套 hardirq 或 NMI 打断时，阻塞型 KMSAN helper 也直接退避，
 * 避免在任意锁状态下重新分配或取锁；代价是可能漏记少量初始化事件。明确保证
 * 不阻塞的 runtime helper 不检查本函数，以尽量缩小漏报窗口。
 */
/*
 * kmsan_in_runtime() - 判断当前 hook 是否必须为递归/不可阻塞上下文退避。
 * 入参：无。出参/返回：嵌套 hardirq、任意 NMI 或当前 ctx 计数非零返回 true。
 * 注意事项：只读瞬时执行上下文，不取锁、不睡眠；true 表示跳过而非报告错误。
 */
static __always_inline bool kmsan_in_runtime(void)
{
	if ((hardirq_count() >> HARDIRQ_SHIFT) > 1)
		return true;
	if (in_nmi())
		return true;
	return kmsan_get_context()->kmsan_in_runtime;
}

/*
 * kmsan_enter_runtime() - 进入一段不应被 KMSAN 再插桩处理的 runtime 临界区。
 * 业务背景：调用可能分配、打印或触达插桩代码的 helper 前建立递归门禁。入参：无。
 * 出参/返回：无直接返回；当前 task/per-CPU ctx 计数从 0 加到 1，嵌套进入会告警。
 * 注意事项：必须在同一上下文由 leave 配对；不提供 CPU 间互斥，也不能替代业务锁。
 */
static __always_inline void kmsan_enter_runtime(void)
{
	struct kmsan_ctx *ctx;

	ctx = kmsan_get_context();
	KMSAN_WARN_ON(ctx->kmsan_in_runtime++);
}

/*
 * kmsan_leave_runtime() - 退出由 enter 建立的 runtime 递归门禁。
 * 业务背景：完成可能递归的内部操作后恢复 compiler hook。入参：无。
 * 出参/返回：无直接返回；计数减一，结果非零会告警以暴露嵌套或配对错误。
 * 注意事项：必须与同一 task/per-CPU ctx 的一次 enter 配对；不得跨上下文迁移使用。
 */
static __always_inline void kmsan_leave_runtime(void)
{
	struct kmsan_ctx *ctx = kmsan_get_context();

	KMSAN_WARN_ON(--ctx->kmsan_in_runtime);
}

/*
 * kmsan_save_stack_with_flags() - 保存当前调用栈并附加 KMSAN origin 位。
 * 业务背景：poison/chain 用 stack-depot handle 表示创建位置。@flags 控制 depot 分配，
 * @extra_bits 编码 UAF 与深度。返回 handle（分配失败可为 0），其存储由 depot 管理。
 * 注意事项：最多采 KMSAN_STACK_DEPTH 帧，可能按 GFP 语义分配，调用者通常先 enter runtime。
 */
depot_stack_handle_t kmsan_save_stack_with_flags(gfp_t flags,
						 unsigned int extra_bits);

/*
 * Pack and unpack the origin chain depth and UAF flag to/from the extra bits
 * provided by the stack depot.
 * The UAF flag is stored in the lowest bit, followed by the depth in the upper
 * bits.
 * set_dsh_extra_bits() is responsible for clamping the value.
 */
/*
 * stack depot 的 extra bits 最低位保存 UAF 标志，其余高位保存 origin chain 深度；
 * 打包端不自行截断，最终写入 depot 时由 set extra-bits helper 钳制可用位宽。
 */
/*
 * kmsan_extra_bits() - 打包 origin 深度与 UAF 属性。
 * @depth 是传播层数，@uaf 表示是否源自 free。返回 `(depth << 1) | uaf`，无副作用。
 * 注意事项：调用者保证 depth 不超过 KMSAN_MAX_ORIGIN_DEPTH，实际位宽由 depot 钳制。
 */
static __always_inline unsigned int kmsan_extra_bits(unsigned int depth,
						     bool uaf)
{
	return (depth << 1) | uaf;
}

/*
 * kmsan_uaf_from_eb() - 从 stack-depot extra bits 解出 UAF 低位。
 * @extra_bits 为按上述格式保存的无符号值。返回最低位的布尔值，无副作用。
 * 注意事项：不验证其余位；用于报告分类并沿 origin 链原样传播该属性。
 */
static __always_inline bool kmsan_uaf_from_eb(unsigned int extra_bits)
{
	return extra_bits & 1;
}

/*
 * kmsan_depth_from_eb() - 从 stack-depot extra bits 解出 origin chain 深度。
 * @extra_bits 为打包值。右移一位返回无符号深度，不修改 depot 或 handle。
 * 注意事项：函数不钳制结果；只有由 KMSAN 写入的 extra bits 才满足最大深度不变量。
 */
static __always_inline unsigned int kmsan_depth_from_eb(unsigned int extra_bits)
{
	return extra_bits >> 1;
}

/*
 * kmsan_internal_ functions are supposed to be very simple and not require the
 * kmsan_in_runtime() checks.
 */
/*
 * `kmsan_internal_*` 是必须保持简单的非插桩原语，调用者通常已建立 runtime guard；
 * 它们不再次调用 kmsan_in_runtime()，否则内部状态更新会被自己跳过。明确可从
 * 非阻塞嵌套上下文调用的路径也依赖这一点，因此每个调用点必须自行满足锁/GFP 约束。
 */
/*
 * kmsan_internal_memmove_metadata() - 按 memmove 重叠语义复制 shadow/origin。
 * @dst/@src 是借用数据区起点，@n 为字节数。无直接返回；反向复制重叠范围，
 * 对未初始化字节链接 origin；源不可跟踪时把目标视为已初始化。
 * 注意事项：两侧 metadata 必须各自连续；不分配数据内存，origin 链可能用 stack depot。
 */
void kmsan_internal_memmove_metadata(void *dst, void *src, size_t n);
/*
 * kmsan_internal_poison_memory() - 把范围标为未初始化并记录创建/释放栈。
 * @address 为借用起点，@size 为字节数，@flags 控制 stack-depot 分配，@poison_flags
 * 是 CHECK/FREE 位组合。无返回；shadow 写 0xff，origin 写新 handle，FREE 沿链保留。
 * 注意事项：metadata 不可用且 CHECK 置位会告警；可能按 GFP 语义分配 depot 记录。
 */
void kmsan_internal_poison_memory(void *address, size_t size, gfp_t flags,
				  unsigned int poison_flags);
/*
 * kmsan_internal_unpoison_memory() - 把范围标为已初始化并清理可清的 origin。
 * @address 为借用起点，@size 为字节数，@checked 决定 metadata 缺失时是否告警。
 * 无直接返回；shadow 置零，只有完整 origin 槽已无毒时才把其 handle 清零。
 * 注意事项：要求 metadata 连续；不修改真实数据字节，也不取得地址 ownership。
 */
void kmsan_internal_unpoison_memory(void *address, size_t size, bool checked);
/*
 * kmsan_internal_set_shadow_origin() - 同步写一段 shadow 字节与 4 字节 origin 槽。
 * @address/@size 定位借用范围，@b 是 shadow 填充值，@origin 是 depot handle，
 * @checked 控制不可跟踪地址告警。无返回；非零 origin 无条件覆盖相关槽，零 origin
 * 仅在对应 shadow 四字节全为零时清槽，避免抹掉同槽仍未初始化字节的来源。
 * 注意事项：metadata 必须连续；本原语不做 runtime 递归检查。
 */
void kmsan_internal_set_shadow_origin(void *address, size_t size, int b,
				      u32 origin, bool checked);
/*
 * kmsan_internal_chain_origin() - 为一次未初始化值传播追加当前栈节点。
 * @id 是上一 origin handle；0 原样返回，达到最大深度返回旧 handle。否则返回新
 * depot handle 并保留 UAF 位、深度加一；分配失败可返回 0，旧 handle ownership 不变。
 * 注意事项：会以 __GFP_HIGH 访问 stack depot，调用者应已进入 runtime guard。
 */
depot_stack_handle_t kmsan_internal_chain_origin(depot_stack_handle_t id);

/*
 * kmsan_internal_task_create() - 初始化新 task 的 KMSAN 调用上下文。
 * @task 是创建路径独占的输入输出 task；函数清零 task->kmsan_ctx，并把当前
 * thread_info metadata 标为已初始化。无直接返回，不取得 task 引用。
 * 注意事项：发布/运行新 task 前调用，外层已 enter runtime；本函数本身不做递归检查。
 */
void kmsan_internal_task_create(struct task_struct *task);

/*
 * kmsan_metadata_is_contiguous() - 验证一段数据的 shadow/origin 可连续线性访问。
 * @addr 为借用起点，@size 为字节数。空范围、单页、全部未跟踪或跨页 metadata
 * 虚拟地址连续时返回 true；混合/断裂时打印诊断并返回 false，无 ownership 变化。
 * 注意事项：结果是当前映射快照，调用者仍需用相应映射生命周期保护后续访问。
 */
bool kmsan_metadata_is_contiguous(void *addr, size_t size);
/*
 * kmsan_internal_check_memory() - 扫描范围并按 origin 分组报告未初始化字节。
 * @addr 为借用起点，@size 为字节数，@user_addr 可为 NULL 或泄漏目标用户地址，
 * @reason 对应 kmsan_bug_reason。无直接返回；未跟踪页按已初始化边界处理并结束前组。
 * 注意事项：要求 metadata 连续；可能多次调用 kmsan_report()，调用者负责 runtime 条件。
 */
void kmsan_internal_check_memory(void *addr, size_t size,
				 const void __user *user_addr, int reason);

/*
 * kmsan_vmalloc_to_page_or_null() - 把 vmalloc/module 虚拟地址解析为有效普通 page。
 * @vaddr 是借用地址。返回借用 page 指针，地址不在两区或映射 PFN 无效时返回 NULL；
 * 不增引用、不解除映射。业务上用于拆除 KMSAN vmalloc metadata 映射后找回 backing。
 * 注意事项：调用者必须稳定且保证区间内确有 vmap；实现未处理 vmalloc_to_page()
 * 返回 NULL 的情况。返回后释放页还需由调用者既有的映射 ownership 授权。
 */
struct page *kmsan_vmalloc_to_page_or_null(void *vaddr);
/*
 * kmsan_setup_meta() - 为一块数据页逐页绑定同阶 shadow 与 origin backing。
 * @page/@shadow/@origin 是分别指向 2^@order 个连续 page 的输入输出块，@order 为阶数。
 * 无直接返回；metadata 页自身被标为无 metadata，数据页保存一一对应指针。
 * 注意事项：启动分配路径独占这些页并转移给 KMSAN/page allocator 协议，@order
 * 必须非负且三个块的页数必须匹配。
 */
void kmsan_setup_meta(struct page *page, struct page *shadow,
		      struct page *origin, int order);

/*
 * kmsan_internal_is_module_addr() and kmsan_internal_is_vmalloc_addr() are
 * non-instrumented versions of is_module_address() and is_vmalloc_addr() that
 * are safe to call from KMSAN runtime without recursion.
 */
/*
 * 下列两个 helper 是 is_module_address()/is_vmalloc_addr() 的非插桩区间判断版本，
 * 可在 KMSAN runtime 内调用而不会因通用 helper 被插桩再次递归。
 */
/*
 * kmsan_internal_is_module_addr() - 判断地址是否落在 `[MODULES_VADDR, MODULES_END)`。
 * @vaddr 为任意借用指针。返回边界比较布尔值，无解引用、引用或副作用。
 * 注意事项：只判断数值区间，不证明当前地址已有模块映射或 metadata。
 */
static inline bool kmsan_internal_is_module_addr(void *vaddr)
{
	return ((u64)vaddr >= MODULES_VADDR) && ((u64)vaddr < MODULES_END);
}

/*
 * kmsan_internal_is_vmalloc_addr() - 判断地址是否落在 `[VMALLOC_START, VMALLOC_END)`。
 * @addr 为任意借用指针。返回边界比较布尔值，无解引用、引用或副作用。
 * 注意事项：只判断地址窗，不证明存在有效 vmap；后续仍须查询 page/metadata。
 */
static inline bool kmsan_internal_is_vmalloc_addr(void *addr)
{
	return ((u64)addr >= VMALLOC_START) && ((u64)addr < VMALLOC_END);
}

#endif /* __MM_KMSAN_KMSAN_H */
/* 结束 KMSAN runtime 内部头文件保护范围。 */
