// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Code for The Physical Address Space
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/*
 * DAMON 的物理地址操作集把核心层使用的抽象地址单位换算为物理地址，并以 folio
 * 为观察和动作对象。一次采样先清除被选 folio 的访问证据，随后汇总 CPU 页表、
 * 设备 MMU notifier 与 idle 标志判断是否重新访问；DAMOS 再按过滤器对页回收、
 * LRU 升降级、NUMA 迁移或仅统计。文件只借用核心层的 target/region/scheme，
 * 对 folio 的每次临时引用都在当前迭代内归还。
 */

#define pr_fmt(fmt) "damon-pa: " fmt

#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/memory-tiers.h>
#include <linux/mm_inline.h>

#include "../internal.h"
#include "ops-common.h"

/*
 * damon_pa_phys_addr() - 把 DAMON 核心地址换算成字节物理地址。
 * 业务背景：核心层允许用 addr_unit 压缩地址空间，本操作集在访问 PFN 前恢复真实
 * 字节地址，调用链为采样/方案回调 -> 本函数 -> PHYS_PFN()。
 * 入参：addr 是核心层地址单位计数；addr_unit 是每单位字节数，必须非零；均为纯输入。
 * 出参/返回：返回 addr * addr_unit 的 phys_addr_t，无引用或全局副作用。
 * 注意事项：调用者保证乘积代表有效物理地址；本函数不检查溢出且不会睡眠。
 */
static phys_addr_t damon_pa_phys_addr(
		unsigned long addr, unsigned long addr_unit)
{
	return (phys_addr_t)addr * addr_unit;
}

/*
 * damon_pa_core_addr() - 把字节物理量折回 DAMON 核心地址单位。
 * 业务背景：DAMOS 的 applied/过滤通过量必须与 region 使用同一单位，本函数位于
 * 页数统计完成后的返回边界。
 * 入参：pa 是字节数或物理地址；addr_unit 是非零的每单位字节数，均为纯输入。
 * 出参/返回：返回向下取整的核心单位数，无副作用。
 * 注意事项：64 位被除数配 32 位除数时显式用 div_u64()，避免某些架构生成未链接
 * 的编译器除法 helper；不会睡眠。
 */
static unsigned long damon_pa_core_addr(
		phys_addr_t pa, unsigned long addr_unit)
{
	/*
	 * Use div_u64() for avoiding linking errors related with __udivdi3,
	 * __aeabi_uldivmod, or similar problems.  This should also improve the
	 * performance optimization (read div_u64() comment for the detail).
	 */
	/*
	 * 译注：使用 div_u64() 可避免链接到 __udivdi3、__aeabi_uldivmod 等
	 * 64 位除法 helper，也便于体系结构提供优化实现；仅在类型宽度组合需要时走此路。
	 */
	if (sizeof(pa) == 8 && sizeof(addr_unit) == 4)
		return div_u64(pa, addr_unit);
	return pa / addr_unit;
}

/*
 * damon_pa_mkold() - 为一个物理地址所在 folio 建立新的访问采样基线。
 * 业务背景：prepare_access_checks 回调随机选点后先清旧访问证据，稍后的 young
 * 查询才能回答“本采样周期是否访问过”。
 * 入参：paddr 是字节物理地址，纯输入，不携带 page 引用。
 * 出参/返回：无直接返回值；找到在线 LRU folio 时清其映射访问证据并归还临时引用。
 * 注意事项：damon_get_folio() 失败属于页洞/离线/非 LRU 等可接受情况；rmap walk
 * 可能受锁竞争影响而尽力执行，本函数可睡眠且不向调用者报告失败。
 */
static void damon_pa_mkold(phys_addr_t paddr)
{
	/* folio 是当前函数持有的临时引用，仅覆盖 mkold 调用窗口。 */
	struct folio *folio = damon_get_folio(PHYS_PFN(paddr));

	if (!folio)
		return;

	damon_folio_mkold(folio);
	folio_put(folio);
}

/*
 * __damon_pa_prepare_access_check() - 为单个 region 选择并复位一个采样点。
 * 业务背景：DAMON 用区域内随机样本近似整区访问频率；本函数是 prepare 回调的
 * 单区步骤，完成后 check 阶段读取同一 sampling_addr。
 * 入参：r 为借用的可写 region，更新 sampling_addr；ctx 为借用上下文，提供随机源、
 * 地址单位及区域约束，所有权均不变。
 * 出参/返回：无直接返回值；写入 r->sampling_addr，并尽力把对应 folio 置旧。
 * 注意事项：调用者串行遍历 DAMON 对象；可能因 rmap 操作睡眠，物理页不存在时仍
 * 保留采样地址，check 阶段会把它视为未访问。
 */
