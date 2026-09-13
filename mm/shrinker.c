// SPDX-License-Identifier: GPL-2.0
#include <linux/memcontrol.h>
#include <linux/rwsem.h>
#include <linux/shrinker.h>
#include <linux/rculist.h>
#include <trace/events/vmscan.h>

#include "internal.h"

LIST_HEAD(shrinker_list);
DEFINE_MUTEX(shrinker_mutex);
/* list 的 RCU 可见性与注册/扩容的 mutex 串行化互补：遍历可睡眠前取得对象引用。 */

#ifdef CONFIG_MEMCG
/* memcg-aware shrinker 以固定大小 unit 分块保存 bitmap/deferred，避免 ID 扩容移动每个元素。 */
static int shrinker_nr_max;

static inline int shrinker_unit_size(int nr_items)
{
	/* 向上取整到 unit 位数，数组尾部允许保留未分配 shrinker ID 的空槽。 */
	return (DIV_ROUND_UP(nr_items, SHRINKER_UNIT_BITS) * sizeof(struct shrinker_info_unit *));
}

static inline void shrinker_unit_free(struct shrinker_info *info, int start)
{
	/* 从 start 起逆向清理“本次扩容”单元；遇 NULL 表示后续从未成功分配。 */
	struct shrinker_info_unit **unit;
	/* unit 指针数组嵌在 info 尾部，而各 unit 独立分配，故析构必须分两层完成。 */
	int nr, i;

	if (!info)
		return;

	unit = info->unit;
	nr = DIV_ROUND_UP(info->map_nr_max, SHRINKER_UNIT_BITS);

	for (i = start; i < nr; i++) {
		/* 连续创建的 unit 首次为空即表示后续均未成功建立。 */
		if (!unit[i])
			break;

		kfree(unit[i]);
		unit[i] = NULL;
	}
}

static inline int shrinker_unit_alloc(struct shrinker_info *new,
				       struct shrinker_info *old, int nid)
{
	/* new 已复制 old 的既有 unit 指针，只为扩展尾部按节点分配零化 unit。 */
	struct shrinker_info_unit *unit;
	int nr = DIV_ROUND_UP(new->map_nr_max, SHRINKER_UNIT_BITS);
	int start = old ? DIV_ROUND_UP(old->map_nr_max, SHRINKER_UNIT_BITS) : 0;
	int i;
	/* nid 选择 NUMA 本地分配节点，减少对应 cgroup reclaim 的远端 metadata 访问。 */

	for (i = start; i < nr; i++) {
		unit = kzalloc_node(sizeof(*unit), GFP_KERNEL, nid);
		if (!unit) {
			/* 失败回滚只释放新尾部，不能触碰仍由 old/new 共同引用的旧 unit。 */
			shrinker_unit_free(new, start);
			return -ENOMEM;
		}

		new->unit[i] = unit;
	}

	return 0;
}

static void __free_shrinker_info(struct mem_cgroup *memcg)
{
	/* mutex 排他下撤每节点 RCU 指针；先销毁 unit，再释放包含其指针数组的 info。 */
	/* mutex 排他下撤每节点 RCU 指针；先释放 unit 再释放包含柔性数组的 info。 */
	struct mem_cgroup_per_node *pn;
	struct shrinker_info *info;
	int nid;

	lockdep_assert_held(&shrinker_mutex);

	for_each_node(nid) {
		/* 每个节点的 info 独立管理，释放时必须逐节点撤销其 RCU 发布指针。 */
		pn = memcg->nodeinfo[nid];
		info = rcu_dereference_protected(pn->shrinker_info, true);
		shrinker_unit_free(info, 0);
		kvfree(info);
		rcu_assign_pointer(pn->shrinker_info, NULL);
	}
}

void free_shrinker_info(struct mem_cgroup *memcg)
{
	/* 公共析构取得 shrinker_mutex，避免与 ID 扩容或 cgroup 欠账迁移交错。 */
	/* 公共析构封装锁，确保与 shrinker ID 扩容和 reparent 不能交叉。 */
	mutex_lock(&shrinker_mutex);
	__free_shrinker_info(memcg);
	mutex_unlock(&shrinker_mutex);
}

int alloc_shrinker_info(struct mem_cgroup *memcg)
{
	/* 对每个 NUMA 节点建立空 bitmap/deferred 容器；任何失败均回收已发布节点。 */
	/* 为现有 memcg 的每个 node 创建 bitmap/deferred 容器，全部成功后才返回发布完成。 */
	int nid, ret = 0;
	int array_size = 0;

	mutex_lock(&shrinker_mutex);
	/* 锁内完成全部节点初始化，避免其他路径拿到容量不完整的 memcg。 */
	array_size = shrinker_unit_size(shrinker_nr_max);
	for_each_node(nid) {
		struct shrinker_info *info = kvzalloc_node(sizeof(*info) + array_size,
							   GFP_KERNEL, nid);
		if (!info)
			/* 任何一个节点无法分配即不能部分成功，错误路径回收之前节点。 */
			goto err;
		info->map_nr_max = shrinker_nr_max;
		/* map_nr_max 与当前全局最大容量快照匹配；后来注册会通过 COW 统一扩大。 */
		if (shrinker_unit_alloc(info, NULL, nid)) {
			kvfree(info);
			goto err;
		}
		rcu_assign_pointer(memcg->nodeinfo[nid]->shrinker_info, info);
		/* 逐节点发布仅发生在初始化锁内；读侧在 RCU 下可立即安全取得完成对象。 */
	}
	mutex_unlock(&shrinker_mutex);

	return ret;

err:
	/* 任一节点失败按已发布指针统一回收，避免留下节点间容量不一致的 memcg。 */
	__free_shrinker_info(memcg);
	mutex_unlock(&shrinker_mutex);
	return -ENOMEM;
}

