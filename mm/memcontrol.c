// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 内存控制器（memcg）学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5，2026-07-30）。
 *
 * 【职责边界】
 * 本文件把物理内存、swap、slab/kmem、socket 以及 zswap 等消耗归属到
 * memory cgroup，并实现层级统计、限额检查、回收/OOM、控制文件和 cgroup
 * 生命周期回调。页表如何建立、LRU 如何选择牺牲页、slab 如何真正分配对象
 * 并不在这里实现；本文件负责在这些路径的协议边界上“认账、限流和销账”。
 *
 * 【主调用链】
 * 用户态缺页/分配
 *   -> mem_cgroup_charge()
 *   -> try_charge_memcg()：先消耗 per-CPU stock，再向 page_counter 预留额度
 *   -> commit_charge()：把 folio 与 objcg 建立归属
 *   -> reclaim/迁移/释放
 *   -> mem_cgroup_uncharge*()：批量归还额度和统计。
 *
 * 内核对象分配
 *   -> current_obj_cgroup()
 *   -> obj_cgroup_charge()
 *   -> __memcg_slab_post_alloc_hook()
 *   -> __memcg_slab_free_hook()/obj_cgroup_uncharge()。
 *
 * cgroup 创建/删除
 *   -> mem_cgroup_css_alloc/online()
 *   -> 正常计费与控制文件读写
 *   -> css_offline/released/free()
 *   -> 将仍存活的 folio、objcg 和统计重挂到父 memcg，最终 RCU 释放。
 *
 * 【核心对象与所有权】
 * - struct mem_cgroup 是 cgroup memory controller 的状态对象，css 引用保证
 *   存储期；online 状态决定能否接受新的长期归属。
 * - struct obj_cgroup（objcg）把可能晚于 memcg offline 才释放的内核对象与
 *   计费身份解耦。对象持有 objcg 的 percpu_ref；offline 时 objcg 可改指向
 *   父 memcg，而不必扫描并改写每个 slab 对象。
 * - folio 的 memcg_data 保存 objcg/特殊标志；指针通常只是借用，跨越
 *   offline/释放边界时必须按相应 RCU 或引用协议稳定它。
 * - page_counter 维护层级 usage/min/low/high/max；vmstats/rstat 维护可容忍
 *   短暂误差的统计，两套数据不能混作同一种一致性保证。
 *
 * 【并发地图】
 * - 热路径优先写 per-CPU stock 和 per-CPU vmstats，批量传播以减少共享
 *   cacheline 争用；读者若要层级新鲜值，需显式 flush rstat。
 * - objcg_lock 保护 objcg 链表及重挂的结构性阶段；percpu_ref 负责对象存储
 *   期；RCU 让无锁读者跨越指针替换，但不冻结字段内容。
 * - lruvec->lru_lock 保护 folio 在 memcg/node LRU 上的归属和计数。重挂时
 *   子、父 lruvec 按固定嵌套顺序加锁，避免与回收/迁移并发破坏不变量。
 * - memory.high 是可恢复的节流边界：先直接回收，再按超额量延迟当前任务；
 *   memory.max 是硬上限：回收失败后可进入 memcg OOM。
 *
 * 【方案权衡】
 * per-CPU 缓存、批量计费和延迟 rstat 显著降低分配热路径成本，代价是短时
 * 统计误差、复杂的 drain/offline 协议以及更难理解的 ownership。objcg
 * 间接层让 cgroup 可在对象之前消亡，代价是额外引用和重挂逻辑。阅读时应
 * 始终区分“额度已经预留”“对象已经发布归属”“统计已经对用户可见”三个
 * 不同提交点。
 */
/* memcontrol.c - Memory Controller
 *
 * Copyright IBM Corporation, 2007
 * Author Balbir Singh <balbir@linux.vnet.ibm.com>
 *
 * Copyright 2007 OpenVZ SWsoft Inc
 * Author: Pavel Emelianov <xemul@openvz.org>
 *
 * Memory thresholds
 * Copyright (C) 2009 Nokia Corporation
 * Author: Kirill A. Shutemov
 *
 * Kernel Memory Controller
 * Copyright (C) 2012 Parallels Inc. and Google Inc.
 * Authors: Glauber Costa and Suleiman Souhlal
 *
 * Native page reclaim
 * Charge lifetime sanitation
 * Lockless page tracking & accounting
 * Unified hierarchy configuration model
 * Copyright (C) 2015 Red Hat, Inc., Johannes Weiner
 *
 * Per memcg lru locking
 * Copyright (C) 2020 Alibaba, Inc, Alex Shi
 */

#include <linux/cgroup-defs.h>
#include <linux/page_counter.h>
#include <linux/memcontrol.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/folio_batch.h>
#include <linux/vm_event_item.h>
#include <linux/smp.h>
#include <linux/page-flags.h>
#include <linux/backing-dev.h>
#include <linux/bit_spinlock.h>
#include <linux/rcupdate.h>
#include <linux/limits.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/swapops.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/vmpressure.h>
#include <linux/memremap.h>
#include <linux/mm_inline.h>
#include <linux/cpu.h>
#include <linux/oom.h>
#include <linux/lockdep.h>
#include <linux/resume_user_mode.h>
#include <linux/psi.h>
#include <linux/seq_buf.h>
#include <linux/sched/isolation.h>
#include <linux/kmemleak.h>
#include "internal.h"
#include "swap_table.h"
#include <net/sock.h>
#include <net/ip.h>
#include "slab.h"
#include "memcontrol-v1.h"

#include <linux/uaccess.h>

#define CREATE_TRACE_POINTS
#include <trace/events/memcg.h>
#undef CREATE_TRACE_POINTS

#include <trace/events/vmscan.h>

struct cgroup_subsys memory_cgrp_subsys __read_mostly;
EXPORT_SYMBOL(memory_cgrp_subsys);

/*
 * root_mem_cgroup 是未启用 memcg 或无法取得更具体归属时的最终归宿。
 * 两个全局指针在初始化后长期存在并以 __read_mostly 优化读多写少布局；
 * memory_cgrp_subsys 末尾的回调表则把本文件接入 cgroup core 生命周期。
 */
struct mem_cgroup *root_mem_cgroup __read_mostly;
EXPORT_SYMBOL(root_mem_cgroup);

/* Active memory cgroup to use from an interrupt context */
/*
 * 中断上下文没有可安全追溯的 current->mm 归属。发起异步操作的进程可临时
 * 设置本 CPU 槽位，让中断阶段的分配继续记到原 memcg；它是借用指针，
 * 使用者必须遵守设置/恢复和禁止迁移的配对协议。
 */
DEFINE_PER_CPU(struct mem_cgroup *, int_active_memcg);
EXPORT_PER_CPU_SYMBOL_GPL(int_active_memcg);

/* Socket memory accounting disabled? */
/* 启动参数形成的只读策略开关；为 true 时 socket 消耗不进入 memcg 计费。 */
static bool cgroup_memory_nosocket __ro_after_init;

/* Kernel memory accounting disabled? */
/* 为 true 时 slab 等内核内存走根组语义；__ro_after_init 防止运行期漂移。 */
static bool cgroup_memory_nokmem __ro_after_init;

/* BPF memory accounting disabled? */
/* 控制 BPF 对象是否建立 objcg 归属，初始化完成后不再改变。 */
static bool cgroup_memory_nobpf __ro_after_init;

/*
 * memcg_wq 承载 high 节流、stock drain 等允许异步执行的工作；全局队列在
 * mem_cgroup_init() 建立，在正常系统寿命内不销毁。
 * 两个 slab cache 分别分配 mem_cgroup 和每 NUMA 节点的附属状态。
 */
static struct workqueue_struct *memcg_wq __ro_after_init;

static struct kmem_cache *memcg_cachep;
static struct kmem_cache *memcg_pn_cachep;

#ifdef CONFIG_CGROUP_WRITEBACK
/*
 * 【cgroup writeback】
 *
 * 脏 folio 的 memcg 与实际 writeback wb 可能不同。这里用 foreign_wb
 * 统计识别持续的“外来脏化”，达到阈值后把 inode 的写回归属切到主要
 * dirtying memcg，使 I/O 带宽与回写压力归责正确。锁与引用必须覆盖 wb
 * 切换，等待队列处理旧 foreign 引用退出；关闭 CONFIG_CGROUP_WRITEBACK
 * 时下面 stub 保持调用点但不提供按组写回域。
 */
/*
 * writeback foreign-node 切换会等待旧归属引用退出；此等待队列连接
 * wb completion 与等待重建 cgwb 的进程，只在 cgroup writeback 配置出现。
 */
static DECLARE_WAIT_QUEUE_HEAD(memcg_cgwb_frn_waitq);
#endif

/*
 * task_is_dying() - 判断当前分配者是否已不适合继续高成本回收。
 *
 * 入参：无。返回 true 表示 current 已被 OOM 选中、收到 fatal signal 或
 * 正在退出。调用者以此避免让注定退出并释放内存的任务继续 reclaim/等待；
 * 这里只读取任务状态，不取得引用、不睡眠，也不改变任务标志。
 */
static inline bool task_is_dying(void)
{
	return tsk_is_oom_victim(current) || fatal_signal_pending(current) ||
		(current->flags & PF_EXITING);
}

/* Some nice accessors for the vmpressure. */
/*
 * memcg_to_vmpressure() - 从 memcg 取得其内嵌 vmpressure 对象。
 *
 * @memcg 是借用指针，可为 NULL；NULL 统一映射根组。返回值同样是借用指针，
 * 生命周期依附 memcg，调用者不能单独释放。该包装把 pressure 事件与内存
 * 控制组一一对应，而不增加 css 引用。
 */
struct vmpressure *memcg_to_vmpressure(struct mem_cgroup *memcg)
{
	if (!memcg)
		memcg = root_mem_cgroup;
	return &memcg->vmpressure;
}

/*
 * vmpressure_to_memcg() - 从内嵌成员反推出所属 memcg。
 *
 * @vmpr 必须确实是 mem_cgroup::vmpressure 的地址。container_of() 只做
 * 地址恢复，不取得引用；返回 memcg 的可用期仍由调用者已有同步保证。
 */
struct mem_cgroup *vmpressure_to_memcg(struct vmpressure *vmpr)
{
	return container_of(vmpr, struct mem_cgroup, vmpressure);
}

#define SEQ_BUF_SIZE SZ_4K
#define CURRENT_OBJCG_UPDATE_BIT 0
#define CURRENT_OBJCG_UPDATE_FLAG (1UL << CURRENT_OBJCG_UPDATE_BIT)

/*
 * objcg_lock 串行化各 NUMA 节点 objcg 链表的插入、摘除和重挂；它与
 * lruvec 锁按 reparent_locks() 给出的顺序组合。持锁不等于持有 objcg
 * 引用，释放存储仍由 percpu_ref + RCU 决定。
 */
static DEFINE_SPINLOCK(objcg_lock);

/*
 * mem_cgroup_kmem_disabled() - 查询启动后冻结的 kmem 计费策略。
 * 入参：无；返回 true 时调用者应跳过对象级 memcg 计费。无副作用、不睡眠。
 */
bool mem_cgroup_kmem_disabled(void)
{
	return cgroup_memory_nokmem;
}

static void memcg_uncharge(struct mem_cgroup *memcg, unsigned int nr_pages);

/*
 * obj_cgroup_release() - objcg 的最后一个 percpu_ref 释放回调。
 *
 * @ref 内嵌于已无外部对象引用的 objcg；回调取得容器但不新增引用。此时
 * 不会再有新的对象记到该 objcg，不过 per-CPU stock 的整页舍入余量可能
 * 最后才汇总回来。函数先把这部分页级额度和 kmem 统计归还，再在
 * objcg_lock 下摘链，销毁 percpu_ref，最终 kfree_rcu() 延迟释放，保证
 * 已进入 RCU 读侧的裸指针不会 UAF。返回：无直接返回值。
 */
static void obj_cgroup_release(struct percpu_ref *ref)
{
	struct obj_cgroup *objcg = container_of(ref, struct obj_cgroup, refcnt);
	unsigned int nr_bytes;
	unsigned int nr_pages;
	unsigned long flags;

	/*
	 * At this point all allocated objects are freed, and
	 * objcg->nr_charged_bytes can't have an arbitrary byte value.
	 * However, it can be PAGE_SIZE or (x * PAGE_SIZE).
	 *
	 * The following sequence can lead to it:
	 * 1) CPU0: objcg cached in one of stock->cached[i]
	 * 2) CPU1: we do a small allocation (e.g. 92 bytes),
	 *          PAGE_SIZE bytes are charged
	 * 3) CPU1: a process from another memcg is allocating something,
	 *          the stock if flushed,
	 *          objcg->nr_charged_bytes = PAGE_SIZE - 92
	 * 4) CPU0: we do release this object,
	 *          92 bytes are added to stock->nr_bytes[i]
	 * 5) CPU0: stock is flushed,
	 *          92 bytes are added to objcg->nr_charged_bytes
	 *
	 * In the result, nr_charged_bytes == PAGE_SIZE.
	 * This page will be uncharged in obj_cgroup_release().
	 */
	/*
	 * 此时所有已分配对象都已释放，nr_charged_bytes 不应再是任意字节数，
	 * 但可因 stock 在不同 CPU 上按 PAGE_SIZE 预充、按对象字节数返还而恰好
	 * 留下一页或若干整页。上面的五步例子说明：CPU1 预充一页只消费 92
	 * 字节，CPU0 归还对象后，两边 stock 最终汇合为 PAGE_SIZE。因此这里
	 * 先验证页对齐，再把整页余量作为最后一笔 uncharge；若忽略它，离线
	 * memcg 会永久虚高。
	 */
	nr_bytes = atomic_read(&objcg->nr_charged_bytes);
	WARN_ON_ONCE(nr_bytes & (PAGE_SIZE - 1));
	nr_pages = nr_bytes >> PAGE_SHIFT;

	if (nr_pages) {
		struct mem_cgroup *memcg;

		/*
		 * objcg->memcg 可在 offline 时被重挂，helper 返回带引用的当前
		 * 所属 memcg。统计、v1 kmem 和 page_counter 必须记到同一个
		 * 身份，完成后用 mem_cgroup_put() 对称释放。
		 */
		memcg = get_mem_cgroup_from_objcg(objcg);
		mod_memcg_state(memcg, MEMCG_KMEM, -nr_pages);
		memcg1_account_kmem(memcg, -nr_pages);
		if (!mem_cgroup_is_root(memcg))
			memcg_uncharge(memcg, nr_pages);
		mem_cgroup_put(memcg);
	}

	/*
	 * 摘链阻止后续重挂遍历再看到该 objcg；percpu_ref_exit() 只能在 kill
	 * 后且 release 回调到达时执行。RCU 延迟释放覆盖并发查询的读侧窗口。
	 */
	spin_lock_irqsave(&objcg_lock, flags);
	list_del(&objcg->list);
	spin_unlock_irqrestore(&objcg_lock, flags);

	percpu_ref_exit(ref);
	kfree_rcu(objcg, rcu);
}

/*
 * obj_cgroup_alloc() - 分配尚未绑定 memcg 的 objcg 身份对象。
 *
 * 入参：无。可睡眠（GFP_KERNEL）。成功返回拥有初始 percpu_ref 的新对象，
 * 链表为空，调用者负责设置 memcg 并发布；失败返回 NULL，所有局部资源已
 * 清理。percpu_ref 的 release 回调建立最终“余量销账—摘链—RCU 释放”路径。
 */
static struct obj_cgroup *obj_cgroup_alloc(void)
{
	struct obj_cgroup *objcg;
	int ret;

	objcg = kzalloc_obj(struct obj_cgroup);
	if (!objcg)
		return NULL;

	ret = percpu_ref_init(&objcg->refcnt, obj_cgroup_release, 0,
			      GFP_KERNEL);
	if (ret) {
		kfree(objcg);
		return NULL;
	}
	INIT_LIST_HEAD(&objcg->list);
	return objcg;
}

/*
 * __memcg_reparent_objcgs() - 在单个 NUMA 节点把子组 objcg 重挂到父组。
 *
 * @memcg/@parent 为借用指针，@nid 指定节点。调用者必须同时持 objcg_lock
 * 以及子、父 lruvec 锁；因此本函数不可睡眠。它先用 RCU 指针替换摘下当前
 * active objcg，再把 active 与历史已重挂 objcg 的 memcg 指针改成 parent，
 * 最后把链表拼入父节点。返回被摘下的 active objcg，锁外由调用者 kill
 * percpu_ref；在 kill 前它仍可承接已经在途的对象引用。
 */
static inline struct obj_cgroup *__memcg_reparent_objcgs(struct mem_cgroup *memcg,
							 struct mem_cgroup *parent,
							 int nid)
{
	struct obj_cgroup *objcg, *iter;
	struct mem_cgroup_per_node *pn = memcg->nodeinfo[nid];
	struct mem_cgroup_per_node *parent_pn = parent->nodeinfo[nid];

	objcg = rcu_replace_pointer(pn->objcg, NULL, true);
	/* 1) Ready to reparent active objcg. */
	/* 第一步：active objcg 已停止从 pn 发布，先并入待重挂集合。 */
	list_add(&objcg->list, &pn->objcg_list);
	/* 2) Reparent active objcg and already reparented objcgs to parent. */
	/*
	 * 第二步：active 以及此前因对象长寿而留下的 objcg 全部改指父组。
	 * WRITE_ONCE 防止编译器拆分/合并该发布写；生命周期仍由引用和 RCU 保证。
	 */
	list_for_each_entry(iter, &pn->objcg_list, list)
		WRITE_ONCE(iter->memcg, parent);
	/* 3) Move already reparented objcgs to the parent's list */
	/* 第三步：转移链表所有权，使父组后续 offline 能继续追踪这些 objcg。 */
	list_splice(&pn->objcg_list, &parent_pn->objcg_list);

	return objcg;
}

#ifdef CONFIG_MEMCG_V1
static void __mem_cgroup_flush_stats(struct mem_cgroup *memcg, bool force);

/*
 * reparent_state_local() - v1 层级删除时转移非层级本地统计。
 *
 * @memcg/@parent 均为借用指针。cgroup v2 的统计天然按 rstat 层级汇总，直接
 * 返回；v1 暴露 non-hierarchical 计数，必须先强制刷新子组，再逐项从子组
 * 扣除并加到父组，最后刷新父组，避免 rate-limited reader 暂时看不到大额
 * 更新。函数可触发 rstat flush，不应从 NMI 等原子上下文调用。
 */
static inline void reparent_state_local(struct mem_cgroup *memcg, struct mem_cgroup *parent)
{
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return;

	/*
	 * Reparent stats exposed non-hierarchically. Flush @memcg's stats first
	 * to read its stats accurately , and conservatively flush @parent's
	 * stats after reparenting to avoid hiding a potentially large stat
	 * update (e.g. from callers of mem_cgroup_flush_stats_ratelimited()).
	 */
	/*
	 * v1 非层级统计必须显式重挂。先刷新 @memcg 才能读取完整本地值；转移后
	 * 再保守刷新 @parent，避免父组仍处于限频窗口时隐藏一笔很大的更新。
	 */
	__mem_cgroup_flush_stats(memcg, true);

	/* The following counts are all non-hierarchical and need to be reparented. */
	/* 下列 state/lruvec state 都不由层级 rstat 自动继承，需逐项搬移。 */
	reparent_memcg1_state_local(memcg, parent);
	reparent_memcg1_lruvec_state_local(memcg, parent);

	__mem_cgroup_flush_stats(parent, true);
}
#else
/*
 * 未启用 v1 时没有需要搬移的 non-hierarchical 统计；空 stub 保持调用点
 * 无条件化。两个参数仅为接口对称，函数无副作用、不睡眠。
 */
static inline void reparent_state_local(struct mem_cgroup *memcg, struct mem_cgroup *parent)
{
}
#endif

/*
 * reparent_locks()/reparent_unlocks() - 建立 objcg 与 LRU 重挂的锁顺序。
 *
 * @memcg/@parent 为借用指针，@nid 选节点。顺序固定为 objcg_lock、子
 * lru_lock、父 lru_lock；nested subclass 告诉 lockdep 这是有意的同类锁
 * 嵌套。irq 在最外层关闭并在最后恢复，防止本 CPU 中断重入同一锁。调用者
 * 必须严格配对，持锁区不可睡眠。
 */
static inline void reparent_locks(struct mem_cgroup *memcg, struct mem_cgroup *parent, int nid)
{
	spin_lock_irq(&objcg_lock);
	spin_lock_nested(&mem_cgroup_lruvec(memcg, NODE_DATA(nid))->lru_lock, 1);
	spin_lock_nested(&mem_cgroup_lruvec(parent, NODE_DATA(nid))->lru_lock, 2);
}

/* 以完全相反顺序解锁，直到 objcg_lock 释放时才恢复本 CPU 中断。 */
static inline void reparent_unlocks(struct mem_cgroup *memcg, struct mem_cgroup *parent, int nid)
{
	spin_unlock(&mem_cgroup_lruvec(parent, NODE_DATA(nid))->lru_lock);
	spin_unlock(&mem_cgroup_lruvec(memcg, NODE_DATA(nid))->lru_lock);
	spin_unlock_irq(&objcg_lock);
}

/*
 * memcg_reparent_objcgs() - memcg offline 后按节点迁移 LRU 与 objcg 归属。
 *
 * @memcg 是待销毁子组的借用指针，父组由层级关系取得且在 css 生命周期内
 * 稳定。每个节点先让 MGLRU 的父组槽位可用，再在三锁临界区同时迁移 LRU
 * 与 objcg，确保 folio 可回收视图和对象计费身份不会看到半迁移状态。
 * recheck 失败说明锁外准备已失效：释放全部锁、让出 CPU 后重试。锁外 kill
 * 旧 active objcg，待在途对象释放后由 obj_cgroup_release() 收尾。返回无
 * 直接值；成功保证子组不再拥有可计费 objcg 或本地统计。
 */
static void memcg_reparent_objcgs(struct mem_cgroup *memcg)
{
	struct obj_cgroup *objcg;
	struct mem_cgroup *parent = parent_mem_cgroup(memcg);
	int nid;

	for_each_node(nid) {
retry:
		/*
		 * MGLRU 的 max sequence 准备可能涉及锁外工作；加锁后必须复核。
		 * 复核失败不能持锁忙等，否则会阻塞令条件成立的并发路径。
		 */
		if (lru_gen_enabled())
			max_lru_gen_memcg(parent, nid);

		reparent_locks(memcg, parent, nid);

		if (lru_gen_enabled()) {
			if (!recheck_lru_gen_max_memcg(parent, nid)) {
				reparent_unlocks(memcg, parent, nid);
				cond_resched();
				goto retry;
			}
			lru_gen_reparent_memcg(memcg, parent, nid);
		} else {
			/* 传统 LRU 与 MGLRU 二选一，但都在相同锁和 ownership 边界内。 */
			lru_reparent_memcg(memcg, parent, nid);
		}

		objcg = __memcg_reparent_objcgs(memcg, parent, nid);

		reparent_unlocks(memcg, parent, nid);

		/*
		 * kill 阻止新 percpu_ref 获取；已有对象引用仍可自然归零，因此
		 * 删除 cgroup 不要求同步扫描并释放所有内核对象。
		 */
		percpu_ref_kill(&objcg->refcnt);
	}

	reparent_state_local(memcg, parent);
}

/*
 * A lot of the calls to the cache allocation functions are expected to be
 * inlined by the compiler. Since the calls to memcg_slab_post_alloc_hook() are
 * conditional to this static branch, we'll have to allow modules that does
 * kmem_cache_alloc and the such to see this symbol as well
 */
/*
 * slab 分配 helper 大量内联进模块；它们需直接测试此 static key，只有
 * 至少一个非根 memcg 启用 kmem 计费时才跳入慢路径。因此符号必须导出。
 * static key 的收益是关闭功能时热路径近似一条可补丁化分支，代价是启停
 * 必须与 memcg online/offline 生命周期严格配对。
 */
DEFINE_STATIC_KEY_FALSE(memcg_kmem_online_key);
EXPORT_SYMBOL(memcg_kmem_online_key);

/*
 * BPF 对象计费使用独立 static key：即使一般 kmem 计费开启，启动参数仍可
 * 单独关闭 BPF 归属。模块读取该 key，不拥有或修改它。
 */
DEFINE_STATIC_KEY_FALSE(memcg_bpf_enabled_key);
EXPORT_SYMBOL(memcg_bpf_enabled_key);

/**
 * get_mem_cgroup_css_from_folio - acquire a css of the memcg associated with a folio
 * @folio: folio of interest
 *
 * If memcg is bound to the default hierarchy, css of the memcg associated
 * with @folio is returned.  The returned css remains associated with @folio
 * until it is released.
 *
 * If memcg is bound to a traditional hierarchy, the css of root_mem_cgroup
 * is returned.
 */
/*
 * get_mem_cgroup_css_from_folio() - 取得 folio 计费 memcg 对应的 css。
 *
 * @folio 为调用者稳定的借用 folio。默认层级中先通过 folio 的 objcg 取得
 * 带引用 memcg，再返回其内嵌 css；该引用保持 css 与 folio 的关联，调用者
 * 最终须按接口约定 css_put()/mem_cgroup_put()。传统 v1 不提供此绑定语义，
 * 固定返回根 css。成功总返回非 NULL；不改变 folio，可在允许相应引用获取
 * 的上下文调用。
 */
struct cgroup_subsys_state *get_mem_cgroup_css_from_folio(struct folio *folio)
{
	struct mem_cgroup *memcg;

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return &root_mem_cgroup->css;

	memcg = get_mem_cgroup_from_folio(folio);

	return memcg ? &memcg->css : &root_mem_cgroup->css;
}

/**
 * page_cgroup_ino - return inode number of the memcg a page is charged to
 * @page: the page
 *
 * Look up the closest online ancestor of the memory cgroup @page is charged to
 * and return its inode number or 0 if @page is not charged to any cgroup. It
 * is safe to call this function without holding a reference to @page.
 *
 * Note, this function is inherently racy, because there is nothing to prevent
 * the cgroup inode from getting torn down and potentially reallocated a moment
 * after page_cgroup_ino() returns, so it only should be used by callers that
 * do not care (such as procfs interfaces).
 */
/*
 * page_cgroup_ino() - 给诊断接口返回 page 最近在线 memcg 的 cgroup inode。
 *
 * @page 只需调用瞬间可访问，函数不取得 page 引用。RCU 读锁保护沿 memcg
 * 父链查找期间的存储期；若原归属已 offline，则向上寻找在线祖先。返回 0
 * 表示未计费，否则返回当时观察到的 inode。inode 可在返回后立即被拆除并
 * 复用，所以结果只能用于 procfs 等容忍竞态的标识，不能作为授权或永久键。
 */
ino_t page_cgroup_ino(struct page *page)
{
	struct mem_cgroup *memcg;
	unsigned long ino = 0;

	rcu_read_lock();
	/* page_folio() is racy here, but the entire function is racy anyway */
	/*
	 * page_folio() 与大页拆分可能竞态；本接口本来只提供瞬时诊断快照，
	 * 因而接受这种不稳定性。RCU 只防内存释放，并不冻结 folio 归属。
	 */
	memcg = folio_memcg_check(page_folio(page));

	while (memcg && !css_is_online(&memcg->css))
		memcg = parent_mem_cgroup(memcg);
	if (memcg)
		ino = cgroup_ino(memcg->css.cgroup);
	rcu_read_unlock();
	return ino;
}
EXPORT_SYMBOL_GPL(page_cgroup_ino);

/* Subset of node_stat_item for memcg stats */
/*
 * 【统计索引层】
 *
 * 全局 vmstat 枚举很大，memcg 只保存需要按组观察的项目。下面两张表把
 * node_stat_item/memcg_stat_item 压缩为 u8 稠密槽位；U8_MAX 表示“不受
 * memcg 支持”。热路径据此访问紧凑 per-CPU 数组，读取路径再按项目换算
 * 页或字节。新增项目若忘记登记，会告警并返回 0，而不是越界访问。
 */
static const unsigned int memcg_node_stat_items[] = {
	NR_INACTIVE_ANON,
	NR_ACTIVE_ANON,
	NR_INACTIVE_FILE,
	NR_ACTIVE_FILE,
	NR_UNEVICTABLE,
	NR_SLAB_RECLAIMABLE_B,
	NR_SLAB_UNRECLAIMABLE_B,
	WORKINGSET_REFAULT_ANON,
	WORKINGSET_REFAULT_FILE,
	WORKINGSET_ACTIVATE_ANON,
	WORKINGSET_ACTIVATE_FILE,
	WORKINGSET_RESTORE_ANON,
	WORKINGSET_RESTORE_FILE,
	WORKINGSET_NODERECLAIM,
	NR_ANON_MAPPED,
	NR_FILE_MAPPED,
	NR_FILE_PAGES,
	NR_FILE_DIRTY,
	NR_WRITEBACK,
	NR_SHMEM,
	NR_SHMEM_THPS,
	NR_FILE_THPS,
	NR_ANON_THPS,
	NR_VMALLOC,
	NR_KERNEL_STACK_KB,
	NR_PAGETABLE,
	NR_SECONDARY_PAGETABLE,
#ifdef CONFIG_SWAP
	NR_SWAPCACHE,
#endif
#ifdef CONFIG_NUMA_BALANCING
	PGPROMOTE_SUCCESS,
#endif
	PGDEMOTE_KSWAPD,
	PGDEMOTE_DIRECT,
	PGDEMOTE_KHUGEPAGED,
	PGDEMOTE_PROACTIVE,
	PGSTEAL_KSWAPD,
	PGSTEAL_DIRECT,
	PGSTEAL_KHUGEPAGED,
	PGSTEAL_PROACTIVE,
	PGSTEAL_ANON,
	PGSTEAL_FILE,
	PGSCAN_KSWAPD,
	PGSCAN_DIRECT,
	PGSCAN_KHUGEPAGED,
	PGSCAN_PROACTIVE,
	PGSCAN_ANON,
	PGSCAN_FILE,
	PGREFILL,
#ifdef CONFIG_HUGETLB_PAGE
	NR_HUGETLB,
#endif
};

static const unsigned int memcg_stat_items[] = {
	MEMCG_SWAP,
	MEMCG_SOCK,
	MEMCG_PERCPU_B,
	MEMCG_KMEM,
	MEMCG_ZSWAP_B,
	MEMCG_ZSWAPPED,
	MEMCG_ZSWAP_INCOMP,
};

#define NR_MEMCG_NODE_STAT_ITEMS ARRAY_SIZE(memcg_node_stat_items)
#define MEMCG_VMSTAT_SIZE (NR_MEMCG_NODE_STAT_ITEMS + \
			   ARRAY_SIZE(memcg_stat_items))
#define BAD_STAT_IDX(index) ((u32)(index) >= U8_MAX)
static u8 mem_cgroup_stats_index[MEMCG_NR_STAT] __read_mostly;

/*
 * init_memcg_stats() - 在根 memcg 构造期间建立统计枚举到稠密槽位的映射。
 *
 * 入参：无；返回：无直接返回值。函数只在早期初始化执行，BUILD_BUG_ON()
 * 保证 U8_MAX 足以同时作为哨兵；两个循环分别登记 node 与 memcg 私有项目。
 * 初始化完成后映射表只读，后续热路径可用一次数组访问代替稀疏查找。
 */
static void init_memcg_stats(void)
{
	u8 i, j = 0;

	BUILD_BUG_ON(MEMCG_NR_STAT >= U8_MAX);

	memset(mem_cgroup_stats_index, U8_MAX, sizeof(mem_cgroup_stats_index));

	for (i = 0; i < NR_MEMCG_NODE_STAT_ITEMS; ++i, ++j)
		mem_cgroup_stats_index[memcg_node_stat_items[i]] = j;

	for (i = 0; i < ARRAY_SIZE(memcg_stat_items); ++i, ++j)
		mem_cgroup_stats_index[memcg_stat_items[i]] = j;
}

/*
 * memcg_stats_index() - 把统计枚举映射为 memcg 稠密数组下标。
 * @idx 必须落在映射表范围；返回有效槽位或 U8_MAX 哨兵。纯只读、不睡眠，
 * 调用者负责用 BAD_STAT_IDX() 拒绝未登记项目。
 */
static inline int memcg_stats_index(int idx)
{
	return mem_cgroup_stats_index[idx];
}

/*
 * lruvec 的统计分为两个阶段：percpu.state 是本 CPU、本 memcg/node 的热
 * 路径源值，state_prev 是上次 flush 基线；聚合对象的 state 包含子树，
 * state_local 只含本组，state_pending 保存本轮自底向上传播的中间增量。
 * 延迟传播降低 cacheline 争用，代价是无 flush 的读可能稍旧或见到负中间态。
 */
struct lruvec_stats_percpu {
	/* Local (CPU and cgroup) state */
	/* 本 CPU、本 cgroup 的原始状态。 */
	long state[NR_MEMCG_NODE_STAT_ITEMS];

	/* Delta calculation for lockless upward propagation */
	/* 保存上次快照，用于无锁计算向上增量。 */
	long state_prev[NR_MEMCG_NODE_STAT_ITEMS];
};

struct lruvec_stats {
	/* Aggregated (CPU and subtree) state */
	/* 聚合所有 CPU 和整个子树的状态。 */
	long state[NR_MEMCG_NODE_STAT_ITEMS];

	/* Non-hierarchical (CPU aggregated) state */
	/* 只聚合本组各 CPU，不包含后代。 */
	long state_local[NR_MEMCG_NODE_STAT_ITEMS];

	/* Pending child counts during tree propagation */
	/* 树形传播中尚待本层领取的子组增量。 */
	long state_pending[NR_MEMCG_NODE_STAT_ITEMS];
};

/*
 * lruvec_page_state() - 读取 memcg/node LRU 的层级聚合统计。
 *
 * @lruvec 是生命周期已稳定的借用指针，@idx 为已登记 node_stat_item。
 * memcg 关闭时退化到全局 node 统计；否则 READ_ONCE 读取最近聚合快照。
 * SMP 上传播中间态可能短暂为负，向外钳为 0。函数不刷新、不睡眠。
 */
unsigned long lruvec_page_state(struct lruvec *lruvec, enum node_stat_item idx)
{
	struct mem_cgroup_per_node *pn;
	long x;
	int i;

	if (mem_cgroup_disabled())
		return node_page_state(lruvec_pgdat(lruvec), idx);

	i = memcg_stats_index(idx);
	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	pn = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	x = READ_ONCE(pn->lruvec_stats->state[i]);
#ifdef CONFIG_SMP
	if (x < 0)
		x = 0;
#endif
	return x;
}

/*
 * lruvec_page_state_local() - 读取同一 lruvec 仅本 memcg 的聚合值。
 * 契约同 lruvec_page_state()，但排除子 cgroup；v1 删除路径据此把非层级
 * 统计从子组守恒地搬到父组。
 */
unsigned long lruvec_page_state_local(struct lruvec *lruvec,
				      enum node_stat_item idx)
{
	struct mem_cgroup_per_node *pn;
	long x;
	int i;

	if (mem_cgroup_disabled())
		return node_page_state(lruvec_pgdat(lruvec), idx);

	i = memcg_stats_index(idx);
	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	pn = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	x = READ_ONCE(pn->lruvec_stats->state_local[i]);
#ifdef CONFIG_SMP
	if (x < 0)
		x = 0;
#endif
	return x;
}

#ifdef CONFIG_MEMCG_V1
static void __mod_memcg_lruvec_state(struct mem_cgroup_per_node *pn,
				     enum node_stat_item idx, long val);

/*
 * reparent_memcg_lruvec_state_local() - 把 v1 非层级 lruvec 统计从子组搬到父组。
 *
 * @memcg/@parent 是生命周期已稳定的借用指针，@idx 是待迁移统计项。函数按
 * NUMA 节点读取子组 local 值，再对子、父执行等量反向更新，保持系统总量
 * 不变。仅 CONFIG_MEMCG_V1 使用；调用方已阻止新的子组归属，返回无直接值。
 */
void reparent_memcg_lruvec_state_local(struct mem_cgroup *memcg,
				       struct mem_cgroup *parent, int idx)
{
	int nid;

	for_each_node(nid) {
		struct lruvec *child_lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
		struct lruvec *parent_lruvec = mem_cgroup_lruvec(parent, NODE_DATA(nid));
		unsigned long value = lruvec_page_state_local(child_lruvec, idx);
		struct mem_cgroup_per_node *child_pn, *parent_pn;

		child_pn = container_of(child_lruvec, struct mem_cgroup_per_node, lruvec);
		parent_pn = container_of(parent_lruvec, struct mem_cgroup_per_node, lruvec);

		__mod_memcg_lruvec_state(child_pn, idx, -value);
		__mod_memcg_lruvec_state(parent_pn, idx, value);
	}
}
#endif

/* Subset of vm_event_item to report for memcg event stats */
/*
 * event 表记录“发生次数”而非驻留量，使用另一套稠密索引。条件编译决定
 * 当前内核能产生的事件集合，用户态解析 memory.stat 时应按名字而非固定
 * 位置处理配置差异。
 */
static const unsigned int memcg_vm_event_stat[] = {
#ifdef CONFIG_MEMCG_V1
	PGPGIN,
	PGPGOUT,
#endif
	PSWPIN,
	PSWPOUT,
	PGFAULT,
	PGMAJFAULT,
	PGACTIVATE,
	PGDEACTIVATE,
	PGLAZYFREE,
	PGLAZYFREED,
#ifdef CONFIG_SWAP
	SWPIN_ZERO,
	SWPOUT_ZERO,
#endif
#ifdef CONFIG_ZSWAP
	ZSWPIN,
	ZSWPOUT,
	ZSWPWB,
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	THP_FAULT_ALLOC,
	THP_COLLAPSE_ALLOC,
	THP_SWPOUT,
	THP_SWPOUT_FALLBACK,
#endif
#ifdef CONFIG_NUMA_BALANCING
	NUMA_PAGE_MIGRATE,
	NUMA_PTE_UPDATES,
	NUMA_HINT_FAULTS,
#endif
};

