// SPDX-License-Identifier: GPL-2.0
/*
 * Shows data access monitoring results in simple metrics.
 */
/* 本模块用一个物理地址 DAMON context 监控系统 RAM，并把聚合结果导出为只读模块参数。 */

#define pr_fmt(fmt) "damon-stat: " fmt

#include <linux/damon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sort.h>

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_stat."

/* enabled 参数的 set/get 回调在后文定义；操作表让写参数成为启停状态转换。 */
static int damon_stat_enabled_store(
		const char *val, const struct kernel_param *kp);

static int damon_stat_enabled_load(char *buffer,
		const struct kernel_param *kp);

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_stat_enabled_store,
	.get = damon_stat_enabled_load,
};

/* enabled 保存用户期望值；真正运行状态始终由 damon_stat_context/kdamond 判断。 */
static bool enabled __read_mostly = IS_ENABLED(
	CONFIG_DAMON_STAT_ENABLED_DEFAULT);
module_param_cb(enabled, &enabled_param_ops, NULL, 0600);
MODULE_PARM_DESC(enabled, "Enable of disable DAMON_STAT");

/* 最近刷新得到的估算访问带宽，单位 bytes/s；只由 kdamond 回调写。 */
static unsigned long estimated_memory_bandwidth __read_mostly;
module_param(estimated_memory_bandwidth, ulong, 0400);
MODULE_PARM_DESC(estimated_memory_bandwidth,
		"Estimated memory bandwidth usage in bytes per second");

/* 下标 0..100 是按被监控字节加权的分位点，值为带符号空闲时长毫秒。 */
static long memory_idle_ms_percentiles[101] = {0,};
module_param_array(memory_idle_ms_percentiles, long, NULL, 0400);
MODULE_PARM_DESC(memory_idle_ms_percentiles,
		"Memory idle time percentiles in milliseconds");

/* 记录 DAMON 自动调优后的当前聚合间隔，单位微秒。 */
static unsigned long aggr_interval_us;
module_param(aggr_interval_us, ulong, 0400);
MODULE_PARM_DESC(aggr_interval_us,
		"Current tuned aggregation interval in microseconds");

/* 模块独占的 context；非 NULL 不等于线程仍运行，启停回调负责其生命周期。 */
static struct damon_ctx *damon_stat_context;

/* 下一次五秒节流判断的基准 jiffies，仅在 kdamond 和启动路径更新。 */
static unsigned long damon_stat_last_refresh_jiffies;

/*
 * 业务背景：把每个区域在一轮聚合内“命中字节次数”换算成每秒估算访问字节数。
 * 入参：c 是 kdamond 线程内借用且稳定的 context，拥有唯一 target 和当前区域链。
 * 出参/返回：void；更新全局 estimated_memory_bandwidth，不转移引用。
 * 注意事项：在 DAMON 工作线程调用、可遍历但不睡眠；乘法可能是近似统计而非硬件带宽计数。
 */
static void damon_stat_set_estimated_memory_bandwidth(struct damon_ctx *c)
{
	/* t/r 是借用迭代游标；access_bytes 累加 区域字节数×聚合期命中次数。 */
	struct damon_target *t;
	struct damon_region *r;
	unsigned long access_bytes = 0;

	/* 阶段 1：按当前区域划分汇总访问量，区域生命周期由 kdamond 串行保证。 */
	damon_for_each_target(t, c) {
		damon_for_each_region(r, t)
			access_bytes += (r->ar.end - r->ar.start) *
				r->nr_accesses;
	}
	/* 阶段 2：用微秒聚合周期归一化到一秒，并一次发布标量结果。 */
	estimated_memory_bandwidth = access_bytes * USEC_PER_MSEC *
		MSEC_PER_SEC / c->attrs.aggr_interval;
}

/*
 * 业务背景：为访问冷热排序构造带符号“空闲年龄”，让近期有访问的区域排在未访问区域之前。
 * 入参：r 是借用区域；nr_accesses/age 属于刚完成的 DAMON 聚合结果。
 * 出参/返回：有访问返 -(age+1)，无访问返 +(age+1)，永不返回 0。
 * 注意事项：不睡眠、不改状态；符号编码同时区分 active/idle，绝对值以聚合轮数计。
 */
