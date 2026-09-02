// SPDX-License-Identifier: GPL-2.0
/*
 * Common Code for Data Access Monitoring
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/* 译注：本文件实现各 DAMON 地址空间后端共享的数据访问监测、过滤、评分与迁移操作。 */

#include <linux/migrate.h>
#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/leafops.h>

#include "../internal.h"
#include "ops-common.h"

/*
 * 学习提示：这些公共操作把页表访问位、folio idle 位和设备 MMU 通知合并为访问样本。
 * 采样、过滤和迁移均采用尽力而为语义，竞争失败时跳过而不阻塞 DAMON 主循环。
 */
/*
 * Get an online page for a pfn if it's in the LRU list.  Otherwise, returns
 * NULL.
 *
 * The body of this function is stolen from the 'page_idle_get_folio()'.  We
 * steal rather than reuse it because the code is quite simple.
 */
/*
 * 译注：按 PFN 取得在线且位于 LRU 的页面，否则返回 NULL；函数体取自 page_idle_get_folio()，
 * 因逻辑很简单所以复制而未复用。
 */
/*
 * 业务背景：访问采样由裸 PFN 起步，需要先取得稳定 folio 引用并排除内存洞、离线页与非 LRU 对象。
 * 入参：pfn 是基础页帧号，可能无效或正与 compound split/迁移竞争。
 * 出参/返回：成功返回调用者拥有一份 folio 引用；失败 NULL；调用者必须 folio_put。
 * 注意事项：尽力 try_get 后重验 page→folio 与 LRU，不加页锁，不保证返回后仍在 LRU。
 */
struct folio *damon_get_folio(unsigned long pfn)
{
	/* online 转换先拒绝洞和下线内存，随后引用稳定 folio。 */
	struct page *page = pfn_to_online_page(pfn);
	struct folio *folio;

	if (!page)
		return NULL;

	folio = page_folio(page);
	/* try_get 不复活已归零对象，失败直接视作不可采样。 */
	if (!folio_try_get(folio))
		return NULL;
	/* 加引用后重验复合页归属和 LRU 身份，闭合 split/迁移竞争窗口。 */
	if (unlikely(page_folio(page) != folio) || !folio_test_lru(folio)) {
		folio_put(folio);
		folio = NULL;
	}
	return folio;
}

/*
 * 业务背景：DAMON 新采样周期清除单页 CPU/设备 young 证据并设置 folio idle 基线。
 * 入参：pte/vma 借用且由调用页表锁稳定；addr 是该 PTE 映射起始地址。
 * 出参/返回：void；若页可取引用则清 CPU/notifier young、保存旧 young 到 folio并置 idle。
 * 注意事项：PFN softleaf 只由 notifier 覆盖设备访问；失败静默跳过，folio 引用在返回前释放。
 */
void damon_ptep_mkold(pte_t *pte, struct vm_area_struct *vma, unsigned long addr)
{
	/* present 与 PFN softleaf 都可能指向真实页，先统一提取 PFN。 */
	pte_t pteval = ptep_get(pte);
	struct folio *folio;
	bool young = false;
	unsigned long pfn;

	if (likely(pte_present(pteval)))
		pfn = pte_pfn(pteval);
	else
		pfn = softleaf_to_pfn(softleaf_from_pte(pteval));

	/* folio 引用覆盖清访问位和设置 idle 的整个观察窗口。 */
	folio = damon_get_folio(pfn);
	if (!folio)
		return;

	/*
	 * PFN swap PTEs, such as device-exclusive ones, that actually map pages
	 * are "old" from a CPU perspective. The MMU notifier takes care of any
	 * device aspects.
	 */
	/* CPU young 位只对 present PTE 有意义，设备访问由 notifier 补充。 */
	if (likely(pte_present(pteval)))
		young |= ptep_test_and_clear_young(vma, addr, pte);
	young |= mmu_notifier_clear_young(vma->vm_mm, addr, addr + PAGE_SIZE);
	/* 任一观察者报告 young 就保存在 folio 位，避免清位时丢失本轮访问信息。 */
	if (young)
		folio_set_young(folio);

	/* 设置 idle 建立下一采样周期的基线，后续访问会将其打破。 */
	folio_set_idle(folio);
	folio_put(folio);
}

