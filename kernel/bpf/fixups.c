// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/bpf_verifier.h>
#include <linux/filter.h>
#include <linux/vmalloc.h>
#include <linux/bsearch.h>
#include <linux/sort.h>
#include <linux/perf_event.h>
#include <net/xdp.h>
#include "disasm.h"

/* 修复阶段沿用验证器日志，错误信息仍进入同一用户可见缓冲区。 */
#define verbose(env, fmt, args...) bpf_verifier_log_write(env, fmt, ##args)

/* 精确识别会把旧内存值写入 R0 的原子 CMPXCHG 指令。 */
static bool is_cmpxchg_insn(const struct bpf_insn *insn)
{
	return BPF_CLASS(insn->code) == BPF_STX &&
	       BPF_MODE(insn->code) == BPF_ATOMIC &&
	       insn->imm == BPF_CMPXCHG;
}

/* Return the regno defined by the insn, or -1. */
/*
 * 中文：返回指令显式定义的寄存器号；纯控制流/store 返回 -1。
 * 原子指令的返回位置依操作种类为 R0、dst 或 src，供 zext/随机高位补丁选择目标。
 */
static int insn_def_regno(const struct bpf_insn *insn)
{
	switch (BPF_CLASS(insn->code)) {
	case BPF_JMP:
	case BPF_JMP32:
	case BPF_ST:
		return -1;
	case BPF_STX:
		/* 原子读改写可能同时是内存 store 和寄存器定义。 */
		if (BPF_MODE(insn->code) == BPF_ATOMIC ||
		    BPF_MODE(insn->code) == BPF_PROBE_ATOMIC) {
			/* 三种带返回值的原子形式各有不同的 ABI 目标寄存器。 */
			if (insn->imm == BPF_CMPXCHG)
				/* CMPXCHG 固定把观察到的旧值定义到 R0。 */
				return BPF_REG_0;
			else if (insn->imm == BPF_LOAD_ACQ)
				/* acquire load 把内存值定义到地址寄存器字段 dst。 */
				return insn->dst_reg;
			else if (insn->imm & BPF_FETCH)
				return insn->src_reg;
		}
		return -1;
	default:
		return insn->dst_reg;
	}
}

/* Return TRUE if INSN has defined any 32-bit value explicitly. */
/* 中文：先找定义寄存器，再询问通用指令语义其定义是否为 32 位；无定义直接为 false。 */
static bool insn_has_def32(struct bpf_insn *insn)
{
	int dst_reg = insn_def_regno(insn);

	if (dst_reg == -1)
		return false;

	return !bpf_is_reg64(insn, dst_reg, NULL, DST_OP);
}

/*
 * 按修复后的调用立即数、再按 insn->off 对 kfunc 描述符建立全序。
 * 返回值符合 sort/bsearch 比较器契约，两个键都相同才返回 0。
 */
static int kfunc_desc_cmp_by_imm_off(const void *a, const void *b)
{
	const struct bpf_kfunc_desc *d0 = a;
	const struct bpf_kfunc_desc *d1 = b;

	if (d0->imm != d1->imm)
		return d0->imm < d1->imm ? -1 : 1;
	if (d0->offset != d1->offset)
		return d0->offset < d1->offset ? -1 : 1;
	return 0;
}

/*
 * 在已排序的 kfunc 表中用调用指令的 imm/off 查找 JIT ABI 模型。
 * 返回指向 prog->aux 表内的借用指针，未登记则返回 NULL；调用期间程序持有表的生命周期。
 */
const struct btf_func_model *
bpf_jit_find_kfunc_model(const struct bpf_prog *prog,
			 const struct bpf_insn *insn)
{
	const struct bpf_kfunc_desc desc = {
		/* 搜索键严格复制调用指令最终的两个编码字段。 */
		.imm = insn->imm,
		.offset = insn->off,
	};
	const struct bpf_kfunc_desc *res;
	/* tab 由已经完成登记和排序的 prog->aux 持有。 */
	struct bpf_kfunc_desc_tab *tab;

	tab = prog->aux->kfunc_tab;
	res = bsearch(&desc, tab->descs, tab->nr_descs,
		      sizeof(tab->descs[0]), kfunc_desc_cmp_by_imm_off);

	return res ? &res->func_model : NULL;
}

/*
 * 把 kfunc 描述符的逻辑 func_id 转成 JIT 可编码的调用 immediate。
 * 支持远调用的架构保留 ID，否则计算相对 __bpf_call_base 的 s32 偏移；越界返回 -EINVAL。
 */
static int set_kfunc_desc_imm(struct bpf_verifier_env *env, struct bpf_kfunc_desc *desc)
{
	unsigned long call_imm;

	if (bpf_jit_supports_far_kfunc_call()) {
		/* 远调用 JIT 会自行解析 func_id，不受相对位移宽度限制。 */
		call_imm = desc->func_id;
	} else {
		call_imm = BPF_CALL_IMM(desc->addr);
		/* Check whether the relative offset overflows desc->imm */
		/* 中文：强转回 s32 后必须保持原无符号比特模式，否则指令字段容纳不下。 */
		if ((unsigned long)(s32)call_imm != call_imm) {
			verbose(env, "address of kernel func_id %u is out of range\n",
				desc->func_id);
			return -EINVAL;
		}
	}
	desc->imm = call_imm;
	return 0;
}

/*
 * 就地修复 prog->aux->kfunc_tab 中每个描述符的 imm，并按 imm/off 排序供 JIT 二分查找。
 * 无表视为成功；任一偏移不可编码立即返回错误，排序只在全部转换成功后发生。
 */
static int sort_kfunc_descs_by_imm_off(struct bpf_verifier_env *env)
{
	struct bpf_kfunc_desc_tab *tab;
	int i, err;

	tab = env->prog->aux->kfunc_tab;
	if (!tab)
		return 0;

	for (i = 0; i < tab->nr_descs; i++) {
		/* 转换就地发生；错误时尚未排序，加载主线会整体失败。 */
		err = set_kfunc_desc_imm(env, &tab->descs[i]);
		if (err)
			return err;
	}

	sort(tab->descs, tab->nr_descs, sizeof(tab->descs[0]),
	     kfunc_desc_cmp_by_imm_off, NULL);
	return 0;
}

/* 扫描一段新生成指令，把其中伪 kfunc 调用登记到 env；首个登记错误立即返回。 */
static int add_kfunc_in_insns(struct bpf_verifier_env *env,
			      struct bpf_insn *insn, int cnt)
{
	int i, ret;

