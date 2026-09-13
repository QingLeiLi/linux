// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains common KASAN error reporting code.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * Some code borrowed from https://github.com/xairy/kasan-prototype by
 *        Andrey Konovalov <andreyknvl@gmail.com>
 */

/* 本文件汇聚打印、锁、trace、slab、vmalloc 和 compiler-instrumentation 的报告依赖。 */
#include <kunit/test.h>
#include <kunit/visibility.h>
#include <linux/bitops.h>
#include <linux/ftrace.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/mm.h>
/* 内存对象、任务、栈、模块与 trace 头按报告的地址归属阶段分组引入。 */
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
/* printk、调度、slab、stack depot 与 vmalloc 共同构成报告的只读诊断依赖。 */
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/kasan.h>
#include <linux/module.h>
#include <linux/sched/task_stack.h>
#include <linux/uaccess.h>
#include <trace/events/error_report.h>

/* 架构 section 与私有 KASAN/slab 头提供地址分类和模式特有报告 ABI。 */
#include <asm/sections.h>

#include "kasan.h"
#include "../slab.h"

/* 报告策略位图：reported 限制首个错误，multi-shot 允许测试/诊断继续输出。 */
static unsigned long kasan_flags;

#define KASAN_BIT_REPORTED	0
#define KASAN_BIT_MULTI_SHOT	1

/* 启动参数决定报告完成后的处置；值在 early 参数解析后不再变化。 */
enum kasan_arg_fault {
	KASAN_ARG_FAULT_DEFAULT,
	KASAN_ARG_FAULT_REPORT,
	KASAN_ARG_FAULT_PANIC,
	KASAN_ARG_FAULT_PANIC_ON_WRITE,
};

static enum kasan_arg_fault kasan_arg_fault __ro_after_init = KASAN_ARG_FAULT_DEFAULT;

/* kasan.fault=report/panic */
/*
 * early_kasan_fault() - 解析 kasan.fault 的启动期报告处置策略
 * 业务背景：同一内存错误可只打印或立即 panic；早期解析避免运行期配置竞态。
 * 入参：arg 是内核命令行借用字符串；返回 0 或 -EINVAL。注意事项：只能 init 期调用。
 */
static int __init early_kasan_fault(char *arg)
{
	if (!arg)
		return -EINVAL;

	/* 三个字符串映射到不同终止策略；未知值必须拒绝避免静默改变安全语义。 */
	if (!strcmp(arg, "report"))
		kasan_arg_fault = KASAN_ARG_FAULT_REPORT;
	else if (!strcmp(arg, "panic"))
		kasan_arg_fault = KASAN_ARG_FAULT_PANIC;
	/* panic_on_write 把 allocator/写访问视为致命，读错误仍允许仅报告。 */
	else if (!strcmp(arg, "panic_on_write"))
		kasan_arg_fault = KASAN_ARG_FAULT_PANIC_ON_WRITE;
	else
		return -EINVAL;

	return 0;
}
early_param("kasan.fault", early_kasan_fault);

/* 设置 multi-shot 位，让后续报告不被首错去重；参数文本本身不需解析。 */
static int __init kasan_set_multi_shot(char *str)
{
	/* 原子位操作与并发 report_enabled 配对，启动期设置后仍保持统一读取规则。 */
	set_bit(KASAN_BIT_MULTI_SHOT, &kasan_flags);
	return 1;
}
__setup("kasan_multi_shot", kasan_set_multi_shot);

/*
 * This function is used to check whether KASAN reports are suppressed for
 * software KASAN modes via kasan_disable/enable_current() critical sections.
 *
 * This is done to avoid:
 * 1. False-positive reports when accessing slab metadata,
 * 2. Deadlocking when poisoned memory is accessed by the reporting code.
 *
 * Hardware Tag-Based KASAN instead relies on:
 * For #1: Resetting tags via kasan_reset_tag().
 * For #2: Suppression of tag checks via CPU, see report_suppress_start/end().
 */
/*
 * report_suppressed_sw() - 判断软件 KASAN 当前任务是否处于报告抑制临界区
 * 业务背景：报告器访问 metadata/poison 内存时不能再次触发 KASAN。
 * 入参：无；返回是否应抑制。注意事项：只适用 Generic/SW_TAGS，HW_TAGS 由 CPU 控制。
 */
static bool report_suppressed_sw(void)
{
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	/* depth 非零表示当前线程已进入 kasan_disable_current 临界区。 */
	if (current->kasan_depth)
		return true;
#endif
	/* 非软件模式或 depth 为零时允许继续进入统一报告路径。 */
	return false;
}