static struct shrinker_info *shrinker_info_protected(struct mem_cgroup *memcg,
						     int nid)
{
	/* 仅供 shrinker_mutex 持有者取指针；RCU 读侧调用必须使用 rcu_dereference。 */
	return rcu_dereference_protected(memcg->nodeinfo[nid]->shrinker_info,
					 lockdep_is_held(&shrinker_mutex));
}
/* protected 读取不额外加 RCU，前提是 shrinker_mutex 已阻止 info 的发布替换。 */

static int expand_one_shrinker_info(struct mem_cgroup *memcg, int new_size,
				    int old_size, int new_nr_max)
{
	/* 扩容是 copy-on-write：新 info 发布后，旧 info 仅在 RCU grace period 后释放。 */
	/* 扩容采用 copy-on-write：发布新 info 后旧 info 经 RCU grace period 再释放。 */
	struct shrinker_info *new, *old;
	struct mem_cgroup_per_node *pn;
	int nid;

	for_each_node(nid) {
		/* 节点循环复制旧容器的指针数组并只新增尾部 unit，保留进行中的原子计数。 */
		pn = memcg->nodeinfo[nid];
		old = shrinker_info_protected(memcg, nid);
		/* Not yet online memcg */
		if (!old)
			/* 尚未 online 的 memcg 没有可替换对象，后续上线流程会按当前全局容量创建。 */
			return 0;

		/* Already expanded this shrinker_info */
		if (new_nr_max <= old->map_nr_max)
			/* 其他注册者已经完成该容量扩展，当前节点无须重新分配。 */
			/* 多个 ID 注册可重复触发扩容检查，已有容量足够时保持旧快照。 */
			continue;

		new = kvzalloc_node(sizeof(*new) + new_size, GFP_KERNEL, nid);
		if (!new)
			return -ENOMEM;

		new->map_nr_max = new_nr_max;

		memcpy(new->unit, old->unit, old_size);
		if (shrinker_unit_alloc(new, old, nid)) {
			/* new 尚未 RCU 发布，失败时直接释放其自身，不影响旧 info。 */
			kvfree(new);
			return -ENOMEM;
		}

		rcu_assign_pointer(pn->shrinker_info, new);
		/* 发布后读者可继续用 old 到 RCU 期结束，new 复用了不可变旧 unit。 */
		kvfree_rcu(old, rcu);
	}

	return 0;
}

static int expand_shrinker_info(int new_id)
{
	/* 全局 ID 上限按 unit 边界增长，必须先为全部现存 memcg 发布新容量。 */
	/* 全局最大 ID 以 unit 边界增长；遍历所有 memcg，成功后才推进 shrinker_nr_max。 */
	int ret = 0;
	int new_nr_max = round_up(new_id + 1, SHRINKER_UNIT_BITS);
	int new_size, old_size = 0;
	struct mem_cgroup *memcg;

	if (!root_mem_cgroup)
		/* 内存 cgroup 子系统未上线时只记录未来容量，无实际 info 可扩。 */
		/* 早期启动尚无 memcg 树时无需分配，记录容量供随后创建的 memcg 使用。 */
		goto out;

	lockdep_assert_held(&shrinker_mutex);

	new_size = shrinker_unit_size(new_nr_max);
	old_size = shrinker_unit_size(shrinker_nr_max);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	/* iterator 在每次循环维持当前 memcg 引用；出错要显式 break 归还它。 */
	do {
		/* 每个 memcg 都必须成功替换；一处失败即停止，不能发布 shrinker_nr_max。 */
		ret = expand_one_shrinker_info(memcg, new_size, old_size,
					       new_nr_max);
		if (ret) {
			/* 显式 iterator_break 归还当前 cgroup，随后 out 保持旧全局容量不变。 */
			mem_cgroup_iter_break(NULL, memcg);
			goto out;
		}
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);
	/* 正常结束时 iterator 已返回 NULL；不再持有任何 memcg 迭代引用。 */
out:
	if (!ret)
		shrinker_nr_max = new_nr_max;

	return ret;
}

static inline int shrinker_id_to_index(int shrinker_id)
{
	/* ID 的商选择 unit，余数选择 unit 内 bitmap 与 deferred 数组槽。 */
	return shrinker_id / SHRINKER_UNIT_BITS;
}

static inline int shrinker_id_to_offset(int shrinker_id)
{
	/* 这两个转换必须使用相同 SHRINKER_UNIT_BITS，否则 bitmap 与计数会错位。 */
	return shrinker_id % SHRINKER_UNIT_BITS;
}

static inline int calc_shrinker_id(int index, int offset)
{
	/* index/offset 来自 for_each_set_bit，逆算全局 ID 后才可从 IDR 获得 shrinker。 */
	/* bitmap 遍历的逆变换，得到可在 IDR 中查找的全局 shrinker ID。 */
	return index * SHRINKER_UNIT_BITS + offset;
}

