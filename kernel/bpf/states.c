// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/cnum.h>
#include <linux/filter.h>

#define verbose(env, fmt, args...) bpf_verifier_log_write(env, fmt, ##args)

#define BPF_COMPLEXITY_LIMIT_STATES	64

/*
 * 学习导览：本文件实现验证器的状态缓存与剪枝。旧状态 old 已经证明安全时，
 * 只有当前状态 cur 的全部可能取值都落在 old 的安全集合内、ID/引用/锁关系也
 * 保持一致，才可停止当前路径。循环中的读活性和精度标记尚未稳定，因而还用
 * SCC callchain 收集回边，等所有分支结束后迭代传播到不动点。
 * env 由单个验证线程独占；这里的链表与引用是验证器内部 ownership，不是运行期
 * BPF 并发。可睡眠分配失败均返回 errno，由上层放弃本次验证。
 */

/*
 * 业务背景：状态剪枝需识别 may_goto 的特殊循环预算指令。
 * 入参：env 为借用的验证环境；insn_idx 是已校验范围内的指令下标。
 * 出参/返回：返回该指令是否为 may_goto；无输出参数和 ownership 变化。
 * 注意事项：只读程序指令，不睡眠且无需锁；调用者负责下标合法。
 */
static bool is_may_goto_insn_at(struct bpf_verifier_env *env, int insn_idx)
{
	return bpf_is_may_goto_insn(&env->prog->insnsi[insn_idx]);
}

/*
 * 业务背景：开放编码迭代器在收敛检测时采用不同于普通回边的规则。
 * 入参：env 为借用环境；insn_idx 是当前检查点的指令下标。
 * 出参/返回：返回预处理阶段记录的 is_iter_next 标志；无副作用。
 * 注意事项：aux 数组必须已建立；纯读取、不睡眠、无锁。
 */
static bool is_iter_next_insn(struct bpf_verifier_env *env, int insn_idx)
{
	return env->insn_aux_data[insn_idx].is_iter_next;
}

/*
 * 业务背景：验证统计需记录同时存活的缓存、延迟释放状态与 SCC 回边峰值。
 * 入参：env 为输入输出借用指针，其三个计数均以“状态对象个数”为单位。
 * 出参/返回：无直接返回值；只单调增大 env->peak_states。
 * 注意事项：调用者须在计数改变后调用；验证线程独占 env，函数不睡眠。
 */
static void update_peak_states(struct bpf_verifier_env *env)
{
	u32 cur_states;

	cur_states = env->explored_states_size + env->free_list_size + env->num_backedges;
	env->peak_states = max(env->peak_states, cur_states);
}

/* struct bpf_verifier_state->parent refers to states
 * that are in either of env->{expored_states,free_list}.
 * In both cases the state is contained in struct bpf_verifier_state_list.
 */
/*
 * 中文：state->parent 若非空，总是指向 explored_states 或 free_list 中某个
 * bpf_verifier_state_list 的内嵌 state，故可用 container_of 还原拥有者。
 *
 * 业务背景：释放子状态前要更新父分支数，并可能回收父状态的外层链表节点。
 * 入参：st 是借用状态，允许 parent 为 NULL；调用期间其父节点必须仍存活。
 * 出参/返回：有父状态时返回借用的容器指针，否则返回 NULL；不改变引用关系。
 * 注意事项：container_of 不做运行期校验；违反内嵌关系会产生无效指针。不睡眠。
 */
static struct bpf_verifier_state_list *state_parent_as_list(struct bpf_verifier_state *st)
{
	if (st->parent)
		return container_of(st->parent, struct bpf_verifier_state_list, state);
	return NULL;
}

/*
 * 业务背景：释放判定需提前引用后文的 SCC 读标记完整性查询。
 * 入参：env/st 为借用的验证环境和状态。
 * 出参/返回：声明返回是否仍有待传播回边；定义及完整契约见下文。
 * 注意事项：这里只建立内部声明，不改变 ownership、状态或执行上下文。
 */
static bool incomplete_read_marks(struct bpf_verifier_env *env,
				  struct bpf_verifier_state *st);

/* A state can be freed if it is no longer referenced:
 * - is in the env->free_list;
 * - has no children states;
 */
/*
 * 中文：状态只有已移入 free_list、子分支计数归零且 SCC 读标记已经完整时才能释放。
 *
 * 业务背景：缓存淘汰不能破坏 parent 链或尚待回边传播的活性信息，本函数集中
 * 判断最终释放条件。
 * 入参：env 为输入输出验证环境；sl 是 free/explored 链上的借用容器指针。
 * 出参/返回：无直接返回值；条件满足时摘链、释放 state 内容及 sl，并递减计数；
 * 否则保持所有状态不变。
 * 注意事项：成功释放后调用者不得再解引用 sl；内部释放可能处理动态状态内容，
 * 仅在验证进程上下文调用，无并发锁需求。
 */
static void maybe_free_verifier_state(struct bpf_verifier_env *env,
				      struct bpf_verifier_state_list *sl)
{
	if (!sl->in_free_list
	    || sl->state.branches != 0
	    || incomplete_read_marks(env, &sl->state))
		return;
	/* 三项门禁通过后 sl 已不可达且无待传播依赖，可执行最终摘除与释放。 */
	list_del(&sl->node);
	bpf_free_verifier_state(&sl->state, false);
	kfree(sl);
	env->free_list_size--;
}

/* For state @st look for a topmost frame with frame_insn_idx() in some SCC,
 * if such frame exists form a corresponding @callchain as an array of
 * call sites leading to this frame and SCC id.
 * E.g.:
 *
 *    void foo()  { A: loop {... SCC#1 ...}; }
 *    void bar()  { B: loop { C: foo(); ... SCC#2 ... }
 *                  D: loop { E: foo(); ... SCC#3 ... } }
 *    void main() { F: bar(); }
 *
 * @callchain at (A) would be either (F,SCC#2) or (F,SCC#3) depending
 * on @st frame call sites being (F,C,A) or (F,E,A).
 */
/*
 * 中文：从最外层到当前帧扫描，记录进入各帧的调用点；遇到第一个 SCC 后停止，
 * 于是同一 SCC 经不同调用位置进入时会得到不同键，如示例的 (F,SCC#2/#3)。
 *
 * 业务背景：SCC 的回边状态依赖具体调用路径，不能把不同调用实例混在一起。
 * 入参：env/st 均为借用且稳定；callchain 是调用者提供的纯输出缓冲。
 * 出参/返回：找到 SCC 返回 true 并完整初始化 callchain；否则清零后返回 false。
 * 注意事项：callsites 的 0 是终止哨兵；只读状态、不睡眠、无 ownership 转移。
 */
static bool compute_scc_callchain(struct bpf_verifier_env *env,
				  struct bpf_verifier_state *st,
				  struct bpf_scc_callchain *callchain)
{
	u32 i, scc, insn_idx;

	memset(callchain, 0, sizeof(*callchain));
	/* 从外到内积累 callsite；首个非零 scc 终止扫描并成为键尾。 */
	for (i = 0; i <= st->curframe; i++) {
		insn_idx = bpf_frame_insn_idx(st, i);
		scc = env->insn_aux_data[insn_idx].scc;
		if (scc) {
			callchain->scc = scc;
			break;
		} else if (i < st->curframe) {
			/* 尚未进入 SCC 的非顶帧以其调用指令区分后续访问实例。 */
			callchain->callsites[i] = insn_idx;
		} else {
			return false;
		}
	}
	return true;
}

/* Check if bpf_scc_visit instance for @callchain exists. */
/*
 * 中文：在指定 SCC 的 visits 柔性数组中线性查找 callchain 完全相同的访问实例。
 * 业务背景：进入、退出和回边登记必须汇合到同一次“调用链 + SCC”访问。
 * 入参：env/callchain 都是借用输入，callchain->scc 必须是有效非零编号。
 * 出参/返回：命中返回借用 visit，未分配或未命中返回 NULL；无副作用。
 * 注意事项：当前源码在检查 info 是否为 NULL 前即求值 info->visits；调用契约必须
 * 保证该槽已分配，否则会空指针解引用。函数不睡眠，返回值不延长生命周期。
 */
static struct bpf_scc_visit *scc_visit_lookup(struct bpf_verifier_env *env,
					      struct bpf_scc_callchain *callchain)
{
	struct bpf_scc_info *info = env->scc_info[callchain->scc];
	struct bpf_scc_visit *visits = info->visits;
	u32 i;

	if (!info)
		return NULL;
	/* info 存活期间 visits 内存稳定；返回项只是该柔性数组内的借用地址。 */
	for (i = 0; i < info->num_visits; i++)
		if (memcmp(callchain, &visits[i].callchain, sizeof(*callchain)) == 0)
			return &visits[i];
	return NULL;
}

/* Allocate a new bpf_scc_visit instance corresponding to @callchain.
 * Allocated instances are alive for a duration of the do_check_common()
 * call and are freed by free_states().
 */
/*
 * 中文：为 callchain 扩展对应 SCC 的 visits 数组；对象存活到 do_check_common()
 * 结束并由 free_states() 释放。
 *
 * 业务背景：首次进入一条 SCC 调用路径时需要持久容器保存入口与回边集合。
 * 入参：env 为输入输出所有者；callchain 为借用键，scc 必须在 scc_info 范围内。
 * 出参/返回：成功返回新元素的借用指针并递增 num_visits；分配失败返回 NULL，
 * 原槽仍有效且内容不变。
 * 注意事项：kvrealloc 可能搬迁整个 info，成功后旧指针失效，故先发布新地址再
 * 初始化尾元素；GFP_KERNEL_ACCOUNT 允许睡眠。
 */
static struct bpf_scc_visit *scc_visit_alloc(struct bpf_verifier_env *env,
					     struct bpf_scc_callchain *callchain)
{
	struct bpf_scc_visit *visit;
	struct bpf_scc_info *info;
	u32 scc, num_visits;
	u64 new_sz;

	/* 旧 info 可空；num_visits 同时决定扩容大小和新元素下标。 */
	scc = callchain->scc;
	info = env->scc_info[scc];
	num_visits = info ? info->num_visits : 0;
	new_sz = sizeof(*info) + sizeof(struct bpf_scc_visit) * (num_visits + 1);
	/* 扩容失败保留旧槽；成功后立即更新 env，旧 info 地址不得再使用。 */
	info = kvrealloc(env->scc_info[scc], new_sz, GFP_KERNEL_ACCOUNT);
	if (!info)
		return NULL;
	env->scc_info[scc] = info;
	info->num_visits = num_visits + 1;
	/* 新尾项先清零再复制键，entry_state/backedges 初始均为空。 */
	visit = &info->visits[num_visits];
	memset(visit, 0, sizeof(*visit));
	memcpy(&visit->callchain, callchain, sizeof(*callchain));
	return visit;
}

/* Form a string '(callsite#1,callsite#2,...,scc)' in env->tmp_str_buf */
/*
 * 中文：把调用点序列和 SCC 编号格式化为“(callsite,...,scc)”诊断文本。
 * 业务背景：SCC 日志需要可区分不同调用实例的短标识。
 * 入参：env 提供共享临时缓冲；callchain 是借用只读键。
 * 出参/返回：返回 env->tmp_str_buf 的借用指针；后续格式化会覆盖，无新分配。
 * 注意事项：只可在当前验证线程、下一次使用临时缓冲前读取；不睡眠。
 */
static char *format_callchain(struct bpf_verifier_env *env, struct bpf_scc_callchain *callchain)
{
	char *buf = env->tmp_str_buf;
	int i, delta = 0;

	delta += snprintf(buf + delta, TMP_STR_BUF_LEN - delta, "(");
	/* callsites 的首个 0 结束有效序列，最后无条件追加 SCC 编号和右括号。 */
	for (i = 0; i < ARRAY_SIZE(callchain->callsites); i++) {
		if (!callchain->callsites[i])
			break;
		delta += snprintf(buf + delta, TMP_STR_BUF_LEN - delta, "%u,",
				  callchain->callsites[i]);
	}
	delta += snprintf(buf + delta, TMP_STR_BUF_LEN - delta, "%u)", callchain->scc);
	return env->tmp_str_buf;
}