/* 开始报告保护：HW 关闭本 CPU tag 检查并禁止迁移，SW 增加 current depth。 */
static void report_suppress_start(void)
{
#ifdef CONFIG_KASAN_HW_TAGS
	/*
	 * Disable preemption for the duration of printing a KASAN report, as
	 * hw_suppress_tag_checks_start() disables checks on the current CPU.
	 */
	/* 抑制状态属于当前 CPU，迁移后会在另一 CPU 意外重新开启 tag 检查。 */
	preempt_disable();
	hw_suppress_tag_checks_start();
#else
	kasan_disable_current();
#endif
}

/* 与 start 配对恢复检查和抢占；必须在报告锁、lockdep 状态恢复之后调用。 */
static void report_suppress_stop(void)
{
#ifdef CONFIG_KASAN_HW_TAGS
	hw_suppress_tag_checks_stop();
	/* 先恢复 tag 检查，再允许迁移；反序会让新 CPU 的状态与抑制深度失配。 */
	preempt_enable();
#else
	/* software 模式以 current depth 成对维护报告抑制，退出时必须减少同一层。 */
	kasan_enable_current();
#endif
}

/*
 * Used to avoid reporting more than one KASAN bug unless kasan_multi_shot
 * is enabled. Note that KASAN tests effectively enable kasan_multi_shot
 * for their duration.
 */
/* 原子 test_and_set 保证默认只由一个 CPU 领取首份报告；multi-shot 是显式例外。 */
static bool report_enabled(void)
{
	/* multi-shot 明确放弃“首错唯一”限制，主要供测试收集多个预期错误。 */
	if (test_bit(KASAN_BIT_MULTI_SHOT, &kasan_flags))
		return true;
	/* test_and_set 的返回旧值，只有看到 0 的 CPU 获得本次首错输出权。 */
	return !test_and_set_bit(KASAN_BIT_REPORTED, &kasan_flags);
}

#if IS_ENABLED(CONFIG_KASAN_KUNIT_TEST)

VISIBLE_IF_KUNIT bool kasan_save_enable_multi_shot(void)
{
	return test_and_set_bit(KASAN_BIT_MULTI_SHOT, &kasan_flags);
}
EXPORT_SYMBOL_IF_KUNIT(kasan_save_enable_multi_shot);

VISIBLE_IF_KUNIT void kasan_restore_multi_shot(bool enabled)
{
	/* 只有保存前未开启时才清位，避免嵌套测试错误关闭外层策略。 */
	if (!enabled)
		clear_bit(KASAN_BIT_MULTI_SHOT, &kasan_flags);
}
EXPORT_SYMBOL_IF_KUNIT(kasan_restore_multi_shot);

#endif

#if IS_ENABLED(CONFIG_KASAN_KUNIT_TEST)

/*
 * Whether the KASAN KUnit test suite is currently being executed.
 * Updated in kasan_test.c.
 */
/* KUnit suite 写者与报告读者用 READ/WRITE_ONCE 防止编译器拆分或缓存该状态。 */
static bool kasan_kunit_executing;

VISIBLE_IF_KUNIT void kasan_kunit_test_suite_start(void)
{
	/* 发布 suite 已启动，报告 CPU 随后用 READ_ONCE 观察这个诊断分类状态。 */
	WRITE_ONCE(kasan_kunit_executing, true);
}
EXPORT_SYMBOL_IF_KUNIT(kasan_kunit_test_suite_start);

VISIBLE_IF_KUNIT void kasan_kunit_test_suite_end(void)
{
	/* suite 结束后恢复普通 KUnit 的失败归属规则。 */
	WRITE_ONCE(kasan_kunit_executing, false);
}
EXPORT_SYMBOL_IF_KUNIT(kasan_kunit_test_suite_end);

static bool kasan_kunit_test_suite_executing(void)
{
	/* READ_ONCE 与 suite start/end 的 WRITE_ONCE 配对，只保证状态读取不被撕裂。 */
	return READ_ONCE(kasan_kunit_executing);
}

#else /* CONFIG_KASAN_KUNIT_TEST */

static inline bool kasan_kunit_test_suite_executing(void) { return false; }

#endif /* CONFIG_KASAN_KUNIT_TEST */

#if IS_ENABLED(CONFIG_KUNIT)

