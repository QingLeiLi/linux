// SPDX-License-Identifier: GPL-2.0-only
/*
 * 通用架构 NUMA 启动与 CPU 映射学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 职责边界：
 * 本文件是 GENERIC_ARCH_NUMA 的架构公共层，当前由 arm64 和 RISC-V
 * 使用。它把 ACPI SRAT 或 Device Tree 解析得到的早期物理拓扑，转换为
 * 内存管理和 CPU hotplug 可消费的节点状态：pg_data_t、node mask、
 * CPU<->node 映射、per-CPU first chunk 和 NUMA emulation 映射。
 * 具体 SRAT/DT 表项解析、页分配器 zone 初始化、调度域构建和运行期
 * memory hotplug 不在本文件完成。
 *
 * 主调用链：
 *   bootmem/misc_mem_init()
 *     -> arch_numa_init()
 *       -> numa_init(ACPI 或 OF parser)
 *         -> numa_memblks_init()
 *              清旧状态 -> 解析物理 memblk/distance -> 清理/模拟/登记 memblock
 *         -> numa_register_nodes()
 *              验证 RAM 覆盖 -> 分配 NODE_DATA -> node online
 *         -> setup_node_to_cpumask_map()
 *       -> 真实拓扑失败时 numa_init(dummy_numa_init)
 *              把全部 DRAM 收敛为 node 0
 *
 * CPU 路径：
 *   固件/DT 枚举 -> early_map_cpu_to_node() 写早期数组
 *   secondary boot -> numa_store_cpu_info() 发布 per-CPU nid
 *                  -> numa_add_cpu() 更新 node_to_cpumask_map
 *   CPU offline    -> numa_remove_cpu()/numa_clear_node()
 *
 * 核心状态与生命周期：
 * cpu_to_node_map[] 在 memblock/静态 per-CPU 区建立前保存早期 CPU nid；
 * node_to_cpumask_map[] 在 node_possible_map 确定后分配，随后由 CPU
 * online/offline 串行路径维护；NODE_DATA(nid) 从 memblock 分配并存活到
 * 系统结束。所有 __init 数据和函数在启动完成后可回收，不能保存给晚期
 * 异步调用。
 *
 * 并发模型：
 * 拓扑解析、距离矩阵、NODE_DATA 和 per-CPU first chunk 都在单线程早期
 * 启动阶段构造，无需普通运行期锁。CPU mask 的后续修改由 CPU hotplug
 * 状态机串行，读者按 cpumask/hotplug 协议观察；本文件本身不提供额外锁。
 *
 * 方案权衡：
 * 公共实现让 ACPI/DT、fake NUMA 和 per-CPU locality 在多个架构间复用；
 * 代价是启动状态分成“早期数组、memblock nid、online node、per-CPU nid”
 * 多个阶段。任何真实拓扑失败都回退单节点 UMA，以保证内核仍可启动，
 * 但会失去局部性优化并把 numa_off 固定为 true。
 */
/*
 * NUMA support, based on the x86 implementation.
 *
 * Copyright (C) 2015 Cavium Inc.
 * Author: Ganapatrao Kulkarni <gkulkarni@cavium.com>
 */
/*
 * 中文对译：
 * 本通用 NUMA 支持以 x86 实现为基础，由 Cavium 于 2015 年贡献；
 * 原作者与版权信息如上。
 */

#define pr_fmt(fmt) "NUMA: " fmt

#include <linux/acpi.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/numa_memblks.h>

#include <asm/sections.h>

/*
 * cpu_to_node_map[] - 静态 per-CPU 区可用前的早期 CPU->nid 表。
 *
 * 固件/DT 枚举通过 early_map_cpu_to_node() 写入；per-CPU allocator 的
 * locality callback 和 secondary CPU 启动读取。NUMA_NO_NODE 初值明确
 * 区分“尚未发现”与合法 node 0。数组覆盖 NR_CPUS，生命周期为全系统；
 * 启动后实际快速查询转向 per-CPU numa_node。
 */
static int cpu_to_node_map[NR_CPUS] = { [0 ... NR_CPUS-1] = NUMA_NO_NODE };

