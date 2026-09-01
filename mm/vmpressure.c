// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux VM pressure
 *
 * Copyright 2012 Linaro Ltd.
 *		  Anton Vorontsov <anton.vorontsov@linaro.org>
 *
 * Based on ideas from Andrew Morton, David Rientjes, KOSAKI Motohiro,
 * Leonid Moiseichuk, Mel Gorman, Minchan Kim and Pekka Enberg.
 */
/*
 * 本文件把 vmscan 的“扫描页数/回收页数”和扫描优先级折算成 low、medium、critical
 * 三级压力。传统 cgroup v1 可通过 eventfd 接收子树通知；cgroup v2 的内核消费者按
 * 本 memcg 效率触发 socket-pressure 滞后状态。计数在回收热路径累积，workqueue 异步
 * 计算并沿 memcg 祖先传播，事件注册表由 mutex 管理。
 */

#include <linux/cgroup.h>
#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/eventfd.h>
/* mm/vmstat/eventfd 提供页级计数单位和用户通知计数器。 */
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/printk.h>
#include <linux/vmpressure.h>
/* cgroup/memcg、eventfd、workqueue 和 vmscan 统计接口共同组成采样到通知的路径。 */

/*
 * The window size (vmpressure_win) is the number of scanned pages before
 * we try to analyze scanned/reclaimed ratio. So the window is used as a
 * rate-limit tunable for the "low" level notification, and also for
 * averaging the ratio for medium/critical levels. Using small window
 * sizes can cause lot of false positives, but too big window size will
 * delay the notifications.
 *
 * As the vmscan reclaimer logic works with chunks which are multiple of
 * SWAP_CLUSTER_MAX, it makes sense to use it for the window size as well.
 *
 * TODO: Make the window size depend on machine size, as we do for vmstat
 * thresholds. Currently we set it to 512 pages (2MB for 4KB pages).
 */
/*
 * vmpressure_win 是计算一次扫描/回收比前累计的基础页数窗口，也是 low 通知限速与
 * medium/critical 平滑周期。窗口太小会产生瞬时假阳性，太大则延迟响应；沿用 vmscan
 * 的 SWAP_CLUSTER_MAX 倍数便于与回收批次对齐。当前固定 512 页（4KiB 页时 2MiB），
 * TODO 是像 vmstat threshold 一样随机器内存规模调整。
 */
static const unsigned long vmpressure_win = SWAP_CLUSTER_MAX * 16;

/*
 * These thresholds are used when we account memory pressure through
 * scanned/reclaimed ratio. The current values were chosen empirically. In
 * essence, they are percents: the higher the value, the more number
 * unsuccessful reclaims there were.
 */
/* 经验阈值按“无效回收百分比”分级：值越高表示扫描中未成功回收的比例越大。 */
static const unsigned int vmpressure_level_med = 60;
static const unsigned int vmpressure_level_critical = 95;

/*
 * When there are too little pages left to scan, vmpressure() may miss the
 * critical pressure as number of pages will be less than "window size".
 * However, in that case the vmscan priority will raise fast as the
 * reclaimer will try to scan LRUs more deeply.
 *
 * The vmscan logic considers these special priorities:
 *
 * prio == DEF_PRIORITY (12): reclaimer starts with that value
 * prio <= DEF_PRIORITY - 2 : kswapd becomes somewhat overwhelmed
 * prio == 0                : close to OOM, kernel scans every page in an lru
 *
 * Any value in this range is acceptable for this tunable (i.e. from 12 to
 * 0). Current value for the vmpressure_level_critical_prio is chosen
 * empirically, but the number, in essence, means that we consider
 * critical level when scanning depth is ~10% of the lru size (vmscan
 * scans 'lru_size >> prio' pages, so it is actually 12.5%, or one
 * eights).
 */
/*
 * 小型 LRU 可能在未满一个窗口时已陷入危急状态，所以另以 vmscan priority 兜底：
 * priority 从 DEF_PRIORITY=12 逐步降至 0，数值越小扫描越深；阈值 ilog2(10) 约为 3，
 * 对应扫描约 1/8 LRU。达到该深度时 vmpressure_prio() 人工注入一个 100% 失败窗口。
 */
static const unsigned int vmpressure_level_critical_prio = ilog2(100 / 10);

