// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON-based page reclamation
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-reclaim: " fmt

#include <linux/damon.h>
#include <linux/kstrtox.h>
#include <linux/module.h>

#include "modules-common.h"

/*
 * 本模块把 DAMON 的物理地址采样、DAMOS_PAGEOUT 动作和 watermark/quota 组合
 * 成一个可由模块参数控制的后台回收器。参数写者只更新候选配置；真正运行中的
 * ctx 必须在 kdamond 上 commit，避免与监控线程并发改写 schemes 和 targets。
 */
#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_reclaim."

/*
 * Enable or disable DAMON_RECLAIM.
 *
 * You can enable DAMON_RCLAIM by setting the value of this parameter as ``Y``.
 * Setting it as ``N`` disables DAMON_RECLAIM.  Note that DAMON_RECLAIM could
 * do no real monitoring and reclamation due to the watermarks-based activation
 * condition.  Refer to below descriptions for the watermarks parameter for
 * this.
 */
/* 中文翻译：enabled 请求启停；watermark 未满足时即使为 Y 也可能不监控、不回收。 */
/* 参数值是用户期望状态，实际状态必须通过 ctx 的 kdamond 是否运行来查询。 */
static bool enabled __read_mostly;

/*
 * Make DAMON_RECLAIM reads the input parameters again, except ``enabled``.
 *
 * Input parameters that updated while DAMON_RECLAIM is running are not applied
 * by default.  Once this parameter is set as ``Y``, DAMON_RECLAIM reads values
 * of parameters except ``enabled`` again.  Once the re-reading is done, this
 * parameter is set as ``N``.  If invalid parameters are found while the
 * re-reading, DAMON_RECLAIM will be disabled.
 */
/* 中文翻译：运行中参数默认不生效；写 Y 请求重读，非法配置会使回收器停用。 */
/* commit_inputs 由自定义 setter 消费，不作为长期状态或运行线程的 ownership 标志。 */
static bool commit_inputs __read_mostly;

/*
 * Time threshold for cold memory regions identification in microseconds.
 *
 * If a memory region is not accessed for this or longer time, DAMON_RECLAIM
 * identifies the region as cold, and reclaims.  120 seconds by default.
 */
/* 中文翻译：区域至少连续 min_age 微秒未访问才视为冷区，默认 120 秒。 */
static unsigned long min_age __read_mostly = 120000000;
module_param(min_age, ulong, 0600);

/* quota 是参数宏原地更新的模板，apply 时由新 scheme 复制其时间/字节/权重约束。 */
static struct damos_quota damon_reclaim_quota = {
	/* use up to 10 ms time, reclaim up to 128 MiB per 1 sec by default */
	/* 中文翻译：默认每秒最多执行 10 ms、回收 128 MiB。 */
	.ms = 10,
	.sz = 128 * 1024 * 1024,
	.reset_interval = 1000,
	/* Within the quota, page out older regions first. */
	/* 中文翻译：额度内只按年龄评分，优先 pageout 更老的区域。 */
	.weight_sz = 0,
	.weight_nr_accesses = 0,
	.weight_age = 1
};
DEFINE_DAMON_MODULES_DAMOS_QUOTAS(damon_reclaim_quota);

/*
 * Desired level of memory pressure-stall time in microseconds.
 *
 * While keeping the caps that set by other quotas, DAMON_RECLAIM automatically
 * increases and decreases the effective level of the quota aiming this level of
 * memory pressure is incurred.  System-wide ``some`` memory PSI in microseconds
 * per quota reset interval (``quota_reset_interval_ms``) is collected and
 * compared to this value to see if the aim is satisfied.  Value zero means
 * disabling this auto-tuning feature.
 *
 * Disabled by default.
 */
/* 中文翻译：以每个 quota 周期的 system-wide some-memory PSI 微秒数为目标调额度。 */
/* 0 表示不创建 PSI quota goal；非零值不突破 quota 模板给出的硬上限。 */
static unsigned long quota_mem_pressure_us __read_mostly;
module_param(quota_mem_pressure_us, ulong, 0600);

