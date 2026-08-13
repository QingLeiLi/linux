// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/lockdep_proc.c
 *
 * Runtime locking correctness validator
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2006,2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra
 *
 * Code for /proc/lockdep and /proc/lockdep_stats:
 *
 */
/*
 * 本文件把 lockdep 的运行期诊断状态发布到 procfs：`lockdep` 展示活动锁类及其直接依赖，
 * `lockdep_chains` 展示已缓存依赖链，`lockdep_stats` 汇总容量与 usage，`lock_stat` 则在启用
 * LOCK_STAT 时提供按竞争次数排序的每锁类快照和清零入口。它只消费 lockdep.c 维护的固定池，
 * 不参与依赖图判定，也不拥有 class、chain 或 trace 的生命周期。
 */
/* seq_file/procfs 提供分页读取和节点注册；kallsyms、排序、用户拷贝及 64 位除法支撑诊断格式化。 */
#include <linux/export.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kallsyms.h>
#include <linux/debug_locks.h>
#include <linux/vmalloc.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <asm/div64.h>

#include "lockdep_internals.h"

/*
 * Since iteration of lock_classes is done without holding the lockdep lock,
 * it is not safe to iterate all_lock_classes list directly as the iteration
 * may branch off to free_lock_classes or the zapped list. Iteration is done
 * directly on the lock_classes array by checking the lock_classes_in_use
 * bitmap and max_lock_class_idx.
 */
/*
 * lock_classes 的遍历没有持有 lockdep 图锁，不能直接沿 all_lock_classes：节点可能并发移到 free
 * 或 zapped 链并让遍历串入另一张表。固定数组本身不会释放，因此改为扫描 0..max_lock_class_idx，
 * 再由调用体用 lock_classes_in_use 过滤空洞。该协议只保证寻址安全，不提供一致时间点快照。
 */
/* idx 是数组下标，class 与其同步递增；上界是历史活动高水位，回收后可留下未占用槽。 */
#define iterate_lock_classes(idx, class)				\
	for (idx = 0, class = lock_classes; idx <= max_lock_class_idx;	\
	     idx++, class++)

/**
 * l_next - 推进 `/proc/lockdep` 的固定 lock-class 数组迭代器
 * @m: seq_file 输出上下文；本回调不读取
 * @v: 当前 lock_classes[] 槽的借用指针
 * @pos: seq_file 位置的输入输出指针，返回前更新为下一数组下标
 *
 * 返回：下一槽的借用指针；越过并发可见的 max_lock_class_idx 时返回 NULL 结束迭代。
 * 上下文与并发：不持 lockdep 图锁、不睡眠、不取得 class 引用；固定数组保证指针寻址有效，槽是否
 * 活动由 l_show() 的 bitmap 检查决定，因此本函数不会跳过中间空洞。
 */
static void *l_next(struct seq_file *m, void *v, loff_t *pos)
{
	/* class 从当前固定槽推进一项，所有权仍属于 lockdep 全局池。 */
	struct lock_class *class = v;

	/* seq_file 的逻辑位置直接采用数组下标，便于 seek 后由 l_start() 恢复。 */
	++class;
	*pos = class - lock_classes;
	/* 上界之外用 NULL 通知 seq_file 正常结束；上界内的 inactive 槽交给 show 过滤。 */
	return (*pos > max_lock_class_idx) ? NULL : class;
}

/**
 * l_start - 按 seq_file 位置定位 `/proc/lockdep` 的起始 class 槽
 * @m: seq_file 输出上下文；本回调不读取
 * @pos: 要恢复的零基数组位置，只读其当前值
 *
 * 返回：位置有效时为 lock_classes[*pos] 借用指针，超过 max_lock_class_idx 时为 NULL。
 * 边界：不验证 in-use 位，也不锁住上界；l_show() 负责识别空洞，所以并发注册/回收只形成诊断视图
 * 的时序差异，不允许调用者长期保存返回指针。
 */
static void *l_start(struct seq_file *m, loff_t *pos)
{
	/* 先复制位置，避免在范围判断和指针运算间重复读取 seq_file 游标。 */
	unsigned long idx = *pos;

	/* 超界表示本轮读取已经到达当前扫描上限。 */
	if (idx > max_lock_class_idx)
		return NULL;
	/* 固定池地址稳定；该槽是否正在使用要到 l_show() 再判断。 */
	return lock_classes + idx;
}

/**
 * l_stop - 结束 `/proc/lockdep` 的一次 seq_file 迭代
 * @m: seq_file 输出上下文；未使用
 * @v: 最后一个迭代元素或终止标记；未使用
 *
 * 返回与副作用：无。start/next 没有取得锁、引用或动态资源，因此 stop 无需清理且不会睡眠。
 */
static void l_stop(struct seq_file *m, void *v)
{
}

/**
 * print_name - 把 lock class 的可读名称追加到 seq_file
 * @m: 接收文本的 seq_file
 * @class: 当前固定池中的借用 class；调用者负责按其迭代协议确认可读
 *
 * 返回：无。显式 name 缺失时用 key 尝试符号化；显式名称后按需追加同名版本 `#N` 和 `/subclass`。
 * 所有权与并发：栈缓冲只在调用期间有效，函数不保存名称；无图锁读取可能观察到回收中的字段变化，
 * 输出仅作诊断快照，不能作为 class 存活引用。
 */
static void print_name(struct seq_file *m, struct lock_class *class)
{
	/* str 承接 kallsyms 结果；name 优先借用 class 的显式名称。 */
	char str[KSYM_NAME_LEN];
	const char *name = class->name;

	/* 匿名 class 以 key 地址符号化，使静态锁仍有可识别名称。 */
	if (!name) {
		name = __get_key_name(class->key, str);
		seq_printf(m, "%s", name);
	} else{
		/* 第一同名版本省略 #1；非基础 subclass 才追加 `/N`。 */
		seq_printf(m, "%s", name);
		if (class->name_version > 1)
			seq_printf(m, "#%d", class->name_version);
		if (class->subclass)
			seq_printf(m, "/%d", class->subclass);
	}
}

/**
 * l_show - 输出一个 `/proc/lockdep` class 槽及其直接后继依赖
 * @m: seq_file 输出上下文
 * @v: l_start()/l_next() 返回的 lock_classes[] 槽
 *
 * 返回：固定 0，让 seq_file 继续迭代；inactive 槽不输出正文也视为成功。
 * 阶段：首槽打印标题；bitmap 过滤空洞；输出 key、可选 ops/正反可达数、usage 和名称；启用依赖
 * 证明时再遍历 locks_after，只列 distance==1 的直接边。依赖计数 helper 自行用图锁稳定 BFS，
 * 本函数整体仍是并发变化中的诊断视图，不向读取者转移任何对象所有权。
 */