/*
 * work_to_vmpressure() - 从嵌入的 work 成员恢复所属 vmpressure
 * 业务背景：workqueue 回调只收到 work_struct，需取回计数器与事件列表容器。
 * 入参：work 必须是某个仍存活 struct vmpressure::work 的借用指针。
 * 出参/返回：返回借用 vmpressure 指针，无引用或副作用。
 * 注意事项：container_of 只做地址换算；cleanup 的 flush_work 保证容器释放前回调结束。
 */
static struct vmpressure *work_to_vmpressure(struct work_struct *work)
{
	return container_of(work, struct vmpressure, work);
}

/*
 * vmpressure_parent() - 取得当前压力对象所属 memcg 的父组压力对象
 * 业务背景：异步通知从压力根向祖先传播，以实现 hierarchy/default 模式。
 * 入参：vmpr 是嵌入有效 memcg 的借用指针。
 * 出参/返回：父 memcg 存在时返回其借用 vmpressure，否则 NULL；不增加 css 引用。
 * 注意事项：调用者依赖 memcg 层级生命周期稳定，不能把返回值保存到保护范围之外。
 */
static struct vmpressure *vmpressure_parent(struct vmpressure *vmpr)
{
	/* 双向 container/accessor 把内嵌对象转换回 memcg，再沿层级取父节点。 */
	struct mem_cgroup *memcg = vmpressure_to_memcg(vmpr);

	memcg = parent_mem_cgroup(memcg);
	if (!memcg)
		return NULL;
	return memcg_to_vmpressure(memcg);
}

/* 每个 level 既是有序严重度，也作为字符串表和订阅阈值索引；数值只能递增比较。 */
enum vmpressure_levels {
	/* LOW：无效回收比例低于 60%，也用于每窗口的基础节流通知。 */
	VMPRESSURE_LOW = 0,
	/* MEDIUM：无效比例至少 60% 但低于 95%，回收效率明显下降。 */
	VMPRESSURE_MEDIUM,
	/* CRITICAL：无效比例至少 95%，或 priority 兜底注入的近 OOM 状态。 */
	VMPRESSURE_CRITICAL,
	/* NUM_LEVELS：仅作数组长度/解析上界，不能注册为真实级别。 */
	VMPRESSURE_NUM_LEVELS,
};

/* mode 决定事件在祖先传播以及已有下层接收者时是否仍触发。 */
enum vmpressure_modes {
	/* default：本组可接收，但已有更近层 default 接收者后不继续穿透祖先。 */
	VMPRESSURE_NO_PASSTHROUGH = 0,
	/* hierarchy：当前组及祖先层订阅均可接收，不因下层已 signal 而停止。 */
	VMPRESSURE_HIERARCHY,
	/* local：只接受本组直接产生的压力，祖先传播阶段跳过。 */
	VMPRESSURE_LOCAL,
	/* NUM_MODES：字符串数组和 match_string 的边界哨兵。 */
	VMPRESSURE_NUM_MODES,
};

/* 用户 ABI 级别字符串按 enum 下标固定映射，表项全局只读常驻。 */
static const char * const vmpressure_str_levels[] = {
	[VMPRESSURE_LOW] = "low",
	[VMPRESSURE_MEDIUM] = "medium",
	[VMPRESSURE_CRITICAL] = "critical",
};

/* 可选模式字符串；省略模式时注册代码使用 default/no-passthrough。 */
static const char * const vmpressure_str_modes[] = {
	[VMPRESSURE_NO_PASSTHROUGH] = "default",
	[VMPRESSURE_HIERARCHY] = "hierarchy",
	[VMPRESSURE_LOCAL] = "local",
};

/*
 * vmpressure_level() - 把 0..100 压力百分比映射为有序级别
 * 业务背景：比率计算与 priority 注入最终共享同一阈值语义。
 * 入参：pressure 是无效回收百分比，正常范围 0..100。
 * 出参/返回：>=95 为 CRITICAL，>=60 为 MEDIUM，其余 LOW；无副作用。
 * 注意事项：先判断高阈值保证边界落入最严重等级，函数不夹紧异常大输入。
 */
static enum vmpressure_levels vmpressure_level(unsigned long pressure)
{
	if (pressure >= vmpressure_level_critical)
		return VMPRESSURE_CRITICAL;
	else if (pressure >= vmpressure_level_med)
		return VMPRESSURE_MEDIUM;
	return VMPRESSURE_LOW;
}

