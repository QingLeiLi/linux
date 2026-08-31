// SPDX-License-Identifier: GPL-2.0
#include <linux/fault-inject.h>
#include <linux/debugfs.h>
#include <linux/error-injection.h>
#include <linux/mm.h>

/*
 * 页分配故障注入的全局策略对象。attr 保存概率、间隔、次数、任务/栈过滤等通用
 * 状态；三个附加字段在进入通用随机判定前排除高端页、可直接回收请求和低阶请求。
 * 启动参数及仅 root 可写的 debugfs 节点会修改它，分配热路径只读取；测试环境
 * 应在稳定配置后运行，不能把这些普通字段当作支持无锁事务更新的生产策略。
 */
static struct {
	struct fault_attr attr;

	bool ignore_gfp_highmem;
	bool ignore_gfp_reclaim;
	u32 min_order;
} fail_page_alloc = {
	/* 默认只注入 order>=1、非 HIGHMEM、不可直接回收的可失败请求。 */
	.attr = FAULT_ATTR_INITIALIZER,
	.ignore_gfp_reclaim = true,
	.ignore_gfp_highmem = true,
	.min_order = 1,
};

/*
 * setup_fail_page_alloc() - 解析 fail_page_alloc= 启动参数的通用故障属性。
 * 业务背景：早期命令行扫描经 __setup 注册调用，为 debugfs 出现前预置注入策略。
 * 入参：str 是内核命令行缓冲区中可修改、NUL 结尾的参数值，调用期间借用。
 * 出参/返回：setup_fault_attr 成功解析四字段时返回 1 表示参数已处理，失败返回 0；
 * 直接更新 fail_page_alloc.attr，不取得字符串所有权。
 * 注意事项：仅 __init 串行执行，可使用初始化代码；附加的 ignore/min_order 仍保留默认值。
 */
static int __init setup_fail_page_alloc(char *str)
{
	return setup_fault_attr(&fail_page_alloc.attr, str);
}
__setup("fail_page_alloc=", setup_fail_page_alloc);

/*
 * should_fail_alloc_page() - 判断本次伙伴页分配是否应被测试性地强制失败。
 * 业务背景：prepare_alloc_pages() 在真正扫描 zonelist 前调用，以可控失败覆盖 OOM
 * 回滚；先保护不可失败或易递归的分配，再由 should_fail_ex() 执行概率/次数过滤。
 * 入参：gfp_mask 是本次分配语义位图，纯输入；order 是 2^order 个连续页的阶数。
 * 出参/返回：true 表示调用者应立即模拟分配失败，false 表示继续正常分配；不分配
 * page、不转移引用，只有通用 fault_attr 计数、随机状态和可选日志发生副作用。
 * 注意事项：调用点已排除 ALLOC_TRYLOCK，因为通用判定可能取自旋锁/打印；
 * __GFP_NOFAIL 永不注入，过滤分支不消耗一次 should_fail_ex 机会。
 */
bool should_fail_alloc_page(gfp_t gfp_mask, unsigned int order)
{
	/* flags 只把调用者的静默要求传给通用注入器，不是 GFP 标志副本。 */
	int flags = 0;

	/* 阶数、不可失败、HIGHMEM 与直接回收过滤均为无副作用快速放行路径。 */
	if (order < fail_page_alloc.min_order)
		return false;
	if (gfp_mask & __GFP_NOFAIL)
		return false;
	if (fail_page_alloc.ignore_gfp_highmem && (gfp_mask & __GFP_HIGHMEM))
		return false;
	if (fail_page_alloc.ignore_gfp_reclaim &&
			(gfp_mask & __GFP_DIRECT_RECLAIM))
		return false;

	/* See comment in __should_failslab() */
	/*
	 * 与 slab 注入相同，__GFP_NOWARN 有时要求完全不打印以避免递归/死锁；
	 * FAULT_NOWARN 把这一契约传给 should_fail_ex，而非只隐藏普通分配告警。
	 */
	if (gfp_mask & __GFP_NOWARN)
		flags |= FAULT_NOWARN;

	/* size 以页数计量，让通用 space 过滤按本次连续分配规模递减。 */
	return should_fail_ex(&fail_page_alloc.attr, 1 << order, flags);
}
ALLOW_ERROR_INJECTION(should_fail_alloc_page, TRUE);

#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS

/*
 * fail_page_alloc_debugfs() - 建立运行期页分配注入控制目录和专用过滤文件。
 * 业务背景：late_initcall 在 debugfs 核心就绪后调用，先创建通用 attr 文件，再暴露
 * 本模块的 ignore-gfp-* 与 min-order，供 root 测试无需重启即可调参。
 * 入参：无。出参/返回：始终返回 0 让启动继续；成功时 debugfs 持有指向全局策略
 * 字段的借用 data 指针，文件随 debugfs 生命周期存在，无需模块卸载清理。
 * 注意事项：仅在 CONFIG_FAULT_INJECTION_DEBUG_FS 编译；本实现不传播单个 debugfs
 * 创建失败，目录/文件可能部分存在，权限 0600 限制普通用户修改但不提供配置快照。
 */
static int __init fail_page_alloc_debugfs(void)
{
	/* mode 是普通文件加仅属主读写；dir 保存新建目录的借用 dentry。 */
	umode_t mode = S_IFREG | 0600;
	struct dentry *dir;

	/* 先建立 probability/interval/times 等公共控制，再挂接三个页分配专用字段。 */
	dir = fault_create_debugfs_attr("fail_page_alloc", NULL,
					&fail_page_alloc.attr);

	debugfs_create_bool("ignore-gfp-wait", mode, dir,
			    &fail_page_alloc.ignore_gfp_reclaim);
	debugfs_create_bool("ignore-gfp-highmem", mode, dir,
			    &fail_page_alloc.ignore_gfp_highmem);
	debugfs_create_u32("min-order", mode, dir, &fail_page_alloc.min_order);

	return 0;
}

late_initcall(fail_page_alloc_debugfs);

/* 关闭该配置时整个运行期控制面不存在，只能使用启动参数和编译时默认值。 */
#endif /* CONFIG_FAULT_INJECTION_DEBUG_FS */
