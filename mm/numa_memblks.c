// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/array_size.h>
#include <linux/sort.h>
#include <linux/printk.h>
#include <linux/memblock.h>
#include <linux/numa.h>
#include <linux/numa_memblks.h>

#include <asm/numa.h>

/*
 * 学习提示：本文件在早期启动阶段把固件 NUMA 描述规范化为 memblock 节点归属。
 * 这些对象多带 __init 生命周期；清洗完成前不得供常规分配器当作最终拓扑使用。
 */
int numa_distance_cnt;
/* 距离矩阵按 [from * cnt + to] 展平，节点号决定所需边长。 */
static u8 *numa_distance;

nodemask_t numa_nodes_parsed __initdata;

static struct numa_meminfo numa_meminfo __initdata_or_meminfo;
/* 保留区单独记忆，以便热插拔时仍能恢复固件给出的目标节点。 */
static struct numa_meminfo numa_reserved_meminfo __initdata_or_meminfo;

/*
 * Set nodes, which have memory in @mi, in *@nodemask.
 */
/*
 * 业务背景：启动期需要从固件内存块补齐“已解析节点”集合，兼容有内存但无 CPU 的节点。
 * 入参：nodemask 是可修改输出集合；mi 是 __init 阶段稳定的 meminfo 借用对象。
 * 出参/返回：void；把所有非空且 nid 有效的块对应位并入 nodemask，不清除原有位。
 * 注意事项：只读取固定数组，不依赖 nr_blks，空槽必须以零长度或 NUMA_NO_NODE 表示。
 */
static void __init numa_nodemask_from_meminfo(nodemask_t *nodemask,
					      const struct numa_meminfo *mi)
{
	int i;

	/* 空区间和 NUMA_NO_NODE 都不构成有效的有内存节点。 */
	for (i = 0; i < ARRAY_SIZE(mi->blk); i++)
		if (mi->blk[i].start != mi->blk[i].end &&
		    mi->blk[i].nid != NUMA_NO_NODE)
			node_set(mi->blk[i].nid, *nodemask);
}

/**
 * numa_reset_distance - Reset NUMA distance table
 *
 * The current table is freed.  The next numa_set_distance() call will
 * create a new one.
 */
/*
 * 业务背景：固件解析重试或 NUMA 初始化重建前，必须释放旧距离矩阵并恢复惰性创建状态。
 * 入参：无；使用启动期全局 numa_distance 与 numa_distance_cnt。
 * 出参/返回：void；真实矩阵归还 memblock，计数清零且指针置 NULL。
 * 注意事项：分配失败用地址 1 作哨兵且 cnt 为零，绝不能对该哨兵调用 memblock_free。
 */
void __init numa_reset_distance(void)
{
	/* cnt 为零也用来识别分配失败哨兵 (void *)1，不能释放该地址。 */
	size_t size = numa_distance_cnt * numa_distance_cnt * sizeof(numa_distance[0]);

	/* numa_distance could be 1LU marking allocation failure, test cnt */
	if (numa_distance_cnt)
		memblock_free(numa_distance, size);
	/* 清零后 NULL 重新允许下一次 set_distance 触发分配。 */
	numa_distance_cnt = 0;
	numa_distance = NULL;	/* enable table creation */
}

/*
 * 业务背景：首次接收固件距离时，按当前已知最高 nid 惰性创建完整的展平距离矩阵。
 * 入参：无；读取已解析节点和 numa_meminfo，写全局距离表。
 * 出参/返回：成功返回 0 并填默认距离；分配失败返回 -ENOMEM 并安装不可释放哨兵。
 * 注意事项：边长是最高 nid 加一而非节点数；对象来自 memblock，仅在 __init 阶段有效操作。
 */