static int damon_stat_idletime(const struct damon_region *r)
{
	if (r->nr_accesses)
		return -1 * (r->age + 1);
	return r->age + 1;
}

/*
 * 业务背景：linux/sort 需要比较器，按上述带符号空闲年龄升序排列区域指针。
 * 入参：a/b 各借用一个“指向 damon_region 指针”的数组元素，均不可空。
 * 出参/返回：负/零/正分别表示 ra 在 rb 前/等价/后；不修改区域。
 * 注意事项：排序期间 context 区域稳定；比较器不睡眠，差值范围受 region age 限制。
 */
static int damon_stat_cmp_regions(const void *a, const void *b)
{
	const struct damon_region *ra = *(const struct damon_region **)a;
	const struct damon_region *rb = *(const struct damon_region **)b;

	return damon_stat_idletime(ra) - damon_stat_idletime(rb);
}

/*
 * 业务背景：百分位统计要按空闲年龄排序，同时以区域覆盖字节而非区域个数加权。
 * 入参：c 为借用 context；sorted_ptr/nr_regions_ptr/total_sz_ptr 均为不可空输出指针。
 * 出参/返回：成功返 0并移交 kmalloc 数组给调用者、写区域数和总字节；失败返 -ENOMEM，输出未提交。
 * 注意事项：当前设计只有一个 target；可睡眠，区域指针只在 kdamond 当前回调期间有效。
 */
static int damon_stat_sort_regions(struct damon_ctx *c,
		struct damon_region ***sorted_ptr, int *nr_regions_ptr,
		unsigned long *total_sz_ptr)
{
	struct damon_target *t;
	struct damon_region *r;
	struct damon_region **region_pointers;
	unsigned int nr_regions = 0;
	unsigned long total_sz = 0;

	/* 阶段 1：为唯一 target 分配指针数组，并建立区域数、总覆盖字节的同一快照。 */
	damon_for_each_target(t, c) {
		/* there is only one target */
		/* 模块构造路径只添加一个系统 RAM target，因此无需扩容或合并多个数组。 */
		region_pointers = kmalloc_objs(*region_pointers,
					       damon_nr_regions(t));
		if (!region_pointers)
			return -ENOMEM;
		damon_for_each_region(r, t) {
			region_pointers[nr_regions++] = r;
			total_sz += r->ar.end - r->ar.start;
		}
	}
	/* 阶段 2：原地排序后才原子式提交三个输出，失败路径不泄漏数组。 */
	sort(region_pointers, nr_regions, sizeof(*region_pointers),
			damon_stat_cmp_regions, NULL);
	*sorted_ptr = region_pointers;
	*nr_regions_ptr = nr_regions;
	*total_sz_ptr = total_sz;
	return 0;
}

/*
 * 业务背景：把排序后的区域转换为按物理内存字节加权的 0..100 空闲时长分位数组。
 * 入参：c 为 kdamond 内借用 context；区域与 attrs 在回调期间稳定。
 * 出参/返回：void；成功覆盖全局 101 个元素，分配失败时保留上次快照。
 * 注意事项：内部 kmalloc 可睡眠；值单位毫秒，负值代表该年龄段仍在被访问。
 */
static void damon_stat_set_idletime_percentiles(struct damon_ctx *c)
{
	/* accounted_bytes 是已排序前缀字节；next_percentile 保证每格仅写一次。 */
	struct damon_region **sorted_regions, *region;
	int nr_regions;
	unsigned long total_sz, accounted_bytes = 0;
	int err, i, next_percentile = 0;

	/* 阶段 1：先取得排序快照；OOM 时不发布半更新的百分位结果。 */
	err = damon_stat_sort_regions(c, &sorted_regions, &nr_regions,
			&total_sz);
	if (err)
		return;
	/* 阶段 2：区域越大跨过的百分位越多，因此按容量而不是条目数加权。 */
	for (i = 0; i < nr_regions; i++) {
		region = sorted_regions[i];
		accounted_bytes += region->ar.end - region->ar.start;
		/* 同一区域覆盖的多个分位点共享其年龄，再用当前聚合间隔换算毫秒。 */
		while (next_percentile <= accounted_bytes * 100 / total_sz)
			memory_idle_ms_percentiles[next_percentile++] =
				damon_stat_idletime(region) *
				(long)c->attrs.aggr_interval / USEC_PER_MSEC;
	}
	/* 指针数组归本函数所有，区域对象仍归 DAMON context。 */
	kfree(sorted_regions);
}