/* 报告发生在非 KASAN KUnit case 时标记该 case 失败，但不干扰 KASAN 自测预期故障。 */
static void fail_non_kasan_kunit_test(void)
{
	struct kunit *test;

	/* KASAN 自身 suite 故意制造错误，不能因此让 KUnit 框架将测试判失败。 */
	if (kasan_kunit_test_suite_executing())
		return;

	/* current 仅在 KUnit 上下文携带 test，空指针表示报告不属于一个可标记的 case。 */
	test = current->kunit_test;
	if (test)
		kunit_set_failure(test);
}

#else /* CONFIG_KUNIT */

static inline void fail_non_kasan_kunit_test(void) { }

#endif /* CONFIG_KUNIT */

/* 串行化 printk、stack dump 与 metadata 输出；raw 锁适合报告可能处在低层错误上下文。 */
static DEFINE_RAW_SPINLOCK(report_lock);

/*
 * start_report() - 建立不可递归、不可交错的 KASAN 输出临界区
 * 业务背景：报告中会访问复杂内核状态，必须关闭 trace/lockdep 干扰并串行输出。
 * 入参：flags 为输出 IRQ 状态，调用者在 end_report 成对归还；返回：无。
 * 注意事项：report_suppress_start 先防递归，report_lock 后保护整段打印。
 */
static void start_report(unsigned long *flags)
{
	/* 先归属 KUnit 失败，再关闭会改写/递归报告的 tracing 和 lockdep 机制。 */
	fail_non_kasan_kunit_test();
	/* Respect the /proc/sys/kernel/traceoff_on_warning interface. */
	disable_trace_on_warning();
	/* Do not allow LOCKDEP mangling KASAN reports. */
	lockdep_off();
	/* Make sure we don't end up in loop. */
	report_suppress_start();
	raw_spin_lock_irqsave(&report_lock, *flags);
	/* 分隔线在锁内输出，保证多 CPU 报告不会互相穿插。 */
	pr_err("==================================================================\n");
}

/*
 * end_report() - 结束报告、应用 panic 策略并恢复诊断环境
 * 入参：flags 来自 start，addr/is_write 供 trace 与 panic_on_write 决策；返回无。
 * 注意事项：panic 分支不返回；正常路径先解锁再恢复 lockdep/抑制状态，避免锁内递归。
 */
static void end_report(unsigned long *flags, const void *addr, bool is_write)
{
	/* trace 只有真实地址才写结束事件；异步 fault 没有可靠地址。 */
	/* 有地址的同步报告记录 trace 结束点，异步报告以 NULL 明确省略。 */
	if (addr)
		trace_error_report_end(ERROR_DETECTOR_KASAN,
				       (unsigned long)addr);
	pr_err("==================================================================\n");
	raw_spin_unlock_irqrestore(&report_lock, *flags);
	if (!test_bit(KASAN_BIT_MULTI_SHOT, &kasan_flags))
		check_panic_on_warn("KASAN");
	/* 启动期策略在锁外执行：report 继续、panic 终止、write-only 只终止写错误。 */
	switch (kasan_arg_fault) {
	case KASAN_ARG_FAULT_DEFAULT:
	case KASAN_ARG_FAULT_REPORT:
		break;
	case KASAN_ARG_FAULT_PANIC:
		panic("kasan.fault=panic set ...\n");
		break;
	case KASAN_ARG_FAULT_PANIC_ON_WRITE:
		/* 缺少访问方向的异步报告保守传 true，因此该策略也会 panic。 */
		if (is_write)
			panic("kasan.fault=panic_on_write set ...\n");
		break;
	}
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
	lockdep_on();
	report_suppress_stop();
}

/* 格式化访问/非法释放的首行；info 是本次报告的栈上快照，不修改其内容。 */
static void print_error_description(struct kasan_report_info *info)
{
	/* bug_type 由 complete_report_info 保证已填，ip 用符号格式关联到违规调用点。 */
	pr_err("BUG: KASAN: %s in %pS\n", info->bug_type, (void *)info->ip);

	/* free 类型没有普通访问宽度，单独使用释放者信息描述操作。 */
	if (info->type != KASAN_REPORT_ACCESS) {
		pr_err("Free of addr %px by task %s/%d\n",
			info->access_addr, current->comm, task_pid_nr(current));
		return;
	}

	if (info->access_size)
		/* 有 size 的插桩访问保留精确字节数；零表示 free 或未知宽度场景。 */
		pr_err("%s of size %zu at addr %px by task %s/%d\n",
			info->is_write ? "Write" : "Read", info->access_size,
			info->access_addr, current->comm, task_pid_nr(current));
	else
		pr_err("%s at addr %px by task %s/%d\n",
			info->is_write ? "Write" : "Read",
			info->access_addr, current->comm, task_pid_nr(current));
}