static int __init numa_alloc_distance(void)
{
	/* 同时考虑固件已解析节点与内存块节点，覆盖 CPU-only/memory-only 情形。 */
	nodemask_t nodes_parsed;
	size_t size;
	int i, j, cnt = 0;

	/* size the new table and allocate it */
	nodes_parsed = numa_nodes_parsed;
	numa_nodemask_from_meminfo(&nodes_parsed, &numa_meminfo);

	/* 矩阵边长取最高节点号加一，而非节点数量，支持稀疏 nid。 */
	for_each_node_mask(i, nodes_parsed)
		cnt = i;
	cnt++;
	size = cnt * cnt * sizeof(numa_distance[0]);

	/* 早期阶段由 memblock 分配并按页对齐，尚不能依赖 slab。 */
	numa_distance = memblock_alloc(size, PAGE_SIZE);
	if (!numa_distance) {
		pr_warn("Warning: can't allocate distance table!\n");
		/* don't retry until explicitly reset */
		/* 非 NULL 哨兵阻止重复告警和重复分配，reset 可显式解除。 */
		numa_distance = (void *)1LU;
		return -ENOMEM;
	}

	numa_distance_cnt = cnt;

	/* fill with the default distances */
	/* 固件未覆盖的单元先采用本地/远端标准默认值。 */
	for (i = 0; i < cnt; i++)
		for (j = 0; j < cnt; j++)
			numa_distance[i * cnt + j] = i == j ?
				LOCAL_DISTANCE : REMOTE_DISTANCE;
	pr_debug("NUMA: Initialized distance table, cnt=%d\n", cnt);

	return 0;
}

/**
 * numa_set_distance - Set NUMA distance from one NUMA to another
 * @from: the 'from' node to set distance
 * @to: the 'to'  node to set distance
 * @distance: NUMA distance
 *
 * Set the distance from node @from to @to to @distance.  If distance table
 * doesn't exist, one which is large enough to accommodate all the currently
 * known nodes will be created.
 *
 * If such table cannot be allocated, a warning is printed and further
 * calls are ignored until the distance table is reset with
 * numa_reset_distance().
 *
 * If @from or @to is higher than the highest known node or lower than zero
 * at the time of table creation or @distance doesn't make sense, the call
 * is ignored.
 * This is to allow simplification of specific NUMA config implementations.
 */
/*
 * 业务背景：架构解析器逐项提交 SLIT/固件距离，需要公共层校验后写入惰性分配矩阵。
 * 入参：from/to 是有方向的节点号；distance 必须能放入 u8，且对角值必须为 LOCAL_DISTANCE。
 * 出参/返回：void；合法项更新一个矩阵单元，非法、越界或既往分配失败均忽略。
 * 注意事项：矩阵创建后不扩容，距离可非对称；reset 前失败哨兵禁止后续重试。
 */
void __init numa_set_distance(int from, int to, int distance)
{
	/* 首次设置惰性创建矩阵，失败后哨兵会让后续调用静默返回。 */
	if (!numa_distance && numa_alloc_distance() < 0)
		return;

	/* 创建后的矩阵不扩容，超出当时已知最高 nid 的输入被忽略。 */
	if (from >= numa_distance_cnt || to >= numa_distance_cnt ||
			from < 0 || to < 0) {
		pr_warn_once("Warning: node ids are out of bound, from=%d to=%d distance=%d\n",
			     from, to, distance);
		return;
	}

	/* u8 可表示性和对角线 LOCAL_DISTANCE 是表的格式不变量。 */
	if ((u8)distance != distance ||
	    (from == to && distance != LOCAL_DISTANCE)) {
		pr_warn_once("Warning: invalid distance parameter, from=%d to=%d distance=%d\n",
			     from, to, distance);
		return;
	}

	/* 距离允许非对称，调用者需分别设置反向单元。 */
	numa_distance[from * numa_distance_cnt + to] = distance;
}

/*
 * 业务背景：调度和内存策略需要查询节点距离，即使固件表缺失或查询 nid 超出初始化边界也须可用。
 * 入参：from/to 是待比较节点号，调用者应传非负 nid。
 * 出参/返回：表内返回固件/默认单元；表外按同节点 LOCAL、异节点 REMOTE 降级。
 * 注意事项：只读全局表，不扩容也不告警；正常运行期初始化已完成，表内容视为不变。
 */
int __node_distance(int from, int to)
{
	/* 表外查询退回标准值，避免早期或残缺固件数据导致越界。 */
	if (from >= numa_distance_cnt || to >= numa_distance_cnt)
		return from == to ? LOCAL_DISTANCE : REMOTE_DISTANCE;
	return numa_distance[from * numa_distance_cnt + to];
}
EXPORT_SYMBOL(__node_distance);

/*
 * 业务背景：各固件解析后端把物理区间积累到指定 NUMA meminfo，稍后统一清洗和注册。
 * 入参：nid 是归属节点；start/end 构成半开区间；mi 是目标固定容量数组。
 * 出参/返回：追加成功或零长度返回 0；容量耗尽返回 -EINVAL；畸形区间告警后按兼容策略返回 0。
 * 注意事项：本函数不排序、不合并；成功会增加 nr_blks，目标对象必须处于 __init 可写期。
 */