/*
 * numa_off - 是否强制禁用真实/模拟 NUMA。
 *
 * `numa=off`、dummy fallback 成功后置 true；arch_numa_init() 据此跳过
 * ACPI/DT 尝试，early CPU 映射也强制 node 0。早期启动单线程读写，发布
 * 后只读，无需锁。该变量不是“内核未编译 NUMA”的配置位。
 */
bool numa_off;

/*
 * numa_parse_early_param() - 解析 `numa=` 的 early boot 选项。
 *
 * opt 是启动命令行解析器借用的可写 NUL 字符串；NULL 返回 -EINVAL。
 * `off` 前缀置 numa_off；`fake=` 把剩余规格交给 emulation parser 并
 * 原样返回其状态；其他值被视为已消费且无副作用。early_param 保证在
 * arch_numa_init() 和 memblock NUMA 登记前执行，无资源 ownership 转移。
 */
static __init int numa_parse_early_param(char *opt)
{
	if (!opt)
		return -EINVAL;
	if (str_has_prefix(opt, "off"))
		numa_off = true;
	if (!strncmp(opt, "fake=", 5))
		return numa_emu_cmdline(opt + 5);

	return 0;
}
early_param("numa", numa_parse_early_param);

/*
 * node_to_cpumask_map[nid] - 每个逻辑 node 的 CPU membership mask。
 *
 * 指针数组全局导出给调度、拓扑和驱动代码；实际 cpumask 存储由
 * setup_node_to_cpumask_map() 按 nr_node_ids 从 bootmem 分配。初始化
 * 前元素为 NULL，之后存活到系统结束，并由 CPU hotplug 路径增删 CPU。
 */
cpumask_var_t node_to_cpumask_map[MAX_NUMNODES];
EXPORT_SYMBOL(node_to_cpumask_map);

#ifdef CONFIG_DEBUG_PER_CPU_MAPS

/*
 * Returns a pointer to the bitmask of CPUs on Node 'node'.
 */
/*
 * 中文对译：
 * 返回 node 所含 CPU 的位图指针。
 */
/*
 * cpumask_of_node() - 调试构建下验证 nid 与 map 初始化时序。
 *
 * node 是待查询逻辑 nid；NUMA_NO_NODE 表示无归属，按通用约定返回
 * cpu_all_mask。越过当前 nr_node_ids 时 WARN 并返回空 mask；映射尚未
 * 分配时 WARN 并退回 cpu_online_mask，使诊断构建尽量继续启动。
 *
 * 返回值均为全局借用 const 指针，无需释放；内容可随 CPU hotplug
 * 改变，调用者若需要稳定快照仍须遵守 hotplug/cpumask 并发协议。
 */
const struct cpumask *cpumask_of_node(int node)
{

	if (node == NUMA_NO_NODE)
		return cpu_all_mask;

	if (WARN_ON(node < 0 || node >= nr_node_ids))
		return cpu_none_mask;

	if (WARN_ON(node_to_cpumask_map[node] == NULL))
		return cpu_online_mask;

	return node_to_cpumask_map[node];
}
EXPORT_SYMBOL(cpumask_of_node);

#endif

#ifndef CONFIG_NUMA_EMU
/*
 * numa_update_cpu() - 非 emulation 构建中更新 CPU 的 node membership。
 *
 * cpu 是逻辑 CPU 号，remove 选择 clear 或 set。函数从已经发布的
 * per-CPU cpu_to_node() 取得 nid；无归属时幂等返回。调用者来自 CPU
 * online/offline 串行路径，node_to_cpumask_map 已完成分配；无返回值。
 */
static void numa_update_cpu(unsigned int cpu, bool remove)
{
	int nid = cpu_to_node(cpu);

	if (nid == NUMA_NO_NODE)
		return;

	if (remove)
		cpumask_clear_cpu(cpu, node_to_cpumask_map[nid]);
	else
		cpumask_set_cpu(cpu, node_to_cpumask_map[nid]);
}

