// SPDX-License-Identifier: GPL-2.0-only
/*
 * HugeTLB sysfs interfaces.
 * (C) Nadia Yvette Chambers, April 2004
 */
/* HugeTLB 的 sysfs 控制面：按页大小及可选 NUMA 节点展示并调整 huge page 池。 */

#include <linux/swap.h>
#include <linux/page_owner.h>
#include <linux/page-isolation.h>

#include "hugetlb_vmemmap.h"
#include "hugetlb_internal.h"

/*
 * 三个生成宏分别声明只读、只写和读写 kobj_attribute；__ATTR_* 按拼接出的
 * name_show/name_store 回调建立 mode 与函数指针，不分配对象，属性均为静态寿命。
 */
#define HSTATE_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

#define HSTATE_ATTR_WO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_WO(_name)

#define HSTATE_ATTR(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RW(_name)

/* 全局 /sys/kernel/mm/hugepages 目录；init 创建后由内核 kobject 层长期持有。 */
static struct kobject *hugepages_kobj;
/* 以 hstate_index 为下标保存每种页大小子目录，供属性回调反查 hstate。 */
static struct kobject *hstate_kobjs[HUGE_MAX_HSTATE];

/* NUMA 配置实现节点目录反查；非 NUMA 配置提供不可达 BUG 桩。 */
static struct hstate *kobj_to_node_hstate(struct kobject *kobj, int *nidp);

/*
 * 业务背景：sysfs 回调只收到 kobject，本函数把全局或 per-node 页大小目录
 * 反解为共享 hstate，并可输出节点身份。
 * 入参：kobj 是已注册 hstate 目录的借用指针；nidp 可为 NULL，否则是输出指针。
 * 出参/返回：返回静态 hstates[] 中的借用指针；全局目录写 NUMA_NO_NODE，节点
 * 目录由 kobj_to_node_hstate 写实际 nid；不取得引用。
 * 注意事项：sysfs/kobject 生命周期保证输入稳定，不睡眠；未知全局对象继续
 * 节点查找，最终未知对象在节点实现中 BUG，调用者不得传任意 kobject。
 */
static struct hstate *kobj_to_hstate(struct kobject *kobj, int *nidp)
{
	/* i 是全局页大小目录索引。 */
	int i;

	/* 先查全局并把节点哨兵输出；失败再进入配置相关节点目录反查。 */
	for (i = 0; i < HUGE_MAX_HSTATE; i++)
		if (hstate_kobjs[i] == kobj) {
			if (nidp)
				*nidp = NUMA_NO_NODE;
			return &hstates[i];
		}

	return kobj_to_node_hstate(kobj, nidp);
}

/*
 * 业务背景：为全局/节点 nr_hugepages 只读属性复用计数选择和格式化逻辑。
 * 入参：kobj 标识页大小及可选节点；attr 为 sysfs 借用属性且本实现不读取；
 * buf 是 PAGE_SIZE 级输出缓冲区，ownership 留给 sysfs。
 * 出参/返回：返回写入字节数；输出当前池 huge page 总数或指定节点数。
 * 注意事项：读取是统计快照且未取 hugetlb_lock，可能与 resize 并发变化；
 * sysfs_emit 保证边界和换行，不会睡眠于本函数自身。
 */
static ssize_t nr_hugepages_show_common(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	/* h/nid 解析目录身份，nr_huge_pages 保存本次无锁计数快照。 */
	struct hstate *h;
	unsigned long nr_huge_pages;
	int nid;

	/* NUMA_NO_NODE 选择全局计数，否则选择同一 hstate 的节点数组槽。 */
	h = kobj_to_hstate(kobj, &nid);
	/* 目录反查输出节点哨兵，据此选择全局或该节点的当前池计数。 */
	if (nid == NUMA_NO_NODE)
		nr_huge_pages = h->nr_huge_pages;
	else
		nr_huge_pages = h->nr_huge_pages_node[nid];

	return sysfs_emit(buf, "%lu\n", nr_huge_pages);
}