/* If callchain for @st exists (@st is in some SCC), ensure that
 * bpf_scc_visit instance for this callchain exists.
 * If instance does not exist or is empty, assign visit->entry_state to @st.
 */
/*
 * 中文：若 st 位于某个 SCC，确保相应 callchain visit 存在；空 visit 记录 st 为
 * 本轮入口，已有入口则保持不变。
 *
 * 业务背景：只有记住一次 SCC 访问的入口，所有分支结束时才能回灌回边标记。
 * 入参：env 为输入输出验证环境；st 为借用状态，记录为 entry_state 后必须保持
 * 存活，直到 maybe_exit_scc() 清空该字段。
 * 出参/返回：成功或不在 SCC 返回 0；扩容失败返回 -ENOMEM，无新入口发布。
 * 注意事项：分配可睡眠；entry_state 是裸借用而非引用，其生命周期由 branches
 * 与延迟释放协议保证。
 */
static int maybe_enter_scc(struct bpf_verifier_env *env, struct bpf_verifier_state *st)
{
	struct bpf_scc_callchain *callchain = &env->callchain_buf;
	struct bpf_scc_visit *visit;

	if (!compute_scc_callchain(env, st, callchain))
		return 0;
	/* 先复用既有实例，否则扩容；?: GNU 简写只对空指针调用分配路径。 */
	visit = scc_visit_lookup(env, callchain);
	visit = visit ?: scc_visit_alloc(env, callchain);
	if (!visit)
		return -ENOMEM;
	/* entry_state 的首次非空写是本次 SCC 访问的入口发布点。 */
	if (!visit->entry_state) {
		visit->entry_state = st;
		if (env->log.level & BPF_LOG_LEVEL2)
			verbose(env, "SCC enter %s\n", format_callchain(env, callchain));
	}
	return 0;
}

/*
 * 业务背景：SCC 退出路径需提前引用后文的回边固定点传播实现。
 * 入参：env 为输入输出验证环境；visit 为拥有回边链的 SCC 访问实例。
 * 出参/返回：声明返回 0 或传播错误；释放语义及完整契约见下文定义。
 * 注意事项：这里只建立内部声明，不消费 visit，也不触发分配或释放。
 */
static int propagate_backedges(struct bpf_verifier_env *env, struct bpf_scc_visit *visit);

/* If callchain for @st exists (@st is in some SCC), make it empty:
 * - set visit->entry_state to NULL;
 * - flush accumulated backedges.
 */
/*
 * 中文：路径离开 SCC 入口状态时，清空 entry_state、移除回边统计，再把回边的
 * 读/精度标记传播到不动点。
 * 业务背景：同一入口派生的所有分支完成后，循环内延迟标记才可最终结算。
 * 入参：env 为输入输出环境；st 是刚把 branches 降为零的借用状态。
 * 出参/返回：无 SCC、非入口或推测特例返回 0；失败返回 -EFAULT 或传播 errno。
 * 注意事项：清空入口和计数后不回滚；非推测状态缺 visit 表示验证器 bug。
 */
static int maybe_exit_scc(struct bpf_verifier_env *env, struct bpf_verifier_state *st)
{
	struct bpf_scc_callchain *callchain = &env->callchain_buf;
	struct bpf_scc_visit *visit;

	if (!compute_scc_callchain(env, st, callchain))
		return 0;
	visit = scc_visit_lookup(env, callchain);
	if (!visit) {
		/*
		 * If path traversal stops inside an SCC, corresponding bpf_scc_visit
		 * must exist for non-speculative paths. For non-speculative paths
		 * traversal stops when:
		 * a. Verification error is found, maybe_exit_scc() is not called.
		 * b. Top level BPF_EXIT is reached. Top level BPF_EXIT is not a member
		 *    of any SCC.
		 * c. A checkpoint is reached and matched. Checkpoints are created by
		 *    is_state_visited(), which calls maybe_enter_scc(), which allocates
		 *    bpf_scc_visit instances for checkpoints within SCCs.
		 * (c) is the only case that can reach this point.
		 */
		/* 中文：上述三类中只有检查点命中能到这里，且进入时理应已创建 visit。 */
		if (!st->speculative) {
			verifier_bug(env, "scc exit: no visit info for call chain %s",
				     format_callchain(env, callchain));
			return -EFAULT;
		}
		return 0;
	}
	if (visit->entry_state != st)
		return 0;
	/* 只有本轮入口负责结算；内部路径结束不能提前清空整个 visit。 */
	if (env->log.level & BPF_LOG_LEVEL2)
		verbose(env, "SCC exit %s\n", format_callchain(env, callchain));
	visit->entry_state = NULL;
	env->num_backedges -= visit->num_backedges;
	visit->num_backedges = 0;
	update_peak_states(env);
	return propagate_backedges(env, visit);
}

/* Lookup an bpf_scc_visit instance corresponding to @st callchain
 * and add @backedge to visit->backedges. @st callchain must exist.
 */
/*
 * 中文：定位 st 的 SCC visit，把 backedge 头插并同步局部/全局回边计数。
 * 业务背景：循环尚未遍历完整时命中的等价状态须暂存，供退出时传播标记。
 * 入参：env/st 为借用；backedge 是新对象，成功后 ownership 转移给 visit，
 * 失败仍归调用者释放。
 * 出参/返回：成功返回 0；缺 SCC 或 visit 返回 -EFAULT，链表不变。
 * 注意事项：发布后由 bpf_free_backedges() 释放；验证线程串行、无锁。
 */
static int add_scc_backedge(struct bpf_verifier_env *env,
			    struct bpf_verifier_state *st,
			    struct bpf_scc_backedge *backedge)
{
	struct bpf_scc_callchain *callchain = &env->callchain_buf;
	struct bpf_scc_visit *visit;

	/* 先证明 st 确实位于某 SCC，失败不消费调用者持有的 backedge。 */
	if (!compute_scc_callchain(env, st, callchain)) {
		verifier_bug(env, "add backedge: no SCC in verification path, insn_idx %d",
			     st->insn_idx);
		return -EFAULT;
	}
	/* callchain 必须在进入检查点时已有 visit，否则回边没有可靠所有者。 */
	visit = scc_visit_lookup(env, callchain);
	if (!visit) {
		verifier_bug(env, "add backedge: no visit info for call chain %s",
			     format_callchain(env, callchain));
		return -EFAULT;
	}
	if (env->log.level & BPF_LOG_LEVEL2)
		verbose(env, "SCC backedge %s\n", format_callchain(env, callchain));
	/* 先接 next 再改表头是完整的单线程头插发布顺序。 */
	backedge->next = visit->backedges;
	visit->backedges = backedge;
	visit->num_backedges++;
	env->num_backedges++;
	update_peak_states(env);
	return 0;
}

/* bpf_reg_state->live marks for registers in a state @st are incomplete,
 * if state @st is in some SCC and not all execution paths starting at this
 * SCC are fully explored.
 */
/*
 * 中文：若 st 的 callchain visit 仍挂有回边，说明 SCC 尚有路径未完成，寄存器
 * read 标记以后仍可能回灌。
 * 业务背景：这种旧状态不能释放，也不能用宽松等价规则过早剪枝。
 * 入参：env/st 均为借用只读对象，st 可以不属于 SCC。
 * 出参/返回：存在待传播回边返回 true，否则 false；无副作用、不睡眠。
 * 注意事项：返回值不持有 visit，只在串行验证的当前判断点有效。
 */
static bool incomplete_read_marks(struct bpf_verifier_env *env,
				  struct bpf_verifier_state *st)
{
	struct bpf_scc_callchain *callchain = &env->callchain_buf;
	struct bpf_scc_visit *visit;

	if (!compute_scc_callchain(env, st, callchain))
		return false;
	/* visit 不存在等价于从未积累回边；存在时链表非空就是“不完整”标志。 */
	visit = scc_visit_lookup(env, callchain);
	if (!visit)
		return false;
	return !!visit->backedges;
}

/*
 * 业务背景：一条路径结束后沿 parent 链递减待探索分支，触发 SCC 退出并回收
 * 不再被子状态引用的淘汰缓存。
 * 入参：env 为输入输出环境；st 为结束路径的借用状态，可沿 parent 直到 NULL。
 * 出参/返回：成功返回 0；SCC 传播失败返回负 errno，已递减计数不回滚。
 * 注意事项：branches 必须为正；先保存 parent 再可能释放容器，释放后禁用旧 sl。
 */
int bpf_update_branch_counts(struct bpf_verifier_env *env, struct bpf_verifier_state *st)
{
	struct bpf_verifier_state_list *sl = NULL, *parent_sl;
	struct bpf_verifier_state *parent;
	int err;

	while (st) {
		u32 br = --st->branches;

		/* verifier_bug_if(br > 1, ...) technically makes sense here,
		 * but see comment in push_stack(), hence:
		 */
		/* 中文：此处看似可断言 br<=1，但 push_stack() 的分支协议允许更大值。 */
		verifier_bug_if((int)br < 0, env, "%s:branches_to_explore=%d", __func__, br);
		if (br)
			break;
		/* 最后一条子分支结束：先完成 SCC 标记，再继续结算父状态。 */
		err = maybe_exit_scc(env, st);
		if (err)
			return err;
		/* 保存父指针和父容器后，上一轮 sl 才允许被最终释放。 */
		parent = st->parent;
		parent_sl = state_parent_as_list(st);
		if (sl)
			maybe_free_verifier_state(env, sl);
		st = parent;
		sl = parent_sl;
	}
	return 0;
}

/* check %cur's range satisfies %old's */
/*
 * 中文：检查当前 64/32 位环形数域是否均为旧状态允许集合的子集。
 * 业务背景：旧状态已安全时，更窄的当前范围可以复用该结论并剪枝。
 * 入参：old/cur 为借用寄存器快照，只读 r64/r32。
 * 出参/返回：两个 cnum 子集关系都成立返回 true；无副作用。
 * 注意事项：参数方向是“old 容纳 cur”，颠倒会错误接受更宽状态。不睡眠。
 */
static bool range_within(const struct bpf_reg_state *old,
			 const struct bpf_reg_state *cur)
{
	return cnum64_is_subset(old->r64, cur->r64) &&
	       cnum32_is_subset(old->r32, cur->r32);
}

/* If in the old state two registers had the same id, then they need to have
 * the same id in the new state as well.  But that id could be different from
 * the old state, so we need to track the mapping from old to new ids.
 * Once we have seen that, say, a reg with old id 5 had new id 9, any subsequent
 * regs with old id 5 must also have new id 9 for the new state to be safe.  But
 * regs with a different old id could still have new id 9, we don't care about
 * that.
 * So we look through our idmap to see if this old id has been seen before.  If
 * so, we require the new id to match; otherwise, we add the id pair to the map.
 */
/*
 * 中文：旧状态中同 ID 的对象在当前状态也必须映射为同一个 ID，但编号可重命名；
 * 首次遇到登记 old→cur，后续必须复用，反向冲突也被拒绝。
 * 业务背景：剪枝必须保留寄存器、栈槽和引用的别名关系，而非比较偶然编号。
 * 入参：old_id/cur_id 是待配对编号，0 表示无 ID；idmap 是输入输出临时映射。
 * 出参/返回：关系一致或新增成功返回 true；不匹配、冲突或容量耗尽返回 false。
 * 注意事项：容量不足时保守地继续验证，不会错误剪枝；无分配、不睡眠。
 */
static bool check_ids(u32 old_id, u32 cur_id, struct bpf_idmap *idmap)
{
	struct bpf_id_pair *map = idmap->map;
	unsigned int i;

	/* either both IDs should be set or both should be zero */
	/* 中文：old/cur 必须同时有 ID 或同时为 0，不能凭空新增或丢失旧身份。 */
	if (!!old_id != !!cur_id)
		return false;

	if (old_id == 0) /* cur_id == 0 as well */
		/* 中文：由上面的成对检查可知，此时 cur_id 也必为 0。 */
		return true;

	/* 已有映射命中立即判定；没有命中才可能占用一个新槽。 */
	for (i = 0; i < idmap->cnt; i++) {
		/* 双向唯一性同时保留旧别名组和当前独立对象的区别。 */
		if (map[i].old == old_id)
			return map[i].cur == cur_id;
		if (map[i].cur == cur_id)
			return false;
	}

	/* Reached the end of known mappings; haven't seen this id before */
	/* 首次关系占用尾部槽并递增有效计数，之后的比较即可复用。 */
	if (idmap->cnt < BPF_ID_MAP_SIZE) {
		map[idmap->cnt].old = old_id;
		map[idmap->cnt].cur = cur_id;
		idmap->cnt++;
		return true;
	}

	/*
	 * idmap slots are bounded by the number of registers and stack slots.
	 * Since referenced dynptrs acquire intermediate references that do
	 * not live in either, so the map can be exhausted. Since it is unlikely,
	 * fail the verification by treating the states as not equivalent.
	 */
	/* 中文：dynptr 中间引用可能耗尽定长表；此时保守判“不等价”并继续验证。 */
	return false;
}