#define NR_MEMCG_EVENTS ARRAY_SIZE(memcg_vm_event_stat)
static u8 mem_cgroup_events_index[NR_VM_EVENT_ITEMS] __read_mostly;

/*
 * init_memcg_events() - 初始化 VM 事件枚举到 memcg event 数组的稠密映射。
 *
 * 入参：无；返回：无直接返回值。先把全部槽位设为 U8_MAX，再只登记当前
 * 配置实际导出的事件；因此未启用 SWAP/ZSWAP/THP 等配置时不会留下伪槽位。
 */
static void init_memcg_events(void)
{
	u8 i;

	BUILD_BUG_ON(NR_VM_EVENT_ITEMS >= U8_MAX);

	memset(mem_cgroup_events_index, U8_MAX,
	       sizeof(mem_cgroup_events_index));

	for (i = 0; i < NR_MEMCG_EVENTS; ++i)
		mem_cgroup_events_index[memcg_vm_event_stat[i]] = i;
}

/*
 * memcg_events_index() - 返回 @idx 在 memcg per-CPU event 数组中的位置。
 * 返回 U8_MAX 表示该事件未登记；不取得锁、不睡眠，调用者必须先验证结果。
 */
static inline int memcg_events_index(enum vm_event_item idx)
{
	return mem_cgroup_events_index[idx];
}

/*
 * vmstats_percpu 是无共享锁写入的源数据：stats_updates 只统计多久需要通知
 * rstat，parent_pcpu/vmstats 缓存祖先传播落点，state/events 保存实际值，
 * *_prev 是上次 flush 快照。首个 cacheline 专供最热的更新路径。
 */
struct memcg_vmstats_percpu {
	/* Stats updates since the last flush */
	/* 自上次 flush 后累计的统计更新规模。 */
	unsigned long			stats_updates;

	/* Cached pointers for fast iteration in memcg_rstat_updated() */
	/* 缓存父级 per-CPU 与聚合端指针，供更新路径快速向上迭代。 */
	struct memcg_vmstats_percpu __percpu	*parent_pcpu;
	struct memcg_vmstats			*vmstats;

	/* The above should fit a single cacheline for memcg_rstat_updated() */
	/* 以上热字段应装入一个 cacheline，降低更新路径访存成本。 */

	/* Local (CPU and cgroup) page state & events */
	long			state[MEMCG_VMSTAT_SIZE];
	unsigned long		events[NR_MEMCG_EVENTS];

	/* Delta calculation for lockless upward propagation */
	/* 上次 state/event 快照用于无锁计算传播增量。 */
	long			state_prev[MEMCG_VMSTAT_SIZE];
	unsigned long		events_prev[NR_MEMCG_EVENTS];
} ____cacheline_aligned;

/*
 * memcg_vmstats 是用户可读的聚合端：state/events 包含后代，*_local 只含
 * 本组，*_pending 是一次树传播尚未提交的孩子增量。stats_updates 决定
 * 同步读取是否值得承担全局 rstat 锁成本。
 */
struct memcg_vmstats {
	/* Aggregated (CPU and subtree) page state & events */
	/* 聚合所有 CPU 与后代的页状态和事件。 */
	long			state[MEMCG_VMSTAT_SIZE];
	unsigned long		events[NR_MEMCG_EVENTS];

	/* Non-hierarchical (CPU aggregated) page state & events */
	/* 仅本组的跨 CPU 状态和事件，不包含后代。 */
	long			state_local[MEMCG_VMSTAT_SIZE];
	unsigned long		events_local[NR_MEMCG_EVENTS];

	/* Pending child counts during tree propagation */
	/* 自底向上传播时等待本层合并的孩子增量。 */
	long			state_pending[MEMCG_VMSTAT_SIZE];
	unsigned long		events_pending[NR_MEMCG_EVENTS];

	/* Stats updates since the last flush */
	/* 自最近一次聚合后尚未收敛的更新量。 */
	atomic_long_t		stats_updates;
};

/*
 * memcg and lruvec stats flushing
 *
 * Many codepaths leading to stats update or read are performance sensitive and
 * adding stats flushing in such codepaths is not desirable. So, to optimize the
 * flushing the kernel does:
 *
 * 1) Periodically and asynchronously flush the stats every 2 seconds to not let
 *    rstat update tree grow unbounded.
 *
 * 2) Flush the stats synchronously on reader side only when there are more than
 *    (MEMCG_CHARGE_BATCH * nr_cpus) update events. Though this optimization
 *    will let stats be out of sync by atmost (MEMCG_CHARGE_BATCH * nr_cpus) but
 *    only for 2 seconds due to (1).
 */
/*
 * 热路径不为每次更新同步刷新：内核每两秒异步收敛一次，读侧仅在累计变化
 * 超过 MEMCG_CHARGE_BATCH * 在线 CPU 数时同步刷新。因而统计最多在限定
 * 时间/批量内陈旧，但最终守恒；这不是 page_counter 限额判断的依据。
 */
static void flush_memcg_stats_dwork(struct work_struct *w);
static DECLARE_DEFERRABLE_WORK(stats_flush_dwork, flush_memcg_stats_dwork);
static u64 flush_last_time;

#define FLUSH_TIME (2UL*HZ)

/*
 * memcg_vmstats_needs_flush() - 判断尚未聚合的更新量是否值得取得 rstat 锁。
 * @vmstats 为借用指针；当累计更新超过批量大小乘在线 CPU 数时返回 true。
 * 该判断只影响统计新鲜度和刷新成本，不参与 memory.max 的正确性判定。
 */
static bool memcg_vmstats_needs_flush(struct memcg_vmstats *vmstats)
{
	return atomic_long_read(&vmstats->stats_updates) >
		MEMCG_CHARGE_BATCH * num_online_cpus();
}

/*
 * memcg_rstat_updated() - 登记本 CPU 对 memcg 产生了 @val 页量级的变化。
 *
 * @memcg 必须在线且为借用，@cpu 是 get_cpu() 固定后的 CPU。函数沿缓存的
 * 父链按批量累计更新数，某层已达可刷新阈值时祖先也已被登记，可提前停止。
 * 它只决定 flush 时机，实际统计增量已经写入 per-CPU state/events。
 */
static inline void memcg_rstat_updated(struct mem_cgroup *memcg, long val,
				       int cpu)
{
	struct memcg_vmstats_percpu __percpu *statc_pcpu;
	struct memcg_vmstats_percpu *statc;
	unsigned long stats_updates;

	if (!val)
		return;

	__css_rstat_updated(&memcg->css, cpu);
	statc_pcpu = memcg->vmstats_percpu;
	for (; statc_pcpu; statc_pcpu = statc->parent_pcpu) {
		statc = this_cpu_ptr(statc_pcpu);
		/*
		 * If @memcg is already flushable then all its ancestors are
		 * flushable as well and also there is no need to increase
		 * stats_updates.
		 */
		/* 本组一旦达到可刷新阈值，祖先也已被标记，无需继续增加计数。 */
		if (memcg_vmstats_needs_flush(statc->vmstats))
			break;

		stats_updates = this_cpu_add_return(statc_pcpu->stats_updates,
						    abs(val));
		if (stats_updates < MEMCG_CHARGE_BATCH)
			continue;

		stats_updates = this_cpu_xchg(statc_pcpu->stats_updates, 0);
		atomic_long_add(stats_updates, &statc->vmstats->stats_updates);
	}
}

/*
 * __mem_cgroup_flush_stats() - 将 memcg 子树的 rstat 增量聚合到读侧数组。
 *
 * @memcg 是借用指针；@force=false 可因变化不足跳过，true 则无条件越过
 * 限频。css_rstat_flush() 用全局 rstat 锁串行树传播，属于可承受较高延迟
 * 的慢路径。返回后只保证 flush 边界前的更新可见，新更新仍可并发发生。
 */
static void __mem_cgroup_flush_stats(struct mem_cgroup *memcg, bool force)
{
	bool needs_flush = memcg_vmstats_needs_flush(memcg->vmstats);

	trace_memcg_flush_stats(memcg, atomic_long_read(&memcg->vmstats->stats_updates),
		force, needs_flush);

	if (!force && !needs_flush)
		return;

	if (mem_cgroup_is_root(memcg))
		WRITE_ONCE(flush_last_time, jiffies_64);

	css_rstat_flush(&memcg->css);
}

/*
 * mem_cgroup_flush_stats - flush the stats of a memory cgroup subtree
 * @memcg: root of the subtree to flush
 *
 * Flushing is serialized by the underlying global rstat lock. There is also a
 * minimum amount of work to be done even if there are no stat updates to flush.
 * Hence, we only flush the stats if the updates delta exceeds a threshold. This
 * avoids unnecessary work and contention on the underlying lock.
 */
/*
 * 对 @memcg（NULL 表示根组）执行阈值控制的子树聚合。底层全局 rstat 锁使
 * flush 串行且即使无增量也有固定成本，所以频繁 reader 不应强制刷新。
 */
void mem_cgroup_flush_stats(struct mem_cgroup *memcg)
{
	if (mem_cgroup_disabled())
		return;

	if (!memcg)
		memcg = root_mem_cgroup;

	__mem_cgroup_flush_stats(memcg, false);
}

/*
 * 只有周期 flusher 已迟到整整一个额外周期时才由当前 reader 代为刷新；
 * 这保护延迟敏感路径不因正常的短暂统计陈旧争用 rstat 锁。
 */
void mem_cgroup_flush_stats_ratelimited(struct mem_cgroup *memcg)
{
	/* Only flush if the periodic flusher is one full cycle late */
	/* 只有周期工作完整迟到一轮，当前读者才代为刷新。 */
	if (time_after64(jiffies_64, READ_ONCE(flush_last_time) + 2*FLUSH_TIME))
		mem_cgroup_flush_stats(memcg);
}

/*
 * 周期工作从根组强制 flush，覆盖整棵层级，然后重新排队。@w 不需要单独
 * 释放；运行在 workqueue 进程上下文，可睡眠。
 */
static void flush_memcg_stats_dwork(struct work_struct *w)
{
	/*
	 * Deliberately ignore memcg_vmstats_needs_flush() here so that flushing
	 * in latency-sensitive paths is as cheap as possible.
	 */
	/* 周期任务故意无视阈值并强制收敛，让延迟敏感读路径尽量无需 flush。 */
	__mem_cgroup_flush_stats(root_mem_cgroup, true);
	queue_delayed_work(system_dfl_wq, &stats_flush_dwork, FLUSH_TIME);
}

/*
 * memcg_page_state() - 读取最近聚合的层级 state。
 * @memcg 为借用，@idx 是已登记项目；返回项目原生单位的非负快照。不主动
 * flush，因此适合热读但不承诺与刚发生的 per-CPU 更新同步。
 */
unsigned long memcg_page_state(struct mem_cgroup *memcg, int idx)
{
	long x;
	int i = memcg_stats_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	x = READ_ONCE(memcg->vmstats->state[i]);
#ifdef CONFIG_SMP
	if (x < 0)
		x = 0;
#endif
	return x;
}

/*
 * memcg_stat_item_valid() - 验证外部统计枚举是否能安全访问 memcg 数组。
 * @idx 可以是任意整数；越过枚举范围或映射为 U8_MAX 都返回 false。纯只读。
 */
bool memcg_stat_item_valid(int idx)
{
	if ((u32)idx >= MEMCG_NR_STAT)
		return false;

	return !BAD_STAT_IDX(memcg_stats_index(idx));
}

static int memcg_page_state_unit(int item);

/*
 * Normalize the value passed into memcg_rstat_updated() to be in pages. Round
 * up non-zero sub-page updates to 1 page as zero page updates are ignored.
 */
/*
 * memcg_state_val_in_pages() - 把某统计项的带符号增量归一化为“页”通知量。
 *
 * @idx 决定原始单位，@val 可正可负。字节级非零小增量向外至少算一页，避免
 * memcg_rstat_updated() 将其当零忽略；符号保持不变。只调整刷新启发值，不
 * 改写实际 state 计数，因此舍入不会破坏最终统计精度。
 */
static long memcg_state_val_in_pages(int idx, long val)
{
	int unit = memcg_page_state_unit(idx);
	long res;

	if (!val || unit == PAGE_SIZE)
		return val;

	/* Get the absolute value of (val * unit / PAGE_SIZE). */
	/* 先计算 val × 单位 / PAGE_SIZE 的绝对值。 */
	res = mult_frac(abs(val), unit, PAGE_SIZE);
	/* Round up zero values. */
	/* 非零小量若整除为零则向上取一页，确保触发刷新启发式。 */
	res = res ? : 1;

	return val < 0 ? -res : res;
}

#ifdef CONFIG_MEMCG_V1
/*
 * Used in mod_memcg_state() and mod_memcg_lruvec_state() to avoid race with
 * reparenting of non-hierarchical state_locals.
 */
/*
 * get_non_dying_memcg_start() - 为 v1 local 统计选择尚未进入重挂阶段的祖先。
 * @memcg 是借用起点，@rcu_locked 为输出。v2 直接返回原组；v1 在 RCU 下向
 * 父链跳过 dying 组，并令结束 helper 负责解锁，防止与 local 统计搬移竞态。
 */
static inline struct mem_cgroup *get_non_dying_memcg_start(struct mem_cgroup *memcg,
							   bool *rcu_locked)
{
	/* Rebinding can cause this value to be changed at runtime */
	/* v1/v2 重新绑定会在运行期改变该判断，不能当编译期常量。 */
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys)) {
		*rcu_locked = false;
		return memcg;
	}

	rcu_read_lock();
	*rcu_locked = true;

	while (memcg_is_dying(memcg))
		memcg = parent_mem_cgroup(memcg);

	return memcg;
}

/* 与 start helper 配对；仅 @rcu_locked 为 true 时结束其 RCU 读侧临界区。 */
static inline void get_non_dying_memcg_end(bool rcu_locked)
{
	if (!rcu_locked)
		return;

	rcu_read_unlock();
}
#else
/* v1 关闭时不存在 non-hierarchical 重挂竞态，stub 原样返回借用指针。 */
static inline struct mem_cgroup *get_non_dying_memcg_start(struct mem_cgroup *memcg,
							   bool *rcu_locked)
{
	return memcg;
}

/* 对应无锁 start stub；@rcu_locked 恒不需要处理，函数无副作用。 */
static inline void get_non_dying_memcg_end(bool rcu_locked)
{
}
#endif

/*
 * __mod_memcg_state() - 在已选定的存活 memcg 上更新一项 per-CPU state。
 *
 * @memcg 为借用且不会在本调用中重挂，@idx 指定项目，@val 是项目原生单位的
 * 带符号增量。get_cpu() 固定当前 CPU，先写真实计数，再把通知量换算为页并
 * 登记 rstat；返回前恢复抢占。无直接返回值，坏索引只告警而不越界写入。
 */
static void __mod_memcg_state(struct mem_cgroup *memcg,
			      enum memcg_stat_item idx, long val)
{
	int i = memcg_stats_index(idx);
	int cpu;

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return;

	cpu = get_cpu();

	this_cpu_add(memcg->vmstats_percpu->state[i], val);
	val = memcg_state_val_in_pages(idx, val);
	memcg_rstat_updated(memcg, val, cpu);

	trace_mod_memcg_state(memcg, idx, val);

	put_cpu();
}

/**
 * mod_memcg_state - update cgroup memory statistics
 * @memcg: the memory cgroup
 * @idx: the stat item - can be enum memcg_stat_item or enum node_stat_item
 * @val: delta to add to the counter, can be negative
 */
/*
 * mod_memcg_state() - 面向通用调用者的 memcg state 更新入口。
 *
 * @memcg 是尚未 released 的借用指针；@idx/@val 分别是项目和带符号增量。
 * v1 下先在 RCU 中避开 dying 组，随后调用 per-CPU 更新核心；v2 直接更新。
 * 返回：无直接返回值。函数不刷新聚合值，reader 可能暂时看到旧的层级统计。
 */
void mod_memcg_state(struct mem_cgroup *memcg, enum memcg_stat_item idx,
		       int val)
{
	bool rcu_locked = false;

	if (mem_cgroup_disabled())
		return;

	memcg = get_non_dying_memcg_start(memcg, &rcu_locked);
	__mod_memcg_state(memcg, idx, val);
	get_non_dying_memcg_end(rcu_locked);
}

#ifdef CONFIG_MEMCG_V1
/* idx can be of type enum memcg_stat_item or node_stat_item. */
/* @idx 可来自两类枚举，因为二者在初始化阶段统一映射到同一稠密数组。 */
/*
 * memcg_page_state_local() - 读取 v1 本组、不含后代的聚合 state 快照。
 * @memcg 为借用，@idx 为已登记项目；SMP 传播中出现的暂时负值对外钳零。
 */
unsigned long memcg_page_state_local(struct mem_cgroup *memcg, int idx)
{
	long x;
	int i = memcg_stats_index(idx);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return 0;

	x = READ_ONCE(memcg->vmstats->state_local[i]);
#ifdef CONFIG_SMP
	if (x < 0)
		x = 0;
#endif
	return x;
}

/*
 * reparent_memcg_state_local() - 守恒迁移一项 v1 non-hierarchical 统计。
 * 读取 @memcg 的 local 值后从子组扣除、向 @parent 加入；两者都是借用指针，
 * @idx 单位保持不变。调用者负责保证本轮搬移不会与新的子组更新交错。
 */
void reparent_memcg_state_local(struct mem_cgroup *memcg,
				struct mem_cgroup *parent, int idx)
{
	unsigned long value = memcg_page_state_local(memcg, idx);

	__mod_memcg_state(memcg, idx, -value);
	__mod_memcg_state(parent, idx, value);
}
#endif

/*
 * __mod_memcg_lruvec_state() - 同步更新 memcg 与其 node 交叉点的 per-CPU 源值。
 * @pn 固定 memcg/node，@idx/@val 是项目及原生单位增量。函数固定当前 CPU，
 * 分别更新 memcg 和 lruvec 数组，再只登记一次 rstat 通知，保持两个视图同步。
 */
static void __mod_memcg_lruvec_state(struct mem_cgroup_per_node *pn,
				     enum node_stat_item idx, long val)
{
	struct mem_cgroup *memcg = pn->memcg;
	int i = memcg_stats_index(idx);
	int cpu;

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return;

	cpu = get_cpu();

	/* Update memcg */
	/* 先更新 cgroup 维度的 per-CPU 原始值。 */
	this_cpu_add(memcg->vmstats_percpu->state[i], val);

	/* Update lruvec */
	/* 再更新同一 NUMA 交叉点的 per-CPU 原始值。 */
	this_cpu_add(pn->lruvec_stats_percpu->state[i], val);

	val = memcg_state_val_in_pages(idx, val);
	memcg_rstat_updated(memcg, val, cpu);
	trace_mod_memcg_lruvec_state(memcg, idx, val);

	put_cpu();
}

/*
 * mod_memcg_lruvec_state() - 处理 v1 dying 重挂后再更新正确 lruvec。
 * @lruvec 提供原 node，函数稳定其 memcg 或祖先，再用同一 node_id 重新取得
 * pn，避免把统计写入已迁走的子组。返回无直接值，RCU 配对由 start/end 管理。
 */
static void mod_memcg_lruvec_state(struct lruvec *lruvec,
				     enum node_stat_item idx,
				     int val)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	struct mem_cgroup_per_node *pn;
	struct mem_cgroup *memcg;
	bool rcu_locked = false;

	pn = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	memcg = get_non_dying_memcg_start(pn->memcg, &rcu_locked);
	pn = memcg->nodeinfo[pgdat->node_id];

	__mod_memcg_lruvec_state(pn, idx, val);

	get_non_dying_memcg_end(rcu_locked);
}

/**
 * mod_lruvec_state - update lruvec memory statistics
 * @lruvec: the lruvec
 * @idx: the stat item
 * @val: delta to add to the counter, can be negative
 *
 * The lruvec is the intersection of the NUMA node and a cgroup. This
 * function updates the all three counters that are affected by a
 * change of state at this level: per-node, per-cgroup, per-lruvec.
 */
/*
 * lruvec 是 NUMA node 与 memcg 的交叉点；一次状态变化必须同时反映到全局
 * node、memcg 子树及该 lruvec 三种观察口径。本函数先更新 node，再在 memcg
 * 启用时更新另两者；@val 为带符号原生单位增量，返回无直接值。
 */
void mod_lruvec_state(struct lruvec *lruvec, enum node_stat_item idx,
			int val)
{
	/* Update node */
	/* 全局 NUMA node 口径始终要更新。 */
	mod_node_page_state(lruvec_pgdat(lruvec), idx, val);

	/* Update memcg and lruvec */
	/* memcg 启用时再同步更新 cgroup 与交叉 lruvec 口径。 */
	if (!mem_cgroup_disabled())
		mod_memcg_lruvec_state(lruvec, idx, val);
}

/*
 * lruvec_stat_mod_folio() - 按 folio 当下归属选择统计落点。
 * @folio 为借用且调用者稳定其存储，@idx/@val 为统计项与增量。RCU 防止
 * memcg 在查找到更新间释放；未跟踪 folio 只更新 node，否则更新完整 lruvec。
 */
void lruvec_stat_mod_folio(struct folio *folio, enum node_stat_item idx,
			     int val)
{
	struct mem_cgroup *memcg;
	pg_data_t *pgdat = folio_pgdat(folio);
	struct lruvec *lruvec;

	rcu_read_lock();
	memcg = folio_memcg(folio);
	/* Untracked pages have no memcg, no lruvec. Update only the node */
	/* 未跟踪页没有 memcg/lruvec 归属，只能更新全局 node。 */
	if (!memcg) {
		rcu_read_unlock();
		mod_node_page_state(pgdat, idx, val);
		return;
	}

	lruvec = mem_cgroup_lruvec(memcg, pgdat);
	mod_lruvec_state(lruvec, idx, val);
	rcu_read_unlock();
}
EXPORT_SYMBOL(lruvec_stat_mod_folio);

/*
 * mod_lruvec_kmem_state() - 从内核虚拟地址恢复 node/objcg 并更新 slab 类统计。
 * @p 必须指向直接映射的已分配对象，@idx/@val 指定项目和字节增量。RCU 覆盖
 * objcg 重挂；无归属时只更新 node，有归属时更新对应 lruvec。无直接返回值。
 */
void mod_lruvec_kmem_state(void *p, enum node_stat_item idx, int val)
{
	pg_data_t *pgdat = page_pgdat(virt_to_page(p));
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;

	rcu_read_lock();
	memcg = mem_cgroup_from_virt(p);

	/*
	 * Untracked pages have no memcg, no lruvec. Update only the
	 * node. If we reparent the slab objects to the root memcg,
	 * when we free the slab object, we need to update the per-memcg
	 * vmstats to keep it correct for the root memcg.
	 */
	/*
	 * 未跟踪对象只更新 node；若 slab 对象重挂到根组，则释放时仍要更新根组
	 * vmstat，才能保持根 memcg 的对象统计守恒。
	 */
	if (!memcg) {
		mod_node_page_state(pgdat, idx, val);
	} else {
		lruvec = mem_cgroup_lruvec(memcg, pgdat);
		mod_lruvec_state(lruvec, idx, val);
	}
	rcu_read_unlock();
}

/**
 * count_memcg_events - account VM events in a cgroup
 * @memcg: the memory cgroup
 * @idx: the event item
 * @count: the number of events that occurred
 */
/*
 * count_memcg_events() - 把 @count 次 VM 事件记入 @memcg 当前 CPU 槽位。
 * @idx 必须是登记事件；函数固定 CPU、更新真实次数并通知 rstat，不同步刷新。
 * @memcg 为借用且调用者保证尚未 released，返回无直接值。
 */
void count_memcg_events(struct mem_cgroup *memcg, enum vm_event_item idx,
			  unsigned long count)
{
	int i = memcg_events_index(idx);
	int cpu;

	if (mem_cgroup_disabled())
		return;

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, idx))
		return;

	cpu = get_cpu();

	this_cpu_add(memcg->vmstats_percpu->events[i], count);
	memcg_rstat_updated(memcg, count, cpu);
	trace_count_memcg_events(memcg, idx, count);

	put_cpu();
}

/*
 * memcg_events() - 读取 @memcg 子树的最近聚合事件次数。
 * @event 为已登记 vm_event_item；返回 unsigned long 快照，不主动触发 rstat
 * flush。坏索引告警并返回 0，函数不取得 memcg 引用。
 */
unsigned long memcg_events(struct mem_cgroup *memcg, int event)
{
	int i = memcg_events_index(event);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, event))
		return 0;

	return READ_ONCE(memcg->vmstats->events[i]);
}

/* 检查 @idx 是否既在 VM 枚举范围内、又由当前配置登记到 memcg event 表。 */
bool memcg_vm_event_item_valid(enum vm_event_item idx)
{
	if (idx >= NR_VM_EVENT_ITEMS)
		return false;

	return !BAD_STAT_IDX(memcg_events_index(idx));
}

#ifdef CONFIG_MEMCG_V1
/*
 * memcg_events_local() - 读取 v1 仅本组、不含后代的事件计数快照。
 * 入参与错误语义同 memcg_events()；只在 CONFIG_MEMCG_V1 下存在。
 */
unsigned long memcg_events_local(struct mem_cgroup *memcg, int event)
{
	int i = memcg_events_index(event);

	if (WARN_ONCE(BAD_STAT_IDX(i), "%s: missing stat item %d\n", __func__, event))
		return 0;

	return READ_ONCE(memcg->vmstats->events_local[i]);
}
#endif

/*
 * 【归属查找与层级遍历】
 *
 * 本区把 task/mm/folio 转换为 memcg，并提供可在回收中跨调用保存游标的
 * mem_cgroup_iter()。task->memcg 可随 cgroup attach 改变，mm_owner 也可
 * 退出；短时查询使用 RCU，跨越临界区则 css_tryget()/css_put()。folio
 * 归属还可能在 cgroup offline 时沿 objcg 重挂到父组，因此“拿到指针”
 * 与“持有稳定引用”必须分开理解。
 */
/*
 * mem_cgroup_from_task() - 在现有 RCU/css 保护下取得任务当前 memcg 裸指针。
 *
 * @p 是借用 task，可因 mm owner 更新竞态而为 NULL；NULL 返回 NULL。成功时
 * 通过 task_css() 返回内嵌 memcg，但不增加 css 引用，调用者不得把结果带出
 * 自己的保护范围。该 helper 只做身份转换，不睡眠、不保证任务不会被迁组。
 */
struct mem_cgroup *mem_cgroup_from_task(struct task_struct *p)
{
	/*
	 * mm_update_next_owner() may clear mm->owner to NULL
	 * if it races with swapoff, page migration, etc.
	 * So this can be called with p == NULL.
	 */
	/*
	 * swapoff、页迁移等路径可与 mm_update_next_owner() 竞态，后者会暂时把
	 * mm->owner 清成 NULL；因此空任务是合法观察结果，不能直接解引用。
	 */
	if (unlikely(!p))
		return NULL;

	return mem_cgroup_from_css(task_css(p, memory_cgrp_id));
}
EXPORT_SYMBOL(mem_cgroup_from_task);

/*
 * active_memcg() - 读取当前执行上下文临时覆盖的计费身份。
 * 任务上下文使用 current->active_memcg；中断上下文改读本 CPU 槽位，因为
 * current 未必是发起 I/O 的任务。返回借用指针，可为 NULL，不取得引用。
 */
static __always_inline struct mem_cgroup *active_memcg(void)
{
	if (!in_task())
		return this_cpu_read(int_active_memcg);
	else
		return current->active_memcg;
}

/**
 * get_mem_cgroup_from_mm: Obtain a reference on given mm_struct's memcg.
 * @mm: mm from which memcg should be extracted. It can be NULL.
 *
 * Obtain a reference on mm->memcg and returns it if successful. If mm
 * is NULL, then the memcg is chosen as follows:
 * 1) The active memcg, if set.
 * 2) current->mm->memcg, if available
 * 3) root memcg
 * If mem_cgroup is disabled, NULL is returned.
 */
/*
 * get_mem_cgroup_from_mm() - 取得用于新分配计费的稳定 memcg 引用。
 *
 * @mm 是借用 mm，可为 NULL。选择顺序为 active 覆盖、current->mm、根组；
 * 非空 mm 则在 RCU 下读取可变化的 owner，并循环 css_tryget()，直到取得不会
 * 被释放的组。返回 NULL 仅表示控制器关闭；其他返回值由调用者 mem_cgroup_put()
 * 或 css_put()，根组因 CSS_NO_REF 可安全执行同一配对。函数本身不睡眠。
 */
struct mem_cgroup *get_mem_cgroup_from_mm(struct mm_struct *mm)
{
	struct mem_cgroup *memcg;

	if (mem_cgroup_disabled())
		return NULL;

	/*
	 * Page cache insertions can happen without an
	 * actual mm context, e.g. during disk probing
	 * on boot, loopback IO, acct() writes etc.
	 *
	 * No need to css_get on root memcg as the reference
	 * counting is disabled on the root level in the
	 * cgroup core. See CSS_NO_REF.
	 */
	/*
	 * page cache 插入并不总有 mm，例如启动探测、loop I/O 和 acct 写入。
	 * 此时优先继承发起者设置的 active memcg；根 css 采用 CSS_NO_REF，故
	 * 返回根组无需真实增加引用，而远端 active memcg 必须显式 css_get()。
	 */
	if (unlikely(!mm)) {
		memcg = active_memcg();
		if (unlikely(memcg)) {
			/* remote memcg must hold a ref */
			/* active 指针可能跨越设置者作用域，返回前必须转成调用者持有引用。 */
			css_get(&memcg->css);
			return memcg;
		}
		mm = current->mm;
		if (unlikely(!mm))
			return root_mem_cgroup;
	}

	rcu_read_lock();
	do {
		memcg = mem_cgroup_from_task(rcu_dereference(mm->owner));
		if (unlikely(!memcg))
			memcg = root_mem_cgroup;
	} while (!css_tryget(&memcg->css));
	rcu_read_unlock();
	return memcg;
}
EXPORT_SYMBOL(get_mem_cgroup_from_mm);

/**
 * get_mem_cgroup_from_current - Obtain a reference on current task's memcg.
 */
/*
 * get_mem_cgroup_from_current() - 取得 current 当下归属的 css 引用。
 * 入参：无。控制器关闭返回 NULL；否则在 RCU 下读取并 css_tryget()，若正逢
 * offline 导致取引用失败，则退出临界区重试。成功结果由调用者 put，不睡眠。
 */
struct mem_cgroup *get_mem_cgroup_from_current(void)
{
	struct mem_cgroup *memcg;

	if (mem_cgroup_disabled())
		return NULL;

again:
	rcu_read_lock();
	memcg = mem_cgroup_from_task(current);
	if (!css_tryget(&memcg->css)) {
		rcu_read_unlock();
		goto again;
	}
	rcu_read_unlock();
	return memcg;
}

/**
 * get_mem_cgroup_from_folio - Obtain a reference on a given folio's memcg.
 * @folio: folio from which memcg should be extracted.
 *
 * See folio_memcg() for folio->objcg/memcg binding rules.
 */
/*
 * get_mem_cgroup_from_folio() - 取得 folio 当前 objcg 所指 memcg 的稳定引用。
 *
 * @folio 是调用者稳定的借用对象。未计费 folio 返回根组；已计费者在 RCU 下
 * 读取可因 offline 重挂的 memcg，并循环到 css_tryget() 成功。控制器关闭
 * 返回 NULL；其余结果由调用者 put。引用只保生命周期，不冻结 folio 归属。
 */
struct mem_cgroup *get_mem_cgroup_from_folio(struct folio *folio)
{
	struct mem_cgroup *memcg;

	if (mem_cgroup_disabled())
		return NULL;

	if (!folio_memcg_charged(folio))
		return root_mem_cgroup;

	rcu_read_lock();
	do {
		memcg = folio_memcg(folio);
	} while (unlikely(!css_tryget(&memcg->css)));
	rcu_read_unlock();
	return memcg;
}

/**
 * mem_cgroup_iter - iterate over memory cgroup hierarchy
 * @root: hierarchy root
 * @prev: previously returned memcg, NULL on first invocation
 * @reclaim: cookie for shared reclaim walks, NULL for full walks
 *
 * Returns references to children of the hierarchy below @root, or
 * @root itself, or %NULL after a full round-trip.
 *
 * Caller must pass the return value in @prev on subsequent
 * invocations for reference counting, or use mem_cgroup_iter_break()
 * to cancel a hierarchy walk before the round-trip is complete.
 *
 * Reclaimers can specify a node in @reclaim to divide up the memcgs
 * in the hierarchy among all concurrent reclaimers operating on the
 * same node.
 */
/*
 * mem_cgroup_iter() - 在 @root 子树中取得下一个可引用 memcg。
 *
 * @root 为遍历根，NULL 表示系统根；@prev 是上次返回且由迭代器持有引用的
 * 节点，调用后该引用会被消费；@reclaim 可选地保存回收游标和 generation，
 * 让多次扫描公平续接。成功返回带 css 引用的 memcg，结束返回 NULL。
 * 节点 offline、游标失效和并发删除通过 css_tryget 与 generation 复核处理；
 * 提前停止必须调用 mem_cgroup_iter_break() 归还 @prev。
 */
struct mem_cgroup *mem_cgroup_iter(struct mem_cgroup *root,
				   struct mem_cgroup *prev,
				   struct mem_cgroup_reclaim_cookie *reclaim)
{
	struct mem_cgroup_reclaim_iter *iter;
	struct cgroup_subsys_state *css;
	struct mem_cgroup *pos;
	struct mem_cgroup *next;

	if (mem_cgroup_disabled())
		return NULL;

	if (!root)
		root = root_mem_cgroup;

	rcu_read_lock();
restart:
	next = NULL;

	if (reclaim) {
		int gen;
		int nid = reclaim->pgdat->node_id;

		iter = &root->nodeinfo[nid]->iter;
		gen = atomic_read(&iter->generation);

		/*
		 * On start, join the current reclaim iteration cycle.
		 * Exit when a concurrent walker completes it.
		 */
		if (!prev)
			reclaim->generation = gen;
		else if (reclaim->generation != gen)
			goto out_unlock;

		pos = READ_ONCE(iter->position);
	} else
		pos = prev;

	css = pos ? &pos->css : NULL;

	while ((css = css_next_descendant_pre(css, &root->css))) {
		/*
		 * Verify the css and acquire a reference.  The root
		 * is provided by the caller, so we know it's alive
		 * and kicking, and don't take an extra reference.
		 */
		if (css == &root->css || css_tryget(css))
			break;
	}

	next = mem_cgroup_from_css(css);

	if (reclaim) {
		/*
		 * The position could have already been updated by a competing
		 * thread, so check that the value hasn't changed since we read
		 * it to avoid reclaiming from the same cgroup twice.
		 */
		if (cmpxchg(&iter->position, pos, next) != pos) {
			if (css && css != &root->css)
				css_put(css);
			goto restart;
		}

		if (!next) {
			atomic_inc(&iter->generation);

			/*
			 * Reclaimers share the hierarchy walk, and a
			 * new one might jump in right at the end of
			 * the hierarchy - make sure they see at least
			 * one group and restart from the beginning.
			 */
			if (!prev)
				goto restart;
		}
	}

out_unlock:
	rcu_read_unlock();
	if (prev && prev != root)
		css_put(&prev->css);

	return next;
}

/**
 * mem_cgroup_iter_break - abort a hierarchy walk prematurely
 * @root: hierarchy root
 * @prev: last visited hierarchy member as returned by mem_cgroup_iter()
 */
/*
 * mem_cgroup_iter_break() - 提前结束层级遍历并归还最后一个节点的 css 引用。
 * @root 可为 NULL，@prev 可为 NULL；根组引用未曾增加，只有非根 @prev 需要
 * css_put()。返回无直接值，调用后不得再把同一 @prev 交给迭代器。
 */
void mem_cgroup_iter_break(struct mem_cgroup *root,
			   struct mem_cgroup *prev)
{
	if (!root)
		root = root_mem_cgroup;
	if (prev && prev != root)
		css_put(&prev->css);
}

/*
 * __invalidate_reclaim_iterators() - 清除 @from 各 NUMA 节点上指向死亡组的游标。
 * @from/@dead_memcg 都是 css 生命周期内的借用指针。cmpxchg 仅在游标仍等于
 * dead_memcg 时改为 NULL，避免覆盖并发回收者已经推进的新位置。
 */
static void __invalidate_reclaim_iterators(struct mem_cgroup *from,
					struct mem_cgroup *dead_memcg)
{
	struct mem_cgroup_reclaim_iter *iter;
	struct mem_cgroup_per_node *mz;
	int nid;

	for_each_node(nid) {
		mz = from->nodeinfo[nid];
		iter = &mz->iter;
		cmpxchg(&iter->position, dead_memcg, NULL);
	}
}

/*
 * invalidate_reclaim_iterators() - 在 css released 阶段修复所有祖先回收游标。
 * 从 @dead_memcg 沿父链清理；v1 非层级模式的 parent helper 不到系统根，
 * 因此额外处理 root_mem_cgroup。返回后新回收遍历不会再解引用死亡组。
 */