/*
 * 业务背景：统一解析全局、节点及 mempolicy 版本的持久池目标值，再委托
 * HugeTLB 核心执行可能分配/释放页的 resize 协议。
 * 入参：obey_mempolicy 决定全局调整是否服从 current policy；kobj 标识 hstate/
 * 节点；buf 是长度至少 len 的用户文本借用缓冲；len 为写入字节数。
 * 出参/返回：解析失败返回负 errno；否则原样返回核心 helper 的负 errno 或 len。
 * 注意事项：可睡眠；核心 helper 负责 resize_lock/hugetlb_lock、部分调整和
 * ownership，属性层不持锁也不缓存 h/nid。
 */
static ssize_t nr_hugepages_store_common(bool obey_mempolicy,
					 struct kobject *kobj, const char *buf,
					 size_t len)
{
	/* h/nid 定位目标；count 为十进制页数；err 传递解析错误。 */
	struct hstate *h;
	unsigned long count;
	int nid;
	int err;

	/* 必须完整解析无符号十进制，非法/溢出在接触池状态前失败。 */
	err = kstrtoul(buf, 10, &count);
	if (err)
		return err;

	/* 核心返回值保留 sysfs 成功字节数或具体调整错误。 */
	h = kobj_to_hstate(kobj, &nid);
	return __nr_hugepages_store_common(obey_mempolicy, h, nid, count, len);
}

/*
 * 业务背景：nr_hugepages 属性的 show 适配器，读取全局或节点当前池数量。
 * 入参：kobj/attr/buf 均为 sysfs 借用参数，语义同 common helper。
 * 出参/返回：返回格式化字节数，不改变 ownership 或池状态。
 * 注意事项：无锁快照，可能随并发 resize 变化；不额外睡眠。
 */
static ssize_t nr_hugepages_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return nr_hugepages_show_common(kobj, attr, buf);
}

/*
 * 业务背景：普通 nr_hugepages 写入口，不采用 current 的 NUMA mempolicy。
 * 入参：kobj/attr/buf/len 为 sysfs 借用参数，len 是输入字节数。
 * 出参/返回：成功返回 len，失败负 errno；副作用由 common/core 调整持久池。
 * 注意事项：可睡眠；节点目录仍天然约束到该 nid。
 */
static ssize_t nr_hugepages_store(struct kobject *kobj,
	       struct kobj_attribute *attr, const char *buf, size_t len)
{
	return nr_hugepages_store_common(false, kobj, buf, len);
}
HSTATE_ATTR(nr_hugepages);

#ifdef CONFIG_NUMA

/*
 * hstate attribute for optionally mempolicy-based constraint on persistent
 * huge page alloc/free.
 */
/* 该 hstate 属性让持久 huge page 增减可选地受当前任务 NUMA mempolicy 约束。 */
/*
 * 业务背景：NUMA 专用 nr_hugepages_mempolicy 的读取与普通目标计数相同。
 * 入参：kobj/attr/buf 是 sysfs 借用参数。
 * 出参/返回：返回全局/节点计数文本长度，无副作用。
 * 注意事项：仅 CONFIG_NUMA 编译；无锁快照可能并发变化。
 */
static ssize_t nr_hugepages_mempolicy_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return nr_hugepages_show_common(kobj, attr, buf);
}

/*
 * 业务背景：NUMA 用户要求按 current mempolicy 分配/释放持久 huge page 的入口。
 * 入参：kobj/attr/buf/len 为 sysfs 借用输入，len 为字节数。
 * 出参/返回：成功 len，失败负 errno；核心调整时 obey_mempolicy=true。
 * 注意事项：仅 CONFIG_NUMA，可能睡眠；policy 只影响全局目标的节点选择。
 */
static ssize_t nr_hugepages_mempolicy_store(struct kobject *kobj,
	       struct kobj_attribute *attr, const char *buf, size_t len)
{
	return nr_hugepages_store_common(true, kobj, buf, len);
}
HSTATE_ATTR(nr_hugepages_mempolicy);
#endif


/*
 * 业务背景：展示该 hstate 允许在 reservation 需求下临时超出持久池的上限。
 * 入参：kobj 标识全局 hstate；attr 未使用；buf 为 sysfs 输出缓冲。
 * 出参/返回：返回十进制页数文本长度，无副作用。
 * 注意事项：无锁统计快照；该属性不用于 per-node 组。
 */