/*
 * Compare scalar register IDs for state equivalence.
 *
 * When old_id == 0, the old register is independent - not linked to any
 * other register. Any linking in the current state only adds constraints,
 * making it more restrictive. Since the old state didn't rely on any ID
 * relationships for this register, it's always safe to accept cur regardless
 * of its ID. Hence, return true immediately.
 *
 * When old_id != 0 but cur_id == 0, we need to ensure that different
 * independent registers in cur don't incorrectly satisfy the ID matching
 * requirements of linked registers in old.
 *
 * Example: if old has r6.id=X and r7.id=X (linked), but cur has r6.id=0
 * and r7.id=0 (both independent), without temp IDs both would map old_id=X
 * to cur_id=0 and pass. With temp IDs: r6 maps X->temp1, r7 tries to map
 * X->temp2, but X is already mapped to temp1, so the check fails correctly.
 *
 * When old_id has BPF_ADD_CONST set, the compound id (base | flag) and the
 * base id (flag stripped) must both map consistently. Example: old has
 * r2.id=A, r3.id=A|flag (r3 = r2 + delta), cur has r2.id=B, r3.id=C|flag
 * (r3 derived from unrelated r4). Without the base check, idmap gets two
 * independent entries A->B and A|flag->C|flag, missing that A->C conflicts
 * with A->B. The base ID cross-check catches this.
 */
/*
 * 中文：old_id 为 0 时旧状态不依赖链接，cur 增加约束仍安全；若 cur 无 ID，
 * 为每个独立值生成临时 ID，避免两个独立寄存器冒充旧状态的同一链接组。
 * BPF_ADD_CONST 还必须核对去标志的 base ID，防止不同基值被误配。
 * 业务背景：标量 ID 决定条件收紧是否联动传播，是安全剪枝的一部分。
 * 入参：两个 ID 为链接编码；idmap 为输入输出映射及临时 ID 生成器。
 * 出参/返回：当前关系至少与旧关系一样严格返回 true，否则 false；可能更新 idmap。
 * 注意事项：临时 ID 只在本次比较有效；无分配、无锁、不睡眠。
 */
static bool check_scalar_ids(u32 old_id, u32 cur_id, struct bpf_idmap *idmap)
{
	if (!old_id)
		return true;

	cur_id = cur_id ? cur_id : ++idmap->tmp_id_gen;

	/* 先比较完整复合 ID；带 ADD_CONST 时再比较去标志的共享基 ID。 */
	if (!check_ids(old_id, cur_id, idmap))
		return false;
	if (old_id & BPF_ADD_CONST) {
		old_id &= ~BPF_ADD_CONST;
		cur_id &= ~BPF_ADD_CONST;
		if (!check_ids(old_id, cur_id, idmap))
			return false;
	}
	return true;
}

/*
 * 业务背景：比较检查点前清除以后不会读取的寄存器/半栈槽，减少无关差异。
 * 入参：env 为借用环境；st 为当前帧输入输出状态；live_regs 是活寄存器位图；
 * frame 是栈活性查询使用的帧号。
 * 出参/返回：无直接返回值；死寄存器置未初始化，可清理字节置 POISON，并按需
 * 保留低半标量的 MISC/ZERO 信息。
 * 注意事项：dynptr/iterator/irq 槽及指针 spill 不能丢元数据；只改死值，不睡眠。
 */
static void __clean_func_state(struct bpf_verifier_env *env,
			       struct bpf_func_state *st,
			       u16 live_regs, int frame)
{
	int i, j;

	for (i = 0; i < BPF_REG_FP; i++) {
		/* liveness must not touch this register anymore */
		/* 中文：活性结果保证死寄存器以后不会再被读取或传播。 */
		if (!(live_regs & BIT(i)))
			/* since the register is unused, clear its state
			 * to make further comparison simpler
			 */
			/* 中文：未使用寄存器可规范化为 NOT_INIT，使等价比较忽略旧内容。 */
			bpf_mark_reg_not_init(env, &st->regs[i]);
	}

	/*
	 * Clean dead 4-byte halves within each SPI independently.
	 * half_spi 2*i   → lower half: slot_type[0..3] (closer to FP)
	 * half_spi 2*i+1 → upper half: slot_type[4..7] (farther from FP)
	 */
	/* 中文：每个 8 字节 SPI 分成靠近 FP 的低半和更远的高半，分别查询活性。 */
	for (i = 0; i < st->allocated_stack / BPF_REG_SIZE; i++) {
		bool lo_live = bpf_stack_slot_alive(env, frame, i * 2);
		bool hi_live = bpf_stack_slot_alive(env, frame, i * 2 + 1);

		if (!hi_live || !lo_live) {
			int start = !lo_live ? 0 : BPF_REG_SIZE / 2;
			int end = !hi_live ? BPF_REG_SIZE : BPF_REG_SIZE / 2;
			u8 stype = st->stack[i].slot_type[7];

			/*
			 * Don't clear special slots.
			 * destroy_if_dynptr_stack_slot() needs STACK_DYNPTR to
			 * detect overwrites and invalidate associated data slices.
			 * is_iter_reg_valid_uninit() and is_irq_flag_reg_valid_uninit()
			 * check for their respective slot types to detect double-create.
			 */
			/* 中文：特殊槽必须保留类型，覆盖/二次创建检查靠它定位仍存活的资源。 */
			if (stype == STACK_DYNPTR || stype == STACK_ITER ||
			    stype == STACK_IRQ_FLAG)
				continue;

			/*
			 * Only scalar spills can be degraded to raw stack bytes
			 * when their high half is dead. Pointer spills need the
			 * saved spilled_ptr metadata so partial fills keep
			 * rejecting as non-scalar register fills.
			 */
			/* 中文：仅标量 spill 的死高半可降级；指针必须保留整项类型元数据。 */
			if (!hi_live) {
				struct bpf_reg_state *spill = &st->stack[i].spilled_ptr;

				if (lo_live && stype == STACK_SPILL) {
					u8 val = STACK_MISC;

					/* 低半仍活时只允许标量降级；指针的部分读取必须继续被拒绝。 */
					if (spill->type != SCALAR_VALUE)
						continue;

					/*
					 * 8 byte spill of scalar 0 where half slot is dead
					 * should become STACK_ZERO in lo 4 bytes.
					 */
					/* 中文：全零标量的活低半仍精确为零，其余标量降为未知字节。 */
					if (bpf_register_is_null(spill))
						val = STACK_ZERO;
					for (j = 0; j < 4; j++) {
						u8 *t = &st->stack[i].slot_type[j];

						if (*t == STACK_SPILL)
							*t = val;
					}
				}
				/* spilled_ptr 已无完整 8 字节语义；剩余死半槽统一毒化。 */
				bpf_mark_reg_not_init(env, spill);
			}
			for (j = start; j < end; j++)
				st->stack[i].slot_type[j] = STACK_POISON;
		}
	}
}

/*
 * 业务背景：状态等价比较前，为调用栈全部帧应用已计算的栈/寄存器活性结果。
 * 入参：env 为借用环境；st 是待原地规范化的验证器状态。
 * 出参/返回：成功返回 0 并清理各帧死值；保留查询初始化的 errno 传播接口，当前
 * 初始化实现固定返回 0。
 * 注意事项：live_regs_before 按各帧当前指令读取；错误必须由调用者传播。
 */
static int clean_verifier_state(struct bpf_verifier_env *env,
				 struct bpf_verifier_state *st)
{
	int i, err;

	err = bpf_live_stack_query_init(env, st);
	if (err)
		return err;
	/* 查询上下文就绪后逐帧用该帧入口的 live_regs_before 清洗。 */
	for (i = 0; i <= st->curframe; i++) {
		u32 ip = bpf_frame_insn_idx(st, i);
		u16 live_regs = env->insn_aux_data[ip].live_regs_before;

		__clean_func_state(env, st->frame[i], live_regs, i);
	}
	return 0;
}

/*
 * 业务背景：精确模式需比较寄存器全部语义字段，同时允许 ID 一致重命名。
 * 入参：rold/rcur 为借用快照；idmap 为本次比较的输入输出映射。
 * 出参/返回：id 之前的字节、id 与 parent_id 全部等价返回 true，否则 false。
 * 注意事项：offsetof(id) 使新增前置字段自动纳入比较；无分配、不睡眠。
 */
static bool regs_exact(const struct bpf_reg_state *rold,
		       const struct bpf_reg_state *rcur,
		       struct bpf_idmap *idmap)
{
	return memcmp(rold, rcur, offsetof(struct bpf_reg_state, id)) == 0 &&
	       check_ids(rold->id, rcur->id, idmap) &&
	       check_ids(rold->parent_id, rcur->parent_id, idmap);
}

enum exact_level {
	/* 普通剪枝：不精确标量可忽略范围，当前状态只需不比旧状态更宽松。 */
	NOT_EXACT,
	/* 无限循环等检查使用：除可重命名 ID 外必须逐字段完全相等。 */
	EXACT,
	/* SCC/迭代器收敛：仍检查范围包含，但允许尚未最终化的精度标记差异。 */
	RANGE_WITHIN
};

/* Returns true if (rold safe implies rcur safe) */
/*
 * 中文：返回 true 表示“rold 已安全”蕴含“rcur 也安全”，即当前状态可被旧状态
 * 覆盖；方向不能颠倒。
 * 业务背景：这是寄存器级状态剪枝核心，按类型选择精确、范围包含或身份关系。
 * 入参：env 为借用环境；rold/rcur 为旧/当前寄存器借用快照；idmap 记录跨字段
 * 一致重命名；exact 选择上述三种比较强度。
 * 出参/返回：安全覆盖返回 true，否则 false；可能向 idmap 添加映射，无其他副作用。
 * 注意事项：类型含所有修饰位必须相同以防指针泄漏；函数不取得引用、不睡眠。
 */
static bool regsafe(struct bpf_verifier_env *env, struct bpf_reg_state *rold,
		    struct bpf_reg_state *rcur, struct bpf_idmap *idmap,
		    enum exact_level exact)
{
	if (exact == EXACT)
		return regs_exact(rold, rcur, idmap);

	if (rold->type == NOT_INIT)
		/* explored state can't have used this */
		/* 中文：旧安全路径从未用该寄存器，当前更具体的值不会破坏结论。 */
		return true;

	/* Enforce that register types have to match exactly, including their
	 * modifiers (like PTR_MAYBE_NULL, MEM_RDONLY, etc), as a general
	 * rule.
	 *
	 * One can make a point that using a pointer register as unbounded
	 * SCALAR would be technically acceptable, but this could lead to
	 * pointer leaks because scalars are allowed to leak while pointers
	 * are not. We could make this safe in special cases if root is
	 * calling us, but it's probably not worth the hassle.
	 *
	 * Also, register types that are *not* MAYBE_NULL could technically be
	 * safe to use as their MAYBE_NULL variants (e.g., PTR_TO_MAP_VALUE
	 * is safe to be used as PTR_TO_MAP_VALUE_OR_NULL, provided both point
	 * to the same map).
	 * However, if the old MAYBE_NULL register then got NULL checked,
	 * doing so could have affected others with the same id, and we can't
	 * check for that because we lost the id when we converted to
	 * a non-MAYBE_NULL variant.
	 * So, as a general rule we don't allow mixing MAYBE_NULL and
	 * non-MAYBE_NULL registers as well.
	 */
	/* 中文：即便理论上某些指针/可空变体可包含，丢失 ID 后无法证明联动 NULL 检查。 */
	if (rold->type != rcur->type)
		return false;