static void invalidate_reclaim_iterators(struct mem_cgroup *dead_memcg)
{
	struct mem_cgroup *memcg = dead_memcg;
	struct mem_cgroup *last;

	do {
		__invalidate_reclaim_iterators(memcg, dead_memcg);
		last = memcg;
	} while ((memcg = parent_mem_cgroup(memcg)));

	/*
	 * When cgroup1 non-hierarchy mode is used,
	 * parent_mem_cgroup() does not walk all the way up to the
	 * cgroup root (root_mem_cgroup). So we have to handle
	 * dead_memcg from cgroup root separately.
	 */
	/*
	 * cgroup v1 non-hierarchy 的 parent_mem_cgroup() 会提前停止，无法自然访问
	 * root 的共享游标；若漏掉这一步，根级并发回收可能保留死亡 memcg 指针。
	 */
	if (!mem_cgroup_is_root(last))
		__invalidate_reclaim_iterators(root_mem_cgroup,
						dead_memcg);
}

/**
 * mem_cgroup_scan_tasks - iterate over tasks of a memory cgroup hierarchy
 * @memcg: hierarchy root
 * @fn: function to call for each task
 * @arg: argument passed to @fn
 *
 * This function iterates over tasks attached to @memcg or to any of its
 * descendants and calls @fn for each task. If @fn returns a non-zero
 * value, the function breaks the iteration loop. Otherwise, it will iterate
 * over all tasks and return 0.
 *
 * This function must not be called for the root memory cgroup.
 */
/*
 * mem_cgroup_scan_tasks() - 遍历 @memcg 及其后代的进程并执行回调 @fn。
 *
 * @arg 原样透传，@fn 返回非零即提前停止。函数通过 css_task_iter 稳定每个
 * task，循环中 cond_resched() 防止大层级触发 softlockup；可睡眠。根组被
 * BUG_ON 拒绝。返回无直接值，提前退出时显式释放 memcg 层级迭代引用。
 */
void mem_cgroup_scan_tasks(struct mem_cgroup *memcg,
			   int (*fn)(struct task_struct *, void *), void *arg)
{
	struct mem_cgroup *iter;
	int ret = 0;

	BUG_ON(mem_cgroup_is_root(memcg));

	for_each_mem_cgroup_tree(iter, memcg) {
		struct css_task_iter it;
		struct task_struct *task;

		css_task_iter_start(&iter->css, CSS_TASK_ITER_PROCS, &it);
		while (!ret && (task = css_task_iter_next(&it))) {
			ret = fn(task, arg);
			/* Avoid potential softlockup warning */
			cond_resched();
		}
		css_task_iter_end(&it);
		if (ret) {
			mem_cgroup_iter_break(memcg, iter);
			break;
		}
	}
}

/**
 * folio_lruvec_lock - Lock the lruvec for a folio.
 * @folio: Pointer to the folio.
 *
 * These functions are safe to use under any of the following conditions:
 * - folio locked
 * - folio_test_lru false
 * - folio frozen (refcount of 0)
 *
 * Return: The lruvec this folio is on with its lock held and rcu read lock held.
 */
/*
 * 【folio 到 lruvec 的锁稳定协议】
 *
 * folio 的 memcg 可与重挂并发变化。三个 folio_lruvec_lock*() 变体先读取
 * 归属、锁定对应 lruvec，再复核锁内归属；若变化则解锁重试。只有“先读后
 * 锁再验证”才能避免拿着旧 memcg 的锁操作新 lruvec。普通、irq、irqsave
 * 变体仅改变中断状态契约，返回时都持有 lru_lock，调用者必须匹配解锁。
 */
struct lruvec *folio_lruvec_lock(struct folio *folio)
{
	struct lruvec *lruvec;

	rcu_read_lock();
retry:
	lruvec = folio_lruvec(folio);
	spin_lock(&lruvec->lru_lock);
	if (unlikely(lruvec_memcg(lruvec) != folio_memcg(folio))) {
		spin_unlock(&lruvec->lru_lock);
		goto retry;
	}

	return lruvec;
}

/**
 * folio_lruvec_lock_irq - Lock the lruvec for a folio.
 * @folio: Pointer to the folio.
 *
 * These functions are safe to use under any of the following conditions:
 * - folio locked
 * - folio_test_lru false
 * - folio frozen (refcount of 0)
 *
 * Return: The lruvec this folio is on with its lock held and interrupts
 * disabled and rcu read lock held.
 */
/*
 * folio_lruvec_lock_irq() - 在关闭本 CPU 中断的同时稳定并锁住 folio lruvec。
 * @folio 必须满足上列三种稳定条件之一。返回时同时持 lru_lock 与 RCU 读锁，
 * 调用者必须用匹配的 irq 解锁 helper；归属复核失败会在不退出 RCU 时重试。
 */
struct lruvec *folio_lruvec_lock_irq(struct folio *folio)
{
	struct lruvec *lruvec;

	rcu_read_lock();
retry:
	lruvec = folio_lruvec(folio);
	spin_lock_irq(&lruvec->lru_lock);
	if (unlikely(lruvec_memcg(lruvec) != folio_memcg(folio))) {
		spin_unlock_irq(&lruvec->lru_lock);
		goto retry;
	}

	return lruvec;
}

/**
 * folio_lruvec_lock_irqsave - Lock the lruvec for a folio.
 * @folio: Pointer to the folio.
 * @flags: Pointer to irqsave flags.
 *
 * These functions are safe to use under any of the following conditions:
 * - folio locked
 * - folio_test_lru false
 * - folio frozen (refcount of 0)
 *
 * Return: The lruvec this folio is on with its lock held and interrupts
 * disabled and rcu read lock held.
 */
/*
 * folio_lruvec_lock_irqsave() - 保存原中断状态后取得稳定 lruvec 锁。
 * @flags 是纯输出，供匹配 unlock_irqrestore 恢复调用前状态；其他前置条件、
 * 重试原因和返回 ownership 与 folio_lruvec_lock_irq() 相同。
 */
struct lruvec *folio_lruvec_lock_irqsave(struct folio *folio,
		unsigned long *flags)
{
	struct lruvec *lruvec;

	rcu_read_lock();
retry:
	lruvec = folio_lruvec(folio);
	spin_lock_irqsave(&lruvec->lru_lock, *flags);
	if (unlikely(lruvec_memcg(lruvec) != folio_memcg(folio))) {
		spin_unlock_irqrestore(&lruvec->lru_lock, *flags);
		goto retry;
	}

	return lruvec;
}

/**
 * mem_cgroup_update_lru_size - account for adding or removing an lru page
 * @lruvec: mem_cgroup per zone lru vector
 * @lru: index of lru list the page is sitting on
 * @zid: zone id of the accounted pages
 * @nr_pages: positive when adding or negative when removing
 *
 * This function must be called under lru_lock, just before a page is added
 * to or just after a page is removed from an lru list.
 */
/*
 * mem_cgroup_update_lru_size() - 在持有 lru_lock 时维护 lruvec/zone 的 LRU 页数。
 * @lruvec 指定 memcg-node 交叉点，@lru 是链表类别，@zid 是 zone，@nr_pages
 * 可正可负。函数同步更新本组与 node 视图并检查下溢；无返回值，调用者保证
 * folio 入队/出队和计数变化处于同一个锁定状态转换中。
 */
void mem_cgroup_update_lru_size(struct lruvec *lruvec, enum lru_list lru,
				int zid, long nr_pages)
{
	struct mem_cgroup_per_node *mz;
	unsigned long *lru_size;
	long size;

	if (mem_cgroup_disabled())
		return;

	mz = container_of(lruvec, struct mem_cgroup_per_node, lruvec);
	lru_size = &mz->lru_zone_size[zid][lru];

	if (nr_pages < 0)
		*lru_size += nr_pages;

	size = *lru_size;
	if (WARN_ONCE(size < 0,
		"%s(%p, %d, %ld): lru_size %ld\n",
		__func__, lruvec, lru, nr_pages, size)) {
		VM_BUG_ON(1);
		*lru_size = 0;
	}

	if (nr_pages > 0)
		*lru_size += nr_pages;
}

/**
 * mem_cgroup_margin - calculate chargeable space of a memory cgroup
 * @memcg: the memory cgroup
 *
 * Returns the maximum amount of memory @mem can be charged with, in
 * pages.
 */
/*
 * 【用户可见统计与 OOM 决策】
 *
 * memory.stat 从 state/event/page_counter 多个来源拼成稳定的按名文本接口；
 * 单位 helper 明确内部页数、KB 或字节如何输出。OOM 区域先选定触发层级，
 * 再在该层级约束下调用通用 out_of_memory()，并可按 memory.oom.group 将
 * 受害进程所在组作为整体杀死。统计输出可容忍瞬时误差，OOM 限额判断则以
 * page_counter 的层级 charge 结果为准，两者不可互相替代。
 */
static unsigned long mem_cgroup_margin(struct mem_cgroup *memcg)
{
	unsigned long margin = 0;
	unsigned long count;
	unsigned long limit;

	count = page_counter_read(&memcg->memory);
	limit = READ_ONCE(memcg->memory.max);
	if (count < limit)
		margin = limit - count;

	if (do_memsw_account()) {
		count = page_counter_read(&memcg->memsw);
		limit = READ_ONCE(memcg->memsw.max);
		if (count < limit)
			margin = min(margin, limit - count);
		else
			margin = 0;
	}

	return margin;
}

struct memory_stat {
	const char *name;
	unsigned int idx;
};

static const struct memory_stat memory_stats[] = {
	{ "anon",			NR_ANON_MAPPED			},
	{ "file",			NR_FILE_PAGES			},
	{ "kernel",			MEMCG_KMEM			},
	{ "kernel_stack",		NR_KERNEL_STACK_KB		},
	{ "pagetables",			NR_PAGETABLE			},
	{ "sec_pagetables",		NR_SECONDARY_PAGETABLE		},
	{ "percpu",			MEMCG_PERCPU_B			},
	{ "sock",			MEMCG_SOCK			},
	{ "vmalloc",			NR_VMALLOC			},
	{ "shmem",			NR_SHMEM			},
#ifdef CONFIG_ZSWAP
	{ "zswap",			MEMCG_ZSWAP_B			},
	{ "zswapped",			MEMCG_ZSWAPPED			},
	{ "zswap_incomp",		MEMCG_ZSWAP_INCOMP		},
#endif
	{ "file_mapped",		NR_FILE_MAPPED			},
	{ "file_dirty",			NR_FILE_DIRTY			},
	{ "file_writeback",		NR_WRITEBACK			},
#ifdef CONFIG_SWAP
	{ "swapcached",			NR_SWAPCACHE			},
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	{ "anon_thp",			NR_ANON_THPS			},
	{ "file_thp",			NR_FILE_THPS			},
	{ "shmem_thp",			NR_SHMEM_THPS			},
#endif
	{ "inactive_anon",		NR_INACTIVE_ANON		},
	{ "active_anon",		NR_ACTIVE_ANON			},
	{ "inactive_file",		NR_INACTIVE_FILE		},
	{ "active_file",		NR_ACTIVE_FILE			},
	{ "unevictable",		NR_UNEVICTABLE			},
	{ "slab_reclaimable",		NR_SLAB_RECLAIMABLE_B		},
	{ "slab_unreclaimable",		NR_SLAB_UNRECLAIMABLE_B		},
#ifdef CONFIG_HUGETLB_PAGE
	{ "hugetlb",			NR_HUGETLB			},
#endif

	/* The memory events */
	{ "workingset_refault_anon",	WORKINGSET_REFAULT_ANON		},
	{ "workingset_refault_file",	WORKINGSET_REFAULT_FILE		},
	{ "workingset_activate_anon",	WORKINGSET_ACTIVATE_ANON	},
	{ "workingset_activate_file",	WORKINGSET_ACTIVATE_FILE	},
	{ "workingset_restore_anon",	WORKINGSET_RESTORE_ANON		},
	{ "workingset_restore_file",	WORKINGSET_RESTORE_FILE		},
	{ "workingset_nodereclaim",	WORKINGSET_NODERECLAIM		},

	{ "pgdemote_kswapd",		PGDEMOTE_KSWAPD		},
	{ "pgdemote_direct",		PGDEMOTE_DIRECT		},
	{ "pgdemote_khugepaged",	PGDEMOTE_KHUGEPAGED	},
	{ "pgdemote_proactive",		PGDEMOTE_PROACTIVE	},
	{ "pgsteal_kswapd",		PGSTEAL_KSWAPD		},
	{ "pgsteal_direct",		PGSTEAL_DIRECT		},
	{ "pgsteal_khugepaged",		PGSTEAL_KHUGEPAGED	},
	{ "pgsteal_proactive",		PGSTEAL_PROACTIVE	},
	{ "pgscan_kswapd",		PGSCAN_KSWAPD		},
	{ "pgscan_direct",		PGSCAN_DIRECT		},
	{ "pgscan_khugepaged",		PGSCAN_KHUGEPAGED	},
	{ "pgscan_proactive",		PGSCAN_PROACTIVE	},
	{ "pgrefill",			PGREFILL		},
#ifdef CONFIG_NUMA_BALANCING
	{ "pgpromote_success",		PGPROMOTE_SUCCESS	},
#endif
};

/* The actual unit of the state item, not the same as the output unit */
/*
 * memcg_page_state_unit() - 返回 @item 内部一个计数单位对应的字节数。
 * 页项目返回 PAGE_SIZE，已按字节累计的 slab/vmalloc 等返回 1，KB 项返回
 * 1024；未知项目触发 WARN 并采用 PAGE_SIZE，供通知量换算而非 ABI 输出。
 */
static int memcg_page_state_unit(int item)
{
	switch (item) {
	case MEMCG_PERCPU_B:
	case MEMCG_ZSWAP_B:
	case NR_SLAB_RECLAIMABLE_B:
	case NR_SLAB_UNRECLAIMABLE_B:
		return 1;
	case NR_KERNEL_STACK_KB:
		return SZ_1K;
	default:
		return PAGE_SIZE;
	}
}

/* Translate stat items to the correct unit for memory.stat output */
/*
 * memcg_page_state_output_unit() - 返回 memory.stat 对 @item 的输出倍率。
 * 它与内部单位 helper 分离，因为某些历史 ABI 使用字节而内部采用页/KB；
 * 调用者把计数乘以该倍率，函数不读取 memcg 状态。
 */
static int memcg_page_state_output_unit(int item)
{
	/*
	 * Workingset state is actually in pages, but we export it to userspace
	 * as a scalar count of events, so special case it here.
	 *
	 * Demotion and promotion activities are exported in pages, consistent
	 * with their global counterparts.
	 */
	switch (item) {
	case WORKINGSET_REFAULT_ANON:
	case WORKINGSET_REFAULT_FILE:
	case WORKINGSET_ACTIVATE_ANON:
	case WORKINGSET_ACTIVATE_FILE:
	case WORKINGSET_RESTORE_ANON:
	case WORKINGSET_RESTORE_FILE:
	case WORKINGSET_NODERECLAIM:
	case PGDEMOTE_KSWAPD:
	case PGDEMOTE_DIRECT:
	case PGDEMOTE_KHUGEPAGED:
	case PGDEMOTE_PROACTIVE:
	case PGSTEAL_KSWAPD:
	case PGSTEAL_DIRECT:
	case PGSTEAL_KHUGEPAGED:
	case PGSTEAL_PROACTIVE:
	case PGSCAN_KSWAPD:
	case PGSCAN_DIRECT:
	case PGSCAN_KHUGEPAGED:
	case PGSCAN_PROACTIVE:
	case PGREFILL:
#ifdef CONFIG_NUMA_BALANCING
	case PGPROMOTE_SUCCESS:
#endif
		return 1;
	default:
		return memcg_page_state_unit(item);
	}
}

/* 读取 @memcg 层级 state 并换算为 memory.stat 约定的输出单位。 */
unsigned long memcg_page_state_output(struct mem_cgroup *memcg, int item)
{
	return memcg_page_state(memcg, item) *
		memcg_page_state_output_unit(item);
}

#ifdef CONFIG_MEMCG_V1
/* v1 版本的本组输出换算，不包含后代；项目与倍率语义同层级读取。 */
unsigned long memcg_page_state_local_output(struct mem_cgroup *memcg, int item)
{
	return memcg_page_state_local(memcg, item) *
		memcg_page_state_output_unit(item);
}
#endif

#ifdef CONFIG_HUGETLB_PAGE
/* 当前配置是否把 hugetlb folio 同时纳入 memory controller；纯策略查询。 */
static bool memcg_accounts_hugetlb(void)
{
	return cgrp_dfl_root.flags & CGRP_ROOT_MEMORY_HUGETLB_ACCOUNTING;
}
#else /* CONFIG_HUGETLB_PAGE */
/* 未编译 hugetlb 支持时恒为 false，使调用点在编译期折叠为普通页语义。 */
static bool memcg_accounts_hugetlb(void)
{
	return false;
}
#endif /* CONFIG_HUGETLB_PAGE */

/*
 * memcg_stat_format() - 把 @memcg 的 memory_stats 表逐项格式化到 @s。
 * 调用者已准备 seq_buf；函数按项目选择 state/event 与配置分支并输出字节或
 * 次数，不负责分配缓冲。缓冲截断由 seq_buf 记录，返回无直接值。
 */
static void memcg_stat_format(struct mem_cgroup *memcg, struct seq_buf *s)
{
	int i;

	/*
	 * Provide statistics on the state of the memory subsystem as
	 * well as cumulative event counters that show past behavior.
	 *
	 * This list is ordered following a combination of these gradients:
	 * 1) generic big picture -> specifics and details
	 * 2) reflecting userspace activity -> reflecting kernel heuristics
	 *
	 * Current memory state:
	 */
	mem_cgroup_flush_stats(memcg);

	for (i = 0; i < ARRAY_SIZE(memory_stats); i++) {
		u64 size;

#ifdef CONFIG_HUGETLB_PAGE
		if (unlikely(memory_stats[i].idx == NR_HUGETLB) &&
			!memcg_accounts_hugetlb())
			continue;
#endif
		size = memcg_page_state_output(memcg, memory_stats[i].idx);
		seq_buf_printf(s, "%s %llu\n", memory_stats[i].name, size);

		if (unlikely(memory_stats[i].idx == NR_SLAB_UNRECLAIMABLE_B)) {
			size += memcg_page_state_output(memcg,
							NR_SLAB_RECLAIMABLE_B);
			seq_buf_printf(s, "slab %llu\n", size);
		}
	}

	/* Accumulated memory events */
	seq_buf_printf(s, "pgscan %lu\n",
		       memcg_page_state(memcg, PGSCAN_KSWAPD) +
		       memcg_page_state(memcg, PGSCAN_DIRECT) +
		       memcg_page_state(memcg, PGSCAN_PROACTIVE) +
		       memcg_page_state(memcg, PGSCAN_KHUGEPAGED));
	seq_buf_printf(s, "pgsteal %lu\n",
		       memcg_page_state(memcg, PGSTEAL_KSWAPD) +
		       memcg_page_state(memcg, PGSTEAL_DIRECT) +
		       memcg_page_state(memcg, PGSTEAL_PROACTIVE) +
		       memcg_page_state(memcg, PGSTEAL_KHUGEPAGED));

	for (i = 0; i < ARRAY_SIZE(memcg_vm_event_stat); i++) {
#ifdef CONFIG_MEMCG_V1
		if (memcg_vm_event_stat[i] == PGPGIN ||
		    memcg_vm_event_stat[i] == PGPGOUT)
			continue;
#endif
		seq_buf_printf(s, "%s %lu\n",
			       vm_event_name(memcg_vm_event_stat[i]),
			       memcg_events(memcg, memcg_vm_event_stat[i]));
	}
}

/*
 * memory_stat_format() - 生成完整 memory.stat，包括通用项和 v1/hugetlb 扩展。
 * @memcg/@s 均为借用；先刷新 rstat 取得层级新鲜值，再调用表驱动 formatter。
 * 返回无直接值，不改变限额或计费 ownership。
 */
static void memory_stat_format(struct mem_cgroup *memcg, struct seq_buf *s)
{
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		memcg_stat_format(memcg, s);
	else
		memcg1_stat_format(memcg, s);
	if (seq_buf_has_overflowed(s))
		pr_warn("%s: Warning, stat buffer overflow, please report\n", __func__);
}

/**
 * mem_cgroup_print_oom_context: Print OOM information relevant to
 * memory controller.
 * @memcg: The memory cgroup that went over limit
 * @p: Task that is going to be killed
 *
 * NOTE: @memcg and @p's mem_cgroup can be different when hierarchy is
 * enabled
 */
/*
 * mem_cgroup_print_oom_context() - 在 memcg OOM 日志中打印触发组与任务上下文。
 * @memcg/@p 为诊断期间借用指针；函数只向日志输出 cgroup 路径和约束，不
 * 选择受害者、不改变引用。允许 @p 为当前 OOM 任务，返回无直接值。
 */
void mem_cgroup_print_oom_context(struct mem_cgroup *memcg, struct task_struct *p)
{
	rcu_read_lock();

	if (memcg) {
		pr_cont(",oom_memcg=");
		pr_cont_cgroup_path(memcg->css.cgroup);
	} else
		pr_cont(",global_oom");
	if (p) {
		pr_cont(",task_memcg=");
		pr_cont_cgroup_path(task_cgroup(p, memory_cgrp_id));
	}
	rcu_read_unlock();
}

/**
 * mem_cgroup_print_oom_meminfo: Print OOM memory information relevant to
 * memory controller.
 * @memcg: The memory cgroup that went over limit
 */
/*
 * mem_cgroup_print_oom_meminfo() - 输出 @memcg OOM 域的 usage、limit 和统计。
 * 这是 best-effort 诊断快照，可与并发 charge/reclaim 变化；不用于决策，
 * 不持久保留传入指针，返回无直接值。
 */
void mem_cgroup_print_oom_meminfo(struct mem_cgroup *memcg)
{
	/* Use static buffer, for the caller is holding oom_lock. */
	static char buf[SEQ_BUF_SIZE];
	struct seq_buf s;
	unsigned long memory_failcnt;

	lockdep_assert_held(&oom_lock);

	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		memory_failcnt = atomic_long_read(&memcg->memory_events[MEMCG_MAX]);
	else
		memory_failcnt = memcg->memory.failcnt;

	pr_info("memory: usage %llukB, limit %llukB, failcnt %lu\n",
		K((u64)page_counter_read(&memcg->memory)),
		K((u64)READ_ONCE(memcg->memory.max)), memory_failcnt);
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		pr_info("swap: usage %llukB, limit %llukB, failcnt %lu\n",
			K((u64)page_counter_read(&memcg->swap)),
			K((u64)READ_ONCE(memcg->swap.max)),
			atomic_long_read(&memcg->memory_events[MEMCG_SWAP_MAX]));
#ifdef CONFIG_MEMCG_V1
	else {
		pr_info("memory+swap: usage %llukB, limit %llukB, failcnt %lu\n",
			K((u64)page_counter_read(&memcg->memsw)),
			K((u64)memcg->memsw.max), memcg->memsw.failcnt);
		pr_info("kmem: usage %llukB, limit %llukB, failcnt %lu\n",
			K((u64)page_counter_read(&memcg->kmem)),
			K((u64)memcg->kmem.max), memcg->kmem.failcnt);
	}
#endif

	pr_info("Memory cgroup stats for ");
	pr_cont_cgroup_path(memcg->css.cgroup);
	pr_cont(":");
	seq_buf_init(&s, buf, SEQ_BUF_SIZE);
	memory_stat_format(memcg, &s);
	seq_buf_do_printk(&s, KERN_INFO);
}

/*
 * Return the memory (and swap, if configured) limit for a memcg.
 */
/*
 * mem_cgroup_get_max() - 返回 @memcg 到根路径上最紧的 memory.max 页数。
 * @memcg 为借用；逐祖先读取并取最小值，结果用于 OOM/分配上下文估算，不
 * 冻结并发配置写。根组或无限制路径可返回 PAGE_COUNTER_MAX。
 */
unsigned long mem_cgroup_get_max(struct mem_cgroup *memcg)
{
	unsigned long max = READ_ONCE(memcg->memory.max);

	if (do_memsw_account()) {
		if (mem_cgroup_swappiness(memcg)) {
			/* Calculate swap excess capacity from memsw limit */
			unsigned long swap = READ_ONCE(memcg->memsw.max) - max;

			max += min(swap, (unsigned long)total_swap_pages);
		}
	} else {
		if (mem_cgroup_swappiness(memcg))
			max += min(READ_ONCE(memcg->swap.max),
				   (unsigned long)total_swap_pages);
	}
	return max;
}

/*
 * __memcg_memory_event() - 记录 memory.events 并向 kernfs poll 等待者发通知。
 * @memcg 是事件发生组，@event 指定 low/high/max/OOM 等类别，@local 决定
 * 是否只增本组数组。原子计数保证并发累计不丢失；通知可能唤醒用户 reader，
 * 因此调用位置应在相应状态已提交之后。返回无直接值。
 */
void __memcg_memory_event(struct mem_cgroup *memcg,
			  enum memcg_memory_event event, bool allow_spinning)
{
	bool swap_event = event == MEMCG_SWAP_HIGH || event == MEMCG_SWAP_MAX ||
			  event == MEMCG_SWAP_FAIL;

	/* For now only MEMCG_MAX can happen with !allow_spinning context. */
	VM_WARN_ON_ONCE(!allow_spinning && event != MEMCG_MAX);

	atomic_long_inc(&memcg->memory_events_local[event]);
	if (!swap_event && allow_spinning)
		cgroup_file_notify(&memcg->events_local_file);

	do {
		atomic_long_inc(&memcg->memory_events[event]);
		if (allow_spinning) {
			if (swap_event)
				cgroup_file_notify(&memcg->swap_events_file);
			else
				cgroup_file_notify(&memcg->events_file);
		}

		if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
			break;
		if (cgrp_dfl_root.flags & CGRP_ROOT_MEMORY_LOCAL_EVENTS)
			break;
	} while ((memcg = parent_mem_cgroup(memcg)) &&
		 !mem_cgroup_is_root(memcg));
}
EXPORT_SYMBOL_GPL(__memcg_memory_event);

/*
 * mem_cgroup_out_of_memory() - 在限定 memcg 域内执行一次 OOM 选择。
 *
 * @memcg 是触发硬限额的借用组，@gfp_mask/@order 描述失败分配；函数把
 * current 的 memcg OOM 上下文临时指向该组，调用通用 OOM killer 后恢复。
 * 返回 true 表示已采取或已有 OOM 处理，false 表示调用者仍需走失败/重试。
 * OOM 选择可能发送致命信号并唤醒 reaper，不能从 NMI/硬中断调用。
 */
static bool mem_cgroup_out_of_memory(struct mem_cgroup *memcg, gfp_t gfp_mask,
				     int order)
{
	struct oom_control oc = {
		.zonelist = NULL,
		.nodemask = NULL,
		.memcg = memcg,
		.gfp_mask = gfp_mask,
		.order = order,
	};
	bool ret = true;

	if (mutex_lock_killable(&oom_lock))
		return true;

	if (mem_cgroup_margin(memcg) >= (1 << order))
		goto unlock;

	/*
	 * A few threads which were not waiting at mutex_lock_killable() can
	 * fail to bail out. Therefore, check again after holding oom_lock.
	 */
	ret = out_of_memory(&oc);

unlock:
	mutex_unlock(&oom_lock);
	return ret;
}

/*
 * Returns true if successfully killed one or more processes. Though in some
 * corner cases it can return true even without killing any process.
 */
/*
 * mem_cgroup_oom() - 协调同一 memcg OOM 域的并发处理者并等待处理结果。
 * @memcg/@mask/@order 描述失败分配。函数进入 memcg OOM 同步域，只有被选中
 * 的处理者调用 killer，其余等待或重试；返回 true 表示产生前进条件，false
 * 表示无法处理。可睡眠，调用者据此决定重试 charge 或返回 -ENOMEM。
 */
static bool mem_cgroup_oom(struct mem_cgroup *memcg, gfp_t mask, int order)
{
	bool locked, ret;

	if (order > PAGE_ALLOC_COSTLY_ORDER)
		return false;

	memcg_memory_event(memcg, MEMCG_OOM);

	if (!memcg1_oom_prepare(memcg, &locked))
		return false;

	ret = mem_cgroup_out_of_memory(memcg, mask, order);

	memcg1_oom_finish(memcg, locked);

	return ret;
}

/**
 * mem_cgroup_get_oom_group - get a memory cgroup to clean up after OOM
 * @victim: task to be killed by the OOM killer
 * @oom_domain: memcg in case of memcg OOM, NULL in case of system-wide OOM
 *
 * Returns a pointer to a memory cgroup, which has to be cleaned up
 * by killing all belonging OOM-killable tasks.
 *
 * Caller has to call mem_cgroup_put() on the returned non-NULL memcg.
 */
/*
 * mem_cgroup_get_oom_group() - 为受害任务寻找启用 memory.oom.group 的在线祖先。
 * @victim 为借用 task，@oom_domain 限制搜索边界。成功返回带 css 引用的组，
 * 无整体杀组策略返回 NULL；调用者负责 put，结果用于扩大受害集合而非计费。
 */
struct mem_cgroup *mem_cgroup_get_oom_group(struct task_struct *victim,
					    struct mem_cgroup *oom_domain)
{
	struct mem_cgroup *oom_group = NULL;
	struct mem_cgroup *memcg;

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return NULL;

	if (!oom_domain)
		oom_domain = root_mem_cgroup;

	rcu_read_lock();

	memcg = mem_cgroup_from_task(victim);
	if (mem_cgroup_is_root(memcg))
		goto out;

	/*
	 * If the victim task has been asynchronously moved to a different
	 * memory cgroup, we might end up killing tasks outside oom_domain.
	 * In this case it's better to ignore memory.group.oom.
	 */
	if (unlikely(!mem_cgroup_is_descendant(memcg, oom_domain)))
		goto out;

	/*
	 * Traverse the memory cgroup hierarchy from the victim task's
	 * cgroup up to the OOMing cgroup (or root) to find the
	 * highest-level memory cgroup with oom.group set.
	 */
	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		if (READ_ONCE(memcg->oom_group))
			oom_group = memcg;

		if (memcg == oom_domain)
			break;
	}

	if (oom_group)
		css_get(&oom_group->css);
out:
	rcu_read_unlock();

	return oom_group;
}

/* 输出整体 OOM kill 目标组的路径；@memcg 为借用，仅用于诊断、无状态副作用。 */
void mem_cgroup_print_oom_group(struct mem_cgroup *memcg)
{
	pr_info("Tasks in ");
	pr_cont_cgroup_path(memcg->css.cgroup);
	pr_cont(" are going to be killed due to memory.oom.group set\n");
}

/*
 * The value of NR_MEMCG_STOCK is selected to keep the cached memcgs and their
 * nr_pages in a single cacheline. This may change in future.
 */
#define NR_MEMCG_STOCK 7
#define FLUSHING_CACHED_CHARGE	0
/*
 * 【页额度 stock】
 *
 * 每次分配都沿 page_counter 祖先链原子 charge 成本很高。memcg_stock 为
 * 每 CPU 预充批次：cached 指定唯一 memcg，nr_pages 是已向层级计费但尚未
 * 交给具体 folio 的余额。命中时 consume_stock() 仅做本 CPU 算术；切组、
 * CPU hotplug、显式 drain 或 offline 时再批量 uncharge。stock 持有 memcg
 * 引用，因而缓存余额不能在不 drain 的情况下覆盖为另一组。
 */
struct memcg_stock_pcp {
	local_trylock_t lock;
	uint8_t nr_pages[NR_MEMCG_STOCK];
	struct mem_cgroup *cached[NR_MEMCG_STOCK];

	struct work_struct work;
	unsigned long flags;
	uint8_t drain_idx;
};

static DEFINE_PER_CPU_ALIGNED(struct memcg_stock_pcp, memcg_stock) = {
	.lock = INIT_LOCAL_TRYLOCK(lock),
};

/*
 * NR_OBJ_STOCK is sized so the entire hot path of obj_stock_pcp
 * (lock, accounting metadata, nr_bytes[] and cached[]) fits within a
 * single 64-byte cache line on non-debug 64-bit builds. With 5 slots:
 *   lock(1) + index(1) + node_id(2) + slab stats(4) + nr_bytes(10)
 *   + pad(6) + cached(40) == 64 bytes.
 * A CPU can thus consume/refill/account against five different objcgs
 * (typically per-node variants of the same memcg) while incurring at
 * most one cache miss on the stock.
 */
#define NR_OBJ_STOCK 5
/*
 * 对象 stock 以字节消费、以页向 page_counter 预充。两个槽位允许常见 objcg
 * 交替而少冲刷；cached objcg 持有 percpu_ref，nr_bytes 为可消费余额，
 * nr_slab_reclaimable_b/unreclaimable_b 延迟合并 slab 统计。local_lock
 * 既固定 CPU 又防 NMI/本地重入；远端 drain 通过 work 在目标 CPU 执行。
 */
struct obj_stock_pcp {
	local_trylock_t lock;
	int8_t index;
	int16_t node_id;
	int16_t nr_slab_reclaimable_b;
	int16_t nr_slab_unreclaimable_b;
#if PAGE_SHIFT > 16
	/*
	 * On rare archs with 256KiB base page size (hexagon and powerpc 44x)
	 * keep nr_bytes to unsigned int as uint16_t cannot represent the full
e patches/memcg-uint16_t-for-nr_bytes-in-obj_stock_pcp.patch	 * sub-page remainder. Such archs are not cacheline optimization target.
	 */
	unsigned int nr_bytes[NR_OBJ_STOCK];
#else
	uint16_t nr_bytes[NR_OBJ_STOCK];
#endif
	struct obj_cgroup *cached[NR_OBJ_STOCK];

	struct work_struct work;
	unsigned long flags;
	uint8_t drain_idx;
};

static DEFINE_PER_CPU_ALIGNED(struct obj_stock_pcp, obj_stock) = {
	.lock = INIT_LOCAL_TRYLOCK(lock),
	.index = -1,
	.node_id = NUMA_NO_NODE,
};

static DEFINE_MUTEX(percpu_charge_mutex);

static void drain_obj_stock_slot(struct obj_stock_pcp *stock, int i);
static void drain_obj_stock(struct obj_stock_pcp *stock);
static bool obj_stock_flush_required(struct obj_stock_pcp *stock,
				     struct mem_cgroup *root_memcg);

/**
 * consume_stock: Try to consume stocked charge on this cpu.
 * @memcg: memcg to consume from.
 * @nr_pages: how many pages to charge.
 *
 * Consume the cached charge if enough nr_pages are present otherwise return
 * failure. Also return failure for charge request larger than
 * MEMCG_CHARGE_BATCH or if the local lock is already taken.
 *
 * returns true if successful, false otherwise.
 */
/*
 * consume_stock() - 尝试从当前 CPU 已预充的 @memcg 页余额满足请求。
 * @nr_pages 为基页数。仅 cached 身份相同且余额足够时扣减并返回 true；否则
 * 不改变 stock、返回 false。local_lock 固定 CPU 并串行本地/远端 drain。
 */
static bool consume_stock(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	struct memcg_stock_pcp *stock;
	uint8_t stock_pages;
	bool ret = false;
	int i;

	if (nr_pages > MEMCG_CHARGE_BATCH ||
	    !local_trylock(&memcg_stock.lock))
		return ret;

	stock = this_cpu_ptr(&memcg_stock);

	for (i = 0; i < NR_MEMCG_STOCK; ++i) {
		if (memcg != READ_ONCE(stock->cached[i]))
			continue;

		stock_pages = READ_ONCE(stock->nr_pages[i]);
		if (stock_pages >= nr_pages) {
			WRITE_ONCE(stock->nr_pages[i], stock_pages - nr_pages);
			ret = true;
		}
		break;
	}

	local_unlock(&memcg_stock.lock);

	return ret;
}

/*
 * memcg_uncharge() - 向 memory 及可选 memsw page_counter 归还 @nr_pages。
 * @memcg 为借用非根组；返回无直接值。该 helper 只销页额度，调用者另行维护
 * folio/objcg 引用及 state/event 统计，避免把多种提交点混在一次操作中。
 */
static void memcg_uncharge(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	page_counter_uncharge(&memcg->memory, nr_pages);
	if (do_memsw_account())
		page_counter_uncharge(&memcg->memsw, nr_pages);
}

/*
 * Returns stocks cached in percpu and reset cached information.
 */
/*
 * drain_stock() - 冲刷页 stock 的 cached 余额或对象 stock 的第 @i 个槽位。
 * 调用者持有对应 local_lock；函数把未消费预充量归还所属 memcg/objcg，并
 * 释放缓存身份引用。@i 为 -1 表示页 stock，否则选择对象槽位。
 */
static void drain_stock(struct memcg_stock_pcp *stock, int i)
{
	struct mem_cgroup *old = READ_ONCE(stock->cached[i]);
	uint8_t stock_pages;

	if (!old)
		return;

	stock_pages = READ_ONCE(stock->nr_pages[i]);
	if (stock_pages) {
		memcg_uncharge(old, stock_pages);
		WRITE_ONCE(stock->nr_pages[i], 0);
	}

	css_put(&old->css);
	WRITE_ONCE(stock->cached[i], NULL);
}

/* 冲刷一个 CPU 上的页 stock 及全部对象槽位；调用者已在目标 CPU 串行执行。 */
static void drain_stock_fully(struct memcg_stock_pcp *stock)
{
	int i;

	for (i = 0; i < NR_MEMCG_STOCK; ++i)
		drain_stock(stock, i);
}

/*
 * drain_local_memcg_stock() - 目标 CPU work 回调，归还本 CPU 的页预充余额。
 * @dummy 仅满足 work 接口；local_lock 防止与分配热路径并发，返回无直接值。
 */
