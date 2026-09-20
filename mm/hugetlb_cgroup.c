// SPDX-License-Identifier: LGPL-2.1
/*
 *
 * Copyright IBM Corporation, 2012
 * Author Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *
 * Cgroup v2
 * Copyright (C) 2019 Red Hat, Inc.
 * Author: Giuseppe Scrivano <gscrivan@redhat.com>
 *
 */

#include <linux/cgroup.h>
#include <linux/page_counter.h>
#include <linux/slab.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>

#define MEMFILE_PRIVATE(x, val)	(((x) << 16) | (val))
#define MEMFILE_IDX(val)	(((val) >> 16) & 0xffff)
#define MEMFILE_ATTR(val)	((val) & 0xffff)
/* private 高位保存 hstate 下标，低位保存该尺寸文件的属性枚举。 */
/*
 * MEMFILE_PRIVATE 只在启动期把两个不超过 16 位的值打包；MEMFILE_IDX/MEMFILE_ATTR
 * 分别解出尺寸槽和 RES_* 属性。它们不做范围检查，模板构造者必须保证截断不会丢失有效位。
 */

/* Use t->m[0] to encode the offset */
/* 补充说明：offset 与字段大小一起编码，使模板可按 hstate 索引定位数组成员。 */
#define MEMFILE_OFFSET(t, m0)	(((offsetof(t, m0) << 16) | sizeof_field(t, m0)))
#define MEMFILE_OFFSET0(val)	(((val) >> 16) & 0xffff)
#define MEMFILE_FIELD_SIZE(val)	((val) & 0xffff)
/*
 * OFFSET 宏同样使用高低 16 位：高位编码数组首字段在 hugetlb_cgroup 内的字节偏移，低位
 * 编码单个 cgroup_file 的字节大小；实例化时 OFFSET0 + FIELD_SIZE * idx 得到具体元素偏移。
 */

#define DFL_TMPL_SIZE		ARRAY_SIZE(hugetlb_dfl_tmpl)
#define LEGACY_TMPL_SIZE	ARRAY_SIZE(hugetlb_legacy_tmpl)

/*
 * 这三个全局量组成控制器的发布状态：root_h_cgroup 在根 css 创建时写入，之后作为
 * reparent 的最终归属；dfl_files/legacy_files 在启动期分配、填充并交给 cgroup 核心。
 * 初始化完成后对象生命周期覆盖控制器运行期，普通 charge 路径只借用指针而不改写它们。
 */
static struct hugetlb_cgroup *root_h_cgroup __read_mostly;
static struct cftype *dfl_files;
static struct cftype *legacy_files;

/*
 * 按 hugepage 尺寸索引取得 cgroup 的实际占用或预留占用计数器。
 * h_cg 是调用者已稳定的 css；idx 对应 hstates[]，rsvd 区分尚未落实为
 * folio 的 reservation。这里只借用数组成员，计数器的生命周期仍随 h_cg。
 */
/*
 * 业务背景：actual 与 reservation 使用同一索引布局，内部选择器让 charge、commit、
 * uncharge 共享一套状态机，而不在每条路径重复字段分派。
 * 入参：h_cg 是非空、由调用者稳定生命周期的借用对象；idx 的范围是
 * [0, HUGE_MAX_HSTATE)，表示 hstates[] 槽；rsvd 为 true 选择预留计数器，为 false
 * 选择已兑现为 folio 的实际占用计数器，三个参数均为纯输入。
 * 出参/返回：返回 h_cg 内嵌 page_counter 的借用指针，不增加 css 引用、不改变计数。
 * 注意事项：本函数无锁、不可睡眠；返回指针不能活过 h_cg，idx 越界也不会在本层拦截，
 * 因此调用者必须从合法 hstate 获取索引并持有 css 引用、RCU 保护或更强生命周期保证。
 */
static inline struct page_counter *
__hugetlb_cgroup_counter_from_cgroup(struct hugetlb_cgroup *h_cg, int idx,
				     bool rsvd)
{
	if (rsvd)
		return &h_cg->rsvd_hugepage[idx];
	return &h_cg->hugepage[idx];
}

/*
 * 业务背景：实际分配成功后的 HugeTLB 页要记入 usage 计数器；这是 allocation
 * 路径在决定是否越过 cgroup 上限时使用的包装层。
 * 入参：h_cg 为借用的目标 cgroup，idx 是 hugepage 尺寸槽。出参/返回：返回
 * 借用的 page_counter 指针。注意事项：不取得 css 引用，调用者必须自行保证 h_cg 有效。
 */
/*
 * 业务背景：这是 actual 计费分支的具名入口，位于 HugeTLB 分配的试扣、提交和释放链上。
 * 入参：h_cg 非空且为借用输入；idx 为 [0, HUGE_MAX_HSTATE) 的 hstate 下标，无字节单位。
 * 出参/返回：返回 hugepage[idx] 的借用指针；无输出参数、无引用或 ownership 转移。
 * 注意事项：纯字段选择，不加锁且不睡眠；计数更新仍须遵守 page_counter 及外层 css 生命周期协议。
 */
static inline struct page_counter *
hugetlb_cgroup_counter_from_cgroup(struct hugetlb_cgroup *h_cg, int idx)
{
	return __hugetlb_cgroup_counter_from_cgroup(h_cg, idx, false);
}

/*
 * 业务背景：reservation 先消耗配额而不绑定具体 folio，因此有独立计数器。
 * 入参：h_cg、idx 与实际占用版本相同。出参/返回：返回预留计数器的借用指针。
 * 注意事项：预留成功后需由 reservation 持有 css 引用，不能把它当作普通 usage。
 */
/*
 * 业务背景：这是 reservation 计费分支的具名入口，预留量尚无 folio 可在 css offline
 * 时迁移，因此其生命周期规则与 actual 计费不同。
 * 入参：h_cg 非空且为借用输入；idx 为 [0, HUGE_MAX_HSTATE) 的 hstate 下标。
 * 出参/返回：返回 rsvd_hugepage[idx] 的借用指针；不取得 css 引用，也不修改计数。
 * 注意事项：无锁且不睡眠；长期保存此计数器的路径必须另持 css 引用，并在预留撤销时 css_put()。
 */
static inline struct page_counter *
hugetlb_cgroup_counter_from_cgroup_rsvd(struct hugetlb_cgroup *h_cg, int idx)
{
	return __hugetlb_cgroup_counter_from_cgroup(h_cg, idx, true);
}

/*
 * 业务背景：cgroup 核心把各子系统状态以通用 css 保存；本文件需要恢复 hugetlb
 * 私有容器以访问配额和 NUMA 统计。入参：s 是可空的借用 css。出参/返回：NULL 或
 * 包含它的 hugetlb_cgroup 借用指针。注意事项：container_of 不增加引用，离开 RCU/引用保护后不可保存结果。
 */
/*
 * 业务背景：cgroup 核心只认识通用 css，本转换器是进入 hugetlb 私有状态的类型边界。
 * 入参：s 是可为 NULL 的纯输入借用指针；非空时必须确属 hugetlb_cgrp_subsys。
 * 出参/返回：s 为 NULL 时返回 NULL，否则返回包含该 css 的 hugetlb_cgroup 借用指针。
 * 注意事项：container_of 只做地址换算，不验证子系统类型、不增加引用且不允许睡眠；
 * 调用者原先用于稳定 s 的 RCU、css 引用或 cgroup 生命周期保证同样约束返回值。
 */
static inline
struct hugetlb_cgroup *hugetlb_cgroup_from_css(struct cgroup_subsys_state *s)
{
	return s ? container_of(s, struct hugetlb_cgroup, css) : NULL;
}

/*
 * 业务背景：分配按 current 所属 cgroup 计费。入参：task 是调用者稳定的任务借用指针。
 * 出参/返回：返回该任务当前 hugetlb css 对应的借用对象。注意事项：task_css() 的
 * 返回仍需由外层 RCU 与 css_tryget() 转化为可跨越调度点的引用。
 */
/*
 * 业务背景：HugeTLB 新分配默认向发起任务所属 cgroup 计费，本函数完成 task 到控制器
 * 私有 css 的查找，是 __hugetlb_cgroup_charge_cgroup() 重试协议的一部分。
 * 入参：task 是非空借用任务指针且只作输入；调用者负责以 RCU 或任务生命周期稳定它。
 * 出参/返回：返回 task 当前 hugetlb css 对应的借用 hugetlb_cgroup；无引用转移。
 * 注意事项：任务可并发迁移到别的 cgroup，返回值只在 task_css() 的保护窗口内可靠；
 * 需要跨越 RCU 临界区时必须 css_tryget()，失败后应重新查找而不是继续使用旧地址。
 */
static inline
struct hugetlb_cgroup *hugetlb_cgroup_from_task(struct task_struct *task)
{
	return hugetlb_cgroup_from_css(task_css(task, hugetlb_cgrp_id));
}

/* 根 cgroup 是层级记账的终点，根本身不受 max 限制。 */
/*
 * 业务背景：写 limit、事件传播和 offline reparent 都需要识别层级根以停止向上遍历。
 * 入参：h_cg 是可为 NULL 的借用指针，只作身份比较；不读取其字段。
 * 出参/返回：仅当 h_cg 与已发布 root_h_cgroup 相同才返回 true；无输出和副作用。
 * 注意事项：无锁且不睡眠；只比较地址不取得引用，调用者仍需保证其后若解引用 h_cg 时对象有效。
 */
static inline bool hugetlb_cgroup_is_root(struct hugetlb_cgroup *h_cg)
{
	return (h_cg == root_h_cgroup);
}

/* 从 css 父链接恢复上一级 hugetlb cgroup；返回值只借用父 css 的生命周期。 */
/*
 * 业务背景：层级事件传播和 css offline 需要沿 cgroup 父链寻找下一记账对象。
 * 入参：h_cg 是非空借用子 cgroup，只作输入；其 css.parent 可在根节点为 NULL。
 * 出参/返回：返回父 hugetlb_cgroup 的借用指针，根节点返回 NULL；不增加父 css 引用。
 * 注意事项：无锁且不睡眠；调用者必须处在 cgroup 保证父链稳定的回调、RCU 临界区或持引用环境。
 */
static inline struct hugetlb_cgroup *
parent_hugetlb_cgroup(struct hugetlb_cgroup *h_cg)
{
	return hugetlb_cgroup_from_css(h_cg->css.parent);
}

/*
 * 离线前循环检查所有 hstate 是否仍有实际页占用；reservation 不会在此迁移。
 * 读 page_counter 是快照，离线流程随后在 hugetlb_lock 下重试迁移来收敛该状态。
 */
/*
 * 业务背景：css_offline 扫描 active list 后用它判断是否仍需下一轮，把实际 HugeTLB folio
 * 全部上移到父 cgroup，避免释放仍被 folio 引用的 css。
 * 入参：h_cg 是正在离线且由 cgroup 核心稳定的非空借用对象。
 * 出参/返回：任一 hstate 的 actual page_counter 非零返回 true，全部为零返回 false；
 * 不检查 reservation，不修改任何对象。
 * 注意事项：读取是并发快照且函数不持 hugetlb_lock、不睡眠；true 只要求继续收敛，false
 * 依赖 offline 已阻止新 charge 的上层协议，不能作为一般运行期的永久空闲保证。
 */
static inline bool hugetlb_cgroup_have_usage(struct hugetlb_cgroup *h_cg)
{
	struct hstate *h;

	for_each_hstate(h) {
		if (page_counter_read(
		    hugetlb_cgroup_counter_from_cgroup(h_cg, hstate_index(h))))
			return true;
	}
	return false;
}

/*
 * 业务背景：每个 cgroup、每种 hugepage 尺寸都有 usage/rsvd 两棵 page_counter
 * 层级树，子树 charge 会向父级传播。入参：h_cgroup 是刚分配且未发布的对象，
 * parent_h_cgroup 是可空的父对象。出参/返回：无直接返回；初始化全部计数器和上限。
 * 注意事项：仅创建阶段调用，可睡眠；上限必须按该 hstate 的页数向下对齐，避免半个 HugeTLB 页配额。
 */
