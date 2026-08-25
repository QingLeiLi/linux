/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Early sched_ext type definitions.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Tejun Heo <tj@kernel.org>
 */
/*
 * sched_ext 在完整调度器内部布局之前就需要的基础类型和尺寸常量。
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 原文所称“早期定义”表示这些类型会被 sched.h/internal.h 等底层头文件共同引用，必须避免
 * 依赖后续完整结构。本文件不分配、不发布对象；它规定 dispatch/watchdog/bypass 的有界工作量，
 * 描述 CPU 经拓扑重排后的稠密 cid 坐标，并为内核/BPF 共享的可变长 cid bitmap 定义内存布局。
 *
 * cmask 的核心不变量是 active 区间为 [base, base + nr_cids)，bits[] 却始终按全局 64-cid
 * 网格对齐；头尾 padding 为 0，容量由 alloc_words 描述。这样不同 base 的 cmask 可按 word
 * AND/OR 而无需移位，但所有构造/重构调用者必须防止区间回绕并保证声明容量足够。
 */
#ifndef _KERNEL_SCHED_EXT_TYPES_H
#define _KERNEL_SCHED_EXT_TYPES_H

#include <linux/types.h>
#include <linux/jiffies.h>
#include <linux/overflow.h>
#include <linux/time64.h>
#include <linux/sched/topology.h>

enum scx_consts {
	/* 单次 BPF dispatch 默认最多接受 32 个插入，scheduler 可在受限范围内覆盖。 */
	SCX_DSP_DFL_MAX_BATCH		= 32,
	/* 一次 pick/dispatch 最多循环 32 轮，防止 BPF 不产出 runnable task 时无界占用 rq 锁。 */
	SCX_DSP_MAX_LOOPS		= 32,
	/* 用户可配置 watchdog timeout 的上限为 30 秒，单位 jiffies。 */
	SCX_WATCHDOG_MAX_TIMEOUT	= 30 * HZ,

	/* per-CPU chunk size for p->scx.tid allocation, see scx_alloc_tid() */
	/* 原文说明 p->scx.tid 按每 CPU 1024 个一块领取，详见 scx_alloc_tid()，以摊薄全局原子操作。 */
	SCX_TID_CHUNK			= 1024,

	/* scheduler 退出诊断最多保存 64 个栈地址。 */
	SCX_EXIT_BT_LEN			= 64,
	/* 退出原因格式化消息缓冲区为 1024 字节，过长文本由格式化路径截断。 */
	SCX_EXIT_MSG_LEN		= 1024,
	/* 未显式配置时，退出状态 dump 缓冲区默认 32 KiB。 */
	SCX_EXIT_DUMP_DFL_LEN		= 32768,

	/* SCX 性能值的“1.0”与调度容量尺度相同，供 cpuperf 与 schedutil 做定点比例运算。 */
	SCX_CPUPERF_ONE			= SCHED_CAPACITY_SCALE,

	/*
	 * Iterating all tasks may take a while. Periodically drop
	 * scx_tasks_lock to avoid causing e.g. CSD and RCU stalls.
	 */
	/*
	 * 原文说明遍历全部 task 可能耗时很长，因此每处理 32 项周期性释放并重新取得
	 * scx_tasks_lock，避免长临界区造成跨 CPU 调用（CSD）和 RCU stall。
	 */
	SCX_TASK_ITER_BATCH		= 32,

	/* bypass dispatch 每两次选择一次 host 路径，在 host 与子调度器救援之间分摊机会。 */
	SCX_BYPASS_HOST_NTH		= 2,

	/* bypass 负载均衡默认间隔为 500ms，量纲由 USEC_PER_MSEC 转为微秒。 */
	SCX_BYPASS_LB_DFL_INTV_US	= 500 * USEC_PER_MSEC,
	/* donor 候选目标数取待平衡目标数的 125%，为过滤无效 donor 留余量。 */
	SCX_BYPASS_LB_DONOR_PCT		= 125,
	/* 同一 donee 的最小重复平衡间隔为总间隔的四分之一。 */
	SCX_BYPASS_LB_MIN_DELTA_DIV	= 4,
	/* bypass 批处理每 256 个迁移点检查/让出，限制一次工作独占时间。 */
	SCX_BYPASS_LB_BATCH		= 256,

	/* 同一本地 DSQ 连续 deferred re-enqueue 最多重复 256 次，超过即按错误处理以防活锁。 */
	SCX_REENQ_LOCAL_MAX_REPEAT	= 256,

	/* 根 scheduler 以下最多允许 4 层 sub-scheduler，装载时达到该深度即拒绝。 */
	SCX_SUB_MAX_DEPTH		= 4,
};