static void __damon_pa_prepare_access_check(struct damon_region *r,
		struct damon_ctx *ctx)
{
	/* 随机点使用核心地址单位，直到触碰 folio 时才换算成字节物理地址。 */
	r->sampling_addr = damon_rand(ctx, r->ar.start, r->ar.end);

	damon_pa_mkold(damon_pa_phys_addr(r->sampling_addr, ctx->addr_unit));
}

/*
 * damon_pa_prepare_access_checks() - 为上下文全部 region 启动一次访问观察窗口。
 * 业务背景：DAMON 核心在等待 sampling_interval 前调用本操作回调，逐区选择样本并
 * 清旧 young/idle 证据；下一阶段是 damon_pa_check_accesses()。
 * 入参：ctx 是借用且可写的监测上下文，target/region 列表在回调期间由核心线程稳定。
 * 出参/返回：无直接返回值；每个 region 的 sampling_addr 及相应 folio 访问基线变化。
 * 注意事项：双层遍历不取得对象所有权；可能睡眠，单页准备失败不会中止其他区域。
 */
static void damon_pa_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t)
			__damon_pa_prepare_access_check(r, ctx);
	}
}

/*
 * damon_pa_young() - 查询物理地址所在 folio 自基线后是否出现访问。
 * 业务背景：check 阶段借助 ops-common 汇总 CPU、设备与 idle 信息，并把 folio 大小
 * 返回给相邻 region 的结果复用逻辑。
 * 入参：paddr 是字节物理地址；folio_sz 是必非 NULL 的输出指针，成功查页后写入字节数。
 * 出参/返回：找到 folio 时返回访问布尔值并填写 *folio_sz；找不到时返回 false，输出
 * 保持原值；临时 folio 引用在返回前释放。
 * 注意事项：结果是瞬时、尽力观测而非锁定快照；可睡眠，调用者用旧的 PAGE_SIZE
 * 初值安全处理查页失败。
 */
static bool damon_pa_young(phys_addr_t paddr, unsigned long *folio_sz)
{
	struct folio *folio = damon_get_folio(PHYS_PFN(paddr));
	bool accessed;

	if (!folio)
		return false;

	/* young 查询不消费引用；先保存大小，再在统一出口释放本次查找引用。 */
	accessed = damon_folio_young(folio);
	*folio_sz = folio_size(folio);
	folio_put(folio);
	return accessed;
}

/*
 * __damon_pa_check_access() - 更新一个 region 的本周期访问计数。
 * 业务背景：prepare 已为 sampling_addr 建立基线，本函数在 check 回调中读取 young
 * 结果；若连续区域样本落在同一 folio，则复用查询以减少昂贵的 rmap walk。
 * 入参：r 为借用的可写 region；attrs 为借用的采样属性；addr_unit 为非零字节单位。
 * 出参/返回：无直接返回值；通过 damon_update_region_access_rate() 更新 r 的访问
 * 统计，静态缓存记录最近物理地址、folio 大小和查询结果。
 * 注意事项：三个静态量构成一组顺序缓存，由 DAMON 检查路径按既定串行调用方式使用；
 * 它们不持有 folio 引用，缓存命中只复用布尔快照，不延长页生命周期。
 */
static void __damon_pa_check_access(struct damon_region *r,
		struct damon_attrs *attrs, unsigned long addr_unit)
{
	static phys_addr_t last_addr;
	static unsigned long last_folio_sz = PAGE_SIZE;
	static bool last_accessed;
	phys_addr_t sampling_addr = damon_pa_phys_addr(
			r->sampling_addr, addr_unit);

	/* If the region is in the last checked page, reuse the result */
	/*
	 * 译注：若本区域样本仍落在上次检查的 folio 内，直接复用访问结果；按 folio
	 * 大小对齐而非 PAGE_SIZE，可同时覆盖大 folio，避免对同一对象重复反向映射遍历。
	 */
	if (ALIGN_DOWN(last_addr, last_folio_sz) ==
				ALIGN_DOWN(sampling_addr, last_folio_sz)) {
		damon_update_region_access_rate(r, last_accessed, attrs);
		return;
	}