/*
 * 业务背景：css_alloc 得到零填充对象后，必须先把每种 hstate 的 actual/rsvd 计数器接入
 * 父层级，后续 page_counter charge 才能自动同时约束本组和祖先上限。
 * 入参：h_cgroup 是非空、未发布、由调用者独占的新对象；parent_h_cgroup 是可为 NULL
 * 的借用父对象，NULL 仅用于根 cgroup；两个参数都只在调用期间使用。
 * 出参/返回：无直接返回值和输出参数；成功初始化全部计数器、父指针、v1 failcnt 策略及
 * 对齐后的无限上限，h_cgroup 的 ownership 仍归调用者。
 * 注意事项：创建路径可睡眠，但本函数自身只初始化内存；不得对已发布或已有 charge 的对象
 * 重复调用，否则会破坏层级计数。父对象生命周期由 cgroup 创建次序保证。
 */
static void hugetlb_cgroup_init(struct hugetlb_cgroup *h_cgroup,
				struct hugetlb_cgroup *parent_h_cgroup)
{
	/* idx 是正在初始化的 hstate 槽；循环结束后不再有单一槽位含义。 */
	int idx;

	/* 每一槽位独立连接父计数器，未启用的 hstate 也先保持一致布局。 */
	for (idx = 0; idx < HUGE_MAX_HSTATE; idx++) {
		struct page_counter *fault, *fault_parent = NULL;
		struct page_counter *rsvd, *rsvd_parent = NULL;
		unsigned long limit;
		/*
		 * fault/rsvd 是新对象当前槽的 actual/预留计数器；两个 *_parent 是可空父槽；
		 * limit 以基础页计，保存按当前 hstate 整页对齐后的内部“无限”值。
		 */

		/* root 没有父计数器；非 root 的 charge 需自动传播到父级。 */
		if (parent_h_cgroup) {
			fault_parent = hugetlb_cgroup_counter_from_cgroup(
				parent_h_cgroup, idx);
			rsvd_parent = hugetlb_cgroup_counter_from_cgroup_rsvd(
				parent_h_cgroup, idx);
		}
		fault = hugetlb_cgroup_counter_from_cgroup(h_cgroup, idx);
		rsvd = hugetlb_cgroup_counter_from_cgroup_rsvd(h_cgroup, idx);

		/* fault 表示已分配页，rsvd 表示未来可能兑现的 reservation。 */
		page_counter_init(fault, fault_parent, false);
		page_counter_init(rsvd, rsvd_parent, false);

		if (!cgroup_subsys_on_dfl(hugetlb_cgrp_subsys)) {
			/* v1 暴露 failcnt，故两个计数器都必须累计拒绝次数。 */
			fault->track_failcnt = true;
			rsvd->track_failcnt = true;
		}

		/* PAGE_COUNTER_MAX 也需对齐，确保无限制值能代表完整 HugeTLB 页数。 */
		limit = round_down(PAGE_COUNTER_MAX,
				   pages_per_huge_page(&hstates[idx]));

		VM_BUG_ON(page_counter_set_max(fault, limit));
		VM_BUG_ON(page_counter_set_max(rsvd, limit));
	}
}

/*
 * 释放尚未或已经从 cgroup 核心摘除的私有对象。
 * 入参：h_cgroup 的所有权已转给 css free 回调。出参/返回：无。
 * 注意事项：先逐节点释放柔性数组指向的 nodeinfo，再释放容器；调用时不得仍有读者借用它。
 */
/*
 * 业务背景：css 分配可能在任意 NUMA 槽失败，正常 css 销毁也有同一资源集合，因此用统一
 * 析构器完成“各 nodeinfo → 外层柔性数组容器”的逆序释放。
 * 入参：h_cgroup 是非空、由调用者交出最终 ownership 的对象；允许部分 nodeinfo 为 NULL。
 * 出参/返回：无直接返回值和输出参数；所有已分配 nodeinfo 与 h_cgroup 本体被释放。
 * 注意事项：kfree 不要求本函数持锁且本函数不主动睡眠；调用后任何裸指针都失效，正常路径
 * 必须等 cgroup 核心确认 css 引用耗尽，失败路径则必须保证对象从未发布。
 */
static void hugetlb_cgroup_free(struct hugetlb_cgroup *h_cgroup)
{
	/* node 是要释放的 NUMA 槽下标；NULL nodeinfo 允许 kfree() 安全跳过。 */
	int node;

	for_each_node(node)
		kfree(h_cgroup->nodeinfo[node]);
	kfree(h_cgroup);
}

/*
 * 业务背景：cgroup 创建路径调用本函数，建立一个可被随后 charge/read 回调使用的
 * hugetlb 子系统状态。入参：parent_css 是可空借用父 css。出参/返回：成功转移
 * h_cgroup 所有权给 cgroup 核心并返回 &css；失败返回 ERR_PTR(-ENOMEM) 且已回收所有 nodeinfo。
 * 注意事项：kzalloc 可睡眠；root 的首次创建发布 root_h_cgroup，失败路径不能遗留部分节点数组。
 */
/*
 * 业务背景：这是 cgroup 核心的 css_alloc 回调，为新层级节点建立 page_counter、事件槽和
 * 每 NUMA 节点 usage 存储；后续 charge/read/offline 回调都以返回的 css 为入口。
 * 入参：parent_css 是纯输入借用指针，根 cgroup 创建时为 NULL，否则必须属于 hugetlb 控制器。
 * 出参/返回：成功返回新对象内嵌 css，ownership 转给 cgroup 核心；任一分配失败返回
 * ERR_PTR(-ENOMEM)，已分配的节点对象和容器均已释放，无部分 css 对外可见。
 * 注意事项：GFP_KERNEL 分配可睡眠，不能在原子上下文调用；root_h_cgroup 的赋值发生在
 * nodeinfo 分配之前，但根 css 创建失败由上层视为控制器初始化失败，不能把该暂存指针当作成功发布。
 */
static struct cgroup_subsys_state *
hugetlb_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct hugetlb_cgroup *parent_h_cgroup = hugetlb_cgroup_from_css(parent_css);
	struct hugetlb_cgroup *h_cgroup;
	int node;
	/*
	 * parent_h_cgroup 是不持新引用的父私有对象；h_cgroup 是本函数创建、发布前独占的候选；
	 * node 依次标识必须具备统计槽的全部可能 NUMA 节点。
	 */

	/* 柔性数组保存每个 nid 的指针，节点对象另行按本地节点分配。 */
	h_cgroup = kzalloc_flex(*h_cgroup, nodeinfo, nr_node_ids);

	if (!h_cgroup)
		return ERR_PTR(-ENOMEM);

	if (!parent_h_cgroup)
		root_h_cgroup = h_cgroup;

	/*
	 * TODO: this routine can waste much memory for nodes which will
	 * never be onlined. It's better to use memory hotplug callback
	 * function.
	 */
	/*
	 * 补充说明：原注释说明离线节点会浪费对象；当前实现仍在创建时遍历全部 nid，
	 * 只把分配的 NUMA 偏好改为 NUMA_NO_NODE，故热插拔节点也已有空统计槽。
	 */
	for_each_node(node) {
		/* nodeinfo 仅保存 NUMA usage；NULL 条目由 kzalloc 失败路径统一处理。 */
		/* Set node_to_alloc to NUMA_NO_NODE for offline nodes. */
		/* 补充说明：离线节点没有本地 normal memory，故不能把分配策略绑定到该 nid。 */
		int node_to_alloc =
			node_state(node, N_NORMAL_MEMORY) ? node : NUMA_NO_NODE;
		h_cgroup->nodeinfo[node] =
			kzalloc_node(sizeof(struct hugetlb_cgroup_per_node),
				     GFP_KERNEL, node_to_alloc);
		if (!h_cgroup->nodeinfo[node])
			/* 任何节点失败都不能发布一个缺失统计槽的 cgroup。 */
			goto fail_alloc_nodeinfo;
	}

	hugetlb_cgroup_init(h_cgroup, parent_h_cgroup);
	return &h_cgroup->css;

fail_alloc_nodeinfo:
	/* 当前只拥有尚未发布的 h_cgroup；统一析构已成功分配的前缀节点。 */
	hugetlb_cgroup_free(h_cgroup);
	return ERR_PTR(-ENOMEM);
}

/* css 核心确认没有引用后调用；把其拥有的私有容器交给统一析构。 */
/*
 * 业务背景：这是 css 生命周期的最终 free 回调，与 css_alloc 成功时的 ownership 转移配对。
 * 入参：css 是非空、已 offline 且引用归零的 hugetlb css，ownership 由 cgroup 核心交入。
 * 出参/返回：无直接返回值和输出参数；私有 nodeinfo 与外层 hugetlb_cgroup 均被释放。
 * 注意事项：不得在此之前释放仍由 folio/reservation 引用的 css；返回后 css 地址彻底失效，
 * 本函数不取得 hugetlb_lock，安全性来自 cgroup 引用计数和先行 offline 收敛。
 */
static void hugetlb_cgroup_css_free(struct cgroup_subsys_state *css)
{
	hugetlb_cgroup_free(hugetlb_cgroup_from_css(css));
}

/*
 * Should be called with hugetlb_lock held.
 * Since we are holding hugetlb_lock, pages cannot get moved from
 * active list or uncharged from the cgroup, So no need to get
 * page reference and test for page active here. This function
 * cannot fail.
 */
/*
 * 补充说明：hugetlb_lock 同时串行化 active list 和 folio 的 cgroup 指针，
 * 因而离线迁移无需额外 folio 引用；如果无此锁，迁移/释放可在检查后摘除 folio。
 */
/*
 * 业务背景：css_offline 不能销毁仍被实际 HugeTLB folio 指向的 cgroup，本函数把一个匹配
 * folio 的 actual charge 及归属上移到父级，是 active-list 扫描的单页状态转换点。
 * 入参：idx 是 [0, HUGE_MAX_HSTATE) 的 hstate 下标；h_cg 是正在离线的非空借用对象；
 * folio 是 active list 上的借用 HugeTLB folio，三者均为纯输入句柄。
 * 出参/返回：无直接返回值；匹配时撤销 h_cg 本地计数并把 folio 归属改为父级；没有归属或
 * 已不属于 h_cg 时无副作用。若 h_cg 没有父级，先向无上限 root 增加相同基础页数。
 * 注意事项：调用者必须持 hugetlb_lock 且本函数不可睡眠；锁同时稳定 active list、folio
 * cgroup 字段和离线迁移，故不另取 folio/css 引用。nr_pages 单位为基础页而非 HugeTLB 页个数。
 */
static void hugetlb_cgroup_move_parent(int idx, struct hugetlb_cgroup *h_cg,
				       struct folio *folio)
{
	unsigned int nr_pages;
	struct page_counter *counter;
	struct hugetlb_cgroup *hcg;
	struct hugetlb_cgroup *parent = parent_hugetlb_cgroup(h_cg);
	/*
	 * hcg 是从 folio 读取的当前实际归属；parent 是目标父归属；counter 是仅做本地扣减的
	 * 原 cgroup actual 计数器；nr_pages 以基础页计并覆盖整个 folio。
	 */

	/* 只迁移仍归本 css 的 folio，避免重复扫描或其它路径已重新归属的页。 */
	hcg = hugetlb_cgroup_from_folio(folio);
	/*
	 * We can have pages in active list without any cgroup
	 * ie, hugepage with less than 3 pages. We can safely
	 * ignore those pages.
	 */
	/*
	 * active list 中允许存在没有任何 cgroup 归属的页，例如少于三个基础页的 hugepage；
	 * 这类 folio 没有可迁移的计费状态，可以安全跳过。hcg 属于别组时也不能替它重归属。
	 */
	if (!hcg || hcg != h_cg)
		goto out;

