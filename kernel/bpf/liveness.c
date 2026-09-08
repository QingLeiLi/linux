// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Meta Platforms, Inc. and affiliates. */

#include <linux/bpf_verifier.h>
#include <linux/btf.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/sort.h>

#define verbose(env, fmt, args...) bpf_verifier_log_write(env, fmt, ##args)

/*
 * 学习导览：本文件先用前向数据流追踪“寄存器来自哪一层 FP、哪个偏移”，据此
 * 把 helper/kfunc/访存转换为各调用帧的栈 use/def；再沿 CFG 后序反向迭代，求出
 * 每条指令之前可能存活的栈半槽与寄存器。states.c 据此清掉永不再读的状态，既
 * 缩小剪枝比较面，又必须对未知实例、未知偏移和多调用目标采取保守全活策略。
 * env 及哈希表由单个验证线程独占；分析对象使用 GFP_KERNEL_ACCOUNT，可睡眠，
 * 任意分配失败都会返回 errno 并由顶层释放已建立的临时或持久对象。
 */

/* 每条指令、每个调用帧的半栈槽传递量；三个位集均以 4 字节 half-SPI 编号。 */
struct per_frame_masks {
	spis_t may_read;	/* stack slots that may be read by this instruction */
	/* 中文：may_read 是当前指令可能读取的半槽集合，即活性传递的 gen。 */
	spis_t must_write;	/* stack slots written by this instruction */
	/* 中文：must_write 是所有本轮路径都完整写入的半槽集合，即安全 kill。 */
	spis_t live_before;	/* stack slots that may be read by this insn and its successors */
	/* 中文：live_before 是当前指令或任一后继可能读取、且尚未被必写覆盖的集合。 */
};

/*
 * A function instance keyed by (callsite, depth).
 * Encapsulates read and write marks for each instruction in the function.
 * Marks are tracked for each frame up to @depth.
 */
/*
 * 中文：函数实例以（调用点，深度）为键，封装该函数各指令的读写标记，并追踪
 * 0..depth 的每一祖先帧。对象由 call_instance() 创建并加入 liveness 哈希表，
 * 验证结束统一销毁；frames 懒分配，验证线程独占，因而不需要锁或 RCU。
 */
struct func_instance {
	struct hlist_node hl_node;
	u32 callsite;		/* call insn that invoked this subprog (subprog_start for depth 0) */
	/* 中文：callsite 是调用本子程序的指令；深度 0 时改用子程序起点作稳定键。 */
	u32 depth;		/* call depth (0 = entry subprog) */
	/* 中文：depth 是绝对调用深度，入口子程序为 0。 */
	u32 subprog;		/* subprog index */
	/* 中文：subprog 是 env->subprog_info 中的子程序编号。 */
	u32 subprog_start;	/* cached env->subprog_info[subprog].start */
	/* 中文：subprog_start 缓存绝对首指令号，供局部下标换算。 */
	u32 insn_cnt;		/* cached number of insns in the function */
	/* 中文：insn_cnt 是该子程序指令槽数，也是每个 frames 数组长度。 */
	/* Per frame, per instruction masks, frames allocated lazily. */
	/* 中文：每帧指向 insn_cnt 个掩码；NULL 表示该帧尚未出现访问，数组按需分配。 */
	struct per_frame_masks *frames[MAX_CALL_FRAMES];
	bool must_write_initialized;
};

/* 一次 states.c 查询的缓存：借用 instance，不延长其由 liveness 拥有的生命周期。 */
struct live_stack_query {
	struct func_instance *instances[MAX_CALL_FRAMES]; /* valid in range [0..curframe] */
	/* 中文：instances 仅在 0..curframe 有效，均为哈希表对象的临时借用。 */
	u32 callsites[MAX_CALL_FRAMES]; /* callsite[i] = insn calling frame i+1 */
	/* 中文：callsites[i] 是实际状态中调用第 i+1 帧的绝对指令号。 */
	u32 curframe;
	/* 当前 verifier 状态最内层帧号，也是 instances/callsites 查询上界。 */
	u32 insn_idx;
	/* 当前状态对应的绝对指令号，首先在最内层实例的执行前位置查询。 */
};

/* 验证期总容器：持有实例哈希表和查询缓存，并以 subprog_calls 实施复杂度上限。 */
struct bpf_liveness {
	DECLARE_HASHTABLE(func_instances, 8);		/* maps (depth, callsite) to func_instance */
	/* 中文：func_instances 把（depth, callsite）映射到唯一持久实例。 */
	struct live_stack_query live_stack_query;	/* cache to avoid repetitive ht lookups */
	/* 中文：live_stack_query 缓存一次 states.c 查询，避免逐槽重复查哈希表。 */
	u32 subprog_calls;				/* analyze_subprog() invocations */
	/* 中文：递归分析调用累计数，超过上限即以 -E2BIG 拒绝复杂程序。 */
};

/*
 * Hash/compare key for func_instance: (depth, callsite).
 * For depth == 0 (entry subprog), @callsite is the subprog start insn.
 * For depth > 0, @callsite is the call instruction index that invoked the subprog.
 */
/*
 * 中文：入口实例以“深度 0 + 子程序首指令”为键，其他实例以“深度 + 调用点”为键。
 * 业务背景：同一子程序从不同调用点/深度进入时，祖先栈可见性不同，不能共用结果。
 * 入参：callsite 是上述规范键，depth 是调用深度。
 * 出参/返回：返回稳定的 32 位哈希值；不修改状态。
 * 注意事项：哈希碰撞由 find_instance() 的字段比较消解；不分配、不睡眠。
 */
static u32 instance_hash(u32 callsite, u32 depth)
{
	u32 key[2] = { depth, callsite };

	return jhash2(key, 2, 0);
}

/*
 * 业务背景：在调用实例哈希表中复用已建立的 use/def 与固定点结果。
 * 入参：env 为借用环境；callsite/depth 组成规范键。
 * 出参/返回：命中返回 liveness 所有的借用指针，未命中返回 NULL；无副作用。
 * 注意事项：返回值仅在 bpf_stack_liveness_free() 前有效；验证线程独占哈希表。
 */
static struct func_instance *find_instance(struct bpf_verifier_env *env,
					   u32 callsite, u32 depth)
{
	struct bpf_liveness *liveness = env->liveness;
	struct func_instance *f;
	u32 key = instance_hash(callsite, depth);

	/* 哈希只缩小桶范围；必须再比较完整键来消解碰撞。 */
	hash_for_each_possible(liveness->func_instances, f, hl_node, key)
		if (f->depth == depth && f->callsite == callsite)
			return f;
	return NULL;
}

/*
 * 业务背景：为顶层或具体调用点取得分析实例，并缓存后续数据流结果。
 * 入参：env 为输入输出所有者；caller 可空；callsite 是调用指令；subprog 是合法编号。
 * 出参/返回：返回已有/新实例的借用指针，分配失败返回 ERR_PTR(-ENOMEM)。
 * 注意事项：新对象在成功 hash_add 后归 env->liveness；GFP_KERNEL_ACCOUNT 可睡眠。
 */
static struct func_instance *call_instance(struct bpf_verifier_env *env,
					   struct func_instance *caller,
					   u32 callsite, int subprog)
{
	u32 depth = caller ? caller->depth + 1 : 0;
	u32 subprog_start = env->subprog_info[subprog].start;
	u32 lookup_key = depth > 0 ? callsite : subprog_start;
	struct func_instance *f;
	u32 hash;

	/* 已发布实例是快速路径；返回借用，不增加引用。 */
	f = find_instance(env, lookup_key, depth);
	if (f)
		return f;

	/* 未命中时零分配并填完身份/范围，最后插入哈希表完成 ownership 发布。 */
	f = kvzalloc(sizeof(*f), GFP_KERNEL_ACCOUNT);
	if (!f)
		return ERR_PTR(-ENOMEM);
	f->callsite = lookup_key;
	f->depth = depth;
	f->subprog = subprog;
	f->subprog_start = subprog_start;
	f->insn_cnt = (env->subprog_info + subprog + 1)->start - subprog_start;
	/* 哈希节点最后插入；此后释放责任从本地转交 liveness 容器。 */
	hash = instance_hash(lookup_key, depth);
	hash_add(env->liveness->func_instances, &f->hl_node, hash);
	return f;
}

/*
 * 业务背景：状态清洗阶段需把实际 verifier 帧映射回此前建立的最具体分析实例。
 * 入参：env/st 为借用只读对象；frameno 是 0..curframe 的实际帧号。
 * 出参/返回：从该深度向入口退化查找，命中返回借用实例，否则 NULL。
 * 注意事项：退化允许缺少深层实例时使用更保守入口结果；无分配、不睡眠。
 */
static struct func_instance *lookup_instance(struct bpf_verifier_env *env,
					     struct bpf_verifier_state *st,
					     u32 frameno)
{
	u32 callsite, subprog_start;
	struct func_instance *f;
	u32 key, depth;

	subprog_start = env->subprog_info[st->frame[frameno]->subprogno].start;
	callsite = frameno > 0 ? st->frame[frameno]->callsite : subprog_start;

	/* 从实际帧深度逐层降级；深层实例缺失时尝试同调用点的较浅过近似实例。 */
	for (depth = frameno; ; depth--) {
		key = depth > 0 ? callsite : subprog_start;
		f = find_instance(env, key, depth);
		if (f || depth == 0)
			return f;
	}
}

/*
 * 业务背景：CFG 完成后为栈活性分析建立验证期私有上下文。
 * 入参：env 为输入输出验证环境，liveness 尚未初始化。
 * 出参/返回：成功返回 0 并发布空哈希表；分配失败返回 -ENOMEM。
 * 注意事项：成功 ownership 交给 env，必须由 bpf_stack_liveness_free() 对称释放。
 */
int bpf_stack_liveness_init(struct bpf_verifier_env *env)
{
	/* 零分配后先初始化桶头，再把完整容器留在 env 供后续阶段持有。 */
	env->liveness = kvzalloc_obj(*env->liveness, GFP_KERNEL_ACCOUNT);
	if (!env->liveness)
		return -ENOMEM;
	hash_init(env->liveness->func_instances);
	return 0;
}

/*
 * 业务背景：验证结束或失败时回收全部调用实例、逐帧掩码和 liveness 容器。
 * 入参：env 为输入输出所有者，允许 liveness 为 NULL。
 * 出参/返回：无；释放所有已发布对象，调用后旧 instance/query 指针均失效。
 * 注意事项：只在验证线程无并发访问时调用；kvfree 可处理 kmalloc/vmalloc 后端。
 */
void bpf_stack_liveness_free(struct bpf_verifier_env *env)
{
	struct func_instance *instance;
	struct hlist_node *tmp;
	int bkt, i;

	/* 未初始化或早期失败无需回收；否则逐桶安全遍历全部持久实例。 */
	if (!env->liveness)
		return;
	hash_for_each_safe(env->liveness->func_instances, bkt, tmp, instance, hl_node) {
		for (i = 0; i <= instance->depth; i++)
			kvfree(instance->frames[i]);
		kvfree(instance);
	}
	/* 帧数组先于实例释放；所有实例结束后再释放顶层容器。 */
	kvfree(env->liveness);
}

/*
 * Convert absolute instruction index @insn_idx to an index relative
 * to start of the function corresponding to @instance.
 */
/*
 * 中文：把绝对指令号减去实例缓存的子程序起点，得到 frames 数组下标。
 * 业务背景：逐实例掩码按子程序局部指令号紧凑存储。
 * 入参：instance 为借用实例；insn_idx 必须属于该子程序。
 * 出参/返回：返回 0..insn_cnt-1 的相对下标；无副作用。
 * 注意事项：不做边界检查，调用者必须先保证指令归属。
 */
static int relative_idx(struct func_instance *instance, u32 insn_idx)
{
	return insn_idx - instance->subprog_start;
}

/*
 * 业务背景：按实例、祖先帧和指令取得已分配的 use/def/live 三元组。
 * 入参：instance 为借用；frame 不超过 depth；insn_idx 属于本子程序。
 * 出参/返回：该帧尚未使用返回 NULL，否则返回实例所有的借用元素指针。
 * 注意事项：不分配且不延长数组生命周期；调用者不得越界。
 */
static struct per_frame_masks *get_frame_masks(struct func_instance *instance,
					       u32 frame, u32 insn_idx)
{
	if (!instance->frames[frame])
		return NULL;

	return &instance->frames[frame][relative_idx(instance, insn_idx)];
}

/*
 * 业务背景：首次记录某祖先帧访问时，懒分配覆盖本子程序全部指令的掩码数组。
 * 入参：instance 为输入输出对象；frame/insn_idx 遵循 get_frame_masks() 约束。
 * 出参/返回：成功返回目标元素；分配失败返回 ERR_PTR(-ENOMEM)。
 * 注意事项：成功数组归 instance；先发布 arr 后判空仍安全，NULL 表示未分配。
 */
static struct per_frame_masks *alloc_frame_masks(struct func_instance *instance,
						 u32 frame, u32 insn_idx)
{
	struct per_frame_masks *arr;

	/* 首次触及该帧时创建全零数组；零值恰好代表无 use/def/live。 */
	if (!instance->frames[frame]) {
		arr = kvzalloc_objs(*arr, instance->insn_cnt,
				    GFP_KERNEL_ACCOUNT);
		instance->frames[frame] = arr;
		if (!arr)
			return ERR_PTR(-ENOMEM);
	}
	return get_frame_masks(instance, frame, insn_idx);
}

/* Accumulate may_read masks for @frame at @insn_idx */
/*
 * 中文：把 mask 并入指定指令的 may_read，重复记录幂等。
 * 业务背景：不同参数/偏移可能汇聚到同一访问点，读取集合取并集。
 * 入参：instance 为输入输出；frame/insn_idx 定位元素；mask 是 half-SPI 位集。
 * 出参/返回：成功返回 0；懒分配失败返回 -ENOMEM。
 * 注意事项：成功后只增不减 may_read；可能睡眠，数组归 instance。
 */
static int mark_stack_read(struct func_instance *instance, u32 frame, u32 insn_idx, spis_t mask)
{
	struct per_frame_masks *masks;

	/* ERR_PTR 让指针返回同时携带 errno；成功才扩大 may_read。 */
	masks = alloc_frame_masks(instance, frame, insn_idx);
	if (IS_ERR(masks))
		return PTR_ERR(masks);
	masks->may_read = spis_or(masks->may_read, mask);
	return 0;
}

/*
 * 业务背景：记录该路径上指令必定完整覆盖的 half-SPI，供反向活性做 kill 集。
 * 入参：instance/frame/insn_idx 定位元素；mask 是本次完整写集合。
 * 出参/返回：成功返回 0 并取并集；懒分配失败返回 -ENOMEM。
 * 注意事项：这里只聚合同一次实例分析，跨重复分析的 must_write 后续取交集。
 */
static int mark_stack_write(struct func_instance *instance, u32 frame, u32 insn_idx, spis_t mask)
{
	struct per_frame_masks *masks;

	/* 与读标记对称，分配成功后把本次完整写集合并入累计 def。 */
	masks = alloc_frame_masks(instance, frame, insn_idx);
	if (IS_ERR(masks))
		return PTR_ERR(masks);
	masks->must_write = spis_or(masks->must_write, mask);
	return 0;
}

/*
 * 业务背景：后继计算需统一读取经典 off16 与 JMP32 JA 使用的 imm32 位移。
 * 入参：insn 为借用且已通过基本指令格式检查。
 * 出参/返回：JMP32|JA 返回 imm，其余跳转返回 off；无副作用。
 * 注意事项：返回单位是 BPF 指令槽，目标还需加当前 idx 和 1。
 */
int bpf_jmp_offset(struct bpf_insn *insn)
{
	u8 code = insn->code;

	if (code == (BPF_JMP32 | BPF_JA))
		return insn->imm;
	return insn->off;
}

__diag_push();
__diag_ignore_all("-Woverride-init", "Allow field initialization overrides for opcode_info_tbl");

/*
 * Returns an array of instructions succ, with succ->items[0], ...,
 * succ->items[n-1] with successor instructions, where n=succ->cnt
 */
/*
 * 中文：返回 succ 数组，items[0..cnt-1] 是 idx 的直接 CFG 后继。
 * 业务背景：栈/寄存器固定点与 cfg.c 必须共享完全一致的控制流边。
 * 入参：env 为借用环境；idx 是已验证的指令下标。
 * 出参/返回：gotox 有持久 jt 时返回其借用指针，否则返回 env->succ 临时缓冲。
 * 注意事项：临时缓冲每次调用都会重置，嵌套/后续调用会覆盖；不分配、不睡眠。
 */
