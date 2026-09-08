// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */

#include <linux/bpf_verifier.h>

/*
 * Forward dataflow analysis to determine constant register values at every
 * instruction. Tracks 64-bit constant values in R0-R9 through the program,
 * using a fixed-point iteration in reverse postorder. Records which registers
 * hold known constants and their values in
 * env->insn_aux_data[].{const_reg_mask, const_reg_vals}.
 */
/*
 * 中文：本文件做前向常量数据流分析，在逆后序上迭代到固定点，追踪 R0-R9 的 64 位常量及特殊指针。
 * 结果写入每条指令的 aux 掩码和值数组，随后可把结果恒定的条件分支改成无条件跳转并缩减 CFG。
 */

/* 每个寄存器的抽象状态；枚举 0 特意留给 kvzalloc 后的“尚未到达”。 */
enum const_arg_state {
	CONST_ARG_UNVISITED,	/* instruction not yet reached */
	/* 中文：该指令入口还没有任何可达前驱贡献状态。 */
	CONST_ARG_UNKNOWN,	/* register value not a known constant */
	/* 中文：寄存器可达，但不同路径或不支持的运算使其值无法确定。 */
	CONST_ARG_CONST,	/* register holds a known 64-bit constant */
	/* 中文：val 保存已知 64 位常量。 */
	CONST_ARG_MAP_PTR,	/* register holds a map pointer, map_index is set */
	/* 中文：map_index 标识 map，本状态只用于向 aux 发布 map 引用。 */
	CONST_ARG_MAP_VALUE,	/* register points to map value data, val is offset */
	/* 中文：map_index 加 val 偏移可用于安全地直接读取只读 map value。 */
	CONST_ARG_SUBPROG,	/* register holds a subprog pointer, val is subprog number */
	/* 中文：val 保存子程序编号，供后续识别函数指针常量。 */
};

/* 单寄存器抽象值；state 决定 map_index/val 中哪些字段有效。 */
struct const_arg_info {
	enum const_arg_state state;
	/* MAP_PTR/MAP_VALUE 使用 map_index；其他状态忽略它。 */
	u32 map_index;
	/* CONST 为数值、MAP_VALUE 为偏移、SUBPROG 为编号。 */
	u64 val;
};

/* 判断数据流格元素是否仍为不可达底元素。 */
static bool ci_is_unvisited(const struct const_arg_info *ci)
{
	return ci->state == CONST_ARG_UNVISITED;
}

/* 判断寄存器已经可达但不具备可用常量信息。 */
static bool ci_is_unknown(const struct const_arg_info *ci)
{
	return ci->state == CONST_ARG_UNKNOWN;
}

/* 判断 val 是否表示普通 64 位常量。 */
static bool ci_is_const(const struct const_arg_info *ci)
{
	return ci->state == CONST_ARG_CONST;
}

/* 判断抽象值是否为带固定偏移的 map value 指针。 */
static bool ci_is_map_value(const struct const_arg_info *ci)
{
	return ci->state == CONST_ARG_MAP_VALUE;
}

/* Transfer function: compute output register state from instruction. */
/*
 * 中文：以 ci_out 中的指令入口状态为原地工作集，按 insn 语义计算出口寄存器抽象值。
 * 能证明的 MOV/ADD/SUB/AND、伪指针和只读 map 加载保留常量；调用或未知写入把受影响寄存器降为
 * UNKNOWN。函数不分配内存、不修改程序，只借用 env 的 map/aux 元数据。
 */
static void const_reg_xfer(struct bpf_verifier_env *env, struct const_arg_info *ci_out,
			   struct bpf_insn *insn, struct bpf_insn *insns, int idx)
{
	/* unknown 是统一的保守顶元素；覆盖整个结构可同时清除陈旧 map_index/val。 */
	struct const_arg_info unknown = { .state = CONST_ARG_UNKNOWN, .val = 0 };
	struct const_arg_info *dst = &ci_out[insn->dst_reg];
	struct const_arg_info *src = &ci_out[insn->src_reg];
	u8 class = BPF_CLASS(insn->code);
	u8 mode = BPF_MODE(insn->code);
	u8 opcode = BPF_OP(insn->code) | BPF_SRC(insn->code);
	int r;

