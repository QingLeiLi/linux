// SPDX-License-Identifier: GPL-2.0
/* MM/section、memblock/vmalloc 和各客户端头共同定义扩展记录的分配与消费边界。 */
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/memblock.h>
#include <linux/page_ext.h>
#include <linux/memory.h>
/* 两种 backing 后端及 kmemleak 账本用于 SPARSEMEM section 的创建和释放。 */
#include <linux/vmalloc.h>
#include <linux/kmemleak.h>
#include <linux/page_owner.h>
#include <linux/page_idle.h>
/* 页表检查、RCU、分配标签和 IOMMU 调试是其余可选客户端及生命周期设施。 */
#include <linux/page_table_check.h>
#include <linux/rcupdate.h>
#include <linux/pgalloc_tag.h>
#include <linux/iommu-debug-pagealloc.h>

/*
 * 本文件把 page_owner、32 位 page-idle、分配标签、页表检查与 IOMMU 调试等
 * 可选客户端的数据拼成统一的每页扩展记录；page_ext 不改变 struct page ABI，
 * 并在启动或内存热插拔时按实际启用的客户端决定是否分配 backing。
 */

/*
 * struct page extension
 *
 * This is the feature to manage memory for extended data per page.
 *
 * Until now, we must modify struct page itself to store extra data per page.
 * This requires rebuilding the kernel and it is really time consuming process.
 * And, sometimes, rebuild is impossible due to third party module dependency.
 * At last, enlarging struct page could cause un-wanted system behaviour change.
 *
 * This feature is intended to overcome above mentioned problems. This feature
 * allocates memory for extended data per page in certain place rather than
 * the struct page itself. This memory can be accessed by the accessor
 * functions provided by this code. During the boot process, it checks whether
 * allocation of huge chunk of memory is needed or not. If not, it avoids
 * allocating memory at all. With this advantage, we can include this feature
 * into the kernel in default and can avoid rebuild and solve related problems.
 *
 * To help these things to work well, there are two callbacks for clients. One
 * is the need callback which is mandatory if user wants to avoid useless
 * memory allocation at boot-time. The other is optional, init callback, which
 * is used to do proper initialization after memory is allocated.
 *
 * The need callback is used to decide whether extended memory allocation is
 * needed or not. Sometimes users want to deactivate some features in this
 * boot and extra memory would be unnecessary. In this case, to avoid
 * allocating huge chunk of memory, each clients represent their need of
 * extra memory through the need callback. If one of the need callbacks
 * returns true, it means that someone needs extra memory so that
 * page extension core should allocates memory for page extension. If
 * none of need callbacks return true, memory isn't needed at all in this boot
 * and page extension core can skip to allocate memory. As result,
 * none of memory is wasted.
 *
 * When need callback returns true, page_ext checks if there is a request for
 * extra memory through size in struct page_ext_operations. If it is non-zero,
 * extra space is allocated for each page_ext entry and offset is returned to
 * user through offset in struct page_ext_operations.
 *
 * The init callback is used to do proper initialization after page extension
 * is completely initialized. In sparse memory system, extra memory is
 * allocated some time later than memmap is allocated. In other words, lifetime
 * of memory for page extension isn't same with memmap for struct page.
 * Therefore, clients can't store extra data until page extension is
 * initialized, even if pages are allocated and used freely. This could
 * cause inadequate state of extra data per page, so, to prevent it, client
 * can utilize this callback to initialize the state of it correctly.
 */
/*
 * page_ext 为每个 PFN 提供 struct page 之外的可选扩展数据，避免扩大核心页描述
 * 导致 ABI、内存占用和第三方模块重建问题。启动时逐个调用客户端 need：无人
 * 需要时完全不分配；需要共享 flags 的客户端先保留 struct page_ext，随后为
 * 各客户端按 size 排列私有区并回填 offset。全部 backing 就绪后才调用 init。
 *
 * SPARSEMEM 的 section backing 可晚于 memmap 出现，因此页已可分配不代表扩展
 * 已可用；读者必须在 RCU 内 lookup/get，热拔先标 invalid、等待 grace period，
 * 再释放。FLATMEM 则按 node 一次性分配，生命周期覆盖运行期。
 */

#ifdef CONFIG_SPARSEMEM
/* 低位哨兵只标记 section->page_ext 已失效；实际分配按对齐保证该位原本为零。 */
#define PAGE_EXT_INVALID       (0x1)
#endif

#if defined(CONFIG_PAGE_IDLE_FLAG) && !defined(CONFIG_64BIT)
/*
 * 业务背景：32 位 struct page 没有空余 flags 位时，page-idle 必须借 page_ext
 * 的共享 flags 保存 YOUNG/IDLE 状态。
 * 入参：无。
 * 出参/返回：恒 true，要求本次启动分配 page_ext；无其他副作用。
 * 注意事项：仅上述配置编译；启动期调用，不睡眠、无需锁。
 */
static bool need_page_idle(void)
{
	return true;
}

/* page_idle_ops 只借共享 flags，不占私有字节；对象仅在 __init 阶段使用。 */
static struct page_ext_operations page_idle_ops __initdata = {
	.need = need_page_idle,
	.need_shared_flags = true,
};
#endif

