/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Tejun Heo <tj@kernel.org>
 */
#include <linux/cacheinfo.h>

#include "internal.h"
#include "cid.h"

/*
 * cid tables.
 *
 * Pointers are published once on first enable and never revoked. The default
 * mapping is populated before ops.init() runs; scx_bpf_cid_override() commits
 * before it returns. As long as the BPF scheduler only uses the tables from
 * those points onward, it sees a consistent view.
 */
/*
 * 三张表只在首次启用时分配并永久保留：默认映射在 BPF ops.init 前写完，自定义映射
 * 在 override 返回前写完，因此运行期读者无需为表指针取得引用。元素在当前 scheduler
 * 生命周期内视为稳定；发布边界之外的并发读写不受本契约保护。
 */
s16 *scx_cid_to_cpu_tbl;
s16 *scx_cpu_to_cid_tbl;
struct scx_cid_topo *scx_cid_topo;

#define SCX_CID_TOPO_NEG	(struct scx_cid_topo) {				\
	.core_cid = -1, .core_idx = -1, .llc_cid = -1, .llc_idx = -1,		\
	.node_cid = -1, .node_idx = -1,						\
}

/*
 * Return @cpu's LLC shared_cpu_map. If cacheinfo isn't populated (offline or
 * !present), record @cpu in @fallbacks and return its node mask instead - the
 * worst that can happen is that the cpu's LLC becomes coarser than reality.
 */
/*
 * 返回 CPU 最末级 cache 的共享 mask；离线/缺 cacheinfo 时记录降级 CPU 并退化到 NUMA
 * node mask，结果可能把多个真实 LLC 合并但不会把 CPU 排除。返回指针由拓扑子系统拥有。
 */
/*
 * 业务背景：CID 初始化按 LLC 分片 CPU，但离线 CPU 的 cacheinfo 可能缺失，需要保守退化到 NUMA node。
 * 入参：cpu 是有效 possible CPU 编号；fallbacks 是不可空、借用且可写的诊断 cpumask。
 * 出参/返回：返回拓扑子系统拥有的非空借用 mask；降级时还在 fallbacks 置 cpu 位，无 ownership 转移。
 * 注意事项：调用者持 cpus_read_lock 稳定拓扑；node mask 可能比真实 LLC 粗，不得修改返回 mask。
 */
static const struct cpumask *cpu_llc_mask(int cpu, struct cpumask *fallbacks)
{
	struct cpu_cacheinfo *ci = get_cpu_cacheinfo(cpu);

	if (!ci || !ci->info_list || !ci->num_leaves) {
		cpumask_set_cpu(cpu, fallbacks);
		return cpumask_of_node(cpu_to_node(cpu));
	}
	return &ci->info_list[ci->num_leaves - 1].shared_cpu_map;
}

/* Allocate the cid tables once on first enable; never freed. */
/*
 * 首次启用分配双向映射和拓扑数组；任一分配失败回滚本次三个临时对象并返回 -ENOMEM。
 * 成功后用 WRITE_ONCE 逐个发布，之后快速返回 0，表故意不释放以简化跨 scheduler 读侧。
 */
/*
 * 业务背景：所有 CID 查询依赖三张全局永久表，首次 scheduler 启用前必须一次性建立稳定存储。
 * 入参：无。
 * 出参/返回：已分配或成功发布返回 0，任一分配失败返回 -ENOMEM；发布三张表指针且永久保留 ownership。
 * 注意事项：可用 GFP_KERNEL 睡眠；调用者串行首次启用，WRITE_ONCE 只发布指针，元素稍后在 init 填充。
 */
static s32 scx_cid_arrays_alloc(void)
{
	u32 npossible = num_possible_cpus();
	s16 *cid_to_cpu, *cpu_to_cid;
	struct scx_cid_topo *cid_topo;

	/* 已发布过永久表时直接复用，后续 scheduler 只重填元素而不更换地址。 */
	if (scx_cid_to_cpu_tbl)
		return 0;

	/* 双向表分别按稠密 CID 数与 raw CPU 上界分配，拓扑表与 CID 数一一对应。 */
	cid_to_cpu = kzalloc_objs(*scx_cid_to_cpu_tbl, npossible, GFP_KERNEL);
	cpu_to_cid = kzalloc_objs(*scx_cpu_to_cid_tbl, nr_cpu_ids, GFP_KERNEL);
	cid_topo = kmalloc_objs(*scx_cid_topo, npossible, GFP_KERNEL);

	/* 任一失败时三个指针仍属本函数，统一释放已成功项后不发布半套状态。 */
	if (!cid_to_cpu || !cpu_to_cid || !cid_topo) {
		kfree(cid_to_cpu);
		kfree(cpu_to_cid);
		kfree(cid_topo);
		return -ENOMEM;
	}

	/* 所有存储齐备后才逐指针发布；永久 ownership 自此转给全局 CID 子系统。 */
	WRITE_ONCE(scx_cid_to_cpu_tbl, cid_to_cpu);
	WRITE_ONCE(scx_cpu_to_cid_tbl, cpu_to_cid);
	WRITE_ONCE(scx_cid_topo, cid_topo);
	return 0;
}

/**
 * scx_cid_init - build the cid mapping
 * @sch: the scx_sched being initialized; used as the scx_error() target
 *
 * See "Topological CPU IDs" in cid.h for the model. Walk online cpus by
 * intersection at each level (parent_scratch & this_level_mask), which keeps
 * containment correct by construction and naturally splits a physical LLC
 * straddling two NUMA nodes into two LLC units. The caller must hold
 * cpus_read_lock.
 */
/*
 * 在持 cpus_read_lock 时构建稠密 cid：按 node→LLC→core 逐层取父范围与子 mask 交集，
 * 因而每层天然连续且跨 NUMA 的 LLC 会被拆开。每个 CPU 写双向表和三层起点/序号；
 * possible 但未分配者追加到无拓扑尾段。表/临时 mask 分配失败返回 -ENOMEM，拓扑交集
 * 自相矛盾时 WARN 并返回 -EINVAL；已永久分配的表不回滚，下一次初始化会重新填充。
 */
/*
 * 业务背景：sched_ext 子调度器需要让同 node/LLC/core 的 CPU 在稠密 CID 空间连续，以低成本传递分片。
 * 入参：sch 是不可空、借用的待启用 scheduler，仅用于 scx_error/上下文归属且 ownership 不变。
 * 出参/返回：成功返回 0；表或临时 mask 分配失败 -ENOMEM，拓扑交集不变量破坏 -EINVAL；原地填三张全局表。
 * 注意事项：调用者持 cpus_read_lock，函数可睡眠；失败不回滚永久表，scheduler 启用中止或后续重建。
 */