/*
 * numa_add_cpu()/numa_remove_cpu() - CPU hotplug membership 薄包装。
 *
 * 两者固定 numa_update_cpu() 的 add/remove 策略，不改变 CPU->nid 本身。
 * add 在 CPU 节点信息发布后把 CPU 加入 node mask；remove 在 offline
 * 阶段摘除。无错误返回，调用者负责合法 CPU 号与 hotplug 串行条件。
 */
void numa_add_cpu(unsigned int cpu)
{
	numa_update_cpu(cpu, false);
}

void numa_remove_cpu(unsigned int cpu)
{
	numa_update_cpu(cpu, true);
}
#endif

/*
 * numa_clear_node() - 同时撤销 CPU 的 node mask 成员和 per-CPU nid。
 *
 * 先从旧 node mask 摘除，再把 per-CPU numa_node 写成 NUMA_NO_NODE；
 * 反序会让 numa_remove_cpu() 无法找到旧 nid，留下陈旧 membership。
 * cpu 必须由 hotplug/架构调用者稳定，函数不触及早期 cpu_to_node_map。
 */
void numa_clear_node(unsigned int cpu)
{
	numa_remove_cpu(cpu);
	set_cpu_numa_node(cpu, NUMA_NO_NODE);
}

/*
 * Allocate node_to_cpumask_map based on number of available nodes
 * Requires node_possible_map to be valid.
 *
 * Note: cpumask_of_node() is not valid until after this is done.
 * (Use CONFIG_DEBUG_PER_CPU_MAPS to check this.)
 */
/*
 * 中文对译：
 * 根据可用节点数分配 node_to_cpumask_map；调用前 node_possible_map
 * 必须有效。在此函数完成前 cpumask_of_node() 不能正常使用，开启
 * CONFIG_DEBUG_PER_CPU_MAPS 可以检测错误调用时序。
 */
/*
 * setup_node_to_cpumask_map() - 为所有可能 nid 建立永久 CPU mask。
 *
 * 运行于 __init 单线程阶段。若 nr_node_ids 仍是 MAX_NUMNODES 哨兵，
 * 先从 node_possible_map 收缩出真实上界；随后对 [0, nr_node_ids)
 * 使用 bootmem 分配 cpumask 并清零。分配 helper 失败属于启动期不可
 * 恢复错误，由其内部终止；成功后 CPU hotplug 路径可以安全更新映射。
 */
static void __init setup_node_to_cpumask_map(void)
{
	int node;

	/* setup nr_node_ids if not done yet */
	/*
	 * 若架构尚未给出有效上界，就根据 possible node 集合计算
	 * nr_node_ids。
	 */
	if (nr_node_ids == MAX_NUMNODES)
		setup_nr_node_ids();

	/* allocate and clear the mapping */
	/*
	 * 为每个可能 nid 分配永久 cpumask，并建立“尚无 CPU”这一
	 * 初始状态。
	 */
	for (node = 0; node < nr_node_ids; node++) {
		alloc_bootmem_cpumask_var(&node_to_cpumask_map[node]);
		cpumask_clear(node_to_cpumask_map[node]);
	}

	/* cpumask_of_node() will now work */
	/* 至此所有表项均已发布，cpumask_of_node() 可以安全返回对应 mask。 */
	pr_debug("Node to cpumask map for %u nodes\n", nr_node_ids);
}

/*
 * Set the cpu to node and mem mapping
 */
/*
 * 中文对译：
 * 建立 CPU 到 NUMA node/内存局部性的映射。
 */
/*
 * numa_store_cpu_info() - 把早期 CPU nid 发布到正式 per-CPU 状态。
 *
 * cpu_to_node_map 在静态 per-CPU 区可用前积累固件结果；secondary CPU
 * 启动或 boot CPU 初始化在 per-CPU area 就绪后调用本函数，通过
 * set_cpu_numa_node() 写正式快速查询字段。cpu/nid 必须已校验，无返回值。
 */
void numa_store_cpu_info(unsigned int cpu)
{
	set_cpu_numa_node(cpu, cpu_to_node_map[cpu]);
}

