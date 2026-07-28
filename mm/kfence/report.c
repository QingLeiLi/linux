// SPDX-License-Identifier: GPL-2.0
/*
 * KFENCE reporting.
 *
 * Copyright (C) 2020, Google LLC.
 */
/*
 * 中文学习注释：KFENCE 报告子系统。
 *
 * 文件职责：
 *   本文件不负责“检测”错误本身，也不负责保护/解除保护页；这些工作主要在
 *   mm/kfence/core.c 中完成。report.c 的职责是在 core.c 已经识别出错误类型后，
 *   把 metadata、fault 地址、访问类型和栈信息组织成开发者能读懂的报告，并根据
 *   kfence.fault 策略决定仅报告、触发 oops，还是直接 panic。
 *
 * 为什么独立成报告层：
 *   1. 检测路径需要尽量小且接近内存状态机，报告路径需要格式化字符串、栈解析、
 *      printk、tracepoint、寄存器打印等较重逻辑；拆开能让 core.c 保持清晰。
 *   2. 报告通常发生在内核已经出现内存安全错误之后，此时系统可能处于锁敏感或
 *      调度敏感上下文。这里的很多选择，例如临时 lockdep_off()、接受 printk 风险、
 *      对 canary 差异只打印有限字节，都是“尽快留下证据”和“避免二次破坏”之间的权衡。
 *   3. 同一份对象快照既服务 console 报告，也服务 debugfs/seq_file 和 printk 的
 *      kmem_obj_info，因此本文件提供 seq_con_printf() 这样的双输出 helper。
 *
 * 读者应关注的主路径：
 *   kfence_handle_page_fault()
 *     -> kfence_report_error()
 *       -> 打印错误头、访问栈、对象 alloc/free 栈、寄存器/栈摘要、tracepoint
 *     -> kfence_handle_fault()
 *       -> 按 kfence.fault=report/oops/panic 执行策略
 *
 *   check_canary()/invalid free 路径
 *     -> kfence_report_error(..., KFENCE_ERROR_CORRUPTION/INVALID_FREE)
 *     -> kfence_handle_fault()
 *
 * 设计收益与代价：
 *   KFENCE 的报告保留了“访问点 + 原始分配点 + 释放点 + cache 名称 + 对象槽号”，
 *   对定位 UAF/OOB 很直接；代价是报告路径依赖 printk 和栈回溯，在非常坏的上下文
 *   中仍有递归告警、输出不完整或符号不准的风险。因此代码更偏向“错误后诊断尽量有用”，
 *   而不是保证报告路径在所有上下文都完全无副作用。
 */

#include <linux/stdarg.h>

#include <linux/bug.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/math.h>
#include <linux/panic.h>
#include <linux/printk.h>
#include <linux/sched/debug.h>
#include <linux/seq_file.h>
#include <linux/sprintf.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/sched/clock.h>
#include <trace/events/error_report.h>

#include <asm/kfence.h>

#include "kfence.h"

/* May be overridden by <asm/kfence.h>. */
/*
 * 中文翻译与补充：体系结构可以在 <asm/kfence.h> 中定义 ARCH_FUNC_PREFIX，
 * 用于修正栈符号名前缀。get_stack_skipnr() 依赖函数名匹配跳过 allocator/KFENCE
 * 内部帧；如果架构在符号前加前缀，不适配会导致报告把内部 helper 当成出错调用点。
 */
#ifndef ARCH_FUNC_PREFIX
#define ARCH_FUNC_PREFIX ""
#endif

static enum kfence_fault kfence_fault __ro_after_init = KFENCE_FAULT_REPORT;
/*
 * kfence_fault 是启动后只读的报告策略：
 *   KFENCE_FAULT_REPORT 只打印报告并继续；
 *   KFENCE_FAULT_OOPS   报告后触发 oops；
 *   KFENCE_FAULT_PANIC  报告后 panic。
 *
 * __ro_after_init 表示 early_param 解析完成后不再允许普通写入，避免运行期错误或攻击
 * 悄悄降低故障处理强度。默认 report 是较温和策略，适合在生产中低开销采样并继续收集证据。
 */

/*
 * early_kfence_fault() - 解析启动参数 kfence.fault。
 *
 * 入参：
 *   @arg: early_param 传入的字符串，允许值为 "report"、"oops"、"panic"；
 *         为 NULL 或未知字符串时返回 -EINVAL。
 *
 * 返回：
 *   0       表示策略已写入 kfence_fault；
 *   -EINVAL 表示参数缺失或非法，启动参数框架会按无效参数处理。
 *
 * 背景：
 *   该参数必须在早期解析，因为 KFENCE 可能在启动后不久就开始报告错误。策略用
 *   __ro_after_init 固化，优点是运行期语义稳定；代价是不能通过 sysfs/debugfs 临时
 *   从 report 改成 panic，需要重启带参数验证。
 */
