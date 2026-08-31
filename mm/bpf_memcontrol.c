// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Memory Controller-related BPF kfuncs and auxiliary code
 *
 * Author: Roman Gushchin <roman.gushchin@linux.dev>
 */
/*
 * 本文件把 memcg 引用获取/释放和统计读取封装为 BPF kfunc，并用 BTF flags 把
 * NULL、RCU、ownership 与可睡眠约束交给 verifier。BPF 程序只能通过这些受
 * 验证接口观察内存控制器，不能直接伪造 mem_cgroup 指针或越界统计索引。
 */

#include <linux/memcontrol.h>
#include <linux/bpf.h>

__bpf_kfunc_start_defs();

/**
 * bpf_get_root_mem_cgroup - Returns a pointer to the root memory cgroup
 *
 * The function has KF_ACQUIRE semantics, even though the root memory
 * cgroup is never destroyed after being created and doesn't require
 * reference counting. And it's perfectly safe to pass it to
 * bpf_put_mem_cgroup()
 *
 * Return: A pointer to the root memory cgroup.
 */
/*
 * bpf_get_root_mem_cgroup() - 取得根 memory cgroup 的 BPF owning reference。
 * 业务背景：BPF 程序需要从全局根开始读取 memcg 统计；虽然根组创建后永不
 * 销毁、无需真实加引用，接口仍声明 KF_ACQUIRE，使 verifier 要求最终调用
 * bpf_put_mem_cgroup()，统一所有 memcg 指针的线性 ownership 规则。
 * 入参：无。
 * 出参/返回：memcg 控制器启用时返回 root_mem_cgroup；禁用时返回 NULL。
 * 对 verifier 是需 release 的 owning pointer，实际根 css 不执行 css_get()。
 * 注意事项：不睡眠、无需 RCU；返回 NULL 由 KF_RET_NULL 强制程序判空，根引用
 * 可安全交给 bpf_put_mem_cgroup()，因为根 css 生命周期/基准引用不会消失。
 */
__bpf_kfunc struct mem_cgroup *bpf_get_root_mem_cgroup(void)
{
	/* 关闭 memcg 时没有可供 BPF 观察的有效控制器对象。 */
	if (mem_cgroup_disabled())
		return NULL;

	/* css_get() is not needed */
	/* 根 css 永生，无需真实递增；KF_ACQUIRE 只建立 verifier 的配对义务。 */
	return root_mem_cgroup;
}

/**
 * bpf_get_mem_cgroup - Get a reference to a memory cgroup
 * @css: pointer to the css structure
 *
 * It's fine to pass a css which belongs to any cgroup controller,
 * e.g. unified hierarchy's main css.
 *
 * Implements KF_ACQUIRE semantics.
 *
 * Return: A pointer to a mem_cgroup structure after bumping
 * the corresponding css's reference counter.
 */
/*
 * bpf_get_mem_cgroup() - 从任意控制器 css 取得同一 cgroup 的 memcg 引用。
 * 业务背景：BPF 上下文常只暴露 unified hierarchy 的某个 css，本函数先判断它
 * 是否已经是 memory css；否则在 RCU 下按 memory subsystem id 横向查槽位。
 * 入参：@css 是 verifier 认可的 RCU 或 trusted 借用 css，非 NULL；可属于任意
 * controller。函数不消费输入引用，成功时为目标 memory css 新增一份引用。
 * 出参/返回：成功返回 container_of 得到的 owning mem_cgroup；控制器关闭、根
 * 尚未初始化、目标槽为空或 css 已离线无法 tryget 时返回 NULL。
 * 注意事项：KF_ACQUIRE|KF_RET_NULL|KF_RCU；调用者判空并最终 put。RCU 只保护
 * subsys[] 指针查找，css_tryget() 才把生命周期延长到离开 RCU 临界区以后。
 */
__bpf_kfunc struct mem_cgroup *
bpf_get_mem_cgroup(struct cgroup_subsys_state *css)
{
	/* memcg 是成功输出；rcu_unlock 记录本函数是否自行进入 RCU 临界区。 */
	struct mem_cgroup *memcg = NULL;
	bool rcu_unlock = false;

	/* 启动未完成或功能关闭时不可解引用 root 以判断 memory subsystem。 */
	if (mem_cgroup_disabled() || !root_mem_cgroup)
		return NULL;

	/* 输入不是 memory controller css 时，转到同一 cgroup 的 memory subsys 槽。 */
	if (root_mem_cgroup->css.ss != css->ss) {
		/* cgroup 由输入 css 借用；ssid 是 memory controller 的稳定注册编号。 */
		struct cgroup *cgroup = css->cgroup;
		int ssid = root_mem_cgroup->css.ss->id;

		/* RCU 保证并发 offline 期间 subsys[ssid] 指针及对象内存仍可尝试取引用。 */
		rcu_read_lock();
		rcu_unlock = true;
		css = rcu_dereference_raw(cgroup->subsys[ssid]);
	}

	/* css_tryget 在非零引用上原子增计数；失败表示对象正退出，不能返回裸指针。 */
	if (css && css_tryget(css))
		memcg = container_of(css, struct mem_cgroup, css);

	/* 只有横向查槽路径由本函数加锁，直接 memory css 路径无需解锁。 */
	if (rcu_unlock)
		rcu_read_unlock();

	return memcg;
}

