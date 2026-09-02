// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON-based LRU-lists Sorting
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/* 译注：本模块使用 DAMON 的访问监测结果对内存页在 LRU 链表中的优先级进行排序。 */

#define pr_fmt(fmt) "damon-lru-sort: " fmt

#include <linux/damon.h>
#include <linux/kstrtox.h>
#include <linux/module.h>

#include "modules-common.h"

/*
 * 学习提示：模块构造两个 DAMOS scheme：热区提升 LRU，长期冷区降低 LRU 优先级。
 * 参数先在临时 ctx 中完整验证，damon_commit_ctx() 成功后才替换运行配置。
 */
#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_lru_sort."

/*
 * Enable or disable DAMON_LRU_SORT.
 *
 * You can enable DAMON_LRU_SORT by setting the value of this parameter as
 * ``Y``.  Setting it as ``N`` disables DAMON_LRU_SORT.  Note that
 * DAMON_LRU_SORT could do no real monitoring and LRU-lists sorting due to the
 * watermarks-based activation condition.  Refer to below descriptions for the
 * watermarks parameter for this.
 */
/*
 * 译注：enabled=Y 启用、N 禁用 DAMON_LRU_SORT；即使设为 Y，watermark 激活条件也可能
 * 让模块暂时既不监测也不排序，具体取决于下述 watermark 参数。
 */
static bool enabled __read_mostly;
/* enabled 的真实读值来自 ctx 是否运行，而非仅返回这个请求变量。 */

/*
 * Make DAMON_LRU_SORT reads the input parameters again, except ``enabled``.
 *
 * Input parameters that updated while DAMON_LRU_SORT is running are not
 * applied by default.  Once this parameter is set as ``Y``, DAMON_LRU_SORT
 * reads values of parameters except ``enabled`` again.  Once the re-reading is
 * done, this parameter is set as ``N``.  If invalid parameters are found while
 * the re-reading, DAMON_LRU_SORT will be disabled.
 */
/*
 * 译注：运行中修改的输入默认不立即生效；把 commit_inputs 设为 Y 会重新读取除 enabled 外的
 * 全部参数，完成后自动恢复 N。若重读发现无效参数，DAMON_LRU_SORT 将被禁用。
 */
static bool commit_inputs __read_mostly;
/* 写 true 触发 kdamond 上下文内提交；参数本身由 no-arg ops 暴露。 */

/*
 * Desired active to [in]active memory ratio in bp (1/10,000).
 *
 * While keeping the caps that set by other quotas, DAMON_LRU_SORT
 * automatically increases and decreases the effective level of the quota
 * aiming the LRU [de]prioritizations of the hot and cold memory resulting in
 * this active to [in]active memory ratio.  Value zero means disabling this
 * auto-tuning feature.
 *
 * Disabled by default.
 */
/*
 * 译注：active_mem_bp 以万分比指定期望 active/[in]active 内存比例；模块在其他 quota 上限内
 * 自动增减热页提升和冷页降级的有效额度以逼近目标。0 关闭自动反馈，默认关闭。
 */
static unsigned long active_mem_bp __read_mostly;
/* 0 禁用目标反馈，非零值以万分比描述期望 active memory。 */
module_param(active_mem_bp, ulong, 0600);

/*
 * Auto-tune monitoring intervals.
 *
 * If this parameter is set as ``Y``, DAMON_LRU_SORT automatically tunes
 * DAMON's sampling and aggregation intervals.  The auto-tuning aims to capture
 * meaningful amount of access events in each DAMON-snapshot, while keeping the
 * sampling interval 5 milliseconds in minimum, and 10 seconds in maximum.
 * Setting this as ``N`` disables the auto-tuning.
 *
 * Disabled by default.
 */
/*
 * 译注：启用后自动调整采样/聚合间隔，使每个 DAMON 快照捕获有意义的访问事件，同时把采样
 * 间隔限制在 5 毫秒到 10 秒；N 关闭，默认关闭。
 */
static bool autotune_monitoring_intervals __read_mostly;
/* 自动调优仅改临时 attrs，模块参数模板仍保留用户输入。 */
module_param(autotune_monitoring_intervals, bool, 0600);

/*
 * Filter [non-]young pages accordingly for LRU [de]prioritizations.
 *
 * If this is set, check page level access (youngness) once again before each
 * LRU [de]prioritization operation.  LRU prioritization operation is skipped
 * if the page has not accessed since the last check (not young).  LRU
 * deprioritization operation is skipped if the page has accessed since the
 * last check (young).  The feature is enabled or disabled if this parameter is
 * set as ``Y`` or ``N``, respectively.
 *
 * Disabled by default.
 */