	/* 一个 HugeTLB folio 按基础页计费，必须整体从旧计数器移走。 */
	nr_pages = folio_nr_pages(folio);
	if (!parent) {
		/* 删除非 root css 时，顶层页改归 root；root 无 max，charge 必然可接受。 */
		parent = root_h_cgroup;
		/* root has no limit */
		/* 根 cgroup 没有配额上限，因此把孤立的顶层 charge 接到 root 不会失败。 */
		page_counter_charge(&parent->hugepage[idx], nr_pages);
	}
	/* 先把层级计数的责任移到 parent，再撤销本地计数并发布新的 folio 归属。 */
	counter = &h_cg->hugepage[idx];
	/* Take the pages off the local counter */
	/* 只从正在离线 cgroup 的本地计数扣除这些基础页；祖先原已包含该 charge，必须保持不变。 */
	page_counter_cancel(counter, nr_pages);

	set_hugetlb_cgroup(folio, parent);
out:
	return;
}

/*
 * Force the hugetlb cgroup to empty the hugetlb resources by moving them to
 * the parent cgroup.
 */
/*
 * 补充说明：offline 是 cgroup rmdir 的资源收敛阶段；每轮在 hugetlb_lock 下扫描
 * 每种 hstate 的 active list 并把属于 h_cg 的 folio 上移。cond_resched() 允许
 * 长列表让出 CPU，循环条件应对并发释放/迁移后的剩余 usage，而不是假设一次扫描足够。
 */
/*
 * 业务背景：这是 cgroup 核心的 css_offline 回调；在最终 free 前把 actual folio 归属逐个
 * 上移，保证后续没有 folio 裸指针指向被销毁的 hugetlb_cgroup。
 * 入参：css 是正在离线、由 cgroup 核心稳定的非空借用 hugetlb css。
 * 出参/返回：无直接返回值；成功返回时所有 hstate 的 actual usage 已收敛为零，folio 已归父级；
 * reservation 不在本路径迁移，仍由其 css 引用和 resv_map/file_region 生命周期负责。
 * 注意事项：每次扫描持 hugetlb_lock 并关闭本地中断，锁外 cond_resched() 表明整个回调可睡眠；
 * 禁止在锁内调度。do/while 处理扫描期间的状态变化，退出依赖 offline 阻止新 charge 的上层保证。
 */
static void hugetlb_cgroup_css_offline(struct cgroup_subsys_state *css)
{
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_css(css);
	struct hstate *h;
	struct folio *folio;
	/* h_cg 是离线目标；h/folio 分别是当前 hstate 与其 active-list 扫描游标，均只在循环内借用。 */

	/* 使用 usage 作为完成判据，直到所有实际页都不再记在将离线的 css。 */
	do {
		for_each_hstate(h) {
			spin_lock_irq(&hugetlb_lock);
			list_for_each_entry(folio, &h->hugepage_activelist, lru)
				hugetlb_cgroup_move_parent(hstate_index(h), h_cg, folio);

			spin_unlock_irq(&hugetlb_lock);
		}
		cond_resched();
	} while (hugetlb_cgroup_have_usage(h_cg));
}

/*
 * 业务背景：cgroup 文件 events/events.local 需要同时保留本 css 发生的上限失败
 * 和对祖先可见的层级总数。入参：hugetlb 是已稳定的起点 css，idx 是 hstate，event
 * 是 HUGETLB_MAX 等事件枚举。出参/返回：无；原子增加计数并唤醒对应 kernfs 观察者。
 * 注意事项：沿 parent 链只累计到非 root；atomic 保证计数更新而非 css 生命周期，调用者必须持有有效 css。
 */
/*
 * 业务背景：page_counter 拒绝 charge 时，用户态既要看到本组失败，也要从祖先 events 文件
 * 看到子树累计失败；本函数完成计数传播和 kernfs poll 唤醒。
 * 入参：hugetlb 是非空、生命周期已稳定的起始 cgroup；idx 是合法 hstate 下标；event 是
 * [0, HUGETLB_NR_MEMORY_EVENTS) 的事件枚举，目前典型值 HUGETLB_MAX 表示超过 max。
 * 出参/返回：无直接返回值；events_local 只增加起点一次，events 从起点逐级增加至 root 之前，
 * 每次更新后通知对应 cgroup_file；不转移任何引用。
 * 注意事项：原子计数允许并发且本函数不睡眠，但 parent 链生命周期不由 atomic 保证；必须由
 * charge 持有的 css 引用及 cgroup 层级规则稳定。root 不暴露这些文件，故传播在 root 前停止。
 */
static inline void hugetlb_event(struct hugetlb_cgroup *hugetlb, int idx,
				 enum hugetlb_memory_event event)
{
	/* local 只记本组，events 则随后向每层祖先传播。 */
	atomic_long_inc(&hugetlb->events_local[idx][event]);
	cgroup_file_notify(&hugetlb->events_local_file[idx]);

	do {
		atomic_long_inc(&hugetlb->events[idx][event]);
		cgroup_file_notify(&hugetlb->events_file[idx]);
	} while ((hugetlb = parent_hugetlb_cgroup(hugetlb)) &&
		 !hugetlb_cgroup_is_root(hugetlb));
}

/*
 * 业务背景：HugeTLB 分配/预留入口先在 current 的 cgroup 试扣配额，后续 commit
 * 才会把实际 folio 绑定到该 css。入参：idx 为 hstate，nr_pages 是基础页数，ptr 为
 * 输出 cgroup 指针，rsvd 选择预留计数。出参/返回：0 表示已 charge 并写入 *ptr；
 * -ENOMEM 表示超过上限；禁用控制器时 *ptr 为 NULL。注意事项：RCU 仅稳定 task css
 * 查找，css_tryget 成功后才可跨越 RCU；reservation 保留该引用，实际页 charge 不保留。
 */
/*
 * 业务背景：这是“试扣配额 → 分配 folio/建立预留 → commit 或 rollback”事务的第一阶段，
 * 先把基础页数计入 current 所属层级，避免实际资源成功后才发现已超过 cgroup 上限。
 * 入参：idx 是合法 hstate 下标；nr_pages 是非零基础页数；ptr 是非空输出槽，调用前内容
 * 不被读取；rsvd 为 true 选择 reservation counter 并保留 css 引用，为 false 选择 actual counter。
 * 出参/返回：返回 0 表示 charge 成功，*ptr 为后续 commit/rollback 使用的 cgroup；控制器
 * 禁用时也返回 0 但 *ptr 为 NULL。超过任一层级上限返回 -ENOMEM；失败时调用者不得解引用
 * *ptr，其内容仅是内部最后一次查找值且对应 css 引用已经释放。
 * 注意事项：RCU 读侧不可睡眠，page_counter_try_charge/事件通知路径也必须适用于当前上下文；
 * css_tryget 失败表示对象正在离线，必须退出 RCU 后重试。成功 actual charge 当场 css_put，
 * 依靠 folio 可在 offline 时 reparent；成功 reservation 将引用交给后续预留对象，必须精确归还。
 */
static int __hugetlb_cgroup_charge_cgroup(int idx, unsigned long nr_pages,
					  struct hugetlb_cgroup **ptr,
					  bool rsvd)
{
	int ret = 0;
	struct page_counter *counter;
	struct hugetlb_cgroup *h_cg = NULL;
	/*
	 * ret 保存最终 errno；counter 仅在 try_charge 失败时指出触顶层供 page_counter 内部语义使用；
	 * h_cg 是 RCU 下查到并尝试升级为 css 引用的候选，只有成功返回时 *ptr 才可使用它。
	 */

	if (hugetlb_cgroup_disabled())
		goto done;

again:
	/* task 的 css 可并发迁移/离线；tryget 失败必须重新从 current 查，而不能使用旧裸指针。 */
	rcu_read_lock();
	h_cg = hugetlb_cgroup_from_task(current);
	if (!css_tryget(&h_cg->css)) {
		rcu_read_unlock();
		goto again;
	}
	rcu_read_unlock();

	/* page_counter 会沿父链检查并原子增加；失败时没有 folio 所有权可提交。 */
	if (!page_counter_try_charge(
		    __hugetlb_cgroup_counter_from_cgroup(h_cg, idx, rsvd),
		    nr_pages, &counter)) {
		/* counter 表示实际拒绝 charge 的层级节点；本文件只需归因到起始 h_cg 的 max 事件。 */
		ret = -ENOMEM;
		hugetlb_event(h_cg, idx, HUGETLB_MAX);
		css_put(&h_cg->css);
		goto done;
	}
	/* Reservations take a reference to the css because they do not get
	 * reparented.
	 */
	/*
	 * 补充说明：真实 folio 在 css offline 时可被上移，因此无需长期 css 引用；
	 * reservation 没有 folio 可迁移，必须由 resv_map 持有引用直到 uncharge。
	 */
	if (!rsvd)
		css_put(&h_cg->css);
done:
	*ptr = h_cg;
	return ret;
}

/* 实际分配入口的公开包装：不设置 rsvd，成功后调用者应 commit 或 cancel 这次 charge。 */
/*
 * 业务背景：HugeTLB 实际 folio 分配在取页前调用此包装，建立可回滚的 actual 配额试扣。
 * 入参：idx 为合法 hstate 下标；nr_pages 为基础页数；ptr 为非空输出槽且调用前内容无要求。
 * 出参/返回：语义完全继承内部函数：0 表示已试扣或控制器禁用，-ENOMEM 表示上限拒绝；
 * 仅在成功且 *ptr 非 NULL 时可把它传给 commit/uncharge_cgroup，返回值本身不转移长期 css 引用。
 * 注意事项：失败时不得使用 *ptr；成功后必须且只能选择 commit 到 folio 或按同样页数 rollback 一次。
 */
int hugetlb_cgroup_charge_cgroup(int idx, unsigned long nr_pages,
				 struct hugetlb_cgroup **ptr)
{
	return __hugetlb_cgroup_charge_cgroup(idx, nr_pages, ptr, false);
}

/* reservation 入口的公开包装：成功返回的 *ptr 隐含一份必须由 uncharge 归还的 css 引用。 */
/*
 * 业务背景：文件 reservation 在尚无 folio 时也要限制承诺量，本包装对预留计数器执行试扣。
 * 入参：idx 为合法 hstate 下标；nr_pages 为基础页数；ptr 为非空输出槽。
 * 出参/返回：0 表示成功或控制器禁用，-ENOMEM 表示层级上限拒绝；成功且 *ptr 非 NULL 时，
 * 输出对象连同一份 css 引用转交给 reservation 生命周期。
 * 注意事项：失败时 *ptr 不可使用；成功后必须由 commit 后的 folio、resv_map/file_region，
 * 或 hugetlb_cgroup_uncharge_cgroup_rsvd() 中恰好一条路径归还 charge 与 css 引用。
 */
int hugetlb_cgroup_charge_cgroup_rsvd(int idx, unsigned long nr_pages,
				      struct hugetlb_cgroup **ptr)
{
	return __hugetlb_cgroup_charge_cgroup(idx, nr_pages, ptr, true);
}

/* Should be called with hugetlb_lock held */
/*
 * 补充说明：commit 是“计数已成功”到“folio 可查到归属”的发布边界；持锁可防止
 * cgroup offline、folio migration 和 NUMA usage 更新看到半完成的绑定。
 */
/*
 * 业务背景：charge 只更新匿名层级计数，本函数把已成功的 charge 绑定到具体 folio，使释放、
 * migration 和 css offline 能从 folio 找回责任 cgroup。
 * 入参：idx 为 folio hstate 下标；nr_pages 为与原 charge 相同的基础页数；h_cg 是 charge
 * 返回的借用对象，控制器禁用时可为 NULL；folio 是非空、尚未携带该类归属的 HugeTLB folio；
 * rsvd 选择 actual 或 reservation 字段，所有参数均只作输入句柄。
 * 出参/返回：无直接返回；有效时把 h_cg 发布到 folio。actual 分支还把 nr_pages 加到 folio
 * 所在 nid 的非层级 usage；reservation 分支不改变 NUMA usage，既有 css 引用继续随预留转移。
 * 注意事项：调用者必须持 hugetlb_lock，本函数不睡眠；idx/nr_pages/rsvd 必须与试扣完全一致，
 * folio 归属只能提交一次。控制器禁用或 h_cg 为 NULL 时无副作用。
 */