inline struct bpf_iarray *
bpf_insn_successors(struct bpf_verifier_env *env, u32 idx)
{
	/* 表项按“可跳转/可顺落”刻画控制流；默认普通指令只顺落。 */
	static const struct opcode_info {
		bool can_jump;
		bool can_fallthrough;
	} opcode_info_tbl[256] = {
		[0 ... 255] = {.can_jump = false, .can_fallthrough = true},
	#define _J(code, ...) \
		[BPF_JMP   | code] = __VA_ARGS__, \
		[BPF_JMP32 | code] = __VA_ARGS__

		/* EXIT 无后继，JA 仅跳转；以下比较跳转均还有顺落边。 */
		_J(BPF_EXIT,  {.can_jump = false, .can_fallthrough = false}),
		_J(BPF_JA,    {.can_jump = true,  .can_fallthrough = false}),
		_J(BPF_JEQ,   {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JNE,   {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JLT,   {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JLE,   {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JGT,   {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JGE,   {.can_jump = true,  .can_fallthrough = true}),
		/* 所有有条件比较（含有/无符号）都同时保留跳转和顺落两条边。 */
		_J(BPF_JSGT,  {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JSGE,  {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JSLT,  {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JSLE,  {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JCOND, {.can_jump = true,  .can_fallthrough = true}),
		_J(BPF_JSET,  {.can_jump = true,  .can_fallthrough = true}),
	#undef _J
	};
	/* 表后局部量分别借用程序/指令/表项/返回数组，并记录指令槽宽。 */
	struct bpf_prog *prog = env->prog;
	struct bpf_insn *insn = &prog->insnsi[idx];
	const struct opcode_info *opcode_info;
	struct bpf_iarray *succ, *jt;
	int insn_sz;

	/* gotox 的多后继表已由 cfg.c 建立，直接返回其持久借用。 */
	jt = env->insn_aux_data[idx].jt;
	if (unlikely(jt))
		return jt;

	/* pre-allocated array of size up to 2; reset cnt, as it may have been used already */
	/* 中文：普通指令最多跳转/顺落两条边，复用前先清空预分配缓冲的有效计数。 */
	succ = env->succ;
	succ->cnt = 0;

	/* ldimm64 占两槽；其他指令顺落一槽，跳转目标则统一用位移 helper 计算。 */
	opcode_info = &opcode_info_tbl[BPF_CLASS(insn->code) | BPF_OP(insn->code)];
	insn_sz = bpf_is_ldimm64(insn) ? 2 : 1;
	if (opcode_info->can_fallthrough)
		succ->items[succ->cnt++] = idx + insn_sz;

	if (opcode_info->can_jump)
		succ->items[succ->cnt++] = idx + bpf_jmp_offset(insn) + 1;

	return succ;
}

__diag_pop();


/*
 * 业务背景：对一个“实例帧 × 指令”应用标准反向活性传递方程。
 * 入参：env/instance 为借用；frame 定位祖先帧；insn_idx 属于实例子程序。
 * 出参/返回：live_before 发生变化返回 true，否则 false；原地更新目标掩码。
 * 注意事项：调用前该可达指令及后继掩码必须已分配；后序遍历保证快速收敛。
 */
static inline bool update_insn(struct bpf_verifier_env *env,
			       struct func_instance *instance, u32 frame, u32 insn_idx)
{
	spis_t new_before, new_after;
	struct per_frame_masks *insn, *succ_insn;
	struct bpf_iarray *succ;
	u32 s;
	bool changed;

	/* 无后继的 EXIT 不需传播；其他点从全部后继入口聚合执行后活性。 */
	succ = bpf_insn_successors(env, insn_idx);
	if (succ->cnt == 0)
		return false;

	changed = false;
	insn = get_frame_masks(instance, frame, insn_idx);
	new_before = SPIS_ZERO;
	new_after = SPIS_ZERO;
	/* CFG 构建保证普通后继在子程序范围内且对应帧数组已分配。 */
	for (s = 0; s < succ->cnt; ++s) {
		succ_insn = get_frame_masks(instance, frame, succ->items[s]);
		new_after = spis_or(new_after, succ_insn->live_before);
	}
	/*
	 * New "live_before" is a union of all "live_before" of successors
	 * minus slots written by instruction plus slots read by instruction.
	 * new_before = (new_after & ~insn->must_write) | insn->may_read
	 */
	/* 中文：后继 live 取并集，减去本指令必写集合，再加入本指令可能读取集合。 */
	new_before = spis_or(spis_and(new_after, spis_not(insn->must_write)),
			     insn->may_read);
	changed |= !spis_equal(new_before, insn->live_before);
	insn->live_before = new_before;
	return changed;
}

/* Fixed-point computation of @live_before marks */
/*
 * 中文：在本实例全部已使用帧上按 CFG 后序反复更新，直到 live_before 不再增长。
 * 业务背景：循环会把后继读取需求回传到回边之前，单轮扫描不足。
 * 入参：env 提供后序数组；instance 为输入输出分析实例。
 * 出参/返回：无；设置 must_write_initialized 并完成全部 live_before 固定点。
 * 注意事项：有限位集单调增长必收敛；不分配、不睡眠。
 */
static void update_instance(struct bpf_verifier_env *env, struct func_instance *instance)
{
	u32 i, frame, po_start, po_end;
	int *insn_postorder = env->cfg.insn_postorder;
	struct bpf_subprog_info *subprog;
	bool changed;

	/* 标记 must_write 已有首轮基线，后续重复分析必须采用交集合并。 */
	instance->must_write_initialized = true;
	subprog = &env->subprog_info[instance->subprog];
	po_start = subprog->postorder_start;
	po_end = (subprog + 1)->postorder_start;
	/* repeat until fixed point is reached */
	/* 中文：循环直到一整轮没有任何位新增。 */
	/* 每轮依次扫描每个已触及祖先帧的子程序后序切片。 */
	do {
		changed = false;
		for (frame = 0; frame <= instance->depth; frame++) {
			if (!instance->frames[frame])
				continue;

			for (i = po_start; i < po_end; i++)
				changed |= update_insn(env, instance, frame, insn_postorder[i]);
		}
	} while (changed);
}

/*
 * 业务背景：查询具体实例中某半栈槽在某指令前是否可能被后续读取。
 * 入参：instance 为借用；insn_idx/frameno 定位掩码；half_spi 是 4 字节槽编号。
 * 出参/返回：掩码存在且对应位为 1 返回 true，否则 false；无副作用。
 * 注意事项：缺少帧数组在实例内表示未见访问，不等同顶层查询的保守未知。
 */
static bool is_live_before(struct func_instance *instance, u32 insn_idx, u32 frameno, u32 half_spi)
{
	struct per_frame_masks *masks;

	masks = get_frame_masks(instance, frameno, insn_idx);
	return masks && spis_test_bit(masks->live_before, half_spi);
}

/*
 * 业务背景：states.c 清洗一个验证状态前，缓存每个实际帧对应的分析实例与调用点。
 * 入参：env 为输入输出环境；st 为借用状态，帧链在后续查询期间保持稳定。
 * 出参/返回：当前实现固定返回 0；重置并填充 env->liveness->live_stack_query。
 * 注意事项：instance 只借用；查询缓存会被下一次初始化覆盖，不可跨验证状态保存。
 */
int bpf_live_stack_query_init(struct bpf_verifier_env *env, struct bpf_verifier_state *st)
{
	struct live_stack_query *q = &env->liveness->live_stack_query;
	struct func_instance *instance;
	u32 frame;

	/* 缓存严格绑定本次 verifier state；先清零防止较短帧链遗留旧借用。 */
	memset(q, 0, sizeof(*q));
	for (frame = 0; frame <= st->curframe; frame++) {
		instance = lookup_instance(env, st, frame);
		if (IS_ERR_OR_NULL(instance))
			q->instances[frame] = NULL;
		else
			q->instances[frame] = instance;
		if (frame < st->curframe)
			q->callsites[frame] = st->frame[frame + 1]->callsite;
	}
	/* 最后发布查询边界和当前指令，随后 states.c 才可逐槽调用 alive。 */
	q->curframe = st->curframe;
	q->insn_idx = st->insn_idx;
	return 0;
}

/*
 * 业务背景：判断状态帧槽能否安全清除；需同时考虑当前函数及所有向内调用的路径。
 * 入参：env 含已初始化 query；frameno 是实际祖先帧；half_spi 是 4 字节槽编号。
 * 出参/返回：任一相关点可能读取或实例映射未知返回 true；证明均不读才返回 false。
 * 注意事项：缺失/越界映射一律保守判活；callback 在调用前、普通调用在返回后查询。
 */
bool bpf_stack_slot_alive(struct bpf_verifier_env *env, u32 frameno, u32 half_spi)
{
	/*
	 * Slot is alive if it is read before q->insn_idx in current func instance,
	 * or if for some outer func instance:
	 * - alive before callsite if callsite calls callback, otherwise
	 * - alive after callsite
	 */
	/* 中文：当前实例查当前指令前；逐层外推时 callback 查调用前，普通子程序查返回点。 */
	struct live_stack_query *q = &env->liveness->live_stack_query;
	struct func_instance *instance, *curframe_instance;
	u32 i, callsite, rel;
	int cur_delta, delta;
	bool alive = false;

	/* 先检查最内层当前指令；无法映射实例时宁可保留槽。 */
	curframe_instance = q->instances[q->curframe];
	if (!curframe_instance)
		return true;
	cur_delta = (int)curframe_instance->depth - (int)q->curframe;
	rel = frameno + cur_delta;
	if (rel <= curframe_instance->depth)
		alive = is_live_before(curframe_instance, q->insn_idx, rel, half_spi);

	if (alive)
		return true;

	/* 再沿实际帧链向内跨越每个调用点，任一层可能读即判活。 */
	for (i = frameno; i < q->curframe; i++) {
		instance = q->instances[i];
		if (!instance)
			return true;
		/* Map actual frameno to frame index within this instance */
		/* 中文：用实例深度与实际帧号之差，把祖先帧换算成该实例的内部帧索引。 */
		delta = (int)instance->depth - (int)i;
		rel = frameno + delta;
		if (rel > instance->depth)
			return true;

		/* Get callsite from verifier state, not from instance callchain */
		/* 中文：调用点取自本次 verifier 状态，避免误用可能已退化匹配的实例调用链。 */
		callsite = q->callsites[i];

		alive = bpf_calls_callback(env, callsite)
			? is_live_before(instance, callsite, rel, half_spi)
			: is_live_before(instance, callsite + 1, rel, half_spi);
		if (alive)
			return true;
	}

	/* 所有相关实例和边界均已证明不读，调用者才可清除该半槽状态。 */
	return false;
}

/*
 * 业务背景：二级日志需把子程序编号和可选 BTF 名称组合成短标签。
 * 入参：env 提供临时缓冲；subprog 是合法索引。
 * 出参/返回：返回 env->tmp_str_buf 借用指针，内容为 subprog 标签。
 * 注意事项：下一次格式化会覆盖；仅当前验证线程使用，不分配。
 */
static char *fmt_subprog(struct bpf_verifier_env *env, int subprog)
{
	const char *name = env->subprog_info[subprog].name;

	snprintf(env->tmp_str_buf, sizeof(env->tmp_str_buf),
		 "subprog#%d%s%s", subprog, name ? " " : "", name ? name : "");
	return env->tmp_str_buf;
}

/*
 * 业务背景：日志用深度和调用点唯一标识同一子程序的不同分析实例。
 * 入参：env 提供临时缓冲；instance 为借用。
 * 出参/返回：返回形如 (dN,csM) 的临时字符串借用指针。
 * 注意事项：与 fmt_subprog()/fmt_spis_mask() 共用缓冲，调用后应立即消费。
 */
static char *fmt_instance(struct bpf_verifier_env *env, struct func_instance *instance)
{
	snprintf(env->tmp_str_buf, sizeof(env->tmp_str_buf),
		 "(d%d,cs%d)", instance->depth, instance->callsite);
	return env->tmp_str_buf;
}

/*
 * 业务背景：诊断输出把从 0 开始的 8 字节 SPI 转换为负 FP 字节偏移。
 * 入参：spi 是非负完整槽编号。
 * 出参/返回：返回 -(spi+1)*8；无副作用。
 * 注意事项：只用于受 STACK_SLOTS 限制的日志路径，不检查算术溢出。
 */
static int spi_off(int spi)
{
	return -(spi + 1) * BPF_REG_SIZE;
}

/*
 * When both halves of an 8-byte SPI are set, print as "-8","-16",...
 * When only one half is set, print as "-4h","-8h",...
 * Runs of 3+ consecutive fully-set SPIs are collapsed: "fp0-8..-24"
 */
/*
 * 中文：整槽打印 -8/-16，半槽加 h，连续至少三个整槽折叠为范围。
 * 业务背景：压缩 use/def 位集，令二级日志仍可人工核对固定点。
 * 入参：env 提供缓冲；frame 是帧号；first 控制前导空格；spis 是 half-SPI 位集。
 * 出参/返回：返回临时缓冲借用指针；内容可能因容量截断。
 * 注意事项：与其他 fmt 函数共用 tmp_str_buf，返回值必须立即消费。
 */
static char *fmt_spis_mask(struct bpf_verifier_env *env, int frame, bool first, spis_t spis)
{
	int buf_sz = sizeof(env->tmp_str_buf);
	char *buf = env->tmp_str_buf;
	int spi, n, run_start;

	/* 每次格式化覆盖共享缓冲；buf/buf_sz 随已写字符向前推进。 */
	buf[0] = '\0';

	/* 逐完整 SPI 读取低/高两个 half-SPI，空槽不产生文本。 */
	for (spi = 0; spi < STACK_SLOTS / 2 && buf_sz > 0; spi++) {
		bool lo = spis_test_bit(spis, spi * 2);
		bool hi = spis_test_bit(spis, spi * 2 + 1);
		const char *space = first ? "" : " ";

		if (!lo && !hi)
			continue;

		if (!lo || !hi) {
			/* half-spi */
			/* 中文：只有一个 4 字节半槽置位，用 h 标明不是完整 8 字节 SPI。 */
			n = scnprintf(buf, buf_sz, "%sfp%d%d%s",
				      space, frame, spi_off(spi) + (lo ? STACK_SLOT_SZ : 0), "h");
		} else if (spi + 2 < STACK_SLOTS / 2 &&
			   spis_test_bit(spis, spi * 2 + 2) &&
			   spis_test_bit(spis, spi * 2 + 3) &&
			   spis_test_bit(spis, spi * 2 + 4) &&
			   spis_test_bit(spis, spi * 2 + 5)) {
			/* 3+ consecutive full spis */
			/* 中文：探测连续整槽的最长区间并一次输出起止偏移。 */
			run_start = spi;
			while (spi + 1 < STACK_SLOTS / 2 &&
			       spis_test_bit(spis, (spi + 1) * 2) &&
			       spis_test_bit(spis, (spi + 1) * 2 + 1))
				spi++;
			n = scnprintf(buf, buf_sz, "%sfp%d%d..%d",
				      space, frame, spi_off(run_start), spi_off(spi));
		} else {
			/* just a full spi */
			/* 中文：不足三个连续项时逐个打印完整 SPI。 */
			n = scnprintf(buf, buf_sz, "%sfp%d%d", space, frame, spi_off(spi));
		}
		first = false;
		buf += n;
		buf_sz -= n;
	}
	return env->tmp_str_buf;
}

/*
 * 业务背景：二级日志逐指令展示一个调用实例的栈 use/def，辅助验证数据流结果。
 * 入参：env 为日志输出环境；instance 为借用且固定点已完成。
 * 出参/返回：无；日志级别不足时无副作用，否则追加诊断文本。
 * 注意事项：格式化共享临时缓冲；ldimm64 第二槽随首槽一起跳过。
 */
static void print_instance(struct bpf_verifier_env *env, struct func_instance *instance)
{
	int start = env->subprog_info[instance->subprog].start;
	struct bpf_insn *insns = env->prog->insnsi;
	struct per_frame_masks *masks;
	int len = instance->insn_cnt;
	int insn_idx, frame, i;
	bool has_use, has_def;
	u64 pos, insn_pos;

	/* 非二级日志快速返回；否则每条指令先暂存日志位置以支持撤回空字段。 */
	if (!(env->log.level & BPF_LOG_LEVEL2))
		return;

	verbose(env, "stack use/def %s ", fmt_subprog(env, instance->subprog));
	verbose(env, "%s:\n", fmt_instance(env, instance));
	for (i = 0; i < len; i++) {
		insn_idx = start + i;
		has_use = false;
		has_def = false;
		pos = env->log.end_pos;
		/* 暂存反汇编起点；后续没有 use/def 时可精确撤回装饰。 */
		verbose(env, "%3d: ", insn_idx);
		bpf_verbose_insn(env, &insns[insn_idx]);
		bpf_vlog_reset(&env->log, env->log.end_pos - 1); /* remove \n */
		/* 中文：撤掉指令打印自带换行，使 use/def 与反汇编保持同一行。 */
		insn_pos = env->log.end_pos;
		verbose(env, "%*c;", bpf_vlog_alignment(insn_pos - pos), ' ');
		pos = env->log.end_pos;
		/* 从最内帧向祖先帧打印非空 use；全空则把字段回滚到分号前。 */
		verbose(env, " use: ");
		for (frame = instance->depth; frame >= 0; --frame) {
			masks = get_frame_masks(instance, frame, insn_idx);
			if (!masks || spis_is_zero(masks->may_read))
				continue;
			verbose(env, "%s", fmt_spis_mask(env, frame, !has_use, masks->may_read));
			has_use = true;
		}
		/* 没有任何帧读取时删除刚追加的 use 标签。 */
		if (!has_use)
			bpf_vlog_reset(&env->log, pos);
		pos = env->log.end_pos;
		/* def 与 use 对称；若二者均空，最终只保留反汇编文本。 */
		verbose(env, " def: ");
		for (frame = instance->depth; frame >= 0; --frame) {
			masks = get_frame_masks(instance, frame, insn_idx);
			if (!masks || spis_is_zero(masks->must_write))
				continue;
			verbose(env, "%s", fmt_spis_mask(env, frame, !has_def, masks->must_write));
			has_def = true;
		}
		/* def 为空时回滚；若 use 也为空则回滚至纯反汇编。 */
		if (!has_def)
			bpf_vlog_reset(&env->log, has_use ? pos : insn_pos);
		verbose(env, "\n");
		if (bpf_is_ldimm64(&insns[insn_idx]))
			i++;
	}
}

/*
 * 业务背景：稳定可读的诊断要求实例先按调用点、再按深度排序。
 * 入参：pa/pb 是 sort() 传入的“实例指针地址”。
 * 出参/返回：返回负/零/正比较结果；不修改实例。
 * 注意事项：字段为 u32 而差值转 int，仅用于验证器受限指令号/深度范围。
 */
static int cmp_instances(const void *pa, const void *pb)
{
	struct func_instance *a = *(struct func_instance **)pa;
	struct func_instance *b = *(struct func_instance **)pb;
	int dcallsite = (int)a->callsite - b->callsite;
	int ddepth = (int)a->depth - b->depth;

	/* sort() 需要严格弱序：调用点优先，完全相同时再比较深度。 */
	if (dcallsite)
		return dcallsite;
	if (ddepth)
		return ddepth;
	return 0;
}

/* print use/def slots for all instances ordered by callsite first, then by depth */
/*
 * 中文：收集哈希表全部实例，按调用点和深度排序后逐个打印 use/def。
 * 业务背景：哈希遍历次序不稳定，排序使回归日志可比较。
 * 入参：env 为借用环境并拥有实例表。
 * 出参/返回：成功返回 0；临时指针数组分配失败返回 -ENOMEM。
 * 注意事项：数组只借用实例，打印后仅释放数组；GFP_KERNEL_ACCOUNT 可睡眠。
 */
static int print_instances(struct bpf_verifier_env *env)
{
	struct func_instance *instance, **sorted_instances;
	struct bpf_liveness *liveness = env->liveness;
	int i, bkt, cnt;

	/* 两遍哈希扫描：首遍计数分配，次遍只借用指针填充数组。 */
	cnt = 0;
	hash_for_each(liveness->func_instances, bkt, instance, hl_node)
		cnt++;
	sorted_instances = kvmalloc_objs(*sorted_instances, cnt, GFP_KERNEL_ACCOUNT);
	if (!sorted_instances)
		return -ENOMEM;
	cnt = 0;
	hash_for_each(liveness->func_instances, bkt, instance, hl_node)
		sorted_instances[cnt++] = instance;
	/* 排序后日志稳定；数组销毁不影响仍由哈希表持有的实例。 */
	sort(sorted_instances, cnt, sizeof(*sorted_instances), cmp_instances, NULL);
	for (i = 0; i < cnt; i++)
		print_instance(env, sorted_instances[i]);
	kvfree(sorted_instances);
	return 0;
}

/*
 * Per-register tracking state for compute_subprog_args().
 * Tracks which frame's FP a value is derived from
 * and the byte offset from that frame's FP.
 *
 * The .frame field forms a lattice with three levels of precision:
 *
 *   precise {frame=N, off=V}      -- known absolute frame index and byte offset
 *        |
 *   offset-imprecise {frame=N, cnt=0}
 *        |                        -- known frame identity, unknown offset
 *   fully-imprecise {frame=ARG_IMPRECISE, mask=bitmask}
 *                                 -- unknown frame identity; .mask is a
 *                                    bitmask of which frame indices might be
 *                                    involved
 *
 * At CFG merge points, arg_track_join() moves down the lattice:
 *   - same frame + same offset  -> precise
 *   - same frame + different offset -> offset-imprecise
 *   - different frames          -> fully-imprecise (bitmask OR)
 *
 * At memory access sites (LDX/STX/ST), offset-imprecise marks only
 * the known frame's access mask as SPIS_ALL, while fully-imprecise
 * iterates bits in the bitmask and routes each frame to its target.
 */
/*
 * 中文：compute_subprog_args() 为每个寄存器追踪“值源自哪一层 FP”及其相对字节偏移。
 * frame 构成三层精度格：{frame=N,off=V} 是精确帧/偏移；{frame=N,cnt=0}
 * 保留帧身份但偏移未知；{ARG_IMPRECISE,mask} 连帧身份也未知，只保留可能帧位图。
 * CFG 汇合只能沿格向更保守方向移动：同帧同偏移保持精确，同帧不同偏移合并有限集合
 * 或降为未知偏移，不同帧降为位图。访存时，同帧未知偏移把该帧记 SPIS_ALL，
 * 完全不精确则逐位把全活记录路由到每个可能帧。
 */
#define MAX_ARG_OFFSETS 4

/* FP 来源抽象值：精确偏移集合、已知帧未知偏移，或未知帧位集。 */
struct arg_track {
	union {
		s16 off[MAX_ARG_OFFSETS]; /* byte offsets; off_cnt says how many */
		/* 中文：off 保存升序去重的候选字节偏移，实际项数由 off_cnt 指定。 */
		u16 mask;	/* arg bitmask when arg == ARG_IMPRECISE */
		/* 中文：frame=ARG_IMPRECISE 时 mask 按绝对帧号列出全部可能来源。 */
	};
	s8 frame;	/* absolute frame index, or enum arg_track_state */
	/* 中文：非负值为绝对帧号，负值取自 arg_track_state 哨兵。 */
	s8 off_cnt;	/* 0 = offset-imprecise, 1-4 = # of precise offsets */
	/* 中文：0 表示偏移未知，1..4 表示 off[] 的有效候选数。 */
};

enum arg_track_state {
	ARG_NONE	= -1,	/* not derived from any argument */
	/* 中文：值不源自任何 FP 参数；由常量、普通内存或破坏身份的运算产生。 */
	ARG_UNVISITED	= -2,	/* not yet reached by dataflow */
	/* 中文：CFG 前向传播尚未到达该元素，只用于固定点初始化。 */
	ARG_IMPRECISE	= -3,	/* lost identity; .mask is arg bitmask */
	/* 中文：具体帧身份已丢失，union 中的 mask 保存可能来源帧集合。 */
};

/* Track callee stack slots fp-8 through fp-512 (64 slots of 8 bytes each) */
/* 中文：跟踪被调者 fp-8..fp-512 的 64 个完整 8 字节 spill 槽。 */
#define MAX_ARG_SPILL_SLOTS 64

/*
 * Combined register + stack arg tracking: R0-R10 at indices 0-10,
 * outgoing stack arg slots at indices MAX_BPF_REG..MAX_BPF_REG+6.
 */
/*
 * 中文：把寄存器和栈参数合并进同一追踪数组：索引 0..10 对应 R0..R10，
 * MAX_BPF_REG 起的后续索引对应七个传出栈参数槽。两个编号域不可混用。
 */
#define MAX_AT_TRACK_REGS (MAX_BPF_REG + MAX_STACK_ARG_SLOTS)

/*
 * 业务背景：把 BPF_REG_PARAMS 的栈参数字节偏移映射为紧凑参数槽。
 * 入参：off 是有符号字节偏移，正负方向按绝对值归一。
 * 出参/返回：合法返回 0..MAX_STACK_ARG_SLOTS-1，否则 -1。
 * 注意事项：调用者仍负责指令确为 stack-arg 访问；不分配。
 */
static int stack_arg_off_to_slot(s16 off)
{
	int aoff = off < 0 ? -off : off;

	if (aoff / 8 > MAX_STACK_ARG_SLOTS)
		return -1;
	return aoff / 8 - 1;
}

/*
 * 业务背景：固定点传播需区分“尚未到达”与实际运行时的 NONE/FP 来源值。
 * 入参：at 是只读借用的规范 arg_track。
 * 出参/返回：frame 不等于 ARG_UNVISITED 返回 true，否则 false；无副作用。
 * 注意事项：任何其他负哨兵也属于已访问值；纯计算、不分配、不睡眠。
 */
static bool arg_is_visited(const struct arg_track *at)
{
	return at->frame != ARG_UNVISITED;
}

/*
 * 业务背景：只有 FP 派生值才可能把 helper/访存连接到 BPF 栈活性。
 * 入参：at 是只读借用的规范 arg_track。
 * 出参/返回：精确非负帧或 ARG_IMPRECISE 返回 true，NONE/UNVISITED 返回 false。
 * 注意事项：不验证 mask 是否非零；调用者在已访问状态上使用，纯计算且不睡眠。
 */
static bool arg_is_fp(const struct arg_track *at)
{
	return at->frame >= 0 || at->frame == ARG_IMPRECISE;
}

/*
 * 业务背景：把 arg_track 格值打印为 `_`、`?`、`IMPmask` 或 fp+offset 集合。
 * 入参：env 为日志接收者；at 为借用抽象值。
 * 出参/返回：无；向验证日志追加文本。
 * 注意事项：仅诊断、不改变抽象值；调用者负责日志级别判断。
 */
static void verbose_arg_track(struct bpf_verifier_env *env, struct arg_track *at)
{
	int i;

	/* 三种负哨兵用单字符/位图；非负帧再区分未知与有限偏移。 */
	switch (at->frame) {
	case ARG_NONE:      verbose(env, "_");                          break;
	case ARG_UNVISITED: verbose(env, "?");                          break;
	case ARG_IMPRECISE: verbose(env, "IMP%x", at->mask);            break;
	default:
		/* frame >= 0: absolute frame index */
		/* 中文：非负 frame 是绝对帧号，off_cnt=0 表示该帧内偏移未知。 */
		if (at->off_cnt == 0) {
			verbose(env, "fp%d ?", at->frame);
		} else {
			for (i = 0; i < at->off_cnt; i++) {
				if (i)
					verbose(env, "|");
				/* 多候选用竖线连接，保留同一绝对帧标签。 */
				verbose(env, "fp%d%+d", at->frame, at->off[i]);
			}
		}
		break;
	}
}

/*
 * 业务背景：固定点迭代只在抽象值真正变化时继续。
 * 入参：a/b 为已规范化的借用 arg_track。
 * 出参/返回：帧、位集或有效偏移序列完全相同返回 true；无副作用。
 * 注意事项：负状态除 IMPRECISE 外不读取 union/off_cnt。
 */
static bool arg_track_eq(const struct arg_track *a, const struct arg_track *b)
{
	int i;

	/* 先比较决定 union 解释方式的 frame，再只读取该状态的有效成员。 */
	if (a->frame != b->frame)
		return false;
	if (a->frame == ARG_IMPRECISE)
		return a->mask == b->mask;
	if (a->frame < 0)
		return true;
	if (a->off_cnt != b->off_cnt)
		return false;
	/* 精确同帧状态还需逐项比较规范化偏移序列。 */
	for (i = 0; i < a->off_cnt; i++)
		if (a->off[i] != b->off[i])
			return false;
	return true;
}

/*
 * 业务背景：构造“确定来自 frame=arg 且只有一个 FP 偏移”的格值。
 * 入参：arg 是绝对帧号；off 是有符号字节偏移。
 * 出参/返回：按值返回 off_cnt=1 的规范 arg_track；无副作用。
 * 注意事项：调用者保证 arg 非负。
 */
static struct arg_track arg_single(s8 arg, s16 off)
{
	struct arg_track at = {};

	at.frame = arg;
	at.off[0] = off;
	at.off_cnt = 1;
	return at;
}

/*
 * Merge two sorted offset arrays, deduplicate.
 * Returns off_cnt=0 if the result exceeds MAX_ARG_OFFSETS.
 * Both args must have the same frame and off_cnt > 0.
 */
/*
 * 中文：合并两个已排序的偏移数组并去重；若结果超过 MAX_ARG_OFFSETS，
 * 便以 off_cnt=0 表示偏移不精确。两个输入必须属于同一帧且偏移集合非空。
 * 业务背景：CFG 路径汇合时，在固定容量内尽量保留 FP 派生指针的精确候选偏移。
 * 入参：a、b 均为按值输入的规范 arg_track，frame 相同，off[] 升序且 off_cnt>0。
 * 出参/返回：返回合并后的按值格元素；容量溢出时仍保留 frame、丢弃偏移精度，无外部副作用。
 * 注意事项：纯计算且不睡眠；调用者随后把结果与旧格值比较以决定固定点是否继续。
 */
static struct arg_track arg_merge_offsets(struct arg_track a, struct arg_track b)
{
	struct arg_track result = { .frame = a.frame };
	struct arg_track imp = { .frame = a.frame };
	int i = 0, j = 0, k = 0;

	/* 两路归并公共前缀；相等元素只消费并输出一次。 */
	while (i < a.off_cnt && j < b.off_cnt) {
		s16 v;

		if (a.off[i] <= b.off[j]) {
			v = a.off[i++];
			if (v == b.off[j])
				j++;
		} else {
			v = b.off[j++];
		}
		/* 跳过与上一输出重复的值；容量检查必须早于数组写入。 */
		if (k > 0 && result.off[k - 1] == v)
			continue;
		if (k >= MAX_ARG_OFFSETS)
			return imp;
		result.off[k++] = v;
	}
	/* 任一剩余尾部仍须检查容量；超限立即退化为同帧未知偏移。 */
	while (i < a.off_cnt) {
		if (k >= MAX_ARG_OFFSETS)
			return imp;
		result.off[k++] = a.off[i++];
	}
	/* b 的尾部与 a 对称，最终 k 成为返回值的有效偏移计数。 */
	while (j < b.off_cnt) {
		if (k >= MAX_ARG_OFFSETS)
			return imp;
		result.off[k++] = b.off[j++];
	}
	result.off_cnt = k;
	return result;
}

/*
 * Merge two arg_tracks into ARG_IMPRECISE, collecting the frame
 * bits from both operands. Precise frame indices (frame >= 0)
 * contribute a single bit; existing ARG_IMPRECISE values
 * contribute their full bitmask.
 */
/*
 * 中文：把两个 arg_track 合并为 ARG_IMPRECISE；精确帧贡献一个位，已有
 * ARG_IMPRECISE 值贡献完整帧位图。
 * 业务背景：路径汇合后若来源帧不同，单一 frame 已无法表达，必须保守记录所有可能祖先帧。
 * 入参：a、b 是按值输入，可为精确帧、IMPRECISE 或不贡献 FP 来源的其他格状态。
 * 出参/返回：按值返回 frame=ARG_IMPRECISE 的并集位图；无输出参数和外部副作用。
 * 注意事项：未知/非 FP 状态不增加位；纯计算、不分配、不睡眠。
 */
static struct arg_track arg_join_imprecise(struct arg_track a, struct arg_track b)
{
	u32 m = 0;

	/* 分别把精确 frame 或已有 mask 投影到同一位图域，再取并集。 */
	if (a.frame >= 0)
		m |= BIT(a.frame);
	else if (a.frame == ARG_IMPRECISE)
		m |= a.mask;

	if (b.frame >= 0)
		m |= BIT(b.frame);
	else if (b.frame == ARG_IMPRECISE)
		m |= b.mask;

	return (struct arg_track){ .mask = m, .frame = ARG_IMPRECISE };
}

/* Join two arg_track values at merge points */
/*
 * 中文：在控制流汇合点求两个 arg_track 的最小安全上界，供前向数据流收敛。
 * 业务背景：所有前驱可能值都必须被结果覆盖，同时尽量保留帧与有限偏移精度。
 * 入参：a、b 为按值输入；ARG_UNVISITED 表示该前驱尚未到达，ARG_NONE 表示非 FP 来源。
 * 出参/返回：返回合并格值；不修改输入，也无分配或可观察副作用。
 * 注意事项：同帧最多保留四个偏移；不同帧退化为帧位图；纯计算且不睡眠。
 */
static struct arg_track __arg_track_join(struct arg_track a, struct arg_track b)
{
	/* 未访问不是实际运行时取值，直接采用已到达一侧，避免凭空降低精度。 */
	if (!arg_is_visited(&b))
		return a;
	if (!arg_is_visited(&a))
		return b;
	if (a.frame == b.frame && a.frame >= 0) {
		/* Both offset-imprecise: stay imprecise */
		/* 中文：任一同帧输入已是未知偏移，汇合后仍只能保持该未知偏移。 */
		if (a.off_cnt == 0 || b.off_cnt == 0)
			return (struct arg_track){ .frame = a.frame };
		/* Merge offset sets; falls back to off_cnt=0 if >4 */
		/* 中文：合并有限偏移集；超过四项时由 helper 退化为 off_cnt=0。 */
		return arg_merge_offsets(a, b);
	}

	/*
	 * args are different, but one of them is known
	 * arg + none -> arg
	 * none + arg -> arg
	 *
	 * none + none -> none
	 */
	/*
	 * 中文：来源不同但一侧是 NONE 时，FP 来源仍可保留；两侧均 NONE 仍是 NONE。
	 * 单偏移 FP 与 NONE 汇合时人为加入 fp+0，使后续既保留潜在栈读取，又不会把
	 * 该位置误认作所有路径都覆盖的栈定义。
	 */
	if (a.frame == ARG_NONE && b.frame == ARG_NONE)
		return a;
	if (a.frame >= 0 && b.frame == ARG_NONE) {
		/*
		 * When joining single fp-N add fake fp+0 to
		 * keep stack_use and prevent stack_def
		 */
		if (a.off_cnt == 1)
			return arg_merge_offsets(a, arg_single(a.frame, 0));
		return a;
	}
	if (b.frame >= 0 && a.frame == ARG_NONE) {
		/* 与上一分支对称，先把精确 FP 放在 merge helper 的左侧。 */
		if (b.off_cnt == 1)
			return arg_merge_offsets(b, arg_single(b.frame, 0));
		return b;
	}

	/* 其余组合无法用单帧表达，转为所有可能来源帧的保守位图。 */
	return arg_join_imprecise(a, b);
}

/*
 * 业务背景：把一个前驱输出汇入目标指令入口，并反馈数据流工作是否发生变化。
 * 入参：env 为借用日志环境；idx/target 是源/目标绝对指令号；r 标识寄存器、栈参数或 FP 槽；
 * in 是目标入口格值的输入输出借用指针；out 是当前前驱的按值输出。
 * 出参/返回：格值改变返回 true 并覆写 *in，否则 false；二级日志下还追加汇合轨迹。
 * 注意事项：验证线程独占 in/env；不分配、不睡眠，日志格式化只借用 env 临时设施。
 */
static bool arg_track_join(struct bpf_verifier_env *env, int idx, int target, int r,
			   struct arg_track *in, struct arg_track out)
{
	struct arg_track old = *in;
	struct arg_track new_val = __arg_track_join(old, out);

	/* 无变化是固定点快速路径，不写内存也不产生日志。 */
	if (arg_track_eq(&new_val, &old))
		return false;

	/* 先提交格值；日志只是提交后的可选诊断，不参与语义。 */
	*in = new_val;
	if (!(env->log.level & BPF_LOG_LEVEL2) || !arg_is_visited(&old))
		return true;

	/* 按 r 的编码域选择人类可读标签，再打印 old + out => new。 */
	verbose(env, "arg JOIN insn %d -> %d ", idx, target);
	if (r >= MAX_BPF_REG)
		verbose(env, "sa%d: ", r - MAX_BPF_REG);
	else if (r >= 0)
		verbose(env, "r%d: ", r);
	else
		verbose(env, "fp%+d: ", r * 8);
	/* 三个格值依次输出旧入口、当前前驱和汇合结果。 */
	verbose_arg_track(env, &old);
	verbose(env, " + ");
	verbose_arg_track(env, &out);
	verbose(env, " => ");
	verbose_arg_track(env, &new_val);
	verbose(env, "\n");
	return true;
}

/*
 * Compute the result when an ALU op destroys offset precision.
 * If a single arg is identifiable, preserve it with OFF_IMPRECISE.
 * If two different args are involved or one is already ARG_IMPRECISE,
 * the result is fully ARG_IMPRECISE.
 */
/*
 * 中文：计算会破坏偏移精度的 64 位 ALU 操作结果；若仍能辨认唯一来源帧，保留该帧
 * 并令偏移未知；若混入不同帧或已有 IMPRECISE，则退化为完整帧位图。
 * 业务背景：算术可改变 FP 派生偏移，但栈访问分析仍需知道结果可能指向哪些调用帧。
 * 入参：dst 为输入输出格值；src 为只读借用格值，二者都必须已被数据流访问。
 * 出参/返回：无直接返回值；原地更新 *dst，src 与 ownership 均不变。
 * 注意事项：WARN 仅诊断违反前提；纯计算、不睡眠，保守退化保证不会漏记栈访问。
 */
static void arg_track_alu64(struct arg_track *dst, const struct arg_track *src)
{
	WARN_ON_ONCE(!arg_is_visited(dst));
	WARN_ON_ONCE(!arg_is_visited(src));

	/* 唯一来源仍是 dst 所属帧，只需放弃数值偏移集合。 */
	if (dst->frame >= 0 && (src->frame == ARG_NONE || src->frame == dst->frame)) {
		/*
		 * rX += rY where rY is not arg derived
		 * rX += rX
		 */
		/* 中文：rX 与非参数值或同源 rY 运算，身份仍来自 rX，但具体偏移已不可知。 */
		dst->off_cnt = 0;
		return;
	}
	if (src->frame >= 0 && dst->frame == ARG_NONE) {
		/*
		 * rX += rY where rX is not arg derived
		 * rY identity leaks into rX
		 */
		/* 中文：原 dst 非 FP，src 的唯一帧身份传播进 dst，同样丢弃具体偏移。 */
		dst->off_cnt = 0;
		dst->frame = src->frame;
		return;
	}

	if (dst->frame == ARG_NONE && src->frame == ARG_NONE)
		return;

	/* 两侧至少一侧携带 FP 且不能归为同一帧，必须合并可能帧集合。 */
	*dst = arg_join_imprecise(*dst, *src);
}

/*
 * 业务背景：在有限 s16 偏移域中验证一次指针常量加法是否仍可精确表示。
 * 入参：off 是原 FP 字节偏移；delta 是待加的 64 位字节增量；out 为输出借用指针。
 * 出参/返回：delta 无法缩为 s16 或加法溢出返回 true；成功返回 false 并写入 *out。
 * 注意事项：失败时调用者不得使用 *out；纯计算、不睡眠，布尔值语义是“是否溢出”。
 */
static bool arg_add(s16 off, s64 delta, s16 *out)
{
	s16 d = delta;

	/* 先用往返比较拒绝截断，再让 checked helper 捕获 s16 加法溢出。 */
	if (d != delta)
		return true;
	return check_add_overflow(off, d, out);
}

/*
 * 业务背景：处理寄存器加常量时，平移同一来源帧的全部候选 FP 偏移。
 * 入参：at 为输入输出借用格值；delta 是有符号字节增量。
 * 出参/返回：无直接返回值；成功原地平移 off[]，任一溢出则令 off_cnt=0 保守降精度。
 * 注意事项：未知偏移是快速空操作；不分配、不睡眠，失败不会留下部分精确集合。
 */
static void arg_padd(struct arg_track *at, s64 delta)
{
	int i;

	/* off_cnt=0 已表示同帧未知偏移，无需再变换。 */
	if (at->off_cnt == 0)
		return;
	/* 逐项平移；一项越界就丢弃整个集合，避免部分更新被误当作完整可能集。 */
	for (i = 0; i < at->off_cnt; i++) {
		s16 new_off;

		if (arg_add(at->off[i], delta, &new_off)) {
			at->off_cnt = 0;
			return;
		}
		at->off[i] = new_off;
	}
}

/*
 * Convert a byte offset from FP to a callee stack slot index.
 * Returns -1 if out of range or not 8-byte aligned.
 * Slot 0 = fp-8, slot 1 = fp-16, ..., slot 7 = fp-64, ....
 */
/*
 * 中文：把相对 FP 的字节偏移换算成被调者 8 字节栈槽号；越界或未对齐返回 -1。
 * 槽 0 对应 fp-8，槽 1 对应 fp-16，槽 7 对应 fp-64，依此类推。
 * 业务背景：前向来源分析只追踪可承载指针 spill 的有限个完整栈槽。
 * 入参：off 是负的、相对当前帧 FP 的 s16 字节偏移。
 * 出参/返回：合法时返回 0..MAX_ARG_SPILL_SLOTS-1，否则 -1；无副作用。
 * 注意事项：仅接受 8 字节对齐的完整槽；纯计算、不睡眠。
 */
static int fp_off_to_slot(s16 off)
{
	/* 正/零偏移在栈外，过深偏移超出本抽象域；非 8 对齐不能代表完整 spill 槽。 */
	if (off >= 0 || off < -(int)(MAX_ARG_SPILL_SLOTS * 8))
		return -1;
	if (off % 8)
		return -1;
	return (-off) / 8 - 1;
}

/*
 * 业务背景：执行栈 load 的抽象语义，从一个或多个可能槽恢复 FP 来源格值。
 * 入参：insn 为只读访存指令；at_out 是各寄存器出口值；reg 是基址寄存器号；
 * at_stack_out 是当前栈槽出口数组；depth 是当前调用深度，以上指针均为借用。
 * 出参/返回：精确可定位时返回所读槽的合并值；未知/越界/溢出时返回覆盖 0..depth 帧的 IMPRECISE。
 * 注意事项：不修改输入、不分配、不睡眠；保守返回确保未知别名不会漏掉祖先栈 use。
 */
static struct arg_track fill_from_stack(struct bpf_insn *insn,
					struct arg_track *at_out, int reg,
					struct arg_track *at_stack_out,
					int depth)
{
	/* imp 覆盖从入口到当前 depth 的全部帧，是任何解析失败的统一返回值。 */
	struct arg_track imp = {
		.mask = (1u << (depth + 1)) - 1,
		.frame = ARG_IMPRECISE
	};
	struct arg_track result = { .frame = ARG_NONE };
	int cnt, i;

	/* 直接 FP 基址只有一个候选槽，可精确读取；非法偏移退化为所有可见帧。 */
	if (reg == BPF_REG_FP) {
		int slot = fp_off_to_slot(insn->off);

		return slot >= 0 ? at_stack_out[slot] : imp;
	}
	/* 非 R10 基址的 frame/off 已由前向 ALU 传递维护。 */
	/* 派生基址必须枚举其有限偏移；未知偏移可能别名任意可见帧。 */
	cnt = at_out[reg].off_cnt;
	if (cnt == 0)
		return imp;

	/* 将指令 off 加到每个基址候选，并合并所有可能被读槽的来源。 */
	for (i = 0; i < cnt; i++) {
		s16 fp_off, slot;

		if (arg_add(at_out[reg].off[i], insn->off, &fp_off))
			return imp;
		slot = fp_off_to_slot(fp_off);
		if (slot < 0)
			return imp;
		/* 多候选 load 可能来自任一槽，使用格 join 保留全部来源。 */
		result = __arg_track_join(result, at_stack_out[slot]);
	}
	return result;
}

/*
 * Spill @val to all possible stack slots indicated by the FP offsets in @reg.
 * For an 8-byte store, single candidate slot gets @val. multi-slots are joined.
 * sub-8-byte store joins with ARG_NONE.
 * When exact offset is unknown conservatively add reg values to all slots in at_stack_out.
 */
/*
 * 中文：把 val spill 到 reg 所描述的全部可能栈槽；8 字节单一候选可精确覆盖，多候选取汇合，
 * 小于 8 字节的写只能与 ARG_NONE 汇合；偏移未知时保守影响全部跟踪槽。
 * 业务背景：前向分析需模拟栈中保存的 FP 派生值，供后续 load 恢复来源。
 * 入参：insn/at_out 为只读借用；reg 是基址；at_stack_out 为输入输出槽数组；
 * val 是待保存格值的只读借用；sz 是写宽度（字节）。
 * 出参/返回：无直接返回值；原地覆盖或汇合可能槽，无 ownership 转移。
 * 注意事项：未知别名不能做强更新；纯计算、不分配、不睡眠。
 */
static void spill_to_stack(struct bpf_insn *insn, struct arg_track *at_out,
			   int reg, struct arg_track *at_stack_out,
			   struct arg_track *val, u32 sz)
{
	struct arg_track none = { .frame = ARG_NONE };
	struct arg_track new_val = sz == 8 ? *val : none;
	int cnt, i;

	/* 直接 FP 且槽合法时是唯一地址，可以强更新该槽。 */
	if (reg == BPF_REG_FP) {
		int slot = fp_off_to_slot(insn->off);

		if (slot >= 0)
			at_stack_out[slot] = new_val;
		return;
	}
	/* 派生基址偏移未知时，每个槽都可能被写，只能与旧值弱合并。 */
	cnt = at_out[reg].off_cnt;
	if (cnt == 0) {
		for (int slot = 0; slot < MAX_ARG_SPILL_SLOTS; slot++)
			at_stack_out[slot] = __arg_track_join(at_stack_out[slot], new_val);
		return;
	}
	/* 有限候选逐一换算；唯一候选强更新，多候选为保留其他路径而弱合并。 */
	for (i = 0; i < cnt; i++) {
		s16 fp_off;
		int slot;

		if (arg_add(at_out[reg].off[i], insn->off, &fp_off))
			continue;
		slot = fp_off_to_slot(fp_off);
		if (slot < 0)
			continue;
		/* 合法候选才参与强/弱更新；候选总数仍决定能否证明唯一地址。 */
		if (cnt == 1)
			at_stack_out[slot] = new_val;
		else
			at_stack_out[slot] = __arg_track_join(at_stack_out[slot], new_val);
		/* 越界候选不属于跟踪栈域，其合法性仍由主验证流程判断。 */
	}
}

/*
 * Clear all tracked callee stack slots overlapping the byte range
 * [off, off+sz-1] where off is a negative FP-relative offset.
 */
/*
 * 中文：清除所有与负 FP 相对字节区间 [off, off+sz-1] 重叠的被调者跟踪槽。
 * 业务背景：非完整 spill 的写会破坏槽内旧指针身份，别名不唯一时还必须保留未写路径。
 * 入参：at_stack 为输入输出槽数组；off/sz 是写区间的字节起点/长度；cnt 是候选地址数，0 表示未知。
 * 出参/返回：无直接返回值；唯一地址强置 NONE，多/未知地址与 NONE 汇合；无 ownership 变化。
 * 注意事项：遍历固定容量数组，不分配、不睡眠；区间采用半开形式判断重叠。
 */
static void clear_overlapping_stack_slots(struct arg_track *at_stack, s16 off, u32 sz, int cnt)
{
	struct arg_track none = { .frame = ARG_NONE };

	/* 未知地址可能命中任意槽，但不能证明必写，因此全部执行弱清除。 */
	if (cnt == 0) {
		for (int i = 0; i < MAX_ARG_SPILL_SLOTS; i++)
			at_stack[i] = __arg_track_join(at_stack[i], none);
		return;
	}
	/* 枚举各 8 字节槽，与写区间相交者按候选数选择强/弱更新。 */
	for (int i = 0; i < MAX_ARG_SPILL_SLOTS; i++) {
		int slot_start = -((i + 1) * 8);
		int slot_end = slot_start + 8;

		if (slot_start < off + (int)sz && slot_end > off) {
			/* 唯一写一定覆盖该身份；多候选只说明“可能覆盖”。 */
			if (cnt == 1)
				at_stack[i] = none;
			else
				at_stack[i] = __arg_track_join(at_stack[i], none);
		}
	}
}

/*
 * Clear stack slots overlapping all possible FP offsets in @reg.
 */
/*
 * 中文：对 reg 的每个可能 FP 偏移，清除与当前写指令重叠的跟踪槽。
 * 业务背景：把派生基址别名集合统一下沉到按槽更新 helper，避免旧 spill 身份穿过覆盖写。
 * 入参：insn/at_out 为只读借用；reg 是基址寄存器；at_stack_out 为输入输出槽数组；sz 为写字节数。
 * 出参/返回：无直接返回值；更新可能重叠槽，无分配或 ownership 转移。
 * 注意事项：偏移未知或加法溢出会保守弱清除全部槽；不睡眠。
 */
static void clear_stack_for_all_offs(struct bpf_insn *insn,
				     struct arg_track *at_out, int reg,
				     struct arg_track *at_stack_out, u32 sz)
{
	int cnt, i;

	/* 真实 FP 给出唯一基址，可直接按指令偏移执行强更新。 */
	if (reg == BPF_REG_FP) {
		clear_overlapping_stack_slots(at_stack_out, insn->off, sz, 1);
		return;
	}
	/* 派生基址无有限偏移集合时，任何跟踪槽都可能被部分覆盖。 */
	cnt = at_out[reg].off_cnt;
	if (cnt == 0) {
		clear_overlapping_stack_slots(at_stack_out, 0, sz, cnt);
		return;
	}
	/* 有限候选逐个叠加指令偏移；一旦溢出便退化到全槽弱清除。 */
	for (i = 0; i < cnt; i++) {
		s16 fp_off;

		if (arg_add(at_out[reg].off[i], insn->off, &fp_off)) {
			clear_overlapping_stack_slots(at_stack_out, 0, sz, 0);
			break;
		}
		clear_overlapping_stack_slots(at_stack_out, fp_off, sz, cnt);
	}
}

/*
 * 业务背景：二级验证日志逐条展示寄存器、栈参数槽与 spill 槽的来源格值变化。
 * 入参：env 为日志输出环境；insn/idx 标识当前指令；at_in/at_stack_in 是入口快照，
 * at_out/at_stack_out 是出口快照，所有数组均为借用且长度由对应 MAX 常量规定。
 * 出参/返回：无直接返回值；仅在 LEVEL2 下向日志追加发生变化的字段。
 * 注意事项：不改变分析状态、不分配；共享 env 日志缓冲，仅验证线程调用。
 */
static void arg_track_log(struct bpf_verifier_env *env, struct bpf_insn *insn, int idx,
			  struct arg_track *at_in, struct arg_track *at_stack_in,
			  struct arg_track *at_out, struct arg_track *at_stack_out)
{
	bool printed = false;
	int i;

	/* 非详细日志是零开销快速路径；随后先输出普通寄存器差异。 */
	if (!(env->log.level & BPF_LOG_LEVEL2))
		return;
	for (i = 0; i < MAX_BPF_REG; i++) {
		if (arg_track_eq(&at_out[i], &at_in[i]))
			continue;
		/* 首个普通寄存器变化负责打印一次指令前缀。 */
		/* 首个差异才打印一次指令，并移除反汇编换行以拼接状态。 */
		if (!printed) {
			verbose(env, "%3d: ", idx);
			bpf_verbose_insn(env, insn);
			bpf_vlog_reset(&env->log, env->log.end_pos - 1);
			/* 此后各类槽共享该前缀，printed 阻止重复反汇编。 */
			printed = true;
		}
		verbose(env, "\tr%d: ", i); verbose_arg_track(env, &at_in[i]);
		verbose(env, " -> "); verbose_arg_track(env, &at_out[i]);
	}
	/* Log outgoing stack arg slot transitions at indices MAX_BPF_REG..MAX_AT_TRACK_REGS-1 */
	/* 中文：再输出紧随寄存器区的传出栈参数槽转换，索引需减去 MAX_BPF_REG。 */
	for (i = 0; i < MAX_STACK_ARG_SLOTS; i++) {
		int ai = MAX_BPF_REG + i;

		if (arg_track_eq(&at_out[ai], &at_in[ai]))
			continue;
		/* 参数槽变化复用已打印的指令前缀，或在此创建它。 */
		if (!printed) {
			verbose(env, "%3d: ", idx);
			bpf_verbose_insn(env, insn);
			bpf_vlog_reset(&env->log, env->log.end_pos - 1);
			/* spill 是最后一类输出；此处仍可能首次发现整条指令的变化。 */
			printed = true;
		}
		verbose(env, "\tsa%d: ", i); verbose_arg_track(env, &at_in[ai]);
		verbose(env, " -> "); verbose_arg_track(env, &at_out[ai]);
	}
	/* 最后输出当前帧负 FP 偏移所对应的本地 spill 槽变化。 */
	for (i = 0; i < MAX_ARG_SPILL_SLOTS; i++) {
		if (arg_track_eq(&at_stack_out[i], &at_stack_in[i]))
			continue;
		/* 本地 spill 槽同样只输出实际变化项。 */
		if (!printed) {
			verbose(env, "%3d: ", idx);
			bpf_verbose_insn(env, insn);
			bpf_vlog_reset(&env->log, env->log.end_pos - 1);
			/* 记录已输出前缀，余下槽只追加自身转换。 */
			printed = true;
		}
		verbose(env, "\tfp%+d: ", -(i + 1) * 8); verbose_arg_track(env, &at_stack_in[i]);
		verbose(env, " -> "); verbose_arg_track(env, &at_stack_out[i]);
	}
	if (printed)
		verbose(env, "\n");
}

/*
 * 业务背景：内存传递函数只应把当前帧 FP 及其派生值当成本地栈基址。
 * 入参：depth 是当前绝对调用深度；regno 是寄存器号；at 是该寄存器的只读借用格值。
 * 出参/返回：真实 FP、精确当前帧或 IMPRECISE 位图含当前帧时返回 true，否则 false。
 * 注意事项：只判来源可能性，不证明偏移合法；纯计算、不睡眠。
 */
static bool can_be_local_fp(int depth, int regno, struct arg_track *at)
{
	return regno == BPF_REG_FP || at->frame == depth ||
	       (at->frame == ARG_IMPRECISE && (at->mask & BIT(depth)));
}

/*
 * Pure dataflow transfer function for arg_track state.
 * Updates at_out[] based on how the instruction modifies registers.
 * Tracks spill/fill, but not other memory accesses.
 */
/*
 * 中文：arg_track 的纯数据流传递函数，按指令修改 at_out[]，跟踪 spill/fill，
 * 但不在这里登记其他内存读写。
 * 业务背景：compute_subprog_args() 用它前向传播 FP 来源，随后才把 helper/访存转换成栈 use/def。
 * 入参：env/insn 为只读上下文；insn_idx 是绝对指令号；at_out/at_stack_out 为输入输出出口状态；
 * at_stack_arg_entry 是入口栈参数快照；instance 给出当前深度；callsites 映射父帧调用点，均为借用。
 * 出参/返回：无直接返回值；原地更新寄存器、参数槽和 spill 槽，不转移 ownership。
 * 注意事项：验证线程独占数组；不分配、不睡眠，所有未知别名均向过近似退化。
 */
static void arg_track_xfer(struct bpf_verifier_env *env, struct bpf_insn *insn,
			   int insn_idx,
			   struct arg_track *at_out, struct arg_track *at_stack_out,
			   const struct arg_track *at_stack_arg_entry,
			   struct func_instance *instance,
			   u32 *callsites)
{
	/* 先缓存当前指令分类与寄存器元素别名，避免各分支重复解码。 */
	int depth = instance->depth;
	u8 class = BPF_CLASS(insn->code);
	u8 code = BPF_OP(insn->code);
	struct arg_track *dst = &at_out[insn->dst_reg];
	struct arg_track *src = &at_out[insn->src_reg];
	struct arg_track none = { .frame = ARG_NONE };
	int r, slot;

	/* dst/src 是当前出口数组元素的借用别名，所有 case 都原地更新同一快照。 */
	/* Handle stack arg stores and loads. */
	/* 中文：阶段一处理 BPF_REG_PARAMS 地址空间；store 发布传出参数，load 读取入口快照。 */
	if (is_stack_arg_st(insn) || is_stack_arg_stx(insn)) {
		slot = stack_arg_off_to_slot(insn->off);
		if (slot >= 0) {
			/* STX 传播源身份，立即数 ST 明确写入非 FP 值。 */
			if (is_stack_arg_stx(insn))
				at_out[MAX_BPF_REG + slot] = at_out[insn->src_reg];
			else
				at_out[MAX_BPF_REG + slot] = none;
		}
	} else if (is_stack_arg_ldx(insn)) {
		slot = stack_arg_off_to_slot(insn->off);
		at_out[insn->dst_reg] = (slot >= 0) ? at_stack_arg_entry[slot] : none;
	/* 阶段二模拟 ALU：常量加减平移偏移，其他算术或地址空间转换按规则丢失精度/身份。 */
	} else if (class == BPF_ALU64 && BPF_SRC(insn->code) == BPF_K) {
		if (code == BPF_MOV) {
			*dst = none;
		} else if (dst->frame >= 0) {
			if (code == BPF_ADD)
				arg_padd(dst, insn->imm);
			else if (code == BPF_SUB)
				arg_padd(dst, -(s64)insn->imm);
			else
				/* Any other 64-bit alu on the pointer makes it imprecise */
				/* 中文：其他 64 位运算仍保留来源帧，但具体指针偏移已无法恢复。 */
				dst->off_cnt = 0;
		} /* else if dst->frame is imprecise it stays so */
		/* 中文：dst 已是 IMPRECISE 时保持原位图即可，运算不会恢复精度。 */
	} else if (class == BPF_ALU64 && BPF_SRC(insn->code) == BPF_X) {
		if (code == BPF_MOV) {
			if (insn->off == 0) {
				*dst = *src;
			} else {
				/* addr_space_cast destroys a pointer */
				/* 中文：带 off 的 MOV 表示地址空间转换，旧 FP 指针身份不能跨域保留。 */
				*dst = none;
			}
		} else {
			arg_track_alu64(dst, src);
		}
	} else if (class == BPF_ALU) {
		/*
		 * 32-bit alu destroys the pointer.
		 * If src was a pointer it cannot leak into dst
		 */
		/* 中文：32 位 ALU 会截断指针；即便 src 源自 FP，也不能把该身份传播到 dst。 */
		*dst = none;
	} else if (class == BPF_JMP && code == BPF_CALL) {
		/*
		 * at_stack_out[slot] is not cleared by the helper and subprog calls.
		 * The fill_from_stack() may return the stale spill — which is an FP-derived arg_track
		 * (the value that was originally spilled there). The loaded register then carries
		 * a phantom FP-derived identity that doesn't correspond to what's actually in the slot.
		 * This phantom FP pointer propagates forward, and wherever it's subsequently used
		 * (as a helper argument, another store, etc.), it sets stack liveness bits.
		 * Those bits correspond to stack accesses that don't actually happen.
		 * So the effect is over-reporting stack liveness — marking slots as live that aren't
		 * actually accessed. The verifier preserves more state than necessary across calls,
		 * which is conservative.
		 *
		 * helpers can scratch stack slots, but they won't make a valid pointer out of it.
		 * subprogs are allowed to write into parent slots, but they cannot write
		 * _any_ FP-derived pointer into it (either their own or parent's FP).
		 */
		/*
		 * 中文：调用会破坏 R0..R5，但这里故意不清 spill 槽。helper 可能改槽，子程序也可写父栈，
		 * 因而以后 fill 可能读到陈旧 FP 身份并多报活性；这只会保留更多状态，仍是安全过近似。
		 * helper 不会凭空造合法指针，子程序也不能把自身或父级 FP 派生指针写入父槽，所以不会漏报。
		 */
		/* 调用 ABI 后仅 R6..R10 保持，清除易失寄存器的来源身份。 */
		for (r = BPF_REG_0; r <= BPF_REG_5; r++)
			at_out[r] = none;
	/* 阶段三模拟内存：仅完整 8 字节普通 load/store 能恢复或保存指针身份。 */
	} else if (class == BPF_LDX) {
		u32 sz = bpf_size_to_bytes(BPF_SIZE(insn->code));
		bool src_is_local_fp = can_be_local_fp(depth, insn->src_reg, src);

		/*
		 * Reload from callee stack: if src is current-frame FP-derived
		 * and the load is an 8-byte BPF_MEM, try to restore the spill
		 * identity.  For imprecise sources fill_from_stack() returns
		 * ARG_IMPRECISE (off_cnt == 0).
		 */
		/*
		 * 中文：从被调者本地栈重载时，若基址可能源自当前帧且是 8 字节 BPF_MEM，
		 * 尝试恢复 spill 身份；基址偏移不精确时 fill helper 返回 IMPRECISE。
		 */
		if (src_is_local_fp && BPF_MODE(insn->code) == BPF_MEM && sz == 8) {
			*dst = fill_from_stack(insn, at_out, insn->src_reg, at_stack_out, depth);
		/* 精确父帧来源需借调用点保存的父栈出口快照解析。 */
		} else if (src->frame >= 0 && src->frame < depth &&
			   BPF_MODE(insn->code) == BPF_MEM && sz == 8) {
			struct arg_track *parent_stack =
				env->callsite_at_stack[callsites[src->frame]];

			*dst = fill_from_stack(insn, at_out, insn->src_reg,
					       parent_stack, src->frame);
		} else if (src->frame == ARG_IMPRECISE &&
			   !(src->mask & BIT(depth)) && src->mask &&
			   BPF_MODE(insn->code) == BPF_MEM && sz == 8) {
			/*
			 * Imprecise src with only parent-frame bits:
			 * conservative fallback.
			 */
			/* 中文：来源只可能是若干父帧但无法选定快照，保留原 IMPRECISE 位图作保守回退。 */
			*dst = *src;
		} else {
			*dst = none;
		}
	} else if (class == BPF_LD && BPF_MODE(insn->code) == BPF_IMM) {
		*dst = none;
	/* STX 先记录本地完整/部分 spill，再按原子操作的隐式写回寄存器修正状态。 */
	} else if (class == BPF_STX) {
		u32 sz = bpf_size_to_bytes(BPF_SIZE(insn->code));
		bool dst_is_local_fp;

		/* Track spills to current-frame FP-derived callee stack */
		/* 中文：只追踪当前帧 FP 派生的被调者栈；普通内存不属于本地 spill 模型。 */
		dst_is_local_fp = can_be_local_fp(depth, insn->dst_reg, dst);
		if (dst_is_local_fp && BPF_MODE(insn->code) == BPF_MEM)
			spill_to_stack(insn, at_out, insn->dst_reg,
				       at_stack_out, src, sz);

		/* 原子 RMW 除内存写外还可能覆盖 R0、dst 或 src；同步更新抽象寄存器身份。 */
		if (BPF_MODE(insn->code) == BPF_ATOMIC) {
			if (dst_is_local_fp && insn->imm != BPF_LOAD_ACQ)
				clear_stack_for_all_offs(insn, at_out, insn->dst_reg,
							 at_stack_out, sz);

			/* 各原子编码的返回寄存器不同，必须清除被硬件旧值覆盖的 FP 身份。 */
			if (insn->imm == BPF_CMPXCHG)
				at_out[BPF_REG_0] = none;
			else if (insn->imm == BPF_LOAD_ACQ)
				*dst = none;
			else if (insn->imm & BPF_FETCH)
				*src = none;
		}
	} else if (class == BPF_ST && BPF_MODE(insn->code) == BPF_MEM) {
		u32 sz = bpf_size_to_bytes(BPF_SIZE(insn->code));
		bool dst_is_local_fp = can_be_local_fp(depth, insn->dst_reg, dst);

		/* BPF_ST to FP-derived dst: clear overlapping stack slots */
		/* 中文：立即数写不保存来源值，只需使所有可能重叠的旧 spill 身份失效。 */
		if (dst_is_local_fp)
			clear_stack_for_all_offs(insn, at_out, insn->dst_reg,
						 at_stack_out, sz);
	}
}

/*
 * Record access_bytes from helper/kfunc or load/store insn.
 *   access_bytes > 0:      stack read
 *   access_bytes < 0:      stack write
 *   access_bytes == S64_MIN: unknown   — conservative, mark [0..slot] as read
 *   access_bytes == 0:      no access
 *
 */
/*
 * 中文：把 helper/kfunc 或访存的字节数转换为 half-SPI use/def：正数为读，负数为写，
 * S64_MIN 表示从 fp_off 到 fp+0 的未知长度读，0 表示无访问。
 * 业务背景：反向活性只理解位集，本函数负责将精确 FP 字节区间离散为可传递的栈半槽。
 * 入参：instance 为输入输出实例；fp_off 是相对 frame FP 的字节偏移；access_bytes 为上述编码；
 * frame 是实例内祖先帧号；insn_idx 是绝对指令号。
 * 出参/返回：成功/无需记录返回 0；懒分配失败返回 -ENOMEM，并保留此前已登记位。
 * 注意事项：越界访问交给主验证器拒绝；读标记所有触碰槽，写只 kill 完整覆盖槽；可能睡眠。
 */
static int record_stack_access_off(struct func_instance *instance, s64 fp_off,
				   s64 access_bytes, u32 frame, u32 insn_idx)
{
	s32 slot_hi, slot_lo;
	spis_t mask;

	/* 非负偏移不属于有效 BPF 栈；活性阶段不抢先替代主验证器的错误诊断。 */
	if (fp_off >= 0)
		/*
		 * out of bounds stack access doesn't contribute
		 * into actual stack liveness. It will be rejected
		 * by the main verifier pass later.
		 */
		/* 中文：越界访问不贡献真实栈活性，稍后的主验证流程会正式拒绝它。 */
		return 0;
	if (access_bytes == S64_MIN) {
		/* helper/kfunc read unknown amount of bytes from fp_off until fp+0 */
		/* 中文：helper/kfunc 从 fp_off 向上读取未知长度，保守把直到 fp+0 的槽全部记为 use。 */
		slot_hi = (-fp_off - 1) / STACK_SLOT_SZ;
		mask = SPIS_ZERO;
		spis_or_range(&mask, 0, slot_hi);
		return mark_stack_read(instance, frame, insn_idx, mask);
	}
	if (access_bytes > 0) {
		/* Mark any touched slot as use */
		/* 中文：读区间触及任一字节都要求旧值存在，因此首尾覆盖的半槽均记 use。 */
		slot_hi = (-fp_off - 1) / STACK_SLOT_SZ;
		slot_lo = max_t(s32, (-fp_off - access_bytes) / STACK_SLOT_SZ, 0);
		mask = SPIS_ZERO;
		spis_or_range(&mask, slot_lo, slot_hi);
		return mark_stack_read(instance, frame, insn_idx, mask);
	} else if (access_bytes < 0) {
		/* Mark only fully covered slots as def */
		/* 中文：写只有完整覆盖半槽才可作为 def 杀死旧活性，部分覆盖仍可能需要旧字节。 */
		access_bytes = -access_bytes;
		slot_hi = (-fp_off) / STACK_SLOT_SZ - 1;
		slot_lo = max_t(s32, (-fp_off - access_bytes + STACK_SLOT_SZ - 1) / STACK_SLOT_SZ, 0);
		/* 只有非空的完整覆盖范围才产生 def；部分字节写保持旧值活性。 */
		if (slot_lo <= slot_hi) {
			mask = SPIS_ZERO;
			spis_or_range(&mask, slot_lo, slot_hi);
			return mark_stack_write(instance, frame, insn_idx, mask);
		}
	}
	return 0;
}

/*
 * 'arg' is FP-derived argument to helper/kfunc or load/store that
 * reads (positive) or writes (negative) 'access_bytes' into 'use' or 'def'.
 */
/*
 * 中文：arg 是 helper/kfunc 或访存使用的 FP 派生参数；正 access_bytes 登记 use，负数登记 def。
 * 业务背景：在有限/未知偏移集合上应用单偏移区间转换，同时避免不唯一写产生错误 kill。
 * 入参：instance 为输入输出实例；arg 为只读来源；access_bytes 为长度编码；frame/insn_idx 定位帧和指令。
 * 出参/返回：成功或不能安全登记写时返回 0；任一次掩码分配失败返回 errno。
 * 注意事项：未知读记整帧，未知写不记 def；多候选写也不记 def；可能睡眠且部分读位可已发布。
 */
static int record_stack_access(struct func_instance *instance,
			       const struct arg_track *arg,
			       s64 access_bytes, u32 frame, u32 insn_idx)
{
	int i, err;

	/* 无访问是快速路径；未知偏移只允许保守扩大读集合，绝不猜测必写集合。 */
	if (access_bytes == 0)
		return 0;
	if (arg->off_cnt == 0) {
		if (access_bytes > 0 || access_bytes == S64_MIN)
			return mark_stack_read(instance, frame, insn_idx, SPIS_ALL);
		return 0;
	}
	if (access_bytes != S64_MIN && access_bytes < 0 && arg->off_cnt != 1)
		/* multi-offset write cannot set stack_def */
		/* 中文：多候选地址的写只命中其中之一，不能把任何槽宣称为所有路径必写。 */
		return 0;

	/* 读或唯一写逐偏移登记；失败立即上传，顶层统一释放分析对象。 */
	for (i = 0; i < arg->off_cnt; i++) {
		err = record_stack_access_off(instance, arg->off[i], access_bytes, frame, insn_idx);
		if (err)
			return err;
	}
	return 0;
}

/*
 * When a pointer is ARG_IMPRECISE, conservatively mark every frame in
 * the bitmask as fully used.
 */
/*
 * 中文：指针为 ARG_IMPRECISE 时，把位图中的每个可能帧完整标记为已读。
 * 业务背景：无法恢复具体帧/偏移的访问仍不能漏掉任何祖先栈依赖。
 * 入参：instance 为输入输出实例；mask 按绝对帧编号置位；insn_idx 是绝对指令号。
 * 出参/返回：全部登记成功返回 0；首次分配失败返回 errno，先前帧标记保留。
 * 注意事项：忽略深于当前实例的无效位；调用 mark_stack_read() 因而可能睡眠。
 */
static int record_imprecise(struct func_instance *instance, u32 mask, u32 insn_idx)
{
	int depth = instance->depth;
	int f, err;

	/* 逐个消费最低位；只把当前实例确实可见的祖先帧标为全活。 */
	for (f = 0; mask; f++, mask >>= 1) {
		if (!(mask & 1))
			continue;
		/* mask 可能带超出当前实例深度的位，这些帧在本调用链中不可见。 */
		if (f <= depth) {
			err = mark_stack_read(instance, f, insn_idx, SPIS_ALL);
			if (err)
				return err;
		}
	}
	return 0;
}

/* Record load/store access for a given 'at' state of 'insn'. */
/*
 * 中文：依据指令入口 arg_track 状态，为一条普通/原子 load-store 登记栈 use/def。
 * 业务背景：传递函数只追踪来源身份，本函数把实际内存方向、宽度与有效地址组合成活性事实。
 * 入参：env 为借用程序环境；instance 为输入输出实例；at 为指令入口数组；insn_idx 为绝对指令号。
 * 出参/返回：成功或非栈访问返回 0；掩码懒分配失败返回 errno。
 * 注意事项：BPF_REG_PARAMS 指令另属参数通道；未知来源帧保守全读；可能睡眠。
 */
static int record_load_store_access(struct bpf_verifier_env *env,
				    struct func_instance *instance,
				    struct arg_track *at, int insn_idx)
{
	struct bpf_insn *insn = &env->prog->insnsi[insn_idx];
	int depth = instance->depth;
	s32 sz = bpf_size_to_bytes(BPF_SIZE(insn->code));
	u8 class = BPF_CLASS(insn->code);
	struct arg_track resolved, *ptr;
	int oi;

	/*
	 * Stack arg insns use dst_reg/src_reg=BPF_REG_PARAMS(11). Since at[]
	 * is extended to MAX_AT_TRACK_REGS, at[11] holds the arg_track for
	 * outgoing stack arg slot 0 — not the pointer used for the memory
	 * access. Skip so the slot's tracked value isn't confused with the
	 * base register that record_stack_access() expects.
	 */
	/*
	 * 中文：栈参数指令把寄存器号 11 当专用地址空间；扩展数组的 at[11] 却存参数槽 0，
	 * 并非 record_stack_access() 所需的基址指针，因此必须跳过以免混淆。
	 */
	if (is_stack_arg_stx(insn) || is_stack_arg_st(insn) || is_stack_arg_ldx(insn))
		return 0;

	/* 阶段一按指令类别选基址，并把写宽度编码为负数；原子 acquire/release 另有方向。 */
	switch (class) {
	case BPF_LDX:
		/* load 从 src 地址读取，所有触及槽都是 use。 */
		ptr = &at[insn->src_reg];
		break;
	case BPF_STX:
		/* 普通 STX 向 dst 写；原子 acquire 是读 src，release/RMW 依具体 imm 选方向。 */
		if (BPF_MODE(insn->code) == BPF_ATOMIC) {
			if (insn->imm == BPF_STORE_REL)
				sz = -sz;
			/* LOAD_ACQ 的地址编码在 src，其余原子访存地址编码在 dst。 */
			if (insn->imm == BPF_LOAD_ACQ)
				ptr = &at[insn->src_reg];
			else
				ptr = &at[insn->dst_reg];
		} else {
			ptr = &at[insn->dst_reg];
			sz = -sz;
		}
		break;
	case BPF_ST:
		/* 立即数 store 写 dst 指向内存，负 sz 表示 def 候选。 */
		ptr = &at[insn->dst_reg];
		sz = -sz;
		break;
	default:
		/* 非访存类别不产生本阶段栈访问事实。 */
		return 0;
	}

	/* Resolve offsets: fold insn->off into arg_track */
	/* 中文：阶段二把指令静态 off 折入每个候选；任一溢出就退化为同帧未知偏移。 */
	if (ptr->off_cnt > 0) {
		resolved.off_cnt = ptr->off_cnt;
		resolved.frame = ptr->frame;
		for (oi = 0; oi < ptr->off_cnt; oi++) {
			if (arg_add(ptr->off[oi], insn->off, &resolved.off[oi])) {
				resolved.off_cnt = 0;
				break;
			}
		}
		/* resolved 是栈上临时格值，ptr 只在本函数余下阶段借用它。 */
		ptr = &resolved;
	}

	/* 阶段三按精确帧、帧位图或非 FP 来源分派最终记录。 */
	if (ptr->frame >= 0 && ptr->frame <= depth)
		return record_stack_access(instance, ptr, sz, ptr->frame, insn_idx);
	if (ptr->frame == ARG_IMPRECISE)
		return record_imprecise(instance, ptr->mask, insn_idx);
	/* ARG_NONE: not derived from any frame pointer, skip */
	/* 中文：ARG_NONE 不源自任何帧指针，因此该访存不属于 BPF 栈活性域。 */
	return 0;
}

/*
 * 业务背景：把 helper/kfunc 的一个寄存器或栈参数转换为对应祖先帧的栈访问事实。
 * 入参：env 为借用环境；instance 为输入输出实例；insn 是调用指令；at 是参数来源格值；
 * arg_idx 是调用 ABI 参数序号；insn_idx 是绝对指令号。
 * 出参/返回：非 FP/无访问返回 0；成功登记返回 0；掩码分配失败返回 errno。
 * 注意事项：未知调用种类会把所有可见帧全读；helper/kfunc 查询不转移对象；可能睡眠。
 */
static int record_arg_access(struct bpf_verifier_env *env,
			     struct func_instance *instance,
			     struct bpf_insn *insn,
			     struct arg_track *at, int arg_idx,
			     int insn_idx)
{
	int depth = instance->depth;
	int frame = at->frame;
	int err = 0;
	s64 bytes;

	/* 非 FP 参数不可能指向任一 BPF 栈，直接跳过。 */
	if (!arg_is_fp(at))
		return 0;

	/* 已知 helper/kfunc 原型给出精确读写字节编码；未知调用采取全帧读取。 */
	if (bpf_helper_call(insn)) {
		bytes = bpf_helper_stack_access_bytes(env, insn, arg_idx, insn_idx);
	} else if (bpf_pseudo_kfunc_call(insn)) {
		bytes = bpf_kfunc_stack_access_bytes(env, insn, arg_idx, insn_idx);
	} else {
		/* 无已知原型时无法判断哪个 FP 参数被解引用，所有可见帧都必须保守全活。 */
		for (int f = 0; f <= depth; f++) {
			err = mark_stack_read(instance, f, insn_idx, SPIS_ALL);
			if (err)
				return err;
		}
		return 0;
	}
	/* 原型明确无访问时无需登记；否则按精确帧或不精确帧集合下沉。 */
	if (bytes == 0)
		return 0;

	if (frame >= 0 && frame <= depth)
		err = record_stack_access(instance, at, bytes, frame, insn_idx);
	else if (frame == ARG_IMPRECISE)
		err = record_imprecise(instance, at->mask, insn_idx);
	return err;
}

/* Record stack access for a given 'at' state of helper/kfunc 'insn' */
/*
 * 中文：依据调用入口的 at 状态，逐个登记 helper/kfunc 参数可能产生的栈访问。
 * 业务背景：调用原型决定实际参数个数，既包括 R1..R5，也包括扩展栈参数槽。
 * 入参：env 为借用环境；instance 为输入输出实例；at 为调用入口格值数组；insn_idx 为绝对指令号。
 * 出参/返回：伪子程序调用或成功返回 0；任一参数掩码分配失败返回 errno。
 * 注意事项：无摘要时按五个寄存器参数处理；可能睡眠，失败前已登记位由实例统一回收。
 */
static int record_call_access(struct bpf_verifier_env *env,
			      struct func_instance *instance,
			      struct arg_track *at,
			      int insn_idx)
{
	struct bpf_insn *insn = &env->prog->insnsi[insn_idx];
	struct bpf_call_summary cs;
	int r, err, num_params = 5;

	/* BPF-to-BPF 调用由递归实例分析覆盖，不按 helper/kfunc 原型重复登记。 */
	if (bpf_pseudo_call(insn))
		return 0;

	/* 可用摘要会把参数上限扩展到真实 ABI，包括 R5 后的栈参数。 */
	if (bpf_get_call_summary(env, insn, &cs))
		num_params = cs.num_params;

	/* 先处理寄存器参数，再处理线性排在其后的栈参数，错误立即上传。 */
	for (r = BPF_REG_1; r < BPF_REG_1 + min(num_params, MAX_BPF_FUNC_REG_ARGS); r++) {
		err = record_arg_access(env, instance, insn, &at[r], r - 1, insn_idx);
		if (err)
			return err;
	}

	/* 参数个数超过寄存器 ABI 容量时，余项从专用栈参数槽继续。 */
	for (r = 0; r < MAX_STACK_ARG_SLOTS && r < num_params - MAX_BPF_FUNC_REG_ARGS; r++) {
		err = record_arg_access(env, instance, insn, &at[MAX_BPF_REG + r],
					r + MAX_BPF_FUNC_REG_ARGS, insn_idx);
		if (err)
			return err;
	}
	return 0;
}

/*
 * For a calls_callback helper, find the callback subprog and determine
 * which caller register maps to which callback register for FP passthrough.
 */
/*
 * 中文：对会调用 callback 的 helper，找出回调子程序及调用者参数到回调寄存器的 FP 来源映射。
 * 业务背景：递归分析 callback 时，helper ABI 穿透的指针参数必须进入正确的回调入口寄存器。
 * 入参：env/insn 为借用；insn_idx 为绝对调用指令号；caller_reg/callee_reg 为输出指针。
 * 出参/返回：成功返回回调 subprog 编号并写两个寄存器；不支持返回 -1，回调不恒定返回 -2。
 * 注意事项：输出先置 -1；只读 aux、不分配不睡眠，调用者仅在非负返回时使用映射。
 */
static int find_callback_subprog(struct bpf_verifier_env *env,
				 struct bpf_insn *insn, int insn_idx,
				 int *caller_reg, int *callee_reg)
{
	struct bpf_insn_aux_data *aux = &env->insn_aux_data[insn_idx];
	int cb_reg = -1;

	*caller_reg = -1;
	*callee_reg = -1;

	/* 非 helper 没有这里定义的 callback ABI。 */
	if (!bpf_helper_call(insn))
		return -1;
	switch (insn->imm) {
	case BPF_FUNC_loop:
		/* bpf_loop(nr, cb, ctx, flags): cb=R2, R3->cb R2 */
		/* 中文：R2 是回调函数，调用者 R3 的 ctx 进入回调 R2；随后返回统一校验。 */
		cb_reg = BPF_REG_2;
		*caller_reg = BPF_REG_3;
		*callee_reg = BPF_REG_2;
		break;
	case BPF_FUNC_for_each_map_elem:
		/* for_each_map_elem(map, cb, ctx, flags): cb=R2, R3->cb R4 */
		/* 中文：R2 是回调函数，调用者 R3 的 ctx 进入回调 R4；随后返回统一校验。 */
		cb_reg = BPF_REG_2;
		*caller_reg = BPF_REG_3;
		*callee_reg = BPF_REG_4;
		break;
	case BPF_FUNC_find_vma:
		/* find_vma(task, addr, cb, ctx, flags): cb=R3, R4->cb R3 */
		/* 中文：R3 是回调函数，调用者 R4 的 ctx 进入回调 R3；随后返回统一校验。 */
		cb_reg = BPF_REG_3;
		*caller_reg = BPF_REG_4;
		*callee_reg = BPF_REG_3;
		break;
	case BPF_FUNC_user_ringbuf_drain:
		/* user_ringbuf_drain(map, cb, ctx, flags): cb=R2, R3->cb R2 */
		/* 中文：R2 是回调函数，调用者 R3 的 ctx 进入回调 R2；随后返回统一校验。 */
		cb_reg = BPF_REG_2;
		*caller_reg = BPF_REG_3;
		*callee_reg = BPF_REG_2;
		break;
	default:
		/* 其他 helper 没有本文件已知的参数穿透契约，直接报告不支持。 */
		return -1;
	}

	/* 回调寄存器必须已解析成恒定伪函数；否则无法选择递归目标。 */
	if (!(aux->const_reg_subprog_mask & BIT(cb_reg)))
		return -2;

	return aux->const_reg_vals[cb_reg];
}

/* Per-subprog intermediate state kept alive across analysis phases */
/*
 * 中文：每个子程序跨分析阶段保留的中间结果。compute_subprog_args() 分配并把 at_in
 * 所有权交给它，analyze_subprog() 的调用者最终释放；len 给出局部指令数和第一维边界。
 * 该对象只由验证线程访问，无锁/RCU；at_in 每项是指令入口的寄存器及栈参数来源快照。
 */
struct subprog_at_info {
	struct arg_track (*at_in)[MAX_AT_TRACK_REGS];
	int len;
};

/*
 * 业务背景：二级日志把收敛后的指令与其 FP 派生寄存器、参数槽和 spill 槽并排输出。
 * 入参：env 为日志环境；subprog 是合法编号；info 借用入口状态；at_stack_in 借用本地栈入口快照。
 * 出参/返回：无直接返回值；LEVEL2 关闭时无副作用，否则追加诊断日志。
 * 注意事项：不修改/释放输入；ldimm64 两槽只打印一次；共享日志缓冲且不睡眠。
 */
static void print_subprog_arg_access(struct bpf_verifier_env *env,
				     int subprog,
				     struct subprog_at_info *info,
				     struct arg_track (*at_stack_in)[MAX_ARG_SPILL_SLOTS])
{
	struct bpf_insn *insns = env->prog->insnsi;
	int start = env->subprog_info[subprog].start;
	int len = info->len;
	int i, r;

	/* 非详细日志快速返回；随后按子程序源码顺序扫描。 */
	if (!(env->log.level & BPF_LOG_LEVEL2))
		return;

	verbose(env, "%s:\n", fmt_subprog(env, subprog));
	for (i = 0; i < len; i++) {
		int idx = start + i;
		bool has_extra = false;
		u8 cls = BPF_CLASS(insns[idx].code);
		bool is_ldx_stx_call = cls == BPF_LDX || cls == BPF_STX ||
				       insns[idx].code == (BPF_JMP | BPF_CALL);

		/* 每条指令先输出基础反汇编，只有 FP 状态存在时才追加扩展段。 */
		verbose(env, "%3d: ", idx);
		bpf_verbose_insn(env, &insns[idx]);

		/* Collect what needs printing */
		/* 中文：先探测该指令是否有值得附加的 FP 来源，避免输出空的 `//`。 */
		if (is_ldx_stx_call &&
		    arg_is_visited(&info->at_in[i][0])) {
			for (r = 0; r < MAX_BPF_REG - 1; r++)
				if (arg_is_fp(&info->at_in[i][r]))
					has_extra = true;
			/* 栈参数位于扩展数组尾部，需独立扫描。 */
			for (r = 0; r < MAX_STACK_ARG_SLOTS; r++)
				if (arg_is_fp(&info->at_in[i][MAX_BPF_REG + r]))
					has_extra = true;
		}
		if (is_ldx_stx_call) {
			for (r = 0; r < MAX_ARG_SPILL_SLOTS; r++)
				if (arg_is_fp(&at_stack_in[i][r]))
					has_extra = true;
		}

		/* 无额外状态时保留普通反汇编；ldimm64 仍须跳过紧随的第二槽。 */
		if (!has_extra) {
			if (bpf_is_ldimm64(&insns[idx]))
				i++;
			continue;
		}

		/* 有状态时撤销反汇编换行，依次拼接寄存器、参数槽和本地 spill 槽。 */
		bpf_vlog_reset(&env->log, env->log.end_pos - 1);
		verbose(env, " //");

		if (is_ldx_stx_call && info->at_in &&
		    arg_is_visited(&info->at_in[i][0])) {
			/* 先输出所有 FP 派生普通寄存器，再输出传出参数槽。 */
			for (r = 0; r < MAX_BPF_REG - 1; r++) {
				if (!arg_is_fp(&info->at_in[i][r]))
					continue;
				verbose(env, " r%d=", r);
				verbose_arg_track(env, &info->at_in[i][r]);
			}
			/* saN 与寄存器使用同一入口快照，但拥有独立编号域。 */
			for (r = 0; r < MAX_STACK_ARG_SLOTS; r++) {
				if (!arg_is_fp(&info->at_in[i][MAX_BPF_REG + r]))
					continue;
				verbose(env, " sa%d=", r);
				verbose_arg_track(env, &info->at_in[i][MAX_BPF_REG + r]);
			}
		}

		if (is_ldx_stx_call) {
			/* 最后附加当前帧所有仍保存 FP 身份的 spill 槽。 */
			for (r = 0; r < MAX_ARG_SPILL_SLOTS; r++) {
				if (!arg_is_fp(&at_stack_in[i][r]))
					continue;
				verbose(env, " fp%+d=", -(r + 1) * 8);
				/* 输出该槽在指令入口持有的来源格值。 */
				verbose_arg_track(env, &at_stack_in[i][r]);
			}
		}

		verbose(env, "\n");
		if (bpf_is_ldimm64(&insns[idx]))
			i++;
	}
}

/*
 * Compute arg tracking dataflow for a single subprog.
 * Runs forward fixed-point with arg_track_xfer(), then records
 * memory accesses in a single linear pass over converged state.
 *
 * @callee_entry: pre-populated entry state for R1-R5 and stack args
 *                NULL for main (subprog 0).
 * @info:         stores at_in, len for debug printing.
 */
/*
 * 中文：为单个子程序计算参数来源数据流；先做前向固定点，再用收敛入口状态登记内存访问。
 * callee_entry 预装 R1..R5 与栈参数，主程序传 NULL；info 保存 at_in 和长度供诊断。
 * 业务背景：同一子程序在不同调用深度/实参下会得到不同 FP 来源，必须按实例重新求解。
 * 入参：env 为输入输出环境；info 为输出容器；callee_entry 为可空只读入口；instance 为输入输出实例；
 * callsites 是各深度调用点数组，所有借用对象在调用期间稳定。
 * 出参/返回：成功返回 0 并转移 at_in 给 info、保存调用点栈快照；失败返回 -ENOMEM/下游 errno。
 * 注意事项：GFP_KERNEL_ACCOUNT 可睡眠；临时数组统一释放，环境中的旧调用点快照可被替换。
 */
static int compute_subprog_args(struct bpf_verifier_env *env,
				struct subprog_at_info *info,
				struct arg_track *callee_entry,
				struct func_instance *instance,
				u32 *callsites)
{
	/* 子程序范围与其后序切片均采用半开区间，len 是局部矩阵第一维。 */
	int subprog = instance->subprog;
	struct bpf_insn *insns = env->prog->insnsi;
	int depth = instance->depth;
	int start = env->subprog_info[subprog].start;
	int po_start = env->subprog_info[subprog].postorder_start;
	int end = env->subprog_info[subprog + 1].start;
	int po_end = env->subprog_info[subprog + 1].postorder_start;
	int len = end - start;
	/* 以下矩阵指针分别表示逐指令寄存器入口、本地栈入口和复用出口。 */
	struct arg_track (*at_in)[MAX_AT_TRACK_REGS] = NULL;
	struct arg_track at_out[MAX_AT_TRACK_REGS];
	struct arg_track (*at_stack_in)[MAX_ARG_SPILL_SLOTS] = NULL;
	struct arg_track *at_stack_out = NULL;
	struct arg_track at_stack_arg_entry[MAX_STACK_ARG_SLOTS];
	struct arg_track unvisited = { .frame = ARG_UNVISITED };
	struct arg_track none = { .frame = ARG_NONE };
	bool changed;
	int i, p, r, err = -ENOMEM;

	/*
	 * 变量地图：at_in/at_stack_in 是逐指令入口矩阵；at_out/at_stack_out 是单指令
	 * 可复用出口；at_stack_arg_entry 冻结调用入口参数；p/i/r 分别遍历后序、局部指令和元素。
	 */
	/* 阶段一分配寄存器入口矩阵、spill 入口矩阵与可复用出口缓冲；失败统一逆序释放。 */
	at_in = kvmalloc_objs(*at_in, len, GFP_KERNEL_ACCOUNT);
	if (!at_in)
		goto err_free;

	at_stack_in = kvmalloc_objs(*at_stack_in, len, GFP_KERNEL_ACCOUNT);
	if (!at_stack_in)
		goto err_free;

	/* 出口 spill 缓冲只需一行，随每条指令从入口覆盖。 */
	at_stack_out = kvmalloc_objs(*at_stack_out, MAX_ARG_SPILL_SLOTS, GFP_KERNEL_ACCOUNT);
	if (!at_stack_out)
		goto err_free;

	/* 全矩阵先置 UNVISITED，区别“尚无前驱”与实际运行时的 ARG_NONE。 */
	for (i = 0; i < len; i++) {
		for (r = 0; r < MAX_AT_TRACK_REGS; r++)
			at_in[i][r] = unvisited;
		for (r = 0; r < MAX_ARG_SPILL_SLOTS; r++)
			at_stack_in[i][r] = unvisited;
	}

	/* 阶段二建立入口：普通值为 NONE，R10 精确指向本帧，实参与参数槽继承调用者。 */
	for (r = 0; r < MAX_AT_TRACK_REGS; r++)
		at_in[0][r] = none;

	/* Entry: R10 is always precisely the current frame's FP */
	/* 中文：R10 在每层入口恒等于当前绝对 depth 的 fp+0。 */
	at_in[0][BPF_REG_FP] = arg_single(depth, 0);

	/* R1-R5: from caller or ARG_NONE for main */
	/* 中文：被调者继承调用点 R1..R5；主程序没有调用者，保持 NONE。 */
	if (callee_entry) {
		for (r = BPF_REG_1; r <= BPF_REG_5; r++)
			at_in[0][r] = callee_entry[r];
	}

	/* Entry: all stack slots are ARG_NONE */
	/* 中文：本地 spill 槽在子程序入口没有可继承的本帧指针身份。 */
	for (r = 0; r < MAX_ARG_SPILL_SLOTS; r++)
		at_stack_in[0][r] = none;

	/* Entry: incoming stack args from caller, or ARG_NONE for main */
	/* 中文：传入栈参数单独冻结为入口快照，供专用 load 指令读取。 */
	for (r = 0; r < MAX_STACK_ARG_SLOTS; r++)
		at_stack_arg_entry[r] = callee_entry ? callee_entry[MAX_BPF_REG + r] : none;

	if (env->log.level & BPF_LOG_LEVEL2)
		verbose(env, "subprog#%d: analyzing (depth %d)...\n", subprog, depth);

	/* Forward fixed-point iteration in reverse post order */
	/* 中文：阶段三按逆后序前向传播；格值只扩大可能集合，有限高度保证收敛。 */
redo:
	changed = false;
	for (p = po_end - 1; p >= po_start; p--) {
		int idx = env->cfg.insn_postorder[p];
		int i = idx - start;
		struct bpf_insn *insn = &insns[idx];
		struct bpf_iarray *succ;

		/* CFG 后序可能含入口尚未到达的点，用两个代表项共同判定并跳过。 */
		if (!arg_is_visited(&at_in[i][0]) && !arg_is_visited(&at_in[i][1]))
			continue;

		/* 从入口复制临时出口，应用单指令传递并按需输出差异。 */
		memcpy(at_out, at_in[i], sizeof(at_out));
		memcpy(at_stack_out, at_stack_in[i], MAX_ARG_SPILL_SLOTS * sizeof(*at_stack_out));

		arg_track_xfer(env, insn, idx, at_out, at_stack_out,
			       at_stack_arg_entry, instance, callsites);
		arg_track_log(env, insn, idx, at_in[i], at_stack_in[i], at_out, at_stack_out);

		/* Propagate to successors within this subprogram */
		/* 中文：只向本子程序范围内的直接后继汇合；调用边由递归实例阶段处理。 */
		succ = bpf_insn_successors(env, idx);
		for (int s = 0; s < succ->cnt; s++) {
			int target = succ->items[s];
			int ti;

			/* Filter: stay within the subprogram's range */
			/* 中文：过滤退出/跨子程序后继，避免用错误局部下标传播。 */
			if (target < start || target >= end)
				continue;
			ti = target - start;

			/* 寄存器/参数槽与本地 spill 槽分别在各自数组域逐项 join。 */
			for (r = 0; r < MAX_AT_TRACK_REGS; r++)
				changed |= arg_track_join(env, idx, target, r,
							  &at_in[ti][r], at_out[r]);

			for (r = 0; r < MAX_ARG_SPILL_SLOTS; r++)
				changed |= arg_track_join(env, idx, target, -r - 1,
							  &at_stack_in[ti][r], at_stack_out[r]);
		}
	}
	/* 任一入口格值扩大就重跑整轮；完全不变即达到固定点。 */
	if (changed)
		goto redo;

	/* Record memory accesses using converged at_in (RPO skips dead code) */
	/* 中文：阶段四仅遍历 CFG 可达指令，用收敛入口登记访存及调用参数的 use/def。 */
	for (p = po_end - 1; p >= po_start; p--) {
		int idx = env->cfg.insn_postorder[p];
		int i = idx - start;
		struct bpf_insn *insn = &insns[idx];

		err = record_load_store_access(env, instance, at_in[i], idx);
		if (err)
			goto err_free;

		/* 只有 CALL 指令需要追加 helper/kfunc 参数访问登记。 */
		if (insn->code == (BPF_JMP | BPF_CALL)) {
			err = record_call_access(env, instance, at_in[i], idx);
			if (err)
				goto err_free;
		}

		/* 递归分析需要调用前 spill 快照；替换同一调用点旧副本后发布新副本。 */
		if (bpf_pseudo_call(insn) || bpf_calls_callback(env, idx)) {
			kvfree(env->callsite_at_stack[idx]);
			env->callsite_at_stack[idx] =
				kvmalloc_objs(*env->callsite_at_stack[idx],
					      MAX_ARG_SPILL_SLOTS, GFP_KERNEL_ACCOUNT);
			if (!env->callsite_at_stack[idx]) {
				err = -ENOMEM;
				goto err_free;
			}
			/* 分配成功后复制完整入口 spill 快照，供下一层解析父帧 load。 */
			memcpy(env->callsite_at_stack[idx],
			       at_stack_in[i], sizeof(struct arg_track) * MAX_ARG_SPILL_SLOTS);
		}
	}

	/* 阶段五把寄存器入口矩阵 ownership 转给 info；其余数组打印后释放。 */
	info->at_in = at_in;
	at_in = NULL;
	info->len = len;
	print_subprog_arg_access(env, subprog, info, at_stack_in);
	err = 0;

err_free:
	/* 成功时 at_in 已置 NULL；失败时三类临时资源均在此对称回收。 */
	kvfree(at_stack_out);
	kvfree(at_stack_in);
	kvfree(at_in);
	return err;
}

/* Return true if any of R1-R5 or stack args is derived from a frame pointer. */
/*
 * 中文：检查 R1..R5 或任一栈参数是否可能派生自帧指针。
 * 业务背景：没有 FP 来源的被调者不可能访问祖先栈，可跳过递归实例分析。
 * 入参：args 是至少含 MAX_AT_TRACK_REGS 项的只读借用入口数组。
 * 出参/返回：发现任一精确/不精确 FP 来源返回 true，否则 false；无副作用。
 * 注意事项：不检查 R0/R6..R10，因为它们不是调用 ABI 输入；不分配、不睡眠。
 */
static bool has_fp_args(struct arg_track *args)
{
	/* 先扫寄存器 ABI，再扫 R5 之后的扩展栈参数域。 */
	for (int r = BPF_REG_1; r <= BPF_REG_5; r++)
		if (arg_is_fp(&args[r]))
			return true;
	for (int r = 0; r < MAX_STACK_ARG_SLOTS; r++)
		if (arg_is_fp(&args[MAX_BPF_REG + r]))
			return true;
	return false;
}

/*
 * Merge a freshly analyzed instance into the original.
 * may_read: union (any pass might read the slot).
 * must_write: intersection (only slots written on ALL passes are guaranteed).
 * live_before is recomputed by a subsequent update_instance() on @dst.
 */
/*
 * 中文：把新一轮分析实例合入原实例：may_read 取并集，must_write 取交集，
 * live_before 稍后由 update_instance() 在 dst 上重算。
 * 业务背景：同一调用实例可因不同入口实参重复分析，只有所有轮次都写的槽才能安全 kill。
 * 入参：dst 为持久输入输出实例；src 为临时输入输出实例，二者元数据和深度相同。
 * 出参/返回：无直接返回值；可能把 src->frames[f] ownership 转给 dst 并置 NULL。
 * 注意事项：调用者随后释放 src；验证线程独占二者，不分配、不睡眠。
 */
static void merge_instances(struct func_instance *dst, struct func_instance *src)
{
	int f, i;

	/* 逐帧覆盖三种存在性组合，再逐指令应用 use 并/def 交。 */
	for (f = 0; f <= dst->depth; f++) {
		if (!src->frames[f]) {
			/* This pass didn't touch frame f — must_write intersects with empty. */
			/* 中文：本轮未触及该帧，必写集合与空帧空集相交后必须清零。 */
			if (dst->frames[f])
				for (i = 0; i < dst->insn_cnt; i++)
					dst->frames[f][i].must_write = SPIS_ZERO;
			continue;
		}
		if (!dst->frames[f]) {
			/* Previous pass didn't touch frame f — take src, zero must_write. */
			/* 中文：旧轮未触及该帧；接管新数组以保留 may_read，但跨轮必写仍为空。 */
			dst->frames[f] = src->frames[f];
			src->frames[f] = NULL;
			for (i = 0; i < dst->insn_cnt; i++)
				dst->frames[f][i].must_write = SPIS_ZERO;
			continue;
		}
		/* 两轮都有数组时，任一轮可能读即读，只有两轮都写才是必写。 */
		for (i = 0; i < dst->insn_cnt; i++) {
			dst->frames[f][i].may_read =
				spis_or(dst->frames[f][i].may_read,
					src->frames[f][i].may_read);
			dst->frames[f][i].must_write =
				spis_and(dst->frames[f][i].must_write,
					 src->frames[f][i].must_write);
		}
	}
}

/*
 * 业务背景：重复分析前复制实例身份，但使用全空帧数组隔离本轮 use/def。
 * 入参：src 为只读借用的已发布实例。
 * 出参/返回：成功返回调用者持有的新实例；分配失败返回 ERR_PTR(-ENOMEM)。
 * 注意事项：只复制键和子程序范围，不复制 frames/hash 节点；GFP_KERNEL_ACCOUNT 可睡眠。
 */
static struct func_instance *fresh_instance(struct func_instance *src)
{
	struct func_instance *f;

	/* 零初始化保证各 frames 为 NULL、must_write_initialized 为 false。 */
	f = kvzalloc_obj(*f, GFP_KERNEL_ACCOUNT);
	if (!f)
		return ERR_PTR(-ENOMEM);
	f->callsite = src->callsite;
	f->depth = src->depth;
	f->subprog = src->subprog;
	f->subprog_start = src->subprog_start;
	f->insn_cnt = src->insn_cnt;
	return f;
}

/*
 * 业务背景：回收未发布的临时实例及其逐帧掩码数组。
 * 入参：instance 是调用者独占且非 NULL 的持有指针，depth/frames 已初始化。
 * 出参/返回：无直接返回值；释放全部帧数组和容器，旧指针立即失效。
 * 注意事项：不能用于仍在哈希表中的持久实例；kvfree 可处理两种分配后端并可能睡眠。
 */
static void free_instance(struct func_instance *instance)
{
	int i;

	/* frames 的有效范围是 0..depth，容器必须最后释放。 */
	for (i = 0; i <= instance->depth; i++)
		kvfree(instance->frames[i]);
	kvfree(instance);
}

/*
 * Recursively analyze a subprog with specific 'entry_args'.
 * Each callee is analyzed with the exact args from its call site.
 *
 * Args are recomputed for each call because the dataflow result at_in[]
 * depends on the entry args and frame depth. Consider: A->C->D and B->C->D
 * Callsites in A and B pass different args into C, so C is recomputed.
 * Then within C the same callsite passes different args into D.
 */
/*
 * 中文：用具体 entry_args 递归分析子程序及其可达被调者。每个调用点使用精确实参；
 * A->C->D 与 B->C->D 会令 C 乃至 C 内同一调用点的 D 重新计算。
 * 业务背景：建立按调用深度/调用点区分的祖先栈访问实例，并把被调者入口活性回灌调用点。
 * 入参：env 为输入输出环境；entry_args 为可空只读入口；info 是各子程序中间结果数组；
 * instance 为持久或临时输入输出实例；callsites 为递归路径输入输出数组。
 * 出参/返回：成功返回 0 并更新实例固定点；复杂度/深度/分配失败返回对应 errno。
 * 注意事项：可 cond_resched，故能睡眠；临时实例失败时释放，持久实例由 liveness 统一回收。
 */
static int analyze_subprog(struct bpf_verifier_env *env,
			   struct arg_track *entry_args,
			   struct subprog_at_info *info,
			   struct func_instance *instance,
			   u32 *callsites)
{
	/* 本层只借用递归参数；subprog/depth/范围均从 instance 与 CFG 缓存恢复。 */
	int subprog = instance->subprog;
	int depth = instance->depth;
	struct bpf_insn *insns = env->prog->insnsi;
	int start = env->subprog_info[subprog].start;
	int po_start = env->subprog_info[subprog].postorder_start;
	int po_end = env->subprog_info[subprog + 1].postorder_start;
	/* prev_instance 区分持久目标与本轮 fresh；j/err 保存局部下标和失败码。 */
	struct func_instance *prev_instance = NULL;
	int j, err;

	/* 阶段一限制跨实例递归工作量，必要时主动让出 CPU，避免验证长期垄断。 */
	if (++env->liveness->subprog_calls > 10000) {
		verbose(env, "liveness analysis exceeded complexity limit (%d calls)\n",
			env->liveness->subprog_calls);
		return -E2BIG;
	}

	if (need_resched())
		cond_resched();


	/*
	 * When an instance is reused (must_write_initialized == true),
	 * record into a fresh instance and merge afterward.  This avoids
	 * stale must_write marks for instructions not reached in this pass.
	 */
	/*
	 * 中文：若持久实例已有必写结果，本轮先写入全新实例再合并，避免未到达指令
	 * 沿用旧 must_write。prev_instance 保留最终合并目标的借用指针。
	 */
	if (instance->must_write_initialized) {
		struct func_instance *fresh = fresh_instance(instance);

		if (IS_ERR(fresh))
			return PTR_ERR(fresh);
		prev_instance = instance;
		instance = fresh;
	}

	/* Free prior analysis if this subprog was already visited */
	/* 中文：阶段二释放该子程序上一次诊断入口矩阵，再以本次实参重算并接管新矩阵。 */
	kvfree(info[subprog].at_in);
	info[subprog].at_in = NULL;

	err = compute_subprog_args(env, &info[subprog], entry_args, instance, callsites);
	if (err)
		goto out_free;

	/* For each reachable call site in the subprog, recurse into callees */
	/* 中文：阶段三遍历 CFG 可达调用点，构造被调入口并递归；非调用指令直接跳过。 */
	for (int p = po_start; p < po_end; p++) {
		int idx = env->cfg.insn_postorder[p];
		struct arg_track callee_args[MAX_AT_TRACK_REGS] = {};
		struct arg_track none = { .frame = ARG_NONE };
		struct bpf_insn *insn = &insns[idx];
		struct func_instance *callee_instance;
		int callee, target;
		int caller_reg, cb_callee_reg;

		j = idx - start; /* relative index within this subprog */
		/* 中文：j 是当前绝对指令号在本子程序 at_in 矩阵中的局部行号。 */

		if (bpf_pseudo_call(insn)) {
			/* 普通子程序调用继承调用点 R1..R5 及全部栈参数来源。 */
			target = idx + insn->imm + 1;
			callee = bpf_find_subprog(env, target);
			if (callee < 0)
				continue;

			/* Build entry args: R1-R5 and stack args from at_in at call site */
			/* 中文：只复制调用 ABI 输入，callee 自身 R10 等状态由其入口初始化。 */
			for (int r = BPF_REG_1; r <= BPF_REG_5; r++)
				callee_args[r] = info[subprog].at_in[j][r];
			for (int r = 0; r < MAX_STACK_ARG_SLOTS; r++)
				callee_args[MAX_BPF_REG + r] = info[subprog].at_in[j][MAX_BPF_REG + r];
		} else if (bpf_calls_callback(env, idx)) {
			/* callback 仅按 helper ABI 映射穿透的 ctx 参数，其余入口显式置 NONE。 */
			callee = find_callback_subprog(env, insn, idx, &caller_reg, &cb_callee_reg);
			if (callee == -2) {
				/*
				 * same bpf_loop() calls two different callbacks and passes
				 * stack pointer to them
				 */
				/* 中文：同一 bpf_loop 可能调用两个不同回调并传栈指针；目标不唯一时将所有祖先帧全读。 */
				if (info[subprog].at_in[j][caller_reg].frame == ARG_NONE)
					continue;
				for (int f = 0; f <= depth; f++) {
					err = mark_stack_read(instance, f, idx, SPIS_ALL);
					if (err)
						goto out_free;
				}
				/* 已完成未知 callback 的保守登记，无需继续创建具体 callee。 */
				continue;
			}
			if (callee < 0)
				continue;

			for (int r = BPF_REG_1; r <= BPF_REG_5; r++)
				callee_args[r] = none;
			for (int r = 0; r < MAX_STACK_ARG_SLOTS; r++)
				callee_args[MAX_BPF_REG + r] = none;
			/* 唯一穿透参数从调用者寄存器复制到 helper ABI 指定的回调寄存器。 */
			callee_args[cb_callee_reg] = info[subprog].at_in[j][caller_reg];
		} else {
			continue;
		}

		/* 无 FP 实参不可能触及祖先栈；有来源时还需先守住最大调用深度。 */
		if (!has_fp_args(callee_args))
			continue;

		if (depth == MAX_CALL_FRAMES - 1) {
			err = -EINVAL;
			goto out_free;
		}

		/* 取得/创建以调用点和深度为键的实例，记录路径后递归求解。 */
		callee_instance = call_instance(env, instance, idx, callee);
		if (IS_ERR(callee_instance)) {
			err = PTR_ERR(callee_instance);
			goto out_free;
		}
		callsites[depth] = idx;
		err = analyze_subprog(env, callee_args, info, callee_instance, callsites);
		/* 递归返回后 callee 实例已完成固定点，可安全读取其入口 live_before。 */
		if (err)
			goto out_free;

		/* Pull callee's entry liveness back to caller's callsite */
		/* 中文：阶段四把被调者入口对各祖先帧的 live_before 合入调用者调用点 may_read。 */
		{
			u32 callee_start = callee_instance->subprog_start;
			struct per_frame_masks *entry;

			for (int f = 0; f < callee_instance->depth; f++) {
				entry = get_frame_masks(callee_instance, f, callee_start);
				if (!entry)
					continue;
				/* callee 的 frame f 对应 caller 同编号祖先帧，直接回灌其入口需求。 */
				err = mark_stack_read(instance, f, idx, entry->live_before);
				if (err)
					goto out_free;
			}
		}
	}

	/* 阶段五合并重复轮次、释放临时实例，再反向计算最终 live_before。 */
	if (prev_instance) {
		merge_instances(prev_instance, instance);
		free_instance(instance);
		instance = prev_instance;
	}
	update_instance(env, instance);
	return 0;

out_free:
	/* 仅临时 fresh 由本层持有；持久哈希实例即使失败也留给顶层统一释放。 */
	if (prev_instance)
		free_instance(instance);
	return err;
}

/*
 * 业务背景：寄存器活性计算前，驱动所有子程序实例的 FP 来源和祖先栈 use/def 分析。
 * 入参：env 为输入输出验证环境，CFG/拓扑/后序已建立，liveness 已初始化。
 * 出参/返回：成功返回 0 并在持久实例中留下掩码；分配、复杂度或递归分析失败返回 errno。
 * 注意事项：GFP_KERNEL_ACCOUNT 可睡眠；callsite_at_stack 与 info 始终在本函数退出前释放，
 * 哈希实例继续由 env->liveness 持有，最终由 bpf_stack_liveness_free() 回收。
 */
int bpf_compute_subprog_arg_access(struct bpf_verifier_env *env)
{
	u32 callsites[MAX_CALL_FRAMES] = {};
	int insn_cnt = env->prog->len;
	struct func_instance *instance;
	struct subprog_at_info *info;
	int k, err = 0;

	/* 阶段一分配逐子程序诊断结果和逐指令调用点 spill 快照指针表。 */
	info = kvzalloc_objs(*info, env->subprog_cnt, GFP_KERNEL_ACCOUNT);
	if (!info)
		return -ENOMEM;

	env->callsite_at_stack = kvzalloc_objs(*env->callsite_at_stack, insn_cnt,
					       GFP_KERNEL_ACCOUNT);
	if (!env->callsite_at_stack) {
		kvfree(info);
		return -ENOMEM;
	}

	/*
	 * Analyze every subprog in reverse topological order (callers
	 * before callees) so that each subprog is analyzed before its
	 * callees, allowing the recursive walk inside analyze_subprog()
	 * to naturally reach callees that receive FP-derived args.
	 *
	 * Subprogs and callbacks that don't receive FP-derived arguments
	 * cannot access ancestor stack frames are analyzed independently.
	 * Async callbacks (timer, workqueue) are handled the same way.
	 */
	/*
	 * 中文：阶段二按逆拓扑顺序令调用者先于被调者作为根分析，递归过程自然触达
	 * 接收 FP 实参的 callee。未接收 FP 的普通/异步 callback 不可能访问祖先栈，
	 * 因而各自独立分析；局部子程序若已由调用链覆盖可跳过，global 仍需独立入口。
	 */
	for (k = env->subprog_cnt - 1; k >= 0; k--) {
		int sub = env->subprog_topo_order[k];

		/* 非 global 子程序已有具体调用实例时无需再用空实参重复求解。 */
		if (info[sub].at_in && !bpf_subprog_is_global(env, sub))
			continue;
		instance = call_instance(env, NULL, 0, sub);
		if (IS_ERR(instance)) {
			err = PTR_ERR(instance);
			goto out;
		}
		err = analyze_subprog(env, NULL, info, instance, callsites);
		/* 任一根分析失败即停止，统一出口仍释放所有临时快照。 */
		if (err)
			goto out;
	}

	/* 所有持久实例已稳定后，以确定顺序输出 use/def 诊断。 */
	if (env->log.level & BPF_LOG_LEVEL2)
		err = print_instances(env);

out:
	/* 阶段三无论成功失败都释放仅供递归传参/诊断的临时矩阵，并撤销环境指针。 */
	for (k = 0; k < insn_cnt; k++)
		kvfree(env->callsite_at_stack[k]);
	kvfree(env->callsite_at_stack);
	env->callsite_at_stack = NULL;
	for (k = 0; k < env->subprog_cnt; k++)
		kvfree(info[k].at_in);
	kvfree(info);
	return err;
}

/* Each field is a register bitmask */
/*
 * 中文：单条指令的寄存器活性四元组，每个 u16 都以寄存器号为位号。
 * use/def 是由指令语义计算的读/写集合；in/out 是固定点得到的执行前/后可能存活集合。
 * 数组由 bpf_compute_live_registers() 创建、独占并在发布 in 后销毁，不跨验证线程共享。
 */
struct insn_live_regs {
	u16 use;	/* registers read by instruction */
	/* 中文：use 是本指令读取集合，也是反向传递的 gen 集。 */
	u16 def;	/* registers written by instruction */
	/* 中文：def 是本指令覆盖集合，也是反向传递的 kill 集。 */
	u16 in;		/* registers that may be alive before instruction */
	/* 中文：in 是指令执行前可能仍需旧值的寄存器集合。 */
	u16 out;	/* registers that may be alive after instruction */
	/* 中文：out 是任一直接后继入口 in 的并集。 */
};

/* Bitmask with 1s for all caller saved registers */
/* 中文：低 CALLER_SAVED_REGS 位全 1，表示一次调用会覆盖 ABI 规定的全部易失寄存器。 */
#define ALL_CALLER_SAVED_REGS ((1u << CALLER_SAVED_REGS) - 1)

/* Compute info->{use,def} fields for the instruction */
/*
 * 中文：按一条 BPF 指令的类别、模式和操作码计算 info 的 use/def 字段。
 * 业务背景：把 ISA 读写语义规范化为反向寄存器活性分析的 gen/kill 位集。
 * 入参：env 为只读调用摘要环境；insn 为只读指令；info 为调用者所有的输出对象。
 * 出参/返回：无直接返回值；覆盖 info->use/def，不触碰 in/out 或转移 ownership。
 * 注意事项：未知/未细分编码默认 use=0xffff、def=0，采取保守全读；不分配、不睡眠。
 */
static void compute_insn_live_regs(struct bpf_verifier_env *env,
				   struct bpf_insn *insn,
				   struct insn_live_regs *info)
{
	struct bpf_call_summary cs;
	u8 class = BPF_CLASS(insn->code);
	u8 code = BPF_OP(insn->code);
	u8 mode = BPF_MODE(insn->code);
	/* src/dst/r0 是寄存器位，def/use 从保守默认逐 case 收紧。 */
	u16 src = BIT(insn->src_reg);
	u16 dst = BIT(insn->dst_reg);
	u16 r0  = BIT(0);
	u16 def = 0;
	u16 use = 0xffff;

	/* 外层按 ISA 类别分派，每个已知 case 都明确覆盖其真正读取和写回的寄存器。 */
	switch (class) {
	case BPF_LD:
		/* LD IMM64 定义 dst 且不读寄存器；传统 ABS/IND 保持保守默认。 */
		switch (mode) {
		case BPF_IMM:
			if (BPF_SIZE(insn->code) == BPF_DW) {
				def = dst;
				use = 0;
			}
			break;
		case BPF_LD | BPF_ABS:
		case BPF_LD | BPF_IND:
			/* stick with defaults */
			/* 中文：ABS/IND 的隐式上下文读取未在此展开，保留全读/无写默认值。 */
			break;
		}
		break;
	case BPF_LDX:
		/* 普通/符号扩展 load 读取 src 基址并定义 dst。 */
		switch (mode) {
		case BPF_MEM:
		case BPF_MEMSX:
			def = dst;
			use = src;
			break;
		}
		break;
	case BPF_ST:
		/* 立即数 store 只读取 dst 基址，不定义 BPF 寄存器。 */
		switch (mode) {
		case BPF_MEM:
			def = 0;
			use = dst;
			break;
		}
		break;
	case BPF_STX:
		/* 普通 STX 同时读取地址 dst 和值 src；原子模式还可能隐式读写寄存器。 */
		switch (mode) {
		case BPF_MEM:
			def = 0;
			use = dst | src;
			break;
		case BPF_ATOMIC:
			/* cmpxchg 读 R0/dst/src 并把旧值写 R0；acquire/release/fetch 按 ABI 分派。 */
			switch (insn->imm) {
			case BPF_CMPXCHG:
				/* 比较值来自 R0，地址和值来自 dst/src，结果旧值覆盖 R0。 */
				use = r0 | dst | src;
				def = r0;
				break;
			case BPF_LOAD_ACQ:
				/* acquire 从 src 地址加载并定义 dst。 */
				def = dst;
				use = src;
				break;
			case BPF_STORE_REL:
				/* release store 读取 dst 地址与 src 值，不写寄存器。 */
				def = 0;
				use = dst | src;
				break;
			default:
				/* 其他 RMW 都读地址和值；FETCH 形式另把旧内存值写回 src。 */
				use = dst | src;
				if (insn->imm & BPF_FETCH)
					def = src;
				else
					def = 0;
				/* 非 FETCH RMW 只改内存，FETCH 才把旧值写回 src。 */
			}
			break;
		}
		break;
	case BPF_ALU:
	case BPF_ALU64:
		/* ALU 通常定义 dst；END 原地读写，MOV 可不读旧 dst，其余还读取操作数。 */
		switch (code) {
		case BPF_END:
			/* 字节序转换原地读取并覆盖 dst。 */
			use = dst;
			def = dst;
			break;
		case BPF_MOV:
			/* 常量 MOV 无寄存器输入，寄存器 MOV 只读 src。 */
			def = dst;
			if (BPF_SRC(insn->code) == BPF_K)
				use = 0;
			else
				use = src;
			break;
		default:
			/* 常量二元运算读旧 dst；寄存器形式还读取 src。 */
			def = dst;
			if (BPF_SRC(insn->code) == BPF_K)
				use = dst;
			else
				use = dst | src;
		}
		break;
	case BPF_JMP:
	case BPF_JMP32:
		/* 跳转不写普通寄存器；条件形式读取比较操作数，调用/退出遵循 ABI。 */
		switch (code) {
		case BPF_JA:
			/* 直接跳转通常不读寄存器；gotox 的 X 编码把 dst 作为动态选择输入。 */
			def = 0;
			if (BPF_SRC(insn->code) == BPF_X)
				use = dst;
			else
				use = 0;
			break;
		case BPF_JCOND:
			/* 特殊内部条件跳转的寄存器依赖由其语义保证为空。 */
			def = 0;
			use = 0;
			break;
		case BPF_EXIT:
			/* 返回路径读取 R0 作为程序/子程序返回值。 */
			def = 0;
			use = r0;
			break;
		case BPF_CALL:
			/* 调用覆盖全部易失寄存器，默认读取 R1..R5；摘要可缩到实际参数数。 */
			def = ALL_CALLER_SAVED_REGS;
			use = def & ~BIT(BPF_REG_0);
			if (bpf_get_call_summary(env, insn, &cs))
				use = GENMASK(min_t(u8, cs.num_params, MAX_BPF_FUNC_REG_ARGS), 1);
			break;
		default:
			/* 普通条件跳转读 dst，寄存器比较还读 src，且不定义寄存器。 */
			def = 0;
			if (BPF_SRC(insn->code) == BPF_K)
				use = dst;
			else
				use = dst | src;
		}
		break;
	}

	/* 所有分支汇合后一次提交结果，避免 info 暂时呈现半更新状态。 */
	info->def = def;
	info->use = use;
}

/* Compute may-live registers after each instruction in the program.
 * The register is live after the instruction I if it is read by some
 * instruction S following I during program execution and is not
 * overwritten between I and S.
 *
 * Store result in env->insn_aux_data[i].live_regs.
 */
/*
 * 中文：计算程序每条指令之前可能存活的寄存器。若 I 之后某条可执行指令 S 会读某寄存器，
 * 且 I 到 S 之间没有覆盖它，该寄存器便在 I 之后存活；结果实际写入
 * env->insn_aux_data[i].live_regs_before（原英文中的 live_regs 是概念旧称）。
 * 修正说明：当前结构体不存在 live_regs 字段，上述英文末句在本版本应读作 live_regs_before。
 * 业务背景：states.c 可据此忽略未来不会读取的寄存器状态，缩小安全状态比较面。
 * 入参：env 为输入输出验证环境，程序、aux、CFG 后序和 liveness 上下文均已准备好。
 * 出参/返回：成功返回 0 并发布每条指令的 live_regs_before；分配或子程序栈分析失败返回 errno。
 * 注意事项：GFP_KERNEL_ACCOUNT 与子分析可睡眠；临时 state 在所有出口释放，失败不发布部分 in。
 */
int bpf_compute_live_registers(struct bpf_verifier_env *env)
{
	struct bpf_insn_aux_data *insn_aux = env->insn_aux_data;
	struct bpf_insn *insns = env->prog->insnsi;
	struct insn_live_regs *state;
	int insn_cnt = env->prog->len;
	int err = 0, i, j;
	bool changed;

	/* Use the following algorithm:
	 * - define the following:
	 *   - I.use : a set of all registers read by instruction I;
	 *   - I.def : a set of all registers written by instruction I;
	 *   - I.in  : a set of all registers that may be alive before I execution;
	 *   - I.out : a set of all registers that may be alive after I execution;
	 *   - insn_successors(I): a set of instructions S that might immediately
	 *                         follow I for some program execution;
	 * - associate separate empty sets 'I.in' and 'I.out' with each instruction;
	 * - visit each instruction in a postorder and update
	 *   state[i].in, state[i].out as follows:
	 *
	 *       state[i].out = U [state[s].in for S in insn_successors(i)]
	 *       state[i].in  = (state[i].out / state[i].def) U state[i].use
	 *
	 *   (where U stands for set union, / stands for set difference)
	 * - repeat the computation while {in,out} fields changes for
	 *   any instruction.
	 */
	/*
	 * 中文：算法定义 I.use（本指令读）、I.def（本指令写）、I.in（执行前可能活）和
	 * I.out（执行后可能活），successors(I) 是所有可能的直接后继。各 in/out 从空集开始，
	 * 按后序反复计算 out=后继 in 并集，in=(out 去 def) 并 use，直至整轮不再变化。
	 */
	/* 阶段一分配每条指令的四元组，并计算不随固定点变化的 use/def。 */
	state = kvzalloc_objs(*state, insn_cnt, GFP_KERNEL_ACCOUNT);
	if (!state) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < insn_cnt; ++i)
		compute_insn_live_regs(env, &insns[i], &state[i]);

	/* Forward pass: resolve stack access through FP-derived pointers */
	/* 中文：阶段二先做前向 FP 来源分析，建立 states.c 同时需要的祖先栈活性。 */
	err = bpf_compute_subprog_arg_access(env);
	if (err)
		goto out;

	/* 阶段三在 CFG 后序上做反向单调迭代；u16 位集有限，因此必然收敛。 */
	changed = true;
	while (changed) {
		changed = false;
		for (i = 0; i < env->cfg.cur_postorder; ++i) {
			int insn_idx = env->cfg.insn_postorder[i];
			struct insn_live_regs *live = &state[insn_idx];
			struct bpf_iarray *succ;
			u16 new_out = 0;
			u16 new_in = 0;

			/* out 汇总全部直接后继，随后 def 杀旧值、use 重新生成入口需求。 */
			succ = bpf_insn_successors(env, insn_idx);
			for (int s = 0; s < succ->cnt; ++s)
				new_out |= state[succ->items[s]].in;
			/* 标准活性方程先 kill 本指令定义，再 gen 本指令读取。 */
			new_in = (new_out & ~live->def) | live->use;
			if (new_out != live->out || new_in != live->in) {
				live->in = new_in;
				live->out = new_out;
				changed = true;
			}
		}
	}

	/* 阶段四固定点完成后一次发布所有执行前活性，供状态剪枝查询。 */
	for (i = 0; i < insn_cnt; ++i)
		insn_aux[i].live_regs_before = state[i].in;

	if (env->log.level & BPF_LOG_LEVEL2) {
		/* 二级日志按指令输出 SCC 编号、R0..R9 活性图和反汇编，ldimm64 跳过第二槽。 */
		verbose(env, "Live regs before insn:\n");
		for (i = 0; i < insn_cnt; ++i) {
			if (env->insn_aux_data[i].scc)
				verbose(env, "%3d ", env->insn_aux_data[i].scc);
			else
				verbose(env, "    ");
			verbose(env, "%3d: ", i);
			/* R0..R9 逐位打印：数字表示活，点表示死。 */
			for (j = BPF_REG_0; j < BPF_REG_10; ++j)
				if (insn_aux[i].live_regs_before & BIT(j))
					verbose(env, "%d", j);
				else
					verbose(env, ".");
			verbose(env, " ");
			bpf_verbose_insn(env, &insns[i]);
			/* 64 位立即数第二槽不是独立指令，随首槽一起跳过。 */
			if (bpf_is_ldimm64(&insns[i]))
				i++;
		}
	}

out:
	/* state 始终由本函数独占；成功发布的是位集副本，不延长临时数组生命周期。 */
	kvfree(state);
	return err;
}