static ssize_t nr_overcommit_hugepages_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct hstate *h = kobj_to_hstate(kobj, NULL);
	return sysfs_emit(buf, "%lu\n", h->nr_overcommit_huge_pages);
}

/*
 * 业务背景：更新 surplus huge page 可增长的全局上限，供 fault/reservation
 * 慢路径决定是否临时扩池。
 * 入参：kobj 标识 hstate；attr 未用；buf/count 是写入文本及字节数，均借用。
 * 出参/返回：成功返回 count；gigantic 不支持运行期分配或解析失败返回负 errno。
 * 注意事项：hugetlb_lock irq-safe 地与分配/回收计数竞争；只改策略上限，不会
 * 立即分配或释放页，持锁区不可睡眠。
 */
static ssize_t nr_overcommit_hugepages_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	/* input 是新页数上限；h 为静态 hstate 借用；err 传递解析失败。 */
	int err;
	unsigned long input;
	struct hstate *h = kobj_to_hstate(kobj, NULL);

	/* 架构不能运行期分配的 gigantic hstate 无法兑现 overcommit，明确拒绝。 */
	if (hstate_is_gigantic_no_runtime(h))
		return -EINVAL;

	err = kstrtoul(buf, 10, &input);
	if (err)
		return err;

	/* 该写入是外界可见提交点，与所有 hugetlb 池计数更新串行。 */
	spin_lock_irq(&hugetlb_lock);
	h->nr_overcommit_huge_pages = input;
	spin_unlock_irq(&hugetlb_lock);

	return count;
}
HSTATE_ATTR(nr_overcommit_hugepages);

/*
 * 业务背景：展示当前池中尚未被使用的 huge page 数，可按全局或节点观察。
 * 入参：kobj/attr/buf 为 sysfs 借用参数。
 * 出参/返回：返回 free 计数文本长度，无副作用。
 * 注意事项：无锁近似快照，不能与其他属性拼成原子一致的统计集合。
 */
static ssize_t free_hugepages_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	/* free_huge_pages 是全局/节点选择后的本次计数快照。 */
	struct hstate *h;
	unsigned long free_huge_pages;
	int nid;

	h = kobj_to_hstate(kobj, &nid);
	/* 目录反查输出节点哨兵，据此选择聚合 free 值或 per-node 数组。 */
	if (nid == NUMA_NO_NODE)
		free_huge_pages = h->free_huge_pages;
	else
		free_huge_pages = h->free_huge_pages_node[nid];

	return sysfs_emit(buf, "%lu\n", free_huge_pages);
}
HSTATE_ATTR_RO(free_hugepages);

/*
 * 业务背景：展示已承诺给 reservation、尚未实际 fault 消耗的 huge page 数。
 * 入参：kobj 标识全局 hstate；attr 未用；buf 为借用输出缓冲。
 * 出参/返回：返回 resv_huge_pages 文本长度，无副作用。
 * 注意事项：仅全局属性组暴露，且为无锁瞬时值。
 */
static ssize_t resv_hugepages_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct hstate *h = kobj_to_hstate(kobj, NULL);
	return sysfs_emit(buf, "%lu\n", h->resv_huge_pages);
}
HSTATE_ATTR_RO(resv_hugepages);

/*
 * 业务背景：展示超过持久目标、可在无需 reservation 后回收的 surplus 页数，
 * 支持全局和 per-node 视图。
 * 入参：kobj/attr/buf 为 sysfs 借用参数。
 * 出参/返回：返回选择后的 surplus 计数文本长度，无副作用。
 * 注意事项：无锁快照；全局 resv 不能按 node 扣分，因此仅展示实际 surplus。
 */
static ssize_t surplus_hugepages_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	/* surplus_huge_pages 与 nid 一起形成当前属性视图。 */
	struct hstate *h;
	unsigned long surplus_huge_pages;
	int nid;

	h = kobj_to_hstate(kobj, &nid);
	/* 与 free 视图相同，节点哨兵决定读取聚合 surplus 值或 per-node 数组。 */
	if (nid == NUMA_NO_NODE)
		surplus_huge_pages = h->surplus_huge_pages;
	else
		surplus_huge_pages = h->surplus_huge_pages_node[nid];

	return sysfs_emit(buf, "%lu\n", surplus_huge_pages);
}
HSTATE_ATTR_RO(surplus_hugepages);