static int __init early_kfence_fault(char *arg)
{
	/*
	 * 阶段 1：拒绝空参数，避免把 "kfence.fault" 无值误解释成默认 report。
	 */
	if (!arg)
		return -EINVAL;

	/*
	 * 阶段 2：字符串到 enum 的显式映射。这里不用前缀匹配，避免 "panicXYZ" 这类
	 * 输入被误接受；每个值都直接决定 kfence_handle_fault() 后续行为。
	 */
	if (!strcmp(arg, "report"))
		kfence_fault = KFENCE_FAULT_REPORT;
	else if (!strcmp(arg, "oops"))
		kfence_fault = KFENCE_FAULT_OOPS;
	else if (!strcmp(arg, "panic"))
		kfence_fault = KFENCE_FAULT_PANIC;
	else
		return -EINVAL;

	return 0;
}
early_param("kfence.fault", early_kfence_fault);
/*
 * early_param 把 kfence.fault=... 注册到启动参数解析阶段。它只影响报告后的处理策略，
 * 不影响 KFENCE 是否启用、采样间隔或对象池大小。
 */

/* Helper function to either print to a seq_file or to console. */
/*
 * 中文翻译与补充：seq_con_printf() 是双输出通道格式化 helper。
 *
 * 入参：
 *   @seq: 非 NULL 时输出到 seq_file，供 debugfs 这类按需读取接口使用；
 *         NULL 时输出到 printk console，供错误报告使用。
 *   @fmt/...: printf 风格格式串和参数，__printf 属性让编译器检查格式匹配。
 *
 * 返回：无直接返回值。副作用是向 @seq 或内核日志写文本。
 *
 * 设计权衡：
 *   复用一套对象打印逻辑避免 console/debugfs 输出分叉；代价是调用者必须清楚当前
 *   上下文是否适合 printk。KFENCE 错误报告处于“已经检测到内存破坏”的情形，选择
 *   尽量输出证据，而不是因为 printk 风险放弃报告。
 */
__printf(2, 3)
static void seq_con_printf(struct seq_file *seq, const char *fmt, ...)
{
	va_list args;

	/*
	 * va_list 的生命周期只覆盖一次格式化调用。这里不保存 args，也不跨越函数返回，
	 * 因此输出完成后必须 va_end() 归还调用约定所需状态。
	 */
	va_start(args, fmt);
	if (seq)
		seq_vprintf(seq, fmt, args);
	else
		vprintk(fmt, args);
	va_end(args);
}

/*
 * Get the number of stack entries to skip to get out of MM internals. @type is
 * optional, and if set to NULL, assumes an allocation or free stack.
 */
/*
 * 中文翻译与补充：
 *   get_stack_skipnr() 计算报告栈中应跳过多少个 KFENCE/MM 内部帧。
 *
 * 入参：
 *   @stack_entries: 栈地址数组，借用，只读取；
 *   @num_entries:   有效栈帧个数；
 *   @type:          可选错误类型。NULL 表示这是 alloc/free track，需要跳过 allocator
 *                   内部入口；非 NULL 表示当前正在打印错误访问栈，不同错误类型
 *                   对“应该从哪里开始显示”有不同要求。
 *
 * 返回：
 *   要跳过的栈帧数量。返回 0 表示从第一帧开始打印。
 *
 * 背景和注意事项：
 *   报告里最有价值的不是 kfence_report_error() 或 __kfence_free() 自己，而是调用
 *   kmalloc/kfree 或触发 fault 的业务函数。这个函数用符号名启发式跳过内部帧；
 *   优点是不需要每个调用点手工传 caller，缺点是依赖编译器栈回溯和符号名称，tail
 *   call、LTO 或架构前缀都可能影响精确度，因此代码保留 fallback 兜底。
 */