/*
 * early_map_cpu_to_node() - 在早期 CPU 枚举阶段记录 CPU->nid。
 *
 * cpu 是已发现但可能尚未 online 的逻辑 CPU；nid 来自 ACPI SRAT 或 DT。
 * 非法 nid 或 numa_off 时收敛到 node 0，保证后续 per-CPU 分配 callback
 * 永远得到合法位置。boot CPU 已经提前 online，因此 CPU0 还必须立即
 * 更新正式 per-CPU numa_node，封住后续 cpu_to_node(0) 的使用窗口。
 *
 * 函数只写静态数组/per-CPU 标量，不分配资源、不失败；启动枚举串行。
 */
void __init early_map_cpu_to_node(unsigned int cpu, int nid)
{
	/* fallback to node 0 */
	/* 无效 nid 或 numa=off 都统一归入 node 0，避免留下不可索引的映射。 */
	if (nid < 0 || nid >= MAX_NUMNODES || numa_off)
		nid = 0;

	cpu_to_node_map[cpu] = nid;

	/*
	 * We should set the numa node of cpu0 as soon as possible, because it
	 * has already been set up online before. cpu_to_node(0) will soon be
	 * called.
	 */
	/*
	 * 中文补充：CPU0 不能等待 secondary bring-up 的 store_cpu_info 路径，
	 * 否则早期内存分配或拓扑代码会短暂观察 NUMA_NO_NODE。
	 */
	if (!cpu)
		set_cpu_numa_node(cpu, nid);
}

#ifdef CONFIG_HAVE_SETUP_PER_CPU_AREA
/*
 * __per_cpu_offset[cpu] - 每个 CPU 的静态 per-CPU 基址偏移。
 *
 * setup_per_cpu_areas() 在 first chunk 成功后一次性填充，链接器/通用
 * this_cpu 访问用它把 __per_cpu_start 内的符号换算到目标 CPU unit。
 * __read_mostly 让启动后只读的数组远离频繁写 cacheline；导出给模块和
 * 通用 per-CPU 访问代码。初始化前不能用于正式地址换算。
 */
unsigned long __per_cpu_offset[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(__per_cpu_offset);

/*
 * early_cpu_to_node() - per-CPU allocator 使用的早期 nid getter。
 *
 * cpu 必须小于 NR_CPUS；返回 cpu_to_node_map 中的当前值，不取锁、不
 * 校验 online 状态。它只在 __init 拓扑稳定阶段作为 callback 使用。
 */
int early_cpu_to_node(int cpu)
{
	return cpu_to_node_map[cpu];
}

/*
 * pcpu_cpu_distance() - 把 CPU 对转换为 NUMA distance。
 *
 * from/to 是 possible CPU 号；先映射各自早期 nid，再查询已构造的距离
 * 矩阵。返回 LOCAL_DISTANCE 或固件/模拟距离，供 embed allocator 把
 * 相互局部的 CPU unit 分组；纯查询、无 ownership 变化。
 */
static int __init pcpu_cpu_distance(unsigned int from, unsigned int to)
{
	return node_distance(early_cpu_to_node(from), early_cpu_to_node(to));
}

/*
 * setup_per_cpu_areas() - 构造架构静态 per-CPU first chunk 并发布偏移。
 *
 * 调用时 CPU->nid、NUMA distance、memblock 均已可用，普通页分配器尚未
 * 接管。优先按 percpu_alloc= 选择 embed/page：embed 在大块 bootmem 中
 * 按 NUMA distance 分组并预留模块/动态区；若架构允许且 embed 失败，
 * page first chunk 以 PAGE_SIZE 离散映射回退。两者都失败会 panic，
 * 因为 task/current 等基础 per-CPU 对象无法运行。
 *
 * 成功后 pcpu_base_addr 和 pcpu_unit_offsets 已由 allocator 发布，本函数
 * 计算每 CPU 相对链接期 __per_cpu_start 的最终 offset。所有局部量只在
 * __init 有效，无可返回错误。
 */
void __init setup_per_cpu_areas(void)
{
	unsigned long delta;
	unsigned int cpu;
	int rc = -EINVAL;

	if (pcpu_chosen_fc != PCPU_FC_PAGE) {
		/*
		 * Always reserve area for module percpu variables.  That's
		 * what the legacy allocator did.
		 */
		/*
		 * 中文对译与补充：
		 * 无论当前是否已加载模块，都为模块 per-CPU 变量保留空间，以
		 * 延续 legacy allocator ABI；同时预留动态分配区。distance/nid
		 * callback 让 allocator 在满足布局利用率的同时保持 NUMA 局部性。
		 */
		rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
					    PERCPU_DYNAMIC_RESERVE, PAGE_SIZE,
					    pcpu_cpu_distance,
					    early_cpu_to_node);
#ifdef CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK
		if (rc < 0)
			pr_warn("PERCPU: %s allocator failed (%d), falling back to page size\n",
				   pcpu_fc_names[pcpu_chosen_fc], rc);
#endif
	}

#ifdef CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK
	/*
	 * embed 未选择或失败时，page allocator 是可配置的启动期 fallback；
	 * rc==0 后 first chunk 已建立，不能再尝试第二种布局。
	 */
	if (rc < 0)
		rc = pcpu_page_first_chunk(PERCPU_MODULE_RESERVE, early_cpu_to_node);
#endif
	/* per-CPU first chunk 是内核运行前提，失败没有可用的降级模式。 */
	if (rc < 0)
		panic("Failed to initialize percpu areas (err=%d).", rc);

	/*
	 * pcpu_unit_offsets 是各 CPU unit 相对 pcpu_base_addr 的偏移；先加
	 * base 与链接期 section 的 delta，得到访问链接符号所需的最终偏移。
	 */
	delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
	for_each_possible_cpu(cpu)
		__per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
}
#endif