/*
 * 译注：启用后每次 LRU 提升/降低前再次检查页级 young 状态；热页提升跳过自上次检查后未访问的页，
 * 冷页降低跳过其间访问过的页。Y/N 分别启用/关闭，默认关闭。
 */
static bool filter_young_pages __read_mostly;
/* 开启后热/冷 scheme 分别排除非 young/young folio。 */
module_param(filter_young_pages, bool, 0600);

/*
 * Access frequency threshold for hot memory regions identification in permil.
 *
 * If a memory region is accessed in frequency of this or higher,
 * DAMON_LRU_SORT identifies the region as hot, and mark it as accessed on the
 * LRU list, so that it could not be reclaimed under memory pressure.  50% by
 * default.
 */
/*
 * 译注：hot_thres_access_freq 以千分比定义热区访问频率下限；达到阈值的区域会在 LRU 上标记
 * 为已访问，以降低内存压力下被回收的机会，默认 50%。
 */
static unsigned long hot_thres_access_freq = 500;
/* permil 阈值稍后按每聚合周期最大采样次数换算。 */
module_param(hot_thres_access_freq, ulong, 0600);

/*
 * Time threshold for cold memory regions identification in microseconds.
 *
 * If a memory region is not accessed for this or longer time, DAMON_LRU_SORT
 * identifies the region as cold, and mark it as unaccessed on the LRU list, so
 * that it could be reclaimed first under memory pressure.  120 seconds by
 * default.
 */
/*
 * 译注：cold_min_age 以微秒定义冷区未访问时长下限；达到阈值的区域在 LRU 上标记为未访问，
 * 从而在内存压力下优先回收，默认 120 秒。
 */
static unsigned long cold_min_age __read_mostly = 120000000;
/* 冷年龄以微秒输入，构造 scheme 时换算成聚合轮数。 */
module_param(cold_min_age, ulong, 0600);

/* damon_lru_sort_quota 是用户参数模板；apply 时按值复制并由热/冷 scheme 各消费一半时间额度。 */
static struct damos_quota damon_lru_sort_quota = {
	/* 总时间额度会在热、冷两个 scheme 之间各分一半。 */
	/* Use up to 10 ms per 1 sec, by default */
	/* 译注：默认每 1 秒最多使用 10 毫秒。 */
	.ms = 10,
	.sz = 0,
	.reset_interval = 1000,
	/* Within the quota, mark hotter regions accessed first. */
	/* 译注：在 quota 内优先把更热的区域标记为 accessed。 */
	.weight_sz = 0,
	.weight_nr_accesses = 1,
	.weight_age = 1,
};
DEFINE_DAMON_MODULES_DAMOS_TIME_QUOTA(damon_lru_sort_quota);

/* damon_lru_sort_wmarks 是 free-memory-rate 激活模板，由每个新 scheme 复制。 */
static struct damos_watermarks damon_lru_sort_wmarks = {
	/* free-memory rate 在 high/mid/low 间控制 scheme 激活状态。 */
	.metric = DAMOS_WMARK_FREE_MEM_RATE,
	.interval = 5000000,	/* 5 seconds */
	/* 译注：watermark 每 5 秒检查一次。 */
	.high = 200,		/* 20 percent */
	.mid = 150,		/* 15 percent */
	.low = 50,		/* 5 percent */
	/* 译注：high/mid/low 分别表示 20%、15% 和 5% 的空闲内存率。 */
};
DEFINE_DAMON_MODULES_WMARKS_PARAMS(damon_lru_sort_wmarks);

/* damon_lru_sort_mon_attrs 是监测参数模板，提交时复制后可再做自动调优。 */
static struct damon_attrs damon_lru_sort_mon_attrs = {
	/* 默认 5ms 采样、100ms 聚合，并限制 region 数量。 */
	.sample_interval = 5000,	/* 5 ms */
	.aggr_interval = 100000,	/* 100 ms */
	/* 译注：默认每 5ms 采样、每 100ms 聚合。 */
	.ops_update_interval = 0,
	.min_nr_regions = 10,
	.max_nr_regions = 1000,
};
DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(damon_lru_sort_mon_attrs);

/*
 * Start of the target memory region in physical address.
 *
 * The start physical address of memory region that DAMON_LRU_SORT will do work
 * against.  By default, the system's entire physical memory is used as the
 * region.
 */
/*
 * 译注：monitor_region_start 是模块工作的物理内存区域起点；默认使用系统全部物理内存。
 */
static unsigned long monitor_region_start __read_mostly;
/* start/end 默认零，由 system-RAM helper 扩展为全物理内存。 */
module_param(monitor_region_start, ulong, 0600);

/*
 * End of the target memory region in physical address.
 *
 * The end physical address of memory region that DAMON_LRU_SORT will do work
 * against.  By default, the system's entire physical memory is used as the
 * region.
 */