static void drain_local_memcg_stock(struct work_struct *dummy)
{
	struct memcg_stock_pcp *stock;

	if (WARN_ONCE(!in_task(), "drain in non-task context"))
		return;

	local_lock(&memcg_stock.lock);

	stock = this_cpu_ptr(&memcg_stock);
	drain_stock_fully(stock);
	clear_bit(FLUSHING_CACHED_CHARGE, &stock->flags);

	local_unlock(&memcg_stock.lock);
}

/* 目标 CPU work 回调，冲刷本 CPU 两个 objcg 槽位及延迟 slab 统计。 */
static void drain_local_obj_stock(struct work_struct *dummy)
{
	struct obj_stock_pcp *stock;

	if (WARN_ONCE(!in_task(), "drain in non-task context"))
		return;

	local_lock(&obj_stock.lock);

	stock = this_cpu_ptr(&obj_stock);
	drain_obj_stock(stock);
	clear_bit(FLUSHING_CACHED_CHARGE, &stock->flags);

	local_unlock(&obj_stock.lock);
}

/*
 * refill_stock() - 把批量 charge 超出本次请求的 @nr_pages 存入当前 CPU。
 * 若 cached 不是 @memcg，先完整 drain 旧身份并转移 css 引用；余额已经计入
 * page_counter，不能再次 charge。过大余额会触发异步 drain，返回无直接值。
 */
static void refill_stock(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	struct memcg_stock_pcp *stock;
	struct mem_cgroup *cached;
	uint8_t stock_pages;
	bool success = false;
	int empty_slot = -1;
	int i;

	/*
	 * For now limit MEMCG_CHARGE_BATCH to 127 and less. In future if we
	 * decide to increase it more than 127 then we will need more careful
	 * handling of nr_pages[] in struct memcg_stock_pcp.
	 */
	BUILD_BUG_ON(MEMCG_CHARGE_BATCH > S8_MAX);

	VM_WARN_ON_ONCE(mem_cgroup_is_root(memcg));

	if (nr_pages > MEMCG_CHARGE_BATCH ||
	    !local_trylock(&memcg_stock.lock)) {
		/*
		 * In case of larger than batch refill or unlikely failure to
		 * lock the percpu memcg_stock.lock, uncharge memcg directly.
		 */
		memcg_uncharge(memcg, nr_pages);
		return;
	}

	stock = this_cpu_ptr(&memcg_stock);
	for (i = 0; i < NR_MEMCG_STOCK; ++i) {
		cached = READ_ONCE(stock->cached[i]);
		if (!cached && empty_slot == -1)
			empty_slot = i;
		if (memcg == READ_ONCE(stock->cached[i])) {
			stock_pages = READ_ONCE(stock->nr_pages[i]) + nr_pages;
			WRITE_ONCE(stock->nr_pages[i], stock_pages);
			if (stock_pages > MEMCG_CHARGE_BATCH)
				drain_stock(stock, i);
			success = true;
			break;
		}
	}

	if (!success) {
		i = empty_slot;
		if (i == -1) {
			i = stock->drain_idx++;
			if (stock->drain_idx == NR_MEMCG_STOCK)
				stock->drain_idx = 0;
			drain_stock(stock, i);
		}
		css_get(&memcg->css);
		WRITE_ONCE(stock->cached[i], memcg);
		WRITE_ONCE(stock->nr_pages[i], nr_pages);
	}

	local_unlock(&memcg_stock.lock);
}

/*
 * is_memcg_drain_needed() - 判断某 CPU stock 是否缓存 @root_memcg 子树的额度。
 * 在持有 stock 锁时检查页身份与对象槽位；返回 true 只表示需要安排目标 CPU
 * flush，不执行销账。@root_memcg 为借用，可代表待 offline 的整棵子树。
 */
static bool is_memcg_drain_needed(struct memcg_stock_pcp *stock,
				  struct mem_cgroup *root_memcg)
{
	struct mem_cgroup *memcg;
	bool flush = false;
	int i;

	rcu_read_lock();
	for (i = 0; i < NR_MEMCG_STOCK; ++i) {
		memcg = READ_ONCE(stock->cached[i]);
		if (!memcg)
			continue;

		if (READ_ONCE(stock->nr_pages[i]) &&
		    mem_cgroup_is_descendant(memcg, root_memcg)) {
			flush = true;
			break;
		}
	}
	rcu_read_unlock();
	return flush;
}

/*
 * schedule_drain_work() - 把 @work 投递到指定 @cpu 并计入全局 drain 完成屏障。
 * CPU 离线竞态由 workqueue/hotplug 协议处理；调用者随后可等待所有已登记工作。
 */
static void schedule_drain_work(int cpu, struct work_struct *work)
{
	/*
	 * Protect housekeeping cpumask read and work enqueue together
	 * in the same RCU critical section so that later cpuset isolated
	 * partition update only need to wait for an RCU GP and flush the
	 * pending work on newly isolated CPUs.
	 */
	guard(rcu)();
	if (!cpu_is_isolated(cpu))
		queue_work_on(cpu, memcg_wq, work);
}

/*
 * Drains all per-CPU charge caches for given root_memcg resp. subtree
 * of the hierarchy under it.
 */
/*
 * drain_all_stock() - 请求所有 CPU 冲刷属于 @root_memcg 子树的缓存余额。
 *
 * @root_memcg 为借用根；函数检查各 CPU 的页/对象 stock，仅对需要者排目标
 * CPU work，并等待本轮 drain 完成。冲刷使 page_counter 和统计重新显露
 * 被预充但未消费的额度，是缩限、offline 和回收前建立准确视图的同步点。
 * 可睡眠，不能在持有 stock 本地锁或原子上下文调用。
 */
void drain_all_stock(struct mem_cgroup *root_memcg)
{
	int cpu, curcpu;

	/* If someone's already draining, avoid adding running more workers. */
	if (!mutex_trylock(&percpu_charge_mutex))
		return;
	/*
	 * Notify other cpus that system-wide "drain" is running
	 * We do not care about races with the cpu hotplug because cpu down
	 * as well as workers from this path always operate on the local
	 * per-cpu data. CPU up doesn't touch memcg_stock at all.
	 */
	migrate_disable();
	curcpu = smp_processor_id();
	for_each_online_cpu(cpu) {
		struct memcg_stock_pcp *memcg_st = &per_cpu(memcg_stock, cpu);
		struct obj_stock_pcp *obj_st = &per_cpu(obj_stock, cpu);

		if (!test_bit(FLUSHING_CACHED_CHARGE, &memcg_st->flags) &&
		    is_memcg_drain_needed(memcg_st, root_memcg) &&
		    !test_and_set_bit(FLUSHING_CACHED_CHARGE,
				      &memcg_st->flags)) {
			if (cpu == curcpu)
				drain_local_memcg_stock(&memcg_st->work);
			else
				schedule_drain_work(cpu, &memcg_st->work);
		}

		if (!test_bit(FLUSHING_CACHED_CHARGE, &obj_st->flags) &&
		    obj_stock_flush_required(obj_st, root_memcg) &&
		    !test_and_set_bit(FLUSHING_CACHED_CHARGE,
				      &obj_st->flags)) {
			if (cpu == curcpu)
				drain_local_obj_stock(&obj_st->work);
			else
				schedule_drain_work(cpu, &obj_st->work);
		}
	}
	migrate_enable();
	mutex_unlock(&percpu_charge_mutex);
}

/*
 * memcg_hotplug_cpu_dead() - CPU @cpu 下线时同步清空其两类 memcg stock。
 * hotplug 核心已阻止该 CPU 再进入本地计费热路径，所以无需本地锁；先销对象
 * 字节缓存，再归还页缓存。始终返回 0，建立 CPUHP_MM_MEMCQ_DEAD 完成条件。
 */
static int memcg_hotplug_cpu_dead(unsigned int cpu)
{
	/* no need for the local lock */
	drain_obj_stock(&per_cpu(obj_stock, cpu));
	drain_stock_fully(&per_cpu(memcg_stock, cpu));

	return 0;
}

/*
 * reclaim_high() - 对 @memcg 及超出 memory.high 的祖先执行直接回收。
 * 每层重新读取 usage/high，仍超限才记 MEMCG_HIGH，并在 PSI memstall 区间内
 * 最多回收 @nr_pages；@gfp_mask 决定可用回收能力。返回各层实际回收页总数，
 * 达到根组停止。该函数不保证把 usage 压到 high，只提供一次有界推进。
 */
static unsigned long reclaim_high(struct mem_cgroup *memcg,
				  unsigned int nr_pages,
				  gfp_t gfp_mask)
{
	unsigned long nr_reclaimed = 0;

	do {
		unsigned long pflags;

		if (page_counter_read(&memcg->memory) <=
		    READ_ONCE(memcg->memory.high))
			continue;

		memcg_memory_event(memcg, MEMCG_HIGH);

		psi_memstall_enter(&pflags);
		nr_reclaimed += try_to_free_mem_cgroup_pages(memcg, nr_pages,
							gfp_mask,
							MEMCG_RECLAIM_MAY_SWAP,
							NULL);
		psi_memstall_leave(&pflags);
	} while ((memcg = parent_mem_cgroup(memcg)) &&
		 !mem_cgroup_is_root(memcg));

	return nr_reclaimed;
}

/*
 * high_work_func() - 异步处理 charge 热路径无法阻塞时遗留的 high 超额。
 * @work 内嵌于 memcg，css 生命周期保证容器有效；以标准批量和 GFP_KERNEL 调用
 * reclaim_high()。结果无需返回，后续 charge/worker 会继续观察剩余超额。
 */
static void high_work_func(struct work_struct *work)
{
	struct mem_cgroup *memcg;

	memcg = container_of(work, struct mem_cgroup, high_work);
	reclaim_high(memcg, MEMCG_CHARGE_BATCH, GFP_KERNEL);
}

/*
 * Clamp the maximum sleep time per allocation batch to 2 seconds. This is
 * enough to still cause a significant slowdown in most cases, while still
 * allowing diagnostics and tracing to proceed without becoming stuck.
 */
#define MEMCG_MAX_HIGH_DELAY_JIFFIES (2UL*HZ)

/*
 * When calculating the delay, we use these either side of the exponentiation to
 * maintain precision and scale to a reasonable number of jiffies (see the table
 * below.
 *
 * - MEMCG_DELAY_PRECISION_SHIFT: Extra precision bits while translating the
 *   overage ratio to a delay.
 * - MEMCG_DELAY_SCALING_SHIFT: The number of bits to scale down the
 *   proposed penalty in order to reduce to a reasonable number of jiffies, and
 *   to produce a reasonable delay curve.
 *
 * MEMCG_DELAY_SCALING_SHIFT just happens to be a number that produces a
 * reasonable delay curve compared to precision-adjusted overage, not
 * penalising heavily at first, but still making sure that growth beyond the
 * limit penalises misbehaviour cgroups by slowing them down exponentially. For
 * example, with a high of 100 megabytes:
 *
 *  +-------+------------------------+
 *  | usage | time to allocate in ms |
 *  +-------+------------------------+
 *  | 100M  |                      0 |
 *  | 101M  |                      6 |
 *  | 102M  |                     25 |
 *  | 103M  |                     57 |
 *  | 104M  |                    102 |
 *  | 105M  |                    159 |
 *  | 106M  |                    230 |
 *  | 107M  |                    313 |
 *  | 108M  |                    409 |
 *  | 109M  |                    518 |
 *  | 110M  |                    639 |
 *  | 111M  |                    774 |
 *  | 112M  |                    921 |
 *  | 113M  |                   1081 |
 *  | 114M  |                   1254 |
 *  | 115M  |                   1439 |
 *  | 116M  |                   1638 |
 *  | 117M  |                   1849 |
 *  | 118M  |                   2000 |
 *  | 119M  |                   2000 |
 *  | 120M  |                   2000 |
 *  +-------+------------------------+
 */
 #define MEMCG_DELAY_PRECISION_SHIFT 20
 #define MEMCG_DELAY_SCALING_SHIFT 14

/*
 * calculate_overage() - 把 @usage 超过 @high 的比例编码为定点数。
 * 未超限返回 0；high=0 按一页处理以避免除零。结果为
 * (usage-high)/high << MEMCG_DELAY_PRECISION_SHIFT，供后续平方形成非线性惩罚。
 */
static u64 calculate_overage(unsigned long usage, unsigned long high)
{
	u64 overage;

	if (usage <= high)
		return 0;

	/*
	 * Prevent division by 0 in overage calculation by acting as if
	 * it was a threshold of 1 page
	 */
	high = max(high, 1UL);

	overage = usage - high;
	overage <<= MEMCG_DELAY_PRECISION_SHIFT;
	return div64_u64(overage, high);
}

/*
 * mem_find_max_overage() - 返回 @memcg 到根之间最大的 memory.high 超额比例。
 * 逐层读取瞬时 counter/策略快照，不刷新、不记事件；最大值代表最严格祖先，
 * 供一次 charge 计算统一节流时间。根组没有 memory.high，故不参与。
 */
static u64 mem_find_max_overage(struct mem_cgroup *memcg)
{
	u64 overage, max_overage = 0;

	do {
		overage = calculate_overage(page_counter_read(&memcg->memory),
					    READ_ONCE(memcg->memory.high));
		max_overage = max(overage, max_overage);
	} while ((memcg = parent_mem_cgroup(memcg)) &&
		 !mem_cgroup_is_root(memcg));

	return max_overage;
}

/*
 * swap_find_max_overage() - 查找层级中最大的 swap.high 超额比例并记事件。
 * 每个实际超额的非根祖先都增加 MEMCG_SWAP_HIGH；返回最大比例供与 memory
 * 超额共同计算延迟。读值可并发变化，事件描述观察到的超限而非预留失败。
 */
static u64 swap_find_max_overage(struct mem_cgroup *memcg)
{
	u64 overage, max_overage = 0;

	do {
		overage = calculate_overage(page_counter_read(&memcg->swap),
					    READ_ONCE(memcg->swap.high));
		if (overage)
			memcg_memory_event(memcg, MEMCG_SWAP_HIGH);
		max_overage = max(overage, max_overage);
	} while ((memcg = parent_mem_cgroup(memcg)) &&
		 !mem_cgroup_is_root(memcg));

	return max_overage;
}

/*
 * Get the number of jiffies that we should penalise a mischievous cgroup which
 * is exceeding its memory.high by checking both it and its ancestors.
 */
/*
 * calculate_high_delay() - 将最大超额 @max_overage 换算成本批 charge 的延迟。
 * 0 表示无需节流；非零先平方放大持续超额，再按精度/尺度位移折算 jiffies，
 * 最后按 @nr_pages 相对 MEMCG_CHARGE_BATCH 的贡献缩放。调用者负责施加全局
 * 两秒上限并真正 schedule_timeout，本函数只计算、无睡眠副作用。
 */
static unsigned long calculate_high_delay(struct mem_cgroup *memcg,
					  unsigned int nr_pages,
					  u64 max_overage)
{
	unsigned long penalty_jiffies;

	if (!max_overage)
		return 0;

	/*
	 * We use overage compared to memory.high to calculate the number of
	 * jiffies to sleep (penalty_jiffies). Ideally this value should be
	 * fairly lenient on small overages, and increasingly harsh when the
	 * memcg in question makes it clear that it has no intention of stopping
	 * its crazy behaviour, so we exponentially increase the delay based on
	 * overage amount.
	 */
	penalty_jiffies = max_overage * max_overage * HZ;
	penalty_jiffies >>= MEMCG_DELAY_PRECISION_SHIFT;
	penalty_jiffies >>= MEMCG_DELAY_SCALING_SHIFT;

	/*
	 * Factor in the task's own contribution to the overage, such that four
	 * N-sized allocations are throttled approximately the same as one
	 * 4N-sized allocation.
	 *
	 * MEMCG_CHARGE_BATCH pages is nominal, so work out how much smaller or
	 * larger the current charge patch is than that.
	 */
	return penalty_jiffies * nr_pages / MEMCG_CHARGE_BATCH;
}

/*
 * Reclaims memory over the high limit. Called directly from
 * try_charge() (context permitting), as well as from the userland
 * return path where reclaim is always able to block.
 */
/*
 * 【memory.high 慢路径】
 *
 * charge 超过 high 不立即失败：try_charge 先记录 current->memcg_nr_pages_over_high，
 * 返回用户态前由 __mem_cgroup_handle_over_high() 合并欠账，尝试直接回收并
 * 按 usage/high 的非线性超额程度计算延迟。这样分配热路径保持短小，同时让
 * 持续超额者自我节流。任务将退出、无法回收或延迟过小时走不同退化分支；
 * high 与 max 不同，前者产生压力和延迟，后者才可触发 memcg OOM。
 */
void __mem_cgroup_handle_over_high(gfp_t gfp_mask)
{
	unsigned long penalty_jiffies;
	unsigned long pflags;
	unsigned long nr_reclaimed;
	unsigned int nr_pages = current->memcg_nr_pages_over_high;
	int nr_retries = MAX_RECLAIM_RETRIES;
	struct mem_cgroup *memcg;
	bool in_retry = false;

	memcg = get_mem_cgroup_from_mm(current->mm);
	/*
	 * 阶段 1：领取并清空当前任务累计的 high 欠账。memcg 带 css 引用，
	 * 即使处理期间任务被迁组，仍对原分配归属完成本轮回收。
	 */
	current->memcg_nr_pages_over_high = 0;

retry_reclaim:
	/*
	 * Bail if the task is already exiting. Unlike memory.max,
	 * memory.high enforcement isn't as strict, and there is no
	 * OOM killer involved, which means the excess could already
	 * be much bigger (and still growing) than it could for
	 * memory.max; the dying task could get stuck in fruitless
	 * reclaim for a long time, which isn't desirable.
	 */
	/* 退出任务即将释放内存；high 是软约束，不应让它长期困在无效回收中。 */
	if (task_is_dying())
		goto out;

	/*
	 * The allocating task should reclaim at least the batch size, but for
	 * subsequent retries we only want to do what's necessary to prevent oom
	 * or breaching resource isolation.
	 *
	 * This is distinct from memory.max or page allocator behaviour because
	 * memory.high is currently batched, whereas memory.max and the page
	 * allocator run every time an allocation is made.
	 */
	/* 首轮覆盖累计欠账，重试只做避免 OOM/破坏隔离所需的最小回收。 */
	nr_reclaimed = reclaim_high(memcg,
				    in_retry ? SWAP_CLUSTER_MAX : nr_pages,
				    gfp_mask);

	/*
	 * 阶段 2：分别计算 RAM 与 swap 的最大祖先超额。平方惩罚让轻微、短暂
	 * 越界成本温和，而持续增长会迅速变贵；同时按本任务实际分配页数分摊。
	 */
	/*
	 * memory.high is breached and reclaim is unable to keep up. Throttle
	 * allocators proactively to slow down excessive growth.
	 */
	/* 回收跟不上 high 超额时主动节流分配者，减缓用量继续增长。 */
	penalty_jiffies = calculate_high_delay(memcg, nr_pages,
					       mem_find_max_overage(memcg));

	penalty_jiffies += calculate_high_delay(memcg, nr_pages,
						swap_find_max_overage(memcg));

	/*
	 * Clamp the max delay per usermode return so as to still keep the
	 * application moving forwards and also permit diagnostics, albeit
	 * extremely slowly.
	 */
	/* 每次用户态返回最多延迟两秒，保证应用仍能缓慢推进并接受诊断。 */
	penalty_jiffies = min(penalty_jiffies, MEMCG_MAX_HIGH_DELAY_JIFFIES);

	/*
	 * Don't sleep if the amount of jiffies this memcg owes us is so low
	 * that it's not even worth doing, in an attempt to be nice to those who
	 * go only a small amount over their memory.high value and maybe haven't
	 * been aggressively reclaimed enough yet.
	 */
	/* 小于百分之一秒的债务不值得睡眠，宽容轻微且可能尚未充分回收的越界。 */
	if (penalty_jiffies <= HZ / 100)
		goto out;

	/*
	 * If reclaim is making forward progress but we're still over
	 * memory.high, we want to encourage that rather than doing allocator
	 * throttling.
	 */
	/* 回收仍有进展时优先继续回收，而不是立即把 allocator 挂起。 */
	if (nr_reclaimed || nr_retries--) {
		in_retry = true;
		goto retry_reclaim;
	}

	/*
	 * 阶段 3：多轮回收仍无法跟上才真正睡眠。PSI 把这段时间标为 memory
	 * stall；killable 睡眠允许退出信号打断，避免 high 反过来阻止任务释放
	 * 内存。所有出口统一 css_put() 归还阶段 1 的引用。
	 */
	/*
	 * Reclaim didn't manage to push usage below the limit, slow
	 * this allocating task down.
	 *
	 * If we exit early, we're guaranteed to die (since
	 * schedule_timeout_killable sets TASK_KILLABLE). This means we don't
	 * need to account for any ill-begotten jiffies to pay them off later.
	 */
	/* 回收最终失败才减速；killable 睡眠若提前结束说明任务将退出，无需结转债务。 */
	psi_memstall_enter(&pflags);
	schedule_timeout_killable(penalty_jiffies);
	psi_memstall_leave(&pflags);

out:
	css_put(&memcg->css);
}

/*
 * try_charge_memcg() - 为即将归属 @memcg 的 @nr_pages 预留层级额度。
 *
 * @memcg 是借用目标，@gfp_mask 控制是否允许 reclaim/OOM/重试，@nr_pages
 * 是基页数。阶段为：stock 快速命中；批量 page_counter_try_charge；
 * memory.high 记欠账；失败后 drain/reclaim/等待；必要时 memcg OOM；最终
 * 重试或返回 -ENOMEM。成功只表示额度已预留，folio 尚未由 commit_charge()
 * 发布归属；失败保证本次未留下 charge。批量多预留部分进入本 CPU stock。
 */
static int try_charge_memcg(struct mem_cgroup *memcg, gfp_t gfp_mask,
			    unsigned int nr_pages)
{
	unsigned int batch = max(MEMCG_CHARGE_BATCH, nr_pages);
	int nr_retries = MAX_RECLAIM_RETRIES;
	struct mem_cgroup *mem_over_limit;
	struct page_counter *counter;
	unsigned long nr_reclaimed;
	bool passed_oom = false;
	unsigned int reclaim_options;
	bool drained = false;
	bool raised_max_event = false;
	unsigned long pflags;
	bool allow_spinning = gfpflags_allow_spinning(gfp_mask);

retry:
	/* 阶段 1：先消费已计费的本 CPU 余额，命中时没有共享原子操作。 */
	if (consume_stock(memcg, nr_pages))
		return 0;

	if (!allow_spinning)
		/* Avoid the refill and flush of the older stock */
		/* 不允许自旋的上下文按实际请求计费，避免触碰旧 stock 的补充与冲刷。 */
		batch = nr_pages;

	reclaim_options = MEMCG_RECLAIM_MAY_SWAP;
	/*
	 * 阶段 2：先尝试批量预留 memsw/memory 层级额度。任一祖先失败时
	 * @counter 精确指出越限 page_counter，再恢复已成功的另一半账目。
	 */
	if (!do_memsw_account() ||
	    page_counter_try_charge(&memcg->memsw, batch, &counter)) {
		if (page_counter_try_charge(&memcg->memory, batch, &counter))
			goto done_restock;
		if (do_memsw_account())
			page_counter_uncharge(&memcg->memsw, batch);
		mem_over_limit = mem_cgroup_from_counter(counter, memory);
	} else {
		mem_over_limit = mem_cgroup_from_counter(counter, memsw);
		reclaim_options &= ~MEMCG_RECLAIM_MAY_SWAP;
	}

	if (batch > nr_pages) {
		/*
		 * 批量预留失败不代表本次实际请求失败；缩为 nr_pages 再试，避免
		 * 因 stock 优化的额外额度触发不必要回收。
		 */
		batch = nr_pages;
		goto retry;
	}

	/*
	 * Prevent unbounded recursion when reclaim operations need to
	 * allocate memory. This might exceed the limits temporarily,
	 * but we prefer facilitating memory reclaim and getting back
	 * under the limit over triggering OOM kills in these cases.
	 */
	/* 回收递归分配走强制路径，宁可短暂越限也要帮助回收完成并避免错误 OOM。 */
	if (unlikely(current->flags & PF_MEMALLOC))
		goto force;

	if (unlikely(task_in_memcg_oom(current)))
		goto nomem;

	if (!gfpflags_allow_blocking(gfp_mask))
		goto nomem;

	/* 阶段 3：普通可阻塞分配先记录 max 事件，再在越限祖先范围直接回收。 */
	__memcg_memory_event(mem_over_limit, MEMCG_MAX, allow_spinning);
	raised_max_event = true;

	psi_memstall_enter(&pflags);
	nr_reclaimed = try_to_free_mem_cgroup_pages(mem_over_limit, nr_pages,
						    gfp_mask, reclaim_options, NULL);
	psi_memstall_leave(&pflags);

	if (mem_cgroup_margin(mem_over_limit) >= nr_pages)
		goto retry;

	if (!drained) {
		/*
		 * 回收后仍无 margin 时只做一次全 CPU drain，把其他 CPU 未消费的
		 * 预充额度归还；重复 drain 只会制造 IPI/workqueue 开销。
		 */
		drain_all_stock(mem_over_limit);
		drained = true;
		goto retry;
	}

	if (gfp_mask & __GFP_NORETRY)
		goto nomem;
	/*
	 * Even though the limit is exceeded at this point, reclaim
	 * may have been able to free some pages.  Retry the charge
	 * before killing the task.
	 *
	 * Only for regular pages, though: huge pages are rather
	 * unlikely to succeed so close to the limit, and we fall back
	 * to regular pages anyway in case of failure.
	 */
	/* 普通小页回收后再试一次；大页临界成功率低且可回退，不继续昂贵重试。 */
	if (nr_reclaimed && nr_pages <= (1 << PAGE_ALLOC_COSTLY_ORDER))
		goto retry;

	if (nr_retries--)
		goto retry;

	if (gfp_mask & __GFP_RETRY_MAYFAIL)
		goto nomem;

	/* Avoid endless loop for tasks bypassed by the oom killer */
	/* OOM 后任务已进入死亡状态时停止重试，避免绕过者形成死循环。 */
	if (passed_oom && task_is_dying())
		goto nomem;

	/*
	 * keep retrying as long as the memcg oom killer is able to make
	 * a forward progress or bypass the charge if the oom killer
	 * couldn't make any progress.
	 */
	/* OOM 能推进就重置重试预算；无法推进则离开 OOM 循环并走失败/特权路径。 */
	if (mem_cgroup_oom(mem_over_limit, gfp_mask,
			   get_order(nr_pages * PAGE_SIZE))) {
		passed_oom = true;
		nr_retries = MAX_RECLAIM_RETRIES;
		goto retry;
	}
nomem:
	/*
	 * 失败出口：普通请求返回 -ENOMEM；NOFAIL/HIGH 类特权请求没有独立
	 * memcg 原子保留，只能 force charge，把回收责任留给后续普通分配。
	 */
	/*
	 * Memcg doesn't have a dedicated reserve for atomic
	 * allocations. But like the global atomic pool, we need to
	 * put the burden of reclaim on regular allocation requests
	 * and let these go through as privileged allocations.
	 */
	/* memcg 无独立原子保留池；普通分配承担回收，NOFAIL/HIGH 特权请求允许通过。 */
	if (!(gfp_mask & (__GFP_NOFAIL | __GFP_HIGH)))
		return -ENOMEM;
force:
	/*
	 * If the allocation has to be enforced, don't forget to raise
	 * a MEMCG_MAX event.
	 */
	/* 强制计费仍必须留下触碰 memory.max 的可观察事件。 */
	if (!raised_max_event)
		__memcg_memory_event(mem_over_limit, MEMCG_MAX, allow_spinning);

	/*
	 * The allocation either can't fail or will lead to more memory
	 * being freed very soon.  Allow memory usage go over the limit
	 * temporarily by force charging it.
	 */
	/* 不可失败或即将释放内存的请求可临时越限，随后由普通路径把用量收敛回来。 */
	page_counter_charge(&memcg->memory, nr_pages);
	if (do_memsw_account())
		page_counter_charge(&memcg->memsw, nr_pages);

	return 0;

done_restock:
	/*
	 * 成功出口：本次页数留作调用者 commit，多预留额度放入当前 CPU stock。
	 * 然后只登记 high 欠账；high 不回滚成功 charge。
	 */
	if (batch > nr_pages)
		refill_stock(memcg, batch - nr_pages);

	/*
	 * If the hierarchy is above the normal consumption range, schedule
	 * reclaim on returning to userland.  We can perform reclaim here
	 * if __GFP_RECLAIM but let's always punt for simplicity and so that
	 * GFP_KERNEL can consistently be used during reclaim.  @memcg is
	 * not recorded as it most likely matches current's and won't
	 * change in the meantime.  As high limit is checked again before
	 * reclaim, the cost of mismatch is negligible.
	 */
	/* 超过任一祖先 high 时安排返回用户态回收；执行前会复核，故不保存 memcg。 */
	do {
		bool mem_high, swap_high;

		mem_high = page_counter_read(&memcg->memory) >
			READ_ONCE(memcg->memory.high);
		swap_high = page_counter_read(&memcg->swap) >
			READ_ONCE(memcg->swap.high);

		/* Don't bother a random interrupted task */
		/* 中断中的 current 不是实际分配主体，改由目标 memcg 的 worker 回收。 */
		if (!in_task()) {
			if (mem_high) {
				schedule_work(&memcg->high_work);
				break;
			}
			continue;
		}

		if (mem_high || swap_high) {
			/*
			 * The allocating tasks in this cgroup will need to do
			 * reclaim or be throttled to prevent further growth
			 * of the memory or swap footprints.
			 *
			 * Target some best-effort fairness between the tasks,
			 * and distribute reclaim work and delay penalties
			 * based on how much each task is actually allocating.
			 */
			/* 按各任务真实分配量分摊回收和延迟，近似维持同组任务间公平。 */
			current->memcg_nr_pages_over_high += batch;
			set_notify_resume(current);
			break;
		}
	} while ((memcg = parent_mem_cgroup(memcg)));

	/*
	 * Reclaim is set up above to be called from the userland
	 * return path. But also attempt synchronous reclaim to avoid
	 * excessive overrun while the task is still inside the
	 * kernel. If this is successful, the return path will see it
	 * when it rechecks the overage and simply bail out.
	 */
	/* 返回用户态前虽会处理，但欠账过大时先同步回收，成功后返回路径复核即退出。 */
	if (current->memcg_nr_pages_over_high > MEMCG_CHARGE_BATCH &&
	    !(current->flags & PF_MEMALLOC) &&
	    gfpflags_allow_blocking(gfp_mask))
		__mem_cgroup_handle_over_high(gfp_mask);
	return 0;
}

/*
 * try_charge() - 根组免 page_counter 限额的薄包装。
 * @memcg/@gfp_mask/@nr_pages 同 try_charge_memcg()；非根组透传其全部错误，
 * 根组直接成功，因为全局页分配器而非 memcg 限制根组。
 */
static inline int try_charge(struct mem_cgroup *memcg, gfp_t gfp_mask,
			     unsigned int nr_pages)
{
	if (mem_cgroup_is_root(memcg))
		return 0;

	return try_charge_memcg(memcg, gfp_mask, nr_pages);
}

/*
 * commit_charge() - 将已预留额度的 objcg 归属发布到 folio。
 *
 * @folio 必须尚未计费且由页锁、LRU 隔离或独占引用稳定；@objcg 的持有引用
 * 随写入 memcg_data 转移给 folio。无失败返回；此后只能由 replace/uncharge
 * 路径清除并 put，不能再按“预留未提交”直接回滚 page_counter。
 */
static void commit_charge(struct folio *folio, struct obj_cgroup *objcg)
{
	VM_BUG_ON_FOLIO(folio_memcg_charged(folio), folio);
	/*
	 * Any of the following ensures folio's objcg stability:
	 *
	 * - the page lock
	 * - LRU isolation
	 * - exclusive reference
	 */
	folio->memcg_data = (unsigned long)objcg;
}

#ifdef CONFIG_MEMCG_NMI_SAFETY_REQUIRES_ATOMIC
/*
 * account_slab_nmi_safe() - 在普通或 NMI 上下文增加指定节点 slab 字节统计。
 * 普通路径走完整 lruvec/rstat；NMI 无法使用可能非原子的 per-CPU 更新，故直接
 * 登记 css dirty 并累加专用 atomic 旁路，稍后 flush_nmi_stats() 合并。
 */
static inline void account_slab_nmi_safe(struct mem_cgroup *memcg,
					 struct pglist_data *pgdat,
					 enum node_stat_item idx, int nr)
{
	struct lruvec *lruvec;

	if (likely(!in_nmi())) {
		lruvec = mem_cgroup_lruvec(memcg, pgdat);
		mod_memcg_lruvec_state(lruvec, idx, nr);
	} else {
		struct mem_cgroup_per_node *pn = memcg->nodeinfo[pgdat->node_id];

		/* preemption is disabled in_nmi(). */
		__css_rstat_updated(&memcg->css, smp_processor_id());
		if (idx == NR_SLAB_RECLAIMABLE_B)
			atomic_add(nr, &pn->slab_reclaimable);
		else
			atomic_add(nr, &pn->slab_unreclaimable);
	}
}
#else
/* 当前架构的统计更新本身可安全用于所需上下文，直接走标准 lruvec 路径。 */
static inline void account_slab_nmi_safe(struct mem_cgroup *memcg,
					 struct pglist_data *pgdat,
					 enum node_stat_item idx, int nr)
{
	struct lruvec *lruvec;

	lruvec = mem_cgroup_lruvec(memcg, pgdat);
	mod_memcg_lruvec_state(lruvec, idx, nr);
}
#endif

/*
 * mod_objcg_mlstate() - 通过 @objcg 当前归属更新 @pgdat 的 slab 状态 @idx。
 * RCU 覆盖 offline 重挂时的 objcg->memcg 读取，@nr 可正可负；底层根据架构
 * 选择普通或 NMI 旁路统计。只更新统计，不改变对象额度和 objcg 引用。
 */
static inline void mod_objcg_mlstate(struct obj_cgroup *objcg,
				       struct pglist_data *pgdat,
				       enum node_stat_item idx, int nr)
{
	struct mem_cgroup *memcg;

	rcu_read_lock();
	memcg = obj_cgroup_memcg(objcg);
	account_slab_nmi_safe(memcg, pgdat, idx, nr);
	rcu_read_unlock();
}

/*
 * mem_cgroup_from_obj_slab() - 从 slab 对象 @p 的扩展元数据查询当前 memcg。
 * 临时固定 slabobj_ext 数组，按对象索引读取 objcg；有归属时返回借用 memcg，
 * 无扩展或无 objcg 返回 NULL。函数不取得 css 引用，调用者必须另行保证 RCU/
 * memcg 生命周期；释放扩展区固定后不得再使用 obj_ext 指针。
 */
static __always_inline
struct mem_cgroup *mem_cgroup_from_obj_slab(struct slab *slab, void *p)
{
	/*
	 * Slab objects are accounted individually, not per-page.
	 * Memcg membership data for each individual object is saved in
	 * slab->obj_exts.
	 */
	unsigned long obj_exts;
	struct slabobj_ext *obj_ext;
	unsigned int off;

	obj_exts = slab_obj_exts(slab);
	if (!obj_exts)
		return NULL;

	get_slab_obj_exts(obj_exts);
	off = obj_to_index(slab->slab_cache, slab, p);
	obj_ext = slab_obj_ext(slab, obj_exts, off);
	if (obj_ext->objcg) {
		struct obj_cgroup *objcg = obj_ext->objcg;

		put_slab_obj_exts(obj_exts);
		return obj_cgroup_memcg(objcg);
	}
	put_slab_obj_exts(obj_exts);

	return NULL;
}

/*
 * Returns a pointer to the memory cgroup to which the kernel object is charged.
 * It is not suitable for objects allocated using vmalloc().
 *
 * A passed kernel object must be a slab object or a generic kernel page.
 *
 * The caller must ensure the memcg lifetime, e.g. by taking rcu_read_lock(),
 * cgroup_mutex, etc.
 */
/*
 * mem_cgroup_from_virt() - 查询直接映射内核地址 @p 的计费 memcg。
 * slab 地址按对象扩展查询，普通页地址按 folio 查询；vmalloc 地址不适用。
 * memcg 关闭返回 NULL。返回值不持有引用，调用者须用 RCU、cgroup_mutex 或
 * 其他所有权保证其存活。
 */
struct mem_cgroup *mem_cgroup_from_virt(void *p)
{
	struct slab *slab;

	if (mem_cgroup_disabled())
		return NULL;

	slab = virt_to_slab(p);
	if (slab)
		return mem_cgroup_from_obj_slab(slab, p);
	return folio_memcg_check(virt_to_folio(p));
}

/*
 * __get_obj_cgroup_from_memcg() - 从 @memcg 向祖先查找本 NUMA 节点的 live objcg。
 * 调用者持有 RCU；offline 组的 node objcg 可为 NULL 或已 kill，故逐层尝试
 * percpu_ref live 引用。成功返回由调用者持有的 objcg，全部失败返回 NULL。
 */
static struct obj_cgroup *__get_obj_cgroup_from_memcg(struct mem_cgroup *memcg)
{
	int nid = numa_node_id();

	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		struct obj_cgroup *objcg = rcu_dereference(memcg->nodeinfo[nid]->objcg);

		if (likely(objcg && obj_cgroup_tryget(objcg)))
			return objcg;
	}

	return NULL;
}

