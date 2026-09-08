// SPDX-License-Identifier: GPL-2.0
/*
 * NUMA emulation
 */
/*
 * 本文件只在启动早期重写 NUMA 拓扑：先保留固件报告的物理 memblk/距离，按
 * numa=fake 参数构造模拟节点，校验成功后一次性提交；运行期 CPU 上下线再把
 * 同一物理节点上的 CPU 映射到它承载的全部模拟节点。
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/topology.h>
#include <linux/memblock.h>
#include <linux/numa_memblks.h>
#include <asm/numa.h>
#include <acpi/acpi_numa.h>

/* 每个模拟块至少 32MiB，掩码同时用于按 32MiB 粒度向下对齐。 */
#define FAKE_NODE_MIN_SIZE	((u64)32 << 20)
#define FAKE_NODE_MIN_HASH_MASK	(~(FAKE_NODE_MIN_SIZE - 1UL))

/* 模拟 nid 到固件物理 nid 的启动期映射；CPU mask 和距离表重建都会读取它。 */
int emu_nid_to_phys[MAX_NUMNODES];
/* 指向内核命令行缓冲区内尚未解析的位置，仅在 __init 阶段有效。 */
static char *emu_cmdline __initdata;

/*
 * numa_emu_cmdline() - 保存 numa=fake 参数供稍后的拓扑初始化解析。
 * 业务背景：early_param 解析与 NUMA memblock 建图分属不同阶段，此处只暂存字符串。
 * 入参：str 是命令行缓冲区中的可写借用指针，可为解析器提供的有效空串。
 * 出参/返回：恒返回 0，并令 emu_cmdline 指向 str；不取得独立内存所有权。
 * 注意事项：仅启动期调用，不复制字符串、不睡眠，后续解析会推进该指针。
 */
int __init numa_emu_cmdline(char *str)
{
	emu_cmdline = str;
	return 0;
}

/*
 * emu_find_memblk_by_nid() - 查找某物理节点当前尚未消费的第一个内存块。
 * 业务背景：切分循环每次从 pi 中取一个块，块耗尽后会被删除并压紧数组。
 * 入参：nid 是物理节点号；mi 是启动期 meminfo 的只读借用快照，必须非 NULL。
 * 出参/返回：找到时返回块下标，否则返回 -ENOENT；无副作用且不睡眠。
 * 注意事项：返回下标只在调用者下一次修改 mi 之前有效。
 */
static int __init emu_find_memblk_by_nid(int nid, const struct numa_meminfo *mi)
{
	int i;

	/* nr_blks 是有效前缀；删除 helper 会维持该紧凑布局。 */
	for (i = 0; i < mi->nr_blks; i++)
		if (mi->blk[i].nid == nid)
			return i;
	return -ENOENT;
}

/*
 * mem_hole_size() - 统计字节区间内没有实际页的容量。
 * 业务背景：非 uniform 切分按可用 RAM 而非地址跨度决定节点大小，必须扣除洞。
 * 入参：start/end 是半开物理字节区间；允许非页对齐及空/不足一页范围。
 * 出参/返回：返回完整页覆盖范围内缺失页的字节数，无状态副作用。
 * 注意事项：边界向内收缩，避免把部分页误算为洞；启动期调用且不睡眠。
 */
static u64 __init mem_hole_size(u64 start, u64 end)
{
	unsigned long start_pfn = PFN_UP(start);
	unsigned long end_pfn = PFN_DOWN(end);

	/* 只有至少包含一个完整页时才向 memblock/稀疏内存查询缺页数。 */
	if (start_pfn < end_pfn)
		return PFN_PHYS(absent_pages_in_range(start_pfn, end_pfn));
	return 0;
}

/*
 * Sets up nid to range from @start to @end.  The return value is -errno if
 * something went wrong, 0 otherwise.
 */
/*
 * 译注：从物理块头部切出 size 字节并归给模拟 nid；失败返回负 errno，成功返回 0。
 * 业务背景：所有切分策略最终经此 helper 追加 ei，同时消费 pi，因而它是构造账本的
 * 单一状态转换点。
 * 入参：ei 是输出模拟布局；pi 是仍待消费的物理布局；nid 为目标模拟节点；phys_blk
 * 是 pi 有效下标；size 为本次字节数，调用者保证不超过该物理块。
 * 出参/返回：成功追加一个 eb、推进或删除 pb、首次建立 nid->物理 nid 映射；表满返回
 * -EINVAL 且两份布局均不变。
 * 注意事项：两个 meminfo 都由启动线程独占；成功后的修改不在本函数内回滚，不睡眠。
 */