static int __init numa_add_memblk_to(int nid, u64 start, u64 end,
				     struct numa_meminfo *mi)
{
	/* 区间采用半开 [start,end)，零长度是合法的无操作。 */
	/* ignore zero length blks */
	if (start == end)
		return 0;

	/* whine about and ignore invalid blks */
	/* 错误固件块告警后忽略，保持其余可用拓扑继续启动。 */
	if (start > end || nid < 0 || nid >= MAX_NUMNODES) {
		pr_warn("Warning: invalid memblk node %d [mem %#010Lx-%#010Lx]\n",
			nid, start, end - 1);
		return 0;
	}

	/* 固定数组溢出无法安全降级，因此返回错误给解析入口。 */
	if (mi->nr_blks >= NR_NODE_MEMBLKS) {
		pr_err("too many memblk ranges\n");
		return -EINVAL;
	}

	/* 追加阶段不排序不合并，统一留给 cleanup 做冲突检查。 */
	mi->blk[mi->nr_blks].start = start;
	mi->blk[mi->nr_blks].end = end;
	mi->blk[mi->nr_blks].nid = nid;
	mi->nr_blks++;
	return 0;
}

/**
 * numa_remove_memblk_from - Remove one numa_memblk from a numa_meminfo
 * @idx: Index of memblk to remove
 * @mi: numa_meminfo to remove memblk from
 *
 * Remove @idx'th numa_memblk from @mi by shifting @mi->blk[] and
 * decrementing @mi->nr_blks.
 */
/*
 * 业务背景：清洗或迁移 memblk 时需保持固定数组前缀连续，避免后续循环读到空洞。
 * 入参：idx 必须落在 mi 的有效前缀内；mi 是调用者独占修改的启动期对象。
 * 出参/返回：void；删除 idx、左移尾部并将 nr_blks 减一，不清理新尾槽。
 * 注意事项：不做边界检查，调用者负责索引有效；尾槽由 cleanup 末尾统一清零。
 */
void __init numa_remove_memblk_from(int idx, struct numa_meminfo *mi)
{
	/* 先减计数再按剩余尾长左移，idx 后所有元素保持连续。 */
	mi->nr_blks--;
	memmove(&mi->blk[idx], &mi->blk[idx + 1],
		(mi->nr_blks - idx) * sizeof(mi->blk[0]));
}

/**
 * numa_move_tail_memblk - Move a numa_memblk from one numa_meminfo to another
 * @dst: numa_meminfo to append block to
 * @idx: Index of memblk to remove
 * @src: numa_meminfo to remove memblk from
 */
/*
 * 业务背景：清洗阶段把非 RAM 或高端保留区的 NUMA 归属从工作集合转存到持久保留集合。
 * 入参：dst 是有剩余容量的目标；idx 指定 src 有效元素；src/dst 由调用者独占。
 * 出参/返回：void；先复制到 dst 尾部，再从 src 删除，区间 ownership 完成转移。
 * 注意事项：内部不检查容量且 src/dst 不应相同；删除会移动 src 后续元素。
 */
static void __init numa_move_tail_memblk(struct numa_meminfo *dst, int idx,
					 struct numa_meminfo *src)
{
	/* 目标追加副本后从源删除；调用者需确保目标仍有容量。 */
	dst->blk[dst->nr_blks++] = src->blk[idx];
	numa_remove_memblk_from(idx, src);
}

/**
 * numa_add_memblk - Add one numa_memblk to numa_meminfo
 * @nid: NUMA node ID of the new memblk
 * @start: Start address of the new memblk
 * @end: End address of the new memblk
 *
 * Add a new memblk to the default numa_meminfo.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/*
 * 业务背景：普通固件 RAM 描述需要进入默认 numa_meminfo，供后续裁剪、合并与 memblock 注册。
 * 入参：nid 与 start/end 半开区间直接来自架构解析器。
 * 出参/返回：透传内部追加结果；成功可能新增一个工作内存块。
 * 注意事项：只在早期初始化调用，不立即改变 memblock.memory 的节点归属。
 */
int __init numa_add_memblk(int nid, u64 start, u64 end)
{
	/* 普通固件内存描述进入默认工作集合。 */
	return numa_add_memblk_to(nid, start, end, &numa_meminfo);
}

