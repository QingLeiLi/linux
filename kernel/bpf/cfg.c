// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/filter.h>
#include <linux/sort.h>

#define verbose(env, fmt, args...) bpf_verifier_log_write(env, fmt, ##args)

/*
 * 学习导览：本文件把已经通过基本指令解码的 BPF 程序转换成可供验证器使用的
 * 控制流信息。第一遍 bpf_check_cfg() 从主入口（必要时再从异常回调入口）做
 * 非递归深度优先搜索，拒绝越界边、不可达指令、跳进 ldimm64 后半条等非法图，
 * 同时标记剪枝点并沿调用边汇总“改包/可睡眠/可抛异常”等子程序副作用。
 * 随后的 bpf_compute_postorder() 为数据流分析生成后序序列，
 * bpf_compute_scc() 则标出真正形成环的强连通分量，供状态收敛与回边传播使用。
 *
 * 所有这些工作都发生在单次 bpf_check() 所拥有的 env 上下文中；这里不自行加锁。
 * 临时 DFS 数组由本文件申请并在返回前释放，写入 env 的跳转表、后序数组和 SCC
 * 元数据则跨阶段存活，最终由验证器统一清理。
 */

/* non-recursive DFS pseudo code
 * 1  procedure DFS-iterative(G,v):
 * 2      label v as discovered
 * 3      let S be a stack
 * 4      S.push(v)
 * 5      while S is not empty
 * 6            t <- S.peek()
 * 7            if t is what we're looking for:
 * 8                return t
 * 9            for all edges e in G.adjacentEdges(t) do
 * 10               if edge e is already labelled
 * 11                   continue with the next edge
 * 12               w <- G.adjacentVertex(t,e)
 * 13               if vertex w is not discovered and not explored
 * 14                   label e as tree-edge
 * 15                   label w as discovered
 * 16                   S.push(w)
 * 17                   continue at 5
 * 18               else if vertex w is discovered
 * 19                   label e as back-edge
 * 20               else
 * 21                   // vertex w is explored
 * 22                   label e as forward- or cross-edge
 * 23           label t as explored
 * 24           S.pop()
 *
 * convention:
 * 0x10 - discovered
 * 0x11 - discovered and fall-through edge labelled
 * 0x12 - discovered and fall-through and branch edges labelled
 * 0x20 - explored
 */
/*
 * 上述伪代码的中文读法：顶点第一次入栈时是“已发现”，但尚未处理完全部出边；
 * 栈顶每次只推进一条尚未标记的边。若后继从未见过，该边是树边并把后继压栈；
 * 若后继仍处于已发现状态，该边是回边；若后继已经完成，则是前向边或交叉边。
 * 当两类出边都处理完后，顶点才变成“已探索”并出栈。低四位保存当前顶点已经
 * 推进到哪一类边，高四位保存顶点颜色，因此 DISCOVERED|FALLTHROUGH 和
 * DISCOVERED|BRANCH 既是颜色也是非递归 DFS 的程序计数器。
 */

enum {
	/* 已发现但尚未完成；高半字节用于与边进度位分离。 */
	DISCOVERED = 0x10,
	/* 所有后继均已处理，之后遇到它只形成前向边或交叉边。 */
	EXPLORED = 0x20,
	/* 第一类边：顺序执行边；也复用于只有一个目标的无条件跳转。 */
	FALLTHROUGH = 1,
	/* 第二类边：条件分支或函数调用的非顺序目标。 */
	BRANCH = 2,
};

/*
 * 业务背景：CFG 扫描遇到会使包数据指针失效的 helper/kfunc 时，要把影响记到
 * 当前子程序，稍后再沿调用图向调用者传播。
 * 入参：env 是本次验证环境；off 是产生副作用的指令下标。
 * 出参/返回：无返回值；将包含 off 的子程序 changes_pkt_data 置真。
 * 注意事项：要求子程序边界已经建立且 off 属于某个子程序；只做单调置位。
 */
static void mark_subprog_changes_pkt_data(struct bpf_verifier_env *env, int off)
{
	struct bpf_subprog_info *subprog;

	subprog = bpf_find_containing_subprog(env, off);
	subprog->changes_pkt_data = true;
}

/*
 * 业务背景：睡眠属性按子程序汇总，使调用上下文检查和最终程序属性能快速查询。
 * 入参：env 是本次验证环境；off 是潜在睡眠调用所在的指令下标。
 * 出参/返回：无返回值；将包含 off 的子程序 might_sleep 置真。
 * 注意事项：该标志是保守属性，只从假变真；死代码消除可能让最终精度略有差异。
 */
static void mark_subprog_might_sleep(struct bpf_verifier_env *env, int off)
{
	struct bpf_subprog_info *subprog;

	subprog = bpf_find_containing_subprog(env, off);
	subprog->might_sleep = true;
}

/*
 * 业务背景：可抛异常的 kfunc 会改变子程序的控制流语义，需要记录并向上传播。
 * 入参：env 是本次验证环境；off 是 throw kfunc 调用的指令下标。
 * 出参/返回：无返回值；将包含 off 的子程序 might_throw 置真。
 * 注意事项：只记录静态“可能”属性，不表示该路径运行时一定抛出异常。
 */
static void mark_subprog_might_throw(struct bpf_verifier_env *env, int off)
{
	struct bpf_subprog_info *subprog;

	subprog = bpf_find_containing_subprog(env, off);
	subprog->might_throw = true;
}

/* 't' is an index of a call-site.
 * 'w' is a callee entry point.
 * Eventually this function would be called when env->cfg.insn_state[w] == EXPLORED.
 * Rely on DFS traversal order and absence of recursive calls to guarantee that
 * callee's effect marks would be correct at that moment.
 */
/*
 * 中文说明：t 是调用点，w 是被调子程序入口。非递归 DFS 会先压入并完整探索
 * callee，回到调用点后才执行本函数；又因为此阶段不允许递归调用，所以此刻
 * callee 的传递副作用已经稳定，可以安全地并入 caller。
 *
 * 业务背景：调用者的属性必须包含所有可达被调函数的副作用，不能只看函数体。
 * 入参：env 是验证环境；t 为调用指令下标；w 为被调子程序入口下标。
 * 出参/返回：无返回值；对调用者的三个布尔属性执行按位或合并。
 * 注意事项：正确性依赖“callee 已 EXPLORED”及无递归调用；标志传播是单调的。
 */