/*
 * User-specifiable feedback for auto-tuning of the effective quota.
 *
 * While keeping the caps that set by other quotas, DAMON_RECLAIM automatically
 * increases and decreases the effective level of the quota aiming receiving this
 * feedback of value ``10,000`` from the user.  DAMON_RECLAIM assumes the feedback
 * value and the quota are positively proportional.  Value zero means disabling
 * this auto-tuning feature.
 *
 * Disabled by default.
 *
 */
/* 中文翻译：用户反馈目标固定为 10000，并假设反馈值与有效额度正相关；0 禁用。 */
static unsigned long quota_autotune_feedback __read_mostly;
module_param(quota_autotune_feedback, ulong, 0600);

/*
 * Auto-tune monitoring intervals.
 *
 * If this parameter is set as ``Y``, DAMON_RECLAIM automatically tunes DAMON's
 * sampling and aggregation intervals.  The auto-tuning aims to capture
 * meaningful amount of access events in each DAMON-snapshot, while keeping the
 * sampling intervals 5 milliseconds in minimum, and 10 seconds in maximum.
 * Setting this as ``N`` disables the auto-tuning.
 *
 * Disabled by default.
 */
/* 中文翻译：开启后让 DAMON 在 5 ms 至 10 s 间自调采样周期以捕获足量事件。 */
static bool autotune_monitoring_intervals __read_mostly;
module_param(autotune_monitoring_intervals, bool, 0600);

/* free-memory rate 以千分比表示；高于 high 停止、降至 mid 启动、低于 low 退避。 */
static struct damos_watermarks damon_reclaim_wmarks = {
	.metric = DAMOS_WMARK_FREE_MEM_RATE,
	.interval = 5000000,	/* 5 seconds */
	.high = 500,		/* 50 percent */
	.mid = 400,		/* 40 percent */
	.low = 200,		/* 20 percent */
};
DEFINE_DAMON_MODULES_WMARKS_PARAMS(damon_reclaim_wmarks);

/* 监控属性模板：微秒周期、区域数上下限；apply 时复制后再选择是否自动调优。 */
static struct damon_attrs damon_reclaim_mon_attrs = {
	.sample_interval = 5000,	/* 5 ms */
	.aggr_interval = 100000,	/* 100 ms */
	.ops_update_interval = 0,
	.min_nr_regions = 10,
	.max_nr_regions = 1000,
};
DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(damon_reclaim_mon_attrs);

/*
 * Start of the target memory region in physical address.
 *
 * The start physical address of memory region that DAMON_RECLAIM will do work
 * against.  By default, the system's entire physical memory is used as the
 * region.
 */
/* 中文翻译：物理监控范围起点；默认 0 与 end=0 组合为全部 system RAM。 */
static unsigned long monitor_region_start __read_mostly;
module_param(monitor_region_start, ulong, 0600);

/*
 * End of the target memory region in physical address.
 *
 * The end physical address of memory region that DAMON_RECLAIM will do work
 * against.  By default, the system's entire physical memory is used as the
 * region.
 */
/* 中文翻译：物理监控范围终点；apply helper 会对齐并回写最终采用的边界。 */
static unsigned long monitor_region_end __read_mostly;
module_param(monitor_region_end, ulong, 0600);

/*
 * Scale factor for DAMON_RECLAIM to ops address conversion.
 *
 * This parameter must not be set to 0.
 */
/* 中文翻译：DAMON ops 地址与物理地址换算比例，必须非零且产生 2 的幂最小区域。 */
static unsigned long addr_unit __read_mostly = 1;

/*
 * Skip anonymous pages reclamation.
 *
 * If this parameter is set as ``Y``, DAMON_RECLAIM does not reclaim anonymous
 * pages.  By default, ``N``.
 */
/* 中文翻译：为 Y 时给 scheme 加 anon 匹配且 allow=false 的排除过滤器。 */
static bool skip_anon __read_mostly;
module_param(skip_anon, bool, 0600);