/*
 * 业务背景：用户向 demote 写入页数，把空闲的大 hstate 页拆成其 demote_order
 * 指定的小 hstate 页，释放更细粒度容量而不归还普通 buddy。
 * 入参：kobj 标识全局或节点 hstate；attr 未用；buf/len 为页数文本及字节数。
 * 出参/返回：成功（包括可用页耗尽后的部分完成）返回 len；解析或核心 demote
 * 失败返回负 errno。不会 demote 已 reservation 的容量。
 * 注意事项：可睡眠，resize_lock 串行 sysfs resize/demote 配置，hugetlb_lock
 * irq-safe 保护池链表/计数；核心 helper 会暂时放开自旋锁，故每轮必须重读。
 */
static ssize_t demote_store(struct kobject *kobj,
	       struct kobj_attribute *attr, const char *buf, size_t len)
{
	/*
	 * nr_demote 是剩余目标页数；nr_available 是扣除全局 reservation 后快照；
	 * nodes_allowed/n_mask 选择单节点或所有内存节点；h/nid 定位目标；err 传错。
	 */
	unsigned long nr_demote;
	unsigned long nr_available;
	nodemask_t nodes_allowed, *n_mask;
	struct hstate *h;
	int err;
	int nid;

	/* 在取锁和修改任何池状态前解析无符号十进制请求。 */
	err = kstrtoul(buf, 10, &nr_demote);
	if (err)
		return err;
	h = kobj_to_hstate(kobj, &nid);

	/* 节点属性只允许从该 nid 拆页；全局属性允许所有 N_MEMORY 节点。 */
	if (nid != NUMA_NO_NODE) {
		init_nodemask_of_node(&nodes_allowed, nid);
		n_mask = &nodes_allowed;
	} else {
		n_mask = &node_states[N_MEMORY];
	}

	/* Synchronize with other sysfs operations modifying huge pages */
	/* resize_lock 与其他 sysfs 池调整串行，内层 hugetlb_lock 保护实际链表和计数。 */
	mutex_lock(&h->resize_lock);
	spin_lock_irq(&hugetlb_lock);

	/* 循环允许 helper 分批完成；每次成功数从剩余目标扣除。 */
	while (nr_demote) {
		long rc;

		/*
		 * Check for available pages to demote each time thorough the
		 * loop as demote_pool_huge_page will drop hugetlb_lock.
		 */
		/*
		 * demote_pool_huge_page 会放开 hugetlb_lock 完成长操作，其他线程可改变
		 * free/resv 计数，故每轮重新计算可安全拆分的未保留页，不能复用旧快照。
		 */
		if (nid != NUMA_NO_NODE)
			nr_available = h->free_huge_pages_node[nid];
		else
			nr_available = h->free_huge_pages;
		nr_available -= h->resv_huge_pages;
		if (!nr_available)
			break;

		/* rc<0 是不可继续的错误；非负值是本轮实际 demote 的大页数量。 */
		rc = demote_pool_huge_page(h, n_mask, nr_demote);
		if (rc < 0) {
			err = rc;
			break;
		}

		nr_demote -= rc;
	}

	/* 所有出口按自旋锁后互斥锁的逆序释放。 */
	spin_unlock_irq(&hugetlb_lock);
	mutex_unlock(&h->resize_lock);

	/* 核心错误覆盖 sysfs 成功；无错误时即使部分完成也消费整次写入。 */
	if (err)
		return err;
	return len;
}
HSTATE_ATTR_WO(demote);

/*
 * 业务背景：展示当前大页 demote 后生成的目标页大小，供用户在执行前确认。
 * 入参：kobj 标识 hstate；attr 未用；buf 为借用输出缓冲。
 * 出参/返回：返回以 kB 表示的目标大小文本长度，无副作用。
 * 注意事项：读取 demote_order 未取 resize_lock，是容许并发写造成前后快照变化
 * 的 sysfs 观察；仅对创建了 demote 属性组的 hstate 可达。
 */
static ssize_t demote_size_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	/* demote_size 将目标 order 换算为 KiB，h 为静态 hstate 借用。 */
	struct hstate *h = kobj_to_hstate(kobj, NULL);
	unsigned long demote_size = (PAGE_SIZE << h->demote_order) / SZ_1K;

	return sysfs_emit(buf, "%lukB\n", demote_size);
}