/*
 * page_ext_ops 是启动期客户端目录；条件编译决定参与者及布局顺序。每个指针
 * 借用客户端静态对象，invoke_need_callbacks() 写 offset，init 后数组可释放。
 */
static struct page_ext_operations *page_ext_ops[] __initdata = {
#ifdef CONFIG_PAGE_OWNER
	/* page_owner 保存分配/释放栈与状态，用 need 决定本次启动是否启用。 */
	&page_owner_ops,
#endif
#if defined(CONFIG_PAGE_IDLE_FLAG) && !defined(CONFIG_64BIT)
	/* 32 位 page-idle 复用 page_ext->flags，不追加私有区。 */
	&page_idle_ops,
#endif
#ifdef CONFIG_MEM_ALLOC_PROFILING
	/* allocation tagging 追加每页 tag 引用，并可能要求 early_page_ext。 */
	&page_alloc_tagging_ops,
#endif
#ifdef CONFIG_PAGE_TABLE_CHECK
	/* page-table-check 记录匿名/文件映射类别，验证非法重复映射。 */
	&page_table_check_ops,
#endif
#ifdef CONFIG_IOMMU_DEBUG_PAGEALLOC
	/* IOMMU debug pagealloc 保存物理页映射调试状态。 */
	&page_iommu_debug_ops,
#endif
};

/* 每个 PFN 对应记录的最终字节跨度；布局完成后只读，供 lookup/迭代做地址运算。 */
unsigned long page_ext_size;

/* 已分配 page_ext backing 的累计字节数，仅用于启动日志，初始化路径串行写入。 */
static unsigned long total_usage;

#ifdef CONFIG_MEM_ALLOC_PROFILING_DEBUG
/*
 * To ensure correct allocation tagging for pages, page_ext should be available
 * before the first page allocation. Otherwise early task stacks will be
 * allocated before page_ext initialization and missing tags will be flagged.
 */
/*
 * 为保证早期页也带正确分配标签，调试配置默认在第一次页分配前准备 page_ext；
 * 否则早期任务栈先于扩展初始化，会被误报为缺少 tag。
 */
bool early_page_ext __meminitdata = true;
#else
/* 普通配置默认允许较晚初始化，可由 early_page_ext 启动参数提前。 */
bool early_page_ext __meminitdata;
#endif
/*
 * 业务背景：解析 early_page_ext 启动参数，强制 MM 初始化选择早期扩展路径。
 * 入参：str 是参数尾串的借用指针，本无值参数不读取且允许为 NULL。
 * 出参/返回：返回 0 表示参数已接受；把全局 early_page_ext 单向置 true。
 * 注意事项：仅启动期单线程调用，不睡眠、无需锁，不能在运行期撤销。
 */
static int __init setup_early_page_ext(char *str)
{
	early_page_ext = true;
	return 0;
}
early_param("early_page_ext", setup_early_page_ext);

/*
 * 业务背景：汇总所有编译进内核且本次启动确实需要的客户端，构造每条记录
 * 的共享头和私有区布局；由 flat/sparse 初始化入口在分配前调用一次。
 * 入参：无，读取 page_ext_ops 静态目录并调用各 need 回调。
 * 出参/返回：至少一个客户端需要扩展时返回 true，并写 page_ext_size/offset；
 * 全部拒绝时返回 false、调用者跳过分配。
 * 注意事项：__init 串行执行，need 必须无破坏性且可重复调用；第二遍再次调用
 * 是当前协议的一部分。布局一旦用于分配就不可再改变。
 */
static bool __init invoke_need_callbacks(void)
{
	/* i/entries 遍历启动期目录；need 汇总第二遍实际参与者。 */
	int i;
	int entries = ARRAY_SIZE(page_ext_ops);
	bool need = false;

	/* 第一遍只判断是否有人需要共享 flags，并据此放入固定 page_ext 头。 */
	for (i = 0; i < entries; i++) {
		if (page_ext_ops[i]->need()) {
			if (page_ext_ops[i]->need_shared_flags) {
				page_ext_size = sizeof(struct page_ext);
				break;
			}
		}
	}

	/* 第二遍为所有需要者按数组顺序分配私有尾部，并发布各自 offset。 */
	for (i = 0; i < entries; i++) {
		if (page_ext_ops[i]->need()) {
			page_ext_ops[i]->offset = page_ext_size;
			page_ext_size += page_ext_ops[i]->size;
			need = true;
		}
	}

	return need;
}

/*
 * 业务背景：所有 page_ext backing 已可查后通知客户端初始化其全局/逐页状态。
 * 入参：无；从 page_ext_ops 借用回调表。
 * 出参/返回：无直接返回值；存在 init 的客户端依数组顺序被调用一次。
 * 注意事项：启动期可睡眠；回调无返回值，失败不能由核心回滚，且此时 offset
 * 与所有已存在内存的 backing 已稳定。
 */
static void __init invoke_init_callbacks(void)
{
	/* entries 固定目录长度，i 是回调游标。 */
	int i;
	int entries = ARRAY_SIZE(page_ext_ops);

	/* need 与 init 独立：客户端只要提供 init 就会在全局 backing 就绪后收到通知。 */
	for (i = 0; i < entries; i++) {
		if (page_ext_ops[i]->init)
			page_ext_ops[i]->init();
	}
}

