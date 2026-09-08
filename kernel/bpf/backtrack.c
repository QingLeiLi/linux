// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/filter.h>
#include <linux/bitmap.h>

/* 所有诊断都复用验证器日志缓冲区；该宏不改变控制流，只统一输出入口。 */
#define verbose(env, fmt, args...) bpf_verifier_log_write(env, fmt, ##args)

/* for any branch, call, exit record the history of jmps in the given state */
/*
 * 中文：为分支、调用和退出保存“当前指令从哪里来”的历史，以便稍后沿真实执行路径逆行。
 * 若同一条原子指令的读写两侧重复登记，则合并 flags，并要求两次描述指向同一栈槽。
 * 新条目通过 krealloc 扩展，由 verifier state 持有；失败保持旧指针有效并返回 -ENOMEM。
 */
int bpf_push_jmp_history(struct bpf_verifier_env *env, struct bpf_verifier_state *cur,
			 int insn_flags, int spi, int frame, u64 linked_regs)
{
	u32 cnt = cur->jmp_history_cnt;
	struct bpf_jmp_history_entry *p;
	size_t alloc_size;

	/* combine instruction flags if we already recorded this instruction */
	/* 中文：cur_hist_ent 非空表示本轮指令已建条目，本次只补齐另一侧的访问语义。 */
	if (env->cur_hist_ent) {
		/* atomic instructions push insn_flags twice, for READ and
		 * WRITE sides, but they should agree on stack slot
		 */
		/* 中文：原子读改写分两次上报；flags 可并集，但 spi/frame/linked_regs 必须一致。 */
		verifier_bug_if((env->cur_hist_ent->flags & insn_flags) &&
				(env->cur_hist_ent->flags & insn_flags) != insn_flags,
				env, "insn history: insn_idx %d cur flags %x new flags %x",
				env->insn_idx, env->cur_hist_ent->flags, insn_flags);
		env->cur_hist_ent->flags |= insn_flags;
		env->cur_hist_ent->spi = spi;
		env->cur_hist_ent->frame = frame;
		/* linked_regs 每条指令只允许发布一次，避免重复打包覆盖先前的等价关系。 */
		verifier_bug_if(env->cur_hist_ent->linked_regs != 0, env,
				"insn history: insn_idx %d linked_regs: %#llx",
				env->insn_idx, env->cur_hist_ent->linked_regs);
		env->cur_hist_ent->linked_regs = linked_regs;
		return 0;
	}

	cnt++;
	/* 向分配器请求可容纳 cnt 项的桶大小，避免每次只增长一个结构体带来的频繁搬迁。 */
	alloc_size = kmalloc_size_roundup(size_mul(cnt, sizeof(*p)));
	p = krealloc(cur->jmp_history, alloc_size, GFP_KERNEL_ACCOUNT);
	if (!p)
		return -ENOMEM;
	cur->jmp_history = p;

	p = &cur->jmp_history[cnt - 1];
	/* idx/prev_idx 构成逆向边，其余字段描述这条边上的栈访问和寄存器联动。 */
	p->idx = env->insn_idx;
	p->prev_idx = env->prev_insn_idx;
	p->flags = insn_flags;
	p->spi = spi;
	p->frame = frame;
	p->linked_regs = linked_regs;
	cur->jmp_history_cnt = cnt;
	env->cur_hist_ent = p;

	return 0;
}

/* 判断指令是否为只执行 acquire load 的原子 STX 形式；用于把其结果按普通加载回溯。 */
static bool is_atomic_load_insn(const struct bpf_insn *insn)
{
	return BPF_CLASS(insn->code) == BPF_STX &&
	       BPF_MODE(insn->code) == BPF_ATOMIC &&
	       insn->imm == BPF_LOAD_ACQ;
}

/* 判断原子操作是否带 FETCH 返回旧值；返回值寄存器会成为精度依赖的定义点。 */
static bool is_atomic_fetch_insn(const struct bpf_insn *insn)
{
	return BPF_CLASS(insn->code) == BPF_STX &&
	       BPF_MODE(insn->code) == BPF_ATOMIC &&
	       (insn->imm & BPF_FETCH);
}

/* Backtrack one insn at a time. If idx is not at the top of recorded
 * history then previous instruction came from straight line execution.
 * Return -ENOENT if we exhausted all instructions within given state.
 *
 * It's legal to have a bit of a looping with the same starting and ending
 * insn index within the same state, e.g.: 3->4->5->3, so just because current
 * instruction index is the same as state's first_idx doesn't mean we are
 * done. If there is still some jump history left, we should keep going. We
 * need to take into account that we might have a jump history between given
 * state's parent and itself, due to checkpointing. In this case, we'll have
 * history entry recording a jump from last instruction of parent state and
 * first instruction of given state.
 */
/*
 * 中文：每次返回真实前驱指令号。若当前位置命中历史栈顶，就消费该跳转边；否则按直线流减一。
 * 到达 first_insn_idx 时仍须先检查历史，因为父状态到本检查点之间也可能有一条跨界跳转记录。
 * 返回 -ENOENT 只表示当前 verifier state 内的路径耗尽，调用者随后可转到 parent state。
 */
static int get_prev_insn_idx(struct bpf_verifier_state *st, int i,
			     u32 *history)
{
	u32 cnt = *history;

	/* first_insn_idx 是本状态片段左边界，但同 idx 的跨检查点历史项仍需保留给外层。 */
	if (i == st->first_insn_idx) {
		if (cnt == 0)
			return -ENOENT;
		if (cnt == 1 && st->jmp_history[0].idx == i)
			return -ENOENT;
	}

	/* 只有历史栈顶能作为当前前驱；其余情况沿物理相邻指令逆行。 */
	if (cnt && st->jmp_history[cnt - 1].idx == i) {
		i = st->jmp_history[cnt - 1].prev_idx;
		(*history)--;
	} else {
		i--;
	}
	return i;
}

/*
 * 在尚未消费的历史前缀中查找当前指令对应的栈顶条目；返回值借用自 st->jmp_history。
 * 历史严格按逆序消费，因此只需比较 hist_end - 1，NULL 表示这一步属于直线执行。
 */
static struct bpf_jmp_history_entry *get_jmp_hist_entry(struct bpf_verifier_state *st,
						        u32 hist_end, int insn_idx)
{
	if (hist_end > 0 && st->jmp_history[hist_end - 1].idx == insn_idx)
		return &st->jmp_history[hist_end - 1];
	return NULL;
}

/* 选择回溯起始调用帧；各位图由调用前准备或 env->bt 的零初始化状态提供。 */
static inline void bt_init(struct backtrack_state *bt, u32 frame)
{
	bt->frame = frame;
}