static void __hugetlb_cgroup_commit_charge(int idx, unsigned long nr_pages,
					   struct hugetlb_cgroup *h_cg,
					   struct folio *folio, bool rsvd)
{
	if (hugetlb_cgroup_disabled() || !h_cg)
		return;
	lockdep_assert_held(&hugetlb_lock);
	/* 先绑定归属；实际页再更新 nodeinfo，reservation 不拥有某个 NUMA folio。 */
	__set_hugetlb_cgroup(folio, h_cg, rsvd);
	if (!rsvd) {
		unsigned long usage =
			h_cg->nodeinfo[folio_nid(folio)]->usage[idx];
		/* usage 是锁内读取的旧 NUMA 基础页数，只在本次 read-modify-write 期间有效。 */
		/*
		 * This write is not atomic due to fetching usage and writing
		 * to it, but that's fine because we call this with
		 * hugetlb_lock held anyway.
		 */
		/*
		 * 补充说明：hugetlb_lock 是读取和写入 usage 的共同同步，WRITE_ONCE 防止
		 * 编译器拆分/合并访问；它不替代该锁的互斥语义。
		 */
		WRITE_ONCE(h_cg->nodeinfo[folio_nid(folio)]->usage[idx],
			   usage + nr_pages);
	}
}

/* 将已 charge 的实际 folio 归属提交给 h_cg；无返回值，调用者仍持有 hugetlb_lock。 */
/*
 * 业务背景：这是实际 folio 分配成功后的公开提交点，把此前的 actual 试扣变成可由 folio 追踪的归属。
 * 入参：idx 为 hstate 下标；nr_pages 为基础页数且与 charge 相同；h_cg 为成功 charge 的借用结果；
 * folio 为非空、新分配且尚未绑定 actual cgroup 的 HugeTLB folio。
 * 出参/返回：无直接返回；folio 获得 actual cgroup 指针，对应 nid usage 增加；不新增 css 引用。
 * 注意事项：必须持 hugetlb_lock、不得睡眠且只能提交一次；失败分配应改走 uncharge_cgroup 而非本函数。
 */
void hugetlb_cgroup_commit_charge(int idx, unsigned long nr_pages,
				  struct hugetlb_cgroup *h_cg,
				  struct folio *folio)
{
	__hugetlb_cgroup_commit_charge(idx, nr_pages, h_cg, folio, false);
}

/* 将已 charge 的 reservation 记录在 folio；rsvd css 引用继续由 reservation 生命周期持有。 */
/*
 * 业务背景：reservation 兑现或暂附于 folio 时，用此入口发布其独立计费归属，供后续撤销时找到 counter。
 * 入参：idx、nr_pages 必须与预留 charge 相同；h_cg 是携带一份 css 引用的成功输出；folio 是
 * 非空 HugeTLB folio，且其 reservation cgroup 字段尚未设置。
 * 出参/返回：无直接返回；只设置 folio 的 rsvd 归属，不改变 NUMA usage，也不消费长期 css 引用。
 * 注意事项：调用者必须持 hugetlb_lock且只能提交一次；该引用最终由 rsvd uncharge 路径 css_put()。
 */
void hugetlb_cgroup_commit_charge_rsvd(int idx, unsigned long nr_pages,
				       struct hugetlb_cgroup *h_cg,
				       struct folio *folio)
{
	__hugetlb_cgroup_commit_charge(idx, nr_pages, h_cg, folio, true);
}

/*
 * Should be called with hugetlb_lock held
 */
/*
 * 补充说明：uncharge 是 commit 的逆操作，先从 folio 摘除可阻止同一 folio 被再次
 * 归还；随后才撤销层级计数并在 reservation 情形归还 css 引用。
 */
/*
 * 业务背景：HugeTLB folio 释放或 reservation 撤销时，必须同时清除 folio 归属和层级计数，
 * 否则会造成重复 uncharge、css 泄漏或永久虚高的配额占用。
 * 入参：idx 为 folio hstate 下标；nr_pages 为原 charge 的基础页数；folio 是非空借用
 * HugeTLB folio；rsvd 选择要清除的归属字段。
 * 出参/返回：无直接返回；无归属或控制器禁用时无副作用。有效时先摘除 folio 指针，再撤销
 * page_counter；reservation 额外释放一份 css 引用，actual 则减少对应 nid usage。
 * 注意事项：必须持 hugetlb_lock 且本函数不睡眠；锁保护“读取归属—清指针—改 NUMA usage”
 * 原子阶段。idx/nr_pages 必须与 commit 配对，否则会破坏层级计数和按节点统计。
 */
static void __hugetlb_cgroup_uncharge_folio(int idx, unsigned long nr_pages,
					   struct folio *folio, bool rsvd)
{
	struct hugetlb_cgroup *h_cg;

	if (hugetlb_cgroup_disabled())
		return;
	lockdep_assert_held(&hugetlb_lock);
	/* 空归属表示该页在控制器关闭或未计费时创建，无需再触及 counter。 */
	h_cg = __hugetlb_cgroup_from_folio(folio, rsvd);
	if (unlikely(!h_cg))
		return;
	/* 先清除 folio 指针，确保后续并发释放不会再次把同一配额归还。 */
	__set_hugetlb_cgroup(folio, NULL, rsvd);

	page_counter_uncharge(__hugetlb_cgroup_counter_from_cgroup(h_cg, idx,
								   rsvd),
			      nr_pages);

	/* reservation 的长期引用在这里终结；实际页则维护按 nid 的非层级快照。 */
	if (rsvd)
		css_put(&h_cg->css);
	else {
		unsigned long usage =
			h_cg->nodeinfo[folio_nid(folio)]->usage[idx];
		/* usage 是 actual 分支当前 nid 的旧基础页数，锁内减去整个 folio 后立即失效。 */
		/*
		 * This write is not atomic due to fetching usage and writing
		 * to it, but that's fine because we call this with
		 * hugetlb_lock held anyway.
		 */
		/*
		 * 读取后再写回不是原子 RMW，但所有同字段更新都在 hugetlb_lock 下，因而不会丢失并发减量；
		 * READ/WRITE_ONCE 只约束单次编译器访问，互斥性仍完全来自这把锁。
		 */
		WRITE_ONCE(h_cg->nodeinfo[folio_nid(folio)]->usage[idx],
			   usage - nr_pages);
	}
}

/* 释放实际 folio 的 usage 计费和 NUMA usage；调用者必须持有 hugetlb_lock。 */
/*
 * 业务背景：实际 HugeTLB folio 离开池或销毁时调用，完成 commit_charge(actual) 的逆操作。
 * 入参：idx 为 hstate 下标；nr_pages 为原 charge 的基础页数；folio 为非空借用 HugeTLB folio。
 * 出参/返回：无直接返回；清除 actual 归属、撤销层级计数并减少 folio nid usage；无归属时无副作用。
 * 注意事项：调用者必须持 hugetlb_lock、不得重复调用；本包装不释放 css 引用，因为 actual 未长期持有它。
 */
void hugetlb_cgroup_uncharge_folio(int idx, unsigned long nr_pages,
				  struct folio *folio)
{
	__hugetlb_cgroup_uncharge_folio(idx, nr_pages, folio, false);
}

/* 释放 folio 携带的 reservation 计费；对应 css_put 只在内部 rsvd 路径执行。 */
/*
 * 业务背景：folio 上的 reservation 责任结束时调用，完成 commit_charge(rsvd) 的逆操作。
 * 入参：idx 为 hstate 下标；nr_pages 为原预留 charge 的基础页数；folio 为非空借用 HugeTLB folio。
 * 出参/返回：无直接返回；清除 rsvd 归属、撤销预留计数并释放其长期 css 引用；无归属时无副作用。
 * 注意事项：必须持 hugetlb_lock且只能与一次成功提交配对；不修改 actual 归属或 NUMA usage。
 */
void hugetlb_cgroup_uncharge_folio_rsvd(int idx, unsigned long nr_pages,
				       struct folio *folio)
{
	__hugetlb_cgroup_uncharge_folio(idx, nr_pages, folio, true);
}

/*
 * 回滚尚未绑定 folio 的 charge。idx/nr_pages 必须与成功 charge 完全配对；h_cg 是
 * 借用对象，rsvd 表示要在撤销计数后归还其长期 css 引用。无返回值，也不修改 NUMA usage。
 */
/*
 * 业务背景：试扣成功但后续 folio/reservation 构造失败时，没有 folio 可承载归属，必须直接
 * 对 cgroup counter 回滚；这是 charge 阶段尚未跨越 commit 发布点的失败出口。
 * 入参：idx 为合法 hstate 下标；nr_pages 为与试扣相同的基础页数；h_cg 为成功输出且在
 * 控制器禁用场景可为 NULL；rsvd 选择 actual 或 reservation counter。
 * 出参/返回：无直接返回；有效时撤销相同页数的层级计数，reservation 还释放 charge 时取得的
 * css 引用；不读取或修改任何 folio、NUMA usage 或事件计数。
 * 注意事项：不要求 hugetlb_lock且本函数不睡眠，但只能对未 commit 的 charge 调用一次；
 * 已绑定 folio 的状态必须走 uncharge_folio，否则 folio 会保留悬空归属。
 */
static void __hugetlb_cgroup_uncharge_cgroup(int idx, unsigned long nr_pages,
					     struct hugetlb_cgroup *h_cg,
					     bool rsvd)
{
	if (hugetlb_cgroup_disabled() || !h_cg)
		return;

	page_counter_uncharge(__hugetlb_cgroup_counter_from_cgroup(h_cg, idx,
								   rsvd),
			      nr_pages);

	/* 此回滚没有 folio，只有 reservation 才需要释放 charge 时取得的 css。 */
	if (rsvd)
		css_put(&h_cg->css);
}

/* 实际页 charge 的公开回滚接口；不涉及 css_put。 */
/*
 * 业务背景：actual 试扣后的资源分配失败由此回滚，调用链回到分配者的错误清理阶段。
 * 入参：idx、nr_pages 必须与成功 charge 一致；h_cg 为其输出且控制器禁用时可为 NULL。
 * 出参/返回：无直接返回；撤销 actual page_counter，不改 folio/NUMA usage且不执行 css_put。
 * 注意事项：只适用于尚未 commit 的一次性回滚；重复调用或页数不匹配会导致计数下溢/失真。
 */
void hugetlb_cgroup_uncharge_cgroup(int idx, unsigned long nr_pages,
				    struct hugetlb_cgroup *h_cg)
{
	__hugetlb_cgroup_uncharge_cgroup(idx, nr_pages, h_cg, false);
}

/* reservation 的公开回滚接口；内部 css_put 与预留成功时 css_tryget 配对。 */
/*
 * 业务背景：reservation 试扣后尚未发布到 folio/resv_map 就失败时，用它同时撤销配额和长期引用。
 * 入参：idx、nr_pages 与成功的 rsvd charge 完全一致；h_cg 为其输出，可在禁用场景为 NULL。
 * 出参/返回：无直接返回；撤销 reservation counter 并 css_put 一次，不触碰 actual/NUMA 状态。
 * 注意事项：只调用一次且不能用于已经把引用转交给 folio、resv_map 或 file_region 的预留。
 */
void hugetlb_cgroup_uncharge_cgroup_rsvd(int idx, unsigned long nr_pages,
					 struct hugetlb_cgroup *h_cg)
{
	__hugetlb_cgroup_uncharge_cgroup(idx, nr_pages, h_cg, true);
}