/* 输出 Stack Depot 历史；track 只借用句柄，仓库负责保存的栈副本生命周期。 */
static void print_track(struct kasan_track *track, const char *prefix)
{
#ifdef CONFIG_KASAN_EXTRA_INFO
	u64 ts_nsec = track->timestamp;
	unsigned long rem_usec;

	ts_nsec <<= 9;
	/* timestamp 压缩存储，左移后再拆为秒和微秒以保持报告可读性。 */
	rem_usec = do_div(ts_nsec, NSEC_PER_SEC) / 1000;

	pr_err("%s by task %u on cpu %d at %lu.%06lus:\n",
			prefix, track->pid, track->cpu,
			(unsigned long)ts_nsec, rem_usec);
#else
	/* 未启用 EXTRA_INFO 时只保留 pid，避免引用不可用的 CPU/时间字段。 */
	pr_err("%s by task %u:\n", prefix, track->pid);
#endif /* CONFIG_KASAN_EXTRA_INFO */
	/* Stack Depot 句柄为零时仍明确提示，避免把缺栈误读为没有分配历史。 */
	/* stack_depot_print 只接受非零句柄，零句柄走显式缺失提示。 */
	/* pid 来自保存 track 时的任务快照，不要求报告时该任务仍存活。 */
	if (track->stack)
		stack_depot_print(track->stack);
	else
		pr_err("(stack is not available)\n");
}

/* 仅线性映射地址可直接转 page；vmalloc 后续由专门分支处理。 */
static inline struct page *addr_to_page(const void *addr)
{
	if (virt_addr_valid(addr))
		return virt_to_head_page(addr);
	return NULL;
}

/* 依据 object 与 alloc_size 计算左/内/右关系；不重新读取可能并发变化的 allocator 状态。 */
static void describe_object_addr(const void *addr, struct kasan_report_info *info)
{
	unsigned long access_addr = (unsigned long)addr;
	unsigned long object_addr = (unsigned long)info->object;
	const char *rel_type, *region_state = "";
	int rel_bytes;

	pr_err("The buggy address belongs to the object at %px\n"
	       " which belongs to the cache %s of size %d\n",
		info->object, info->cache->name, info->cache->object_size);

	/* 三分关系按请求 alloc_size，而不是 cache object_size，才能正确展示 kmalloc 尾部。 */
	/* 相对偏移按有符号 int 打印，数值来自两个已稳定地址的差。 */
	/* left/right/inside 三种字符串与随后区间输出配对，方便读者复原边界。 */
	/* 计算使用 unsigned long 地址差，分支先保证不会产生负数。 */
	if (access_addr < object_addr) {
		rel_type = "to the left";
		rel_bytes = object_addr - access_addr;
	} else if (access_addr >= object_addr + info->alloc_size) {
		rel_type = "to the right";
		rel_bytes = access_addr - (object_addr + info->alloc_size);
	} else {
		rel_type = "inside";
		rel_bytes = access_addr - object_addr;
	}

	/*
	 * Tag-Based modes use the stack ring to infer the bug type, but the
	 * memory region state description is generated based on the metadata.
	 * Thus, defining the region state as below can contradict the metadata.
	 * Fixing this requires further improvements, so only infer the state
	 * for the Generic mode.
	 */
	/* tag 模式的 stack ring 与 metadata 状态可能不同，故仅 Generic 推断 allocated/freed。 */
	if (IS_ENABLED(CONFIG_KASAN_GENERIC)) {
		/* 字符串来自 mode-specific 分类，比较仅影响人类可读 region_state。 */
		if (strcmp(info->bug_type, "slab-out-of-bounds") == 0)
			region_state = "allocated ";
		else if (strcmp(info->bug_type, "slab-use-after-free") == 0)
			region_state = "freed ";
	}

	pr_err("The buggy address is located %d bytes %s of\n"
	       " %s%zu-byte region [%px, %px)\n",
	       rel_bytes, rel_type, region_state, info->alloc_size,
	       (void *)object_addr, (void *)(object_addr + info->alloc_size));
}