/*
 * 业务背景：把某块 page_ext backing 的逻辑记录号换算成记录地址，统一封装
 * 可变 page_ext_size 的字节步进。
 * 入参：base 为借用的已分配基址；index 为零起始记录号，必须在容量内。
 * 出参/返回：返回借用的对应 page_ext 指针，不取得引用、不修改数据。
 * 注意事项：GNU C void* 字节算术；调用者必须以 RCU或启动期/热插拔协议保证
 * backing 生命周期，乘法范围由分配规模保证。
 */
static inline struct page_ext *get_entry(void *base, unsigned long index)
{
	return base + page_ext_size * index;
}

#ifndef CONFIG_SPARSEMEM
/*
 * 业务背景：FLATMEM 在较晚阶段统一通知客户端进入初始化；若 need 阶段有人
 * 启用扩展，此时 node backing 已齐备，否则回调也必须容忍没有 backing。
 * 入参：无。
 * 出参/返回：无直接返回值；执行全部客户端 init 回调。
 * 注意事项：仅 FLATMEM 启动路径调用一次，可睡眠；不再分配 backing，且当前
 * 实现不会按 need 结果过滤 init 回调。
 */
void __init page_ext_init_flatmem_late(void)
{
	invoke_init_callbacks();
}

/*
 * 业务背景：初始化新 pgdat 中的 FLATMEM page_ext 发布指针，防止分配前查找
 * 把未初始化内存误当基址；由 pgdat 初始化路径调用。
 * 入参：pgdat 为正在构造的非空节点描述，调用者拥有且尚未并发发布。
 * 出参/返回：无直接返回值；node_page_ext 被置 NULL，无 ownership 转移。
 * 注意事项：启动/热添加早期调用，不睡眠、无需锁。
 */
void __meminit pgdat_page_ext_init(struct pglist_data *pgdat)
{
	pgdat->node_page_ext = NULL;
}

/*
 * 业务背景：FLATMEM 客户端从 struct page 定位同节点连续扩展表中的对应记录。
 * 入参：page 为有效普通页的借用指针，调用者已持 RCU read lock。
 * 出参/返回：backing 已发布时返回 RCU 借用的 page_ext，否则返回 NULL。
 * 注意事项：不睡眠；RCU 约束由 WARN 检查但不自动获取。节点起点向下按 buddy
 * 最大块对齐，因此 index 可覆盖 buddy 检查越过精确 node 边界的页。
 */
static struct page_ext *lookup_page_ext(const struct page *page)
{
	/* pfn/index 是页帧与节点表偏移；base 是 RCU 保护的节点 backing 借用指针。 */
	unsigned long pfn = page_to_pfn(page);
	unsigned long index;
	struct page_ext *base;

	WARN_ON_ONCE(!rcu_read_lock_held());
	base = NODE_DATA(page_to_nid(page))->node_page_ext;
	/*
	 * The sanity checks the page allocator does upon freeing a
	 * page can reach here before the page_ext arrays are
	 * allocated when feeding a range of pages to the allocator
	 * for the first time during bootup or memory hotplug.
	 */
	/*
	 * 启动或热插拔首次把页交给 allocator 时，free 侧健全性检查可能早于扩展表
	 * 分配到达这里；NULL 是合法的暂态，调用者必须把它当作“无扩展信息”。
	 */
	if (unlikely(!base))
		return NULL;
	index = pfn - round_down(node_start_pfn(page_to_nid(page)),
					MAX_ORDER_NR_PAGES);
	return get_entry(base, index);
}

/*
 * 业务背景：为一个 FLATMEM node 的整个跨度分配连续 page_ext 表，供后续
 * page->PFN 查找 O(1) 定位；由 page_ext_init_flatmem() 逐在线节点调用。
 * 入参：nid 是有效在线 NUMA 节点号。
 * 出参/返回：0 表示空节点无需分配或表已发布；-ENOMEM 表示 memblock 分配失败。
 * 成功把 backing ownership 交给节点并累计 total_usage/memmap 启动页计数。
 * 注意事项：__init 串行且可使用 memblock；一旦发布不提供运行期释放。范围
 * 两端可能为 buddy 检查额外扩一最大阶块，不能只按 node_spanned_pages 分配。
 */
static int __init alloc_node_page_ext(int nid)
{
	/* base 是待发布表；table_size 为字节；nr_pages 为需覆盖的 PFN 记录数。 */
	struct page_ext *base;
	unsigned long table_size;
	unsigned long nr_pages;

	/* 没有跨度的 node 不需要 backing，成功返回且不更新统计。 */
	nr_pages = NODE_DATA(nid)->node_spanned_pages;
	if (!nr_pages)
		return 0;

	/*
	 * Need extra space if node range is not aligned with
	 * MAX_ORDER_NR_PAGES. When page allocator's buddy algorithm
	 * checks buddy's status, range could be out of exact node range.
	 */
	/*
	 * node 边界未按 MAX_ORDER_NR_PAGES 对齐时，buddy 合并检查可访问精确范围外
	 * 的伙伴页，所以多留一个最大阶块的记录，防止查找越界。
	 */
	if (!IS_ALIGNED(node_start_pfn(nid), MAX_ORDER_NR_PAGES) ||
		!IS_ALIGNED(node_end_pfn(nid), MAX_ORDER_NR_PAGES))
		nr_pages += MAX_ORDER_NR_PAGES;

	table_size = page_ext_size * nr_pages;

	/* 在可访问、低于 MAX_DMA_ADDRESS 的 memblock 中尽量按目标 node 分配。 */
	base = memblock_alloc_try_nid(
			table_size, PAGE_SIZE, __pa(MAX_DMA_ADDRESS),
			MEMBLOCK_ALLOC_ACCESSIBLE, nid);
	if (!base)
		return -ENOMEM;
	/* 发布基址后查找开始可见；启动串行使这里不需要额外 release 屏障。 */
	NODE_DATA(nid)->node_page_ext = base;
	total_usage += table_size;
	memmap_boot_pages_add(DIV_ROUND_UP(table_size, PAGE_SIZE));
	return 0;
}