/**
 * numa_add_reserved_memblk - Add one numa_memblk to numa_reserved_meminfo
 * @nid: NUMA node ID of the new memblk
 * @start: Start address of the new memblk
 * @end: End address of the new memblk
 *
 * Add a new memblk to the numa_reserved_meminfo.
 *
 * Usage Case: numa_cleanup_meminfo() reconciles all numa_memblk instances
 * against memblock_type information and moves any that intersect reserved
 * ranges to numa_reserved_meminfo. However, when that information is known
 * ahead of time, we use numa_add_reserved_memblk() to add the numa_memblk
 * to numa_reserved_meminfo directly.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/*
 * 业务背景：已知非在线 RAM/保留物理区仍需保存目标 nid，供热添加与物理地址查询恢复归属。
 * 入参：nid 与 start/end 半开区间描述已知保留范围。
 * 出参/返回：透传追加结果；成功把描述写入 numa_reserved_meminfo。
 * 注意事项：直接进入保留集合，避免 cleanup 再从普通集合识别和搬移；容量与输入规则相同。
 */
int __init numa_add_reserved_memblk(int nid, u64 start, u64 end)
{
	/* 已知保留区直接旁路后续从 RAM 集合剥离的识别步骤。 */
	return numa_add_memblk_to(nid, start, end, &numa_reserved_meminfo);
}

/**
 * numa_cleanup_meminfo - Cleanup a numa_meminfo
 * @mi: numa_meminfo to clean up
 *
 * Sanitize @mi by merging and removing unnecessary memblks.  Also check for
 * conflicts and clear unused memblks.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/*
 * 业务背景：固件块可能越界、重叠、分裂或覆盖保留区，注册前必须规范化为无跨节点冲突的集合。
 * 入参：mi 是调用者独占的启动期 meminfo，通常为默认工作集合。
 * 出参/返回：成功返回 0 并原地裁剪/合并/清尾；跨 nid 冲突返回 -EINVAL，部分早期整理已生效。
 * 注意事项：非 RAM 信息会转存全局 reserved 集合；同 nid 可跨不含他节点内存的空洞合并。
 */
int __init numa_cleanup_meminfo(struct numa_meminfo *mi)
{
	/* low/high 是 memblock 当前可用 DRAM 的全局包络。 */
	const u64 low = memblock_start_of_DRAM();
	const u64 high = memblock_end_of_DRAM();
	int i, j, k;

	/* first, trim all entries */
	/* 第一阶段裁剪范围并把完全不与 RAM 相交的描述移到保留集合。 */
	for (i = 0; i < mi->nr_blks; i++) {
		struct numa_memblk *bi = &mi->blk[i];

		/* move / save reserved memory ranges */
		/* i-- 重新检查左移到当前位置的下一项。 */
		if (!memblock_overlaps_region(&memblock.memory,
					bi->start, bi->end - bi->start)) {
			numa_move_tail_memblk(&numa_reserved_meminfo, i--, mi);
			continue;
		}

		/* make sure all non-reserved blocks are inside the limits */
		/* 低端截到 DRAM 起点，高端超出部分仍保存其 NUMA 归属。 */
		bi->start = max(bi->start, low);

		/* preserve info for non-RAM areas above 'max_pfn': */
		/* max_pfn 之外可能是可热添加内存，不能简单丢弃信息。 */
		if (bi->end > high) {
			numa_add_memblk_to(bi->nid, high, bi->end,
					   &numa_reserved_meminfo);
			bi->end = high;
		}

		/* and there's no empty block */
		/* 裁剪后空区间立即删除，防止进入合并与注册阶段。 */
		if (bi->start >= bi->end)
			numa_remove_memblk_from(i--, mi);
	}

	/* merge neighboring / overlapping entries */
	/* 第二阶段两两检查重叠，并尝试合并同节点区间。 */
	for (i = 0; i < mi->nr_blks; i++) {
		struct numa_memblk *bi = &mi->blk[i];

		for (j = i + 1; j < mi->nr_blks; j++) {
			struct numa_memblk *bj = &mi->blk[j];
			u64 start, end;

			/*
			 * See whether there are overlapping blocks.  Whine
			 * about but allow overlaps of the same nid.  They
			 * will be merged below.
			 */
			/* 不同 nid 的物理重叠不可消解，同 nid 重叠可由合并修复。 */
			if (bi->end > bj->start && bi->start < bj->end) {
				if (bi->nid != bj->nid) {
					/* 跨节点重叠会让同一 PFN 拥有两个 nid，只能拒绝。 */
					pr_err("node %d [mem %#010Lx-%#010Lx] overlaps with node %d [mem %#010Lx-%#010Lx]\n",
					       bi->nid, bi->start, bi->end - 1,
					       bj->nid, bj->start, bj->end - 1);
					return -EINVAL;
				}
				pr_warn("Warning: node %d [mem %#010Lx-%#010Lx] overlaps with itself [mem %#010Lx-%#010Lx]\n",
					bi->nid, bi->start, bi->end - 1,
					bj->start, bj->end - 1);
			}

			/*
			 * Join together blocks on the same node, holes
			 * between which don't overlap with memory on other
			 * nodes.
			 */
			/* 仅同节点候选才允许跨中间空洞扩展包络。 */
			if (bi->nid != bj->nid)
				continue;
			start = min(bi->start, bj->start);
			end = max(bi->end, bj->end);
			/* 若包络会吞入其他节点内存，则必须保留为两个分离区间。 */
			for (k = 0; k < mi->nr_blks; k++) {
				struct numa_memblk *bk = &mi->blk[k];

				/* 自身节点块不会阻止同节点包络合并。 */
				if (bi->nid == bk->nid)
					continue;
				if (start < bk->end && end > bk->start)
					break;
			}
			if (k < mi->nr_blks)
				continue;
			pr_info("NUMA: Node %d [mem %#010Lx-%#010Lx] + [mem %#010Lx-%#010Lx] -> [mem %#010Lx-%#010Lx]\n",
			       bi->nid, bi->start, bi->end - 1, bj->start,
			       bj->end - 1, start, end - 1);
			/* 合并写回 bi 并删除 bj；j-- 复查左移的新候选。 */
			bi->start = start;
			bi->end = end;
			numa_remove_memblk_from(j--, mi);
		}
	}

	/* clear unused ones */
	/* 清空尾槽避免旧数据被调试或持久 meminfo 消费者误读。 */
	for (i = mi->nr_blks; i < ARRAY_SIZE(mi->blk); i++) {
		mi->blk[i].start = mi->blk[i].end = 0;
		mi->blk[i].nid = NUMA_NO_NODE;
	}

	return 0;
}

