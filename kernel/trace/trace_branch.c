// SPDX-License-Identifier: GPL-2.0
/*
 * unlikely profiler
 *
 * Copyright (C) 2008 Steven Rostedt <srostedt@redhat.com>
 */
/*
 * likely()/unlikely() 分支预测质量分析器。编译器宏为每个调用点生成静态
 * ftrace_likely_data，本文件一方面把每次结果累积为 correct/incorrect，另一
 * 方面在启用 branch tracer 时把事件复制进 trace ring buffer；stat tracer
 * 再遍历链接器 section，将长期累计结果输出到 tracefs。
 *
 * 生命周期：调用点描述符随内核或模块存在；ring-buffer entry 是独立快照，
 * 避免模块卸载后追踪读取悬空描述符。branch_tracing_mutex 只串行化启停，热
 * 路径靠发布顺序、每 CPU ring buffer 和 recursion bit 工作；统计递增并非原子，
 * 所以结果用于性能诊断而非精确计费。
 */
#include <linux/kallsyms.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/irqflags.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/ftrace.h>
#include <linux/hash.h>
#include <linux/fs.h>
#include <asm/local.h>

#include "trace.h"
#include "trace_stat.h"
#include "trace_output.h"

#ifdef CONFIG_BRANCH_TRACER

static struct tracer branch_trace;
static int branch_tracing_enabled __read_mostly;
static DEFINE_MUTEX(branch_tracing_mutex);

static struct trace_array *branch_tracer;

/*
 * 全局实体：branch_trace 描述 tracer 回调表；branch_tracing_enabled 是启用
 * 引用计数，允许多个使用者嵌套启停；branch_tracing_mutex 保护该计数和
 * branch_tracer 的发布。branch_tracer 借用 trace core 管理的 trace_array，
 * 只在 enabled 发布后被热路径读取。
 */

/*
 * probe_likely_condition() - 把一次分支判断写入 branch tracer ring buffer
 * @f: 调用点静态描述符的借用指针，模块卸载后可能失效，故不能存入 event。
 * @val: 本次条件的 0/1 结果；@expect: 编写者预测的 0/1 值。
 *
 * 可从被插桩分支所在的原子上下文进入，不能睡眠。函数先阻止递归，再关本地
 * 中断并设置 current recursion bit，检查当前 CPU tracer 状态、预留 event，
 * 把函数名/文件 basename/行号和正确性复制为自包含快照，最后提交并恢复状态。
 * 返回：无；预留失败、tracer 关闭或递归时静默丢弃事件，不转移 @f ownership。
 */
static void
probe_likely_condition(struct ftrace_likely_data *f, int val, int expect)
{
	struct trace_array *tr = branch_tracer;
	struct trace_buffer *buffer;
	struct ring_buffer_event *event;
	struct trace_branch *entry;
	unsigned long flags;
	unsigned int trace_ctx;
	const char *p;

	if (current->trace_recursion & TRACE_BRANCH_BIT)
		return;

	/*
	 * I would love to save just the ftrace_likely_data pointer, but
	 * this code can also be used by modules. Ugly things can happen
	 * if the module is unloaded, and then we go and read the
	 * pointer.  This is slower, but much safer.
	 */
	/*
	 * 最理想是只保存 ftrace_likely_data 指针，但该代码也用于模块；模块卸载后
	 * 再读取指针会成为 UAF。这里付出字符串复制成本换取 event 的独立生命周期。
	 */

	if (unlikely(!tr))
		return;

	raw_local_irq_save(flags);
	current->trace_recursion |= TRACE_BRANCH_BIT;
	if (!tracer_tracing_is_on_cpu(tr, raw_smp_processor_id()))
		goto out;

	trace_ctx = tracing_gen_ctx_flags(flags);
	buffer = tr->array_buffer.buffer;
	event = trace_buffer_lock_reserve(buffer, TRACE_BRANCH,
					  sizeof(*entry), trace_ctx);
	if (!event)
		goto out;

	entry	= ring_buffer_event_data(event);

	/* Strip off the path, only save the file */
	/* 去掉目录路径，只保存文件 basename，减小固定大小 trace entry 占用。 */
	p = f->data.file + strlen(f->data.file);
	while (p >= f->data.file && *p != '/')
		p--;
	p++;

	strscpy(entry->func, f->data.func);
	strscpy(entry->file, p);
	entry->constant = f->constant;
	entry->line = f->data.line;
	entry->correct = val == expect;

	trace_buffer_unlock_commit_nostack(buffer, event);

 out:
	current->trace_recursion &= ~TRACE_BRANCH_BIT;
	raw_local_irq_restore(flags);
}