	/* Stack arg stores (r11-based) are outside the tracked register set. */
	/* 中文：r11 出栈参数不属于 R0-R9 数据流；store 不变，load 只使目的寄存器未知。 */
	if (is_stack_arg_st(insn) || is_stack_arg_stx(insn))
		return;
	if (is_stack_arg_ldx(insn)) {
		ci_out[insn->dst_reg] = unknown;
		return;
	}

	switch (class) {
	case BPF_ALU:
	case BPF_ALU64:
		/* 只实现可封闭在本抽象域中的少量运算，其余一律遗忘目的寄存器。 */
		switch (opcode) {
		case BPF_MOV | BPF_K:
			/* imm 为有符号 32 位立即数，MOV64 按指令语义符号扩展后保存。 */
			dst->state = CONST_ARG_CONST;
			dst->val = (s64)insn->imm;
			break;
		case BPF_MOV | BPF_X:
			/* 无 cast 时复制完整抽象值；带 off 的窄化只对普通常量有定义。 */
			*dst = *src;
			if (!insn->off)
				break;
			if (!ci_is_const(dst)) {
				*dst = unknown;
				break;
			}
			switch (insn->off) {
			/* off 编码有符号 8/16/32 位扩展，未知宽度安全降级。 */
			case 8:  dst->val = (s8)dst->val; break;
			case 16: dst->val = (s16)dst->val; break;
			case 32: dst->val = (s32)dst->val; break;
			default: *dst = unknown; break;
			}
			break;
		case BPF_ADD | BPF_K:
			/* 常量和 map-value 固定偏移对加减立即数封闭，map 基址身份保持不变。 */
			if (!ci_is_const(dst) && !ci_is_map_value(dst)) {
				*dst = unknown;
				break;
			}
			dst->val += insn->imm;
			break;
		case BPF_SUB | BPF_K:
			/* SUB 与 ADD 使用同一可接受类别，只改变 val 中的数值/偏移。 */
			if (!ci_is_const(dst) && !ci_is_map_value(dst)) {
				*dst = unknown;
				break;
			}
			dst->val -= insn->imm;
			break;
		case BPF_AND | BPF_K:
			/* x & 0 可独立证明为零；其他情况要求旧 dst 已知。 */
			if (!ci_is_const(dst)) {
				if (!insn->imm) {
					/* 零吸收律不依赖旧 dst，因此可从 UNKNOWN 提升到已知零。 */
					dst->state = CONST_ARG_CONST;
					dst->val = 0;
				} else {
					*dst = unknown;
				}
				break;
			}
			dst->val &= (s64)insn->imm;
			break;
		case BPF_AND | BPF_X:
			/* 任一操作数为零即可折叠；否则必须两侧都是普通常量。 */
			if (ci_is_const(dst) && dst->val == 0)
				break; /* 0 & x == 0 */
			if (ci_is_const(src) && src->val == 0) {
				/* src 为零时覆盖 dst 的原有类别和值。 */
				dst->state = CONST_ARG_CONST;
				dst->val = 0;
				break;
			}
			if (!ci_is_const(dst) || !ci_is_const(src)) {
				/* 任一非普通常量都不能应用逐位计算，退化后停止。 */
				*dst = unknown;
				break;
			}
			dst->val &= src->val;
			break;
		default:
			/* 未建模 ALU 运算可能改变所有位，清除 dst 的常量/指针类别。 */
			*dst = unknown;
			break;
		}
		if (class == BPF_ALU) {
			/* ALU32 的结果零扩展到 64 位；特殊指针类别不能穿过 32 位运算。 */
			if (ci_is_const(dst))
				dst->val = (u32)dst->val;
			else if (!ci_is_unknown(dst))
				*dst = unknown;
		}
		break;
	case BPF_LD:
		/* ABS/IND 具有隐式调用式 clobber；这里只精确处理合法的双字立即数加载。 */
		if (mode == BPF_ABS || mode == BPF_IND)
			goto process_call;
		if (mode != BPF_IMM || BPF_SIZE(insn->code) != BPF_DW)
			break;
		if (insn->src_reg == BPF_PSEUDO_FUNC) {
			/* 伪函数立即数先解析为稳定 subprog 编号，非法目标降为未知。 */
			int subprog = bpf_find_subprog(env, idx + insn->imm + 1);

			if (subprog >= 0) {
				dst->state = CONST_ARG_SUBPROG;
				dst->val = subprog;
			} else {
				*dst = unknown;
			}
		} else if (insn->src_reg == BPF_PSEUDO_MAP_VALUE ||
			   insn->src_reg == BPF_PSEUDO_MAP_IDX_VALUE) {
			dst->state = CONST_ARG_MAP_VALUE;
			/* map_index/map_off 已在早期验证阶段写入本指令 aux。 */
			dst->map_index = env->insn_aux_data[idx].map_index;
			dst->val = env->insn_aux_data[idx].map_off;
		} else if (insn->src_reg == BPF_PSEUDO_MAP_FD ||
			   insn->src_reg == BPF_PSEUDO_MAP_IDX) {
			dst->state = CONST_ARG_MAP_PTR;
			/* map 指针只需身份，不携带 value 偏移。 */
			dst->map_index = env->insn_aux_data[idx].map_index;
		} else if (insn->src_reg == 0) {
			/* 普通 ldimm64 从连续两槽拼接低/高 32 位；CFG 已保证第二槽存在。 */
			dst->state = CONST_ARG_CONST;
			dst->val = (u64)(u32)insn->imm | ((u64)(u32)insns[idx + 1].imm << 32);
		} else {
			*dst = unknown;
		}
		break;
	case BPF_LDX:
		/* 只有固定 map-value 指针可能在验证时安全读取并折叠为常量。 */
		if (!ci_is_map_value(src)) {
			*dst = unknown;
			break;
		}
		struct bpf_map *map = env->used_maps[src->map_index];
		/* size/sign-extension/off 完全来自已验证指令和抽象指针。 */
		int size = bpf_size_to_bytes(BPF_SIZE(insn->code));
		bool is_ldsx = mode == BPF_MEMSX;
		int off = src->val + insn->off;
		u64 val = 0;

		if (!bpf_map_is_rdonly(map) || !map->ops->map_direct_value_addr ||
		    map->map_type == BPF_MAP_TYPE_INSN_ARRAY ||
		    off < 0 || off + size > map->value_size ||
		    bpf_map_direct_read(map, off, size, &val, is_ldsx)) {
			/* 可写、无直接地址、指令数组、越界或读取失败均不得假定内容稳定。 */
			*dst = unknown;
			break;
		}
		dst->state = CONST_ARG_CONST;
		dst->val = val;
		break;
	case BPF_JMP:
		/* 非调用跳转不写寄存器；调用与 ABS/IND 共用 R0-R5 clobber 处理。 */
		if (opcode != BPF_CALL)
			break;
process_call:
		/* BPF 调用约定允许返回值和五个参数寄存器全部改变。 */
		for (r = BPF_REG_0; r <= BPF_REG_5; r++)
			ci_out[r] = unknown;
		break;
	case BPF_STX:
		/* 普通 store 不改寄存器；原子形式可能把旧内存值写回 R0/dst/src。 */
		if (mode != BPF_ATOMIC)
			break;
		/* CMPXCHG 返回旧值到 R0，LOAD_ACQ 写 dst，FETCH 类写 src。 */
		if (insn->imm == BPF_CMPXCHG)
			ci_out[BPF_REG_0] = unknown;
		else if (insn->imm == BPF_LOAD_ACQ)
			*dst = unknown;
		else if (insn->imm & BPF_FETCH)
			*src = unknown;
		break;
	}
}