/*
 * vmpressure_calc_level() - 从一个窗口的扫描/回收计数计算压力级别
 * 业务背景：tree work 与本地内核通知都用“扫描工作中有多少未转化为回收”衡量效率。
 * 入参：scanned/reclaimed 是同一采样窗口的基础页计数，纯输入；reclaimed 可因 slab
 * 回收统计而大于 scanned。
 * 出参/返回：返回 LOW/MEDIUM/CRITICAL；同时 pr_debug 输出百分比与原计数，不改计数器。
 * 注意事项：调用者保证 scanned 非零；先处理 reclaimed>=scanned 避免减法下溢和除零。
 * 算式用 scale=scanned+reclaimed 降低极端比例的离散偏差，但超大计数由窗口限制避免溢出。
 */
static enum vmpressure_levels vmpressure_calc_level(unsigned long scanned,
						    unsigned long reclaimed)
{
	/* scale 扩展分母精度；pressure 初始 0 对应回收效率不差。 */
	unsigned long scale = scanned + reclaimed;
	unsigned long pressure = 0;

	/*
	 * reclaimed can be greater than scanned for things such as reclaimed
	 * slab pages. shrink_node() just adds reclaimed pages without a
	 * related increment to scanned pages.
	 */
	/* slab 等回收可只增加 reclaimed；此时没有证据表明 LRU 压力，按 0% 处理。 */
	if (reclaimed >= scanned)
		goto out;
	/*
	 * We calculate the ratio (in percents) of how many pages were
	 * scanned vs. reclaimed in a given time frame (window). Note that
	 * time is in VM reclaimer's "ticks", i.e. number of pages
	 * scanned. This makes it possible to set desired reaction time
	 * and serves as a ratelimit.
	 */
	/*
	 * 以“扫描页数”作为虚拟时间：每累计一个窗口才反应一次，既给比率做平均也限速。
	 * 两步定点整数算术得到约 `(scanned-reclaimed)/scanned*100` 的失败百分比。
	 */
	pressure = scale - (reclaimed * scale / scanned);
	pressure = pressure * 100 / scale;

out:
	/* 调试输出不参与通知状态；随后统一按经验阈值离散化。 */
	pr_debug("%s: %3lu  (s: %lu  r: %lu)\n", __func__, pressure,
		 scanned, reclaimed);

	return vmpressure_level(pressure);
}

/*
 * struct vmpressure_event 表示一个 memcg pressure 订阅，由 register 分配并插入所属
 * vmpressure::events，unregister 在 events_lock 下摘除并释放。efd 是借用 eventfd_ctx，
 * 引用生命周期由 cgroup event 基础设施覆盖；level 是最低触发严重度，mode 控制本地/祖先
 * 传播，node 是受 events_lock 保护的链表节点。
 */
struct vmpressure_event {
	/* 注册期间只保存借用指针，本对象销毁不 eventfd_ctx_put()。 */
	struct eventfd_ctx *efd;
	/* level/mode 发布后只读，遍历者在 events_lock 下观察。 */
	enum vmpressure_levels level;
	enum vmpressure_modes mode;
	struct list_head node;
};

/*
 * vmpressure_event() - 在一个 memcg 的订阅表中筛选并 signal 合格 eventfd
 * 业务背景：work_fn 对压力源及每个祖先调用这里，mode 决定哪些订阅可见这次传播。
 * 入参：vmpr 是当前层借用压力对象；level 是本窗口严重度；ancestor 表示已离开压力源组；
 * signalled 表示更近层是否已有订阅被触发。
 * 出参/返回：至少 signal 一个 eventfd 返回 true，否则 false；每次命中给 eventfd 计数加一，
 * 不转移 event/eventfd ownership。
 * 注意事项：events_lock 与注册/注销串行并稳定 ev；mutex 允许 eventfd_signal 路径，函数可
 * 睡眠。local 在祖先层跳过，default 在已有下层 signal 后跳过，hierarchy 始终可传播。
 */