/*
 * 业务背景：选择当前 hstate 的 demote 目标 hstate，限制拆分只能流向内核已
 * 注册且更小、同时不小于默认 HugeTLB 页阶的池。
 * 入参：kobj/attr 是 sysfs 借用对象；buf/count 是带单位大小文本和字节数。
 * 出参/返回：成功返回 count 并更新 demote_order；无法匹配或阶数非法返回
 * -EINVAL且保持原值。
 * 注意事项：memparse 接受 K/M/G 等单位；resize_lock 与 demote_store/其他写
 * 串行。只改未来策略，不立即移动任何页。
 */
static ssize_t demote_size_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	/* h 是源，demote_hstate 是目标；size 为字节，order 为校验后提交值。 */
	struct hstate *h, *demote_hstate;
	unsigned long demote_size;
	unsigned int demote_order;

	demote_size = (unsigned long)memparse(buf, NULL);

	/* 目标大小必须精确对应已注册 hstate。 */
	demote_hstate = size_to_hstate(demote_size);
	if (!demote_hstate)
		return -EINVAL;
	demote_order = demote_hstate->order;
	/* 不允许拆到默认 HugeTLB 最小阶以下，避免越过该子系统边界。 */
	if (demote_order < HUGETLB_PAGE_ORDER)
		return -EINVAL;

	/* demote order must be smaller than hstate order */
	/* demote 只能从大到小；相同或更大目标不会释放更细粒度容量。 */
	h = kobj_to_hstate(kobj, NULL);
	if (demote_order >= h->order)
		return -EINVAL;

	/* resize_lock synchronizes access to demote size and writes */
	/* 锁内赋值是策略提交点，正在执行的 demote 与本次修改互斥。 */
	mutex_lock(&h->resize_lock);
	h->demote_order = demote_order;
	mutex_unlock(&h->resize_lock);

	return count;
}
HSTATE_ATTR(demote_size);

/* 全局页大小目录的基础属性表；NULL 终止，NUMA 配置额外暴露 mempolicy 写法。 */
static struct attribute *hstate_attrs[] = {
	/* 持久目标可读写；overcommit 策略可读写；后三项是池状态只读快照。 */
	&nr_hugepages_attr.attr,
	&nr_overcommit_hugepages_attr.attr,
	&free_hugepages_attr.attr,
	&resv_hugepages_attr.attr,
	&surplus_hugepages_attr.attr,
#ifdef CONFIG_NUMA
	&nr_hugepages_mempolicy_attr.attr,
#endif
	NULL,
};

/* sysfs 以该静态 group 一次性创建/删除基础属性，attrs 的 ownership 不转移。 */
static const struct attribute_group hstate_attr_group = {
	.attrs = hstate_attrs,
};

/* 仅 demote_order 非零的 hstate 创建这组目标大小与执行入口。 */
static struct attribute *hstate_demote_attrs[] = {
	&demote_size_attr.attr,
	&demote_attr.attr,
	NULL,
};

/* demote group 与基础 group 分开，便于创建失败时精确逆序回滚。 */
static const struct attribute_group hstate_demote_attr_group = {
	.attrs = hstate_demote_attrs,
};

/*
 * 业务背景：在给定父目录下为一个 hstate 创建以页大小命名的 kobject，并发布
 * 基础属性和可选 demote 属性；全局与每节点注册路径共享。
 * 入参：h 是静态 hstate 借用；parent 是已持有的父 kobject；hstate_kobjs 是
 * 调用者拥有的索引数组输出；hstate_attr_group 是待发布静态属性组借用。
 * 出参/返回：成功 0 且数组槽持有新 kobject；失败负 errno，删除已建 group、
 * put kobject 并把槽恢复 NULL，不遗留部分可见目录。
 * 注意事项：可睡眠；调用者串行注册。demote group 只在 demote_order 非零时
 * 创建，数组参数可能是全局表或某节点表。
 */
