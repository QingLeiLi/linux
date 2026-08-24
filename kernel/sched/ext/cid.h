/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Topological CPU IDs (cids)
 * --------------------------
 *
 * Raw cpu numbers are clumsy for sharding work and communication across
 * topology units, especially from BPF: the space can be sparse, numerical
 * closeness doesn't imply topological closeness (x86 hyperthreading often puts
 * SMT siblings far apart), and a range of cpu ids doesn't mean anything.
 * Sub-scheds make this acute - cpu allocation, revocation and other state are
 * constantly communicated across sub-scheds, and passing whole cpumasks scales
 * poorly with cpu count. cpumasks are also awkward in BPF: a variable-length
 * kernel type sized for the maximum NR_CPUS (4k), with verbose helper sequences
 * for every op.
 *
 * cids give every cpu a dense, topology-ordered id. CPUs sharing a core, LLC or
 * NUMA node get contiguous cid ranges, so a topology unit becomes a (start,
 * length) slice of cid space. Communication can pass a slice instead of a
 * cpumask, and BPF code can process, for example, a u64 word's worth of cids at
 * a time.
 *
 * The mapping is built once at root scheduler enable time by walking the
 * topology of online cpus only. Going by online cpus is out of necessity:
 * depending on the arch, topology info isn't reliably available for offline
 * cpus. The expected usage model is restarting the scheduler on hotplug events
 * so the mapping is rebuilt against the new online set. A scheduler that wants
 * to handle hotplug without a restart can provide its own cid and shard mapping
 * through the override interface.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Tejun Heo <tj@kernel.org>
 */
/*
 * 拓扑 CPU ID（cid）为稀疏、数值上不表达拓扑距离的 raw CPU 编号建立稠密顺序。
 * 同一 core、LLC、NUMA node 的 CPU 获得连续 cid，因此子调度器可传递“起点+长度”
 * 分片并按 u64 批处理，而无需传递最大 NR_CPUS 尺寸的 cpumask。默认映射在 root
 * scheduler 启用时仅根据 online CPU 构建；离线 CPU 拓扑不可靠，通常由热插拔重启
 * scheduler 重建，若要不停机处理则由 BPF override 提供自定义映射。
 */
#ifndef _KERNEL_SCHED_EXT_CID_H
#define _KERNEL_SCHED_EXT_CID_H

#include "internal.h"

struct scx_sched;

/*
 * Cid space (total is always num_possible_cpus()) is laid out with
 * topology-annotated cids first, then no-topo cids at the tail. The
 * topology-annotated block covers the cpus that were online when scx_cid_init()
 * ran and remains valid even after those cpus go offline. The tail block covers
 * possible-but-not-online cpus and carries all-(-1) topo info (see
 * scx_cid_topo); callers detect it via the -1 sentinels.
 *
 * See the comment above the table definitions in cid.c for the
 * memory-ordering and visibility contract.
 */
/*
 * cid 总数恒为 num_possible_cpus()：初始化时 online 且有拓扑的 CPU 位于前段，同一
 * 拓扑单元连续；其后即使下线映射仍稳定。possible 但未 online/无可用拓扑的 CPU
 * 位于尾段，scx_cid_topo 六字段均为 -1。表指针首次启用时以 WRITE_ONCE 发布且永不
 * 撤销；默认映射在 ops.init 前填完，override 在返回前提交，使用者只能从这些边界后读。
 */
/* cid→raw CPU 和 raw CPU→cid 双向表；元素在当前 scheduler 生命周期内稳定。 */
extern s16 *scx_cid_to_cpu_tbl;
extern s16 *scx_cpu_to_cid_tbl;
/* 按 cid 索引的 core/LLC/node 起点与序号，只读拓扑快照。 */
extern struct scx_cid_topo *scx_cid_topo;
/* 只允许 root ops.init 调用的 override kfunc BTF 集，由 ext 注册流程消费。 */
extern struct btf_id_set8 scx_kfunc_ids_init;