/*
 * 清空一次精度回溯留下的寄存器、栈槽和帧号，但保留 env 反向引用。
 * 该状态嵌在 verifier env 中，不涉及释放；重置后可供下一次请求复用。
 */
static inline void bt_reset(struct backtrack_state *bt)
{
	struct bpf_verifier_env *env = bt->env;

	memset(bt, 0, sizeof(*bt));
	bt->env = env;
}

/*
 * 汇总 0..当前帧的三类需求位图；全部为零才说明传递依赖已找到定义点。
 * 返回 u32 是历史接口选择，实际语义仍是布尔判定。
 */
static inline u32 bt_empty(struct backtrack_state *bt)
{
	u64 mask = 0;
	int i;

	for (i = 0; i <= bt->frame; i++)
		mask |= bt->reg_masks[i] | bt->stack_masks[i] | bt->stack_arg_masks[i];

	return mask == 0;
}

/* 从指定调用帧删除一个出栈参数槽的精度需求；slot 必须已由调用者做容量约束。 */
static inline void bt_clear_frame_stack_arg_slot(struct backtrack_state *bt, u32 frame, u32 slot)
{
	bt->stack_arg_masks[frame] &= ~(1 << slot);
}

/* 查询指定帧的出栈参数槽是否仍待追踪；结果只读取 env 内嵌位图。 */
static inline bool bt_is_frame_stack_arg_slot_set(struct backtrack_state *bt, u32 frame, u32 slot)
{
	return bt->stack_arg_masks[frame] & (1 << slot);
}

/*
 * 逆向遇到被调函数的 EXIT 时进入更深帧，使后续位图解释切换到 callee。
 * MAX_CALL_FRAMES 是硬边界；越界代表历史/帧转换不一致，报告验证器内部错误。
 */
static inline int bt_subprog_enter(struct backtrack_state *bt)
{
	if (bt->frame == MAX_CALL_FRAMES - 1) {
		verifier_bug(bt->env, "subprog enter from frame %d", bt->frame);
		return -EFAULT;
	}
	bt->frame++;
	return 0;
}

/*
 * 逆向越过静态子程序或同步回调的调用点时退回 caller 帧。
 * frame 0 不存在调用者，因此下溢被视为内部一致性错误。
 */
static inline int bt_subprog_exit(struct backtrack_state *bt)
{
	if (bt->frame == 0) {
		verifier_bug(bt->env, "subprog exit from frame 0");
		return -EFAULT;
	}
	bt->frame--;
	return 0;
}

/* 清除指定帧的寄存器需求位；reg 范围由 BPF 寄存器编号和调用者不变量保证。 */
static inline void bt_clear_frame_reg(struct backtrack_state *bt, u32 frame, u32 reg)
{
	bt->reg_masks[frame] &= ~(1 << reg);
}

/* 在当前逆向帧加入一个寄存器精度依赖，实际置位逻辑由公共内联助手完成。 */
static inline void bt_set_reg(struct backtrack_state *bt, u32 reg)
{
	bpf_bt_set_frame_reg(bt, bt->frame, reg);
}

/* 从当前逆向帧移除一个已经解释或已抵达定义点的寄存器依赖。 */
static inline void bt_clear_reg(struct backtrack_state *bt, u32 reg)
{
	bt_clear_frame_reg(bt, bt->frame, reg);
}

/* 清除指定帧的 8 字节栈槽依赖；一位对应 fp-(slot+1)*8。 */
static inline void bt_clear_frame_slot(struct backtrack_state *bt, u32 frame, u32 slot)
{
	bt->stack_masks[frame] &= ~(1ull << slot);
}

/* 读取指定帧尚未解析的寄存器集合，供跨 parent 状态逐帧提交精度标记。 */
static inline u32 bt_frame_reg_mask(struct backtrack_state *bt, u32 frame)
{
	return bt->reg_masks[frame];
}

/* 读取当前逆向帧的寄存器依赖集合。 */
static inline u32 bt_reg_mask(struct backtrack_state *bt)
{
	return bt->reg_masks[bt->frame];
}

/* 读取指定帧的普通 spill 栈槽依赖集合。 */
static inline u64 bt_frame_stack_mask(struct backtrack_state *bt, u32 frame)
{
	return bt->stack_masks[frame];
}

/* 读取当前逆向帧的普通 spill 栈槽依赖集合。 */
static inline u64 bt_stack_mask(struct backtrack_state *bt)
{
	return bt->stack_masks[bt->frame];
}

/* 读取当前帧尚未解析的跨子程序出栈参数槽集合。 */
static inline u8 bt_stack_arg_mask(struct backtrack_state *bt)
{
	return bt->stack_arg_masks[bt->frame];
}

/* 判断当前帧某寄存器是否仍影响目标精确标量。 */
static inline bool bt_is_reg_set(struct backtrack_state *bt, u32 reg)
{
	return bt->reg_masks[bt->frame] & (1 << reg);
}


/* format registers bitmask, e.g., "r0,r2,r4" for 0x15 mask */
/*
 * 中文：把寄存器位图按 rN 逗号列表写入调用者缓冲区，仅供详细日志使用。
 * snprintf 的返回值用于推进剩余窗口；空间耗尽即停止，不读取或修改回溯状态。
 */
static void fmt_reg_mask(char *buf, ssize_t buf_sz, u32 reg_mask)
{
	DECLARE_BITMAP(mask, 64);
	bool first = true;
	int i, n;

	buf[0] = '\0';

	/* u32 只使用低 32 位；bitmap 临时量让统一的 set-bit 迭代器完成格式化。 */
	bitmap_from_u64(mask, reg_mask);
	for_each_set_bit(i, mask, 32) {
		n = snprintf(buf, buf_sz, "%sr%d", first ? "" : ",", i);
		first = false;
		/* snprintf 返回理想写入长度，减到负数说明本项已截断并应结束。 */
		buf += n;
		buf_sz -= n;
		if (buf_sz < 0)
			break;
	}
}
/* format stack slots bitmask, e.g., "-8,-24,-40" for 0x15 mask */
/*
 * 中文：把栈槽位图转换为用户可读的负 fp 偏移，每一位代表连续的 8 字节槽。
 * 输出缓冲区由调用者拥有；函数始终先写 NUL，并在可用空间耗尽时截断。
 */