/*
 * resv_map 区间删除时按 [start,end) 覆盖的 HugeTLB 页归还预留配额。
 * resv 是借用 map；缺少 counter/css 说明从未成功计费。成功后释放该 map 持有的 css 引用。
 */
/*
 * 业务背景：整个 resv_map 的某段预留被删除时，需要按文件 HugeTLB 页区间归还预留 charge，
 * 并在这条销毁路径结束 map 持有的 css 生命周期责任。
 * 入参：resv 是可为 NULL 的借用 map；start/end 是以 HugeTLB 页为单位的半开区间，要求
 * end >= start；函数只读取 map 的 reservation_counter、css 和 pages_per_hpage。
 * 出参/返回：无直接返回；有完整记账信息时从 counter 撤销 (end-start)*pages_per_hpage
 * 个基础页并 css_put 一次；控制器禁用或信息缺失时无副作用。
 * 注意事项：调用者必须保证 map 稳定且该 css 引用确由当前销毁事件拥有；区间或调用次数错误
 * 会造成计数与引用失配。本函数不清空 map 字段，外层必须保证不会再次以同一 ownership 调用。
 */
void hugetlb_cgroup_uncharge_counter(struct resv_map *resv, unsigned long start,
				     unsigned long end)
{
	if (hugetlb_cgroup_disabled() || !resv || !resv->reservation_counter ||
	    !resv->css)
		return;

	page_counter_uncharge(resv->reservation_counter,
			      (end - start) * resv->pages_per_hpage);
	css_put(resv->css);
}

/*
 * file_region 的部分收缩只减少计数；仅完全删除 region 时才能归还它唯一拥有的 css 引用。
 * 这防止剩余范围继续引用已释放的 css，调用者须保证 resv/rg 生命周期稳定。
 */
/*
 * 业务背景：reservation map 在拆分、裁剪或删除 file_region 时按区域保存的独立 counter
 * 归还配额；region 仍存在时必须继续保留其唯一 css 引用。
 * 入参：resv/rg 是可为 NULL 的借用对象；nr_pages 是本次删除的 HugeTLB 页个数且零表示无事；
 * region_del 表示整个 rg 是否被销毁。函数只在 rg 有独立 counter、resv 有换算倍率且 map
 * 本身没有统一 reservation_counter 时处理该区域。
 * 出参/返回：无直接返回；有效时撤销 nr_pages*pages_per_hpage 个基础页；region_del 为 true
 * 时额外 css_put(rg->css)，部分删除则保留引用供剩余 rg 使用。
 * 注意事项：调用者必须稳定两个对象并保证 nr_pages 不超过 region 原 charge；该条件分支区分
 * map 级和 region 级记账，不能同时对同一预留再调用 map 级 uncharge，否则会双重归还。
 */
void hugetlb_cgroup_uncharge_file_region(struct resv_map *resv,
					 struct file_region *rg,
					 unsigned long nr_pages,
					 bool region_del)
{
	if (hugetlb_cgroup_disabled() || !resv || !rg || !nr_pages)
		return;

	if (rg->reservation_counter && resv->pages_per_hpage &&
	    !resv->reservation_counter) {
		/* nr_pages 是文件区间单位，转换为基础页后才与 page_counter 的计量一致。 */
		page_counter_uncharge(rg->reservation_counter,
				      nr_pages * resv->pages_per_hpage);
		/*
		 * Only do css_put(rg->css) when we delete the entire region
		 * because one file_region must hold exactly one css reference.
		 */
		/*
		 * 只有删除整个区域时才 css_put(rg->css)，因为一个 file_region 必须且只持有一份
		 * css 引用；部分裁剪后剩余区域仍需该引用来保证其 reservation_counter 生命周期。
		 */
		if (region_del)
			/* 仅销毁整个 region 时，rg->css 的唯一持有者才消失。 */
			css_put(rg->css);
	}
}

/* cftype.private 的属性字段选择 usage、reservation、limit、峰值或失败计数。 */
enum {
	/* 实际 HugeTLB folio 的当前层级基础页计数，用于 current/usage_in_bytes。 */
	RES_USAGE,
	/* 尚未兑现为实际 folio 的 reservation 当前层级计数，用于 rsvd.current。 */
	RES_RSVD_USAGE,
	/* actual counter 的最大值；写 max/limit_in_bytes 时由用户设置。 */
	RES_LIMIT,
	/* reservation counter 的独立最大值，防止未兑现承诺无限累积。 */
	RES_RSVD_LIMIT,
	/* actual counter 的历史水位，仅 legacy ABI 读取和重置。 */
	RES_MAX_USAGE,
	/* reservation counter 的历史水位，仅 legacy ABI 读取和重置。 */
	RES_RSVD_MAX_USAGE,
	/* actual charge 因上限失败的累计次数，仅 legacy ABI 暴露。 */
	RES_FAILCNT,
	/* reservation charge 因上限失败的累计次数，仅 legacy ABI 暴露。 */
	RES_RSVD_FAILCNT,
};

/*
 * 向 numa_stat 输出当前或层级 HugeTLB usage。seq 提供稳定 css，dummy 未使用，返回 0。
 * v1 先输出非层级快照；子树聚合在 RCU 读侧遍历，READ_ONCE 只保证单次读取，允许并发变化。
 */
/*
 * 业务背景：numa_stat 既要呈现 page_counter 已维护的层级总量，又要补出没有层级 counter 的
 * 每节点子树总量；v1 还兼容输出本 css 的非层级视图。
 * 入参：seq 是非空 seq_file 输入输出对象，关联的 cftype/private 给出 hstate 下标、关联 css
 * 给出统计根；dummy 为回调占位且可为 NULL，不读取也不取得 ownership。
 * 出参/返回：向 seq 追加字节单位的 total 和 N<n> 字段，成功恒返回 0；无独立输出参数，
 * 不修改计数和 css。任一行中的各字段是逐次读取的近似快照而非事务一致快照。
 * 注意事项：legacy 本地统计只读当前对象；层级 NUMA 统计在 RCU 读侧遍历 descendants，期间
 * 不可睡眠且转换出的 css 指针不能带出临界区。READ_ONCE 防止撕裂/重复取值，不冻结并发 charge。
 */
static int hugetlb_cgroup_read_numa_stat(struct seq_file *seq, void *dummy)
{
	int nid;
	struct cftype *cft = seq_cft(seq);
	int idx = MEMFILE_IDX(cft->private);
	bool legacy = !cgroup_subsys_on_dfl(hugetlb_cgrp_subsys);
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_css(seq_css(seq));
	struct cgroup_subsys_state *css;
	unsigned long usage;
	/*
	 * cft/idx 选择统计的 hstate；legacy 决定是否先输出本地视图；h_cg 是输出根；nid 是节点
	 * 游标，css 是 RCU 下的后代游标；usage 以基础页累计，打印时才乘 PAGE_SIZE。
	 */

	/* legacy ABI 需要额外显示不含子 cgroup 的 total 与逐节点值。 */
	if (legacy) {
		/* Add up usage across all nodes for the non-hierarchical total. */
		/* 累加所有有内存节点上的本地 usage，得到不含子 cgroup 的非层级总量。 */
		usage = 0;
		for_each_node_state(nid, N_MEMORY)
			/* 非层级总量仅累加本 css 的 nodeinfo，而不遍历 descendants。 */
			usage += READ_ONCE(h_cg->nodeinfo[nid]->usage[idx]);
		seq_printf(seq, "total=%lu", usage * PAGE_SIZE);

		/* Simply print the per-node usage for the non-hierarchical total. */
		/* 逐节点打印同一非层级本地 usage，让用户态能把 N<n> 字段与 total 对照。 */
		for_each_node_state(nid, N_MEMORY)
			/* 保持 ABI 的 N<n>=bytes 顺序，读者可与 total 对照。 */
			seq_printf(seq, " N%d=%lu", nid,
				   READ_ONCE(h_cg->nodeinfo[nid]->usage[idx]) *
					   PAGE_SIZE);
		seq_putc(seq, '\n');
	}

	/*
	 * The hierarchical total is pretty much the value recorded by the
	 * counter, so use that.
	 */
	/* 补充说明：层级总量由 page_counter 的父链维护，无需手工遍历 css。 */
	seq_printf(seq, "%stotal=%lu", legacy ? "hierarchical_" : "",
		   page_counter_read(&h_cg->hugepage[idx]) * PAGE_SIZE);

	/*
	 * For each node, transverse the css tree to obtain the hierarchical
	 * node usage.
	 */
	/* 补充说明：节点维度没有层级 counter，因此只能在 RCU 临界区累加 descendants。 */
	for_each_node_state(nid, N_MEMORY) {
		/* 每个节点独立重新遍历整棵子树，避免跨 nid 复用会变化的 css 游标。 */
		usage = 0;
		rcu_read_lock();
		css_for_each_descendant_pre(css, &h_cg->css) {
			/* css 指针仅在 RCU 临界区有效，转换后的 h_cg 也不能带出循环。 */
			usage += READ_ONCE(hugetlb_cgroup_from_css(css)
						   ->nodeinfo[nid]
						   ->usage[idx]);
		}
		rcu_read_unlock();
		seq_printf(seq, " N%d=%lu", nid, usage * PAGE_SIZE);
	}

	seq_putc(seq, '\n');

	return 0;
}

/*
 * legacy read_u64 根据 private 属性返回字节数或失败次数。css/cft 是 kernfs 借用输入，
 * 无输出参数；未知属性违反模板不变量而 BUG。页数相关项目统一转换成字节。
 */
/*
 * 业务背景：cgroup v1 为同一 hstate 生成 usage、limit、max_usage 与 failcnt 等多个 u64 文件，
 * 本回调按 cftype.private 分派到 actual/rsvd counter 的对应字段。
 * 入参：css 是非空且由 kernfs 打开文件稳定的借用 hugetlb css；cft 是非空静态/启动期发布
 * cftype，private 高位为合法 hstate 下标、低位为本文件支持的 RES_* 属性。
 * 出参/返回：usage/limit/watermark 以字节 u64 返回，failcnt 以次数返回；无输出参数和状态修改。
 * 注意事项：读取是并发快照且函数不取得额外引用；未知属性表示模板构造错误并触发 BUG，
 * 不能把用户可控值直接放入 private。乘 PAGE_SIZE 前提升到 u64，避免 unsigned long 中间溢出。
 */
static u64 hugetlb_cgroup_read_u64(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	struct page_counter *counter;
	struct page_counter *rsvd_counter;
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_css(css);
	/* counter/rsvd_counter 是当前 hstate 两套借用计数器；h_cg 生命周期由 kernfs 稳定。 */

	counter = &h_cg->hugepage[MEMFILE_IDX(cft->private)];
	rsvd_counter = &h_cg->rsvd_hugepage[MEMFILE_IDX(cft->private)];

	/* 每个 case 精确对应一个 legacy 文件，watermark/failcnt 不等同于当前 usage。 */
	switch (MEMFILE_ATTR(cft->private)) {
	case RES_USAGE:
		/* current 是 page_counter 的瞬时层级值。 */
		return (u64)page_counter_read(counter) * PAGE_SIZE;
	case RES_RSVD_USAGE:
		/* rsvd usage 与 actual current 独立，返回尚未兑现承诺的当前字节数。 */
		return (u64)page_counter_read(rsvd_counter) * PAGE_SIZE;
	case RES_LIMIT:
		/* max 直接保存基础页上限，ABI 再转换为字节。 */
		return (u64)counter->max * PAGE_SIZE;
	case RES_RSVD_LIMIT:
		/* rsvd limit 是 reservation 独立基础页上限，按 ABI 转成字节。 */
		return (u64)rsvd_counter->max * PAGE_SIZE;
	case RES_MAX_USAGE:
		/* watermark 是历史峰值，可由 legacy reset 清零。 */
		return (u64)counter->watermark * PAGE_SIZE;
	case RES_RSVD_MAX_USAGE:
		/* reservation 历史峰值独立保存，可由对应 legacy 文件重置。 */
		return (u64)rsvd_counter->watermark * PAGE_SIZE;
	case RES_FAILCNT:
		/* failcnt 只在 v1 初始化时启用，表示曾被上限拒绝的次数。 */
		return counter->failcnt;
	case RES_RSVD_FAILCNT:
		/* 只返回 reservation charge 被其 max 拒绝的累计次数。 */
		return rsvd_counter->failcnt;
	default:
		/* 模板绝不应生成其它属性；BUG 防止静默把未知文件读成错误统计。 */
		BUG();
	}
}