/*
 * Per-cid topology info. For each topology level (core, LLC, node), records
 * the first cid in the unit and its global index. Global indices are
 * consecutive integers assigned in cid-walk order, so e.g. core_idx ranges
 * over [0, nr_cores_at_init) with no gaps. No-topo cids have all fields set
 * to -1.
 *
 * @core_cid: first cid of this cid's core (smt-sibling group)
 * @core_idx: global index of that core, in [0, nr_cores_at_init)
 * @llc_cid: first cid of this cid's LLC
 * @llc_idx: global index of that LLC, in [0, nr_llcs_at_init)
 * @node_cid: first cid of this cid's NUMA node
 * @node_idx: global index of that node, in [0, nr_nodes_at_init)
 */
/*
 * scx_cid_topo 保存每个 cid 在 core、LLC 和 NUMA node 三层拓扑中的信息。
 * 每层记录该单元第一个 cid 以及按 cid 遍历顺序分配的连续全局索引；例如 core_idx 在
 * [0, nr_cores_at_init) 内无空洞。缺少拓扑的 cid 六个字段都为 -1。
 *
 * 该结构是 scx_cid_init() 构建并发布的只读值快照，BPF 通过 scx_bpf_cid_topo() 复制读取；
 * CPU 热插拔通常通过重启 scheduler 重建映射。`*_cid` 用于得到拓扑单元在稠密 cid 空间的
 * 起点，`*_idx` 用于索引按单元分配的外部数组；二者不可混用成 CPU 编号。
 */
struct scx_cid_topo {
	/* 当前 cid 所属 SMT core 的首 cid，以及该 core 的无空洞全局序号。 */
	s32 core_cid;
	s32 core_idx;
	/* 当前 cid 所属最后级缓存共享域的首 cid，以及 LLC 全局序号。 */
	s32 llc_cid;
	s32 llc_idx;
	/* 当前 cid 所属 NUMA node 的首 cid，以及 node 全局序号。 */
	s32 node_cid;
	s32 node_idx;
};

/*
 * cmask: variable-length, base-windowed bitmap over cid space
 * -----------------------------------------------------------
 *
 * A cmask covers the cid range [base, base + nr_cids). bits[] is aligned to the
 * global 64-cid grid: bits[0] spans [base & ~63, (base & ~63) + 64), so the
 * first (base & 63) bits of bits[0] are head padding and the trailing bits of
 * the last active word past base + nr_cids are tail padding. Both stay zero;
 * all mutating helpers preserve that. Words past the last active word are not
 * read by any helper and have no constraint.
 *
 * Grid alignment means two cmasks always address bits[] against the same global
 * 64-cid windows, so cross-cmask word ops (AND, OR, ...) reduce to
 *
 *	dst->bits[i] OP= src->bits[i - delta]
 *
 * with no bit-shifting, regardless of how the two bases relate mod 64.
 */
/*
 * cmask 是覆盖 cid 空间的“可变长度、带 base 窗口”位图。active 范围为
 * [base, base + nr_cids)，bits[0] 对齐到包含 base 的全局 64-cid word；其前
 * (base & 63) 位是头 padding，最后 active cid 之后是尾 padding，所有修改 helper 都保持
 * 二者为 0。最后 active word 之后的已分配 word 不会被 helper 读取，内容无约束。
 *
 * 因所有 cmask 都按同一全局网格解释 word，跨 mask AND/OR 可用目标索引减 word 偏移直接
 * 运算，无论两个 base 对 64 的余数如何都无需位移。结构通常位于栈上 `_DEFINE_FLEX` 存储或
 * arena/per-CPU scratch 中；调用者负责生命周期和并发，racy helper 只在显式容忍竞态时使用。
 */
struct scx_cmask {
	/* active 窗口首 cid；必须与 nr_cids 相加不发生 u32 回绕。 */
	u32 base;
	/* active cid 数，不是 bits 数组的容量。 */
	u32 nr_cids;
	/* bits[] 实际分配的 u64 word 数，是 __counted_by 的边界与 reframe 容量门禁。 */
	u32 alloc_words;
	/* 按全局 64-cid 网格解释的柔性数组；active 之外的头尾 padding 必须保持 0。 */
	u64 bits[] __counted_by(alloc_words);
};

/*
 * Number of u64 words of bits[] storage that covers @nr_cids regardless of base
 * alignment. The +1 absorbs up to 63 bits of head padding when base is not
 * 64-aligned - always allocating one extra word beats branching on base or
 * splitting the compute. The u64 cast keeps the +63 from wrapping when @nr_cids
 * is near U32_MAX, so callers bounds-checking the result against @alloc_words
 * catch the overflow instead of seeing a small value.
 */