/*
 * 业务背景：FLATMEM 早期总入口，先确定记录布局，再为每个在线 node 分配；
 * 后续 page_ext_init_flatmem_late() 才启动客户端。
 * 入参：无。
 * 出参/返回：无直接返回值；无人需要时无副作用，成功发布所有节点表；任一
 * 节点失败会 panic，避免只覆盖部分节点却让客户端误以为全局可用。
 * 注意事项：__init 可睡眠/使用 memblock，系统尚未并发运行；没有局部回滚，
 * 因为启动无法在缺失扩展不变量的情况下安全继续。
 */
void __init page_ext_init_flatmem(void)
{

	/* nid 遍历在线节点；fail 保存当前节点分配 errno。 */
	int nid, fail;

	/* need 全 false 是零内存快速路径，也不会执行客户端 init。 */
	if (!invoke_need_callbacks())
		return;

	/* 全量节点发布；中途失败直接转不可恢复启动错误。 */
	for_each_online_node(nid)  {
		fail = alloc_node_page_ext(nid);
		if (fail)
			goto fail;
	}
	pr_info("allocated %ld bytes of page_ext\n", total_usage);
	return;

fail:
	/* 已分配节点无需回滚：panic 终止启动，保留日志以定位失败规模。 */
	pr_crit("allocation of page_ext failed.\n");
	panic("Out of memory");
}

#else /* CONFIG_SPARSEMEM */
/*
 * 业务背景：判定 SPARSEMEM section 的 page_ext 指针是缺失还是热拔失效哨兵。
 * 入参：page_ext 是 READ_ONCE 得到的借用值，可为 NULL 或带 INVALID 低位。
 * 出参/返回：NULL/带哨兵返回 true，正常对齐基址返回 false；无副作用。
 * 注意事项：不睡眠、无需解引用；整数转换只检查编码位。
 */
static bool page_ext_invalid(struct page_ext *page_ext)
{
	return !page_ext || (((unsigned long)page_ext & PAGE_EXT_INVALID) == PAGE_EXT_INVALID);
}

/*
 * 业务背景：SPARSEMEM 按 section 从 page 定位扩展记录，并识别未分配或热拔
 * 已失效的 backing；是公开 lookup/get 的配置实现。
 * 入参：page 为有效普通页借用指针；调用者已持 RCU read lock。
 * 出参/返回：有效时返回只在当前 RCU 临界区可用的借用记录，否则 NULL。
 * 注意事项：READ_ONCE 与 invalid->synchronize_rcu->free 配对；RCU只保证内存
 * 生命周期，不冻结客户端字段，字段同步仍由各客户端负责。
 */
static struct page_ext *lookup_page_ext(const struct page *page)
{
	/* pfn/section 定位元数据；page_ext 是可能带 INVALID 位的单次快照。 */
	unsigned long pfn = page_to_pfn(page);
	struct mem_section *section = __pfn_to_section(pfn);
	struct page_ext *page_ext = READ_ONCE(section->page_ext);

	WARN_ON_ONCE(!rcu_read_lock_held());
	/*
	 * The sanity checks the page allocator does upon freeing a
	 * page can reach here before the page_ext arrays are
	 * allocated when feeding a range of pages to the allocator
	 * for the first time during bootup or memory hotplug.
	 */
	/* 启动/热添加早期允许暂时无 backing，free 侧检查应安全退化为 NULL。 */
	if (page_ext_invalid(page_ext))
		return NULL;
	return get_entry(page_ext, pfn);
}

/*
 * 业务背景：为一个 section 分配清零的 page_ext backing，优先物理连续页，
 * 内存碎片时退化为 vmalloc；由 init_section_page_ext() 调用。
 * 入参：size 为所需字节数且非零；nid 是首选 NUMA 节点。
 * 出参/返回：成功返回由调用者接管的清零基址并增加 memmap_pages 统计；失败
 * 返回 NULL、无待释放资源。
 * 注意事项：__meminit 可睡眠；exact-pages 分支手工登记 kmemleak，vmalloc
 * 自带跟踪；__GFP_NOWARN 抑制预期的连续分配回退噪声。
 */