/* 分配/释放栈独立可用；缺一个不妨碍打印另一个和辅助栈。 */
static void describe_object_stacks(struct kasan_report_info *info)
{
	/* 两条历史独立存在，释放前错误通常只有 alloc 栈。 */
	if (info->alloc_track.stack) {
		print_track(&info->alloc_track, "Allocated");
		pr_err("\n");
	}

	/* free 栈存在说明对象至少经过一次记录的释放，UAF 报告可同时展示两端。 */
	/* 释放历史只在对象实际离开分配态后存在，缺失不是报告失败。 */
	if (info->free_track.stack) {
		print_track(&info->free_track, "Freed");
		pr_err("\n");
	}

	/* Generic 的辅助栈描述 realloc/mempool 等历史；标签模式实现为空桩。 */
	/* 辅助栈可能为空，但调用保持 Generic/tags 报告格式的一致性。 */
	kasan_print_aux_stacks(info->cache, info->object);
}

/* 栈采集关闭时仍输出对象位置，避免诊断完全依赖可选的 Stack Depot。 */
static void describe_object(const void *addr, struct kasan_report_info *info)
{
	/* 关闭栈采集后跳过 depot 查找，但地址相对位置仍必须打印。 */
	if (kasan_stack_collection_enabled())
		describe_object_stacks(info);
	describe_object_addr(addr, info);
}

/* 判断符号解析是否有意义；不把 init task stack 误报成普通全局变量。 */
static inline bool kernel_or_module_addr(const void *addr)
{
	/* 内核与模块符号均可由 %pS 解析，其他地址转由栈/vmalloc/page 路径处理。 */
	/* 该 helper 只分类地址空间，不触发符号解析或页表访问。 */
	if (is_kernel((unsigned long)addr))
		return true;
	/* module 地址可在模块卸载前解析；报告窗口不保证延长模块引用。 */
	if (is_module_address((unsigned long)addr))
		return true;
	return false;
}

/* init 线程栈是静态对象但需要由栈路径描述，故单独排除。 */
static inline bool init_task_stack_addr(const void *addr)
{
	return addr >= (void *)&init_thread_union.stack &&
		(addr <= (void *)&init_thread_union.stack +
			sizeof(init_thread_union.stack));
}

/*
 * print_address_description() - 依次从对象、符号、栈、vmalloc 与 page 解释坏地址
 * 入参：addr 是已 reset_tag 的地址，tag 保留原逻辑 tag，info 为报告快照；返回无。
 * 注意事项：仅在 metadata 可达时调用；各 helper 诊断性读取，不取得对象长期引用。
 */
static void print_address_description(void *addr, u8 tag,
				      struct kasan_report_info *info)
{
	struct page *page = addr_to_page(addr);

	/* 报告者自身栈先输出，为后续对象归属/metadata 提供调用上下文。 */
	dump_stack_lvl(KERN_ERR);
	pr_err("\n");

	/* 两者必须同时存在才按 slab 对象解释，防止 non-slab 地址误解引用 NULL cache。 */
	/* 非 slab 地址没有 object 元数据，后续仍可走符号、vmalloc 或 page 诊断。 */
	if (info->cache && info->object) {
		describe_object(addr, info);
		pr_err("\n");
	}

	/* 符号归属与对象归属可同时输出，它们回答不同层次的地址来源。 */
	if (kernel_or_module_addr(addr) && !init_task_stack_addr(addr)) {
		pr_err("The buggy address belongs to the variable:\n");
		pr_err(" %pS\n", addr);
		pr_err("\n");
	}

	/* 仅当前任务自身栈支持可靠 frame 解析，其他栈地址仍由 page/符号路径描述。 */
	if (object_is_on_stack(addr)) {
		/*
		 * Currently, KASAN supports printing frame information only
		 * for accesses to the task's own stack.
		 */
		kasan_print_address_stack_frame(addr);
		pr_err("\n");
	}

	/* vmalloc 地址不经 virt_to_page；先让 vmalloc 子系统尝试输出所属对象。 */
	if (is_vmalloc_addr(addr)) {
		pr_err("The buggy address belongs to a");
		if (!vmalloc_dump_obj(addr))
			/* vmalloc 没有归属对象时仍说明这是虚拟映射，再尝试取得 backing page。 */
			pr_cont(" vmalloc virtual mapping\n");
		page = vmalloc_to_page(addr);
	}

	if (page) {
		pr_err("The buggy address belongs to the physical page:\n");
		dump_page(page, "kasan: bad access detected");
		pr_err("\n");
	}
}

/* row 覆盖一个 metadata 打印行；此判断决定在 hex dump 下方标出脱靶字节。 */
static bool meta_row_is_guilty(const void *row, const void *addr)
{
	/* row 是闭开区间起点，右端排除避免相邻行的指示符重复。 */
	return (row <= addr) && (addr < row + META_MEM_BYTES_PER_ROW);
}