/*
 * 业务背景：THP PMD 映射以大页范围执行与 PTE 同构的 young 清除和 idle 基线建立。
 * 入参：pmd/vma 借用且页表锁稳定；addr 是 PMD 映射起点。
 * 出参/返回：void；THP 配置下更新 CPU/MMU-notifier/folio 状态，无 THP 时为空操作。
 * 注意事项：notifier 范围为 HPAGE_PMD_SIZE；try_get 失败不阻塞拆分/迁移。
 */
void damon_pmdp_mkold(pmd_t *pmd, struct vm_area_struct *vma, unsigned long addr)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	/* PMD 路径与 PTE 路径同构，但通知范围覆盖整个 HPAGE_PMD_SIZE。 */
	pmd_t pmdval = pmdp_get(pmd);
	struct folio *folio;
	bool young = false;
	unsigned long pfn;

	if (likely(pmd_present(pmdval)))
		pfn = pmd_pfn(pmdval);
	else
		pfn = softleaf_to_pfn(softleaf_from_pmd(pmdval));

	/* THP 也通过头 folio 引用稳定，失败时不等待拆分完成。 */
	folio = damon_get_folio(pfn);
	if (!folio)
		return;

	if (likely(pmd_present(pmdval)))
		young |= pmdp_test_and_clear_young(vma, addr, pmd);
	/* notifier 覆盖设备页表，防止只看 CPU PMD 而低估访问。 */
	young |= mmu_notifier_clear_young(vma->vm_mm, addr, addr + HPAGE_PMD_SIZE);
	if (young)
		folio_set_young(folio);

	/* 即使本轮未观察到 young，也要设置 idle 作为下一轮参照。 */
	folio_set_idle(folio);
	folio_put(folio);
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
}

#define DAMON_MAX_SUBSCORE	(100)
/* 年龄对数上限限制极老区域对加权结果的支配程度。 */
#define DAMON_MAX_AGE_IN_LOG	(32)

/*
 * 业务背景：DAMOS quota 排序把区域访问频率与持续年龄归一成统一热度，用权重决定优先处理顺序。
 * 入参：c/r/s 均为监测线程内稳定借用对象；attrs、样本、age 和 quota 权重只读。
 * 出参/返回：返回夹紧到 0..DAMOS_MAX_SCORE 的整数；无状态/ownership 变化。
 * 注意事项：零频率时年龄方向翻转表示越久越冷；权重全零返回0，聚合间隔单位为微秒。
 */
int damon_hot_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s)
{
	/* 热度由访问频率和持续年龄两个可配权重子分数组成。 */
	int freq_subscore;
	unsigned int age_in_sec;
	int age_in_log, age_subscore;
	unsigned int freq_weight = s->quota.weight_nr_accesses;
	unsigned int age_weight = s->quota.weight_age;
	int hotness;

	/* 频率按当前监测属性允许的最大采样次数归一化到 0..100。 */
	freq_subscore = r->nr_accesses * DAMON_MAX_SUBSCORE /
		damon_max_nr_accesses(&c->attrs);

	/* region age 是聚合轮数，先换算秒再取对数压缩长尾。 */
	age_in_sec = (unsigned long)r->age * c->attrs.aggr_interval / 1000000;
	if (age_in_sec)
		age_in_log = min_t(int, ilog2(age_in_sec) + 1,
				DAMON_MAX_AGE_IN_LOG);
	else
		age_in_log = 0;


	/* If frequency is 0, higher age means it's colder */
	/* 译注：访问频率为零时，age 越高表示区域越冷。 */
	/* 从未访问时年龄越大越冷，因此将对数年龄翻转为负方向。 */
	if (freq_subscore == 0)
		age_in_log *= -1;

	/*
	 * Now age_in_log is in [-DAMON_MAX_AGE_IN_LOG, DAMON_MAX_AGE_IN_LOG].
	 * Scale it to be in [0, 100] and set it as age subscore.
	 */
	/* 译注：此时 age_in_log 位于正负上限之间，把它线性缩放到 0..100 作为年龄子分。 */
	age_in_log += DAMON_MAX_AGE_IN_LOG;
	age_subscore = age_in_log * DAMON_MAX_SUBSCORE /
		DAMON_MAX_AGE_IN_LOG / 2;

	/* 权重和为零时保留零值，避免除零并表示没有热度偏好。 */
	hotness = (freq_weight * freq_subscore + age_weight * age_subscore);
	if (freq_weight + age_weight)
		hotness /= freq_weight + age_weight;
	/*
	 * Transform it to fit in [0, DAMOS_MAX_SCORE]
	 */
	/* 译注：把组合结果转换到 DAMOS 公共的 0..DAMOS_MAX_SCORE 分值域。 */
	/* 最后映射到 DAMOS 公共分值域并夹紧舍入边界。 */
	hotness = hotness * DAMOS_MAX_SCORE / DAMON_MAX_SUBSCORE;
	hotness = max(min(hotness, DAMOS_MAX_SCORE), 0);

	return hotness;
}