static void *__meminit alloc_page_ext(size_t size, int nid)
{
	/* flags 固定清零/静默；addr 在 exact-pages 与 vzalloc 两后端间传递 ownership。 */
	gfp_t flags = GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN;
	void *addr = NULL;

	/* 快速路径取得物理连续 backing，成功后显式登记 kmemleak。 */
	addr = alloc_pages_exact_nid(nid, size, flags);
	if (addr)
		kmemleak_alloc(addr, size, 1, flags);
	else
		/* 慢速路径用节点感知 vmalloc，接受虚拟连续而物理离散。 */
		addr = vzalloc_node(size, nid);

	if (addr)
		memmap_pages_add(DIV_ROUND_UP(size, PAGE_SIZE));

	return addr;
}

/*
 * 业务背景：确保 PFN 所在 SPARSEMEM section 有一整段扩展表，并把基址编码成
 * “base - section_start_pfn*stride”，使 lookup 可直接用全局 PFN 索引。
 * 入参：pfn 可未按 section 对齐但须属于有效 section；nid 为分配首选节点。
 * 出参/返回：已有/新建成功返回 0；分配失败返回 -ENOMEM。成功发布 section
 * 指针并把 backing ownership 转交 section 生命周期。
 * 注意事项：由启动或热插拔串行化调用；发布前不允许普通查找使用新表。
 */
static int __meminit init_section_page_ext(unsigned long pfn, int nid)
{
	/* section/base/table_size 分别记录发布目标、真实分配基址与字节规模。 */
	struct mem_section *section;
	struct page_ext *base;
	unsigned long table_size;

	section = __pfn_to_section(pfn);

	/* 重叠 node 扫描或已在线 section 可能重复到达，现有表即成功。 */
	if (section->page_ext)
		return 0;

	table_size = page_ext_size * PAGES_PER_SECTION;
	base = alloc_page_ext(table_size, nid);

	/*
	 * The value stored in section->page_ext is (base - pfn)
	 * and it does not point to the memory block allocated above,
	 * causing kmemleak false positives.
	 */
	/*
	 * section 中保存的是带全局 PFN 偏移的伪基址，并非 allocator 返回值；禁止
	 * kmemleak 把真实 backing 判为失联，最终释放会先还原真实地址。
	 */
	kmemleak_not_leak(base);

	if (!base) {
		pr_err("page ext allocation failure\n");
		return -ENOMEM;
	}

	/*
	 * The passed "pfn" may not be aligned to SECTION.  For the calculation
	 * we need to apply a mask.
	 */
	/* 对齐到 section 首 PFN 后构造偏移基址；这是读者地址算术开始可用的发布点。 */
	pfn &= PAGE_SECTION_MASK;
	section->page_ext = (void *)base - page_ext_size * pfn;
	total_usage += table_size;
	return 0;
}

/*
 * 业务背景：释放一个 section 的真实 page_ext backing，并与两种分配后端及
 * memmap_pages 统计配对；仅由 __free_page_ext() 在 RCU 排空后调用。
 * 入参：addr 必须是 alloc_page_ext() 返回的真实非空基址，ownership 转入本函数。
 * 出参/返回：无直接返回值；vmalloc 或 exact-pages backing 被最终释放。
 * 注意事项：可睡眠；exact-pages 不应带 PageReserved，BUG 表示生命周期错误。
 */
static void free_page_ext(void *addr)
{
	/* table_size 可由固定 section 容量重算；page 仅服务 exact-pages 健全性检查。 */
	size_t table_size;
	struct page *page;

	table_size = page_ext_size * PAGES_PER_SECTION;
	memmap_pages_add(-1L * (DIV_ROUND_UP(table_size, PAGE_SIZE)));

	/* 按地址类型选择与分配后端严格对称的释放和 kmemleak 注销。 */
	if (is_vmalloc_addr(addr)) {
		vfree(addr);
	} else {
		page = virt_to_page(addr);
		BUG_ON(PageReserved(page));
		kmemleak_free(addr);
		free_pages_exact(addr, table_size);
	}
}

/*
 * 业务背景：从 section 的偏移编码指针恢复真实基址并释放，用于热拔和上线
 * 失败回滚；调用者已保证读者排空或该表尚未对外使用。
 * 入参：pfn 属于目标 section，可为该 section 内任意 PFN。
 * 出参/返回：无直接返回值；缺失时无操作，存在时清空发布指针并释放 backing。
 * 注意事项：若指针带 INVALID 位先去哨兵；WRITE_ONCE 防止编译器撕裂发布。
 */
static void __free_page_ext(unsigned long pfn)
{
	/* ms 是 section 借用指针；base 从偏移编码/INVALID 状态恢复真实地址。 */
	struct mem_section *ms;
	struct page_ext *base;

	ms = __pfn_to_section(pfn);
	if (!ms || !ms->page_ext)
		return;

	base = READ_ONCE(ms->page_ext);
	/*
	 * page_ext here can be valid while doing the roll back
	 * operation in online_page_ext().
	 */
	/* 上线批次回滚时表可能仍有效，热拔时则已带 INVALID；两种状态都可释放。 */
	if (page_ext_invalid(base))
		base = (void *)base - PAGE_EXT_INVALID;
	/* 先从 section 摘除，再用目标 pfn 抵消发布时的全局 PFN 负偏移。 */
	WRITE_ONCE(ms->page_ext, NULL);

	base = get_entry(base, pfn);
	free_page_ext(base);
}