void bpf_fmt_stack_mask(char *buf, ssize_t buf_sz, u64 stack_mask)
{
	DECLARE_BITMAP(mask, 64);
	bool first = true;
	int i, n;

	buf[0] = '\0';

	/* 位号 i 映射为 ABI 栈偏移 -(i+1)*8，与验证器 spi 编号保持一致。 */
	bitmap_from_u64(mask, stack_mask);
	for_each_set_bit(i, mask, 64) {
		n = snprintf(buf, buf_sz, "%s%d", first ? "" : ",", -(i + 1) * 8);
		first = false;
		/* 与寄存器格式化相同，负的剩余长度是停止条件。 */
		buf += n;
		buf_sz -= n;
		if (buf_sz < 0)
			break;
	}
}


/* For given verifier state backtrack_insn() is called from the last insn to
 * the first insn. Its purpose is to compute a bitmask of registers and
 * stack slots that needs precision in the parent verifier state.
 *
 * @idx is an index of the instruction we are currently processing;
 * @subseq_idx is an index of the subsequent instruction that:
 *   - *would be* executed next, if jump history is viewed in forward order;
 *   - *was* processed previously during backtracking.
 */
/*
 * 中文：对一条指令执行反向精度传递：bt 表示指令出口所需精度，返回时改写为入口需求。
 * hist 提供本次真实路径上的栈槽、调用帧及联动寄存器；无历史时只能采用直线流语义。
 * 0 表示成功，-ENOTSUPP 要求上层保守地标记全部标量，-EFAULT 表示验证器内部不变量破坏。
 */
static int backtrack_insn(struct bpf_verifier_env *env, int idx, int subseq_idx,
			  struct bpf_jmp_history_entry *hist, struct backtrack_state *bt)
{
	/* insn 与各解码字段均借用 env->prog，函数只改写 bt 位图。 */
	struct bpf_insn *insn = env->prog->insnsi + idx;
	u8 class = BPF_CLASS(insn->code);
	u8 opcode = BPF_OP(insn->code);
	u8 mode = BPF_MODE(insn->code);
	u32 dreg = insn->dst_reg;
	u32 sreg = insn->src_reg;
	/* spi 是 8 字节槽号，fr 是历史记录归属帧；仅栈访问分支使用。 */
	u32 spi, i, fr;

	if (insn->code == 0)
		return 0;
	/* 二级日志在传递前打印需求集合，因此可与处理后的下一条日志对照数据流变化。 */
	if (env->log.level & BPF_LOG_LEVEL2) {
		fmt_reg_mask(env->tmp_str_buf, TMP_STR_BUF_LEN, bt_reg_mask(bt));
		verbose(env, "mark_precise: frame%d: regs=%s ",
			bt->frame, env->tmp_str_buf);
		bpf_fmt_stack_mask(env->tmp_str_buf, TMP_STR_BUF_LEN, bt_stack_mask(bt));
		verbose(env, "stack=%s before ", env->tmp_str_buf);
		verbose(env, "%d: ", idx);
		bpf_verbose_insn(env, insn);
	}

	/* If there is a history record that some registers gained range at this insn,
	 * propagate precision marks to those registers, so that bt_is_reg_set()
	 * accounts for these registers.
	 */
	/* 中文：先展开同一约束形成的寄存器等价组，避免只追踪其中一个成员而丢失精度来源。 */
	bpf_bt_sync_linked_regs(bt, hist);