/*
 * v2 seq_show 把内部无限上限输出为 "max"，其余输出字节或当前 usage；返回 0。
 * reservation case 复用相同格式但切换 counter，limit 必须保持 hugepage 大小对齐。
 */
/*
 * 业务背景：cgroup v2 的 current/max 文件不是纯 u64 ABI，无限值必须格式化成文字 "max"；
 * 本回调同时服务 actual 和 reservation 两组文件。
 * 入参：seq 是非空输出对象，其关联 css/cftype 在回调期间稳定；v 是可为 NULL 的 seq 占位参数，
 * 不读取。private 必须编码合法 hstate 下标及 RES_USAGE/RES_LIMIT 两组属性之一。
 * 出参/返回：向 seq 写一行字节值或 "max" 并返回 0；无输出 ownership 或计数副作用。
 * 注意事项：读到的是并发快照；reservation 分支通过显式贯穿复用格式逻辑，每个 case
 * 都必须先选对 counter。未知属性触发 BUG，limit 比较使用按该 hstate 向下对齐的内部最大值。
 */
static int hugetlb_cgroup_read_u64_max(struct seq_file *seq, void *v)
{
	int idx;
	u64 val;
	struct cftype *cft = seq_cft(seq);
	unsigned long limit;
	struct page_counter *counter;
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_css(seq_css(seq));
	/*
	 * idx/counter 标识 private 选中的 hstate 与计数器；limit 是该 hstate 对齐后的内部无限值；
	 * val 保存一次读取快照并在打印前转换为字节，h_cg 由 seq 的 css 生命周期稳定。
	 */

	idx = MEMFILE_IDX(cft->private);
	/* 默认先指向 actual counter；reservation 属性在 switch 中覆盖它。 */
	counter = &h_cg->hugepage[idx];

	limit = round_down(PAGE_COUNTER_MAX,
			   pages_per_huge_page(&hstates[idx]));

	/* 此分支先选择 reservation counter，随后继续执行对应 current/limit 输出阶段。 */
	switch (MEMFILE_ATTR(cft->private)) {
	case RES_RSVD_USAGE:
		/* rsvd.current 先切换到 reservation counter，再复用 current 数字输出。 */
		counter = &h_cg->rsvd_hugepage[idx];
		fallthrough;
	case RES_USAGE:
		/* 读取成功后总是输出字节，即使内部计数单位是基础页。 */
		val = (u64)page_counter_read(counter);
		seq_printf(seq, "%llu\n", val * PAGE_SIZE);
		break;
	case RES_RSVD_LIMIT:
		/* rsvd.max 先切换到 reservation counter，再复用 max/字节格式判断。 */
		counter = &h_cg->rsvd_hugepage[idx];
		fallthrough;
	case RES_LIMIT:
		/* 精确等于对齐后的最大值才向用户态显示 max。 */
		val = (u64)counter->max;
		/* 其它数值必须保留为字节，避免 ABI 泄露内部基础页单位。 */
		if (val == limit)
			seq_puts(seq, "max\n");
		else
			seq_printf(seq, "%llu\n", val * PAGE_SIZE);
		break;
	default:
		/* v2 模板只允许 current/max 两组属性，未知 private 是内核构造错误。 */
		BUG();
	}

	return 0;
}

/* 串行化同一 cgroup limit 的写者；charge 约束本身仍由 page_counter 保证。 */
static DEFINE_MUTEX(hugetlb_limit_mutex);

/*
 * v1/v2 limit 文件的共用写路径：解析用户输入、按 hstate 页大小对齐并更新 usage 或
 * reservation 上限。成功返回 nbytes，错误返回 errno；root 和不可写属性拒绝。
 * max 是 ABI 的无限制文字；of/css 为 kernfs 借用对象，buf 会被原地 strstrip，off 未使用。
 */
/*
 * 业务背景：用户写入每种 hstate 的 max/rsvd.max 时，需要把文本字节值转换为基础页、按
 * HugeTLB folio 大小规范化，并以 page_counter 原子更新层级上限。
 * 入参：of 是非空 kernfs 打开文件借用对象；buf 是可写、长度由 nbytes 描述的输入缓冲，
 * 会被 strstrip 原地调整内容；nbytes 为用户请求字节数；off 为未使用的文件偏移；max 是
 * 非空 ABI 无限值字符串，v1 传 "-1"、v2 传 "max"。
 * 出参/返回：成功设置 actual 或 reservation 上限后返回 nbytes；根节点/错误属性返回 -EINVAL，
 * 文本解析或低于当前 usage 等 page_counter 拒绝返回相应 errno；失败不部分改变旧上限。
 * 注意事项：mutex 可睡眠，故不得在原子上下文调用；它串行化 limit 写者但不阻塞并发 charge，
 * 最终一致性由 page_counter_set_max() 保证。向下对齐意味着非整 HugeTLB 页的尾数字节不生效。
 */
static ssize_t hugetlb_cgroup_write(struct kernfs_open_file *of,
				    char *buf, size_t nbytes, loff_t off,
				    const char *max)
{
	int ret, idx;
	unsigned long nr_pages;
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_css(of_css(of));
	bool rsvd = false;
	/*
	 * h_cg 是目标文件所属 css；idx 从 private 解出 hstate；nr_pages 是解析、对齐后的基础页上限；
	 * rsvd 决定计数器种类；ret 在解析/设置阶段携带 errno 或成功零值。
	 */

	if (hugetlb_cgroup_is_root(h_cg)) /* Can't set limit on root */
		/* 根是父链最后一层，允许限制会破坏子树层级记账的无上限假设。 */
		return -EINVAL;

	/* 不能接受半个 HugeTLB 页的配额，解析后向下取整。 */
	buf = strstrip(buf);
	ret = page_counter_memparse(buf, max, &nr_pages);
	if (ret)
		/* 解析失败不改变旧上限，调用者可修正输入后重试。 */
		return ret;

	idx = MEMFILE_IDX(of_cft(of)->private);
	nr_pages = round_down(nr_pages, pages_per_huge_page(&hstates[idx]));

	/* 只有 max/rsvd.max 是写接口，历史统计文件不能借此被篡改。 */
	switch (MEMFILE_ATTR(of_cft(of)->private)) {
	case RES_RSVD_LIMIT:
		/* rsvd.max/legacy rsvd.limit 选择预留计数器，随后共用同一加锁设置阶段。 */
		rsvd = true;
		fallthrough;
	case RES_LIMIT:
		/* actual max 直接使用默认 rsvd=false；两类上限都必须串行化写者。 */
		/* mutex 覆盖设置阶段，set_max 若低于当前 usage 会返回失败而不部分修改。 */
		mutex_lock(&hugetlb_limit_mutex);
		ret = page_counter_set_max(
			__hugetlb_cgroup_counter_from_cgroup(h_cg, idx, rsvd),
			nr_pages);
		/* 解锁后返回完整写入长度或 set_max 的拒绝 errno。 */
		mutex_unlock(&hugetlb_limit_mutex);
		break;
	default:
		/* current、watermark、failcnt 等属性不具备 limit 写语义。 */
		ret = -EINVAL;
		break;
	}
	return ret ?: nbytes;
}

/* v1 ABI 用 -1 表示无限制；其余所有语义由共用写路径处理。 */
/*
 * 业务背景：cgroup v1 write 回调只负责选择 legacy 无限值拼写，再交给共用 limit 状态机。
 * 入参：of、buf、nbytes、off 均为 kernfs 原样传入；buf 是可修改输入，off 不参与语义。
 * 出参/返回：成功返回 nbytes，失败透传解析、属性、root 或 set_max errno；无额外副作用。
 * 注意事项：可因内部 mutex 睡眠；所有 ownership、对齐和失败保证与 hugetlb_cgroup_write() 相同。
 */
static ssize_t hugetlb_cgroup_write_legacy(struct kernfs_open_file *of,
					   char *buf, size_t nbytes, loff_t off)
{
	return hugetlb_cgroup_write(of, buf, nbytes, off, "-1");
}

/* v2 ABI 用 max 表示无限制；这是不保存状态的格式适配器。 */
/*
 * 业务背景：cgroup v2 write 回调把 ABI 的 "max" 哨兵注入共用 limit 状态机。
 * 入参：of、buf、nbytes、off 来自 kernfs；buf 为可修改输入，off 不使用。
 * 出参/返回：成功返回 nbytes，失败透传底层 errno；无本层输出或 ownership 转移。
 * 注意事项：内部可能因 mutex 睡眠；不能绕过共用函数直接写 counter->max。
 */
static ssize_t hugetlb_cgroup_write_dfl(struct kernfs_open_file *of,
					char *buf, size_t nbytes, loff_t off)
{
	return hugetlb_cgroup_write(of, buf, nbytes, off, "max");
}

/*
 * legacy reset 清零峰值或失败次数而不改变当前 charge/limit。of 给出目标 css/属性，
 * buf/off 仅是回调签名，成功返回 nbytes；其它属性返回 -EINVAL，调用者可立即开始新的观测周期。
 */
/*
 * 业务背景：cgroup v1 允许向 max_usage/failcnt 文件写任意内容来开始新的统计周期，本函数只
 * 重置历史观测值，不能改变正在使用的页数或配额上限。
 * 入参：of 是非空 kernfs 文件，private 指定 hstate 与可重置 RES_* 属性；buf 内容、off 均
 * 忽略；nbytes 仅用于成功返回写入长度。
 * 出参/返回：支持的 actual/rsvd watermark 或 failcnt 被清零并返回 nbytes；其它属性返回
 * -EINVAL 且无修改。无 ownership 转移。
 * 注意事项：并发 charge 可紧接着产生新峰值/失败，因此 reset 是观测边界而非冻结快照；
 * kernfs/cgroup 生命周期稳定 css，本函数不另取引用且不应被用于 current/limit 文件。
 */
static ssize_t hugetlb_cgroup_reset(struct kernfs_open_file *of,
				    char *buf, size_t nbytes, loff_t off)
{
	int ret = 0;
	struct page_counter *counter, *rsvd_counter;
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_css(of_css(of));
	/*
	 * h_cg 是目标 css；counter/rsvd_counter 分别指向 private 选中 hstate 的两套计数器；
	 * ret 默认成功，仅未知属性改为 -EINVAL。
	 */

	counter = &h_cg->hugepage[MEMFILE_IDX(of_cft(of)->private)];
	rsvd_counter = &h_cg->rsvd_hugepage[MEMFILE_IDX(of_cft(of)->private)];

	/* 只允许重置历史统计；usage 与 limit 的状态转换必须走专门路径。 */
	switch (MEMFILE_ATTR(of_cft(of)->private)) {
	case RES_MAX_USAGE:
		/* reset_watermark 不接触 counter 的当前 charge。 */
		page_counter_reset_watermark(counter);
		break;
	case RES_RSVD_MAX_USAGE:
		/* rsvd.max_usage 只把 reservation 历史水位重置到其当前 usage。 */
		page_counter_reset_watermark(rsvd_counter);
		break;
	case RES_FAILCNT:
		/* 失败次数不是原子快照接口；legacy reset 语义允许直接从新周期开始计数。 */
		counter->failcnt = 0;
		break;
	case RES_RSVD_FAILCNT:
		/* 预留失败计数使用独立 counter，不能误清实际页 failcnt。 */
		rsvd_counter->failcnt = 0;
		break;
	default:
		/* 非历史统计文件拒绝 reset，确保 current/limit 不被这个兼容接口修改。 */
		ret = -EINVAL;
		break;
	}
	return ret ?: nbytes;
}

