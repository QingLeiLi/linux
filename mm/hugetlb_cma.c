// SPDX-License-Identifier: GPL-2.0-only

#include <linux/mm.h>
#include <linux/cma.h>
#include <linux/compiler.h>
#include <linux/mm_inline.h>

#include <asm/page.h>
#include <asm/setup.h>

#include <linux/hugetlb.h>
#include "internal.h"
#include "hugetlb_cma.h"


static struct cma *hugetlb_cma[MAX_NUMNODES] __ro_after_init;
// 由 cmdline_parse_hugetlb_cma 解析命令行参数，将数据存入该数组
static unsigned long hugetlb_cma_size_in_node[MAX_NUMNODES] __initdata;
static bool hugetlb_cma_only __ro_after_init;
static unsigned long hugetlb_cma_size __ro_after_init;

void hugetlb_cma_free_frozen_folio(struct folio *folio)
{
	WARN_ON_ONCE(!cma_release_frozen(hugetlb_cma[folio_nid(folio)],
					 &folio->page, folio_nr_pages(folio)));
}

struct folio *hugetlb_cma_alloc_frozen_folio(int order, gfp_t gfp_mask,
		int nid, nodemask_t *nodemask)
{
	int node;
	struct folio *folio;
	struct page *page = NULL;

	if (!hugetlb_cma_size)
		return NULL;

	if (hugetlb_cma[nid])
		page = cma_alloc_frozen_compound(hugetlb_cma[nid], order);

	if (!page && !(gfp_mask & __GFP_THISNODE)) {
		for_each_node_mask(node, *nodemask) {
			if (node == nid || !hugetlb_cma[node])
				continue;

			page = cma_alloc_frozen_compound(hugetlb_cma[node], order);
			if (page)
				break;
		}
	}

	if (!page)
		return NULL;

	folio = page_folio(page);
	folio_set_hugetlb_cma(folio);
	return folio;
}

struct huge_bootmem_page * __init
hugetlb_cma_alloc_bootmem(struct hstate *h, int *nid, bool node_exact)
{
	struct cma *cma;
	struct huge_bootmem_page *m;
	int node = *nid;

	cma = hugetlb_cma[*nid];
	m = cma_reserve_early(cma, huge_page_size(h));
	if (!m) {
		if (node_exact)
			return NULL;

		for_each_node_mask(node, hugetlb_bootmem_nodes) {
			cma = hugetlb_cma[node];
			if (!cma || node == *nid)
				continue;
			m = cma_reserve_early(cma, huge_page_size(h));
			if (m) {
				*nid = node;
				break;
			}
		}
	}

	if (m) {
		m->flags = HUGE_BOOTMEM_CMA;
		m->cma = cma;
	}

	return m;
}

/*
命令行参数的两种格式

# 格式1：总量，不指定节点（hugetlb_cma_size_in_node[] 全部为 0）
hugetlb_cma=4G
→ hugetlb_cma_size = 4GB
→ hugetlb_cma_size_in_node[] 全部为 0（由 hugetlb_cma_reserve() 均分到各节点）

# 格式2：节点专属（node:size 形式，逗号分隔）
hugetlb_cma=0:2G,1:2G
→ hugetlb_cma_size_in_node[0] = 2GB
→ hugetlb_cma_size_in_node[1] = 2GB
→ hugetlb_cma_size = 4GB（累加）
*/
static int __init cmdline_parse_hugetlb_cma(char *p)
{
	int nid, count = 0;
	unsigned long tmp;
	char *s = p;

	while (*s) {
		if (sscanf(s, "%lu%n", &tmp, &count) != 1)
			break;

		if (s[count] == ':') {
			if (tmp >= MAX_NUMNODES)
				break;
			nid = array_index_nospec(tmp, MAX_NUMNODES);

			s += count + 1;
			tmp = memparse(s, &s);
			hugetlb_cma_size_in_node[nid] = tmp;
			hugetlb_cma_size += tmp;

			/*
			 * Skip the separator if have one, otherwise
			 * break the parsing.
			 */
			if (*s == ',')
				s++;
			else
				break;
		} else {
			hugetlb_cma_size = memparse(p, &p);
			break;
		}
	}

	return 0;
}

early_param("hugetlb_cma", cmdline_parse_hugetlb_cma);

static int __init cmdline_parse_hugetlb_cma_only(char *p)
{
	return kstrtobool(p, &hugetlb_cma_only);
}

early_param("hugetlb_cma_only", cmdline_parse_hugetlb_cma_only);

unsigned int __weak arch_hugetlb_cma_order(void)
{
	return 0;
}