	switch (base_type(rold->type)) {
	case SCALAR_VALUE:
		/* 标量根据模式比较完整状态或“旧范围包含当前范围”及链接 ID。 */
		if (env->explore_alu_limits) {
			/* explore_alu_limits disables tnum_in() and range_within()
			 * logic and requires everything to be strict
			 */
			/* 中文：探索 ALU 极限时禁用集合包含快路，要求字段严格相同。 */
			return memcmp(rold, rcur, offsetof(struct bpf_reg_state, id)) == 0 &&
			       check_scalar_ids(rold->id, rcur->id, idmap);
		}
		if (!rold->precise && exact == NOT_EXACT)
			return true;
		/*
		 * Linked register tracking uses rold->id to detect relationships.
		 * When rold->id == 0, the register is independent and any linking
		 * in rcur only adds constraints. When rold->id != 0, we must verify
		 * id mapping and (for BPF_ADD_CONST) offset consistency.
		 *
		 * +------------------+-----------+------------------+---------------+
		 * |                  | rold->id  | rold + ADD_CONST | rold->id == 0 |
		 * |------------------+-----------+------------------+---------------|
		 * | rcur->id         | range,ids | false            | range         |
		 * | rcur + ADD_CONST | false     | range,ids,off    | range         |
		 * | rcur->id == 0    | range,ids | false            | range         |
		 * +------------------+-----------+------------------+---------------+
		 *
		 * Why check_ids() for scalar registers?
		 *
		 * Consider the following BPF code:
		 *   1: r6 = ... unbound scalar, ID=a ...
		 *   2: r7 = ... unbound scalar, ID=b ...
		 *   3: if (r6 > r7) goto +1
		 *   4: r6 = r7
		 *   5: if (r6 > X) goto ...
		 *   6: ... memory operation using r7 ...
		 *
		 * First verification path is [1-6]:
		 * - at (4) same bpf_reg_state::id (b) would be assigned to r6 and r7;
		 * - at (5) r6 would be marked <= X, sync_linked_regs() would also mark
		 *   r7 <= X, because r6 and r7 share same id.
		 * Next verification path is [1-4, 6].
		 *
		 * Instruction (6) would be reached in two states:
		 *   I.  r6{.id=b}, r7{.id=b} via path 1-6;
		 *   II. r6{.id=a}, r7{.id=b} via path 1-4, 6.
		 *
		 * Use check_ids() to distinguish these states.
		 * ---
		 * Also verify that new value satisfies old value range knowledge.
		 */
		/*
		 * 中文：示例中旧路径的 r6/r7 同 ID 会让 r6<=X 联动收紧 r7；新路径若
		 * 二者不同 ID 就不能复用该安全结论，因此范围检查之外必须核对 ID 映射。
		 */

		/*
		 * ADD_CONST flags must match exactly: BPF_ADD_CONST32 and
		 * BPF_ADD_CONST64 have different linking semantics in
		 * sync_linked_regs() (alu32 zero-extends, alu64 does not),
		 * so pruning across different flag types is unsafe.
		 */
		/* 中文：32 位加法会清高位、64 位不会，ADD_CONST32/64 的联动语义不可混用。 */
		if (rold->id &&
		    (rold->id & BPF_ADD_CONST) != (rcur->id & BPF_ADD_CONST))
			return false;

		/* Both have offset linkage: offsets must match */
		/* 中文：两侧都带常量偏移链接时，delta 也必须完全一致。 */
		if ((rold->id & BPF_ADD_CONST) && rold->delta != rcur->delta)
			return false;

		if (!check_scalar_ids(rold->id, rcur->id, idmap))
			return false;

		/* cnum 与 tnum 两种抽象都必须证明当前集合包含于旧集合。 */
		return range_within(rold, rcur) && tnum_in(rold->var_off, rcur->var_off);
	case PTR_TO_MAP_KEY:
	case PTR_TO_MAP_VALUE:
	case PTR_TO_MEM:
	case PTR_TO_BUF:
	case PTR_TO_TP_BUFFER:
		/* 普通内存类指针要求固定元数据相同，且当前数值域不超出旧安全域。 */
		/* If the new min/max/var_off satisfy the old ones and
		 * everything else matches, we are OK.
		 */
		/* 中文：固定字段相同且当前 min/max/var_off 落在旧知识内即可覆盖。 */
		return memcmp(rold, rcur, offsetof(struct bpf_reg_state, var_off)) == 0 &&
		       range_within(rold, rcur) &&
		       tnum_in(rold->var_off, rcur->var_off) &&
		       check_ids(rold->id, rcur->id, idmap) &&
		       check_ids(rold->parent_id, rcur->parent_id, idmap);
	case PTR_TO_PACKET_META:
	case PTR_TO_PACKET:
		/* 包指针还需保留至少与旧状态一样大的已验证可访问范围及 ID 关系。 */
		/* We must have at least as much range as the old ptr
		 * did, so that any accesses which were safe before are
		 * still safe.  This is true even if old range < old off,
		 * since someone could have accessed through (ptr - k), or
		 * even done ptr -= k in a register, to get a safe access.
		 */
		/* 中文：当前包指针可访问 range 至少与旧值一样大，包含先减偏移再访问的用法。 */
		if (rold->range < 0 || rcur->range < 0) {
			/* special case for [BEYOND|AT]_PKT_END */
			/* 中文：负 range 编码包尾相对状态，只能按哨兵值精确匹配。 */
			if (rold->range != rcur->range)
				return false;
		} else if (rold->range > rcur->range) {
			return false;
		}
		/* range 合格后仍要保留别名关系，并验证数值偏移域没有变宽。 */
		/* id relations must be preserved */
		/* 中文：包指针身份关系必须保留。 */
		if (!check_ids(rold->id, rcur->id, idmap))
			return false;
		/* new val must satisfy old val knowledge */
		/* 中文：最后以 cnum 与 tnum 双重检查偏移数域包含。 */
		return range_within(rold, rcur) &&
		       tnum_in(rold->var_off, rcur->var_off);
	case PTR_TO_STACK:
		/* 栈偏移相同仍不够：frameno 区分不同调用帧的 fp-8。 */
		/* two stack pointers are equal only if they're pointing to
		 * the same stack frame, since fp-8 in foo != fp-8 in bar
		 */
		/* 中文：不同帧的同一 fp 偏移指向不同对象，因此 frameno 必须一致。 */
		return regs_exact(rold, rcur, idmap) && rold->frameno == rcur->frameno;
	case PTR_TO_ARENA:
		/* arena 指针的可用性由类型本身表达，当前剪枝无需比较数值字段。 */
		return true;
	case PTR_TO_INSN:
		return memcmp(rold, rcur, offsetof(struct bpf_reg_state, var_off)) == 0 &&
		       range_within(rold, rcur) && tnum_in(rold->var_off, rcur->var_off);
	default:
		/* 未专门支持的引用类型采取最保守的精确字段与 ID 比较。 */
		return regs_exact(rold, rcur, idmap);
	}
}

/*
 * 未约束标量的只读模板：初始化后代表任意标量值，供 MISC/INVALID 栈字节合成
 * 比较用寄存器；对象静态存活，调用者只借用，不能修改或释放。
 */
static struct bpf_reg_state unbound_reg;

/*
 * 业务背景：启动后为上述栈比较模板建立“未知且不精确”的规范标量状态。
 * 入参：无。
 * 出参/返回：初始化成功固定返回 0；副作用是写一次静态 unbound_reg。
 * 注意事项：由 late_initcall 在内核初始化期调用，之后模板只读；不分配、不失败。
 */
static __init int unbound_reg_init(void)
{
	bpf_mark_reg_unknown_imprecise(&unbound_reg);
	return 0;
}
late_initcall(unbound_reg_init);

/*
 * 业务背景：半槽比较需判断从字节 im 起是否仍可视为同一标量 spill。
 * 入参：stack 为借用栈槽；im 是 0..7 的字节索引。
 * 出参/返回：该位置标为 SPILL 且保存寄存器为标量时返回 true；无副作用。
 * 注意事项：调用者保证索引有效；不睡眠。
 */
static bool is_spilled_scalar_after(const struct bpf_stack_state *stack, int im)
{
	return stack->slot_type[im] == STACK_SPILL &&
	       stack->spilled_ptr.type == SCALAR_VALUE;
}

/*
 * 业务背景：MISC 或允许未初始化读取的尾部字节可抽象成任意标量参与比较。
 * 入参：env 提供 allow_uninit_stack 策略；stack 为借用槽；im 是起始字节。
 * 出参/返回：从 im 到槽尾全部满足可作为 MISC 的条件返回 true；无副作用。
 * 注意事项：严格模式下 INVALID/POISON 不合格；线性扫描、不睡眠。
 */
static bool is_stack_misc_after(struct bpf_verifier_env *env,
				struct bpf_stack_state *stack, int im)
{
	u32 i;

	for (i = im; i < ARRAY_SIZE(stack->slot_type); ++i) {
		/* 每个尾部字节都必须是 MISC，或在宽松策略下为未初始化类标记。 */
		if ((stack->slot_type[i] == STACK_MISC) ||
		    ((stack->slot_type[i] == STACK_INVALID || stack->slot_type[i] == STACK_POISON) &&
		     env->allow_uninit_stack))
			continue;
		return false;
	}

	return true;
}

/*
 * 业务背景：把标量 spill 或无类型字节统一转换成 regsafe() 可比较的寄存器视图。
 * 入参：env/stack 为借用输入；im 是半槽起点。
 * 出参/返回：返回借用的 spilled_ptr、静态 unbound_reg，或不适用时 NULL。
 * 注意事项：返回对象均不转移 ownership；静态模板只读，不睡眠。
 */
static struct bpf_reg_state *scalar_reg_for_stack(struct bpf_verifier_env *env,
						  struct bpf_stack_state *stack, int im)
{
	if (is_spilled_scalar_after(stack, im))
		return &stack->spilled_ptr;

	if (is_stack_misc_after(env, stack, im))
		return &unbound_reg;

	return NULL;
}

/*
 * 业务背景：逐字节比较旧/当前帧栈，确认当前路径仍满足旧路径已经证明的安全性。
 * 入参：env 为借用环境；old/cur 为帧快照；idmap 维护 spill 身份映射；exact 决定
 * 严格相等或安全包含模式。
 * 出参/返回：所有旧状态用过的字节、spill、dynptr、iterator 与 irq 标记均兼容
 * 返回 true，否则 false；只可能更新 idmap。
 * 注意事项：当前额外未使用槽可忽略；旧已用槽不能缺失。函数不持有引用、不睡眠。
 */
static bool stacksafe(struct bpf_verifier_env *env, struct bpf_func_state *old,
		      struct bpf_func_state *cur, struct bpf_idmap *idmap,
		      enum exact_level exact)
{
	int i, spi;