	/* 缓存未命中才取得 folio 临时引用并刷新大小；随后把同一结果计入当前 region。 */
	last_accessed = damon_pa_young(sampling_addr, &last_folio_sz);
	damon_update_region_access_rate(r, last_accessed, attrs);

	last_addr = sampling_addr;
}

/*
 * damon_pa_check_accesses() - 检查上下文全部采样点并返回最大访问次数。
 * 业务背景：这是 DAMON_OPS_PADDR 的 check_accesses 回调，核心层用最大值归一化
 * 区域热度并继续聚合/分裂及 scheme 匹配。
 * 入参：ctx 是借用且可写的上下文；其 target/region 列表在本轮遍历期间稳定。
 * 出参/返回：返回所有 region 更新后的 nr_accesses 最大值；同时更新各 region 统计。
 * 注意事项：空上下文返回 0；查询可能睡眠，单个无效物理页按未访问处理且不中止全局。
 */
static unsigned int damon_pa_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;

	/* 每个 region 先提交本轮观测，再把更新后的计数折叠为核心层需要的最大值。 */
	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t) {
			__damon_pa_check_access(
					r, &ctx->attrs, ctx->addr_unit);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
		}
	}

	return max_nr_accesses;
}

/*
 * damon_pa_filter_match() - 判断 folio 是否满足一个 probe 过滤条件。
 * 业务背景：probe 命中计数需要在 region 样本之上按匿名属性或 memcg 再分类；本函数
 * 先计算原始 matched，再用 filter->matching 表达正向/反向选择。
 * 入参：filter 为借用且只读的过滤器；folio 为借用指针，可为 NULL，调用者持有引用。
 * 出参/返回：返回条件经 matching 极性修正后的匹配结果，不转移引用、无持久副作用。
 * 注意事项：memcg 指针仅在 RCU 读侧临界区内解引用；RCU 保证生命周期而不冻结成员。
 * 未支持类型及 NULL folio 的原始结果为 false，因此 matching=false 时会反向命中。
 */
static bool damon_pa_filter_match(struct damon_filter *filter,
		struct folio *folio)
{
	bool matched = false;
	struct mem_cgroup *memcg;

	switch (filter->type) {
	case DAMON_FILTER_TYPE_ANON:
		/* 匿名过滤只需观察 folio 属性；页洞先记为“不匹配”。 */
		if (!folio) {
			matched = false;
			break;
		}
		matched = folio_test_anon(folio);
		break;
	case DAMON_FILTER_TYPE_MEMCG:
		/* memcg 归属可并发迁移/离线，借助 RCU 稳定读取到的对象生命周期。 */
		if (!folio) {
			matched = false;
			break;
		}
		rcu_read_lock();
		memcg = folio_memcg_check(folio);
		/* 只在 RCU 窗口内把瞬时 memcg 转成稳定的数值 id，不把裸指针带出。 */
		if (!memcg)
			matched = false;
		else
			matched = filter->memcg_id == mem_cgroup_id(memcg);
		rcu_read_unlock();
		break;
	default:
		/* 物理地址 probe 当前不解释其他类型，保留 false 后统一应用极性。 */
		break;
	}
	return matched == filter->matching;
}

/*
 * damon_pa_filter_pass() - 按顺序规则判定一个样本是否允许计入 probe。
 * 业务背景：probe filters 采用首个匹配规则决定 allow，若均不匹配则沿途形成最后一条
 * 规则的反向默认；apply_probes() 仅对通过者增加命中数。
 * 入参：pa 是当前字节物理地址（接口上下文，当前实现不解引用）；folio 是借用且可空
 * 的临时引用；p 是借用 probe，持有有序 filter 链。
 * 出参/返回：true 允许计数，false 排除；不改变过滤器和 folio ownership。
 * 注意事项：无过滤器时默认 true；遍历期间配置由 DAMON 核心稳定，函数本身不睡眠。
 */
static bool damon_pa_filter_pass(phys_addr_t pa, struct folio *folio,
		struct damon_probe *p)
{
	struct damon_filter *f;
	bool pass = true;