s32 scx_cid_init(struct scx_sched *sch)
{
	/* scratch mask 由 cleanup 属性覆盖所有返回路径；计数器分别生成 CID 及三层全局序号。 */
	cpumask_var_t to_walk __free(free_cpumask_var) = CPUMASK_VAR_NULL;
	cpumask_var_t node_scratch __free(free_cpumask_var) = CPUMASK_VAR_NULL;
	cpumask_var_t llc_scratch __free(free_cpumask_var) = CPUMASK_VAR_NULL;
	cpumask_var_t core_scratch __free(free_cpumask_var) = CPUMASK_VAR_NULL;
	cpumask_var_t llc_fallback __free(free_cpumask_var) = CPUMASK_VAR_NULL;
	cpumask_var_t online_no_topo __free(free_cpumask_var) = CPUMASK_VAR_NULL;
	u32 next_cid = 0;
	s32 next_node_idx = 0, next_llc_idx = 0, next_core_idx = 0;
	s32 cpu, ret;

	/* CMASK_MAX_WORDS in cid.bpf.h covers NR_CPUS up to 8192 */
	/* 编译期拒绝 BPF cmask 固定上限无法表达的内核 NR_CPUS 配置。 */
	BUILD_BUG_ON(NR_CPUS > 8192);

	lockdep_assert_cpus_held();

	/* 阶段一：确保永久表存在，再分配本次遍历需要的六个自动清理 scratch mask。 */
	ret = scx_cid_arrays_alloc();
	if (ret)
		return ret;

	if (!zalloc_cpumask_var(&to_walk, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&node_scratch, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&llc_scratch, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&core_scratch, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&llc_fallback, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&online_no_topo, GFP_KERNEL))
		return -ENOMEM;

	/* -1 sentinels for sparse-possible cpu id holes (0 is a valid cid) */
	/* raw CPU 编号洞必须用 -1 标识，不能用合法 cid 0。 */
	for (cpu = 0; cpu < nr_cpu_ids; cpu++)
		scx_cpu_to_cid_tbl[cpu] = -1;

	/* 阶段二：只对初始化快照中的 online CPU 按 node→LLC→core 三层建立连续 CID。 */
	cpumask_copy(to_walk, cpu_online_mask);

	while (!cpumask_empty(to_walk)) {
		s32 next_cpu = cpumask_first(to_walk);
		s32 nid = cpu_to_node(next_cpu);
		s32 node_cid = next_cid;
		s32 node_idx;

		/*
		 * No NUMA info: skip and let the tail loop assign a no-topo
		 * cid. cpumask_of_node(-1) is undefined.
		 */
		/* 缺 NUMA id 时暂时移出拓扑遍历，尾段稍后仍会给它唯一 cid。 */
		if (nid < 0) {
			cpumask_clear_cpu(next_cpu, to_walk);
			continue;
		}

		/* 为整个 node 固定 CID 起点和全局 node 序号，再从待处理集截出该 node。 */
		node_idx = next_node_idx++;

		/* node_scratch = to_walk & this node */
		cpumask_and(node_scratch, to_walk, cpumask_of_node(nid));
		if (WARN_ON_ONCE(!cpumask_test_cpu(next_cpu, node_scratch)))
			return -EINVAL;

		while (!cpumask_empty(node_scratch)) {
			s32 ncpu = cpumask_first(node_scratch);
			const struct cpumask *llc_mask = cpu_llc_mask(ncpu, llc_fallback);
			s32 llc_cid = next_cid;
			s32 llc_idx = next_llc_idx++;

			/* LLC mask 必须再与当前 node 取交集，保证跨 NUMA cache 被拆成独立连续单元。 */
			/* llc_scratch = node_scratch & this llc */
			cpumask_and(llc_scratch, node_scratch, llc_mask);
			if (WARN_ON_ONCE(!cpumask_test_cpu(ncpu, llc_scratch)))
				return -EINVAL;

			while (!cpumask_empty(llc_scratch)) {
				s32 lcpu = cpumask_first(llc_scratch);
				const struct cpumask *sib = topology_sibling_cpumask(lcpu);
				s32 core_cid = next_cid;
				s32 core_idx = next_core_idx++;
				s32 ccpu;

				/* core 同样限制在父 LLC 范围内，随后为每个 sibling 按序提交双向映射。 */
				/* core_scratch = llc_scratch & this core */
				cpumask_and(core_scratch, llc_scratch, sib);
				if (WARN_ON_ONCE(!cpumask_test_cpu(lcpu, core_scratch)))
					return -EINVAL;

				for_each_cpu(ccpu, core_scratch) {
					s32 cid = next_cid++;

					/* 三张表在同一循环步写完后，再从三层 scratch 删除该 CPU 保证只处理一次。 */
					scx_cid_to_cpu_tbl[cid] = ccpu;
					scx_cpu_to_cid_tbl[ccpu] = cid;
					/* topo 项记录各层连续区间起始 CID 与全局索引，供 BPF 按层切片。 */
					scx_cid_topo[cid] = (struct scx_cid_topo){
						.core_cid = core_cid,
						.core_idx = core_idx,
						.llc_cid = llc_cid,
						.llc_idx = llc_idx,
						.node_cid = node_cid,
						.node_idx = node_idx,
					};

					/* 从所有祖先待处理集合同步删除，维持 next iteration 不会再次选择该 CPU。 */
					cpumask_clear_cpu(ccpu, llc_scratch);
					cpumask_clear_cpu(ccpu, node_scratch);
					cpumask_clear_cpu(ccpu, to_walk);
				}
			}
		}
	}

	/*
	 * No-topo section: any possible cpu without a cid - normally just the
	 * not-online ones. Collect any currently-online cpus that land here in
	 * @online_no_topo so we can warn about them at the end.
	 */
	/*
	 * 所有尚无 cid 的 possible CPU（通常离线）按 raw CPU 顺序追加并填 -1
	 * 拓扑；意外在线者仍获得可用映射，同时收集到 warning mask 暴露拓扑缺失。
	 */
	for_each_cpu(cpu, cpu_possible_mask) {
		s32 cid;

		/* 已在拓扑前段赋值的 CPU 跳过；剩余 possible CPU 进入全 -1 拓扑尾段。 */
		if (__scx_cpu_to_cid(cpu) != -1)
			continue;
		if (cpu_online(cpu))
			cpumask_set_cpu(cpu, online_no_topo);

		cid = next_cid++;
		scx_cid_to_cpu_tbl[cid] = cpu;
		scx_cpu_to_cid_tbl[cpu] = cid;
		scx_cid_topo[cid] = SCX_CID_TOPO_NEG;
	}

	/* 阶段四：映射已经可用，最后只报告不精确 LLC 与在线无拓扑两类降级集合。 */
	if (!cpumask_empty(llc_fallback))
		pr_warn("scx_cid: cpus without cacheinfo, using node mask as llc: %*pbl\n",
			cpumask_pr_args(llc_fallback));
	if (!cpumask_empty(online_no_topo))
		pr_warn("scx_cid: online cpus with no usable topology: %*pbl\n",
			cpumask_pr_args(online_no_topo));

	return 0;
}