/* 将真实地址换算为 hex dump 字符列；格式常量必须和 print_hex_dump 输出配对。 */
static int meta_pointer_offset(const void *row, const void *addr)
{
	/*
	 * Memory state around the buggy address:
	 *  ff00ff00ff00ff00: 00 00 00 05 fe fe fe fe fe fe fe fe fe fe fe fe
	 *  ...
	 *
	 * The length of ">ff00ff00ff00ff00: " is
	 *    3 + (BITS_PER_LONG / 8) * 2 chars.
	 * The length of each granule metadata is 2 bytes
	 *    plus 1 byte for space.
	 */
	/* 地址文本宽度随字长变化，granule 偏移每格占两个 hex 字符和空格。 */
	return 3 + (BITS_PER_LONG / 8) * 2 +
		(addr - row) / KASAN_GRANULE_SIZE * 3 + 1;
}

/* 打印故障行前后固定数量 metadata 行；避免传 shadow 地址触发 Generic 递归检查。 */
static void print_memory_metadata(const void *addr)
{
	int i;
	void *row;

	/* 从故障地址所在行之前的固定窗口开始，确保中间行可被 ^ 标记。 */
	row = (void *)round_down((unsigned long)addr, META_MEM_BYTES_PER_ROW)
			- META_ROWS_AROUND_ADDR * META_MEM_BYTES_PER_ROW;

	pr_err("Memory state around the buggy address:\n");

	/* 每轮只读取一行 metadata，避免报告路径跨越过多 shadow 而增加递归风险。 */
	for (i = -META_ROWS_AROUND_ADDR; i <= META_ROWS_AROUND_ADDR; i++) {
		char buffer[4 + (BITS_PER_LONG / 8) * 2];
		char metadata[META_BYTES_PER_ROW];

		snprintf(buffer, sizeof(buffer),
				(i == 0) ? ">%px: " : " %px: ", row);

		/*
		 * We should not pass a shadow pointer to generic
		 * function, because generic functions may try to
		 * access kasan mapping for the passed address.
		 */
		/* 传真实 row 而非 shadow 指针，避免 generic helper 对 shadow 再做一次映射。 */
		kasan_metadata_fetch_row(&metadata[0], row);

		print_hex_dump(KERN_ERR, buffer,
			DUMP_PREFIX_NONE, META_BYTES_PER_ROW, 1,
			metadata, META_BYTES_PER_ROW, 0);

		if (meta_row_is_guilty(row, addr))
			pr_err("%*c\n", meta_pointer_offset(row, addr), '^');

		row += META_MEM_BYTES_PER_ROW;
	}
}

/* 汇总首行、tag、地址归属及 metadata；访问地址先 reset_tag，原 tag 单独传给 tag 输出。 */
static void print_report(struct kasan_report_info *info)
{
	void *addr = kasan_reset_tag((void *)info->access_addr);
	u8 tag = get_tag((void *)info->access_addr);

	/* 先写摘要，随后条件化写 tag/元数据；无 metadata 仍给出当前栈。 */
	print_error_description(info);
	if (addr_has_metadata(addr))
		kasan_print_tags(tag, info->first_bad_addr);
	pr_err("\n");

	/* metadata 分支才可安全读取 shadow/tag；否则退化为栈回溯。 */
	if (addr_has_metadata(addr)) {
		print_address_description(addr, tag, info);
		print_memory_metadata(info->first_bad_addr);
	} else {
		dump_stack_lvl(KERN_ERR);
	}
}

/*
 * complete_report_info() - 从访问快照定位对象并委托模式层判定 bug 类型
 * 入参/出参：info 输入访问现场，原地补全 first_bad_addr、cache/object、大小和 track。
 * 注意事项：slab/object 仅在报告窗口借用；找不到 slab 时清空二者防止误用。
 */
