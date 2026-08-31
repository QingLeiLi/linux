// SPDX-License-Identifier: GPL-2.0
#include <linux/fault-inject.h>
#include <linux/error-injection.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include "slab.h"

/*
 * slab 对象分配故障注入的全局策略。attr 由通用框架维护概率、次数与过滤器；
 * ignore_gfp_reclaim 默认保护可能进入回收/递归路径的分配，cache_filter 可要求
 * 只有带 SLAB_FAILSLAB 的 cache 才参与。debugfs/启动参数写，SLUB 热路径读取。
 */
static struct {
	struct fault_attr attr;
	bool ignore_gfp_reclaim;
	bool cache_filter;
} failslab = {
	/* 默认允许所有 cache，但跳过带直接回收语义的请求。 */
	.attr = FAULT_ATTR_INITIALIZER,
	.ignore_gfp_reclaim = true,
	.cache_filter = false,
};

/*
 * should_failslab() - 判断一次 slab 对象分配是否应返回 -ENOMEM。
 * 业务背景：slab_pre_alloc_hook() 在进入 SLUB 分配器前调用，用受控失败验证上层
 * cleanup；本函数先保护 bootstrap/不可失败/回收相关请求，再调用通用注入器。
 * 入参：s 是调用期间借用且非 NULL 的 kmem_cache；gfpflags 是本次分配语义位图。
 * 出参/返回：注入时返回 -ENOMEM，否则返回 0；不分配或释放对象、不改变 cache
 * ownership，但 should_fail_ex 可能更新 attr 计数/随机状态并按配置打印。
 * 注意事项：可在分配热路径调用，不能递归依赖 slab；过滤字段的并发 debugfs 更新
 * 只提供即时策略而非原子快照。__GFP_NOFAIL 请求绝不能被人为打破。
 */
int should_failslab(struct kmem_cache *s, gfp_t gfpflags)
{
	/* flags 是传给 fault-inject 核心的控制位，object_size 是后续 space 计量单位。 */
	int flags = 0;

	/* No fault-injection for bootstrap cache */
	/*
	 * kmem_cache 是创建其他 cache 所依赖的 bootstrap cache；让它失败可能使注入器
	 * 自身或 slab 初始化递归崩溃，因此即使概率命中也必须放行。
	 */
	if (unlikely(s == kmem_cache))
		return 0;

	/* 不可失败和允许直接回收的请求默认跳过，避免破坏承诺或在回收中递归注入。 */
	if (gfpflags & __GFP_NOFAIL)
		return 0;

	if (failslab.ignore_gfp_reclaim &&
			(gfpflags & __GFP_DIRECT_RECLAIM))
		return 0;

	/* cache-filter 打开时由创建者用 SLAB_FAILSLAB 显式选择可注入的对象类型。 */
	if (failslab.cache_filter && !(s->flags & SLAB_FAILSLAB))
		return 0;

	/*
	 * In some cases, it expects to specify __GFP_NOWARN
	 * to avoid printing any information(not just a warning),
	 * thus avoiding deadlocks. See commit 6b9dbedbe349 for
	 * details.
	 */
	/*
	 * 某些 __GFP_NOWARN 调用要求不输出任何注入信息，而不只是隐藏分配告警；
	 * 否则 printk 自身分配/锁路径可能与当前失败路径形成递归死锁。
	 */
	if (gfpflags & __GFP_NOWARN)
		flags |= FAULT_NOWARN;

	/* 按对象字节数消耗通用 space 配额，命中时把布尔决策转成 slab 约定 errno。 */
	return should_fail_ex(&failslab.attr, s->object_size, flags) ? -ENOMEM : 0;
}
ALLOW_ERROR_INJECTION(should_failslab, ERRNO);

/*
 * setup_failslab() - 解析 failslab= 启动参数并初始化通用故障属性。
 * 业务背景：__setup 命令行扫描在 slab 测试开始前调用。入参：str 是借用的可修改
 * NUL 结尾参数串。出参/返回：解析成功返回 1，失败返回 0，符合 __setup 约定；
 * 直接更新全局 attr，不持有字符串。注意事项：仅 __init 串行执行，专用过滤保持默认。
 */
static int __init setup_failslab(char *str)
{
	return setup_fault_attr(&failslab.attr, str);
}
__setup("failslab=", setup_failslab);

#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS
/*
 * failslab_debugfs_init() - 创建运行期 slab 注入的通用及 cache 专用控制文件。
 * 业务背景：late_initcall 在 debugfs 可用后发布 failslab/ 目录，供 root 调整策略。
 * 入参：无。出参/返回：成功返回 0；目录创建的错误指针转换为负 errno，后续两个
 * 文件创建失败不单独传播；debugfs 借用全局字段直到系统结束。
 * 注意事项：仅 DEBUG_FS 配置编译，权限 0600；失败只影响控制面，不回滚已创建项，
 * 也不改变编译进热路径的默认注入策略。
 */
static int __init failslab_debugfs_init(void)
{
	/* dir 是新目录 dentry；mode 将所有控制限制为属主读写的普通文件。 */
	struct dentry *dir;
	umode_t mode = S_IFREG | 0600;

	/* 公共 helper 先创建概率/次数等文件；错误时不能把 ERR_PTR 传给子项创建。 */
	dir = fault_create_debugfs_attr("failslab", NULL, &failslab.attr);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	/* 成功后发布本模块的回收过滤和 cache opt-in 开关。 */
	debugfs_create_bool("ignore-gfp-wait", mode, dir,
			    &failslab.ignore_gfp_reclaim);
	debugfs_create_bool("cache-filter", mode, dir,
			    &failslab.cache_filter);

	return 0;
}

late_initcall(failslab_debugfs_init);

/* 关闭该配置时没有运行期目录，只保留启动参数和编译时默认策略。 */
#endif /* CONFIG_FAULT_INJECTION_DEBUG_FS */