static int get_stack_skipnr(const unsigned long stack_entries[], int num_entries,
			    const enum kfence_error_type *type)
{
	char buf[64];
	int skipnr, fallback = 0;
	/*
	 * 变量地图：
	 *   buf      临时保存 %ps 解析出的符号名，只在本轮循环有效；
	 *   skipnr   当前扫描的栈帧下标；
	 *   fallback 遇到 KFENCE/free 内部帧后的兜底跳过位置，用于处理 tail-call 省略。
	 */

	if (type) {
		/* Depending on error type, find different stack entries. */
		/*
		 * 中文翻译与补充：错误访问类 OOB/UAF/INVALID 的栈通常已经从 fault 现场或
		 * 当前执行点保存，第一帧就是有价值证据；而 CORRUPTION/INVALID_FREE 来自
		 * free/canary 检查路径，需要继续向下跳过 allocator 内部帧。
		 */
		switch (*type) {
		case KFENCE_ERROR_UAF:
		case KFENCE_ERROR_OOB:
		case KFENCE_ERROR_INVALID:
			/*
			 * kfence_handle_page_fault() may be called with pt_regs
			 * set to NULL; in that case we'll simply show the full
			 * stack trace.
			 */
			/*
			 * 中文翻译与补充：page fault 入口有时没有 pt_regs，只能保存当前栈。
			 * 为避免误删真正访问点，这几类错误不做 allocator 前缀跳过，直接展示
			 * 完整栈；后续读报告时再结合对象 alloc/free 栈判断责任点。
			 */
			return 0;
		case KFENCE_ERROR_CORRUPTION:
		case KFENCE_ERROR_INVALID_FREE:
			break;
		}
	}

	/*
	 * 阶段 2：逐帧转成符号名并寻找 allocator 初始入口。%ps 给出符号名而非裸地址；
	 * ARCH_FUNC_PREFIX 处理架构符号前缀差异。
	 */
	for (skipnr = 0; skipnr < num_entries; skipnr++) {
		int len = scnprintf(buf, sizeof(buf), "%ps", (void *)stack_entries[skipnr]);

		if (str_has_prefix(buf, ARCH_FUNC_PREFIX "kfence_") ||
		    str_has_prefix(buf, ARCH_FUNC_PREFIX "__kfence_") ||
		    str_has_prefix(buf, ARCH_FUNC_PREFIX "__kmem_cache_free") ||
		    !strncmp(buf, ARCH_FUNC_PREFIX "__slab_free", len)) {
			/*
			 * In case of tail calls from any of the below to any of
			 * the above, optimized by the compiler such that the
			 * stack trace would omit the initial entry point below.
			 */
			/*
			 * 中文翻译与补充：如果编译器把下层入口 tail-call 优化掉，栈里可能只剩
			 * KFENCE 或 __slab_free 这类内部帧。fallback 记住“这些内部帧之后”的
			 * 位置，保证找不到正式 allocator 入口时仍能少打印一些框架噪声。
			 */
			fallback = skipnr + 1;
		}

		/*
		 * The below list should only include the initial entry points
		 * into the slab allocators. Includes the *_bulk() variants by
		 * checking prefixes.
		 */
		/*
		 * 中文翻译与补充：正式停止点是 kfree/kmem_cache_free/__kmalloc/
		 * kmem_cache_alloc 这类对外分配器入口。找到后跳过它本身，让报告第一行
		 * 更接近调用 allocator 的业务函数，而不是 allocator wrapper。
		 */
		if (str_has_prefix(buf, ARCH_FUNC_PREFIX "kfree") ||
		    str_has_prefix(buf, ARCH_FUNC_PREFIX "kmem_cache_free") ||
		    str_has_prefix(buf, ARCH_FUNC_PREFIX "__kmalloc") ||
		    str_has_prefix(buf, ARCH_FUNC_PREFIX "kmem_cache_alloc"))
			goto found;
	}
	/*
	 * 未找到正式入口时使用 fallback；如果连 fallback 都越界，说明栈太短或符号不匹配，
	 * 返回 0 保留完整栈，宁可多一些内部帧也不丢失证据。
	 */
	if (fallback < num_entries)
		return fallback;
found:
	/*
	 * 跳过 allocator 入口本身，返回其调用者位置。若入口已经是最后一帧，则返回 0，
	 * 避免打印空栈。
	 */
	skipnr++;
	return skipnr < num_entries ? skipnr : 0;
}

/*
 * kfence_print_stack() - 打印某个 KFENCE 对象的分配栈或释放栈。
 *
 * 入参：
 *   @seq:        非 NULL 输出到 seq_file，NULL 输出到 printk；
 *   @meta:       被打印对象的 metadata，借用；调用者必须持有 meta->lock；
 *   @show_alloc: true 打印 alloc_track，false 打印 free_track。
 *
 * 返回：无直接返回值。
 *
 * 锁与一致性：
 *   __must_hold(&meta->lock) 表示调用者已经固定 metadata 快照。否则 state、track
 *   时间戳和栈数组可能与并发 free/realloc/report 交错，导致报告把不同生命周期的
 *   分配点和释放点拼在一起。
 *
 * 报告价值：
 *   访问栈告诉“哪里触发错误”，alloc/free 栈告诉“对象从哪里来、何时释放”。KFENCE
 *   的优点正在于把这三类证据放在同一份报告里；代价是每个 KFENCE 对象 metadata
 *   需要保存两份栈，增加调试内存开销。
 */
static void kfence_print_stack(struct seq_file *seq, const struct kfence_metadata *meta,
			       bool show_alloc)
	__must_hold(&meta->lock)
{
	const struct kfence_track *track = show_alloc ? &meta->alloc_track : &meta->free_track;
	u64 ts_sec = track->ts_nsec;
	unsigned long rem_nsec = do_div(ts_sec, NSEC_PER_SEC);
	u64 interval_nsec = local_clock() - track->ts_nsec;
	unsigned long rem_interval_nsec = do_div(interval_nsec, NSEC_PER_SEC);
	/*
	 * 变量地图：
	 *   track                 当前要打印的 alloc/free 轨迹，借用自 metadata；
	 *   ts_sec/rem_nsec       事件发生时刻，拆成秒和微秒对齐 printk 时间格式；
	 *   interval_nsec/rem...  距离当前 local_clock() 的时间差，用于判断对象存活多久。
	 *
	 * do_div() 会原地把 64 位被除数改成商并返回余数，所以 ts_sec 和 interval_nsec
	 * 在声明后立即被转换成“秒”值；这是内核常见的 64 位除法惯用法。
	 */

	/* Timestamp matches printk timestamp format. */
	/*
	 * 中文翻译与补充：时间戳格式与 printk 前缀保持一致，便于把 KFENCE 报告与附近
	 * 日志按时间线对齐。state 为 RCU_FREEING 时显示 "rcu freeing"，提醒读者对象
	 * 已进入延迟释放窗口，不能简单等同于普通 freed。
	 */
	seq_con_printf(seq, "%s by task %d on cpu %d at %lu.%06lus (%lu.%06lus ago):\n",
		       show_alloc ? "allocated" : meta->state == KFENCE_OBJECT_RCU_FREEING ?
		       "rcu freeing" : "freed", track->pid,
		       track->cpu, (unsigned long)ts_sec, rem_nsec / 1000,
		       (unsigned long)interval_nsec, rem_interval_nsec / 1000);

	if (track->num_stack_entries) {
		/* Skip allocation/free internals stack. */
		/*
		 * 中文翻译与补充：跳过分配器内部栈帧，使报告的第一批符号更接近业务调用点。
		 * 这里 @type 传 NULL，因为 alloc/free track 的目标是从 allocator 入口外开始。
		 */
		int i = get_stack_skipnr(track->stack_entries, track->num_stack_entries, NULL);

		/* stack_trace_seq_print() does not exist; open code our own. */
		/*
		 * 中文翻译与补充：内核没有直接把 stack_trace 打到 seq_file 的通用 helper，
		 * 所以这里逐帧用 seq_con_printf() 输出。console 与 seq_file 共享同一格式。
		 */
		for (; i < track->num_stack_entries; i++)
			seq_con_printf(seq, " %pS\n", (void *)track->stack_entries[i]);
	} else {
		/*
		 * 没有栈并不代表没有分配/释放，只表示当时栈采集不可用或被配置/上下文限制。
		 */
		seq_con_printf(seq, " no %s stack\n", show_alloc ? "allocation" : "deallocation");
	}
}