/*
 * 业务背景：热拔第一阶段把 section->page_ext 低位标为 INVALID，阻止新 RCU
 * 读者取得 backing，同时保留真实地址供最终释放。
 * 入参：pfn 定位目标 section。
 * 出参/返回：无直接返回值；缺失时无操作，存在时原子样式发布失效哨兵。
 * 注意事项：不释放内存；必须随后 synchronize_rcu() 再 __free_page_ext()。
 */
static void __invalidate_page_ext(unsigned long pfn)
{
	/* ms 为目标 section；val 保存原偏移基址加低位哨兵后的发布值。 */
	struct mem_section *ms;
	void *val;

	ms = __pfn_to_section(pfn);
	if (!ms || !ms->page_ext)
		return;
	/* 对齐保证低位为零，加 1 等价于设置 INVALID 编码且仍可逆。 */
	val = (void *)ms->page_ext + PAGE_EXT_INVALID;
	WRITE_ONCE(ms->page_ext, val);
}

/*
 * 业务背景：内存即将上线时为覆盖范围内每个 section 准备 page_ext，保证页
 * 交给 allocator 前扩展 backing 已齐全；由 hotplug notifier 调用。
 * 入参：start_pfn 为起始页帧；nr_pages 为基础页数量，范围可不按 section 对齐。
 * 出参/返回：全部成功返回 0；任一分配失败返回 -ENOMEM，并释放本批此前经过
 * 的 section 表。
 * 注意事项：可睡眠且受热插拔状态机串行；nid 取起始 PFN，跨节点范围应由
 * 上层拆分。协议依赖本范围对应待上线 section；若混入进入前已有表，当前
 * 回滚循环同样会释放它，因此调用者不能违反该前置条件。
 */
static int __meminit online_page_ext(unsigned long start_pfn,
				unsigned long nr_pages)
{
	/* nid 决定分配位置；start/end 是 section 对齐半开区间；pfn/fail 驱动批次。 */
	int nid = pfn_to_nid(start_pfn);
	unsigned long start, end, pfn;
	int fail = 0;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	/* 顺序发布每个 section；fail 后停止，pfn 此时已前进到失败项的下一步值。 */
	for (pfn = start; !fail && pfn < end; pfn += PAGES_PER_SECTION)
		fail = init_section_page_ext(pfn, nid);
	if (!fail)
		return 0;

	/* rollback */
	/* 回滚本批成功前缀；已有表的重复初始化返回 0，但热插拔范围通常为新 section。 */
	end = pfn - PAGES_PER_SECTION;
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION)
		__free_page_ext(pfn);

	return -ENOMEM;
}

/*
 * 业务背景：内存下线或上线取消时撤销范围内 page_ext，并用三阶段协议阻止
 * 并发 page_ext_get()/lookup 读者发生 use-after-free。
 * 入参：start_pfn/nr_pages 描述基础页范围，可不按 section 对齐。
 * 出参/返回：无直接返回值；覆盖 section 的发布指针失效、旧 RCU 读者排空、
 * backing 最终释放。
 * 注意事项：会 synchronize_rcu/vfree，必须可睡眠；调用者的热插拔状态机保证
 * 不再产生需要这些 section 扩展的新页用户。
 */
static void __meminit offline_page_ext(unsigned long start_pfn,
				unsigned long nr_pages)
{
	/* start/end 为 section 对齐半开范围，pfn 是三阶段共享游标。 */
	unsigned long start, end, pfn;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	/*
	 * Freeing of page_ext is done in 3 steps to avoid
	 * use-after-free of it:
	 * 1) Traverse all the sections and mark their page_ext
	 *    as invalid.
	 * 2) Wait for all the existing users of page_ext who
	 *    started before invalidation to finish.
	 * 3) Free the page_ext.
	 */
	/*
	 * 释放严格分三步：先遍历全部 section 标 invalid，阻止新读者；再等待标记前
	 * 已进入的 RCU 读者结束；最后才释放 backing。若逐段立即释放，后段处理时
	 * 仍可能有读者持有前段指针而形成 UAF。
	 */
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION)
		__invalidate_page_ext(pfn);

	synchronize_rcu();

	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION)
		__free_page_ext(pfn);
}

/*
 * 业务背景：把内存热插拔 notifier 事件翻译为 page_ext 上线/下线动作，使
 * 扩展 backing 与 section 可分配生命周期同步。
 * 入参：self 为注册 notifier 的借用指针且本实现不用；action 是 MEM_* 事件；
 * arg 必须是借用的 memory_notify，含 PFN 起点和页数。
 * 出参/返回：返回 notifier 编码；上线分配失败传播 -ENOMEM，其他事件成功。
 * 注意事项：在可睡眠热插拔链调用；各 case 的时点不同，只有 GOING_ONLINE
 * 分配，OFFLINE/CANCEL_ONLINE 释放，纯通知或取消下线不改变 backing。
 */