/*
 * 业务背景：冷优先 DAMOS 动作需要与热度同权重、同尺度的互补排序值。
 * 入参：c/r/s 借用，语义与 hot_score 相同。
 * 出参/返回：返回 DAMOS_MAX_SCORE-hotness 的冷度；无副作用。
 * 注意事项：继承 hot_score 的归一化/零权重边界，分值越高表示越冷。
 */
int damon_cold_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s)
{
	/* 冷度与热度互补，沿用相同权重和边界。 */
	int hotness = damon_hot_score(c, r, s);

	/* Return coldness of the region */
	/* 译注：返回该区域的冷度分值。 */
	return DAMOS_MAX_SCORE - hotness;
}

/*
 * 业务背景：rmap_walk 对 folio 的每个 VMA 映射回调，逐 PTE/PMD 建立下一轮访问采样基线。
 * 入参：folio/vma 借用且外层持 folio 锁；addr 是起始地址；arg 未使用。
 * 出参/返回：处理完当前 VMA 返回 true，允许 rmap 继续；会清 young/设置 idle。
 * 注意事项：page_vma_mapped_walk 自行管理页表锁，PTE/PMD 粒度不可混淆。
 */
static bool damon_folio_mkold_one(struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr, void *arg)
{
	/* rmap walk 枚举 folio 的所有映射，每个映射独立清除访问证据。 */
	DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, addr, 0);

	/* pte/pmd 二选一由映射 walk 给出，调用相应粒度的 mkold。 */
	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte)
			damon_ptep_mkold(pvmw.pte, vma, addr);
		else
			damon_pmdp_mkold(pvmw.pmd, vma, addr);
	}
	/* true 允许 rmap 框架继续枚举该 folio 的其他 VMA 映射。 */
	return true;
}

/*
 * 业务背景：DAMOS young 过滤或监测初始化清理 folio 所有映射访问位并设置 idle 基线。
 * 入参：folio 是有引用的借用对象，可能 mapped/unmapped；函数不消费引用。
 * 出参/返回：void；无映射直接置 idle，mapped 时 trylock 成功才 rmap walk。
 * 注意事项：锁竞争静默跳过以保持低侵入；匿名 VMA 锁由 rwc 回调获取，返回前解 folio 锁。
 */
void damon_folio_mkold(struct folio *folio)
{
	/* anon_lock 回调让匿名 folio 的反向映射遍历获得正确保护。 */
	struct rmap_walk_control rwc = {
		.rmap_one = damon_folio_mkold_one,
		.anon_lock = folio_lock_anon_vma_read,
	};

	/* 无可遍历映射时只设置 idle，下一轮靠该位判断访问。 */
	if (!folio_mapped(folio) || !folio_raw_mapping(folio)) {
		folio_set_idle(folio);
		return;
	}

	/* 监测不能阻塞业务线程；锁竞争时留待以后采样。 */
	if (!folio_trylock(folio))
		return;

	/* folio 锁覆盖 rmap walk，完成后立即释放以缩短扰动。 */
	rmap_walk(folio, &rwc);
	folio_unlock(folio);

}

/*
 * 业务背景：rmap young 查询合并 CPU PTE/PMD、folio idle 与设备 MMU-notifier 三类访问证据。
 * 入参：folio/vma 借用且外层持 folio 锁；addr 起点；arg 指向调用者栈上 bool 输出。
 * 出参/返回：发现访问写 true并返回 false停止 rmap；未发现写 false并返回 true继续。
 * 注意事项：PFN softleaf 不看 CPU young；THP 关闭却遇 PMD 会 WARN，walk_done 负责提前解锁。
 */