static void merge_callee_effects(struct bpf_verifier_env *env, int t, int w)
{
	struct bpf_subprog_info *caller, *callee;

	caller = bpf_find_containing_subprog(env, t);
	callee = bpf_find_containing_subprog(env, w);
	caller->changes_pkt_data |= callee->changes_pkt_data;
	caller->might_sleep |= callee->might_sleep;
	caller->might_throw |= callee->might_throw;
}

enum {
	/* 当前栈顶已经处理完毕，外层循环应把它染成 EXPLORED 并弹栈。 */
	DONE_EXPLORING = 0,
	/* 新顶点已压栈，应先探索新栈顶，暂时保留当前顶点。 */
	KEEP_EXPLORING = 1,
};

/* t, w, e - match pseudo-code above:
 * t - index of current instruction
 * w - next instruction
 * e - edge
 */
/*
 * 中文说明：t、w、e 分别对应顶部伪代码中的当前顶点、邻接顶点和边类别。
 * 函数先用 t 的低位判断该类边是否已经处理，再验证 w 范围、标记分支目标，
 * 最后依据 w 的颜色把它分类为树边、回边或前向/交叉边。
 *
 * 业务背景：在不使用 C 递归的前提下推进一条 CFG 边，并在结构检查期间识别环。
 * 入参：t 是当前指令；w 是目标指令；e 为 FALLTHROUGH 或 BRANCH；env 持有栈、
 * 状态数组、程序长度以及权限信息。
 * 出参/返回：KEEP_EXPLORING 表示压入了新顶点；DONE_EXPLORING 表示本轮不需
 * 下钻；负 errno 表示越界、栈容量异常、非法回边或内部状态损坏。
 * 注意事项：CAP_BPF 调用者允许把回边留给后续有界循环验证；非特权程序在这里
 * 直接拒绝回边。函数只给 BRANCH 目标设置剪枝点/跳转点，调用者负责其余特例。
 */
static int push_insn(int t, int w, int e, struct bpf_verifier_env *env)
{
	int *insn_stack = env->cfg.insn_stack;
	int *insn_state = env->cfg.insn_state;

	if (e == FALLTHROUGH && insn_state[t] >= (DISCOVERED | FALLTHROUGH))
		/* 该顶点的顺序边已经推进过，不能重复压入同一后继。 */
		return DONE_EXPLORING;

	if (e == BRANCH && insn_state[t] >= (DISCOVERED | BRANCH))
		/* BRANCH 数值更大，因此到达此状态也隐含顺序边已经处理。 */
		return DONE_EXPLORING;

	if (w < 0 || w >= env->prog->len) {
		verbose_linfo(env, t, "%d: ", t);
		verbose(env, "jump out of range from insn %d to %d\n", t, w);
		return -EINVAL;
	}

	if (e == BRANCH) {
		/* mark branch target for state pruning */
		/* 中文：分支汇合处需要保存/比较状态，也是非线性历史的落点。 */
		mark_prune_point(env, w);
		mark_jmp_point(env, w);
	}

	if (insn_state[w] == 0) {
		/* tree-edge */
		/* 中文：首次发现 w，先记录 t 已处理的边，再把 w 压成新栈顶。 */
		insn_state[t] = DISCOVERED | e;
		insn_state[w] = DISCOVERED;
		if (env->cfg.cur_stack >= env->prog->len)
			return -E2BIG;
		insn_stack[env->cfg.cur_stack++] = w;
		return KEEP_EXPLORING;
	} else if ((insn_state[w] & 0xF0) == DISCOVERED) {
		/* w 仍在 DFS 活动路径上，t -> w 因而是回边，也就是图中的环。 */
		if (env->bpf_capable)
			return DONE_EXPLORING;
		verbose_linfo(env, t, "%d: ", t);
		verbose_linfo(env, w, "%d: ", w);
		verbose(env, "back-edge from insn %d to %d\n", t, w);
		return -EINVAL;
	} else if (insn_state[w] == EXPLORED) {
		/* forward- or cross-edge */
		/* 中文：目标已完成，无需再压栈，但仍须推进 t 的边状态。 */
		insn_state[t] = DISCOVERED | e;
	} else {
		verifier_bug(env, "insn state internal bug");
		return -EFAULT;
	}
	return DONE_EXPLORING;
}

/*
 * 业务背景：BPF-to-BPF 调用在 CFG 中同时具有“返回后继续执行”和“进入 callee”
 * 两类关系；伪函数引用也复用这一结构，但可选择不遍历 callee。
 * 入参：t 是调用点；insns 是程序指令数组；env 是验证环境；visit_callee 指示
 * 是否把 imm 指向的子程序入口作为分支边访问。
 * 出参/返回：返回 push_insn() 的三态结果或负 errno。
 * 注意事项：先推进返回地址边，第二次访问调用点时才合并 callee 副作用并推进
 * 调用边；ldimm64 形式的伪函数占两条槽位，返回地址需跨过完整指令。
 */
static int visit_func_call_insn(int t, struct bpf_insn *insns,
				struct bpf_verifier_env *env,
				bool visit_callee)
{
	int ret, insn_sz;
	int w;

	insn_sz = bpf_is_ldimm64(&insns[t]) ? 2 : 1;
	ret = push_insn(t, t + insn_sz, FALLTHROUGH, env);
	if (ret)
		return ret;

	mark_prune_point(env, t + insn_sz);
	/* when we exit from subprog, we need to record non-linear history */
	/* 中文：callee 返回不是 caller 内的线性前驱，故返回点必须记录跳转历史。 */
	mark_jmp_point(env, t + insn_sz);

	if (visit_callee) {
		w = t + insns[t].imm + 1;
		mark_prune_point(env, t);
		merge_callee_effects(env, t, w);
		ret = push_insn(t, w, BRANCH, env);
	}
	return ret;
}