/*
 * Initialize NODE_DATA for a node on the local memory
 */
/*
 * 中文对译：
 * 为一个 node 初始化尽量位于该节点本地内存上的 NODE_DATA。
 */
/*
 * setup_node_data() - 建立 nid 对应的 pg_data_t 基础物理范围。
 *
 * nid 已在 numa_nodes_parsed；[start_pfn, end_pfn) 是 memblock 聚合出的
 * PFN 包围范围，二者相等表示 memoryless node。alloc_node_data() 优先
 * 从 nid 本地 memblock 分配、失败再尝试任意节点，彻底失败会 panic。
 *
 * 成功后 NODE_DATA(nid) 可全局查找，node_id/start/spanned_pages 已发布；
 * zone、present pages、LRU 和伙伴系统仍由后续 free_area_init 完成。
 */
static void __init setup_node_data(int nid, u64 start_pfn, u64 end_pfn)
{
	if (start_pfn >= end_pfn)
		pr_info("Initmem setup node %d [<memory-less node>]\n", nid);

	alloc_node_data(nid);

	NODE_DATA(nid)->node_id = nid;
	NODE_DATA(nid)->node_start_pfn = start_pfn;
	NODE_DATA(nid)->node_spanned_pages = end_pfn - start_pfn;
}

/*
 * numa_register_nodes() - 验证 memblock nid 覆盖并发布全部 online node。
 *
 * numa_memblks_init() 已把固件/模拟区间写入 memblock.memory，并建立
 * numa_nodes_parsed。threshold=0 要求所有 RAM 都有合法 nid，任何未覆盖
 * 字节都会使本次真实拓扑失败。随后逐 parsed nid 计算 PFN 包围范围、
 * 分配 NODE_DATA，并置 online。
 *
 * 全部节点成功后才把 node_possible_map 替换为 parsed 集合。失败返回
 * -EINVAL；该失败发生在任何 NODE_DATA 分配之前。分配失败由
 * alloc_node_data() panic，因此不存在部分 NODE_DATA 的普通错误回滚。
 */