static bool damon_folio_young_one(struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr, void *arg)
{
	/* accessed 是跨映射的短路结果，发现一次访问即可停止。 */
	bool *accessed = arg;
	DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, addr, 0);
	pte_t pte;

	/* 每个 rmap 回调先清本次结果，再合并页表、idle 和设备证据。 */
	*accessed = false;
	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte) {
			pte = ptep_get(pvmw.pte);

			/*
			 * PFN swap PTEs, such as device-exclusive ones, that
			 * actually map pages are "old" from a CPU perspective.
			 * The MMU notifier takes care of any device aspects.
			 */
			/* PFN softleaf 不读取 CPU young，但仍检查 folio idle 与设备 MMU。 */
			*accessed = (pte_present(pte) && pte_young(pte)) ||
				!folio_test_idle(folio) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
		} else {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			pmd_t pmd = pmdp_get(pvmw.pmd);

			/* THP 的 PMD young 与 notifier 共同覆盖 CPU/设备访问源。 */
			*accessed = (pmd_present(pmd) && pmd_young(pmd)) ||
				!folio_test_idle(folio) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
#else
			WARN_ON_ONCE(1);
#endif	/* CONFIG_TRANSPARENT_HUGEPAGE */
		}
		/* 显式结束映射 walk，避免发现访问后继续扫描其余 PTE。 */
		if (*accessed) {
			page_vma_mapped_walk_done(&pvmw);
			break;
		}
	}

	/* If accessed, stop walking */
	/* 译注：一旦发现访问即返回 false，停止继续枚举其他反向映射。 */
	return *accessed == false;
}

/*
 * 业务背景：DAMON 采样/过滤以尽力方式判断 folio 自上次 mkold 后是否被 CPU 或设备访问。
 * 入参：folio 是有引用借用对象，不消费 ownership。
 * 出参/返回：观察到访问返回 true；无证据、idle 或锁竞争返回 false；不清访问证据。
 * 注意事项：mapped 对象只 trylock，false 可能是采样保守退化；无 mapping 时仅依据 idle 位。
 */
bool damon_folio_young(struct folio *folio)
{
	/* 返回值是本轮观察结果，不承诺锁竞争下绝对准确。 */
	bool accessed = false;
	struct rmap_walk_control rwc = {
		.arg = &accessed,
		.rmap_one = damon_folio_young_one,
		.anon_lock = folio_lock_anon_vma_read,
	};

	/* 无映射对象退化为读取 idle 位：非 idle 即认为被访问。 */
	if (!folio_mapped(folio) || !folio_raw_mapping(folio)) {
		if (folio_test_idle(folio))
			return false;
		else
			return true;
	}

	/* trylock 失败返回冷的保守采样结果，保持监测低侵入。 */
	if (!folio_trylock(folio))
		return false;

	rmap_walk(folio, &rwc);
	folio_unlock(folio);

	return accessed;
}

/*
 * 业务背景：DAMOS 执行动作前把统一 filter 类型解释为 folio 条件，并应用 matching 正选/反选。
 * 入参：filter/folio 借用；filter union 字段须与 type 匹配，folio 生命周期由调用者稳定。
 * 出参/返回：最终条件等于 matching 返回 true；YOUNG 命中还会 mkold，其他类型只观察。
 * 注意事项：MEMCG 指针只在 RCU 内解引用；未知 type 的正向 matched=false，再统一反选。
 */
bool damos_folio_filter_match(struct damos_filter *filter, struct folio *folio)
{
	/* 先计算正向条件，结尾再统一应用 matching 反选语义。 */
	bool matched = false;
	struct mem_cgroup *memcg;
	/* folio_sz 仅由大小过滤器初始化和消费。 */
	size_t folio_sz;

	switch (filter->type) {
	case DAMOS_FILTER_TYPE_ANON:
		/* ANON：进入条件为匿名性过滤；读取 folio flag，不改状态，随后统一应用 matching。 */
		matched = folio_test_anon(folio);
		break;
	case DAMOS_FILTER_TYPE_ACTIVE:
		/* ACTIVE：按 LRU active flag 快照匹配，不取得锁，随后统一反选。 */
		matched = folio_test_active(folio);
		break;
	case DAMOS_FILTER_TYPE_MEMCG:
		/* MEMCG：RCU 内读 folio memcg id；无 memcg 为 false，退出 RCU 后统一反选。 */
		rcu_read_lock();
		memcg = folio_memcg_check(folio);
		if (!memcg)
			matched = false;
		else
			matched = filter->memcg_id == mem_cgroup_id(memcg);
		/* memcg 对象仅在这对 RCU 临界区内保证可解引用。 */
		rcu_read_unlock();
		break;
	case DAMOS_FILTER_TYPE_YOUNG:
		/* YOUNG：查询访问证据；命中后立即 mkold 建下轮基线，再统一应用 matching。 */
		matched = damon_folio_young(folio);
		if (matched)
			damon_folio_mkold(folio);
		break;
	case DAMOS_FILTER_TYPE_HUGEPAGE_SIZE:
		/* HUGEPAGE_SIZE：folio 字节大小落在闭区间即匹配，普通页也参与，随后统一反选。 */
		folio_sz = folio_size(folio);
		matched = filter->sz_range.min <= folio_sz &&
			  folio_sz <= filter->sz_range.max;
		break;
	case DAMOS_FILTER_TYPE_UNMAPPED:
		/* UNMAPPED：无 PTE 映射或无 raw mapping 即匹配，只观察状态。 */
		matched = !folio_mapped(folio) || !folio_raw_mapping(folio);
		break;
	default:
		/* 未支持类型：保持 false；matching=false 时统一反选会得到 true。 */
		break;
	}

	/* matching=false 将任意支持的过滤器反转，避免各 case 重复逻辑。 */
	return matched == filter->matching;
}