/*
 * 把 HugeTLB 字节大小格式化为 cgroup 文件名前缀。buf 为调用者提供的输出空间，size 是上限，
 * hsize 为字节；返回同一 buf 借用指针。只供 init 使用，不涉及并发或所有权转移。
 */
/*
 * 业务背景：每个 hstate 都生成一组以 2MB、1GB 等尺寸区分的 cgroup 文件，本 helper 统一
 * 构造尺寸前缀，供 cftype 实例化阶段拼接属性名。
 * 入参：buf 是非空可写输出缓冲；size 是其字节容量且应大于零；hsize 是正的 hugepage
 * 字节数，按 G/M/K 中第一个适用单位做整数换算。
 * 出参/返回：向 buf 写入 NGB/NMB/NKB 的 NUL 结尾字符串并返回同一借用指针；不分配内存。
 * 注意事项：snprintf 会按 size 截断，当前唯一调用者使用 32 字节并依赖 hstate 尺寸可完整表示；
 * 仅启动路径调用，无共享状态和睡眠点。
 */
static char *mem_fmt(char *buf, int size, unsigned long hsize)
{
	if (hsize >= SZ_1G)
		snprintf(buf, size, "%luGB", hsize / SZ_1G);
	else if (hsize >= SZ_1M)
		snprintf(buf, size, "%luMB", hsize / SZ_1M);
	else
		snprintf(buf, size, "%luKB", hsize / SZ_1K);
	return buf;
}

/*
 * events/events.local 的共同输出：按 local 选择本组或层级 HUGETLB_MAX 计数并写入 seq。
 * 返回 0；原子读取得到瞬时快照，不能与同时发生的失败形成事务边界。
 */
/*
 * 业务背景：v2 events 与 events.local 格式相同，仅统计范围不同，因此共用一次原子读取和 ABI 输出。
 * 入参：seq 是非空输出对象，其 cftype/private 与 css 在回调期间稳定；local 为 true 选择本组
 * events_local，为 false 选择包含子组传播的 events。
 * 出参/返回：输出固定格式 "max <次数>" 一行并返回 0；无输出参数、引用变化或计数修改。
 * 注意事项：atomic_long_read 只保证单计数值读取，不与 charge 失败建立全局快照；idx 必须来自
 * 启动期生成的合法 private，本函数不检查越界且不可把 seq/cgroup 借用指针带出回调。
 */
static int __hugetlb_events_show(struct seq_file *seq, bool local)
{
	int idx;
	long max;
	/* cft->private 指定本次 show 的 hugepage 尺寸，max 是最终输出的事件快照。 */
	struct cftype *cft = seq_cft(seq);
	struct hugetlb_cgroup *h_cg = hugetlb_cgroup_from_css(seq_css(seq));
	/* idx 解出 hstate 槽；cft/h_cg 都只在 seq 回调期间借用，max 单位是事件次数。 */

	idx = MEMFILE_IDX(cft->private);

	if (local)
		max = atomic_long_read(&h_cg->events_local[idx][HUGETLB_MAX]);
	else
		max = atomic_long_read(&h_cg->events[idx][HUGETLB_MAX]);
	/* 两个来源统一成 ABI 固定的单行 "max <count>"。 */

	seq_printf(seq, "max %lu\n", max);

	return 0;
}

/* 层级 events 的 seq_show 包装，v 未使用。 */
/*
 * 业务背景：events 文件通过独立回调固定选择层级累计视图，供 cftype 模板直接注册。
 * 入参：seq 为非空 kernfs 输出对象；v 是未使用且可为 NULL 的 seq 占位参数。
 * 出参/返回：透传共同输出函数的 0 返回并写出层级 HUGETLB_MAX 次数；无额外副作用。
 * 注意事项：读取为瞬时原子快照，css 与 cftype 生命周期由 kernfs 回调环境保证。
 */
static int hugetlb_events_show(struct seq_file *seq, void *v)
{
	return __hugetlb_events_show(seq, false);
}

/* 仅本 css events.local 的 seq_show 包装，v 未使用。 */
/*
 * 业务背景：events.local 文件固定选择不含子组传播的本地失败视图。
 * 入参：seq 为非空 kernfs 输出对象；v 是未使用且可为 NULL 的 seq 占位参数。
 * 出参/返回：透传共同输出函数的 0 返回并写出本 css 的 HUGETLB_MAX 次数；无额外副作用。
 * 注意事项：语义与 events 的层级累计不同，不能因格式相同而互换回调中的 local 参数。
 */
static int hugetlb_events_local_show(struct seq_file *seq, void *v)
{
	return __hugetlb_events_show(seq, true);
}