/*
 * 译注：monitor_region_end 是模块工作的物理内存区域终点；默认使用系统全部物理内存。
 */
static unsigned long monitor_region_end __read_mostly;
/* 显式端点也会按 addr_unit/min_region_sz 规范化。 */
module_param(monitor_region_end, ulong, 0600);

/*
 * Scale factor for DAMON_LRU_SORT to ops address conversion.
 *
 * This parameter must not be set to 0.
 */
/* 译注：addr_unit 是 DAMON_LRU_SORT 转换到 ops 地址坐标的比例因子，禁止设置为 0。 */
static unsigned long addr_unit __read_mostly = 1;
/* 地址单位参与 ctx 坐标换算，store 明确拒绝零。 */

/* hot_stat/cold_stat 是回调周期性发布给只读模块参数的按 action 统计快照。 */
static struct damos_stat damon_lru_sort_hot_stat;
/* stats 参数由重复 damon_call 从运行 scheme 快照更新。 */
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(damon_lru_sort_hot_stat,
		lru_sort_tried_hot_regions, lru_sorted_hot_regions,
		hot_quota_exceeds);

static struct damos_stat damon_lru_sort_cold_stat;
/* 热、冷统计按 action 分类，避免依赖 scheme 链表顺序。 */
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(damon_lru_sort_cold_stat,
		lru_sort_tried_cold_regions, lru_sorted_cold_regions,
		cold_quota_exceeds);

/* stub_pattern 是创建热/冷 scheme 的全范围只读模板，各 helper 按值复制后收紧条件。 */
static struct damos_access_pattern damon_lru_sort_stub_pattern = {
	/* stub 先覆盖所有有效 region，派生函数只收紧热度或冷龄条件。 */
	/* Find regions having PAGE_SIZE or larger size */
	/* 译注：只考虑大小至少为 PAGE_SIZE 的区域。 */
	.min_sz_region = PAGE_SIZE,
	.max_sz_region = ULONG_MAX,
	/* no matter its access frequency */
	/* 译注：模板阶段不限制访问频率。 */
	.min_nr_accesses = 0,
	.max_nr_accesses = UINT_MAX,
	/* no matter its age */
	/* 译注：模板阶段不限制区域年龄。 */
	.min_age_region = 0,
	.max_age_region = UINT_MAX,
};

static struct damon_ctx *ctx;
/* ctx/target 在 init 创建一次；临时参数 ctx 仅用于原子提交。 */
static struct damon_target *target;

/*
 * 业务背景：热/冷构造器共享 quota/watermark 装配，本 helper 创建一个尚未挂 ctx 的 DAMOS scheme。
 * 入参：pattern 是调用期间借用的按值配置；action 仅应为 LRU_PRIO 或 LRU_DEPRIO，决定提升/降低。
 * 出参/返回：成功返回调用者拥有的新 scheme，失败 NULL；全局 quota 模板不被修改。
 * 注意事项：可分配内存；每个 scheme 只取总 ms quota 的一半，后续须挂 ctx 或 destroy。
 */
static struct damos *damon_lru_sort_new_scheme(
		struct damos_access_pattern *pattern, enum damos_action action)
{
	/* 按值复制 quota，拆半不会改写全局参数模板。 */
	struct damos_quota quota = damon_lru_sort_quota;

	/* Use half of total quota for hot/cold pages sorting */
	/* 译注：热页和冷页排序各使用总 quota 的一半。 */
	/* 奇数毫秒向下取整，使两 scheme 总和不超过用户上限。 */
	quota.ms = quota.ms / 2;

	/* watermark 与 quota 快照随新 scheme 一并建立。 */
	return damon_new_scheme(
			/* find the pattern, and */
			/* 译注：匹配给定访问模式。 */
			pattern,
			/* (de)prioritize on LRU-lists */
			/* 译注：在 LRU 链表上提升或降低优先级。 */
			action,
			/* for each aggregation interval */
			/* 译注：每个聚合周期均可应用。 */
			0,
			/* under the quota. */
			/* 译注：受上述 quota 限制。 */
			&quota,
			/* (De)activate this according to the watermarks. */
			/* 译注：依据 watermark 激活或停用。 */
			&damon_lru_sort_wmarks,
			NUMA_NO_NODE);
}

/* Create a DAMON-based operation scheme for hot memory regions */
/* 译注：为热内存区域创建一个基于 DAMON 的操作 scheme。 */
/*
 * 业务背景：apply 把千分比热阈值换成访问次数后，由此收紧模板并选择 LRU_PRIO 动作。
 * 入参：hot_thres 是每聚合周期的最小访问次数，纯值输入。
 * 出参/返回：成功返回调用者拥有的热 scheme，分配失败 NULL；无全局状态变化。
 * 注意事项：局部 pattern 只在 new_scheme 调用期间借用，core 必须复制其内容。
 */