/*
 * get_obj_cgroup_from_memcg() - 为上述层级查找封装 RCU 读侧临界区。
 * 成功返回已取引用的 objcg，调用者最终 obj_cgroup_put()；NULL 表示本组及祖先
 * 均无可接收新对象的在线 objcg。
 */
static inline struct obj_cgroup *get_obj_cgroup_from_memcg(struct mem_cgroup *memcg)
{
	struct obj_cgroup *objcg;

	rcu_read_lock();
	objcg = __get_obj_cgroup_from_memcg(memcg);
	rcu_read_unlock();

	return objcg;
}

/*
 * 【objcg 获取、计费与 slab 发布】
 *
 * current_obj_cgroup() 从 current 的 memcg 取得 active objcg。offline 可把
 * pn->objcg 置 NULL 并重挂旧对象，因此获取路径用 CURRENT_OBJCG_UPDATE_FLAG
 * 协调同一 task 的递归更新，并以 percpu_ref_tryget_live() 保证返回对象
 * 仍可接收引用。对象 charge 先消费 obj_stock，不足时按页向 memcg 预留；
 * slab post-alloc 成功后才把 objcg 写入对象元数据，这是外界可观察的归属
 * 提交点。free hook 反向清统计、返还字节并 put 引用。
 */
static struct obj_cgroup *current_objcg_update(void)
{
	struct mem_cgroup *memcg;
	struct obj_cgroup *old, *objcg = NULL;

	do {
		/* Atomically drop the update bit. */
		/* 原子清除更新位，并取得清除前的缓存值用于竞争判断。 */
		old = xchg(&current->objcg, NULL);
		if (old) {
			old = (struct obj_cgroup *)
				((unsigned long)old & ~CURRENT_OBJCG_UPDATE_FLAG);
			obj_cgroup_put(old);

			old = NULL;
		}

		/* If new objcg is NULL, no reason for the second atomic update. */
		/* 新归属为空时无需第二次原子发布，当前 NULL 已是正确结果。 */
		if (!current->mm || (current->flags & PF_KTHREAD))
			return NULL;

		/*
		 * Release the objcg pointer from the previous iteration,
		 * if try_cmpxcg() below fails.
		 */
		/* 若上一轮 cmpxchg 竞争失败，先释放当轮临时取得的 objcg 引用再重试。 */
		if (unlikely(objcg)) {
			obj_cgroup_put(objcg);
			objcg = NULL;
		}

		/*
		 * Obtain the new objcg pointer. The current task can be
		 * asynchronously moved to another memcg and the previous
		 * memcg can be offlined. So let's get the memcg pointer
		 * and try get a reference to objcg under a rcu read lock.
		 */
		/* 任务可能异步迁组且旧组可 offline，故在 RCU 下重读 memcg 并取得 live 引用。 */

		rcu_read_lock();
		memcg = mem_cgroup_from_task(current);
		objcg = __get_obj_cgroup_from_memcg(memcg);
		rcu_read_unlock();

		/*
		 * Try set up a new objcg pointer atomically. If it
		 * fails, it means the update flag was set concurrently, so
		 * the whole procedure should be repeated.
		 */
		/* 原子发布新缓存；失败说明更新位又被并发设置，必须从身份读取开始重做。 */
	} while (!try_cmpxchg(&current->objcg, &old, objcg));

	return objcg;
}

/*
 * current_obj_cgroup() - 返回当前分配作用域可使用的临时 objcg 指针。
 * 入参：无。先读取 current 缓存；发现 UPDATE_FLAG 或缓存缺失时进入更新慢
 * 路径。返回值不自带永久引用，只在 current/active_memcg 作用域内有效；
 * 对象发布前必须 obj_cgroup_get()。控制器关闭或根策略可返回 NULL/根 objcg。
 */
__always_inline struct obj_cgroup *current_obj_cgroup(void)
{
	struct mem_cgroup *memcg;
	struct obj_cgroup *objcg;
	int nid = numa_node_id();

	if (IS_ENABLED(CONFIG_MEMCG_NMI_UNSAFE) && in_nmi())
		return NULL;

	if (in_task()) {
		memcg = current->active_memcg;
		if (unlikely(memcg))
			goto from_memcg;

		objcg = READ_ONCE(current->objcg);
		if (unlikely((unsigned long)objcg & CURRENT_OBJCG_UPDATE_FLAG))
			objcg = current_objcg_update();
		/*
		 * Objcg reference is kept by the task, so it's safe
		 * to use the objcg by the current task.
		 */
		return objcg ? : rcu_dereference_check(root_mem_cgroup->nodeinfo[nid]->objcg, 1);
	}

	memcg = this_cpu_read(int_active_memcg);
	if (unlikely(memcg))
		goto from_memcg;

	return rcu_dereference_check(root_mem_cgroup->nodeinfo[nid]->objcg, 1);

from_memcg:
	for (; memcg; memcg = parent_mem_cgroup(memcg)) {
		/*
		 * Memcg pointer is protected by scope (see set_active_memcg())
		 * and is pinning the corresponding objcg, so objcg can't go
		 * away and can be used within the scope without any additional
		 * protection.
		 */
		objcg = rcu_dereference_check(memcg->nodeinfo[nid]->objcg, 1);
		if (likely(objcg))
			return objcg;
	}

	return rcu_dereference_check(root_mem_cgroup->nodeinfo[nid]->objcg, 1);
}

/*
 * get_obj_cgroup_from_folio() - 从已稳定 folio 取得带 percpu_ref 的 objcg。
 * @folio 为借用；未计费返回 NULL。RCU 保护读取与 offline 重挂，成功返回值
 * 必须 obj_cgroup_put()。引用保证 objcg 存储期，但其 memcg 指向仍可重挂。
 */
struct obj_cgroup *get_obj_cgroup_from_folio(struct folio *folio)
{
	struct obj_cgroup *objcg;

	objcg = folio_objcg(folio);
	if (objcg)
		obj_cgroup_get(objcg);

	return objcg;
}

#ifdef CONFIG_MEMCG_NMI_SAFETY_REQUIRES_ATOMIC
/*
 * account_kmem_nmi_safe() - 更新总 kmem 页统计，并为 NMI 提供原子旁路。
 * 普通上下文直接走 memcg rstat；NMI 中先登记 css dirty，再累加 kmem_stat，
 * 由 flush_nmi_stats() 合并。@val 可正可负，不负责 page_counter 计费。
 */
static inline void account_kmem_nmi_safe(struct mem_cgroup *memcg, int val)
{
	if (likely(!in_nmi())) {
		mod_memcg_state(memcg, MEMCG_KMEM, val);
	} else {
		/* preemption is disabled in_nmi(). */
		__css_rstat_updated(&memcg->css, smp_processor_id());
		atomic_add(val, &memcg->kmem_stat);
	}
}
#else
/* 当前配置无需 NMI 原子旁路，直接更新标准 MEMCG_KMEM state。 */
static inline void account_kmem_nmi_safe(struct mem_cgroup *memcg, int val)
{
	mod_memcg_state(memcg, MEMCG_KMEM, val);
}
#endif

/*
 * obj_cgroup_uncharge_pages: uncharge a number of kernel pages from a objcg
 * @objcg: object cgroup to uncharge
 * @nr_pages: number of pages to uncharge
 */
/*
 * obj_cgroup_uncharge_pages() - 向 objcg 当前所属 memcg 归还整页 kmem 额度。
 * @objcg 由调用者持有，@nr_pages 是基页数。函数稳定 memcg 引用，同步扣减
 * MEMCG_KMEM/v1 统计和非根 page_counter，最后 put；返回无直接值。
 */
static void obj_cgroup_uncharge_pages(struct obj_cgroup *objcg,
				      unsigned int nr_pages)
{
	struct mem_cgroup *memcg;

	memcg = get_mem_cgroup_from_objcg(objcg);

	account_kmem_nmi_safe(memcg, -nr_pages);
	memcg1_account_kmem(memcg, -nr_pages);
	if (!mem_cgroup_is_root(memcg))
		refill_stock(memcg, nr_pages);

	css_put(&memcg->css);
}

/*
 * obj_cgroup_charge_pages: charge a number of kernel pages to a objcg
 * @objcg: object cgroup to charge
 * @gfp: reclaim mode
 * @nr_pages: number of pages to charge
 *
 * Returns 0 on success, an error code on failure.
 */
/*
 * obj_cgroup_charge_pages() - 为 @objcg 预留 @nr_pages 整页 kmem 额度。
 * @gfp 决定 reclaim/OOM 能力。根 objcg 免限额；非根通过当前重挂后的 memcg
 * try_charge，成功再增加 kmem 统计。失败返回 errno，未留下统计或额度。
 */
static int obj_cgroup_charge_pages(struct obj_cgroup *objcg, gfp_t gfp,
				   unsigned int nr_pages)
{
	struct mem_cgroup *memcg;
	int ret;

	memcg = get_mem_cgroup_from_objcg(objcg);

	ret = try_charge_memcg(memcg, gfp, nr_pages);
	if (ret)
		goto out;

	account_kmem_nmi_safe(memcg, nr_pages);
	memcg1_account_kmem(memcg, nr_pages);
out:
	css_put(&memcg->css);

	return ret;
}

/* page_objcg() 解码 page->memcg_data 中的 objcg 裸指针；调用者负责生命周期。 */
static struct obj_cgroup *page_objcg(const struct page *page)
{
	unsigned long memcg_data = page->memcg_data;

	if (mem_cgroup_disabled() || !memcg_data)
		return NULL;

	VM_BUG_ON_PAGE((memcg_data & OBJEXTS_FLAGS_MASK) != MEMCG_DATA_KMEM,
			page);
	return (struct obj_cgroup *)(memcg_data - MEMCG_DATA_KMEM);
}

/*
 * page_set_objcg() - 把已持有引用的 @objcg 发布到非 slab 内核页元数据。
 * @page 必须由调用者独占且尚未计费；函数只写归属，不增加引用、不做 charge。
 */
static void page_set_objcg(struct page *page, const struct obj_cgroup *objcg)
{
	page->memcg_data = (unsigned long)objcg | MEMCG_DATA_KMEM;
}

/**
 * __memcg_kmem_charge_page: charge a kmem page to the current memory cgroup
 * @page: page to charge
 * @gfp: reclaim mode
 * @order: allocation order
 *
 * Returns 0 on success, an error code on failure.
 */
/*
 * __memcg_kmem_charge_page() - 给非 slab 的内核页建立 current objcg 归属。
 *
 * @page 是尚未发布归属的首页，@order 表示 2^order 个基页，@gfp 决定回收
 * 能力。成功返回 0，并在 page memcg_data 保存持有引用的 objcg；失败返回
 * -ENOMEM 等且 page 仍未计费。调用者释放时必须以相同 order 调用 uncharge。
 */
int __memcg_kmem_charge_page(struct page *page, gfp_t gfp, int order)
{
	struct obj_cgroup *objcg;
	int ret = 0;

	objcg = current_obj_cgroup();
	if (objcg && !obj_cgroup_is_root(objcg)) {
		ret = obj_cgroup_charge_pages(objcg, gfp, 1 << order);
		if (!ret) {
			obj_cgroup_get(objcg);
			page_set_objcg(page, objcg);
			return 0;
		}
	}
	return ret;
}

/**
 * __memcg_kmem_uncharge_page: uncharge a kmem page
 * @page: page to uncharge
 * @order: allocation order
 */
/*
 * __memcg_kmem_uncharge_page() - 对称撤销内核页 charge 并清除归属。
 * @page/@order 必须与成功 charge 时一致。先取出 objcg、清 page 元数据阻止
 * 重复销账，再归还 2^order 页额度并 put 对象引用。无直接返回值。
 */
void __memcg_kmem_uncharge_page(struct page *page, int order)
{
	struct obj_cgroup *objcg = page_objcg(page);
	unsigned int nr_pages = 1 << order;

	if (!objcg)
		return;

	obj_cgroup_uncharge_pages(objcg, nr_pages);
	page->memcg_data = 0;
	obj_cgroup_put(objcg);
}

/*
 * trylock_stock() - 尝试取得当前 CPU obj_stock 的 local_lock。
 * 成功返回固定在本 CPU 的借用 stock，失败返回 NULL；调用者必须无条件把
 * 结果交给 unlock_stock()，NULL 表示 NMI/递归竞争时走无缓存慢路径。
 */
static struct obj_stock_pcp *trylock_stock(void)
{
	if (local_trylock(&obj_stock.lock))
		return this_cpu_ptr(&obj_stock);

	return NULL;
}

/* 释放 trylock_stock() 成功取得的本地锁；@stock 为 NULL 时安全空操作。 */
static void unlock_stock(struct obj_stock_pcp *stock)
{
	if (stock)
		local_unlock(&obj_stock.lock);
}

/* Call after __refill_obj_stock() so a slot for objcg exists in the stock */
/*
 * __account_obj_stock() - 在已锁定的本 CPU 对象 stock 中消费或返还字节。
 *
 * 正 @nr_bytes 表示释放对象后返还，负向效果由调用者的 charge 路径表达；
 * @allow_uncharge 决定余额过大时能否立即把整页归还 page_counter。函数还
 * 批量累计 slab reclaimable/unreclaimable 统计。调用者必须持 stock
 * local_lock，不能迁移；槽位替换前会完整 drain 旧 objcg 余额和引用。
 */
static void __account_obj_stock(struct obj_cgroup *objcg,
				struct obj_stock_pcp *stock, int nr,
				struct pglist_data *pgdat, enum node_stat_item idx)
{
	int16_t *bytes;
	int i;

	/*
	 * Though at the moment MAX_NUMNODES <= 1024 in all archs but let's make
	 * sure it does not exceed S16_MAX otherwise we need to fix node_id type
	 * in struct obj_stock_pcp.
	 */
	BUILD_BUG_ON(MAX_NUMNODES >= S16_MAX);

	if (!stock)
		goto direct;

	for (i = 0; i < NR_OBJ_STOCK; ++i) {
		if (READ_ONCE(stock->cached[i]) == objcg)
			break;
	}
	if (i == NR_OBJ_STOCK)
		goto direct;

	/*
	 * Save vmstat data in stock and skip vmstat array update unless
	 * accumulating over a page of vmstat data or when the objcg slot or
	 * pgdat the stats belong to changes.
	 */
	if (stock->index < 0) {
		stock->index = i;
		stock->node_id = pgdat->node_id;
	} else if (stock->index != i || stock->node_id != pgdat->node_id) {
		struct obj_cgroup *old = READ_ONCE(stock->cached[stock->index]);
		struct pglist_data *oldpg = NODE_DATA(stock->node_id);

		if (stock->nr_slab_reclaimable_b) {
			mod_objcg_mlstate(old, oldpg, NR_SLAB_RECLAIMABLE_B,
					  stock->nr_slab_reclaimable_b);
			stock->nr_slab_reclaimable_b = 0;
		}
		if (stock->nr_slab_unreclaimable_b) {
			mod_objcg_mlstate(old, oldpg, NR_SLAB_UNRECLAIMABLE_B,
					  stock->nr_slab_unreclaimable_b);
			stock->nr_slab_unreclaimable_b = 0;
		}
		stock->index = i;
		stock->node_id = pgdat->node_id;
	}

	bytes = (idx == NR_SLAB_RECLAIMABLE_B) ? &stock->nr_slab_reclaimable_b
					       : &stock->nr_slab_unreclaimable_b;

	/*
	 * Fold @nr into the cached value and decide whether to keep it cached
	 * or flush it directly. Cache the combined value when it fits in the
	 * int16_t storage and either the cache was empty (so even a value
	 * above PAGE_SIZE gets a chance to be canceled by a paired delta) or
	 * the combined value is within the PAGE_SIZE flush threshold.
	 */
	nr += *bytes;
	if (abs(nr) <= S16_MAX && (!*bytes || abs(nr) <= PAGE_SIZE)) {
		*bytes = nr;
		nr = 0;
	} else {
		*bytes = 0;
	}
direct:
	if (nr)
		mod_objcg_mlstate(objcg, pgdat, idx, nr);
}

/*
 * __consume_obj_stock() - 在已锁 stock 中消费 @nr_bytes 的 objcg 预充余额。
 * 仅身份匹配且字节足够时扣减并返回 true；否则保持账目不变。调用者必须持
 * local_lock，@objcg 是借用但槽位自身持有 percpu_ref。
 */
static bool __consume_obj_stock(struct obj_cgroup *objcg,
				struct obj_stock_pcp *stock,
				unsigned int nr_bytes)
{
	int i;

	for (i = 0; i < NR_OBJ_STOCK; ++i) {
		if (READ_ONCE(stock->cached[i]) != objcg)
			continue;
		if (stock->nr_bytes[i] >= nr_bytes) {
			stock->nr_bytes[i] -= nr_bytes;
			return true;
		}
		return false;
	}

	return false;
}

/* 封装 trylock/consume/unlock；锁竞争或余额不足均返回 false 进入 charge 慢路。 */
static bool consume_obj_stock(struct obj_cgroup *objcg, unsigned int nr_bytes)
{
	struct obj_stock_pcp *stock;
	bool ret = false;

	stock = trylock_stock();
	if (!stock)
		return ret;

	ret = __consume_obj_stock(objcg, stock, nr_bytes);
	unlock_stock(stock);

	return ret;
}

/* Flush the cached slab stats (if any) back to their owning objcg/pgdat. */
/*
 * drain_obj_stock_stats() - 把 stock 延迟的 slab 字节统计提交到 objcg/pgdat。
 * 调用者持 local_lock；函数按 reclaimable/unreclaimable 分别更新后清零缓存，
 * 不归还 page_counter 字节余额。
 */
static void drain_obj_stock_stats(struct obj_stock_pcp *stock)
{
	struct obj_cgroup *old;
	struct pglist_data *oldpg;

	if (stock->index < 0)
		return;

	old = READ_ONCE(stock->cached[stock->index]);
	oldpg = NODE_DATA(stock->node_id);

	if (stock->nr_slab_reclaimable_b) {
		mod_objcg_mlstate(old, oldpg, NR_SLAB_RECLAIMABLE_B,
				  stock->nr_slab_reclaimable_b);
		stock->nr_slab_reclaimable_b = 0;
	}
	if (stock->nr_slab_unreclaimable_b) {
		mod_objcg_mlstate(old, oldpg, NR_SLAB_UNRECLAIMABLE_B,
				  stock->nr_slab_unreclaimable_b);
		stock->nr_slab_unreclaimable_b = 0;
	}
	stock->index = -1;
	stock->node_id = NUMA_NO_NODE;
}

/*
 * drain_obj_stock_slot() - 完整清空第 @i 个 objcg 槽位。
 * 先把可组成整页的字节归还所属 memcg，再提交残余/统计并释放 cached 引用；
 * 调用者持 local_lock，返回后该槽位可安全接纳另一 objcg。
 */
static void drain_obj_stock_slot(struct obj_stock_pcp *stock, int i)
{
	struct obj_cgroup *old = READ_ONCE(stock->cached[i]);

	if (!old)
		return;

	if (stock->nr_bytes[i]) {
		unsigned int nr_pages = stock->nr_bytes[i] >> PAGE_SHIFT;
		unsigned int nr_bytes = stock->nr_bytes[i] & (PAGE_SIZE - 1);

		if (nr_pages) {
			struct mem_cgroup *memcg;

			memcg = get_mem_cgroup_from_objcg(old);

			mod_memcg_state(memcg, MEMCG_KMEM, -nr_pages);
			memcg1_account_kmem(memcg, -nr_pages);
			if (!mem_cgroup_is_root(memcg))
				memcg_uncharge(memcg, nr_pages);

			css_put(&memcg->css);
		}

		/*
		 * The leftover is flushed to the centralized per-memcg value.
		 * On the next attempt to refill obj stock it will be moved
		 * to a per-cpu stock (probably, on an other CPU), see
		 * refill_obj_stock().
		 *
		 * How often it's flushed is a trade-off between the memory
		 * limit enforcement accuracy and potential CPU contention,
		 * so it might be changed in the future.
		 */
		atomic_add(nr_bytes, &old->nr_charged_bytes);
		stock->nr_bytes[i] = 0;
	}

	/* Flush vmstat data when its owning slot is being drained. */
	if (stock->index == i)
		drain_obj_stock_stats(stock);

	WRITE_ONCE(stock->cached[i], NULL);
	obj_cgroup_put(old);
}

/* 依次冲刷 obj_stock 的全部槽位；用于 CPU hotplug、offline 与同步 drain。 */
static void drain_obj_stock(struct obj_stock_pcp *stock)
{
	int i;

	for (i = 0; i < NR_OBJ_STOCK; ++i)
		drain_obj_stock_slot(stock, i);
}

/* 判断 stock 是否缓存指定 objcg/memcg 子树或已超过应主动冲刷的余额阈值。 */
static bool obj_stock_flush_required(struct obj_stock_pcp *stock,
				     struct mem_cgroup *root_memcg)
{
	struct obj_cgroup *objcg;
	struct mem_cgroup *memcg;
	bool flush = false;
	int i;

	rcu_read_lock();
	for (i = 0; i < NR_OBJ_STOCK; ++i) {
		objcg = READ_ONCE(stock->cached[i]);
		if (!objcg)
			continue;
		memcg = obj_cgroup_memcg(objcg);
		if (memcg && mem_cgroup_is_descendant(memcg, root_memcg)) {
			flush = true;
			break;
		}
	}
	rcu_read_unlock();

	return flush;
}

/*
 * __refill_obj_stock() - 把释放对象产生的 @nr_bytes 缓存或归还给 @objcg。
 * @stock 非空表示调用者持有本 CPU stock 锁；函数优先复用/分配槽位，必要时
 * 轮转冲刷旧槽。整页部分在允许时归还 page_counter，尾数字节保留本地或
 * objcg 原子余量。@allow_uncharge=false 可避免刚 charge 后立即反向销账。
 */
static void __refill_obj_stock(struct obj_cgroup *objcg,
			       struct obj_stock_pcp *stock,
			       unsigned int nr_bytes,
			       bool allow_uncharge)
{
	unsigned int nr_pages = 0;
	unsigned int stock_nr_bytes;
	int i, slot = -1, empty_slot = -1;

	if (!stock) {
		nr_pages = nr_bytes >> PAGE_SHIFT;
		nr_bytes = nr_bytes & (PAGE_SIZE - 1);
		atomic_add(nr_bytes, &objcg->nr_charged_bytes);
		goto out;
	}

	for (i = 0; i < NR_OBJ_STOCK; ++i) {
		struct obj_cgroup *cached = READ_ONCE(stock->cached[i]);

		if (!cached) {
			if (empty_slot == -1)
				empty_slot = i;
			continue;
		}
		if (cached == objcg) {
			slot = i;
			break;
		}
	}

	if (slot == -1) {
		slot = empty_slot;
		if (slot == -1) {
			slot = stock->drain_idx++;
			if (stock->drain_idx == NR_OBJ_STOCK)
				stock->drain_idx = 0;
			drain_obj_stock_slot(stock, slot);
		}
		obj_cgroup_get(objcg);
		/*
		 * Keep the xchg result in the unsigned int local; storing
		 * it directly into stock->nr_bytes[slot] (uint16_t) would
		 * silently truncate values >= U16_MAX and bypass the flush
		 * guard below, leaking page-counter charges.
		 */
		stock_nr_bytes = atomic_read(&objcg->nr_charged_bytes)
				? atomic_xchg(&objcg->nr_charged_bytes, 0) : 0;
		WRITE_ONCE(stock->cached[slot], objcg);

		allow_uncharge = true;	/* Allow uncharge when objcg changes */
	} else {
		stock_nr_bytes = stock->nr_bytes[slot];
	}

	stock_nr_bytes += nr_bytes;

	if ((allow_uncharge && (stock_nr_bytes > PAGE_SIZE)) ||
	    stock_nr_bytes > U16_MAX) {
		nr_pages = stock_nr_bytes >> PAGE_SHIFT;
		stock_nr_bytes &= (PAGE_SIZE - 1);
	}
	stock->nr_bytes[slot] = stock_nr_bytes;

out:
	if (nr_pages)
		obj_cgroup_uncharge_pages(objcg, nr_pages);
}

/* refill_obj_stock() - 加锁封装对象字节返还；锁不可得时自动走集中余量路径。 */
static void refill_obj_stock(struct obj_cgroup *objcg,
			     unsigned int nr_bytes,
			     bool allow_uncharge)
{
	struct obj_stock_pcp *stock = trylock_stock();
	__refill_obj_stock(objcg, stock, nr_bytes, allow_uncharge);
	unlock_stock(stock);
}

/*
 * __obj_cgroup_charge() - 按整页向 @objcg 预留容纳 @size 字节的额度。
 * 成功返回 0，并把页尾多收字节写入 *@remainder 供 stock 复用；失败返回底层
 * charge errno，remainder 不可用，且没有已提交额度。
 */
static int __obj_cgroup_charge(struct obj_cgroup *objcg, gfp_t gfp,
			       size_t size, size_t *remainder)
{
	size_t charge_size;
	int ret;

	charge_size = PAGE_ALIGN(size);
	ret = obj_cgroup_charge_pages(objcg, gfp, charge_size >> PAGE_SHIFT);
	if (!ret)
		*remainder = charge_size - size;

	return ret;
}

/*
 * obj_cgroup_charge() - 为非 slab 后端对象计收 @size 字节。
 * 先整页预留，再把对齐余量放回 obj stock，使多对象共享页额度。成功返回 0；
 * 失败不留账。调用者须在对象释放时以同一大小调用 obj_cgroup_uncharge()。
 */
int obj_cgroup_charge(struct obj_cgroup *objcg, gfp_t gfp, size_t size)
{
	size_t remainder;
	int ret;

	if (likely(consume_obj_stock(objcg, size)))
		return 0;

	/*
	 * In theory, objcg->nr_charged_bytes can have enough
	 * pre-charged bytes to satisfy the allocation. However,
	 * flushing objcg->nr_charged_bytes requires two atomic
	 * operations, and objcg->nr_charged_bytes can't be big.
	 * The shared objcg->nr_charged_bytes can also become a
	 * performance bottleneck if all tasks of the same memcg are
	 * trying to update it. So it's better to ignore it and try
	 * grab some new pages. The stock's nr_bytes will be flushed to
	 * objcg->nr_charged_bytes later on when objcg changes.
	 *
	 * The stock's nr_bytes may contain enough pre-charged bytes
	 * to allow one less page from being charged, but we can't rely
	 * on the pre-charged bytes not being changed outside of
	 * consume_obj_stock() or refill_obj_stock(). So ignore those
	 * pre-charged bytes as well when charging pages. To avoid a
	 * page uncharge right after a page charge, we set the
	 * allow_uncharge flag to false when calling refill_obj_stock()
	 * to temporarily allow the pre-charged bytes to exceed the page
	 * size limit. The maximum reachable value of the pre-charged
	 * bytes is (sizeof(object) + PAGE_SIZE - 2) if there is no data
	 * race.
	 */
	ret = __obj_cgroup_charge(objcg, gfp, size, &remainder);
	if (!ret && remainder)
		refill_obj_stock(objcg, remainder, false);

	return ret;
}

/*
 * obj_cgroup_uncharge() - 归还此前由 obj_cgroup_charge() 收取的 @size 字节。
 * 字节先进入本 CPU stock，达到整页后再批量退还 counter；无直接返回值，故
 * 调用者必须保证 objcg、大小和 charge 成功路径严格配对。
 */
void obj_cgroup_uncharge(struct obj_cgroup *objcg, size_t size)
{
	refill_obj_stock(objcg, size, true);
}

/* obj_full_size() - 返回 slab 对象连同 allocator 元数据应归入 kmem 的真实字节。 */
static inline size_t obj_full_size(struct kmem_cache *s)
{
	/*
	 * For each accounted object there is an extra space which is used
	 * to store obj_cgroup membership. Charge it too.
	 */
	return s->size + sizeof(struct obj_cgroup *);
}

/*
 * __memcg_slab_post_alloc_hook() - slab 对象成功分配后的 memcg 提交阶段。
 *
 * @s 给出对象实际计费大小，@lru 可选地决定 list_lru 归属，@gfp 控制补充
 * charge 的回收能力，@size 是对象数组长度，@p 是输出对象数组。函数为每个
 * 非 NULL 对象写入持有引用的 objcg 并更新 slab/lru 统计；若批量预 charge
 * 不足，会对未能计费的对象执行相应失败处理并返回 false。归属写入之前对象
 * 不得向其他 CPU 发布，否则 free 路径无法找到对称账目。
 */
bool __memcg_slab_post_alloc_hook(struct kmem_cache *s, struct list_lru *lru,
				  gfp_t flags, unsigned int slab_alloc_flags,
				  size_t size, void **p)
{
	size_t obj_size = obj_full_size(s);
	struct obj_cgroup *objcg;
	struct slab *slab;
	unsigned long off;
	size_t i;

	/*
	 * The obtained objcg pointer is safe to use within the current scope,
	 * defined by current task or set_active_memcg() pair.
	 * obj_cgroup_get() is used to get a permanent reference.
	 */
	/*
	 * 阶段 1：current_obj_cgroup() 只返回受 current/active_memcg 作用域保护的
	 * 临时指针；真正写入对象元数据前，每个成功对象还要 obj_cgroup_get()。
	 * 根 objcg 不做对象级账目，直接保持 allocator 的成功结果。
	 */
	objcg = current_obj_cgroup();
	if (!objcg || obj_cgroup_is_root(objcg))
		return true;

	/*
	 * slab_alloc_node() avoids the NULL check, so we might be called with a
	 * single NULL object. kmem_cache_alloc_bulk() aborts if it can't fill
	 * the whole requested size.
	 * return success as there's nothing to free back
	 */
	/*
	 * 单对象接口允许把分配失败的 NULL 传到 post hook；批量接口则保证全有
	 * 或全无。NULL 尚未拥有 objcg，也没有任何额度需要回滚，因此直接成功。
	 */
	if (unlikely(*p == NULL))
		return true;

	flags &= gfp_allowed_mask;

	if (lru) {
		int ret;
		struct mem_cgroup *memcg;

		/*
		 * 阶段 2：list_lru 必须先为目标 memcg 分配节点状态。helper 返回带引用
		 * memcg，失败时对象尚未发布 objcg，调用者仍可整体释放 allocator 结果。
		 */
		memcg = get_mem_cgroup_from_objcg(objcg);
		ret = memcg_list_lru_alloc(memcg, lru, flags);
		css_put(&memcg->css);

		if (ret)
			return false;
	}

	/*
	 * 阶段 3：逐对象确保 slabobj_ext 存在、取得字节额度，然后发布 objcg。
	 * 扩展区和 pgdat 可能随对象变化，当前实现选择逐个提交以保持简单回滚。
	 */
	for (i = 0; i < size; i++) {
		unsigned long obj_exts;
		struct slabobj_ext *obj_ext;
		struct obj_stock_pcp *stock;

		slab = virt_to_slab(p[i]);

		if (!slab_obj_exts(slab) &&
		    alloc_slab_obj_exts(slab, s, flags, slab_alloc_flags)) {
			continue;
		}

		/*
		 * if we fail and size is 1, memcg_alloc_abort_single() will
		 * just free the object, which is ok as we have not assigned
		 * objcg to its obj_ext yet
		 *
		 * for larger sizes, kmem_cache_free_bulk() will uncharge
		 * any objects that were already charged and obj_ext assigned
		 *
		 * TODO: we could batch this until slab_pgdat(slab) changes
		 * between iterations, with a more complicated undo
		 */
		/*
		 * 单对象失败由 memcg_alloc_abort_single() 释放尚未发布的对象；批量
		 * 失败由 kmem_cache_free_bulk() 扫描已写 obj_ext 的前缀并逐个销账。
		 * 因此 charge 必须严格早于 obj_ext->objcg 发布，不能交换两步顺序。
		 */
		stock = trylock_stock();
		if (!stock || !__consume_obj_stock(objcg, stock, obj_size)) {
			size_t remainder;

			unlock_stock(stock);
			if (__obj_cgroup_charge(objcg, flags, obj_size, &remainder))
				return false;
			stock = trylock_stock();
			if (remainder)
				__refill_obj_stock(objcg, stock, remainder, false);
		}
		__account_obj_stock(objcg, stock, obj_size,
				    slab_pgdat(slab), cache_vmstat_idx(s));
		unlock_stock(stock);

		/*
		 * 阶段 4：额度和统计均已成功后才取得扩展区并写 objcg。get/put 保护
		 * 扩展区存储期，obj_cgroup_get() 的永久引用转移给新对象的 free hook。
		 */
		obj_exts = slab_obj_exts(slab);
		get_slab_obj_exts(obj_exts);
		off = obj_to_index(s, slab, p[i]);
		obj_ext = slab_obj_ext(slab, obj_exts, off);
		obj_cgroup_get(objcg);
		obj_ext->objcg = objcg;
		put_slab_obj_exts(obj_exts);
	}

	return true;
}

/*
 * __memcg_slab_free_hook() - 在 slab 对象真正回收到 allocator 前解除归属。
 *
 * @s/@slab 描述 cache 与所在 slab，@p/@objects 给出待释放数组。函数读取
 * 每个对象保存的 objcg，清除/合并 slab 与 list_lru 统计，把实际字节返还
 * obj_stock 并释放对象持有的 percpu_ref。无直接返回值；完成后对象元数据
 * 不再可用于 memcg 查询，调用者才可复用其存储。
 */
void __memcg_slab_free_hook(struct kmem_cache *s, struct slab *slab,
			    void **p, int objects, unsigned long obj_exts)
{
	size_t obj_size = obj_full_size(s);

	for (int i = 0; i < objects; i++) {
		struct obj_cgroup *objcg;
		struct slabobj_ext *obj_ext;
		struct obj_stock_pcp *stock;
		unsigned int off;

		off = obj_to_index(s, slab, p[i]);
		obj_ext = slab_obj_ext(slab, obj_exts, off);
		objcg = obj_ext->objcg;
		if (!objcg)
			continue;

		obj_ext->objcg = NULL;

		stock = trylock_stock();
		__refill_obj_stock(objcg, stock, obj_size, true);
		__account_obj_stock(objcg, stock, -obj_size,
				    slab_pgdat(slab), cache_vmstat_idx(s));
		unlock_stock(stock);

		obj_cgroup_put(objcg);
	}
}

/*
 * The objcg is only set on the first page, so transfer it to all the
 * other pages.
 */
/*
 * split_page_memcg() - 大阶 page 拆分后把首页 objcg 复制到其余基页。
 * @page 指向 order 块首页；无归属为空操作。每个新增归属都取得独立引用，
 * 使拆出的基页可分别 uncharge/free；page_counter 用量不变。
 */
void split_page_memcg(struct page *page, unsigned order)
{
	struct obj_cgroup *objcg = page_objcg(page);
	unsigned int i, nr = 1 << order;

	if (!objcg)
		return;

	for (i = 1; i < nr; i++)
		page_set_objcg(&page[i], objcg);

	obj_cgroup_get_many(objcg, nr - 1);
}

/*
 * folio_split_memcg_refs() - 为 folio 从 @old_order 拆到 @new_order 补齐 objcg 引用。
 * 元数据复制由拆分页路径完成，本函数只增加“新 folio 数减一”的引用；未启用
 * memcg 或未计费 folio 为空操作，不改变 counter 和统计。
 */
void folio_split_memcg_refs(struct folio *folio, unsigned old_order,
		unsigned new_order)
{
	unsigned new_refs;

	if (mem_cgroup_disabled() || !folio_memcg_charged(folio))
		return;

	new_refs = (1 << (old_order - new_order)) - 1;
	obj_cgroup_get_many(folio_objcg(folio), new_refs);
}

/*
 * memcg_online_kmem() - 让新上线的非根 @memcg 开始接收内核对象计费。
 * kmem 禁用时为空操作；启用全局 static key 后发布稳定 kmemcg_id。该 ID 用于
 * allocator 快速路径，offline 不在这里回收已有对象。
 */
static void memcg_online_kmem(struct mem_cgroup *memcg)
{
	if (mem_cgroup_kmem_disabled())
		return;

	if (unlikely(mem_cgroup_is_root(memcg)))
		return;

	static_branch_enable(&memcg_kmem_online_key);

	memcg->kmemcg_id = memcg->id.id;
}

/*
 * memcg_offline_kmem() - 将离线 @memcg 的 list_lru 归属重挂到父组。
 * 已分配对象仍由 objcg 引用维持并在释放时销账；这里处理 allocator 的分组
 * 索引，使离线组不再拥有独立 list_lru。根组和 kmem 禁用配置为空操作。
 */
static void memcg_offline_kmem(struct mem_cgroup *memcg)
{
	struct mem_cgroup *parent;

	if (mem_cgroup_kmem_disabled())
		return;

	if (unlikely(mem_cgroup_is_root(memcg)))
		return;

	parent = parent_mem_cgroup(memcg);
	memcg_reparent_list_lrus(memcg, parent);
}

#ifdef CONFIG_CGROUP_WRITEBACK

#include <trace/events/writeback.h>

/* memcg_wb_domain_init() - 为 @memcg 初始化独立写回带宽域并透传分配错误。 */
static int memcg_wb_domain_init(struct mem_cgroup *memcg, gfp_t gfp)
{
	return wb_domain_init(&memcg->cgwb_domain, gfp);
}

/* memcg_wb_domain_exit() - 销毁离线 memcg 的写回域；调用者保证用户已退出。 */
static void memcg_wb_domain_exit(struct mem_cgroup *memcg)
{
	wb_domain_exit(&memcg->cgwb_domain);
}

/* memcg_wb_domain_size_changed() - 通知写回控制器 memcg 可用内存尺度已变化。 */
static void memcg_wb_domain_size_changed(struct mem_cgroup *memcg)
{
	wb_domain_size_changed(&memcg->cgwb_domain);
}