void set_shrinker_bit(struct mem_cgroup *memcg, int nid, int shrinker_id)
{
	/* 生产者把“此 cgroup/node 可能有对象”的提示写入 bitmap，消费者据此避免全表扫描。 */
	/* 对某 memcg/node 有可回收对象的生产者置位；root 不使用此 per-memcg bitmap。 */
	if (shrinker_id >= 0 && memcg && !mem_cgroup_is_root(memcg)) {
		/* root 没有 per-memcg bitmap；无效参数无副作用，方便对象生产路径直接调用。 */
		struct shrinker_info *info;

		rcu_read_lock();
		/* info 可在 shrinker 注册扩容时替换，RCU 覆盖取 unit 及 bitmap 原子置位。 */
		info = rcu_dereference(memcg->nodeinfo[nid]->shrinker_info);
		if (!WARN_ON_ONCE(shrinker_id >= info->map_nr_max)) {
			/* RCU 保证 info 存活；范围检查防止扩容发布异常时破坏相邻 unit。 */
			struct shrinker_info_unit *unit;

			unit = info->unit[shrinker_id_to_index(shrinker_id)];
			/* index/offset 共同决定本 shrinker 的唯一位，unit 内数据在 memcg 存活期不搬迁。 */
			/* Pairs with smp mb in shrink_slab() */
			smp_mb__before_atomic();
			/* 先发布对象插入，再置提示位；清位方用 after_atomic 复查关闭竞争窗口。 */
			/* 与 clear_bit 后 barrier 配对，避免消费者清位后漏看此前已加入的对象。 */
			set_bit(shrinker_id_to_offset(shrinker_id), unit->map);
		}
		rcu_read_unlock();
	}
}

static DEFINE_IDR(shrinker_idr);

static int shrinker_memcg_alloc(struct shrinker *shrinker)
{
	/* 分配 ID、必要时扩全部 memcg info，任一步失败都会撤销 ID 防止死 bitmap 位。 */
	/* 分配 ID 并确保所有 memcg 的 info 容纳它；失败时撤销 ID，不能留下可遍历空洞。 */
	int id;

	if (mem_cgroup_disabled())
		/* 编译有 memcg 但启动禁用时回 -ENOSYS，调用者据此降级全局 deferred 数组。 */
		return -ENOSYS;
	if (mem_cgroup_kmem_disabled() && !(shrinker->flags & SHRINKER_NONSLAB))
		/* 非 slab shrinker 仍可在 kmem 禁用时参与 cgroup reclaim，普通 slab shrinker 不可。 */
		return -ENOSYS;

	guard(mutex)(&shrinker_mutex);
	id = idr_alloc(&shrinker_idr, shrinker, 0, 0, GFP_KERNEL);
	if (id < 0)
		/* IDR 失败不触发扩容，错误码直接返回给 shrinker_alloc 回滚名称/主体。 */
		return id;

	if (id >= shrinker_nr_max) {
		/* 新 ID 超过已发布容量前必须原子地扩展所有 cgroup，之后才可返回成功。 */
		if (expand_shrinker_info(id)) {
			idr_remove(&shrinker_idr, id);
			return -ENOMEM;
		}
	}
	shrinker->id = id;
	return 0;
}

static void shrinker_memcg_remove(struct shrinker *shrinker)
{
	/* 移除 IDR 后 bitmap 旧位至多产生一次空查找，不会引用已释放 shrinker。 */
	/* unregister 锁内从 IDR 摘除，后续 memcg bitmap 即使有残位也无法再取得对象。 */
	int id = shrinker->id;

	BUG_ON(id < 0);

	lockdep_assert_held(&shrinker_mutex);

	idr_remove(&shrinker_idr, id);
}

static long xchg_nr_deferred_memcg(int nid, struct shrinker *shrinker,
				   struct mem_cgroup *memcg)
{
	/* 原子交换领取历史扫描债务，使并发 reclaimer 不会重复完成同一预算。 */
	/* 原子交换领取该 memcg/node 历史欠账，使并发 reclaimers 不会重复扫描同一额度。 */
	struct shrinker_info *info;
	struct shrinker_info_unit *unit;
	long nr_deferred;

	rcu_read_lock();
	/* RCU 保护 info 的 COW 替换；原子 xchg 后才离开读侧，确保 unit 地址有效。 */
	info = rcu_dereference(memcg->nodeinfo[nid]->shrinker_info);
	unit = info->unit[shrinker_id_to_index(shrinker->id)];
	nr_deferred = atomic_long_xchg(&unit->nr_deferred[shrinker_id_to_offset(shrinker->id)], 0);
	/* 领取动作清零共享欠账，让并发 reclaim 将新增工作累计到下一轮而非重复消费。 */
	rcu_read_unlock();

	return nr_deferred;
}

static long add_nr_deferred_memcg(long nr, int nid, struct shrinker *shrinker,
				  struct mem_cgroup *memcg)
{
	/* 未完成扫描重新累加；返回新总值仅用于 trace，不是强一致 reclaim 预算。 */
	struct shrinker_info *info;
	struct shrinker_info_unit *unit;
	long nr_deferred;

	rcu_read_lock();
	info = rcu_dereference(memcg->nodeinfo[nid]->shrinker_info);
	unit = info->unit[shrinker_id_to_index(shrinker->id)];
	nr_deferred =
		atomic_long_add_return(nr, &unit->nr_deferred[shrinker_id_to_offset(shrinker->id)]);
	/* 返回的新债务仅作 trace；精确性由 atomic add 保证，而非由返回值使用者保证。 */
	rcu_read_unlock();