/*
 * 业务背景：后继集合大小可能是 0、2 或任意跳转表长度，柔性数组可统一承载。
 * 入参：old 是原数组（可为 NULL）；n_elem 是新 items 容量及初始 cnt。
 * 出参/返回：成功返回可由 kvfree() 释放的新地址；失败返回 NULL。
 * 注意事项：kvrealloc() 可能搬迁对象；失败时本函数会主动释放 old，因此调用者
 * 不得继续使用旧指针，也不能再次释放它。大小计算沿用内核分配器的记账标志。
 */
struct bpf_iarray *bpf_iarray_realloc(struct bpf_iarray *old, size_t n_elem)
{
	size_t new_size = sizeof(struct bpf_iarray) + n_elem * sizeof(old->items[0]);
	struct bpf_iarray *new;

	new = kvrealloc(old, new_size, GFP_KERNEL_ACCOUNT);
	if (!new) {
		/* this is what callers always want, so simplify the call site */
		/* 中文：约定“扩容失败即放弃整个数组”，集中释放可简化所有调用点。 */
		kvfree(old);
		return NULL;
	}

	new->cnt = n_elem;
	return new;
}

/*
 * 业务背景：gotox 的候选目标保存在指令数组 map 中，建 CFG 前要把指定键区间
 * 对应的已翻译指令偏移复制到普通内存。
 * 入参：map 是指令数组 map；start/end 是闭区间键；items 至少可容纳
 * end-start+1 个 u32。
 * 出参/返回：成功返回 0 并按键顺序写入 xlated_off；失败返回 map 查询错误或
 * -EINVAL。
 * 注意事项：调用阶段假定 map 内容在验证期间稳定；array map 正常不会返回错误
 * 或 NULL，但仍防御性检查，避免静态分析遗漏异常 map 实现。
 */
static int copy_insn_array(struct bpf_map *map, u32 start, u32 end, u32 *items)
{
	struct bpf_insn_array_value *value;
	u32 i;

	for (i = start; i <= end; i++) {
		value = map->ops->map_lookup_elem(map, &i);
		/*
		 * map_lookup_elem of an array map will never return an error,
		 * but not checking it makes some static analysers to worry
		 */
		/* 中文：数组 map 按契约不会失败；显式检查仍让错误传播和静态分析完整。 */
		if (IS_ERR(value))
			return PTR_ERR(value);
		else if (!value)
			return -EINVAL;
		items[i - start] = value->xlated_off;
	}
	return 0;
}

/*
 * 业务背景：内核通用 sort() 需要 void 指针形式的比较回调来排列指令偏移。
 * 入参：a、b 分别指向一个 u32 元素。
 * 出参/返回：返回负数、零或正数，分别表示 *a 小于、等于或大于 *b。
 * 注意事项：这是现有的减法比较实现；学习时应留意极端 u32 差值转换为 int 的
 * 平台语义。调用处只依赖形成稳定的相等分组以去重，不取得元素所有权。
 */
static int cmp_ptr_to_u32(const void *a, const void *b)
{
	return *(u32 *)a - *(u32 *)b;
}

/*
 * 业务背景：多个 map 项或多个 map 可能指向同一指令，遍历 CFG 前需排序去重，
 * 避免同一目标被反复处理。
 * 入参：items 是可原地修改的 u32 数组；cnt 是有效元素数。
 * 出参/返回：返回去重后的元素数，唯一元素紧凑存放在 items[0..返回值)。
 * 注意事项：函数会重排输入；当前调用路径保证 cnt 至少为 1，故 unique 从 1
 * 开始。尾部旧元素不清零，必须只按返回计数读取。
 */
static int sort_insn_array_uniq(u32 *items, int cnt)
{
	int unique = 1;
	int i;

	sort(items, cnt, sizeof(items[0]), cmp_ptr_to_u32, NULL);

	for (i = 1; i < cnt; i++)
		if (items[i] != items[unique - 1])
			items[unique++] = items[i];

	return unique;
}

/*
 * sort_unique({map[start], ..., map[end]}) into off
 */
/* 中文：把闭区间 map[start..end] 的偏移复制到 off，随后原地排序并去重。 */
/*
 * 业务背景：为跳转表构造提供“读取 + 规范化”为唯一偏移集合的公共入口。
 * 入参：map 是指令数组 map；start/end 是闭区间键；off 是调用者提供的输出缓冲。
 * 出参/返回：成功返回唯一偏移数量；失败返回 copy_insn_array() 的负 errno。
 * 注意事项：要求 start <= end 且 off 容量不少于 end-start+1；输出顺序为升序，
 * 与 map 原始键顺序无关。
 */
int bpf_copy_insn_array_uniq(struct bpf_map *map, u32 start, u32 end, u32 *off)
{
	u32 n = end - start + 1;
	int err;

	err = copy_insn_array(map, start, end, off);
	if (err)
		return err;

	return sort_insn_array_uniq(off, n);
}

/*
 * Copy all unique offsets from the map
 */
/* 中文：读取 map 的全部槽位，返回排序去重后的目标偏移集合。 */
/*
 * 业务背景：每张候选指令数组 map 先独立转换成规范化 bpf_iarray，供跨 map 合并。
 * 入参：map 是待读取的指令数组 map，使用其 max_entries 决定容量。
 * 出参/返回：成功返回调用者拥有的 jt；失败返回 ERR_PTR(-ENOMEM/-EINVAL/查询错误)。
 * 注意事项：成功对象须 kvfree()；空集合视为非法。bpf_iarray_realloc() 先把 cnt
 * 设为容量，去重后再缩小逻辑计数而不缩小实际分配。
 */
static struct bpf_iarray *jt_from_map(struct bpf_map *map)
{
	struct bpf_iarray *jt;
	int err;
	int n;

	jt = bpf_iarray_realloc(NULL, map->max_entries);
	if (!jt)
		return ERR_PTR(-ENOMEM);