static int __init emu_setup_memblk(struct numa_meminfo *ei,
				   struct numa_meminfo *pi,
				   int nid, int phys_blk, u64 size)
{
	struct numa_memblk *eb = &ei->blk[ei->nr_blks];
	struct numa_memblk *pb = &pi->blk[phys_blk];

	/* 在取得输出槽之前检查固定数组容量，保持失败原子性。 */
	if (ei->nr_blks >= NR_NODE_MEMBLKS) {
		pr_err("NUMA: Too many emulated memblks, failing emulation\n");
		return -EINVAL;
	}

	/* 先发布模拟块字段，再从物理块头部扣除同一范围。 */
	ei->nr_blks++;
	eb->start = pb->start;
	eb->end = pb->start + size;
	eb->nid = nid;

	/* 一个模拟 nid 可跨多个物理块；拓扑归属采用它首次落到的物理 nid。 */
	if (emu_nid_to_phys[nid] == NUMA_NO_NODE)
		emu_nid_to_phys[nid] = pb->nid;

	pb->start += size;
	/* 块恰好耗尽时从 pi 删除；删除会把后续元素左移。 */
	if (pb->start >= pb->end) {
		WARN_ON_ONCE(pb->start > pb->end);
		numa_remove_memblk_from(phys_blk, pi);
	}

	printk(KERN_INFO "Faking node %d at [mem %#018Lx-%#018Lx] (%LuMB)\n",
	       nid, eb->start, eb->end - 1, (eb->end - eb->start) / SZ_1M);
	return 0;
}

/*
 * Sets up nr_nodes fake nodes interleaved over physical nodes ranging from addr
 * to max_addr.
 *
 * Returns zero on success or negative on error.
 */
/*
 * 译注：把 addr..max_addr 的可用 RAM 按物理节点轮转切成 nr_nodes 个模拟节点；成功
 * 返回 0，失败返回负值。
 * 业务背景：numa=fake=N 希望模拟节点在机器物理节点间交错，既保留 locality，又让
 * 各模拟 nid 获得近似相同的非保留容量。
 * 入参：ei/pi 分别为输出布局和可消费物理副本；地址为字节半开范围；nr_nodes 必须
 * 大于零，过大时截为 MAX_NUMNODES。
 * 出参/返回：成功耗尽 pi 并填充 ei/emu_nid_to_phys；块表溢出等返回负值，调用者丢弃
 * 整个候选布局，因此无需本地回滚。
 * 注意事项：启动期单线程；32 位避免 u64 除法 helper；洞、DMA32 尾段和物理块尾段
 * 都可能扩大当前模拟块，函数不睡眠。
 */