/*
 * mem_cgroup_wb_domain() - 返回 @wb 非根 memcg 的 writeback domain 借用指针。
 * 根组继续使用全局域而返回 NULL；调用者通过 wb->memcg_css 生命周期保证结果。
 */
struct wb_domain *mem_cgroup_wb_domain(struct bdi_writeback *wb)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(wb->memcg_css);

	if (!memcg->css.parent)
		return NULL;

	return &memcg->cgwb_domain;
}

/**
 * mem_cgroup_wb_stats - retrieve writeback related stats from its memcg
 * @wb: bdi_writeback in question
 * @pfilepages: out parameter for number of file pages
 * @pheadroom: out parameter for number of allocatable pages according to memcg
 * @pdirty: out parameter for number of dirty pages
 * @pwriteback: out parameter for number of pages under writeback
 *
 * Determine the numbers of file, headroom, dirty, and writeback pages in
 * @wb's memcg.  File, dirty and writeback are self-explanatory.  Headroom
 * is a bit more involved.
 *
 * A memcg's headroom is "min(max, high) - used".  In the hierarchy, the
 * headroom is calculated as the lowest headroom of itself and the
 * ancestors.  Note that this doesn't consider the actual amount of
 * available memory in the system.  The caller should further cap
 * *@pheadroom accordingly.
 */
/*
 * mem_cgroup_wb_stats() - 为 @wb 采集文件页、脏页、回写页和层级 headroom。
 * 先限频刷新统计；headroom 取各非根祖先 min(max,high)-usage 的最小值，但不
 * 考虑系统全局可用内存，调用者还须再截断。四个输出参数均被完整写入。
 */
void mem_cgroup_wb_stats(struct bdi_writeback *wb, unsigned long *pfilepages,
			 unsigned long *pheadroom, unsigned long *pdirty,
			 unsigned long *pwriteback)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(wb->memcg_css);
	struct mem_cgroup *parent;

	mem_cgroup_flush_stats_ratelimited(memcg);

	*pdirty = memcg_page_state(memcg, NR_FILE_DIRTY);
	*pwriteback = memcg_page_state(memcg, NR_WRITEBACK);
	*pfilepages = memcg_page_state(memcg, NR_INACTIVE_FILE) +
			memcg_page_state(memcg, NR_ACTIVE_FILE);

	*pheadroom = PAGE_COUNTER_MAX;
	while ((parent = parent_mem_cgroup(memcg))) {
		unsigned long ceiling = min(READ_ONCE(memcg->memory.max),
					    READ_ONCE(memcg->memory.high));
		unsigned long used = page_counter_read(&memcg->memory);

		*pheadroom = min(*pheadroom, ceiling - min(ceiling, used));
		memcg = parent;
	}
}

/*
 * Foreign dirty flushing
 *
 * There's an inherent mismatch between memcg and writeback.  The former
 * tracks ownership per-page while the latter per-inode.  This was a
 * deliberate design decision because honoring per-page ownership in the
 * writeback path is complicated, may lead to higher CPU and IO overheads
 * and deemed unnecessary given that write-sharing an inode across
 * different cgroups isn't a common use-case.
 *
 * Combined with inode majority-writer ownership switching, this works well
 * enough in most cases but there are some pathological cases.  For
 * example, let's say there are two cgroups A and B which keep writing to
 * different but confined parts of the same inode.  B owns the inode and
 * A's memory is limited far below B's.  A's dirty ratio can rise enough to
 * trigger balance_dirty_pages() sleeps but B's can be low enough to avoid
 * triggering background writeback.  A will be slowed down without a way to
 * make writeback of the dirty pages happen.
 *
 * Conditions like the above can lead to a cgroup getting repeatedly and
 * severely throttled after making some progress after each
 * dirty_expire_interval while the underlying IO device is almost
 * completely idle.
 *
 * Solving this problem completely requires matching the ownership tracking
 * granularities between memcg and writeback in either direction.  However,
 * the more egregious behaviors can be avoided by simply remembering the
 * most recent foreign dirtying events and initiating remote flushes on
 * them when local writeback isn't enough to keep the memory clean enough.
 *
 * The following two functions implement such mechanism.  When a foreign
 * page - a page whose memcg and writeback ownerships don't match - is
 * dirtied, mem_cgroup_track_foreign_dirty() records the inode owning
 * bdi_writeback on the page owning memcg.  When balance_dirty_pages()
 * decides that the memcg needs to sleep due to high dirty ratio, it calls
 * mem_cgroup_flush_foreign() which queues writeback on the recorded
 * foreign bdi_writebacks which haven't expired.  Both the numbers of
 * recorded bdi_writebacks and concurrent in-flight foreign writebacks are
 * limited to MEMCG_CGWB_FRN_CNT.
 *
 * The mechanism only remembers IDs and doesn't hold any object references.
 * As being wrong occasionally doesn't matter, updates and accesses to the
 * records are lockless and racy.
 */
/*
 * mem_cgroup_track_foreign_dirty_slowpath() - 记录 folio 与 @wb 归属不一致的脏化。
 * 固定槽表中优先刷新已有记录，否则替换最老且无在途 IO 的槽；只保存 bdi/
 * memcg ID 与时间，不持引用，故允许对象回收造成的陈旧记录。该提示供后续
 * balance_dirty_pages() 触发远端写回，不直接启动 IO。
 */
void mem_cgroup_track_foreign_dirty_slowpath(struct folio *folio,
					     struct bdi_writeback *wb)
{
	struct mem_cgroup *memcg = folio_memcg(folio);
	struct memcg_cgwb_frn *frn;
	u64 now = get_jiffies_64();
	u64 oldest_at = now;
	int oldest = -1;
	int i;

	trace_track_foreign_dirty(folio, wb);

	/*
	 * Pick the slot to use.  If there is already a slot for @wb, keep
	 * using it.  If not replace the oldest one which isn't being
	 * written out.
	 */
	for (i = 0; i < MEMCG_CGWB_FRN_CNT; i++) {
		frn = &memcg->cgwb_frn[i];
		if (frn->bdi_id == wb->bdi->id &&
		    frn->memcg_id == wb->memcg_css->id)
			break;
		if (time_before64(frn->at, oldest_at) &&
		    atomic_read(&frn->done.cnt) == 1) {
			oldest = i;
			oldest_at = frn->at;
		}
	}

	if (i < MEMCG_CGWB_FRN_CNT) {
		/*
		 * Re-using an existing one.  Update timestamp lazily to
		 * avoid making the cacheline hot.  We want them to be
		 * reasonably up-to-date and significantly shorter than
		 * dirty_expire_interval as that's what expires the record.
		 * Use the shorter of 1s and dirty_expire_interval / 8.
		 */
		unsigned long update_intv =
			min_t(unsigned long, HZ,
			      msecs_to_jiffies(dirty_expire_interval * 10) / 8);

		if (time_before64(frn->at, now - update_intv))
			frn->at = now;
	} else if (oldest >= 0) {
		/* replace the oldest free one */
		frn = &memcg->cgwb_frn[oldest];
		frn->bdi_id = wb->bdi->id;
		frn->memcg_id = wb->memcg_css->id;
		frn->at = now;
	}
}

/* issue foreign writeback flushes for recorded foreign dirtying events */
/*
 * mem_cgroup_flush_foreign() - 为仍新鲜且无在途请求的 foreign 记录启动写回。
 * @wb 确定当前 memcg；函数清记录时间后按保存的 ID 异步排队，并用 completion
 * 限制每槽一个请求。ID 查找失败由 writeback 层容忍，无同步完成保证。
 */
void mem_cgroup_flush_foreign(struct bdi_writeback *wb)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(wb->memcg_css);
	unsigned long intv = msecs_to_jiffies(dirty_expire_interval * 10);
	u64 now = jiffies_64;
	int i;

	for (i = 0; i < MEMCG_CGWB_FRN_CNT; i++) {
		struct memcg_cgwb_frn *frn = &memcg->cgwb_frn[i];

		/*
		 * If the record is older than dirty_expire_interval,
		 * writeback on it has already started.  No need to kick it
		 * off again.  Also, don't start a new one if there's
		 * already one in flight.
		 */
		if (time_after64(frn->at, now - intv) &&
		    atomic_read(&frn->done.cnt) == 1) {
			frn->at = 0;
			trace_flush_foreign(wb, frn->bdi_id, frn->memcg_id);
			cgroup_writeback_by_id(frn->bdi_id, frn->memcg_id,
					       WB_REASON_FOREIGN_FLUSH,
					       &frn->done);
		}
	}
}

#else	/* CONFIG_CGROUP_WRITEBACK */

/* 未启用 cgroup writeback 时无需私有域，初始化恒成功。 */
static int memcg_wb_domain_init(struct mem_cgroup *memcg, gfp_t gfp)
{
	return 0;
}

/* 未启用 cgroup writeback 时没有域资源需要释放。 */
static void memcg_wb_domain_exit(struct mem_cgroup *memcg)
{
}

/* 未启用 cgroup writeback 时容量变化无需通知任何后端。 */
static void memcg_wb_domain_size_changed(struct mem_cgroup *memcg)
{
}

#endif	/* CONFIG_CGROUP_WRITEBACK */

/*
 * Private memory cgroup IDR
 *
 * Swap-out records and page cache shadow entries need to store memcg
 * references in constrained space, so we maintain an ID space that is
 * limited to 16 bit (MEM_CGROUP_ID_MAX), limiting the total number of
 * memory-controlled cgroups to 64k.
 *
 * However, there usually are many references to the offline CSS after
 * the cgroup has been destroyed, such as page cache or reclaimable
 * slab objects, that don't need to hang on to the ID. We want to keep
 * those dead CSS from occupying IDs, or we might quickly exhaust the
 * relatively small ID space and prevent the creation of new cgroups
 * even when there are much fewer than 64k cgroups - possibly none.
 *
 * Maintain a private 16-bit ID space for memcg, and allow the ID to
 * be freed and recycled when it's no longer needed, which is usually
 * when the CSS is offlined.
 *
 * The only exception to that are records of swapped out tmpfs/shmem
 * pages that need to be attributed to live ancestors on swapin. But
 * those references are manageable from userspace.
 */

#define MEM_CGROUP_ID_MAX	((1UL << MEM_CGROUP_ID_SHIFT) - 1)
static DEFINE_XARRAY_ALLOC1(mem_cgroup_private_ids);

/*
 * mem_cgroup_private_id_remove() - 从 xarray 撤销 @memcg 的可复用 16 位私有 ID。
 * 仅正 ID 曾被发布；先 erase 使新 RCU 查找不可达，再把字段清零。调用者保证
 * private-id ref 已归零或对象尚未发布，根组保留其特殊 ID。
 */
static void mem_cgroup_private_id_remove(struct mem_cgroup *memcg)
{
	if (memcg->id.id > 0) {
		xa_erase(&mem_cgroup_private_ids, memcg->id.id);
		memcg->id.id = 0;
	}
}

/*
 * mem_cgroup_private_id_put() - 释放 @n 个 swap/shadow 对私有 ID 的引用。
 * 最后一批引用归零时摘除 ID，并释放分配 ID 时固定的 css；@n 必须与此前 get
 * 数量配对，refcount 会检测下溢。函数不等待已进入的 RCU reader。
 */
static inline void mem_cgroup_private_id_put(struct mem_cgroup *memcg, unsigned int n)
{
	if (refcount_sub_and_test(n, &memcg->id.ref)) {
		mem_cgroup_private_id_remove(memcg);

		/* Memcg ID pins CSS */
		css_put(&memcg->css);
	}
}

/*
 * mem_cgroup_private_id_get_online() - 为 @memcg 的 @n 个持久记录取得有效 ID 引用。
 * 若目标已 offline 且引用为零，就向父组重试，直到找到仍可加引用的祖先；
 * 根组按协议永不归零。返回的 memcg 由 ID 引用固定，调用者须按 n 对称 put。
 */
struct mem_cgroup *mem_cgroup_private_id_get_online(struct mem_cgroup *memcg, unsigned int n)
{
	while (!refcount_add_not_zero(n, &memcg->id.ref)) {
		/*
		 * The root cgroup cannot be destroyed, so it's refcount must
		 * always be >= 1.
		 */
		if (WARN_ON_ONCE(mem_cgroup_is_root(memcg))) {
			VM_BUG_ON(1);
			break;
		}
		memcg = parent_mem_cgroup(memcg);
	}
	return memcg;
}

/**
 * mem_cgroup_from_private_id - look up a memcg from a memcg id
 * @id: the memcg id to look up
 *
 * Caller must hold rcu_read_lock().
 */
/*
 * mem_cgroup_from_private_id() - 在 RCU 下按 16 位 @id 查询当前 memcg。
 * 返回借用指针或 NULL；private-id 引用防止被记录引用的 css 消失，但调用者
 * 仍必须保持 RCU 临界区覆盖使用。本函数用 WARN 检查该前置条件。
 */
struct mem_cgroup *mem_cgroup_from_private_id(unsigned short id)
{
	WARN_ON_ONCE(!rcu_read_lock_held());
	return xa_load(&mem_cgroup_private_ids, id);
}

/*
 * mem_cgroup_get_from_id() - 按用户可见 64 位 cgroup ID 取得 memory css 引用。
 * 先固定 cgroup，再取得有效 memory css；成功返回需 mem_cgroup_put() 的 memcg，
 * 查找或子系统 css 失败返回 NULL。释放临时 cgroup 不影响已取得 css 引用。
 */
struct mem_cgroup *mem_cgroup_get_from_id(u64 id)
{
	struct cgroup *cgrp;
	struct cgroup_subsys_state *css;
	struct mem_cgroup *memcg = NULL;

	cgrp = cgroup_get_from_id(id);
	if (IS_ERR(cgrp))
		return NULL;

	css = cgroup_get_e_css(cgrp, &memory_cgrp_subsys);
	if (css)
		memcg = container_of(css, struct mem_cgroup, css);

	cgroup_put(cgrp);

	return memcg;
}

/* free_mem_cgroup_per_node_info() - 逆序释放一个 nodeinfo 的 per-CPU、聚合与主体。 */
static void free_mem_cgroup_per_node_info(struct mem_cgroup_per_node *pn)
{
	if (!pn)
		return;

	free_percpu(pn->lruvec_stats_percpu);
	kfree(pn->lruvec_stats);
	kfree(pn);
}

/*
 * alloc_mem_cgroup_per_node_info() - 为 @memcg 的 NUMA @node 构造 lruvec 统计容器。
 * 各级分配完成后才发布到 memcg->nodeinfo；任一步失败走统一释放并返回 false，
 * 成功初始化锁、lruvec、objcg 发布槽和 per-CPU 统计后返回 true。
 */
static bool alloc_mem_cgroup_per_node_info(struct mem_cgroup *memcg, int node)
{
	struct mem_cgroup_per_node *pn;

	pn = kmem_cache_alloc_node(memcg_pn_cachep, GFP_KERNEL | __GFP_ZERO,
				   node);
	if (!pn)
		return false;

	pn->lruvec_stats = kzalloc_node(sizeof(struct lruvec_stats),
					GFP_KERNEL_ACCOUNT, node);
	if (!pn->lruvec_stats)
		goto fail;

	pn->lruvec_stats_percpu = alloc_percpu_gfp(struct lruvec_stats_percpu,
						   GFP_KERNEL_ACCOUNT);
	if (!pn->lruvec_stats_percpu)
		goto fail;

	INIT_LIST_HEAD(&pn->objcg_list);

	lruvec_init(&pn->lruvec);
	pn->memcg = memcg;

	memcg->nodeinfo[node] = pn;
	return true;
fail:
	free_mem_cgroup_per_node_info(pn);
	return false;
}

/*
 * 【mem_cgroup 生命周期】
 *
 * alloc 逐节点创建 lruvec/vmstats/objcg/page_counter 等私有状态；online
 * 发布层级关系并启用 static key；offline 停止新计费、清 OOM/high 状态并
 * 重挂仍存活资源；released 阶段排异步清理；free 最终释放节点数组、ID 与
 * slab 对象。失败标签严格逆序撤销已成功的分配。css 引用归零不代表 folio/
 * slab 对象已消失，objcg 重挂正是解耦两种寿命的关键。
 */
static void __mem_cgroup_free(struct mem_cgroup *memcg)
{
	int node;

	for_each_node(node) {
		struct mem_cgroup_per_node *pn = memcg->nodeinfo[node];
		if (!pn)
			continue;

		obj_cgroup_put(pn->orig_objcg);
		free_mem_cgroup_per_node_info(pn);
	}
	memcg1_free_events(memcg);
	kfree(memcg->vmstats);
	free_percpu(memcg->vmstats_percpu);
	kfree(memcg);
}

/*
 * mem_cgroup_free() - 在 css 最终释放阶段退出 MGLRU/writeback 后销毁主体。
 * @memcg 已离线、不可再被新查找，调用者拥有最终释放责任。函数可睡眠，
 * 无返回值；退出子系统附属状态后由 __mem_cgroup_free() 释放逐节点资源。
 */
static void mem_cgroup_free(struct mem_cgroup *memcg)
{
	lru_gen_exit_memcg(memcg);
	memcg_wb_domain_exit(memcg);
	__mem_cgroup_free(memcg);
}

/*
 * mem_cgroup_alloc() - 构造尚未 online、外界不可发现的 memcg。
 *
 * @parent 为层级父组借用指针，根创建时可为 NULL。函数可睡眠，依次分配
 * 主对象、per-CPU vmstats、每 NUMA 节点状态、page_counter 与可选 writeback
 * 域。成功返回由 cgroup core 接管的对象；失败返回错误指针，并沿标签只
 * 释放已取得资源。此时没有任务/folio 能查到新组，所以回滚无需重挂。
 */
static struct mem_cgroup *mem_cgroup_alloc(struct mem_cgroup *parent)
{
	struct memcg_vmstats_percpu *statc;
	struct memcg_vmstats_percpu __percpu *pstatc_pcpu;
	struct mem_cgroup *memcg;
	int node, cpu;
	int __maybe_unused i;
	long error;

	/*
	 * 阶段 1：分配尚未发布的主对象并预留私有 ID。此时没有并发查找者，
	 * 任一步失败都可直接进入统一 fail；error 保存要编码进 ERR_PTR 的 errno。
	 */
	memcg = kmem_cache_zalloc(memcg_cachep, GFP_KERNEL);
	if (!memcg)
		return ERR_PTR(-ENOMEM);

	error = xa_alloc(&mem_cgroup_private_ids, &memcg->id.id, NULL,
			 XA_LIMIT(1, MEM_CGROUP_ID_MAX), GFP_KERNEL);
	if (error)
		goto fail;
	error = -ENOMEM;

	/*
	 * 阶段 2：建立聚合端、per-CPU 源端和 v1 事件。分配都使用可睡眠 GFP，
	 * 且尚未连接父链；失败时 __mem_cgroup_free() 只释放已经非 NULL 的成员。
	 */
	memcg->vmstats = kzalloc_obj(struct memcg_vmstats, GFP_KERNEL_ACCOUNT);
	if (!memcg->vmstats)
		goto fail;

	memcg->vmstats_percpu = alloc_percpu_gfp(struct memcg_vmstats_percpu,
						 GFP_KERNEL_ACCOUNT);
	if (!memcg->vmstats_percpu)
		goto fail;

	if (!memcg1_alloc_events(memcg))
		goto fail;

	/*
	 * 阶段 3：缓存每 CPU 的父传播目标。父 memcg 在 css_alloc 调用期间稳定，
	 * 后续 rstat 热路径可直接沿 parent_pcpu 走，不必重复解析 cgroup 层级。
	 */
	pstatc_pcpu = parent ? parent->vmstats_percpu : NULL;
	for_each_possible_cpu(cpu) {
		statc = per_cpu_ptr(memcg->vmstats_percpu, cpu);
		statc->parent_pcpu = pstatc_pcpu;
		statc->vmstats = memcg->vmstats;
	}

	/* 每个可能 NUMA 节点都要有 lruvec/objcg 容器，保证运行期无需懒分配。 */
	for_each_node(node)
		if (!alloc_mem_cgroup_per_node_info(memcg, node))
			goto fail;

	if (memcg_wb_domain_init(memcg, GFP_KERNEL))
		goto fail;

	/*
	 * 阶段 4：所有可失败的大资源到位后初始化工作、压力、peak 与配置分支。
	 * 这些对象尚未排队或发布；css_online() 成功后才允许并发使用。
	 */
	INIT_WORK(&memcg->high_work, high_work_func);
	vmpressure_init(&memcg->vmpressure);
	INIT_LIST_HEAD(&memcg->memory_peaks);
	INIT_LIST_HEAD(&memcg->swap_peaks);
	spin_lock_init(&memcg->peaks_lock);
	memcg->socket_pressure = get_jiffies_64();
#if BITS_PER_LONG < 64
	seqlock_init(&memcg->socket_pressure_seqlock);
#endif
	memcg1_memcg_init(memcg);
	memcg->kmemcg_id = -1;
#ifdef CONFIG_CGROUP_WRITEBACK
	INIT_LIST_HEAD(&memcg->cgwb_list);
	for (i = 0; i < MEMCG_CGWB_FRN_CNT; i++)
		memcg->cgwb_frn[i].done =
			__WB_COMPLETION_INIT(&memcg_cgwb_frn_waitq);
#endif
	lru_gen_init_memcg(memcg);
	return memcg;
fail:
	/*
	 * 回滚点：memcg 从未 online，也未写入 private ID XArray；先撤 ID 预留，
	 * 再由统一析构检查并释放 per-node、events、vmstats 与主对象。没有引用
	 * 转移给外部，故返回 ERR_PTR(error) 后调用者无需额外清理。
	 */
	mem_cgroup_private_id_remove(memcg);
	__mem_cgroup_free(memcg);
	return ERR_PTR(error);
}

/*
 * mem_cgroup_css_alloc() - cgroup core 的构造入口并连接 page_counter 父链。
 *
 * @parent_css 可为 NULL（根组）；函数临时把 active_memcg 设为父组，使构造
 * 自身产生的内存也归父组，随后无条件恢复。成功返回新 css；失败返回错误
 * 指针。根组还一次性初始化统计索引并发布 root_mem_cgroup；非根组继承
 * swappiness/部分 v1 策略并增加所需 static key，后续由 css_online 发布。
 */
static struct cgroup_subsys_state * __ref
mem_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct mem_cgroup *parent = mem_cgroup_from_css(parent_css);
	struct mem_cgroup *memcg, *old_memcg;
	bool memcg_on_dfl = cgroup_subsys_on_dfl(memory_cgrp_subsys);

	old_memcg = set_active_memcg(parent);
	/* 构造失败也必须恢复 active 覆盖，否则当前任务后续分配会错误归父组。 */
	memcg = mem_cgroup_alloc(parent);
	set_active_memcg(old_memcg);
	if (IS_ERR(memcg))
		return ERR_CAST(memcg);

	page_counter_set_high(&memcg->memory, PAGE_COUNTER_MAX);
	memcg1_soft_limit_reset(memcg);
#ifdef CONFIG_ZSWAP
	memcg->zswap_max = PAGE_COUNTER_MAX;
	WRITE_ONCE(memcg->zswap_writeback, true);
#endif
	page_counter_set_high(&memcg->swap, PAGE_COUNTER_MAX);
	if (parent) {
		/* 非根组把各 counter 接到父 counter，形成限额的层级传播链。 */
		WRITE_ONCE(memcg->swappiness, mem_cgroup_swappiness(parent));

		page_counter_init(&memcg->memory, &parent->memory, memcg_on_dfl);
		page_counter_init(&memcg->swap, &parent->swap, false);
#ifdef CONFIG_MEMCG_V1
		memcg->memory.track_failcnt = !memcg_on_dfl;
		WRITE_ONCE(memcg->oom_kill_disable, READ_ONCE(parent->oom_kill_disable));
		page_counter_init(&memcg->kmem, &parent->kmem, false);
		page_counter_init(&memcg->tcpmem, &parent->tcpmem, false);
#endif
	} else {
		/* 根组没有父 counter，并成为所有无法细分归属的稳定 fallback。 */
		init_memcg_stats();
		init_memcg_events();
		page_counter_init(&memcg->memory, NULL, true);
		page_counter_init(&memcg->swap, NULL, false);
#ifdef CONFIG_MEMCG_V1
		page_counter_init(&memcg->kmem, NULL, false);
		page_counter_init(&memcg->tcpmem, NULL, false);
#endif
		root_mem_cgroup = memcg;
		return &memcg->css;
	}

	if (memcg_on_dfl && !cgroup_memory_nosocket)
		static_branch_inc(&memcg_sockets_enabled_key);

	if (!cgroup_memory_nobpf)
		static_branch_inc(&memcg_bpf_enabled_key);

	return &memcg->css;
}

/*
 * mem_cgroup_css_online() - 把已构造 memcg 发布为可计费的在线 css。
 *
 * @css 内嵌于 memcg，由 cgroup core 持有。函数连接 page_counter 父链、
 * 初始化水位/保护值并使 kmem/BPF static key 生效。返回 0 表示后续 attach
 * 和 charge 可观察该组；失败 errno 由 core 触发 offline/free 回滚。发布
 * 后字段更新必须遵守各自锁/RCU，而不能再依赖构造期独占。
 */
static int mem_cgroup_css_online(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct obj_cgroup *objcg;
	int nid;

	memcg_online_kmem(memcg);

	/*
	 * A memcg must be visible for expand_shrinker_info()
	 * by the time the maps are allocated. So, we allocate maps
	 * here, when for_each_mem_cgroup() can't skip it.
	 */
	if (alloc_shrinker_info(memcg))
		goto offline_kmem;

	/*
	 * 阶段 1：每个 NUMA 节点建立 active objcg。rcu_assign_pointer 是发布点；
	 * orig_objcg 额外引用留给最终 free，普通对象引用可跨越 offline。
	 */
	for_each_node(nid) {
		objcg = obj_cgroup_alloc();
		if (!objcg)
			goto free_objcg;

		if (unlikely(mem_cgroup_is_root(memcg)))
			objcg->is_root = true;

		objcg->memcg = memcg;
		rcu_assign_pointer(memcg->nodeinfo[nid]->objcg, objcg);
		obj_cgroup_get(objcg);
		memcg->nodeinfo[nid]->orig_objcg = objcg;
	}

	if (unlikely(mem_cgroup_is_root(memcg)) && !mem_cgroup_disabled())
		queue_delayed_work(system_dfl_wq, &stats_flush_dwork,
				   FLUSH_TIME);
	lru_gen_online_memcg(memcg);

	/* Online state pins memcg ID, memcg ID pins CSS */
	/*
	 * 在线 ID 反向 pin css：只要 swap/workingset 等私有 ID 仍可查找，就不能
	 * 释放 memcg。refcount 与 css_get 必须由 offline 的 private_id_put 配对。
	 */
	refcount_set(&memcg->id.ref, 1);
	css_get(css);

	/*
	 * Ensure mem_cgroup_from_private_id() works once we're fully online.
	 *
	 * We could do this earlier and require callers to filter with
	 * css_tryget_online(). But right now there are no users that
	 * need earlier access, and the workingset code relies on the
	 * cgroup tree linkage (mem_cgroup_get_nr_swap_pages()). So
	 * publish it here at the end of onlining. This matches the
	 * regular ID destruction during offlining.
	 */
	xa_store(&mem_cgroup_private_ids, memcg->id.id, memcg, GFP_KERNEL);

	return 0;
free_objcg:
	/*
	 * 回滚栈：已有节点 objcg 先从 RCU 发布点摘除并 kill；orig 引用清空以防
	 * __mem_cgroup_free() 二次 put；随后撤 shrinker、kmem 与 private ID。
	 */
	for_each_node(nid) {
		struct mem_cgroup_per_node *pn = memcg->nodeinfo[nid];

		objcg = rcu_replace_pointer(pn->objcg, NULL, true);
		if (objcg)
			percpu_ref_kill(&objcg->refcnt);

		if (pn->orig_objcg) {
			obj_cgroup_put(pn->orig_objcg);
			/*
			 * Reset pn->orig_objcg to NULL to prevent
			 * obj_cgroup_put() from being called again in
			 * __mem_cgroup_free().
			 */
			pn->orig_objcg = NULL;
		}
	}
	free_shrinker_info(memcg);
offline_kmem:
	memcg_offline_kmem(memcg);
	mem_cgroup_private_id_remove(memcg);
	return -ENOMEM;
}

/*
 * mem_cgroup_css_offline() - 关闭新归属并把存量资源迁往父组。
 *
 * @css 仍由 cgroup core 保证存储期。函数先让控制组离开 online 查找，再
 * drain stock、解除 OOM/事件状态并重挂 LRU/objcg/统计。返回无直接值；
 * 已发布对象可继续存活但其计费身份已沿父链接管，最终 free 不再等待每个
 * slab/folio 同步死亡。该路径可睡眠且必须与并发 charge 的引用协议配合。
 */
static void mem_cgroup_css_offline(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	memcg1_css_offline(memcg);

	/* 阶段 1：先清保护水位，避免离线子组继续从父组 reclaim 中获得保护。 */
	page_counter_set_min(&memcg->memory, 0);
	page_counter_set_low(&memcg->memory, 0);

	zswap_memcg_offline_cleanup(memcg);

	memcg_offline_kmem(memcg);
	/*
	 * The reparenting of objcg must be after the reparenting of
	 * the list_lru in memcg_offline_kmem(), which ensures that
	 * they will not mistakenly get the parent list_lru.
	 */
	/*
	 * 必须先重挂 list_lru 再重挂 objcg：否则仍引用旧对象的 free 路径可能
	 * 按父 objcg 去访问尚属于子组的 list_lru，导致统计和链表归属错配。
	 */
	memcg_reparent_objcgs(memcg);
	reparent_shrinker_deferred(memcg);
	wb_memcg_offline(memcg);
	lru_gen_offline_memcg(memcg);

	drain_all_stock(memcg);

	/* 最后摘除私有 ID 的在线 pin；已有 ID 使用者各自持有剩余引用。 */
	mem_cgroup_private_id_put(memcg, 1);
}

/*
 * mem_cgroup_css_released() - css 引用归零后清除仍可能指向本组的共享游标。
 * @css 的存储仍由 cgroup core 保证；函数使回收/MGLRU 不再发现该组，不释放
 * memcg 主体。返回无直接值，后续 css_free() 才执行最终资源回收。
 */
static void mem_cgroup_css_released(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	invalidate_reclaim_iterators(memcg);
	lru_gen_release_memcg(memcg);
}

/*
 * mem_cgroup_css_free() - cgroup core 的 memcg 最终析构回调。
 *
 * @css 已 released 且不再可查找。函数等待 writeback foreign completion，
 * 对称递减 online 阶段 static key，取消 high work、清 vmpressure/shrinker，
 * 最后释放主体。可睡眠；返回后 @css/@memcg 均不可再访问。
 */
static void mem_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	int __maybe_unused i;

#ifdef CONFIG_CGROUP_WRITEBACK
	for (i = 0; i < MEMCG_CGWB_FRN_CNT; i++)
		wb_wait_for_completion(&memcg->cgwb_frn[i].done);
#endif
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys) && !cgroup_memory_nosocket)
		static_branch_dec(&memcg_sockets_enabled_key);

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys) && memcg1_tcpmem_active(memcg))
		static_branch_dec(&memcg_sockets_enabled_key);

	if (!cgroup_memory_nobpf)
		static_branch_dec(&memcg_bpf_enabled_key);

	vmpressure_cleanup(&memcg->vmpressure);
	cancel_work_sync(&memcg->high_work);
	memcg1_remove_from_trees(memcg);
	free_shrinker_info(memcg);
	mem_cgroup_free(memcg);
}

/**
 * mem_cgroup_css_reset - reset the states of a mem_cgroup
 * @css: the target css
 *
 * Reset the states of the mem_cgroup associated with @css.  This is
 * invoked when the userland requests disabling on the default hierarchy
 * but the memcg is pinned through dependency.  The memcg should stop
 * applying policies and should revert to the vanilla state as it may be
 * made visible again.
 *
 * The current implementation only resets the essential configurations.
 * This needs to be expanded to cover all the visible parts.
 */
/*
 * mem_cgroup_css_reset() 的原意是：默认层级请求禁用、但依赖关系仍 pin 住
 * css 时，撤销其限额策略以恢复普通行为。当前实现只复位 max/min/low/high、
 * swap 和 v1 soft limit 等核心项，并未清空全部用户可见状态；@css 仍在线，
 * 函数不转移引用、无直接返回值，后续可再次对用户可见。
 */
static void mem_cgroup_css_reset(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	page_counter_set_max(&memcg->memory, PAGE_COUNTER_MAX);
	page_counter_set_max(&memcg->swap, PAGE_COUNTER_MAX);
#ifdef CONFIG_MEMCG_V1
	page_counter_set_max(&memcg->kmem, PAGE_COUNTER_MAX);
	page_counter_set_max(&memcg->tcpmem, PAGE_COUNTER_MAX);
#endif
	page_counter_set_min(&memcg->memory, 0);
	page_counter_set_low(&memcg->memory, 0);
	page_counter_set_high(&memcg->memory, PAGE_COUNTER_MAX);
	memcg1_soft_limit_reset(memcg);
	page_counter_set_high(&memcg->swap, PAGE_COUNTER_MAX);
	memcg_wb_domain_size_changed(memcg);
}

/*
 * rstat flush 回调使用 aggregate_control 把当前 CPU 的 state/event 增量
 * 同时汇入本组 local、子树 aggregated 以及父组 pending。字段只在一次
 * flush 临界区有效；它是传播游标而不是长期对象。
 */
struct aggregate_control {
	/* pointer to the aggregated (CPU and subtree aggregated) counters */
	long *aggregate;
	/* pointer to the non-hierarchichal (CPU aggregated) counters */
	long *local;
	/* pointer to the pending child counters during tree propagation */
	long *pending;
	/* pointer to the parent's pending counters, could be NULL */
	long *ppending;
	/* pointer to the percpu counters to be aggregated */
	long *cstat;
	/* pointer to the percpu counters of the last aggregation*/
	long *cstat_prev;
	/* size of the above counters */
	int size;
};

/*
 * mem_cgroup_stat_aggregate() - 把一个 per-CPU 数组相对上次快照的增量向上提交。
 * @ac 是 flush 栈上描述符，给出 local/aggregate/pending/parent pending 数组及
 * 长度。函数逐槽计算 delta：更新本组 local 与 subtree，再把 subtree 增量
 * 交给父组 pending；调用者持有 rstat 串行化，返回无直接值。
 */
static void mem_cgroup_stat_aggregate(struct aggregate_control *ac)
{
	int i;
	long delta, delta_cpu, v;

	for (i = 0; i < ac->size; i++) {
		/*
		 * Collect the aggregated propagation counts of groups
		 * below us. We're in a per-cpu loop here and this is
		 * a global counter, so the first cycle will get them.
		 */
		delta = ac->pending[i];
		if (delta)
			ac->pending[i] = 0;

		/* Add CPU changes on this level since the last flush */
		delta_cpu = 0;
		v = READ_ONCE(ac->cstat[i]);
		if (v != ac->cstat_prev[i]) {
			delta_cpu = v - ac->cstat_prev[i];
			delta += delta_cpu;
			ac->cstat_prev[i] = v;
		}

		/* Aggregate counts on this level and propagate upwards */
		if (delta_cpu)
			ac->local[i] += delta_cpu;

		if (delta) {
			ac->aggregate[i] += delta;
			if (ac->ppending)
				ac->ppending[i] += delta;
		}
	}
}

#ifdef CONFIG_MEMCG_NMI_SAFETY_REQUIRES_ATOMIC
/*
 * flush_nmi_stats() - 把 NMI 中只能原子累计的 slab 字节并入普通 rstat 数组。
 * @memcg/@parent 为当前 flush 节点及父组，@cpu 指定源 CPU。atomic_xchg() 既
 * 领取本批增量又清零源槽，避免与新 NMI 更新丢失；关闭特殊配置时为空 stub。
 */
static void flush_nmi_stats(struct mem_cgroup *memcg, struct mem_cgroup *parent,
			    int cpu)
{
	int nid;

	if (atomic_read(&memcg->kmem_stat)) {
		int kmem = atomic_xchg(&memcg->kmem_stat, 0);
		int index = memcg_stats_index(MEMCG_KMEM);

		memcg->vmstats->state[index] += kmem;
		if (parent)
			parent->vmstats->state_pending[index] += kmem;
	}

	for_each_node_state(nid, N_MEMORY) {
		struct mem_cgroup_per_node *pn = memcg->nodeinfo[nid];
		struct lruvec_stats *lstats = pn->lruvec_stats;
		struct lruvec_stats *plstats = NULL;

		if (parent)
			plstats = parent->nodeinfo[nid]->lruvec_stats;

		if (atomic_read(&pn->slab_reclaimable)) {
			int slab = atomic_xchg(&pn->slab_reclaimable, 0);
			int index = memcg_stats_index(NR_SLAB_RECLAIMABLE_B);

			lstats->state[index] += slab;
			if (plstats)
				plstats->state_pending[index] += slab;
			memcg->vmstats->state[index] += slab;
			if (parent)
				parent->vmstats->state_pending[index] += slab;
		}
		if (atomic_read(&pn->slab_unreclaimable)) {
			int slab = atomic_xchg(&pn->slab_unreclaimable, 0);
			int index = memcg_stats_index(NR_SLAB_UNRECLAIMABLE_B);

			lstats->state[index] += slab;
			if (plstats)
				plstats->state_pending[index] += slab;
			memcg->vmstats->state[index] += slab;
			if (parent)
				parent->vmstats->state_pending[index] += slab;
		}
	}
}
#else
/* 无 NMI 旁路统计的配置下，rstat flush 无需额外合并。 */
static void flush_nmi_stats(struct mem_cgroup *memcg, struct mem_cgroup *parent,
			    int cpu)
{}
#endif