/*
 * kfence_print_object() - 打印单个 KFENCE metadata 的对象摘要。
 *
 * 入参：
 *   @seq:  非 NULL 输出到 seq_file，NULL 输出到 printk；
 *   @meta: 被打印对象 metadata，借用；调用者必须持有 meta->lock。
 *
 * 返回：无直接返回值。输出内容包括 KFENCE 槽号、地址范围、请求大小、cache 名称、
 * 分配栈，以及在 freed/RCU_FREEING 状态下的释放栈。
 *
 * 背景：
 *   普通 slab 报告常依赖 slab page 和 cache 内对象索引；KFENCE 每个对象独占页并有
 *   guard 页，报告更关心“kfence-#N”这个 metadata 槽位。cache 可能已被销毁，因此
 *   cache 名称必须容忍 NULL 或 <destroyed>，不能为了打印名字解引用悬空 cache。
 */
void kfence_print_object(struct seq_file *seq, const struct kfence_metadata *meta)
{
	const int size = abs(meta->size);
	const unsigned long start = meta->addr;
	const struct kmem_cache *const cache = meta->cache;
	/*
	 * 变量地图：
	 *   size  使用 abs(meta->size) 是因为 metadata size 可能用符号表达对象放置方向
	 *         或内部状态，报告只需要用户可理解的正向字节数；
	 *   start 对象起始地址，metadata 保存的真实起点，不能由页边界和 size 简单反推；
	 *   cache 借用的 kmem_cache 快照，可能因 shutdown_cache() 已清空。
	 */

	lockdep_assert_held(&meta->lock);

	/*
	 * UNUSED 槽位从未承载有效对象。此时 alloc/free track 和 cache 都没有诊断意义，
	 * 只打印槽号，避免读者把未初始化字段当作真实分配历史。
	 */
	if (meta->state == KFENCE_OBJECT_UNUSED) {
		seq_con_printf(seq, "kfence-#%td unused\n", meta - kfence_metadata);
		return;
	}

	/*
	 * 对象地址范围按 [start, start + size - 1] 打印，帮助定位 fault 地址是在对象内、
	 * 左 redzone 还是右 redzone。cache 名称缺失时显示 <destroyed>，对应 cache
	 * shutdown 后 zombie/freed 对象仍可能被报告的生命周期现实。
	 */
	seq_con_printf(seq, "kfence-#%td: 0x%p-0x%p, size=%d, cache=%s\n\n",
		       meta - kfence_metadata, (void *)start, (void *)(start + size - 1),
		       size, (cache && cache->name) ? cache->name : "<destroyed>");

	/*
	 * alloc_track 是所有已使用对象最核心的来源证据；free_track 只有在对象已释放或
	 * 正处于 RCU 延迟释放时才有意义。
	 */
	kfence_print_stack(seq, meta, true);

	if (meta->state == KFENCE_OBJECT_FREED || meta->state == KFENCE_OBJECT_RCU_FREEING) {
		seq_con_printf(seq, "\n");
		kfence_print_stack(seq, meta, false);
	}
}

/*
 * Show bytes at @addr that are different from the expected canary values, up to
 * @max_bytes.
 */
/*
 * 中文翻译与补充：
 *   print_diff_canary() 打印 canary 区域中与期望模式不同的字节。
 *
 * 入参：
 *   @address:       已发现 canary 损坏的起始地址；
 *   @bytes_to_show: 最多展示多少字节，单位字节；
 *   @meta:          所属对象 metadata，借用，用来限制展示边界。
 *
 * 返回：无直接返回值。副作用是通过 pr_cont() 续写当前错误报告行。
 *
 * 注意事项：
 *   这个函数只服务 KFENCE_ERROR_CORRUPTION 报告。它展示“哪些 canary 字节不符合预期”，
 *   但不会打印对象内容，也不会跨进下一页 guard page。这样既能提供越界写痕迹，又
 *   降低泄露内核内存和在报告路径再次 fault 的风险。
 */