	/* 第一条匹配规则立即裁决；未匹配规则的 allow 反值构成继续扫描时的默认结果。 */
	damon_for_each_filter(f, p) {
		if (damon_pa_filter_match(f, folio)) {
			pass = f->allow;
			break;
		}
		pass = !f->allow;
	}
	return pass;
}

/*
 * damon_pa_apply_probes() - 对每个 region 样本执行全部 probe 并累计命中。
 * 业务背景：访问检查之后，用户可用附加过滤器统计特定匿名页或 memcg 样本；本回调
 * 将 region 的单个采样地址映射为 folio，再与 probe 数组位置一一对应。
 * 入参：ctx 为借用且可写上下文；target/region/probe 列表在核心回调期间稳定。
 * 出参/返回：无直接返回值；通过过滤的 probe 对应 r->probe_hits[i] 加一，临时 folio
 * 引用在处理该 region 后归还。
 * 注意事项：folio 可为 NULL，反向过滤仍可能允许该样本；i 必须与核心分配的命中数组
 * 和 probe 遍历顺序一致，函数不保留任何 page 引用。
 */
static void damon_pa_apply_probes(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	struct damon_probe *p;

	/* target/region 确定样本，probe 是同一 folio 上的多组独立计数规则。 */
	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t) {
			int i = 0;
			phys_addr_t pa;
			struct folio *folio;

			pa = damon_pa_phys_addr(r->sampling_addr,
					ctx->addr_unit);
			folio = damon_get_folio(PHYS_PFN(pa));
			/* i 与 probe 链表的物理顺序同步推进，选择对应的命中槽。 */
			damon_for_each_probe(p, ctx) {
				if (damon_pa_filter_pass(pa, folio, p))
					r->probe_hits[i]++;
				i++;
			}
			if (folio)
				folio_put(folio);
		}
	}
}

/*
 * damos_pa_filter_out - Return true if the page should be filtered out.
 */
/*
 * 译注：返回 true 表示当前 folio 应被本次 DAMOS 动作排除。
 * 业务背景：核心层无法在 region 粒度可靠执行页属性过滤时，由 paddr 操作层逐 folio
 * 应用 ops_filters；若核心已允许全部过滤，则无需重复判断。
 * 入参：scheme 为借用且可写的方案状态；folio 为调用者持有引用的借用 folio，必非 NULL。
 * 出参/返回：首个匹配过滤器返回 !allow；无匹配返回预计算的默认拒绝值，无引用转移。
 * 注意事项：damos_folio_filter_match() 的 young 类型可能复位访问基线，因此过滤不一定
 * 是纯读；规则顺序决定首个命中的裁决，配置在方案执行期间必须稳定。
 */
static bool damos_pa_filter_out(struct damos *scheme, struct folio *folio)
{
	struct damos_filter *filter;

	/* 核心层已经完成可执行的过滤时，操作层不得再次改变裁决。 */
	if (scheme->core_filters_allowed)
		return false;

	damos_for_each_ops_filter(filter, scheme) {
		if (damos_folio_filter_match(filter, folio))
			return !filter->allow;
	}
	return scheme->ops_filters_default_reject;
}

/*
 * damon_pa_invalid_damos_folio() - 拒绝页洞或本 scheme 上次已处理的大 folio。
 * 业务背景：region 边界和步进可能再次落入同一个大 folio；用 last_applied 的地址身份
 * 避免跨 apply_scheme 调用重复动作，同时统一处理无法取得 folio 的物理页。
 * 入参：folio 是 damon_get_folio() 返回的持有引用或 NULL；s 是借用 scheme。
 * 出参/返回：NULL 或地址等于 s->last_applied 时返回 true；重复 folio 的新引用在此释放，
 * 其他 folio 返回 false 并仍由调用者持有。
 * 注意事项：last_applied 只是不可解引用的身份标记，不拥有引用；NULL 路径没有引用可放。
 */
static bool damon_pa_invalid_damos_folio(struct folio *folio, struct damos *s)
{
	if (!folio)
		return true;
	if (folio == s->last_applied) {
		folio_put(folio);
		return true;
	}
	return false;
}