/* kdamond 周期回调把 scheme 内部累计值快照到这个只读模块参数组。 */
static struct damos_stat damon_reclaim_stat;
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(damon_reclaim_stat,
		reclaim_tried_regions, reclaimed_regions, quota_exceeds);

/* ctx/target 从 init 存活到模块生命周期结束；ctx 拥有 target，target 仅作借用别名。 */
static struct damon_ctx *ctx;
static struct damon_target *target;

/*
 * damon_reclaim_new_scheme() - 用当前参数构造一条冷页回收 scheme。
 * @aggr_interval 是微秒聚合周期，必须非零；返回由调用者/ctx 接管的 damos，失败
 * 返回 NULL。函数可睡眠分配，不持 DAMON 锁。它把 min_age 换算成聚合次数，匹配
 * 至少一页、零访问的老区域，动作是 PAGEOUT，并借用 quota/watermark 模板复制值。
 */
static struct damos *damon_reclaim_new_scheme(unsigned long aggr_interval)
{
	struct damos_access_pattern pattern = {
		/* Find regions having PAGE_SIZE or larger size */
		/* 中文翻译：只匹配至少一页大小的区域。 */
		.min_sz_region = PAGE_SIZE,
		.max_sz_region = ULONG_MAX,
		/* and not accessed at all */
		/* 中文翻译：该聚合周期内访问次数必须为零。 */
		.min_nr_accesses = 0,
		.max_nr_accesses = 0,
		/* for min_age or more micro-seconds */
		/* 中文翻译：未访问年龄至少达到 min_age 微秒换算后的聚合次数。 */
		.min_age_region = min_age / aggr_interval,
		.max_age_region = UINT_MAX,
	};

	/* damon_new_scheme() 深拷贝 pattern/quota/wmarks，成功后返回独立对象。 */
	return damon_new_scheme(
			&pattern,
			/* page out those, as soon as found */
			/* 中文翻译：发现匹配冷区后执行 pageout。 */
			DAMOS_PAGEOUT,
			/* for each aggregation interval */
			/* 中文翻译：每个聚合周期都允许检查并执行。 */
			0,
			/* under the quota. */
			/* 中文翻译：实际工作量仍受 quota 限制。 */
			&damon_reclaim_quota,
			/* (De)activate this according to the watermarks. */
			/* 中文翻译：按空闲内存 watermark 激活或停用该 scheme。 */
			&damon_reclaim_wmarks,
			NUMA_NO_NODE);
}

/*
 * damon_reclaim_apply_parameters() - 校验参数并把完整候选配置提交给全局 ctx。
 * 无入参；成功返回 0，非法参数返回 -EINVAL，分配失败返回 -ENOMEM，其他错误透传。
 * 可在启动路径直接调用，也可由 damon_call 在 kdamond 上调用；允许睡眠。函数先
 * 在私有 param_ctx 中构造 target/attrs/scheme/goals/filter/range，只有全部成功后
 * damon_commit_ctx() 才替换运行配置，因此失败不会留下半套全局配置。param_ctx
 * 始终在 out 销毁，并连带释放尚未提交的对象。
 */