/*
 * trace_likely_condition() - branch tracer 热路径开关
 * @f/@val/@expect: 原样借给 probe_likely_condition()，语义同上。
 * 不睡眠、无返回值；disabled 时只做一次读并返回，enabled 时生成事件。
 */
static inline
void trace_likely_condition(struct ftrace_likely_data *f, int val, int expect)
{
	if (!branch_tracing_enabled)
		return;

	probe_likely_condition(f, val, expect);
}

/*
 * enable_branch_tracing() - 发布 trace_array 并增加 branch tracer 启用计数
 * @tr: trace core 持有的 trace_array 借用指针，启用期间必须保持有效。
 * 进程上下文调用且会取得 mutex，因此可睡眠。先写 branch_tracer，再用 wmb
 * 保证热路径观察到 enabled 时也能看到有效指针，最后递增计数。返回恒为 0；
 * 副作用是后续分支开始写事件。
 */
int enable_branch_tracing(struct trace_array *tr)
{
	mutex_lock(&branch_tracing_mutex);
	branch_tracer = tr;
	/*
	 * Must be seen before enabling. The reader is a condition
	 * where we do not need a matching rmb()
	 */
	/*
	 * 指针发布必须先于 enabled。读侧对 enabled 的控制依赖足以约束后续使用，
	 * 因而原注释说明无需匹配 rmb；这里的 wmb 仍不可移到计数递增之后。
	 */
	smp_wmb();
	branch_tracing_enabled++;
	mutex_unlock(&branch_tracing_mutex);

	return 0;
}

/*
 * disable_branch_tracing() - 递减启用引用计数但不允许下溢
 * 入参/返回：无。进程上下文取得 mutex，可睡眠；计数已为零时保持不变。
 * 归零后热路径停止写 event，branch_tracer 指针保留供下一次启用覆盖。
 */
void disable_branch_tracing(void)
{
	mutex_lock(&branch_tracing_mutex);

	if (!branch_tracing_enabled)
		goto out_unlock;

	branch_tracing_enabled--;

 out_unlock:
	mutex_unlock(&branch_tracing_mutex);
}

/* branch_trace_init() 是 tracer .init 包装器，把 core 提供的 @tr 交给启用协议。 */
static int branch_trace_init(struct trace_array *tr)
{
	return enable_branch_tracing(tr);
}

/* branch_trace_reset() 是 .reset 包装器；@tr 无需使用，停用由全局引用计数完成。 */
static void branch_trace_reset(struct trace_array *tr)
{
	disable_branch_tracing();
}

/*
 * trace_branch_print() - 格式化一条 TRACE_BRANCH event
 * @iter: 当前 trace 迭代器，持有输入 entry 与输出 trace_seq；@flags 未使用；
 * @event: 事件描述符，当前实现无需读取。函数不取得对象 ownership，输出
 * ok/MISS、函数、文件和行号，返回 trace_handle_return() 的打印状态。
 */
static enum print_line_t trace_branch_print(struct trace_iterator *iter,
					    int flags, struct trace_event *event)
{
	struct trace_branch *field;

	trace_assign_type(field, iter->ent);

	trace_seq_printf(&iter->seq, "[%s] %s:%s:%d\n",
			 field->correct ? "  ok  " : " MISS ",
			 field->func,
			 field->file,
			 field->line);

	return trace_handle_return(&iter->seq);
}

/* branch_print_header() 向借用的 seq_file @s 输出列标题；无返回值，可睡眠性由 seq_file 调用上下文决定。 */
static void branch_print_header(struct seq_file *s)
{
	seq_puts(s, "#           TASK-PID    CPU#    TIMESTAMP  CORRECT"
		    "  FUNC:FILE:LINE\n"
		    "#              | |       |          |         |   "
		    "    |\n");
}