/**
 * bpf_put_mem_cgroup - Put a reference to a memory cgroup
 * @memcg: memory cgroup to release
 *
 * Releases a previously acquired memcg reference.
 * Implements KF_RELEASE semantics.
 */
/*
 * bpf_put_mem_cgroup() - 释放先前 kfunc 获取的一份 memcg ownership。
 * 业务背景：与两个 get kfunc 的 KF_ACQUIRE 配对，verifier 在调用后使指针失效。
 * 入参：@memcg 是非 NULL owning pointer，必须恰好释放一次；函数消费该引用。
 * 出参/返回：无直接返回值；css 引用计数减一，非根对象可能随后进入最终释放。
 * 注意事项：KF_RELEASE，不睡眠；调用后 BPF 程序不得再次解引用或重复 put。
 */
__bpf_kfunc void bpf_put_mem_cgroup(struct mem_cgroup *memcg)
{
	css_put(&memcg->css);
}

/**
 * bpf_mem_cgroup_vm_events - Read memory cgroup's vm event counter
 * @memcg: memory cgroup
 * @event: event id
 *
 * Allows to read memory cgroup event counters.
 *
 * Return: The current value of the corresponding events counter.
 */
/*
 * bpf_mem_cgroup_vm_events() - 读取一个合法 VM event 的 memcg 累计值。
 * 业务背景：BPF 监控程序按 enum vm_event_item 查询缺页、回收等事件，先验证
 * 索引以防越界读取内部数组。
 * 入参：@memcg 是仍持有效引用的借用对象；@event 是待查枚举编号。
 * 出参/返回：合法时返回当前无符号计数；非法编号返回 ULONG_MAX 哨兵。
 * 注意事项：不刷新分层 rstat、不提供快照一致性；并发更新下是某时刻近似值。
 */
__bpf_kfunc unsigned long bpf_mem_cgroup_vm_events(struct mem_cgroup *memcg,
						   enum vm_event_item event)
{
	/* -1 转 unsigned long 明确形成全 1 错误哨兵，不与 errno 返回通道混用。 */
	if (unlikely(!memcg_vm_event_item_valid(event)))
		return (unsigned long)-1;

	return memcg_events(memcg, event);
}

/**
 * bpf_mem_cgroup_usage - Read memory cgroup's usage
 * @memcg: memory cgroup
 *
 * Please, note that the root memory cgroup it special and is exempt
 * from the memory accounting. The returned value is a sum of sub-cgroup's
 * usages and it not reflecting the size of the root memory cgroup itself.
 * If you need to get an approximation, you can use root level statistics:
 * e.g. NR_FILE_PAGES + NR_ANON_MAPPED.
 *
 * Return: The current memory cgroup size in bytes.
 */
/*
 * bpf_mem_cgroup_usage() - 读取 memcg memory page_counter 并换算为字节。
 * 业务背景：普通组 page_counter 表示其层级用量；根组免于直接 memory 计费，
 * 因而返回的是子组用量汇总而非根自身实际驻留量。若需根近似值，可组合
 * NR_FILE_PAGES 与 NR_ANON_MAPPED 等根级统计。
 * 入参：@memcg 是持有效引用的借用对象，非 NULL。
 * 出参/返回：返回 page_counter 当前页数乘 PAGE_SIZE 的字节数，无错误通道。
 * 注意事项：无锁近似读取、可能与并发 charge/uncharge 变化；不刷新统计且不睡眠。
 */
__bpf_kfunc unsigned long bpf_mem_cgroup_usage(struct mem_cgroup *memcg)
{
	return page_counter_read(&memcg->memory) * PAGE_SIZE;
}

/**
 * bpf_mem_cgroup_memory_events - Read memory cgroup's memory event value
 * @memcg: memory cgroup
 * @event: memory event id
 *
 * Return: The current value of the memory event counter.
 */
/*
 * bpf_mem_cgroup_memory_events() - 读取 memory.events 类原子计数器。
 * 业务背景：向 BPF 暴露 low/high/max/oom 等 memcg memory 事件，同时保护数组边界。
 * 入参：@memcg 是借用有效对象；@event 为 [0,MEMCG_NR_MEMORY_EVENTS) 枚举。
 * 出参/返回：合法时返回 atomic_long 当前值；非法时返回 ULONG_MAX 哨兵。
 * 注意事项：原子读取避免撕裂但不冻结其他计数器，不形成多字段一致快照；不睡眠。
 */
__bpf_kfunc unsigned long bpf_mem_cgroup_memory_events(struct mem_cgroup *memcg,
						       enum memcg_memory_event event)
{
	if (unlikely(event >= MEMCG_NR_MEMORY_EVENTS))
		return (unsigned long)-1;

	return atomic_long_read(&memcg->memory_events[event]);
}