static int damon_reclaim_apply_parameters(void)
{
	/* param_ctx 独占候选对象；attrs 是参数模板快照，err 贯穿统一释放出口。 */
	struct damon_ctx *param_ctx;
	struct damon_target *param_target;
	struct damon_attrs attrs;
	struct damos *scheme;
	struct damos_quota_goal *goal;
	struct damos_filter *filter;
	int err;

	/* 阶段一：分配只含一个物理地址 target 的候选 context。 */
	err = damon_modules_new_paddr_ctx_target(&param_ctx, &param_target);
	if (err)
		return err;

	/* addr_unit 换算最小区域；DAMON 区域粒度必须仍为 2 的幂。 */
	param_ctx->addr_unit = addr_unit;
	param_ctx->min_region_sz = max(DAMON_MIN_REGION_SZ / addr_unit, 1);

	if (!is_power_of_2(param_ctx->min_region_sz)) {
		err = -EINVAL;
		goto out;
	}

	/* min_age 除以聚合周期，故零周期在创建 scheme 前必须拒绝。 */
	if (!damon_reclaim_mon_attrs.aggr_interval) {
		err = -EINVAL;
		goto out;
	}

	/* 阶段二：复制监控属性；自动调优只改候选快照，不回写模块参数模板。 */
	attrs = damon_reclaim_mon_attrs;
	if (autotune_monitoring_intervals) {
		attrs.sample_interval = 5000;
		attrs.aggr_interval = 100000;
		attrs.intervals_goal.access_bp = 40;
		attrs.intervals_goal.aggrs = 3;
		attrs.intervals_goal.min_sample_us = 5000;
		attrs.intervals_goal.max_sample_us = 10 * 1000 * 1000;
	}
	/* damon_set_attrs() 进一步校验周期/区域数并写入尚未运行的 param_ctx。 */
	err = damon_set_attrs(param_ctx, &attrs);
	if (err)
		goto out;

	/* 阶段三：构造唯一 scheme；set_schemes 后其 ownership 转给 param_ctx。 */
	err = -ENOMEM;
	scheme = damon_reclaim_new_scheme(attrs.aggr_interval);
	if (!scheme)
		goto out;
	damon_set_schemes(param_ctx, &scheme, 1);

	/* 可选 PSI goal：目标值直接使用每 quota 周期允许的 stall 微秒数。 */
	if (quota_mem_pressure_us) {
		goal = damos_new_quota_goal(DAMOS_QUOTA_SOME_MEM_PSI_US,
				quota_mem_pressure_us);
		if (!goal)
			goto out;
		/* add 后 goal 由 scheme->quota 管理，统一随候选 context 销毁/提交。 */
		damos_add_quota_goal(&scheme->quota, goal);
	}

	/* 可选用户反馈 goal：target=10000，current_value 是本次参数快照。 */
	if (quota_autotune_feedback) {
		goal = damos_new_quota_goal(DAMOS_QUOTA_USER_INPUT, 10000);
		if (!goal)
			goto out;
		goal->current_value = quota_autotune_feedback;
		damos_add_quota_goal(&scheme->quota, goal);
	}

	/* 可选排除 anonymous folio；过滤器成功 add 后 ownership 转给 scheme。 */
	if (skip_anon) {
		filter = damos_new_filter(DAMOS_FILTER_TYPE_ANON, true, false);
		if (!filter)
			goto out;
		damos_add_filter(scheme, filter);
	}

	/* 阶段四：规范化物理范围并在 target 中建立 system-RAM regions。 */
	err = damon_set_region_system_rams_default(param_target,
			&monitor_region_start, &monitor_region_end,
			param_ctx->addr_unit, param_ctx->min_region_sz);
	if (err)
		goto out;
	/* 最终提交点：把完整候选复制/迁移到全局 ctx，运行中由 kdamond 串行执行。 */
	err = damon_commit_ctx(ctx, param_ctx);
out:
	/* 无论 commit 成败，候选骨架都不再使用；destroy 递归释放其剩余 ownership。 */
	damon_destroy_ctx(param_ctx);
	return err;
}

/*
 * damon_reclaim_commit_inputs_fn() - damon_call 适配器，在 kdamond 上应用参数。
 * @arg 当前未使用且无 ownership；返回 apply 的完整 errno。回调可睡眠，并借助
 * kdamond 串行性避免运行中 ctx 被参数写线程直接改动。
 */
static int damon_reclaim_commit_inputs_fn(void *arg)
{
	return damon_reclaim_apply_parameters();
}

