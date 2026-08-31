// SPDX-License-Identifier: GPL-2.0-only
/* Inject a hwpoison memory failure on a arbitrary pfn */
/*
 * 通过任意 PFN 注入软件模拟的 hwpoison 内存故障；原句中的“a arbitrary”意为
 * “an arbitrary”。debugfs 过滤器用于把破坏范围限制在测试目标内。
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/hugetlb.h>
#include <linux/page-flags.h>
#include <linux/memcontrol.h>
#include "internal.h"

/* 总开关为 0 时绕过全部早期过滤；非零时依次应用设备、flags 和 memcg 条件。 */
static u32 hwpoison_filter_enable;
/* major/minor 的 ~0U 是各自独立通配符，可只约束其中一个设备号分量。 */
static u32 hwpoison_filter_dev_major = ~0U;
static u32 hwpoison_filter_dev_minor = ~0U;
/* 只有 (stable_page_flags & mask) == value 才匹配；mask=0 表示不启用 flags 过滤。 */
static u64 hwpoison_filter_flags_mask;
static u64 hwpoison_filter_flags_value;

/*
 * hwpoison_filter_dev() - 检查页面是否属于指定块设备承载的文件系统。
 * 业务背景：注入测试可用 major/minor 把目标限制到某个 backing device，避免误伤
 * 其他文件映射或匿名页。
 * 入参：@p 是调用期间有效的 page 借用指针，不可为 NULL；函数不取得页引用。
 * 出参/返回：未配置设备条件或设备号匹配返回 0；无文件 mapping/host 或任一已配置
 * 分量不匹配返回 -EINVAL，无状态副作用。
 * 注意事项：mapping 是瞬时观察，最终 memory_failure() 会在自身同步范围内重做
 * 注册的组合过滤；这里的结果仅用于尽早缩小测试范围。
 */
static int hwpoison_filter_dev(struct page *p)
{
	/* folio 统一处理复合页；mapping/dev 都是当前检查窗口内的借用快照。 */
	struct folio *folio = page_folio(p);
	struct address_space *mapping;
	dev_t dev;

	/* 两个分量都是通配符时，不要求页面具备文件 backing。 */
	if (hwpoison_filter_dev_major == ~0U &&
	    hwpoison_filter_dev_minor == ~0U)
		return 0;

	/* 启用设备过滤后，匿名页或缺少 inode host 的 mapping 无法匹配。 */
	mapping = folio_mapping(folio);
	if (mapping == NULL || mapping->host == NULL)
		return -EINVAL;

	/* s_dev 编码 superblock 的块设备号，分别对非通配的 major/minor 做匹配。 */
	dev = mapping->host->i_sb->s_dev;
	if (hwpoison_filter_dev_major != ~0U &&
	    hwpoison_filter_dev_major != MAJOR(dev))
		return -EINVAL;
	if (hwpoison_filter_dev_minor != ~0U &&
	    hwpoison_filter_dev_minor != MINOR(dev))
		return -EINVAL;

	return 0;
}

/*
 * hwpoison_filter_flags() - 按 /proc/kpageflags 语义匹配稳定页标志。
 * 业务背景：压力测试借助 mask/value 选择特定类型页面，如 LRU、dirty 或 huge。
 * 入参：@p 是有效 page 借用指针，不取得引用。
 * 出参/返回：mask=0 或掩码后的稳定 flags 等于 value 返回 0，否则返回 -EINVAL；
 * 不修改页面。
 * 注意事项：value 中 mask 之外的非零位永远无法匹配；该快照仍可能在后续阶段变化。
 */
static int hwpoison_filter_flags(struct page *p)
{
	if (!hwpoison_filter_flags_mask)
		return 0;

	if ((stable_page_flags(p) & hwpoison_filter_flags_mask) ==
				    hwpoison_filter_flags_value)
		return 0;
	else
		return -EINVAL;
}