static int l_show(struct seq_file *m, void *v)
{
	/* class/entry 是全局池借用游标；usage 是调用者栈上的格式化缓冲；idx 用于 bitmap 身份检查。 */
	struct lock_class *class = v;
	struct lock_list *entry;
	char usage[LOCK_USAGE_CHARS];
	int idx = class - lock_classes;

	/* 即使第 0 槽为空，文件标题仍只在数组起点输出一次。 */
	if (v == lock_classes)
		seq_printf(m, "all lock classes:\n");

	/* 上界内可能有回收空洞；未置位槽的其余字段不能作为活动 class 解释。 */
	if (!test_bit(idx, lock_classes_in_use))
		return 0;

	/* key 是 class 的稳定诊断身份；DEBUG_LOCKDEP 额外显示该类被操作的近似次数。 */
	seq_printf(m, "%p", class->key);
#ifdef CONFIG_DEBUG_LOCKDEP
	seq_printf(m, " OPS:%8ld", debug_class_ops_read(class));
#endif
	/* 依赖证明启用时才执行 BFS 计数并把 usage_mask 编成紧凑字符列。 */
	if (IS_ENABLED(CONFIG_PROVE_LOCKING)) {
		seq_printf(m, " FD:%5ld", lockdep_count_forward_deps(class));
		seq_printf(m, " BD:%5ld", lockdep_count_backward_deps(class));

		get_usage_chars(class, usage);
		seq_printf(m, " %s", usage);
	}

	seq_printf(m, ": ");
	print_name(m, class);
	seq_puts(m, "\n");

	/* 只把距离为 1 的观测边列为直接依赖；传递路径已由 FD/BD 计数概括，不在此重复展开。 */
	if (IS_ENABLED(CONFIG_PROVE_LOCKING)) {
		list_for_each_entry(entry, &class->locks_after, entry) {
			if (entry->distance == 1) {
				/* 每条边打印目标 key 和与 class 主行一致的消歧名称。 */
				seq_printf(m, " -> [%p] ", entry->class->key);
				print_name(m, entry->class);
				seq_puts(m, "\n");
			}
		}
		seq_puts(m, "\n");
	}

	return 0;
}

/* `/proc/lockdep` 的 seq_file 回调表；四个函数共同使用固定 class 数组位置协议。 */
static const struct seq_operations lockdep_ops = {
	.start	= l_start,
	.next	= l_next,
	.stop	= l_stop,
	.show	= l_show,
};

#ifdef CONFIG_PROVE_LOCKING
/**
 * lc_start - 按 seq_file 位置启动 `/proc/lockdep_chains` 迭代
 * @m: seq_file 输出上下文；本回调不读取
 * @pos: 逻辑位置；0 表示文件标题，正数 N 表示 lock_chains[N-1]
 *
 * 返回：位置 0 为 SEQ_START_TOKEN，正位置为对应静态 chain 槽借用指针，负位置为 NULL。
 * 边界：正位置的有效性由 lc_next() 使用 in-use bitmap 枚举协议保证；函数本身不加图锁、不取得
 * 引用，也不检查数组上界，不能脱离 seq_operations 调用链单独传入任意位置。
 */
static void *lc_start(struct seq_file *m, loff_t *pos)
{
	/* -1 是 lc_next() 在没有后继 chain 时编码出的结束位置。 */
	if (*pos < 0)
		return NULL;

	/* 标题 token 与任何真实 lock_chain 地址不同，只由 lc_show() 解释。 */
	if (*pos == 0)
		return SEQ_START_TOKEN;

	/* seq 位置为数组下标加一，空出零给标题。 */
	return lock_chains + (*pos - 1);
}

/**
 * lc_next - 跳到下一个正在使用的 lock-chain 描述符
 * @m: seq_file 输出上下文；仅原样传给 lc_start()
 * @v: 当前标题 token 或 chain；本函数无需解引用
 * @pos: 当前逻辑位置的输入输出指针
 *
 * 返回：下一活动 chain、或没有后继时为 NULL。lockdep_next_lockchain() 接收真实数组下标并跳过
 * bitmap 空洞；其 -2 结束值加一成为 -1，再由 lc_start() 统一终止。
 */
static void *lc_next(struct seq_file *m, void *v, loff_t *pos)
{
	/* `*pos-1` 把标题位置 0 转为“从首项前 -1 开始”，也把 chain 位置还原为当前数组下标。 */
	*pos = lockdep_next_lockchain(*pos - 1) + 1;
	/* 复用 start 的 token/下标/结束解释，避免两处维护位置编码。 */
	return lc_start(m, pos);
}

/**
 * lc_stop - 结束一次 lock-chain seq_file 迭代
 * @m: seq_file 输出上下文；未使用
 * @v: 最后元素或终止值；未使用
 *
 * 返回与副作用：无。chain 枚举没有获取锁、RCU read lock 或动态资源，故无需配对释放。
 */
static void lc_stop(struct seq_file *m, void *v)
{
}

/**
 * lc_show - 输出标题或一条缓存 lock chain 的 class 序列
 * @m: seq_file 输出上下文
 * @v: SEQ_START_TOKEN 或 lc_start()/lc_next() 返回的 lock_chain 槽
 *
 * 返回：固定 0。标题阶段在 chain_hlocks 无剩余空间时附加 `(buggered)`；数据阶段先打印 chain 的
 * IRQ context，再按保存顺序解析每个压缩 hlock，只输出 key 仍有效的 class，最后用空行分隔。
 * 并发与生命周期：chain/class 均来自静态池，函数不持图锁且只形成诊断视图；zapped class 的 key
 * 已清零时跳过，不能把本次输出当作对整条 chain 的一致快照或对象引用。
 */