/*
 * 以下 cmask 函数只操作各对象 active range；双掩码操作只触及区间交集，交集之外
 * dst 位保持不变。普通版本要求调用者同步 src/dst；_racy 版本逐 word data_race 读取
 * src，只承诺各 bit 独立的混合时刻快照，内存序由调用者负责。对象存储归调用者所有。
 */
/* 清零 @m active range；无返回值，范围外存储不变，调用者独占写。 */
void scx_cmask_clear(struct scx_cmask *m);
/* 置位 @m active range 并清零首尾 padding；无返回值，范围外存储不变。 */
void scx_cmask_fill(struct scx_cmask *m);
/* 对区间交集执行 @dst &= @src；参数借用，无返回值，调用者同步两者。 */
void scx_cmask_and(struct scx_cmask *dst, const struct scx_cmask *src);
/* 对区间交集执行 @dst |= @src；参数借用，无返回值，调用者同步两者。 */
void scx_cmask_or(struct scx_cmask *dst, const struct scx_cmask *src);
/* OR 版本允许 src 并发变化，得到逐 word 混合快照；dst 仍须独占。 */
void scx_cmask_or_racy(struct scx_cmask *dst, const struct scx_cmask *src);
/* 把区间交集的 src 位覆盖到 dst，dst 位于 src 范围外部分保持不变。 */
void scx_cmask_copy(struct scx_cmask *dst, const struct scx_cmask *src);
/* copy 的无锁 src 快照版本；各 word 时刻可不同，内存序由调用者负责。 */
void scx_cmask_copy_racy(struct scx_cmask *dst, const struct scx_cmask *src);
/* 对区间交集执行 @dst &= ~@src；参数借用，无返回值。 */
void scx_cmask_andnot(struct scx_cmask *dst, const struct scx_cmask *src);
/* 返回 sub 的每个置位是否都存在于 super；范围外置位也会使结果为 false。 */
bool scx_cmask_subset(const struct scx_cmask *sub, const struct scx_cmask *super);
/* 返回两个 active range 的交集中是否至少有一个共同置位；空交集为 false。 */
bool scx_cmask_intersects(const struct scx_cmask *a, const struct scx_cmask *b);
/* 返回 @m active range 是否没有置位；空范围为 true。 */
bool scx_cmask_empty(const struct scx_cmask *m);
/* 构建映射可睡眠且要求 cpus_read_lock；返回 0 或负 errno，并可对 @sch 报错。 */
s32 scx_cid_init(struct scx_sched *sch);
/* 注册 CID BTF kfunc 集，返回 0 或首个注册错误；仅初始化期调用。 */
int scx_cid_kfunc_init(void);
/* 清空 @dst active range 后，把 @src 中 CPU 的有效且落窗 cid 置位；无返回值。 */
void scx_cpumask_to_cmask(const struct cpumask *src, struct scx_cmask *dst);

/**
 * cid_valid - Verify a cid value, to be used on ops input args
 * @sch: scx_sched to abort on error
 * @cid: cid which came from a BPF ops
 *
 * Return true if @cid is in [0, num_possible_cpus()). On failure, trigger
 * scx_error() and return false.
 */
/*
 * cid_valid() - 校验来自 BPF ops 的 cid
 * @sch 是错误归属的活动 scheduler，借用且不可为 NULL；@cid 是有符号外部输入。
 * [0,num_possible_cpus()) 返回 true；越界时触发 scx_error() 使 scheduler 进入错误路径
 * 并返回 false。函数不查表、不取得引用且不能睡眠。
 */
static inline bool cid_valid(struct scx_sched *sch, s32 cid)
{
	if (likely(cid >= 0 && cid < num_possible_cpus()))
		return true;
	scx_error(sch, "invalid cid %d", cid);
	return false;
}

/**
 * __scx_cid_to_cpu - Unchecked cid->cpu table lookup
 * @cid: cid to look up. Must be in [0, num_possible_cpus()).
 *
 * Intended for callsites that have already validated @cid and that hold a
 * non-NULL @sch from scx_prog_sched() - a live sched implies the table has
 * been allocated, so no NULL check is needed here.
 */