/* Join function: merge output state into a successor's input state. */
/*
 * 中文：把一个前驱出口逐寄存器并入后继入口。首次可达直接复制；多个可达前驱只有状态及载荷完全
 * 相同才保留，否则单调退化为 UNKNOWN。返回 true 表示后继变化，需要继续固定点迭代。
 */
static bool const_reg_join(struct const_arg_info *ci_target,
			   struct const_arg_info *ci_out)
{
	bool changed = false;
	int r;

	for (r = 0; r < MAX_BPF_REG; r++) {
		/* old 属于持久工作矩阵，new 只借用本轮栈上出口副本。 */
		struct const_arg_info *old = &ci_target[r];
		struct const_arg_info *new = &ci_out[r];

		if (ci_is_unvisited(old) && !ci_is_unvisited(new)) {
			/* 底元素收到首条可达边后获得该边的完整抽象值。 */
			ci_target[r] = *new;
			changed = true;
		} else if (!ci_is_unknown(old) && !ci_is_unvisited(old) &&
			   (new->state != old->state || new->val != old->val ||
			    new->map_index != old->map_index)) {
			old->state = CONST_ARG_UNKNOWN;
			/* UNKNOWN 是汇合顶元素，后续前驱不能把它重新提高为常量。 */
			changed = true;
		}
	}
	return changed;
}