static int hugetlb_sysfs_add_hstate(struct hstate *h, struct kobject *parent,
				    struct kobject **hstate_kobjs,
				    const struct attribute_group *hstate_attr_group)
{
	/* retval 传递创建错误；hi 是 h 在所有平行 kobject 数组中的稳定下标。 */
	int retval;
	int hi = hstate_index(h);

	/* 创建并发布目录；失败尚无需回滚属性。 */
	hstate_kobjs[hi] = kobject_create_and_add(h->name, parent);
	if (!hstate_kobjs[hi])
		return -ENOMEM;

	/* 基础属性组失败时 put 目录并清槽，恢复调用前状态。 */
	retval = sysfs_create_group(hstate_kobjs[hi], hstate_attr_group);
	if (retval) {
		kobject_put(hstate_kobjs[hi]);
		hstate_kobjs[hi] = NULL;
		return retval;
	}

	/* 仅有合法 demote 目标的页大小发布拆分页接口。 */
	if (h->demote_order) {
		retval = sysfs_create_group(hstate_kobjs[hi],
					    &hstate_demote_attr_group);
		/* 第二组失败必须先删第一组，再 put kobject，避免半成品控制面。 */
		if (retval) {
			pr_warn("HugeTLB unable to create demote interfaces for %s\n", h->name);
			/* 基础组已经对用户可见，必须先摘除属性再释放承载目录。 */
			sysfs_remove_group(hstate_kobjs[hi], hstate_attr_group);
			kobject_put(hstate_kobjs[hi]);
			hstate_kobjs[hi] = NULL;
			return retval;
		}
	}

	return 0;
}

#ifdef CONFIG_NUMA
/* init 完成后置 true 并只读，阻止 node device 过早注册 HugeTLB 属性。 */
static bool hugetlb_sysfs_initialized __ro_after_init;

/*
 * node_hstate/s - associate per node hstate attributes, via their kobjects,
 * with node devices in node_devices[] using a parallel array.  The array
 * index of a node device or _hstate == node id.
 * This is here to avoid any static dependency of the node device driver, in
 * the base kernel, on the hugetlb module.
 */
/*
 * node_hstate 以节点设备为所有者保存 hugepages 父目录及每种页大小子目录；
 * node_hstates[nid] 与 node_devices[nid] 平行，避免基础 node 驱动静态依赖
 * HugeTLB。kobject 引用由 register 创建、unregister 逆序 put。
 */
struct node_hstate {
	/* 节点下的 hugepages 父目录，NULL 表示尚未注册。 */
	struct kobject		*hugepages_kobj;
	/* 以 hstate_index 为下标的页大小子目录，允许部分创建期间暂为 NULL。 */
	struct kobject		*hstate_kobjs[HUGE_MAX_HSTATE];
};

/* 每个可能 NUMA node 的静态控制面账本，node id 即数组下标。 */
static struct node_hstate node_hstates[MAX_NUMNODES];

/*
 * A subset of global hstate attributes for node devices
 */
/* 节点目录只暴露可按 nid 正确定义的持久目标、free 和 surplus 三个属性。 */
static struct attribute *per_node_hstate_attrs[] = {
	&nr_hugepages_attr.attr,
	&free_hugepages_attr.attr,
	&surplus_hugepages_attr.attr,
	NULL,
};

/* per-node 属性组借用上述静态数组，由每个 hstate kobject 各创建一份链接。 */
static const struct attribute_group per_node_hstate_attr_group = {
	.attrs = per_node_hstate_attrs,
};

/*
 * kobj_to_node_hstate - lookup global hstate for node device hstate attr kobj.
 * Returns node id via non-NULL nidp.
 */
/* 按节点 hstate 属性 kobject 查全局 hstate，并在 nidp 非空时返回 node id。 */
/*
 * 业务背景：per-node sysfs 回调需要从目录身份恢复 hstate+nid 二元组。
 * 入参：kobj 必须属于 node_hstates；nidp 可为 NULL，否则为输出指针。
 * 出参/返回：返回静态 hstate 借用并写 nid；未知对象触发 BUG 后理论上返回 NULL。
 * 注意事项：sysfs 保证 kobject 生命周期；双层线性扫描不睡眠、无引用转移，
 * 注册/注销与属性文件可达性保证不会并发传入已删除对象。
 */