/*
 * This allows stress tests to limit test scope to a collection of tasks
 * by putting them under some memcg. This prevents killing unrelated/important
 * processes such as /sbin/init. Note that the target task may share clean
 * pages with init (eg. libc text), which is harmless. If the target task
 * share _dirty_ pages with another task B, the test scheme must make sure B
 * is also included in the memcg. At last, due to race conditions this filter
 * can only guarantee that the page either belongs to the memcg tasks, or is
 * a freed page.
 */
/*
 * 该机制让压力测试把一组任务放入同一 memcg，从而限制注入范围，避免杀死
 * /sbin/init 等无关关键进程。目标任务与 init 共享干净页（例如 libc text）不会
 * 带来脏数据传播问题；若它与任务 B 共享脏页，测试方案必须也把 B 纳入该 memcg。
 * 由于页面归属可并发变化，过滤器最终只能保证检查时页面属于目标 memcg，或者已
 * 经释放；它不是跨整个 memory_failure() 过程冻结 ownership 的安全边界。
 */
#ifdef CONFIG_MEMCG
/* 目标 memcg 的 kernfs inode 号；0 为通配，值由 corrupt-filter-memcg 写入。 */
static u64 hwpoison_filter_memcg;
/*
 * hwpoison_filter_task() - 按 memcg inode 限制页面所有者。
 * 业务背景：组合过滤的最后一层，保护目标 cgroup 之外的任务页面。
 * 入参：@p 是有效 page 借用指针，不取得 page/memcg 引用。
 * 出参/返回：未配置 inode 或 page_cgroup_ino() 相等返回 0，否则 -EINVAL；无副作用。
 * 注意事项：归属检查具有竞态，只适合作为测试保护带，不能提供强隔离保证。
 */
static int hwpoison_filter_task(struct page *p)
{
	if (!hwpoison_filter_memcg)
		return 0;

	if (page_cgroup_ino(p) != hwpoison_filter_memcg)
		return -EINVAL;

	return 0;
}
#else
/*
 * hwpoison_filter_task() - CONFIG_MEMCG=n 时的无条件匹配桩。
 * 业务背景：保持组合过滤器无条件调用结构，当前内核没有 memcg ownership 可筛选。
 * 入参：@p 为未使用的借用 page 指针。
 * 出参/返回：恒返回 0，无副作用、引用变化或睡眠。
 * 注意事项：此配置下 debugfs 不创建 corrupt-filter-memcg。
 */
static int hwpoison_filter_task(struct page *p) { return 0; }
#endif

/*
 * hwpoison_filter() - 以 AND 关系组合设备、flags 与 memcg 过滤条件。
 * 业务背景：既供本文件注入前预筛，也注册给 memory_failure() 在 folio 锁内复核。
 * 入参：@p 是当前检查窗口内有效的借用 page 指针，不可为 NULL。
 * 出参/返回：过滤关闭或全部条件匹配返回 0；任一条件拒绝统一返回 -EINVAL。
 * 注意事项：配置项由 debugfs 独立写入，组合读取不是事务快照；函数不睡眠且不持引用。
 */
static int hwpoison_filter(struct page *p)
{
	/* 总开关关闭时不读取任何子条件，注入直接遵循 memory_failure() 默认策略。 */
	if (!hwpoison_filter_enable)
		return 0;

	/* 短路顺序先排除 backing device，再检查 flags 与可选 memcg ownership。 */
	if (hwpoison_filter_dev(p))
		return -EINVAL;

	if (hwpoison_filter_flags(p))
		return -EINVAL;

	if (hwpoison_filter_task(p))
		return -EINVAL;

	return 0;
}

/* 模块创建的 debugfs 根目录；初始化发布，退出时递归摘除全部子项。 */
static struct dentry *hwpoison_dir;