static bool vmpressure_event(struct vmpressure *vmpr,
			     const enum vmpressure_levels level,
			     bool ancestor, bool signalled)
{
	/* ev 是锁内借用迭代项；ret 聚合本层是否至少触发一个订阅。 */
	struct vmpressure_event *ev;
	bool ret = false;

	mutex_lock(&vmpr->events_lock);
	list_for_each_entry(ev, &vmpr->events, node) {
		/* local 只接收压力源本组事件，祖先传播时不可见。 */
		if (ancestor && ev->mode == VMPRESSURE_LOCAL)
			continue;
		/* default 禁止穿透已触发的更近层；hierarchy 不受 signalled 抑制。 */
		if (signalled && ev->mode == VMPRESSURE_NO_PASSTHROUGH)
			continue;
		/* 事件严重度尚未达到订阅阈值时保持静默。 */
		if (level < ev->level)
			continue;
		/* eventfd_signal 是外界可观察发布点；同层多个匹配订阅都会各自增加计数。 */
		eventfd_signal(ev->efd);
		ret = true;
	}
	mutex_unlock(&vmpr->events_lock);

	return ret;
}

/*
 * vmpressure_work_fn() - 消费一个子树采样窗口并异步向当前组及祖先发送事件
 *
 * 业务背景：tree 模式回收热路径只在 sr_lock 下累积计数并 schedule_work；本回调把较慢的
 * 比率计算、events mutex 和 eventfd 信号移出 reclaim 上下文。
 * 入参：work 是嵌入某个仍存活 vmpressure 的借用 work_struct。
 * 出参/返回：无直接返回；原子快照并清零 tree 计数，计算 level，随后沿父 memcg 传播。
 * 不释放 vmpressure 或事件对象。
 * 注意事项：多上下文可在旧 work 运行时再次累计/调度；sr_lock 保证快照与清零不丢失已纳入
 * 本批的计数。再次运行却看到 scanned==0 时直接退出；cleanup 用 flush_work 稳定生命周期。
 */
static void vmpressure_work_fn(struct work_struct *work)
{
	/* scanned/reclaimed 是锁内取得的本批快照；ancestor/signalled 驱动传播过滤状态机。 */
	struct vmpressure *vmpr = work_to_vmpressure(work);
	unsigned long scanned;
	unsigned long reclaimed;
	enum vmpressure_levels level;
	bool ancestor = false;
	bool signalled = false;

	/* 与所有 vmpressure() 生产者串行，取得并消费 tree 计数。 */
	spin_lock(&vmpr->sr_lock);
	/*
	 * Several contexts might be calling vmpressure(), so it is
	 * possible that the work was rescheduled again before the old
	 * work context cleared the counters. In that case we will run
	 * just after the old work returns, but then scanned might be zero
	 * here. No need for any locks here since we don't care if
	 * vmpr->reclaimed is in sync.
	 */
	/*
	 * 多个生产者可能在旧 work 清零前再次 schedule，导致紧随其后的 work 看到 scanned=0；
	 * 此时无需通知。原文所说无需额外锁是指零扫描时不必读取 reclaimed，而非取消 sr_lock。
	 */
	scanned = vmpr->tree_scanned;
	if (!scanned) {
		spin_unlock(&vmpr->sr_lock);
		return;
	}

	/* scanned 非零时在同一锁临界区取得配对 reclaimed，再把累计器归零交给下一窗口。 */
	reclaimed = vmpr->tree_reclaimed;
	vmpr->tree_scanned = 0;
	vmpr->tree_reclaimed = 0;
	spin_unlock(&vmpr->sr_lock);

	/* 锁外执行整数计算和可能获取 events mutex 的通知路径。 */
	level = vmpressure_calc_level(scanned, reclaimed);

	/*
	 * 首轮 ancestor=false 允许 local；一旦任一层 signal，后续祖先的 default 被抑制，
	 * hierarchy 仍继续。parent accessor 返回 NULL 时到达根以上并终止。
	 */
	do {
		if (vmpressure_event(vmpr, level, ancestor, signalled))
			signalled = true;
		ancestor = true;
	} while ((vmpr = vmpressure_parent(vmpr)));
}

/**
 * vmpressure() - Account memory pressure through scanned/reclaimed ratio
 * @gfp:	reclaimer's gfp mask
 * @order:	allocation order being reclaimed for
 * @memcg:	cgroup memory controller handle
 * @tree:	legacy subtree mode
 * @scanned:	number of pages scanned
 * @reclaimed:	number of pages reclaimed
 *
 * This function should be called from the vmscan reclaim path to account
 * "instantaneous" memory pressure (scanned/reclaimed ratio). The raw
 * pressure index is then further refined and averaged over time.
 *
 * If @tree is set, vmpressure is in traditional userspace reporting
 * mode: @memcg is considered the pressure root and userspace is
 * notified of the entire subtree's reclaim efficiency.
 *
 * If @tree is not set, reclaim efficiency is recorded for @memcg, and
 * only in-kernel users are notified.
 *
 * This function does not return any value.
 */