static void print_diff_canary(unsigned long address, size_t bytes_to_show,
			      const struct kfence_metadata *meta)
{
	const unsigned long show_until_addr = address + bytes_to_show;
	const u8 *cur, *end;
	/*
	 * 变量地图：
	 *   show_until_addr  用户请求展示窗口的右边界；
	 *   cur/end          实际扫描游标和终点，end 会被对象边界或页边界收窄。
	 */

	/* Do not show contents of object nor read into following guard page. */
	/*
	 * 中文翻译与补充：如果 address 在对象左侧，只能显示到 meta->addr 前；如果在对象
	 * 右侧，只能显示到当前页末。这样不读取对象有效内容，也不越过 guard 页边界。
	 */
	end = (const u8 *)(address < meta->addr ? min(show_until_addr, meta->addr)
						: min(show_until_addr, PAGE_ALIGN(address)));

	pr_cont("[");
	for (cur = (const u8 *)address; cur < end; cur++) {
		/*
		 * "." 表示该字节仍等于期望 canary；具体字节值只在 no_hash_pointers 下打印。
		 * 普通构建输出 "!"，既告诉读者这里坏了，又避免泄露内核内存布局/内容。
		 */
		if (*cur == KFENCE_CANARY_PATTERN_U8(cur))
			pr_cont(" .");
		else if (no_hash_pointers)
			pr_cont(" 0x%02x", *cur);
		else /* Do not leak kernel memory in non-debug builds. */
			pr_cont(" !");
	}
	pr_cont(" ]");
}

/*
 * get_access_type() - 把访问方向布尔值转换成报告文案。
 *
 * 入参：
 *   @is_write: true 表示写访问，false 表示读访问。
 *
 * 返回：
 *   静态字符串 "write" 或 "read"。无副作用、不会睡眠、不取得任何引用。
 *
 * 宏观位置：
 *   kfence_report_error() 在 OOB/UAF/INVALID 报告头中调用它，把 page fault 路径传入
 *   的访问方向转成人类可读文本。当前只区分读写；若未来要区分 execute/unknown，
 *   可以在这里集中扩展，而不必修改每个 pr_err() 格式化点。
 */
static const char *get_access_type(bool is_write)
{
	/*
	 * get_access_type() - 把访问方向布尔值转换成报告文案。
	 *
	 * 入参 @is_write: true 表示写访问，false 表示读访问。
	 * 返回：静态字符串 "write" 或 "read"。无副作用。
	 *
	 * 这层薄包装让 KFENCE 报告不直接依赖 string_choices.h 的具体命名，后续若报告
	 * 需要把 execute/unknown 也区分出来，可以在这里集中扩展。
	 */
	return str_write_read(is_write);
}

/*
 * kfence_report_error() - 生成一次 KFENCE 内存安全错误报告。
 *
 * 入参：
 *   @address: 出错地址，单位字节；可能落在对象内、redzone、freed object 或 pool 内
 *             无法归因的位置。
 *   @is_write: true 表示写访问，false 表示读访问；invalid free/corruption 这类非
 *              普通 load/store 场景通常传 false，只用于报告文案。
 *   @regs:    fault 现场寄存器快照，可为 NULL。非 NULL 时优先从寄存器保存访问栈；
 *             NULL 时从当前栈保存并按错误类型决定是否跳过内部帧。
 *   @meta:    出错对象 metadata，借用；除 KFENCE_ERROR_INVALID 外必须非 NULL。
 *             调用者通常已持有 meta->lock，便于本函数打印一致对象快照。
 *   @type:    错误类型，决定报告标题、归因方式和是否打印 canary 差异。
 *
 * 返回：
 *   kfence_fault 当前策略，交给 kfence_handle_fault() 执行。若调用契约明显错误
 *   例如非 INVALID 错误却没有 metadata，则返回 KFENCE_FAULT_NONE。
 *
 * 背景和设计权衡：
 *   报告路径可能在 scheduler、free path、fault handler 等 printk 不友好的上下文中
 *   运行。严格做法是避免复杂输出，但内存错误发生后系统已经不可信；KFENCE 选择
 *   尽量把关键证据打出来，并临时关闭 lockdep 抑制二次告警。代价是报告本身不能
 *   被视作完全无风险路径，尤其在严重内存破坏后可能输出不完整。
 */