/**
 * scx_cmask_clear - Zero every bit in @m's active range
 * @m: cmask to clear
 *
 * Storage past the active range is left as is.
 */
/* 只清 active range 覆盖的完整存储 word；空范围快速返回，范围外容量保持原值。 */
/*
 * 业务背景：复用 cmask 存储前需清除其 active 窗口，同时保留对象容量之外/重构后不可见存储。
 * 入参：m 是不可空、调用者拥有的可写 cmask，base/nr_cids/alloc_words 已有效。
 * 出参/返回：无直接返回值；清零 active range 覆盖的存储 word，空范围无副作用且 ownership 不变。
 * 注意事项：函数可能连同首尾 padding 一起清零但不越过活动 word；调用者排除并发读写，不睡眠。
 */
void scx_cmask_clear(struct scx_cmask *m)
{
	u32 nr_words;

	if (!m->nr_cids)
		return;
	/* 先覆盖 active range 所在全部 word，随后再恢复首尾 word 的窗口外 padding。 */
	nr_words = (m->base + m->nr_cids - 1) / 64 - m->base / 64 + 1;
	memset(m->bits, 0, nr_words * sizeof(u64));
}

/**
 * scx_cmask_fill - Set every bit in @m's active range
 * @m: cmask to fill
 *
 * Counterpart to scx_cmask_clear(). Storage past the active range is left as is.
 */
/*
 * 把 active range 覆盖 word 先全置 1，再清首 word 的 base 前缀和尾 word 的范围后缀，
 * 保持 padding 恒为 0；空范围无写入，调用者须独占 dst。
 */
/*
 * 业务背景：调度器常以“全部可选 CID”初始化候选集，需要只置 active 窗口而不污染 padding。
 * 入参：m 是不可空、调用者拥有的可写 cmask，header 与实际 bits 容量必须匹配。
 * 出参/返回：无直接返回值；active range 全置 1、首尾窗口外 padding 清 0，ownership 不变。
 * 注意事项：空范围不写；调用者独占对象，base+nr_cids 不得溢出且存储须覆盖计算出的 word。
 */
void scx_cmask_fill(struct scx_cmask *m)
{
	u32 nr_words, head_bits, tail_bits;

	if (!m->nr_cids)
		return;
	nr_words = (m->base + m->nr_cids - 1) / 64 - m->base / 64 + 1;
	memset(m->bits, 0xff, nr_words * sizeof(u64));

	/* 首 word 的 base 以下不是 active CID，必须从刚写入的全 1 中清除。 */
	/* clear word-0 bits below base */
	head_bits = m->base & 63;
	if (head_bits)
		m->bits[0] &= ~((1ULL << head_bits) - 1);

	/* 尾 word 在区间终点之后的 padding 同样清 0；整 word 结束时 tail_bits 为 0。 */
	/* clear last-word bits at or past base + nr_cids */
	tail_bits = (m->base + m->nr_cids) & 63;
	if (tail_bits)
		m->bits[nr_words - 1] &= (1ULL << tail_bits) - 1;
}

/**
 * scx_cpumask_to_cmask - Translate a kernel cpumask into a cmask
 * @src: source cpumask
 * @dst: cmask to write
 *
 * Clear @dst's active range and set the bit for each cid whose cpu is in
 * @src and lies within that range. Out-of-range cids are silently ignored.
 */
/*
 * 先清空 dst，再逐 raw CPU 查 cid 并调用有范围检查的 set；raw 编号洞/不在 dst 窗口的
 * cid 被忽略。src/dst 均为借用对象，函数不分配且要求调用者稳定映射和掩码。
 */
/*
 * 业务背景：内核拓扑/亲和性仍使用 raw cpumask，sched_ext 的分片接口需要把它投影为稠密 CID mask。
 * 入参：src 是不可空只读借用 cpumask；dst 是不可空、借用且可写的已初始化 cmask。
 * 出参/返回：无直接返回值；先清 dst 窗口，再置入 src 中有效且落窗 CPU 的 CID，ownership 均不变。
 * 注意事项：调用者稳定全局映射和 src，并独占 dst；possible 编号洞与窗口外 CID 被静默忽略。
 */
void scx_cpumask_to_cmask(const struct cpumask *src, struct scx_cmask *dst)
{
	s32 cpu;

	scx_cmask_clear(dst);
	/* 每个 src CPU 经永久 raw→CID 表转换，有效结果再由带范围检查的 setter 写入窗口。 */
	for_each_cpu(cpu, src) {
		s32 cid = __scx_cpu_to_cid(cpu);

		if (cid >= 0)
			__scx_cmask_set(cid, dst);
	}
}

__bpf_kfunc_start_defs();

/**
 * scx_bpf_cid_override - Install an explicit cpu->cid mapping
 * @cpu_to_cid: array of nr_cpu_ids s32 entries (cid for each cpu)
 * @cpu_to_cid__sz: must be nr_cpu_ids * sizeof(s32) bytes
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * May only be called from ops.init() of the root scheduler. Replace the
 * topology-probed cid mapping with the caller-provided one. Each possible cpu
 * must map to a unique cid in [0, num_possible_cpus()). Topo info is cleared.
 * On invalid input, trigger scx_error() to abort the scheduler.
 */
/*
 * 仅 root scheduler 的 sleepable ops.init 可覆盖映射。先在进入 RCU 前分配 seen mask，
 * 再校验活动 scheduler、root 身份、精确字节长度以及每个 possible CPU 的 cid 有效且唯一；
 * 任何失败通过 scx_error 终止加载。成功写完双向表并把旧拓扑全部置 -1，自定义映射不
 * 声称 core/LLC/node 连续关系。输入数组由 BPF verifier 保证可读，不取得所有权。
 */