/*
 * vmpressure() - 从一次 vmscan 增量采样累计并发布内存压力
 *
 * 业务背景：vmscan 在单 memcg reclaim 与目标子树 reclaim 后调用；函数过滤用户无法帮助的
 * GFP 类型，以 vmpressure_win 平滑扫描效率。tree=true 服务传统用户 eventfd，false 服务
 * cgroup v2 内核 socket allocator。
 * 入参：gfp 是本次回收目标约束；order 是申请阶数；memcg 是借用目标组，tree 路径可由
 * accessor 把 NULL 视为根；tree 选择子树异步或本组同步模式；scanned/reclaimed 是本次
 * 基础页增量。所有输入 ownership 不变。
 * 出参/返回：无直接返回；tree 模式累计 tree_* 并在满窗口后调度 work；本地模式满窗口后
 * 计算 level，非 LOW 且 order 不高于 COSTLY_ORDER 时发布 socket pressure。过滤路径无副作用。
 * 注意事项：sr_lock 保护两套计数器，可在 reclaim/kswapd 并发上下文调用；schedule_work
 * 合并待处理 work，回调以计数器快照吸收合并。函数不直接持 events mutex。
 */
void vmpressure(gfp_t gfp, int order, struct mem_cgroup *memcg, bool tree,
		unsigned long scanned, unsigned long reclaimed)
{
	/* vmpr 是内嵌于目标/根 memcg 的借用状态对象。 */
	struct vmpressure *vmpr;

	/* CONFIG_MEMCG 运行期开关关闭时没有有效层级或消费者。 */
	if (mem_cgroup_disabled())
		return;

	/*
	 * The in-kernel users only care about the reclaim efficiency
	 * for this @memcg rather than the whole subtree, and there
	 * isn't and won't be any in-kernel user in a legacy cgroup.
	 */
	/* 旧层级只有 tree 用户 ABI；非 tree 的内核效率消费者仅存在于默认 cgroup v2。 */
	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys) && !tree)
		return;

	vmpr = memcg_to_vmpressure(memcg);

	/*
	 * Here we only want to account pressure that userland is able to
	 * help us with. For example, suppose that DMA zone is under
	 * pressure; if we notify userland about that kind of pressure,
	 * then it will be mostly a waste as it will trigger unnecessary
	 * freeing of memory by userland (since userland is more likely to
	 * have HIGHMEM/MOVABLE pages instead of the DMA fallback). That
	 * is why we include only movable, highmem and FS/IO pages.
	 * Indirect reclaim (kswapd) sets sc->gfp_mask to GFP_KERNEL, so
	 * we account it too.
	 */
	/*
	 * 只统计用户释放普通/高端/可移动或触发 FS/IO 后可能缓解的回收；纯 DMA 等约束
	 * 通知用户通常只会释放不可用 zone 的页。kswapd 用 GFP_KERNEL，含 FS/IO，故纳入。
	 */
	if (!(gfp & (__GFP_HIGHMEM | __GFP_MOVABLE | __GFP_IO | __GFP_FS)))
		return;

	/*
	 * If we got here with no pages scanned, then that is an indicator
	 * that reclaimer was unable to find any shrinkable LRUs at the
	 * current scanning depth. But it does not mean that we should
	 * report the critical pressure, yet. If the scanning priority
	 * (scanning depth) goes too high (deep), we will be notified
	 * through vmpressure_prio(). But so far, keep calm.
	 */
	/* 零扫描只表示当前深度无可扫描 LRU，尚不足以直接定为 critical；prio 路径另行兜底。 */
	if (!scanned)
		return;

	if (tree) {
		/* 子树模式把本次增量原子累加；局部 scanned 改写为累计值供窗口判断。 */
		spin_lock(&vmpr->sr_lock);
		scanned = vmpr->tree_scanned += scanned;
		vmpr->tree_reclaimed += reclaimed;
		spin_unlock(&vmpr->sr_lock);

		/* 未满窗口只保存计数；满窗口后 work 异步消费，重复 schedule 可安全合并。 */
		if (scanned < vmpressure_win)
			return;
		schedule_work(&vmpr->work);
	} else {
		/* level 只在完整本组窗口上计算，不向用户祖先传播。 */
		enum vmpressure_levels level;

		/* For now, no users for root-level efficiency */
		/* 当前没有根组 socket-pressure 消费者，NULL/显式 root 都跳过。 */
		if (!memcg || mem_cgroup_is_root(memcg))
			return;

		spin_lock(&vmpr->sr_lock);
		/* 同一锁下累计并在达到窗口时把本地计数器交给当前调用者消费。 */
		scanned = vmpr->scanned += scanned;
		reclaimed = vmpr->reclaimed += reclaimed;
		if (scanned < vmpressure_win) {
			spin_unlock(&vmpr->sr_lock);
			return;
		}
		vmpr->scanned = vmpr->reclaimed = 0;
		spin_unlock(&vmpr->sr_lock);

		level = vmpressure_calc_level(scanned, reclaimed);

		/*
		 * Once we go above COSTLY_ORDER, reclaim relies heavily on
		 * compaction to make progress. Reclaim efficiency was never a
		 * great proxy for pressure to begin with, but it's outright
		 * misleading with these high orders. Don't throttle sockets
		 * because somebody is attempting something crazy like an order-7
		 * and predictably struggling.
		 */
		/*
		 * 高阶请求超过 COSTLY_ORDER 后进展主要取决于 compaction，扫描效率不再可靠；
		 * 不应因一次可预期失败的 order-7 分配错误节流所有 socket。
		 */
		if (level > VMPRESSURE_LOW && order <= PAGE_ALLOC_COSTLY_ORDER) {
			/*
			 * Let the socket buffer allocator know that
			 * we are having trouble reclaiming LRU pages.
			 *
			 * For hysteresis keep the pressure state
			 * asserted for a second in which subsequent
			 * pressure events can occur.
			 */
			/* 发布一秒滞后状态，让紧邻压力事件共享断言窗口，避免 socket 节流抖动。 */
			mem_cgroup_set_socket_pressure(memcg);
		}
	}
}