/* trace_branch_funcs/event/branch_trace 是静态注册描述符，由 trace core 借用到内核结束。 */

static struct trace_event_functions trace_branch_funcs = {
	.trace		= trace_branch_print,
};

static struct trace_event trace_branch_event = {
	.type		= TRACE_BRANCH,
	.funcs		= &trace_branch_funcs,
};

static struct tracer branch_trace __read_mostly =
{
	.name		= "branch",
	.init		= branch_trace_init,
	.reset		= branch_trace_reset,
#ifdef CONFIG_FTRACE_SELFTEST
	.selftest	= trace_selftest_startup_branch,
#endif /* CONFIG_FTRACE_SELFTEST */
	.print_header	= branch_print_header,
};

/*
 * init_branch_tracer() - 在 core initcall 阶段注册 event 与 branch tracer
 * 入参：无；仅初始化阶段调用，可睡眠。先注册 TRACE_BRANCH 打印事件，失败时
 * 警告并返回 1；成功后注册 tracer 并透传其返回值。注册成功后描述符由 core
 * 长期借用，无回滚资源由本函数持有。
 */
__init static int init_branch_tracer(void)
{
	int ret;

	ret = register_trace_event(&trace_branch_event);
	if (!ret) {
		pr_warn("Warning: could not register branch events\n");
		return 1;
	}
	return register_tracer(&branch_trace);
}
core_initcall(init_branch_tracer);

#else
/* CONFIG_BRANCH_TRACER=n 时保留空 stub，使累计统计路径无需条件编译。 */
static inline
void trace_likely_condition(struct ftrace_likely_data *f, int val, int expect)
{
}
#endif /* CONFIG_BRANCH_TRACER */

/*
 * ftrace_likely_update() - 更新调用点累计统计，并按需生成 branch trace event
 * @f: 当前调用点静态描述符的借用指针；@val: 实际布尔结果；@expect: 预测值；
 * @is_constant: 编译器是否判定条件恒定。
 *
 * 任意被插桩内核上下文可调用，不睡眠。user_access_save() 暂时切换 uaccess
 * 状态，避免跟踪代码在特殊访问域中运行；常量条件计入 constant 并强制视为
 * 预测正确，然后调用可配置 event 路径，最后累计 correct/incorrect 并恢复
 * uaccess。返回：无；计数递增当前非原子，允许诊断统计有少量丢失更新。
 */
void ftrace_likely_update(struct ftrace_likely_data *f, int val,
			  int expect, int is_constant)
{
	unsigned long flags = user_access_save();

	/* A constant is always correct */
	/* 编译期常量没有预测失败意义：单独累计 constant，并归入 correct。 */
	if (is_constant) {
		f->constant++;
		val = expect;
	}
	/*
	 * I would love to have a trace point here instead, but the
	 * trace point code is so inundated with unlikely and likely
	 * conditions that the recursive nightmare that exists is too
	 * much to try to get working. At least for now.
	 */
	/*
	 * 原意是希望使用 tracepoint，但 tracepoint 实现自身充满 likely/unlikely，
	 * 会形成难以控制的递归；当前专用调用绕开该递归噩梦。
	 */
	trace_likely_condition(f, val, expect);

	/* FIXME: Make this atomic! */
	/* FIXME：这些递增仍需改成原子统计；并发 CPU 可能丢失更新。 */
	if (val == expect)
		f->data.correct++;
	else
		f->data.incorrect++;

	user_access_restore(flags);
}
EXPORT_SYMBOL(ftrace_likely_update);

extern unsigned long __start_annotated_branch_profile[];
extern unsigned long __stop_annotated_branch_profile[];

/* 两个链接器边界符号界定 _ftrace_annotated_branch section，stat 迭代器只借用其间静态对象。 */

/* annotated_branch_stat_headers() 向 @m 输出 annotated 统计列头，成功恒返回 0。 */
static int annotated_branch_stat_headers(struct seq_file *m)
{
	seq_puts(m, " correct incorrect  % "
		    "       Function                "
		    "  File              Line\n"
		    " ------- ---------  - "
		    "       --------                "
		    "  ----              ----\n");
	return 0;
}