static unsigned int __damon_migrate_folio_list(
		struct list_head *migrate_folios, struct pglist_data *pgdat,
		int target_nid)
{
	/*
	 * 业务背景：同一源节点的已隔离 folio 需要一次性提交给通用迁移器，供 DAMOS 改变其 NUMA 归属。
	 * 入参：migrate_folios 由本层借用且含隔离 folio；pgdat 标识源节点；target_nid 标识目标节点。
	 * 出参/返回：返回成功迁移的基础页数；失败 folio 仍留在输入链表，函数不负责 putback。
	 * 注意事项：目标分配快速失败且禁止 reclaim/储备，MIGRATE_ASYNC 忽略 cpuset 与 mempolicy。
	 */
	/* 迁移目标分配禁止 reclaim/储备内存，优先快速失败而非递归回收。 */
	unsigned int nr_succeeded = 0;
	struct migration_target_control mtc = {
		/*
		 * Allocate from 'node', or fail quickly and quietly.
		 * When this happens, 'page' will likely just be discarded
		 * instead of migrated.
		 */
		/* 译注：只从目标节点分配；分配会快速、静默失败，此时页面通常放弃迁移。 */
		.gfp_mask = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) |
			__GFP_NOMEMALLOC | GFP_NOWAIT,
		.nid = target_nid,
	};

	/* 同节点、无目标或空列表都无需进入昂贵的迁移框架。 */
	if (pgdat->node_id == target_nid || target_nid == NUMA_NO_NODE)
		return 0;

	if (list_empty(migrate_folios))
		return 0;

	/* Migration ignores all cpuset and mempolicy settings */
	/* 译注：该迁移路径忽略全部 cpuset 与内存策略设置。 */
	/* 异步模式忽略 cpuset/mempolicy，这是 DAMON 动作明确选择的全局策略。 */
	migrate_pages(migrate_folios, alloc_migration_target, NULL,
		      (unsigned long)&mtc, MIGRATE_ASYNC, MR_DAMON,
		      &nr_succeeded);

	return nr_succeeded;
}

static unsigned int damon_migrate_folio_list(struct list_head *folio_list,
						struct pglist_data *pgdat,
						int target_nid)
{
	/*
	 * 业务背景：DAMON 批次在一个源节点内筛出可锁 folio，迁移后把跳过项和失败项统一归还 LRU。
	 * 入参：folio_list 传入隔离 folio 的所有权；pgdat 是其源节点；target_nid 是合法目标节点。
	 * 出参/返回：返回成功迁移的基础页数；返回时消费并清空 folio_list，所有剩余 folio 已 putback。
	 * 注意事项：trylock 失败仅跳过本轮；migrate_pages 留下的失败项仍须 flush unmap 并 putback。
	 */
	/* 输入按源节点分组；本层负责锁筛选、迁移和所有 folio 的 LRU 回放。 */
	unsigned int nr_migrated = 0;
	struct folio *folio;
	LIST_HEAD(ret_folios);
	LIST_HEAD(migrate_folios);

	/* 逐项移出输入，确保循环结束时所有权只在两个临时链表。 */
	while (!list_empty(folio_list)) {
		struct folio *folio;

		cond_resched();

		folio = lru_to_folio(folio_list);
		list_del(&folio->lru);

		/* 锁竞争对象进入 ret_folios，不参与本轮迁移。 */
		if (!folio_trylock(folio))
			goto keep;

		/* Relocate its contents to another node. */
		/* 译注：把 folio 内容迁移到另一个节点。 */
		/* migrate_pages 自行重新锁定，加入列表前即释放筛选锁。 */
		list_add(&folio->lru, &migrate_folios);
		folio_unlock(folio);
		continue;
keep:
		list_add(&folio->lru, &ret_folios);
	}
	/* 'folio_list' is always empty here */
	/* 译注：此处 folio_list 必然为空，元素均已转入 migrate_folios 或 ret_folios。 */

	/* Migrate folios selected for migration */
	/* 译注：提交筛选出的 folio 进行迁移。 */
	/* 失败项仍留在 migrate_folios，必须与跳过项一起归还 LRU。 */
	nr_migrated += __damon_migrate_folio_list(
			&migrate_folios, pgdat, target_nid);
	/*
	 * Folios that could not be migrated are still in @migrate_folios.  Add
	 * those back on @folio_list
	 */
	/* 译注：未迁移成功的 folio 仍在 migrate_folios，把它们接回 folio_list。 */
	if (!list_empty(&migrate_folios))
		list_splice_init(&migrate_folios, folio_list);

	/* 批量兑现迁移造成的延迟 unmap，再逐个 putback_lru。 */
	try_to_unmap_flush();

	list_splice(&ret_folios, folio_list);

	/* 最终消费所有临时所有权，返回时调用者链表为空。 */
	while (!list_empty(folio_list)) {
		folio = lru_to_folio(folio_list);
		list_del(&folio->lru);
		folio_putback_lru(folio);
	}

	return nr_migrated;
}