/**
 * vmpressure_prio() - Account memory pressure through reclaimer priority level
 * @gfp:	reclaimer's gfp mask
 * @memcg:	cgroup memory controller handle
 * @prio:	reclaimer's priority
 *
 * This function should be called from the reclaim path every time when
 * the vmscan's reclaiming priority (scanning depth) changes.
 *
 * This function does not return any value.
 */
/*
 * vmpressure_prio() - 用 vmscan 扫描深度补报小 LRU 的 critical 压力
 * 业务背景：可扫描页少于 vmpressure_win 时比率窗口永远不满，但 priority 持续降低仍说明
 * 回收已深入 LRU；vmscan 每次 priority 变化调用这里。
 * 入参：gfp 是回收约束；memcg 是借用目标组；prio 是当前扫描优先级，数值越小扫描越深。
 * 出参/返回：无直接返回；未达到阈值无副作用，达到时通过 tree vmpressure 人工注入一个
 * scanned=window、reclaimed=0 的窗口，异步产生 CRITICAL 事件。
 * 注意事项：order 固定 0 避免高阶过滤；仍会经过 vmpressure 的 memcg/GFP 过滤与锁规则。
 */
void vmpressure_prio(gfp_t gfp, struct mem_cgroup *memcg, int prio)
{
	/*
	 * We only use prio for accounting critical level. For more info
	 * see comment for vmpressure_level_critical_prio variable above.
	 */
	/* priority 数值仍高于经验阈值时回收深度不足，不提前制造 critical。 */
	if (prio > vmpressure_level_critical_prio)
		return;

	/*
	 * OK, the prio is below the threshold, updating vmpressure
	 * information before shrinker dives into long shrinking of long
	 * range vmscan. Passing scanned = vmpressure_win, reclaimed = 0
	 * to the vmpressure() basically means that we signal 'critical'
	 * level.
	 */
	/* 一个 0% 回收成功率的完整窗口经 calc_level 必定达到 critical。 */
	vmpressure(gfp, 0, memcg, true, vmpressure_win, 0);
}

/* 最长合法 `critical,hierarchy\0` 大小；kstrndup 以此限制用户参数复制与解析成本。 */
#define MAX_VMPRESSURE_ARGS_LEN	(strlen("critical") + strlen("hierarchy") + 2)