/*
 * 未检查的 cid→CPU 查表。@cid 必须已验证；调用者持有 scx_prog_sched() 返回的活动
 * sch，从而保证表已分配且不会撤销。返回 raw CPU，无错误码、无引用转移；READ_ONCE
 * 与首次分配的 WRITE_ONCE 配对读取已发布指针，不为元素更新提供额外同步。
 */
static inline s32 __scx_cid_to_cpu(s32 cid)
{
	/* READ_ONCE pairs with WRITE_ONCE in scx_cid_arrays_alloc() */
	/* READ_ONCE 与 scx_cid_arrays_alloc() 中的 WRITE_ONCE 发布配对。 */
	return READ_ONCE(scx_cid_to_cpu_tbl)[cid];
}

/**
 * __scx_cpu_to_cid - Unchecked cpu->cid table lookup
 * @cpu: cpu to look up. Must be a valid possible cpu id.
 *
 * Same usage constraints as __scx_cid_to_cpu().
 */
/* 未检查的 raw CPU→cid 查表；@cpu 必须是 possible CPU，其余生命周期契约同上。 */
static inline s32 __scx_cpu_to_cid(s32 cpu)
{
	return READ_ONCE(scx_cpu_to_cid_tbl)[cpu];
}

/**
 * scx_cid_to_cpu - Translate @cid to its cpu
 * @sch: scx_sched for error reporting
 * @cid: cid to look up
 *
 * Return the cpu for @cid or a negative errno on failure. Invalid cid triggers
 * scx_error() on @sch. The cid arrays are allocated on first scheduler enable
 * and never freed, so the returned cpu is stable for the lifetime of the loaded
 * scheduler.
 */
/*
 * 安全 cid→CPU 接口。@sch 仅用于错误上报，@cid 可来自 BPF；有效时返回稳定 raw CPU，
 * 无效时 scx_error 并返回 -EINVAL。函数无状态修改（错误状态除外）、不睡眠。
 */
static inline s32 scx_cid_to_cpu(struct scx_sched *sch, s32 cid)
{
	if (!cid_valid(sch, cid))
		return -EINVAL;
	return __scx_cid_to_cpu(cid);
}

/**
 * scx_cpu_to_cid - Translate @cpu to its cid
 * @sch: scx_sched for error reporting
 * @cpu: cpu to look up
 *
 * Return the cid for @cpu or a negative errno on failure. Invalid cpu triggers
 * scx_error() on @sch. Same lifetime guarantee as scx_cid_to_cpu().
 */
/*
 * 安全 raw CPU→cid 接口。scx_cpu_valid() 同时验证 possible 范围并把错误记到 @sch；
 * 成功返回稳定 cid，失败返回 -EINVAL。参数均借用，无引用/ownership 变化。
 */
static inline s32 scx_cpu_to_cid(struct scx_sched *sch, s32 cpu)
{
	if (!scx_cpu_valid(sch, cpu, NULL))
		return -EINVAL;
	return __scx_cpu_to_cid(cpu);
}

/**
 * scx_is_cid_type - Test whether the active scheduler hierarchy is cid-form
 */
/* 读取静态键判断当前活动 scheduler hierarchy 是否使用 cid 形式；无锁、无副作用。 */
static inline bool scx_is_cid_type(void)
{
	return static_branch_unlikely(&__scx_is_cid_type);
}

/* 判断 @cid 是否落在 @m 的半开 active range；仅读 header，不访问 bits。 */
static inline bool __scx_cmask_contains(u32 cid, const struct scx_cmask *m)
{
	return likely(cid >= m->base && cid < m->base + m->nr_cids);
}

/* Word in bits[] covering @cid. @cid must satisfy __scx_cmask_contains(). */
/*
 * 返回覆盖 @cid 的 bits[] word 指针；调用前必须确认 cid 在 active range 内。
 * base/64 把全局 cid word 编号换算为本地柔性数组下标，返回指针不转移存储所有权。
 */
static inline u64 *__scx_cmask_word(u32 cid, const struct scx_cmask *m)
{
	return (u64 *)&m->bits[cid / 64 - m->base / 64];
}