	if (class == BPF_ALU || class == BPF_ALU64) {
		/* ALU 仅在目的寄存器被需求时反传；未使用的计算不影响目标值。 */
		if (!bt_is_reg_set(bt, dreg))
			return 0;
		if (opcode == BPF_END || opcode == BPF_NEG) {
			/* sreg is reserved and unused
			 * dreg still need precision before this insn
			 */
			/* 中文：字节序转换和取负只依赖旧 dreg，故需求位原样穿过。 */
			return 0;
		} else if (opcode == BPF_MOV) {
			if (BPF_SRC(insn->code) == BPF_X) {
				/* dreg = sreg or dreg = (s8, s16, s32)sreg
				 * dreg needs precision after this insn
				 * sreg needs precision before this insn
				 */
				/* 中文：寄存器 MOV 把出口 dreg 的需求转移到入口 sreg；FP 不是标量来源。 */
				bt_clear_reg(bt, dreg);
				if (sreg != BPF_REG_FP)
					bt_set_reg(bt, sreg);
			} else {
				/* dreg = K
				 * dreg needs precision after this insn.
				 * Corresponding register is already marked
				 * as precise=true in this verifier state.
				 * No further markings in parent are necessary
				 */
				/* 中文：立即数 MOV 在本指令定义常量，逆向依赖到此终止。 */
				bt_clear_reg(bt, dreg);
			}
		} else {
			if (BPF_SRC(insn->code) == BPF_X) {
				/* dreg += sreg
				 * both dreg and sreg need precision
				 * before this insn
				 */
				/* 中文：二元寄存器 ALU 同时依赖旧 dreg 与 sreg，保留前者并加入后者。 */
				if (sreg != BPF_REG_FP)
					bt_set_reg(bt, sreg);
			} /* else dreg += K
			   * dreg still needs precision before this insn
			   */
		}
	} else if (class == BPF_LDX ||
		   is_atomic_load_insn(insn) ||
		   is_atomic_fetch_insn(insn)) {
		u32 load_reg = dreg;
		/* 加载是目的寄存器的定义点；只有其结果被需求才需继续寻找内存来源。 */

		/*
		 * Atomic fetch operation writes the old value into
		 * a register (sreg or r0) and if it was tracked for
		 * precision, propagate to the stack slot like we do
		 * in regular ldx.
		 */
		if (is_atomic_fetch_insn(insn))
			load_reg = insn->imm == BPF_CMPXCHG ?
				   BPF_REG_0 : sreg;

		if (!bt_is_reg_set(bt, load_reg))
			return 0;
		bt_clear_reg(bt, load_reg);

		if (hist && hist->flags & INSN_F_STACK_ARG_ACCESS) {
			spi = hist->spi;
			/*
			 * Stack arg read: callee reads from r11+off, but
			 * the data lives in the caller's stack_arg_regs.
			 * Set the mask in the caller frame so precision
			 * is marked in the caller's slot at the callee
			 * entry checkpoint.
			 */
			/* 中文：r11 参数区实存于 caller，故跨一层帧把槽需求交回调用者。 */
			bt_set_frame_stack_arg_slot(bt, bt->frame - 1, spi);
			return 0;
		}

		/* scalars can only be spilled into stack w/o losing precision.
		 * Load from any other memory can be zero extended.
		 * The desire to keep that precision is already indicated
		 * by 'precise' mark in corresponding register of this state.
		 * No further tracking necessary.
		 */
		/* 中文：非栈内存的加载结果已由当前状态 precise 标记兜底，无可安全追溯的 spill 来源。 */
		if (!hist || !(hist->flags & INSN_F_STACK_ACCESS))
			return 0;
		/* dreg = *(u64 *)[fp - off] was a fill from the stack.
		 * that [fp - off] slot contains scalar that needs to be
		 * tracked with precision
		 */
		/* 中文：普通 fill 将寄存器需求改写为历史记录所指帧/槽的 spill 需求。 */
		spi = hist->spi;
		fr = hist->frame;
		bpf_bt_set_frame_slot(bt, fr, spi);
	} else if (class == BPF_STX || class == BPF_ST) {
		/* store 定义内存而非标量 dreg；若 dreg 被当成标量依赖，说明遇到不支持的指针运算。 */
		if (bt_is_reg_set(bt, dreg))
			/* stx & st shouldn't be using _scalar_ dst_reg
			 * to access memory. It means backtracking
			 * encountered a case of pointer subtraction.
			 */
			return -ENOTSUPP;

		if (hist && hist->flags & INSN_F_STACK_ARG_ACCESS) {
			spi = hist->spi;
			if (!bt_is_frame_stack_arg_slot_set(bt, bt->frame, spi))
				return 0;
			bt_clear_frame_stack_arg_slot(bt, bt->frame, spi);
			if (class == BPF_STX)
				/* 寄存器 store 把槽需求转成 sreg；立即数 store 则在此终止。 */
				bt_set_reg(bt, sreg);
			return 0;
		}

		/* scalars can only be spilled into stack */
		/* 中文：只有验证阶段确认为栈访问的 store 才能定义普通 spill 槽。 */
		if (!hist || !(hist->flags & INSN_F_STACK_ACCESS))
			return 0;
		spi = hist->spi;
		fr = hist->frame;
		if (!bt_is_frame_slot_set(bt, fr, spi))
			return 0;
		bt_clear_frame_slot(bt, fr, spi);
		if (class == BPF_STX)
			bt_set_reg(bt, sreg);
	} else if (class == BPF_JMP || class == BPF_JMP32) {
		/* 调用/退出负责切换帧；条件跳转则把精度需求扩展到参与比较的两个操作数。 */
		if (bpf_pseudo_call(insn)) {
			int subprog_insn_idx, subprog;

			subprog_insn_idx = idx + insn->imm + 1;
			subprog = bpf_find_subprog(env, subprog_insn_idx);
			if (subprog < 0)
				return -EFAULT;

			if (bpf_subprog_is_global(env, subprog)) {
				/* check that jump history doesn't have any
				 * extra instructions from subprog; the next
				 * instruction after call to global subprog
				 * should be literally next instruction in
				 * caller program
				 */
				/* 中文：全局函数按摘要验证，不把其内部指令串进当前历史，故后继必须就是 idx+1。 */
				verifier_bug_if(idx + 1 != subseq_idx, env,
						"extra insn from subprog");
				/* r1-r5 are invalidated after subprog call,
				 * so for global func call it shouldn't be set
				 * anymore
				 */
				/* 中文：全局调用已使 R1-R5 无效，残留参数需求意味着逆向传递漏掉了定义。 */
				if (bt_reg_mask(bt) & BPF_REGMASK_ARGS) {
					verifier_bug(env, "global subprog unexpected regs %x",
						     bt_reg_mask(bt));
					return -EFAULT;
				}
				/* global subprog always sets R0 */
				/* 中文：R0 由全局被调函数重新定义，调用前的 R0 不再影响调用后结果。 */
				bt_clear_reg(bt, BPF_REG_0);
				return 0;
			} else {
				/* static subprog call instruction, which
				 * means that we are exiting current subprog,
				 * so only r1-r5 could be still requested as
				 * precise, r0 and r6-r10 or any stack slot in
				 * the current frame should be zero by now
				 */
				/* 中文：逆向越过静态调用点时正离开 callee，只允许把其入口参数交回 caller。 */
				if (bt_reg_mask(bt) & ~BPF_REGMASK_ARGS) {
					verifier_bug(env, "static subprog unexpected regs %x",
						     bt_reg_mask(bt));
					return -EFAULT;
				}
				/* we are now tracking register spills correctly,
				 * so any instance of leftover slots is a bug
				 */
				/* 中文：callee 自有 spill 应已在其内部找到定义点，跨调用点残留即为实现错误。 */
				if (bt_stack_mask(bt) != 0) {
					verifier_bug(env,
						     "static subprog leftover stack slots %llx",
						     bt_stack_mask(bt));
					return -EFAULT;
				}
				/* propagate r1-r5 to the caller */
				/* 中文：逐位把 callee 的 R1-R5 需求搬到上一帧同名参数寄存器。 */
				for (i = BPF_REG_1; i <= BPF_REG_5; i++) {
					if (bt_is_reg_set(bt, i)) {
						bt_clear_reg(bt, i);
						bpf_bt_set_frame_reg(bt, bt->frame - 1, i);
					}
				}
				if (bt_stack_arg_mask(bt)) {
					/* 出栈参数也必须已被 callee 的读取/调用点配对消费。 */
					verifier_bug(env,
						     "static subprog leftover stack arg slots %x",
						     bt_stack_arg_mask(bt));
					return -EFAULT;
				}
				if (bt_subprog_exit(bt))
					return -EFAULT;
				return 0;
			}
		} else if (bpf_is_sync_callback_calling_insn(insn) && idx != subseq_idx - 1) {
			/* exit from callback subprog to callback-calling helper or
			 * kfunc call. Use idx/subseq_idx check to discern it from
			 * straight line code backtracking.
			 * Unlike the subprog call handling above, we shouldn't
			 * propagate precision of r1-r5 (if any requested), as they are
			 * not actually arguments passed directly to callback subprogs
			 */
			/* 中文：非相邻后继表明路径从回调返回；回调参数由 helper/kfunc 构造，不能转给 caller。 */
			if (bt_reg_mask(bt) & ~BPF_REGMASK_ARGS) {
				verifier_bug(env, "callback unexpected regs %x",
					     bt_reg_mask(bt));
				return -EFAULT;
			}
			if (bt_stack_mask(bt) != 0) {
				verifier_bug(env, "callback leftover stack slots %llx",
					     bt_stack_mask(bt));
				return -EFAULT;
			}
			/* clear r1-r5 in callback subprog's mask */
			/* 中文：清掉回调帧参数需求后退栈，caller 原有位图保持不变。 */
			for (i = BPF_REG_1; i <= BPF_REG_5; i++)
				bt_clear_reg(bt, i);
			if (bt_subprog_exit(bt))
				return -EFAULT;
			return 0;
		} else if (opcode == BPF_CALL) {
			/* kfunc with imm==0 is invalid and fixup_kfunc_call will
			 * catch this error later. Make backtracking conservative
			 * with ENOTSUPP.
			 */
			/* 中文：未修复的空 kfunc 目标缺少可靠语义，只能触发全标 precise 的保守路径。 */
			if (insn->src_reg == BPF_PSEUDO_KFUNC_CALL && insn->imm == 0)
				return -ENOTSUPP;
			/* regular helper call sets R0 */
			/* 中文：普通 helper/kfunc 定义返回寄存器 R0，因此旧 R0 依赖在调用点终止。 */
			bt_clear_reg(bt, BPF_REG_0);
			if (bt_reg_mask(bt) & BPF_REGMASK_ARGS) {
				/* if backtracking was looking for registers R1-R5
				 * they should have been found already.
				 */
				/* 中文：参数精度应在验证调用实参时已触发；调用后仍追踪它们说明历史不自洽。 */
				verifier_bug(env, "backtracking call unexpected regs %x",
					     bt_reg_mask(bt));
				return -EFAULT;
			}
			if (insn->src_reg == BPF_REG_0 && insn->imm == BPF_FUNC_tail_call
			    && subseq_idx - idx != 1) {
				/* 非直线后继表示沿 tail-call 的回退路径进入另一帧，逆向需相应加深帧号。 */
				if (bt_subprog_enter(bt))
					return -EFAULT;
			}
		} else if (opcode == BPF_EXIT) {
			bool r0_precise;

			/* Backtracking to a nested function call, 'idx' is a part of
			 * the inner frame 'subseq_idx' is a part of the outer frame.
			 * In case of a regular function call, instructions giving
			 * precision to registers R1-R5 should have been found already.
			 * In case of a callback, it is ok to have R1-R5 marked for
			 * backtracking, as these registers are set by the function
			 * invoking callback.
			 */
			/* 中文：从嵌套帧的 EXIT 逆行；同步回调的 R1-R5 来源在调用器，允许先清除。 */
			if (subseq_idx >= 0 && bpf_calls_callback(env, subseq_idx))
				for (i = BPF_REG_1; i <= BPF_REG_5; i++)
					bt_clear_reg(bt, i);
			if (bt_reg_mask(bt) & BPF_REGMASK_ARGS) {
				verifier_bug(env, "backtracking exit unexpected regs %x",
					     bt_reg_mask(bt));
				return -EFAULT;
			}

			/* BPF_EXIT in subprog or callback always returns
			 * right after the call instruction, so by checking
			 * whether the instruction at subseq_idx-1 is subprog
			 * call or not we can distinguish actual exit from
			 * *subprog* from exit from *callback*. In the former
			 * case, we need to propagate r0 precision, if
			 * necessary. In the former we never do that.
			 */
			/* 中文：查看外层后继前一条是否 pseudo call，以区分普通子程序返回和回调返回。 */
			r0_precise = subseq_idx - 1 >= 0 &&
				     bpf_pseudo_call(&env->prog->insnsi[subseq_idx - 1]) &&
				     bt_is_reg_set(bt, BPF_REG_0);

			bt_clear_reg(bt, BPF_REG_0);
			if (bt_subprog_enter(bt))
				return -EFAULT;

			if (r0_precise)
				/* 普通子程序的返回值依赖在新进入的 callee 帧继续追踪 R0。 */
				bt_set_reg(bt, BPF_REG_0);
			/* r6-r9 and stack slots will stay set in caller frame
			 * bitmasks until we return back from callee(s)
			 */
			/* 中文：callee-saved 寄存器与 caller 栈属于外层帧，切帧时无需复制或清除。 */
			return 0;
		} else if (BPF_SRC(insn->code) == BPF_X) {
			/* 只有比较任一操作数影响目标精度时，分支条件的两侧才都需要精确。 */
			if (!bt_is_reg_set(bt, dreg) && !bt_is_reg_set(bt, sreg))
				return 0;
			/* dreg <cond> sreg
			 * Both dreg and sreg need precision before
			 * this insn. If only sreg was marked precise
			 * before it would be equally necessary to
			 * propagate it to dreg.
			 */
			/* 中文：若操作数实际来自栈比较，history flag 表示其寄存器字段不是标量来源。 */
			if (!hist || !(hist->flags & INSN_F_SRC_REG_STACK))
				bt_set_reg(bt, sreg);
			if (!hist || !(hist->flags & INSN_F_DST_REG_STACK))
				bt_set_reg(bt, dreg);
		} else if (BPF_SRC(insn->code) == BPF_K) {
			 /* dreg <cond> K
			  * Only dreg still needs precision before
			  * this insn, so for the K-based conditional
			  * there is nothing new to be marked.
			  */
			 /* 中文：立即数不引入新依赖，已有 dreg 需求原样穿过条件跳转。 */
		}
	} else if (class == BPF_LD) {
		/* LD_IMM64 在本条定义 dreg；ABS/IND 尚无精确反传模型，要求保守降级。 */
		if (!bt_is_reg_set(bt, dreg))
			return 0;
		bt_clear_reg(bt, dreg);
		/* It's ld_imm64 or ld_abs or ld_ind.
		 * For ld_imm64 no further tracking of precision
		 * into parent is necessary
		 */
		/* 中文：立即数加载无需追溯；包绝对/间接加载可能零扩展且依赖隐式寄存器。 */
		if (mode == BPF_IND || mode == BPF_ABS)
			/* to be analyzed */
			return -ENOTSUPP;
	}
	/* Propagate precision marks to linked registers, to account for
	 * registers marked as precise in this function.
	 */
	/* 中文：指令处理后再同步一次，覆盖本条计算中新加入需求所连接的等价寄存器。 */
	bpf_bt_sync_linked_regs(bt, hist);
	return 0;
}