/*
 * damon_reclaim_commit_inputs_store() - 处理 commit_inputs 模块参数写请求。
 * @val 可空（无参数写等价 true），@kp 仅满足参数 ABI；false 返回 0 不提交，解析
 * 失败返回 errno，ctx 未初始化返回 -EINVAL，其他返回 damon_call 或回调 errno。
 * 调用在可睡眠参数写上下文，control 位于栈上，damon_call 同步等 kdamond 用完。
 */
static int damon_reclaim_commit_inputs_store(const char *val,
					     const struct kernel_param *kp)
{
	/* control 的 return_code 由 kdamond 写回，调用返回前其栈生命周期始终有效。 */
	bool commit_inputs_request;
	int err;
	struct damon_call_control control = {
		.fn = damon_reclaim_commit_inputs_fn,
	};

	/* 第一阶段把无参/Y/N 统一解析成一次性请求，不直接改 commit_inputs 变量。 */
	if (!val) {
		commit_inputs_request = true;
	} else {
		err = kstrtobool(val, &commit_inputs_request);
		if (err)
			return err;
	}

	/* 写 N 是成功的空操作，现有运行配置保持不变。 */
	if (!commit_inputs_request)
		return 0;

	/*
	 * Skip damon_call() if ctx is not initialized to avoid
	 * NULL pointer dereference.
	 */
	/* 中文翻译：ctx 尚未初始化时跳过 damon_call，防止空指针解引用。 */
	if (!ctx)
		return -EINVAL;

	/* damon_call 注册请求并等待 kdamond 执行，期间 control 不得离开当前栈帧。 */
	err = damon_call(ctx, &control);

	return err ? err : control.return_code;
}

/* 无参标志允许 `commit_inputs` 单独出现；get 仍把 backing bool 格式化给用户。 */
static const struct kernel_param_ops commit_inputs_param_ops = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = damon_reclaim_commit_inputs_store,
	.get = param_get_bool,
};

module_param_cb(commit_inputs, &commit_inputs_param_ops, &commit_inputs, 0600);

/*
 * damon_reclaim_damon_call_fn() - 在 kdamond 周期快照 scheme 统计。
 * @arg 是 call_control.data 中借用的 ctx；遍历其 schemes，把最后一条 stat 复制到
 * 模块参数快照，返回 0 以保留 repeat 回调。由 kdamond 串行调用，不需额外锁；
 * 当前模块只有一条 scheme，若将来增加多条则最后一条覆盖前值。
 */
static int damon_reclaim_damon_call_fn(void *arg)
{
	struct damon_ctx *c = arg;
	struct damos *s;

	/* update the stats parameter */
	/* 中文翻译：把 kdamond 内部统计发布到用户可读的参数快照。 */
	damon_for_each_scheme(s, c)
		damon_reclaim_stat = s->stat;

	return 0;
}

/* 静态 control 随模块存活；repeat 使 core 在每轮 kdamond 循环再次调用它。 */
static struct damon_call_control call_control = {
	.fn = damon_reclaim_damon_call_fn,
	.repeat = true,
};

/*
 * damon_reclaim_turn() - 使实际 kdamond 状态收敛到 @on。
 * @on=false 同步停止并返回 damon_stop errno；true 先应用参数、再独占启动 ctx，
 * 最后注册永久统计回调。函数可睡眠。apply/start 失败不运行；若 damon_call 失败，
 * kdamond 已启动且不会在此回滚，调用者需依据返回值和实际状态决定后续 stop。
 */
static int damon_reclaim_turn(bool on)
{
	int err;

	/* 停止路径等待 worker 退出，是其后安全观察/重配 ctx 的生命周期屏障。 */
	if (!on)
		return damon_stop(&ctx, 1);

	/* 启动阶段一：在 worker 尚未运行时提交一套经过校验的完整配置。 */
	err = damon_reclaim_apply_parameters();
	if (err)
		return err;

	/* 阶段二：exclusive=true 排除其他 DAMON 独占使用者并创建 kdamond。 */
	err = damon_start(&ctx, 1, true);
	if (err)
		return err;
	/* 阶段三注册重复统计回调；这是启动成功后的附加发布，失败不撤销线程。 */
	return damon_call(ctx, &call_control);
}