/*
 * get_incorrect_percent() - 计算借用统计 @p 的错误百分比
 * 有样本时返回 0..100；完全无样本返回 -1。先乘 100 再除总数，整数结果向下
 * 截断；只读快照可能与并发非原子更新交错，因此是近似诊断值。
 */
static inline long get_incorrect_percent(const struct ftrace_branch_data *p)
{
	long percent;

	if (p->correct) {
		percent = p->incorrect * 100;
		percent /= p->correct + p->incorrect;
	} else
		percent = p->incorrect ? 100 : -1;

	return percent;
}

/* branch_stat_process_file() 从借用的 @p->file 返回指向 basename 的内部借用指针；不分配内存。 */
static const char *branch_stat_process_file(struct ftrace_branch_data *p)
{
	const char *f;

	/* Only print the file, not the path */
	/* 只打印文件名而非完整路径；逆向扫描到最后一个 '/'。 */
	f = p->file + strlen(p->file);
	while (f >= p->file && *f != '/')
		f--;
	return ++f;
}

/* branch_stat_show() 输出错误百分比和调用点；@m/@p/@f 均借用，无返回值。 */
static void branch_stat_show(struct seq_file *m,
			     struct ftrace_branch_data *p, const char *f)
{
	long percent;

	/*
	 * The miss is overlayed on correct, and hit on incorrect.
	 */
	/* 同一 union 中 miss 覆盖 correct、hit 覆盖 incorrect，允许两类 section 共用格式器。 */
	percent = get_incorrect_percent(p);

	if (percent < 0)
		seq_puts(m, "  X ");
	else
		seq_printf(m, "%3ld ", percent);

	seq_printf(m, "%-30.30s %-20.20s %d\n", p->func, f, p->line);
}

/* branch_stat_show_normal() 输出原始 correct/incorrect 后复用公共尾部，成功返回 0。 */
static int branch_stat_show_normal(struct seq_file *m,
				   struct ftrace_branch_data *p, const char *f)
{
	seq_printf(m, "%8lu %8lu ",  p->correct, p->incorrect);
	branch_stat_show(m, p, f);
	return 0;
}

/*
 * annotate_branch_stat_show() - 输出一条 likely/unlikely 调用点统计
 * @m: seq_file 输出；@v: 借用的 ftrace_likely_data。无常量样本走普通格式；
 * 有常量样本则显示 correct/constant，并动态补齐 incorrect 列，最后返回 0。
 */
static int annotate_branch_stat_show(struct seq_file *m, void *v)
{
	struct ftrace_likely_data *p = v;
	const char *f;
	int l;

	f = branch_stat_process_file(&p->data);

	if (!p->constant)
		return branch_stat_show_normal(m, &p->data, f);

	l = snprintf(NULL, 0, "/%lu", p->constant);
	l = l > 8 ? 0 : 8 - l;

	seq_printf(m, "%8lu/%lu %*lu ",
		   p->data.correct, p->constant, l, p->data.incorrect);
	branch_stat_show(m, &p->data, f);
	return 0;
}

/* stat_start 返回 annotated section 首地址；@trace 不参与，返回指针不转移 ownership。 */
static void *annotated_branch_stat_start(struct tracer_stat *trace)
{
	return __start_annotated_branch_profile;
}

/* stat_next 前进一个定长描述符，到达链接器结束边界返回 NULL；@idx 由框架维护但本实现无需使用。 */
static void *
annotated_branch_stat_next(void *v, int idx)
{
	struct ftrace_likely_data *p = v;

	++p;

	if ((void *)p >= (void *)__stop_annotated_branch_profile)
		return NULL;

	return p;
}

/*
 * annotated_branch_stat_cmp() - 为统计排序：错误率更低者返回负值，随后比较错误数，
 * 最后让 correct 更多者返回负值。tracer_stat 的排序方向使更差案例优先展示；
 * @p1/@p2 均为只读借用对象，无副作用，返回 -1/0/1。
 */