static int __meminit page_ext_callback(struct notifier_block *self,
			       unsigned long action, void *arg)
{
	/* mn 是本次事件范围的借用描述；ret 仅承载上线错误。 */
	struct memory_notify *mn = arg;
	int ret = 0;

	/* 按热插拔状态选择创建、销毁或保持，返回前统一转换 notifier errno。 */
	switch (action) {
	case MEM_GOING_ONLINE:
		/* 页尚未在线，先准备扩展；失败可阻止本次上线。 */
		ret = online_page_ext(mn->start_pfn, mn->nr_pages);
		break;
	case MEM_OFFLINE:
		/* 页已完成下线，不会再产生新用户，现在可执行 RCU 安全释放。 */
		offline_page_ext(mn->start_pfn,
				mn->nr_pages);
		break;
	case MEM_CANCEL_ONLINE:
		/* 上线在后续阶段取消，撤销 GOING_ONLINE 已分配的 backing。 */
		offline_page_ext(mn->start_pfn,
				mn->nr_pages);
		break;
	case MEM_GOING_OFFLINE:
		/* 预下线仍可能取消，暂保留 backing，避免恢复在线时重新创建。 */
		break;
	case MEM_ONLINE:
		/* 上线完成无需动作，表已在 GOING_ONLINE 发布。 */
	case MEM_CANCEL_OFFLINE:
		/* 下线取消后原表从未失效，保持即可；与 MEM_ONLINE 共用空分支。 */
		break;
	}

	return notifier_from_errno(ret);
}

/*
 * 业务背景：SPARSEMEM 启动总入口，为所有含内存 node 的有效 section 分配
 * page_ext，注册热插拔维护，并在完整发布后启动客户端。
 * 入参：无。
 * 出参/返回：无直接返回值；无人需要时无副作用；成功发布初始 section 表、
 * notifier 和客户端状态；任何初始分配失败均 panic。
 * 注意事项：__init 可睡眠，遍历时 node 的 PFN 区间可能重叠且边界不对齐，
 * 必须验证 pfn 与 nid；notifier 注册是未来热插拔的发布边界。
 */
void __init page_ext_init(void)
{
	/* pfn 是跨 node 的 section 游标，nid 是 N_MEMORY 节点游标。 */
	unsigned long pfn;
	int nid;

	/* 无客户端需要扩展时连 notifier 都不注册，保持零运行期开销。 */
	if (!invoke_need_callbacks())
		return;

	/* 按含内存节点遍历；start/end 是当前 node 的 PFN 半开跨度。 */
	for_each_node_state(nid, N_MEMORY) {
		unsigned long start_pfn, end_pfn;

		start_pfn = node_start_pfn(nid);
		end_pfn = node_end_pfn(nid);
		/*
		 * start_pfn and end_pfn may not be aligned to SECTION and the
		 * page->flags of out of node pages are not initialized.  So we
		 * scan [start_pfn, the biggest section's pfn < end_pfn) here.
		 */
		/*
		 * node 边界可落在 section 中部，边界外 page->flags 尚未初始化；只从
		 * start_pfn 起跳到后续 section 边界，且不越过 end_pfn。
		 */
		for (pfn = start_pfn; pfn < end_pfn;
			pfn = ALIGN(pfn + 1, PAGES_PER_SECTION)) {

			/* hole 没有有效 struct page，自然不需要扩展 backing。 */
			if (!pfn_valid(pfn))
				continue;
			/*
			 * Nodes's pfns can be overlapping.
			 * We know some arch can have a nodes layout such as
			 * -------------pfn-------------->
			 * N0 | N1 | N2 | N0 | N1 | N2|....
			 */
			/*
			 * 某些架构的 node PFN 包络彼此重叠并交错；再次核对实际 nid，防止
			 * 为同一 section 按错误节点重复分配。
			 */
			if (pfn_to_nid(pfn) != nid)
				continue;
			/* 每个有效且归属匹配的 section 必须成功，否则不能部分启用客户端。 */
			if (init_section_page_ext(pfn, nid))
				goto oom;
			cond_resched();
		}
	}
	/* 初始表齐备后注册未来增删维护，再通知客户端；顺序避免回调看到缺表。 */
	hotplug_memory_notifier(page_ext_callback, DEFAULT_CALLBACK_PRI);
	pr_info("allocated %ld bytes of page_ext\n", total_usage);
	invoke_init_callbacks();
	return;

oom:
	/* 启动期无安全降级：客户端已声明强依赖，部分 backing 会破坏每页状态。 */
	panic("Out of memory");
}

/*
 * 业务背景：SPARSEMEM 不在 pgdat 保存 node 级 page_ext，提供与通用 pgdat
 * 初始化调用点匹配的空实现。
 * 入参：pgdat 是正在构造的节点描述，借用且本配置不读取。
 * 出参/返回：无直接返回值、无副作用。
 * 注意事项：仅 SPARSEMEM 编译，不睡眠；实际 backing 由 section 路径管理。
 */
void __meminit pgdat_page_ext_init(struct pglist_data *pgdat)
{
}

#endif

/**
 * page_ext_lookup() - Lookup a page extension for a PFN.
 * @pfn: PFN of the page we're interested in.
 *
 * Must be called with RCU read lock taken and @pfn must be valid.
 *
 * Return: NULL if no page_ext exists for this page.
 */
/*
 * 查找目标 PFN 的页扩展；调用者必须已持 RCU read lock 且 PFN 有效，返回 NULL
 * 表示尚未分配或正在下线，非 NULL 指针只在该 RCU 临界区内有效。
 */