/*
 * damon_pa_pageout() - 过滤、隔离并回收 region 内可回收 folio。
 * 业务背景：DAMOS_PAGEOUT 把区域级冷判断落实到页级回收；默认临时添加 young 过滤器
 * 二次排除近期访问页，再批量交给 reclaim_pages()。
 * 入参：r 为借用 region；addr_unit 为非零每单位字节数；s 为借用且可写 scheme；
 * sz_filter_passed 为必非 NULL 的输入输出计数器，按核心单位累加通过过滤的容量。
 * 出参/返回：返回实际回收页数折算的核心单位；更新过滤计数和 s->last_applied；临时
 * young filter 在返回前销毁，隔离成功的 folio ownership 交给 reclaim_pages()。
 * 注意事项：分配默认过滤器失败返回 0 且无页动作；扫描可睡眠，过滤通过不等于隔离或
 * 回收成功；unevictable folio 立即放回 LRU，避免留在隔离状态。
 */
static unsigned long damon_pa_pageout(struct damon_region *r,
		unsigned long addr_unit, struct damos *s,
		unsigned long *sz_filter_passed)
{
	phys_addr_t addr, applied;
	LIST_HEAD(folio_list);
	bool install_young_filter = true;
	struct damos_filter *filter;
	struct folio *folio = NULL;

	/* check access in page level again by default */
	/*
	 * 译注：默认在真正回收前再做一次页级访问检查；若用户已有 YOUNG 规则，就尊重
	 * 其匹配极性和顺序，不再插入隐式规则。
	 */
	damos_for_each_ops_filter(filter, s) {
		if (filter->type == DAMOS_FILTER_TYPE_YOUNG) {
			install_young_filter = false;
			break;
		}
	}
	if (install_young_filter) {
		/* matching=true 且 allow=false：发现 young folio 时把它排除在回收动作外。 */
		filter = damos_new_filter(
				DAMOS_FILTER_TYPE_YOUNG, true, false);
		if (!filter)
			return 0;
		damos_add_filter(s, filter);
	}

	/* 按 folio 大小前进；页洞只能按基础页跨过，因为此时未知真实 folio 边界。 */
	addr = damon_pa_phys_addr(r->ar.start, addr_unit);
	while (addr < damon_pa_phys_addr(r->ar.end, addr_unit)) {
		folio = damon_get_folio(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_folio(folio, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		/* 操作层过滤器的首个匹配结果决定该 folio 是否进入回收候选。 */
		if (damos_pa_filter_out(s, folio))
			goto put_folio;
		else
			*sz_filter_passed += folio_size(folio) / addr_unit;

		/* 清回收启发信息后尝试从 LRU 隔离；失败只影响本 folio。 */
		folio_clear_referenced(folio);
		folio_test_clear_young(folio);
		if (!folio_isolate_lru(folio))
			goto put_folio;
		if (folio_test_unevictable(folio))
			folio_putback_lru(folio);
		else
			/* 隔离引用支撑 folio 留在批处理链，当前查找引用稍后仍会释放。 */
			list_add(&folio->lru, &folio_list);
put_folio:
		/* 无论过滤/隔离结果如何，都按完整 folio 跨过并归还查找引用。 */
		addr += folio_size(folio);
		folio_put(folio);
	}
	if (install_young_filter)
		damos_destroy_filter(filter);
	/* reclaim_pages() 消费隔离链；其返回页数才是向核心层报告的实际动作量。 */
	applied = reclaim_pages(&folio_list);
	cond_resched();
	s->last_applied = folio;
	return damon_pa_core_addr(applied * PAGE_SIZE, addr_unit);
}

/*
 * damon_pa_de_activate() - 统一执行 region 内 folio 的 LRU 提升或降级。
 * 业务背景：DAMOS_LRU_PRIO/DEPRIO 共享过滤、去重和步进协议，仅最终调用的 LRU
 * 状态转换不同，因此由两个薄 wrapper 传入 activate 选择。
 * 入参：r 为借用 region；addr_unit 为非零字节单位；s 为借用且可写 scheme；
 * activate=true 提升、false 降级；sz_filter_passed 为必非 NULL 的输入输出容量计数。
 * 出参/返回：返回已调用 LRU 动作的 folio 页数折算值；更新过滤计数及 last_applied，
 * 每次 damon_get_folio() 引用均在迭代内释放。
 * 注意事项：LRU helper 可能只排队或受当前 folio 状态影响，返回量表示尝试覆盖量而非
 * 后续驻留保证；扫描可调度，last_applied 只作地址身份标记。
 */
static inline unsigned long damon_pa_de_activate(
		struct damon_region *r, unsigned long addr_unit,
		struct damos *s, bool activate,
		unsigned long *sz_filter_passed)
{
	phys_addr_t addr, applied = 0;
	struct folio *folio = NULL;

	/* 与 pageout 同样按真实 folio 大小跳步，避免大 folio 被重复提升或降级。 */
	addr = damon_pa_phys_addr(r->ar.start, addr_unit);
	while (addr < damon_pa_phys_addr(r->ar.end, addr_unit)) {
		folio = damon_get_folio(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_folio(folio, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		/* 被排除者只归还引用；通过者才计入容量并改变 LRU 状态。 */
		if (damos_pa_filter_out(s, folio))
			goto put_folio;
		else
			*sz_filter_passed += folio_size(folio) / addr_unit;

		/* 过滤通过后才改变 LRU 代际/活跃状态，并以 folio 基础页数计量。 */
		if (activate)
			folio_activate(folio);
		else
			folio_deactivate(folio);
		applied += folio_nr_pages(folio);
put_folio:
		/* 被过滤的 folio 也按其完整大小前进；查找引用不跨迭代保留。 */
		addr += folio_size(folio);
		folio_put(folio);
	}
	s->last_applied = folio;
	return damon_pa_core_addr(applied * PAGE_SIZE, addr_unit);
}

/*
 * damon_pa_activate_pages() - 将 LRU_PRIO 动作适配到共享升降级实现。
 * 业务背景：apply_scheme 的热页分支通过本 wrapper 固定 activate=true。
 * 入参：r、s 均为借用对象；addr_unit 是字节单位；sz_filter_passed 是输入输出计数器。
 * 出参/返回：返回共享 helper 报告的核心单位动作量，并继承其过滤和状态副作用。
 * 注意事项：不额外持锁或引用，睡眠与失败语义完全由 damon_pa_de_activate() 决定。
 */
static unsigned long damon_pa_activate_pages(struct damon_region *r,
		unsigned long addr_unit, struct damos *s,
		unsigned long *sz_filter_passed)
{
	return damon_pa_de_activate(r, addr_unit, s, true, sz_filter_passed);
}

/*
 * damon_pa_deactivate_pages() - 将 LRU_DEPRIO 动作适配到共享升降级实现。
 * 业务背景：apply_scheme 的冷页分支通过本 wrapper 固定 activate=false。
 * 入参：r、s 均为借用对象；addr_unit 是字节单位；sz_filter_passed 是输入输出计数器。
 * 出参/返回：返回共享 helper 报告的核心单位动作量，并继承其过滤和状态副作用。
 * 注意事项：不额外持锁或引用，降级是回收提示而非立即回收保证。
 */
static unsigned long damon_pa_deactivate_pages(struct damon_region *r,
		unsigned long addr_unit, struct damos *s,
		unsigned long *sz_filter_passed)
{
	return damon_pa_de_activate(r, addr_unit, s, false, sz_filter_passed);
}

/*
 * damon_pa_migrate() - 隔离 region 内通过过滤的 folio 并迁往目标 NUMA 节点。
 * 业务背景：冷热迁移 scheme 共享同一物理页动作，目标节点由 s->target_nid 指定；
 * 本函数负责候选收集，damon_migrate_pages() 负责分配目标页和迁移/放回失败页。
 * 入参：r 为借用 region；addr_unit 为非零字节单位；s 为借用且可写 scheme；
 * sz_filter_passed 为必非 NULL 的输入输出容量计数。
 * 出参/返回：返回成功迁移页数折算的核心单位；更新过滤计数和 last_applied；隔离链
 * ownership 在调用 damon_migrate_pages() 时转移给迁移 helper。
 * 注意事项：通过过滤或成功隔离都不保证迁移成功；该路径可睡眠并主动 cond_resched()，
 * damon_migrate_pages() 必须处理未迁移 folio 的 LRU 恢复。
 */
static unsigned long damon_pa_migrate(struct damon_region *r,
		unsigned long addr_unit, struct damos *s,
		unsigned long *sz_filter_passed)
{
	phys_addr_t addr, applied;
	LIST_HEAD(folio_list);
	struct folio *folio = NULL;

	/* 第一阶段只收集候选，按 folio 大小跳步并保持隔离链的引用契约。 */
	addr = damon_pa_phys_addr(r->ar.start, addr_unit);
	while (addr < damon_pa_phys_addr(r->ar.end, addr_unit)) {
		folio = damon_get_folio(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_folio(folio, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		/* 过滤通过量与迁移成功量分开记账，便于识别隔离或迁移失败。 */
		if (damos_pa_filter_out(s, folio))
			goto put_folio;
		else
			*sz_filter_passed += folio_size(folio) / addr_unit;

		if (!folio_isolate_lru(folio))
			goto put_folio;
		/* 成功隔离后链表持有其隔离状态，普通查找引用可在本轮末释放。 */
		list_add(&folio->lru, &folio_list);
put_folio:
		addr += folio_size(folio);
		folio_put(folio);
	}
	/* 第二阶段批量迁移到 scheme 目标节点，并由 helper 收束成功与失败候选。 */
	applied = damon_migrate_pages(&folio_list, s->target_nid);
	cond_resched();
	s->last_applied = folio;
	return damon_pa_core_addr(applied * PAGE_SIZE, addr_unit);
}

/*
 * damon_pa_stat() - 仅统计 region 内通过 ops 过滤器的容量。
 * 业务背景：DAMOS_STAT 本身不改变页状态；只有存在操作层过滤时才需逐 folio 计算
 * sz_filter_passed，否则核心层已有的 region 统计足够。
 * 入参：r 为借用 region；addr_unit 为非零字节单位；s 为借用且可写 scheme；
 * sz_filter_passed 为必非 NULL 的输入输出计数器。
 * 出参/返回：始终返回 0，表示没有实际页动作；可能累计过滤通过量并更新 last_applied。
 * 注意事项：不持久持有 folio 引用；无 ops filter 快速返回且不改 last_applied，扫描
 * 结果是并发内存状态的瞬时统计。
 */
static unsigned long damon_pa_stat(struct damon_region *r,
		unsigned long addr_unit, struct damos *s,
		unsigned long *sz_filter_passed)
{
	phys_addr_t addr;
	struct folio *folio = NULL;

	/* 无页级过滤时没有额外统计信息可补充，避免无意义遍历整段物理区间。 */
	if (!damos_ops_has_filter(s))
		return 0;

	addr = damon_pa_phys_addr(r->ar.start, addr_unit);
	/* 有过滤器时按 folio 粒度累加通过容量，但绝不执行 LRU、回收或迁移动作。 */
	while (addr < damon_pa_phys_addr(r->ar.end, addr_unit)) {
		folio = damon_get_folio(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_folio(folio, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		/* STAT 只计数：无论过滤结果如何，本轮都不隔离也不改 LRU。 */
		if (!damos_pa_filter_out(s, folio))
			*sz_filter_passed += folio_size(folio) / addr_unit;
		addr += folio_size(folio);
		folio_put(folio);
	}
	s->last_applied = folio;
	return 0;
}

/*
 * damon_pa_apply_scheme() - 把 DAMOS 动作枚举分派到物理地址实现。
 * 业务背景：DAMON 核心完成 region 匹配和配额选择后进入该操作集提交动作；本函数是
 * 抽象 scheme 与 folio 回收、LRU、迁移或统计之间的状态转换边界。
 * 入参：ctx 为借用上下文并提供地址单位；t 为借用 target（物理实现当前无需其字段）；
 * r 为借用候选 region；scheme 为借用且可写方案；sz_filter_passed 为输入输出容量计数。
 * 出参/返回：返回各动作实际处理量的核心单位；不支持动作返回 0；具体 helper 可更新
 * scheme->last_applied、过滤统计及 folio/LRU/NUMA 状态。
 * 注意事项：调用可睡眠；返回 0 既可能表示不支持，也可能表示无候选、分配失败或动作
 * 未成功，调用者应结合 action 与统计解释。
 */
static unsigned long damon_pa_apply_scheme(struct damon_ctx *ctx,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme, unsigned long *sz_filter_passed)
{
	unsigned long aunit = ctx->addr_unit;

	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		/* 冷页回收：页级复核、隔离后交给 reclaim_pages()。 */
		return damon_pa_pageout(r, aunit, scheme, sz_filter_passed);
	case DAMOS_LRU_PRIO:
		/* 热页提升：通过过滤的 folio 进入活跃侧，改善近期访问命中。 */
		return damon_pa_activate_pages(r, aunit, scheme,
				sz_filter_passed);
	case DAMOS_LRU_DEPRIO:
		/* 冷页降级：把候选移向更易回收的 LRU 状态，但不立即释放。 */
		return damon_pa_deactivate_pages(r, aunit, scheme,
				sz_filter_passed);
	case DAMOS_MIGRATE_HOT:
		/* 热迁移与冷迁移的动作相同，差异已体现在匹配和目标节点配置。 */
	case DAMOS_MIGRATE_COLD:
		/* 两个枚举在此汇合，统一隔离并迁往 scheme->target_nid。 */
		return damon_pa_migrate(r, aunit, scheme, sz_filter_passed);
	case DAMOS_STAT:
		/* 统计动作只补充页级过滤通过量，不改变 folio 状态。 */
		return damon_pa_stat(r, aunit, scheme, sz_filter_passed);
	default:
		/* DAMOS actions that not yet supported by 'paddr'. */
		/* 译注：paddr 尚未实现的 DAMOS 动作在此保持无操作，由 0 表示未处理容量。 */
		break;
	}
	return 0;
}

/*
 * damon_pa_scheme_score() - 为物理地址 scheme 计算 region 的配额优先级。
 * 业务背景：当 scheme 配额不足以覆盖所有匹配 region 时，核心层调用本回调排序；
 * 回收/降级/冷迁移偏好冷区，提升/热迁移偏好热区。
 * 入参：context 为借用监测上下文；r 为借用 region；scheme 为借用方案，均只读使用。
 * 出参/返回：受支持动作返回 0..DAMOS_MAX_SCORE 的冷热评分；其他动作返回最大值，
 * 不修改 region、scheme 或引用。
 * 注意事项：评分依赖当前聚合访问统计，是配额选择提示而非动作成功保证；不会睡眠。
 */
static int damon_pa_scheme_score(struct damon_ctx *context,
		struct damon_region *r, struct damos *scheme)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		/* 回收优先选择更冷且更符合 scheme 条件的 region。 */
		return damon_cold_score(context, r, scheme);
	case DAMOS_LRU_PRIO:
		/* LRU 提升优先选择近期更热的 region。 */
		return damon_hot_score(context, r, scheme);
	case DAMOS_LRU_DEPRIO:
		/* LRU 降级与回收一样使用冷度评分。 */
		return damon_cold_score(context, r, scheme);
	case DAMOS_MIGRATE_HOT:
		/* 热迁移优先处理访问更频繁的 region。 */
		return damon_hot_score(context, r, scheme);
	case DAMOS_MIGRATE_COLD:
		/* 冷迁移优先处理访问更少的 region。 */
		return damon_cold_score(context, r, scheme);
	default:
		/* STAT 或未知动作无需冷热区分，使用最大分值保持通用语义。 */
		break;
	}

	return DAMOS_MAX_SCORE;
}

/*
 * damon_pa_initcall() - 注册 DAMON 物理地址空间操作表。
 * 业务背景：subsys_initcall 阶段把 PADDR id 与本文件回调发布给 DAMON 核心，之后
 * 用户配置物理监测上下文时才能按 id 选择该实现。
 * 入参：无。
 * 出参/返回：返回 damon_register_ops() 的 0 或负 errno；成功后核心持有操作表内容
 * 的注册副本/记录，本栈上 ops 不需要调用者释放。
 * 注意事项：运行于内核初始化进程上下文、允许注册路径所需睡眠；init/update/
 * target_valid 为 NULL 表示沿用核心通用行为，注册失败则该操作集不可选择。
 */
static int __init damon_pa_initcall(void)
{
	/* 操作表把采样、probe、动作和评分阶段连接成完整 PADDR 后端。 */
	struct damon_operations ops = {
		.id = DAMON_OPS_PADDR,
		.init = NULL,
		.update = NULL,
		.prepare_access_checks = damon_pa_prepare_access_checks,
		.check_accesses = damon_pa_check_accesses,
		/* probe 与 scheme 回调共享 region 样本，但分别维护命中和动作统计。 */
		.apply_probes = damon_pa_apply_probes,
		.target_valid = NULL,
		.apply_scheme = damon_pa_apply_scheme,
		.get_scheme_score = damon_pa_scheme_score,
	};

	return damon_register_ops(&ops);
};

/* 子系统初始化阶段完成注册，早于依赖 DAMON 物理操作集的普通模块启动。 */
subsys_initcall(damon_pa_initcall);