/*
 * Mark all currently memblock-reserved physical memory (which covers the
 * kernel's own memory ranges) as hot-unswappable.
 */
/*
 * 业务背景：含内核保留页的节点不能整体热拔，需把其所有内存从可热插拔集合中排除。
 * 入参：无；读取已清洗 numa_meminfo，并原地规范化 memblock.reserved/memory。
 * 出参/返回：void；为 reserved 区补 nid，构造节点掩码并清除这些节点所有区间的 HOTPLUG 标志。
 * 注意事项：按节点而非单个保留区降级热拔能力；memblock_set_node 异常只 WARN，初始化继续。
 */
static void __init numa_clear_kernel_node_hotplug(void)
{
	/* 只要节点含内核保留内存，就把整个节点视作不可完整热拔。 */
	nodemask_t reserved_nodemask = NODE_MASK_NONE;
	struct memblock_region *mb_region;
	int i;

	/*
	 * We have to do some preprocessing of memblock regions, to
	 * make them suitable for reservation.
	 *
	 * At this time, all memory regions reserved by memblock are
	 * used by the kernel, but those regions are not split up
	 * along node boundaries yet, and don't necessarily have their
	 * node ID set yet either.
	 *
	 * So iterate over all parsed memory blocks and use those ranges to
	 * set the nid in memblock.reserved.  This will split up the
	 * memblock regions along node boundaries and will set the node IDs
	 * as well.
	 */
	/* 先按 NUMA 边界切分 reserved 区并补齐其 nid。 */
	for (i = 0; i < numa_meminfo.nr_blks; i++) {
		struct numa_memblk *mb = numa_meminfo.blk + i;
		int ret;

		/* memblock_set_node 会切分交叠 region，WARN 暴露早期描述异常。 */
		ret = memblock_set_node(mb->start, mb->end - mb->start,
					&memblock.reserved, mb->nid);
		WARN_ON_ONCE(ret);
	}

	/*
	 * Now go over all reserved memblock regions, to construct a
	 * node mask of all kernel reserved memory areas.
	 *
	 * [ Note, when booting with mem=nn[kMG] or in a kdump kernel,
	 *   numa_meminfo might not include all memblock.reserved
	 *   memory ranges, because quirks such as trim_snb_memory()
	 *   reserve specific pages for Sandy Bridge graphics. ]
	 */
	/* 再从规范化 reserved 表构造含内核占用的节点集合。 */
	for_each_reserved_mem_region(mb_region) {
		int nid = memblock_get_region_node(mb_region);

		if (numa_valid_node(nid))
			node_set(nid, reserved_nodemask);
	}

	/*
	 * Finally, clear the MEMBLOCK_HOTPLUG flag for all memory
	 * belonging to the reserved node mask.
	 *
	 * Note that this will include memory regions that reside
	 * on nodes that contain kernel memory - entire nodes
	 * become hot-unpluggable:
	 */
	/* 最后对这些节点的全部普通内存清除 HOTPLUG 标志。 */
	for (i = 0; i < numa_meminfo.nr_blks; i++) {
		struct numa_memblk *mb = numa_meminfo.blk + i;

		if (!node_isset(mb->nid, reserved_nodemask))
			continue;

		memblock_clear_hotplug(mb->start, mb->end - mb->start);
	}
}