static int lc_show(struct seq_file *m, void *v)
{
	/* chain 在 token 分支不会解引用；class/i 遍历压缩 chain 内容。 */
	struct lock_chain *chain = v;
	struct lock_class *class;
	int i;
	/* irq_context 是 hardirq/softirq 两个标志位的组合，四个数组项覆盖完整值域。 */
	static const char * const irq_strs[] = {
		[0]			     = "0",
		[LOCK_CHAIN_HARDIRQ_CONTEXT] = "hardirq",
		[LOCK_CHAIN_SOFTIRQ_CONTEXT] = "softirq",
		[LOCK_CHAIN_SOFTIRQ_CONTEXT|
		 LOCK_CHAIN_HARDIRQ_CONTEXT] = "hardirq|softirq",
	};

	/* token 只负责文件级标题，不代表实际 chain。 */
	if (v == SEQ_START_TOKEN) {
		/* 零空闲 hlock 槽提示缓存存储已经耗尽或不可继续扩展。 */
		if (!nr_free_chain_hlocks)
			seq_printf(m, "(buggered) ");
		seq_printf(m, "all lock chains:\n");
		return 0;
	}

	/* chain 创建时保存所属 IRQ context；它决定这条持锁序列的执行类别。 */
	seq_printf(m, "irq_context: %s\n", irq_strs[chain->irq_context]);

	/* base/depth 切出 chain_hlocks 的有效区间，helper 把每项 class_idx 还原为静态 class 指针。 */
	for (i = 0; i < chain->depth; i++) {
		class = lock_chain_get_class(chain, i);
		/* key 清零表示 class 已逻辑注销，避免输出已经失效的名称身份。 */
		if (!class->key)
			continue;

		seq_printf(m, "[%p] ", class->key);
		print_name(m, class);
		seq_puts(m, "\n");
	}
	seq_puts(m, "\n");

	return 0;
}

/* `/proc/lockdep_chains` 的 seq_file 回调表；位置 0 为标题，其余位置映射活动 chain 下标。 */
static const struct seq_operations lockdep_chains_ops = {
	.start	= lc_start,
	.next	= lc_next,
	.stop	= lc_stop,
	.show	= lc_show,
};
#endif /* CONFIG_PROVE_LOCKING */

/**
 * lockdep_stats_debug_show - 追加仅 DEBUG_LOCKDEP 构建可用的内部操作计数
 * @m: 接收统计文本的 seq_file
 *
 * 返回：无。启用 DEBUG_LOCKDEP 时读取 chain lookup、BFS/usage 检查和 IRQ 开关事件计数并逐行输出；
 * 关闭该配置时函数体为空。读取使用调试原子 helper，但多项之间没有共同快照时点，仅供趋势诊断。
 */
static void lockdep_stats_debug_show(struct seq_file *m)
{
#ifdef CONFIG_DEBUG_LOCKDEP
	/* hi/hr 是 hardirq 有效/冗余 on/off，si/sr 是对应 softirq 计数；局部快照避免同一行重复读取。 */
	unsigned long long hi1 = debug_atomic_read(hardirqs_on_events),
			   hi2 = debug_atomic_read(hardirqs_off_events),
			   hr1 = debug_atomic_read(redundant_hardirqs_on),
			   hr2 = debug_atomic_read(redundant_hardirqs_off),
			   si1 = debug_atomic_read(softirqs_on_events),
			   si2 = debug_atomic_read(softirqs_off_events),
			   sr1 = debug_atomic_read(redundant_softirqs_on),
			   sr2 = debug_atomic_read(redundant_softirqs_off);

	/* 第一组描述 chain cache 效果、环路/冗余证明和 usage 子图搜索工作量。 */
	seq_printf(m, " chain lookup misses:           %11llu\n",
		debug_atomic_read(chain_lookup_misses));
	seq_printf(m, " chain lookup hits:             %11llu\n",
		debug_atomic_read(chain_lookup_hits));
	seq_printf(m, " cyclic checks:                 %11llu\n",
		debug_atomic_read(nr_cyclic_checks));
	seq_printf(m, " redundant checks:              %11llu\n",
		debug_atomic_read(nr_redundant_checks));
	seq_printf(m, " redundant links:               %11llu\n",
		debug_atomic_read(nr_redundant));
	seq_printf(m, " find-mask forwards checks:     %11llu\n",
		debug_atomic_read(nr_find_usage_forwards_checks));
	seq_printf(m, " find-mask backwards checks:    %11llu\n",
		debug_atomic_read(nr_find_usage_backwards_checks));

	/* 第二组对比真实 IRQ 状态转换与重复通知，帮助定位 annotation 或调用次序异常。 */
	seq_printf(m, " hardirq on events:             %11llu\n", hi1);
	seq_printf(m, " hardirq off events:            %11llu\n", hi2);
	seq_printf(m, " redundant hardirq ons:         %11llu\n", hr1);
	seq_printf(m, " redundant hardirq offs:        %11llu\n", hr2);
	seq_printf(m, " softirq on events:             %11llu\n", si1);
	seq_printf(m, " softirq off events:            %11llu\n", si2);
	seq_printf(m, " redundant softirq ons:         %11llu\n", sr1);
	seq_printf(m, " redundant softirq offs:        %11llu\n", sr2);
#endif
}

/**
 * lockdep_stats_show - 汇总并输出 `/proc/lockdep_stats` 的全局诊断视图
 * @m: 接收文本的 seq_file
 * @v: single_open 风格回调的私有参数；本函数不使用
 *
 * 返回：固定 0。启用 PROVE_LOCKING 时先扫描活动 class，按 usage 位分类并累计正向可达数；随后输出
 * 固定池占用、chain/trace、IRQ 类别、深度峰值和回收复用统计，最后追加 DEBUG_LOCKDEP 专属计数。
 * 并发边界：不持全程图锁，各字段和各 class 可能来自不同瞬间；内部 BFS helper 会分别持图锁，
 * 因而结果适合容量与趋势排障，不是事务一致快照，也不授予任何 class/chain 生命周期引用。
 */