enum kfence_fault
kfence_report_error(unsigned long address, bool is_write, struct pt_regs *regs,
		    const struct kfence_metadata *meta, enum kfence_error_type type)
{
	unsigned long stack_entries[KFENCE_STACK_DEPTH] = { 0 };
	const ptrdiff_t object_index = meta ? meta - kfence_metadata : -1;
	int num_stack_entries;
	int skipnr = 0;
	/*
	 * 变量地图：
	 *   stack_entries 保存访问点或当前报告点的栈；
	 *   object_index  metadata 在全局数组中的槽号，-1 表示无法归因到具体对象；
	 *   num_stack_entries 有效栈帧数；
	 *   skipnr       打印访问栈时跳过的内部帧数量。
	 */

	/*
	 * 阶段 1：获取访问栈。fault 路径传入 regs 时，regs 更接近真正触发访问的现场；
	 * 没有 regs 时只能保存当前调用栈，再由 get_stack_skipnr() 尝试裁剪内部帧。
	 */
	if (regs) {
		num_stack_entries = stack_trace_save_regs(regs, stack_entries, KFENCE_STACK_DEPTH, 0);
	} else {
		num_stack_entries = stack_trace_save(stack_entries, KFENCE_STACK_DEPTH, 1);
		skipnr = get_stack_skipnr(stack_entries, num_stack_entries, &type);
	}

	/* Require non-NULL meta, except if KFENCE_ERROR_INVALID. */
	/*
	 * 中文翻译与补充：除 INVALID 外，报告必须能归因到具体 metadata。OOB/UAF/
	 * CORRUPTION/INVALID_FREE 都需要对象大小、cache、alloc/free 栈等上下文；
	 * 若 meta 缺失，说明调用者分类协议有 bug，WARN 后返回 NONE，避免继续解引用 NULL。
	 */
	if (WARN_ON(type != KFENCE_ERROR_INVALID && !meta))
		return KFENCE_FAULT_NONE;

	/*
	 * Because we may generate reports in printk-unfriendly parts of the
	 * kernel, such as scheduler code, the use of printk() could deadlock.
	 * Until such time that all printing code here is safe in all parts of
	 * the kernel, accept the risk, and just get our message out (given the
	 * system might already behave unpredictably due to the memory error).
	 * As such, also disable lockdep to hide warnings, and avoid disabling
	 * lockdep for the rest of the kernel.
	 */
	/*
	 * 中文翻译与补充：这里主动接受 printk 在坏上下文中的风险。内存安全错误可能已经
	 * 让系统行为不可预测，报告最重要的是留下可定位证据。lockdep_off()/on 只包住
	 * 本次输出，避免把“错误后报告路径的锁告警”污染成全局 lockdep 关闭。
	 */
	lockdep_off();

	pr_err("==================================================================\n");
	/* Print report header. */
	/*
	 * 阶段 2：按错误类型打印标题和第一段归因。标题回答“发生了什么、读还是写、
	 * 当前访问点在哪里”；下一行回答“地址与对象的关系”。
	 */
	switch (type) {
	case KFENCE_ERROR_OOB: {
		const bool left_of_object = address < meta->addr;
		/*
		 * OOB 根据 fault 地址在对象左侧还是右侧计算距离。这里使用 meta->addr 而非
		 * slab 布局，因为 KFENCE 对象可能位于页首或页尾，真实起点只在 metadata 中。
		 */

		pr_err("BUG: KFENCE: out-of-bounds %s in %pS\n\n", get_access_type(is_write),
		       (void *)stack_entries[skipnr]);
		pr_err("Out-of-bounds %s at 0x%p (%luB %s of kfence-#%td):\n",
		       get_access_type(is_write), (void *)address,
		       left_of_object ? meta->addr - address : address - meta->addr,
		       left_of_object ? "left" : "right", object_index);
		break;
	}
	case KFENCE_ERROR_UAF:
		/*
		 * UAF 报告的关键不是越界距离，而是对象已释放后仍被读/写。后面对象信息会
		 * 追加 free_track，帮助定位释放点与错误访问点之间的生命周期断裂。
		 */
		pr_err("BUG: KFENCE: use-after-free %s in %pS\n\n", get_access_type(is_write),
		       (void *)stack_entries[skipnr]);
		pr_err("Use-after-free %s at 0x%p (in kfence-#%td):\n",
		       get_access_type(is_write), (void *)address, object_index);
		break;
	case KFENCE_ERROR_CORRUPTION:
		/*
		 * CORRUPTION 来自 free/panic canary 检查：没有即时 fault 现场，只能展示发现
		 * canary 损坏的检查栈和局部差异。它常表示之前发生过越界写但当时未触发 guard。
		 */
		pr_err("BUG: KFENCE: memory corruption in %pS\n\n", (void *)stack_entries[skipnr]);
		pr_err("Corrupted memory at 0x%p ", (void *)address);
		print_diff_canary(address, 16, meta);
		pr_cont(" (in kfence-#%td):\n", object_index);
		break;
	case KFENCE_ERROR_INVALID:
		/*
		 * INVALID 表示地址落在 KFENCE pool 相关范围但无法确认对象或错误类型。它是
		 * 保守报告：提示可疑访问，同时避免编造 OOB/UAF 归因。
		 */
		pr_err("BUG: KFENCE: invalid %s in %pS\n\n", get_access_type(is_write),
		       (void *)stack_entries[skipnr]);
		pr_err("Invalid %s at 0x%p:\n", get_access_type(is_write),
		       (void *)address);
		break;
	case KFENCE_ERROR_INVALID_FREE:
		/*
		 * INVALID_FREE 在释放路径发现地址虽关联 KFENCE metadata，但不是当前活动对象
		 * 的合法释放地址，例如 double free 或内部偏移释放。对象信息帮助区分二者。
		 */
		pr_err("BUG: KFENCE: invalid free in %pS\n\n", (void *)stack_entries[skipnr]);
		pr_err("Invalid free of 0x%p (in kfence-#%td):\n", (void *)address,
		       object_index);
		break;
	}

	/* Print stack trace and object info. */
	/*
	 * 阶段 3：先打印访问/发现错误的栈，再打印对象生命周期。顺序很重要：读者先看到
	 * 触发点，再看对象从哪里分配、何时释放、属于哪个 cache，才能建立因果链。
	 */
	stack_trace_print(stack_entries + skipnr, num_stack_entries - skipnr, 0);

	if (meta) {
		lockdep_assert_held(&meta->lock);
		pr_err("\n");
		/*
		 * 调用者持有 meta->lock，因此 kfence_print_object() 打印的是同一生命周期的
		 * alloc/free 快照。INVALID 无 meta 时跳过对象块，避免伪造上下文。
		 */
		kfence_print_object(NULL, meta);
	}

	/* Print report footer. */
	/*
	 * 阶段 4：补充寄存器或通用栈环境，并发出 tracepoint。no_hash_pointers 打开时
	 * 允许显示更完整寄存器；普通配置下避免暴露过多地址，只打印 dump_stack 摘要。
	 */
	pr_err("\n");
	if (no_hash_pointers && regs)
		show_regs(regs);
	else
		dump_stack_print_info(KERN_ERR);
	trace_error_report_end(ERROR_DETECTOR_KFENCE, address);
	pr_err("==================================================================\n");

	/*
	 * 恢复 lockdep 后再处理 panic_on_warn/taint，保证 KFENCE 报告的临时抑制不会影响
	 * 后续内核锁调试。报告结束并不代表错误已修复，只是证据已经输出。
	 */
	lockdep_on();

	check_panic_on_warn("KFENCE");

	/* We encountered a memory safety error, taint the kernel! */
	/*
	 * 中文翻译与补充：检测到内存安全错误后给内核打 TAINT_BAD_PAGE。LOCKDEP_STILL_OK
	 * 表示该 taint 不自动关闭 lockdep；后续问题分析需要知道系统已经经历过内存破坏。
	 */
	add_taint(TAINT_BAD_PAGE, LOCKDEP_STILL_OK);

	/*
	 * 返回策略而不在本函数里直接 BUG/panic，是为了把“生成报告”和“执行故障策略”
	 * 分离。调用者可以先完成必要的解除保护或状态处理，再调用 kfence_handle_fault()。
	 */
	return kfence_fault;
}