/*
 * damon_reclaim_addr_unit_store() - 校验并暂存地址换算比例。
 * @val 是 NUL 结尾用户字符串，@kp 未使用；解析失败透传 errno，0 返回 -EINVAL，
 * 成功返回 0 并发布 addr_unit。这里只更新候选参数，运行 ctx 要等下次 apply/commit；
 * 参数核心串行调用 setter，因此简单赋值足够。
 */
static int damon_reclaim_addr_unit_store(const char *val,
		const struct kernel_param *kp)
{
	unsigned long input_addr_unit;
	int err = kstrtoul(val, 0, &input_addr_unit);

	/* 先写局部变量，保证解析/范围校验失败时旧全局值完全不变。 */
	if (err)
		return err;
	if (!input_addr_unit)
		return -EINVAL;

	/* 赋值是用户可观察的参数发布点，不代表当前 kdamond 已采用该值。 */
	addr_unit = input_addr_unit;
	return 0;
}

/* 自定义 set 保证非零，标准 ulong get 仅格式化已发布的候选值。 */
static const struct kernel_param_ops addr_unit_param_ops = {
	.set = damon_reclaim_addr_unit_store,
	.get = param_get_ulong,
};

module_param_cb(addr_unit, &addr_unit_param_ops, &addr_unit, 0600);
MODULE_PARM_DESC(addr_unit,
	"Scale factor for DAMON_RECLAIM to ops address conversion (default: 1)");

/*
 * damon_reclaim_enabled() - 查询实际 worker 状态而非 enabled 请求值。
 * 无入参；ctx 未构造或未运行返回 false，kdamond 活跃返回 true。只读、不睡眠，
 * 返回值是瞬时快照，参数写路径随后仍通过 DAMON 生命周期 API 串行改变状态。
 */
static bool damon_reclaim_enabled(void)
{
	if (!ctx)
		return false;
	return damon_is_running(ctx);
}

/*
 * damon_reclaim_enabled_store() - 处理 enabled 写入并执行必要的启停转换。
 * @val 为布尔字符串，@kp 未使用；解析/构造/启停错误原样返回。先更新期望值
 * enabled；若实际状态相同则幂等返回。早期命令行阶段只记请求，init 稍后执行；
 * 正常阶段调用 turn，允许睡眠。turn 失败不会自动恢复 enabled，init 路径除外。
 */
static int damon_reclaim_enabled_store(const char *val,
		const struct kernel_param *kp)
{
	int err;

	/* 解析成功即发布期望状态，实际 worker 状态在后续分支才改变。 */
	err = kstrtobool(val, &enabled);
	if (err)
		return err;

	/* 幂等快速路径避免重复 start/stop 及不必要的参数重建。 */
	if (damon_reclaim_enabled() == enabled)
		return 0;

	/* Called before init function.  The function will handle this. */
	/* 中文翻译：若发生在 init 前，只保留 enabled；初始化函数会完成真正启停。 */
	if (!damon_initialized())
		return 0;

	/* damon_modules_new_paddr_ctx_target() in the init function failed. */
	/* 中文翻译：init 构造 ctx 失败后无法启动，向用户报告内存不足。 */
	if (!ctx)
		return -ENOMEM;

	/* turn 是实际状态提交点：stop 等退出，start 创建 worker 并注册统计回调。 */
	return damon_reclaim_turn(enabled);
}

/*
 * damon_reclaim_enabled_load() - 把实际运行状态格式化为 Y/N 参数文本。
 * @buffer 由参数核心提供且可写，@kp 未使用；返回 sprintf 写入字节数。只读瞬时
 * kdamond 状态，不取得 ctx ownership，调用者消费文本后即可释放 buffer。
 */
static int damon_reclaim_enabled_load(char *buffer,
		const struct kernel_param *kp)
{
	return sprintf(buffer, "%c\n", damon_reclaim_enabled() ? 'Y' : 'N');
}