static struct damos *damon_lru_sort_new_hot_scheme(unsigned int hot_thres)
{
	/* 热 scheme 只设置最小访问次数，action 为 LRU_PRIO。 */
	struct damos_access_pattern pattern = damon_lru_sort_stub_pattern;

	pattern.min_nr_accesses = hot_thres;
	return damon_lru_sort_new_scheme(&pattern, DAMOS_LRU_PRIO);
}

/* Create a DAMON-based operation scheme for cold memory regions */
/* 译注：为冷内存区域创建一个基于 DAMON 的操作 scheme。 */
/*
 * 业务背景：apply 把冷时间换成聚合 age 后，由此要求零访问且达到年龄并选择 LRU_DEPRIO。
 * 入参：cold_thres 是最小 DAMON age 聚合轮数，纯值输入。
 * 出参/返回：成功返回调用者拥有的冷 scheme，分配失败 NULL；无全局状态变化。
 * 注意事项：局部 pattern 由下层复制，返回对象必须挂 ctx 或销毁。
 */
static struct damos *damon_lru_sort_new_cold_scheme(unsigned int cold_thres)
{
	/* 冷 scheme 要求零访问且 age 达阈值，action 为 LRU_DEPRIO。 */
	struct damos_access_pattern pattern = damon_lru_sort_stub_pattern;

	pattern.max_nr_accesses = 0;
	pattern.min_age_region = cold_thres;
	return damon_lru_sort_new_scheme(&pattern, DAMOS_LRU_DEPRIO);
}

/*
 * 业务背景：启用 active_mem_bp 时，热/冷 scheme 需要相反的 quota feedback goal 维持目标比例。
 * 入参：hot_scheme/cold_scheme 是尚未发布、由 param_ctx 拥有的可修改对象。
 * 出参/返回：关闭反馈或两目标成功返回 0；分配失败 -ENOMEM；已挂 goal ownership 属于对应 quota。
 * 注意事项：第二个 goal 失败不会局部拆除第一个，外层销毁整个 param_ctx 完成统一回滚。
 */
static int damon_lru_sort_add_quota_goals(struct damos *hot_scheme,
		struct damos *cold_scheme)
{
	/* active_mem_bp=0 时不创建反馈目标，沿用静态 quota。 */
	struct damos_quota_goal *goal;

	if (!active_mem_bp)
		return 0;
	/* 热动作以 active 比例为目标，创建成功后所有权交给 quota。 */
	goal = damos_new_quota_goal(DAMOS_QUOTA_ACTIVE_MEM_BP, active_mem_bp);
	if (!goal)
		return -ENOMEM;
	damos_add_quota_goal(&hot_scheme->quota, goal);
	/* aim 0.2 % goal conflict, to keep little ping pong */
	/* 译注：让两个目标相差 0.02%（2bp），以少量冲突形成滞回并减少来回振荡。 */
	/* 冷目标加 0.02% 滞回，减少两个方向来回振荡。 */
	goal = damos_new_quota_goal(DAMOS_QUOTA_INACTIVE_MEM_BP,
			10000 - active_mem_bp + 2);
	if (!goal)
		return -ENOMEM;
	damos_add_quota_goal(&cold_scheme->quota, goal);
	return 0;
}

/*
 * 业务背景：可选 young 二次检查在 DAMON 区域粒度判断后过滤与页级状态矛盾的候选。
 * 入参：hot_scheme/cold_scheme 是 param_ctx 拥有的可修改对象。
 * 出参/返回：关闭或两个 filter 成功返回 0；分配失败 -ENOMEM；成功 filter ownership 转给 scheme。
 * 注意事项：第二次失败由外层整体 ctx 回滚；matching 值决定排除 non-young 或 young。
 */
static int damon_lru_sort_add_filters(struct damos *hot_scheme,
		struct damos *cold_scheme)
{
	/* 过滤关闭时不分配对象，两个 scheme 都直接处理候选。 */
	struct damos_filter *filter;

	if (!filter_young_pages)
		return 0;

	/* disallow prioritizing not-young pages */
	/* 译注：禁止提升并非 young 的页面。 */
	/* matching=false 排除非 young 页，避免错误提升冷页。 */
	filter = damos_new_filter(DAMOS_FILTER_TYPE_YOUNG, false, false);
	if (!filter)
		return -ENOMEM;
	damos_add_filter(hot_scheme, filter);