/*
 * 计算覆盖 @nr_cids 所需的 u64 word 数时无条件多分配一个 word，用它吸收 base
 * 非 64 对齐时最多 63 位头 padding，比按 base 分支或拆分公式更简单。先转 u64 再加 63，
 * 防止 @nr_cids 接近 U32_MAX 时加法回绕成小值；调用者随后用 alloc_words 做容量检查。
 * 返回值是存储上界而非当前 active word 精确数，@nr_cids 本身不决定 base 对齐。
 */
#define SCX_CMASK_NR_WORDS(nr_cids)	((u32)(((u64)(nr_cids) + 63) / 64 + 1))

/**
 * __SCX_CMASK_DEFINE - Define an on-stack cmask with explicit storage capacity
 * @NAME: variable name to define
 * @BASE: first cid of the active range
 * @NR_CIDS: active range length
 * @ALLOC_CIDS: storage capacity in cids, at least @NR_CIDS
 *
 * @NAME aliases zero-initialized storage with the active range set to
 * [BASE, BASE + NR_CIDS). Use scx_cmask_reframe() to reshape later, up to
 * @ALLOC_CIDS.
 */
/*
 * __SCX_CMASK_DEFINE() 在栈上定义具有显式容量的 cmask。@NAME 是生成的变量名；
 * @BASE/@NR_CIDS 设置 active 区间；@ALLOC_CIDS 是不少于 active 长度的容量。@NAME 指向
 * 零初始化 `_DEFINE_FLEX` 存储，之后可由 scx_cmask_reframe() 在容量内改变窗口。
 *
 * 宏会多次用于编译期/初始化表达式求值，实参应无副作用；它不运行动态边界检查，调用者必须
 * 保证 ALLOC_CIDS >= NR_CIDS 及区间不回绕。栈存储离开作用域即失效，不得被异步保存。
 */
#define __SCX_CMASK_DEFINE(NAME, BASE, NR_CIDS, ALLOC_CIDS)			\
	_DEFINE_FLEX(struct scx_cmask, NAME, bits, SCX_CMASK_NR_WORDS(ALLOC_CIDS), \
		     = { .base = (BASE),					\
			 .nr_cids = (NR_CIDS),					\
			 .alloc_words = SCX_CMASK_NR_WORDS(ALLOC_CIDS) })

/**
 * SCX_CMASK_DEFINE - Define an on-stack cmask on tight storage
 * @NAME: variable name to define
 * @BASE: first cid of the active range
 * @NR_CIDS: active range length, also storage capacity
 *
 * @NAME aliases zero-initialized storage with the active range and storage
 * both [BASE, BASE + NR_CIDS).
 */
/*
 * SCX_CMASK_DEFINE() 用紧凑容量定义栈上 cmask；@NAME 为变量名，@BASE 为首 cid，
 * @NR_CIDS 同时是 active 长度和容量。它转发到通用宏，仍会按最坏 base 对齐多留一个 word；
 * 后续不能 reframe 成更长区间，ownership 与作用域规则同上。
 */
#define SCX_CMASK_DEFINE(NAME, BASE, NR_CIDS)					\
	__SCX_CMASK_DEFINE(NAME, BASE, NR_CIDS, NR_CIDS)

/**
 * SCX_CMASK_DEFINE_SHARD - Define an on-stack cmask sized to one shard
 * @NAME: variable name to define
 * @BASE: first cid of the active range
 * @NR_CIDS: active range length, must be <= SCX_CID_SHARD_MAX_CPUS
 *
 * Storage is fixed at SCX_CID_SHARD_MAX_CPUS, active range framed by
 * (BASE, NR_CIDS). Passing NR_CIDS > SCX_CID_SHARD_MAX_CPUS leaves the
 * cmask claiming more bits than storage holds and subsequent cmask
 * operations will overrun.
 */
/*
 * SCX_CMASK_DEFINE_SHARD() 定义容量固定为 SCX_CID_SHARD_MAX_CPUS 的栈上 cmask，
 * active 窗口由 @BASE/@NR_CIDS 指定，且 @NR_CIDS 必须不超过 shard 上限。若违反，结构会
 * 声称拥有超出真实 bits[] 的 active 位，后续 helper 将越界；宏本身不检查也不截断。
 * 该形式用于在不同实际 shard 长度间复用固定最大栈布局。
 */
#define SCX_CMASK_DEFINE_SHARD(NAME, BASE, NR_CIDS)				\
	__SCX_CMASK_DEFINE(NAME, BASE, NR_CIDS, SCX_CID_SHARD_MAX_CPUS)

/* 结束早期 SCX 类型定义的重复包含保护。 */
#endif /* _KERNEL_SCHED_EXT_TYPES_H */