	n = bpf_copy_insn_array_uniq(map, 0, map->max_entries - 1, jt->items);
	if (n < 0) {
		err = n;
		goto err_free;
	}
	if (n == 0) {
		err = -EINVAL;
		goto err_free;
	}
	jt->cnt = n;
	return jt;

err_free:
	kvfree(jt);
	return ERR_PTR(err);
}

/*
 * Find and collect all maps which fit in the subprog. Return the result as one
 * combined jump table in jt->items (allocated with kvcalloc)
 */
/*
 * 中文：扫描验证环境记录的候选指令数组 map，把看起来属于指定子程序的表合并，
 * 最后整体排序去重。注释中的 kvcalloc 是历史性描述；当前实际通过
 * bpf_iarray_realloc()/kvrealloc 分配，释放接口仍统一为 kvfree()。
 *
 * 业务背景：gotox 指令本身不编码全部目标，需要从与其所在子程序关联的 map
 * 集合恢复多后继跳转表。
 * 入参：env 提供候选 maps；subprog_start/subprog_end 定义左闭右开指令范围。
 * 出参/返回：成功返回合并且唯一化的 jt；失败返回 ERR_PTR，并已释放中间对象。
 * 注意事项：当前只能用每张表的首元素做初筛，且候选列表还可能包含 static key
 * 或间接调用 map；create_jt() 必须随后逐项做完整边界验证。
 */
static struct bpf_iarray *jt_from_subprog(struct bpf_verifier_env *env,
					  int subprog_start, int subprog_end)
{
	struct bpf_iarray *jt = NULL;
	struct bpf_map *map;
	struct bpf_iarray *jt_cur;
	int i;

	for (i = 0; i < env->insn_array_map_cnt; i++) {
		/*
		 * TODO (when needed): collect only jump tables, not static keys
		 * or maps for indirect calls
		 */
		/* 中文待办：未来若元数据足够，应只收集跳转表，排除静态键和间接调用 map。 */
		map = env->insn_array_maps[i];

		jt_cur = jt_from_map(map);
		if (IS_ERR(jt_cur)) {
			kvfree(jt);
			return jt_cur;
		}

		/*
		 * This is enough to check one element. The full table is
		 * checked to fit inside the subprog later in create_jt()
		 */
		/* 中文：首元素仅作归属初筛；安全边界由 create_jt() 的逐元素检查兜底。 */
		if (jt_cur->items[0] >= subprog_start && jt_cur->items[0] < subprog_end) {
			u32 old_cnt = jt ? jt->cnt : 0;
			jt = bpf_iarray_realloc(jt, old_cnt + jt_cur->cnt);
			if (!jt) {
				kvfree(jt_cur);
				return ERR_PTR(-ENOMEM);
			}
			memcpy(jt->items + old_cnt, jt_cur->items, jt_cur->cnt << 2);
		}

		kvfree(jt_cur);
	}

	if (!jt) {
		verbose(env, "no jump tables found for subprog starting at %u\n", subprog_start);
		return ERR_PTR(-EINVAL);
	}

	jt->cnt = sort_insn_array_uniq(jt->items, jt->cnt);
	return jt;
}

/*
 * 业务背景：为一条 gotox 指令生成最终的多目标 CFG 后继表，并阻止跨子程序跳转。
 * 入参：t 是 gotox 指令下标；env 提供子程序边界及候选指令数组 maps。
 * 出参/返回：成功返回调用者拥有的 jt；失败返回 ERR_PTR(-errno)。
 * 注意事项：范围采用 [start,end)；任何一个目标越界都会释放整张表并拒绝程序，
 * 不允许只丢弃坏目标后继续。成功对象随后存入 insn_aux_data[t].jt 统一托管。
 */
static struct bpf_iarray *
create_jt(int t, struct bpf_verifier_env *env)
{
	struct bpf_subprog_info *subprog;
	int subprog_start, subprog_end;
	struct bpf_iarray *jt;
	int i;

	subprog = bpf_find_containing_subprog(env, t);
	subprog_start = subprog->start;
	subprog_end = (subprog + 1)->start;
	jt = jt_from_subprog(env, subprog_start, subprog_end);
	if (IS_ERR(jt))
		return jt;

	/* Check that the every element of the jump table fits within the given subprogram */
	/* 中文：逐项验证是安全检查，保证间接跳转既不越界也不跨越子程序边界。 */
	for (i = 0; i < jt->cnt; i++) {
		if (jt->items[i] < subprog_start || jt->items[i] >= subprog_end) {
			verbose(env, "jump table for insn %d points outside of the subprog [%u,%u]\n",
					t, subprog_start, subprog_end);
			kvfree(jt);
			return ERR_PTR(-EINVAL);
		}
	}

	return jt;
}

/* "conditional jump with N edges" */
/* 中文：gotox 可视为拥有 N 条分支边的条件跳转，而不是普通的二分支指令。 */
/*
 * 业务背景：遍历 gotox 的所有静态候选目标，使 CFG 检查和后续数据流分析看见
 * 间接跳转的完整后继集合。
 * 入参：t 是 gotox 指令下标；env 提供 DFS 栈/颜色及 jt 缓存。
 * 出参/返回：若至少压入一个新目标返回 KEEP_EXPLORING；全部已见返回
 * DONE_EXPLORING；构表、越界或栈容量问题返回负 errno。
 * 注意事项：这里一次压入全部未见目标，DFS 会先处理最后压入者；jt 缓存在
 * insn_aux_data[t]，既供本轮复用也供 bpf_insn_successors() 后续读取，最终统一释放。
 */