/**
 * vmpressure_register_event() - Bind vmpressure notifications to an eventfd
 * @memcg:	memcg that is interested in vmpressure notifications
 * @eventfd:	eventfd context to link notifications with
 * @args:	event arguments (pressure level threshold, optional mode)
 *
 * This function associates eventfd context with the vmpressure
 * infrastructure, so that the notifications will be delivered to the
 * @eventfd. The @args parameter is a comma-delimited string that denotes a
 * pressure level threshold (one of vmpressure_str_levels, i.e. "low", "medium",
 * or "critical") and an optional mode (one of vmpressure_str_modes, i.e.
 * "hierarchy" or "local").
 *
 * To be used as memcg event method.
 *
 * Return: 0 on success, -ENOMEM on memory failure or -EINVAL if @args could
 * not be parsed.
 */
/*
 * vmpressure_register_event() - 解析 cgroup event 参数并发布一个 eventfd 压力订阅
 *
 * 业务背景：memcg v1 event_control 把用户 eventfd 与 `<level>[,<mode>]` 绑定，本函数将其
 * 转为受 vmpressure::events_lock 保护的订阅对象。
 * 入参：memcg 是借用目标组；eventfd 是由上层基础设施保持引用的借用 ctx；args 是借用
 * NUL 结尾字符串，要求级别为 low/medium/critical，可选模式为 default/hierarchy/local。
 * 出参/返回：成功插入列表返回 0；对象或字符串分配失败返回 -ENOMEM；级别/模式无法匹配
 * 返回 match_string 的负值（通常 -EINVAL）。成功后 ev 归 vmpressure 列表所有。
 * 注意事项：函数可睡眠；先完整构造再在 mutex 下发布，失败时不改变列表。spec_orig 保存
 * 分配基址，因为 strsep 会推进 spec；eventfd 引用本函数不增不减。
 */
int vmpressure_register_event(struct mem_cgroup *memcg,
			      struct eventfd_ctx *eventfd, const char *args)
{
	/* vmpr 为借用容器；ev 待发布；mode 默认 no-passthrough；spec 是可破坏解析副本。 */
	struct vmpressure *vmpr = memcg_to_vmpressure(memcg);
	struct vmpressure_event *ev;
	enum vmpressure_modes mode = VMPRESSURE_NO_PASSTHROUGH;
	enum vmpressure_levels level;
	char *spec, *spec_orig;
	char *token;
	int ret = 0;

	/* 有界复制隔离用户/上层字符串，避免 strsep 修改原 args；失败尚无资源可回滚。 */
	spec_orig = spec = kstrndup(args, MAX_VMPRESSURE_ARGS_LEN, GFP_KERNEL);
	if (!spec)
		return -ENOMEM;

	/* Find required level */
	/* 第一段必需且按 enum 顺序匹配，返回下标可直接转换为 level。 */
	token = strsep(&spec, ",");
	ret = match_string(vmpressure_str_levels, VMPRESSURE_NUM_LEVELS, token);
	if (ret < 0)
		goto out;
	level = ret;

	/* Find optional mode */
	/* 第二段缺失时保留 default；存在则同样按字符串表解析为 mode。 */
	token = strsep(&spec, ",");
	if (token) {
		ret = match_string(vmpressure_str_modes, VMPRESSURE_NUM_MODES, token);
		if (ret < 0)
			goto out;
		mode = ret;
	}

	/* 参数全部有效后才分配订阅对象，避免失败对象进入共享列表。 */
	ev = kzalloc_obj(*ev);
	if (!ev) {
		ret = -ENOMEM;
		goto out;
	}

	/* 三个字段在发布前初始化完成；efd 只借用，不由 ev 销毁路径 put。 */
	ev->efd = eventfd;
	ev->level = level;
	ev->mode = mode;

	/* list_add 是并发通知可见的发布点，与 work_fn 的锁内遍历串行。 */
	mutex_lock(&vmpr->events_lock);
	list_add(&ev->node, &vmpr->events);
	mutex_unlock(&vmpr->events_lock);
	ret = 0;
out:
	/* 无论解析、对象分配失败还是成功，都释放唯一字符串副本。 */
	kfree(spec_orig);
	return ret;
}

/**
 * vmpressure_unregister_event() - Unbind eventfd from vmpressure
 * @memcg:	memcg handle
 * @eventfd:	eventfd context that was used to link vmpressure with the @cg
 *
 * This function does internal manipulations to detach the @eventfd from
 * the vmpressure notifications, and then frees internal resources
 * associated with the @eventfd (but the @eventfd itself is not freed).
 *
 * To be used as memcg event method.
 */