	/* disabllow de-prioritizing young pages */
	/* 译注：禁止降低 young 页面的优先级。 */
	/* matching=true 排除 young 页，避免降低近期访问页。 */
	filter = damos_new_filter(DAMOS_FILTER_TYPE_YOUNG, true, false);
	if (!filter)
		return -ENOMEM;
	damos_add_filter(cold_scheme, filter);
	return 0;
}

/*
 * 业务背景：启用/commit 都要先在隔离 ctx 中验证全部参数和资源，最后原子替换运行 ctx 配置。
 * 入参：无；读取模块参数模板与长期全局 ctx/target，但不接管它们。
 * 出参/返回：成功 0并由 damon_commit_ctx 发布完整配置；格式/范围错误或分配失败返回负 errno。
 * 注意事项：可睡眠；所有中间对象归 param_ctx，唯一 out 销毁实现回滚，commit 是不可见到可见的边界。
 */
static int damon_lru_sort_apply_parameters(void)
{
	/* 所有候选对象先挂到 param_ctx，失败可整体 destroy 回滚。 */
	struct damon_ctx *param_ctx;
	struct damon_target *param_target;
	struct damon_attrs attrs;
	struct damos *hot_scheme, *cold_scheme;
	unsigned int hot_thres, cold_thres;
	int err;

	/* 创建物理地址 ops 的临时 ctx/target，不直接扰动运行 ctx。 */
	err = damon_modules_new_paddr_ctx_target(&param_ctx, &param_target);
	if (err)
		return err;

	/* 最小 region 换算到 ops 地址单位，并至少保留一个单位。 */
	param_ctx->addr_unit = addr_unit;
	param_ctx->min_region_sz = max(DAMON_MIN_REGION_SZ / addr_unit, 1);

	/* DAMON region 对齐要求换算后的最小尺寸为二次幂。 */
	if (!is_power_of_2(param_ctx->min_region_sz)) {
		err = -EINVAL;
		goto out;
	}

	/* 零采样间隔会破坏次数换算，直接拒绝。 */
	if (!damon_lru_sort_mon_attrs.sample_interval) {
		err = -EINVAL;
		goto out;
	}

	/* attrs 按值快照，避免并发模块参数写造成半套配置。 */
	attrs = damon_lru_sort_mon_attrs;
	if (autotune_monitoring_intervals) {
		/* 自动目标在 5ms..10s 内调整，期望每快照约 40% access。 */
		attrs.sample_interval = 5000;
		attrs.aggr_interval = 100000;
		attrs.intervals_goal.access_bp = 40;
		attrs.intervals_goal.aggrs = 3;
		attrs.intervals_goal.min_sample_us = 5000;
		attrs.intervals_goal.max_sample_us = 10 * 1000 * 1000;
	}
	/* core 完成 attrs 交叉约束校验后才继续构造 scheme。 */
	err = damon_set_attrs(param_ctx, &attrs);
	if (err)
		goto out;

	err = -ENOMEM;
	/* permil 热阈值换算为一个聚合周期内的访问次数。 */
	hot_thres = damon_max_nr_accesses(&attrs) *
		hot_thres_access_freq / 1000;
	hot_scheme = damon_lru_sort_new_hot_scheme(hot_thres);
	if (!hot_scheme)
		goto out;

	/* 微秒冷龄向下换算为 DAMON age 聚合轮数。 */
	cold_thres = cold_min_age / attrs.aggr_interval;
	cold_scheme = damon_lru_sort_new_cold_scheme(cold_thres);
	if (!cold_scheme) {
		damon_destroy_scheme(hot_scheme);
		goto out;
	}

	/* set 接管热 scheme，add 再接管冷 scheme；ctx 销毁覆盖后续失败。 */
	damon_set_schemes(param_ctx, &hot_scheme, 1);
	damon_add_scheme(param_ctx, cold_scheme);

	/* goal/filter 分配失败统一销毁 param_ctx 及已挂对象。 */
	err = damon_lru_sort_add_quota_goals(hot_scheme, cold_scheme);
	if (err)
		goto out;
	err = damon_lru_sort_add_filters(hot_scheme, cold_scheme);
	if (err)
		goto out;

	/* 规范化监测范围，零端点退化为系统 RAM 默认范围。 */
	err = damon_set_region_system_rams_default(param_target,
					&monitor_region_start,
					&monitor_region_end,
					param_ctx->addr_unit,
					param_ctx->min_region_sz);
	if (err)
		goto out;
	/* commit 是唯一发布点，成功后运行 ctx 获得完整候选配置。 */
	err = damon_commit_ctx(ctx, param_ctx);
out:
	/* commit 复制/接管所需状态后，临时 ctx 始终可销毁。 */
	damon_destroy_ctx(param_ctx);
	return err;
}