	return nr_deferred;
}

void reparent_shrinker_deferred(struct mem_cgroup *memcg)
{
	/* cgroup 离线时把 child 的未完成扫描额度汇入 parent，内存压力不会丢失。 */
	/* cgroup 离线时把 child 未完成工作并入 parent，保持 reclaim 压力不会凭空消失。 */
	int nid, index, offset;
	long nr;
	struct mem_cgroup *parent = parent_mem_cgroup(memcg);
	struct shrinker_info *child_info, *parent_info;
	struct shrinker_info_unit *child_unit, *parent_unit;

	/* Prevent from concurrent shrinker_info expand */
	mutex_lock(&shrinker_mutex);
	/* reparent 和扩容用同一锁，使 child/parent 的 unit 数量和对应 ID 均稳定。 */
	for_each_node(nid) {
		child_info = shrinker_info_protected(memcg, nid);
		parent_info = shrinker_info_protected(parent, nid);
		for (index = 0; index < shrinker_id_to_index(child_info->map_nr_max); index++) {
			/* 逐 unit、逐 offset 合并未完成扫描，不依赖 bitmap 是否当前置位。 */
			child_unit = child_info->unit[index];
			parent_unit = parent_info->unit[index];
			for (offset = 0; offset < SHRINKER_UNIT_BITS; offset++) {
				/* 先读 child 再原子加入 parent；child 随后会整体销毁，迁移不要求清零它。 */
				nr = atomic_long_read(&child_unit->nr_deferred[offset]);
				atomic_long_add(nr, &parent_unit->nr_deferred[offset]);
			}
		}
	}
	mutex_unlock(&shrinker_mutex);
}
#else
/* feature-off 桩刻意不修改任何状态；全局 shrink 路径会自动承担工作。 */
/* 无 MEMCG 的桩让上层保留统一调用流；全局 nr_deferred 数组承担全部欠账。 */
static int shrinker_memcg_alloc(struct shrinker *shrinker)
{
	/* 配置关闭时明确告知构造器改用非 memcg 路径，而非悄悄成功。 */
	return -ENOSYS;
}

static void shrinker_memcg_remove(struct shrinker *shrinker)
{
	/* 没有 IDR/bitmap 状态可撤销，此桩保持销毁路径可无条件调用。 */
}

static long xchg_nr_deferred_memcg(int nid, struct shrinker *shrinker,
				   struct mem_cgroup *memcg)
{
	/* 返回零使全局路由永远不会从未编译的 memcg 存储领取债务。 */
	return 0;
}

static long add_nr_deferred_memcg(long nr, int nid, struct shrinker *shrinker,
				  struct mem_cgroup *memcg)
{
	/* 同样不记录债务，调用者的 flags/配置路由应当避免实际抵达此桩。 */
	return 0;
}
#endif /* CONFIG_MEMCG */

static long xchg_nr_deferred(struct shrinker *shrinker,
			     struct shrink_control *sc)
{
	/* 非 NUMA shrinker 把所有节点归并到 0；memcg-aware 则转入 cgroup 私有计数。 */
	/* 非 NUMA shrinker 强制聚合到 node 0；memcg-aware 时改走每 cgroup 的原子计数。 */
	int nid = sc->nid;

	if (!(shrinker->flags & SHRINKER_NUMA_AWARE))
		/* 未声明 NUMA aware 的 callback 只需维护 nr_deferred[0]。 */
		nid = 0;

	if (sc->memcg &&
	    (shrinker->flags & SHRINKER_MEMCG_AWARE))
		return xchg_nr_deferred_memcg(nid, shrinker,
					      sc->memcg);

	return atomic_long_xchg(&shrinker->nr_deferred[nid], 0);
}

/* 后面的 add 必须复用完全相同的 cgroup/node 路由，否则欠账会写入永不领取的槽。 */


static long add_nr_deferred(long nr, struct shrinker *shrinker,
			    struct shrink_control *sc)
{
	/* 已扫描但未完成的预算通过原子加回共享槽，不能用普通 store 覆盖并发新增。 */
	/* 与 xchg 成对：处理后把剩余扫描债务以原子加法归还，保留并发生产更新。 */
	int nid = sc->nid;

	if (!(shrinker->flags & SHRINKER_NUMA_AWARE))
		/* 该归一化与 xchg 对称，确保 non-NUMA shrinker 的并发债务集中。 */
		nid = 0;

	if (sc->memcg &&
	    (shrinker->flags & SHRINKER_MEMCG_AWARE))
		return add_nr_deferred_memcg(nr, nid, shrinker,
					     sc->memcg);

	return atomic_long_add_return(nr, &shrinker->nr_deferred[nid]);
}

/* SHRINK_BATCH 下的循环始终允许 callback 修改 sc->nr_scanned，预算以反馈值为准。 */

#define SHRINK_BATCH 128
/* 默认批大小平衡 callback 开销与调度延迟；具体 shrinker 可通过 batch 覆盖。 */