static int annotated_branch_stat_cmp(const void *p1, const void *p2)
{
	const struct ftrace_branch_data *a = p1;
	const struct ftrace_branch_data *b = p2;

	long percent_a, percent_b;

	percent_a = get_incorrect_percent(a);
	percent_b = get_incorrect_percent(b);

	if (percent_a < percent_b)
		return -1;
	if (percent_a > percent_b)
		return 1;

	if (a->incorrect < b->incorrect)
		return -1;
	if (a->incorrect > b->incorrect)
		return 1;

	/*
	 * Since the above shows worse (incorrect) cases
	 * first, we continue that by showing best (correct)
	 * cases last.
	 */
	/* 前面让错误案例优先，这里继续该次序，把最佳（correct 多）案例放到最后。 */
	if (a->correct > b->correct)
		return -1;
	if (a->correct < b->correct)
		return 1;

	return 0;
}

static struct tracer_stat annotated_branch_stats = {
	.name = "branch_annotated",
	.stat_start = annotated_branch_stat_start,
	.stat_next = annotated_branch_stat_next,
	.stat_cmp = annotated_branch_stat_cmp,
	.stat_headers = annotated_branch_stat_headers,
	.stat_show = annotate_branch_stat_show
};

/* annotated_branch_stats 汇集 section 遍历、排序、表头和单行输出回调，注册后由 tracefs 借用。 */

/* init_annotated_branch_stats() 在 fs initcall 注册统计视图；失败警告并原样返回 errno。 */
__init static int init_annotated_branch_stats(void)
{
	int ret;

	ret = register_stat_tracer(&annotated_branch_stats);
	if (ret) {
		pr_warn("Warning: could not register annotated branches stats\n");
		return ret;
	}
	return 0;
}
fs_initcall(init_annotated_branch_stats);

#ifdef CONFIG_PROFILE_ALL_BRANCHES

extern unsigned long __start_branch_profile[];
extern unsigned long __stop_branch_profile[];

/* CONFIG_PROFILE_ALL_BRANCHES 的链接器边界覆盖所有 if 插桩描述符。 */

/* all_branch_stat_headers() 输出 miss/hit 统计列头，恒返回 0。 */
static int all_branch_stat_headers(struct seq_file *m)
{
	seq_puts(m, "   miss      hit    % "
		    "       Function                "
		    "  File              Line\n"
		    " ------- ---------  - "
		    "       --------                "
		    "  ----              ----\n");
	return 0;
}

/* all_branch_stat_start() 返回全分支 section 首地址，返回借用指针。 */
static void *all_branch_stat_start(struct tracer_stat *trace)
{
	return __start_branch_profile;
}

/* all_branch_stat_next() 按定长描述符推进，到结束边界返回 NULL。 */
static void *
all_branch_stat_next(void *v, int idx)
{
	struct ftrace_branch_data *p = v;

	++p;

	if ((void *)p >= (void *)__stop_branch_profile)
		return NULL;

	return p;
}

/* all_branch_stat_show() 把 @v 解释为 branch_data，提取 basename 并输出普通统计。 */
static int all_branch_stat_show(struct seq_file *m, void *v)
{
	struct ftrace_branch_data *p = v;
	const char *f;

	f = branch_stat_process_file(p);
	return branch_stat_show_normal(m, p, f);
}

static struct tracer_stat all_branch_stats = {
	.name = "branch_all",
	.stat_start = all_branch_stat_start,
	.stat_next = all_branch_stat_next,
	.stat_headers = all_branch_stat_headers,
	.stat_show = all_branch_stat_show
};

/* all_branch_stats 注册名为 branch_all 的 tracefs 统计视图；无需排序回调。 */

/* all_annotated_branch_stats() 在 fs initcall 注册全 if 统计，返回 0 或注册 errno。 */
__init static int all_annotated_branch_stats(void)
{
	int ret;

	ret = register_stat_tracer(&all_branch_stats);
	if (ret) {
		pr_warn("Warning: could not register all branches stats\n");
		return ret;
	}
	return 0;
}
fs_initcall(all_annotated_branch_stats);
#endif /* CONFIG_PROFILE_ALL_BRANCHES */