/*
 * mem_cgroup_css_rstat_flush() - cgroup rstat 自底向上刷新 memcg 在 @cpu 的数据。
 *
 * @css 映射当前 memcg，父节点已由 rstat 顺序保证可接收 pending。函数先领取
 * NMI 旁路统计，再分别聚合 state、events 和每 NUMA 节点 lruvec，最后清本轮
 * update 计数。运行在全局 rstat flush 串行区，无失败返回，也不释放 css。
 */
static void mem_cgroup_css_rstat_flush(struct cgroup_subsys_state *css, int cpu)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	struct mem_cgroup *parent = parent_mem_cgroup(memcg);
	struct memcg_vmstats_percpu *statc;
	struct aggregate_control ac;
	int nid;

	/* 阶段 1：先领取 NMI 旁路值，避免统一快照漏掉只能原子累计的 slab 更新。 */
	flush_nmi_stats(memcg, parent, cpu);

	statc = per_cpu_ptr(memcg->vmstats_percpu, cpu);

	/*
	 * 阶段 2：state 当前值减 prev 得到本轮增量，同时更新本组 local、子树
	 * aggregate 与父 pending；父节点随后在同一自底向上 flush 中继续领取。
	 */
	ac = (struct aggregate_control) {
		.aggregate = memcg->vmstats->state,
		.local = memcg->vmstats->state_local,
		.pending = memcg->vmstats->state_pending,
		.ppending = parent ? parent->vmstats->state_pending : NULL,
		.cstat = statc->state,
		.cstat_prev = statc->state_prev,
		.size = MEMCG_VMSTAT_SIZE,
	};
	mem_cgroup_stat_aggregate(&ac);

	/* 阶段 3：events 使用同一传播协议，但拥有独立数组长度和历史快照。 */
	ac = (struct aggregate_control) {
		.aggregate = memcg->vmstats->events,
		.local = memcg->vmstats->events_local,
		.pending = memcg->vmstats->events_pending,
		.ppending = parent ? parent->vmstats->events_pending : NULL,
		.cstat = statc->events,
		.cstat_prev = statc->events_prev,
		.size = NR_MEMCG_EVENTS,
	};
	mem_cgroup_stat_aggregate(&ac);

	/*
	 * 阶段 4：逐个有内存的 NUMA node 传播 lruvec state；node 维度不变，
	 * 只把 cgroup 维度的子树增量交给同一节点上的父 lruvec。
	 */
	for_each_node_state(nid, N_MEMORY) {
		struct mem_cgroup_per_node *pn = memcg->nodeinfo[nid];
		struct lruvec_stats *lstats = pn->lruvec_stats;
		struct lruvec_stats *plstats = NULL;
		struct lruvec_stats_percpu *lstatc;

		if (parent)
			plstats = parent->nodeinfo[nid]->lruvec_stats;

		lstatc = per_cpu_ptr(pn->lruvec_stats_percpu, cpu);

		ac = (struct aggregate_control) {
			.aggregate = lstats->state,
			.local = lstats->state_local,
			.pending = lstats->state_pending,
			.ppending = plstats ? plstats->state_pending : NULL,
			.cstat = lstatc->state,
			.cstat_prev = lstatc->state_prev,
			.size = NR_MEMCG_NODE_STAT_ITEMS,
		};
		mem_cgroup_stat_aggregate(&ac);

	}
	/* 当前 CPU 的局部批次已领取；后续新更新会重新增加 stats_updates。 */
	WRITE_ONCE(statc->stats_updates, 0);
	/* We are in a per-cpu loop here, only do the atomic write once */
	/*
	 * css_rstat_flush 会逐 CPU 调用本函数，但全局阈值只需清一次。若并发更新
	 * 在此之后发生，它会再次登记 css；清零只影响启发阈值，不丢实际数组值。
	 */
	if (atomic_long_read(&memcg->vmstats->stats_updates))
		atomic_long_set(&memcg->vmstats->stats_updates, 0);
}

/*
 * mem_cgroup_fork() - 初始化新任务的 objcg 缓存为“首次分配时更新”。
 * fork 路径操作尚未并发运行的 @task，无需原子同步；不立即查 memcg，可避免
 * 从未做内核对象分配的任务承担引用开销。
 */
static void mem_cgroup_fork(struct task_struct *task)
{
	/*
	 * Set the update flag to cause task->objcg to be initialized lazily
	 * on the first allocation. It can be done without any synchronization
	 * because it's always performed on the current task, so does
	 * current_objcg_update().
	 */
	task->objcg = (struct obj_cgroup *)CURRENT_OBJCG_UPDATE_FLAG;
}

/*
 * mem_cgroup_exit() - 释放任务缓存的 objcg 引用并清空指针。
 * 先去掉 UPDATE_FLAG 再 put，因 NULL/仅 flag 均可安全处理；只对 current 执行，
 * 与 current_objcg_update() 不并发。退出后极晚分配不再精确归入原 memcg。
 */
static void mem_cgroup_exit(struct task_struct *task)
{
	struct obj_cgroup *objcg = task->objcg;

	objcg = (struct obj_cgroup *)
		((unsigned long)objcg & ~CURRENT_OBJCG_UPDATE_FLAG);
	obj_cgroup_put(objcg);

	/*
	 * Some kernel allocations can happen after this point,
	 * but let's ignore them. It can be done without any synchronization
	 * because it's always performed on the current task, so does
	 * current_objcg_update().
	 */
	task->objcg = NULL;
}

#ifdef CONFIG_LRU_GEN
/*
 * mem_cgroup_lru_gen_attach() - 任务迁组时迁移首个进程组 leader 拥有的 mm。
 * task_lock 下复核 mm->owner，成立才通知 MGLRU；无线程 leader 或 owner 已变化
 * 则为空操作。页 charge 不在此迁移，只调整代际 LRU 的 memcg 关联。
 */
static void mem_cgroup_lru_gen_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;

	/* find the first leader if there is any */
	cgroup_taskset_for_each_leader(task, css, tset)
		break;

	if (!task)
		return;

	task_lock(task);
	if (task->mm && READ_ONCE(task->mm->owner) == task)
		lru_gen_migrate_mm(task->mm);
	task_unlock(task);
}
#else
/* 未启用 MGLRU 时任务 attach 不需要代际状态迁移。 */
static void mem_cgroup_lru_gen_attach(struct cgroup_taskset *tset) {}
#endif /* CONFIG_LRU_GEN */

/*
 * mem_cgroup_kmem_attach() - 标记迁组任务在下一次对象分配时刷新 objcg 缓存。
 * 对 taskset 每个任务原子设置 UPDATE_BIT，不在 attach 锁路径立即换引用；实际
 * 更新由任务自己的 current_objcg_update() 完成，避免跨任务修改竞态。
 */
static void mem_cgroup_kmem_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;

	cgroup_taskset_for_each(task, css, tset) {
		/* atomically set the update bit */
		set_bit(CURRENT_OBJCG_UPDATE_BIT, (unsigned long *)&task->objcg);
	}
}

/*
 * 【任务 attach 与 cgroupfs 控制面】
 *
 * attach 不迁移任务已经分配的匿名页；它改变以后从 current/mm 取得的计费
 * 身份，并通知 kmem、MGLRU 等子路径。随后 memory_files[] 把 kernfs 读写
 * 映射到 min/low/high/max、peak、events、stat、oom.group 等 helper。
 * show 路径输出快照；write 路径解析 PAGE_COUNTER_MAX 或字节数，在缩限时
 * drain/reclaim 并触发事件。文件表的 private 字段是回调策略参数，不是
 * 用户可控指针。
 */
static void mem_cgroup_attach(struct cgroup_taskset *tset)
{
	mem_cgroup_lru_gen_attach(tset);
	mem_cgroup_kmem_attach(tset);
}

/*
 * seq_puts_memcg_tunable() - 按 cgroup ABI 输出页计数型 tunable。
 * @m 是借用 seq_file，@value 为基页数或 PAGE_COUNTER_MAX；后者打印 "max"，
 * 其余安全换算为字节。返回 seq_file 写入状态，不修改 memcg。
 */
static int seq_puts_memcg_tunable(struct seq_file *m, unsigned long value)
{
	if (value == PAGE_COUNTER_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)value * PAGE_SIZE);

	return 0;
}

/*
 * memory_current_read() - 实现 memory.current 的无副作用快照读取。
 * @css 指定 memcg，@cft 仅满足统一回调签名；返回 memory page_counter 当前
 * usage 的字节数。层级 counter 自身提供并发读取语义，本函数不强制 flush。
 */
static u64 memory_current_read(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	return (u64)page_counter_read(&memcg->memory) * PAGE_SIZE;
}

#define OFP_PEAK_UNSET (((-1UL)))

/*
 * peak_show() - 输出 @pc 面向当前 fd 的峰值字节数。
 * fd 从未写过时读全局 watermark；写过后取该 fd 基线以来的 local watermark
 * 与保存值最大者。READ_ONCE 容忍并发更新，成功返回 0。
 */
static int peak_show(struct seq_file *sf, void *v, struct page_counter *pc)
{
	struct cgroup_of_peak *ofp = of_peak(sf->private);
	u64 fd_peak = READ_ONCE(ofp->value), peak;

	/* User wants global or local peak? */
	if (fd_peak == OFP_PEAK_UNSET)
		peak = pc->watermark;
	else
		peak = max(fd_peak, READ_ONCE(pc->local_watermark));

	seq_printf(sf, "%llu\n", peak * PAGE_SIZE);
	return 0;
}

/* memory_peak_show() - 将 memory.peak 的 seq 回调绑定到 memory counter。 */
static int memory_peak_show(struct seq_file *sf, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(sf));

	return peak_show(sf, v, &memcg->memory);
}

/* peak_open() - 把新打开 fd 标为全局峰值视图，尚未加入重置观察者链表。 */
static int peak_open(struct kernfs_open_file *of)
{
	struct cgroup_of_peak *ofp = of_peak(of);

	ofp->value = OFP_PEAK_UNSET;
	return 0;
}

/*
 * peak_release() - 若该 fd 曾重置峰值，则在 peaks_lock 下摘除观察者节点。
 * 从未写入的 fd 未入链，走无锁快路；返回后 kernfs 可释放其私有上下文。
 */
static void peak_release(struct kernfs_open_file *of)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct cgroup_of_peak *ofp = of_peak(of);

	if (ofp->value == OFP_PEAK_UNSET) {
		/* fast path (no writes on this fd) */
		return;
	}
	spin_lock(&memcg->peaks_lock);
	list_del(&ofp->list);
	spin_unlock(&memcg->peaks_lock);
}

/*
 * peak_write() - 把当前 @pc usage 设为本 fd 及所有观察者的新峰值基线。
 * 输入文本内容按 ABI 被忽略，写动作本身即“重置”；首次写把 fd 加入 @watchers。
 * peaks_lock 串行更新 local watermark/观察者，成功总返回 @nbytes。
 */
static ssize_t peak_write(struct kernfs_open_file *of, char *buf, size_t nbytes,
			  loff_t off, struct page_counter *pc,
			  struct list_head *watchers)
{
	unsigned long usage;
	struct cgroup_of_peak *peer_ctx;
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct cgroup_of_peak *ofp = of_peak(of);

	spin_lock(&memcg->peaks_lock);

	usage = page_counter_read(pc);
	WRITE_ONCE(pc->local_watermark, usage);

	list_for_each_entry(peer_ctx, watchers, list)
		if (usage > peer_ctx->value)
			WRITE_ONCE(peer_ctx->value, usage);

	/* initial write, register watcher */
	if (ofp->value == OFP_PEAK_UNSET)
		list_add(&ofp->list, watchers);

	WRITE_ONCE(ofp->value, usage);
	spin_unlock(&memcg->peaks_lock);

	return nbytes;
}

/* memory_peak_write() - 用通用 peak_write() 重置 memory counter 的 fd 局部峰值。 */
static ssize_t memory_peak_write(struct kernfs_open_file *of, char *buf,
				 size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));

	return peak_write(of, buf, nbytes, off, &memcg->memory,
			  &memcg->memory_peaks);
}

#undef OFP_PEAK_UNSET

/* memory_min_show() - 输出 memory.min 硬保护值，PAGE_COUNTER_MAX 显示为 "max"。 */
static int memory_min_show(struct seq_file *m, void *v)
{
	return seq_puts_memcg_tunable(m,
		READ_ONCE(mem_cgroup_from_seq(m)->memory.min));
}

/*
 * memory_min_write() - 解析并设置本组的绝对最小保护。
 * 成功返回 @nbytes，错误保持旧值；page_counter helper 同步更新祖先保护传播
 * 所需字段，但不会立即触发回收。
 */
static ssize_t memory_min_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long min;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &min);
	if (err)
		return err;

	page_counter_set_min(&memcg->memory, min);

	return nbytes;
}

/* memory_low_show() - 输出 memory.low 尽力保护值，单位由通用 helper 转为字节。 */
static int memory_low_show(struct seq_file *m, void *v)
{
	return seq_puts_memcg_tunable(m,
		READ_ONCE(mem_cgroup_from_seq(m)->memory.low));
}

/*
 * memory_low_write() - 解析容量/"max" 并设置可被超越的低保护阈值。
 * 解析失败返回负 errno；成功返回 @nbytes。新值影响后续 reclaim 保护计算，
 * 不迁移页面也不保证并发回收立刻观察到。
 */
static ssize_t memory_low_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long low;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &low);
	if (err)
		return err;

	page_counter_set_low(&memcg->memory, low);

	return nbytes;
}

/* memory_high_show() 输出当前 memory.high；@v 未使用，返回 seq 写入状态。 */
static int memory_high_show(struct seq_file *m, void *v)
{
	return seq_puts_memcg_tunable(m,
		READ_ONCE(mem_cgroup_from_seq(m)->memory.high));
}

/*
 * memory_high_write() - 更新 memory.high，并尽力把现有 usage 回收到新水位。
 *
 * @of 确定目标 memcg，@buf/@nbytes 是用户输入，@off 不改变语义。解析失败
 * 返回 errno；成功先发布 high，再循环 drain stock 与直接回收。信号、无进展
 * 或重试耗尽会结束尽力回收，但写入仍返回 nbytes；最后同步 writeback 域大小。
 * 函数可睡眠，O_NONBLOCK 跳过同步回收，不把 high 当成硬失败边界。
 */
static ssize_t memory_high_write(struct kernfs_open_file *of,
				 char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned int nr_retries = MAX_RECLAIM_RETRIES;
	bool drained = false;
	unsigned long high;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &high);
	if (err)
		return err;

	page_counter_set_high(&memcg->memory, high);

	if (of->file->f_flags & O_NONBLOCK)
		goto out;

	for (;;) {
		unsigned long nr_pages = page_counter_read(&memcg->memory);
		unsigned long reclaimed;

		if (nr_pages <= high)
			break;

		if (signal_pending(current))
			break;

		if (!drained) {
			drain_all_stock(memcg);
			drained = true;
			continue;
		}

		reclaimed = try_to_free_mem_cgroup_pages(memcg, nr_pages - high,
					GFP_KERNEL, MEMCG_RECLAIM_MAY_SWAP, NULL);

		if (!reclaimed && !nr_retries--)
			break;
	}
out:
	memcg_wb_domain_size_changed(memcg);
	return nbytes;
}

/* memory_max_show() 输出 memory.max，PAGE_COUNTER_MAX 按 ABI 显示为 "max"。 */
static int memory_max_show(struct seq_file *m, void *v)
{
	return seq_puts_memcg_tunable(m,
		READ_ONCE(mem_cgroup_from_seq(m)->memory.max));
}

/*
 * memory_max_write() - 发布新的硬上限并尝试把存量 usage 压回限额内。
 *
 * @buf 可为字节数或 "max"；解析失败返回 errno。xchg() 先让新分配立即看到
 * 上限，再对阻塞写执行一次 stock drain 和多轮 memcg reclaim。O_NONBLOCK
 * 只发布不等待；信号或无进展可提前结束，超额后续由 charge/OOM 协议处理。
 * 返回 nbytes 表示配置已接受，不保证返回瞬间 usage 已降到 max。
 */
static ssize_t memory_max_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned int nr_reclaims = MAX_RECLAIM_RETRIES;
	bool drained = false;
	unsigned long max;
	int err;

	/* 阶段 1：只在解析完整成功后发布新值，错误输入不会改变现有上限。 */
	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &max);
	if (err)
		return err;

	/*
	 * xchg 是策略提交点：从这里起并发 charge 已按新 max 判断。旧 usage 可以
	 * 暂时高于新值，下面的同步回收负责收敛，而不是回滚已经发布的配置。
	 */
	xchg(&memcg->memory.max, max);

	if (of->file->f_flags & O_NONBLOCK)
		goto out;

	/*
	 * 阶段 2：先观察 usage；第一次越限先 drain 所有 CPU 预充余额，避免把
	 * 已计费但未实际消费的 stock 误当成必须扫描 LRU 的存量页面。
	 */
	for (;;) {
		unsigned long nr_pages = page_counter_read(&memcg->memory);

		if (nr_pages <= max)
			break;

		if (signal_pending(current))
			break;

		if (!drained) {
			drain_all_stock(memcg);
			drained = true;
			continue;
		}

		/* 阶段 3：按当前超额页数直接回收；只有无进展才消耗有限重试预算。 */
		if (nr_reclaims) {
			if (!try_to_free_mem_cgroup_pages(memcg, nr_pages - max,
					GFP_KERNEL, MEMCG_RECLAIM_MAY_SWAP, NULL))
				nr_reclaims--;
			continue;
		}

		/*
		 * 阶段 4：普通回收耗尽后进入 memcg OOM。OOM killer 若能推进，循环
		 * 重新观察 usage；若不能选择受害者则结束，保留已发布的新 max。
		 */
		memcg_memory_event(memcg, MEMCG_OOM);
		if (!mem_cgroup_out_of_memory(memcg, GFP_KERNEL, 0))
			break;
		cond_resched();
	}
out:
	memcg_wb_domain_size_changed(memcg);
	return nbytes;
}

/*
 * Note: don't forget to update the 'samples/cgroup/memcg_event_listener'
 * if any new events become available.
 */
/*
 * memory.events ABI 与 samples/cgroup/memcg_event_listener 示例绑定；新增字段时
 * 必须同步示例，避免用户空间遗漏。__memory_events_show() 只格式化调用者
 * 传入的原子数组，按固定名字输出当前快照，不重置计数、不触发 rstat flush。
 */
static void __memory_events_show(struct seq_file *m, atomic_long_t *events)
{
	seq_printf(m, "low %lu\n", atomic_long_read(&events[MEMCG_LOW]));
	seq_printf(m, "high %lu\n", atomic_long_read(&events[MEMCG_HIGH]));
	seq_printf(m, "max %lu\n", atomic_long_read(&events[MEMCG_MAX]));
	seq_printf(m, "oom %lu\n", atomic_long_read(&events[MEMCG_OOM]));
	seq_printf(m, "oom_kill %lu\n",
		   atomic_long_read(&events[MEMCG_OOM_KILL]));
	seq_printf(m, "oom_group_kill %lu\n",
		   atomic_long_read(&events[MEMCG_OOM_GROUP_KILL]));
	seq_printf(m, "sock_throttled %lu\n",
		   atomic_long_read(&events[MEMCG_SOCK_THROTTLED]));
}

/* memory_events_show() 输出包含后代传播的事件数组；@v 未使用，成功返回 0。 */
static int memory_events_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);

	__memory_events_show(m, memcg->memory_events);
	return 0;
}

/* memory_events_local_show() 只输出本组事件，不包含子 cgroup 的传播值。 */
static int memory_events_local_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);

	__memory_events_show(m, memcg->memory_events_local);
	return 0;
}

/*
 * memory_stat_show() - 生成 memory.stat 的一致格式文本。
 * @m 隐含目标 memcg，@v 未使用。函数可睡眠分配 4K 临时缓冲；失败返回
 * -ENOMEM，成功由 memory_stat_format() 刷新/格式化后写入 seq_file 并释放。
 */
int memory_stat_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);
	char *buf = kmalloc(SEQ_BUF_SIZE, GFP_KERNEL);
	struct seq_buf s;

	if (!buf)
		return -ENOMEM;
	seq_buf_init(&s, buf, SEQ_BUF_SIZE);
	memory_stat_format(memcg, &s);
	seq_puts(m, buf);
	kfree(buf);
	return 0;
}

#ifdef CONFIG_NUMA
/* 把 lruvec 项目的内部计数乘以对应输出单位，供 memory.numa_stat 使用。 */
static inline unsigned long lruvec_page_state_output(struct lruvec *lruvec,
						     int item)
{
	return lruvec_page_state(lruvec, item) *
		memcg_page_state_output_unit(item);
}

/*
 * memory_numa_stat_show() - 按统计名输出每个有内存 NUMA 节点的 memcg 值。
 * 先强制刷新目标子树，再只选择 node_stat_item；每行以 N<nid>=bytes 展开。
 * @v 未使用，函数可因 rstat flush 产生延迟，成功返回 0。
 */
static int memory_numa_stat_show(struct seq_file *m, void *v)
{
	int i;
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);

	mem_cgroup_flush_stats(memcg);

	for (i = 0; i < ARRAY_SIZE(memory_stats); i++) {
		int nid;

		if (memory_stats[i].idx >= NR_VM_NODE_STAT_ITEMS)
			continue;

		seq_printf(m, "%s", memory_stats[i].name);
		for_each_node_state(nid, N_MEMORY) {
			u64 size;
			struct lruvec *lruvec;

			lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
			size = lruvec_page_state_output(lruvec,
							memory_stats[i].idx);
			seq_printf(m, " N%d=%llu", nid, size);
		}
		seq_putc(m, '\n');
	}

	return 0;
}
#endif

/* memory_oom_group_show() - 以 0/1 输出 OOM 时是否把该 memcg 作为整体杀死。 */
static int memory_oom_group_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);

	seq_printf(m, "%d\n", READ_ONCE(memcg->oom_group));

	return 0;
}

/*
 * memory_oom_group_write() - 设置 memory.oom.group 布尔策略。
 * 只接受可解析的 0 或 1，错误不改旧值；WRITE_ONCE 发布给并发 OOM 路径，
 * 成功返回 @nbytes。该开关不主动触发 OOM。
 */
static ssize_t memory_oom_group_write(struct kernfs_open_file *of,
				      char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int ret, oom_group;

	buf = strstrip(buf);
	if (!buf)
		return -EINVAL;

	ret = kstrtoint(buf, 0, &oom_group);
	if (ret)
		return ret;

	if (oom_group != 0 && oom_group != 1)
		return -EINVAL;

	WRITE_ONCE(memcg->oom_group, oom_group);

	return nbytes;
}

/*
 * memory_reclaim() - 实现 memory.reclaim 的用户主动回收请求。
 * @buf 由 user_proactive_reclaim() 解析目标字节数及可选参数，并在当前 memcg
 * 执行同步回收；失败透传 errno，成功返回 @nbytes。它是尽力接口，成功不等于
 * 精确回收到请求量。
 */
static ssize_t memory_reclaim(struct kernfs_open_file *of, char *buf,
			      size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int ret;

	ret = user_proactive_reclaim(buf, memcg, NULL);
	if (ret)
		return ret;

	return nbytes;
}

/*
 * memory_files[] 是 cgroup v2 ABI 的声明式路由表：name 决定文件名，
 * seq_show/read_u64/write 分别决定读写入口，flags 约束 namespace/层级
 * 可见性。表在子系统注册后长期只读；更改名字或语义会影响用户空间 ABI。
 */
static struct cftype memory_files[] = {
	{
		.name = "current",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = memory_current_read,
	},
	{
		.name = "peak",
		.flags = CFTYPE_NOT_ON_ROOT,
		.open = peak_open,
		.release = peak_release,
		.seq_show = memory_peak_show,
		.write = memory_peak_write,
	},
	{
		.name = "min",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_min_show,
		.write = memory_min_write,
	},
	{
		.name = "low",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_low_show,
		.write = memory_low_write,
	},
	{
		.name = "high",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_high_show,
		.write = memory_high_write,
	},
	{
		.name = "max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memory_max_show,
		.write = memory_max_write,
	},
	{
		.name = "events",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, events_file),
		.seq_show = memory_events_show,
	},
	{
		.name = "events.local",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, events_local_file),
		.seq_show = memory_events_local_show,
	},
	{
		.name = "stat",
		.seq_show = memory_stat_show,
	},
#ifdef CONFIG_NUMA
	{
		.name = "numa_stat",
		.seq_show = memory_numa_stat_show,
	},
#endif
	{
		.name = "oom.group",
		.flags = CFTYPE_NOT_ON_ROOT | CFTYPE_NS_DELEGATABLE,
		.seq_show = memory_oom_group_show,
		.write = memory_oom_group_write,
	},
	{
		.name = "reclaim",
		.flags = CFTYPE_NS_DELEGATABLE,
		.write = memory_reclaim,
	},
	{ }	/* terminate */
};

/*
 * memory_cgrp_subsys 把上述生命周期、attach、rstat 与文件表回调交给 cgroup
 * core。early_init 保证根 memcg 在普通内存分配广泛发生前可用；dfl_cftypes
 * 是 v2 文件，legacy_cftypes 在 CONFIG_MEMCG_V1 下另由 memcontrol-v1.c
 * 提供。回调间的先后由 cgroup core 契约保证。
 */
struct cgroup_subsys memory_cgrp_subsys = {
	.css_alloc = mem_cgroup_css_alloc,
	.css_online = mem_cgroup_css_online,
	.css_offline = mem_cgroup_css_offline,
	.css_released = mem_cgroup_css_released,
	.css_free = mem_cgroup_css_free,
	.css_reset = mem_cgroup_css_reset,
	.css_rstat_flush = mem_cgroup_css_rstat_flush,
	.attach = mem_cgroup_attach,
	.fork = mem_cgroup_fork,
	.exit = mem_cgroup_exit,
	.dfl_cftypes = memory_files,
#ifdef CONFIG_MEMCG_V1
	.legacy_cftypes = mem_cgroup_legacy_files,
#endif
	.early_init = 0,
};

/**
 * mem_cgroup_calculate_protection - check if memory consumption is in the normal range
 * @root: the top ancestor of the sub-tree being checked
 * @memcg: the memory cgroup to check
 *
 * WARNING: This function is not stateless! It can only be used as part
 *          of a top-down tree iteration, not for isolated queries.
 */
/*
 * mem_cgroup_calculate_protection() - 在自顶向下回收遍历中计算 min/low 有效保护。
 * @root 为本轮回收域顶点，可为 NULL；@memcg 是当前节点。函数读取父节点已
 * 算出的分摊结果并写当前 page_counter 派生字段，因此不是无状态查询，脱离
 * 树遍历单独调用会得到错误保护量。返回无直接值，不取得 css 引用。
 */
void mem_cgroup_calculate_protection(struct mem_cgroup *root,
				     struct mem_cgroup *memcg)
{
	bool recursive_protection =
		cgrp_dfl_root.flags & CGRP_ROOT_MEMORY_RECURSIVE_PROT;

	if (mem_cgroup_disabled())
		return;

	if (!root)
		root = root_mem_cgroup;

	page_counter_calculate_protection(&root->memory, &memcg->memory, recursive_protection);
}

/*
 * charge_memcg() - 对指定 @memcg 执行 folio 的“预留额度 + 发布归属”。
 * @folio 尚未计费且由调用者独占，@memcg 为稳定借用，@gfp 决定回收能力。
 * 根 objcg 跳过层级限额；非根先 try_charge，失败 put objcg 并返回 errno。
 * 成功时 objcg 引用转移给 folio，v1 同步提交，返回 0。
 */
static int charge_memcg(struct folio *folio, struct mem_cgroup *memcg,
			gfp_t gfp)
{
	int ret = 0;
	struct obj_cgroup *objcg;

	objcg = get_obj_cgroup_from_memcg(memcg);
	/* Do not account at the root objcg level. */
	/* 根 objcg 由全局内存负责，不重复收取 memcg 层级额度。 */
	if (!obj_cgroup_is_root(objcg))
		ret = try_charge_memcg(memcg, gfp, folio_nr_pages(folio));
	if (ret) {
		obj_cgroup_put(objcg);
		return ret;
	}
	commit_charge(folio, objcg);
	memcg1_commit_charge(folio, memcg);

	return ret;
}

/*
 * 【用户页 charge/commit/uncharge】
 *
 * folio 首次成为匿名页或进入 page cache 时在此确定归属。目标通常来自 mm
 * owner，也可由 active_memcg 覆盖；try_charge_memcg() 先预留 page_counter
 * 额度，commit_charge() 再把持有引用的 objcg 写进 folio 并更新统计/LRU
 * 语义。若提交前失败，预留必须立即 uncharge；提交后释放责任转移给 folio，
 * 只能由迁移或 uncharge 路径解除。批量 uncharge_gather 按 memcg 聚合，减少
 * 每页原子操作，同时保持 swap、anon/file 与 page_counter 账目成对。
 */
/*
 * __mem_cgroup_charge() - 按 @mm/active_memcg 选择归属并为 @folio 计费。
 * @folio 是未发布的新 folio，@mm 可为 NULL，@gfp 控制慢路径。函数先取得
 * 带 css 引用 memcg，调用 charge_memcg() 后无条件 put。返回 0 或 charge
 * errno；失败时 folio 不持有 objcg，调用者仍拥有全部释放责任。
 */
int __mem_cgroup_charge(struct folio *folio, struct mm_struct *mm, gfp_t gfp)
{
	struct mem_cgroup *memcg;
	int ret;

	memcg = get_mem_cgroup_from_mm(mm);
	ret = charge_memcg(folio, memcg, gfp);
	css_put(&memcg->css);

	return ret;
}

/**
 * mem_cgroup_charge_hugetlb - charge the memcg for a hugetlb folio
 * @folio: folio being charged
 * @gfp: reclaim mode
 *
 * This function is called when allocating a huge page folio, after the page has
 * already been obtained and charged to the appropriate hugetlb cgroup
 * controller (if it is enabled).
 *
 * Returns ENOMEM if the memcg is already full.
 * Returns 0 if either the charge was successful, or if we skip the charging.
 */
/*
 * mem_cgroup_charge_hugetlb() - 在 hugetlb controller 计费后补做 memory 计费。
 * @folio 已分配且由调用者稳定，@gfp 控制回收；仅 v2 且启用 hugetlb memory
 * accounting 时实际 charge。成功或按配置跳过返回 0，memcg 满返回 -ENOMEM；
 * 所有出口都归还 current memcg 引用，失败不改变 folio 归属。
 */
int mem_cgroup_charge_hugetlb(struct folio *folio, gfp_t gfp)
{
	struct mem_cgroup *memcg = get_mem_cgroup_from_current();
	int ret = 0;

	/*
	 * Even memcg does not account for hugetlb, we still want to update
	 * system-level stats via lruvec_stat_mod_folio. Return 0, and skip
	 * charging the memcg.
	 */
	if (mem_cgroup_disabled() || !memcg_accounts_hugetlb() ||
		!memcg || !cgroup_subsys_on_dfl(memory_cgrp_subsys))
		goto out;

	if (charge_memcg(folio, memcg, gfp))
		ret = -ENOMEM;

out:
	mem_cgroup_put(memcg);
	return ret;
}

/**
 * mem_cgroup_swapin_charge_folio - Charge a newly allocated folio for swapin.
 * @folio: the folio to charge
 * @id: memory cgroup id
 * @mm: mm context of the victim
 * @gfp: reclaim mode
 *
 * This function charges a folio allocated for swapin. Please call this before
 * adding the folio to the swapcache.
 *
 * Returns 0 on success. Otherwise, an error code is returned.
 */
/*
 * mem_cgroup_swapin_charge_folio() - 在 folio 加入 swapcache 前恢复其 memcg 账目。
 *
 * @folio 是尚未发布到 swapcache 的新页，@id 是 swap entry 保存的私有 memcg
 * ID，@mm 是失效 ID 的回退归属，@gfp 控制回收能力。RCU 下先按 ID 查找并
 * css_tryget_online()；组已离线则改取 @mm 当前组。成功返回 0 且 folio 持有
 * objcg 归属；失败返回 charge errno，folio 未提交，调用者负责释放/回退。
 */
int mem_cgroup_swapin_charge_folio(struct folio *folio, unsigned short id,
				   struct mm_struct *mm, gfp_t gfp)
{
	struct mem_cgroup *memcg;
	int ret;

	if (mem_cgroup_disabled())
		return 0;

	rcu_read_lock();
	memcg = mem_cgroup_from_private_id(id);
	if (!memcg || !css_tryget_online(&memcg->css))
		memcg = get_mem_cgroup_from_mm(mm);
	rcu_read_unlock();

	ret = charge_memcg(folio, memcg, gfp);

	css_put(&memcg->css);
	return ret;
}

/*
 * uncharge_gather 是一次批量销账的栈上累加器。memcg/pgdat 标识当前可合并
 * 批次，nr_pages 是要归还 page_counter 的基页数，各字段保存需同步扣减的
 * anon/file/hugetlb 等统计。切换 memcg 或批次结束时统一 flush；结构本身
 * 不持久发布，但批次期间持有必要 objcg/memcg 引用。
 */
struct uncharge_gather {
	struct obj_cgroup *objcg;
	unsigned long nr_memory;
	unsigned long pgpgout;
	unsigned long nr_kmem;
	int nid;
};

/* 初始化栈上批处理器为“尚未选择 objcg、累计量全零”；不持有任何引用。 */
static inline void uncharge_gather_clear(struct uncharge_gather *ug)
{
	memset(ug, 0, sizeof(*ug));
}

/*
 * uncharge_batch() - 一次性提交 @ug 收集的 page_counter、kmem 与 v1 销账。
 * @ug 必须持有 objcg 批次引用。RCU 下按 objcg 当前重挂身份选择 memcg，完成
 * 所有统计后 put 批次引用；返回后 @ug 内容失效，调用者需 clear 后再复用。
 */
static void uncharge_batch(const struct uncharge_gather *ug)
{
	struct mem_cgroup *memcg;

	rcu_read_lock();
	/*
	 * objcg->memcg 可在收集期间被 offline 重挂，flush 时在 RCU 下读取当前
	 * 所属组，确保整批 page_counter、kmem 与 v1 统计记到同一身份。
	 */
	memcg = obj_cgroup_memcg(ug->objcg);
	if (ug->nr_memory) {
		memcg_uncharge(memcg, ug->nr_memory);
		if (ug->nr_kmem) {
			mod_memcg_state(memcg, MEMCG_KMEM, -ug->nr_kmem);
			memcg1_account_kmem(memcg, -ug->nr_kmem);
		}
		memcg1_oom_recover(memcg);
	}

	memcg1_uncharge_batch(memcg, ug->pgpgout, ug->nr_memory, ug->nid);
	rcu_read_unlock();

	/* drop reference from uncharge_folio */
	/* 这份引用只保护批次跨多个 folio 存活，与每个 folio 自身引用相互独立。 */
	obj_cgroup_put(ug->objcg);
}

/*
 * uncharge_folio() - 从独占 folio 摘下 objcg，并把销账量合并进 @ug。
 * @folio 必须不在 LRU 且不会被并发检查归属；@ug 是输入输出批处理器。objcg
 * 改变时先提交旧批次；函数清 memcg_data 并释放 folio 持有引用，但把一份
 * 批次引用保留到 uncharge_batch()，返回无直接值。
 */
static void uncharge_folio(struct folio *folio, struct uncharge_gather *ug)
{
	long nr_pages;
	struct obj_cgroup *objcg;

	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	/*
	 * Nobody should be changing or seriously looking at
	 * folio objcg at this point, we have fully exclusive
	 * access to the folio.
	 */
	/*
	 * 到达此处时 folio 已从 LRU/并发可见结构隔离，因而无需页锁外的额外
	 * 同步即可读取并清除 objcg；若仍有人查归属，这里会产生重复销账或 UAF。
	 */
	objcg = folio_objcg(folio);
	if (!objcg)
		return;

	if (ug->objcg != objcg) {
		/* objcg 改变即提交上一批，避免把不同层级的页数合并后一次销错组。 */
		if (ug->objcg) {
			uncharge_batch(ug);
			uncharge_gather_clear(ug);
		}
		ug->objcg = objcg;
		ug->nid = folio_nid(folio);

		/* pairs with obj_cgroup_put in uncharge_batch */
		/* 该批次引用最终由 uncharge_batch() 中的 obj_cgroup_put() 配对释放。 */
		obj_cgroup_get(objcg);
	}

	nr_pages = folio_nr_pages(folio);

	if (folio_memcg_kmem(folio)) {
		ug->nr_memory += nr_pages;
		ug->nr_kmem += nr_pages;
	} else {
		/* LRU pages aren't accounted at the root level */
		/* LRU 页在根组不做额外 page_counter 计费，避免与全局计数重复。 */
		if (!obj_cgroup_is_root(objcg))
			ug->nr_memory += nr_pages;
		ug->pgpgout++;

		WARN_ON_ONCE(folio_unqueue_deferred_split(folio));
	}

	folio->memcg_data = 0;
	/*
	 * 清零后 folio 不再拥有 objcg；紧随的 put 对称 commit_charge() 转移的
	 * 引用。批处理自身另持一份引用，直到 uncharge_batch() 完成才释放。
	 */
	obj_cgroup_put(objcg);
}