static int __init numa_register_nodes(void)
{
	int nid;

	/* Check the validity of the memblock/node mapping */
	/* 覆盖阈值为 0：generic arch NUMA 不接受任何 RAM 留在 NUMA_NO_NODE。 */
	if (!memblock_validate_numa_coverage(0))
		return -EINVAL;

	/* Finally register nodes. */
	/*
	 * 最后逐个建立 pgdat 并置 online，使解析结果成为内存管理
	 * 可见的 node。
	 */
	for_each_node_mask(nid, numa_nodes_parsed) {
		unsigned long start_pfn, end_pfn;

		get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);
		setup_node_data(nid, start_pfn, end_pfn);
		node_set_online(nid);
	}

	/* Setup online nodes to actual nodes*/
	/*
	 * 此时 possible 与 online 都应准确反映最终 parsed 节点，包括可能
	 * memoryless 但由 CPU 拓扑发现的节点。
	 */
	node_possible_map = numa_nodes_parsed;

	return 0;
}

/*
 * numa_init() - 执行一次“解析器 + 通用登记”的 NUMA 初始化事务。
 *
 * init_func 是 ACPI、OF 或 dummy 解析入口，无参数，返回 0/-errno。
 * 函数先清 parsed/possible/online mask，再由 numa_memblks_init() 清理
 * memblock nid、距离表，执行解析、规范化区间和可选 fake NUMA。解析成功
 * 但没有节点同样视为 -EINVAL。
 *
 * 登记成功后 NODE_DATA、online/possible map 和 node cpumask 存储全部
 * 可用，返回 0。任一失败统一 reset distance 并返回 errno，让
 * arch_numa_init() 尝试下一来源；本函数只在单线程 __init 上下文执行。
 */
static int __init numa_init(int (*init_func)(void))
{
	int ret;

	/*
	 * 阶段 1：撤销上一解析器的逻辑发布状态。numa_memblks_init() 还会
	 * 重置临时 meminfo、memblock nid/hotplug 标志与距离表。
	 */
	nodes_clear(numa_nodes_parsed);
	nodes_clear(node_possible_map);
	nodes_clear(node_online_map);

	/*
	 * 阶段 2：执行选定 parser，并把区间清理、fake NUMA、memblock nid
	 * 登记统一收敛为一份规范化拓扑。
	 */
	/*
	 * 内联英文参数名说明这里不强制 memblock 自顶向下分配；
	 * 保持当前方向，避免 NUMA 初始化擅自改变后续早期分配策略。
	 */
	ret = numa_memblks_init(init_func, /* memblock_force_top_down */ false);
	if (ret < 0)
		goto out_free_distance;

	/*
	 * parser 返回成功却未声明任何 node 仍是无效拓扑，不能继续分配
	 * pgdat。
	 */
	if (nodes_empty(numa_nodes_parsed)) {
		pr_info("No NUMA configuration found\n");
		ret = -EINVAL;
		goto out_free_distance;
	}

	/* 阶段 3：验证 RAM 全覆盖并发布 NODE_DATA/online/possible 状态。 */
	ret = numa_register_nodes();
	if (ret < 0)
		goto out_free_distance;

	/* 最后分配反向 CPU mask；从此 CPU online 路径可以填充 membership。 */
	setup_node_to_cpumask_map();

	return 0;
out_free_distance:
	/*
	 * 距离矩阵由 memblock 分配且与本次 node 编号绑定；失败后必须清除，
	 * 防止下一解析器或 dummy fallback 读取上一轮的陈旧维度。
	 */
	numa_reset_distance();
	return ret;
}

/**
 * dummy_numa_init() - Fallback dummy NUMA init
 *
 * Used if there's no underlying NUMA architecture, NUMA initialization
 * fails, or NUMA is disabled on the command line.
 *
 * Must online at least one node (node 0) and add memory blocks that cover all
 * allowed memory. It is unlikely that this function fails.
 *
 * Return: 0 on success, -errno on failure.
 */
/*
 * 中文对译：
 * dummy_numa_init() 是没有底层 NUMA 架构、真实初始化失败或命令行禁用
 * NUMA 时的最终回退。它必须至少 online node 0，并添加覆盖全部允许
 * 内存的区间；通常不会失败。成功返回 0，失败返回 errno。
 */