/* the scalar precision tracking algorithm:
 * . at the start all registers have precise=false.
 * . scalar ranges are tracked as normal through alu and jmp insns.
 * . once precise value of the scalar register is used in:
 *   .  ptr + scalar alu
 *   . if (scalar cond K|scalar)
 *   .  helper_call(.., scalar, ...) where ARG_CONST is expected
 *   backtrack through the verifier states and mark all registers and
 *   stack slots with spilled constants that these scalar registers
 *   should be precise.
 * . during state pruning two registers (or spilled stack slots)
 *   are equivalent if both are not precise.
 *
 * Note the verifier cannot simply walk register parentage chain,
 * since many different registers and stack slots could have been
 * used to compute single precise scalar.
 *
 * The approach of starting with precise=true for all registers and then
 * backtrack to mark a register as not precise when the verifier detects
 * that program doesn't care about specific value (e.g., when helper
 * takes register as ARG_ANYTHING parameter) is not safe.
 *
 * It's ok to walk single parentage chain of the verifier states.
 * It's possible that this backtracking will go all the way till 1st insn.
 * All other branches will be explored for needing precision later.
 *
 * The backtracking needs to deal with cases like:
 *   R8=map_value(id=0,off=0,ks=4,vs=1952,imm=0) R9_w=map_value(id=0,off=40,ks=4,vs=1952,imm=0)
 * r9 -= r8
 * r5 = r9
 * if r5 > 0x79f goto pc+7
 *    R5_w=inv(id=0,umax_value=1951,var_off=(0x0; 0x7ff))
 * r5 += 1
 * ...
 * call bpf_perf_event_output#25
 *   where .arg5_type = ARG_CONST_SIZE_OR_ZERO
 *
 * and this case:
 * r6 = 1
 * call foo // uses callee's r6 inside to compute r0
 * r0 += r6
 * if r0 == 0 goto
 *
 * to track above reg_mask/stack_mask needs to be independent for each frame.
 *
 * Also if parent's curframe > frame where backtracking started,
 * the verifier need to mark registers in both frames, otherwise callees
 * may incorrectly prune callers. This is similar to
 * commit 7640ead93924 ("bpf: verifier: make sure callees don't prune with caller differences")
 *
 * For now backtracking falls back into conservative marking.
 */