	/* walk slots of the explored stack and ignore any additional
	 * slots in the current stack, since explored(safe) state
	 * didn't use them
	 */
	/* 中文：只遍历旧安全状态用过的槽；当前状态额外分配但旧路径未读取的槽可忽略。 */
	for (i = 0; i < old->allocated_stack; i++) {
		struct bpf_reg_state *old_reg, *cur_reg;
		int im = i % BPF_REG_SIZE;

		spi = i / BPF_REG_SIZE;

		/* 精确模式先规范化 POISON/INVALID，再要求逐字节槽类型一致。 */
		if (exact == EXACT) {
			u8 old_type = old->stack[spi].slot_type[i % BPF_REG_SIZE];
			u8 cur_type = i < cur->allocated_stack ?
				      cur->stack[spi].slot_type[i % BPF_REG_SIZE] : STACK_INVALID;

			/* STACK_INVALID and STACK_POISON are equivalent for pruning */
			/* 中文：剪枝语义下 INVALID 与 POISON 都表示不可依赖的未初始化字节。 */
			if (old_type == STACK_POISON)
				old_type = STACK_INVALID;
			if (cur_type == STACK_POISON)
				cur_type = STACK_INVALID;
			/* 任一有效槽缺失或规范化类型不同都破坏精确等价。 */
			if (i >= cur->allocated_stack || old_type != cur_type)
				return false;
		}

		if (old->stack[spi].slot_type[i % BPF_REG_SIZE] == STACK_INVALID ||
		    old->stack[spi].slot_type[i % BPF_REG_SIZE] == STACK_POISON)
			continue;

		if (env->allow_uninit_stack &&
		    old->stack[spi].slot_type[i % BPF_REG_SIZE] == STACK_MISC)
			continue;

		/* 从此处起旧字节确实参与过安全证明，当前帧必须具有对应槽。 */
		/* explored stack has more populated slots than current stack
		 * and these slots were used
		 */
		/* 中文：旧安全状态使用过的槽在当前状态中不存在，不能复用旧路径结论。 */
		if (i >= cur->allocated_stack)
			return false;

		/*
		 * 64 and 32-bit scalar spills vs MISC/INVALID slots and vice versa.
		 * Load from MISC/INVALID slots produces unbound scalar.
		 * Construct a fake register for such stack and call
		 * regsafe() to ensure scalar ids are compared.
		 */
		/* 中文：把 spill 与原始字节合成寄存器视图，以免跨表示时漏掉标量 ID 关系。 */
		if (im == 0 || im == 4) {
			/* 每个半槽起点尝试把 spill/MISC 统一成标量寄存器比较并整段跳过。 */
			old_reg = scalar_reg_for_stack(env, &old->stack[spi], im);
			cur_reg = scalar_reg_for_stack(env, &cur->stack[spi], im);
			if (old_reg && cur_reg) {
				if (!regsafe(env, old_reg, cur_reg, idmap, exact))
					return false;
				i += (im == 0 ? BPF_REG_SIZE - 1 : 3);
				continue;
			}
		}
		/* 无法统一成标量视图时回到逐字节类型及完整特殊槽比较。 */

		/* if old state was safe with misc data in the stack
		 * it will be safe with zero-initialized stack.
		 * The opposite is not true
		 */
		/* 中文：旧路径接受任意 MISC 时当前确定为零更严格；旧零不能覆盖当前任意字节。 */
		if (old->stack[spi].slot_type[i % BPF_REG_SIZE] == STACK_MISC &&
		    cur->stack[spi].slot_type[i % BPF_REG_SIZE] == STACK_ZERO)
			continue;
		/* 除 MISC→ZERO 的安全收紧外，逐字节槽类型必须相同。 */
		if (old->stack[spi].slot_type[i % BPF_REG_SIZE] !=
		    cur->stack[spi].slot_type[i % BPF_REG_SIZE])
			/* Ex: old explored (safe) state has STACK_SPILL in
			 * this stack slot, but current has STACK_MISC ->
			 * this verifier states are not equivalent,
			 * return false to continue verification of this path
			 */
			/* 中文：旧槽保存完整 spill 而当前仅为 MISC 时语义变宽，必须继续验证。 */
			return false;
		if (i % BPF_REG_SIZE != BPF_REG_SIZE - 1)
			continue;
		/* Both old and cur are having same slot_type */
		/* 中文：到这里两侧槽类型相同，再按类型核对其完整载荷。 */
		switch (old->stack[spi].slot_type[BPF_REG_SIZE - 1]) {
		case STACK_SPILL:
			/* 完整 spill 用 regsafe() 比较指针/标量类型、范围和 ID。 */
			/* when explored and current stack slot are both storing
			 * spilled registers, check that stored pointers types
			 * are the same as well.
			 * Ex: explored safe path could have stored
			 * (bpf_reg_state) {.type = PTR_TO_STACK, .off = -8}
			 * but current path has stored:
			 * (bpf_reg_state) {.type = PTR_TO_STACK, .off = -16}
			 * such verifier states are not equivalent.
			 * return false to continue verification of this path
			 */
			/* 中文：同为 spill 仍须比较保存寄存器；例如不同栈偏移绝不等价。 */
			if (!regsafe(env, &old->stack[spi].spilled_ptr,
				     &cur->stack[spi].spilled_ptr, idmap, exact))
				return false;
			break;
		case STACK_DYNPTR:
			/* dynptr 的种类、首槽、拥有/父引用 ID 必须保持一致。 */
			old_reg = &old->stack[spi].spilled_ptr;
			cur_reg = &cur->stack[spi].spilled_ptr;
			if (old_reg->dynptr.type != cur_reg->dynptr.type ||
			    old_reg->dynptr.first_slot != cur_reg->dynptr.first_slot ||
			    !check_ids(old_reg->id, cur_reg->id, idmap) ||
			    !check_ids(old_reg->parent_id, cur_reg->parent_id, idmap))
				return false;
			/* 所有 dynptr 身份字段一致后继续检查下一 SPI。 */
			break;
		case STACK_ITER:
			old_reg = &old->stack[spi].spilled_ptr;
			cur_reg = &cur->stack[spi].spilled_ptr;
			/* iter.depth is not compared between states as it
			 * doesn't matter for correctness and would otherwise
			 * prevent convergence; we maintain it only to prevent
			 * infinite loop check triggering, see
			 * iter_active_depths_differ()
			 */
			/* 中文：depth 仅防误报无限循环，不影响资源语义，比较它会阻止收敛。 */
			if (old_reg->iter.btf != cur_reg->iter.btf ||
			    old_reg->iter.btf_id != cur_reg->iter.btf_id ||
			    old_reg->iter.state != cur_reg->iter.state ||
			    /* ignore {old_reg,cur_reg}->iter.depth, see above */
			    /* 中文：按上文约定忽略两侧 iterator depth。 */
			    !check_ids(old_reg->id, cur_reg->id, idmap))
				return false;
			break;
		case STACK_IRQ_FLAG:
			/* irq 状态按身份和 kfunc 类别配对，避免跨临界区错误剪枝。 */
			old_reg = &old->stack[spi].spilled_ptr;
			cur_reg = &cur->stack[spi].spilled_ptr;
			if (!check_ids(old_reg->id, cur_reg->id, idmap) ||
			    old_reg->irq.kfunc_class != cur_reg->irq.kfunc_class)
				return false;
			/* 同类 IRQ 标记可安全复用旧路径的临界区证明。 */
			break;
		case STACK_MISC:
		case STACK_ZERO:
		case STACK_INVALID:
		case STACK_POISON:
			continue;
		/* Ensure that new unhandled slot types return false by default */
		/* 中文：未知新槽类型默认拒绝，要求新增类型显式定义剪枝语义。 */
		default:
			/* 新增但未建模的槽类型必须保守拒绝，不能落入“相等”快路。 */
			return false;
		}
	}
	return true;
}

/*
 * Compare stack arg slots between old and current states.
 * Outgoing stack args are path-local state and must agree for pruning.
 */
/*
 * 中文：逐个比较 old/cur 的出栈参数槽；一侧缺失时用 NOT_INIT 临时寄存器补齐。
 * 业务背景：跨函数传递的栈参数是路径局部状态，剪枝时必须与寄存器同样安全覆盖。
 * 入参：env、两帧为借用；idmap 为输入输出身份映射；exact 为比较强度。
 * 出参/返回：所有参数 regsafe() 均成立返回 true，否则 false；可能更新 idmap。
 * 注意事项：遍历两侧最大槽数，临时 not_init 仅在本轮栈上有效；不睡眠。
 */
static bool stack_arg_safe(struct bpf_verifier_env *env, struct bpf_func_state *old,
			   struct bpf_func_state *cur, struct bpf_idmap *idmap,
			   enum exact_level exact)
{
	int i, nslots;

	nslots = max(old->out_stack_arg_cnt, cur->out_stack_arg_cnt);
	/* 取最大值才能发现任一侧独有的已初始化参数，而不是静默截断。 */
	for (i = 0; i < nslots; i++) {
		struct bpf_reg_state *old_arg, *cur_arg;
		struct bpf_reg_state not_init = { .type = NOT_INIT };

		old_arg = i < old->out_stack_arg_cnt ?
			  &old->stack_arg_regs[i] : &not_init;
		cur_arg = i < cur->out_stack_arg_cnt ?
			  &cur->stack_arg_regs[i] : &not_init;
		/* regsafe 同时核对参数值域与跨参数 ID 链接。 */
		if (!regsafe(env, old_arg, cur_arg, idmap, exact))
			return false;
	}

	return true;
}

/*
 * 业务背景：即使寄存器数值相容，持有引用、锁、RCU/抢占/IRQ 临界区不同也绝不
 * 能互相剪枝，本函数核对这些资源状态与身份关系。
 * 入参：old/cur 为借用验证状态；idmap 为输入输出 ID 映射。
 * 出参/返回：计数、活动锁身份及每项引用类型/父 ID/资源指针均相容返回 true；
 * 否则 false。只更新 idmap。
 * 注意事项：引用计数保证生命周期而非字段同步；未知引用枚举会 WARN 并拒绝。
 */
static bool refsafe(struct bpf_verifier_state *old, struct bpf_verifier_state *cur,
		    struct bpf_idmap *idmap)
{
	int i;

	if (old->acquired_refs != cur->acquired_refs)
		return false;

	if (old->active_locks != cur->active_locks)
		return false;

	if (old->active_preempt_locks != cur->active_preempt_locks)
		return false;

	/* 各类临界区深度、IRQ 身份和当前资源锁必须逐项保持。 */
	if (old->active_rcu_locks != cur->active_rcu_locks)
		return false;

	if (!check_ids(old->active_irq_id, cur->active_irq_id, idmap))
		return false;

	if (!check_ids(old->active_lock_id, cur->active_lock_id, idmap) ||
	    old->active_lock_ptr != cur->active_lock_ptr)
		return false;

	for (i = 0; i < old->acquired_refs; i++) {
		/* 引用数组按获取次序配对；先核对身份/类型，再检查类型专属字段。 */
		if (!check_ids(old->refs[i].id, cur->refs[i].id, idmap) ||
		    old->refs[i].type != cur->refs[i].type)
			return false;
		switch (old->refs[i].type) {
		case REF_TYPE_PTR:
			/* 普通对象引用还需保持其父引用关系。 */
			if (!check_ids(old->refs[i].parent_id, cur->refs[i].parent_id, idmap))
				return false;
			break;
		case REF_TYPE_IRQ:
			/* IRQ 引用的身份已由公共 ID 比较覆盖，无额外指针字段。 */
			break;
		case REF_TYPE_LOCK:
		case REF_TYPE_RES_LOCK:
		case REF_TYPE_RES_LOCK_IRQ:
			/* 锁类引用必须仍指向同一被保护资源，ID 重命名不能替代指针一致。 */
			if (old->refs[i].ptr != cur->refs[i].ptr)
				return false;
			break;
		default:
			WARN_ONCE(1, "Unhandled enum type for reference state: %d\n", old->refs[i].type);
			return false;
		}
	}

	return true;
}

/* compare two verifier states
 *
 * all states stored in state_list are known to be valid, since
 * verifier reached 'bpf_exit' instruction through them
 *
 * this function is called when verifier exploring different branches of
 * execution popped from the state stack. If it sees an old state that has
 * more strict register state and more strict stack state then this execution
 * branch doesn't need to be explored further, since verifier already
 * concluded that more strict state leads to valid finish.
 *
 * Therefore two states are equivalent if register state is more conservative
 * and explored stack state is more conservative than the current one.
 * Example:
 *       explored                   current
 * (slot1=INV slot2=MISC) == (slot1=MISC slot2=MISC)
 * (slot1=MISC slot2=MISC) != (slot1=INV slot2=MISC)
 *
 * In other words if current stack state (one being explored) has more
 * valid slots than old one that already passed validation, it means
 * the verifier can stop exploring and conclude that current state is valid too
 *
 * Similarly with registers. If explored state has register type as invalid
 * whereas register type in current state is meaningful, it means that
 * the current state will reach 'bpf_exit' instruction safely
 */
/*
 * 中文：旧状态已走到安全出口；若旧寄存器/栈比当前更保守（如旧 INVALID、当前
 * MISC），当前的额外已知信息不会制造旧路径未覆盖的危险，因此可判等并剪枝；
 * 反方向不成立。
 * 业务背景：聚合单个调用帧的寄存器、普通栈和出栈参数安全包含关系。
 * 入参：env 为借用环境；old/cur 为帧快照；insn_idx 选择该点 live_regs；exact
 * 指定比较强度。
 * 出参/返回：callback/no_stack_arg_load 约束及全部活实体兼容返回 true；否则 false。
 * 注意事项：只比较 live_regs 中寄存器，ID 映射复用 env scratch；不睡眠。
 */