/*
 * hwpoison_inject() - 处理向 corrupt-pfn 写入 PFN 的软件故障注入请求。
 *
 * 业务背景：DEFINE_DEBUGFS_ATTRIBUTE 把用户写入的 u64 交给本 setter；函数完成权限、
 * PFN 合法性、可选早期过滤后调用 memory_failure(MF_SW_SIMULATED)。
 * 入参：@data 为本属性未使用的私有指针，可为 NULL；@val 是用户提供的 PFN，转换
 * 为 unsigned long 后使用。
 * 出参/返回：无权限返回 -EPERM，无效 PFN 返回 -ENXIO；实际注入返回 memory_failure
 * 状态，但 -EOPNOTSUPP 被折叠为 0。早期过滤拒绝也返回 0，表示跳过而非注入成功。
 * 注意事项：进程上下文、可睡眠；软件模拟可能隔离页面、解除映射并向任务发 SIGBUS，
 * 因而仅允许 CAP_SYS_ADMIN。过滤是降低误伤概率而非绝对 ownership 隔离。
 */
static int hwpoison_inject(void *data, u64 val)
{
	/* pfn 是 debugfs 输入；p/folio 只在 memory_failure 接管前作为短期检查视图。 */
	unsigned long pfn = val;
	struct page *p;
	struct folio *folio;
	int err;

	/* 权限与 PFN 结构化有效性必须先于 pfn_to_page()。 */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!pfn_valid(pfn))
		return -ENXIO;

	p = pfn_to_page(pfn);
	folio = page_folio(p);

	/* 未启用过滤时跳过 shake/归属快照，由 memory_failure() 直接执行核心恢复。 */
	if (!hwpoison_filter_enable)
		goto inject;

	shake_folio(folio);
	/*
	 * This implies unable to support non-LRU pages except free page.
	 */
	/*
	 * 这意味着启用过滤时除空闲 buddy 页外不支持普通 non-LRU 页；hugetlb 作为
	 * 显式例外仍可继续。shake_folio() 先尝试把可回收状态整理到可识别形态。
	 */
	if (!folio_test_lru(folio) && !folio_test_hugetlb(folio) &&
	    !is_free_buddy_page(p))
		return 0;

	/*
	 * do a racy check to make sure PG_hwpoison will only be set for
	 * the targeted owner (or on a free page).
	 * memory_failure() will redo the check reliably inside page lock.
	 */
	/*
	 * 此处做有竞态的预检查，尽量确保 PG_hwpoison 只施加给目标 owner（或空闲页）；
	 * memory_failure() 随后会在 folio 锁内调用已注册过滤器可靠复核。预检查拒绝按
	 * “成功跳过”返回 0，便于压力测试扫描大量非目标 PFN。
	 */
	err = hwpoison_filter(&folio->page);
	if (err)
		return 0;

inject:
	/* MF_SW_SIMULATED 标明并非真实硬件故障，使后续仍允许软件 unpoison。 */
	pr_info("Injecting memory failure at pfn %#lx\n", pfn);
	err = memory_failure(pfn, MF_SW_SIMULATED);
	return (err == -EOPNOTSUPP) ? 0 : err;
}

/*
 * hwpoison_unpoison() - 处理向 unpoison-pfn 写入 PFN 的软件解毒请求。
 * 业务背景：测试结束后尝试撤销 Linux 软件注入的 PG_hwpoison 状态并让页重新可用。
 * 入参：@data 未使用且可为 NULL；@val 是待恢复 PFN。
 * 出参/返回：无权限返回 -EPERM，否则原样返回 unpoison_memory() 的 0 或负 errno。
 * 注意事项：仅对软件注入有效；一旦系统记录真实硬件故障，核心会拒绝该能力。
 */
static int hwpoison_unpoison(void *data, u64 val)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	return unpoison_memory(val);
}

/* 两个只写 debugfs 属性把十进制 PFN 解析为 u64，分别分派到注入与撤销 setter。 */
DEFINE_DEBUGFS_ATTRIBUTE(hwpoison_fops, NULL, hwpoison_inject, "%lli\n");
DEFINE_DEBUGFS_ATTRIBUTE(unpoison_fops, NULL, hwpoison_unpoison, "%lli\n");