/* v2 模板定义每种 hugepage 尺寸生成的 max/current/events/numa_stat 文件和回调契约。 */
static struct cftype hugetlb_dfl_tmpl[] = {
	/* max 读写 actual counter 上限；根 cgroup 不创建该文件。 */
	{
		.name = "max",
		.private = RES_LIMIT,
		.seq_show = hugetlb_cgroup_read_u64_max,
		.write = hugetlb_cgroup_write_dfl,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	/* rsvd.max 限制尚未兑现的 reservation，防止承诺量无限累积。 */
	{
		.name = "rsvd.max",
		/* v2 写回调与 max 共用，private 决定它选择 reservation counter。 */
		.private = RES_RSVD_LIMIT,
		.seq_show = hugetlb_cgroup_read_u64_max,
		.write = hugetlb_cgroup_write_dfl,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "current",
		/* 只读 current 不安装 write 回调，避免用户态伪造实际占用。 */
		.private = RES_USAGE,
		.seq_show = hugetlb_cgroup_read_u64_max,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	/* current 仅统计已经获得实际 folio 的基础页数。 */
	{
		.name = "rsvd.current",
		.private = RES_RSVD_USAGE,
		.seq_show = hugetlb_cgroup_read_u64_max,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	/* rsvd.current 与 current 分离，用户态可区分承诺与实际占用。 */
	{
		.name = "events",
		.seq_show = hugetlb_events_show,
		/* file_offset 让 cgroup_file_notify 找到该 hstate 的层级事件文件。 */
		.file_offset = MEMFILE_OFFSET(struct hugetlb_cgroup, events_file[0]),
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	/* events.local 不向祖先累计，适合定位本 cgroup 自身的上限失败。 */
	{
		.name = "events.local",
		.seq_show = hugetlb_events_local_show,
		.file_offset = MEMFILE_OFFSET(struct hugetlb_cgroup, events_local_file[0]),
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	/* numa_stat 提供按 nid 的实际页快照，不含 reservation 的位置归属。 */
	{
		.name = "numa_stat",
		.seq_show = hugetlb_cgroup_read_numa_stat,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	/* don't need terminator here */
	/* 此处不需要终止项；复制器使用 DFL_TMPL_SIZE 的显式条目数，实际数组另预留零终止项。 */
};

/* v1 模板保留旧文件名以及可复位的 max_usage/failcnt 统计接口。 */
static struct cftype hugetlb_legacy_tmpl[] = {
	/* limit_in_bytes 读写 actual counter 上限，legacy 用 -1 表示无限。 */
	{
		.name = "limit_in_bytes",
		.private = RES_LIMIT,
		.read_u64 = hugetlb_cgroup_read_u64,
		.write = hugetlb_cgroup_write_legacy,
	},
	/* v1 的 rsvd.limit_in_bytes 是 reservation 配额，不影响 actual limit。 */
	{
		.name = "rsvd.limit_in_bytes",
		/* legacy write 使用 -1 作为无限制哨兵，再由 private 选择 rsvd。 */
		.private = RES_RSVD_LIMIT,
		.read_u64 = hugetlb_cgroup_read_u64,
		.write = hugetlb_cgroup_write_legacy,
	},
	{
		.name = "usage_in_bytes",
		.private = RES_USAGE,
		.read_u64 = hugetlb_cgroup_read_u64,
	},
	/* usage_in_bytes 返回层级 page_counter 当前值。 */
	{
		.name = "rsvd.usage_in_bytes",
		.private = RES_RSVD_USAGE,
		.read_u64 = hugetlb_cgroup_read_u64,
	},
	/* rsvd.usage_in_bytes 返回当前预留而非已分配页。 */
	{
		.name = "max_usage_in_bytes",
		.private = RES_MAX_USAGE,
		.write = hugetlb_cgroup_reset,
		.read_u64 = hugetlb_cgroup_read_u64,
	},
	/* max_usage 可 reset，作为使用高水位的观测边界。 */
	{
		.name = "rsvd.max_usage_in_bytes",
		.private = RES_RSVD_MAX_USAGE,
		/* reset 回调只作用于此 reservation watermark。 */
		.write = hugetlb_cgroup_reset,
		.read_u64 = hugetlb_cgroup_read_u64,
	},
	/* reservation 高水位独立于实际页高水位。 */
	{
		.name = "failcnt",
		.private = RES_FAILCNT,
		.write = hugetlb_cgroup_reset,
		.read_u64 = hugetlb_cgroup_read_u64,
	},
	/* failcnt 记录实际页 charge 被 max 拒绝的历史次数。 */
	{
		.name = "rsvd.failcnt",
		.private = RES_RSVD_FAILCNT,
		.write = hugetlb_cgroup_reset,
		.read_u64 = hugetlb_cgroup_read_u64,
	},
	/* rsvd.failcnt 则记录 reservation charge 的拒绝。 */
	{
		.name = "numa_stat",
		.seq_show = hugetlb_cgroup_read_numa_stat,
	},
	/* don't need terminator here */
	/* 模板按 LEGACY_TMPL_SIZE 复制，故本静态表无需额外空 cftype 终止。 */
};

/*
 * 将一个 hstate 的模板复制为实际 cftype 文件，并把 idx 编进 private/file_offset。
 * h/cft/tmpl 都是 init 阶段借用对象，tmpl_size 为条目数；无返回值，完成后 cft 归全局文件数组所有。
 * lockdep key 必须逐文件注册，否则不同 cgroup 文件的锁类会被错误合并。
 */
/*
 * 业务背景：静态模板只描述属性后缀和回调；每个已启用 hstate 都需要独立完整文件名、private
 * 索引、事件 file_offset 和 lockdep key，本函数把模板实例化为可注册数组。
 * 入参：h 是非空借用 hstate；cft 指向至少 tmpl_size 个可写、尚未发布的输出槽；tmpl 指向
 * 至少 tmpl_size 个只读模板条目；tmpl_size 为非负条目数，四者仅在启动期有效。
 * 出参/返回：无直接返回；逐项填满 cft 输出区，name 变为“尺寸.属性”，private 嵌入 idx，
 * 非零 file_offset 调整到对应 hstate 数组元素，并为每项注册 lockdep key；不接管 h/tmpl。
 * 注意事项：__init 阶段单线程执行且不需要运行期锁；目标数组容量和 idx 合法性由 pre_init
 * 的计数保证。浅复制后只能改写目标 cft，绝不能回写共享模板。
 */
static void __init
hugetlb_cgroup_cfttypes_init(struct hstate *h, struct cftype *cft,
			     struct cftype *tmpl, int tmpl_size)
{
	char buf[32];
	int i, idx = hstate_index(h);
	/* buf 暂存尺寸前缀；idx 标识输出数组切片；i 是 [0, tmpl_size) 的模板/目标同步游标。 */

	/* format the size */
	/* 补充说明：文件名以 2MB/1GB 等 hstate 大小区分，避免不同池共享同一 ABI 节点。 */
	mem_fmt(buf, sizeof(buf), huge_page_size(h));

	/* 逐项复制而非共享模板，因为每个 hstate 的 private 和 offset 均不同。 */
	for (i = 0; i < tmpl_size; cft++, tmpl++, i++) {
		/* 复制后所有可变字段只属于该 hstate 的具体 cftype。 */
		*cft = *tmpl;
		/* rebuild the name */
		/* 重建名称：把模板后缀与本 hstate 的人类可读尺寸拼成唯一 ABI 文件名。 */
		scnprintf(cft->name, MAX_CFTYPE_NAME, "%s.%s", buf, tmpl->name);
		/* rebuild the private */
		/* 重建 private：高位写入 hstate 下标，低位保留模板的 RES_* 属性。 */
		cft->private = MEMFILE_PRIVATE(idx, tmpl->private);
		/* rebuild the file_offset */
		/* 重建事件文件偏移：从数组首元素定位到当前 idx 对应的 cgroup_file。 */
		if (tmpl->file_offset) {
			/* 数组字段的元素偏移 = 首元素偏移 + 单元素大小乘 idx。 */
			unsigned int offset = tmpl->file_offset;

			cft->file_offset = MEMFILE_OFFSET0(offset) +
					   MEMFILE_FIELD_SIZE(offset) * idx;
		}

		lockdep_register_key(&cft->lockdep_key);
	}
}

/* 为一个 hstate 填充 v2 默认层级文件槽位；无返回，调用仅在启动 init 阶段。 */
/*
 * 业务背景：总初始化循环用此包装选择 v2 模板和该 hstate 在 dfl_files 中的输出切片。
 * 入参：h 是非空借用 hstate，其 index 必须小于 hugetlb_max_hstate。
 * 出参/返回：无直接返回；填充 DFL_TMPL_SIZE 个 v2 cftype，不转移 h 的 ownership。
 * 注意事项：仅 __init 单线程调用；dfl_files 必须已由 pre_init 分配且尚未注册。
 */
static void __init __hugetlb_cgroup_file_dfl_init(struct hstate *h)
{
	int idx = hstate_index(h);
	/* idx 唯一决定本 hstate 在连续 v2 cftype 数组中的起始偏移。 */

	hugetlb_cgroup_cfttypes_init(h, dfl_files + idx * DFL_TMPL_SIZE,
				     hugetlb_dfl_tmpl, DFL_TMPL_SIZE);
}

/* 为一个 hstate 填充 legacy v1 文件槽位；无返回，调用仅在启动 init 阶段。 */
/*
 * 业务背景：总初始化循环用此包装选择 v1 模板和该 hstate 在 legacy_files 中的输出切片。
 * 入参：h 是非空借用 hstate，其 index 必须小于 hugetlb_max_hstate。
 * 出参/返回：无直接返回；填充 LEGACY_TMPL_SIZE 个 v1 cftype，不转移 h 的 ownership。
 * 注意事项：仅 __init 单线程调用；legacy_files 必须已分配且尚未交给 cgroup 核心。
 */
static void __init __hugetlb_cgroup_file_legacy_init(struct hstate *h)
{
	int idx = hstate_index(h);
	/* idx 唯一决定本 hstate 在连续 legacy cftype 数组中的起始偏移。 */

	hugetlb_cgroup_cfttypes_init(h, legacy_files + idx * LEGACY_TMPL_SIZE,
				     hugetlb_legacy_tmpl, LEGACY_TMPL_SIZE);
}

/* 同一 hstate 同时准备两套 ABI，随后由 cgroup 核心按 dfl/legacy 挂载选择。 */
/*
 * 业务背景：每个 hstate 必须同时具备 cgroup v2 与 v1 文件描述，挂载模式只决定最终使用哪套。
 * 入参：h 是非空、启动期已建立的借用 hstate。
 * 出参/返回：无直接返回；依次填充该 hstate 的 dfl 和 legacy 输出切片。
 * 注意事项：pre_init 必须先成功，post_init 尚未发生；本函数不自行回滚，内存不足已在前阶段 BUG。
 */
static void __init __hugetlb_cgroup_file_init(struct hstate *h)
{
	__hugetlb_cgroup_file_dfl_init(h);
	__hugetlb_cgroup_file_legacy_init(h);
}

/*
 * 启动阶段先按 hugetlb_max_hstate 分配两套 cftype 数组，并预留全零 terminator。
 * 无入参/返回；BUG_ON 表示启动内存不足无法安全注册部分控制器，不能静默继续。
 */
/*
 * 业务背景：cgroup_add_*_cftypes 使用零条目终止数组，因此在按 hstate 展开模板前必须一次性
 * 分配“全部实例 + 一个终止项”，避免发布只有部分 hstate 的 ABI。
 * 入参：无；读取启动期已确定的 hugetlb_max_hstate 和模板大小。
 * 出参/返回：无直接返回；dfl_files/legacy_files 分别指向零填充输出数组，ownership 保留在
 * 控制器全局并在随后 post_init 注册；最后一项因 kzalloc 保持全零。
 * 注意事项：仅 __init 可睡眠上下文调用；任一分配失败 BUG_ON 停止启动，不存在可恢复的部分成功。
 */
static void __init __hugetlb_cgroup_file_pre_init(void)
{
	int cft_count;
	/* cft_count 临时保存当前 ABI 的“hstate 数 × 模板条目 + 终止项”对象数。 */

	cft_count = hugetlb_max_hstate * DFL_TMPL_SIZE + 1; /* add terminator */
	/* 额外一个全零 cftype 作为 cgroup 核心遍历 v2 数组时的终止标记。 */
	dfl_files = kzalloc_objs(struct cftype, cft_count);
	BUG_ON(!dfl_files);
	cft_count = hugetlb_max_hstate * LEGACY_TMPL_SIZE + 1; /* add terminator */
	/* v1 数组同样预留独立终止项，不能复用 v2 存储。 */
	legacy_files = kzalloc_objs(struct cftype, cft_count);
	BUG_ON(!legacy_files);
}

/* 将已完整填充的数组交给 cgroup 核心注册；WARN 保留启动诊断但不改变返回控制流。 */
/*
 * 业务背景：所有 hstate 条目构造完成后，本函数跨越发布边界，让 cgroup 核心据数组创建 v1/v2 文件。
 * 入参：无；读取已经填充且以零项终止的 dfl_files/legacy_files。
 * 出参/返回：无直接返回；两套数组的可见性和使用责任交给 cgroup 核心，注册失败仅 WARN 记录。
 * 注意事项：仅 __init 顺序调用一次；WARN 后函数继续意味着可能缺少对应 ABI 文件，调用者没有 errno
 * 可回滚。发布后不得再改写 cftype 字段或释放数组。
 */
static void __init __hugetlb_cgroup_file_post_init(void)
{
	WARN_ON(cgroup_add_dfl_cftypes(&hugetlb_cgrp_subsys,
				       dfl_files));
	WARN_ON(cgroup_add_legacy_cftypes(&hugetlb_cgrp_subsys,
					  legacy_files));
}

/*
 * hugetlb 子系统启动入口：分配文件表、遍历现有 hstate 填充 ABI 条目、最后发布给 cgroup。
 * 无入参/返回；__init 表示启动后代码可回收。初始化失败由内部 BUG/WARN 记录，调用者没有可恢复 errno。
 */
/*
 * 业务背景：hugetlb_init() 在 hstate 集合确定后调用本入口，为每种 HugeTLB 尺寸生成并注册
 * cgroup v1/v2 控制文件，是运行期配额 ABI 可见的总发布流程。
 * 入参：无；遍历全局 hstate 集合。
 * 出参/返回：无直接返回；成功后全部 cftype 已注册，dfl_files/legacy_files 进入只读运行期；
 * pre_init 分配失败 BUG，注册失败 WARN，均没有向调用者返回的错误码。
 * 注意事项：__init 只允许启动期单线程调用一次且可睡眠；顺序必须是完整分配、全部填充、最后发布，
 * 否则 kernfs 读写可能观察未初始化的 private/file_offset。
 */
void __init hugetlb_cgroup_file_init(void)
{
	struct hstate *h;
	/* h 是 for_each_hstate 的启动期借用游标，循环外不保存。 */

	__hugetlb_cgroup_file_pre_init();
	for_each_hstate(h)
		__hugetlb_cgroup_file_init(h);
	__hugetlb_cgroup_file_post_init();
}

/*
 * hugetlb_lock will make sure a parallel cgroup rmdir won't happen
 * when we migrate hugepages
 */
/*
 * 补充说明：迁移在同一 hugetlb_lock 临界区摘除 old 的 actual/rsvd 归属、绑定 new，
 * 再把 active-list 节点替换为 new；rmdir 离线也持同一锁，所以不会把归属迁移到已销毁 css。
 */
/*
 * 业务背景：HugeTLB 页面迁移会用 new_folio 替换物理承载，但 cgroup charge 与 reservation
 * 责任必须连续保留；本函数把两种归属和 active-list 身份一起从旧 folio 原子转交给新 folio。
 * 入参：old_folio/new_folio 均为非空借用 HugeTLB folio；两者代表同一 hstate 和同一逻辑页，
 * old 当前在 active list，new 尚未发布且调用者稳定二者生命周期。
 * 出参/返回：无直接返回；控制器启用时清空 old 的 actual/rsvd 指针，把原值写入 new，并以
 * new 的 lru 节点替换 active-list 位置；page_counter、NUMA usage 和 css 引用数量均不改变。
 * 注意事项：内部 spin_lock_irq 不可睡眠；hugetlb_lock 与 css_offline/释放路径配对，保证转移期间
 * 没有并发 reparent 或 uncharge。控制器禁用时立即返回，调用者继续其非 cgroup 迁移流程。
 */
void hugetlb_cgroup_migrate(struct folio *old_folio, struct folio *new_folio)
{
	struct hugetlb_cgroup *h_cg;
	struct hugetlb_cgroup *h_cg_rsvd;
	struct hstate *h = folio_hstate(old_folio);
	/* h_cg/h_cg_rsvd 暂存 old 的两种归属以跨越清空再写入 new；h 决定 active list。 */

	if (hugetlb_cgroup_disabled())
		return;

	/* 这把锁同时保护两份 folio 指针和 h->hugepage_activelist 的结构不变量。 */
	spin_lock_irq(&hugetlb_lock);
	h_cg = hugetlb_cgroup_from_folio(old_folio);
	h_cg_rsvd = hugetlb_cgroup_from_folio_rsvd(old_folio);
	set_hugetlb_cgroup(old_folio, NULL);
	set_hugetlb_cgroup_rsvd(old_folio, NULL);

	/* move the h_cg details to new cgroup */
	/* 补充说明：这里的 “cgroup” 是随 folio 迁移的计费归属，不是把 css 对象移入新 cgroup。 */
	set_hugetlb_cgroup(new_folio, h_cg);
	set_hugetlb_cgroup_rsvd(new_folio, h_cg_rsvd);
	list_move(&new_folio->lru, &h->hugepage_activelist);
	spin_unlock_irq(&hugetlb_lock);
}

/* 空终止表让早期 cgroup 注册拥有默认字段；运行时实际文件由上方 init 替换添加。 */
static struct cftype hugetlb_files[] = {
	{} /* terminate */
	/* 空 cftype 是数组终止哨兵，cgroup 核心读到它后停止枚举默认文件。 */
};

/* cgroup 核心通过这张操作表把 css 生命周期和两套文件 ABI 分派给本文件。 */
struct cgroup_subsys hugetlb_cgrp_subsys = {
	/* 创建新 css 时分配私有计数器和 NUMA 统计对象。 */
	.css_alloc	= hugetlb_cgroup_css_alloc,
	/* 删除 css 时先把仍绑定的 actual folio 归属上移。 */
	.css_offline	= hugetlb_cgroup_css_offline,
	/* css 引用耗尽后最终释放私有对象。 */
	.css_free	= hugetlb_cgroup_css_free,
	/* 启动早期先用空终止表，具体 v2 文件随后由 file_init 动态注册。 */
	.dfl_cftypes	= hugetlb_files,
	/* v1 同样先保持有效空表，避免 cgroup 核心读取 NULL 描述数组。 */
	.legacy_cftypes	= hugetlb_files,
};