/**
 * __scx_cmask_init - Initialize @m with explicit storage capacity
 * @m: cmask to initialize
 * @base: first cid of the active range
 * @nr_cids: number of cids in the active range
 * @alloc_cids: storage capacity in cids, at least @nr_cids
 *
 * Use when storage is sized larger than the initial active range. All of
 * bits[] is zeroed.
 */
/*
 * 以显式容量初始化调用者提供的 cmask。@base/@nr_cids 定义 active 半开区间，
 * @alloc_cids 决定实际 word 容量且应不少于 nr_cids；不足只告警并截短 active 长度。
 * 函数写 header 并清零全部已分配 word，保证头尾 padding 为零；无返回、无分配，
 * 调用者必须提供 SCX_CMASK_NR_WORDS(alloc_cids) 大小且排除并发访问。
 */
static inline void __scx_cmask_init(struct scx_cmask *m, u32 base, u32 nr_cids,
				    u32 alloc_cids)
{
	if (WARN_ON_ONCE(alloc_cids < nr_cids))
		nr_cids = alloc_cids;

	m->base = base;
	m->nr_cids = nr_cids;
	m->alloc_words = SCX_CMASK_NR_WORDS(alloc_cids);
	memset(m->bits, 0, m->alloc_words * sizeof(u64));
}

/**
 * scx_cmask_init - Initialize @m on tight storage
 * @m: cmask to initialize
 * @base: first cid of the active range
 * @nr_cids: number of cids in the active range
 *
 * All of bits[] is zeroed.
 */
/*
 * 紧容量初始化 wrapper：存储容量与 active 长度相同。参数/同步契约同内部版本；
 * nr_cids 为 0 时 alloc_words 为 0 且不触及 bits。
 */
static inline void scx_cmask_init(struct scx_cmask *m, u32 base, u32 nr_cids)
{
	__scx_cmask_init(m, base, nr_cids, nr_cids);
}

/**
 * scx_cmask_reframe - Reshape @m's active range without resizing storage
 * @m: cmask to reframe
 * @base: new active range base
 * @nr_cids: new active range length, must fit within @m->alloc_words
 *
 * Body bits within the new range become garbage - only the head and tail
 * words are zeroed to keep the padding invariant.
 */
/*
 * 在不扩容的情况下重设 @m active range。新范围所需 word 超过 alloc_words 时告警
 * 并保持对象原样；非空范围先清零首尾 word，确保范围外 padding 为零，但中间 body
 * word 故意保留旧垃圾，调用者必须随后 fill/copy/set 后才能把它当有效集合。空范围
 * 只更新 header。函数无锁/分配，要求独占修改 header 与边界 word。
 */
static inline void scx_cmask_reframe(struct scx_cmask *m, u32 base, u32 nr_cids)
{
	if (WARN_ON_ONCE(SCX_CMASK_NR_WORDS(nr_cids) > m->alloc_words))
		return;

	if (nr_cids) {
		/* last_word 是新 active range 相对 bits[0] 覆盖的最后 word。 */
		u32 last_word = ((base & 63) + nr_cids - 1) / 64;

		m->bits[0] = 0;
		m->bits[last_word] = 0;
	}

	m->base = base;
	m->nr_cids = nr_cids;
}

/*
 * 若 @cid 落在 @m active range 内则以非原子 OR 置位，否则静默忽略。调用者负责
 * dst 写同步；函数不提供跨 CPU 原子性或内存序，仅修改对应 word。
 */
static inline void __scx_cmask_set(u32 cid, struct scx_cmask *m)
{
	if (!__scx_cmask_contains(cid, m))
		return;
	*__scx_cmask_word(cid, m) |= BIT_U64(cid & 63);
}

/**
 * scx_cmask_test - test whether @cid is set in @m
 * @cid: cid to test
 * @m: cmask to test
 *
 * Return %false if @cid is outside @m's active range. Otherwise return the
 * bit's value. Read via READ_ONCE so callers can race set/clear writers.
 */