static bool func_states_equal(struct bpf_verifier_env *env, struct bpf_func_state *old,
			      struct bpf_func_state *cur, u32 insn_idx, enum exact_level exact)
{
	u16 live_regs = env->insn_aux_data[insn_idx].live_regs_before;
	u16 i;

	if (old->callback_depth > cur->callback_depth)
		return false;

	if (!old->no_stack_arg_load && cur->no_stack_arg_load)
		return false;

	/* 先比较该指令以后仍活的寄存器；死寄存器已清洗，无需阻止剪枝。 */
	for (i = 0; i < MAX_BPF_REG; i++)
		if (((1 << i) & live_regs) &&
		    !regsafe(env, &old->regs[i], &cur->regs[i],
			     &env->idmap_scratch, exact))
			return false;

	/* 再比较帧内普通栈与路径局部出栈参数，三类实体共享同一 ID 映射。 */
	if (!stacksafe(env, old, cur, &env->idmap_scratch, exact))
		return false;

	if (!stack_arg_safe(env, old, cur, &env->idmap_scratch, exact))
		return false;

	return true;
}

/*
 * 业务背景：每次整状态比较都必须从空 ID 重命名关系开始，避免上次结果污染。
 * 入参：env 为输入输出环境，id_gen 是已分配真实 ID 的上界。
 * 出参/返回：无直接返回值；清零映射计数，并从 env->id_gen 初始化临时 ID。
 * 注意事项：不会回退全局 id_gen；scratch 由验证线程独占，不睡眠。
 */
static void reset_idmap_scratch(struct bpf_verifier_env *env)
{
	struct bpf_idmap *idmap = &env->idmap_scratch;

	idmap->tmp_id_gen = env->id_gen;
	idmap->cnt = 0;
}

/*
 * 业务背景：在检查点判断当前完整调用栈是否已被某个旧安全状态覆盖，是最终剪枝
 * 判定入口。
 * 入参：env 为借用环境；old/cur 为旧/当前状态；exact 为寄存器和栈比较强度。
 * 出参/返回：帧数、推测性、睡眠上下文、资源及每帧状态都兼容返回 true；否则
 * false。会重置并使用 idmap_scratch。
 * 注意事项：推测状态不能剪枝真实路径；每帧 callsite 必须相同，函数不睡眠。
 */
static bool states_equal(struct bpf_verifier_env *env,
			 struct bpf_verifier_state *old,
			 struct bpf_verifier_state *cur,
			 enum exact_level exact)
{
	u32 insn_idx;
	int i;

	/* 先核对调用栈深度；不同帧数没有可逐帧建立的一一对应关系。 */
	if (old->curframe != cur->curframe)
		return false;

	reset_idmap_scratch(env);

	/* Verification state from speculative execution simulation
	 * must never prune a non-speculative execution one.
	 */
	/* 中文：推测执行模拟所得安全结论不能用来截断真实执行路径。 */
	if (old->speculative && !cur->speculative)
		return false;

	/* 睡眠上下文与持有资源是控制流约束，必须早于逐帧数值比较。 */
	if (old->in_sleepable != cur->in_sleepable)
		return false;

	if (!refsafe(old, cur, &env->idmap_scratch))
		return false;
	/* 资源关系通过后，剩余工作才是调用位置和各帧值域的结构比较。 */

	/* for states to be equal callsites have to be the same
	 * and all frame states need to be equivalent
	 */
	/* 中文：调用链位置先逐帧相同，再以该帧指令点的活性信息比较局部状态。 */
	for (i = 0; i <= old->curframe; i++) {
		insn_idx = bpf_frame_insn_idx(old, i);
		if (old->frame[i]->callsite != cur->frame[i]->callsite)
			return false;
		if (!func_states_equal(env, old->frame[i], cur->frame[i], insn_idx, exact))
			return false;
	}
	return true;
}

/* find precise scalars in the previous equivalent state and
 * propagate them into the current state
 */
/*
 * 中文：收集旧等价状态中全部 precise 标量寄存器和 spill，在 bt 中标记位置，
 * 再沿当前状态 parent 链反向传播精度需求。
 * 业务背景：剪枝不能丢掉 JIT 与安全检查所需的精确标量来源。
 * 入参：env 为输入输出回溯环境；old 为只读状态；cur 为传播起点；changed 可空。
 * 出参/返回：成功返回 0；精度回溯失败返回负 errno。
 * 注意事项：bt 是 env 临时工作区，不取得状态 ownership；日志缓冲可改变。
 */
static int propagate_precision(struct bpf_verifier_env *env,
			       const struct bpf_verifier_state *old,
			       struct bpf_verifier_state *cur,
			       bool *changed)
{
	struct bpf_reg_state *state_reg;
	struct bpf_func_state *state;
	int i, err = 0, fr;
	bool first;

	for (fr = old->curframe; fr >= 0; fr--) {
		/* 阶段 1：逐帧把旧状态的 precise 位置编码进 bt 工作集。 */
		state = old->frame[fr];
		state_reg = state->regs;
		first = true;
		/* 活寄存器中仅 precise 标量需要加入反向工作集，其余跳过。 */
		for (i = 0; i < BPF_REG_FP; i++, state_reg++) {
			if (state_reg->type != SCALAR_VALUE ||
			    !state_reg->precise)
				continue;
			if (env->log.level & BPF_LOG_LEVEL2) {
				/* 首个寄存器打印帧前缀，后续只追加编号。 */
				if (first)
					verbose(env, "frame %d: propagating r%d", fr, i);
				else
					verbose(env, ",r%d", i);
			}
			bpf_bt_set_frame_reg(&env->bt, fr, i);
			first = false;
		}

		/* 随后扫描完整 spill 槽，采用与寄存器相同的 precise 标量条件。 */
		for (i = 0; i < state->allocated_stack / BPF_REG_SIZE; i++) {
			if (!bpf_is_spilled_reg(&state->stack[i]))
				continue;
			state_reg = &state->stack[i].spilled_ptr;
			if (state_reg->type != SCALAR_VALUE ||
			    !state_reg->precise)
				continue;
			/* 精确 spill 也写入同一 bt，位置以帧号和 SPI 唯一标识。 */
			if (env->log.level & BPF_LOG_LEVEL2) {
				/* first 同时控制日志前缀，避免一个帧重复打印标题。 */
				if (first)
					verbose(env, "frame %d: propagating fp%d",
						fr, (-i - 1) * BPF_REG_SIZE);
				else
					verbose(env, ",fp%d", (-i - 1) * BPF_REG_SIZE);
			}
			/* 日志只描述工作集；真正改变回溯请求的是这一位设置。 */
			bpf_bt_set_frame_slot(&env->bt, fr, i);
			first = false;
		}
		if (!first && (env->log.level & BPF_LOG_LEVEL2))
			verbose(env, "\n");
	}

	/* 阶段 2：沿 cur 历史反向追踪工作集中的定义点。 */
	err = bpf_mark_chain_precision(env, cur, -1, changed);
	if (err < 0)
		return err;

	return 0;
}

#define MAX_BACKEDGE_ITERS 64

/* Propagate read and precision marks from visit->backedges[*].state->equal_state
 * to corresponding parent states of visit->backedges[*].state until fixed point is reached,
 * then free visit->backedges.
 * After execution of this function incomplete_read_marks() will return false
 * for all states corresponding to @visit->callchain.
 */
/*
 * 中文：反复把各回边 equal_state 的读/精度需求传播到回边副本 parent 链，直到
 * 没有新增标记，再释放全部回边；完成后该 visit 的标记已完整。
 * 业务背景：循环各路径互相依赖，单轮逆向传播不足以覆盖整个环。
 * 入参：env 为输入输出传播环境；visit 拥有 backedges 链表。
 * 出参/返回：收敛返回 0 并释放链表；传播失败返回 errno 且保留链表；超过 64 轮
 * 则把相关状态所有标量置 precise 后安全退化。
 * 注意事项：释放后旧 backedge 指针失效；退化只损失优化，不放宽验证。
 */
static int propagate_backedges(struct bpf_verifier_env *env, struct bpf_scc_visit *visit)
{
	struct bpf_scc_backedge *backedge;
	struct bpf_verifier_state *st;
	bool changed;
	int i, err;

	i = 0;
	do {
		/* 固定点迭代设安全上限，防验证器自身在复杂回边关系中无限工作。 */
		if (i++ > MAX_BACKEDGE_ITERS) {
			if (env->log.level & BPF_LOG_LEVEL2)
				verbose(env, "%s: too many iterations\n", __func__);
			for (backedge = visit->backedges; backedge; backedge = backedge->next)
				bpf_mark_all_scalars_precise(env, &backedge->state);
			break;
		}
		changed = false;
		/* 一轮遍历所有回边，任一传播新增标记都会触发下一轮。 */
		for (backedge = visit->backedges; backedge; backedge = backedge->next) {
			st = &backedge->state;
			err = propagate_precision(env, st->equal_state, st, &changed);
			if (err)
				return err;
		}
	} while (changed);

	/* 无新增标记意味着固定点；此时回边副本及其 parent 依赖可一起释放。 */
	bpf_free_backedges(visit);
	return 0;
}

/*
 * 业务背景：昂贵精确比较前，以当前帧寄存器核心字段筛出可能重复的循环状态。
 * 入参：old/cur 为借用状态快照。
 * 出参/返回：帧深相同且当前帧全部寄存器在 frameno 前字段相同返回 true，否则
 * false；无副作用。
 * 注意事项：仅是候选判定，不能单独证明无限循环；不比较栈和后置字段。
 */
static bool states_maybe_looping(struct bpf_verifier_state *old,
				 struct bpf_verifier_state *cur)
{
	struct bpf_func_state *fold, *fcur;
	int i, fr = cur->curframe;

	if (old->curframe != fr)
		return false;

	fold = old->frame[fr];
	fcur = cur->frame[fr];
	/* offsetof(frameno) 前是此启发式关心的类型、范围、ID 等核心字段。 */
	for (i = 0; i < MAX_BPF_REG; i++)
		if (memcmp(&fold->regs[i], &fcur->regs[i],
			   offsetof(struct bpf_reg_state, frameno)))
			return false;
	return true;
}

/* is_state_visited() handles iter_next() (see process_iter_next_call() for
 * terminology) calls specially: as opposed to bounded BPF loops, it *expects*
 * states to match, which otherwise would look like an infinite loop. So while
 * iter_next() calls are taken care of, we still need to be careful and
 * prevent erroneous and too eager declaration of "infinite loop", when
 * iterators are involved.
 *
 * Here's a situation in pseudo-BPF assembly form:
 *
 *   0: again:                          ; set up iter_next() call args
 *   1:   r1 = &it                      ; <CHECKPOINT HERE>
 *   2:   call bpf_iter_num_next        ; this is iter_next() call
 *   3:   if r0 == 0 goto done
 *   4:   ... something useful here ...
 *   5:   goto again                    ; another iteration
 *   6: done:
 *   7:   r1 = &it
 *   8:   call bpf_iter_num_destroy     ; clean up iter state
 *   9:   exit
 *
 * This is a typical loop. Let's assume that we have a prune point at 1:,
 * before we get to `call bpf_iter_num_next` (e.g., because of that `goto
 * again`, assuming other heuristics don't get in a way).
 *
 * When we first time come to 1:, let's say we have some state X. We proceed
 * to 2:, fork states, enqueue ACTIVE, validate NULL case successfully, exit.
 * Now we come back to validate that forked ACTIVE state. We proceed through
 * 3-5, come to goto, jump to 1:. Let's assume our state didn't change, so we
 * are converging. But the problem is that we don't know that yet, as this
 * convergence has to happen at iter_next() call site only. So if nothing is
 * done, at 1: verifier will use bounded loop logic and declare infinite
 * looping (and would be *technically* correct, if not for iterator's
 * "eventual sticky NULL" contract, see process_iter_next_call()). But we
 * don't want that. So what we do in process_iter_next_call() when we go on
 * another ACTIVE iteration, we bump slot->iter.depth, to mark that it's
 * a different iteration. So when we suspect an infinite loop, we additionally
 * check if any of the *ACTIVE* iterator states depths differ. If yes, we
 * pretend we are not looping and wait for next iter_next() call.
 *
 * This only applies to ACTIVE state. In DRAINED state we don't expect to
 * loop, because that would actually mean infinite loop, as DRAINED state is
 * "sticky", and so we'll keep returning into the same instruction with the
 * same state (at least in one of possible code paths).
 *
 * This approach allows to keep infinite loop heuristic even in the face of
 * active iterator. E.g., C snippet below is and will be detected as
 * infinitely looping:
 *
 *   struct bpf_iter_num it;
 *   int *p, x;
 *
 *   bpf_iter_num_new(&it, 0, 10);
 *   while ((p = bpf_iter_num_next(&t))) {
 *       x = p;
 *       while (x--) {} // <<-- infinite loop here
 *   }
 *
 */