static unsigned long do_shrink_slab(struct shrink_control *shrinkctl,
				    struct shrinker *shrinker, int priority)
{
	/* 预算器负责 count、领取 deferred、按 priority/seeks 计算扫描、回填未完成债务。 */
	/* 单 shrinker 的预算器：count→领取 deferred→按 seeks/priority 算扫描→回填未完成欠账。 */
	unsigned long freed = 0;
	unsigned long long delta;
	long total_scan;
	long freeable;
	long nr;
	long new_nr;
	long batch_size = shrinker->batch ? shrinker->batch
					  : SHRINK_BATCH;
	/* 自定义 batch 是 shrinker 对 callback 颗粒度的提示，非硬性释放目标。 */
	long scanned = 0, next_deferred;

	freeable = shrinker->count_objects(shrinker, shrinkctl);
	/* count_objects 是估算而非锁定；callback 扫描期间对象可新增/删除，预算允许近似。 */
	/* SHRINK_EMPTY 是“确定无对象”的特殊返回，调用者据此清 memcg bitmap；0 不等价。 */
	if (freeable == 0 || freeable == SHRINK_EMPTY)
		/* SHRINK_EMPTY 携带清 bitmap 语义，不能和“本次 count 为零”混为一谈。 */
		return freeable;

	/*
	 * copy the current shrinker scan count into a local variable
	 * and zero it so that other concurrent shrinker invocations
	 * don't also do this scanning work.
	 */
	nr = xchg_nr_deferred(shrinker, shrinkctl);
	/* 领取后局部 nr 独占，其他并发调用会把新债务累加到已归零的共享计数。 */

	if (shrinker->seeks) {
		/* seeks 近似重新构造对象所需 IO；较小值允许较积极地把 freeable 转为扫描增量。 */
		/* seeks 越低代表重建代价低，delta 越大；priority 越高则对 freeable 右移更激进。 */
		delta = freeable >> priority;
		delta *= 4;
		do_div(delta, shrinker->seeks);
	} else {
		/* 无 IO 重建代价的对象倾向快速淘汰，以降低在紧张内存下缓存反复挤占。 */
		/*
		 * These objects don't require any IO to create. Trim
		 * them aggressively under memory pressure to keep
		 * them from causing refetches in the IO caches.
		 */
		delta = freeable / 2;
	}

	total_scan = nr >> priority;
	total_scan += delta;
	total_scan = min(total_scan, (2 * freeable));

	trace_mm_shrink_slab_start(shrinker, shrinkctl, nr,
				   freeable, delta, total_scan, priority,
				   shrinkctl->memcg);
	/* trace 记录预算形成原因，便于区分 count 波动、priority 与旧 deferred 的影响。 */

	/*
	 * Normally, we should not scan less than batch_size objects in one
	 * pass to avoid too frequent shrinker calls, but if the slab has less
	 * than batch_size objects in total and we are really tight on memory,
	 * we will try to reclaim all available objects, otherwise we can end
	 * up failing allocations although there are plenty of reclaimable
	 * objects spread over several slabs with usage less than the
	 * batch_size.
	 *
	 * We detect the "tight on memory" situations by looking at the total
	 * number of objects we want to scan (total_scan). If it is greater
	 * than the total number of objects on slab (freeable), we must be
	 * scanning at high prio and therefore should try to reclaim as much as
	 * possible.
	 */
	while (total_scan >= batch_size ||
	       total_scan >= freeable) {
		unsigned long ret;
		unsigned long nr_to_scan = min(batch_size, total_scan);
		/* 每次 callback 至多请求一批，减少长时间占用 CPU 并给其他 shrinker 机会。 */

		shrinkctl->nr_to_scan = nr_to_scan;
		/* sc 是每个调用的栈对象，callback 不可在返回后保存其地址。 */
		/* callback 可下调 nr_scanned，预算扣减必须信任其实际扫描量而非请求量。 */
		shrinkctl->nr_scanned = nr_to_scan;
		ret = shrinker->scan_objects(shrinker, shrinkctl);
		if (ret == SHRINK_STOP)
			/* STOP 留下未完成 total_scan，后续通过 deferred 回填而不是假报 empty。 */
			/* STOP 表示本轮停止而非“空”，未执行预算会通过 next_deferred 保留。 */
			break;
		freed += ret;

		count_vm_events(SLABS_SCANNED, shrinkctl->nr_scanned);
		/* 统计实际扫描量；callback 可少于 nr_to_scan，故不能用请求量扣预算。 */
		total_scan -= shrinkctl->nr_scanned;
		scanned += shrinkctl->nr_scanned;

		cond_resched();
		/* 此处无 RCU 读锁和 shrinker_list 锁；callback 可安全阻塞或让出 CPU。 */
		/* shrinker callback 可耗时，批间让出 CPU 但不持有全局 list/memcg RCU 锁。 */
	}

	/*
	 * The deferred work is increased by any new work (delta) that wasn't
	 * done, decreased by old deferred work that was done now.
	 *
	 * And it is capped to two times of the freeable items.
	 */
	next_deferred = max_t(long, (nr + delta - scanned), 0);
	/* 上界为两倍 freeable，防止对象数骤减后陈旧欠账无限累积。 */
	next_deferred = min(next_deferred, (2 * freeable));
	/* 上限按当前可回收量自适应，避免陈旧债务让下一次低优先级 scan 失控。 */

	/*
	 * move the unused scan count back into the shrinker in a
	 * manner that handles concurrent updates.
	 */
	new_nr = add_nr_deferred(next_deferred, shrinker, shrinkctl);

	trace_mm_shrink_slab_end(shrinker, shrinkctl->nid, freed, nr, new_nr, total_scan,
				 shrinkctl->memcg);
	return freed;
}