/*
 * 为每条可达指令计算 MAX_BPF_REG 个入口状态，并把 R0-R9 的可编码结果发布到 insn_aux_data。
 * ci_in 由本函数分配和释放；成功写入普通常量、map 身份和子程序编号三类互斥掩码。
 * 返回 0 或 -ENOMEM，分析阶段不改写程序和 CFG。
 */
int bpf_compute_const_regs(struct bpf_verifier_env *env)
{
	/* unknown 用于初始化所有子程序入口；其余指令保持零值 UNVISITED。 */
	struct const_arg_info unknown = { .state = CONST_ARG_UNKNOWN, .val = 0 };
	struct bpf_insn_aux_data *insn_aux = env->insn_aux_data;
	struct bpf_insn *insns = env->prog->insnsi;
	int insn_cnt = env->prog->len;
	struct const_arg_info (*ci_in)[MAX_BPF_REG];
	/* ci_out 是处理单条指令时的栈上副本，避免污染该指令入口状态。 */
	struct const_arg_info ci_out[MAX_BPF_REG];
	struct bpf_iarray *succ;
	bool changed;
	int i, r;

	/* kvzalloc zeroes memory, so all entries start as CONST_ARG_UNVISITED (0) */
	/* 中文：二维矩阵按指令数分配；枚举布局让清零内存自然表示不可达。 */
	ci_in = kvzalloc_objs(*ci_in, insn_cnt, GFP_KERNEL_ACCOUNT);
	if (!ci_in)
		return -ENOMEM;

	/* Subprogram entries (including main at subprog 0): all registers unknown */
	/* 中文：每个子程序都可能作为独立入口，不能从 caller 假定任何初始寄存器常量。 */
	for (i = 0; i < env->subprog_cnt; i++) {
		int start = env->subprog_info[i].start;

		for (r = 0; r < MAX_BPF_REG; r++)
			ci_in[start][r] = unknown;
	}

redo:
	/* 每轮按 CFG 逆后序前向传播；格只会 UNVISITED→具体值→UNKNOWN，必然收敛。 */
	changed = false;
	for (i = env->cfg.cur_postorder - 1; i >= 0; i--) {
		int idx = env->cfg.insn_postorder[i];
		struct bpf_insn *insn = &insns[idx];
		struct const_arg_info *ci = ci_in[idx];

		/* 入口复制到出口，传递函数只修改被该指令定义或 clobber 的寄存器。 */
		memcpy(ci_out, ci, sizeof(ci_out));

		const_reg_xfer(env, ci_out, insn, insns, idx);

		succ = bpf_insn_successors(env, idx);
		/* 所有真实后继共享同一出口；任一 join 变化都会触发下一轮。 */
		for (int s = 0; s < succ->cnt; s++)
			changed |= const_reg_join(ci_in[succ->items[s]], ci_out);
	}
	if (changed)
		goto redo;

	/* Save computed constants into insn_aux[] if they fit into 32-bit */
	/* 中文：普通常量仅发布 32 位可表示值；特殊类别的 vals 分别保存 map/subprog 编号。 */
	for (i = 0; i < insn_cnt; i++) {
		u16 mask = 0, map_mask = 0, subprog_mask = 0;
		struct bpf_insn_aux_data *aux = &insn_aux[i];
		struct const_arg_info *ci = ci_in[i];

		for (r = BPF_REG_0; r < ARRAY_SIZE(aux->const_reg_vals); r++) {
			/* aux 数组不包含 FP，循环上界由目标字段自身大小约束。 */
			struct const_arg_info *c = &ci[r];

			switch (c->state) {
			case CONST_ARG_CONST: {
				u64 val = c->val;

				if (val != (u32)val)
					/* 分支改写只消费可无歧义编码的 32 位常量。 */
					break;
				mask |= BIT(r);
				aux->const_reg_vals[r] = val;
				break;
			}
			case CONST_ARG_MAP_PTR:
				/* map 指针发布身份索引，不把内核地址泄漏到 vals。 */
				map_mask |= BIT(r);
				aux->const_reg_vals[r] = c->map_index;
				break;
			case CONST_ARG_SUBPROG:
				/* 子程序指针同样只发布逻辑编号。 */
				subprog_mask |= BIT(r);
				aux->const_reg_vals[r] = c->val;
				break;
			default:
				/* MAP_VALUE/UNKNOWN/UNVISITED 不供分支常量折叠消费。 */
				break;
			}
		}
		aux->const_reg_mask = mask;
		/* 三个 mask 与同一 vals 数组配套，类别由命中的 mask 解释。 */
		aux->const_reg_map_mask = map_mask;
		aux->const_reg_subprog_mask = subprog_mask;
	}

	kvfree(ci_in);
	/* aux 已取得按值副本，释放固定点矩阵不会留下悬空引用。 */
	return 0;
}