/*
 * kfence_handle_fault() - 执行 kfence_report_error() 返回的故障策略。
 *
 * 入参：
 *   @fault: 报告策略，通常来自全局 kfence_fault，也可能是 KFENCE_FAULT_NONE。
 *
 * 返回：无直接返回值。
 *
 * 副作用：
 *   REPORT/NONE 不终止当前路径；OOPS 触发 BUG()；PANIC 会先关闭 KFENCE 再 panic。
 *
 * 注意事项：
 *   这个函数刻意很小，让调用点清楚“报告已经完成，接下来是否升级故障”。如果把 panic
 *   逻辑混进 kfence_report_error()，报告格式化与系统终止会更难审计，也更难避免递归。
 */
void kfence_handle_fault(enum kfence_fault fault)
{
	/*
	 * 策略分发表：
	 *   NONE/REPORT: 只保留证据，适合持续采样；
	 *   OOPS:        让当前任务走 oops，用于测试或强制暴露错误；
	 *   PANIC:       系统级失败，适合希望第一处内存错误立即停机取证的环境。
	 */
	switch (fault) {
	case KFENCE_FAULT_NONE:
	case KFENCE_FAULT_REPORT:
		break;
	case KFENCE_FAULT_OOPS:
		BUG();
		break;
	case KFENCE_FAULT_PANIC:
		/* Disable KFENCE to avoid recursion if check_on_panic is set. */
		/*
		 * 中文翻译与补充：panic 路径可能触发 kfence_check_on_panic 扫描 canary。先关闭
		 * kfence_enabled，避免扫描或后续 fault 再递归进入 KFENCE 报告，导致 panic
		 * 输出被重复报告淹没。
		 */
		WRITE_ONCE(kfence_enabled, false);
		panic("kfence.fault=panic set ...\n");
		break;
	}
}

#ifdef CONFIG_PRINTK
/*
 * CONFIG_PRINTK 下还需要把 KFENCE 私有的 kfence_track 转成通用 kmem_obj_info
 * 使用的栈数组。没有 PRINTK 时，slab 对象来源打印路径不存在，这部分接口也不编译。
 */
/*
 * kfence_to_kp_stack() - 把 KFENCE track 栈转换成 kmem_obj_info 栈数组。
 *
 * 入参：
 *   @track:    KFENCE alloc/free track，借用，只读取；
 *   @kp_stack: 调用者提供的输出数组，最多写 KS_ADDRS_COUNT 个地址，并在有空间时
 *              以 NULL 结尾。
 *
 * 返回：无直接返回值。副作用是填充 @kp_stack。
 *
 * 背景：
 *   KFENCE metadata 保存 KFENCE_STACK_DEPTH 深度的原始栈；printk 的 kmem_obj_info
 *   使用固定大小的 kp_stack。这里负责裁剪 allocator 内部帧，并把格式转换为普通
 *   slab 诊断能理解的数组。
 */
static void kfence_to_kp_stack(const struct kfence_track *track, void **kp_stack)
{
	int i, j;
	/*
	 * i 是源栈下标，初始化为跳过 allocator/KFENCE 内部后的第一帧；
	 * j 是输出数组下标。两个下标分开是因为源栈可能很深，而输出数组大小固定。
	 */

	i = get_stack_skipnr(track->stack_entries, track->num_stack_entries, NULL);
	for (j = 0; i < track->num_stack_entries && j < KS_ADDRS_COUNT; ++i, ++j)
		kp_stack[j] = (void *)track->stack_entries[i];
	/*
	 * 如果输出数组还有空间，写 NULL 作为终止哨兵，方便后续打印逻辑不用额外传长度。
	 * 如果数组正好填满，则调用者按固定长度处理。
	 */
	if (j < KS_ADDRS_COUNT)
		kp_stack[j] = NULL;
}