/*
 * 业务背景：清洗后的 NUMA 描述要正式发布到 node_possible_map 与 memblock，供页分配和 pfn→nid 使用。
 * 入参：mi 是已无冲突的启动期 meminfo 借用对象。
 * 出参/返回：成功返回 0；无 possible 节点或 section 粒度无法表达边界时返回 -EINVAL。
 * 注意事项：失败前可能已写 memblock 节点和热插拔标志；调用者处于不可并发的早期启动阶段。
 */
static int __init numa_register_meminfo(struct numa_meminfo *mi)
{
	/* 注册把已清洗的临时描述正式写入 node_possible_map 和 memblock。 */
	int i;

	/* Account for nodes with cpus and no memory */
	/* possible 同时包含有 CPU 无内存和有内存无 CPU 的节点。 */
	node_possible_map = numa_nodes_parsed;
	numa_nodemask_from_meminfo(&node_possible_map, mi);
	if (WARN_ON(nodes_empty(node_possible_map)))
		return -EINVAL;

	/* 每个半开物理区间的 nid 写回 memblock.memory。 */
	for (i = 0; i < mi->nr_blks; i++) {
		struct numa_memblk *mb = &mi->blk[i];

		memblock_set_node(mb->start, mb->end - mb->start,
				  &memblock.memory, mb->nid);
	}

	/*
	 * At very early time, the kernel have to use some memory such as
	 * loading the kernel image. We cannot prevent this anyway. So any
	 * node the kernel resides in should be un-hotpluggable.
	 *
	 * And when we come here, alloc node data won't fail.
	 */
	/* 节点数据可分配后再处理内核驻留导致的不可热拔约束。 */
	numa_clear_kernel_node_hotplug();

	/*
	 * If sections array is gonna be used for pfn -> nid mapping, check
	 * whether its granularity is fine enough.
	 */
	/* 若 page flags 不存 nid，section 映射粒度必须足以表达节点边界。 */
	if (IS_ENABLED(NODE_NOT_IN_PAGE_FLAGS)) {
		unsigned long pfn_align = node_map_pfn_alignment();

		/* 节点对齐细于 section 时 pfn->nid 会含混，拒绝该拓扑。 */
		if (pfn_align && pfn_align < PAGES_PER_SECTION) {
			unsigned long node_align_mb = PFN_PHYS(pfn_align) / SZ_1M;

			/* 统一换算为 MiB，使告警直接呈现两种映射粒度。 */
			unsigned long sect_align_mb = PFN_PHYS(PAGES_PER_SECTION) / SZ_1M;

			pr_warn("Node alignment %luMB < min %luMB, rejecting NUMA config\n",
				node_align_mb, sect_align_mb);
			return -EINVAL;
		}
	}

	/* 到此 memblock 节点标注与 pfn->nid 表示能力均已验证。 */
	return 0;
}

/*
 * 业务背景：通用 NUMA 启动入口协调架构解析、距离重建、块清洗、模拟改写和最终注册。
 * 入参：init_func 填充全局解析状态；memblock_force_top_down 指示解析后是否恢复 top-down 分配。
 * 出参/返回：成功返回最终注册结果 0；任一解析、清洗或注册错误原样返回负 errno。
 * 注意事项：先把全部地址 nid 重置为 NUMA_NO_NODE；回调可能改分配方向，距离/掩码旧状态被销毁。
 */