/*
 * 业务背景：damon_call 需要一个在 kdamond 安全点执行的无类型回调来应用最新模块参数。
 * 入参：arg 当前未使用，调用者仍通过 control 同步等待回调完成。
 * 出参/返回：原样返回 apply 的 0 或负 errno；可能原子更新运行 ctx 配置。
 * 注意事项：运行于 kdamond 上下文，可睡眠能力遵循 damon_call 契约；不保存 arg。
 */
static int damon_lru_sort_commit_inputs_fn(void *arg)
{
	/* damon_call 在 kdamond 安全上下文执行真正参数提交。 */
	return damon_lru_sort_apply_parameters();
}

/*
 * 业务背景：commit_inputs 模块参数把运行中模板修改同步送到 kdamond，避免外部线程直接改运行 ctx。
 * 入参：val 可为 NULL（NOARG=true）或布尔字符串；kp 未使用且不保存。
 * 出参/返回：false 返回 0；未初始化 -EINVAL；解析/transport/apply 错误原样返回，成功 0。
 * 注意事项：damon_call 同步消费栈上 control；只提交参数，不改变 enabled 请求变量。
 */
static int damon_lru_sort_commit_inputs_store(const char *val,
					      const struct kernel_param *kp)
{
	/* no-arg 写等价 true；显式 false 仅更新参数而不触发提交。 */
	bool commit_inputs_request;
	int err;
	struct damon_call_control control = {
		/* control 位于栈上，damon_call 返回前完成同步消费。 */
		.fn = damon_lru_sort_commit_inputs_fn,
	};

	if (!val) {
		commit_inputs_request = true;
	} else {
		err = kstrtobool(val, &commit_inputs_request);
		if (err)
			return err;
	}

	/* false 请求幂等返回，不访问尚未初始化的 ctx。 */
	if (!commit_inputs_request)
		return 0;

	/*
	 * Skip damon_call() if ctx is not initialized to avoid
	 * NULL pointer dereference.
	 */
	/* 译注：ctx 尚未初始化时跳过 damon_call()，以免解引用 NULL。 */
	if (!ctx)
		return -EINVAL;

	/* 返回 transport 错误优先，否则返回回调内 apply 的结果。 */
	err = damon_call(ctx, &control);

	return err ? err : control.return_code;
}

/* commit_inputs_param_ops 是模块存续期只读操作表：NOARG set 触发提交，get 读取请求变量。 */
static const struct kernel_param_ops commit_inputs_param_ops = {
	/* NOARG 允许仅写参数名触发 true 请求。 */
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = damon_lru_sort_commit_inputs_store,
	.get = param_get_bool,
};

module_param_cb(commit_inputs, &commit_inputs_param_ops, &commit_inputs, 0600);

/*
 * 业务背景：重复 damon_call 在监测线程安全点把两个运行 scheme 的统计发布到模块参数快照。
 * 入参：arg 是 call_control.data 保存的 ctx 借用指针，运行期间稳定。
 * 出参/返回：始终 0；按 action 覆盖 hot/cold stat，全局快照供参数读取。
 * 注意事项：只识别 LRU_PRIO/DEPRIO，未知 action 不改统计；由 kdamond 串行 scheme 遍历。
 */
static int damon_lru_sort_damon_call_fn(void *arg)
{
	/* 重复回调在 kdamond 上下文中安全遍历运行 scheme。 */
	struct damon_ctx *c = arg;
	struct damos *s;

	/* update the stats parameter */
	/* 译注：更新对外统计参数。 */
	/* action 决定写入热或冷的对外统计快照。 */
	damon_for_each_scheme(s, c) {
		if (s->action == DAMOS_LRU_PRIO)
			damon_lru_sort_hot_stat = s->stat;
		else if (s->action == DAMOS_LRU_DEPRIO)
			damon_lru_sort_cold_stat = s->stat;
	}

	return 0;
}

/* call_control 在模块全生命周期存活；init 后 data 指向长期 ctx，repeat 使 core 周期调用 fn。 */
static struct damon_call_control call_control = {
	/* repeat=true 让 core 周期性刷新模块参数统计。 */
	.fn = damon_lru_sort_damon_call_fn,
	.repeat = true,
};

/*
 * 业务背景：enabled 参数与 init 共用此状态转换，负责停止或以完整新配置启动唯一 DAMON ctx。
 * 入参：on=true 请求 apply→start→注册重复回调，false 请求停止；纯值输入。
 * 出参/返回：返回 stop/apply/start/call 的结果；start 后 call 失败仍可能留下运行 ctx，调用者须按实际状态查询。
 * 注意事项：可睡眠；开启不是事务性回滚，damon_start 是运行态发布点，关闭依赖 core 完成线程停止。
 */
