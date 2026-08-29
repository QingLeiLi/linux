// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/page-isolation.h>

/*
 * _debug_guardpage_minorder 保存启动参数指定的 order 阈值：拆分出的 buddy 块仅在
 * order 小于该值时被保留为 guard，而不进入 freelist。0 表示关闭 guard page；
 * 启动期写入后只读热路径查询，单位是 buddy order（块含 2^order 页）。
 */
unsigned int _debug_guardpage_minorder;

/*
 * _debug_pagealloc_enabled_early 是 static key 初始化前的策略真值：默认来自 Kconfig，
 * 还可被 debug_pagealloc= 覆盖。__read_mostly 把启动后几乎只读的变量放到适合
 * 缓存布局的段；导出符号供体系结构/模块查询，但所有权始终属于内存初始化代码。
 */
bool _debug_pagealloc_enabled_early __read_mostly
			= IS_ENABLED(CONFIG_DEBUG_PAGEALLOC_ENABLE_DEFAULT);
EXPORT_SYMBOL(_debug_pagealloc_enabled_early);
/*
 * _debug_pagealloc_enabled 是 mem_debugging_and_hardening_init() 后供分配/释放热路径
 * 使用的 jump-label 键；默认 false，确认运行期开启后一次性 enable，避免每页操作
 * 反复读取普通布尔变量。导出只允许使用者查询分支，不转移状态所有权。
 */
DEFINE_STATIC_KEY_FALSE(_debug_pagealloc_enabled);
EXPORT_SYMBOL(_debug_pagealloc_enabled);

/*
 * _debug_guardpage_enabled 是 guard page 专用热路径键；只有 debug pagealloc 已启用
 * 且 minorder 非零时才由 mm_init 打开，page allocator 据此跳过全部 guard 逻辑。
 */
DEFINE_STATIC_KEY_FALSE(_debug_guardpage_enabled);

/*
 * early_debug_pagealloc() - 解析 debug_pagealloc= 启动参数的布尔值。
 * 业务背景：early_param 在 static key 初始化前调用它，用命令行覆盖 Kconfig 默认值；
 * 稍后的 mem_debugging_and_hardening_init() 才把最终策略提交到 jump label。
 * 入参：buf 是启动参数值的 NUL 结尾借用字符串，不可为 NULL；生命周期仅限解析。
 * 出参/返回：kstrtobool() 成功返回 0 并更新全局 early 布尔值；格式非法返回负 errno，
 * 不产生对象 ownership 变化。
 * 注意事项：__init 函数仅在启动期存在，不取锁且在单线程早期解析阶段调用；这里
 * 尚不能直接启用 static key，否则会越过 jump-label 初始化顺序。
 */
static int __init early_debug_pagealloc(char *buf)
{
	return kstrtobool(buf, &_debug_pagealloc_enabled_early);
}

/* 把命令行键 debug_pagealloc 绑定到上述早期解析器，值如 on/off、1/0。 */
early_param("debug_pagealloc", early_debug_pagealloc);

/*
 * debug_guardpage_minorder_setup() - 校验并保存 guard page 的 order 阈值。
 * 业务背景：用户用早期启动参数提高被故意留空且受保护的 buddy 块比例，以增加
 * 捕获随机越界访问的机会；值越大，可用内存损失也越大。
 * 入参：buf 是十进制、NUL 结尾的借用字符串，不可为 NULL。
 * 出参/返回：合法时写 _debug_guardpage_minorder、记录日志并返回 0；解析失败或值
 * 大于 MAX_PAGE_ORDER/2 时也返回 0 让启动继续，但打印错误且保留旧值。
 * 注意事项：仅启动期、无并发且可调用解析/日志 helper；0 关闭 guard page，允许
 * 范围是 0..MAX_PAGE_ORDER/2，单位为 buddy order 而不是页数。
 */
static int __init debug_guardpage_minorder_setup(char *buf)
{
	/* res 暂存十进制解析结果；只有完整校验通过后才提交到全局策略。 */
	unsigned long res;

	/* 阶段 1：同时拒绝非数字/溢出输入和会保留过多内存的越界 order。 */
	if (kstrtoul(buf, 10, &res) < 0 ||  res > MAX_PAGE_ORDER / 2) {
		pr_err("Bad debug_guardpage_minorder value: %s\n", buf);
		return 0;
	}
	/* 阶段 2：提交已验证阈值；static key 稍后根据非零值决定是否启用。 */
	_debug_guardpage_minorder = res;
	pr_info("Setting debug_guardpage_minorder to %lu\n", res);
	return 0;
}

/* 参数必须在 buddy allocator 正式运行前确定，故使用 early_param。 */
early_param("debug_guardpage_minorder", debug_guardpage_minorder_setup);

/*
 * __set_page_guard() - 把一次 buddy 拆分产生的低阶空闲块标记为 guard。
 * 业务背景：page_alloc.c 的 expand()/break_down_buddy_pages() 在准备把剩余块加入
 * freelist 前调用；命中策略时以不可分配的洞捕获跨块访问，未来可随 buddy 合并回收。
 * 入参：zone 是调用者已锁定的所属 zone 借用指针，本实现无需直接访问；page 是块头
 * 的输入输出借用指针；order 是块阶数，表示 2^order 个页。
 * 出参/返回：order >= minorder 返回 false 且不改 page，调用者继续加入 freelist；
 * 否则返回 true，设置 PageGuard、初始化 buddy_list 并把 order 写入 page_private。
 * 注意事项：仅在 debug_guardpage static key 已启用且持有 zone->lock 的 buddy 路径
 * 调用；函数不睡眠、不取得 page 引用。true 表示块被保留，调用者不得再发布为空闲块。
 */
bool __set_page_guard(struct zone *zone, struct page *page, unsigned int order)
{
	/* 达到阈值的较大块仍供正常分配，只隔离更低 order 的拆分余块。 */
	if (order >= debug_guardpage_minorder())
		return false;

	/* PageGuard 与 private(order) 共同让后续 buddy 合并识别并恢复这块内存。 */
	__SetPageGuard(page);
	INIT_LIST_HEAD(&page->buddy_list);
	set_page_private(page, order);

	return true;
}

/*
 * __clear_page_guard() - 在相邻 buddy 释放并合并时撤销 guard 标记。
 * 业务背景：__free_one_page() 发现 page_is_guard(buddy) 后不从 freelist 删除它，
 * 而调用本函数把 guard 块恢复为可参与更高 order 合并的普通页块。
 * 入参：zone 是已持锁的所属 zone 借用指针，page 是 guard 块头的输入输出借用指针；
 * order 是当前合并阶数；后二者均由 buddy 查找确认，本实现只需修改 page 元数据。
 * 出参/返回：无直接返回值；清除 PageGuard 并把 page_private 从保存的 order 归零，
 * 不释放引用，也不自行把块加入 freelist。
 * 注意事项：调用者必须持 zone->lock 并确认 page_is_guard(page)；函数不睡眠，返回后
 * 上层立即继续合并，不能把清标记后的块当作已经独立发布的空闲块。
 */
void __clear_page_guard(struct zone *zone, struct page *page, unsigned int order)
{
	/* 元数据恢复必须先于上层把合并后的更大块重新发布到 buddy freelist。 */
	__ClearPageGuard(page);
	set_page_private(page, 0);
}