static struct hstate *kobj_to_node_hstate(struct kobject *kobj, int *nidp)
{
	/* nid/i 遍历平行目录表；nhs 是当前节点账本借用。 */
	int nid;

	/* 找到完全相同的 kobject 地址才输出节点并复用相同下标的全局 hstate。 */
	for (nid = 0; nid < nr_node_ids; nid++) {
		struct node_hstate *nhs = &node_hstates[nid];
		int i;
		for (i = 0; i < HUGE_MAX_HSTATE; i++)
			if (nhs->hstate_kobjs[i] == kobj) {
				if (nidp)
					*nidp = nid;
				return &hstates[i];
			}
	}

	/* 属性回调出现未知目录说明注册账本已损坏，无法安全选择池，故 BUG。 */
	BUG();
	return NULL;
}

/*
 * Unregister hstate attributes from a single node device.
 * No-op if no hstate attributes attached.
 */
/* 从单个 node device 注销全部 hstate 属性；未附着属性时不执行任何操作。 */
/*
 * 业务背景：节点设备下线或注册失败回滚时，逆序释放其 HugeTLB sysfs 树。
 * 入参：node 是仍存活的节点设备借用指针，dev.id 必须为有效 node id。
 * 出参/返回：无直接返回值；删除全部属性组、put 子/父 kobject 并清空账本。
 * 注意事项：可睡眠，调用者串行 node 注册生命周期；清槽防止后续误认为仍注册。
 */
void hugetlb_unregister_node(struct node *node)
{
	/* h 遍历页大小；nhs 是该节点的静态 ownership 账本。 */
	struct hstate *h;
	struct node_hstate *nhs = &node_hstates[node->dev.id];

	/* 父目录 NULL 是幂等快速路径。 */
	if (!nhs->hugepages_kobj)
		return;		/* no hstate attributes */

	/* 每个存在的子目录先删可选 demote，再删基础组，最后 put 并清槽。 */
	for_each_hstate(h) {
		int idx = hstate_index(h);
		struct kobject *hstate_kobj = nhs->hstate_kobjs[idx];

		/* 注册可能只完成前缀，空槽无需删除也没有待 put 引用。 */
		if (!hstate_kobj)
			continue;
		if (h->demote_order)
			sysfs_remove_group(hstate_kobj, &hstate_demote_attr_group);
		sysfs_remove_group(hstate_kobj, &per_node_hstate_attr_group);
		kobject_put(hstate_kobj);
		nhs->hstate_kobjs[idx] = NULL;
	}

	/* 所有孩子已释放后才能 put 父目录。 */
	kobject_put(nhs->hugepages_kobj);
	nhs->hugepages_kobj = NULL;
}


/*
 * Register hstate attributes for a single node device.
 * No-op if attributes already registered.
 */
/* 为单个 node device 注册 hstate 属性；已注册时保持不变。 */
/*
 * 业务背景：节点设备上线后创建 node/hugepages/<hstate> 控制面，使用户能按
 * NUMA 节点观察和调整持久池。
 * 入参：node 是已注册且生命周期稳定的节点设备借用指针。
 * 出参/返回：无直接返回值；成功账本持有父/子 kobject；失败记录日志并调用
 * unregister 完整回滚，接口无法向 node 核心传播错误。
 * 注意事项：全局 HugeTLB sysfs 未初始化时跳过；可睡眠且要求外层串行，重复
 * 调用以父指针判定幂等。
 */
void hugetlb_register_node(struct node *node)
{
	/* h 遍历页大小；nhs 对应该 nid；err 保存当前子目录创建结果。 */
	struct hstate *h;
	struct node_hstate *nhs = &node_hstates[node->dev.id];
	int err;

	/* node 可能早于 HugeTLB init 出现，此时由 register_all_nodes 稍后补建。 */
	if (!hugetlb_sysfs_initialized)
		return;

	/* 父目录存在说明整棵或正在构造的树已由本串行路径负责，避免重复引用。 */
	if (nhs->hugepages_kobj)
		return;		/* already allocated */

	/* 先创建节点父目录，失败只能静默跳过，因为本 API 无返回值。 */
	nhs->hugepages_kobj = kobject_create_and_add("hugepages",
							&node->dev.kobj);
	if (!nhs->hugepages_kobj)
		return;

	/* 逐 hstate 发布 per-node 子集；任一失败撤销此前全部子目录和父目录。 */
	for_each_hstate(h) {
		err = hugetlb_sysfs_add_hstate(h, nhs->hugepages_kobj,
						nhs->hstate_kobjs,
						&per_node_hstate_attr_group);
		if (err) {
			pr_err("HugeTLB: Unable to add hstate %s for node %d\n",
				h->name, node->dev.id);
			/* API 无法上抛错误；日志后撤销整个节点树，保持全有或全无。 */
			hugetlb_unregister_node(node);
			break;
		}
	}
}