static int __init split_nodes_interleave(struct numa_meminfo *ei,
					 struct numa_meminfo *pi,
					 u64 addr, u64 max_addr, int nr_nodes)
{
	nodemask_t physnode_mask = numa_nodes_parsed;
	u64 size;
	int big;
	int nid = 0;
	int i, ret;

	/* 参数无效不触碰候选表；节点过多则显式降级到编译上限。 */
	if (nr_nodes <= 0)
		return -1;
	if (nr_nodes > MAX_NUMNODES) {
		pr_info("numa=fake=%d too large, reducing to %d\n",
			nr_nodes, MAX_NUMNODES);
		nr_nodes = MAX_NUMNODES;
	}

	/*
	 * Calculate target node size.  x86_32 freaks on __udivdi3() so do
	 * the division in ulong number of pages and convert back.
	 */
	/*
	 * 译注：先扣掉物理洞得到总可用页数，再用 unsigned long 页数做除法，避免 x86_32
	 * 生成 __udivdi3；结果转回字节后按 32MiB 粒度分配。
	 */
	size = max_addr - addr - mem_hole_size(addr, max_addr);
	size = PFN_PHYS((unsigned long)(size >> PAGE_SHIFT) / nr_nodes);

	/*
	 * Calculate the number of big nodes that can be allocated as a result
	 * of consolidating the remainder.
	 */
	/* 译注：不足一个 32MiB 的余数合并成 big 个额外 32MiB 节点。 */
	big = ((size & ~FAKE_NODE_MIN_HASH_MASK) * nr_nodes) /
		FAKE_NODE_MIN_SIZE;

	size &= FAKE_NODE_MIN_HASH_MASK;
	/* 对齐后为零说明无法给每个请求节点提供最小块，整次模拟失败。 */
	if (!size) {
		pr_err("Not enough memory for each node.  "
			"NUMA emulation disabled.\n");
		return -1;
	}

	/*
	 * Continue to fill physical nodes with fake nodes until there is no
	 * memory left on any of them.
	 */
	/* 译注：只要任一物理 nid 仍有块，就按 nodemask 顺序各切一块，形成轮转分布。 */
	while (!nodes_empty(physnode_mask)) {
		for_each_node_mask(i, physnode_mask) {
			u64 dma32_end = numa_emu_dma_end();
			u64 start, limit, end;
			int phys_blk;

			/* 某物理节点已无块时清位，外层 while 最终自然终止。 */
			phys_blk = emu_find_memblk_by_nid(i, pi);
			if (phys_blk < 0) {
				node_clear(i, physnode_mask);
				continue;
			}
			start = pi->blk[phys_blk].start;
			limit = pi->blk[phys_blk].end;
			end = start + size;

			if (nid < big)
				end += FAKE_NODE_MIN_SIZE;

			/*
			 * Continue to add memory to this fake node if its
			 * non-reserved memory is less than the per-node size.
			 */
			/* 译注：按 32MiB 步长跨过洞，直到净 RAM 达标或触及物理块上界。 */
			while (end - start - mem_hole_size(start, end) < size) {
				end += FAKE_NODE_MIN_SIZE;
				if (end > limit) {
					end = limit;
					break;
				}
			}

			/*
			 * If there won't be at least FAKE_NODE_MIN_SIZE of
			 * non-reserved memory in ZONE_DMA32 for the next node,
			 * this one must extend to the boundary.
			 */
			/* 译注：若 DMA32 边界前剩余净 RAM 不足最小节点，本节点吞并尾巴。 */
			if (end < dma32_end && dma32_end - end -
			    mem_hole_size(end, dma32_end) < FAKE_NODE_MIN_SIZE)
				end = dma32_end;

			/*
			 * If there won't be enough non-reserved memory for the
			 * next node, this one must extend to the end of the
			 * physical node.
			 */
			/* 译注：物理块尾部不足一个目标节点时也并入当前块，避免不可用碎片。 */
			if (limit - end - mem_hole_size(end, limit) < size)
				end = limit;

			/* nid 取模使多个物理块继续映射到同一组模拟节点。 */
			ret = emu_setup_memblk(ei, pi, nid++ % nr_nodes,
					       phys_blk,
					       min(end, limit) - start);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

/*
 * Returns the end address of a node so that there is at least `size' amount of
 * non-reserved memory or `max_addr' is reached.
 */
/*
 * 译注：寻找使 start..end 至少含 size 字节非保留内存的终点，最远不越过 max_addr。
 * 业务背景：固定大小模式遇到物理洞时必须扩展地址跨度，才能兑现最小净容量。
 * 入参：三个参数均为物理字节地址/容量；start 不大于 max_addr，size 已按 32MiB 对齐。
 * 出参/返回：返回计算的半开区间终点；无输出参数和全局副作用。
 * 注意事项：以最小块步长推进，触顶可能仍不足 size；启动期独占且不睡眠。
 */
static u64 __init find_end_of_node(u64 start, u64 max_addr, u64 size)
{
	u64 end = start + size;

	/* 地址跨度不足净容量时逐块扩张，洞越大所需跨度越大。 */
	while (end - start - mem_hole_size(start, end) < size) {
		end += FAKE_NODE_MIN_SIZE;
		if (end > max_addr) {
			end = max_addr;
			break;
		}
	}
	return end;
}

/*
 * uniform_size() - 用 PFN 算术求每个模拟节点的平均净字节数。
 * 业务背景：统一模式按物理容量等分，普通固定大小模式用它推导受节点上限约束的下限。
 * 入参：max_addr/base/hole 为页对齐语义的字节量；nr_nodes 必须大于零。
 * 出参/返回：返回整页向下取整的平均字节数，无副作用且不睡眠。
 * 注意事项：余页由后续尾段合并处理，不能把结果视为精确最终节点大小。
 */
static u64 uniform_size(u64 max_addr, u64 base, u64 hole, int nr_nodes)
{
	unsigned long max_pfn = PHYS_PFN(max_addr);
	unsigned long base_pfn = PHYS_PFN(base);
	unsigned long hole_pfns = PHYS_PFN(hole);

	return PFN_PHYS((max_pfn - base_pfn - hole_pfns) / nr_nodes);
}

/*
 * Sets up fake nodes of `size' interleaved over physical nodes ranging from
 * `addr' to `max_addr'.
 *
 * Returns node ID of the next node on success or negative error code.
 */
/*
 * 译注：按固定 size 或对单个物理块严格均分的方式，轮转构造模拟节点；成功返回下一
 * 个可用 nid，失败返回负值。
 * 业务背景：同时承载 numa=fake=<size> 与 numa=fake=<N>U；前者跨全机按净 RAM 下限
 * 切块，后者忽略洞、严格按指定物理块地址容量均分。
 * 入参：ei/pi 为输出和剩余物理布局；addr/max_addr 是字节范围；size 为固定模式目标；
 * nr_nodes/pblk 仅 uniform 模式使用；nid 是起始模拟编号。
 * 出参/返回：成功追加块、消费 pi 并返回递增后的 nid；非法组合或块表失败返回负值，
 * 候选修改由上层整体丢弃。
 * 注意事项：uniform 要求 nr_nodes 与 pblk 同时有效；模拟 nid 用 MAX_NUMNODES 取模；
 * 启动期独占，不睡眠。
 */
static int __init split_nodes_size_interleave_uniform(struct numa_meminfo *ei,
					      struct numa_meminfo *pi,
					      u64 addr, u64 max_addr, u64 size,
					      int nr_nodes, struct numa_memblk *pblk,
					      int nid)
{
	nodemask_t physnode_mask = numa_nodes_parsed;
	int i, ret, uniform = 0;
	u64 min_size;

	/* 两种模式必须恰有可用的大小来源，避免除零或解引用空物理块。 */
	if ((!size && !nr_nodes) || (nr_nodes && !pblk))
		return -1;

	/*
	 * In the 'uniform' case split the passed in physical node by
	 * nr_nodes, in the non-uniform case, ignore the passed in
	 * physical block and try to create nodes of at least size
	 * @size.
	 *
	 * In the uniform case, split the nodes strictly by physical
	 * capacity, i.e. ignore holes. In the non-uniform case account
	 * for holes and treat @size as a minimum floor.
	 */
	/*
	 * 译注：uniform 只遍历 pblk 所属物理 nid，并按地址容量等分；非 uniform 遍历全部
	 * 物理节点、扣除洞，把 @size 当作每块净 RAM 的最低值。
	 */
	if (!nr_nodes)
		nr_nodes = MAX_NUMNODES;
	else {
		nodes_clear(physnode_mask);
		node_set(pblk->nid, physnode_mask);
		uniform = 1;
	}

	/* 先求可接受最小值，再提升到 32MiB 和 MAX_NUMNODES 两项约束共同决定的下限。 */
	if (uniform) {
		min_size = uniform_size(max_addr, addr, 0, nr_nodes);
		size = min_size;
	} else {
		/*
		 * The limit on emulated nodes is MAX_NUMNODES, so the
		 * size per node is increased accordingly if the
		 * requested size is too small.  This creates a uniform
		 * distribution of node sizes across the entire machine
		 * (but not necessarily over physical nodes).
		 */
		/*
		 * 译注：模拟节点总数不能超过 MAX_NUMNODES；请求 size 太小时，提高到把全机
		 * 净 RAM 至多分成该数量所需的平均值，避免 nid 取模后反复产生过多块。
		 */
		min_size = uniform_size(max_addr, addr,
				mem_hole_size(addr, max_addr), nr_nodes);
	}
	min_size = ALIGN(max(min_size, FAKE_NODE_MIN_SIZE), FAKE_NODE_MIN_SIZE);
	if (size < min_size) {
		pr_err("Fake node size %LuMB too small, increasing to %LuMB\n",
			size / SZ_1M, min_size / SZ_1M);
		size = min_size;
	}
	/* 最终切分粒度固定向下对齐，尾余量由后面的块尾合并策略吸收。 */
	size = ALIGN_DOWN(size, FAKE_NODE_MIN_SIZE);

	/*
	 * Fill physical nodes with fake nodes of size until there is no memory
	 * left on any of them.
	 */
	/* 译注：与节点数模式相同，逐物理 nid 轮转，直到候选 pi 被完全消费。 */
	while (!nodes_empty(physnode_mask)) {
		for_each_node_mask(i, physnode_mask) {
			u64 dma32_end = numa_emu_dma_end();
			u64 start, limit, end;
			int phys_blk;

			/* 找不到剩余块便永久移除此物理 nid，否则本轮从该块头继续消费。 */
			phys_blk = emu_find_memblk_by_nid(i, pi);
			if (phys_blk < 0) {
				node_clear(i, physnode_mask);
				continue;
			}

			start = pi->blk[phys_blk].start;
			limit = pi->blk[phys_blk].end;

			/* uniform 按地址等分；普通模式扩张越过洞以满足净容量。 */
			if (uniform)
				end = start + size;
			else
				end = find_end_of_node(start, limit, size);
			/*
			 * If there won't be at least FAKE_NODE_MIN_SIZE of
			 * non-reserved memory in ZONE_DMA32 for the next node,
			 * this one must extend to the boundary.
			 */
			/* 译注：避免在 DMA32 边界前留下小于最小块的不可分尾巴。 */
			if (end < dma32_end && dma32_end - end -
			    mem_hole_size(end, dma32_end) < FAKE_NODE_MIN_SIZE)
				end = dma32_end;

			/*
			 * If there won't be enough non-reserved memory for the
			 * next node, this one must extend to the end of the
			 * physical node.
			 */
			/* 译注：只有非 uniform 才吞并不足 size 的物理尾段；uniform 保持严格份数。 */
			if ((limit - end - mem_hole_size(end, limit) < size)
					&& !uniform)
				end = limit;

			/* setup 是唯一提交点；失败由顶层放弃整个 ei，成功则继续下个物理块。 */
			ret = emu_setup_memblk(ei, pi, nid++ % MAX_NUMNODES,
					       phys_blk,
					       min(end, limit) - start);
			if (ret < 0)
				return ret;
		}
	}
	return nid;
}

/*
 * split_nodes_size_interleave() - 固定净容量模式的薄包装。
 * 业务背景：numa=fake=<size> 不限定单个物理块或节点数，复用通用切分器。
 * 入参：ei/pi 与地址范围同通用函数；size 是请求的最小净字节数。
 * 出参/返回：透传下一 nid 或负错误；副作用和回滚归通用函数/顶层负责。
 * 注意事项：传 nr_nodes=0 明确选择非 uniform 模式，启动期不睡眠。
 */
static int __init split_nodes_size_interleave(struct numa_meminfo *ei,
					      struct numa_meminfo *pi,
					      u64 addr, u64 max_addr, u64 size)
{
	return split_nodes_size_interleave_uniform(ei, pi, addr, max_addr, size,
			0, NULL, 0);
}

/*
 * setup_emu2phys_nid() - 汇总有效模拟 nid 上界并选择默认物理归属。
 * 业务背景：有编号空洞或后续 PXM 节点时仍需完整映射，顶层用首个物理 nid 填洞。
 * 入参：dfl_phys_nid 是必非 NULL 输出指针，调用前内容无意义。
 * 出参/返回：写入首个有效物理 nid（无映射则 NUMA_NO_NODE），返回最大有效模拟 nid。
 * 注意事项：只读取启动期映射数组，无引用/锁/睡眠；调用者应保证至少构造一块。
 */
static int __init setup_emu2phys_nid(int *dfl_phys_nid)
{
	int i, max_emu_nid = 0;

	*dfl_phys_nid = NUMA_NO_NODE;
	/* 全数组扫描同时找上界和首个可作为空洞回退的真实节点。 */
	for (i = 0; i < ARRAY_SIZE(emu_nid_to_phys); i++) {
		if (emu_nid_to_phys[i] != NUMA_NO_NODE) {
			max_emu_nid = i;
			if (*dfl_phys_nid == NUMA_NO_NODE)
				*dfl_phys_nid = emu_nid_to_phys[i];
		}
	}

	return max_emu_nid;
}

/**
 * numa_emulation - Emulate NUMA nodes
 * @numa_meminfo: NUMA configuration to massage
 * @numa_dist_cnt: The size of the physical NUMA distance table
 *
 * Emulate NUMA nodes according to the numa=fake kernel parameter.
 * @numa_meminfo contains the physical memory configuration and is modified
 * to reflect the emulated configuration on success.  @numa_dist_cnt is
 * used to determine the size of the physical distance table.
 *
 * On success, the following modifications are made.
 *
 * - @numa_meminfo is updated to reflect the emulated nodes.
 *
 * - __apicid_to_node[] is updated such that APIC IDs are mapped to the
 *   emulated nodes.
 *
 * - NUMA distance table is rebuilt to represent distances between emulated
 *   nodes.  The distances are determined considering how emulated nodes
 *   are mapped to physical nodes and match the actual distances.
 *
 * - emu_nid_to_phys[] reflects how emulated nodes are mapped to physical
 *   nodes.  This is used by numa_add_cpu() and numa_remove_cpu().
 *
 * If emulation is not enabled or fails, emu_nid_to_phys[] is filled with
 * identity mapping and no other modification is made.
 */
/*
 * 译注：按 numa=fake 参数模拟 NUMA；成功时替换 meminfo、CPU/APIC 映射和距离表，并
 * 建立 emu_nid_to_phys；未启用或失败时只建立恒等映射，保留原拓扑。
 * 业务背景：x86 NUMA 初始化在把 memblock/距离/CPU 拓扑发布给通用层之前调用本函数，
 * 候选布局在局部副本中构造，直到校验通过才提交。
 * 入参：numa_meminfo 是必非 NULL 的输入输出物理布局，成功后被模拟布局替换；
 * numa_dist_cnt 是原物理距离表的边长/节点数，可为 0。
 * 出参/返回：无直接返回值；成功重建全局节点集合、PXM、CPU 映射和距离，失败保持输入
 * meminfo 不变并令映射为 identity；临时距离副本来自 memblock，成功路径释放。
 * 注意事项：仅启动期单线程、可调用 memblock 分配；commit 后的后续步骤会修改全局状态，
 * 当前代码失败标签不撤销已完成的 PXM/parsed-node 更改，调用顺序依赖这些 helper 成功。
 */
void __init numa_emulation(struct numa_meminfo *numa_meminfo, int numa_dist_cnt)
{
	static struct numa_meminfo ei __initdata;
	static struct numa_meminfo pi __initdata;
	const u64 max_addr = PFN_PHYS(max_pfn);
	u8 *phys_dist = NULL;
	size_t phys_size = numa_dist_cnt * numa_dist_cnt * sizeof(phys_dist[0]);
	int max_emu_nid, dfl_phys_nid;
	int i, j, ret;
	nodemask_t physnode_mask = numa_nodes_parsed;

	/* 未提供参数时不构造候选，直接为 CPU 热插拔准备恒等映射。 */
	if (!emu_cmdline)
		goto no_emu;

	/* ei 是空候选，pi 是可破坏消费的物理副本，原输入保持到 commit。 */
	memset(&ei, 0, sizeof(ei));
	pi = *numa_meminfo;

	for (i = 0; i < MAX_NUMNODES; i++)
		emu_nid_to_phys[i] = NUMA_NO_NODE;

	/*
	 * If the numa=fake command-line contains a 'M' or 'G', it represents
	 * the fixed node size.  Otherwise, if it is just a single number N,
	 * split the system RAM into N fake nodes.
	 */
	/*
	 * 译注：含 U 表示每个物理节点严格拆为 N 份；含 M/G 表示固定大小；纯数字 N
	 * 表示全机交错为 N 个模拟节点。解析 helper 会推进 emu_cmdline 到可选距离参数。
	 */
	if (strchr(emu_cmdline, 'U')) {
		unsigned long n;
		int nid = 0, nr_created;

		n = simple_strtoul(emu_cmdline, &emu_cmdline, 0);
		ret = -1;
		/* 每个物理节点分别严格建 n 个模拟节点，nid 在节点间连续递增。 */
		for_each_node_mask(i, physnode_mask) {
			/*
			 * The reason we pass in blk[0] is due to
			 * numa_remove_memblk_from() called by
			 * emu_setup_memblk() will delete entry 0
			 * and then move everything else up in the pi.blk
			 * array. Therefore we should always be looking
			 * at blk[0].
			 */
			/*
			 * 译注：setup 耗尽 pi.blk[0] 时会删除它并左移数组，所以循环始终传当前
			 * 第 0 项，不能缓存后续下标。
			 */
			ret = split_nodes_size_interleave_uniform(&ei, &pi,
					pi.blk[0].start, pi.blk[0].end, 0,
					n, &pi.blk[0], nid);
			if (ret < 0)
				break;

			/*
			 * If no memory was found for this physical node,
			 * skip the under-allocation check.
			 */
			/* 译注：物理节点没有可用内存时不做“必须创建 n 个”的不足检查。 */
			if (ret == nid)
				continue;

			/* 严格均分必须为有内存的物理节点创建恰好 n 份，否则放弃整个模拟。 */
			nr_created = ret - nid;
			if (nr_created < n) {
				/* 部分创建不能满足 U 模式的拓扑承诺，记录物理 nid 后整体回退。 */
				pr_info("%s: phys: %d only got %d of %ld nodes, failing\n",
						__func__, i, nr_created, n);
				ret = -1;
				break;
			}
			/* 下一物理节点从尚未占用的连续模拟 nid 开始。 */
			nid = ret;
		}
	} else if (strchr(emu_cmdline, 'M') || strchr(emu_cmdline, 'G')) {
		u64 size;

		/* memparse 同时理解 M/G 后缀并把游标推进到可选冒号。 */
		size = memparse(emu_cmdline, &emu_cmdline);
		ret = split_nodes_size_interleave(&ei, &pi, 0, max_addr, size);
	} else {
		unsigned long n;

		/* 无单位后缀的纯数值表示期望模拟节点总数。 */
		n = simple_strtoul(emu_cmdline, &emu_cmdline, 0);
		ret = split_nodes_interleave(&ei, &pi, 0, max_addr, n);
	}
	/* 冒号后的整数序列按行优先顺序覆盖模拟距离矩阵。 */
	if (*emu_cmdline == ':')
		emu_cmdline++;

	if (ret < 0)
		goto no_emu;

	/* 提交前合并/裁剪并验证候选区间，任何非法布局都退回原物理拓扑。 */
	if (numa_cleanup_meminfo(&ei) < 0) {
		pr_warn("NUMA: Warning: constructed meminfo invalid, disabling emulation\n");
		goto no_emu;
	}

	/* copy the physical distance table */
	/* 译注：重置全局距离表前先复制物理矩阵；0 表示平台没有可复用的矩阵。 */
	if (numa_dist_cnt) {
		phys_dist = memblock_alloc(phys_size, PAGE_SIZE);
		if (!phys_dist) {
			pr_warn("NUMA: Warning: can't allocate copy of distance table, disabling emulation\n");
			goto no_emu;
		}

		/* node_distance() 此刻仍读取固件物理矩阵，逐项保存为启动期私有副本。 */
		for (i = 0; i < numa_dist_cnt; i++)
			for (j = 0; j < numa_dist_cnt; j++)
				phys_dist[i * numa_dist_cnt + j] =
					node_distance(i, j);
	}

	/*
	 * Determine the max emulated nid and the default phys nid to use
	 * for unmapped nodes.
	 */
	/* 译注：确定模拟编号上界，以及给编号空洞使用的首个物理节点。 */
	max_emu_nid = setup_emu2phys_nid(&dfl_phys_nid);

	/* Make sure numa_nodes_parsed only contains emulated nodes */
	/* 译注：从候选有效块重建 parsed mask，阻止旧物理 nid 泄漏到后续通用初始化。 */
	nodes_clear(numa_nodes_parsed);
	for (i = 0; i < ARRAY_SIZE(ei.blk); i++)
		if (ei.blk[i].start != ei.blk[i].end &&
		    ei.blk[i].nid != NUMA_NO_NODE)
			node_set(ei.blk[i].nid, numa_nodes_parsed);

	/* fix pxm_to_node_map[] and node_to_pxm_map[] to avoid collision
	 * with faked numa nodes, particularly during later memory hotplug
	 * handling, and also update numa_nodes_parsed accordingly.
	 */
	/*
	 * 译注：模拟 nid 会占用低编号，需重排固件 PXM 映射以避免热插拔节点碰撞，并同步
	 * parsed mask；失败尚未提交 meminfo，转 no_emu 恢复原 mask。
	 */
	ret = fix_pxm_node_maps(max_emu_nid);
	if (ret < 0)
		goto no_emu;

	/* commit */
	/* 译注：这是候选内存布局的提交点；从此通用 NUMA 层观察到模拟 memblk。 */
	*numa_meminfo = ei;

	/* 依据映射重写启动 CPU/APIC 到模拟节点的归属。 */
	numa_emu_update_cpu_to_node(emu_nid_to_phys, max_emu_nid + 1);

	/* make sure all emulated nodes are mapped to a physical node */
	/* 译注：编号空洞也填默认物理 nid，保证后续 CPU mask/距离查询不读 NO_NODE。 */
	for (i = 0; i < max_emu_nid + 1; i++)
		if (emu_nid_to_phys[i] == NUMA_NO_NODE)
			emu_nid_to_phys[i] = dfl_phys_nid;

	/* transform distance table */
	/*
	 * 译注：先为模拟节点对建立距离：命令行值优先；物理索引越界时退化为本地/远端
	 * 常量；否则继承它们各自承载物理节点间的真实距离。
	 */
	numa_reset_distance();
	/* 行优先遍历矩阵；get_option 每次消费一个显式距离，耗尽后自动走继承规则。 */
	for (i = 0; i < max_emu_nid + 1; i++) {
		for (j = 0; j < max_emu_nid + 1; j++) {
			int physi = emu_nid_to_phys[i];
			int physj = emu_nid_to_phys[j];
			int dist;

			if (get_option(&emu_cmdline, &dist) == 2)
				;
			/* 缺少可索引物理矩阵时，至少维持同物理节点为 local 的拓扑语义。 */
			else if (physi >= numa_dist_cnt || physj >= numa_dist_cnt)
				dist = physi == physj ?
					LOCAL_DISTANCE : REMOTE_DISTANCE;
			else
				dist = phys_dist[physi * numa_dist_cnt + physj];

			/* 每项立即写入新全局表；双层循环结束后模拟子矩阵完整发布。 */
			numa_set_distance(i, j, dist);
		}
	}
	/*
	 * PXM 修复可能在模拟 nid 后追加真实热插拔节点；第二遍只补“至少一端非模拟”的
	 * 节点对，把追加编号还原成物理矩阵下标，保留第一遍已确定的模拟节点对。
	 */
	for (i = 0; i < numa_distance_cnt; i++) {
		for (j = 0; j < numa_distance_cnt; j++) {
			int physi, physj;
			u8 dist;

			/* distance between fake nodes is already ok */
			/* 译注：两端已有物理映射即属于第一遍模拟子矩阵，避免覆盖命令行值。 */
			if (emu_nid_to_phys[i] != NUMA_NO_NODE &&
			    emu_nid_to_phys[j] != NUMA_NO_NODE)
				continue;
			/* 一端为追加真实节点时，其物理编号由模拟编号上界后的偏移恢复。 */
			if (emu_nid_to_phys[i] != NUMA_NO_NODE)
				physi = emu_nid_to_phys[i];
			else
				physi = i - max_emu_nid;
			if (emu_nid_to_phys[j] != NUMA_NO_NODE)
				physj = emu_nid_to_phys[j];
			else
				physj = j - max_emu_nid;
			/* 这里仅在存在原物理距离矩阵及追加节点的配置下执行有效索引。 */
			dist = phys_dist[physi * numa_dist_cnt + physj];
			numa_set_distance(i, j, dist);
		}
	}

	/* free the copied physical distance table */
	/* 译注：距离已经复制进全局矩阵，归还启动期临时 memblock；NULL/0 组合可安全释放。 */
	memblock_free(phys_dist, phys_size);
	return;

no_emu:
	/* 失败回退恢复进入函数时的物理 parsed mask，输入 meminfo 从未被候选构造改写。 */
	numa_nodes_parsed = physnode_mask;
	/* No emulation.  Build identity emu_nid_to_phys[] for numa_add_cpu() */
	/* 译注：即使禁用模拟，CPU 上下线统一接口仍要求每个 nid 有可用物理映射。 */
	for (i = 0; i < ARRAY_SIZE(emu_nid_to_phys); i++)
		emu_nid_to_phys[i] = i;
}

#ifndef CONFIG_DEBUG_PER_CPU_MAPS
/*
 * numa_add_cpu() - 把上线 CPU 加入其物理节点承载的全部模拟节点 mask。
 * 业务背景：一个物理节点可被拆成多个模拟节点，CPU 对这些节点具有相同 locality。
 * 入参：cpu 是正在上线的逻辑 CPU 编号；不取得对象引用。
 * 出参/返回：无；设置一个或多个 node_to_cpumask_map 位。
 * 注意事项：非调试配置要求 early 映射有效且节点在线，否则 BUG；由 CPU 热插拔串行化。
 */
void numa_add_cpu(unsigned int cpu)
{
	int physnid, nid;

	nid = early_cpu_to_node(cpu);
	BUG_ON(nid == NUMA_NO_NODE || !node_online(nid));

	/* 先恢复 CPU 的真实物理归属，再寻找共享该归属的全部模拟 nid。 */
	physnid = emu_nid_to_phys[nid];

	/*
	 * Map the cpu to each emulated node that is allocated on the physical
	 * node of the cpu's apic id.
	 */
	/* 译注：CPU APIC 所在物理节点上分配出的每个模拟节点都获得这个 CPU。 */
	for_each_online_node(nid)
		if (emu_nid_to_phys[nid] == physnid)
			cpumask_set_cpu(cpu, node_to_cpumask_map[nid]);
}

/*
 * numa_remove_cpu() - 从全部在线模拟节点的 CPU mask 删除下线 CPU。
 * 业务背景：上线可能把一个 CPU 发布到多个模拟节点，退出必须对称清除所有副本。
 * 入参：cpu 为正在下线的逻辑编号。出参/返回：无；清除对应 mask 位。
 * 注意事项：CPU 热插拔框架串行化更新，不需查询物理 nid，也不睡眠。
 */
void numa_remove_cpu(unsigned int cpu)
{
	int i;

	for_each_online_node(i)
		cpumask_clear_cpu(cpu, node_to_cpumask_map[i]);
}
#else	/* !CONFIG_DEBUG_PER_CPU_MAPS */
/* 译注：启用 CONFIG_DEBUG_PER_CPU_MAPS 时经校验 helper 统一执行增删。 */
/*
 * numa_set_cpumask() - 调试配置下校验并批量更新 CPU 的模拟节点归属。
 * 业务背景：add/remove 共享物理到模拟节点的展开逻辑，并让 debug helper 检查映射一致性。
 * 入参：cpu 为逻辑编号；enable=true 设置、false 清除。
 * 出参/返回：无；可能更新多个节点 mask，早期映射缺失时告警后不修改。
 * 注意事项：early_cpu_to_node 已负责告警/trace；热插拔上下文串行，不睡眠。
 */
static void numa_set_cpumask(unsigned int cpu, bool enable)
{
	int nid, physnid;

	nid = early_cpu_to_node(cpu);
	if (nid == NUMA_NO_NODE) {
		/* early_cpu_to_node() already emits a warning and trace */
		return;
	}

	/* 将物理归属相同的模拟节点视为同一 CPU locality 集合。 */
	physnid = emu_nid_to_phys[nid];

	for_each_online_node(nid) {
		if (emu_nid_to_phys[nid] != physnid)
			continue;

		debug_cpumask_set_cpu(cpu, nid, enable);
	}
}

/*
 * numa_add_cpu() - 调试配置下发布 CPU 到所有对应模拟节点。
 * 入参：cpu 为上线逻辑编号。出参/返回：无；委托 numa_set_cpumask(true)。
 * 注意事项：业务背景和热插拔约束同统一 helper。
 */
void numa_add_cpu(unsigned int cpu)
{
	numa_set_cpumask(cpu, true);
}

/*
 * numa_remove_cpu() - 调试配置下撤销 CPU 的全部模拟节点归属。
 * 入参：cpu 为下线逻辑编号。出参/返回：无；委托 numa_set_cpumask(false)。
 * 注意事项：必须与 add 对称，业务背景和同步约束同统一 helper。
 */
void numa_remove_cpu(unsigned int cpu)
{
	numa_set_cpumask(cpu, false);
}
#endif	/* !CONFIG_DEBUG_PER_CPU_MAPS */