static int lockdep_stats_show(struct seq_file *m, void *v)
{
	/*
	 * 前两项区分从未设置 usage 与仅有一般 USED 的 class；其余变量分别统计 IRQ 聚合、softirq、
	 * hardirq 的 safe/unsafe 写模式和 read 模式。sum_forward_deps 累加每个活动 class 的正向 BFS 值。
	 */
	unsigned long nr_unused = 0, nr_uncategorized = 0,
		      nr_irq_safe = 0, nr_irq_unsafe = 0,
		      nr_softirq_safe = 0, nr_softirq_unsafe = 0,
		      nr_hardirq_safe = 0, nr_hardirq_unsafe = 0,
		      nr_irq_read_safe = 0, nr_irq_read_unsafe = 0,
		      nr_softirq_read_safe = 0, nr_softirq_read_unsafe = 0,
		      nr_hardirq_read_safe = 0, nr_hardirq_read_unsafe = 0,
		      sum_forward_deps = 0;

#ifdef CONFIG_PROVE_LOCKING
	/* class/idx 按固定数组协议扫描；bitmap 过滤 zapped/free 空洞。 */
	struct lock_class *class;
	unsigned long idx;

	iterate_lock_classes(idx, class) {
		/* 只有当前标为 in-use 的槽才具备活动 usage 和依赖语义。 */
		if (!test_bit(idx, lock_classes_in_use))
			continue;

		/* usage_mask==0 是完全未使用；恰为 LOCKF_USED 表示使用过但没有 IRQ 分类。 */
		if (class->usage_mask == 0)
			nr_unused++;
		if (class->usage_mask == LOCKF_USED)
			nr_uncategorized++;
		/* 聚合 IRQ 与 hard/softirq 分项的非 READ safe/unsafe 历史；同一 class 可进入多个计数。 */
		if (class->usage_mask & LOCKF_USED_IN_IRQ)
			nr_irq_safe++;
		if (class->usage_mask & LOCKF_ENABLED_IRQ)
			nr_irq_unsafe++;
		if (class->usage_mask & LOCKF_USED_IN_SOFTIRQ)
			nr_softirq_safe++;
		if (class->usage_mask & LOCKF_ENABLED_SOFTIRQ)
			nr_softirq_unsafe++;
		if (class->usage_mask & LOCKF_USED_IN_HARDIRQ)
			nr_hardirq_safe++;
		if (class->usage_mask & LOCKF_ENABLED_HARDIRQ)
			nr_hardirq_unsafe++;
		/* READ usage 使用独立位，不与上面的写模式计数互相排斥。 */
		if (class->usage_mask & LOCKF_USED_IN_IRQ_READ)
			nr_irq_read_safe++;
		if (class->usage_mask & LOCKF_ENABLED_IRQ_READ)
			nr_irq_read_unsafe++;
		if (class->usage_mask & LOCKF_USED_IN_SOFTIRQ_READ)
			nr_softirq_read_safe++;
		if (class->usage_mask & LOCKF_ENABLED_SOFTIRQ_READ)
			nr_softirq_read_unsafe++;
		if (class->usage_mask & LOCKF_USED_IN_HARDIRQ_READ)
			nr_hardirq_read_safe++;
		if (class->usage_mask & LOCKF_ENABLED_HARDIRQ_READ)
			nr_hardirq_read_unsafe++;

		/* helper 的当前定义包含搜索根自身，故该和不是“纯间接边去重数”，只沿用历史输出口径。 */
		sum_forward_deps += lockdep_count_forward_deps(class);
	}

#ifdef CONFIG_DEBUG_LOCKDEP
	/* 调试构建用独立原子统计反查本次全池扫描，发现 unused 账本漂移即告警。 */
	DEBUG_LOCKS_WARN_ON(debug_atomic_read(nr_unused_locks) != nr_unused);
#endif

#endif
	/* 第一组显示 class/key/直接边池当前占用；无 PROVE_LOCKING 时相关累计值保持初始化的零。 */
	seq_printf(m, " lock-classes:                  %11lu [max: %lu]\n",
			nr_lock_classes, MAX_LOCKDEP_KEYS);
	seq_printf(m, " dynamic-keys:                  %11lu\n",
			nr_dynamic_keys);
	seq_printf(m, " direct dependencies:           %11lu [max: %lu]\n",
			nr_list_entries, MAX_LOCKDEP_ENTRIES);
	seq_printf(m, " indirect dependencies:         %11lu\n",
			sum_forward_deps);

	/*
	 * Total number of dependencies:
	 *
	 * All irq-safe locks may nest inside irq-unsafe locks,
	 * plus all the other known dependencies:
	 */
	/*
	 * 依赖总数估算：所有 irq-safe 锁理论上都可嵌套在 irq-unsafe 锁内，再加已知直接边。当前公式
	 * 同时加入 IRQ 聚合乘积与 hardirq 分项乘积，因此是历史容量估算口径，不是去重后的实际边数。
	 */
	seq_printf(m, " all direct dependencies:       %11lu\n",
			nr_irq_unsafe * nr_irq_safe +
			nr_hardirq_unsafe * nr_hardirq_safe +
			nr_list_entries);

#ifdef CONFIG_PROVE_LOCKING
	/* chain 描述符、压缩 hlock 存储使用量和永久单项碎片分别展示。 */
	seq_printf(m, " dependency chains:             %11lu [max: %lu]\n",
			lock_chain_count(), MAX_LOCKDEP_CHAINS);
	seq_printf(m, " dependency chain hlocks used:  %11lu [max: %lu]\n",
			MAX_LOCKDEP_CHAIN_HLOCKS -
			(nr_free_chain_hlocks + nr_lost_chain_hlocks),
			MAX_LOCKDEP_CHAIN_HLOCKS);
	seq_printf(m, " dependency chain hlocks lost:  %11u\n",
			nr_lost_chain_hlocks);
#endif

#ifdef CONFIG_TRACE_IRQFLAGS
	/* IRQFLAGS 跟踪启用时可区分 hardirq/softirq chain；process 计数在所有配置都存在。 */
	seq_printf(m, " in-hardirq chains:             %11u\n",
			nr_hardirq_chains);
	seq_printf(m, " in-softirq chains:             %11u\n",
			nr_softirq_chains);
#endif
	seq_printf(m, " in-process chains:             %11u\n",
			nr_process_chains);
	seq_printf(m, " stack-trace entries:           %11lu [max: %lu]\n",
			nr_stack_trace_entries, MAX_STACK_TRACE_ENTRIES);
#if defined(CONFIG_TRACE_IRQFLAGS) && defined(CONFIG_PROVE_LOCKING)
	/* 两项配置同时存在时，附加 trace 对象数和其 hash 链数。 */
	seq_printf(m, " number of stack traces:        %11llu\n",
		   lockdep_stack_trace_count());
	seq_printf(m, " number of stack hash chains:   %11llu\n",
		   lockdep_stack_hash_count());
#endif
	/* 三类 chain 数各加一后相乘，给出上下文组合空间的诊断上界。 */
	seq_printf(m, " combined max dependencies:     %11u\n",
			(nr_hardirq_chains + 1) *
			(nr_softirq_chains + 1) *
			(nr_process_chains + 1)
	);
	seq_printf(m, " hardirq-safe locks:            %11lu\n",
			nr_hardirq_safe);
	seq_printf(m, " hardirq-unsafe locks:          %11lu\n",
			nr_hardirq_unsafe);
	seq_printf(m, " softirq-safe locks:            %11lu\n",
			nr_softirq_safe);
	seq_printf(m, " softirq-unsafe locks:          %11lu\n",
			nr_softirq_unsafe);
	seq_printf(m, " irq-safe locks:                %11lu\n",
			nr_irq_safe);
	seq_printf(m, " irq-unsafe locks:              %11lu\n",
			nr_irq_unsafe);

	/* READ safe/unsafe 使用独立六行，避免与独占/写 usage 混为一个类别。 */
	seq_printf(m, " hardirq-read-safe locks:       %11lu\n",
			nr_hardirq_read_safe);
	seq_printf(m, " hardirq-read-unsafe locks:     %11lu\n",
			nr_hardirq_read_unsafe);
	seq_printf(m, " softirq-read-safe locks:       %11lu\n",
			nr_softirq_read_safe);
	seq_printf(m, " softirq-read-unsafe locks:     %11lu\n",
			nr_softirq_read_unsafe);
	seq_printf(m, " irq-read-safe locks:           %11lu\n",
			nr_irq_read_safe);
	seq_printf(m, " irq-read-unsafe locks:         %11lu\n",
			nr_irq_read_unsafe);

	seq_printf(m, " uncategorized locks:           %11lu\n",
			nr_uncategorized);
	seq_printf(m, " unused locks:                  %11lu\n",
			nr_unused);
	/* 深度与最大 class 下标都是运行期历史峰值/扫描边界，不等于当前数量。 */
	seq_printf(m, " max locking depth:             %11u\n",
			max_lockdep_depth);
#ifdef CONFIG_PROVE_LOCKING
	seq_printf(m, " max bfs queue depth:           %11u\n",
			max_bfs_queue_depth);
#endif
	seq_printf(m, " max lock class index:          %11lu\n",
			max_lock_class_idx);
	/* DEBUG_LOCKDEP 开关关闭时该 helper 不追加任何行。 */
	lockdep_stats_debug_show(m);
	seq_printf(m, " debug_locks:                   %11u\n",
			debug_locks);

	/*
	 * Zapped classes and lockdep data buffers reuse statistics.
	 */
	/* 已注销 class 与 lockdep 数据缓冲复用统计，用于观察模块卸载和静态池碎片。 */
	seq_puts(m, "\n");
	seq_printf(m, " zapped classes:                %11lu\n",
			nr_zapped_classes);
#ifdef CONFIG_PROVE_LOCKING
	/* zapped chain 累计回收量；large blocks 反映 chain_hlocks 可变尺寸空闲块碎片。 */
	seq_printf(m, " zapped lock chains:            %11lu\n",
			nr_zapped_lock_chains);
	seq_printf(m, " large chain blocks:            %11u\n",
			nr_large_chain_blocks);
#endif
	/* single-show 回调以零表示输出成功。 */
	return 0;
}