/*
 * hugetlb_cma_reserve - 为 gigantic 巨页向 CMA 框架预留连续物理内存
 *
 * 调用时机：memblock 可用，free_area_init() 之前。
 * hugetlb_cma_size 由命令行参数 hugetlb_cma=<size>[:<node>] 设置。
 *
 * gigantic 巨页（order > MAX_PAGE_ORDER，如 x86 上的 1GiB 页）无法通过
 * buddy 分配器获取，因为 buddy 管理的最大块是 MAX_PAGE_ORDER 阶。
 * 必须在 memblock 阶段抢先预留足够大的连续物理内存，
 * 一旦内存被 buddy 接管后碎片化，就再也无法凑出连续的 gigantic 块。
 *
 * 成功预留的区域写入 hugetlb_cma[nid]（每节点一个 struct cma *），
 * 后续 hugetlb_cma_alloc_frozen_folio() 从这些区域分配 gigantic 页。
 * 若一个节点都没预留成功，hugetlb_cma_size 清零，禁用 CMA 路径。
 */
void __init hugetlb_cma_reserve(void)
{
	unsigned long size, reserved, per_node, order, gigantic_page_size;
	bool node_specific_cma_alloc = false;
	int nid;

	/* hugetlb_cma_size 为 0 表示用户未指定 hugetlb_cma= 参数，无需预留。 */
	if (!hugetlb_cma_size)
		return;

	/* arch_hugetlb_cma_order() 是 __weak 函数，返回平台支持的 gigantic 页阶数。
	 * 返回 0 表示当前架构不支持 gigantic 巨页（如无 1G 页支持的 32 位系统）。 */
	order = arch_hugetlb_cma_order();
	if (!order) {
		pr_warn("hugetlb_cma: the option isn't supported by current arch\n");
		return;
	}

	/*
	 * HugeTLB CMA reservation is required for gigantic
	 * huge pages which could not be allocated via the
	 * page allocator. Just warn if there is any change
	 * breaking this assumption.
	 *
	 * 断言：CMA 路径仅用于 gigantic 页（order > MAX_PAGE_ORDER），
	 * 普通大页由 buddy 分配器处理，不走此路径。
	 */
	VM_WARN_ON(order <= MAX_PAGE_ORDER);
	/* 单个 gigantic 页的字节大小，也是 CMA 区域的对齐粒度。
	 * 例如 x86 上 order=18，gigantic_page_size = 4K << 18 = 1GiB。 */
	gigantic_page_size = PAGE_SIZE << order;

	/* 扫描 memblock，构建 hugetlb_bootmem_nodes 掩码，记录所有有物理内存的节点。
	 * 后续循环用此掩码跳过无内存节点，避免在空节点上浪费 CMA 预留。 */
	hugetlb_bootmem_set_nodes();

	/* 第一轮：校验用户指定的 per-node 配置（hugetlb_cma=<size>:<node> 格式）。
	 * 对每个有节点专属配置的节点检查两个条件：
	 *   1. 节点必须在 hugetlb_bootmem_nodes 中（即有实际物理内存）
	 *   2. 分配大小必须是 gigantic_page_size 的整数倍
	 * 不满足条件的节点：从 hugetlb_cma_size 中扣除其配额并清零，
	 * 满足条件的节点：设置 node_specific_cma_alloc = true，进入节点专属分配路径。 */
	for (nid = 0; nid < MAX_NUMNODES; nid++) {
		size = hugetlb_cma_size_in_node[nid];
		if (size == 0)
			continue;

		if (!node_isset(nid, hugetlb_bootmem_nodes)) {
			pr_warn("hugetlb_cma: invalid node %d specified\n", nid);
		} else if (!IS_ALIGNED(size, gigantic_page_size)) {
			pr_warn("hugetlb_cma: cma area of node %d must be a multiple of %lu MiB\n",
				nid, gigantic_page_size / SZ_1M);
		} else {
			node_specific_cma_alloc = true;
			continue;
		}

		/* 该节点配置无效，从总量中扣除并清零，避免后续误用。 */
		hugetlb_cma_size -= size;
		hugetlb_cma_size_in_node[nid] = 0;
	}

	/* Validate the CMA size again in case some invalid nodes specified.
	 * 清洗无效节点后若总量归零，说明所有节点配置均无效，直接退出。 */
	if (!hugetlb_cma_size)
		return;

	/* 无节点专属配置时，总量也必须是 gigantic_page_size 的整数倍，
	 * 否则无法整除到各节点，清零并退出。 */
	if (!IS_ALIGNED(hugetlb_cma_size, gigantic_page_size)) {
		pr_warn("hugetlb_cma: cma area must be a multiple of %lu MiB\n",
			gigantic_page_size / SZ_1M);
		hugetlb_cma_size = 0;
		return;
	}

	if (!node_specific_cma_alloc) {
		/*
		 * If 3 GB area is requested on a machine with 4 numa nodes,
		 * let's allocate 1 GB on first three nodes and ignore the last one.
		 *
		 * 无节点专属配置时，将总量均分到所有有内存的节点。
		 * DIV_ROUND_UP 向上取整防止末尾节点分到的量不足一个 gigantic 页，
		 * 再 round_up 到 gigantic_page_size 对齐（CMA 区域必须对齐）。
		 * 实际分配时用 min(per_node, remaining) 防止超出总量。
		 */
		per_node = DIV_ROUND_UP(hugetlb_cma_size,
					nodes_weight(hugetlb_bootmem_nodes));
		per_node = round_up(per_node, gigantic_page_size);
		pr_info("hugetlb_cma: reserve %lu MiB, up to %lu MiB per node\n",
			hugetlb_cma_size / SZ_1M, per_node / SZ_1M);
	}

	/* 第二轮：在每个有效节点上实际向 CMA 框架登记保留区域。 */
	reserved = 0;
	for_each_node_mask(nid, hugetlb_bootmem_nodes) {
		int res;
		char name[CMA_MAX_NAME];

		if (node_specific_cma_alloc) {
			/* 节点专属模式：跳过未指定配额的节点，使用该节点的专属配额。 */
			if (hugetlb_cma_size_in_node[nid] == 0)
				continue;

			size = hugetlb_cma_size_in_node[nid];
		} else {
			/* 均分模式：取 per_node 与剩余量的较小值，防止超出总量。 */
			size = min(per_node, hugetlb_cma_size - reserved);
		}

		/* CMA 区域名称格式 "hugetlbN"，用于 /sys/kernel/debug/cma/ 下的调试接口。 */
		snprintf(name, sizeof(name), "hugetlb%d", nid);
		/*
		 * Note that 'order per bit' is based on smallest size that
		 * may be returned to CMA allocator in the case of
		 * huge page demotion.
		 *
		 * cma_declare_contiguous_multi() 向 memblock 登记保留区域并初始化 CMA 位图。
		 * 参数说明：
		 *   size             - 本节点预留的总字节数
		 *   gigantic_page_size - 对齐粒度，CMA 区域起始地址和大小必须对齐到此值
		 *   HUGETLB_PAGE_ORDER - order_per_bit：位图中每个 bit 代表 2^N 个页，
		 *                        这里取普通大页的阶数，是因为 gigantic 页降级
		 *                        （demotion）时会拆分成普通大页归还给 CMA，
		 *                        位图粒度必须能精确追踪普通大页大小的块。
		 *   name             - CMA 区域名称（调试用）
		 *   &hugetlb_cma[nid] - 输出：成功后写入该节点的 struct cma 指针
		 *   nid              - 指定 NUMA 节点，CMA 只在该节点的内存上预留
		 */
		res = cma_declare_contiguous_multi(size, gigantic_page_size,
					HUGETLB_PAGE_ORDER, name,
					&hugetlb_cma[nid], nid);
		if (res) {
			pr_warn("hugetlb_cma: reservation failed: err %d, node %d",
				res, nid);
			continue;
		}

		reserved += size;
		pr_info("hugetlb_cma: reserved %lu MiB on node %d\n",
			size / SZ_1M, nid);

		/* 已预留量达到总量，无需继续遍历剩余节点。 */
		if (reserved >= hugetlb_cma_size)
			break;
	}

	if (!reserved)
		/*
		 * hugetlb_cma_size is used to determine if allocations from
		 * cma are possible.  Set to zero if no cma regions are set up.
		 *
		 * 所有节点均预留失败，禁用 CMA 路径（后续分配时以此值判断是否可用）。
		 */
		hugetlb_cma_size = 0;
}

bool hugetlb_cma_exclusive_alloc(void)
{
	return hugetlb_cma_only;
}

unsigned long __init hugetlb_cma_total_size(void)
{
	return hugetlb_cma_size;
}

void __init hugetlb_cma_validate_params(void)
{
	if (!hugetlb_cma_size)
		hugetlb_cma_only = false;
}

bool __init hugetlb_early_cma(struct hstate *h)
{
	if (arch_has_huge_bootmem_alloc())
		return false;

	return hstate_is_gigantic(h) && hugetlb_cma_only;
}