static int damon_lru_sort_turn(bool on)
{
	/* 关闭直接停止唯一 ctx；开启先提交参数再启动并注册重复回调。 */
	int err;

	if (!on)
		return damon_stop(&ctx, 1);

	/* 参数无效时保持停止状态，不启动半配置 context。 */
	err = damon_lru_sort_apply_parameters();
	if (err)
		return err;

	/* start 成功后 damon_call 失败会作为启用失败返回给参数写者。 */
	err = damon_start(&ctx, 1, true);
	if (err)
		return err;
	return damon_call(ctx, &call_control);
}

/*
 * 业务背景：自定义 setter 必须在发布 addr_unit 模板前拒绝解析错误和零除风险。
 * 入参：val 是用户数字字符串；kp 未使用，input_addr_unit 是解析暂存值。
 * 出参/返回：合法非零返回 0并更新模板；解析错误或零值返回负 errno且旧值不变。
 * 注意事项：运行中写入不自动 commit；无额外锁，模块参数框架串行同一参数操作。
 */
static int damon_lru_sort_addr_unit_store(const char *val,
		const struct kernel_param *kp)
{
	/* 先解析局部变量，验证非零后一次性发布全局参数。 */
	unsigned long input_addr_unit;
	int err = kstrtoul(val, 0, &input_addr_unit);

	if (err)
		return err;
	if (!input_addr_unit)
		return -EINVAL;

	/* 运行中修改只更新模板，需 commit_inputs 才应用。 */
	addr_unit = input_addr_unit;
	return 0;
}

/* addr_unit_param_ops 在模块存续期只读，set 执行非零验证，get 使用通用 ulong 格式。 */
static const struct kernel_param_ops addr_unit_param_ops = {
	/* 自定义 set 保证零值永远不会发布。 */
	.set = damon_lru_sort_addr_unit_store,
	.get = param_get_ulong,
};

module_param_cb(addr_unit, &addr_unit_param_ops, &addr_unit, 0600);
MODULE_PARM_DESC(addr_unit,
	"Scale factor for DAMON_LRU_SORT to ops address conversion (default: 1)");

/*
 * 业务背景：参数 get/幂等判断必须报告 core 的真实运行态，而非可能尚未兑现的 enabled 请求变量。
 * 入参：无，读取全局 ctx。
 * 出参/返回：ctx 存在且 kdamond 正运行返回 true，否则 false；无副作用。
 * 注意事项：这是瞬时状态查询，不取得 ctx 引用；模块生命周期保证对象不并发销毁。
 */
static bool damon_lru_sort_enabled(void)
{
	/* ctx 未创建与已停止都对用户报告 disabled。 */
	if (!ctx)
		return false;
	return damon_is_running(ctx);
}

/*
 * 业务背景：enabled setter 解析用户期望，并在 DAMON/ctx 就绪后执行实际启停状态转换。
 * 入参：val 是布尔字符串；kp 未使用；结果先写全局 enabled 请求变量。
 * 出参/返回：解析/初始化/turn 错误为负 errno，幂等或早期留存请求返回 0。
 * 注意事项：早期命令行只记录请求由 init 兑现；turn 失败时 enabled 可能与真实运行态暂时不同，get 查询后者。
 */
static int damon_lru_sort_enabled_store(const char *val,
		const struct kernel_param *kp)
{
	/* enabled 先保存请求值，后续依据生命周期决定是否立即 turn。 */
	int err;

	err = kstrtobool(val, &enabled);
	if (err)
		return err;

	/* 真实运行态已等于请求时幂等返回。 */
	if (damon_lru_sort_enabled() == enabled)
		return 0;

	/* Called before init function.  The function will handle this. */
	/* 译注：若在 init 函数前调用，只保留请求，init 会负责处理。 */
	/* 早期命令行写只留存请求，由模块 init 稍后执行。 */
	if (!damon_initialized())
		return 0;

	/* damon_modules_new_paddr_ctx_target() in the init function failed. */
	/* 译注：此状态表示 init 中 damon_modules_new_paddr_ctx_target() 失败。 */
	/* DAMON 已初始化但本模块 ctx 为空表示 init 分配失败。 */
	if (!ctx)
		return -ENOMEM;

	return damon_lru_sort_turn(enabled);
}

/*
 * 业务背景：enabled 参数读取端把真实 kdamond 状态格式化为标准 Y/N 文本。
 * 入参：buffer 是模块参数框架提供的可写缓冲区；kp 未使用。
 * 出参/返回：返回含换行的写入长度，内容为 Y 或 N；无状态变化。
 * 注意事项：不直接读取请求变量，因早期请求或启停失败时它可能与运行态不同。
 */
static int damon_lru_sort_enabled_load(char *buffer,
		const struct kernel_param *kp)
{
	/* get 显示真实运行态并附带参数 ABI 所需换行。 */
	return sprintf(buffer, "%c\n", damon_lru_sort_enabled() ? 'Y' : 'N');
}