int __init numa_memblks_init(int (*init_func)(void),
			     bool memblock_force_top_down)
{
	/* 入口先撤销旧拓扑，让架构回调从统一的未归属状态重新解析。 */
	phys_addr_t max_addr = (phys_addr_t)ULLONG_MAX;
	int ret;

	/* 三类节点掩码、工作 meminfo 与两个 memblock 类型均被复位。 */
	nodes_clear(numa_nodes_parsed);
	nodes_clear(node_possible_map);
	nodes_clear(node_online_map);
	/* 工作数组清零后 nr_blks 也回到零，旧解析结果不会残留。 */
	memset(&numa_meminfo, 0, sizeof(numa_meminfo));
	WARN_ON(memblock_set_node(0, max_addr, &memblock.memory, NUMA_NO_NODE));
	WARN_ON(memblock_set_node(0, max_addr, &memblock.reserved,
				  NUMA_NO_NODE));
	/* In case that parsing SRAT failed. */
	WARN_ON(memblock_clear_hotplug(0, max_addr));
	/* 距离表也必须与本轮解析的最高 nid 同步重建。 */
	numa_reset_distance();

	/* 架构/固件回调填充节点、内存块和距离，负值直接终止。 */
	ret = init_func();
	if (ret < 0)
		return ret;

	/*
	 * We reset memblock back to the top-down direction
	 * here because if we configured ACPI_NUMA, we have
	 * parsed SRAT in init_func(). It is ok to have the
	 * reset here even if we didn't configure ACPI_NUMA
	 * or acpi numa init fails and fallbacks to dummy
	 * numa init.
	 */
	/* SRAT 解析可能临时改为 bottom-up，此处恢复调用者要求的方向。 */
	if (memblock_force_top_down)
		memblock_set_bottom_up(false);

	/* 先规范化真实拓扑，再允许 NUMA emulation 改写它。 */
	ret = numa_cleanup_meminfo(&numa_meminfo);
	if (ret < 0)
		return ret;

	/* 模拟层接收清洗后的块与原距离边长，最终结果再注册。 */
	numa_emulation(&numa_meminfo, numa_distance_cnt);

	return numa_register_meminfo(&numa_meminfo);
}

/*
 * 业务背景：填洞逻辑需按物理起点遍历 memblk 指针，但不能移动 numa_meminfo 的实体数组。
 * 入参：a/b 各指向一个“numa_memblk 指针”元素，目标对象在排序期间稳定。
 * 出参/返回：a 起点小于/等于/大于 b 时返回负/零/正值。
 * 注意事项：用关系表达式避免 u64 相减溢出；只比较 start，相同起点次序未定义。
 */
static int __init cmp_memblk(const void *a, const void *b)
{
	/* sort 数组保存的是指针，比较其目标块的起始物理地址。 */
	const struct numa_memblk *ma = *(const struct numa_memblk **)a;
	const struct numa_memblk *mb = *(const struct numa_memblk **)b;

	/* 布尔差避免直接相减 u64 产生整数溢出。 */
	return (ma->start > mb->start) - (ma->start < mb->start);
}

static struct numa_memblk *numa_memblk_list[NR_NODE_MEMBLKS] __initdata;

/**
 * numa_fill_memblks - Fill gaps in numa_meminfo memblks
 * @start: address to begin fill
 * @end: address to end fill
 *
 * Find and extend numa_meminfo memblks to cover the physical
 * address range @start-@end
 *
 * RETURNS:
 * 0		  : Success
 * NUMA_NO_MEMBLK : No memblks exist in address range @start-@end
 */

/*
 * 业务背景：某些固件只描述离散锚点，需要把目标物理范围内的 NUMA 块原地延展为连续覆盖。
 * 入参：start/end 是待覆盖半开区间，默认 numa_meminfo 已含候选锚点。
 * 出参/返回：找到锚点并完成填洞返回 0；区间内无任何 memblk 返回 NUMA_NO_MEMBLK。
 * 注意事项：空洞归属右侧块，首尾可越界延展；只排序全局临时指针数组，不移动实体块。
 */