/*
 * __kfence_obj_info() - 为通用 slab/printk 对象诊断填充 KFENCE 对象快照。
 *
 * 宏观位置：
 *   mm/slab_common.c::kmem_obj_info()
 *     -> __kfence_obj_info()
 *     -> 普通 __kmem_obj_info() fallback
 *
 * 入参：
 *   @kpp:    调用者提供的输出结构，成功识别 KFENCE 对象后由本函数填充；
 *   @object: printk/slab 诊断路径传入的疑似对象地址，借用，不释放；
 *   @slab:   调用者已知的 slab 上下文，作为诊断字段记录，不取得引用。
 *
 * 返回：
 *   false 表示 @object 不是 KFENCE 对象，调用者应继续普通 slab 诊断；
 *   true  表示 KFENCE 已经处理 @kpp，调用者不能再按普通 slab 布局解释该地址。
 *
 * 并发与注意事项：
 *   KFENCE 对象不挂普通 slab 对象布局，必须先由 addr_to_metadata() 识别。详细字段
 *   在 meta->lock 下复制成快照；出锁后对象可能继续变化，但 @kpp 中的信息自洽。
 */
bool __kfence_obj_info(struct kmem_obj_info *kpp, void *object, struct slab *slab)
{
	struct kfence_metadata *meta = addr_to_metadata((unsigned long)object);
	unsigned long flags;
	/*
	 * 变量地图：
	 *   @kpp    调用者提供的输出结构，成功识别 KFENCE 对象后由本函数填充；
	 *   @object printk/slab 诊断路径传入的疑似对象地址，借用，不释放；
	 *   @slab   调用者已知的 slab 上下文，作为诊断字段记录，不取得引用；
	 *   meta    由地址反推的 KFENCE metadata，NULL 表示应退回普通 slab 诊断；
	 *   flags   保存中断状态，保证 meta->lock 的 irqsave/restore 成对。
	 *
	 * 返回 false 表示“不是 KFENCE 对象，继续 __kmem_obj_info()”；返回 true 表示
	 * KFENCE 已经负责填充 @kpp，调用者不应再用普通 slab 布局解释该地址。
	 */

	/*
	 * 阶段 1：只按 KFENCE pool 地址反查 metadata。这里不解引用普通 slab 元数据，
	 * 因而可作为 kmem_obj_info() 的第一道分流，避免把 KFENCE 单对象页误按 slab
	 * 连续对象布局解析。
	 */
	if (!meta)
		return false;

	/*
	 * If state is UNUSED at least show the pointer requested; the rest
	 * would be garbage data.
	 */
	/*
	 * 中文翻译与补充：如果 metadata 从未被分配使用，除了用户请求查询的指针本身，
	 * 其它 cache、object、栈字段都可能是未初始化或无意义数据。因此先填 kp_ptr，
	 * 保证调用者至少能在报告中看到原始地址。
	 */
	kpp->kp_ptr = object;

	/* Requesting info an a never-used object is almost certainly a bug. */
	/*
	 * 中文翻译与补充：查询从未使用过的 KFENCE 槽位通常说明调用者传入了错误地址
	 * 或诊断路径误判。这里 WARN 后返回 true，是为了阻止普通 slab 继续解释该地址，
	 * 同时避免把垃圾 metadata 字段复制到输出结构。
	 */
	if (WARN_ON(meta->state == KFENCE_OBJECT_UNUSED))
		return true;

	/*
	 * 阶段 2：锁定 metadata 并复制一致的诊断快照。meta->lock 保护 state、cache、
	 * addr、alloc/free track 等字段组合，防止与 free、fault report 或重新分配交错。
	 */
	raw_spin_lock_irqsave(&meta->lock, flags);

	/*
	 * 输出字段的 ownership：
	 *   kp_slab/kp_slab_cache/kp_objp 都是诊断用裸指针快照，不增加引用；
	 *   kp_stack/kp_free_stack 是栈地址数组拷贝，不延长相关代码或对象生命周期；
	 *   kp_ret 选择分配栈第一条 allocator 外地址，供 printk 标出“谁分配了对象”。
	 */
	kpp->kp_slab = slab;
	kpp->kp_slab_cache = meta->cache;
	kpp->kp_objp = (void *)meta->addr;
	kfence_to_kp_stack(&meta->alloc_track, kpp->kp_stack);
	/*
	 * 只有已释放或正在 RCU 延迟释放的对象才有有意义的 free_track。活动对象没有
	 * 释放栈，保持输出结构中对应字段由调用者初始化/后续逻辑处理。
	 */
	if (meta->state == KFENCE_OBJECT_FREED || meta->state == KFENCE_OBJECT_RCU_FREEING)
		kfence_to_kp_stack(&meta->free_track, kpp->kp_free_stack);
	/* get_stack_skipnr() ensures the first entry is outside allocator. */
	/*
	 * 中文翻译与补充：get_stack_skipnr() 保证第一个保留下来的栈入口已经跳出
	 * allocator 内部帧，因此 kp_ret 更接近真正请求分配的调用者，适合 printk
	 * 用作对象来源摘要。
	 */
	kpp->kp_ret = kpp->kp_stack[0];

	/*
	 * 出锁后 @kpp 保存的是快照，不是持锁视图。对象随后可能被释放或重新分配，
	 * 但这不影响当前诊断输出的自洽性。
	 */
	raw_spin_unlock_irqrestore(&meta->lock, flags);

	return true;
}
#endif