#ifdef CONFIG_LOCK_STAT

/* 一条可排序的 lockstat 快照记录：class 只借用静态池地址，stats 是 open 时复制的数值快照。 */
struct lock_stat_data {
	/* 用于名称、调用点和版本信息；不增加 class 引用。 */
	struct lock_class *class;
	/* 读取各 CPU 统计后汇总出的独立副本，后续排序和输出不修改生产侧。 */
	struct lock_class_stats stats;
};

/* 每次打开 `/proc/lock_stat` 独占的 vmalloc 缓冲，生命周期从 lock_stat_open 到 release。 */
struct lock_stat_seq {
	/* 指向 stats[] 首个无效尾后元素，限定本次快照和排序范围。 */
	struct lock_stat_data *iter_end;
	/* 最多为每个 class 槽保存一项；inactive 槽不会写入，有效项紧密排列在前缀。 */
	struct lock_stat_data stats[MAX_LOCKDEP_KEYS];
};

/*
 * sort on absolute number of contentions
 */
/* 按读写竞争次数的绝对总数排序。 */
/**
 * lock_stat_cmp - 比较两条 lockstat 快照的总竞争次数
 * @l: 左侧 lock_stat_data，只读借用
 * @r: 右侧 lock_stat_data，只读借用
 *
 * 返回：右侧读写 wait 事件总数减左侧总数，供 sort() 形成竞争次数降序；相等返回 0。
 * 边界：沿用现有 unsigned long 差值再收窄为 int 的实现，极端计数差可能截断；函数不修改记录、
 * 不睡眠，也不需要访问仍在变化的 per-CPU 统计，因为输入已经是 open 时快照。
 */
static int lock_stat_cmp(const void *l, const void *r)
{
	/* dl/dr 只在比较回调期间借用；nl/nr 是各自 read+write contention 样本数。 */
	const struct lock_stat_data *dl = l, *dr = r;
	unsigned long nl, nr;

	/* waittime.nr 在每次发生竞争等待时增加，因此两种模式之和就是排序键。 */
	nl = dl->stats.read_waittime.nr + dl->stats.write_waittime.nr;
	nr = dr->stats.read_waittime.nr + dr->stats.write_waittime.nr;

	/* sort 要求负值表示 l 排在 r 前；nr-nl 因而把更大的 nl 提到前面。 */
	return nr - nl;
}

/**
 * seq_line - 向 seq_file 输出一条带左缩进的重复字符分隔线
 * @m: 目标 seq_file
 * @c: 分隔线字符
 * @offset: 前导空格个数，调用者传非负值
 * @length: 字符 @c 的重复次数，调用者传非负值
 *
 * 返回：无；依次输出 offset 个空格、length 个字符和一个换行，不分配或保存缓冲。
 */
static void seq_line(struct seq_file *m, char c, int offset, int length)
{
	/* i 在两个独立阶段分别计数缩进和分隔线宽度。 */
	int i;

	/* 先建立与 class 名称列一致的左侧对齐。 */
	for (i = 0; i < offset; i++)
		seq_puts(m, " ");
	/* 再逐字符写分隔线；seq_file 负责分页缓冲扩展/重放。 */
	for (i = 0; i < length; i++)
		seq_putc(m, c);
	seq_puts(m, "\n");
}

/**
 * snprint_time - 把纳秒 lockstat 时长格式化为带两位小数的微秒文本
 * @buf: 调用者提供的输出缓冲
 * @bufsiz: @buf 的字节容量，包含结尾 NUL
 * @nr: 纳秒时长，正常统计为非负 s64
 *
 * 返回：无；输出 `微秒.百分之一微秒`。先加 5ns 完成 10ns 精度的四舍五入，再以 1000 分解微秒
 * 商和纳秒余数；snprintf() 负责按 bufsiz 截断并终止字符串。
 */
static void snprint_time(char *buf, size_t bufsiz, s64 nr)
{
	/* div 保存整微秒，rem 保存不足 1 微秒的纳秒余数。 */
	s64 div;
	s32 rem;

	/* 显示精度为 10ns，预加半个单位后再舍去 rem 的个位。 */
	nr += 5; /* for display rounding */
	/* div_s64_rem() 同时返回商并写余数，不分配资源。 */
	div = div_s64_rem(nr, 1000, &rem);
	snprintf(buf, bufsiz, "%lld.%02d", (long long)div, (int)rem/10);
}