/*
 * 业务背景：为已经管理 RCU 临界区的批量迭代器提供无额外加锁的 PFN 查找。
 * 入参：pfn 是有效普通页的物理页帧号，纯输入。
 * 出参/返回：返回 RCU 借用 page_ext 或 NULL，不改变引用/锁状态。
 * 注意事项：不睡眠；本函数不验证 PFN，MMIO/洞/离线地址不能直接传入。
 */
struct page_ext *page_ext_lookup(unsigned long pfn)
{
	return lookup_page_ext(pfn_to_page(pfn));
}

/**
 * page_ext_get() - Get the extended information for a page.
 * @page: The page we're interested in.
 *
 * Ensures that the page_ext will remain valid until page_ext_put()
 * is called.
 *
 * Return: NULL if no page_ext exists for this page.
 * Context: Any context.  Caller may not sleep until they have called
 * page_ext_put().
 */
/*
 * 为 page 取得扩展信息并保持到 page_ext_put()；任意上下文可调用，但成功后
 * 实际持有 RCU read lock，调用者在 put 前不得睡眠。NULL 表示无扩展且不持锁。
 */
/*
 * 业务背景：给单页客户端封装 RCU acquire 与配置相关 lookup，建立清晰的
 * get/put 生命周期。
 * 入参：page 是有效普通页的借用指针，调用期间其 memmap 身份稳定。
 * 出参/返回：成功返回 RCU 借用 page_ext 并把一个读侧临界区交给调用者；失败
 * 返回 NULL且已自行解锁，无 ownership 转移。
 * 注意事项：任意上下文可调用；成功后必须且只能调用一次 page_ext_put()，
 * put 前不得睡眠，客户端字段的并发仍由客户端自身同步。
 */
struct page_ext *page_ext_get(const struct page *page)
{
	/* page_ext 是 lookup 的借用结果，也作为是否把 RCU 锁交给调用者的判据。 */
	struct page_ext *page_ext;

	/* 先进入 RCU，再读取 section/node 发布指针，防止热拔释放 backing。 */
	rcu_read_lock();
	page_ext = lookup_page_ext(page);
	/* 失败路径在返回 NULL 前自行配平锁；成功路径故意保持锁定。 */
	if (!page_ext) {
		rcu_read_unlock();
		return NULL;
	}

	return page_ext;
}

/**
 * page_ext_from_phys() - Get the page_ext structure for a physical address.
 * @phys: The physical address to query.
 *
 * This function safely gets the `struct page_ext` associated with a given
 * physical address. It performs validation to ensure the address corresponds
 * to a valid, online struct page before attempting to access it.
 * It returns NULL for MMIO, ZONE_DEVICE, holes and offline memory.
 *
 * Return: NULL if no page_ext exists for this physical address.
 * Context: Any context.  Caller may not sleep until they have called
 * page_ext_put().
 */
/*
 * 安全取得物理地址对应的 page_ext：先确认它映射到有效、在线的普通 struct
 * page，再执行 page_ext_get。MMIO、ZONE_DEVICE、洞和离线内存均返回 NULL；
 * 成功后同样要求 page_ext_put，且在此之前不得睡眠。
 */
/*
 * 业务背景：IOMMU 等只有物理地址的诊断路径需要访问普通 RAM 的扩展信息，
 * 本函数把地址合法性和 RCU acquire 合并，避免盲目 pfn_to_page。
 * 入参：phys 是物理字节地址纯输入，页内偏移会在 PFN 转换中丢弃。
 * 出参/返回：返回持有 RCU 临界区的 page_ext，或 NULL 且不持锁。
 * 注意事项：任意上下文可调用；成功后必须 page_ext_put 且此前不得睡眠。
 */
struct page_ext *page_ext_from_phys(phys_addr_t phys)
{
	/* page 仅是在线普通页的借用结果，NULL 统一表示所有不可接受物理范围。 */
	struct page *page = pfn_to_online_page(__phys_to_pfn(phys));

	/* pfn_to_online_page 已排除 MMIO、洞、离线与 ZONE_DEVICE，无扩展资源需回滚。 */
	if (!page)
		return NULL;

	return page_ext_get(page);
}

/**
 * page_ext_put() - Working with page extended information is done.
 * @page_ext: Page extended information received from page_ext_get().
 *
 * The page extended information of the page may not be valid after this
 * function is called.
 *
 * Return: None.
 * Context: Any context with corresponding page_ext_get() is called.
 */
/*
 * 结束对页扩展信息的使用。返回后原 page_ext 可能立即因热拔失效并被释放；
 * 可传 NULL，此时无操作。上下文必须与成功的 page_ext_get() 一一对应。
 */
/*
 * 业务背景：释放 page_ext_get()/from_phys() 转交给调用者的 RCU 读侧保护。
 * 入参：page_ext 是先前 get 返回的借用指针，可为 NULL；不拥有 backing。
 * 出参/返回：无直接返回值；非 NULL 时退出一个 RCU read lock。
 * 注意事项：任意非睡眠上下文可调用；重复 put、传入 lookup 的指针或遗漏 put
 * 都会破坏 RCU 嵌套/热拔生命周期，返回后不得再解引用该指针。
 */
void page_ext_put(struct page_ext *page_ext)
{
	if (unlikely(!page_ext))
		return;

	rcu_read_unlock();
}