static int visit_gotox_insn(int t, struct bpf_verifier_env *env)
{
	int *insn_stack = env->cfg.insn_stack;
	int *insn_state = env->cfg.insn_state;
	bool keep_exploring = false;
	struct bpf_iarray *jt;
	int i, w;

	jt = env->insn_aux_data[t].jt;
	if (!jt) {
		jt = create_jt(t, env);
		if (IS_ERR(jt))
			return PTR_ERR(jt);

		env->insn_aux_data[t].jt = jt;
	}

	mark_prune_point(env, t);
	for (i = 0; i < jt->cnt; i++) {
		w = jt->items[i];
		if (w < 0 || w >= env->prog->len) {
			verbose(env, "indirect jump out of range from insn %d to %d\n", t, w);
			return -EINVAL;
		}

		mark_jmp_point(env, w);

		/* EXPLORED || DISCOVERED */
		/* 中文：无论目标已完成还是仍在活动路径上，都无需重复压栈。 */
		if (insn_state[w])
			continue;

		if (env->cfg.cur_stack >= env->prog->len)
			return -E2BIG;

		insn_stack[env->cfg.cur_stack++] = w;
		insn_state[w] |= DISCOVERED;
		keep_exploring = true;
	}

	return keep_exploring ? KEEP_EXPLORING : DONE_EXPLORING;
}

/*
 * Instructions that can abnormally return from a subprog (tail_call
 * upon success, ld_{abs,ind} upon load failure) have a hidden exit
 * that the verifier must account for.
 */
/*
 * 中文：tail_call 成功时不回到下一条指令，ld_abs/ld_ind 读取失败时也会提前退出；
 * 两者都有普通继续路径和隐藏退出路径，必须显式建成两个后继。
 *
 * 业务背景：把解释器/JIT 隐含的异常退出转换为数据流分析可观察的 CFG 边。
 * 入参：env 是验证环境；t 是 tail_call 或 ld_abs/ld_ind 指令下标。
 * 出参/返回：成功（含已有缓存）返回 0；分配失败返回 -ENOMEM。
 * 注意事项：items[0] 是 t+1 的普通路径，items[1] 是当前子程序预先记录的 exit_idx；
 * 数组转交 insn_aux_data[t].jt 所有，不能由调用者释放。
 */
static int visit_abnormal_return_insn(struct bpf_verifier_env *env, int t)
{
	struct bpf_subprog_info *subprog;
	struct bpf_iarray *jt;

	if (env->insn_aux_data[t].jt)
		return 0;

	jt = bpf_iarray_realloc(NULL, 2);
	if (!jt)
		return -ENOMEM;

	subprog = bpf_find_containing_subprog(env, t);
	jt->items[0] = t + 1;
	jt->items[1] = subprog->exit_idx;
	env->insn_aux_data[t].jt = jt;
	return 0;
}

/* Visits the instruction at index t and returns one of the following:
 *  < 0 - an error occurred
 *  DONE_EXPLORING - the instruction was fully explored
 *  KEEP_EXPLORING - there is still work to be done before it is fully explored
 */
/*
 * 中文返回约定：负值表示错误；DONE_EXPLORING 表示 t 的边已处理完；
 * KEEP_EXPLORING 表示刚压入新后继，外层应先继续深入而不能弹出 t。
 *
 * 业务背景：根据指令类别枚举当前指令的控制流边，并顺手生成剪枝、检查点和
 * 子程序副作用元数据，是非递归 DFS 的指令级分派器。
 * 入参：t 是当前栈顶指令下标；env 提供程序、DFS 工作区、helper/kfunc 元数据。
 * 出参/返回：返回上述 DFS 三态之一或负 errno。
 * 注意事项：普通指令、调用、无条件跳转和条件跳转的“第一/第二条边”推进次序
 * 不同；函数只构造静态控制流，不在此处执行寄存器语义验证。
 */