/* enabled 的 get 故意不读 backing bool，以免把失败的启动请求误报为运行中。 */
static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_reclaim_enabled_store,
	.get = damon_reclaim_enabled_load,
};

module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled,
	"Enable or disable DAMON_RECLAIM (default: disabled)");

/*
 * damon_reclaim_kdamond_pid_store() - 吞掉命令行对只读 PID 参数的早期写入。
 * @val/@kp 均不消费、不接管；始终返回 0 且无副作用。运行期权限为 0400，不会
 * 通过 sysfs 进入该 setter；保留它只为 module_param_cb 的命令行解析 ABI。
 */
static int damon_reclaim_kdamond_pid_store(const char *val,
		const struct kernel_param *kp)
{
	/*
	 * kdamond_pid is read-only, but kernel command line could write it.
	 * Do nothing here.
	 */
	/* 中文翻译：kdamond_pid 对用户只读，但命令行解析可能调用 set；这里刻意忽略。 */
	return 0;
}

/*
 * damon_reclaim_kdamond_pid_load() - 输出当前 worker PID 或 -1。
 * @buffer 为参数核心所有的输出区，@kp 未使用；返回写入长度。ctx 缺失、未运行或
 * DAMON 返回负值都规范化为 -1；只借用 ctx，不延长 worker 生命周期。
 */
static int damon_reclaim_kdamond_pid_load(char *buffer,
		const struct kernel_param *kp)
{
	int kdamond_pid = -1;

	/* pid 查询是状态快照；worker 可在相邻参数操作间改变，但本次格式化自洽。 */
	if (ctx) {
		kdamond_pid = damon_kdamond_pid(ctx);
		if (kdamond_pid < 0)
			kdamond_pid = -1;
	}
	return sprintf(buffer, "%d\n", kdamond_pid);
}

/* set 仅兼容命令行，get 才是 0400 参数对外提供的有效操作。 */
static const struct kernel_param_ops kdamond_pid_param_ops = {
	.set = damon_reclaim_kdamond_pid_store,
	.get = damon_reclaim_kdamond_pid_load,
};

/*
 * PID of the DAMON thread
 *
 * If DAMON_RECLAIM is enabled, this becomes the PID of the worker thread.
 * Else, -1.
 */
/* 中文翻译：启用时显示 DAMON worker PID，否则显示 -1。 */
module_param_cb(kdamond_pid, &kdamond_pid_param_ops, NULL, 0400);

/*
 * damon_reclaim_init() - 在 DAMON core 就绪后构造模块的长期 ctx/target。
 * 无入参；返回 DAMON 初始化、分配或启用失败 errno。仅在 init 进程上下文调用，
 * 可睡眠。先验证 core，原子式构造 ctx/target，再把统计 control 绑定 ctx；若启动
 * 参数已请求 enabled 则启动 worker。任何错误且曾请求启用都会把 enabled 回滚
 * 为 false；成功后 ctx 拥有 target 并存续整个内建模块生命周期。
 */
static int __init damon_reclaim_init(void)
{
	int err;

	/* 阶段一：core 未完成全局初始化时不能创建 ops/context。 */
	if (!damon_initialized()) {
		err = -ENOMEM;
		goto out;
	}
	/* 阶段二：构造唯一物理地址 context；helper 失败不会留下半初始化输出。 */
	err = damon_modules_new_paddr_ctx_target(&ctx, &target);
	if (err)
		goto out;

	/* 静态重复回调借用长期 ctx，绑定必须发生在任何 damon_call 之前。 */
	call_control.data = ctx;

	/* 'enabled' has set before this function, probably via command line */
	/* 中文翻译：enabled 可能已由内核命令行预置，此处兑现早期启动请求。 */
	if (enabled)
		err = damon_reclaim_turn(true);

out:
	/* 失败时修正用户可见期望状态；ctx 若已构造仍保留供后续诊断/参数查询。 */
	if (err && enabled)
		enabled = false;
	return err;
}

module_init(damon_reclaim_init);
