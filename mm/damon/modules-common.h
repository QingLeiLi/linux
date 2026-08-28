/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 中文学习注释由 OpenAI Codex（GPT-5，2026-08-27）生成。
 *
 * 文件地图：DAMON_RECLAIM 和 DAMON_LRU_SORT 用本头文件把各自的结构字段批量
 * 暴露为同名模块参数，并共享物理地址监控 context/target 构造入口。宏只生成
 * module_param 描述与权限，不复制结构值；模块参数读写因而直接读写传入对象的
 * 字段，真正把新值提交给运行中的 DAMON 仍由模块的 commit_inputs 路径负责。
 *
 * 接口边界：可写参数统一为属主可读写的 0600，运行统计为只读的 0400；时间
 * 字段必须区分监控/水位的微秒与 quota 的毫秒，大小 quota 和统计大小以字节
 * 计。宏参数必须是可形成字段访问表达式的对象，展开点还决定最终参数前缀。
 */
/*
 * Common Code for DAMON Modules
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/*
 * DAMON 内核模块共享的声明与模块参数生成宏。
 * 作者信息保持上游原文；这些宏统一用户可见 ABI 名称，避免两个模块对相同
 * DAMON 字段使用不同单位或访问权限。
 */

#include <linux/moduleparam.h>

/*
 * DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS() - 导出基础监控精度/开销参数。
 *
 * 业务背景：模块用一个 struct damon_attrs 保存待提交配置，本宏把其中四个
 * 用户可调字段生成 sample_interval、aggr_interval、min_nr_regions 和
 * max_nr_regions 模块参数；前两者单位为微秒，后两者是自适应区域数量边界。
 * 入参：attrs 是当前翻译单元内长期存活、可取字段地址的对象表达式；宏不取得
 * 所有权。展开结果直接引用 attrs 的相应字段。
 * 出参/返回：无运行时返回值；生成四个 ulong、0600 参数描述，sysfs 写入会
 * 更新字段，但不会自动调用 damon_set_attrs() 或唤醒 kdamond。
 * 注意事项：MODULE_PARAM_PREFIX 在展开点决定名称前缀；调用模块必须自行校验
 * 采样/聚合关系和 min<=max，并通过 commit_inputs 明确提交运行期变更。
 */
#define DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(attrs)			\
	module_param_named(sample_interval, attrs.sample_interval,	\
			ulong, 0600);					\
	module_param_named(aggr_interval, attrs.aggr_interval, ulong,	\
			0600);						\
	module_param_named(min_nr_regions, attrs.min_nr_regions, ulong,	\
			0600);						\
	module_param_named(max_nr_regions, attrs.max_nr_regions, ulong,	\
			0600);

/*
 * DEFINE_DAMON_MODULES_DAMOS_TIME_QUOTA() - 导出 DAMOS 时间配额控制面。
 *
 * 业务背景：DAMOS 每个重置周期最多花费 quota.ms 毫秒执行动作；该宏生成
 * quota_ms 和 quota_reset_interval_ms 两个可写参数，使模块共享同一命名。
 * 入参：quota 是长期存活的 struct damos_quota 对象表达式，字段单位均为毫秒；
 * 宏只借用字段地址，不复制对象或改变 ownership。
 * 出参/返回：无运行时返回值；生成两个 ulong、0600 参数。写入只改变待提交
 * 字段，不重置当前周期已消费量，也不直接重配正在执行的 scheme。
 * 注意事项：时间单位不同于 damon_attrs 与 watermarks 的微秒；模块必须在
 * 参数应用事务中校验并构造/更新 scheme，避免读者把 sysfs 写入当成即时提交。
 */
#define DEFINE_DAMON_MODULES_DAMOS_TIME_QUOTA(quota)			\
	module_param_named(quota_ms, quota.ms, ulong, 0600);		\
	module_param_named(quota_reset_interval_ms,			\
			quota.reset_interval, ulong, 0600);

/*
 * DEFINE_DAMON_MODULES_DAMOS_QUOTAS() - 同时导出时间上限与字节上限。
 *
 * 业务背景：DAMON_RECLAIM 同时按执行时间和处理数据量限流；本宏先复用时间
 * quota 宏，再增加 quota_sz，使 DAMON core 可取两个上限中更严格的有效配额。
 * 入参：quota 是长期存活的 struct damos_quota 对象表达式；quota.sz 以字节
 * 为单位，所有权不变。
 * 出参/返回：无运行时返回值；总计生成 quota_ms、quota_reset_interval_ms 与
 * quota_sz 三个 ulong、0600 参数。
 * 注意事项：该宏会嵌套展开前一宏，调用点不可再单独展开时间宏，否则会生成
 * 重名模块参数；零值的具体“无限制”语义由 DAMOS quota 核心解释。
 */