#ifdef CONFIG_MEMCG
static unsigned long shrink_slab_memcg(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority)
{
	/* memcg 路径按 bitmap 只访问曾被置位的 shrinker；RCU 与 refcount 分离保护 info 和 shrinker。 */
	struct shrinker_info *info;
	unsigned long ret, freed = 0;
	int offset, index = 0;

	if (!mem_cgroup_online(memcg))
		/* 离线 cgroup 不再参与 reclaim，避免对正在拆除的 per-node info 取引用。 */
		return 0;

	/*
	 * lockless algorithm of memcg shrink.
	 *
	 * The shrinker_info may be freed asynchronously via RCU in the
	 * expand_one_shrinker_info(), so the rcu_read_lock() needs to be used
	 * to ensure the existence of the shrinker_info.
	 *
	 * The shrinker_info_unit is never freed unless its corresponding memcg
	 * is destroyed. Here we already hold the refcount of memcg, so the
	 * memcg will not be destroyed, and of course shrinker_info_unit will
	 * not be freed.
	 *
	 * So in the memcg shrink:
	 *  step 1: use rcu_read_lock() to guarantee existence of the
	 *          shrinker_info.
	 *  step 2: after getting shrinker_info_unit we can safely release the
	 *          RCU lock.
	 *  step 3: traverse the bitmap and calculate shrinker_id
	 *  step 4: use rcu_read_lock() to guarantee existence of the shrinker.
	 *  step 5: use shrinker_id to find the shrinker, then use
	 *          shrinker_try_get() to guarantee existence of the shrinker,
	 *          then we can release the RCU lock to do do_shrink_slab() that
	 *          may sleep.
	 *  step 6: do shrinker_put() paired with step 5 to put the refcount,
	 *          if the refcount reaches 0, then wake up the waiter in
	 *          shrinker_free() by calling complete().
	 *          Note: here is different from the global shrink, we don't
	 *                need to acquire the RCU lock to guarantee existence of
	 *                the shrinker, because we don't need to use this
	 *                shrinker to traverse the next shrinker in the bitmap.
	 *  step 7: we have already exited the read-side of rcu critical section
	 *          before calling do_shrink_slab(), the shrinker_info may be
	 *          released in expand_one_shrinker_info(), so go back to step 1
	 *          to reacquire the shrinker_info.
	 */
again:
	/* 每处理一个 unit 都重新 RCU 获取 info，因为扩容可在 callback 睡眠期间替换它。 */
	rcu_read_lock();
	info = rcu_dereference(memcg->nodeinfo[nid]->shrinker_info);
	if (unlikely(!info))
		/* 尚未初始化或刚离线时无 bitmap，安全返回当前已累计 freed。 */
		goto unlock;

	if (index < shrinker_id_to_index(info->map_nr_max)) {
		struct shrinker_info_unit *unit;

		unit = info->unit[index];
		/* unit 一旦从存活 memcg 的 info 取出即不会单独释放，可脱离 RCU 遍历其 bitmap。 */

		rcu_read_unlock();

		for_each_set_bit(offset, unit->map, SHRINKER_UNIT_BITS) {
			/* 每个置位表示此 shrinker 可能对当前 memcg/node 有对象，虚假正例允许但漏例不允许。 */
			struct shrink_control sc = {
				.gfp_mask = gfp_mask,
				.nid = nid,
				.memcg = memcg,
			};
			struct shrinker *shrinker;
			int shrinker_id = calc_shrinker_id(index, offset);

			rcu_read_lock();
			shrinker = idr_find(&shrinker_idr, shrinker_id);
			/* IDR 查找受 RCU/注册锁发布规则保护，try_get 成功后 callback 可睡眠。 */
			if (unlikely(!shrinker || !shrinker_try_get(shrinker))) {
				clear_bit(offset, unit->map);
				rcu_read_unlock();
				continue;
			}
			rcu_read_unlock();

			/* Call non-slab shrinkers even though kmem is disabled */
			if (!memcg_kmem_online() &&
				/* kmem off 时清除普通 slab 位；NONSLAB 仍是该 cgroup 的有效回收者。 */
			    !(shrinker->flags & SHRINKER_NONSLAB)) {
				clear_bit(offset, unit->map);
				shrinker_put(shrinker);
				continue;
			}

			ret = do_shrink_slab(&sc, shrinker, priority);
			if (ret == SHRINK_EMPTY) {
				/* 先清位降低后续空扫描；随后 barrier+复查关闭“生产者恰好并发置位”的窗口。 */
				clear_bit(offset, unit->map);
				/*
				 * After the shrinker reported that it had no objects to
				 * free, but before we cleared the corresponding bit in
				 * the memcg shrinker map, a new object might have been
				 * added. To make sure, we have the bit set in this
				 * case, we invoke the shrinker one more time and reset
				 * the bit if it reports that it is not empty anymore.
				 * The memory barrier here pairs with the barrier in
				 * set_shrinker_bit():
				 *
				 * list_lru_add()     shrink_slab_memcg()
				 *   list_add_tail()    clear_bit()
				 *   <MB>               <MB>
				 *   set_bit()          do_shrink_slab()
				 */
				smp_mb__after_atomic();
				/* 与 set_shrinker_bit 的 before_atomic 建立全序，保证新对象不会被永久遗漏。 */
				ret = do_shrink_slab(&sc, shrinker, priority);
				if (ret == SHRINK_EMPTY)
					ret = 0;
				else
					set_shrinker_bit(memcg, nid, shrinker_id);
			}
			freed += ret;
			/* 每个 shrinker 释放量仅统计对象数；实际页数/字节由各 callback 语义决定。 */
			shrinker_put(shrinker);
		}

		index++;
		goto again;
	}
unlock:
	rcu_read_unlock();
	return freed;
}
#else /* !CONFIG_MEMCG */
/* 编译关闭 memcg 时根路径永远走全局列表，此桩不应被当作错误或回收失败。 */
static unsigned long shrink_slab_memcg(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority)
{
	return 0;
}
#endif /* CONFIG_MEMCG */

