// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/memblock.h>
#include <linux/printk.h>
#include <linux/numa.h>
#include <linux/numa_memblks.h>

/*
 * node_data[] 是 nid 到 pg_data_t 的全局发布表：启动代码分配并清零条目，页分配、
 * 回收和热插拔随后通过 NODE_DATA(nid) 借用它。数组与 pgdat 均为内核全寿命对象；
 * 启动发布依赖初始化串行性，运行期不能无锁替换已在线节点的指针。
 */
struct pglist_data *node_data[MAX_NUMNODES];
EXPORT_SYMBOL(node_data);

/* Allocate NODE_DATA for a node on the local memory */
/*
 * 为节点优先从其本地物理内存分配 NODE_DATA；若本地不可用，memblock 会退化到
 * 其他节点。该对象承载节点的 zones、回收和统计状态，必须在后续 free_area_init()
 * 等初始化读取 node_data[nid] 前完成分配与清零。
 */
/*
 * alloc_node_data() - 分配、清零并发布一个在线节点的 pg_data_t。
 * 业务背景：架构 NUMA 初始化在发现节点后调用；memblock 提供启动期物理内存，
 * NODE_DATA 宏随后成为所有通用 MM 节点操作的入口。
 * 入参：nid 是 [0, MAX_NUMNODES) 的节点号，纯输入；调用者已验证节点存在。
 * 出参/返回：无直接返回值；成功后 node_data[nid] 指向内核直映射中的零初始化
 * pg_data_t，内核全寿命持有；无法分配则 panic，不存在可返回的部分成功状态。
 * 注意事项：仅 __init、可分配且不用于运行期热路径；按 cache line 对齐以避免
 * 热字段跨线。实际物理页可退化到其他节点，日志中的 tnid 明确记录这种情况。
 */
void __init alloc_node_data(int nid)
{
	/* nd_size 含 cache-line 尾部填充；nd_pa 是 memblock 返回的物理首地址。 */
	const size_t nd_size = roundup(sizeof(pg_data_t), SMP_CACHE_BYTES);
	u64 nd_pa;
	int tnid;

	/* Allocate node data.  Try node-local memory and then any node. */
	/* 先请求 nid 本地内存，分配器失败时再搜索任意可用节点；0 表示彻底失败。 */
	nd_pa = memblock_phys_alloc_try_nid(nd_size, SMP_CACHE_BYTES, nid);
	if (!nd_pa)
		panic("Cannot allocate %zu bytes for node %d data\n",
		      nd_size, nid);

	/* report and initialize */
	/* 先报告最终物理位置，再把物理地址转为启动期可用的线性映射虚拟地址。 */
	pr_info("NODE_DATA(%d) allocated [mem %#010Lx-%#010Lx]\n", nid,
		nd_pa, nd_pa + nd_size - 1);
	tnid = early_pfn_to_nid(nd_pa >> PAGE_SHIFT);
	if (tnid != nid)
		pr_info("    NODE_DATA(%d) on node %d\n", nid, tnid);

	/* 发布指针后立即清零；启动阶段尚无并发 reader，二者共同构成初始化提交。 */
	node_data[nid] = __va(nd_pa);
	memset(NODE_DATA(nid), 0, sizeof(pg_data_t));
}

/*
 * alloc_offline_node_data() - 为无内存/离线节点预建一个空 pg_data_t。
 * 业务背景：通用节点初始化希望每个可能节点都有 NODE_DATA，即使暂时没有 PFN；
 * 后续内存上线可在该稳定对象上填充 zone 状态。
 * 入参：nid 是已验证的可能节点号，纯输入。出参/返回：无直接返回值；成功后
 * node_data[nid] 持有 memblock 分配的 cache-line 对齐、零初始化对象直到关机。
 * 注意事项：仅 __init，失败由 memblock_alloc_or_panic() 终止启动；没有普通回滚，
 * 局部 pgdat 只用于 sizeof 推导，不是已初始化指针，也不转移任何 ownership。
 */
void __init alloc_offline_node_data(int nid)
{
	/* memblock_alloc_or_panic 返回已清零的启动内存，并处理不可恢复的分配失败。 */
	pg_data_t *pgdat;
	node_data[nid] = memblock_alloc_or_panic(sizeof(*pgdat), SMP_CACHE_BYTES);
}

/* Stub functions: */
/*
 * 以下是体系结构未提供物理地址到 NUMA 节点映射时的保守桩。它们保持热插拔和
 * CXL/DAX 等通用调用链可链接，但只能选择 node 0；平台若有真实拓扑，会通过
 * 同名宏/实现排除这些分支。
 */

#ifndef memory_add_physaddr_to_nid
/*
 * memory_add_physaddr_to_nid() - 为待上线物理内存选择归属节点的通用回退。
 * 业务背景：memory hotplug 在创建内存块前查询 nid；无架构映射时避免返回无效值。
 * 入参：start 是待添加范围的物理字节地址，纯输入。出参/返回：始终返回节点 0，
 * 不修改拓扑或取得引用；首次调用额外打印一次精度退化提示。
 * 注意事项：可并发调用且 pr_info_once 保证日志一次；结果是兼容回退，不代表硬件
 * 真实亲和性，平台需要准确放置时必须提供覆盖实现。
 */
int memory_add_physaddr_to_nid(u64 start)
{
	pr_info_once("Unknown online node for memory at 0x%llx, assuming node 0\n",
			start);
	return 0;
}
EXPORT_SYMBOL_GPL(memory_add_physaddr_to_nid);
#endif

#ifndef phys_to_target_node
/*
 * phys_to_target_node() - 查询物理地址目标/性能节点的通用回退。
 * 业务背景：CXL、DAX 等驱动用 target node 描述设备内存亲和性；缺少平台实现时
 * 仍须给上层一个有效 nid。入参：start 为物理字节地址，纯输入。
 * 出参/返回：始终返回 0，无对象或 ownership 副作用；首次调用打印回退提示。
 * 注意事项：不加锁、不睡眠；返回值只保证 API 可用，不能据此推导真实距离，
 * `#ifndef` 使具有 numa_memblks/架构实现的配置不会编译本桩。
 */
int phys_to_target_node(u64 start)
{
	pr_info_once("Unknown target node for memory at 0x%llx, assuming node 0\n",
			start);
	return 0;
}
EXPORT_SYMBOL_GPL(phys_to_target_node);
#endif