/*
 * 业务背景：不重启处理热插拔或自定义分片的 root BPF scheduler 可在 ops.init 提交自己的 CPU↔CID 双射。
 * 入参：cpu_to_cid 是 verifier 保证可读的借用 s32 数组；cpu_to_cid__sz 是字节长度且须等于
 * nr_cpu_ids*sizeof(s32)；aux 是不可空隐式 BPF 程序上下文借用指针，三者 ownership 均不转移。
 * 出参/返回：无直接返回值；成功覆盖双向表并清空拓扑，失败通过 scx_error 中止 scheduler 加载。
 * 注意事项：仅 root sleepable ops.init；分配在 RCU 前完成，数组每个 possible CPU 的 CID 必须唯一且有效。
 */
__bpf_kfunc void scx_bpf_cid_override(const s32 *cpu_to_cid, u32 cpu_to_cid__sz,
				      const struct bpf_prog_aux *aux)
{
	cpumask_var_t seen __free(free_cpumask_var) = CPUMASK_VAR_NULL;
	struct scx_sched *sch;
	bool alloced;
	s32 cpu, cid;

	/* GFP_KERNEL alloc must happen before the rcu read section */
	/* 可睡眠分配不能放进随后 guard(rcu) 的不可睡眠读侧区间。 */
	alloced = zalloc_cpumask_var(&seen, GFP_KERNEL);

	guard(rcu)();

	/* 阶段一：把 aux 解析为仍活动的 scheduler；卸载竞态下无对象可报告便直接返回。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return;

	/* 阶段二：所有可报告错误都归到 sch；仅 root scheduler 且长度精确时进入元素校验。 */
	if (!alloced) {
		scx_error(sch, "scx_bpf_cid_override: failed to allocate cpumask");
		return;
	}

	/* 非 root scheduler 不能替换全 hierarchy 共享的映射，即使数组内容本身合法。 */
	if (scx_parent(sch)) {
		scx_error(sch, "scx_bpf_cid_override() only allowed from root sched");
		return;
	}

	if (cpu_to_cid__sz != nr_cpu_ids * sizeof(s32)) {
		scx_error(sch, "scx_bpf_cid_override: expected %zu bytes, got %u",
			  nr_cpu_ids * sizeof(s32), cpu_to_cid__sz);
		return;
	}

	/* 阶段三：seen 同时验证唯一性；每个元素通过后立即写双向表，失败由加载中止隔离半成品。 */
	for_each_possible_cpu(cpu) {
		s32 c = cpu_to_cid[cpu];

		/* 先验证范围，再以 CID 为 seen 位原子检测重复，任何失败都不继续发布后续元素。 */
		if (!cid_valid(sch, c))
			return;
		if (cpumask_test_and_set_cpu(c, seen)) {
			scx_error(sch, "cid %d assigned to multiple cpus", c);
			return;
		}
		scx_cpu_to_cid_tbl[cpu] = c;
		scx_cid_to_cpu_tbl[c] = cpu;
	}

	/* Invalidate stale topo info - the override carries no topology. */
	/* 覆盖只承诺一一映射，必须清除默认探测留下的拓扑，防止 BPF 误用陈旧分片。 */
	for (cid = 0; cid < num_possible_cpus(); cid++)
		scx_cid_topo[cid] = SCX_CID_TOPO_NEG;
}

/**
 * scx_bpf_cid_to_cpu - Return the raw CPU id for @cid
 * @cid: cid to look up
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Return the raw CPU id for @cid. Trigger scx_error() and return -EINVAL if
 * @cid is invalid. The cid<->cpu mapping is static for the lifetime of the
 * loaded scheduler, so the BPF side can cache the result to avoid repeated
 * kfunc invocations.
 */
/*
 * 在 RCU 下解析发起 BPF 程序所属 scheduler；无活动 scheduler 返回 -EINVAL，有效时
 * 复用带 scx_error 的边界检查并返回稳定 raw CPU。映射生命周期允许 BPF 缓存结果。
 */
/*
 * 业务背景：BPF 策略最终调用内核 CPU API 时需把稠密 CID 翻回 raw CPU，并获得稳定可缓存结果。
 * 入参：cid 是外部有符号输入，合法范围 [0,num_possible_cpus())；aux 是不可空隐式 BPF 上下文借用指针。
 * 出参/返回：成功返回非负 raw CPU；无活动 scheduler 或无效 CID 返回 -EINVAL，后者还触发 scx_error。
 * 注意事项：RCU 只稳定 scheduler 解析；表永久存在且当前 scheduler 生命周期内元素不变，函数不睡眠。
 */
__bpf_kfunc s32 scx_bpf_cid_to_cpu(s32 cid, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -EINVAL;
	return scx_cid_to_cpu(sch, cid);
}

/**
 * scx_bpf_cpu_to_cid - Return the cid for @cpu
 * @cpu: cpu to look up
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Return the cid for @cpu. Trigger scx_error() and return -EINVAL if @cpu is
 * invalid. The cid<->cpu mapping is static for the lifetime of the loaded
 * scheduler, so the BPF side can cache the result to avoid repeated kfunc
 * invocations.
 */
/* 与 cid_to_cpu 对偶：验证 BPF 上下文和 possible CPU，成功返回当前 scheduler 稳定 cid。 */
/*
 * 业务背景：BPF 从内核获得 raw CPU 后需转换为其分片使用的稠密 CID，避免稀疏编号泄漏到策略布局。
 * 入参：cpu 是外部有符号 raw 编号且须为 possible CPU；aux 是不可空隐式 BPF 上下文借用指针。
 * 出参/返回：成功返回非负 CID；无活动 scheduler 或无效 CPU 返回 -EINVAL，后者触发 scx_error。
 * 注意事项：RCU 内只读永久表，映射可在 scheduler 生命周期内缓存；无引用转移且不可睡眠。
 */
__bpf_kfunc s32 scx_bpf_cpu_to_cid(s32 cpu, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -EINVAL;
	return scx_cpu_to_cid(sch, cpu);
}

/*
 * Set ops on cmasks. cmask_walk_op2() shares one walk across mutating
 * (and/or/copy/andnot) and predicate (subset/intersects) two-cmask forms;
 * cmask_walk_op1() does the same shape over a single cmask range. Every public
 * entry passes a compile-time-constant @op; cmask_walk_op{1,2}() and
 * cmask_word_op{1,2}() are __always_inline so the inner switch collapses to the
 * selected op and cmask_op2_is_pred() folds the predicate early-exit out of
 * mutating ops.
 *
 * Two-cmask ops only touch @dst bits inside the intersection of the two ranges;
 * bits outside stay untouched. In particular, scx_cmask_copy() does NOT zero
 * @dst bits that lie outside @src's range.
 *
 * The _RACY variants are otherwise identical to their non-racy counterpart but
 * read @src word-by-word via data_race(). Memory ordering with concurrent
 * writers is the caller's responsibility.
 */