/*
 * 业务背景：damon_call() 在 kdamond 上周期调用此函数，安全读取本轮区域聚合结果并发布指标。
 * 入参：data 是 call_control.data 传入的借用 damon_ctx，类型由启动路径保证。
 * 出参/返回：恒返 0 让 repeat 调用继续；至多每五秒更新三个指标集合。
 * 注意事项：运行在 DAMON 工作线程，可睡眠；节流避免每个聚合周期排序和分配。
 */
static int damon_stat_damon_call_fn(void *data)
{
	struct damon_ctx *c = data;

	/* avoid unnecessarily frequent stat update */
	/* 五秒窗口内直接成功返回，模块参数继续展示上次完整快照。 */
	if (time_before_eq(jiffies, damon_stat_last_refresh_jiffies +
				msecs_to_jiffies(5 * MSEC_PER_SEC)))
		return 0;
	/* 先推进节流时间，再依次发布间隔、带宽和百分位；OOM 仅影响百分位刷新。 */
	damon_stat_last_refresh_jiffies = jiffies;

	aggr_interval_us = c->attrs.aggr_interval;
	damon_stat_set_estimated_memory_bandwidth(c);
	damon_stat_set_idletime_percentiles(c);
	return 0;
}

/*
 * 业务背景：为统计模块构造独立的物理地址监控 context、一个 target 和系统 RAM 默认区域。
 * 入参：无。
 * 出参/返回：成功返回由调用者持有的新 ctx；任一步失败返回 NULL，内部对象全部销毁。
 * 注意事项：进程上下文调用且可睡眠；返回对象尚未启动，damon_stat_start() 负责发布/销毁。
 */
static struct damon_ctx *damon_stat_build_ctx(void)
{
	/* start/end 接收默认 RAM 区域边界；target 添加后由 ctx 接管销毁责任。 */
	struct damon_ctx *ctx;
	struct damon_attrs attrs;
	struct damon_target *target;
	unsigned long start = 0, end = 0;

	/* 阶段 1：创建未运行 context，并定义采样、聚合、ops 更新及区域数量边界。 */
	ctx = damon_new_ctx();
	if (!ctx)
		return NULL;
	attrs = (struct damon_attrs) {
		.sample_interval = 5 * USEC_PER_MSEC,
		.aggr_interval = 100 * USEC_PER_MSEC,
		.ops_update_interval = 60 * USEC_PER_MSEC * MSEC_PER_SEC,
		.min_nr_regions = 10,
		.max_nr_regions = 1000,
	};
	/*
	 * auto-tune sampling and aggregation interval aiming 4% DAMON-observed
	 * accesses ratio, keeping sampling interval in [5ms, 10s] range.
	 */
	/*
	 * 自动调整采样/聚合间隔，使观察到的访问比例趋近 4%；每 3 次聚合调整，
	 * 采样间隔限制在 5ms 到 10s，避免开销或反应时间失控。
	 */
	attrs.intervals_goal = (struct damon_intervals_goal) {
		.access_bp = 400, .aggrs = 3,
		.min_sample_us = 5000, .max_sample_us = 10000000,
	};
	/* set_attrs 校验时间/区域不变量；失败时 ctx 尚未发布，可统一销毁。 */
	if (damon_set_attrs(ctx, &attrs))
		goto free_out;

	/* 阶段 2：选择物理地址监控 ops，使区域表示系统 RAM 地址范围。 */
	if (damon_select_ops(ctx, DAMON_OPS_PADDR))
		goto free_out;

	/* 阶段 3：创建唯一 target；add 后其 ownership 转移给 ctx。 */
	target = damon_new_target();
	if (!target)
		goto free_out;
	damon_add_target(ctx, target);
	/* 用在线系统 RAM 生成默认监控范围；失败由 destroy_ctx 连同 target 回滚。 */
	if (damon_set_region_system_rams_default(target, &start, &end,
				ctx->addr_unit, ctx->min_region_sz))
		goto free_out;
	return ctx;
free_out:
	/* 统一回滚 attrs/ops/target 的所有已完成阶段；调用者不会见到半构造对象。 */
	damon_destroy_ctx(ctx);
	return NULL;
}