/*
 * 测试 @cid；范围外返回 false，范围内以 READ_ONCE 读取整个 u64 后取位。它允许与
 * 对齐 word 的 set/clear 竞态并避免编译器撕裂/重复读取，但不形成一致快照或 acquire
 * 顺序；@m 存储和 header 必须保持有效稳定。
 */
static inline bool scx_cmask_test(u32 cid, const struct scx_cmask *m)
{
	if (!__scx_cmask_contains(cid, m))
		return false;
	return READ_ONCE(*__scx_cmask_word(cid, m)) & BIT_U64(cid & 63);
}

/*
 * Words of bits[] the active range spans, 0 if empty. Tighter than the storage
 * SCX_CMASK_NR_WORDS() sizes for the worst-case base alignment.
 */
/*
 * 返回 active range 实际跨越的 word 数；空范围为 0。计算包含 base 在首 word 的偏移，
 * 因而比按最坏对齐分配的容量更紧。仅读 header，无失败或副作用。
 */
static inline u32 scx_cmask_nr_used_words(const struct scx_cmask *m)
{
	if (!m->nr_cids)
		return 0;
	return ((m->base & 63) + m->nr_cids - 1) / 64 + 1;
}

/**
 * scx_cmask_for_each_cid - iterate set cids in @m
 * @cid: s32 loop var that receives each set cid in turn
 * @m: cmask to iterate
 *
 * Visits set bits within @m's active range in ascending order. Scans only the
 * words the active range spans, where head and tail padding is kept zero, so
 * no per-cid range check is needed.
 */
/*
 * 按升序遍历 @m active range 中所有置位 cid。@cid 必须是调用者提供的 s32 左值；宏
 * 每个 word 仅 READ_ONCE 一次，用 __ffs64 找最低位并以 w&=w-1 删除它。正确性依赖
 * init/fill/reframe 维持首尾 padding 为零，因此循环内无需逐 cid 边界判断。并发写时
 * 只得到逐 word 混合快照；宏不加锁，@m header 与存储生命周期必须稳定。
 */
#define scx_cmask_for_each_cid(cid, m)						\
	for (u64 __bs = (m)->base & ~63u, __wi = 0,				\
		     __nw = scx_cmask_nr_used_words(m);				\
	     __wi < __nw; __wi++)						\
		for (u64 __w = READ_ONCE((m)->bits[__wi]);			\
		     __w && ((cid) = __bs + __wi * 64 + __ffs64(__w), true);	\
		     __w &= __w - 1)

/*
 * scx_cpu_arg() wraps a cpu arg being handed to an SCX op. For cid-form
 * schedulers it resolves to the matching cid; for cpu-form it passes @cpu
 * through. scx_cpu_ret() is the inverse for a cpu/cid returned from an op
 * (currently only ops.select_cpu); it validates the BPF-supplied cid and
 * triggers scx_error() on @sch if invalid.
 */
/*
 * SCX op 的 CPU 参数适配层：cid-form hierarchy 把内核 raw CPU 转成 cid 后交给 BPF，
 * cpu-form 原样传递；反向返回路径保留负错误码，非负 cid 先校验再转 raw CPU。两者
 * 都只查稳定映射，不取得引用；调用者保证活动 scheduler 生命周期。
 */
/* @cpu 是内核已验证 raw CPU；返回传给 ops 的 raw CPU 或 cid，不会失败。 */
static inline s32 scx_cpu_arg(s32 cpu)
{
	if (scx_is_cid_type())
		return __scx_cpu_to_cid(cpu);
	return cpu;
}

/* @cpu_or_cid 为 ops 返回值；负值/CPU-form 原样返回，非法 cid 触发 @sch 错误并 -EINVAL。 */
static inline s32 scx_cpu_ret(struct scx_sched *sch, s32 cpu_or_cid)
{
	if (cpu_or_cid < 0 || !scx_is_cid_type())
		return cpu_or_cid;
	return scx_cid_to_cpu(sch, cpu_or_cid);
}

#endif /* _KERNEL_SCHED_EXT_CID_H */