/*
 * vmpressure_unregister_event() - 从一个 memcg 摘除首个匹配 eventfd 订阅
 * 业务背景：cgroup event_control 解绑时调用；必须先阻止 work_fn 再看到 ev，才能释放对象。
 * 入参：memcg 是借用目标组；eventfd 是注册时使用的借用 ctx。
 * 出参/返回：无直接返回；找到首个 efd 指针相等的 ev 后从列表摘除并释放，未找到则无副作用；
 * eventfd_ctx 本身不释放，ownership 仍由上层 event 基础设施管理。
 * 注意事项：events_lock 同时保护遍历、list_del 和 kfree，故通知者不会使用已释放 ev；
 * 函数可睡眠。若同一 efd 多次注册，每次调用只移除一个对象。
 */
void vmpressure_unregister_event(struct mem_cgroup *memcg,
				 struct eventfd_ctx *eventfd)
{
	/* vmpr 是借用容器，ev 仅在 events_lock 临界区内有效。 */
	struct vmpressure *vmpr = memcg_to_vmpressure(memcg);
	struct vmpressure_event *ev;

	mutex_lock(&vmpr->events_lock);
	list_for_each_entry(ev, &vmpr->events, node) {
		/* efd 身份而非 level/mode 标识上层请求解绑的注册实例。 */
		if (ev->efd != eventfd)
			continue;
		/* 先摘除阻止新通知查找，再在同一互斥区释放并结束扫描。 */
		list_del(&ev->node);
		kfree(ev);
		break;
	}
	mutex_unlock(&vmpr->events_lock);
}

/**
 * vmpressure_init() - Initialize vmpressure control structure
 * @vmpr:	Structure to be initialized
 *
 * This function should be called on every allocated vmpressure structure
 * before any usage.
 */
/*
 * vmpressure_init() - 初始化一个新 memcg 内嵌的压力控制对象
 * 业务背景：memcg 分配后、任何 reclaim 计数或 event 注册前调用，建立同步与 work 回调。
 * 入参：vmpr 是创建路径独占、存储已分配但尚未发布的输入输出对象。
 * 出参/返回：无直接返回；初始化 sr_lock、events_lock、空事件链表和 work item；计数字段
 * 依赖外层零初始化保持为 0，不取得外部引用。
 * 注意事项：同一对象只能初始化一次且须早于并发访问；INIT_WORK 把回调固定为 work_fn。
 */
void vmpressure_init(struct vmpressure *vmpr)
{
	/* spinlock 保护 reclaim 热路径计数，mutex 保护可能睡眠的 eventfd 订阅生命周期。 */
	spin_lock_init(&vmpr->sr_lock);
	mutex_init(&vmpr->events_lock);
	INIT_LIST_HEAD(&vmpr->events);
	/* work 发布后可由 tree 模式 schedule，容器销毁前必须 cleanup/flush。 */
	INIT_WORK(&vmpr->work, vmpressure_work_fn);
}

/**
 * vmpressure_cleanup() - shuts down vmpressure control structure
 * @vmpr:	Structure to be cleaned up
 *
 * This function should be called before the structure in which it is
 * embedded is cleaned up.
 */
/*
 * vmpressure_cleanup() - 在宿主 memcg 销毁前排空压力 work
 * 业务背景：work_to_vmpressure() 通过 container_of 访问宿主，若异步回调越过 memcg 释放会 UAF。
 * 入参：vmpr 是即将停止但仍有效的输入输出对象；外层已阻止新的合法 schedule 来源。
 * 出参/返回：无直接返回；同步等待待处理/运行中的 work 完成，不释放事件链表或 vmpr。
 * 注意事项：flush_work 可睡眠，不能在原子上下文调用；它是 work 生命周期屏障，不替代上层
 * 逐项注销 eventfd，也不清零计数器。返回后方可拆除 eventfd 基础设施和宿主存储。
 */
void vmpressure_cleanup(struct vmpressure *vmpr)
{
	/*
	 * Make sure there is no pending work before eventfd infrastructure
	 * goes away.
	 */
	/* 等最后一个回调完成，保证其不再获取 events_lock 或解引用父 memcg。 */
	flush_work(&vmpr->work);
}