/**
 * bpf_mem_cgroup_page_state - Read memory cgroup's page state counter
 * @memcg: memory cgroup
 * @idx: counter idx
 *
 * Allows to read memory cgroup statistics. The output is in bytes.
 *
 * Return: The value of the page state counter in bytes.
 */
/*
 * bpf_mem_cgroup_page_state() - 读取合法 memcg/node page-state 的输出值。
 * 业务背景：复用 memory.stat 的索引验证和单位转换，使 BPF 得到与用户接口一致
 * 的字节数，而不是内部页数或混合单位。
 * 入参：@memcg 是借用有效对象；@idx 是 memcg_stat_item 或允许的 node_stat_item。
 * 出参/返回：合法时返回转换后的字节值；非法索引返回 ULONG_MAX。
 * 注意事项：不主动 flush rstat，读数可能滞后；调用 flush kfunc 后可提高新鲜度。
 */
__bpf_kfunc unsigned long bpf_mem_cgroup_page_state(struct mem_cgroup *memcg, int idx)
{
	if (unlikely(!memcg_stat_item_valid(idx)))
		return (unsigned long)-1;

	return memcg_page_state_output(memcg, idx);
}

/**
 * bpf_mem_cgroup_flush_stats - Flush memory cgroup's statistics
 * @memcg: memory cgroup
 *
 * Propagate memory cgroup's statistics up the cgroup tree.
 */
/*
 * bpf_mem_cgroup_flush_stats() - 把 memcg 子树 rstat 增量传播到层级统计。
 * 业务背景：统计 kfunc 默认偏向低开销近似读，sleepable BPF 程序可先调用本函数
 * 获得较新聚合值，再读取 page state。
 * 入参：@memcg 是持有效引用的借用目标，非 NULL，不转移 ownership。
 * 出参/返回：无直接返回值；更新该层级的聚合统计缓存。
 * 注意事项：标为 KF_SLEEPABLE，可能获取 cgroup rstat 锁并调度，只能从允许睡眠
 * 的 BPF 上下文调用；传播仍不等于与后续并发更新构成冻结快照。
 */
__bpf_kfunc void bpf_mem_cgroup_flush_stats(struct mem_cgroup *memcg)
{
	mem_cgroup_flush_stats(memcg);
}

__bpf_kfunc_end_defs();

/*
 * BTF 表把 C 函数契约编码给 verifier：两个 get 返回可空 owning pointer，通用
 * get 接受 RCU/trusted css，put 消费引用；统计读取不改变 ownership；flush 可
 * 睡眠。BPF_PROG_TYPE_UNSPEC 注册使允许 kfunc 的程序类型由通用 verifier 规则
 * 决定，而不是绑定单一 prog type。
 */
BTF_KFUNCS_START(bpf_memcontrol_kfuncs)
BTF_ID_FLAGS(func, bpf_get_root_mem_cgroup, KF_ACQUIRE | KF_RET_NULL)
BTF_ID_FLAGS(func, bpf_get_mem_cgroup, KF_ACQUIRE | KF_RET_NULL | KF_RCU)
BTF_ID_FLAGS(func, bpf_put_mem_cgroup, KF_RELEASE)

BTF_ID_FLAGS(func, bpf_mem_cgroup_vm_events)
BTF_ID_FLAGS(func, bpf_mem_cgroup_memory_events)
BTF_ID_FLAGS(func, bpf_mem_cgroup_usage)
BTF_ID_FLAGS(func, bpf_mem_cgroup_page_state)
BTF_ID_FLAGS(func, bpf_mem_cgroup_flush_stats, KF_SLEEPABLE)

BTF_KFUNCS_END(bpf_memcontrol_kfuncs)

/* kfunc 集合由当前内核模块拥有，set 指向上述只读 BTF ID/flag 表。 */
static const struct btf_kfunc_id_set bpf_memcontrol_kfunc_set = {
	.owner          = THIS_MODULE,
	.set            = &bpf_memcontrol_kfuncs,
};

/*
 * bpf_memcontrol_init() - 在 late init 阶段向 BPF 子系统注册 memcg kfunc 集合。
 * 业务背景：memcg/root 初始化完成后，verifier 才能接受程序对这些 kfunc 的调用。
 * 入参：无。
 * 出参/返回：注册成功返回 0；失败返回 register_btf_kfunc_id_set errno 并打印警告。
 * 注意事项：__init、可在启动线程上下文分配并睡眠；失败不会回滚 memcg，只使
 * 这些 kfunc 不可用于 BPF，late_initcall 把返回值交给 initcall 诊断。
 */
static int __init bpf_memcontrol_init(void)
{
	/* err 是注册结果，既用于日志也原样返回给 initcall 框架。 */
	int err;

	/* 单一注册点发布整个集合；失败时没有部分可见接口由本文件清理。 */
	err = register_btf_kfunc_id_set(BPF_PROG_TYPE_UNSPEC,
					&bpf_memcontrol_kfunc_set);
	if (err)
		pr_warn("error while registering bpf memcontrol kfuncs: %d", err);

	return err;
}
late_initcall(bpf_memcontrol_init);