/**
 * shrink_slab - shrink slab caches
 * @gfp_mask: allocation context
 * @nid: node whose slab caches to target
 * @memcg: memory cgroup whose slab caches to target
 * @priority: the reclaim priority
 *
 * Call the shrink functions to age shrinkable caches.
 *
 * @nid is passed along to shrinkers with SHRINKER_NUMA_AWARE set,
 * unaware shrinkers will receive a node id of 0 instead.
 *
 * @memcg specifies the memory cgroup to target. Unaware shrinkers
 * are called only if it is the root cgroup.
 *
 * @priority is sc->priority, we take the number of objects and >> by priority
 * in order to get the scan target.
 *
 * Returns the number of reclaimed slab objects.
 */
unsigned long shrink_slab(gfp_t gfp_mask, int nid, struct mem_cgroup *memcg,
			  int priority)
{
	/* 顶层入口在 root 走全局 RCU list，在非 root 走 memcg bitmap；二者都最终调用同一预算器。 */
	unsigned long ret, freed = 0;
	struct shrinker *shrinker;

	/*
	 * The root memcg might be allocated even though memcg is disabled
	 * via "cgroup_disable=memory" boot parameter.  This could make
	 * mem_cgroup_is_root() return false, then just run memcg slab
	 * shrink, but skip global shrink.  This may result in premature
	 * oom.
	 */
	if (!mem_cgroup_disabled() && !mem_cgroup_is_root(memcg))
		/* root 对象即便 memcg 编译开启也不可误走 bitmap，否则全局 shrinker 会被跳过。 */
		return shrink_slab_memcg(gfp_mask, nid, memcg, priority);

	/*
	 * lockless algorithm of global shrink.
	 *
	 * In the unregistration setp, the shrinker will be freed asynchronously
	 * via RCU after its refcount reaches 0. So both rcu_read_lock() and
	 * shrinker_try_get() can be used to ensure the existence of the shrinker.
	 *
	 * So in the global shrink:
	 *  step 1: use rcu_read_lock() to guarantee existence of the shrinker
	 *          and the validity of the shrinker_list walk.
	 *  step 2: use shrinker_try_get() to try get the refcount, if successful,
	 *          then the existence of the shrinker can also be guaranteed,
	 *          so we can release the RCU lock to do do_shrink_slab() that
	 *          may sleep.
	 *  step 3: *MUST* to reacquire the RCU lock before calling shrinker_put(),
	 *          which ensures that neither this shrinker nor the next shrinker
	 *          will be freed in the next traversal operation.
	 *  step 4: do shrinker_put() paired with step 2 to put the refcount,
	 *          if the refcount reaches 0, then wake up the waiter in
	 *          shrinker_free() by calling complete().
	 */
	rcu_read_lock();
	/* 遍历期间 list 节点不释放；每次调用前再 try_get，将对象存活期扩到 RCU 锁外。 */
	list_for_each_entry_rcu(shrinker, &shrinker_list, list) {
		struct shrink_control sc = {
			.gfp_mask = gfp_mask,
			.nid = nid,
			.memcg = memcg,
		};

		if (!shrinker_try_get(shrinker))
			/* 已进入 free 的 shrinker 拒绝新引用，直接继续 RCU 安全的下一节点。 */
			continue;

		rcu_read_unlock();

		ret = do_shrink_slab(&sc, shrinker, priority);
		/* callback 允许调度，所以先放 RCU；结束后重新取得 RCU 再 put/继续遍历。 */
		if (ret == SHRINK_EMPTY)
			ret = 0;
		freed += ret;

		rcu_read_lock();
		shrinker_put(shrinker);
		/* put 可能唤醒 shrinker_free 等待者，但 RCU 锁保证当前迭代器仍安全。 */
	}

	rcu_read_unlock();
	cond_resched();
	return freed;
}

struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	/* 动态 shrinker 的构造事务：对象→debug 名→flags/ID→deferred 数组，失败按逆序销毁。 */
	struct shrinker *shrinker;
	unsigned int size;
	va_list ap;
	int err;

	shrinker = kzalloc_obj(struct shrinker);
	/* 零化使未启用字段（list/refcount 等）在 register 前保持可预测初始值。 */
	if (!shrinker)
		return NULL;

	va_start(ap, fmt);
	err = shrinker_debugfs_name_alloc(shrinker, fmt, ap);
	va_end(ap);
	if (err)
		goto err_name;

	shrinker->flags = flags | SHRINKER_ALLOCATED;
	shrinker->seeks = DEFAULT_SEEKS;

	if (flags & SHRINKER_MEMCG_AWARE) {
		/* 请求 memcg 支持时优先领取 ID；平台不支持只降级标志，不视作分配失败。 */
		err = shrinker_memcg_alloc(shrinker);
		if (err == -ENOSYS) {
			/* Memcg is not supported, fallback to non-memcg-aware shrinker. */
			shrinker->flags &= ~SHRINKER_MEMCG_AWARE;
			goto non_memcg;
		}

		if (err)
			goto err_flags;

		return shrinker;
	}