/*
 * 中文：精度追踪从“默认不精确”开始，只有标量参与指针偏移、分支裁剪或常量参数时，才沿父状态
 * 反向标记所有传递来源。一个结果可能由多个寄存器和 spill 槽共同形成，不能只走单一 parentage。
 * 各调用帧必须保有独立位图，否则 callee 的缓存等价判断可能错误忽略 caller 差异。
 * 当某类指令尚无可靠逆向模型时，本函数作为安全兜底，把整条父链的所有标量来源设为 precise。
 */
/*
 * 从 st 的父状态开始扫描全部活动帧，将普通寄存器和已 spill 的标量统一标为精确。
 * 本函数无失败返回；它牺牲状态剪枝效果换取安全性，不修改非标量和当前未检查点状态。
 */
void bpf_mark_all_scalars_precise(struct bpf_verifier_env *env,
				 struct bpf_verifier_state *st)
{
	struct bpf_func_state *func;
	struct bpf_reg_state *reg;
	int i, j;

	if (env->log.level & BPF_LOG_LEVEL2) {
		verbose(env, "mark_precise: frame%d: falling back to forcing all scalars precise\n",
			st->curframe);
	}

	/* big hammer: mark all scalars precise in this path.
	 * pop_stack may still get !precise scalars.
	 * We also skip current state and go straight to first parent state,
	 * because precision markings in current non-checkpointed state are
	 * not needed. See why in the comment in __mark_chain_precision below.
	 */
	/* 中文：当前状态是短命工作副本，真正参与未来剪枝的是 parent 检查点，所以从 st->parent 起步。 */
	for (st = st->parent; st; st = st->parent) {
		/* 父链节点拥有 0..curframe 的完整函数状态；逐帧覆盖 caller 与所有尚未退出的 callee。 */
		for (i = 0; i <= st->curframe; i++) {
			func = st->frame[i];
			for (j = 0; j < BPF_REG_FP; j++) {
				/* R10/FP 不是标量，普通可写寄存器只对 SCALAR_VALUE 置 precise。 */
				reg = &func->regs[j];
				if (reg->type != SCALAR_VALUE || reg->precise)
					continue;
				reg->precise = true;
				/* 日志只报告本轮真正由 false 变为 true 的普通寄存器。 */
				if (env->log.level & BPF_LOG_LEVEL2) {
					verbose(env, "force_precise: frame%d: forcing r%d to be precise\n",
						i, j);
				}
			}
			for (j = 0; j < func->allocated_stack / BPF_REG_SIZE; j++) {
				/* 栈按 8 字节槽遍历；仅完整 spill 的寄存器携带可传播的精度状态。 */
				if (!bpf_is_spilled_reg(&func->stack[j]))
					continue;
				reg = &func->stack[j].spilled_ptr;
				/* 非标量或已经 precise 的槽无需重复更新和打印。 */
				if (reg->type != SCALAR_VALUE || reg->precise)
					continue;
				reg->precise = true;
				if (env->log.level & BPF_LOG_LEVEL2) {
					verbose(env, "force_precise: frame%d: forcing fp%d to be precise\n",
						i, -(j + 1) * 8);
				}
				/* 下一槽继续独立判断，直到覆盖该帧全部 allocated_stack。 */
			}
		}
	}
}