static int visit_insn(int t, struct bpf_verifier_env *env)
{
	struct bpf_insn *insns = env->prog->insnsi, *insn = &insns[t];
	int ret, off, insn_sz;

	if (bpf_pseudo_func(insn))
		/* 伪函数地址仍有顺序边，并遍历其目标以纳入可达性和副作用传播。 */
		return visit_func_call_insn(t, insns, env, true);

	/* All non-branch instructions have a single fall-through edge. */
	/* 中文：非跳转指令通常只有顺序后继；ld_abs/ld_ind 还附带隐藏失败出口。 */
	if (BPF_CLASS(insn->code) != BPF_JMP &&
	    BPF_CLASS(insn->code) != BPF_JMP32) {
		if (BPF_CLASS(insn->code) == BPF_LD &&
		    (BPF_MODE(insn->code) == BPF_ABS ||
		     BPF_MODE(insn->code) == BPF_IND)) {
			ret = visit_abnormal_return_insn(env, t);
			if (ret)
				return ret;
		}
		insn_sz = bpf_is_ldimm64(insn) ? 2 : 1;
		return push_insn(t, t + insn_sz, FALLTHROUGH, env);
	}

	switch (BPF_OP(insn->code)) {
	case BPF_EXIT:
		/* 子程序退出没有同一帧内后继，当前顶点可立即完成。 */
		return DONE_EXPLORING;

	case BPF_CALL:
		if (bpf_is_async_callback_calling_insn(insn))
			/* Mark this call insn as a prune point to trigger
			 * is_state_visited() check before call itself is
			 * processed by __check_func_call(). Otherwise new
			 * async state will be pushed for further exploration.
			 */
			/* 中文：调用前先比较状态，避免每次都无条件派生新的异步回调状态。 */
			mark_prune_point(env, t);
		/* For functions that invoke callbacks it is not known how many times
		 * callback would be called. Verifier models callback calling functions
		 * by repeatedly visiting callback bodies and returning to origin call
		 * instruction.
		 * In order to stop such iteration verifier needs to identify when a
		 * state identical some state from a previous iteration is reached.
		 * Check below forces creation of checkpoint before callback calling
		 * instruction to allow search for such identical states.
		 */
		/*
		 * 中文：同步回调可被 helper 重复调用，验证器以“进入回调、返回原调用点”
		 * 模拟迭代。调用点前的强制检查点让 is_state_visited() 能发现与上一轮相同
		 * 的状态并收敛，否则循环本身没有显式 BPF 回边可供普通逻辑识别。
		 */
		if (bpf_is_sync_callback_calling_insn(insn)) {
			mark_calls_callback(env, t);
			mark_force_checkpoint(env, t);
			mark_prune_point(env, t);
			mark_jmp_point(env, t);
		}
		if (bpf_helper_call(insn)) {
			const struct bpf_func_proto *fp;

			ret = bpf_get_helper_proto(env, insn->imm, &fp);
			/* If called in a non-sleepable context program will be
			 * rejected anyway, so we should end up with precise
			 * sleepable marks on subprogs, except for dead code
			 * elimination.
			 */
			/*
			 * 中文：非睡眠上下文中的睡眠 helper 之后仍会被拒绝；这里先记录静态
			 * 属性，通常能精确反映可达调用，只有后续死代码变换可能留下保守标记。
			 */
			if (ret == 0 && fp->might_sleep)
				mark_subprog_might_sleep(env, t);
			if (bpf_helper_changes_pkt_data(insn->imm))
				mark_subprog_changes_pkt_data(env, t);
			if (insn->imm == BPF_FUNC_tail_call) {
				/* 成功分支离开当前程序，失败分支才顺序执行，需补隐藏出口。 */
				ret = visit_abnormal_return_insn(env, t);
				if (ret)
					return ret;
			}
		} else if (insn->src_reg == BPF_PSEUDO_KFUNC_CALL) {
			struct bpf_kfunc_call_arg_meta meta;

			ret = bpf_fetch_kfunc_arg_meta(env, insn->imm, insn->off, &meta);
			if (ret == 0 && bpf_is_iter_next_kfunc(&meta)) {
				mark_prune_point(env, t);
				/* Checking and saving state checkpoints at iter_next() call
				 * is crucial for fast convergence of open-coded iterator loop
				 * logic, so we need to force it. If we don't do that,
				 * is_state_visited() might skip saving a checkpoint, causing
				 * unnecessarily long sequence of not checkpointed
				 * instructions and jumps, leading to exhaustion of jump
				 * history buffer, and potentially other undesired outcomes.
				 * It is expected that with correct open-coded iterators
				 * convergence will happen quickly, so we don't run a risk of
				 * exhausting memory.
				 */
				/*
				 * 中文：开放编码迭代器的 next 调用是逻辑循环关口。强制保存状态可让
				 * 相邻轮次尽快比较并收敛，避免过长的无检查点路径耗尽跳转历史。
				 */
				mark_force_checkpoint(env, t);
			}
			/* Same as helpers, if called in a non-sleepable context
			 * program will be rejected anyway, so we should end up
			 * with precise sleepable marks on subprogs, except for
			 * dead code elimination.
			 */
			/* 中文：与 helper 相同，先保守记录 kfunc 的睡眠属性，再由后续阶段判定上下文。 */
			if (ret == 0 && bpf_is_kfunc_sleepable(&meta))
				mark_subprog_might_sleep(env, t);
			if (ret == 0 && bpf_is_kfunc_pkt_changing(&meta))
				mark_subprog_changes_pkt_data(env, t);
			if (ret == 0 && bpf_is_throw_kfunc(insn))
				mark_subprog_might_throw(env, t);
		}
		return visit_func_call_insn(t, insns, env, insn->src_reg == BPF_PSEUDO_CALL);

	case BPF_JA:
		if (BPF_SRC(insn->code) == BPF_X)
			/* BPF_X 形式从跳转表取 N 个目标，不能按单一立即数偏移处理。 */
			return visit_gotox_insn(t, env);

		if (BPF_CLASS(insn->code) == BPF_JMP)
			off = insn->off;
		else
			off = insn->imm;

		/* unconditional jump with single edge */
		/* 中文：虽然没有真正“落空”路径，仍借 FALLTHROUGH 槽位推进唯一出边。 */
		ret = push_insn(t, t + off + 1, FALLTHROUGH, env);
		if (ret)
			return ret;

		mark_prune_point(env, t + off + 1);
		mark_jmp_point(env, t + off + 1);

		return ret;

	default:
		/* conditional jump with two edges */
		/* 中文：先访问不跳转的 t+1，再访问采用 off 的分支目标。 */
		mark_prune_point(env, t);
		if (bpf_is_may_goto_insn(insn))
			mark_force_checkpoint(env, t);

		ret = push_insn(t, t + 1, FALLTHROUGH, env);
		if (ret)
			return ret;

		return push_insn(t, t + insn->off + 1, BRANCH, env);
	}
}

/* non-recursive depth-first-search to detect loops in BPF program
 * loop == back-edge in directed graph
 */
/* 中文：以显式栈执行 DFS；在有向 CFG 中，指向活动祖先的回边即表明存在环。 */
/*
 * 业务背景：在昂贵的抽象执行开始前，验证整张 CFG 的基本结构并建立剪枝点、
 * 跳转点、异常后继及传递副作用等基础元数据。
 * 入参：env 是已完成子程序发现和调用元数据准备的验证环境。
 * 出参/返回：CFG 合法返回 0；内存不足、边越界、非特权回边、不可达代码、跳入
 * ldimm64 中部或内部不变量破坏时返回负 errno，并通过日志给出位置。
 * 注意事项：临时 state/stack 在所有出口释放并把 env 指针清空；异常回调不是主
 * 入口可达路径时作为第二个根单独遍历。成功后才发布主子程序的程序级副作用。
 */