int __init numa_fill_memblks(u64 start, u64 end)
{
	/* 该操作原地扩展交叠块，用于覆盖固件描述中的地址空洞。 */
	struct numa_memblk **blk = &numa_memblk_list[0];
	struct numa_meminfo *mi = &numa_meminfo;
	int count = 0;
	u64 prev_end;

	/*
	 * Create a list of pointers to numa_meminfo memblks that
	 * overlap start, end. The list is used to make in-place
	 * changes that fill out the numa_meminfo memblks.
	 */
	/* 只收集与目标半开区间相交的块，非交叠节点不参与填洞。 */
	for (int i = 0; i < mi->nr_blks; i++) {
		struct numa_memblk *bi = &mi->blk[i];

		if (memblock_addrs_overlap(start, end - start, bi->start,
					   bi->end - bi->start)) {
			blk[count] = &mi->blk[i];
			count++;
		}
	}
	/* 完全没有锚点时不能猜测空洞应归属哪个节点。 */
	if (!count)
		return NUMA_NO_MEMBLK;

	/* Sort the list of pointers in memblk->start order */
	/* 指针排序不移动 numa_meminfo 本体，只决定填洞遍历次序。 */
	sort(&blk[0], count, sizeof(blk[0]), cmp_memblk, NULL);

	/* Make sure the first/last memblks include start/end */
	/* 首尾分别向目标边界延伸，中间空洞在下一循环回填。 */
	blk[0]->start = min(blk[0]->start, start);
	blk[count - 1]->end = max(blk[count - 1]->end, end);

	/*
	 * Fill any gaps by tracking the previous memblks
	 * end address and backfilling to it if needed.
	 */
	prev_end = blk[0]->end;
	/* prev_end 单调前进；空洞归给右侧块并向左扩展到前一终点。 */
	for (int i = 1; i < count; i++) {
		struct numa_memblk *curr = blk[i];

		/* 重叠时只扩大已覆盖终点，分离时回填当前块 start。 */
		if (prev_end >= curr->start) {
			if (prev_end < curr->end)
				prev_end = curr->end;
		} else {
			curr->start = prev_end;
			prev_end = curr->end;
		}
	}
	/* 所有交叠锚点现已连续覆盖请求区间。 */
	return 0;
}

#ifdef CONFIG_NUMA_KEEP_MEMINFO
/*
 * 业务背景：保留启动期 meminfo 的配置下，需要用物理地址恢复 RAM 或 reserved 范围的原始 nid。
 * 入参：mi 是只读持久描述；start 是待查询的单个物理地址。
 * 出参/返回：首个包含 start 的半开区间 nid；没有匹配返回 NUMA_NO_NODE。
 * 注意事项：cleanup 已保证不同节点不重叠；函数不加锁，发布后 meminfo 必须保持不变。
 */
static int meminfo_to_nid(struct numa_meminfo *mi, u64 start)
{
	/* 持久 meminfo 使用半开区间查询单个物理地址。 */
	int i;

	/* 首个匹配即可返回；cleanup 已消除不同节点重叠。 */
	for (i = 0; i < mi->nr_blks; i++)
		if (mi->blk[i].start <= start && mi->blk[i].end > start)
			return mi->blk[i].nid;
	return NUMA_NO_NODE;
}

/*
 * 业务背景：设备/内存热插拔根据物理地址选择目标节点，并优先恢复保留范围记录的固件归属。
 * 入参：start 是待定位物理地址。
 * 出参/返回：若 reserved 命中返回其 nid，否则返回在线 RAM nid；两者均未命中返回 NUMA_NO_NODE。
 * 注意事项：reserved 与在线描述同时命中时 reserved 优先；只读持久 meminfo，无引用转移。
 */
int phys_to_target_node(u64 start)
{
	/* 同时查询在线 RAM 描述与启动期保存的 reserved 描述。 */
	int nid = meminfo_to_nid(&numa_meminfo, start);
	int reserved_nid = meminfo_to_nid(&numa_reserved_meminfo, start);

	/*
	 * Prefer online nodes unless the address is also described
	 * by reserved ranges, in which case use the reserved nid.
	 */
	/* 地址若也落入 reserved，优先保留其原始目标 nid。 */
	if (nid != NUMA_NO_NODE && reserved_nid == NUMA_NO_NODE)
		return nid;

	return reserved_nid;
}
EXPORT_SYMBOL_GPL(phys_to_target_node);

/*
 * 业务背景：添加新内存时需把物理地址映射到已知节点，未知固件区间也必须选择可用回退节点。
 * 入参：start 是新内存起始物理地址。
 * 出参/返回：命中默认 meminfo 时返回其 nid，否则返回第一个已注册块的 nid。
 * 注意事项：初始化已保证 meminfo 非空；与 phys_to_target_node 不同，本路径不查询 reserved 集合。
 */
int memory_add_physaddr_to_nid(u64 start)
{
	/* 热添加优先采用历史物理区间归属，未知地址退到首个节点。 */
	int nid = meminfo_to_nid(&numa_meminfo, start);

	/* 回退假定已注册至少一个块，由初始化的非空检查保证。 */
	if (nid == NUMA_NO_NODE)
		nid = numa_meminfo.blk[0].nid;
	return nid;
}
EXPORT_SYMBOL_GPL(memory_add_physaddr_to_nid);

#endif /* CONFIG_NUMA_KEEP_MEMINFO */