non_memcg:
	/* 非 memcg 路径由 shrinker 自己拥有每节点 deferred 数组，NUMA-aware 才扩展为多槽。 */
	/*
	 * The nr_deferred is available on per memcg level for memcg aware
	 * shrinkers, so only allocate nr_deferred in the following cases:
	 *  - non-memcg-aware shrinkers
	 *  - !CONFIG_MEMCG
	 *  - memcg is disabled by kernel command line
	 *  - non-slab shrinkers: when memcg kmem is disabled
	 */
	size = sizeof(*shrinker->nr_deferred);
	if (flags & SHRINKER_NUMA_AWARE)
		size *= nr_node_ids;

	shrinker->nr_deferred = kzalloc(size, GFP_KERNEL);
	if (!shrinker->nr_deferred)
		goto err_flags;

	return shrinker;

err_flags:
	/* name 与主体分开释放，确保 debugfs 名称分配失败/后续失败均无泄漏。 */
	shrinker_debugfs_name_free(shrinker);
err_name:
	kfree(shrinker);
	return NULL;
}
EXPORT_SYMBOL_GPL(shrinker_alloc);

void shrinker_register(struct shrinker *shrinker)
{
	/* 注册的发布顺序很关键：先 RCU 入链与 debugfs，最后 refcount=1 开放 try_get。 */
	if (unlikely(!(shrinker->flags & SHRINKER_ALLOCATED))) {
		pr_warn("Must use shrinker_alloc() to dynamically allocate the shrinker");
		return;
	}

	mutex_lock(&shrinker_mutex);
	list_add_tail_rcu(&shrinker->list, &shrinker_list);
	/* mutex 阻止与 free/remove 并发；RCU 使已有 reclaim 遍历器仍可看到完整节点。 */
	shrinker->flags |= SHRINKER_REGISTERED;
	shrinker_debugfs_add(shrinker);
	mutex_unlock(&shrinker_mutex);

	init_completion(&shrinker->done);
	/* done 在 initial ref 被释放、所有 callback 引用归零时完成，供 unregister 等待。 */
	/*
	 * Now the shrinker is fully set up, take the first reference to it to
	 * indicate that lookup operations are now allowed to use it via
	 * shrinker_try_get().
	 */
	refcount_set(&shrinker->refcount, 1);
}
EXPORT_SYMBOL_GPL(shrinker_register);

static void shrinker_free_rcu_cb(struct rcu_head *head)
{
	/* grace period 后没有 list 读者能解引用 shrinker，才可释放 deferred 和主体。 */
	struct shrinker *shrinker = container_of(head, struct shrinker, rcu);

	kfree(shrinker->nr_deferred);
	kfree(shrinker);
}

void shrinker_free(struct shrinker *shrinker)
{
	/* 销毁按“禁止新引用→等待现存引用→RCU 摘链→异步物理释放”分阶段完成。 */
	struct dentry *debugfs_entry = NULL;
	int debugfs_id;

	if (!shrinker)
		/* 允许无条件 cleanup 调用，空指针没有资源或同步语义。 */
		return;

	if (shrinker->flags & SHRINKER_REGISTERED) {
		/* 先 drop 注册时的初始引用，让最后一个活动 shrinker_put 完成 done。 */
		/* drop the initial refcount */
		shrinker_put(shrinker);
		/*
		 * Wait for all lookups of the shrinker to complete, after that,
		 * no shrinker is running or will run again, then we can safely
		 * free it asynchronously via RCU and safely free the structure
		 * where the shrinker is located, such as super_block etc.
		 */
		wait_for_completion(&shrinker->done);
		/* 返回后保证不会再进入 callback；但 RCU 遍历器仍可持有 list 节点，故随后 list_del_rcu。 */
	}

	mutex_lock(&shrinker_mutex);
	if (shrinker->flags & SHRINKER_REGISTERED) {
		/* 在 mutex 下摘链和 IDR，防止新注册/扩容与销毁交错。 */
		/*
		 * Now we can safely remove it from the shrinker_list and then
		 * free it.
		 */
		list_del_rcu(&shrinker->list);
		debugfs_entry = shrinker_debugfs_detach(shrinker, &debugfs_id);
		shrinker->flags &= ~SHRINKER_REGISTERED;
	}

	shrinker_debugfs_name_free(shrinker);

	if (shrinker->flags & SHRINKER_MEMCG_AWARE)
		shrinker_memcg_remove(shrinker);
	mutex_unlock(&shrinker_mutex);

	if (debugfs_entry)
		/* debugfs 删除放锁外，避免文件系统路径与 shrinker_mutex 形成锁依赖。 */
		shrinker_debugfs_remove(debugfs_entry, debugfs_id);

	call_rcu(&shrinker->rcu, shrinker_free_rcu_cb);
	/* 调用者可紧接着释放包裹该 shrinker 的宿主，只要宿主自身不再被 RCU 读者访问。 */
}
EXPORT_SYMBOL_GPL(shrinker_free);