int bpf_check_cfg(struct bpf_verifier_env *env)
{
	int insn_cnt = env->prog->len;
	int *insn_stack, *insn_state;
	int ex_insn_beg, i, ret = 0;

	insn_state = env->cfg.insn_state = kvzalloc_objs(int, insn_cnt,
							 GFP_KERNEL_ACCOUNT);
	if (!insn_state)
		return -ENOMEM;

	insn_stack = env->cfg.insn_stack = kvzalloc_objs(int, insn_cnt,
							 GFP_KERNEL_ACCOUNT);
	if (!insn_stack) {
		kvfree(insn_state);
		return -ENOMEM;
	}

	ex_insn_beg = env->exception_callback_subprog
		      ? env->subprog_info[env->exception_callback_subprog].start
		      : 0;

	insn_state[0] = DISCOVERED; /* mark 1st insn as discovered */
	insn_stack[0] = 0; /* 0 is the first instruction */
	/* 中文：主程序第 0 条指令是第一棵 DFS 树的根。 */
	env->cfg.cur_stack = 1;

walk_cfg:
	/* 标签允许主入口完成后复用同一工作区遍历独立的异常回调根。 */
	while (env->cfg.cur_stack > 0) {
		int t = insn_stack[env->cfg.cur_stack - 1];

		ret = visit_insn(t, env);
		switch (ret) {
		case DONE_EXPLORING:
			/* 顶点所有边已推进，染黑并弹出显式 DFS 栈。 */
			insn_state[t] = EXPLORED;
			env->cfg.cur_stack--;
			break;
		case KEEP_EXPLORING:
			/* 新后继已成为栈顶；保留 t，下一轮优先下钻。 */
			break;
		default:
			if (ret > 0) {
				verifier_bug(env, "visit_insn internal bug");
				ret = -EFAULT;
			}
			goto err_free;
		}
	}

	if (env->cfg.cur_stack < 0) {
		verifier_bug(env, "pop stack internal bug");
		ret = -EFAULT;
		goto err_free;
	}

	if (ex_insn_beg && insn_state[ex_insn_beg] != EXPLORED) {
		/* 异常回调可合法地与主 CFG 不连通，因此把它视为第二入口。 */
		insn_state[ex_insn_beg] = DISCOVERED;
		insn_stack[0] = ex_insn_beg;
		env->cfg.cur_stack = 1;
		goto walk_cfg;
	}

	for (i = 0; i < insn_cnt; i++) {
		struct bpf_insn *insn = &env->prog->insnsi[i];

		if (insn_state[i] != EXPLORED) {
			/* 除 ldimm64 数据槽外，每条指令都必须从某个合法入口可达。 */
			verbose(env, "unreachable insn %d\n", i);
			ret = -EINVAL;
			goto err_free;
		}
		if (bpf_is_ldimm64(insn)) {
			/* ldimm64 占两个槽；第二槽只能作为前一条指令的数据，不能成为顶点。 */
			if (insn_state[i + 1] != 0) {
				verbose(env, "jump into the middle of ldimm64 insn %d\n", i);
				ret = -EINVAL;
				goto err_free;
			}
			i++; /* skip second half of ldimm64 */
			/* 中文：跳过已检查的第二槽，防止把数据字误当独立指令再检查。 */
		}
	}
	ret = 0; /* cfg looks good */
	/* 中文：只有整图通过后，才把主子程序的传递属性提交到程序 aux。 */
	env->prog->aux->changes_pkt_data = env->subprog_info[0].changes_pkt_data;
	env->prog->aux->might_sleep = env->subprog_info[0].might_sleep;

err_free:
	/* 单一清理出口保证成功和失败都不把临时 DFS 指针遗留在 env 中。 */
	kvfree(insn_state);
	kvfree(insn_stack);
	env->cfg.insn_state = env->cfg.insn_stack = NULL;
	return ret;
}

/*
 * For each subprogram 'i' fill array env->cfg.insn_subprogram sub-range
 * [env->subprog_info[i].postorder_start, env->subprog_info[i+1].postorder_start)
 * with indices of 'i' instructions in postorder.
 */
int bpf_compute_postorder(struct bpf_verifier_env *env)
{
	u32 cur_postorder, i, top, stack_sz, s;
	int *stack = NULL, *postorder = NULL, *state = NULL;
	struct bpf_iarray *succ;

	postorder = kvzalloc_objs(int, env->prog->len, GFP_KERNEL_ACCOUNT);
	state = kvzalloc_objs(int, env->prog->len, GFP_KERNEL_ACCOUNT);
	stack = kvzalloc_objs(int, env->prog->len, GFP_KERNEL_ACCOUNT);
	if (!postorder || !state || !stack) {
		kvfree(postorder);
		kvfree(state);
		kvfree(stack);
		return -ENOMEM;
	}
	cur_postorder = 0;
	for (i = 0; i < env->subprog_cnt; i++) {
		env->subprog_info[i].postorder_start = cur_postorder;
		stack[0] = env->subprog_info[i].start;
		stack_sz = 1;
		do {
			top = stack[stack_sz - 1];
			state[top] |= DISCOVERED;
			if (state[top] & EXPLORED) {
				postorder[cur_postorder++] = top;
				stack_sz--;
				continue;
			}
			succ = bpf_insn_successors(env, top);
			for (s = 0; s < succ->cnt; ++s) {
				if (!state[succ->items[s]]) {
					stack[stack_sz++] = succ->items[s];
					state[succ->items[s]] |= DISCOVERED;
				}
			}
			state[top] |= EXPLORED;
		} while (stack_sz);
	}
	env->subprog_info[i].postorder_start = cur_postorder;
	env->cfg.insn_postorder = postorder;
	env->cfg.cur_postorder = cur_postorder;
	kvfree(stack);
	kvfree(state);
	return 0;
}

/*
 * Compute strongly connected components (SCCs) on the CFG.
 * Assign an SCC number to each instruction, recorded in env->insn_aux[*].scc.
 * If instruction is a sole member of its SCC and there are no self edges,
 * assign it SCC number of zero.
 * Uses a non-recursive adaptation of Tarjan's algorithm for SCC computation.
 */