unsigned long damon_migrate_pages(struct list_head *folio_list, int target_nid)
{
	/*
	 * 业务背景：DAMOS NUMA 动作接收可能混合源节点的隔离 folio，按源 pgdat 分批迁往同一目标节点。
	 * 入参：folio_list 在有效目标时把元素所有权交给函数；target_nid 是期望目标节点号。
	 * 出参/返回：返回成功迁移的基础页数；有效请求返回时列表为空，非法目标时列表保持原样。
	 * 注意事项：整个流程置 memalloc_noreclaim；分组依赖当前 folio_nid，最后严格恢复调用线程标志。
	 */
	/* 顶层按源 NUMA 节点切分，保证每次传入正确 pgdat。 */
	int nid;
	unsigned long nr_migrated = 0;
	LIST_HEAD(node_folio_list);
	unsigned int noreclaim_flag;

	if (list_empty(folio_list))
		return nr_migrated;

	/* 目标必须合法且拥有内存，非法请求保持输入不变。 */
	if (target_nid < 0 || target_nid >= MAX_NUMNODES ||
			!node_state(target_nid, N_MEMORY))
		return nr_migrated;

	/* 整轮禁止直接 reclaim，防止迁移目标分配递归进入内存回收。 */
	noreclaim_flag = memalloc_noreclaim_save();

	nid = folio_nid(lru_to_folio(folio_list));
	do {
		struct folio *folio = lru_to_folio(folio_list);

		/* 连续抽取同节点 folio，遇到新节点即提交上一组。 */
		if (nid == folio_nid(folio)) {
			list_move(&folio->lru, &node_folio_list);
			continue;
		}

		nr_migrated += damon_migrate_folio_list(&node_folio_list,
							   NODE_DATA(nid),
							   target_nid);
		nid = folio_nid(lru_to_folio(folio_list));
	} while (!list_empty(folio_list));

	/* 主输入耗尽后别忘记提交最后一个源节点分组。 */
	nr_migrated += damon_migrate_folio_list(&node_folio_list,
						   NODE_DATA(nid),
						   target_nid);

	/* 与 save 严格配对，恢复调用线程原有的分配上下文。 */
	memalloc_noreclaim_restore(noreclaim_flag);

	return nr_migrated;
}

bool damos_ops_has_filter(struct damos *s)
{
	/*
	 * 业务背景：DAMOS 后端需要快速判断 scheme 是否配置了后端专属过滤器，以选择过滤执行路径。
	 * 入参：s 是监测线程稳定持有的 scheme 借用指针。
	 * 出参/返回：ops_filter 链至少有一个元素返回 true，否则 false；不修改链表。
	 * 注意事项：迭代只读取首项即可短路，过滤器 ownership 始终属于 scheme。
	 */
	/* ops filter 链只需判空，首个元素即可短路返回。 */
	struct damos_filter *f;

	damos_for_each_ops_filter(f, s)
		return true;
	/* 空链表表示后端没有任何额外的 folio 过滤约束。 */
	return false;
}