/**
 * seq_time - 以固定列宽向 seq_file 输出一个 lockstat 时长
 * @m: 目标 seq_file
 * @time: 纳秒时长
 *
 * 返回：无；先在 22 字节栈缓冲中格式化为微秒小数，再以一个前导空格和 14 字符右对齐输出。
 */
static void seq_time(struct seq_file *m, s64 time)
{
	/* num 足以容纳 s64 微秒十进制、小数点、两位小数和 NUL。 */
	char num[22];

	/* 格式转换和列对齐分离，保证表头与所有时间列共享宽度。 */
	snprint_time(num, sizeof(num), time);
	seq_printf(m, " %14s", num);
}

/**
 * seq_lock_time - 输出一组 lock_time 的样本数、最小、最大、总计与平均时长
 * @m: 目标 seq_file
 * @lt: open 快照中的 lock_time，只读借用
 *
 * 返回：无。nr 为零时平均值显式取 0，避免除零；其余时间以纳秒传给 seq_time() 转为显示单位。
 */
static void seq_lock_time(struct seq_file *m, struct lock_time *lt)
{
	/* 样本数本身不带时间单位，使用与其他数值列相同的 14 字符宽度。 */
	seq_printf(m, "%14lu", lt->nr);
	/* min/max/total 原样输出；平均值用无符号 64 位除法，零样本走显式零分支。 */
	seq_time(m, lt->min);
	seq_time(m, lt->max);
	seq_time(m, lt->total);
	seq_time(m, lt->nr ? div64_u64(lt->total, lt->nr) : 0);
}

/**
 * seq_stats - 输出一个 class 的 lockstat 读写时延、bounce 和热点调用点
 * @m: 目标 seq_file
 * @data: lock_stat_open() 建立的一条快照；stats 数值归该快照所有，class 仅借用静态池地址
 *
 * 返回：无。先在 sched-RCU 下复制 class 名称或 key 符号名；class 已同时失去 name/key 时跳过。
 * 随后分别输出有样本的写、读统计行；没有任何 wait 样本时结束，否则追加 contention/contending
 * 调用点和计数。函数不释放 data；名称栈缓冲在解 RCU 后仍自包含，但其他 class 字段仍是并发诊断
 * 读取，可能与 open 时统计快照不处于同一时刻。
 */
static void seq_stats(struct seq_file *m, struct lock_stat_data *data)
{
	/* ckey/cname 只在 sched-RCU 临界区借用；stats 指向私有快照，class 指向全局静态槽。 */
	const struct lockdep_subclass_key *ckey;
	struct lock_class_stats *stats;
	struct lock_class *class;
	const char *cname;
	/* i 遍历热点数组，namelen 同时控制名称截断和分隔线宽度。 */
	int i, namelen;
	/* 38 个可见字符加 NUL；后续最多再预留两个两字符 suffix。 */
	char name[39];

	/* 数值统计已在 open 阶段复制，名称与调用点仍从 class 静态槽读取。 */
	class = data->class;
	stats = &data->stats;

	/* 基础名称最多占 38 字符；为可能追加的 `#N` 和 `/N` 各预留两个字符。 */
	namelen = 38;
	if (class->name_version > 1)
		namelen -= 2; /* XXX truncates versions > 9 */
	/* 原提示：这里只预留并写入两个字符，因此 name_version 大于 9 时显示会被截断。 */
	if (class->subclass)
		namelen -= 2;

	/* zap_class() 以 RCU 方式清 name/key；保护期覆盖检查和把文本复制进本地 name 的全过程。 */
	rcu_read_lock_sched();
	cname = rcu_dereference_sched(class->name);
	ckey  = rcu_dereference_sched(class->key);

	/* 两者同时为空表示 class 已逻辑注销，本次 open 快照不再输出该槽。 */
	if (!cname && !ckey) {
		rcu_read_unlock_sched();
		return;

	} else if (!cname) {
		/* 匿名 class 在保护期内把 key 的符号名复制到本地缓冲。 */
		char str[KSYM_NAME_LEN];
		const char *key_name;

		key_name = __get_key_name(ckey, str);
		snprintf(name, namelen, "%s", key_name);
	} else {
		/* 显式名称也必须在保护期内完成截断复制，解锁后不再借用原字符串。 */
		snprintf(name, namelen, "%s", cname);
	}
	rcu_read_unlock_sched();

	/* name 已自包含；按 class 的消歧元数据追加版本号和 subclass。 */
	namelen = strlen(name);
	if (class->name_version > 1) {
		snprintf(name+namelen, 3, "#%d", class->name_version);
		namelen += 2;
	}
	if (class->subclass) {
		snprintf(name+namelen, 3, "/%d", class->subclass);
		namelen += 2;
	}

	/* 有写持有样本才输出写行；若同时有读样本，用 -W 与后面的 -R 区分。 */
	if (stats->write_holdtime.nr) {
		if (stats->read_holdtime.nr)
			seq_printf(m, "%38s-W:", name);
		else
			seq_printf(m, "%40s:", name);

		/* 顺序是竞争迁移数、等待时间组、取得迁移数和持有时间组。 */
		seq_printf(m, "%14lu ", stats->bounces[bounce_contended_write]);
		seq_lock_time(m, &stats->write_waittime);
		seq_printf(m, " %14lu ", stats->bounces[bounce_acquired_write]);
		seq_lock_time(m, &stats->write_holdtime);
		seq_puts(m, "\n");
	}

	/* 读统计始终追加 -R，并使用同一 open 时数值快照。 */
	if (stats->read_holdtime.nr) {
		seq_printf(m, "%38s-R:", name);
		seq_printf(m, "%14lu ", stats->bounces[bounce_contended_read]);
		seq_lock_time(m, &stats->read_waittime);
		seq_printf(m, " %14lu ", stats->bounces[bounce_acquired_read]);
		seq_lock_time(m, &stats->read_holdtime);
		seq_puts(m, "\n");
	}

	/* 没有任何读写等待事件就不存在有意义的竞争热点。 */
	if (stats->read_waittime.nr + stats->write_waittime.nr == 0)
		return;

	/* 后续名称下划线要覆盖用户实际看到的 `-R` suffix。 */
	if (stats->read_holdtime.nr)
		namelen += 2;

	/* 第一组为观察到 contention 的调用位置；零地址是紧密数组的结束哨兵。 */
	for (i = 0; i < LOCKSTAT_POINTS; i++) {
		/* ip 保存当前地址的 `[<...>]` 文本，符号名由 `%pS` 同时解析。 */
		char ip[32];

		if (class->contention_point[i] == 0)
			break;

		/* 首个热点前只画一次与 class 名称等宽的短横线。 */
		if (!i)
			seq_line(m, '-', 40-namelen, namelen);

		snprintf(ip, sizeof(ip), "[<%p>]",
				(void *)class->contention_point[i]);
		seq_printf(m, "%40s %14lu %29s %pS\n",
			   name, stats->contention_point[i],
			   ip, (void *)class->contention_point[i]);
	}
	/* 第二组为 class 记录的 contending 调用位置，布局和终止规则相同。 */
	for (i = 0; i < LOCKSTAT_POINTS; i++) {
		char ip[32];

		if (class->contending_point[i] == 0)
			break;

		if (!i)
			seq_line(m, '-', 40-namelen, namelen);

		snprintf(ip, sizeof(ip), "[<%p>]",
				(void *)class->contending_point[i]);
		seq_printf(m, "%40s %14lu %29s %pS\n",
			   name, stats->contending_point[i],
			   ip, (void *)class->contending_point[i]);
	}
	/* 当前实现仅在第二个循环至少输出一项时追加整表宽度的点线分隔。 */
	if (i) {
		seq_puts(m, "\n");
		seq_line(m, '.', 0, 40 + 1 + 12 * (14 + 1));
		seq_puts(m, "\n");
	}
}