/*
 * 中文：普通循环重复精确状态意味着不前进，但 iter_next() 约定最终返回黏性
 * NULL；ACTIVE 迭代每轮增加 depth，使检查点前的相同业务状态暂不被误报。
 * 只有 ACTIVE depth 不同才放行；DRAINED 再重复就是真无限循环。内层无关死循环
 * 因 depth 不变仍可检出。
 * 业务背景：为普通无限循环启发式提供开放编码迭代器专属反例检查。
 * 入参：old/cur 为布局已由精确比较确认兼容的借用状态。
 * 出参/返回：任一 ACTIVE iterator 槽的 depth 不同返回 true，否则 false。
 * 注意事项：只读状态、不持有资源、不睡眠。
 */
static bool iter_active_depths_differ(struct bpf_verifier_state *old, struct bpf_verifier_state *cur)
{
	struct bpf_reg_state *slot, *cur_slot;
	struct bpf_func_state *state;
	int i, fr;

	for (fr = old->curframe; fr >= 0; fr--) {
		state = old->frame[fr];
		/* 只查看 iterator 首槽；非 ACTIVE 槽的 depth 不具有放行意义。 */
		for (i = 0; i < state->allocated_stack / BPF_REG_SIZE; i++) {
			if (state->stack[i].slot_type[0] != STACK_ITER)
				continue;

			slot = &state->stack[i].spilled_ptr;
			if (slot->iter.state != BPF_ITER_STATE_ACTIVE)
				continue;

			/* old/cur 槽布局已相等，可直接比较同一 SPI 的迭代深度。 */
			cur_slot = &cur->frame[fr]->stack[i].spilled_ptr;
			if (cur_slot->iter.depth != slot->iter.depth)
				return true;
		}
	}
	return false;
}

/*
 * 业务背景：保存新检查点时清掉继承的 precise 标记，之后只按真实消费者恢复，
 * 减少不必要的精度传播。
 * 入参：env 为借用上下文；st 为输入输出完整状态。
 * 出参/返回：无直接返回值；全部帧寄存器和 spill 标量的 precise 置 false。
 * 注意事项：env 本身不修改；非标量、范围和 ID 不变，函数不睡眠。
 */
static void mark_all_scalars_imprecise(struct bpf_verifier_env *env, struct bpf_verifier_state *st)
{
	struct bpf_func_state *func;
	struct bpf_reg_state *reg;
	int i, j;

	for (i = 0; i <= st->curframe; i++) {
		func = st->frame[i];
		/* 每帧先处理 R0..R9，再处理其已分配的完整 spill。 */
		for (j = 0; j < BPF_REG_FP; j++) {
			reg = &func->regs[j];
			if (reg->type != SCALAR_VALUE)
				continue;
			reg->precise = false;
		}
		/* 栈中仅完整 spill 保存寄存器状态，再筛选其中的标量。 */
		for (j = 0; j < func->allocated_stack / BPF_REG_SIZE; j++) {
			if (!bpf_is_spilled_reg(&func->stack[j]))
				continue;
			reg = &func->stack[j].spilled_ptr;
			if (reg->type != SCALAR_VALUE)
				continue;
			reg->precise = false;
		}
	}
}

/*
 * 业务背景：每到剪枝点，把当前状态与同指令的历史状态比较；命中安全覆盖便停止
 * 当前路径，未命中时按启发式保存新检查点，同时识别真正无限循环与迭代器收敛。
 * 这是 do_check() 状态爆炸控制的核心入口。
 * 入参：env 为本次验证的输入输出环境并独占所有状态链；insn_idx 是当前合法指令
 * 下标，与 env->cur_state 对应。
 * 出参/返回：返回 1 表示命中旧状态、当前路径可剪枝；返回 0 表示继续验证（可能
 * 已保存新状态）；-ENOMEM/-EINVAL/-EFAULT 等表示分配、无限循环或内部错误。
 * 注意事项：成功保存后 new_sl 由 explored_states 拥有，cur->parent 借用其 state；
 * SCC 命中时 backedge ownership 转给 visit。多处统计/标记为非事务性，错误使
 * 整体验证失败并由上层 free_states() 清理。GFP_KERNEL_ACCOUNT 分配可睡眠。
 */