/*
 * 双掩码操作共用按 word 遍历器，公开入口传入编译期常量 op，使 inline switch 在生成
 * 代码中折叠。只操作两个 active range 的交集，dst 交集外保持不变；谓词命中即短路。
 * _RACY 仅用 data_race 允许 src 并发更新，得到逐 word 混合时刻快照，不建立内存序。
 */
enum cmask_op2 {
	/* mutating */
	CMASK_OP2_AND,
	CMASK_OP2_OR,
	CMASK_OP2_OR_RACY,
	CMASK_OP2_COPY,
	CMASK_OP2_COPY_RACY,
	CMASK_OP2_ANDNOT,
	/* predicates - short-circuit when the per-word result is true */
	CMASK_OP2_SUBSET,
	CMASK_OP2_INTERSECTS,
};

/* 标识需要在首个 true word 处短路的两种只读谓词；变更操作必须遍历全部交集。 */
/*
 * 业务背景：共享双 mask walker 只有谓词可在首个命中 word 提前结束，变更操作必须处理完整交集。
 * 入参：op 是 enum cmask_op2 的纯输入合法枚举值。
 * 出参/返回：SUBSET/INTERSECTS 返回 true，其余变更 op 返回 false；无输出参数或副作用。
 * 注意事项：新增谓词必须同步更新此分类；编译期常量 op 让 always_inline 分支折叠，函数不睡眠。
 */
static __always_inline bool cmask_op2_is_pred(const enum cmask_op2 op)
{
	return op == CMASK_OP2_SUBSET || op == CMASK_OP2_INTERSECTS;
}

/*
 * 在 mask 指定的一个全局 cid word 区间执行 op。变更操作仅更新 av 的 mask 位并返回
 * false；subset 把 bp 解释为 sub、av 为 super，intersects 测共同位，二者返回是否命中。
 */
/*
 * 业务背景：范围化 cmask 操作需要在首尾 word 只处理有效位，同时复用同一内核热路径实现六种变更和两种谓词。
 * 入参：av 是不可空、借用的可写目标 word；bp 是不可空只读借用源 word；mask 是有效位位图；op 是合法枚举。
 * 出参/返回：变更 op 原地更新 av 并返回 false；SUBSET 返回存在缺位，INTERSECTS 返回存在共同位。
 * 注意事项：RACY op 允许 bp 并发且无顺序保证，其他 op 由调用者同步；未知 op 进入 unreachable。
 */
static __always_inline bool cmask_word_op2(u64 *av, const u64 *bp, u64 mask,
					   const enum cmask_op2 op)
{
	switch (op) {
	/* 变更组始终保留 mask 外目标位，并以 false 表示 walker 不应短路。 */
	case CMASK_OP2_AND:
		*av &= ~mask | *bp;
		return false;
	case CMASK_OP2_OR:
		*av |= *bp & mask;
		return false;
	case CMASK_OP2_OR_RACY:
		*av |= data_race(*bp) & mask;
		return false;
	/* copy 组覆盖而非累加 mask 位，RACY 版本仅放宽源 word 的并发读取标注。 */
	case CMASK_OP2_COPY:
		*av = (*av & ~mask) | (*bp & mask);
		return false;
	case CMASK_OP2_COPY_RACY:
		*av = (*av & ~mask) | (data_race(*bp) & mask);
		return false;
	case CMASK_OP2_ANDNOT:
		*av &= ~(*bp & mask);
		return false;
	/* 谓词组不修改 av：返回值直接表示当前 word 已找到反例或交点。 */
	case CMASK_OP2_SUBSET:
		/* stop on the first bit in @sub not set in @super */
		/* 找到 sub 中存在而 super 中缺失的首个 word 即可判定不是子集。 */
		return (*bp & ~*av) & mask;
	case CMASK_OP2_INTERSECTS:
		return (*av & *bp) & mask;
	}
	unreachable();
}

/*
 * Walk the intersection of [@a_base, @a_base + @a_nr_cids) with [@b_base,
 * @b_base + @b_nr_cids) word by word, applying @op. Mutating ops walk all words
 * and return false; predicates return true on the first word whose per-word
 * test is true. Empty intersection returns false (matches "no bits to consider"
 * for both mutate and predicate).
 *
 * Base/nr_cids are taken as parameters so callers with snapshotted bounds can
 * drive the walk with values independent of the cmask's header.
 */
/*
 * 计算两半开区间交集，把全局 word 编号换算成各 bits[] 局部下标；首尾分别用 head/
 * tail mask 遮掉 padding，同 word 时合并两 mask。空交集返回 false，谓词可早停，变更
 * 操作处理到尾 word。独立 base/长度参数允许调用者使用已快照的 header。
 */
/*
 * 业务背景：公开双 cmask API 的 base/长度可以不同，必须只遍历交集并正确遮住各自存储的 padding。
 * 入参：a_bits 是不可空可写目标数组，a_base/a_nr_cids 定义其半开窗口；b_bits 是不可空只读源数组，
 * b_base/b_nr_cids 定义其窗口；op 是合法双 mask 操作，所有指针均借用且容量须覆盖 header。
 * 出参/返回：谓词首个命中返回 true，否则 false；变更 op 返回 false并只修改 a 交集位，ownership 不变。
 * 注意事项：base+长度不得 u32 溢出；空交集不访问 bits，RACY 与非 RACY 同步责任由 op/调用者决定。
 */