/**
 * seq_header - 输出 `/proc/lock_stat` 的版本、状态警告和固定列标题
 * @m: 目标 seq_file
 *
 * 返回：无。即使 debug_locks 已关闭也继续显示已收集快照，但先给出醒目警告；随后输出总宽度分隔
 * 线、class 名称列和 12 个数值列标题。只写 seq_file，不修改 lockstat 状态。
 */
static void seq_header(struct seq_file *m)
{
	/* 格式版本让用户空间解析器识别当前列布局。 */
	seq_puts(m, "lock_stat version 0.4\n");

	/* 验证器已因告警关闭时，后续数据可能不完整或停止更新，但仍保留诊断价值。 */
	if (unlikely(!debug_locks))
		seq_printf(m, "*WARNING* lock debugging disabled!! - possibly due to a lockdep warning\n");

	/* 40 字符名称列、一个分隔空格及 12 组“14 字符列+1 空格”共同决定表宽。 */
	seq_line(m, '-', 0, 40 + 1 + 12 * (14 + 1));
	seq_printf(m, "%40s %14s %14s %14s %14s %14s %14s %14s %14s %14s %14s "
			"%14s %14s\n",
			"class name",
			"con-bounces",
			"contentions",
			"waittime-min",
			"waittime-max",
			"waittime-total",
			"waittime-avg",
			"acq-bounces",
			"acquisitions",
			"holdtime-min",
			"holdtime-max",
			"holdtime-total",
			"holdtime-avg");
	seq_line(m, '-', 0, 40 + 1 + 12 * (14 + 1));
	seq_printf(m, "\n");
}

/**
 * ls_start - 按位置启动一个 open 私有 lockstat 快照的 seq_file 迭代
 * @m: seq_file；private 必须指向 lock_stat_open() 成功建立的 lock_stat_seq
 * @pos: 逻辑位置，0 为表头，正数 N 对应 stats[N-1]
 *
 * 返回：位置 0 为 SEQ_START_TOKEN；有效快照项为借用指针；到达 iter_end 时为 NULL。
 * 生命周期：data 由本次 file open 独占并持续到 lock_stat_release()，迭代不另取引用或锁。
 */
static void *ls_start(struct seq_file *m, loff_t *pos)
{
	/* data 是 file 私有快照，iter 只在其紧密有效前缀内移动。 */
	struct lock_stat_seq *data = m->private;
	struct lock_stat_data *iter;

	/* 位置 0 留给列标题，不消耗 stats[] 项。 */
	if (*pos == 0)
		return SEQ_START_TOKEN;

	/* 其余位置减一映射数组；尾后及更远位置统一结束读取。 */
	iter = data->stats + (*pos - 1);
	if (iter >= data->iter_end)
		iter = NULL;

	return iter;
}

/**
 * ls_next - 推进 lockstat 私有快照迭代位置
 * @m: 持有 lock_stat_seq 私有数据的 seq_file
 * @v: 当前 token 或快照项；无需解引用
 * @pos: 位置输入输出指针，返回前加一
 *
 * 返回：下一快照项，或越过 iter_end 时为 NULL；位置解释统一复用 ls_start()。
 */
static void *ls_next(struct seq_file *m, void *v, loff_t *pos)
{
	/* 标题后的第一次推进把 0 变成 1，从而选择 stats[0]。 */
	(*pos)++;
	return ls_start(m, pos);
}

/**
 * ls_stop - 结束一次 lockstat seq_file 迭代
 * @m: seq_file；未使用
 * @v: 最后元素或终止值；未使用
 *
 * 返回与副作用：无。私有 vmalloc 快照属于整个 open file，不在分页迭代停止时释放，而由
 * lock_stat_release() 与 file 生命周期配对。
 */
static void ls_stop(struct seq_file *m, void *v)
{
}

/**
 * ls_show - 输出 lockstat 表头 token 或一条 class 统计快照
 * @m: 目标 seq_file
 * @v: SEQ_START_TOKEN 或 lock_stat_data 借用指针
 *
 * 返回：固定 0；token 分派到 seq_header()，数据项分派到 seq_stats()。不改变迭代位置或私有快照。
 */
static int ls_show(struct seq_file *m, void *v)
{
	/* 标题与数据共享一个 show 回调，以位置 0 的唯一 token 区分。 */
	if (v == SEQ_START_TOKEN)
		seq_header(m);
	else
		seq_stats(m, v);

	return 0;
}

/* `/proc/lock_stat` 的快照 seq_file 回调表；private 缓冲由自定义 open/release 管理。 */
static const struct seq_operations lockstat_ops = {
	.start	= ls_start,
	.next	= ls_next,
	.stop	= ls_stop,
	.show	= ls_show,
};

/**
 * lock_stat_open - 为一次 `/proc/lock_stat` 打开建立、排序私有统计快照
 * @inode: proc inode；本实现不读取，交由后续 seq_release() 生命周期配对
 * @file: 新打开的 file；seq_open() 成功后其 private_data 指向 seq_file
 *
 * 返回：成功为 0；vmalloc 失败为 -ENOMEM；seq_open() 失败原样返回其负 errno。
 * 阶段与所有权：先 vmalloc 最大快照缓冲，再创建 seq_file；成功后扫描活动 class、复制聚合统计、
 * 记录 iter_end、按竞争数降序原地排序，并把缓冲转交给 m->private；seq_open 失败则立即 vfree。
 * 本函数在可睡眠的 open 上下文运行；扫描不持全程图锁，故各 class 数值是近似快照。
 */