int bpf_is_state_visited(struct bpf_verifier_env *env, int insn_idx)
{
	struct bpf_verifier_state_list *new_sl;
	struct bpf_verifier_state_list *sl;
	struct bpf_verifier_state *cur = env->cur_state, *new;
	bool force_new_state, add_new_state, loop;
	int n, err, states_cnt = 0;
	struct list_head *pos, *tmp, *head;

	/* 阶段 1：决定是否强制保存；跳转历史过长时用检查点截断 parent 追踪长度。 */
	force_new_state = env->test_state_freq || bpf_is_force_checkpoint(env, insn_idx) ||
			  /* Avoid accumulating infinitely long jmp history */
			  /* 中文：跳转历史达到上限时强制建立检查点以截断无界增长。 */
			  cur->jmp_history_cnt > 40;

	/* bpf progs typically have pruning point every 4 instructions
	 * http://vger.kernel.org/bpfconf2019.html#session-1
	 * Do not add new state for future pruning if the verifier hasn't seen
	 * at least 2 jumps and at least 8 instructions.
	 * This heuristics helps decrease 'total_states' and 'peak_states' metric.
	 * In tests that amounts to up to 50% reduction into total verifier
	 * memory consumption and 20% verifier time speedup.
	 */
	/*
	 * 中文：BPF 程序通常约每四条指令有剪枝点；至少新增 2 次跳转且处理 8 条
	 * 指令才保存，可在测试中最多降低约一半状态数和约两成验证时间。
	 */
	add_new_state = force_new_state;
	if (env->jmps_processed - env->prev_jmps_processed >= 2 &&
	    env->insn_processed - env->prev_insn_processed >= 8)
		add_new_state = true;

	/* keep cleaning the current state as registers/stack become dead */
	/* 中文：阶段 2：先清死值再比较，避免不再使用的寄存器/栈差异阻止命中。 */
	err = clean_verifier_state(env, cur);
	if (err)
		return err;

	/* 阶段 3：扫描同一指令的缓存；safe 迭代允许命中过程中移动/淘汰节点。 */
	loop = false;
	head = bpf_explored_state(env, insn_idx);
	list_for_each_safe(pos, tmp, head) {
		sl = container_of(pos, struct bpf_verifier_state_list, node);
		states_cnt++;
		if (sl->state.insn_idx != insn_idx)
			continue;

		if (sl->state.branches) {
			/* 未完成旧状态只能用于循环诊断，尚不能作为已证安全状态剪枝。 */
			struct bpf_func_state *frame = sl->state.frame[sl->state.curframe];

			if (frame->in_async_callback_fn &&
			    frame->async_entry_cnt != cur->frame[cur->curframe]->async_entry_cnt) {
				/* Different async_entry_cnt means that the verifier is
				 * processing another entry into async callback.
				 * Seeing the same state is not an indication of infinite
				 * loop or infinite recursion.
				 * But finding the same state doesn't mean that it's safe
				 * to stop processing the current state. The previous state
				 * hasn't yet reached bpf_exit, since state.branches > 0.
				 * Checking in_async_callback_fn alone is not enough either.
				 * Since the verifier still needs to catch infinite loops
				 * inside async callbacks.
				 */
				/*
				 * 中文：async_entry_cnt 不同表示另一次回调进入，相同状态既不证明
				 * 递归/循环，也不能剪枝尚未到 EXIT 的旧分支；但同一次进入仍需检测死循环。
				 */
				goto skip_inf_loop_check;
			}
			/* BPF open-coded iterators loop detection is special.
			 * states_maybe_looping() logic is too simplistic in detecting
			 * states that *might* be equivalent, because it doesn't know
			 * about ID remapping, so don't even perform it.
			 * See process_iter_next_call() and iter_active_depths_differ()
			 * for overview of the logic. When current and one of parent
			 * states are detected as equivalent, it's a good thing: we prove
			 * convergence and can stop simulating further iterations.
			 * It's safe to assume that iterator loop will finish, taking into
			 * account iter_next() contract of eventually returning
			 * sticky NULL result.
			 *
			 * Note, that states have to be compared exactly in this case because
			 * read and precision marks might not be finalized inside the loop.
			 * E.g. as in the program below:
			 *
			 *     1. r7 = -16
			 *     2. r6 = bpf_get_prandom_u32()
			 *     3. while (bpf_iter_num_next(&fp[-8])) {
			 *     4.   if (r6 != 42) {
			 *     5.     r7 = -32
			 *     6.     r6 = bpf_get_prandom_u32()
			 *     7.     continue
			 *     8.   }
			 *     9.   r0 = r10
			 *    10.   r0 += r7
			 *    11.   r8 = *(u64 *)(r0 + 0)
			 *    12.   r6 = bpf_get_prandom_u32()
			 *    13. }
			 *
			 * Here verifier would first visit path 1-3, create a checkpoint at 3
			 * with r7=-16, continue to 4-7,3. Existing checkpoint at 3 does
			 * not have read or precision mark for r7 yet, thus inexact states
			 * comparison would discard current state with r7=-32
			 * => unsafe memory access at 11 would not be caught.
			 */
			/*
			 * 中文：iter_next 期待检查点最终匹配，且其黏性 NULL 保证迭代结束；因此
			 * 跳过粗略循环候选判断，直接做 RANGE_WITHIN。循环内 read/precise 尚未
			 * 完整，若用宽松比较，示例会让 r7=-32 路径被 r7=-16 检查点错误丢弃，
			 * 从而漏掉 fp-32 的越界读取。
			 */
			if (is_iter_next_insn(env, insn_idx)) {
				if (states_equal(env, &sl->state, cur, RANGE_WITHIN)) {
					struct bpf_func_state *cur_frame;
					struct bpf_reg_state *iter_state, *iter_reg;
					int spi;

					cur_frame = cur->frame[cur->curframe];
					/* btf_check_iter_kfuncs() enforces that
					 * iter state pointer is always the first arg
					 */
					/* 中文：BTF 检查保证 R1 指向迭代器首槽，可直接由常量偏移求 SPI。 */
					iter_reg = &cur_frame->regs[BPF_REG_1];
					/* current state is valid due to states_equal(),
					 * so we can assume valid iter and reg state,
					 * no need for extra (re-)validations
					 */
					/* 中文：states_equal 已证明槽有效，此处无需重复类型/边界检查。 */
					spi = bpf_get_spi(iter_reg->var_off.value);
					iter_state = &bpf_func(env, iter_reg)->stack[spi].spilled_ptr;
					if (iter_state->iter.state == BPF_ITER_STATE_ACTIVE) {
						loop = true;
						goto hit;
					}
				}
				goto skip_inf_loop_check;
			}
			if (is_may_goto_insn_at(env, insn_idx)) {
				/* may_goto 深度变化且状态覆盖表示一次受预算控制的合法回边命中。 */
				if (sl->state.may_goto_depth != cur->may_goto_depth &&
				    states_equal(env, &sl->state, cur, RANGE_WITHIN)) {
					loop = true;
					goto hit;
				}
			}
			if (bpf_calls_callback(env, insn_idx)) {
				/* 回调调用隐含循环；状态覆盖证明收敛，未覆盖则继续展开回调。 */
				if (states_equal(env, &sl->state, cur, RANGE_WITHIN)) {
					loop = true;
					goto hit;
				}
				goto skip_inf_loop_check;
			}
			/* attempt to detect infinite loop to avoid unnecessary doomed work */
			/* 中文：普通路径只有精确重复且迭代 depth/预算/回调展开均未前进才判死循环。 */
			if (states_maybe_looping(&sl->state, cur) &&
			    states_equal(env, &sl->state, cur, EXACT) &&
			    !iter_active_depths_differ(&sl->state, cur) &&
			    sl->state.may_goto_depth == cur->may_goto_depth &&
			    sl->state.callback_unroll_depth == cur->callback_unroll_depth) {
				/* 输出当前/旧状态差异证据后拒绝，不再保存注定无进展的检查点。 */
				verbose_linfo(env, insn_idx, "; ");
				verbose(env, "infinite loop detected at insn %d\n", insn_idx);
				verbose(env, "cur state:");
				print_verifier_state(env, cur, cur->curframe, true);
				verbose(env, "old state:");
				print_verifier_state(env, &sl->state, cur->curframe, true);
				return -EINVAL;
			}
			/* if the verifier is processing a loop, avoid adding new state
			 * too often, since different loop iterations have distinct
			 * states and may not help future pruning.
			 * This threshold shouldn't be too low to make sure that
			 * a loop with large bound will be rejected quickly.
			 * The most abusive loop will be:
			 * r1 += 1
			 * if r1 < 1000000 goto pc-2
			 * 1M insn_procssed limit / 100 == 10k peak states.
			 * This threshold shouldn't be too high either, since states
			 * at the end of the loop are likely to be useful in pruning.
			 */
			/*
			 * 中文：循环迭代状态差异大，过密保存浪费内存；阈值过低又会延迟拒绝百万
			 * 次循环，过高则丢掉靠近循环出口的有用检查点，故采用 20 跳转/100 指令。
			 */
skip_inf_loop_check:
			if (!force_new_state &&
			    env->jmps_processed - env->prev_jmps_processed < 20 &&
			    env->insn_processed - env->prev_insn_processed < 100)
				add_new_state = false;
			goto miss;
		}
		/* See comments for mark_all_regs_read_and_precise() */
		/* 中文：已完成旧状态可比较；若 SCC 标记未完整则强制保留范围信息。 */
		loop = incomplete_read_marks(env, &sl->state);
		if (states_equal(env, &sl->state, cur, loop ? RANGE_WITHIN : NOT_EXACT)) {
hit:
			sl->hit_cnt++;

			/* if previous state reached the exit with precision and
			 * current state is equivalent to it (except precision marks)
			 * the precision needs to be propagated back in
			 * the current state.
			 */
			/* 中文：命中后把旧状态 precise 需求沿当前路径回灌，避免剪枝丢精度。 */
			err = 0;
			if (bpf_is_jmp_point(env, env->insn_idx))
				err = bpf_push_jmp_history(env, cur, 0, 0, 0, 0);
			err = err ? : propagate_precision(env, &sl->state, cur, NULL);
			if (err)
				return err;
			/* When processing iterator based loops above propagate_liveness and
			 * propagate_precision calls are not sufficient to transfer all relevant
			 * read and precision marks. E.g. consider the following case:
			 *
			 *  .-> A --.  Assume the states are visited in the order A, B, C.
			 *  |   |   |  Assume that state B reaches a state equivalent to state A.
			 *  |   v   v  At this point, state C is not processed yet, so state A
			 *  '-- B   C  has not received any read or precision marks from C.
			 *             Thus, marks propagated from A to B are incomplete.
			 *
			 * The verifier mitigates this by performing the following steps:
			 *
			 * - Prior to the main verification pass, strongly connected components
			 *   (SCCs) are computed over the program's control flow graph,
			 *   intraprocedurally.
			 *
			 * - During the main verification pass, `maybe_enter_scc()` checks
			 *   whether the current verifier state is entering an SCC. If so, an
			 *   instance of a `bpf_scc_visit` object is created, and the state
			 *   entering the SCC is recorded as the entry state.
			 *
			 * - This instance is associated not with the SCC itself, but with a
			 *   `bpf_scc_callchain`: a tuple consisting of the call sites leading to
			 *   the SCC and the SCC id. See `compute_scc_callchain()`.
			 *
			 * - When a verification path encounters a `states_equal(...,
			 *   RANGE_WITHIN)` condition, there exists a call chain describing the
			 *   current state and a corresponding `bpf_scc_visit` instance. A copy
			 *   of the current state is created and added to
			 *   `bpf_scc_visit->backedges`.
			 *
			 * - When a verification path terminates, `maybe_exit_scc()` is called
			 *   from `bpf_update_branch_counts()`. For states with `branches == 0`, it
			 *   checks whether the state is the entry state of any `bpf_scc_visit`
			 *   instance. If it is, this indicates that all paths originating from
			 *   this SCC visit have been explored. `propagate_backedges()` is then
			 *   called, which propagates read and precision marks through the
			 *   backedges until a fixed point is reached.
			 *   (In the earlier example, this would propagate marks from A to B,
			 *    from C to A, and then again from A to B.)
			 *
			 * A note on callchains
			 * --------------------
			 *
			 * Consider the following example:
			 *
			 *     void foo() { loop { ... SCC#1 ... } }
			 *     void main() {
			 *       A: foo();
			 *       B: ...
			 *       C: foo();
			 *     }
			 *
			 * Here, there are two distinct callchains leading to SCC#1:
			 * - (A, SCC#1)
			 * - (C, SCC#1)
			 *
			 * Each callchain identifies a separate `bpf_scc_visit` instance that
			 * accumulates backedge states. The `propagate_{liveness,precision}()`
			 * functions traverse the parent state of each backedge state, which
			 * means these parent states must remain valid (i.e., not freed) while
			 * the corresponding `bpf_scc_visit` instance exists.
			 *
			 * Associating `bpf_scc_visit` instances directly with SCCs instead of
			 * callchains would break this invariant:
			 * - States explored during `C: foo()` would contribute backedges to
			 *   SCC#1, but SCC#1 would only be exited once the exploration of
			 *   `A: foo()` completes.
			 * - By that time, the states explored between `A: foo()` and `C: foo()`
			 *   (i.e., `B: ...`) may have already been freed, causing the parent
			 *   links for states from `C: foo()` to become invalid.
			 */
			/*
			 * 中文：A/B/C 示例说明命中时其他分支尚未处理，A 的标记仍不完整。
			 * 验证器先求 SCC；进入时以“调用点序列 + SCC id”创建 visit；命中时
			 * 复制当前状态为回边；入口所有 branches 归零后迭代传播到固定点。
			 * visit 必须按 callchain 区分：foo 从 A/C 调用时 parent 链寿命不同；若
			 * 只按 SCC 合并，C 回边可能延迟到 A 退出，此时中间 B 已释放而形成悬指针。
			 */
			if (loop) {
				struct bpf_scc_backedge *backedge;

				/* 阶段 4：复制当前状态；登记成功才把 backedge ownership 转给 visit。 */
				backedge = kzalloc_obj(*backedge,
						       GFP_KERNEL_ACCOUNT);
				if (!backedge)
					return -ENOMEM;
				err = bpf_copy_verifier_state(&backedge->state, cur);
				backedge->state.equal_state = &sl->state;
				backedge->state.insn_idx = insn_idx;
				err = err ?: add_scc_backedge(env, &sl->state, backedge);
				if (err) {
					/* 未成功发布时由当前分支释放深拷贝内容及外层容器。 */
					bpf_free_verifier_state(&backedge->state, false);
					kfree(backedge);
					return err;
				}
			}
			return 1;
		}
miss:
		/* when new state is not going to be added do not increase miss count.
		 * Otherwise several loop iterations will remove the state
		 * recorded earlier. The goal of these heuristics is to have
		 * states from some iterations of the loop (some in the beginning
		 * and some at the end) to help pruning.
		 */
		/* 中文：未计划保存新状态时不记 miss，避免连续循环轮次淘汰早期检查点。 */
		if (add_new_state)
			sl->miss_cnt++;
		/* heuristic to determine whether this state is beneficial
		 * to keep checking from state equivalence point of view.
		 * Higher numbers increase max_states_per_insn and verification time,
		 * but do not meaningfully decrease insn_processed.
		 * 'n' controls how many times state could miss before eviction.
		 * Use bigger 'n' for checkpoints because evicting checkpoint states
		 * too early would hinder iterator convergence.
		 */
		/* 中文：普通状态容忍 3 倍 miss，未完成强制检查点容忍 64 倍以利收敛。 */
		n = bpf_is_force_checkpoint(env, insn_idx) && sl->state.branches > 0 ? 64 : 3;
		if (sl->miss_cnt > sl->hit_cnt * n + n) {
			/* the state is unlikely to be useful. Remove it to
			 * speed up verification
			 */
			/* 中文：先从 explored 摘除并移入 free_list；有 parent/回边引用则延迟释放。 */
			sl->in_free_list = true;
			list_del(&sl->node);
			list_add(&sl->node, &env->free_list);
			env->free_list_size++;
			env->explored_states_size--;
			maybe_free_verifier_state(env, sl);
		}
	}

	/* 阶段 4 尾声：更新观测峰值；非特权复杂度或保存节流命中时继续当前路径。 */
	if (env->max_states_per_insn < states_cnt)
		env->max_states_per_insn = states_cnt;

	if (!env->bpf_capable && states_cnt > BPF_COMPLEXITY_LIMIT_STATES)
		return 0;

	if (!add_new_state)
		return 0;

	/* There were no equivalent states, remember the current one.
	 * Technically the current state is not proven to be safe yet,
	 * but it will either reach outer most bpf_exit (which means it's safe)
	 * or it will be rejected. When there are no loops the verifier won't be
	 * seeing this tuple (frame[0].callsite, frame[1].callsite, .. insn_idx)
	 * again on the way to bpf_exit.
	 * When looping the sl->state.branches will be > 0 and this state
	 * will not be considered for equivalence until branches == 0.
	 */
	/*
	 * 中文：阶段 5：没有等价状态时保存当前快照。它尚未证明安全；无循环时最终
	 * 只会走到顶层 EXIT 或被拒绝，有循环时 branches>0 会阻止它提前参与剪枝，
	 * 直到所有派生路径完成。
	 */
	new_sl = kzalloc_obj(struct bpf_verifier_state_list, GFP_KERNEL_ACCOUNT);
	if (!new_sl)
		return -ENOMEM;
	/* 外层容器分配成功但尚未发布；先更新统计和下一检查点的节流基线。 */
	env->total_states++;
	env->explored_states_size++;
	update_peak_states(env);
	env->prev_jmps_processed = env->jmps_processed;
	env->prev_insn_processed = env->insn_processed;

	/* forget precise markings we inherited, see __mark_chain_precision */
	/* 中文：特权验证按需重建精度；随后清除只在单一路径成立的 singular ID。 */
	if (env->bpf_capable)
		mark_all_scalars_imprecise(env, cur);

	bpf_clear_singular_ids(env, cur);

	/* add new state to the head of linked list */
	/*
	 * 中文：阶段 6：先深拷贝并登记 SCC 入口，任一步失败都释放未发布 new_sl；
	 * 成功后再连接 cur->parent、重置跳转历史，并以 list_add 作为缓存发布点。
	 */
	new = &new_sl->state;
	err = bpf_copy_verifier_state(new, cur);
	if (err) {
		/* 深拷贝可能部分构造 new，先释放其内部字段再释放容器。 */
		bpf_free_verifier_state(new, false);
		kfree(new_sl);
		return err;
	}
	new->insn_idx = insn_idx;
	verifier_bug_if(new->branches != 1, env,
			"%s:branches_to_explore=%d insn %d",
			__func__, new->branches, insn_idx);
	err = maybe_enter_scc(env, new);
	if (err) {
		/* SCC visit 未能建立时 new 仍未入链，保持由当前错误分支回滚。 */
		bpf_free_verifier_state(new, false);
		kfree(new_sl);
		return err;
	}

	/* new_sl 即将由 head 链表拥有；cur 的 parent 裸指针寿命由 branches 协议保证。 */
	cur->parent = new;
	cur->first_insn_idx = insn_idx;
	cur->dfs_depth = new->dfs_depth + 1;
	bpf_clear_jmp_history(cur);
	list_add(&new_sl->node, head);
	return 0;
}