/* repeat 使 fn 在每轮 DAMON 主循环重入；data 在每次成功 start 后绑定当前 ctx。 */
static struct damon_call_control call_control = {
	.fn = damon_stat_damon_call_fn,
	.repeat = true,
};

/*
 * 业务背景：把 enabled=true 转为“构造 context、启动 kdamond、安装重复统计调用”的事务。
 * 入参：无；使用模块私有全局状态。
 * 出参/返回：成功返 damon_call() 的 0；运行中返 -EAGAIN；构造失败 -ENOMEM；其余透传 DAMON errno。
 * 注意事项：模块参数写路径串行调用且可睡眠；start 失败销毁 ctx，damon_call 失败时线程可能已启动。
 */
static int damon_stat_start(void)
{
	int err;

	/* 阶段 1：拒绝重复启动；若只剩已停止旧 context，则先销毁再重建。 */
	if (damon_stat_context) {
		if (damon_is_running(damon_stat_context))
			return -EAGAIN;
		damon_destroy_ctx(damon_stat_context);
	}

	/* 阶段 2：构造并启动唯一 context；damon_start 创建其 kdamond。 */
	damon_stat_context = damon_stat_build_ctx();
	if (!damon_stat_context)
		return -ENOMEM;
	err = damon_start(&damon_stat_context, 1, true);
	if (err) {
		damon_destroy_ctx(damon_stat_context);
		damon_stat_context = NULL;
		return err;
	}

	/* 阶段 3：线程运行后设置节流基线，并把重复回调排入该线程。 */
	damon_stat_last_refresh_jiffies = jiffies;
	call_control.data = damon_stat_context;
	return damon_call(damon_stat_context, &call_control);
}

/*
 * 业务背景：enabled=false 时同步停止 kdamond，并释放模块拥有的完整 context 图。
 * 入参：无；要求 damon_stat_context 指向已启动对象。
 * 出参/返回：无直接返回值；停止线程、销毁 ctx 并把全局指针清为 NULL。
 * 注意事项：可睡眠；先 stop 再 destroy，防止工作线程继续访问已释放区域和 call_control.data。
 */
static void damon_stat_stop(void)
{
	/* damon_stop 等线程退出，是 context 最终释放前的生命周期屏障。 */
	damon_stop(&damon_stat_context, 1);
	damon_destroy_ctx(damon_stat_context);
	damon_stat_context = NULL;
}

/*
 * 业务背景：模块参数读取和写入去重都需要查询实际线程状态，而非用户期望变量 enabled。
 * 入参：无。
 * 出参/返回：无 context 或 kdamond 未运行返 false，正在运行返 true；无副作用。
 * 注意事项：只借用全局指针，不取得长期引用；依赖参数回调的外部串行化，不睡眠。
 */
static bool damon_stat_enabled(void)
{
	if (!damon_stat_context)
		return false;
	return damon_is_running(damon_stat_context);
}

/*
 * 业务背景：处理 damon_stat.enabled 写入，把文本布尔值应用为监控启停状态。
 * 入参：val 为借用 NUL 字符串；kp 为参数描述符但本实现不使用，均不转移所有权。
 * 出参/返回：解析/启动错误原样返回，成功或启动前命令行暂存返 0。
 * 注意事项：DAMON 尚未初始化时只能记录期望，init 后才可分配 context；停用会同步等待线程。
 */
static int damon_stat_enabled_store(
		const char *val, const struct kernel_param *kp)
{
	int err;

	/* 阶段 1：先解析并更新期望值；非法文本不改变运行状态。 */
	err = kstrtobool(val, &enabled);
	if (err)
		return err;

	/* 实际状态已满足请求时幂等返回，避免销毁/重建 context。 */
	if (damon_stat_enabled() == enabled)
		return 0;

	if (!damon_initialized())
		/*
		 * probably called from command line parsing (parse_args()).
		 * Cannot call damon_new_ctx().  Let damon_stat_init() handle.
		 */
		/* 启动早期 parse_args 不能调用 damon_new_ctx，留给 module_init 收敛状态。 */
		return 0;

	/* 阶段 2：初始化完成后执行真正状态转换；停用路径没有失败返回。 */
	if (enabled)
		return damon_stat_start();
	damon_stat_stop();
	return 0;
}