static int lock_stat_open(struct inode *inode, struct file *file)
{
	/* res 传播 seq_open 结果；class 是固定池游标；data 在成功后归本 file，失败前归本函数。 */
	int res;
	struct lock_class *class;
	struct lock_stat_seq *data = vmalloc(sizeof(struct lock_stat_seq));

	/* 大数组使用 vmalloc 避免高阶连续物理页要求；失败时尚无其他资源。 */
	if (!data)
		return -ENOMEM;

	/* seq_open 成功会创建 seq_file 并安装到 file->private_data。 */
	res = seq_open(file, &lockstat_ops);
	if (!res) {
		/* iter 写入紧密有效前缀；m 是刚由 seq_open 建立的 file 私有对象；idx 扫描 class 池。 */
		struct lock_stat_data *iter = data->stats;
		struct seq_file *m = file->private_data;
		unsigned long idx;

		/* bitmap 过滤未活动槽；每个有效 class 保存借用地址并汇总各 CPU 统计到独立副本。 */
		iterate_lock_classes(idx, class) {
			if (!test_bit(idx, lock_classes_in_use))
				continue;
			iter->class = class;
			lock_stats(class, &iter->stats);
			iter++;
		}

		/* 尾后指针冻结本次 open 的有效项数，后续 seq 迭代不再扫描全局 bitmap。 */
		data->iter_end = iter;

		/* 只排序有效前缀；NULL swap 让通用 sort() 使用默认交换实现。 */
		sort(data->stats, data->iter_end - data->stats,
				sizeof(struct lock_stat_data),
				lock_stat_cmp, NULL);

		/* 从此由 lock_stat_release() 经 seq_file 私有指针回收 data。 */
		m->private = data;
	} else
		/* seq_file 未建立，调用者不会进入自定义 release，必须在本错误路径释放缓冲。 */
		vfree(data);

	return res;
}

/**
 * lock_stat_write - 处理 `/proc/lock_stat` 的全局统计清零命令
 * @file: 已打开 proc file；当前实现不读取其私有快照
 * @buf: 用户缓冲，只检查首字节
 * @count: 用户提供的字节数；零表示无操作
 * @ppos: 文件位置；本实现不读取或更新
 *
 * 返回：首字节读取失败为 -EFAULT；其余情况返回 count。只有首字符为 `0` 时扫描活动 class 并调用
 * clear_lock_stats()，其他内容被接受为无操作。清零不与并发统计更新形成全局快照，也不更新已经
 * 打开的私有快照；用户需重新读取/打开才能观察新的汇总。
 */
static ssize_t lock_stat_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	/* class/idx 扫描固定池；c 接收唯一解释的首个用户字符。 */
	struct lock_class *class;
	unsigned long idx;
	char c;

	/* 空写直接成功；非空时只做一次可能 fault 的用户字节读取。 */
	if (count) {
		if (get_user(c, buf))
			return -EFAULT;

		/* 协议仅把 ASCII `0` 识别为 reset，其他首字符不报错也不改变统计。 */
		if (c != '0')
			return count;

		/* 只清当前检查瞬间仍活动的 class；并发注册/回收或更新会让结果成为近似边界。 */
		iterate_lock_classes(idx, class) {
			if (!test_bit(idx, lock_classes_in_use))
				continue;
			clear_lock_stats(class);
		}
	}
	return count;
}

/**
 * lock_stat_release - 释放一次 lockstat open 的私有快照和 seq_file
 * @inode: 交给 seq_release() 的 inode
 * @file: private_data 指向 seq_file，且 seq->private 指向 vmalloc 快照
 *
 * 返回：seq_release() 的结果。先 vfree 自定义快照，再让 seq_release() 释放 seq_file；成功 open 后
 * 两者一一配对，函数不再访问已经释放的 private 缓冲。
 */
static int lock_stat_release(struct inode *inode, struct file *file)
{
	/* seq 本身仍由 seq_file 核心拥有，本函数只先取出并释放其中的自定义私有区。 */
	struct seq_file *seq = file->private_data;

	vfree(seq->private);
	/* 最后交还 seq_file 及其分页缓冲，完成 file 生命周期清理。 */
	return seq_release(inode, file);
}

/* `lock_stat` 的 proc 操作表：自定义 open/write/release 管快照，通用 seq helper 负责 read/seek。 */
static const struct proc_ops lock_stat_proc_ops = {
	.proc_open	= lock_stat_open,
	.proc_write	= lock_stat_write,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= lock_stat_release,
};
#endif /* CONFIG_LOCK_STAT */

/**
 * lockdep_proc_init - 按构建配置注册 lockdep 的 procfs 诊断节点
 *
 * 输入：无；在 initcall 阶段使用静态回调表，parent 为 NULL 表示 proc 根目录。
 * 返回：固定 0。始终尝试创建只读 `lockdep` 和 `lockdep_stats`；PROVE_LOCKING 增加只读
 * `lockdep_chains`，LOCK_STAT 增加 root 可读写的 `lock_stat`。当前实现不检查 proc_create*()
 * 返回指针，因此个别节点创建失败也不会回滚已创建节点或向 initcall 框架报告错误。
 */
static int __init lockdep_proc_init(void)
{
	/* 活动 class 使用完整 seq_operations，权限仅允许 owner 读取。 */
	proc_create_seq("lockdep", S_IRUSR, NULL, &lockdep_ops);
#ifdef CONFIG_PROVE_LOCKING
	/* 没有依赖证明就不存在 chain cache 节点及其回调表。 */
	proc_create_seq("lockdep_chains", S_IRUSR, NULL, &lockdep_chains_ops);
#endif
	/* 单次 show 足以输出汇总统计，不需要显式 start/next 状态。 */
	proc_create_single("lockdep_stats", S_IRUSR, NULL, lockdep_stats_show);
#ifdef CONFIG_LOCK_STAT
	/* lock_stat 的写权限服务字符 `0` 清零协议，其余操作由 proc_ops 分派。 */
	proc_create("lock_stat", S_IRUSR | S_IWUSR, NULL, &lock_stat_proc_ops);
#endif

	/* 保持历史 initcall 契约：节点注册失败不影响内核继续启动。 */
	return 0;
}

/* 在普通 initcall 阶段执行节点注册；回调表和名称均为静态生命周期。 */
__initcall(lockdep_proc_init);