int bpf_compute_scc(struct bpf_verifier_env *env)
{
	const u32 NOT_ON_STACK = U32_MAX;

	struct bpf_insn_aux_data *aux = env->insn_aux_data;
	const u32 insn_cnt = env->prog->len;
	int stack_sz, dfs_sz, err = 0;
	u32 *stack, *pre, *low, *dfs;
	u32 i, j, t, w;
	u32 next_preorder_num;
	u32 next_scc_id;
	bool assign_scc;
	struct bpf_iarray *succ;

	next_preorder_num = 1;
	next_scc_id = 1;
	/*
	 * - 'stack' accumulates vertices in DFS order, see invariant comment below;
	 * - 'pre[t] == p' => preorder number of vertex 't' is 'p';
	 * - 'low[t] == n' => smallest preorder number of the vertex reachable from 't' is 'n';
	 * - 'dfs' DFS traversal stack, used to emulate explicit recursion.
	 */
	stack = kvcalloc(insn_cnt, sizeof(int), GFP_KERNEL_ACCOUNT);
	pre = kvcalloc(insn_cnt, sizeof(int), GFP_KERNEL_ACCOUNT);
	low = kvcalloc(insn_cnt, sizeof(int), GFP_KERNEL_ACCOUNT);
	dfs = kvcalloc(insn_cnt, sizeof(*dfs), GFP_KERNEL_ACCOUNT);
	if (!stack || !pre || !low || !dfs) {
		err = -ENOMEM;
		goto exit;
	}
	/*
	 * References:
	 * [1] R. Tarjan "Depth-First Search and Linear Graph Algorithms"
	 * [2] D. J. Pearce "A Space-Efficient Algorithm for Finding Strongly Connected Components"
	 *
	 * The algorithm maintains the following invariant:
	 * - suppose there is a path 'u' ~> 'v', such that 'pre[v] < pre[u]';
	 * - then, vertex 'u' remains on stack while vertex 'v' is on stack.
	 *
	 * Consequently:
	 * - If 'low[v] < pre[v]', there is a path from 'v' to some vertex 'u',
	 *   such that 'pre[u] == low[v]'; vertex 'u' is currently on the stack,
	 *   and thus there is an SCC (loop) containing both 'u' and 'v'.
	 * - If 'low[v] == pre[v]', loops containing 'v' have been explored,
	 *   and 'v' can be considered the root of some SCC.
	 *
	 * Here is a pseudo-code for an explicitly recursive version of the algorithm:
	 *
	 *    NOT_ON_STACK = insn_cnt + 1
	 *    pre = [0] * insn_cnt
	 *    low = [0] * insn_cnt
	 *    scc = [0] * insn_cnt
	 *    stack = []
	 *
	 *    next_preorder_num = 1
	 *    next_scc_id = 1
	 *
	 *    def recur(w):
	 *        nonlocal next_preorder_num
	 *        nonlocal next_scc_id
	 *
	 *        pre[w] = next_preorder_num
	 *        low[w] = next_preorder_num
	 *        next_preorder_num += 1
	 *        stack.append(w)
	 *        for s in successors(w):
	 *            # Note: for classic algorithm the block below should look as:
	 *            #
	 *            # if pre[s] == 0:
	 *            #     recur(s)
	 *            #     low[w] = min(low[w], low[s])
	 *            # elif low[s] != NOT_ON_STACK:
	 *            #     low[w] = min(low[w], pre[s])
	 *            #
	 *            # But replacing both 'min' instructions with 'low[w] = min(low[w], low[s])'
	 *            # does not break the invariant and makes iterative version of the algorithm
	 *            # simpler. See 'Algorithm #3' from [2].
	 *
	 *            # 's' not yet visited
	 *            if pre[s] == 0:
	 *                recur(s)
	 *            # if 's' is on stack, pick lowest reachable preorder number from it;
	 *            # if 's' is not on stack 'low[s] == NOT_ON_STACK > low[w]',
	 *            # so 'min' would be a noop.
	 *            low[w] = min(low[w], low[s])
	 *
	 *        if low[w] == pre[w]:
	 *            # 'w' is the root of an SCC, pop all vertices
	 *            # below 'w' on stack and assign same SCC to them.
	 *            while True:
	 *                t = stack.pop()
	 *                low[t] = NOT_ON_STACK
	 *                scc[t] = next_scc_id
	 *                if t == w:
	 *                    break
	 *            next_scc_id += 1
	 *
	 *    for i in range(0, insn_cnt):
	 *        if pre[i] == 0:
	 *            recur(i)
	 *
	 * Below implementation replaces explicit recursion with array 'dfs'.
	 */
	for (i = 0; i < insn_cnt; i++) {
		if (pre[i])
			continue;
		stack_sz = 0;
		dfs_sz = 1;
		dfs[0] = i;
dfs_continue:
		while (dfs_sz) {
			w = dfs[dfs_sz - 1];
			if (pre[w] == 0) {
				low[w] = next_preorder_num;
				pre[w] = next_preorder_num;
				next_preorder_num++;
				stack[stack_sz++] = w;
			}
			/* Visit 'w' successors */
			succ = bpf_insn_successors(env, w);
			for (j = 0; j < succ->cnt; ++j) {
				if (pre[succ->items[j]]) {
					low[w] = min(low[w], low[succ->items[j]]);
				} else {
					dfs[dfs_sz++] = succ->items[j];
					goto dfs_continue;
				}
			}
			/*
			 * Preserve the invariant: if some vertex above in the stack
			 * is reachable from 'w', keep 'w' on the stack.
			 */
			if (low[w] < pre[w]) {
				dfs_sz--;
				goto dfs_continue;
			}
			/*
			 * Assign SCC number only if component has two or more elements,
			 * or if component has a self reference, or if instruction is a
			 * callback calling function (implicit loop).
			 */
			assign_scc = stack[stack_sz - 1] != w;	/* two or more elements? */
			for (j = 0; j < succ->cnt; ++j) {	/* self reference? */
				if (succ->items[j] == w) {
					assign_scc = true;
					break;
				}
			}
			if (bpf_calls_callback(env, w)) /* implicit loop? */
				assign_scc = true;
			/* Pop component elements from stack */
			do {
				t = stack[--stack_sz];
				low[t] = NOT_ON_STACK;
				if (assign_scc)
					aux[t].scc = next_scc_id;
			} while (t != w);
			if (assign_scc)
				next_scc_id++;
			dfs_sz--;
		}
	}
	env->scc_info = kvzalloc_objs(*env->scc_info, next_scc_id,
				      GFP_KERNEL_ACCOUNT);
	if (!env->scc_info) {
		err = -ENOMEM;
		goto exit;
	}
	env->scc_cnt = next_scc_id;
exit:
	kvfree(stack);
	kvfree(pre);
	kvfree(low);
	kvfree(dfs);
	return err;
}