static void complete_report_info(struct kasan_report_info *info)
{
	void *addr = kasan_reset_tag((void *)info->access_addr);
	struct slab *slab;

	/* access 需扫描首坏字节；free 类错误直接以目标地址为证据。 */
	/* 访问与 free 两类入口在这里汇合为同一 report_info。 */
	/* first_bad_addr 是所有后续 tag、对象和 metadata 打印的统一锚点。 */
	if (info->type == KASAN_REPORT_ACCESS)
		info->first_bad_addr = kasan_find_first_bad_addr(
					(void *)info->access_addr, info->access_size);
	else
		info->first_bad_addr = addr;

	/* slab 查找成功才可调用 nearest_obj；否则明确清空避免输出陈旧字段。 */
	slab = kasan_addr_to_slab(addr);
	/* nearest_obj 只在确认 slab 后执行，避免把任意线性映射地址解释为 cache 对象。 */
	if (slab) {
		info->cache = slab->slab_cache;
		info->object = nearest_obj(info->cache, slab, addr);

		/* Try to determine allocation size based on the metadata. */
		info->alloc_size = kasan_get_alloc_size(info->object, info->cache);
		/* Fallback to the object size if failed. */
		if (!info->alloc_size)
			info->alloc_size = info->cache->object_size;
	} else
		info->cache = info->object = NULL;

	/* invalid/double free 是公共语义；普通访问类型由 Generic/tags 层进一步分类。 */
	switch (info->type) {
	case KASAN_REPORT_INVALID_FREE:
		info->bug_type = "invalid-free";
		break;
	case KASAN_REPORT_DOUBLE_FREE:
		info->bug_type = "double-free";
		break;
	default:
		/* bug_type filled in by kasan_complete_mode_report_info. */
		break;
	}

	/* Fill in mode-specific report info fields. */
	/* Generic/tags 层在公共字段已就绪后填 bug_type 与历史栈，顺序不可颠倒。 */
	kasan_complete_mode_report_info(info);
}

/*
 * kasan_report_invalid_free() - 报告分配器发现的非法或重复释放
 * 入参：ptr 是被释放地址，ip 是调用点，type 指定 invalid/double-free；返回无。
 * 注意事项：不能因 software 抑制深度跳过：该错误不是 poison 访问递归造成；完成后
 * 以 write 语义执行 panic_on_write，因为释放会改 allocator metadata。
 */
void kasan_report_invalid_free(void *ptr, unsigned long ip, enum kasan_report_type type)
{
	unsigned long flags;
	struct kasan_report_info info;

	/*
	 * Do not check report_suppressed_sw(), as an invalid-free cannot be
	 * caused by accessing poisoned memory and thus should not be suppressed
	 * by kasan_disable/enable_current() critical sections.
	 *
	 * Note that for Hardware Tag-Based KASAN, kasan_report_invalid_free()
	 * is triggered by explicit tag checks and not by the ones performed by
	 * the CPU. Thus, reporting invalid-free is not suppressed as well.
	 */
	/* 首错去重在进入 report_lock 前完成，避免被抑制的重复错误淹没日志。 */
	if (unlikely(!report_enabled()))
		return;

	start_report(&flags);

	/* 栈上 info 先清零，保证可选模式字段在未填充时不会泄露垃圾数据到 printk。 */
	__memset(&info, 0, sizeof(info));
	info.type = type;
	info.access_addr = ptr;
	info.access_size = 0;
	info.is_write = false;
	info.ip = ip;

	complete_report_info(&info);

	/* 报告快照完整后才打印，保证输出各段使用同一访问和对象结论。 */
	print_report(&info);

	/*
	 * Invalid free is considered a "write" since the allocator's metadata
	 * updates involves writes.
	 */
	end_report(&flags, ptr, true);
}

/*
 * kasan_report() is the only reporting function that uses
 * user_access_save/restore(): kasan_report_invalid_free() cannot be called
 * from a UACCESS region, and kasan_report_async() is not used on x86.
 */
/*
 * kasan_report() - 处理插桩访问检查发现的非法读写
 * 入参：addr/size/is_write/ip 是违规现场；返回 true 表示已产生报告，false 为抑制/去重。
 * 注意事项：唯一需要 user_access_save/restore 的报告入口，所有出口都必须恢复 UACCESS。
 */
bool kasan_report(const void *addr, size_t size, bool is_write,
			unsigned long ip)
{
	bool ret = true;
	/* UACCESS 区会影响 report 中的用户访问约束，所有抑制出口也必须 restore。 */
	unsigned long ua_flags = user_access_save();
	unsigned long irq_flags;
	struct kasan_report_info info;

	/* software depth 防止报告器自触发；去重拒绝后仍跳 out 恢复 UACCESS 状态。 */
	/* 抑制或去重均不进入 start_report，避免无效路径取得 raw spinlock。 */
	if (unlikely(report_suppressed_sw()) || unlikely(!report_enabled())) {
		ret = false;
		goto out;
	}

	start_report(&irq_flags);

	/* 将访问现场一次性快照到 info，之后任何打印都不依赖调用者栈上的原参数变化。 */
	__memset(&info, 0, sizeof(info));
	info.type = KASAN_REPORT_ACCESS;
	info.access_addr = addr;
	info.access_size = size;
	info.is_write = is_write;
	info.ip = ip;

	complete_report_info(&info);

	print_report(&info);

	end_report(&irq_flags, (void *)addr, is_write);

out:
	/* 无论报告被跳过还是完成，恢复调用者原有 UACCESS 状态。 */
	user_access_restore(ua_flags);

	return ret;
}