/* enabled_param_ops 在模块存续期只读，set 控制状态机，get 显示真实运行状态。 */
static const struct kernel_param_ops enabled_param_ops = {
	/* get 不直接读 enabled 变量，而查询实际 kdamond 状态。 */
	.set = damon_lru_sort_enabled_store,
	.get = damon_lru_sort_enabled_load,
};

module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled,
	"Enable or disable DAMON_LRU_SORT (default: disabled)");

/*
 * 业务背景：kdamond_pid 对用户只读，但内核命令行解析仍可能调用 setter，因此需要兼容空操作。
 * 入参：val/kp 均不使用、不保存。
 * 出参/返回：始终返回 0且不改变 PID、ctx 或任何参数状态。
 * 注意事项：该桩不代表运行时属性可写，文件权限仍为 0400。
 */
static int damon_lru_sort_kdamond_pid_store(const char *val,
		const struct kernel_param *kp)
{
	/* 只读参数仍需 set 桩吸收内核命令行的早期赋值。 */
	/*
	 * kdamond_pid is read-only, but kernel command line could write it.
	 * Do nothing here.
	 */
	/* 译注：kdamond_pid 是只读参数，但内核命令行可能尝试写入；这里刻意不做任何事。 */
	return 0;
}

/*
 * 业务背景：诊断参数需要暴露当前 DAMON worker PID，并把未运行/查询失败统一表示为 -1。
 * 入参：buffer 是参数框架输出缓冲区；kp 未使用。
 * 出参/返回：返回十进制 PID 加换行的长度；ctx 缺失或 core 负结果时写 -1。
 * 注意事项：只做快照查询，不取得 task/PID 引用，结果返回后线程可立即退出。
 */
static int damon_lru_sort_kdamond_pid_load(char *buffer,
		const struct kernel_param *kp)
{
	/* 未创建或未运行统一报告 -1。 */
	int kdamond_pid = -1;

	/* ctx 存在时向 core 查询当前 kdamond pid，负错误折叠为 -1。 */
	if (ctx) {
		kdamond_pid = damon_kdamond_pid(ctx);
		/* core 的任意负返回对只读 ABI 统一显示 -1。 */
		if (kdamond_pid < 0)
			kdamond_pid = -1;
	}
	return sprintf(buffer, "%d\n", kdamond_pid);
}

/* kdamond_pid_param_ops 是模块存续期只读表；set 仅兼容命令行，get 查询 worker。 */
static const struct kernel_param_ops kdamond_pid_param_ops = {
	/* set 是命令行兼容空操作，get 提供运行线程 PID。 */
	.set = damon_lru_sort_kdamond_pid_store,
	.get = damon_lru_sort_kdamond_pid_load,
};

/*
 * PID of the DAMON thread
 *
 * If DAMON_LRU_SORT is enabled, this becomes the PID of the worker thread.
 * Else, -1.
 */
/* 译注：该参数显示 DAMON 线程 PID；模块启用时为 worker PID，否则为 -1。 */
module_param_cb(kdamond_pid, &kdamond_pid_param_ops, NULL, 0400);

/*
 * 业务背景：模块初始化建立唯一物理地址 ctx/target，连接重复统计回调，并兑现命令行预启用请求。
 * 入参：无；读取全局 enabled 与参数模板。
 * 出参/返回：成功 0；DAMON 未就绪、ctx 创建或预启用失败返回负 errno，失败时清 enabled 请求。
 * 注意事项：init 上下文可睡眠；ctx/target 成功后由内核长期拥有，Kconfig 为 bool 且本文件无 exit 路径。
 */
static int __init damon_lru_sort_init(void)
{
	/* init 创建长期 ctx/target，绑定统计回调数据并兑现早期 enabled 请求。 */
	int err;

	/* DAMON core 未就绪时模块无法建立物理监测上下文。 */
	if (!damon_initialized()) {
		err = -ENOMEM;
		goto out;
	}
	err = damon_modules_new_paddr_ctx_target(&ctx, &target);
	if (err)
		goto out;

	/* 重复回调参数在 ctx 成功创建后才发布。 */
	call_control.data = ctx;

	/* 'enabled' has set before this function, probably via command line */
	/* 译注：enabled 可能已在本函数前由内核命令行设置。 */
	/* 命令行预启用在此执行完整 apply/start/call 流程。 */
	if (enabled)
		err = damon_lru_sort_turn(true);

out:
	/* 初始化失败时清除对外 enabled 请求，避免状态与运行态不符。 */
	if (err && enabled)
		enabled = false;
	return err;
}

module_init(damon_lru_sort_init);