static __always_inline bool cmask_walk_op2(u64 *a_bits, u32 a_base, u32 a_nr_cids,
					   const u64 *b_bits, u32 b_base, u32 b_nr_cids,
					   const enum cmask_op2 op)
{
	/* 先快照交集边界和全局/局部 word 偏移；hi-1 只在后续空集检查后用于访问。 */
	u32 lo = max(a_base, b_base);
	u32 hi = min(a_base + a_nr_cids, b_base + b_nr_cids);
	u32 a_word_off = a_base / 64;
	u32 b_word_off = b_base / 64;
	u32 lo_word = lo / 64;
	u32 hi_word = (hi - 1) / 64;
	u64 head_mask = GENMASK_U64(63, lo & 63);
	u64 tail_mask = GENMASK_U64((hi - 1) & 63, 0);
	u32 w;

	/* 先拒绝空交集，避免 hi-1 的回绕结果被用于任何 bits 下标。 */
	if (lo >= hi)
		return false;

	/* 单 word 交集必须同时应用 head/tail mask，一次调用即可得到完整结果。 */
	if (lo_word == hi_word)
		return cmask_word_op2(&a_bits[lo_word - a_word_off],
				      &b_bits[lo_word - b_word_off],
				      head_mask & tail_mask, op);

	/* 多 word 先处理带 head mask 的首 word，谓词命中才允许提前结束。 */
	if (cmask_word_op2(&a_bits[lo_word - a_word_off],
			   &b_bits[lo_word - b_word_off], head_mask, op) &&
	    cmask_op2_is_pred(op))
		return true;

	/* 中间整 word 使用全 mask；变更 op 全遍历，谓词在首个 true 处短路。 */
	for (w = lo_word + 1; w < hi_word; w++)
		if (cmask_word_op2(&a_bits[w - a_word_off],
				   &b_bits[w - b_word_off], ~0ULL, op) &&
		    cmask_op2_is_pred(op))
			return true;

	/* 最后只处理 tail mask 内有效位，并把末 word 谓词结果直接交给调用者。 */
	return cmask_word_op2(&a_bits[hi_word - a_word_off],
			      &b_bits[hi_word - b_word_off], tail_mask, op);
}

enum cmask_op1 {
	CMASK_OP1_ANY_SET,
};

/* 单 word 谓词当前仅检测 mask 范围内是否有任一置位。 */
/*
 * 业务背景：单 cmask 范围扫描把每个 word 的有效位测试抽出，便于首尾 padding 与中间整 word 共用。
 * 入参：ap 是不可空只读借用 word；mask 是要检查的有效位；op 当前只允许 CMASK_OP1_ANY_SET。
 * 出参/返回：mask 范围存在任一置位返回 true，否则 false；无输出参数、ownership 变化或副作用。
 * 注意事项：调用者稳定 ap；新增 op 必须补充 switch 与 walker 的短路分类，未知值进入 unreachable。
 */
static __always_inline bool cmask_word_op1(const u64 *ap, u64 mask,
					   const enum cmask_op1 op)
{
	switch (op) {
	case CMASK_OP1_ANY_SET:
		return *ap & mask;
	}
	unreachable();
}

/*
 * Walk [@a_base, @a_base + @a_nr_cids) of @a_bits word by word, applying @op.
 * Returns true on the first word whose per-word test is true; returns false if
 * no word matches or the range is empty. All current op1s short-circuit on
 * per-word true; if a non-predicate op1 lands here, add a cmask_op1_is_pred()
 * guard analogous to cmask_op2_is_pred().
 */
/*
 * 单掩码版按 word 扫描半开区间，首尾遮 padding，首个置位即返回 true；空范围或全零
 * 返回 false。若未来加入变更 op，必须另加谓词判断，不能沿用当前无条件短路结构。
 */
/*
 * 业务背景：empty 和范围外子集检查需要在任意 base 的 cmask 窗口内高效寻找首个置位。
 * 入参：a_bits 是不可空只读借用数组；a_base/a_nr_cids 是 u32 半开窗口；op 当前仅 ANY_SET。
 * 出参/返回：窗口任一有效位置位返回 true，空/全零返回 false；不修改 bits 或转移 ownership。
 * 注意事项：数组容量须覆盖窗口且 base+长度不得溢出；当前所有 op 都是谓词，新增变更 op 要改短路逻辑。
 */
static __always_inline bool cmask_walk_op1(const u64 *a_bits, u32 a_base,
					   u32 a_nr_cids,
					   const enum cmask_op1 op)
{
	/* 将全局 CID 窗口预计算成 bits[] 局部 word 偏移与首尾有效位 mask。 */
	u32 lo = a_base;
	u32 hi = a_base + a_nr_cids;
	u32 a_word_off = a_base / 64;
	u32 lo_word = lo / 64;
	u32 hi_word = (hi - 1) / 64;
	u64 head_mask = GENMASK_U64(63, lo & 63);
	u64 tail_mask = GENMASK_U64((hi - 1) & 63, 0);
	u32 w;

	/* 空窗口在计算出的回绕 hi_word 被使用前返回，集合中自然没有置位。 */
	if (lo >= hi)
		return false;

	/* 单 word 合并首尾遮罩；多 word 则依次检查头、中间和尾部。 */
	if (lo_word == hi_word)
		return cmask_word_op1(&a_bits[lo_word - a_word_off],
				      head_mask & tail_mask, op);

	if (cmask_word_op1(&a_bits[lo_word - a_word_off], head_mask, op))
		return true;
	/* 当前 op 都是谓词，因此中间任一 word 命中可立即返回而无需继续读源。 */
	for (w = lo_word + 1; w < hi_word; w++)
		if (cmask_word_op1(&a_bits[w - a_word_off], ~0ULL, op))
			return true;
	return cmask_word_op1(&a_bits[hi_word - a_word_off], tail_mask, op);
}

/* 对 active range 交集执行 dst &= src；交集外 dst 不变，调用者负责同步读写。 */
/*
 * 业务背景：候选 CID 集需要与约束集求交，同时保留 dst 不在 src 表示范围内的独立分片。
 * 入参：dst 是不可空借用可写 cmask；src 是不可空只读借用 cmask，header/存储均须有效。
 * 出参/返回：无直接返回值；仅交集位执行 dst&=src，交集外不变且 ownership 均不转移。
 * 注意事项：调用者排除 dst/src 并发写并处理别名；函数不分配、不睡眠，空交集无副作用。
 */
void scx_cmask_and(struct scx_cmask *dst, const struct scx_cmask *src)
{
	cmask_walk_op2(dst->bits, dst->base, dst->nr_cids,
		       src->bits, src->base, src->nr_cids, CMASK_OP2_AND);
}

/* 对 active range 交集执行 dst |= src；交集外 dst 不变。 */
/*
 * 业务背景：合并两个 CID 候选集合时只应影响双方都能表示的窗口，避免越界解释 src。
 * 入参：dst 是不可空借用可写 cmask；src 是不可空只读借用 cmask，二者存储/header 有效。
 * 出参/返回：无直接返回值；交集位执行 dst|=src，交集外保持不变且 ownership 不转移。
 * 注意事项：普通版本要求调用者同步 src/dst 并处理别名；空交集无副作用，函数不睡眠。
 */
void scx_cmask_or(struct scx_cmask *dst, const struct scx_cmask *src)
{
	cmask_walk_op2(dst->bits, dst->base, dst->nr_cids,
		       src->bits, src->base, src->nr_cids, CMASK_OP2_OR);
}