/*
 * 用已知的两个操作数直接求条件跳转结果；无符号、有符号和按位测试分别遵循 BPF opcode 语义。
 * 返回 1/0 表示恒真/恒假，-1 表示调用者传入了未识别的条件码。
 */
static int eval_const_branch(u8 opcode, u64 dst_val, u64 src_val)
{
	/* BPF_OP 去掉来源/宽度位，只保留比较运算种类。 */
	switch (BPF_OP(opcode)) {
	case BPF_JEQ:	return dst_val == src_val;
	case BPF_JNE:	return dst_val != src_val;
	case BPF_JGT:	return dst_val > src_val;
	case BPF_JGE:	return dst_val >= src_val;
	case BPF_JLT:	return dst_val < src_val;
	case BPF_JLE:	return dst_val <= src_val;
	/* 带 S 的四类比较先把同一 64 位比特模式解释为 s64。 */
	case BPF_JSGT:	return (s64)dst_val > (s64)src_val;
	case BPF_JSGE:	return (s64)dst_val >= (s64)src_val;
	case BPF_JSLT:	return (s64)dst_val < (s64)src_val;
	case BPF_JSLE:	return (s64)dst_val <= (s64)src_val;
	/* JSET 只关心交集是否非零，显式转 bool 归一化为 0/1。 */
	case BPF_JSET:	return (bool)(dst_val & src_val);
	default:	return -1;
	}
}