/*
 * 
 * 调用时 memblock 已包含最终 DRAM，numa_memblks_init() 已清除上一轮
 * nid/距离状态。start/end 是物理字节地址，numa_add_memblk() 接受
 * [start, end) 半开区间，所以先计算 inclusive end 再传 end+1。
 *
 * 成功后 numa_nodes_parsed 只含 node 0，并置 numa_off=true，确保 CPU
 * 映射和后续逻辑统一按 UMA 处理；若 memblk 表容量/参数异常则返回错误，
 * arch_numa_init() 已无进一步 fallback，但启动仍沿调用者策略继续。
 */
static int __init dummy_numa_init(void)
{
	phys_addr_t start = memblock_start_of_DRAM();
	phys_addr_t end = memblock_end_of_DRAM() - 1;
	int ret;

	if (numa_off)
		pr_info("NUMA disabled\n"); /* Forced off on command line. */
	/*
	 * 命令行强制关闭时打印原因；该行尾英文表示此分支来自
	 * numa=off。
	 */
	pr_info("Faking a node at [mem %pap-%pap]\n", &start, &end);

	/* 把完整 DRAM 作为 node 0 的唯一物理 memblk，end+1 恢复半开区间。 */
	ret = numa_add_memblk(0, start, end + 1);
	if (ret) {
		pr_err("NUMA init failed\n");
		return ret;
	}
	node_set(0, numa_nodes_parsed);

	numa_off = true;
	return 0;
}

#ifdef CONFIG_ACPI_NUMA
/*
 * arch_acpi_numa_init() - 以 ACPI SRAT/SLIT 构造物理 NUMA 描述。
 *
 * acpi_numa_init() 解析固件并通过 numa_add_memblk()/distance helper 写入
 * 通用临时状态；非零返回表示解析失败。即使解析入口返回成功，SRAT 被
 * quirks/命令行禁用时仍返回 -EINVAL，迫使上层放弃该拓扑。
 *
 * CONFIG_ACPI_NUMA=n 的 stub 固定返回 -EOPNOTSUPP，使 arch_numa_init()
 * 可保持统一调用结构。两种实现都只在 __init 阶段执行。
 */
static int __init arch_acpi_numa_init(void)
{
	int ret;

	ret = acpi_numa_init();
	if (ret) {
		pr_debug("Failed to initialise from firmware\n");
		return ret;
	}

	return srat_disabled() ? -EINVAL : 0;
}
#else
static int __init arch_acpi_numa_init(void)
{
	return -EOPNOTSUPP;
}
#endif

/**
 * arch_numa_init() - Initialize NUMA
 *
 * Try each configured NUMA initialization method until one succeeds. The
 * last fallback is dummy single node config encompassing whole memory.
 */
/*
 * 中文对译：
 * arch_numa_init() 依次尝试已配置的 NUMA 初始化方法，直到一个成功；
 * 最后的 fallback 是覆盖全部内存的 dummy 单节点配置。
 */
/*
 * 
 * arm64/RISC-V 在 memblock 建立、zone 初始化之前调用，无参数和返回值。
 * `numa=off` 直接跳过真实来源。ACPI 可用时只尝试 ACPI；ACPI 全局禁用
 * 时尝试 Device Tree，避免同一次启动把两种固件拓扑混合。
 *
 * 每次 numa_init() 都会清空上一轮 mask/memblock nid/距离状态，成功立即
 * 返回；所有真实来源失败后必须执行 dummy。dummy 正常应成功，其返回值
 * 在此不再传播；正常平台最终至少 node 0 online 且 node cpumask 已分配。
 * 若这个最后 fallback 也失败，只会留下错误日志，后续内存初始化将因
 * 缺少有效 node 状态暴露致命问题。
 */
void __init arch_numa_init(void)
{
	if (!numa_off) {
		/*
		 * 固件来源互斥选择：ACPI 启用时以 SRAT/SLIT 为权威，否则才
		 * 使用 DT。numa_init()==0 表示完整通用登记已提交，可立即返回。
		 */
		if (!acpi_disabled && !numa_init(arch_acpi_numa_init))
			return;
		if (acpi_disabled && !numa_init(of_numa_init))
			return;
	}

	/*
	 * 强制关闭或真实来源失败：重新清状态并把全部 DRAM 登记为
	 * node 0。
	 */
	numa_init(dummy_numa_init);
}