/*
 * pfn_inject_exit() - 停止过滤回调并移除模块全部 debugfs 接口。
 * 业务背景：模块卸载必须先阻止新注入及 memory_failure() 对模块函数的并发调用。
 * 入参：无。
 * 出参/返回：无直接返回值；关闭过滤、注销 RCU 回调并递归删除目录。
 * 注意事项：hwpoison_filter_unregister() 内含 synchronize_rcu()，必须先于模块代码卸载；
 * 删除 debugfs 后已打开文件的 core 生命周期仍由 debugfs 自身协调。
 */
static void __exit pfn_inject_exit(void)
{
	/* 先让早期路径绕过配置，再等待所有核心 RCU 读者离开，最后撤销用户入口。 */
	hwpoison_filter_enable = 0;
	hwpoison_filter_unregister();
	debugfs_remove_recursive(hwpoison_dir);
}

/*
 * pfn_inject_init() - 创建 hwpoison debugfs 控制面并注册核心过滤回调。
 *
 * 业务背景：模块加载时在 debugfs 根下发布 poison/unpoison setter 与独立过滤参数，
 * 然后让 memory_failure() 可在页锁内调用同一组合过滤器。
 * 入参：无。
 * 出参/返回：恒返回 0；发布 debugfs 项并通过 RCU 发布过滤函数指针。
 * 注意事项：创建结果未逐项检查，因此 debugfs 不可用或个别节点失败时模块仍可加载；
 * 接口只模拟软件状态，不改变硬件错误寄存器，也不要求 MCE 硬件支持。
 */
static int __init pfn_inject_init(void)
{
	/* 根目录是全部文件的父 ownership 节点，退出时一次递归删除。 */
	hwpoison_dir = debugfs_create_dir("hwpoison", NULL);

	/*
	 * Note that the below poison/unpoison interfaces do not involve
	 * hardware status change, hence do not require hardware support.
	 * They are mainly for testing hwpoison in software level.
	 */
	/*
	 * 下列 poison/unpoison 接口不改变硬件状态，因此不要求硬件支持；它们主要用于
	 * 软件层验证 hwpoison 恢复、隔离与进程通知路径，ABI 也不承诺长期稳定。
	 */
	/* 第一组 0200 文件只允许写 PFN，并通过上方 attribute setter 执行动作。 */
	debugfs_create_file("corrupt-pfn", 0200, hwpoison_dir, NULL,
			    &hwpoison_fops);

	debugfs_create_file("unpoison-pfn", 0200, hwpoison_dir, NULL,
			    &unpoison_fops);

	/* 第二组 0600 文件直接读写过滤配置；各字段更新彼此独立、没有事务提交。 */
	debugfs_create_u32("corrupt-filter-enable", 0600, hwpoison_dir,
			   &hwpoison_filter_enable);

	debugfs_create_u32("corrupt-filter-dev-major", 0600, hwpoison_dir,
			   &hwpoison_filter_dev_major);

	debugfs_create_u32("corrupt-filter-dev-minor", 0600, hwpoison_dir,
			   &hwpoison_filter_dev_minor);

	/* flags 的 mask/value 也须由测试者按所需顺序设置；enable 通常最后开启。 */
	debugfs_create_u64("corrupt-filter-flags-mask", 0600, hwpoison_dir,
			   &hwpoison_filter_flags_mask);

	debugfs_create_u64("corrupt-filter-flags-value", 0600, hwpoison_dir,
			   &hwpoison_filter_flags_value);

#ifdef CONFIG_MEMCG
	/* 只有 memcg 可用时才公开 inode ownership 条件。 */
	debugfs_create_u64("corrupt-filter-memcg", 0600, hwpoison_dir,
			   &hwpoison_filter_memcg);
#endif

	/* 最后以 RCU 发布回调，确保核心读者看到它时全部配置存储和模块代码已就绪。 */
	hwpoison_filter_register(hwpoison_filter);

	return 0;
}

/* 模块加载发布测试入口，卸载先注销 RCU 回调再移除控制面。 */
module_init(pfn_inject_init);
module_exit(pfn_inject_exit);
MODULE_DESCRIPTION("HWPoison pages injector");
MODULE_LICENSE("GPL");