#define DEFINE_DAMON_MODULES_DAMOS_QUOTAS(quota)			\
	DEFINE_DAMON_MODULES_DAMOS_TIME_QUOTA(quota)			\
	module_param_named(quota_sz, quota.sz, ulong, 0600);

/*
 * DEFINE_DAMON_MODULES_WMARKS_PARAMS() - 导出 scheme 激活水位参数。
 *
 * 业务背景：reclaim/lru_sort 周期性读取系统指标，仅在中水位区间执行动作；
 * 本宏生成检查间隔以及 high/mid/low 三个阈值参数。interval 单位为微秒；当
 * metric 为 FREE_MEM_RATE 时三个阈值采用千分比 0..1000。
 * 入参：wmarks 是长期存活的 struct damos_watermarks 对象表达式；宏借用四个
 * 字段地址，不导出 metric，也不转移对象所有权。
 * 出参/返回：无运行时返回值；生成四个 ulong、0600 参数，写入只更新模块的
 * 待提交配置。
 * 注意事项：调用模块必须校验 high、mid、low 的合法顺序并显式提交；水位会
 * 控制 scheme 是否活跃，但不会替代 quota 对单次动作量的限制。
 */
#define DEFINE_DAMON_MODULES_WMARKS_PARAMS(wmarks)			\
	module_param_named(wmarks_interval, wmarks.interval, ulong,	\
			0600);						\
	module_param_named(wmarks_high, wmarks.high, ulong, 0600);	\
	module_param_named(wmarks_mid, wmarks.mid, ulong, 0600);	\
	module_param_named(wmarks_low, wmarks.low, ulong, 0600);

/*
 * DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS() - 以调用点指定名称导出动作统计。
 *
 * 业务背景：不同模块/冷热 scheme 需要区分统计参数名，但都读取 damos_stat 的
 * nr_tried、sz_tried、nr_applied、sz_applied 和 qt_exceeds；token 拼接让一个
 * 宏覆盖 reclaim、hot-sort 与 cold-sort 三组 ABI。
 * 入参：stat 是由 DAMOS 更新且长期存活的 struct damos_stat 对象；try_name、
 * succ_name、qt_exceed_name 是用于拼接参数名的标识符，不是字符串或运行时值。
 * 出参/返回：无运行时返回值；生成五个 ulong、0400 只读参数。nr_* 表示区域
 * 次数，bytes_* 表示区域累计字节，quota exceed 表示配额耗尽次数。
 * 注意事项：0400 阻止用户写统计，但读值仍是运行中的瞬时累计快照；宏不导出
 * sz_ops_filter_passed 或 nr_snapshots，若需要这些指标必须另设明确 ABI。
 */
#define DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(stat, try_name,		\
		succ_name, qt_exceed_name)				\
	module_param_named(nr_##try_name, stat.nr_tried, ulong, 0400);	\
	module_param_named(bytes_##try_name, stat.sz_tried, ulong,	\
			0400);						\
	module_param_named(nr_##succ_name, stat.nr_applied, ulong,	\
			0400);						\
	module_param_named(bytes_##succ_name, stat.sz_applied, ulong,	\
			0400);						\
	module_param_named(nr_##qt_exceed_name, stat.qt_exceeds, ulong,	\
			0400);

/*
 * damon_modules_new_paddr_ctx_target() - 构造共享的物理地址监控空骨架。
 *
 * 业务背景：reclaim/lru_sort 的初始化和参数试算路径通过本函数一次取得已经
 * 选择 DAMON_OPS_PADDR 的 context 及其唯一 target；具体实现位于
 * modules-common.c，成功后调用者继续填充 attrs、region 和 scheme。
 * 入参：ctxp、targetp 是不可为 NULL 的输出槽；失败时保持原值。成功后 *ctxp
 * 持有整体销毁责任，*targetp 只是已归 context 管理的 target 借用入口。
 * 出参/返回：0 表示两输出均有效；-ENOMEM 表示对象分配失败；-EINVAL 表示
 * PADDR operations 不可选择。失败路径不会泄漏半构造对象。
 * 注意事项：调用可能分配内存并取得 operations mutex，要求可睡眠上下文；
 * 最终只调用 damon_destroy_ctx(*ctxp)，不可再单独销毁 *targetp。
 */
int damon_modules_new_paddr_ctx_target(struct damon_ctx **ctxp,
		struct damon_target **targetp);