/**
 * scx_cmask_or_racy - OR @src into @dst, reading @src without locking
 *
 * @src is read word-by-word through data_race(). Same per-bit independence
 * rationale as scx_cmask_copy_racy(). Memory ordering with writers is the
 * caller's responsibility.
 */
/* 允许 src 并发变化的 OR；逐 word data_race 快照可能来自不同时刻，dst 仍须独占。 */
/*
 * 业务背景：统计/提示类候选集可接受 src 更新中的混合快照，从而避免为只增量合并获取源锁。
 * 入参：dst 是不可空且调用者独占的可写借用 cmask；src 是不可空、可并发变化的只读借用 cmask。
 * 出参/返回：无直接返回值；逐 word 把交集 src 位 OR 入 dst，交集外不变且 ownership 不转移。
 * 注意事项：data_race 不提供一致快照或内存序，只保证显式接受竞态；src header 必须保持稳定。
 */
void scx_cmask_or_racy(struct scx_cmask *dst, const struct scx_cmask *src)
{
	cmask_walk_op2(dst->bits, dst->base, dst->nr_cids,
		       src->bits, src->base, src->nr_cids, CMASK_OP2_OR_RACY);
}

/* 用 src 覆盖两个 active range 的交集；不会清除 dst 位于 src 范围外的位。 */
/*
 * 业务背景：把一个 CID 集合快照复制到不同窗口的目标时，目标独有范围不能被误清零。
 * 入参：dst 是不可空借用可写 cmask；src 是不可空只读借用 cmask，调用者稳定两者。
 * 出参/返回：无直接返回值；仅交集位被 src 覆盖，dst 其余位保持原值且 ownership 不变。
 * 注意事项：这不是整个 dst 清零后复制；需要全替换时调用者先 clear，函数不分配、不睡眠。
 */
void scx_cmask_copy(struct scx_cmask *dst, const struct scx_cmask *src)
{
	cmask_walk_op2(dst->bits, dst->base, dst->nr_cids,
		       src->bits, src->base, src->nr_cids, CMASK_OP2_COPY);
}

/**
 * scx_cmask_copy_racy - Snapshot @src into @dst without locking
 *
 * @src is read word-by-word through data_race(). Head/tail masking matches
 * scx_cmask_copy(). Each bit in a cmask is independent, so partial updates
 * just leave some bits fresher than others. Memory ordering with writers is
 * the caller's responsibility.
 */
/* copy 的无锁 src 版本；各 bit 独立使混合快照可用，但一致性和发布次序由调用者定义。 */
/*
 * 业务背景：无需时间一致性的观察路径可在源集合并发变化时复制各 bit，减少同步开销。
 * 入参：dst 是不可空且独占写的借用 cmask；src 是不可空、允许并发写的只读借用 cmask。
 * 出参/返回：无直接返回值；逐 word 覆盖交集位，交集外 dst 不变且 ownership 均不转移。
 * 注意事项：结果可混合多个时刻，data_race 不建立发布/获取关系；src header 与存储生命周期必须稳定。
 */
void scx_cmask_copy_racy(struct scx_cmask *dst, const struct scx_cmask *src)
{
	cmask_walk_op2(dst->bits, dst->base, dst->nr_cids,
		       src->bits, src->base, src->nr_cids, CMASK_OP2_COPY_RACY);
}

/* 对交集执行 dst &= ~src；src 范围外不会意外清除 dst。 */
/*
 * 业务背景：从候选 CID 集删除另一个集合时，只能清除 src 确实表示的交集位。
 * 入参：dst 是不可空借用可写 cmask；src 是不可空只读借用 cmask，调用者同步并稳定存储。
 * 出参/返回：无直接返回值；交集位执行 dst&=~src，dst 其余范围不变且 ownership 不转移。
 * 注意事项：普通读取不容许 src 并发更新；空交集无副作用，函数不分配、不睡眠。
 */
void scx_cmask_andnot(struct scx_cmask *dst, const struct scx_cmask *src)
{
	cmask_walk_op2(dst->bits, dst->base, dst->nr_cids,
		       src->bits, src->base, src->nr_cids, CMASK_OP2_ANDNOT);
}

/*
 * Return true if @cm has any bit set in [@lo, @hi). Caller must ensure
 * [@lo, @hi) is contained in @cm's range.
 */
/*
 * 检查 cm 内已保证有效的子区间是否有置位；把 bits 指针预偏移到 lo 所在 word 后复用
 * op1 遍历器。空区间为 false，越界是调用者错误且本层不检查。
 */
/*
 * 业务背景：subset 需额外检查交集外的 sub 区段，复用单 mask walker 可避免逐 CID 测试。
 * 入参：cm 是不可空只读借用 cmask；lo/hi 定义其 active range 内的 u32 半开子区间。
 * 出参/返回：子区间任一位置位返回 true，空/全零返回 false；不修改 cm 或转移 ownership。
 * 注意事项：调用者必须保证 [lo,hi) 完全包含于 cm 窗口且 header/存储稳定；本层不做越界防护。
 */
static bool cmask_any_set_in_range(const struct scx_cmask *cm, u32 lo, u32 hi)
{
	if (lo >= hi)
		return false;
	return cmask_walk_op1(&cm->bits[lo / 64 - cm->base / 64], lo, hi - lo,
			      CMASK_OP1_ANY_SET);
}

/**
 * scx_cmask_subset - test whether @sub is a subset of @super
 * @sub: cmask to test
 * @super: cmask to test against
 *
 * Return true iff every set bit of @sub is also set in @super.
 */
/*
 * 子集判断先扫描 sub 落在 super active range 左右之外的部分，任何置位都直接失败；
 * 再在交集内查找 sub 有而 super 无的位。空 sub 自然为 true，范围无需相同。
 */
/*
 * 业务背景：不同 CID 分片窗口之间验证授权/候选包含关系时，super 窗口外的 sub 置位也必须判失败。
 * 入参：sub、super 是不可空且稳定的只读借用 cmask，范围可不同且 ownership 不变。
 * 出参/返回：sub 每个置位都存在于 super 返回 true，否则 false；空 sub 返回 true，无输出参数。
 * 注意事项：调用者负责同步两对象；base+nr_cids 不得溢出，函数不分配、不睡眠。
 */