/*
 * Rewrite conditional branches with constant outcomes into unconditional
 * jumps using register values resolved by bpf_compute_const_regs() pass.
 * This eliminates dead edges from the CFG so that compute_live_registers()
 * doesn't propagate liveness through dead code.
 */
/*
 * 中文：消费常量分析写入的 aux 信息，把结果恒定的条件分支改写为 BPF_JMP_A。
 * 恒真保留原 off，恒假使用 off=0；若发生任何改写，旧 CFG 后序数组失效并由本函数释放、重算。
 * 成功返回 0；未知条件码或后序重算失败返回负 errno。程序指令改写不可在本函数内回滚。
 */
int bpf_prune_dead_branches(struct bpf_verifier_env *env)
{
	/* insns 可写且归 env->prog 所有；aux 提供本指令入口处的已知寄存器值。 */
	struct bpf_insn_aux_data *insn_aux = env->insn_aux_data;
	struct bpf_insn *insns = env->prog->insnsi;
	int insn_cnt = env->prog->len;
	bool changed = false;
	int i;

	for (i = 0; i < insn_cnt; i++) {
		/* 每条指令独立判断，只有普通条件跳转会进入常量求值。 */
		struct bpf_insn_aux_data *aux = &insn_aux[i];
		struct bpf_insn *insn = &insns[i];
		u8 class = BPF_CLASS(insn->code);
		u64 dst_val, src_val;
		int taken;

		if (!bpf_insn_is_cond_jump(insn->code))
			continue;
		if (bpf_is_may_goto_insn(insn))
			/* may_goto 承载验证器循环语义，即使条件看似常量也不能在此消除。 */
			continue;

		/* 目的寄存器未知就无法证明整个条件结果。 */
		if (!(aux->const_reg_mask & BIT(insn->dst_reg)))
			continue;
		dst_val = aux->const_reg_vals[insn->dst_reg];

		if (BPF_SRC(insn->code) == BPF_K) {
			/* 立即数来源直接使用指令字段；寄存器来源必须也命中常量 mask。 */
			src_val = insn->imm;
		} else {
			if (!(aux->const_reg_mask & BIT(insn->src_reg)))
				continue;
			src_val = aux->const_reg_vals[insn->src_reg];
		}

		if (class == BPF_JMP32) {
			/*
			 * The (s32) cast maps the 32-bit range into two u64 sub-ranges:
			 * [0x00000000, 0x7FFFFFFF] -> [0x0000000000000000, 0x000000007FFFFFFF]
			 * [0x80000000, 0xFFFFFFFF] -> [0xFFFFFFFF80000000, 0xFFFFFFFFFFFFFFFF]
			 * The ordering is preserved within each sub-range, and
			 * the second sub-range is above the first as u64.
			 */
			/* 中文：先转成 s32 再扩到 u64，使有符号比较函数可与 64 位路径共用。 */
			dst_val = (s32)dst_val;
			src_val = (s32)src_val;
		}

		taken = eval_const_branch(insn->code, dst_val, src_val);
		/* 所有合法条件跳转都应有求值分支；负值表示内部 opcode 表漏项。 */
		if (taken < 0) {
			bpf_log(&env->log, "Unknown conditional jump %x\n", insn->code);
			return -EFAULT;
		}
		*insn = BPF_JMP_A(taken ? insn->off : 0);
		/* BPF_JMP_A 不读取寄存器：真分支跳原目标，假分支跳到下一条。 */
		changed = true;
	}

	if (!changed)
		/* 没改程序就继续复用现有 CFG 和后序数组。 */
		return 0;
	/* recompute postorder, since CFG has changed */
	/* 中文：先撤销旧数组 ownership，再让 bpf_compute_postorder 重新分配并发布。 */
	kvfree(env->cfg.insn_postorder);
	env->cfg.insn_postorder = NULL;
	return bpf_compute_postorder(env);
}