/*
 * __mem_cgroup_uncharge() - 解除单个 folio 的已提交 memcg 归属。
 *
 * @folio 必须已从会再次计费/查找的外部结构中隔离，调用者保证不会与迁移
 * 或重复 uncharge 并发。函数清 folio memcg_data、扣统计、归还基页额度并
 * put objcg；无直接返回值。清指针是 ownership 提交边界，之后 free 路径
 * 不得再次依据旧归属销账。
 */
void __mem_cgroup_uncharge(struct folio *folio)
{
	struct uncharge_gather ug;

	/* Don't touch folio->lru of any random page, pre-check: */
	/* 先检查计费位，避免为任意未计费页触碰 folio->lru 等联合字段。 */
	if (!folio_memcg_charged(folio))
		return;

	uncharge_gather_clear(&ug);
	uncharge_folio(folio, &ug);
	uncharge_batch(&ug);
}

/*
 * __mem_cgroup_uncharge_folios() - 批量解除 @folios 中所有 folio 的 memcg 归属。
 * batch 由调用者稳定；相邻同 objcg folio 合并为一次原子销账，身份切换时
 * 自动 flush。返回无直接值；完成后每个已计费 folio 的 memcg_data 已清零。
 */
void __mem_cgroup_uncharge_folios(struct folio_batch *folios)
{
	struct uncharge_gather ug;
	unsigned int i;

	uncharge_gather_clear(&ug);
	for (i = 0; i < folios->nr; i++)
		uncharge_folio(folios->folios[i], &ug);
	if (ug.objcg)
		uncharge_batch(&ug);
}

/**
 * mem_cgroup_replace_folio - Charge a folio's replacement.
 * @old: Currently circulating folio.
 * @new: Replacement folio.
 *
 * Charge @new as a replacement folio for @old. @old will
 * be uncharged upon free.
 *
 * Both folios must be locked, @new->mapping must be set up.
 */
/*
 * mem_cgroup_replace_folio() - 为即将替换 @old 的 @new 复制归属并暂时双计费。
 * 两个 folio 均须锁定、类型和页数一致；@new 已计费则无需操作。函数强制为
 * new 增加同量 page_counter 并取得 objcg 引用，old 稍后 free 时再销旧账，
 * 因而替换窗口可短暂双计费但不会在提交失败后留下半归属。返回无直接值。
 */
void mem_cgroup_replace_folio(struct folio *old, struct folio *new)
{
	struct mem_cgroup *memcg;
	struct obj_cgroup *objcg;
	long nr_pages = folio_nr_pages(new);

	VM_BUG_ON_FOLIO(!folio_test_locked(old), old);
	VM_BUG_ON_FOLIO(!folio_test_locked(new), new);
	VM_BUG_ON_FOLIO(folio_test_anon(old) != folio_test_anon(new), new);
	VM_BUG_ON_FOLIO(folio_nr_pages(old) != nr_pages, new);

	if (mem_cgroup_disabled())
		return;

	/* Page cache replacement: new folio already charged? */
	/* page cache 替换中若新 folio 已有归属，不能再次复制 charge。 */
	if (folio_memcg_charged(new))
		return;

	objcg = folio_objcg(old);
	VM_WARN_ON_ONCE_FOLIO(!objcg, old);
	if (!objcg)
		return;

	rcu_read_lock();
	memcg = obj_cgroup_memcg(objcg);
	/* Force-charge the new page. The old one will be freed soon */
	/* 强制为新页计费；旧页很快释放，短暂双计费是替换协议的一部分。 */
	if (!obj_cgroup_is_root(objcg)) {
		page_counter_charge(&memcg->memory, nr_pages);
		if (do_memsw_account())
			page_counter_charge(&memcg->memsw, nr_pages);
	}

	obj_cgroup_get(objcg);
	commit_charge(new, objcg);
	memcg1_commit_charge(new, memcg);
	rcu_read_unlock();
}

/**
 * mem_cgroup_migrate - Transfer the memcg data from the old to the new folio.
 * @old: Currently circulating folio.
 * @new: Replacement folio.
 *
 * Transfer the memcg data from the old folio to the new folio for migration.
 * The old folio's data info will be cleared. Note that the memory counters
 * will remain unchanged throughout the process.
 *
 * Both folios must be locked, @new->mapping must be set up.
 */
/*
 * mem_cgroup_migrate() - 把 @old 的既有 charge 与 objcg 引用原子地转交 @new。
 * 两者均锁定、页数和匿名属性相同，old 已离开 LRU。page_counter 不增不减：
 * commit_charge(new) 接收原引用，随后清 old->memcg_data，故 ownership 只移动
 * 不复制。无 objcg 的合法 hugetlb 场景直接返回。
 */
void mem_cgroup_migrate(struct folio *old, struct folio *new)
{
	struct obj_cgroup *objcg;

	VM_BUG_ON_FOLIO(!folio_test_locked(old), old);
	VM_BUG_ON_FOLIO(!folio_test_locked(new), new);
	VM_BUG_ON_FOLIO(folio_test_anon(old) != folio_test_anon(new), new);
	VM_BUG_ON_FOLIO(folio_nr_pages(old) != folio_nr_pages(new), new);
	VM_BUG_ON_FOLIO(folio_test_lru(old), old);

	if (mem_cgroup_disabled())
		return;

	objcg = folio_objcg(old);
	/*
	 * Note that it is normal to see !objcg for a hugetlb folio.
	 * For e.g, it could have been allocated when memory_hugetlb_accounting
	 * was not selected.
	 */
	VM_WARN_ON_ONCE_FOLIO(!folio_test_hugetlb(old) && !objcg, old);
	if (!objcg)
		return;

	/* Transfer the charge and the objcg ref */
	/* 把 charge 身份及其 objcg 引用一并转交给新 folio。 */
	commit_charge(new, objcg);

	/* Warning should never happen, so don't worry about refcount non-0 */
	/* 该告警理论上不可达，不必为其额外处理仍非零的引用计数。 */
	WARN_ON_ONCE(folio_unqueue_deferred_split(old));
	old->memcg_data = 0;
}

DEFINE_STATIC_KEY_FALSE(memcg_sockets_enabled_key);
EXPORT_SYMBOL(memcg_sockets_enabled_key);

/*
 * mem_cgroup_sk_alloc() - 在 socket 创建时把 current 的 memcg 引用绑定到 @sk。
 * 仅 static key 开启且处于任务上下文时执行，避免把中断到来的随机 current
 * 归给 socket。根组或 v1 未启用 tcp memory 时跳过；成功 css_tryget() 后
 * sk_memcg 持有引用，直到 sk_free() 对称 put。函数不睡眠、无直接返回值。
 */
void mem_cgroup_sk_alloc(struct sock *sk)
{
	struct mem_cgroup *memcg;

	if (!mem_cgroup_sockets_enabled)
		return;

	/* Do not associate the sock with unrelated interrupted task's memcg. */
	/* 中断上下文的 current 与创建 socket 的业务主体无关，绑定会造成随机归账。 */
	if (!in_task())
		return;

	rcu_read_lock();
	memcg = mem_cgroup_from_task(current);
	if (mem_cgroup_is_root(memcg))
		goto out;
	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys) && !memcg1_tcpmem_active(memcg))
		goto out;
	if (css_tryget(&memcg->css))
		sk->sk_memcg = memcg;
out:
	rcu_read_unlock();
}

/*
 * mem_cgroup_sk_free() - 释放 socket 在 alloc/inherit 阶段持有的 memcg css 引用。
 * @sk 为即将销毁或重绑定的借用 socket；无归属为空操作，返回无直接值。
 */
void mem_cgroup_sk_free(struct sock *sk)
{
	struct mem_cgroup *memcg = mem_cgroup_from_sk(sk);

	if (memcg)
		css_put(&memcg->css);
}

/*
 * mem_cgroup_sk_inherit() - 让 accept 派生的 @newsk 继承监听 @sk 的计费身份。
 * 先释放 newsk 旧引用，再对父 socket memcg css_get()，最后发布同一 sk_memcg
 * 指针；身份相同直接返回。引用 ownership 属于 newsk，free 时对称释放。
 */
void mem_cgroup_sk_inherit(const struct sock *sk, struct sock *newsk)
{
	struct mem_cgroup *memcg;

	if (sk->sk_memcg == newsk->sk_memcg)
		return;

	mem_cgroup_sk_free(newsk);

	memcg = mem_cgroup_from_sk(sk);
	if (memcg)
		css_get(&memcg->css);

	newsk->sk_memcg = sk->sk_memcg;
}

/**
 * mem_cgroup_sk_charge - charge socket memory
 * @sk: socket in memcg to charge
 * @nr_pages: number of pages to charge
 * @gfp_mask: reclaim mode
 *
 * Charges @nr_pages to @memcg. Returns %true if the charge fit within
 * @memcg's configured limit, %false if it doesn't.
 */
/*
 * mem_cgroup_sk_charge() - 为 @sk 所属 memcg 预留 @nr_pages 页 socket 内存。
 * @gfp_mask 控制回收/OOM；v1 委托 tcpmem，v2 走通用 try_charge。成功后增加
 * MEMCG_SOCK state 并返回 true；失败返回 false，未留下额度或统计。
 */
bool mem_cgroup_sk_charge(const struct sock *sk, unsigned int nr_pages,
			  gfp_t gfp_mask)
{
	struct mem_cgroup *memcg = mem_cgroup_from_sk(sk);

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return memcg1_charge_skmem(memcg, nr_pages, gfp_mask);

	if (try_charge_memcg(memcg, gfp_mask, nr_pages) == 0) {
		mod_memcg_state(memcg, MEMCG_SOCK, nr_pages);
		return true;
	}

	return false;
}

/**
 * mem_cgroup_sk_uncharge - uncharge socket memory
 * @sk: socket in memcg to uncharge
 * @nr_pages: number of pages to uncharge
 */
/*
 * mem_cgroup_sk_uncharge() - 对称归还 @sk 的 @nr_pages 页 socket 账目。
 * v1 交给 tcpmem；v2 先扣 MEMCG_SOCK 统计，再把已计费页放回 per-CPU stock，
 * 供同组下一次分配复用。返回无直接值，调用者保证页数与成功 charge 配对。
 */
void mem_cgroup_sk_uncharge(const struct sock *sk, unsigned int nr_pages)
{
	struct mem_cgroup *memcg = mem_cgroup_from_sk(sk);

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys)) {
		memcg1_uncharge_skmem(memcg, nr_pages);
		return;
	}

	mod_memcg_state(memcg, MEMCG_SOCK, -nr_pages);

	refill_stock(memcg, nr_pages);
}

/*
 * mem_cgroup_flush_workqueue() - 排空 memcg 专用异步工作队列。
 *
 * 入参：无；返回：无直接返回值。可睡眠，返回只保证 flush 边界前排入 memcg_wq
 * 的 work 已完成，不销毁队列。housekeeping_update() 在调整 unbound workqueue
 * housekeeping 掩码前调用，防止旧亲和性下尚未完成的 memcg 工作与 pool 切换
 * 交错；之后新工作仍可正常入队并采用更新后的有效 affinity。
 */
void mem_cgroup_flush_workqueue(void)
{
	flush_workqueue(memcg_wq);
}

/*
 * cgroup_memory() - 解析 cgroup.memory=nosocket,nokmem,nobpf 启动参数。
 * @s 是启动期可修改字符串，strsep() 原地切分；识别项冻结到 __ro_after_init
 * 全局开关，未知项被忽略。返回 1 表示参数已消费，只在 early boot 调用。
 */
static int __init cgroup_memory(char *s)
{
	char *token;

	while ((token = strsep(&s, ",")) != NULL) {
		if (!*token)
			continue;
		if (!strcmp(token, "nosocket"))
			cgroup_memory_nosocket = true;
		if (!strcmp(token, "nokmem"))
			cgroup_memory_nokmem = true;
		if (!strcmp(token, "nobpf"))
			cgroup_memory_nobpf = true;
	}
	return 1;
}
__setup("cgroup.memory=", cgroup_memory);

/*
 * Memory controller init before cgroup_init() initialize root_mem_cgroup.
 *
 * Some parts like memcg_hotplug_cpu_dead() have to be initialized from this
 * context because of lock dependencies (cgroup_lock -> cpu hotplug) but
 * basically everything that doesn't depend on a specific mem_cgroup structure
 * should be initialized from here.
 */
/*
 * mem_cgroup_init() - 在 cgroup 核心创建根 css 之前准备全局 memcg 基础设施。
 *
 * 此时尚无可遍历的具体 memcg，因而这里只建立与实例无关、且受锁顺序限制
 * 必须提前注册的资源：CPU hotplug 回调、异步 workqueue、每 CPU stock work、
 * mem_cgroup 及 per-node slab cache。返回 0；cache 以 SLAB_PANIC 创建，分配
 * 失败不会作为普通错误返回。后续 root_mem_cgroup 初始化依赖这些对象就绪。
 */
int __init mem_cgroup_init(void)
{
	unsigned int memcg_size;
	int cpu;

	/*
	 * Currently s32 type (can refer to struct batched_lruvec_stat) is
	 * used for per-memcg-per-cpu caching of per-node statistics. In order
	 * to work fine, we should make sure that the overfill threshold can't
	 * exceed S32_MAX / PAGE_SIZE.
	 */
	BUILD_BUG_ON(MEMCG_CHARGE_BATCH > S32_MAX / PAGE_SIZE);

	/* 先固定 CPU 下线时的 stock 清理入口，避免 cgroup_lock 与 hotplug 锁倒置。 */
	cpuhp_setup_state_nocalls(CPUHP_MM_MEMCQ_DEAD, "mm/memctrl:dead", NULL,
				  memcg_hotplug_cpu_dead);

	/* memcg_wq 承载跨 CPU drain；WQ_PERCPU 保留本地 stock 的执行语义。 */
	memcg_wq = alloc_workqueue("memcg", WQ_PERCPU, 0);
	WARN_ON(!memcg_wq);

	/* 每个可能上线的 CPU 都预置 page stock 与 obj stock 的异步排空 work。 */
	for_each_possible_cpu(cpu) {
		INIT_WORK(&per_cpu_ptr(&memcg_stock, cpu)->work,
			  drain_local_memcg_stock);
		INIT_WORK(&per_cpu_ptr(&obj_stock, cpu)->work,
			  drain_local_obj_stock);
	}

	/* nodeinfo 是按启动时节点数展开的柔性数组，故 cache 对象大小需动态计算。 */
	memcg_size = struct_size_t(struct mem_cgroup, nodeinfo, nr_node_ids);
	memcg_cachep = kmem_cache_create("mem_cgroup", memcg_size, 0,
					 SLAB_PANIC | SLAB_HWCACHE_ALIGN, NULL);

	memcg_pn_cachep = KMEM_CACHE(mem_cgroup_per_node,
				     SLAB_PANIC | SLAB_HWCACHE_ALIGN);

	return 0;
}

#ifdef CONFIG_SWAP
/**
 * __mem_cgroup_try_charge_swap - try charging swap space for a folio
 * @folio: folio being added to swap
 *
 * Try to charge @folio's memcg for the swap space at folio->swap.
 *
 * Returns 0 on success, -ENOMEM on failure.
 */
/*
 * __mem_cgroup_try_charge_swap() - 把 @folio 对应 swap slot 计入其 memcg。
 * @folio 必须已进入 swapcache，且其 swap entry 已确定；函数在 folio 仍持有
 * objcg 时调用。旧式 memsw 统一计费时无需另收 swap；v2 则先取得可跨 offline
 * 存活的 private-id 引用，再尝试层级 swap counter，最后把 ID 写入 swap_cgroup。
 * 成功返回 0；限额失败返回 -ENOMEM 并撤销 ID 引用。写入 ID 是提交点，此后
 * slot 释放必须通过 __mem_cgroup_uncharge_swap() 对称销账。
 *
 * swap 归属：
 * swapout 在释放内存 folio 之前把 memcg ID 与额度转移到 swap entry，使页
 * 不驻留 RAM 时仍计入 memory+swap 限额；swapin/slot 释放再按 ID 找回在线
 * memcg 并销账。ID 查找持有 private_id/css 引用以跨越 cgroup offline。
 * CONFIG_SWAP 关闭时整区不存在，不能据此假定所有内核都支持 swap.max。
 */
int __mem_cgroup_try_charge_swap(struct folio *folio)
{
	unsigned int nr_pages = folio_nr_pages(folio);
	struct swap_cluster_info *ci;
	struct page_counter *counter;
	struct mem_cgroup *memcg;
	struct obj_cgroup *objcg;

	if (do_memsw_account())
		return 0;

	/* folio charge 提供 swap 归属；无 objcg 的异常/兼容路径按未计费处理。 */
	objcg = folio_objcg(folio);
	VM_WARN_ON_ONCE_FOLIO(!objcg, folio);
	if (!objcg)
		return 0;

	rcu_read_lock();
	memcg = obj_cgroup_memcg(objcg);
	/* 只有 swapcache folio 才有稳定 slot；竞态退出只记失败事件，不拦截回收。 */
	if (!folio_test_swapcache(folio)) {
		memcg_memory_event(memcg, MEMCG_SWAP_FAIL);
		rcu_read_unlock();
		return 0;
	}

	memcg = mem_cgroup_private_id_get_online(memcg, nr_pages);
	/* memcg is pined by memcg ID. */
	/* 此处 memcg 已由私有 ID 引用固定，可在退出 RCU 后继续使用。 */
	rcu_read_unlock();

	/* private ID 已持有；额度失败必须先记事件，再对称放掉这批 ID 引用。 */
	if (!mem_cgroup_is_root(memcg) &&
	    !page_counter_try_charge(&memcg->swap, nr_pages, &counter)) {
		memcg_memory_event(memcg, MEMCG_SWAP_MAX);
		memcg_memory_event(memcg, MEMCG_SWAP_FAIL);
		mem_cgroup_private_id_put(memcg, nr_pages);
		return -ENOMEM;
	}
	mod_memcg_state(memcg, MEMCG_SWAP, nr_pages);

	/* 在 cluster 锁内发布 slot -> memcg ID 映射，使后续 swapin/free 能找回账主。 */
	ci = swap_cluster_get_and_lock(folio);
	__swap_cgroup_set(ci, swp_cluster_offset(folio->swap), nr_pages,
			  mem_cgroup_private_id(memcg));
	swap_cluster_unlock(ci);

	return 0;
}

/**
 * __mem_cgroup_uncharge_swap - uncharge swap space
 * @id: cgroup id to uncharge
 * @nr_pages: the amount of swap space to uncharge
 */
/*
 * __mem_cgroup_uncharge_swap() - 按 swap_cgroup 保存的 @id 释放 @nr_pages 页。
 * 调用者已从 slot 取出并清理 ID；RCU 下查找可能已 offline 但仍由 private-id
 * 引用固定的 memcg。分别归还 v1 memsw 或 v2 swap counter、扣 MEMCG_SWAP，
 * 最后释放同量 ID 引用。未知/已消失 ID 安全忽略，无直接返回值。
 */
void __mem_cgroup_uncharge_swap(unsigned short id, unsigned int nr_pages)
{
	struct mem_cgroup *memcg;

	rcu_read_lock();
	memcg = mem_cgroup_from_private_id(id);
	if (memcg) {
		if (!mem_cgroup_is_root(memcg)) {
			if (do_memsw_account())
				page_counter_uncharge(&memcg->memsw, nr_pages);
			else
				page_counter_uncharge(&memcg->swap, nr_pages);
		}
		mod_memcg_state(memcg, MEMCG_SWAP, -nr_pages);
		mem_cgroup_private_id_put(memcg, nr_pages);
	}
	rcu_read_unlock();
}

/*
 * mem_cgroup_get_nr_swap_pages() - 计算 @memcg 当前层级真正可用的 swap 页数。
 * 从系统全局余量开始，逐祖先取“max - usage”的最小值；因此任一祖先限额
 * 都会约束子组。memcg 关闭或 v1 memsw 模式返回全局值，结果可能因并发计费
 * 立即变化，只适合作为分配/回收决策提示而非额度预留。
 */
long mem_cgroup_get_nr_swap_pages(struct mem_cgroup *memcg)
{
	long nr_swap_pages = get_nr_swap_pages();

	if (mem_cgroup_disabled() || do_memsw_account())
		return nr_swap_pages;
	for (; !mem_cgroup_is_root(memcg); memcg = parent_mem_cgroup(memcg))
		nr_swap_pages = min_t(long, nr_swap_pages,
				      READ_ONCE(memcg->swap.max) -
				      page_counter_read(&memcg->swap));
	return nr_swap_pages;
}

/*
 * mem_cgroup_swap_full() - 判断 @folio 所属层级是否应按 swap 紧张路径处理。
 * folio 必须锁定；全局 swap full 立即为真。v2 独立 swap 计费下，只要任一
 * 非根祖先 usage 达到 high 或 max 的一半即返回 true，给回收留出提前量。
 * 查询在 RCU 下完成且是瞬时快照，不修改计数。
 */
bool mem_cgroup_swap_full(struct folio *folio)
{
	struct mem_cgroup *memcg;
	bool ret = false;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (vm_swap_full())
		return true;
	if (do_memsw_account() || !folio_memcg_charged(folio))
		return ret;

	rcu_read_lock();
	memcg = folio_memcg(folio);
	for (; !mem_cgroup_is_root(memcg); memcg = parent_mem_cgroup(memcg)) {
		unsigned long usage = page_counter_read(&memcg->swap);

		if (usage * 2 >= READ_ONCE(memcg->swap.high) ||
		    usage * 2 >= READ_ONCE(memcg->swap.max)) {
			ret = true;
			break;
		}
	}
	rcu_read_unlock();

	return ret;
}

/*
 * setup_swap_account() - 兼容解析已废弃的 swapaccount= 启动参数。
 * 参数可解析且显式为 0 时仅告警，不再关闭计费；始终返回 1 表示已消费，
 * 从而把策略统一交给 cgroupfs 的 swap 控制文件。
 */
static int __init setup_swap_account(char *s)
{
	bool res;

	if (!kstrtobool(s, &res) && !res)
		pr_warn_once("The swapaccount=0 commandline option is deprecated "
			     "in favor of configuring swap control via cgroupfs. "
			     "Please report your usecase to linux-mm@kvack.org if you "
			     "depend on this functionality.\n");
	return 1;
}
__setup("swapaccount=", setup_swap_account);

/* swap_current_read() - 以字节返回该 css 的当前 swap page_counter 用量。 */
static u64 swap_current_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	return (u64)page_counter_read(&memcg->swap) * PAGE_SIZE;
}

/* swap_peak_show() - 输出本次打开视图记录的 swap 峰值，格式由 peak_show 统一。 */
static int swap_peak_show(struct seq_file *sf, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(sf));

	return peak_show(sf, v, &memcg->swap);
}

/* swap_peak_write() - 按 peak_write 规则重置/更新当前打开者的 swap 峰值基线。 */
static ssize_t swap_peak_write(struct kernfs_open_file *of, char *buf,
			       size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));

	return peak_write(of, buf, nbytes, off, &memcg->swap,
			  &memcg->swap_peaks);
}

/* swap_high_show() - 输出 swap.high 的页数限额；无限值编码为字符串 "max"。 */
static int swap_high_show(struct seq_file *m, void *v)
{
	return seq_puts_memcg_tunable(m,
		READ_ONCE(mem_cgroup_from_seq(m)->swap.high));
}

/*
 * swap_high_write() - 解析并设置软 swap 阈值。
 * 接受容量或 "max"；解析失败返回负 errno，成功返回 @nbytes。设置 high 不会
 * 立即驱逐已有 slot，后续计费/回收路径观察新阈值并产生 high 事件。
 */
static ssize_t swap_high_write(struct kernfs_open_file *of,
			       char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long high;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &high);
	if (err)
		return err;

	page_counter_set_high(&memcg->swap, high);

	return nbytes;
}

/* swap_max_show() - 输出 swap.max 硬上限；读取使用 READ_ONCE 接受并发更新。 */
static int swap_max_show(struct seq_file *m, void *v)
{
	return seq_puts_memcg_tunable(m,
		READ_ONCE(mem_cgroup_from_seq(m)->swap.max));
}

/*
 * swap_max_write() - 原子发布新的 swap 硬上限。
 * 已有用量可暂时高于缩小后的 max；函数只改变后续 charge 的准入条件，
 * 不在 kernfs 写路径同步回收。成功返回 @nbytes，格式错误返回负 errno。
 */
static ssize_t swap_max_write(struct kernfs_open_file *of,
			      char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long max;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &max);
	if (err)
		return err;

	xchg(&memcg->swap.max, max);

	return nbytes;
}

/* swap_events_show() - 导出本 memcg 的 high、max 与 charge fail 累计事件。 */
static int swap_events_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);

	seq_printf(m, "high %lu\n",
		   atomic_long_read(&memcg->memory_events[MEMCG_SWAP_HIGH]));
	seq_printf(m, "max %lu\n",
		   atomic_long_read(&memcg->memory_events[MEMCG_SWAP_MAX]));
	seq_printf(m, "fail %lu\n",
		   atomic_long_read(&memcg->memory_events[MEMCG_SWAP_FAIL]));

	return 0;
}

/*
 * swap_files 把 cgroup v2 文件名连接到上述读写回调；current/peak 是观测面，
 * high/max 是策略面，events 是结果面。均不在根组出现，因为根组受全局 swap
 * 管理；swap.events 还保存 kernfs 文件偏移，以便事件变化时发送通知。
 */
static struct cftype swap_files[] = {
	{
		.name = "swap.current",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = swap_current_read,
	},
	{
		.name = "swap.high",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = swap_high_show,
		.write = swap_high_write,
	},
	{
		.name = "swap.max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = swap_max_show,
		.write = swap_max_write,
	},
	{
		.name = "swap.peak",
		.flags = CFTYPE_NOT_ON_ROOT,
		.open = peak_open,
		.release = peak_release,
		.seq_show = swap_peak_show,
		.write = swap_peak_write,
	},
	{
		.name = "swap.events",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct mem_cgroup, swap_events_file),
		.seq_show = swap_events_show,
	},
	{ }	/* terminate */
};

#ifdef CONFIG_ZSWAP
/*
 * 【zswap 限额与写回策略】
 *
 * zswap 把压缩页留在 RAM，因此同时受 memory 与 zswap.max 约束。charge
 * 成功后记录压缩字节和对象数；uncharge 必须使用相同实际大小。达到
 * zswap.writeback/上限时选择写回磁盘或拒绝新条目，并通过 memory.events
 * 暴露失败。这里的额度只描述 zswap 存储，不替代 swap slot 的 memcg ID。
 */
/**
 * obj_cgroup_may_zswap - check if this cgroup can zswap
 * @objcg: the object cgroup
 *
 * Check if the hierarchical zswap limit has been reached.
 *
 * This doesn't check for specific headroom, and it is not atomic
 * either. But with zswap, the size of the allocation is only known
 * once compression has occurred, and this optimistic pre-check avoids
 * spending cycles on compression when there is already no room left
 * or zswap is disabled altogether somewhere in the hierarchy.
 */
/*
 * obj_cgroup_may_zswap() - 乐观预检 @objcg 所属层级是否允许尝试压缩入 zswap。
 * v1 不提供该控制，直接允许；v2 对每个非根祖先检查 zswap.max，0 直接拒绝，
 * 有限值则强制刷新统计后比较当前压缩字节折算页数。返回值不预留空间，也不
 * 与随后 charge 原子化，只用于避免明知无额度仍做昂贵压缩；调用中临时持有
 * original_memcg 引用并在返回前释放。
 */
bool obj_cgroup_may_zswap(struct obj_cgroup *objcg)
{
	struct mem_cgroup *memcg, *original_memcg;
	bool ret = true;

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return true;

	original_memcg = get_mem_cgroup_from_objcg(objcg);
	for (memcg = original_memcg; !mem_cgroup_is_root(memcg);
	     memcg = parent_mem_cgroup(memcg)) {
		unsigned long max = READ_ONCE(memcg->zswap_max);
		unsigned long pages;

		if (max == PAGE_COUNTER_MAX)
			continue;
		if (max == 0) {
			ret = false;
			break;
		}

		/* Force flush to get accurate stats for charging */
		/* 限额判断需要跨 CPU 统计尽量新鲜；仍允许刷新后的并发小幅漂移。 */
		__mem_cgroup_flush_stats(memcg, true);
		pages = memcg_page_state(memcg, MEMCG_ZSWAP_B) / PAGE_SIZE;
		if (pages < max)
			continue;
		ret = false;
		break;
	}
	mem_cgroup_put(original_memcg);
	return ret;
}

/**
 * obj_cgroup_charge_zswap - charge compression backend memory
 * @objcg: the object cgroup
 * @size: size of compressed object
 *
 * This forces the charge after obj_cgroup_may_zswap() allowed
 * compression and storage in zswap for this cgroup to go ahead.
 */
/*
 * obj_cgroup_charge_zswap() - 为已压缩完成的对象强制提交 @size 字节账目。
 * 仅 v2 非根 objcg 生效，调用者必须处于 PF_MEMALLOC，保证写回/回收保留路径
 * 不因普通内存限额死锁。先向 objcg 收取后端内存，再在 RCU 下增加压缩字节、
 * 对象数及不可压缩页统计；无返回值，失败仅 WARN，因为预检后此阶段必须提交。
 */
void obj_cgroup_charge_zswap(struct obj_cgroup *objcg, size_t size)
{
	struct mem_cgroup *memcg;

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return;

	if (obj_cgroup_is_root(objcg))
		return;

	VM_WARN_ON_ONCE(!(current->flags & PF_MEMALLOC));

	/* PF_MEMALLOC context, charging must succeed */
	/* 对象已放入后端，不能用普通失败返回拆开“存储成功”和“memcg 已计费”。 */
	if (obj_cgroup_charge(objcg, GFP_KERNEL, size))
		VM_WARN_ON_ONCE(1);

	rcu_read_lock();
	memcg = obj_cgroup_memcg(objcg);
	mod_memcg_state(memcg, MEMCG_ZSWAP_B, size);
	mod_memcg_state(memcg, MEMCG_ZSWAPPED, 1);
	if (size == PAGE_SIZE)
		mod_memcg_state(memcg, MEMCG_ZSWAP_INCOMP, 1);
	rcu_read_unlock();
}

/**
 * obj_cgroup_uncharge_zswap - uncharge compression backend memory
 * @objcg: the object cgroup
 * @size: size of compressed object
 *
 * Uncharges zswap memory on page in.
 */
/*
 * obj_cgroup_uncharge_zswap() - 在 page-in、淘汰或释放压缩对象时撤销 @size 账目。
 * 必须与成功的 charge 使用同一 objcg 和实际压缩大小；先归还 objcg 后端内存，
 * 再扣压缩字节、对象数及 size==PAGE_SIZE 的不可压缩统计。v1/根组为空操作，
 * 无直接返回值。
 */
void obj_cgroup_uncharge_zswap(struct obj_cgroup *objcg, size_t size)
{
	struct mem_cgroup *memcg;

	if (!cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return;

	if (obj_cgroup_is_root(objcg))
		return;

	obj_cgroup_uncharge(objcg, size);

	rcu_read_lock();
	memcg = obj_cgroup_memcg(objcg);
	mod_memcg_state(memcg, MEMCG_ZSWAP_B, -size);
	mod_memcg_state(memcg, MEMCG_ZSWAPPED, -1);
	if (size == PAGE_SIZE)
		mod_memcg_state(memcg, MEMCG_ZSWAP_INCOMP, -1);
	rcu_read_unlock();
}

/*
 * mem_cgroup_zswap_writeback_enabled() - 检查 @memcg 到根的写回策略是否全允许。
 * zswap 全局关闭时必须返回 true，让页继续落到真实 swap 设备；启用时任何祖先
 * zswap.writeback=0 都禁止写回。READ_ONCE 提供无锁策略快照，返回后配置可变。
 */
bool mem_cgroup_zswap_writeback_enabled(struct mem_cgroup *memcg)
{
	/* if zswap is disabled, do not block pages going to the swapping device */
	/* 没有 zswap 后端时，“禁止写回”会误伤普通 swapout，故显式旁路层级开关。 */
	if (!zswap_is_enabled())
		return true;

	for (; memcg; memcg = parent_mem_cgroup(memcg))
		if (!READ_ONCE(memcg->zswap_writeback))
			return false;

	return true;
}

/* zswap_current_read() - 刷新统计后返回该 css 当前占用的压缩后端字节数。 */
static u64 zswap_current_read(struct cgroup_subsys_state *css,
			      struct cftype *cft)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);

	mem_cgroup_flush_stats(memcg);
	return memcg_page_state(memcg, MEMCG_ZSWAP_B);
}

/* zswap_max_show() - 输出 zswap.max 页数限制，无限值由通用助手显示为 "max"。 */
static int zswap_max_show(struct seq_file *m, void *v)
{
	return seq_puts_memcg_tunable(m,
		READ_ONCE(mem_cgroup_from_seq(m)->zswap_max));
}

/*
 * zswap_max_write() - 解析容量/"max" 并原子更新压缩存储上限。
 * 缩小上限不回收既有对象；它只使后续 may_zswap 预检拒绝新压缩。成功返回
 * @nbytes，解析失败保持旧值并返回负 errno。
 */
static ssize_t zswap_max_write(struct kernfs_open_file *of,
			       char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned long max;
	int err;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &max);
	if (err)
		return err;

	xchg(&memcg->zswap_max, max);

	return nbytes;
}

/* zswap_writeback_show() - 以 0/1 输出本层是否允许把 zswap 对象写到 swap。 */
static int zswap_writeback_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);

	seq_printf(m, "%d\n", READ_ONCE(memcg->zswap_writeback));
	return 0;
}

/*
 * zswap_writeback_write() - 设置本 memcg 的 zswap 写回布尔策略。
 * 仅接受整数 0 或 1；非法输入返回 -EINVAL，成功以 WRITE_ONCE 发布并返回
 * @nbytes。最终准入还需 mem_cgroup_zswap_writeback_enabled() 检查所有祖先。
 */
static ssize_t zswap_writeback_write(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int zswap_writeback;
	ssize_t parse_ret = kstrtoint(strstrip(buf), 0, &zswap_writeback);

	if (parse_ret)
		return parse_ret;

	if (zswap_writeback != 0 && zswap_writeback != 1)
		return -EINVAL;

	WRITE_ONCE(memcg->zswap_writeback, zswap_writeback);
	return nbytes;
}

/*
 * zswap_files 暴露 v2 的压缩存储观测值、层级容量上限和写回开关；current/max
 * 不显示在根组，writeback 保留根级总开关语义。表尾空项供 cgroup core 停止。
 */
static struct cftype zswap_files[] = {
	{
		.name = "zswap.current",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = zswap_current_read,
	},
	{
		.name = "zswap.max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = zswap_max_show,
		.write = zswap_max_write,
	},
	{
		.name = "zswap.writeback",
		.seq_show = zswap_writeback_show,
		.write = zswap_writeback_write,
	},
	{ }	/* terminate */
};
#endif /* CONFIG_ZSWAP */

/*
 * mem_cgroup_swap_init() - 在 subsys initcall 阶段注册 swap 相关 cgroup 文件。
 * memcg 禁用时为空操作；v2 始终注册 swap_files，配置允许时再注册 v1 memsw
 * 和 v2 zswap 文件。注册失败会 WARN 但函数仍返回 0，避免可选控制面阻断启动。
 */
static int __init mem_cgroup_swap_init(void)
{
	if (mem_cgroup_disabled())
		return 0;

	WARN_ON(cgroup_add_dfl_cftypes(&memory_cgrp_subsys, swap_files));
#ifdef CONFIG_MEMCG_V1
	WARN_ON(cgroup_add_legacy_cftypes(&memory_cgrp_subsys, memsw_files));
#endif
#ifdef CONFIG_ZSWAP
	WARN_ON(cgroup_add_dfl_cftypes(&memory_cgrp_subsys, zswap_files));
#endif
	return 0;
}
subsys_initcall(mem_cgroup_swap_init);

#endif /* CONFIG_SWAP */

/*
 * mem_cgroup_node_filter_allowed() - 用 @memcg 的 cpuset 有效节点收窄 *@mask。
 * @mask 原地更新为交集，空 memcg 保持不变。该接口服务迁移路径，不持有能冻结
 * cpuset/hotplug 的重锁，因此结果允许瞬时过期；调用者仍须处理迁移竞态。
 */
void mem_cgroup_node_filter_allowed(struct mem_cgroup *memcg, nodemask_t *mask)
{
	nodemask_t allowed;

	if (!memcg)
		return;

	/*
	 * Since this interface is intended for use by migration paths, and
	 * reclaim and migration are subject to race conditions such as changes
	 * in effective_mems and hot-unpluging of nodes, inaccurate allowed
	 * mask is acceptable.
	 */
	/* 这里只取得决策快照；不承诺节点在真正迁移时仍在线且仍被 cpuset 允许。 */
	cpuset_nodes_allowed(memcg->css.cgroup, &allowed);
	nodes_and(*mask, *mask, allowed);
}

/*
 * mem_cgroup_show_protected_memory() - 打印 @memcg 子树的 min/low 保护用量摘要。
 * 仅 cgroup v2 memcg 生效；@memcg 为空时选择根组。读取原子聚合值并换算为 kB，
 * 用于诊断保护导致的回收行为，不刷新统计、不改变保护配置，也无直接返回值。
 */
void mem_cgroup_show_protected_memory(struct mem_cgroup *memcg)
{
	if (mem_cgroup_disabled() || !cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return;

	if (!memcg)
		memcg = root_mem_cgroup;

	pr_warn("Memory cgroup min protection %lukB -- low protection %lukB",
		K(atomic_long_read(&memcg->memory.children_min_usage)),
		K(atomic_long_read(&memcg->memory.children_low_usage)));
}