bool scx_cmask_subset(const struct scx_cmask *sub, const struct scx_cmask *super)
{
	u32 super_end = super->base + super->nr_cids;
	u32 sub_end = sub->base + sub->nr_cids;

	/*
	 * Set bits in @sub outside @super's range can't be in @super, so any
	 * such bit means not a subset. The walk below only visits words
	 * common to both ranges, so these need a separate scan.
	 */
	/* 交集遍历看不到 super 范围外的 sub 位，必须分别补查左右两段。 */
	if (sub->base < super->base &&
	    cmask_any_set_in_range(sub, sub->base, min(super->base, sub_end)))
		return false;
	if (sub_end > super_end &&
	    cmask_any_set_in_range(sub, max(sub->base, super_end), sub_end))
		return false;

	return !cmask_walk_op2((u64 *)super->bits, super->base, super->nr_cids,
			       sub->bits, sub->base, sub->nr_cids, CMASK_OP2_SUBSET);
}

/* 返回两个 active range 交集内是否存在共同置位；空交集为 false，不修改任一对象。 */
/*
 * 业务背景：调度器需快速判断两个 CID 候选/授权集合是否存在至少一个共同 CPU 分片。
 * 入参：a、b 是不可空、稳定的只读借用 cmask，active range 可以不同。
 * 出参/返回：交集内存在共同置位返回 true，空交集或无共同位返回 false；无副作用和 ownership 变化。
 * 注意事项：调用者同步并保证 header/存储有效；内部为复用 walker 对 a_bits 去 const 但谓词不会写。
 */
bool scx_cmask_intersects(const struct scx_cmask *a, const struct scx_cmask *b)
{
	return cmask_walk_op2((u64 *)a->bits, a->base, a->nr_cids,
			      b->bits, b->base, b->nr_cids, CMASK_OP2_INTERSECTS);
}

/**
 * scx_cmask_empty - Test whether @m has no bits set
 * @m: cmask to test
 *
 * Return true iff @m's active range has no bits set.
 */
/* 对完整 active range 做 any-set 的逻辑取反；空范围按集合语义返回 true。 */
/*
 * 业务背景：候选集快速路径需要区分“没有可用 CID”而无需遍历每个逻辑编号。
 * 入参：m 是不可空、稳定的只读借用 cmask，header 与 bits 容量一致。
 * 出参/返回：active range 无置位（含空范围）返回 true，否则 false；无输出参数或 ownership 变化。
 * 注意事项：调用者负责与 writer 同步；只检查 active range，容量中不可见的旧位不影响结果。
 */
bool scx_cmask_empty(const struct scx_cmask *m)
{
	return !cmask_any_set_in_range(m, m->base, m->base + m->nr_cids);
}

/**
 * scx_bpf_cid_topo - Copy out per-cid topology info
 * @cid: cid to look up
 * @out__uninit: where to copy the topology info; fully written by this call
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Fill @out__uninit with the topology info for @cid. Trigger scx_error() if
 * @cid is out of range. If @cid is valid but in the no-topo section, all fields
 * are set to -1.
 */
/*
 * 在 RCU 下取得活动 scheduler 并验证 cid；失败时也完整写出全 -1，避免 BPF 观察未初始化
 * 输出。成功按值复制稳定拓扑项，无拓扑尾段本身即为全 -1。
 */
/*
 * 业务背景：BPF 策略按 CID 分片后需要查询其 core/LLC/node 连续范围起点和索引。
 * 入参：cid 是外部有符号编号；out__uninit 是 verifier 提供、不可空且由本函数完整写出的输出对象；
 * aux 是不可空隐式 BPF 上下文借用指针，所有权均不转移。
 * 出参/返回：无直接返回值；成功复制拓扑，scheduler/CID 无效或无拓扑时完整写全 -1。
 * 注意事项：RCU 稳定 scheduler 解析；cid 无效触发 scx_error，写输出优先于返回以免泄漏未初始化字节。
 */
__bpf_kfunc void scx_bpf_cid_topo(s32 cid, struct scx_cid_topo *out__uninit,
				  const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	/* 输出对象先由分支完整赋值；RCU 读侧仅负责把 aux 安全解析为活动 scheduler。 */
	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch) || !cid_valid(sch, cid)) {
		*out__uninit = SCX_CID_TOPO_NEG;
		return;
	}

	*out__uninit = READ_ONCE(scx_cid_topo)[cid];
}

/* kfunc 函数定义区到此结束，以下 BTF ID/集合只描述 verifier 可见性而不执行查询。 */
__bpf_kfunc_end_defs();

/* override 只允许 sleepable STRUCT_OPS init 上下文，避免运行期改写稳定映射。 */
BTF_KFUNCS_START(scx_kfunc_ids_init)
BTF_ID_FLAGS(func, scx_bpf_cid_override, KF_IMPLICIT_ARGS | KF_SLEEPABLE)
BTF_KFUNCS_END(scx_kfunc_ids_init)

static const struct btf_kfunc_id_set scx_kfunc_set_init = {
	.owner	= THIS_MODULE,
	.set	= &scx_kfunc_ids_init,
	.filter	= scx_kfunc_context_filter,
};

/* 三个只读查询可供已通过 filter 的 STRUCT_OPS、TRACING 和 SYSCALL BPF 程序调用。 */
BTF_KFUNCS_START(scx_kfunc_ids_cid)
BTF_ID_FLAGS(func, scx_bpf_cid_to_cpu, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cpu_to_cid, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cid_topo, KF_IMPLICIT_ARGS)
BTF_KFUNCS_END(scx_kfunc_ids_cid)

static const struct btf_kfunc_id_set scx_kfunc_set_cid = {
	.owner	= THIS_MODULE,
	.set	= &scx_kfunc_ids_cid,
};

/*
 * 依次为 STRUCT_OPS init 专用集合及 STRUCT_OPS/TRACING/SYSCALL 通用查询集合注册 BTF
 * kfunc；GNU `?:` 在首个非零错误处短路并返回，全部成功才返回 0，无回滚已注册集合。
 */
/*
 * 业务背景：BPF verifier 只有登记 kfunc ID 集后，才能按程序类型允许 CID override 与只读查询调用。
 * 入参：无。
 * 出参/返回：四次注册全成功返回 0，否则返回首个非零错误；先前成功的集合保持注册，无输出参数。
 * 注意事项：仅 sched_ext 初始化期调用；GNU 省略中项 `?:` 保证短路，不提供事务回滚或 ownership 转移。
 */
int scx_cid_kfunc_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &scx_kfunc_set_init) ?:
		register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &scx_kfunc_set_cid) ?:
		register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING, &scx_kfunc_set_cid) ?:
		register_btf_kfunc_id_set(BPF_PROG_TYPE_SYSCALL, &scx_kfunc_set_cid);
}