#ifdef CONFIG_NUMA_EMU
/*
 * numa_emu_update_cpu_to_node() - 把早期 CPU 物理 nid 改写为 emulated nid。
 *
 * emu_nid_to_phys[emu_nid] 由 mm/numa_emulation.c 构造，nr_emu_nids 是
 * 有效前缀长度。函数遍历完整 NR_CPUS 早期表：未发现 CPU 保持
 * NUMA_NO_NODE；已映射 CPU 反向查找第一个承载于其 physical node 的
 * fake node，找不到则防御性回退 node 0。
 *
 * 该改写发生在 per-CPU area 和 CPU hotplug membership 发布之前，无需
 * 锁。后续 numa_store_cpu_info()/early_cpu_to_node() 只看到 emulated
 * 编号；CPU 加入同一 physical node 上所有 fake node mask 的策略由
 * mm/numa_emulation.c 的 numa_add_cpu() 处理。
 */
void __init numa_emu_update_cpu_to_node(int *emu_nid_to_phys,
					unsigned int nr_emu_nids)
{
	int i, j;

	/*
	 * Transform cpu_to_node_map table to use emulated nids by
	 * reverse-mapping phys_nid.  The maps should always exist but fall
	 * back to zero just in case.
	 */
	/*
	 * 中文对译：
	 * 通过反向匹配 physical nid，把 cpu_to_node_map 转成 emulated nid。
	 * 正常情况下映射一定存在，防御性路径在缺失时回退到 node 0。
	 */
	for (i = 0; i < ARRAY_SIZE(cpu_to_node_map); i++) {
		if (cpu_to_node_map[i] == NUMA_NO_NODE)
			continue;
		/* 找到第一个回指该 physical nid 的 fake node 作为 CPU 主 nid。 */
		for (j = 0; j < nr_emu_nids; j++)
			if (cpu_to_node_map[i] == emu_nid_to_phys[j])
				break;
		cpu_to_node_map[i] = j < nr_emu_nids ? j : 0;
	}
}

/*
 * numa_emu_dma_end() - 给通用 NUMA emulation 提供首个 4 GiB DMA 分界。
 *
 * 返回物理字节地址 `DRAM 起点 + 4GiB`，无输入和副作用。emulation 分割
 * 内存块时用它避免低地址 DMA 可达区与其余内存被任意 fake node 边界
 * 混合；结果不是实际 DRAM 末尾，也不负责溢出/容量裁剪。
 */
u64 __init numa_emu_dma_end(void)
{
	return memblock_start_of_DRAM() + SZ_4G;
}

/*
 * debug_cpumask_set_cpu() - 调试构建下安全更新一个 node CPU mask。
 *
 * cpu/node/enable 分别表示逻辑 CPU、目标 emulated nid 和 add/remove。
 * NUMA_NO_NODE 幂等忽略；NULL mask 打印错误和栈后返回，避免静默空指针。
 * 成功后输出完整 mask 便于核对一个 physical node 对应多个 fake node 的
 * membership。调用者持有 CPU hotplug 串行语义，函数不额外加锁。
 */
void debug_cpumask_set_cpu(unsigned int cpu, int node, bool enable)
{
	struct cpumask *mask;

	if (node == NUMA_NO_NODE)
		return;

	mask = node_to_cpumask_map[node];
	if (!cpumask_available(mask)) {
		pr_err("node_to_cpumask_map[%i] NULL\n", node);
		dump_stack();
		return;
	}

	if (enable)
		cpumask_set_cpu(cpu, mask);
	else
		cpumask_clear_cpu(cpu, mask);

	pr_debug("%s cpu %d node %d: mask now %*pbl\n",
		 enable ? "numa_add_cpu" : "numa_remove_cpu",
		 cpu, node, cpumask_pr_args(mask));
}
#endif /* CONFIG_NUMA_EMU */
/* CONFIG_NUMA_EMU=n 时 CPU mask 更新使用本文件前部的直接 nid 路径。 */