/*
 * bpf_mark_chain_precision() backtracks BPF program instruction sequence and
 * chain of verifier states making sure that register *regno* (if regno >= 0)
 * and/or stack slot *spi* (if spi >= 0) are marked as precisely tracked
 * SCALARS, as well as any other registers and slots that contribute to
 * a tracked state of given registers/stack slots, depending on specific BPF
 * assembly instructions (see backtrack_insns() for exact instruction handling
 * logic). This backtracking relies on recorded jmp_history and is able to
 * traverse entire chain of parent states. This process ends only when all the
 * necessary registers/slots and their transitive dependencies are marked as
 * precise.
 *
 * One important and subtle aspect is that precise marks *do not matter* in
 * the currently verified state (current state). It is important to understand
 * why this is the case.
 *
 * First, note that current state is the state that is not yet "checkpointed",
 * i.e., it is not yet put into env->explored_states, and it has no children
 * states as well. It's ephemeral, and can end up either a) being discarded if
 * compatible explored state is found at some point or BPF_EXIT instruction is
 * reached or b) checkpointed and put into env->explored_states, branching out
 * into one or more children states.
 *
 * In the former case, precise markings in current state are completely
 * ignored by state comparison code (see regsafe() for details). Only
 * checkpointed ("old") state precise markings are important, and if old
 * state's register/slot is precise, regsafe() assumes current state's
 * register/slot as precise and checks value ranges exactly and precisely. If
 * states turn out to be compatible, current state's necessary precise
 * markings and any required parent states' precise markings are enforced
 * after the fact with propagate_precision() logic, after the fact. But it's
 * important to realize that in this case, even after marking current state
 * registers/slots as precise, we immediately discard current state. So what
 * actually matters is any of the precise markings propagated into current
 * state's parent states, which are always checkpointed (due to b) case above).
 * As such, for scenario a) it doesn't matter if current state has precise
 * markings set or not.
 *
 * Now, for the scenario b), checkpointing and forking into child(ren)
 * state(s). Note that before current state gets to checkpointing step, any
 * processed instruction always assumes precise SCALAR register/slot
 * knowledge: if precise value or range is useful to prune jump branch, BPF
 * verifier takes this opportunity enthusiastically. Similarly, when
 * register's value is used to calculate offset or memory address, exact
 * knowledge of SCALAR range is assumed, checked, and enforced. So, similar to
 * what we mentioned above about state comparison ignoring precise markings
 * during state comparison, BPF verifier ignores and also assumes precise
 * markings *at will* during instruction verification process. But as verifier
 * assumes precision, it also propagates any precision dependencies across
 * parent states, which are not yet finalized, so can be further restricted
 * based on new knowledge gained from restrictions enforced by their children
 * states. This is so that once those parent states are finalized, i.e., when
 * they have no more active children state, state comparison logic in
 * is_state_visited() would enforce strict and precise SCALAR ranges, if
 * required for correctness.
 *
 * To build a bit more intuition, note also that once a state is checkpointed,
 * the path we took to get to that state is not important. This is crucial
 * property for state pruning. When state is checkpointed and finalized at
 * some instruction index, it can be correctly and safely used to "short
 * circuit" any *compatible* state that reaches exactly the same instruction
 * index. I.e., if we jumped to that instruction from a completely different
 * code path than original finalized state was derived from, it doesn't
 * matter, current state can be discarded because from that instruction
 * forward having a compatible state will ensure we will safely reach the
 * exit. States describe preconditions for further exploration, but completely
 * forget the history of how we got here.
 *
 * This also means that even if we needed precise SCALAR range to get to
 * finalized state, but from that point forward *that same* SCALAR register is
 * never used in a precise context (i.e., it's precise value is not needed for
 * correctness), it's correct and safe to mark such register as "imprecise"
 * (i.e., precise marking set to false). This is what we rely on when we do
 * not set precise marking in current state. If no child state requires
 * precision for any given SCALAR register, it's safe to dictate that it can
 * be imprecise. If any child state does require this register to be precise,
 * we'll mark it precise later retroactively during precise markings
 * propagation from child state to parent states.
 *
 * Skipping precise marking setting in current state is a mild version of
 * relying on the above observation. But we can utilize this property even
 * more aggressively by proactively forgetting any precise marking in the
 * current state (which we inherited from the parent state), right before we
 * checkpoint it and branch off into new child state. This is done by
 * mark_all_scalars_imprecise() to hopefully get more permissive and generic
 * finalized states which help in short circuiting more future states.
 */
/*
 * 中文：本入口从当前指令沿真实历史和 verifier parent 链逆行，把 regno 以及 env->bt 中预置的栈槽
 * 需求扩展到全部传递来源。当前状态尚未成为可复用检查点，给它设置 precise 对剪枝没有作用；因此
 * 只在跨入 parent 后提交标记。检查点描述的是“从这里继续执行所需的前置条件”，到达它的旧路径
 * 已被遗忘；子状态后来发现的精度需求必须反向补到这些可复用父检查点，才能保持状态比较安全。
 * 返回 0 表示精确传播或保守降级成功，-EFAULT 表示历史/帧/类型不变量破坏；changed 可选地报告
 * 是否给父状态新置 precise 位。函数不分配内存，env->bt 为验证器独占的可复用工作区。
 */