	for (i = 0; i < cnt; i++, insn++) {
		/* 普通生成指令跳过，伪 kfunc 保留 imm/off 逻辑身份用于登记。 */
		if (bpf_pseudo_kfunc_call(insn)) {
			ret = bpf_add_kfunc_call(env, insn->imm, insn->off);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

#ifndef CONFIG_BPF_JIT_ALWAYS_ON
/*
 * 解释器回退时解析伪调用目标，并返回被调子程序的验证后栈深度。
 * 找不到目标表示内部 CFG/指令已失配，返回 -EFAULT。
 */
static int get_callee_stack_depth(struct bpf_verifier_env *env,
				  const struct bpf_insn *insn, int idx)
{
	int start = idx + insn->imm + 1, subprog;

	subprog = bpf_find_subprog(env, start);
	if (verifier_bug_if(subprog < 0, env, "get stack depth: no program at insn %d", start))
		return -EFAULT;
	return env->subprog_info[subprog].stack_depth;
}
#endif

/* single env->prog->insni[off] instruction was replaced with the range
 * insni[off, off + cnt).  Adjust corresponding insn_aux_data by copying
 * [0, off) and [off, end) to new locations, so the patched range stays zero
 */
/*
 * 中文：单条旧指令扩成 len 条后，同步搬移 insn_aux_data，使旧 aux 跟随补丁最后一条，新插入前缀
 * 保持清零；同时为每条补丁重算 seen/zext，并把 indirect_target 恢复到补丁首条。
 * 数组容量已由调用者扩展，本函数不分配且无失败返回。
 */
static void adjust_insn_aux_data(struct bpf_verifier_env *env,
				 struct bpf_prog *new_prog, u32 off, u32 cnt)
{
	struct bpf_insn_aux_data *data = env->insn_aux_data;
	struct bpf_insn *insn = new_prog->insnsi;
	u32 old_seen = data[off].seen;
	u32 prog_len;
	int i;

	/* aux info at OFF always needs adjustment, no matter fast path
	 * (cnt == 1) is taken or not. There is no guarantee INSN at OFF is the
	 * original insn at old prog.
	 */
	/* 中文：即使 len=1，补丁尾指令也可能不同，zext_dst 必须按新定义重新判定。 */
	data[off].zext_dst = insn_has_def32(insn + off + cnt - 1);

	if (cnt == 1)
		return;
	prog_len = new_prog->len;

	memmove(data + off + cnt - 1, data + off,
		sizeof(struct bpf_insn_aux_data) * (prog_len - off - cnt + 1));
	memset(data + off, 0, sizeof(struct bpf_insn_aux_data) * (cnt - 1));
	/* 新增前缀继承原指令的可达次数，但各自的 32 位定义属性按实际指令计算。 */
	for (i = off; i < off + cnt - 1; i++) {
		/* Expand insni[off]'s seen count to the patched range. */
		data[i].seen = old_seen;
		data[i].zext_dst = insn_has_def32(insn + i);
	}

	/*
	 * The indirect_target flag of the original instruction was moved to the last of the
	 * new instructions by the above memmove and memset, but the indirect jump target is
	 * actually the first instruction, so move it back. This also matches with the behavior
	 * of bpf_insn_array_adjust(), which preserves xlated_off to point to the first new
	 * instruction.
	 */
	/* 中文：间接跳转目标语义绑定补丁首地址，而不是携带旧 aux 的补丁尾。 */
	if (data[off + cnt - 1].indirect_target) {
		data[off].indirect_target = 1;
		data[off + cnt - 1].indirect_target = 0;
	}
}

/*
 * 单条指令扩为 len 条后，把 off 之后所有真实及伪 exit 子程序入口右移 len-1。
 * 位于 off 的入口仍指向补丁首条，len=1 无变化。
 */
static void adjust_subprog_starts(struct bpf_verifier_env *env, u32 off, u32 len)
{
	int i;

	if (len == 1)
		return;
	/* NOTE: fake 'exit' subprog should be updated as well. */
	/* 中文：循环包含 subprog_cnt 项，最后一项是程序尾的哨兵入口。 */
	for (i = 0; i <= env->subprog_cnt; i++) {
		if (env->subprog_info[i].start <= off)
			continue;
		env->subprog_info[i].start += len - 1;
	}
}

/* 通知所有 insn-array map：off 处一条指令被扩成 len 条，以保持 xlated_off 映射。 */
static void adjust_insn_arrays(struct bpf_verifier_env *env, u32 off, u32 len)
{
	int i;

	if (len == 1)
		return;

	for (i = 0; i < env->insn_array_map_cnt; i++)
		bpf_insn_array_adjust(env->insn_array_maps[i], off, len);
}

/* 通知所有 insn-array map 删除了 [off, off+len)，同步收缩其翻译偏移。 */
static void adjust_insn_arrays_after_remove(struct bpf_verifier_env *env, u32 off, u32 len)
{
	int i;

	for (i = 0; i < env->insn_array_map_cnt; i++)
		bpf_insn_array_adjust_after_remove(env->insn_array_maps[i], off, len);
}

/*
 * 补丁扩张后右移位于 off 之后的 JIT poke 描述符；off 本身仍指向补丁首条。
 * 表由 prog->aux 持有，本函数只原地更新索引。
 */
static void adjust_poke_descs(struct bpf_prog *prog, u32 off, u32 len)
{
	struct bpf_jit_poke_descriptor *tab = prog->aux->poke_tab;
	int i, sz = prog->aux->size_poke_tab;
	struct bpf_jit_poke_descriptor *desc;

	for (i = 0; i < sz; i++) {
		/* 描述符等于 off 时仍绑定补丁首条，只有严格位于其后者右移。 */
		desc = &tab[i];
		if (desc->insn_idx <= off)
			continue;
		desc->insn_idx += len - 1;
	}
}

/*
 * 用 patch[0..len) 替换 off 处单条指令，并原子化地同步 aux、子程序入口、insn-array 和 poke 索引。
 * 扩张时先 vrealloc aux；程序补丁失败返回 NULL（-ERANGE 另记日志）。成功返回可能重分配的新 prog，
 * 调用者必须发布 env->prog。注意 aux 扩张先于程序补丁，后者失败时容量可能已增长但仍有效。
 */
struct bpf_prog *bpf_patch_insn_data(struct bpf_verifier_env *env, u32 off,
				     const struct bpf_insn *patch, u32 len)
{
	struct bpf_prog *new_prog;
	struct bpf_insn_aux_data *new_data = NULL;

	if (len > 1) {
		/* __GFP_ZERO 保证新增 aux 项满足“补丁前缀从空元数据开始”的契约。 */
		new_data = vrealloc(env->insn_aux_data,
				    array_size(env->prog->len + len - 1,
					       sizeof(struct bpf_insn_aux_data)),
				    GFP_KERNEL_ACCOUNT | __GFP_ZERO);
		if (!new_data)
			return NULL;

		env->insn_aux_data = new_data;
	}

	new_prog = bpf_patch_insn_single(env->prog, off, patch, len);
	/* bpf_patch_insn_single 可能返回错误指针；本接口统一折叠成 NULL。 */
	if (IS_ERR(new_prog)) {
		if (PTR_ERR(new_prog) == -ERANGE)
			verbose(env,
				"insn %d cannot be patched due to 16-bit range\n",
				env->insn_aux_data[off].orig_idx);
		return NULL;
	}
	adjust_insn_aux_data(env, new_prog, off, len);
	/* 以下索引更新都基于同一个 off/len，顺序完成后元数据重新一致。 */
	adjust_subprog_starts(env, off, len);
	adjust_insn_arrays(env, off, len);
	adjust_poke_descs(new_prog, off, len);
	return new_prog;
}

/*
 * For all jmp insns in a given 'prog' that point to 'tgt_idx' insn adjust the
 * jump offset by 'delta'.
 */
/*
 * 中文：把所有原本跳到 tgt_idx 的普通跳转目标右移 delta；补丁区自身及 call/exit 不处理。
 * JA32 使用 32 位 imm，其余跳转使用 16 位 off；加法溢出返回 -ERANGE，之前已改条目不回滚。
 */
static int adjust_jmp_off(struct bpf_prog *prog, u32 tgt_idx, u32 delta)
{
	struct bpf_insn *insn = prog->insnsi;
	u32 insn_cnt = prog->len, i;
	s32 imm;
	s16 off;

	for (i = 0; i < insn_cnt; i++, insn++) {
		/* code 随指针逐条读取，i 用于重建“当前位置+1+偏移”的绝对目标。 */
		u8 code = insn->code;

		if (tgt_idx <= i && i < tgt_idx + delta)
			continue;

		if ((BPF_CLASS(code) != BPF_JMP && BPF_CLASS(code) != BPF_JMP32) ||
		    BPF_OP(code) == BPF_CALL || BPF_OP(code) == BPF_EXIT)
			continue;

		if (insn->code == (BPF_JMP32 | BPF_JA)) {
			/* JA32 的大范围无条件跳转把位移编码在 imm。 */
			if (i + 1 + insn->imm != tgt_idx)
				continue;
			if (check_add_overflow(insn->imm, delta, &imm))
				return -ERANGE;
			insn->imm = imm;
		} else {
			/* 传统跳转的 s16 off 需要单独做窄位宽溢出检测。 */
			if (i + 1 + insn->off != tgt_idx)
				continue;
			if (check_add_overflow(insn->off, delta, &off))
				return -ERANGE;
			insn->off = off;
		}
	}
	return 0;
}

/*
 * 删除 [off,off+cnt) 后，移除完全落入区间的真实子程序/func_info，并把存活入口左移 cnt。
 * fake exit 哨兵始终参与搬移；若只删某子程序前缀则保留其入口项并改到 off。
 */
static int adjust_subprog_starts_after_remove(struct bpf_verifier_env *env,
					      u32 off, u32 cnt)
{
	int i, j;

	/* find first prog starting at or after off (first to remove) */
	/* 中文：i 是候选删除入口，j 是删除区间之后首个存活入口。 */
	for (i = 0; i < env->subprog_cnt; i++)
		if (env->subprog_info[i].start >= off)
			break;
	/* find first prog starting at or after off + cnt (first to stay) */
	for (j = i; j < env->subprog_cnt; j++)
		if (env->subprog_info[j].start >= off + cnt)
			break;
	/* if j doesn't start exactly at off + cnt, we are just removing
	 * the front of previous prog
	 */
	/* 中文：右边界落在某子程序内部时，j-- 保留该跨界子程序的元数据。 */
	if (env->subprog_info[j].start != off + cnt)
		j--;

	if (j > i) {
		struct bpf_prog_aux *aux = env->prog->aux;
		int move;

		/* move fake 'exit' subprog as well */
		/* 中文：move 包含尾部哨兵，memmove 允许源/目的区间重叠。 */
		move = env->subprog_cnt + 1 - j;

		memmove(env->subprog_info + i,
			env->subprog_info + j,
			sizeof(*env->subprog_info) * move);
		/* j-i 个完整子程序被删除，新的计数与压缩数组一致。 */
		env->subprog_cnt -= j - i;

		/* remove func_info */
		/* BTF func_info 与真实子程序一一对应，不包含 fake exit。 */
		if (aux->func_info) {
			move = aux->func_info_cnt - j;

			memmove(aux->func_info + i,
				aux->func_info + j,
				sizeof(*aux->func_info) * move);
			/* func_info_cnt 与 subprog_cnt 删除相同数量的真实函数项。 */
			aux->func_info_cnt -= j - i;
			/* func_info->insn_off is set after all code rewrites,
			 * in adjust_btf_func() - no need to adjust
			 */
		}
	} else {
		/* convert i from "first prog to remove" to "first to adjust" */
		if (env->subprog_info[i].start == off)
			i++;
	}

	/* update fake 'exit' subprog as well */
	/* 中文：从首个保留项到尾哨兵统一减去删除长度。 */
	for (; i <= env->subprog_cnt; i++)
		env->subprog_info[i].start -= cnt;

	return 0;
}

/*
 * 删除指令区间后同步 BTF line_info：移除落入区间的记录、必要时让首条存活指令继承最后被删行号，
 * 再左移后续 insn_off 和各子程序 linfo_idx。函数原地压缩已分配数组，无内存失败。
 */
static int bpf_adj_linfo_after_remove(struct bpf_verifier_env *env, u32 off,
				      u32 cnt)
{
	struct bpf_prog *prog = env->prog;
	u32 i, l_off, l_cnt, nr_linfo;
	struct bpf_line_info *linfo;

	nr_linfo = prog->aux->nr_linfo;
	if (!nr_linfo)
		return 0;

	linfo = prog->aux->linfo;

	/* find first line info to remove, count lines to be removed */
	/* 中文：l_off 定位首个受影响记录，l_cnt 统计严格落在删除区间内的项。 */
	for (i = 0; i < nr_linfo; i++)
		if (linfo[i].insn_off >= off)
			break;

	l_off = i;
	l_cnt = 0;
	for (; i < nr_linfo; i++)
		if (linfo[i].insn_off < off + cnt)
			l_cnt++;
		else
			break;

	/* First live insn doesn't match first live linfo, it needs to "inherit"
	 * last removed linfo.  prog is already modified, so prog->len == off
	 * means no live instructions after (tail of the program was removed).
	 */
	/* 中文：把最后一个被删 linfo 改挂到右边界，随后整体左移后成为 off 的继承记录。 */
	if (prog->len != off && l_cnt &&
	    (i == nr_linfo || linfo[i].insn_off != off + cnt)) {
		l_cnt--;
		linfo[--i].insn_off = off + cnt;
	}

	/* remove the line info which refer to the removed instructions */
	/* 中文：压缩尾部并更新 nr_linfo，底层容量保留给 prog->aux 最终释放。 */
	if (l_cnt) {
		memmove(linfo + l_off, linfo + i,
			sizeof(*linfo) * (nr_linfo - i));

		prog->aux->nr_linfo -= l_cnt;
		nr_linfo = prog->aux->nr_linfo;
	}

	/* pull all linfo[i].insn_off >= off + cnt in by cnt */
	/* 中文：此时从 l_off 开始均为存活记录，绝对指令号统一收缩。 */
	for (i = l_off; i < nr_linfo; i++)
		linfo[i].insn_off -= cnt;

	/* fix up all subprogs (incl. 'exit') which start >= off */
	/* 中文：子程序若原首行被删则钳到 l_off，否则按实际删除行数左移。 */
	for (i = 0; i <= env->subprog_cnt; i++)
		if (env->subprog_info[i].linfo_idx > l_off) {
			/* program may have started in the removed region but
			 * may not be fully removed
			 */
			if (env->subprog_info[i].linfo_idx >= l_off + l_cnt)
				env->subprog_info[i].linfo_idx -= l_cnt;
			else
				env->subprog_info[i].linfo_idx = l_off;
		}

	return 0;
}

/*
 * Clean up dynamically allocated fields of aux data for instructions [start, ...]
 */
/*
 * 中文：释放指定指令区间内 aux 持有的跳转表并清 NULL；ldimm64 的第二槽随首槽一起跳过。
 * 必须在程序指令被移除前调用，因为识别双槽指令仍依赖旧 insnsi。
 */
void bpf_clear_insn_aux_data(struct bpf_verifier_env *env, int start, int len)
{
	struct bpf_insn_aux_data *aux_data = env->insn_aux_data;
	struct bpf_insn *insns = env->prog->insnsi;
	int end = start + len;
	int i;

	for (i = start; i < end; i++) {
		/* jt 是 aux 中本文件需要显式析构的动态字段。 */
		if (aux_data[i].jt) {
			kvfree(aux_data[i].jt);
			aux_data[i].jt = NULL;
		}

		if (bpf_is_ldimm64(&insns[i]))
			i++;
	}
}

/*
 * 从已验证程序删除连续指令，并同步 offload、动态 aux、子程序/BTF 行号、insn-array 及 aux 主数组。
 * 返回首个负 errno；部分外部元数据可能已更新，调用者把失败视为整个加载失败而非继续使用程序。
 */
static int verifier_remove_insns(struct bpf_verifier_env *env, u32 off, u32 cnt)
{
	struct bpf_insn_aux_data *aux_data = env->insn_aux_data;
	unsigned int orig_prog_len = env->prog->len;
	int err;

	if (bpf_prog_is_offloaded(env->prog->aux))
		bpf_prog_offload_remove_insns(env, off, cnt);

	/* Should be called before bpf_remove_insns, as it uses prog->insnsi */
	/* 中文：先释放被删 aux 的内部指针，再让底层程序数组发生搬移。 */
	bpf_clear_insn_aux_data(env, off, cnt);

	err = bpf_remove_insns(env->prog, off, cnt);
	if (err)
		return err;

	err = adjust_subprog_starts_after_remove(env, off, cnt);
	/* 子程序坐标先于 line-info 坐标更新，两者都使用旧删除区间。 */
	if (err)
		return err;

	err = bpf_adj_linfo_after_remove(env, off, cnt);
	if (err)
		return err;

	adjust_insn_arrays_after_remove(env, off, cnt);

	/* 程序长度已缩短，按旧长度计算尾部 aux 项数并覆盖删除窗口。 */
	memmove(aux_data + off,	aux_data + off + cnt,
		sizeof(*aux_data) * (orig_prog_len - off - cnt));

	return 0;
}

/* 标准 ja +0 占位指令，后续 remove_nops 会物理删除。 */
static const struct bpf_insn NOP = BPF_JMP_IMM(BPF_JA, 0, 0, 0);
/* 零偏移 may_goto 也可在语义固化后作为冗余控制流删除。 */
static const struct bpf_insn MAY_GOTO_0 = BPF_RAW_INSN(BPF_JMP | BPF_JCOND, 0, 0, 0, 0);

/* 判断 opcode 是否为读取条件并产生两后继的 JMP/JMP32，排除 JA、EXIT 和 CALL。 */
bool bpf_insn_is_cond_jump(u8 code)
{
	u8 op;

	op = BPF_OP(code);
	if (BPF_CLASS(code) == BPF_JMP32)
		return op != BPF_JA;

	if (BPF_CLASS(code) != BPF_JMP)
		return false;

	return op != BPF_JA && op != BPF_EXIT && op != BPF_CALL;
}

/*
 * 根据验证阶段的 seen 标记，把只有一条可达边的条件跳转原地改成 JA。
 * fall-through 未见则跳原目标，目标未见则 off=0；offload 程序同时通知设备替换。
 */
void bpf_opt_hard_wire_dead_code_branches(struct bpf_verifier_env *env)
{
	struct bpf_insn_aux_data *aux_data = env->insn_aux_data;
	struct bpf_insn ja = BPF_JMP_IMM(BPF_JA, 0, 0, 0);
	struct bpf_insn *insn = env->prog->insnsi;
	const int insn_cnt = env->prog->len;
	int i;

	for (i = 0; i < insn_cnt; i++, insn++) {
		/* 两个后继的 seen 来自完整验证；都可达时保留原条件。 */
		if (!bpf_insn_is_cond_jump(insn->code))
			continue;

		if (!aux_data[i + 1].seen)
			ja.off = insn->off;
		else if (!aux_data[i + 1 + insn->off].seen)
			ja.off = 0;
		else
			continue;

		if (bpf_prog_is_offloaded(env->prog->aux))
			/* 设备侧先接收等价 JA，再更新主程序内存副本。 */
			bpf_prog_offload_replace_insn(env, i, &ja);

		memcpy(insn, &ja, sizeof(ja));
	}
}

/*
 * 扫描 seen==0 的最大连续区间并交给统一删除器；每次删除后在同一 i 继续检查收缩后的新指令。
 * 成功返回 0，任一元数据/程序删除失败原样返回。
 */
int bpf_opt_remove_dead_code(struct bpf_verifier_env *env)
{
	struct bpf_insn_aux_data *aux_data = env->insn_aux_data;
	int insn_cnt = env->prog->len;
	int i, err;

	for (i = 0; i < insn_cnt; i++) {
		int j;

		j = 0;
		/* j 统计从 i 开始的连续不可达区间，避免逐条删除反复搬移。 */
		while (i + j < insn_cnt && !aux_data[i + j].seen)
			j++;
		if (!j)
			continue;

		err = verifier_remove_insns(env, i, j);
		if (err)
			return err;
		insn_cnt = env->prog->len;
		/* i 不前移：for 的 i++ 后指向原删除区间之后的首条。 */
	}

	return 0;
}

/*
 * 删除标准 NOP 和 may_goto +0；补丁后回退索引，以捕获删除形成的新相邻/零偏移组合。
 * 通过 verifier_remove_insns 保持全部索引元数据同步。
 */
int bpf_opt_remove_nops(struct bpf_verifier_env *env)
{
	struct bpf_insn *insn = env->prog->insnsi;
	int insn_cnt = env->prog->len;
	bool is_may_goto_0, is_ja;
	int i, err;

	for (i = 0; i < insn_cnt; i++) {
		is_may_goto_0 = !memcmp(&insn[i], &MAY_GOTO_0, sizeof(MAY_GOTO_0));
		/* 用完整结构比较，避免把带不同寄存器/立即数的合法指令误判为占位。 */
		is_ja = !memcmp(&insn[i], &NOP, sizeof(NOP));

		if (!is_may_goto_0 && !is_ja)
			continue;

		err = verifier_remove_insns(env, i, 1);
		if (err)
			return err;
		insn_cnt--;
		/* Go back one insn to catch may_goto +1; may_goto +0 sequence */
		/* 中文：may_goto 删除会改变前一跳的相对距离，额外回退一项重新归约。 */
		i -= (is_may_goto_0 && i > 0) ? 2 : 1;
	}

	return 0;
}

/*
 * 根据 JIT 要求在 32 位定义后插入显式 zext；测试模式还可给无需 zext 的 32 位结果随机化高 32 位，
 * 暴露错误依赖。每次补丁后刷新 prog/insns/aux 指针并累计 delta。返回 0、-ENOMEM 或内部 -EFAULT。
 */
int bpf_opt_subreg_zext_lo32_rnd_hi32(struct bpf_verifier_env *env,
					 const union bpf_attr *attr)
{
	struct bpf_insn *patch;
	/* use env->insn_buf as two independent buffers */
	/* 中文：前两槽用于原指令+zext，后四槽用于原指令+随机高位构造，生命周期限于 env。 */
	struct bpf_insn *zext_patch = env->insn_buf;
	struct bpf_insn *rnd_hi32_patch = &env->insn_buf[2];
	struct bpf_insn_aux_data *aux = env->insn_aux_data;
	/* delta 是此前补丁累计新增长度，len 始终保持原程序扫描上界。 */
	int i, patch_len, delta = 0, len = env->prog->len;
	struct bpf_insn *insns = env->prog->insnsi;
	struct bpf_prog *new_prog;
	bool rnd_hi32;

	rnd_hi32 = attr->prog_flags & BPF_F_TEST_RND_HI32;
	/* 预先填充补丁不随目标寄存器变化的部分，循环内只改 imm/dst/src。 */
	zext_patch[1] = BPF_ZEXT_REG(0);
	rnd_hi32_patch[1] = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, 0);
	rnd_hi32_patch[2] = BPF_ALU64_IMM(BPF_LSH, BPF_REG_AX, 32);
	rnd_hi32_patch[3] = BPF_ALU64_REG(BPF_OR, 0, BPF_REG_AX);
	for (i = 0; i < len; i++) {
		int adj_idx = i + delta;
		/* insn 按值复制，后续修改补丁模板不会提前碰当前程序。 */
		struct bpf_insn insn;
		int load_reg;

		insn = insns[adj_idx];
		load_reg = insn_def_regno(&insn);
		if (!aux[adj_idx].zext_dst) {
			/* 未要求 zext 时，仅测试标志开启才构造随机高半部。 */
			u8 code, class;
			u32 imm_rnd;

			if (!rnd_hi32)
				continue;

			code = insn.code;
			class = BPF_CLASS(code);
			if (load_reg == -1)
				continue;

			/* NOTE: arg "reg" (the fourth one) is only used for
			 *       BPF_STX + SRC_OP, so it is safe to pass NULL
			 *       here.
			 */
			/* 中文：这里以 DST_OP 查询，reg 参数不会被读取，传 NULL 符合助手契约。 */
			if (bpf_is_reg64(&insn, load_reg, NULL, DST_OP)) {
				if (class == BPF_LD &&
				    BPF_MODE(code) == BPF_IMM)
					i++;
				continue;
			}

			/* ctx load could be transformed into wider load. */
			/* 中文：上下文转换可能改变加载宽度，随机化必须等转换完成后才有可靠目标。 */
			if (class == BPF_LDX &&
			    aux[adj_idx].ptr_type == PTR_TO_CTX)
				continue;

			imm_rnd = get_random_u32();
			/* 随机值左移到高半部后 OR 入原 32 位定义结果。 */
			rnd_hi32_patch[0] = insn;
			rnd_hi32_patch[1].imm = imm_rnd;
			rnd_hi32_patch[3].dst_reg = load_reg;
			patch = rnd_hi32_patch;
			patch_len = 4;
			goto apply_patch_buffer;
		}

		/* Add in an zero-extend instruction if a) the JIT has requested
		 * it or b) it's a CMPXCHG.
		 *
		 * The latter is because: BPF_CMPXCHG always loads a value into
		 * R0, therefore always zero-extends. However some archs'
		 * equivalent instruction only does this load when the
		 * comparison is successful. This detail of CMPXCHG is
		 * orthogonal to the general zero-extension behaviour of the
		 * CPU, so it's treated independently of bpf_jit_needs_zext.
		 */
		/* 中文：CMPXCHG 的架构指令可能仅成功时自然零扩展，因此无论通用 JIT 标志都显式补齐。 */
		if (!bpf_jit_needs_zext() && !is_cmpxchg_insn(&insn))
			continue;

		/* Zero-extension is done by the caller. */
		/* 中文：伪 kfunc 调用的返回宽度由调用修复路径负责，避免重复插入。 */
		if (bpf_pseudo_kfunc_call(&insn))
			continue;

		if (verifier_bug_if(load_reg == -1, env,
				    "zext_dst is set, but no reg is defined"))
			return -EFAULT;

		zext_patch[0] = insn;
		/* 自寄存器 ZEXT 的 dst/src 都绑定本条实际定义寄存器。 */
		zext_patch[1].dst_reg = load_reg;
		zext_patch[1].src_reg = load_reg;
		patch = zext_patch;
		patch_len = 2;
apply_patch_buffer:
		/* patch 首条保留原指令，尾部完成 zext/随机化；统一补丁器同步所有索引。 */
		new_prog = bpf_patch_insn_data(env, adj_idx, patch, patch_len);
		if (!new_prog)
			return -ENOMEM;
		env->prog = new_prog;
		/* 补丁可能重分配两个数组，所有跨迭代借用都必须刷新。 */
		insns = new_prog->insnsi;
		aux = env->insn_aux_data;
		delta += patch_len - 1;
	}

	return 0;
}

/* convert load instructions that access fields of a context type into a
 * sequence of instructions that access fields of the underlying structure:
 *     struct __sk_buff    -> struct sk_buff
 *     struct bpf_sock_ops -> struct sock
 */
/*
 * 中文：把验证器接受的抽象上下文访问转换为程序类型/JIT 可执行的底层结构访问，同时插入 prologue、
 * epilogue、Spectre nospec 和 arena/BTF 探测模式。每次扩张后用 delta 映射原指令号到当前程序。
 * 成功发布可能重分配的 env->prog；生成器越界/元数据矛盾返回 -EFAULT，分配失败返回 -ENOMEM。
 */
int bpf_convert_ctx_accesses(struct bpf_verifier_env *env)
{
	struct bpf_subprog_info *subprogs = env->subprog_info;
	const struct bpf_verifier_ops *ops = env->ops;
	int i, cnt, size, ctx_field_size, ret, delta = 0, epilogue_cnt = 0;
	const int insn_cnt = env->prog->len;
	struct bpf_insn *epilogue_buf = env->epilogue_buf;
	/* 两个固定缓冲区由 env 独占，生成长度均须小于 INSN_BUF_SIZE。 */
	struct bpf_insn *insn_buf = env->insn_buf;
	struct bpf_insn *insn;
	u32 target_size, size_default, off;
	struct bpf_prog *new_prog;
	enum bpf_access_type type;
	bool is_narrower_load;
	int epilogue_idx = 0;
	/* epilogue_idx 为首次展开位置，零同时作为“尚未生成”哨兵。 */

	if (ops->gen_epilogue) {
		/* epilogue 使用主程序新增的一个栈槽保存原始 ctx，并可能包含需登记的 kfunc 调用。 */
		epilogue_cnt = ops->gen_epilogue(epilogue_buf, env->prog,
						 -(subprogs[0].stack_depth + 8));
		if (epilogue_cnt >= INSN_BUF_SIZE) {
			verifier_bug(env, "epilogue is too long");
			return -EFAULT;
		} else if (epilogue_cnt) {
			/* Save the ARG_PTR_TO_CTX for the epilogue to use */
			/* 中文：在原首指令前写 ctx 到新槽，补丁尾仍执行原首指令。 */
			cnt = 0;
			subprogs[0].stack_depth += 8;
			insn_buf[cnt++] = BPF_STX_MEM(BPF_DW, BPF_REG_FP, BPF_REG_1,
						      -subprogs[0].stack_depth);
			insn_buf[cnt++] = env->prog->insnsi[0];
			new_prog = bpf_patch_insn_data(env, 0, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;
			env->prog = new_prog;
			/* 保存 ctx 的单条新增使所有原指令当前位置整体右移一。 */
			delta += cnt - 1;

			ret = add_kfunc_in_insns(env, epilogue_buf, epilogue_cnt - 1);
			if (ret < 0)
				return ret;
		}
	}

	if (ops->gen_prologue || env->seen_direct_write) {
		/* 直接写上下文要求程序类型必须提供 prologue 转换器。 */
		if (!ops->gen_prologue) {
			verifier_bug(env, "gen_prologue is null");
			return -EFAULT;
		}
		cnt = ops->gen_prologue(insn_buf, env->seen_direct_write,
					env->prog);
		/* 生成器返回的最后一条应承载被替换的原入口语义。 */
		if (cnt >= INSN_BUF_SIZE) {
			verifier_bug(env, "prologue is too long");
			return -EFAULT;
		} else if (cnt) {
			new_prog = bpf_patch_insn_data(env, 0, insn_buf, cnt);
			/* prologue 失败不能继续使用尚未适配抽象 ctx 的程序。 */
			if (!new_prog)
				return -ENOMEM;

			env->prog = new_prog;
			delta += cnt - 1;

			ret = add_kfunc_in_insns(env, insn_buf, cnt - 1);
			if (ret < 0)
				return ret;
		}
	}

	if (delta)
		/* 首条扩张后，所有原先跳到入口的边应改指向真正原首指令。 */
		WARN_ON(adjust_jmp_off(env->prog, 0, delta));

	if (bpf_prog_is_offloaded(env->prog->aux))
		/* 设备后端自行处理上下文访问，主机不再做逐条转换。 */
		return 0;

	insn = env->prog->insnsi + delta;

	for (i = 0; i < insn_cnt; i++, insn++) {
		/* i 遍历原程序，insn/i+delta 指向当前扩张后的对应指令。 */
		bpf_convert_ctx_access_t convert_ctx_access;
		u8 mode;

		if (env->insn_aux_data[i + delta].nospec) {
			/* nospec 前栅栏与原指令组成两条补丁，随后仍允许对原指令继续转换。 */
			WARN_ON_ONCE(env->insn_aux_data[i + delta].alu_state);
			struct bpf_insn *patch = insn_buf;

		*patch++ = BPF_ST_NOSPEC();
		/* 前屏障后复制原指令，保证功能访问仍位于补丁尾。 */
			*patch++ = *insn;
			cnt = patch - insn_buf;
			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			/* This can not be easily merged with the
			 * nospec_result-case, because an insn may require a
			 * nospec before and after itself. Therefore also do not
			 * 'continue' here but potentially apply further
			 * patching to insn. *insn should equal patch[1] now.
			 */
			/* 中文：同一指令可能同时需要前后屏障，故这里不能 continue。 */
		}

		if (insn->code == (BPF_LDX | BPF_MEM | BPF_B) ||
		    insn->code == (BPF_LDX | BPF_MEM | BPF_H) ||
		    insn->code == (BPF_LDX | BPF_MEM | BPF_W) ||
		    insn->code == (BPF_LDX | BPF_MEM | BPF_DW) ||
		    insn->code == (BPF_LDX | BPF_MEMSX | BPF_B) ||
		    insn->code == (BPF_LDX | BPF_MEMSX | BPF_H) ||
		    insn->code == (BPF_LDX | BPF_MEMSX | BPF_W)) {
			/* 普通和符号扩展 load 的 B/H/W/DW 都归为上下文读。 */
			type = BPF_READ;
		} else if (insn->code == (BPF_STX | BPF_MEM | BPF_B) ||
			   insn->code == (BPF_STX | BPF_MEM | BPF_H) ||
			   insn->code == (BPF_STX | BPF_MEM | BPF_W) ||
			   insn->code == (BPF_STX | BPF_MEM | BPF_DW) ||
			   insn->code == (BPF_ST | BPF_MEM | BPF_B) ||
			   insn->code == (BPF_ST | BPF_MEM | BPF_H) ||
			   insn->code == (BPF_ST | BPF_MEM | BPF_W) ||
			   insn->code == (BPF_ST | BPF_MEM | BPF_DW)) {
			/* 寄存器/立即数的四种宽度 store 都归为上下文写。 */
			type = BPF_WRITE;
		} else if ((insn->code == (BPF_STX | BPF_ATOMIC | BPF_B) ||
			    insn->code == (BPF_STX | BPF_ATOMIC | BPF_H) ||
			    insn->code == (BPF_STX | BPF_ATOMIC | BPF_W) ||
			    insn->code == (BPF_STX | BPF_ATOMIC | BPF_DW)) &&
			   env->insn_aux_data[i + delta].ptr_type == PTR_TO_ARENA) {
			insn->code = BPF_STX | BPF_PROBE_ATOMIC | BPF_SIZE(insn->code);
			/* arena 原子访问可能 fault，改探测模式并为 JIT 异常表计数。 */
			env->prog->aux->num_exentries++;
			continue;
		} else if (insn->code == (BPF_JMP | BPF_EXIT) &&
			   epilogue_cnt &&
			   i + delta < subprogs[1].start) {
			/* Generate epilogue for the main prog */
			/* 中文：主程序首个 exit 展开完整 epilogue，后续 exit 用 JA32 复用它。 */
			if (epilogue_idx) {
				/* jump back to the earlier generated epilogue */
				insn_buf[0] = BPF_JMP32_A(epilogue_idx - i - delta - 1);
				cnt = 1;
			} else {
				/* 首次展开复制完整 epilogue；后续只生成回跳以共享代码。 */
			memcpy(insn_buf, epilogue_buf,
				       epilogue_cnt * sizeof(*epilogue_buf));
			cnt = epilogue_cnt;
			/* epilogue_buf 的最后一条包含原 exit 等价语义。 */
				/* epilogue_idx cannot be 0. It must have at
				 * least one ctx ptr saving insn before the
				 * epilogue.
				 */
				epilogue_idx = i + delta;
			}
			goto patch_insn_buf;
		} else {
			continue;
		}

		if (type == BPF_WRITE &&
		    env->insn_aux_data[i + delta].nospec_result) {
			/* nospec_result is only used to mitigate Spectre v4 and
			 * to limit verification-time for Spectre v1.
			 */
			/* 中文：写后屏障独立补在原指令之后，完成后无需再走上下文转换。 */
			struct bpf_insn *patch = insn_buf;

			*patch++ = *insn;
			/* 屏障位于补丁尾，保证 store 结果之后才抑制推测。 */
			*patch++ = BPF_ST_NOSPEC();
			cnt = patch - insn_buf;
			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			/* 新程序/指令指针刷新后直接 continue 到下一个原指令。 */
			env->prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			continue;
		}

		switch ((int)env->insn_aux_data[i + delta].ptr_type) {
		/* ptr_type 选择程序自定义转换器、公共 socket 转换器或直接探测模式。 */
		case PTR_TO_CTX:
			if (!ops->convert_ctx_access)
				continue;
			convert_ctx_access = ops->convert_ctx_access;
			break;
		case PTR_TO_SOCKET:
		case PTR_TO_SOCK_COMMON:
			/* 通用 socket 两类共享相同底层字段转换器。 */
			convert_ctx_access = bpf_sock_convert_ctx_access;
			break;
		case PTR_TO_TCP_SOCK:
			/* TCP/XDP socket 各有专用字段布局转换器。 */
			convert_ctx_access = bpf_tcp_sock_convert_ctx_access;
			break;
		case PTR_TO_XDP_SOCK:
			convert_ctx_access = bpf_xdp_sock_convert_ctx_access;
			break;
		case PTR_TO_BTF_ID:
		case PTR_TO_BTF_ID | PTR_UNTRUSTED:
		/* PTR_TO_BTF_ID | MEM_ALLOC always has a valid lifetime, unlike
		 * PTR_TO_BTF_ID, and an active referenced id, but the same cannot
		 * be said once it is marked PTR_UNTRUSTED, hence we must handle
		 * any faults for loads into such types. BPF_WRITE is disallowed
		 * for this case.
		 */
		case PTR_TO_BTF_ID | MEM_ALLOC | PTR_UNTRUSTED:
		case PTR_TO_MEM | MEM_RDONLY | PTR_UNTRUSTED:
			/* 这些不可信只读指针不做字段映射，只给原 load 增加 fault 恢复。 */
			if (type == BPF_READ) {
				/* 不可信对象只允许读，改写 opcode 后由异常表把 fault 转为安全结果。 */
				if (BPF_MODE(insn->code) == BPF_MEM)
					/* 保持原访问宽度，仅把 mode 切换为 probe 版本。 */
					insn->code = BPF_LDX | BPF_PROBE_MEM |
						     BPF_SIZE((insn)->code);
				else
				insn->code = BPF_LDX | BPF_PROBE_MEMSX |
						     BPF_SIZE((insn)->code);
				env->prog->aux->num_exentries++;
			}
			continue;
		case PTR_TO_ARENA:
			/* arena 地址宽度/故障语义使用专用 MEM32 模式，符号扩展还需 JIT 支持。 */
			if (BPF_MODE(insn->code) == BPF_MEMSX) {
				/* 符号扩展 arena load 需 JIT 能处理专用 MEM32SX opcode。 */
				if (!bpf_jit_supports_insn(insn, true)) {
					verbose(env, "sign extending loads from arena are not supported yet\n");
					return -EOPNOTSUPP;
				}
				insn->code = BPF_CLASS(insn->code) | BPF_PROBE_MEM32SX | BPF_SIZE(insn->code);
			} else {
				insn->code = BPF_CLASS(insn->code) | BPF_PROBE_MEM32 | BPF_SIZE(insn->code);
			}
			env->prog->aux->num_exentries++;
			/* 每次 opcode 探测化都对应一个 JIT 异常恢复条目。 */
			continue;
		default:
			continue;
		}

		ctx_field_size = env->insn_aux_data[i + delta].ctx_field_size;
		/* size 是源 BPF 访问宽度，ctx_field_size 是验证时识别的逻辑字段宽度。 */
		size = BPF_LDST_BYTES(insn);
		mode = BPF_MODE(insn->code);

		/* If the read access is a narrower load of the field,
		 * convert to a 4/8-byte load, to minimum program type specific
		 * convert_ctx_access changes. If conversion is successful,
		 * we will apply proper mask to the result.
		 */
		/* 中文：先扩到字段自然宽度交给转换器，再用移位/掩码恢复原窄读语义。 */
		is_narrower_load = size < ctx_field_size;
		size_default = bpf_ctx_off_adjust_machine(ctx_field_size);
		off = insn->off;
		if (is_narrower_load) {
			u8 size_code;

			if (type == BPF_WRITE) {
				verifier_bug(env, "narrow ctx access misconfigured");
				return -EFAULT;
			}

			size_code = BPF_H;
			/* 字段自然宽度只可能映射为 2/4/8 字节 load。 */
			if (ctx_field_size == 4)
				size_code = BPF_W;
			else if (ctx_field_size == 8)
				size_code = BPF_DW;

			insn->off = off & ~(size_default - 1);
			/* 对齐到底层字段起点，窄子字段位置留给后续 shift 计算。 */
			insn->code = BPF_LDX | BPF_MEM | size_code;
		}

		target_size = 0;
		cnt = convert_ctx_access(type, insn, insn_buf, env->prog,
					 &target_size);
		if (cnt == 0 || cnt >= INSN_BUF_SIZE ||
		    (ctx_field_size && !target_size)) {
			verifier_bug(env, "error during ctx access conversion (%d)", cnt);
			return -EFAULT;
		}
		/* target_size 是底层实际加载宽度；字段型访问必须由转换器明确返回。 */

		if (is_narrower_load && size < target_size) {
			/* 转换后仍比请求宽时追加截取；否则生成器已完成窄化。 */
			u8 shift = bpf_ctx_narrow_access_offset(
				off, size, size_default) * 8;
			if (shift && cnt + 1 >= INSN_BUF_SIZE) {
				verifier_bug(env, "narrow ctx load misconfigured");
				return -EFAULT;
			}
			if (ctx_field_size <= 4) {
				/* 32 位移位/AND 自动清高位，掩码保留请求的 size 字节。 */
				if (shift)
					insn_buf[cnt++] = BPF_ALU32_IMM(BPF_RSH,
									insn->dst_reg,
									shift);
				insn_buf[cnt++] = BPF_ALU32_IMM(BPF_AND, insn->dst_reg,
								(1 << size * 8) - 1);
			} else {
				/* 64 位字段先用 ALU64 右移，再用 ALU32 AND 提取至多 32 位窄值。 */
				if (shift)
					insn_buf[cnt++] = BPF_ALU64_IMM(BPF_RSH,
									insn->dst_reg,
									shift);
				insn_buf[cnt++] = BPF_ALU32_IMM(BPF_AND, insn->dst_reg,
								(1ULL << size * 8) - 1);
			}
		}
		if (mode == BPF_MEMSX)
			/* 最后追加带 off=位宽的 MOVSX，恢复原有符号扩展加载。 */
			insn_buf[cnt++] = BPF_RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_X,
						       insn->dst_reg, insn->dst_reg,
						       size * 8, 0);

patch_insn_buf:
		/* 所有转换路径在此统一替换，随后跳过刚插入的补丁继续下一个原指令。 */
		new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
		if (!new_prog)
			return -ENOMEM;

		delta += cnt - 1;

		/* keep walking new program and skip insns we just inserted */
		env->prog = new_prog;
		insn      = new_prog->insnsi + i + delta;
	}

	return 0;
}

/* 复制所有真实子程序起点，供常量盲化/JIT 失败时回滚；调用者负责 kvfree。 */
static u32 *bpf_dup_subprog_starts(struct bpf_verifier_env *env)
{
	u32 *starts = NULL;

	starts = kvmalloc_objs(u32, env->subprog_cnt, GFP_KERNEL_ACCOUNT);
	if (starts) {
		for (int i = 0; i < env->subprog_cnt; i++)
			starts[i] = env->subprog_info[i].start;
	}
	return starts;
}

/* 从备份恢复真实子程序起点，并用当前程序长度重建 fake exit 哨兵。 */
static void bpf_restore_subprog_starts(struct bpf_verifier_env *env, u32 *orig_starts)
{
	for (int i = 0; i < env->subprog_cnt; i++)
		env->subprog_info[i].start = orig_starts[i];
	/* restore the start of fake 'exit' subprog as well */
	env->subprog_info[env->subprog_cnt].start = env->prog->len;
}

/*
 * 为当前程序长度完整复制 insn_aux_data；使用 __vmalloc，失败返回 NULL。
 * 返回数组 ownership 交给调用者，主要用于 JIT 常量盲化回滚。
 */
struct bpf_insn_aux_data *bpf_dup_insn_aux_data(struct bpf_verifier_env *env)
{
	size_t size;
	void *new_aux;

	size = array_size(sizeof(struct bpf_insn_aux_data), env->prog->len);
	new_aux = __vmalloc(size, GFP_KERNEL_ACCOUNT);
	if (new_aux)
		memcpy(new_aux, env->insn_aux_data, size);
	return new_aux;
}

/* 释放扩张后的当前 aux 数组并把备份重新交还 env；新增项清零故无需逐项析构。 */
void bpf_restore_insn_aux_data(struct bpf_verifier_env *env,
			       struct bpf_insn_aux_data *orig_insn_aux)
{
	/* the expanded elements are zero-filled, so no special handling is required */
	vfree(env->insn_aux_data);
	env->insn_aux_data = orig_insn_aux;
}

/*
 * 把单一已验证程序拆成各子程序 JIT 镜像，分两轮编译：首轮取得地址，修复互调后次轮定稿。
 * 成功把 func 数组和主镜像发布到 prog->aux；失败注销 poke、释放所有子镜像并结束 JIT 尝试。
 */
static int jit_subprogs(struct bpf_verifier_env *env)
{
	/* func 中的子镜像最终作为一个共享数组由主程序 aux 管理。 */
	struct bpf_prog *prog = env->prog, **func, *tmp;
	int i, j, subprog_start, subprog_end = 0, len, subprog;
	struct bpf_map *map_ptr;
	struct bpf_insn *insn;
	void *old_bpf_func;
	int err, num_exentries;

	for (i = 0, insn = prog->insnsi; i < prog->len; i++, insn++) {
		/* 第一遍把相对目标解析成 off 中的 subprog id，并在 aux 保存原 imm 供回退。 */
		if (!bpf_pseudo_func(insn) && !bpf_pseudo_call(insn))
			continue;

		/* Upon error here we cannot fall back to interpreter but
		 * need a hard reject of the program. Thus -EFAULT is
		 * propagated in any case.
		 */
		/* 中文：目标缺失意味着验证器内部结构损坏，不能当作普通 JIT 不支持处理。 */
		subprog = bpf_find_subprog(env, i + insn->imm + 1);
		if (verifier_bug_if(subprog < 0, env, "No program to jit at insn %d",
				    i + insn->imm + 1))
			return -EFAULT;
		/* temporarily remember subprog id inside insn instead of
		 * aux_data, since next loop will split up all insns into funcs
		 */
		/* 中文：稍后程序会被拆分，off 随指令副本进入每个 func，aux 不会。 */
		insn->off = subprog;
		/* remember original imm in case JIT fails and fallback
		 * to interpreter will be needed
		 */
		/* 中文：call_imm 是解释器回退与最终可读 dump 的恢复来源。 */
		env->insn_aux_data[i].call_imm = insn->imm;
		/* point imm to __bpf_call_base+1 from JITs point of view */
		insn->imm = 1;
		if (bpf_pseudo_func(insn)) {
			/* 给首轮 JIT 一个接近最终镜像的占位地址，稳定其编码长度选择。 */
#if defined(MODULES_VADDR)
			u64 addr = MODULES_VADDR;
#else
			u64 addr = VMALLOC_START;
#endif
			/* jit (e.g. x86_64) may emit fewer instructions
			 * if it learns a u32 imm is the same as a u64 imm.
			 * Set close enough to possible prog address.
			 */
			insn[0].imm = (u32)addr;
			insn[1].imm = addr >> 32;
		}
	}

	err = bpf_prog_alloc_jited_linfo(prog);
	/* JIT 行号表由主程序持有，必须先于子镜像构造成功。 */
	if (err)
		goto out_undo_insn;

	err = -ENOMEM;
	func = kzalloc_objs(prog, env->subprog_cnt);
	if (!func)
		goto out_undo_insn;

	for (i = 0; i < env->subprog_cnt; i++) {
		/* 按相邻 start 切出一个独立 bpf_prog，并复制运行/JIT 所需共享元数据。 */
		subprog_start = subprog_end;
		subprog_end = env->subprog_info[i + 1].start;

		len = subprog_end - subprog_start;
		/* bpf_prog_run() doesn't call subprogs directly,
		 * hence main prog stats include the runtime of subprogs.
		 * subprogs don't have IDs and not reachable via prog_get_next_id
		 * func[i]->stats will never be accessed and stays NULL
		 */
		/* 中文：只有主程序统计覆盖整次运行，子程序镜像故意不单独分配 stats。 */
		func[i] = bpf_prog_alloc_no_stats(bpf_prog_size(len), GFP_USER);
		/* 分配的 bpf_prog 独立拥有指令存储，但大量 aux 字段仅借用主程序。 */
		if (!func[i])
			goto out_free;
		memcpy(func[i]->insnsi, &prog->insnsi[subprog_start],
		       len * sizeof(struct bpf_insn));
		func[i]->type = prog->type;
		/* 复制影响 JIT 输出和运行约束的程序级属性。 */
		func[i]->len = len;
		if (bpf_prog_calc_tag(func[i]))
			goto out_free;
		func[i]->is_func = 1;
		func[i]->sleepable = prog->sleepable;
		func[i]->blinded = prog->blinded;
		func[i]->aux->func_idx = i;
		/* Below members will be freed only at prog->aux */
		/* 中文：以下指针只借用主 prog->aux，失败释放子镜像时必须避免重复释放。 */
		func[i]->aux->btf = prog->aux->btf;
		/* BTF/func_info/poke 表仍由主程序释放，子 aux 只记录视图。 */
		func[i]->aux->subprog_start = subprog_start;
		/* subprog_start/linfo_idx 让共享行号表映射回本镜像局部坐标。 */
		func[i]->aux->func_info = prog->aux->func_info;
		func[i]->aux->func_info_cnt = prog->aux->func_info_cnt;
		func[i]->aux->poke_tab = prog->aux->poke_tab;
		/* poke 表稍后按绝对范围把 desc->aux 指回具体子镜像。 */
		func[i]->aux->size_poke_tab = prog->aux->size_poke_tab;
		func[i]->aux->main_prog_aux = prog->aux;

		for (j = 0; j < prog->aux->size_poke_tab; j++) {
			/* 根据原绝对区间把每个 poke 的回调 aux 指向对应子镜像。 */
			struct bpf_jit_poke_descriptor *poke;

		poke = &prog->aux->poke_tab[j];
		/* 半开区间 [subprog_start,subprog_end) 唯一确定描述符归属。 */
			if (poke->insn_idx < subprog_end &&
			    poke->insn_idx >= subprog_start)
				poke->aux = func[i]->aux;
		}

		func[i]->aux->name[0] = 'F';
		func[i]->aux->stack_depth = env->subprog_info[i].stack_depth;
		if (env->subprog_info[i].priv_stack_mode == PRIV_STACK_ADAPTIVE)
			func[i]->aux->jits_use_priv_stack = true;

		func[i]->jit_requested = 1;
		/* 下面复制 JIT 查表、行号、arena、map 与安全属性。 */
		func[i]->blinding_requested = prog->blinding_requested;
		func[i]->aux->kfunc_tab = prog->aux->kfunc_tab;
		func[i]->aux->kfunc_btf_tab = prog->aux->kfunc_btf_tab;
		func[i]->aux->linfo = prog->aux->linfo;
		func[i]->aux->nr_linfo = prog->aux->nr_linfo;
		/* jited_linfo 也是主程序统一分配，所有子 aux 共享底层数组。 */
		func[i]->aux->jited_linfo = prog->aux->jited_linfo;
		func[i]->aux->linfo_idx = env->subprog_info[i].linfo_idx;
		func[i]->aux->arena = prog->aux->arena;
		func[i]->aux->used_maps = env->used_maps;
		func[i]->aux->used_map_cnt = env->used_map_cnt;
		num_exentries = 0;
		/* 每个子镜像重新统计会生成异常表项的 probe load/store/atomic。 */
		insn = func[i]->insnsi;
		for (j = 0; j < func[i]->len; j++, insn++) {
			/* 每个 probe 类访问在本子镜像异常表中占一个恢复项。 */
			if (BPF_CLASS(insn->code) == BPF_LDX &&
			    (BPF_MODE(insn->code) == BPF_PROBE_MEM ||
			     BPF_MODE(insn->code) == BPF_PROBE_MEM32 ||
			     BPF_MODE(insn->code) == BPF_PROBE_MEM32SX ||
			     BPF_MODE(insn->code) == BPF_PROBE_MEMSX))
				num_exentries++;
		if ((BPF_CLASS(insn->code) == BPF_STX ||
			     BPF_CLASS(insn->code) == BPF_ST) &&
		    BPF_MODE(insn->code) == BPF_PROBE_MEM32)
			/* arena 探测 store 也需要 fault fixup 表项。 */
				num_exentries++;
			if (BPF_CLASS(insn->code) == BPF_STX &&
			     BPF_MODE(insn->code) == BPF_PROBE_ATOMIC)
				num_exentries++;
		}
		func[i]->aux->num_exentries = num_exentries;
		/* 验证阶段按子程序汇总的副作用标记转交运行时/JIT aux。 */
		func[i]->aux->tail_call_reachable = env->subprog_info[i].tail_call_reachable;
		func[i]->aux->exception_cb = env->subprog_info[i].is_exception_cb;
		func[i]->aux->changes_pkt_data = env->subprog_info[i].changes_pkt_data;
		func[i]->aux->might_sleep = env->subprog_info[i].might_sleep;
		func[i]->aux->token = prog->aux->token;
		if (!i)
			func[i]->aux->exception_boundary = env->seen_exception;
		func[i] = bpf_int_jit_compile(env, func[i]);
		/* 首轮必须真正 JIT；否则整个多函数 JIT 回退，而非混用解释器。 */
		if (!func[i]->jited) {
			err = -ENOTSUPP;
			goto out_free;
		}
		cond_resched();
	}

	/* at this point all bpf functions were successfully JITed
	 * now populate all bpf_calls with correct addresses and
	 * run last pass of JIT
	 */
	/* 中文：地址现已稳定，把伪函数常量和伪调用改成具体镜像地址/相对调用。 */
	for (i = 0; i < env->subprog_cnt; i++) {
		insn = func[i]->insnsi;
		/* 此轮只改每个子镜像内部的伪地址/调用，不再改变指令长度。 */
		for (j = 0; j < func[i]->len; j++, insn++) {
			if (bpf_pseudo_func(insn)) {
				subprog = insn->off;
				insn[0].imm = (u32)(long)func[subprog]->bpf_func;
				insn[1].imm = ((u64)(long)func[subprog]->bpf_func) >> 32;
				continue;
			}
			if (!bpf_pseudo_call(insn))
				/* 普通指令和已处理伪函数不需要调用地址修复。 */
				continue;
			subprog = insn->off;
			insn->imm = BPF_CALL_IMM(func[subprog]->bpf_func);
		}

		/* we use the aux data to keep a list of the start addresses
		 * of the JITed images for each function in the program
		 *
		 * for some architectures, such as powerpc64, the imm field
		 * might not be large enough to hold the offset of the start
		 * address of the callee's JITed image from __bpf_call_base
		 *
		 * in such cases, we can lookup the start address of a callee
		 * by using its subprog id, available from the off field of
		 * the call instruction, as an index for this list
		 */
		/* 中文：func 数组也为短位移不足的架构提供按 subprog id 查地址的旁路表。 */
		func[i]->aux->func = func;
		func[i]->aux->func_cnt = env->subprog_cnt - env->hidden_subprog_cnt;
		func[i]->aux->real_func_cnt = env->subprog_cnt;
	}
	for (i = 0; i < env->subprog_cnt; i++) {
		old_bpf_func = func[i]->bpf_func;
		/* 第二轮只允许原对象原入口就地定稿；地址变化会让已修复调用失效。 */
		tmp = bpf_int_jit_compile(env, func[i]);
		if (tmp != func[i] || func[i]->bpf_func != old_bpf_func) {
			verbose(env, "JIT doesn't support bpf-to-bpf calls\n");
			err = -ENOTSUPP;
			goto out_free;
		}
		cond_resched();
	}

	/*
	 * Cleanup func[i]->aux fields which aren't required
	 * or can become invalid in future
	 */
	/* 中文：JIT 完成后子镜像不再借用 used_maps，防止主程序变化留下悬空引用。 */
	for (i = 0; i < env->subprog_cnt; i++) {
		func[i]->aux->used_maps = NULL;
		func[i]->aux->used_map_cnt = 0;
	}

	/* finally lock prog and jit images for all functions and
	 * populate kallsysm. Begin at the first subprogram, since
	 * bpf_prog_load will add the kallsyms for the main program.
	 */
	/* 中文：主程序由加载主线加符号，其余子镜像先只读锁定再逐一发布。 */
	for (i = 1; i < env->subprog_cnt; i++) {
		err = bpf_prog_lock_ro(func[i]);
		if (err)
			goto out_free;
	}

	for (i = 1; i < env->subprog_cnt; i++)
		/* 锁定成功后才向 kallsyms 发布，避免暴露仍可写镜像。 */
		bpf_prog_kallsyms_add(func[i]);

	/* Last step: make now unused interpreter insns from main
	 * prog consistent for later dump requests, so they can
	 * later look the same as if they were interpreted only.
	 */
	/* 中文：恢复主程序逻辑 imm/off，使用户 dump 与纯解释执行形式一致。 */
	for (i = 0, insn = prog->insnsi; i < prog->len; i++, insn++) {
		/* 主程序只用于 dump/可能回退，恢复逻辑目标而非机器地址。 */
		if (bpf_pseudo_func(insn)) {
			insn[0].imm = env->insn_aux_data[i].call_imm;
			insn[1].imm = insn->off;
			insn->off = 0;
			continue;
		}
		if (!bpf_pseudo_call(insn))
			continue;
		insn->imm = env->insn_aux_data[i].call_imm;
		/* off 保留 subprog id，便于 dump 消费者识别逻辑调用目标。 */
		subprog = bpf_find_subprog(env, i + insn->imm + 1);
		insn->off = subprog;
	}

	/* 成功提交点：主 prog 借用 func[0] 入口/异常表，并取得 func 数组 ownership。 */
	prog->jited = 1;
	prog->bpf_func = func[0]->bpf_func;
	prog->jited_len = func[0]->jited_len;
	prog->aux->extable = func[0]->aux->extable;
	prog->aux->num_exentries = func[0]->aux->num_exentries;
	prog->aux->func = func;
	/* func_cnt 排除隐藏子程序的用户可见计数，real_func_cnt 保留 JIT 实际数量。 */
	prog->aux->func_cnt = env->subprog_cnt - env->hidden_subprog_cnt;
	prog->aux->real_func_cnt = env->subprog_cnt;
	prog->aux->bpf_exception_cb = (void *)func[env->exception_callback_subprog]->bpf_func;
	prog->aux->exception_boundary = func[0]->aux->exception_boundary;
	prog->aux->stack_arg_sp_adjust = func[0]->aux->stack_arg_sp_adjust;
	bpf_prog_jit_attempt_done(prog);
	return 0;
out_free:
	/* We failed JIT'ing, so at this point we need to unregister poke
	 * descriptors from subprogs, so that kernel is not attempting to
	 * patch it anymore as we're freeing the subprog JIT memory.
	 */
	/* 中文：先让每个 map 停止动态 patch，再释放可能已生成的可执行镜像。 */
	for (i = 0; i < prog->aux->size_poke_tab; i++) {
		map_ptr = prog->aux->poke_tab[i].tail_call.map;
		map_ptr->ops->map_poke_untrack(map_ptr, prog->aux);
	}
	/* At this point we're guaranteed that poke descriptors are not
	 * live anymore. We can just unlink its descriptor table as it's
	 * released with the main prog.
	 */
	/* 中文：清 NULL 避免 bpf_jit_free 重复释放主程序统一持有的 poke_tab。 */
	for (i = 0; i < env->subprog_cnt; i++) {
		/* kzalloc 保证未创建项为 NULL，可安全跳过部分构造结果。 */
		if (!func[i])
			continue;
		func[i]->aux->poke_tab = NULL;
		bpf_jit_free(func[i]);
	}
	kfree(func);
out_undo_insn:
	bpf_prog_jit_attempt_done(prog);
	return err;
}

/*
 * 多子程序 JIT 的事务包装：必要时先备份 aux/start 并常量盲化，再调用 jit_subprogs。
 * 普通不支持可回退解释器，-EFAULT 必须硬失败；盲化失败时恢复原 prog 与两份元数据。
 */
int bpf_jit_subprogs(struct bpf_verifier_env *env)
{
	int err, i;
	bool blinded = false;
	struct bpf_insn *insn;
	struct bpf_prog *prog, *orig_prog;
	struct bpf_insn_aux_data *orig_insn_aux;
	u32 *orig_subprog_starts;

	if (env->subprog_cnt <= 1)
		/* 无 bpf-to-bpf 调用时主程序由普通单体 JIT 路径处理。 */
		return 0;

	prog = orig_prog = env->prog;
	if (bpf_prog_need_blind(prog)) {
		/* 盲化会扩张指令并改索引，因此 aux 和 subprog starts 必须成对备份。 */
		orig_insn_aux = bpf_dup_insn_aux_data(env);
		/* 任一备份失败都在尚未盲化时退出，不需恢复程序内容。 */
		if (!orig_insn_aux) {
			err = -ENOMEM;
			goto out_cleanup;
		}
		orig_subprog_starts = bpf_dup_subprog_starts(env);
		/* start 备份使用 kvalloc，aux 备份使用 vmalloc，释放接口不同。 */
		if (!orig_subprog_starts) {
			/* 第二份备份失败时立即释放已成功的 aux 副本。 */
			vfree(orig_insn_aux);
			err = -ENOMEM;
			goto out_cleanup;
		}
		prog = bpf_jit_blind_constants(env, prog);
		/* 盲化器成功时同时把 env->prog 指向扩张副本。 */
		if (IS_ERR(prog)) {
			/* 盲化失败未产生可用副本，回滚入口仍以 orig_prog 为准。 */
			err = -ENOMEM;
			prog = orig_prog;
			goto out_restore;
		}
		blinded = true;
	}

	err = jit_subprogs(env);
	if (err)
		goto out_jit_err;

	if (blinded) {
		/* 成功后保留盲化程序，释放原程序以及只为回滚准备的副本。 */
		bpf_jit_prog_release_other(prog, orig_prog);
		kvfree(orig_subprog_starts);
		vfree(orig_insn_aux);
	}

	return 0;

out_jit_err:
	if (blinded) {
		/* 失败后释放盲化副本，重新发布干净原程序。 */
		bpf_jit_prog_release_other(orig_prog, prog);
		/* roll back to the clean original prog */
		prog = env->prog = orig_prog;
		goto out_restore;
	} else {
		if (err != -EFAULT) {
			/*
			 * We will fall back to interpreter mode when err is not -EFAULT, before
			 * that, insn->off and insn->imm should be restored to their original
			 * values since they were modified by jit_subprogs.
			 */
			/* 中文：非盲化普通失败恢复 call imm/off，随后允许解释器接管。 */
			for (i = 0, insn = prog->insnsi; i < prog->len; i++, insn++) {
				if (!bpf_pseudo_call(insn))
					continue;
				insn->off = 0;
				insn->imm = env->insn_aux_data[i].call_imm;
			}
		}
		goto out_cleanup;
	}

	out_restore:
	/* start 与 aux 必须在同一原程序坐标系一起恢复。 */
	bpf_restore_subprog_starts(env, orig_subprog_starts);
	bpf_restore_insn_aux_data(env, orig_insn_aux);
	kvfree(orig_subprog_starts);
out_cleanup:
	/* cleanup main prog to be interpreted */
	/* 中文：清除两项请求标志，防止后续误认为已有可执行 JIT 镜像。 */
	prog->jit_requested = 0;
	prog->blinding_requested = 0;
	return err;
}

/*
 * 校验出栈参数容量并优先尝试多子程序 JIT；若允许解释器回退，则拒绝解释器不支持的 kfunc、栈参数、
 * callback 及 tail-call+子程序组合，并把伪调用 imm 修成解释器需要的 callee 栈深度。
 */
int bpf_fixup_call_args(struct bpf_verifier_env *env)
{
#ifndef CONFIG_BPF_JIT_ALWAYS_ON
	struct bpf_prog *prog = env->prog;
	/* 解释器回退专用变量在 ALWAYS_ON 配置下完全不编译。 */
	struct bpf_insn *insn = prog->insnsi;
	bool has_kfunc_call = bpf_prog_has_kfunc_call(prog);
	int depth;
#endif
	int i, err = 0;

	for (i = 0; i < env->subprog_cnt; i++) {
		/* 最大实际写槽数不得超过所有调用所需的 outgoing 容量。 */
		struct bpf_subprog_info *subprog = &env->subprog_info[i];
		u16 outgoing = subprog->stack_arg_cnt - bpf_in_stack_arg_cnt(subprog);

		if (subprog->max_out_stack_arg_cnt > outgoing) {
			/* 写出超过任一调用布局会覆盖未保留栈区，必须拒绝。 */
			verbose(env,
				"func#%d writes %u stack arg slots, but calls only require %u\n",
				i, subprog->max_out_stack_arg_cnt, outgoing);
			return -EINVAL;
		}
	}

	if (env->prog->jit_requested &&
	    !bpf_prog_is_offloaded(env->prog->aux)) {
		err = bpf_jit_subprogs(env);
		if (err == 0)
			/* JIT 成功已完成调用地址和多函数镜像修复。 */
			return 0;
		if (err == -EFAULT)
			/* 内部一致性错误不能降级成解释器执行。 */
			return err;
	}
#ifndef CONFIG_BPF_JIT_ALWAYS_ON
	/* 以下限制只存在于内核保留解释器的配置。 */
	if (has_kfunc_call) {
		/* kfunc 没有解释器调用 ABI，只能依赖成功 JIT。 */
		verbose(env, "calling kernel functions are not allowed in non-JITed programs\n");
		return -EINVAL;
	}
	for (i = 0; i < env->subprog_cnt; i++) {
		/* R11 出栈参数同样只由 JIT 调用约定支持。 */
		if (bpf_in_stack_arg_cnt(&env->subprog_info[i])) {
			verbose(env, "stack args are not supported in non-JITed programs\n");
			return -EINVAL;
		}
	}
	if (env->subprog_cnt > 1 && env->prog->aux->tail_call_reachable) {
		/* When JIT fails the progs with bpf2bpf calls and tail_calls
		 * have to be rejected, since interpreter doesn't support them yet.
		 */
		/* 中文：解释器尚不能安全组合 bpf-to-bpf 与 tail call。 */
		verbose(env, "tail_calls are not allowed in non-JITed programs with bpf-to-bpf calls\n");
		return -EINVAL;
	}
	for (i = 0; i < prog->len; i++, insn++) {
		if (bpf_pseudo_func(insn)) {
			/* When JIT fails the progs with callback calls
			 * have to be rejected, since interpreter doesn't support them yet.
			 */
			/* 中文：伪函数地址用于回调，解释器没有相应函数指针实现。 */
			verbose(env, "callbacks are not allowed in non-JITed programs\n");
			return -EINVAL;
		}

		if (!bpf_pseudo_call(insn))
			continue;
		depth = get_callee_stack_depth(env, insn, i);
		/* 目标查找使用尚未改写的相对 imm 和当前绝对指令号。 */
		/* 每个伪调用携带 callee 栈深度，解释器据此切换栈帧。 */
		if (depth < 0)
			return depth;
		/* bpf_patch_call_args 只改当前伪调用，不改变程序长度。 */
		err = bpf_patch_call_args(insn, depth);
		/* 超出解释器编码上限时保留原错误并拒绝加载。 */
		if (err) {
			verbose(env, "stack depth %d exceeds interpreter stack depth limit\n",
				depth);
			return err;
		}
	}
	err = 0;
#endif
	return err;
}


/* The function requires that first instruction in 'patch' is insnsi[prog->len - 1] */
/*
 * 中文：在程序尾哨兵前追加唯一隐藏子程序；patch[0] 必须复制原最后指令以符合单条替换接口。
 * 成功更新 prog、真实/fake-exit start 和两个计数；失败返回 -ENOMEM 或唯一性 -EFAULT。
 */
static int add_hidden_subprog(struct bpf_verifier_env *env, struct bpf_insn *patch, int len)
{
	struct bpf_subprog_info *info = env->subprog_info;
	int cnt = env->subprog_cnt;
	struct bpf_prog *prog;

	/* We only reserve one slot for hidden subprogs in subprog_info. */
	/* 中文：subprog_info 只预留一个隐藏槽，重复追加会越界。 */
	if (env->hidden_subprog_cnt) {
		verifier_bug(env, "only one hidden subprog supported");
		return -EFAULT;
	}
	/* We're not patching any existing instruction, just appending the new
	 * ones for the hidden subprog. Hence all of the adjustment operations
	 * in bpf_patch_insn_data are no-ops.
	 */
	/* 中文：替换旧尾指令并在其后追加其余 patch，原有索引无需右移。 */
	prog = bpf_patch_insn_data(env, env->prog->len - 1, patch, len);
	if (!prog)
		return -ENOMEM;
	env->prog = prog;
	/* 原 fake-exit start 先搬到新尾，再把旧尾位置变成隐藏入口。 */
	info[cnt + 1].start = info[cnt].start;
	info[cnt].start = prog->len - len + 1;
	env->subprog_cnt++;
	env->hidden_subprog_cnt++;
	return 0;
}

/* Do various post-verification rewrites in a single program pass.
 * These rewrites simplify JIT and interpreter implementations.
 */
/*
 * 中文：在一次原指令扫描中完成异常回调、地址空间转换、除法保护、探测访问、Spectre 屏蔽、
 * may_goto、kfunc/helper 内联及调用地址修复；之后初始化额外栈槽并发布 poke/kfunc 表。
 * 每次指令扩张都更新 delta 和所有借用指针。返回生成器/登记错误；已做补丁不在本函数内回滚。
 */
int bpf_do_misc_fixups(struct bpf_verifier_env *env)
{
	/* prog/attach/type 决定可用的转换器和 helper 内联方案。 */
	struct bpf_prog *prog = env->prog;
	enum bpf_attach_type eatype = prog->expected_attach_type;
	enum bpf_prog_type prog_type = resolve_prog_type(prog);
	struct bpf_insn *insn = prog->insnsi;
	const struct bpf_func_proto *fn;
	const int insn_cnt = prog->len;
	const struct bpf_map_ops *ops;
	struct bpf_insn_aux_data *aux;
	struct bpf_insn *insn_buf = env->insn_buf;
	/* new_prog 接收每次可能重分配的结果，map_ptr/ops 仅在确定 helper 分支借用。 */
	struct bpf_prog *new_prog;
	struct bpf_map *map_ptr;
	int i, ret, cnt, delta = 0, cur_subprog = 0;
	struct bpf_subprog_info *subprogs = env->subprog_info;
	u16 stack_depth = subprogs[cur_subprog].stack_depth;
	/* stack_depth_extra 只在当前子程序发现 may_goto 时增长，并在边界提交。 */
	u16 stack_depth_extra = 0;

	if (env->seen_exception && !env->exception_callback_subprog) {
		/* 未显式提供异常回调时追加隐藏的“返回参数 R1”默认子程序。 */
		struct bpf_insn *patch = insn_buf;

		*patch++ = env->prog->insnsi[insn_cnt - 1];
		*patch++ = BPF_MOV64_REG(BPF_REG_0, BPF_REG_1);
		*patch++ = BPF_EXIT_INSN();
		ret = add_hidden_subprog(env, insn_buf, patch - insn_buf);
		if (ret < 0)
			return ret;
		prog = env->prog;
		insn = prog->insnsi;

		env->exception_callback_subprog = env->subprog_cnt - 1;
		/* Don't update insn_cnt, as add_hidden_subprog always appends insns */
		/* 中文：主循环只处理原程序，隐藏尾部稍后由子程序元数据/JIT 单独识别。 */
		bpf_mark_subprog_exc_cb(env, env->exception_callback_subprog);
	}

	for (i = 0; i < insn_cnt;) {
		/* i 是原指令序号，insn 指向当前扩张程序中 i+delta 的对应指令。 */
		if (insn->code == (BPF_ALU64 | BPF_MOV | BPF_X) && insn->imm) {
			if ((insn->off == BPF_ADDR_SPACE_CAST && insn->imm == 1) ||
			    (((struct bpf_map *)env->prog->aux->arena)->map_flags & BPF_F_NO_USER_CONV)) {
				/* convert to 32-bit mov that clears upper 32-bit */
				/* 中文：允许的 arena 地址空间转换用 wX=wY 清高位，JIT 看作普通 MOV32。 */
				insn->code = BPF_ALU | BPF_MOV | BPF_X;
				/* clear off and imm, so it's a normal 'wX = wY' from JIT pov */
				insn->off = 0;
				insn->imm = 0;
			} /* cast from as(0) to as(1) should be handled by JIT */
			/* 中文：用户到 arena 的另一方向仍保留标记，交给架构 JIT 实现。 */
			goto next_insn;
		}

		if (env->insn_aux_data[i + delta].needs_zext)
			/* Convert BPF_CLASS(insn->code) == BPF_ALU64 to 32-bit ALU */
			/* 中文：验证器证明只需低 32 位时直接改类，统一获得零扩展语义。 */
			insn->code = BPF_ALU | BPF_OP(insn->code) | BPF_SRC(insn->code);

		/* Make sdiv/smod divide-by-minus-one exceptions impossible. */
		/* 中文：立即数 -1 的有符号除/模可直接化为 NEG 或零，避开硬件溢出异常。 */
		if ((insn->code == (BPF_ALU64 | BPF_MOD | BPF_K) ||
		     insn->code == (BPF_ALU64 | BPF_DIV | BPF_K) ||
		     insn->code == (BPF_ALU | BPF_MOD | BPF_K) ||
		     insn->code == (BPF_ALU | BPF_DIV | BPF_K)) &&
		    insn->off == 1 && insn->imm == -1) {
			bool is64 = BPF_CLASS(insn->code) == BPF_ALU64;
			bool isdiv = BPF_OP(insn->code) == BPF_DIV;
			struct bpf_insn *patch = insn_buf;

			if (isdiv)
				/* x / -1 等价 -x，最小负数按 BPF 二补码语义保持自身。 */
				*patch++ = BPF_RAW_INSN((is64 ? BPF_ALU64 : BPF_ALU) |
							BPF_NEG | BPF_K, insn->dst_reg,
							0, 0, 0);
			else
				*patch++ = BPF_MOV32_IMM(insn->dst_reg, 0);

			cnt = patch - insn_buf;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			/* 四条计数补丁成功后刷新当前位置并进入统一子程序边界处理。 */
			/* 即使替换长度为一，也通过统一接口重算 aux.zext_dst。 */
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Make divide-by-zero and divide-by-minus-one exceptions impossible. */
		/* 中文：寄存器除数需要运行时分支，显式实现除零及有符号 -1 的 BPF 规定结果。 */
		if (insn->code == (BPF_ALU64 | BPF_MOD | BPF_X) ||
		    insn->code == (BPF_ALU64 | BPF_DIV | BPF_X) ||
		    insn->code == (BPF_ALU | BPF_MOD | BPF_X) ||
		    insn->code == (BPF_ALU | BPF_DIV | BPF_X)) {
			bool is64 = BPF_CLASS(insn->code) == BPF_ALU64;
			bool isdiv = BPF_OP(insn->code) == BPF_DIV;
			bool is_sdiv = isdiv && insn->off == 1;
			bool is_smod = !isdiv && insn->off == 1;
			struct bpf_insn *patch = insn_buf;

			if (is_sdiv) {
				/* [R,W]x sdiv 0 -> 0
				 * LLONG_MIN sdiv -1 -> LLONG_MIN
				 * INT_MIN sdiv -1 -> INT_MIN
				 */
				/* 中文：用 src+1 的无符号范围同时识别 0/-1，其他值执行原 sdiv。 */
				*patch++ = BPF_MOV64_REG(BPF_REG_AX, insn->src_reg);
				/* AX=src+1：对 0 得 1，对 -1 得 0，便于两次条件跳转分流。 */
				*patch++ = BPF_RAW_INSN((is64 ? BPF_ALU64 : BPF_ALU) |
							BPF_ADD | BPF_K, BPF_REG_AX,
							0, 0, 1);
			*patch++ = BPF_RAW_INSN((is64 ? BPF_JMP : BPF_JMP32) |
							BPF_JGT | BPF_K, BPF_REG_AX,
							0, 4, 1);
			/* AX>1 表示原除数既非 -1 也非 0，可直接跳向原运算。 */
				*patch++ = BPF_RAW_INSN((is64 ? BPF_JMP : BPF_JMP32) |
							BPF_JEQ | BPF_K, BPF_REG_AX,
							0, 1, 0);
				*patch++ = BPF_RAW_INSN((is64 ? BPF_ALU64 : BPF_ALU) |
							BPF_MOV | BPF_K, insn->dst_reg,
							0, 0, 0);
				/* 除数为零写 dst=0，-1 分支随后对原 dst 取负。 */
				/* BPF_NEG(LLONG_MIN) == -LLONG_MIN == LLONG_MIN */
				*patch++ = BPF_RAW_INSN((is64 ? BPF_ALU64 : BPF_ALU) |
							BPF_NEG | BPF_K, insn->dst_reg,
							0, 0, 0);
				*patch++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
				*patch++ = *insn;
				cnt = patch - insn_buf;
			} else if (is_smod) {
				/* [R,W]x mod 0 -> [R,W]x */
				/* [R,W]x mod -1 -> 0 */
				/* 中文：零除数保留被除数，-1 产生零，其他值执行原 smod。 */
				*patch++ = BPF_MOV64_REG(BPF_REG_AX, insn->src_reg);
				/* smod 同样以 src+1 区分 -1，并单独识别原 src 为零。 */
				*patch++ = BPF_RAW_INSN((is64 ? BPF_ALU64 : BPF_ALU) |
							BPF_ADD | BPF_K, BPF_REG_AX,
							0, 0, 1);
				*patch++ = BPF_RAW_INSN((is64 ? BPF_JMP : BPF_JMP32) |
							BPF_JGT | BPF_K, BPF_REG_AX,
							0, 3, 1);
				*patch++ = BPF_RAW_INSN((is64 ? BPF_JMP : BPF_JMP32) |
							BPF_JEQ | BPF_K, BPF_REG_AX,
							0, 3 + (is64 ? 0 : 1), 1);
				*patch++ = BPF_MOV32_IMM(insn->dst_reg, 0);
				/* -1 分支写零并越过原 smod；普通分支落入原指令。 */
				*patch++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
				*patch++ = *insn;

				if (!is64) {
					/* smod32 的零除数路径需显式清除 dst 高 32 位。 */
					*patch++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
					*patch++ = BPF_MOV32_REG(insn->dst_reg, insn->dst_reg);
				}
				cnt = patch - insn_buf;
			} else if (isdiv) {
				/* [R,W]x div 0 -> 0 */
				/* 中文：无符号除法仅需检查零；非零路径落到原指令。 */
				*patch++ = BPF_RAW_INSN((is64 ? BPF_JMP : BPF_JMP32) |
							BPF_JNE | BPF_K, insn->src_reg,
							0, 2, 0);
				*patch++ = BPF_ALU32_REG(BPF_XOR, insn->dst_reg, insn->dst_reg);
				/* XOR 自身是最短的写零方式，随后 JA 跳过原 div。 */
				*patch++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
				*patch++ = *insn;
				cnt = patch - insn_buf;
			} else {
				/* [R,W]x mod 0 -> [R,W]x */
				/* 中文：无符号模零跳过原运算；ALU32 额外自 MOV 保证高位清零。 */
				*patch++ = BPF_RAW_INSN((is64 ? BPF_JMP : BPF_JMP32) |
							BPF_JEQ | BPF_K, insn->src_reg,
							0, 1 + (is64 ? 0 : 1), 0);
				*patch++ = *insn;

				if (!is64) {
					/* mod32 跳过运算时仍需兑现 ALU32 的隐式零扩展。 */
					*patch++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
					*patch++ = BPF_MOV32_REG(insn->dst_reg, insn->dst_reg);
				}
				cnt = patch - insn_buf;
			}

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			/* 扩张成功后 delta 增加 cnt-1，当前 insn 重定位到补丁尾。 */
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Make it impossible to de-reference a userspace address */
		/* 中文：probe load 在架构给出用户地址上界时先检查高 32 位，越界返回零而不解引用。 */
		if (BPF_CLASS(insn->code) == BPF_LDX &&
		    (BPF_MODE(insn->code) == BPF_PROBE_MEM ||
		     BPF_MODE(insn->code) == BPF_PROBE_MEMSX)) {
			struct bpf_insn *patch = insn_buf;
			u64 uaddress_limit = bpf_arch_uaddress_limit();

			if (!uaddress_limit)
				/* 架构不要求额外屏蔽时保留原探测指令。 */
				goto next_insn;

			*patch++ = BPF_MOV64_REG(BPF_REG_AX, insn->src_reg);
			/* AX 计算含 insn->off 的实际地址并与 uaddress_limit 高半部比较。 */
			if (insn->off)
				*patch++ = BPF_ALU64_IMM(BPF_ADD, BPF_REG_AX, insn->off);
			*patch++ = BPF_ALU64_IMM(BPF_RSH, BPF_REG_AX, 32);
			*patch++ = BPF_JMP_IMM(BPF_JLE, BPF_REG_AX, uaddress_limit >> 32, 2);
			/* 内核地址才执行 probe load；用户区地址跨过加载并把 dst 置零。 */
			*patch++ = *insn;
			*patch++ = BPF_JMP_IMM(BPF_JA, 0, 0, 1);
			/* 地址合法的 load 完成后跳过末尾“dst=0”替代结果。 */
			*patch++ = BPF_MOV64_IMM(insn->dst_reg, 0);

			cnt = patch - insn_buf;
			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;
			/* classic load 补丁成功后当前位置重定向到序列末端。 */

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Implement LD_ABS and LD_IND with a rewrite, if supported by the program type. */
		/* 中文：经典包访问由程序类型生成显式现代 BPF 序列，生成长度必须落入共享缓冲区。 */
		if (BPF_CLASS(insn->code) == BPF_LD &&
		    (BPF_MODE(insn->code) == BPF_ABS ||
		     BPF_MODE(insn->code) == BPF_IND)) {
			cnt = env->ops->gen_ld_abs(insn, insn_buf);
			/* 程序类型负责给出与其 skb/context ABI 对应的完整替代序列。 */
			if (cnt == 0 || cnt >= INSN_BUF_SIZE) {
				verifier_bug(env, "%d insns generated for ld_abs", cnt);
				return -EFAULT;
			}

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;
			/* timed may_goto 的七条序列已占用本子程序统一的两槽状态。 */

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Rewrite pointer arithmetic to mitigate speculation attacks. */
		/* 中文：对验证器标记的指针算术生成掩码，把推测越界 offset 压成安全值。 */
		if (insn->code == (BPF_ALU64 | BPF_ADD | BPF_X) ||
		    insn->code == (BPF_ALU64 | BPF_SUB | BPF_X)) {
			const u8 code_add = BPF_ALU64 | BPF_ADD | BPF_X;
			const u8 code_sub = BPF_ALU64 | BPF_SUB | BPF_X;
			struct bpf_insn *patch = insn_buf;
			bool issrc, isneg, isimm;
			u32 off_reg;

			aux = &env->insn_aux_data[i + delta];
			/* alu_state 为零/非指针表示无需 Spectre v1 消毒。 */
			if (!aux->alu_state ||
			    aux->alu_state == BPF_ALU_NON_POINTER)
				goto next_insn;

			isneg = aux->alu_state & BPF_ALU_NEG_VALUE;
			/* 三个标志决定 offset 在 src/dst、符号方向以及是否可直接用常量 limit。 */
			issrc = (aux->alu_state & BPF_ALU_SANITIZE) ==
				BPF_ALU_SANITIZE_SRC;
			isimm = aux->alu_state & BPF_ALU_IMMEDIATE;

			off_reg = issrc ? insn->src_reg : insn->dst_reg;
			if (isimm) {
				/* 立即数偏移已由验证器证明范围，AX 直接装上界掩码。 */
				*patch++ = BPF_MOV32_IMM(BPF_REG_AX, aux->alu_limit);
			} else {
				/* 变量偏移以 limit-off|off 的符号构造全零/全一有效性掩码。 */
				if (isneg)
					*patch++ = BPF_ALU64_IMM(BPF_MUL, off_reg, -1);
				*patch++ = BPF_MOV32_IMM(BPF_REG_AX, aux->alu_limit);
				*patch++ = BPF_ALU64_REG(BPF_SUB, BPF_REG_AX, off_reg);
				*patch++ = BPF_ALU64_REG(BPF_OR, BPF_REG_AX, off_reg);
				*patch++ = BPF_ALU64_IMM(BPF_NEG, BPF_REG_AX, 0);
				*patch++ = BPF_ALU64_IMM(BPF_ARSH, BPF_REG_AX, 63);
			*patch++ = BPF_ALU64_REG(BPF_AND, BPF_REG_AX, off_reg);
			/* AND 后 AX 只可能是安全 offset 或零，成为最终指针运算源。 */
			}
			if (!issrc)
				/* 指针在 src 时先交换到 dst，保持最终 ALU 的寄存器角色。 */
				*patch++ = BPF_MOV64_REG(insn->dst_reg, insn->src_reg);
			insn->src_reg = BPF_REG_AX;
			if (isneg)
				/* 负 offset 通过 ADD/SUB 对换保持原数学表达式。 */
				insn->code = insn->code == code_add ?
					     code_sub : code_add;
			*patch++ = *insn;
			if (issrc && isneg && !isimm)
				/* 临时取反了源 offset 时在原寄存器中恢复，保持后续程序状态。 */
				*patch++ = BPF_ALU64_IMM(BPF_MUL, off_reg, -1);
			cnt = patch - insn_buf;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;
			/* 普通 may_goto 补丁复用同一子程序计数槽。 */

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		if (bpf_is_may_goto_insn(insn) && bpf_jit_supports_timed_may_goto()) {
			/* 支持计时版本时为每个子程序保留计数和起始时间两个 8 字节槽。 */
			int stack_off_cnt = -stack_depth - 16;

			/*
			 * Two 8 byte slots, depth-16 stores the count, and
			 * depth-8 stores the start timestamp of the loop.
			 *
			 * The starting value of count is BPF_MAX_TIMED_LOOPS
			 * (0xffff).  Every iteration loads it and subs it by 1,
			 * until the value becomes 0 in AX (thus, 1 in stack),
			 * after which we call arch_bpf_timed_may_goto, which
			 * either sets AX to 0xffff to keep looping, or to 0
			 * upon timeout. AX is then stored into the stack. In
			 * the next iteration, we either see 0 and break out, or
			 * continue iterating until the next time value is 0
			 * after subtraction, rinse and repeat.
			 */
			/* 中文：快速计数耗尽时调用架构时钟助手续期或超时退出，避免每轮读取时间。 */
			stack_depth_extra = 16;
			insn_buf[0] = BPF_LDX_MEM(BPF_DW, BPF_REG_AX, BPF_REG_10, stack_off_cnt);
			if (insn->off >= 0)
				/* 正/负目标因补丁插入位置不同，分别修正跳过序列的相对偏移。 */
				insn_buf[1] = BPF_JMP_IMM(BPF_JEQ, BPF_REG_AX, 0, insn->off + 5);
			else
				insn_buf[1] = BPF_JMP_IMM(BPF_JEQ, BPF_REG_AX, 0, insn->off - 1);
			insn_buf[2] = BPF_ALU64_IMM(BPF_SUB, BPF_REG_AX, 1);
			insn_buf[3] = BPF_JMP_IMM(BPF_JNE, BPF_REG_AX, 0, 2);
			/*
			 * AX is used as an argument to pass in stack_off_cnt
			 * (to add to r10/fp), and also as the return value of
			 * the call to arch_bpf_timed_may_goto.
			 */
			/* 中文：AX 先传递栈槽偏移，调用后复用为新计数并写回同一槽。 */
			insn_buf[4] = BPF_MOV64_IMM(BPF_REG_AX, stack_off_cnt);
			insn_buf[5] = BPF_EMIT_CALL(arch_bpf_timed_may_goto);
			insn_buf[6] = BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_AX, stack_off_cnt);
			/* 最后一条写回 0/新批次计数，下一轮首先读取并决定退出。 */
			cnt = 7;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta += cnt - 1;
			env->prog = prog = new_prog;
			insn = new_prog->insnsi + i + delta;
			goto next_insn;
		} else if (bpf_is_may_goto_insn(insn)) {
			/* 无计时 JIT 支持时只用单槽固定 BPF_MAX_LOOPS 计数。 */
			int stack_off = -stack_depth - 8;

			stack_depth_extra = 8;
			/* 每轮零值退出，否则减一写回；四条序列替代原 may_goto。 */
			insn_buf[0] = BPF_LDX_MEM(BPF_DW, BPF_REG_AX, BPF_REG_10, stack_off);
			if (insn->off >= 0)
				insn_buf[1] = BPF_JMP_IMM(BPF_JEQ, BPF_REG_AX, 0, insn->off + 2);
			else
				insn_buf[1] = BPF_JMP_IMM(BPF_JEQ, BPF_REG_AX, 0, insn->off - 1);
			insn_buf[2] = BPF_ALU64_IMM(BPF_SUB, BPF_REG_AX, 1);
			insn_buf[3] = BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_AX, stack_off);
			/* 固定计数版本每次无条件写回递减值。 */
			cnt = 4;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta += cnt - 1;
			env->prog = prog = new_prog;
			insn = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		if (insn->code != (BPF_JMP | BPF_CALL))
			goto next_insn;
		if (insn->src_reg == BPF_PSEUDO_CALL)
			/* bpf-to-bpf 调用留给后续 JIT/解释器专门路径。 */
			goto next_insn;
		if (insn->src_reg == BPF_PSEUDO_KFUNC_CALL) {
			/* kfunc 修复器可能原地修 imm 或生成替代序列；cnt=0 表示无需扩张。 */
			ret = bpf_fixup_kfunc_call(env, insn, insn_buf, i + delta, &cnt);
			/* kfunc 修复可能登记 BTF/异常信息，错误不能退化为普通 helper。 */
			if (ret)
				return ret;
			if (cnt == 0)
				goto next_insn;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta	 += cnt - 1;
			env->prog = prog = new_prog;
			insn	  = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Skip inlining the helper call if the JIT does it. */
		/* 中文：架构承诺内联的 helper 保留逻辑 ID，供 JIT 识别。 */
		if (bpf_jit_inlines_helper_call(insn->imm))
			goto next_insn;

		if (insn->imm == BPF_FUNC_get_route_realm)
			/* 记录运行路径需要 skb dst，供程序执行包装准备上下文。 */
			prog->dst_needed = 1;
		if (insn->imm == BPF_FUNC_get_prandom_u32)
			/* 首次使用随机 helper 时初始化用户随机源，后续调用为一次性快路。 */
			bpf_user_rnd_init_once();
		if (insn->imm == BPF_FUNC_override_return)
			prog->kprobe_override = 1;
		if (insn->imm == BPF_FUNC_tail_call) {
			/* If we tail call into other programs, we
			 * cannot make any assumptions since they can
			 * be replaced dynamically during runtime in
			 * the program array.
			 */
			/* 中文：program array 可动态替换目标，尾调用后的上下文/包偏移必须按最坏情况处理。 */
			prog->cb_access = 1;
			if (!bpf_allow_tail_call_in_subprogs(env))
				prog->aux->stack_depth = MAX_BPF_STACK;
			prog->aux->max_pkt_offset = MAX_PACKET_OFF;

			/* mark bpf_tail_call as different opcode to avoid
			 * conditional branch in the interpreter for every normal
			 * call and to prevent accidental JITing by JIT compiler
			 * that doesn't support bpf_tail_call yet
			 */
			/* 中文：专用 opcode 让解释器/JIT 显式选择尾调用实现，避免误作普通 helper。 */
			insn->imm = 0;
			insn->code = BPF_JMP | BPF_TAIL_CALL;

			aux = &env->insn_aux_data[i + delta];
			/* 特权、未盲化且 map/key 均确定时，可登记运行时直接 patch 描述符。 */
			if (env->bpf_capable && !prog->blinding_requested &&
			    prog->jit_requested &&
			    !bpf_map_key_poisoned(aux) &&
			    !bpf_map_ptr_poisoned(aux) &&
			    !bpf_map_ptr_unpriv(aux)) {
			struct bpf_jit_poke_descriptor desc = {
				/* 描述符绑定确定的 map/key 与当前最终指令坐标。 */
					.reason = BPF_POKE_REASON_TAIL_CALL,
					.tail_call.map = aux->map_ptr_state.map_ptr,
					.tail_call.key = bpf_map_key_immediate(aux),
					.insn_idx = i + delta,
				};

			ret = bpf_jit_add_poke_descriptor(prog, &desc);
			/* 返回索引加一写入 imm，零仍保留“无描述符”哨兵语义。 */
				if (ret < 0) {
					verbose(env, "adding tail call poke descriptor failed\n");
					return ret;
				}

				insn->imm = ret + 1;
				goto next_insn;
			}

			if (!bpf_map_ptr_unpriv(aux))
				/* 非非特权路径无需额外索引屏蔽，保留专用 opcode。 */
				goto next_insn;

			/* instead of changing every JIT dealing with tail_call
			 * emit two extra insns:
			 * if (index >= max_entries) goto out;
			 * index &= array->index_mask;
			 * to avoid out-of-bounds cpu speculation
			 */
			/* 中文：非特权索引先做 max_entries 边界判断，再与 index_mask 相与以阻断推测越界。 */
			if (bpf_map_ptr_poisoned(aux)) {
				verbose(env, "tail_call abusing map_ptr\n");
				return -EINVAL;
			}

			map_ptr = aux->map_ptr_state.map_ptr;
			/* map_ptr 已验证为唯一且非 poison，可安全读取固定容量和数组掩码。 */
			insn_buf[0] = BPF_JMP_IMM(BPF_JGE, BPF_REG_3,
						  map_ptr->max_entries, 2);
			insn_buf[1] = BPF_ALU32_IMM(BPF_AND, BPF_REG_3,
						    container_of(map_ptr,
								 struct bpf_array,
								 map)->index_mask);
			insn_buf[2] = *insn;
			/* 第三条仍是专用 tail-call opcode，前两条只负责索引消毒。 */
			cnt = 3;
			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		if (insn->imm == BPF_FUNC_timer_set_callback) {
			/* The verifier will process callback_fn as many times as necessary
			 * with different maps and the register states prepared by
			 * set_timer_callback_state will be accurate.
			 *
			 * The following use case is valid:
			 *   map1 is shared by prog1, prog2, prog3.
			 *   prog1 calls bpf_timer_init for some map1 elements
			 *   prog2 calls bpf_timer_set_callback for some map1 elements.
			 *     Those that were not bpf_timer_init-ed will return -EINVAL.
			 *   prog3 calls bpf_timer_start for some map1 elements.
			 *     Those that were not both bpf_timer_init-ed and
			 *     bpf_timer_set_callback-ed will return -EINVAL.
			 */
			/* 中文：把当前 prog->aux 作为隐藏第三参数传给 timer helper，运行时据此绑定回调程序。 */
			struct bpf_insn ld_addrs[2] = {
				/* 双槽 ldimm64 承载内核指针，仅出现在验证后修复程序。 */
				BPF_LD_IMM64(BPF_REG_3, (long)prog->aux),
			};

			insn_buf[0] = ld_addrs[0];
			insn_buf[1] = ld_addrs[1];
			insn_buf[2] = *insn;
			/* 原 timer helper 移到补丁尾，随后统一 patch_call_imm。 */
			cnt = 3;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			/* 扩张改变后续坐标，goto 前刷新当前原 helper 指针。 */
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto patch_call_imm;
		}

		/* bpf_per_cpu_ptr() and bpf_this_cpu_ptr() */
		if (env->insn_aux_data[i + delta].call_with_percpu_alloc_ptr) {
			/* patch with 'r1 = *(u64 *)(r1 + 0)' since for percpu data,
			 * bpf_mem_alloc() returns a ptr to the percpu data ptr.
			 */
			/* 中文：内存分配器返回“指向 per-cpu 指针的指针”，调用 helper 前先解一层。 */
			insn_buf[0] = BPF_LDX_MEM(BPF_DW, BPF_REG_1, BPF_REG_1, 0);
			insn_buf[1] = *insn;
			/* 解引用与原 helper 形成两条序列，调用地址仍在公共标签修复。 */
			cnt = 2;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta += cnt - 1;
			env->prog = prog = new_prog;
			insn = new_prog->insnsi + i + delta;
			goto patch_call_imm;
		}

		/* BPF_EMIT_CALL() assumptions in some of the map_gen_lookup
		 * and other inlining handlers are currently limited to 64 bit
		 * only.
		 */
		/* 中文：仅 64 位 JIT 尝试 map 操作内联/直连，其他平台走统一 helper 地址修复。 */
		if (prog->jit_requested && BITS_PER_LONG == 64 &&
			/* 仅列出的 map helper 具有与 ops 原型精确匹配的直连方案。 */
		    (insn->imm == BPF_FUNC_map_lookup_elem ||
		     insn->imm == BPF_FUNC_map_update_elem ||
		     insn->imm == BPF_FUNC_map_delete_elem ||
		     insn->imm == BPF_FUNC_map_push_elem   ||
		     insn->imm == BPF_FUNC_map_pop_elem    ||
		     insn->imm == BPF_FUNC_map_peek_elem   ||
		     insn->imm == BPF_FUNC_redirect_map    ||
		     insn->imm == BPF_FUNC_for_each_map_elem ||
		     insn->imm == BPF_FUNC_map_lookup_percpu_elem)) {
			aux = &env->insn_aux_data[i + delta];
			/* 多路径合并导致 map 指针 poison 时不能选择某个具体 ops。 */
			if (bpf_map_ptr_poisoned(aux))
				goto patch_call_imm;

			map_ptr = aux->map_ptr_state.map_ptr;
			/* map 引用由 prog 持有，ops 在整个修复/JIT 生命周期稳定。 */
			ops = map_ptr->ops;
			if (insn->imm == BPF_FUNC_map_lookup_elem &&
			    ops->map_gen_lookup) {
				cnt = ops->map_gen_lookup(map_ptr, insn_buf);
				/* map 自定义生成器可拒绝内联，再回退到其通用函数指针。 */
				if (cnt == -EOPNOTSUPP)
					goto patch_map_ops_generic;
				if (cnt <= 0 || cnt >= INSN_BUF_SIZE) {
					verifier_bug(env, "%d insns generated for map lookup", cnt);
					return -EFAULT;
				}

				new_prog = bpf_patch_insn_data(env, i + delta,
							       insn_buf, cnt);
				if (!new_prog)
					return -ENOMEM;
				/* 自定义 lookup 序列完全替代 helper，不再走通用调用地址标签。 */

				delta    += cnt - 1;
				env->prog = prog = new_prog;
				insn      = new_prog->insnsi + i + delta;
				goto next_insn;
			}

			BUILD_BUG_ON(!__same_type(ops->map_lookup_elem,
			/* 编译期逐项核对 ops 原型与 BPF 调用约定，防止直连错误签名。 */
				     (void *(*)(struct bpf_map *map, void *key))NULL));
			BUILD_BUG_ON(!__same_type(ops->map_delete_elem,
				     (long (*)(struct bpf_map *map, void *key))NULL));
			BUILD_BUG_ON(!__same_type(ops->map_update_elem,
			/* update/push/redirect 等含额外参数，逐项断言可防 ABI 漂移。 */
				     (long (*)(struct bpf_map *map, void *key, void *value,
					      u64 flags))NULL));
			BUILD_BUG_ON(!__same_type(ops->map_push_elem,
				     (long (*)(struct bpf_map *map, void *value,
					      u64 flags))NULL));
			BUILD_BUG_ON(!__same_type(ops->map_pop_elem,
			/* pop/peek 均接收 map 与 value 两参，但仍分别锁定成员签名。 */
				     (long (*)(struct bpf_map *map, void *value))NULL));
			BUILD_BUG_ON(!__same_type(ops->map_peek_elem,
				     (long (*)(struct bpf_map *map, void *value))NULL));
			BUILD_BUG_ON(!__same_type(ops->map_redirect,
				     (long (*)(struct bpf_map *map, u64 index, u64 flags))NULL));
			BUILD_BUG_ON(!__same_type(ops->map_for_each_callback,
			/* 回调与 per-cpu lookup 的函数指针签名也必须精确一致。 */
				     (long (*)(struct bpf_map *map,
					      bpf_callback_t callback_fn,
					      void *callback_ctx,
					      u64 flags))NULL));
			BUILD_BUG_ON(!__same_type(ops->map_lookup_percpu_elem,
				     (void *(*)(struct bpf_map *map, void *key, u32 cpu))NULL));

patch_map_ops_generic:
			/* 将受支持 helper ID 映射为该确定 map 实例的 ops 函数地址。 */
			switch (insn->imm) {
			case BPF_FUNC_map_lookup_elem:
				insn->imm = BPF_CALL_IMM(ops->map_lookup_elem);
				goto next_insn;
			case BPF_FUNC_map_update_elem:
				insn->imm = BPF_CALL_IMM(ops->map_update_elem);
				goto next_insn;
			case BPF_FUNC_map_delete_elem:
				/* 每个分支只替换 imm，opcode 和寄存器调用约定保持原样。 */
				insn->imm = BPF_CALL_IMM(ops->map_delete_elem);
				goto next_insn;
			case BPF_FUNC_map_push_elem:
				/* push/pop/peek 的 BPF 参数布局分别匹配已断言的 ops 原型。 */
				insn->imm = BPF_CALL_IMM(ops->map_push_elem);
				goto next_insn;
			case BPF_FUNC_map_pop_elem:
				insn->imm = BPF_CALL_IMM(ops->map_pop_elem);
				goto next_insn;
			case BPF_FUNC_map_peek_elem:
				insn->imm = BPF_CALL_IMM(ops->map_peek_elem);
				goto next_insn;
			case BPF_FUNC_redirect_map:
				/* redirect/for_each/percpu 三类使用各自 ops 成员。 */
				insn->imm = BPF_CALL_IMM(ops->map_redirect);
				goto next_insn;
			case BPF_FUNC_for_each_map_elem:
				/* for_each 的回调指针在验证/JIT 前已具有正确 bpf_callback_t ABI。 */
				insn->imm = BPF_CALL_IMM(ops->map_for_each_callback);
				goto next_insn;
			case BPF_FUNC_map_lookup_percpu_elem:
				insn->imm = BPF_CALL_IMM(ops->map_lookup_percpu_elem);
				goto next_insn;
			}

			goto patch_call_imm;
		}

		/* Implement bpf_jiffies64 inline. */
		/* 中文：64 位 JIT 可直接加载 jiffies 地址再读值，省去普通 helper 调用。 */
		if (prog->jit_requested && BITS_PER_LONG == 64 &&
		    insn->imm == BPF_FUNC_jiffies64) {
			struct bpf_insn ld_jiffies_addr[2] = {
				/* 内核地址通过 ldimm64 两槽嵌入修复后程序。 */
				BPF_LD_IMM64(BPF_REG_0,
					     (unsigned long)&jiffies),
			};

			insn_buf[0] = ld_jiffies_addr[0];
			insn_buf[1] = ld_jiffies_addr[1];
			insn_buf[2] = BPF_LDX_MEM(BPF_DW, BPF_REG_0,
						  BPF_REG_0, 0);
			cnt = 3;
			/* 两槽地址加载后第三条解引用当前 64 位 jiffies。 */

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf,
						       cnt);
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

#if defined(CONFIG_X86_64) && !defined(CONFIG_UML)
		/* Implement bpf_get_smp_processor_id() inline. */
		/* 中文：x86_64 可通过 per-cpu cpu_number 直接取得 CPU id；UP 内核恒为零。 */
		if (insn->imm == BPF_FUNC_get_smp_processor_id &&
		    bpf_verifier_inlines_helper_call(env, insn->imm)) {
			/* BPF_FUNC_get_smp_processor_id inlining is an
			 * optimization, so if cpu_number is ever
			 * changed in some incompatible and hard to support
			 * way, it's fine to back out this inlining logic
			 */
			/* 中文：这是可撤销优化，per-cpu 实现变化时可退回 helper。 */
#ifdef CONFIG_SMP
			/* 先构造 per-cpu 符号地址，再转换为当前 CPU 地址并读取 u32。 */
			insn_buf[0] = BPF_MOV64_IMM(BPF_REG_0, (u32)(unsigned long)&cpu_number);
			insn_buf[1] = BPF_MOV64_PERCPU_REG(BPF_REG_0, BPF_REG_0);
			insn_buf[2] = BPF_LDX_MEM(BPF_W, BPF_REG_0, BPF_REG_0, 0);
			/* 最终 u32 load 天然满足 helper 返回值的零扩展要求。 */
			cnt = 3;
#else
			insn_buf[0] = BPF_ALU32_REG(BPF_XOR, BPF_REG_0, BPF_REG_0);
			cnt = 1;
#endif
			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			/* get_func_ret 替换后继续按原指令 i 更新 delta。 */
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Implement bpf_get_current_task() and bpf_get_current_task_btf() inline. */
		/* 中文：两类 helper 都从 per-cpu current_task 读取同一 task_struct 指针。 */
		if ((insn->imm == BPF_FUNC_get_current_task || insn->imm == BPF_FUNC_get_current_task_btf) &&
		    bpf_verifier_inlines_helper_call(env, insn->imm)) {
			insn_buf[0] = BPF_MOV64_IMM(BPF_REG_0, (u32)(unsigned long)&current_task);
			/* MOV64_PERCPU_REG 把符号偏移解析到当前 CPU 的槽地址。 */
			insn_buf[1] = BPF_MOV64_PERCPU_REG(BPF_REG_0, BPF_REG_0);
			insn_buf[2] = BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_0, 0);
			/* current_task 是 per-cpu 指针槽，最终需要一次 DW 解引用。 */
			cnt = 3;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			/* get_func_arg_cnt 成功后当前指针重定位到补丁尾。 */
			if (!new_prog)
				return -ENOMEM;
			/* get_func_ret 的成功/不支持两类序列在这里统一替换。 */

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}
#endif
		/* Implement bpf_get_func_arg inline. */
		/* 中文：tracing 上下文把参数视作 8 字节数组，先界检再把选中值写到 R3 指针。 */
		if (prog_type == BPF_PROG_TYPE_TRACING &&
		    insn->imm == BPF_FUNC_get_func_arg) {
			if (eatype == BPF_TRACE_RAW_TP) {
				/* raw tracepoint 参数数可由 attach BTF 静态取得。 */
				int nr_args = btf_type_vlen(prog->aux->attach_func_proto);

				/* skip 'void *__data' in btf_trace_##name() and save to reg0 */
				insn_buf[0] = BPF_MOV64_IMM(BPF_REG_0, nr_args - 1);
				cnt = 1;
			} else {
				/* Load nr_args from ctx - 8 */
				/* 中文：其他 tracing 上下文在 ctx 前 8 字节编码低 8 位参数数。 */
				insn_buf[0] = BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_1, -8);
				insn_buf[1] = BPF_ALU64_IMM(BPF_AND, BPF_REG_0, 0xFF);
				cnt = 2;
			}
			insn_buf[cnt++] = BPF_JMP32_REG(BPF_JGE, BPF_REG_2, BPF_REG_0, 6);
			/* 成功返回 0，越界返回 -EINVAL；R2 在检查后转换为字节偏移。 */
			insn_buf[cnt++] = BPF_ALU64_IMM(BPF_LSH, BPF_REG_2, 3);
			insn_buf[cnt++] = BPF_ALU64_REG(BPF_ADD, BPF_REG_2, BPF_REG_1);
			insn_buf[cnt++] = BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_2, 0);
			/* 读出的参数先经 R0 暂存，再写入调用者提供的 R3 地址。 */
			insn_buf[cnt++] = BPF_STX_MEM(BPF_DW, BPF_REG_3, BPF_REG_0, 0);
			insn_buf[cnt++] = BPF_MOV64_IMM(BPF_REG_0, 0);
			insn_buf[cnt++] = BPF_JMP_A(1);
			insn_buf[cnt++] = BPF_MOV64_IMM(BPF_REG_0, -EINVAL);

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			/* branch snapshot 的 11 条序列完全替代原 helper 调用。 */
			/* 九条左右的内联序列仍由共享缓冲区上界保证。 */
			if (!new_prog)
				return -ENOMEM;
			/* 参数计数序列不改变其他参数寄存器，只定义 R0。 */

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Implement bpf_get_func_ret inline. */
		/* 中文：仅有返回值语义的 fexit/fsession/modify_return 从参数数组尾部取返回值。 */
		if (prog_type == BPF_PROG_TYPE_TRACING &&
		    insn->imm == BPF_FUNC_get_func_ret) {
			if (eatype == BPF_TRACE_FEXIT ||
			    eatype == BPF_TRACE_FSESSION ||
			    eatype == BPF_TRACE_FEXIT_MULTI ||
			    eatype == BPF_TRACE_FSESSION_MULTI ||
			    eatype == BPF_MODIFY_RETURN) {
				/* Load nr_args from ctx - 8 */
				/* 中文：nr_args*8 定位数组尾返回值，写入 R2 指向的用户结果槽。 */
				insn_buf[0] = BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_1, -8);
				insn_buf[1] = BPF_ALU64_IMM(BPF_AND, BPF_REG_0, 0xFF);
				insn_buf[2] = BPF_ALU64_IMM(BPF_LSH, BPF_REG_0, 3);
				insn_buf[3] = BPF_ALU64_REG(BPF_ADD, BPF_REG_0, BPF_REG_1);
				insn_buf[4] = BPF_LDX_MEM(BPF_DW, BPF_REG_3, BPF_REG_0, 0);
				/* 返回值暂存 R3 后写到 R2 指针，R0 按 helper ABI 返回零。 */
				insn_buf[5] = BPF_STX_MEM(BPF_DW, BPF_REG_2, BPF_REG_3, 0);
				insn_buf[6] = BPF_MOV64_IMM(BPF_REG_0, 0);
				cnt = 7;
			} else {
				/* 不支持返回值的 attach 类型直接返回 -EOPNOTSUPP。 */
				insn_buf[0] = BPF_MOV64_IMM(BPF_REG_0, -EOPNOTSUPP);
				cnt = 1;
			}

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Implement get_func_arg_cnt inline. */
		/* 中文：参数计数复用 raw BTF 常量或 ctx 前缀低 8 位方案，直接在 R0 返回。 */
		if (prog_type == BPF_PROG_TYPE_TRACING &&
		    insn->imm == BPF_FUNC_get_func_arg_cnt) {
			if (eatype == BPF_TRACE_RAW_TP) {
				/* BTF vlen 包含隐藏 __data 参数，公开计数需减一。 */
				int nr_args = btf_type_vlen(prog->aux->attach_func_proto);

				/* skip 'void *__data' in btf_trace_##name() and save to reg0 */
				insn_buf[0] = BPF_MOV64_IMM(BPF_REG_0, nr_args - 1);
				cnt = 1;
			} else {
				/* Load nr_args from ctx - 8 */
				/* 非 raw 上下文沿用低 8 位动态计数编码。 */
				insn_buf[0] = BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_1, -8);
				insn_buf[1] = BPF_ALU64_IMM(BPF_AND, BPF_REG_0, 0xFF);
				cnt = 2;
			}

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Implement bpf_get_func_ip inline. */
		/* 中文：tracing ABI 在 ctx 前 16 字节保存函数 IP，单条加载即可内联。 */
		if (prog_type == BPF_PROG_TYPE_TRACING &&
		    insn->imm == BPF_FUNC_get_func_ip) {
			/* Load IP address from ctx - 16 */
			insn_buf[0] = BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_1, -16);

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, 1);
			if (!new_prog)
				return -ENOMEM;

			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Implement bpf_get_branch_snapshot inline. */
		/* 中文：将 BPF 字节容量换算为 perf_branch_entry 数量并直调 perf 静态调用点。 */
		if (IS_ENABLED(CONFIG_PERF_EVENTS) &&
		    prog->jit_requested && BITS_PER_LONG == 64 &&
		    insn->imm == BPF_FUNC_get_branch_snapshot) {
			/* We are dealing with the following func protos:
			 * u64 bpf_get_branch_snapshot(void *buf, u32 size, u64 flags);
			 * int perf_snapshot_branch_stack(struct perf_branch_entry *entries, u32 cnt);
			 */
			/* 中文：内联序列需把 helper 的字节单位适配为底层函数的元素个数。 */
			const u32 br_entry_size = sizeof(struct perf_branch_entry);

			/* struct perf_branch_entry is part of UAPI and is
			 * used as an array element, so extremely unlikely to
			 * ever grow or shrink
			 */
			/* 中文：编译期锁定元素为 24 字节，避免魔数除法在结构变化后静默错误。 */
			BUILD_BUG_ON(br_entry_size != 24);

			/* if (unlikely(flags)) return -EINVAL */
			insn_buf[0] = BPF_JMP_IMM(BPF_JNE, BPF_REG_3, 0, 7);

			/* Transform size (bytes) into number of entries (cnt = size / 24).
			 * But to avoid expensive division instruction, we implement
			 * divide-by-3 through multiplication, followed by further
			 * division by 8 through 3-bit right shift.
			 * Refer to book "Hacker's Delight, 2nd ed." by Henry S. Warren, Jr.,
			 * p. 227, chapter "Unsigned Division by 3" for details and proofs.
			 *
			 * N / 3 <=> M * N / 2^33, where M = (2^33 + 1) / 3 = 0xaaaaaaab.
			 */
			/* 中文：乘 0xaaaaaaab 后右移 36 等价无符号除以 24，避免昂贵除法。 */
			insn_buf[1] = BPF_MOV32_IMM(BPF_REG_0, 0xaaaaaaab);
			insn_buf[2] = BPF_ALU64_REG(BPF_MUL, BPF_REG_2, BPF_REG_0);
			insn_buf[3] = BPF_ALU64_IMM(BPF_RSH, BPF_REG_2, 36);

			/* call perf_snapshot_branch_stack implementation */
			/* 静态调用返回写入的 entry 数，零需转换为 -ENOENT。 */
			insn_buf[4] = BPF_EMIT_CALL(static_call_query(perf_snapshot_branch_stack));
			/* if (entry_cnt == 0) return -ENOENT */
			insn_buf[5] = BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 4);
			/* 非零 entry_cnt 乘回 24 字节并跨过两条错误返回路径。 */
			/* return entry_cnt * sizeof(struct perf_branch_entry) */
			insn_buf[6] = BPF_ALU32_IMM(BPF_MUL, BPF_REG_0, br_entry_size);
			insn_buf[7] = BPF_JMP_A(3);
			/* return -EINVAL; */
			/* flags 非零分支与空结果分支各自产生规定负 errno。 */
			insn_buf[8] = BPF_MOV64_IMM(BPF_REG_0, -EINVAL);
			insn_buf[9] = BPF_JMP_A(1);
			/* return -ENOENT; */
			insn_buf[10] = BPF_MOV64_IMM(BPF_REG_0, -ENOENT);
			cnt = 11;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}

		/* Implement bpf_kptr_xchg inline */
		/* 中文：JIT 支持指针 XCHG 时用两条指令把 R2 新值交换到 R1 地址并在 R0 返回旧值。 */
		if (prog->jit_requested && BITS_PER_LONG == 64 &&
		    insn->imm == BPF_FUNC_kptr_xchg &&
		    bpf_jit_supports_ptr_xchg()) {
			insn_buf[0] = BPF_MOV64_REG(BPF_REG_0, BPF_REG_2);
			insn_buf[1] = BPF_ATOMIC_OP(BPF_DW, BPF_XCHG, BPF_REG_1, BPF_REG_0, 0);
			/* XCHG 原子地把 R0 新指针写入并用同寄存器返回旧指针。 */
			cnt = 2;

			new_prog = bpf_patch_insn_data(env, i + delta, insn_buf, cnt);
			if (!new_prog)
				return -ENOMEM;

			delta    += cnt - 1;
			env->prog = prog = new_prog;
			insn      = new_prog->insnsi + i + delta;
			goto next_insn;
		}
patch_call_imm:
		/* 未内联 helper 的最终形式是相对 __bpf_call_base 的内核函数位移。 */
		fn = env->ops->get_func_proto(insn->imm, env->prog);
		/* all functions that have prototype and verifier allowed
		 * programs to call them, must be real in-kernel functions
		 */
		/* 中文：验证已允许的原型必须绑定真实实现，NULL 表示内核内部表不一致。 */
		if (!fn->func) {
			verifier_bug(env,
				     "not inlined functions %s#%d is missing func",
				     func_id_name(insn->imm), insn->imm);
			return -EFAULT;
		}
		insn->imm = fn->func - __bpf_call_base;
		/* s32 可编码性由 helper 表/JIT ABI 保证，指令至此成为直接 BPF call。 */
next_insn:
		/* 跨过子程序尾时提交 may_goto 额外栈深，并为下一子程序重置累计量。 */
		if (subprogs[cur_subprog + 1].start == i + delta + 1) {
			subprogs[cur_subprog].stack_depth += stack_depth_extra;
			subprogs[cur_subprog].stack_extra = stack_depth_extra;

			stack_depth = subprogs[cur_subprog].stack_depth;
			if (stack_depth > MAX_BPF_STACK && !prog->jit_requested) {
				/* 解释器栈仍受固定 MAX_BPF_STACK 限制，JIT 可按架构处理更深栈。 */
				verbose(env, "stack size %d(extra %d) is too large\n",
					stack_depth, stack_depth_extra);
				return -EINVAL;
			}
			cur_subprog++;
			/* fake exit 哨兵保证最后一个真实子程序也会命中边界。 */
			stack_depth = subprogs[cur_subprog].stack_depth;
			stack_depth_extra = 0;
		}
		i++;
		insn++;
	}

	env->prog->aux->stack_depth = subprogs[0].stack_depth;
	/* 第二遍在每个使用 may_goto 的子程序入口初始化刚预留的计数/时间槽。 */
	for (i = 0; i < env->subprog_cnt; i++) {
		int delta = bpf_jit_supports_timed_may_goto() ? 2 : 1;
		int subprog_start = subprogs[i].start;
		int stack_slots = subprogs[i].stack_extra / 8;
		int slots = delta, cnt = 0;

		if (!stack_slots)
			continue;
		/* We need two slots in case timed may_goto is supported. */
		/* 中文：当前实现只允许普通一槽或 timed 两槽，其他 extra 值是内部错误。 */
		if (stack_slots > slots) {
			verifier_bug(env, "stack_slots supports may_goto only");
			return -EFAULT;
		}

		stack_depth = subprogs[i].stack_depth;
		if (bpf_jit_supports_timed_may_goto()) {
			/* 计数初值 0xffff，时间戳初值零，首次耗尽时由架构助手写入。 */
			insn_buf[cnt++] = BPF_ST_MEM(BPF_DW, BPF_REG_FP, -stack_depth,
						     BPF_MAX_TIMED_LOOPS);
			insn_buf[cnt++] = BPF_ST_MEM(BPF_DW, BPF_REG_FP, -stack_depth + 8, 0);
		} else {
			/* Add ST insn to subprog prologue to init extra stack */
			/* 中文：普通实现仅初始化固定最大循环次数。 */
			insn_buf[cnt++] = BPF_ST_MEM(BPF_DW, BPF_REG_FP, -stack_depth,
						     BPF_MAX_LOOPS);
		}
		/* Copy first actual insn to preserve it */
		/* 中文：统一补丁接口替换一条，因此尾项必须复制原入口指令。 */
		insn_buf[cnt++] = env->prog->insnsi[subprog_start];

		new_prog = bpf_patch_insn_data(env, subprog_start, insn_buf, cnt);
		if (!new_prog)
			return -ENOMEM;
		env->prog = prog = new_prog;
		/*
		 * If may_goto is a first insn of a prog there could be a jmp
		 * insn that points to it, hence adjust all such jmps to point
		 * to insn after BPF_ST that inits may_goto count.
		 * Adjustment will succeed because bpf_patch_insn_data() didn't fail.
		 */
		/* 中文：原来跳向子程序首条的内部边必须越过初始化 ST，避免每轮重置计数。 */
		WARN_ON(adjust_jmp_off(env->prog, subprog_start, delta));
	}

	/* Since poke tab is now finalized, publish aux to tracker. */
	/* 中文：所有指令索引稳定后才能向 map 注册 poke；此后运行期可能动态改尾调用点。 */
	for (i = 0; i < prog->aux->size_poke_tab; i++) {
		map_ptr = prog->aux->poke_tab[i].tail_call.map;
		if (!map_ptr->ops->map_poke_track ||
		    !map_ptr->ops->map_poke_untrack ||
		    !map_ptr->ops->map_poke_run) {
			verifier_bug(env, "poke tab is misconfigured");
			return -EFAULT;
		}
		/* track 成功后 map 与 prog->aux 建立跨生命周期关系，释放路径会 untrack。 */

		ret = map_ptr->ops->map_poke_track(map_ptr, prog->aux);
		if (ret < 0) {
			verbose(env, "tracking tail call prog failed\n");
			return ret;
		}
	}

	ret = sort_kfunc_descs_by_imm_off(env);
	/* 最后修复并排序 kfunc 描述符，使 JIT 查询键与最终调用指令一致。 */
	if (ret)
		return ret;

	return 0;
}

/*
 * 把一个 bpf_loop helper 调用展开为显式循环：保存 R6-R8、检查上限、调用回调、计数并恢复寄存器。
 * position 补丁会移动 callback 起点，所以先扩张再回填相对 call imm；total_cnt 返回补丁长度。
 */
static struct bpf_prog *inline_bpf_loop(struct bpf_verifier_env *env,
					int position,
					s32 stack_base,
					u32 callback_subprogno,
					u32 *total_cnt)
{
	/* 三个 8 字节槽分别保存原 R6/R7/R8，寄存器随后承载上限、计数和 ctx。 */
	s32 r6_offset = stack_base + 0 * BPF_REG_SIZE;
	s32 r7_offset = stack_base + 1 * BPF_REG_SIZE;
	s32 r8_offset = stack_base + 2 * BPF_REG_SIZE;
	int reg_loop_max = BPF_REG_6;
	int reg_loop_cnt = BPF_REG_7;
	int reg_loop_ctx = BPF_REG_8;

	struct bpf_insn *insn_buf = env->insn_buf;
	struct bpf_prog *new_prog;
	u32 callback_start;
	/* 三个 offset 在补丁成功后共同定位占位 BPF_CALL_REL。 */
	u32 call_insn_offset;
	s32 callback_offset;
	u32 cnt = 0;

	/* This represents an inlined version of bpf_iter.c:bpf_loop,
	 * be careful to modify this code in sync.
	 */
	/* 中文：语义必须与真实 helper 同步，任何返回码/停止条件变化都需同时维护。 */

	/* Return error and jump to the end of the patch if
	 * expected number of iterations is too big.
	 */
	/* 中文：请求次数超过 BPF_MAX_LOOPS 时返回 -E2BIG 并跳过全部保存/循环代码。 */
	insn_buf[cnt++] = BPF_JMP_IMM(BPF_JLE, BPF_REG_1, BPF_MAX_LOOPS, 2);
	insn_buf[cnt++] = BPF_MOV32_IMM(BPF_REG_0, -E2BIG);
	insn_buf[cnt++] = BPF_JMP_IMM(BPF_JA, 0, 0, 16);
	/* spill R6, R7, R8 to use these as loop vars */
	/* 中文：callee-saved 寄存器在 helper 语义下必须对调用者透明，故入口 spill、出口 fill。 */
	insn_buf[cnt++] = BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_6, r6_offset);
	insn_buf[cnt++] = BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_7, r7_offset);
	insn_buf[cnt++] = BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_8, r8_offset);
	/* initialize loop vars */
	insn_buf[cnt++] = BPF_MOV64_REG(reg_loop_max, BPF_REG_1);
	insn_buf[cnt++] = BPF_MOV32_IMM(reg_loop_cnt, 0);
	insn_buf[cnt++] = BPF_MOV64_REG(reg_loop_ctx, BPF_REG_3);
	/* loop header,
	 * if reg_loop_cnt >= reg_loop_max skip the loop body
	 */
	/* 中文：计数达到上限直接结束，否则准备 R1=序号、R2=ctx 调 callback。 */
	insn_buf[cnt++] = BPF_JMP_REG(BPF_JGE, reg_loop_cnt, reg_loop_max, 5);
	/* callback call,
	 * correct callback offset would be set after patching
	 */
	insn_buf[cnt++] = BPF_MOV64_REG(BPF_REG_1, reg_loop_cnt);
	insn_buf[cnt++] = BPF_MOV64_REG(BPF_REG_2, reg_loop_ctx);
	insn_buf[cnt++] = BPF_CALL_REL(0);
	/* increment loop counter */
	insn_buf[cnt++] = BPF_ALU64_IMM(BPF_ADD, reg_loop_cnt, 1);
	/* jump to loop header if callback returned 0 */
	/* 中文：回调非零请求提前停止；零则回跳并继续下一迭代。 */
	insn_buf[cnt++] = BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, -6);
	/* return value of bpf_loop,
	 * set R0 to the number of iterations
	 */
	insn_buf[cnt++] = BPF_MOV64_REG(BPF_REG_0, reg_loop_cnt);
	/* restore original values of R6, R7, R8 */
	insn_buf[cnt++] = BPF_LDX_MEM(BPF_DW, BPF_REG_6, BPF_REG_10, r6_offset);
	insn_buf[cnt++] = BPF_LDX_MEM(BPF_DW, BPF_REG_7, BPF_REG_10, r7_offset);
	insn_buf[cnt++] = BPF_LDX_MEM(BPF_DW, BPF_REG_8, BPF_REG_10, r8_offset);

	*total_cnt = cnt;
	/* 先补丁才能获得更新后的 subprog_info callback start。 */
	new_prog = bpf_patch_insn_data(env, position, insn_buf, cnt);
	if (!new_prog)
		return new_prog;

	/* callback start is known only after patching */
	/* 中文：补丁器已同步所有 start，此时用绝对位置计算 BPF_CALL_REL 的 imm。 */
	callback_start = env->subprog_info[callback_subprogno].start;
	/* Note: insn_buf[12] is an offset of BPF_CALL_REL instruction */
	call_insn_offset = position + 12;
	callback_offset = callback_start - call_insn_offset - 1;
	new_prog->insnsi[call_insn_offset].imm = callback_offset;

	return new_prog;
}

/* 判断指令是否为尚未修复的普通 bpf_loop helper 调用。 */
static bool is_bpf_loop_call(struct bpf_insn *insn)
{
	return insn->code == (BPF_JMP | BPF_CALL) &&
		insn->src_reg == 0 &&
		insn->imm == BPF_FUNC_loop;
}

/* For all sub-programs in the program (including main) check
 * insn_aux_data to see if there are bpf_loop calls that require
 * inlining. If such calls are found the calls are replaced with a
 * sequence of instructions produced by `inline_bpf_loop` function and
 * subprog stack_depth is increased by the size of 3 registers.
 * This stack space is used to spill values of the R6, R7, R8.  These
 * registers are used to store the loop bound, counter and context
 * variables.
 */
/*
 * 中文：逐子程序查找验证阶段判定可内联的 bpf_loop，调用 inline_bpf_loop 并为 R6-R8 spill 增加栈深。
 * 每个子程序只需一次额外空间，delta 跟踪扩张；成功更新主程序 stack_depth。
 */
int bpf_optimize_bpf_loop(struct bpf_verifier_env *env)
{
	struct bpf_subprog_info *subprogs = env->subprog_info;
	int i, cur_subprog = 0, cnt, delta = 0;
	struct bpf_insn *insn = env->prog->insnsi;
	int insn_cnt = env->prog->len;
	u16 stack_depth = subprogs[cur_subprog].stack_depth;
	/* roundup 只为首个新增 spill 槽满足 8 字节对齐。 */
	u16 stack_depth_roundup = round_up(stack_depth, 8) - stack_depth;
	u16 stack_depth_extra = 0;

	for (i = 0; i < insn_cnt; i++, insn++) {
		/* inline_state 与当前 i+delta 指令对齐，记录回调子程序号和适用性。 */
		struct bpf_loop_inline_state *inline_state =
			&env->insn_aux_data[i + delta].loop_inline_state;

		if (is_bpf_loop_call(insn) && inline_state->fit_for_inline) {
			/* 栈深先补齐 8 字节对齐，再追加三个保存槽。 */
			struct bpf_prog *new_prog;

			stack_depth_extra = BPF_REG_SIZE * 3 + stack_depth_roundup;
			/* 负 stack_base 指向扩张后的新栈底，三个槽均在验证上限内。 */
			new_prog = inline_bpf_loop(env,
						   i + delta,
						   -(stack_depth + stack_depth_extra),
						   inline_state->callback_subprogno,
						   &cnt);
			if (!new_prog)
				return -ENOMEM;

			delta     += cnt - 1;
			env->prog  = new_prog;
			insn       = new_prog->insnsi + i + delta;
		}

		if (subprogs[cur_subprog + 1].start == i + delta + 1) {
			/* 到达子程序尾时提交本段额外栈并为下一段重新计算对齐。 */
			subprogs[cur_subprog].stack_depth += stack_depth_extra;
			cur_subprog++;
			stack_depth = subprogs[cur_subprog].stack_depth;
			stack_depth_roundup = round_up(stack_depth, 8) - stack_depth;
			stack_depth_extra = 0;
		}
	}

	env->prog->aux->stack_depth = env->subprog_info[0].stack_depth;

	return 0;
}

/* Remove unnecessary spill/fill pairs, members of fastcall pattern,
 * adjust subprograms stack depth when possible.
 */
/*
 * 中文：把 fastcall 模式两侧成对 spill/fill 改为 NOP，并在允许时把子程序栈深收缩到 fastcall 区边界。
 * 本函数只做等长原地改写；物理删除由后续 bpf_opt_remove_nops 完成。
 */
int bpf_remove_fastcall_spills_fills(struct bpf_verifier_env *env)
{
	struct bpf_subprog_info *subprog = env->subprog_info;
	struct bpf_insn_aux_data *aux = env->insn_aux_data;
	struct bpf_insn *insn = env->prog->insnsi;
	int insn_cnt = env->prog->len;
	u32 spills_num;
	bool modified = false;
	int i, j;

	for (i = 0; i < insn_cnt; i++, insn++) {
		/* call 指令 aux 记录两侧对称 spill 数，j 从一开始向外覆盖。 */
		if (aux[i].fastcall_spills_num > 0) {
			spills_num = aux[i].fastcall_spills_num;
			/* NOPs would be removed by opt_remove_nops() */
			/* 中文：先保持长度可避免当前扫描中同步任何跳转/元数据偏移。 */
			for (j = 1; j <= spills_num; ++j) {
				*(insn - j) = NOP;
				*(insn + j) = NOP;
			}
			modified = true;
		}
		if ((subprog + 1)->start == i + 1) {
			/* 子程序边界提交是否缩栈；keep_fastcall_stack 要求保留原布局。 */
			if (modified && !subprog->keep_fastcall_stack)
				subprog->stack_depth = -subprog->fastcall_stack_off;
			subprog++;
			modified = false;
		}
	}

	return 0;
}