#ifdef CONFIG_KASAN_HW_TAGS
void kasan_report_async(void)
{
	unsigned long flags;

	/*
	 * Do not check report_suppressed_sw(), as
	 * kasan_disable/enable_current() critical sections do not affect
	 * Hardware Tag-Based KASAN.
	 */
	/* 异步 CPU fault 没有可用地址/方向细节，只能按全局报告策略输出一次。 */
	if (unlikely(!report_enabled()))
		return;

	/* async fault 无地址细节但仍需使用相同锁/抑制协议保护输出。 */
	start_report(&flags);
	pr_err("BUG: KASAN: invalid-access\n");
	pr_err("Asynchronous fault: no details available\n");
	pr_err("\n");
	dump_stack_lvl(KERN_ERR);
	/*
	 * Conservatively set is_write=true, because no details are available.
	 * In this mode, kasan.fault=panic_on_write is like kasan.fault=panic.
	 */
	end_report(&flags, NULL, true);
}
#endif /* CONFIG_KASAN_HW_TAGS */

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
/*
 * With compiler-based KASAN modes, accesses to bogus pointers (outside of the
 * mapped kernel address space regions) cause faults when KASAN tries to check
 * the shadow memory before the actual memory access. This results in cryptic
 * GPF reports, which are hard for users to interpret. This hook helps users to
 * figure out what the original bogus pointer was.
 */
/*
 * kasan_non_canonical_hook() - 将访问 shadow 时产生的难懂 GPF 反解为原坏指针线索
 * 入参：addr 是触发异常的 shadow 候选地址；返回无。注意事项：非 canonical 反解
 * 只能给出概率性诊断，不能当作真实访问地址或对象 ownership 的证明。
 */
void kasan_non_canonical_hook(unsigned long addr)
{
	unsigned long orig_addr, user_orig_addr;
	const char *bug_type;

	/*
	 * All addresses that came as a result of the memory-to-shadow mapping
	 * (even for bogus pointers) must be >= KASAN_SHADOW_OFFSET.
	 */
	/* 不可能由 mem-to-shadow 变换产生的低地址不作猜测，避免误导诊断。 */
	if (addr < KASAN_SHADOW_OFFSET)
		return;

	/* 反解只恢复 shadow 比例关系，非 canonical 情况仍需在最终提示中保留不确定性。 */
	orig_addr = (unsigned long)kasan_shadow_to_mem((void *)addr);

	/* Strip pointer tag before comparing against userspace ranges */
	user_orig_addr = (unsigned long)set_tag((void *)orig_addr, 0);

	/*
	 * For faults near the shadow address for NULL, we can be fairly certain
	 * that this is a KASAN shadow memory access.
	 * For faults that correspond to the shadow for low or high canonical
	 * addresses, we can still be pretty sure: these shadow regions are a
	 * fairly narrow chunk of the address space.
	 * But the shadow for non-canonical addresses is a really large chunk
	 * of the address space. For this case, we still print the decoded
	 * address, but make it clear that this is not necessarily what's
	 * actually going on.
	 */
	/* NULL、用户区、狭窄 shadow 区和非 canonical 大区依次降低诊断确定性。 */
	if (user_orig_addr < PAGE_SIZE) {
		bug_type = "null-ptr-deref";
		orig_addr = user_orig_addr;
	} else if (user_orig_addr < TASK_SIZE) {
		bug_type = "probably user-memory-access";
		orig_addr = user_orig_addr;
	} else if (addr_in_shadow((void *)addr))
		bug_type = "probably wild-memory-access";
	else
		bug_type = "maybe wild-memory-access";
	/* 只输出反解后的候选区间，不把概率性分类升级为确定的对象诊断。 */
	pr_alert("KASAN: %s in range [0x%016lx-0x%016lx]\n", bug_type,
		 orig_addr, orig_addr + KASAN_GRANULE_SIZE - 1);
}
#endif