int bpf_mark_chain_precision(struct bpf_verifier_env *env,
			    struct bpf_verifier_state *starting_state,
			    int regno,
			    bool *changed)
{
	/* st 在外层循环沿 parent 迁移；bt 位图则跨这些节点累计未解析依赖。 */
	struct bpf_verifier_state *st = starting_state;
	struct backtrack_state *bt = &env->bt;
	/* first/last 划定当前状态片段，subseq_idx 帮助识别调用/返回而非直线相邻。 */
	int first_idx = st->first_insn_idx;
	int last_idx = starting_state->insn_idx;
	int subseq_idx = -1;
	struct bpf_func_state *func;
	bool tmp, skip_first = true;
	struct bpf_reg_state *reg;
	/* fr 遍历调用帧，i 兼作指令号或位图下标，err 传播逆向传递结果。 */
	int i, fr, err;

	/* 非特权验证不启用这项剪枝优化，也就无需维护精度依赖。 */
	if (!env->bpf_capable)
		return 0;

	/* NULL changed 使用只写临时变量，统一后续置位路径而不要求调用者提供存储。 */
	changed = changed ?: &tmp;
	/* set frame number from which we are starting to backtrack */
	/* 中文：从当前执行帧解释 regno；其他需求可能已由状态传播预置在 env->bt 的各帧位图。 */
	bt_init(bt, starting_state->curframe);

	/* Do sanity checks against current state of register and/or stack
	 * slot, but don't set precise flag in current state, as precision
	 * tracking in the current state is unnecessary.
	 */
	/* 中文：只校验起点确为标量并置工作位，不改 starting_state 自身的 precise 标志。 */
	func = st->frame[bt->frame];
	if (regno >= 0) {
		reg = &func->regs[regno];
		if (reg->type != SCALAR_VALUE) {
			verifier_bug(env, "backtracking misuse");
			return -EFAULT;
		}
		bt_set_reg(bt, regno);
	}

	if (bt_empty(bt))
		/* 没有寄存器或栈槽需求时无需扫描历史。 */
		return 0;

	/* 外层每轮消费一个 verifier state，内层沿该状态记录的实际指令路径逆行。 */
	for (;;) {
		DECLARE_BITMAP(mask, 64);
		u32 history = st->jmp_history_cnt;
		struct bpf_jmp_history_entry *hist;

		if (env->log.level & BPF_LOG_LEVEL2) {
			verbose(env, "mark_precise: frame%d: last_idx %d first_idx %d subseq_idx %d \n",
				bt->frame, last_idx, first_idx, subseq_idx);
		}

		if (last_idx < 0) {
			/* we are at the entry into subprog, which
			 * is expected for global funcs, but only if
			 * requested precise registers are R1-R5
			 * (which are global func's input arguments)
			 */
			/* 中文：独立验证的全局子程序没有 caller，只能把 R1-R5 输入直接标到入口状态。 */
			if (st->curframe == 0 &&
			    st->frame[0]->subprogno > 0 &&
			    st->frame[0]->callsite == BPF_MAIN_FUNC &&
			    bt_stack_mask(bt) == 0 &&
			    (bt_reg_mask(bt) & ~BPF_REGMASK_ARGS) == 0) {
				bitmap_from_u64(mask, bt_reg_mask(bt));
				for_each_set_bit(i, mask, 32) {
					reg = &st->frame[0]->regs[i];
					bt_clear_reg(bt, i);
					if (reg->type == SCALAR_VALUE) {
						/* 输入仍为标量时，入口本身就是追踪边界；非标量需求可直接丢弃。 */
						reg->precise = true;
						*changed = true;
					}
				}
				return 0;
			}

			/* 主程序入口或仍带非参数/栈需求都不应越过边界，属于历史不完整。 */
			verifier_bug(env, "backtracking func entry subprog %d reg_mask %x stack_mask %llx",
				     st->frame[0]->subprogno, bt_reg_mask(bt), bt_stack_mask(bt));
			return -EFAULT;
		}

		for (i = last_idx;;) {
			if (skip_first) {
				/* starting_state->insn_idx 是尚未执行的当前指令，第一次不能反传它。 */
				err = 0;
				skip_first = false;
			} else {
				hist = get_jmp_hist_entry(st, history, i);
				err = backtrack_insn(env, i, subseq_idx, hist, bt);
			}
			if (err == -ENOTSUPP) {
				/* 无模型不等于程序非法；以全父链 precise 换取安全但较弱的剪枝。 */
				bpf_mark_all_scalars_precise(env, starting_state);
				bt_reset(bt);
				return 0;
			} else if (err) {
				return err;
			}
			if (bt_empty(bt))
				/* Found assignment(s) into tracked register in this state.
				 * Since this state is already marked, just return.
				 * Nothing to be tracked further in the parent state.
				 */
				/* 中文：所有需求已在本状态内部遇到定义点，父检查点无需再收紧。 */
				return 0;
			subseq_idx = i;
			i = get_prev_insn_idx(st, i, &history);
			if (i == -ENOENT)
				break;
			if (i >= env->prog->len) {
				/* This can happen if backtracking reached insn 0
				 * and there are still reg_mask or stack_mask
				 * to backtrack.
				 * It means the backtracking missed the spot where
				 * particular register was initialized with a constant.
				 */
				/* 中文：-ENOENT 使用负数哨兵；转换后出现超界说明依赖越过指令 0 仍未解析。 */
				verifier_bug(env, "backtracking idx %d", i);
				return -EFAULT;
			}
		}
		st = st->parent;
		/* parent 为空表示已越过整条检查点链，剩余需求稍后走保守兜底。 */
		if (!st)
			break;

		for (fr = bt->frame; fr >= 0; fr--) {
			/* 在每个父检查点提交本帧尚存需求，并清掉已精确或已不再是标量的位。 */
			func = st->frame[fr];
			bitmap_from_u64(mask, bt_frame_reg_mask(bt, fr));
			for_each_set_bit(i, mask, 32) {
				reg = &func->regs[i];
				if (reg->type != SCALAR_VALUE) {
					/* 类型变化截断标量依赖，防止把指针状态误标为 precise 标量。 */
					bt_clear_frame_reg(bt, fr, i);
					continue;
				}
				if (reg->precise) {
					/* 已经精确的父值是固定点边界，无需继续越过它追溯更老来源。 */
					bt_clear_frame_reg(bt, fr, i);
				} else {
					reg->precise = true;
					*changed = true;
				}
			}

			bitmap_from_u64(mask, bt_frame_stack_mask(bt, fr));
			for_each_set_bit(i, mask, 64) {
				/* 位图槽必须落在该父帧实际分配范围内，否则历史记录已损坏。 */
				if (verifier_bug_if(i >= func->allocated_stack / BPF_REG_SIZE,
						    env, "stack slot %d, total slots %d",
						    i, func->allocated_stack / BPF_REG_SIZE))
					return -EFAULT;

				if (!bpf_is_spilled_scalar_reg(&func->stack[i])) {
					/* 只有完整 spill 的标量保存了可比较的寄存器状态。 */
					bt_clear_frame_slot(bt, fr, i);
					continue;
				}
				reg = &func->stack[i].spilled_ptr;
				/* 已精确槽截断此依赖；否则提交新标记并保留位以继续追踪其更老定义。 */
				if (reg->precise) {
					bt_clear_frame_slot(bt, fr, i);
				} else {
					reg->precise = true;
					*changed = true;
				}
			}
			for (i = 0; i < func->out_stack_arg_cnt; i++) {
				/* 出栈参数使用独立数组；同样在非标量或已精确处终止反传。 */
				if (!bt_is_frame_stack_arg_slot_set(bt, fr, i))
					continue;
				reg = &func->stack_arg_regs[i];
				/* 已到非标量/精确边界便清位，新置 precise 时继续向更早状态追踪来源。 */
				if (reg->type != SCALAR_VALUE || reg->precise) {
					bt_clear_frame_stack_arg_slot(bt, fr, i);
				} else {
					reg->precise = true;
					*changed = true;
				}
			}
			if (env->log.level & BPF_LOG_LEVEL2) {
				/* 打印提交后仍要跨过此 parent 继续追踪的集合。 */
				fmt_reg_mask(env->tmp_str_buf, TMP_STR_BUF_LEN,
					     bt_frame_reg_mask(bt, fr));
				verbose(env, "mark_precise: frame%d: parent state regs=%s ",
					fr, env->tmp_str_buf);
				bpf_fmt_stack_mask(env->tmp_str_buf, TMP_STR_BUF_LEN,
					       bt_frame_stack_mask(bt, fr));
				/* print_verifier_state 补充完整寄存器/栈抽象值，便于诊断为何需求仍未清空。 */
				verbose(env, "stack=%s: ", env->tmp_str_buf);
				print_verifier_state(env, st, fr, true);
			}
		}

		if (bt_empty(bt))
			return 0;

		subseq_idx = first_idx;
		/* 跨检查点时，旧状态 last_insn_idx 是下一段逆行起点，first_idx 更新为其左边界。 */
		last_idx = st->last_insn_idx;
		first_idx = st->first_insn_idx;
	}

	/* if we still have requested precise regs or slots, we missed
	 * something (e.g., stack access through non-r10 register), so
	 * fallback to marking all precise
	 */
	/* 中文：例如经非 R10 指针访问栈时无法定位槽；剩余任何需求都必须用全标精确消除风险。 */
	if (!bt_empty(bt)) {
		bpf_mark_all_scalars_precise(env, starting_state);
		bt_reset(bt);
	}

	return 0;
}