/*
 * 业务背景：读取 enabled 参数时展示实际 kdamond 状态，避免报告未兑现的命令行期望。
 * 入参：buffer 为模块参数框架提供的可写输出缓冲；kp 为未使用的借用描述符。
 * 出参/返回：写入 "Y\n" 或 "N\n"，返回 sprintf 写入字节数；不转移 ownership。
 * 注意事项：不睡眠；输出是瞬时状态快照。
 */
static int damon_stat_enabled_load(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%c\n", damon_stat_enabled() ? 'Y' : 'N');
}

/*
 * 业务背景：kdamond_pid 对用户只读，但内核命令行解析仍可能走参数 set 回调，因此提供无操作适配。
 * 入参：val/kp 均为参数框架借用值，本函数不读取也不保存。
 * 出参/返回：恒返 0，不创建线程、不改变 PID 或任何全局状态。
 * 注意事项：该桩不是运行期写权限；module_param_cb 的 0400 已阻止 sysfs 用户写入。
 */
static int damon_stat_kdamond_pid_store(
		const char *val, const struct kernel_param *kp)
{
	/*
	 * kdamond_pid is read-only, but kernel command line could write it.
	 * Do nothing here.
	 */
	/* 只读参数仍可能被内核命令行赋值；刻意忽略，PID 必须来自真实 kdamond。 */
	return 0;
}

/*
 * 业务背景：向用户报告本模块 DAMON 工作线程 PID，便于关联调度和性能观测。
 * 入参：buffer 为可写输出缓冲；kp 为未使用的借用参数描述符。
 * 出参/返回：写入有效正 PID 或 -1 加换行，返回写入字节数。
 * 注意事项：context 不存在或线程尚未建立/已退出均映射为 -1；只取瞬时快照、不持有 task 引用。
 */
static int damon_stat_kdamond_pid_load(
		char *buffer, const struct kernel_param *kp)
{
	int pid;

	/* 阶段 1：无 context 直接形成 -1；否则向 DAMON 核心查询线程标识。 */
	if (!damon_stat_context) {
		pid = -1;
	} else {
		pid = damon_kdamond_pid(damon_stat_context);
		if (pid < 1)
			pid = -1;
	}
	/* 阶段 2：模块参数 ABI 始终输出一个十进制整数和换行。 */
	return sprintf(buffer, "%d\n", pid);
}

/* PID 参数操作表保留命令行 set 桩，并把运行期读取连接到实际线程查询。 */
static const struct kernel_param_ops kdamond_pid_param_ops = {
	.set = damon_stat_kdamond_pid_store,
	.get = damon_stat_kdamond_pid_load,
};

/*
 * PID of the DAMON thread
 *
 * If DAMON_STAT is enabled, this becomes the PID of the worker thread.
 * Else, -1.
 */
/* DAMON_STAT 启用且线程有效时显示其 PID，否则显示 -1；0400 令 sysfs 侧只读。 */
module_param_cb(kdamond_pid, &kdamond_pid_param_ops, NULL, 0400);
MODULE_PARM_DESC(kdamond_pid, "pid of the kdamond");

/*
 * 业务背景：模块初始化时兑现启动早期保存的 enabled 期望，并确认 DAMON 核心可用。
 * 入参：无。
 * 出参/返回：成功返 0；核心未初始化返 -ENOMEM；启动错误原样返回，同时清 enabled。
 * 注意事项：init 上下文可睡眠；失败不会把期望值伪装成已启用状态，context 由 start 负责回滚。
 */
static int __init damon_stat_init(void)
{
	int err = 0;

	/* 阶段 1：DAMON 全局设施不可用时不能安全构造 context。 */
	if (!damon_initialized()) {
		err = -ENOMEM;
		goto out;
	}

	/* probably set via command line */
	/* 阶段 2：enabled 可能由默认配置或 parse_args 设置；此时才真正启动。 */
	if (enabled)
		err = damon_stat_start();

out:
	/* 失败时使用户可见期望与实际未运行状态重新一致。 */
	if (err && enabled)
		enabled = false;
	return err;
}

module_init(damon_stat_init);