/*
 * hugetlb init time:  register hstate attributes for all registered node
 * devices of nodes that have memory.  All on-line nodes should have
 * registered their associated device by this time.
 */
/*
 * HugeTLB 初始化时为所有已有内存的在线 node device 补注册 hstate 属性；此时
 * 所有在线节点应已注册对应 device，后续节点再走 hugetlb_register_node()。
 */
/*
 * 业务背景：弥合“node device 先注册、HugeTLB sysfs 后初始化”的启动顺序。
 * 入参：无。
 * 出参/返回：无直接返回值；逐在线 nid 调用节点注册，单节点失败仅记录日志。
 * 注意事项：__init 可睡眠；node_devices[nid] 必须按启动顺序已有效。
 */
static void __init hugetlb_register_all_nodes(void)
{
	/* nid 是在线节点游标。 */
	int nid;

	for_each_online_node(nid)
		hugetlb_register_node(node_devices[nid]);
}
#else	/* !CONFIG_NUMA */

/*
 * 业务背景：非 NUMA 构建不应存在节点 hstate kobject；提供链接所需的不可达桩。
 * 入参：kobj/nidp 为误调用参数，nidp 非空时在 BUG 后写 -1 仅满足控制流分析。
 * 出参/返回：正常不可返回；形式上返回 NULL，无 ownership 或状态输出保证。
 * 注意事项：任何调用都是内部协议错误并触发 BUG，不可把它当普通查找失败。
 */
static struct hstate *kobj_to_node_hstate(struct kobject *kobj, int *nidp)
{
	BUG();
	if (nidp)
		*nidp = -1;
	return NULL;
}

/*
 * 业务背景：非 NUMA 没有节点控制面，为通用 init 调用提供空操作。
 * 入参：无。出参/返回：无直接返回值、无副作用。
 * 注意事项：不睡眠；全局 hstate sysfs 仍由 hugetlb_sysfs_init() 创建。
 */
static void hugetlb_register_all_nodes(void) { }

#endif

/*
 * 业务背景：创建 /sys/kernel/mm/hugepages 全局树，为每个 hstate 发布基础和
 * 可选 demote 属性，并在 NUMA 系统补齐每节点视图。
 * 入参：无。
 * 出参/返回：无直接返回值；父目录创建失败时无控制面，单 hstate 失败只缺该
 * 子目录并记录错误，其余继续；成功 kobject 由内核长期持有。
 * 注意事项：__init 可睡眠且只调用一次；NUMA initialized 标志必须在遍历节点
 * 前发布，使节点注册入口不再跳过。
 */
void __init hugetlb_sysfs_init(void)
{
	/* h 遍历静态页大小；err 保存单个子树创建错误。 */
	struct hstate *h;
	int err;

	/* 父目录创建是全局发布前提，失败无可回滚对象也无法向启动者报错。 */
	hugepages_kobj = kobject_create_and_add("hugepages", mm_kobj);
	if (!hugepages_kobj)
		return;

	/* 各页大小相互独立：单项失败不妨碍其他 hstate 仍提供控制面。 */
	for_each_hstate(h) {
		err = hugetlb_sysfs_add_hstate(h, hugepages_kobj,
					 hstate_kobjs, &hstate_attr_group);
		if (err)
			pr_err("HugeTLB: Unable to add hstate %s\n", h->name);
	}

#ifdef CONFIG_NUMA
	/* __ro_after_init 单向发布后，已有和未来 node 均可真正进入注册路径。 */
	hugetlb_sysfs_initialized = true;
#endif
	/* 全局目录完成后补齐启动早期已经注册的 node devices。 */
	hugetlb_register_all_nodes();
}
