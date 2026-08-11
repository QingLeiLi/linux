// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Intel Corp.
 * Author: Jiang Liu <jiang.liu@linux.intel.com>
 *
 * This file is licensed under GPLv2.
 *
 * This file contains common code to support Message Signaled Interrupts for
 * PCI compatible and non PCI compatible devices.
 */
#include <linux/device.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include "internals.h"

/*
 * MSI 把设备发出的内存写消息转换为中断，而不是依赖固定物理中断线。本文件在
 * 通用 irq_domain 之上管理三层对象：设备拥有的 msi_device_data、按 domain ID
 * 划分的 XArray 描述符仓库，以及每个 msi_desc 关联的一组 Linux virq/irq_data。
 *
 * 主要同步边界是 dev->msi.data->mutex：它保护 domain 指针、描述符 XArray、
 * 迭代游标及描述符与 virq 的关联。分配先建立描述符和层级 IRQ 资源，再按需
 * 激活/写入 MSI 消息并发布 sysfs；失败必须逆序撤销。设备销毁时 devres 最终
 * 移除 per-device domain、确认仓库清空并释放设备级容器。
 */

/**
 * struct msi_device_data - MSI per device data
 * @properties:		MSI properties which are interesting to drivers
 * @mutex:		Mutex protecting the MSI descriptor store
 * @__domains:		Internal data for per device MSI domains
 * @__iter_idx:		Index to search the next entry for iterators
 */
/*
 * 每个设备的 MSI 核心状态。
 * @properties：驱动可查询的设备 MSI 能力位。
 * @mutex：串行化下列 domain 槽、XArray 描述符仓库和迭代状态的可睡眠锁。
 * @__domains：按 domid 索引的 per-device domain 与描述符 store；容器随设备生灭。
 * @__iter_idx：锁内迭代器的下一搜索位置，解锁时失效，禁止跨临界区续用。
 */
struct msi_device_data {
	unsigned long			properties;
	struct mutex			mutex;
	struct msi_dev_domain		__domains[MSI_MAX_DEVICE_IRQDOMAINS];
	unsigned long			__iter_idx;
};

/**
 * struct msi_ctrl - MSI internal management control structure
 * @domid:	ID of the domain on which management operations should be done
 * @first:	First (hardware) slot index to operate on
 * @last:	Last (hardware) slot index to operate on
 * @nirqs:	The number of Linux interrupts to allocate. Can be larger
 *		than the range due to PCI/multi-MSI.
 */
/*
 * 一次 MSI 管理事务的闭区间控制块。
 * @domid 选择设备的 domain 槽；@first/@last 是该 domain 硬件表中的闭区间索引；
 * @nirqs 是希望取得的 Linux IRQ 数量。PCI multi-MSI 可由一个描述符/索引覆盖
 * 多个连续向量，因此 nirqs 可以大于索引区间长度。结构只在调用栈上借用。
 */
struct msi_ctrl {
	unsigned int			domid;
	unsigned int			first;
	unsigned int			last;
	unsigned int			nirqs;
};

/* Invalid Xarray index which is outside of any searchable range */
/* 位于所有合法搜索范围之外的 XArray 哨兵索引，用来表示迭代已经结束/失效。 */
#define MSI_XA_MAX_INDEX	(ULONG_MAX - 1)
/* The maximum domain size */
/* 单个 MSI domain 可表示的最大索引数量。 */
#define MSI_XA_DOMAIN_SIZE	(MSI_MAX_INDEX + 1)

static void msi_domain_free_locked(struct device *dev, struct msi_ctrl *ctrl);
static unsigned int msi_domain_get_hwsize(struct device *dev, unsigned int domid);
static inline int msi_sysfs_create_group(struct device *dev);
static int msi_domain_prepare_irqs(struct irq_domain *domain, struct device *dev,
				   int nvec, msi_alloc_info_t *arg);

/**
 * msi_alloc_desc - Allocate an initialized msi_desc
 * @dev:	Pointer to the device for which this is allocated
 * @nvec:	The number of vectors used in this entry
 * @affinity:	Optional pointer to an affinity mask array size of @nvec
 *
 * If @affinity is not %NULL then an affinity array[@nvec] is allocated
 * and the affinity masks and flags from @affinity are copied.
 *
 * Return: pointer to allocated &msi_desc on success or %NULL on failure
 */
/*
 * 为 @dev 分配一个使用 @nvec 个向量的已初始化 MSI 描述符。
 * @affinity 为可空只读数组；非空时按 nvec 深拷贝，因此返回后调用者仍拥有原数组，
 * 新副本归 desc 所有。成功返回尚未插入 XArray、尚未关联 virq 的独占指针；失败
 * 返回 NULL 且已回收部分对象。GFP_KERNEL 决定调用环境必须可睡眠。
 */
static struct msi_desc *msi_alloc_desc(struct device *dev, int nvec,
				       const struct irq_affinity_desc *affinity)
{
	/* 先建立全零容器；此时尚未对描述符仓库或 irq_data 发布。 */
	struct msi_desc *desc = kzalloc_obj(*desc);

	if (!desc)
		return NULL;

	desc->dev = dev;
	desc->nvec_used = nvec;
	/* affinity 是唯一需要在基础描述符之外另行取得所有权的数据。 */
	if (affinity) {
		desc->affinity = kmemdup_array(affinity, nvec, sizeof(*desc->affinity), GFP_KERNEL);
		if (!desc->affinity) {
			kfree(desc);
			return NULL;
		}
	}
	return desc;
}

/*
 * 释放一个已从描述符仓库摘除且不再关联 IRQ 的 msi_desc。
 * 先释放由 msi_alloc_desc() 深拷贝的 affinity 数组，再释放容器；dev、消息和
 * 类型专用联合体不单独拥有外部对象。调用者必须已移除 sysfs 并清空 desc->irq。
 */
static void msi_free_desc(struct msi_desc *desc)
{
	kfree(desc->affinity);
	kfree(desc);
}

/*
 * 把新描述符插入 @dev 的 @domid XArray，并确定其硬件索引。
 * 调用者持有设备 MSI mutex，且把 @desc 的唯一所有权交给本函数。MSI_ANY_INDEX
 * 让 XArray 在 [0, hwsize-1] 内原子选择空槽；固定 index 则先做上界检查并拒绝
 * 冲突。成功后 store 拥有 desc 且 desc->msi_index 有效；任何失败都会释放 desc，
 * 调用者不得再访问它，并返回 -ERANGE、-EBUSY、-ENOMEM 等 XArray 错误。
 */
static int msi_insert_desc(struct device *dev, struct msi_desc *desc,
			   unsigned int domid, unsigned int index)
{
	struct msi_device_data *md = dev->msi.data;
	struct xarray *xa = &md->__domains[domid].store;
	unsigned int hwsize;
	int ret;

	/* 容量来自目标 domain；无 domain 的兼容路径会返回通用最大值。 */
	hwsize = msi_domain_get_hwsize(dev, domid);

	if (index == MSI_ANY_INDEX) {
		struct xa_limit limit = { .min = 0, .max = hwsize - 1 };
		unsigned int index;

		/* Let the xarray allocate a free index within the limit */
		/* 由 XArray 在硬件容量范围内选择并占用一个空闲索引。 */
		ret = xa_alloc(xa, &index, desc, limit, GFP_KERNEL);
		if (ret)
			goto fail;

		desc->msi_index = index;
		return 0;
	} else {
		/* 固定索引先拒绝越界，再由 xa_insert() 原子拒绝已有条目。 */
		if (index >= hwsize) {
			ret = -ERANGE;
			goto fail;
		}

		desc->msi_index = index;
		ret = xa_insert(xa, index, desc, GFP_KERNEL);
		if (ret)
			goto fail;
		return 0;
	}
fail:
	/* 尚未成功发布到 XArray，失败路径仍拥有并销毁 desc。 */
	msi_free_desc(desc);
	return ret;
}

/**
 * msi_domain_insert_msi_desc - Allocate and initialize a MSI descriptor and
 *				insert it at @init_desc->msi_index
 *
 * @dev:	Pointer to the device for which the descriptor is allocated
 * @domid:	The id of the interrupt domain to which the desriptor is added
 * @init_desc:	Pointer to an MSI descriptor to initialize the new descriptor
 *
 * Return: 0 on success or an appropriate failure code.
 */
/*
 * 根据模板 @init_desc 为 @dev 创建独立描述符，并插入 @domid 的指定索引。
 * 调用者必须持有设备 MSI mutex。函数只深拷贝向量数、affinity 和 PCI 类型数据，
 * 不复制现有 virq/消息/sysfs 状态。成功后新对象归 XArray store；失败时内部已
 * 释放新对象，模板始终仍归调用者所有。
 */
int msi_domain_insert_msi_desc(struct device *dev, unsigned int domid,
			       struct msi_desc *init_desc)
{
	struct msi_desc *desc;

	lockdep_assert_held(&dev->msi.data->mutex);

	desc = msi_alloc_desc(dev, init_desc->nvec_used, init_desc->affinity);
	if (!desc)
		return -ENOMEM;

	/* Copy type specific data to the new descriptor. */
	/* 复制 PCI/MSI 或 MSI-X 专用属性，但不复制运行期关联状态。 */
	desc->pci = init_desc->pci;

	return msi_insert_desc(dev, desc, domid, init_desc->msi_index);
}

/*
 * 按描述符是否已关联 Linux IRQ 进行筛选。
 * MSI_DESC_ALL 总为真；desc->irq 为 0 表示未关联，非 0 表示已关联。未知过滤值
 * 说明核心调用协议错误，只告警一次并返回 false。函数只读对象且不会睡眠。
 */
static bool msi_desc_match(struct msi_desc *desc, enum msi_desc_filter filter)
{
	switch (filter) {
	case MSI_DESC_ALL:
		return true;
	case MSI_DESC_NOTASSOCIATED:
		return !desc->irq;
	case MSI_DESC_ASSOCIATED:
		return !!desc->irq;
	}
	WARN_ON_ONCE(1);
	return false;
}

/*
 * 验证管理控制块能安全索引设备的 domain 槽和硬件表。
 * domid 必须在数组范围内；设备已有全局 MSI domain 时，对应 per-device 槽也必须
 * 已绑定 domain；first/last 必须形成非空、完全落在 hwsize 内的闭区间。失败属于
 * 内核调用者错误，WARN_ON_ONCE 后返回 false；成功不取得任何引用。
 */
static bool msi_ctrl_valid(struct device *dev, struct msi_ctrl *ctrl)
{
	unsigned int hwsize;

	if (WARN_ON_ONCE(ctrl->domid >= MSI_MAX_DEVICE_IRQDOMAINS ||
			 (dev->msi.domain &&
			  !dev->msi.data->__domains[ctrl->domid].domain)))
		return false;

	hwsize = msi_domain_get_hwsize(dev, ctrl->domid);
	if (WARN_ON_ONCE(ctrl->first > ctrl->last ||
			 ctrl->first >= hwsize ||
			 ctrl->last >= hwsize))
		return false;
	return true;
}

/*
 * 从指定 domain 的 XArray 摘除并释放 @ctrl 闭区间内的 MSI 描述符。
 * 调用者持有设备 MSI mutex；先 xa_erase() 撤销可见性，再检查 desc->irq。仍关联
 * IRQ 的描述符违反释放顺序：为避免 use-after-free，函数告警并故意泄漏已摘除
 * 对象；只有未关联对象才调用 msi_free_desc()。无返回值，非法控制块只告警返回。
 */
static void msi_domain_free_descs(struct device *dev, struct msi_ctrl *ctrl)
{
	struct msi_desc *desc;
	struct xarray *xa;
	unsigned long idx;

	lockdep_assert_held(&dev->msi.data->mutex);

	if (!msi_ctrl_valid(dev, ctrl))
		return;

	xa = &dev->msi.data->__domains[ctrl->domid].store;
	xa_for_each_range(xa, idx, desc, ctrl->first, ctrl->last) {
		xa_erase(xa, idx);

		/* Leak the descriptor when it is still referenced */
		/* 仍由 virq 引用时宁可泄漏，也不能释放后留下 irq_data 悬空指针。 */
		if (WARN_ON_ONCE(msi_desc_match(desc, MSI_DESC_ASSOCIATED)))
			continue;
		msi_free_desc(desc);
	}
}

/**
 * msi_domain_free_msi_descs_range - Free a range of MSI descriptors of a device in an irqdomain
 * @dev:	Device for which to free the descriptors
 * @domid:	Id of the domain to operate on
 * @first:	Index to start freeing from (inclusive)
 * @last:	Last index to be freed (inclusive)
 */
/*
 * 释放 @domid 中 [@first, @last] 的 MSI 描述符。
 * 本接口只组装控制块并调用锁内实现，要求调用者已经持有设备 MSI mutex；它不
 * 释放已关联 IRQ，正确顺序必须先走 msi_domain_free_irqs_*() 清空 desc->irq。
 */
void msi_domain_free_msi_descs_range(struct device *dev, unsigned int domid,
				     unsigned int first, unsigned int last)
{
	struct msi_ctrl ctrl = {
		.domid	= domid,
		.first	= first,
		.last	= last,
	};

	msi_domain_free_descs(dev, &ctrl);
}

/**
 * msi_domain_add_simple_msi_descs - Allocate and initialize MSI descriptors
 * @dev:	Pointer to the device for which the descriptors are allocated
 * @ctrl:	Allocation control struct
 *
 * Return: 0 on success or an appropriate failure code.
 */
/*
 * 为 @ctrl 闭区间的每个硬件槽创建一个单向量描述符。
 * 调用者持设备 MSI mutex。循环按固定索引分配并插入；任一对象/插入失败时，
 * msi_domain_free_descs() 会回收本区间内本次及此前存在的未关联描述符，使结果
 * 回到空区间。成功返回 0，失败返回 -ENOMEM 或 XArray/范围错误。
 */
static int msi_domain_add_simple_msi_descs(struct device *dev, struct msi_ctrl *ctrl)
{
	struct msi_desc *desc;
	unsigned int idx;
	int ret;

	lockdep_assert_held(&dev->msi.data->mutex);

	if (!msi_ctrl_valid(dev, ctrl))
		return -EINVAL;

	/* 按索引逐项发布；任一插入失败都转入同一整区间回滚点。 */
	for (idx = ctrl->first; idx <= ctrl->last; idx++) {
		desc = msi_alloc_desc(dev, 1, NULL);
		if (!desc)
			goto fail_mem;
		ret = msi_insert_desc(dev, desc, ctrl->domid, idx);
		if (ret)
			goto fail;
	}
	return 0;

fail_mem:
	ret = -ENOMEM;
fail:
	/* 擦除已经提交的前缀，使调用者看不到半建成的简单描述符区间。 */
	msi_domain_free_descs(dev, ctrl);
	return ret;
}

/*
 * 把 @entry 中缓存的单条 MSI 消息按值复制到调用者输出 @msg。
 * 两个指针都必须非空；函数不加锁、不访问硬件，也不转移所有权，调用者负责
 * 保证描述符在复制期间稳定。level MSI 的第二条消息不由此旧接口返回。
 */
void __get_cached_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
	*msg = entry->msg;
}

/*
 * 按 Linux IRQ 查找其 MSI 描述符并返回缓存消息。
 * @irq 必须已关联有效 msi_desc，@msg 为调用者拥有的输出缓冲区；本函数不验证
 * NULL，适用于 IRQ 生命周期已稳定的内部/驱动调用路径，不会重新读取设备表。
 */
void get_cached_msi_msg(unsigned int irq, struct msi_msg *msg)
{
	struct msi_desc *entry = irq_get_msi_desc(irq);

	__get_cached_msi_msg(entry, msg);
}
EXPORT_SYMBOL_GPL(get_cached_msi_msg);

/*
 * devres 最终释放设备 MSI 状态时执行的析构回调。
 * 依次移除每个 per-device MSI domain，确认其描述符仓库已由更早释放路径清空，
 * 再销毁空 XArray；最后清除 dev->msi.data 发布指针。@res 由 devres 框架拥有，
 * 回调返回后框架释放容器本身。非空 store 只告警，表明驱动遗漏 IRQ/描述符回收。
 */
static void msi_device_data_release(struct device *dev, void *res)
{
	struct msi_device_data *md = res;
	int i;

	for (i = 0; i < MSI_MAX_DEVICE_IRQDOMAINS; i++) {
		msi_remove_device_irq_domain(dev, i);
		WARN_ON_ONCE(!xa_empty(&md->__domains[i].store));
		xa_destroy(&md->__domains[i].store);
	}
	dev->msi.data = NULL;
}

/**
 * msi_setup_device_data - Setup MSI device data
 * @dev:	Device for which MSI device data should be set up
 *
 * Return: 0 on success, appropriate error code otherwise
 *
 * This can be called more than once for @dev. If the MSI device data is
 * already allocated the call succeeds. The allocated memory is
 * automatically released when the device is destroyed.
 */
/*
 * 为 @dev 建立可重复调用的 MSI 设备级状态。
 * 已存在时快速成功；否则分配 devres 对象、创建 msi_irqs sysfs 组、初始化所有
 * domain XArray，并把兼容的全局 MSI domain 放入默认槽。mutex 初始化完成后才
 * 发布 dev->msi.data，最后把对象交给 devres，因此并发调用需由设备初始化时序
 * 串行化。成功返回 0；内存或 sysfs 失败时释放未发布对象并返回错误。
 */
int msi_setup_device_data(struct device *dev)
{
	struct msi_device_data *md;
	int ret, i;

	if (dev->msi.data)
		return 0;

	md = devres_alloc(msi_device_data_release, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;

	ret = msi_sysfs_create_group(dev);
	if (ret) {
		devres_free(md);
		return ret;
	}

	for (i = 0; i < MSI_MAX_DEVICE_IRQDOMAINS; i++)
		xa_init_flags(&md->__domains[i].store, XA_FLAGS_ALLOC);

	/*
	 * If @dev::msi::domain is set and is a global MSI domain, copy the
	 * pointer into the domain array so all code can operate on domain
	 * ids. The NULL pointer check is required to keep the legacy
	 * architecture specific PCI/MSI support working.
	 */
	/*
	 * 若设备已有非 MSI-parent 的全局 domain，把借用指针复制到默认槽，使后续代码
	 * 统一按 domid 操作。NULL 检查保留旧架构专用 PCI/MSI 支持。
	 */
	if (dev->msi.domain && !irq_domain_is_msi_parent(dev->msi.domain))
		md->__domains[MSI_DEFAULT_DOMAIN].domain = dev->msi.domain;

	mutex_init(&md->mutex);
	dev->msi.data = md;
	devres_add(dev, md);
	return 0;
}

/**
 * __msi_lock_descs - Lock the MSI descriptor storage of a device
 * @dev:	Device to operate on
 *
 * Internal function for guard(msi_descs_lock). Don't use in code.
 */
/*
 * 获取设备 MSI 描述符仓库的互斥锁。
 * 仅作为 guard(msi_descs_lock) 的底层钩子；调用前 dev->msi.data 必须已建立。
 * mutex 可睡眠，保护 domain 槽、XArray、desc 关联和共享迭代游标。
 */
void __msi_lock_descs(struct device *dev)
{
	mutex_lock(&dev->msi.data->mutex);
}
EXPORT_SYMBOL_GPL(__msi_lock_descs);

/**
 * __msi_unlock_descs - Unlock the MSI descriptor storage of a device
 * @dev:	Device to operate on
 *
 * Internal function for guard(msi_descs_lock). Don't use in code.
 */
/*
 * 结束设备 MSI 描述符临界区。
 * 解锁前把共享迭代索引置为哨兵，使任何错误跨临界区续迭代都会立即失败；随后
 * 释放 mutex。仅供 guard 清理使用，必须与 __msi_lock_descs() 成对。
 */
void __msi_unlock_descs(struct device *dev)
{
	/* Invalidate the index which was cached by the iterator */
	/* 使锁内缓存的迭代位置失效，禁止解锁后继续使用。 */
	dev->msi.data->__iter_idx = MSI_XA_MAX_INDEX;
	mutex_unlock(&dev->msi.data->mutex);
}
EXPORT_SYMBOL_GPL(__msi_unlock_descs);

/*
 * 从 md->__iter_idx 开始在 @domid 的 XArray 中寻找下一条符合 @filter 的描述符。
 * 调用者持 md->mutex，返回借用指针且游标停在命中索引；遍历耗尽则把游标设为
 * MSI_XA_MAX_INDEX 并返回 NULL。共享游标意味着同一设备不能交错两次迭代。
 */
static struct msi_desc *msi_find_desc(struct msi_device_data *md, unsigned int domid,
				      enum msi_desc_filter filter)
{
	struct xarray *xa = &md->__domains[domid].store;
	struct msi_desc *desc;

	xa_for_each_start(xa, md->__iter_idx, desc, md->__iter_idx) {
		if (msi_desc_match(desc, filter))
			return desc;
	}
	md->__iter_idx = MSI_XA_MAX_INDEX;
	return NULL;
}

/**
 * msi_domain_first_desc - Get the first MSI descriptor of an irqdomain associated to a device
 * @dev:	Device to operate on
 * @domid:	The id of the interrupt domain which should be walked.
 * @filter:	Descriptor state filter
 *
 * Must be called with the MSI descriptor mutex held, i.e. msi_lock_descs()
 * must be invoked before the call.
 *
 * Return: Pointer to the first MSI descriptor matching the search
 *	   criteria, NULL if none found.
 */
/*
 * 开始遍历 @dev 的 @domid 描述符仓库，并返回首个符合 @filter 的对象。
 * 调用者必须持有 MSI mutex；函数验证设备状态和 domid，把共享游标重置为 0，
 * 再由 msi_find_desc() 搜索。返回值是仅在同一锁临界区内稳定的借用指针，未命中
 * 返回 NULL。
 */
struct msi_desc *msi_domain_first_desc(struct device *dev, unsigned int domid,
				       enum msi_desc_filter filter)
{
	struct msi_device_data *md = dev->msi.data;

	if (WARN_ON_ONCE(!md || domid >= MSI_MAX_DEVICE_IRQDOMAINS))
		return NULL;

	lockdep_assert_held(&md->mutex);

	md->__iter_idx = 0;
	return msi_find_desc(md, domid, filter);
}
EXPORT_SYMBOL_GPL(msi_domain_first_desc);

/**
 * msi_next_desc - Get the next MSI descriptor of a device
 * @dev:	Device to operate on
 * @domid:	The id of the interrupt domain which should be walked.
 * @filter:	Descriptor state filter
 *
 * The first invocation of msi_next_desc() has to be preceeded by a
 * successful invocation of __msi_first_desc(). Consecutive invocations are
 * only valid if the previous one was successful. All these operations have
 * to be done within the same MSI mutex held region.
 *
 * Return: Pointer to the next MSI descriptor matching the search
 *	   criteria, NULL if none found.
 */
/*
 * 继续同一设备、同一锁临界区内已经开始的描述符遍历。
 * 首次调用前必须成功调用 msi_domain_first_desc()，且上次返回非 NULL；函数把
 * 游标前移一位再搜索下一匹配项。哨兵表示迭代已结束，直接返回 NULL。
 */
struct msi_desc *msi_next_desc(struct device *dev, unsigned int domid,
			       enum msi_desc_filter filter)
{
	struct msi_device_data *md = dev->msi.data;

	if (WARN_ON_ONCE(!md || domid >= MSI_MAX_DEVICE_IRQDOMAINS))
		return NULL;

	lockdep_assert_held(&md->mutex);

	if (md->__iter_idx >= (unsigned long)MSI_MAX_INDEX)
		return NULL;

	md->__iter_idx++;
	return msi_find_desc(md, domid, filter);
}
EXPORT_SYMBOL_GPL(msi_next_desc);

/**
 * msi_domain_get_virq - Lookup the Linux interrupt number for a MSI index on a interrupt domain
 * @dev:	Device to operate on
 * @domid:	Domain ID of the interrupt domain associated to the device
 * @index:	MSI interrupt index to look for (0-based)
 *
 * Return: The Linux interrupt number on success (> 0), 0 if not found
 */
/*
 * 查询 @dev 的 @domid 中硬件 @index 对应的 Linux virq。
 * 函数自行获取 MSI mutex。普通 MSI-X/平台 MSI 每索引一个描述符，直接返回
 * desc->irq；传统 PCI-MSI 用索引 0 的单个描述符表示连续多向量，需验证 index
 * 小于 nvec_used 后返回 irq+index。状态缺失、越界或未关联均返回 0。
 */
unsigned int msi_domain_get_virq(struct device *dev, unsigned int domid, unsigned int index)
{
	struct msi_desc *desc;
	bool pcimsi = false;
	struct xarray *xa;

	if (!dev->msi.data)
		return 0;

	if (WARN_ON_ONCE(index > MSI_MAX_INDEX || domid >= MSI_MAX_DEVICE_IRQDOMAINS))
		return 0;

	/* This check is only valid for the PCI default MSI domain */
	/* 只有 PCI 默认 domain 才使用“单描述符覆盖多向量”的传统 MSI 编码。 */
	if (dev_is_pci(dev) && domid == MSI_DEFAULT_DOMAIN)
		pcimsi = to_pci_dev(dev)->msi_enabled;

	guard(msi_descs_lock)(dev);
	xa = &dev->msi.data->__domains[domid].store;
	desc = xa_load(xa, pcimsi ? 0 : index);
	if (desc && desc->irq) {
		/*
		 * PCI-MSI has only one descriptor for multiple interrupts.
		 * PCI-MSIX and platform MSI use a descriptor per
		 * interrupt.
		 */
		/*
		 * PCI-MSI 用一个描述符覆盖多个连续 IRQ；PCI-MSIX 和平台 MSI 则每个中断
		 * 各有一个描述符。
		 */
		if (!pcimsi)
			return desc->irq;
		if (index < desc->nvec_used)
			return desc->irq + index;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(msi_domain_get_virq);

#ifdef CONFIG_SYSFS
/* 空属性数组作为动态 msi_irqs 属性组的固定骨架，具体文件按描述符另行添加。 */
static struct attribute *msi_dev_attrs[] = {
	NULL
};

/* 每个设备下名为 msi_irqs 的只读 sysfs 属性组。 */
static const struct attribute_group msi_irqs_group = {
	.name	= "msi_irqs",
	.attrs	= msi_dev_attrs,
};

/*
 * 通过 devres 为设备创建空的 msi_irqs 属性组。
 * 成功后组随设备自动删除；返回 0 或设备模型错误，可睡眠。
 */
static inline int msi_sysfs_create_group(struct device *dev)
{
	return devm_device_add_group(dev, &msi_irqs_group);
}

/*
 * 显示设备当前使用“msi”还是“msix”。
 * 模式是 PCI 设备级属性而非单中断属性，因此 @attr 仅用于 sysfs 回调签名；
 * 非 PCI 设备统一显示 msi。成功返回写入 @buf 的字节数。
 */
static ssize_t msi_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	/* MSI vs. MSIX is per device not per interrupt */
	/* MSI/MSI-X 模式由整个设备选择，而不是每个中断分别选择。 */
	bool is_msix = dev_is_pci(dev) ? to_pci_dev(dev)->msix_enabled : false;

	return sysfs_emit(buf, "%s\n", is_msix ? "msix" : "msi");
}

/*
 * 删除一个描述符对应的所有 msi_irqs/<virq> 文件并释放动态属性数组。
 * desc->sysfs_attrs 为空时幂等返回；否则先清空发布指针，再逐项删除已经成功添加
 * 的文件（show 非空为提交标记）和名称，最后释放数组。调用者持 MSI mutex，
 * 并保证 sysfs 回调不再依赖即将释放的 desc 状态。
 */
static void msi_sysfs_remove_desc(struct device *dev, struct msi_desc *desc)
{
	struct device_attribute *attrs = desc->sysfs_attrs;
	int i;

	if (!attrs)
		return;

	desc->sysfs_attrs = NULL;
	for (i = 0; i < desc->nvec_used; i++) {
		if (attrs[i].show)
			sysfs_remove_file_from_group(&dev->kobj, &attrs[i].attr, msi_irqs_group.name);
		kfree(attrs[i].attr.name);
	}
	kfree(attrs);
}

/*
 * 为描述符覆盖的每个连续 virq 创建只读 msi_irqs 属性文件。
 * 先分配 nvec_used 个属性并暂存到 desc，随后按 irq+i 命名、初始化 show 回调并
 * 逐项发布。任一步失败调用 remove_desc()，利用 show 是否置位只撤销已发布项，
 * 同时释放所有名称/数组；成功返回 0，失败返回 -ENOMEM 或 sysfs 错误。
 */
static int msi_sysfs_populate_desc(struct device *dev, struct msi_desc *desc)
{
	struct device_attribute *attrs;
	int ret, i;

	/* 先把完整属性数组挂到 desc，统一失败清理才能看见所有部分对象。 */
	attrs = kzalloc_objs(*attrs, desc->nvec_used);
	if (!attrs)
		return -ENOMEM;

	desc->sysfs_attrs = attrs;
	/* 每个连续 virq 对应一个文件；show 置位表示该项已经成功发布。 */
	for (i = 0; i < desc->nvec_used; i++) {
		sysfs_attr_init(&attrs[i].attr);
		attrs[i].attr.name = kasprintf(GFP_KERNEL, "%d", desc->irq + i);
		if (!attrs[i].attr.name) {
			ret = -ENOMEM;
			goto fail;
		}

		attrs[i].attr.mode = 0444;
		attrs[i].show = msi_mode_show;

		ret = sysfs_add_file_to_group(&dev->kobj, &attrs[i].attr, msi_irqs_group.name);
		if (ret) {
			attrs[i].show = NULL;
			goto fail;
		}
	}
	return 0;

fail:
	/* helper 根据每项 show 状态只撤销已发布文件，并释放全部名称和数组。 */
	msi_sysfs_remove_desc(dev, desc);
	return ret;
}

#if defined(CONFIG_PCI_MSI_ARCH_FALLBACKS) || defined(CONFIG_PCI_XEN)
/**
 * msi_device_populate_sysfs - Populate msi_irqs sysfs entries for a device
 * @dev:	The device (PCI, platform etc) which will get sysfs entries
 */
/*
 * 为设备所有已关联 MSI 描述符补建 sysfs 文件。
 * 仅用于架构回退/Xen 配置；迭代宏在 MSI mutex 下遍历，已存在 attrs 的描述符
 * 跳过。首次失败立即返回，已成功描述符保留，由 destroy 路径统一回收。
 */
int msi_device_populate_sysfs(struct device *dev)
{
	struct msi_desc *desc;
	int ret;

	msi_for_each_desc(desc, dev, MSI_DESC_ASSOCIATED) {
		if (desc->sysfs_attrs)
			continue;
		ret = msi_sysfs_populate_desc(dev, desc);
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * msi_device_destroy_sysfs - Destroy msi_irqs sysfs entries for a device
 * @dev:		The device (PCI, platform etc) for which to remove
 *			sysfs entries
 */
/*
 * 删除设备全部 MSI 描述符的动态 sysfs 文件。
 * 在 MSI mutex 保护的迭代中对所有状态调用幂等 remove helper；不释放描述符或
 * IRQ，只撤销用户空间展示入口。
 */
void msi_device_destroy_sysfs(struct device *dev)
{
	struct msi_desc *desc;

	msi_for_each_desc(desc, dev, MSI_DESC_ALL)
		msi_sysfs_remove_desc(dev, desc);
}
#endif /* CONFIG_PCI_MSI_ARCH_FALLBACKS || CONFIG_PCI_XEN */
#else /* CONFIG_SYSFS */
/* SYSFS 关闭时无需创建属性组，调用路径按成功继续。 */
static inline int msi_sysfs_create_group(struct device *dev) { return 0; }
/* SYSFS 关闭时描述符无需发布属性，按成功返回。 */
static inline int msi_sysfs_populate_desc(struct device *dev, struct msi_desc *desc) { return 0; }
/* SYSFS 关闭时没有动态属性需要撤销。 */
static inline void msi_sysfs_remove_desc(struct device *dev, struct msi_desc *desc) { }
#endif /* !CONFIG_SYSFS */

/*
 * 在持有设备 MSI mutex 时取得 @domid 对应的可分配 device domain。
 * domid 越界告警；空槽正常返回 NULL；槽中若误放 MSI parent domain 则告警拒绝，
 * 因为 parent 只提供下层能力而不直接拥有该设备描述符。返回借用指针。
 */
static struct irq_domain *msi_get_device_domain(struct device *dev, unsigned int domid)
{
	struct irq_domain *domain;

	lockdep_assert_held(&dev->msi.data->mutex);

	if (WARN_ON_ONCE(domid >= MSI_MAX_DEVICE_IRQDOMAINS))
		return NULL;

	domain = dev->msi.data->__domains[domid].domain;
	if (!domain)
		return NULL;

	if (WARN_ON_ONCE(irq_domain_is_msi_parent(domain)))
		return NULL;

	return domain;
}

/*
 * 返回 @domid 可用的硬件/MSI 描述符索引容量。
 * 有 device domain 时读取其 msi_domain_info::hwsize；没有 domain 时为传统描述符
 * 管理提供完整 MSI_XA_DOMAIN_SIZE 兜底。调用者持设备 MSI mutex。
 */
static unsigned int msi_domain_get_hwsize(struct device *dev, unsigned int domid)
{
	struct msi_domain_info *info;
	struct irq_domain *domain;

	domain = msi_get_device_domain(dev, domid);
	if (domain) {
		info = domain->host_data;
		return info->hwsize;
	}
	/* No domain, default to MSI_XA_DOMAIN_SIZE */
	/* 尚无 device domain 时允许使用通用最大索引空间。 */
	return MSI_XA_DOMAIN_SIZE;
}

/*
 * 通过当前 irq_chip 把一条或两条已组成的 MSI 消息写入设备/中断控制器。
 * data、chip 和 irq_write_msi_msg 回调必须有效；具体写寄存器和缓存副作用由
 * 控制器实现。本薄封装不加锁、不返回错误，调用上下文遵循 chip 回调契约。
 */
static inline void irq_chip_write_msi_msg(struct irq_data *data,
					  struct msi_msg *msg)
{
	data->chip->irq_write_msi_msg(data, msg);
}

/*
 * 验证 MSI provider 只有在 domain 和 irq_chip 都声明 level MSI 能力时才使用
 * msg[1]。第二条消息非零却缺少任一能力标志表示 provider 破坏协议，WARN_ON；
 * 函数只做诊断，不改写消息或阻止随后编程硬件。
 */
static void msi_check_level(struct irq_domain *domain, struct msi_msg *msg)
{
	struct msi_domain_info *info = domain->host_data;

	/*
	 * If the MSI provider has messed with the second message and
	 * not advertized that it is level-capable, signal the breakage.
	 */
	/* provider 填写第二条消息却未同时公布 domain/chip 的 level 能力时告警。 */
	WARN_ON(!((info->flags & MSI_FLAG_LEVEL_CAPABLE) &&
		  (info->chip->flags & IRQCHIP_SUPPORTS_LEVEL_MSI)) &&
		(msg[1].address_lo || msg[1].address_hi || msg[1].data));
}

/**
 * msi_domain_set_affinity - Generic affinity setter function for MSI domains
 * @irq_data:	The irq data associated to the interrupt
 * @mask:	The affinity mask to set
 * @force:	Flag to enforce setting (disable online checks)
 *
 * Intended to be used by MSI interrupt controllers which are
 * implemented with hierarchical domains.
 *
 * Return: IRQ_SET_MASK_* result code
 */
/*
 * 层级 MSI domain 的通用亲和性设置器。
 * 先把 @mask/@force 交给 parent irq_chip 移动底层向量；失败或父级已完成消息更新
 * 时直接返回。其他成功结果要求重新为外层 irq_data 组成 MSI 消息、验证 level
 * 能力并写回设备，使消息中的目标与新向量一致。返回父级 IRQ_SET_MASK_* 结果。
 */
int msi_domain_set_affinity(struct irq_data *irq_data,
			    const struct cpumask *mask, bool force)
{
	struct irq_data *parent = irq_data->parent_data;
	struct msi_msg msg[2] = { [1] = { }, };
	int ret;

	ret = parent->chip->irq_set_affinity(parent, mask, force);
	if (ret >= 0 && ret != IRQ_SET_MASK_OK_DONE) {
		BUG_ON(irq_chip_compose_msi_msg(irq_data, msg));
		msi_check_level(irq_data->domain, msg);
		irq_chip_write_msi_msg(irq_data, msg);
	}

	return ret;
}

/*
 * 激活 MSI domain 的一层：根据 irq_data 组成消息并写入设备。
 * msg[1] 预清零以支持可选 level MSI；compose 失败属于不可恢复的 irq_chip 协议
 * 错误并 BUG。@early 由通用两阶段激活协议传入，本实现写消息时不区分早晚，
 * 始终返回 0。
 */
static int msi_domain_activate(struct irq_domain *domain,
			       struct irq_data *irq_data, bool early)
{
	struct msi_msg msg[2] = { [1] = { }, };

	BUG_ON(irq_chip_compose_msi_msg(irq_data, msg));
	msi_check_level(irq_data->domain, msg);
	irq_chip_write_msi_msg(irq_data, msg);
	return 0;
}

/*
 * 停用 MSI domain 的一层：向设备写入全零消息，撤销此前可投递地址/数据。
 * 不释放向量、irq_data 或描述符，后续仍可重新 activate；写回错误由 chip 契约
 * 处理，本函数无返回值。
 */
static void msi_domain_deactivate(struct irq_domain *domain,
				  struct irq_data *irq_data)
{
	struct msi_msg msg[2];

	memset(msg, 0, sizeof(msg));
	irq_chip_write_msi_msg(irq_data, msg);
}

/*
 * irq_domain 层级 alloc 回调：为连续 virq 建立 MSI domain 本层状态。
 * 先由 domain ops 从 @arg 得到起始 hwirq 并拒绝重复映射；若有 parent，先向内层
 * 分配硬件资源。随后逐个调用 msi_init() 设置本层 hwirq/chip/handler。
 *
 * 某项初始化失败时，仅对已经成功的前缀逆序调用可选 msi_free()，再由
 * irq_domain_free_irqs_top() 清理 handler、本层数据和父级资源。成功返回 0；
 * 重复为 -EEXIST，父级或 init 错误原样返回。调用时 root domain mutex 已持有。
 */
static int msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
			    unsigned int nr_irqs, void *arg)
{
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;
	irq_hw_number_t hwirq = ops->get_hwirq(info, arg);
	int i, ret;

	/* 已存在同一 hwirq 映射时不能再构造第二条 irq_data 层级。 */
	if (irq_resolve_mapping(domain, hwirq))
		return -EEXIST;

	/* 层级 domain 必须先让 parent 分配底层硬件资源。 */
	if (domain->parent) {
		ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);
		if (ret < 0)
			return ret;
	}

	/* 再逐项提交本层 chip/hwirq；失败只撤销已经初始化成功的前缀。 */
	for (i = 0; i < nr_irqs; i++) {
		ret = ops->msi_init(domain, info, virq + i, hwirq + i, arg);
		if (ret < 0) {
			if (ops->msi_free) {
				for (i--; i >= 0; i--)
					ops->msi_free(domain, info, virq + i);
			}
			irq_domain_free_irqs_top(domain, virq, nr_irqs);
			return ret;
		}
	}

	return 0;
}

/*
 * irq_domain 层级 free 回调：释放一段 virq 的 MSI 本层和父层资源。
 * 若 provider 实现 msi_free()，按正序逐项撤销类型专用状态；随后公共 top helper
 * 清除流控处理器、重置 irq_data 并递归父级。revmap/desc 由 irqdomain 外层释放。
 */
static void msi_domain_free(struct irq_domain *domain, unsigned int virq,
			    unsigned int nr_irqs)
{
	struct msi_domain_info *info = domain->host_data;
	int i;

	if (info->ops->msi_free) {
		for (i = 0; i < nr_irqs; i++)
			info->ops->msi_free(domain, info, virq + i);
	}
	irq_domain_free_irqs_top(domain, virq, nr_irqs);
}

/*
 * 为确实支持固件有线中断描述的 MSI domain 翻译 fwspec。
 * 普通 MSI 分配必须走描述符 API，若 provider 未实现 msi_translate 则返回
 * -ENOTSUPP，借此拦截误走常规 irqdomain 映射；MBIGEN 等 wired-to-MSI 可由回调
 * 输出 hwirq/type。
 */
static int msi_domain_translate(struct irq_domain *domain, struct irq_fwspec *fwspec,
				irq_hw_number_t *hwirq, unsigned int *type)
{
	struct msi_domain_info *info = domain->host_data;

	/*
	 * This will catch allocations through the regular irqdomain path except
	 * for MSI domains which really support this, e.g. MBIGEN.
	 */
	/* 除 MBIGEN 等明确支持者外，拒绝从常规 irqdomain 路径分配 MSI。 */
	if (!info->ops->msi_translate)
		return -ENOTSUPP;
	return info->ops->msi_translate(domain, fwspec, hwirq, type);
}

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
/*
 * 向 IRQ domain debugfs 输出描述符缓存的 MSI 地址高/低位和 data。
 * domain 级展示可能没有具体 irqd，此时或没有 msi_desc 时为空；只读缓存，不访问硬件。
 */
static void msi_domain_debug_show(struct seq_file *m, struct irq_domain *d,
				  struct irq_data *irqd, int ind)
{
	struct msi_desc *desc = irqd ? irq_data_get_msi_desc(irqd) : NULL;

	if (!desc)
		return;

	seq_printf(m, "\n%*saddress_hi: 0x%08x", ind + 1, "", desc->msg.address_hi);
	seq_printf(m, "\n%*saddress_lo: 0x%08x", ind + 1, "", desc->msg.address_lo);
	seq_printf(m, "\n%*smsg_data:   0x%08x\n", ind + 1, "", desc->msg.data);
}
#endif

/* 把通用 irq_domain 生命周期连接到 MSI 分配、消息激活、释放和可选调试实现。 */
static const struct irq_domain_ops msi_domain_ops = {
	.alloc		= msi_domain_alloc,
	.free		= msi_domain_free,
	.activate	= msi_domain_activate,
	.deactivate	= msi_domain_deactivate,
	.translate	= msi_domain_translate,
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
	.debug_show     = msi_domain_debug_show,
#endif
};

/* 默认 get_hwirq：直接返回分配参数中由 provider/调用者准备的硬件中断号。 */
static irq_hw_number_t msi_domain_ops_get_hwirq(struct msi_domain_info *info,
						msi_alloc_info_t *arg)
{
	return arg->hwirq;
}

/*
 * 默认 prepare：把输出分配参数整体清零并返回成功。
 * domain、dev 和 nvec 无专用处理；后续 set_desc/provider 可逐项填充 arg。
 */
static int msi_domain_ops_prepare(struct irq_domain *domain, struct device *dev,
				  int nvec, msi_alloc_info_t *arg)
{
	memset(arg, 0, sizeof(*arg));
	return 0;
}

/* 默认 teardown 不拥有额外准备资源，因此为空操作。 */
static void msi_domain_ops_teardown(struct irq_domain *domain, msi_alloc_info_t *arg)
{
}

/* 默认 set_desc 把当前借用 msi_desc 写入分配参数，供 get_hwirq/msi_init 使用。 */
static void msi_domain_ops_set_desc(msi_alloc_info_t *arg,
				    struct msi_desc *desc)
{
	arg->desc = desc;
}

/*
 * 默认初始化一个 virq 的 MSI domain 层。
 * 设置该层 hwirq、irq_chip 和 chip_data；若 handler 与名称同时存在，再安装流控
 * handler 及可选 handler_data。调用者已经构造 irq_data 链且持 root mutex。
 * 当前 helper 总返回 0；provider 若需可失败初始化应覆盖 msi_init。
 */
static int msi_domain_ops_init(struct irq_domain *domain,
			       struct msi_domain_info *info,
			       unsigned int virq, irq_hw_number_t hwirq,
			       msi_alloc_info_t *arg)
{
	irq_domain_set_hwirq_and_chip(domain, virq, hwirq, info->chip,
				      info->chip_data);
	if (info->handler && info->handler_name) {
		__irq_set_handler(virq, info->handler, 0, info->handler_name);
		if (info->handler_data)
			irq_set_handler_data(virq, info->handler_data);
	}
	return 0;
}

/* 缺省 provider 操作集合；可整套使用，也可按 MSI_FLAG_USE_DEF_DOM_OPS 补洞。 */
static struct msi_domain_ops msi_domain_ops_default = {
	.get_hwirq		= msi_domain_ops_get_hwirq,
	.msi_init		= msi_domain_ops_init,
	.msi_prepare		= msi_domain_ops_prepare,
	.msi_teardown		= msi_domain_ops_teardown,
	.set_desc		= msi_domain_ops_set_desc,
};

/*
 * 为 msi_domain_info 补齐可选的 provider 操作。
 * ops 为空时直接借用全局默认表；非空且设置 USE_DEF_DOM_OPS 时，仅把缺失槽位
 * 写成默认实现，保留 provider 覆盖。调用发生在 domain 发布前，但会原位修改
 * 调用者提供的 ops 表，因此该表必须可写且生命周期覆盖 domain。
 */
static void msi_domain_update_dom_ops(struct msi_domain_info *info)
{
	struct msi_domain_ops *ops = info->ops;

	if (ops == NULL) {
		/* 没有 provider 表时整表借用只读的全局默认实现。 */
		info->ops = &msi_domain_ops_default;
		return;
	}

	if (!(info->flags & MSI_FLAG_USE_DEF_DOM_OPS))
		return;

	/* provider 表存在时只填空槽，绝不覆盖其专用实现。 */
	if (ops->get_hwirq == NULL)
		ops->get_hwirq = msi_domain_ops_default.get_hwirq;
	if (ops->msi_init == NULL)
		ops->msi_init = msi_domain_ops_default.msi_init;
	if (ops->msi_prepare == NULL)
		ops->msi_prepare = msi_domain_ops_default.msi_prepare;
	if (ops->msi_teardown == NULL)
		ops->msi_teardown = msi_domain_ops_default.msi_teardown;
	if (ops->set_desc == NULL)
		ops->set_desc = msi_domain_ops_default.set_desc;
}

/*
 * 校验并补齐 irq_chip 的通用 MSI 操作。
 * mask/unmask 是 MSI chip 必需契约，缺失直接 BUG；若 provider 未声明无亲和性且
 * 未实现 irq_set_affinity，则安装层级通用 setter。只在 domain 创建前修改可写 chip。
 */
static void msi_domain_update_chip_ops(struct msi_domain_info *info)
{
	struct irq_chip *chip = info->chip;

	BUG_ON(!chip || !chip->irq_mask || !chip->irq_unmask);
	if (!chip->irq_set_affinity && !(info->flags & MSI_FLAG_NO_AFFINITY))
		chip->irq_set_affinity = msi_domain_set_affinity;
}

/*
 * 创建并发布一个通用或 per-device MSI 层级 domain。
 * 先验证 hwsize；0 为兼容“无硬件表/未知容量”，扩展为最大索引空间。随后补齐
 * domain/chip ops，调用 irq_domain_create_hierarchy() 发布带 MSI 标志的 domain。
 * 成功后更新总线 token、设备指针，并可从 parent 借用 pm_dev；失败返回 NULL。
 *
 * @info、其 ops/chip 与可选数据均作为 host_data 被 domain 长期借用，调用者必须
 * 保证其寿命。若设置 MSI_FLAG_PARENT_PM_DEV，@parent 必须非空且有效。
 */
static struct irq_domain *__msi_create_irq_domain(struct fwnode_handle *fwnode,
						  struct msi_domain_info *info,
						  unsigned int flags,
						  struct irq_domain *parent)
{
	struct irq_domain *domain;

	if (info->hwsize > MSI_XA_DOMAIN_SIZE)
		return NULL;

	/*
	 * Hardware size 0 is valid for backwards compatibility and for
	 * domains which are not backed by a hardware table. Grant the
	 * maximum index space.
	 */
	/* hwsize=0 兼容无硬件表或未知容量的 domain，授予完整 MSI 索引空间。 */
	if (!info->hwsize)
		info->hwsize = MSI_XA_DOMAIN_SIZE;

	msi_domain_update_dom_ops(info);
	if (info->flags & MSI_FLAG_USE_DEF_CHIP_OPS)
		msi_domain_update_chip_ops(info);

	domain = irq_domain_create_hierarchy(parent, flags | IRQ_DOMAIN_FLAG_MSI, 0,
					     fwnode, &msi_domain_ops, info);

	if (domain) {
		irq_domain_update_bus_token(domain, info->bus_token);
		domain->dev = info->dev;
		if (info->flags & MSI_FLAG_PARENT_PM_DEV)
			domain->pm_dev = parent->pm_dev;
	}

	return domain;
}

/**
 * msi_create_irq_domain - Create an MSI interrupt domain
 * @fwnode:	Optional fwnode of the interrupt controller
 * @info:	MSI domain info
 * @parent:	Parent irq domain
 *
 * Return: pointer to the created &struct irq_domain or %NULL on failure
 */
/*
 * 用 @fwnode、@info 和 @parent 创建普通 MSI irq_domain。
 * 此薄封装不附加 device-domain 标志；成功返回已发布 domain 的借用/管理指针，
 * 失败返回 NULL，最终由调用者 irq_domain_remove()。
 */
struct irq_domain *msi_create_irq_domain(struct fwnode_handle *fwnode,
					 struct msi_domain_info *info,
					 struct irq_domain *parent)
{
	return __msi_create_irq_domain(fwnode, info, 0, parent);
}

/**
 * msi_create_parent_irq_domain - Create an MSI-parent interrupt domain
 * @info:		MSI irqdomain creation info
 * @msi_parent_ops:	MSI parent callbacks and configuration
 *
 * Return: pointer to the created &struct irq_domain or %NULL on failure
 */
/*
 * 创建可为每设备 MSI domain 提供能力约束的 MSI parent domain。
 * 规范化 size/hwirq_max，设置 MSI_PARENT 标志和选择 token，再通过通用实例化入口
 * 发布；成功后安装长期借用的 msi_parent_ops。失败把 ERR_PTR 转换为 NULL。
 */
struct irq_domain *msi_create_parent_irq_domain(struct irq_domain_info *info,
						const struct msi_parent_ops *msi_parent_ops)
{
	struct irq_domain *d;

	info->hwirq_max		= max(info->hwirq_max, info->size);
	info->size		= info->hwirq_max;
	info->domain_flags	|= IRQ_DOMAIN_FLAG_MSI_PARENT;
	info->bus_token		= msi_parent_ops->bus_select_token;

	d = irq_domain_instantiate(info);
	if (IS_ERR(d))
		return NULL;

	d->msi_parent_ops = msi_parent_ops;
	return d;
}
EXPORT_SYMBOL_GPL(msi_create_parent_irq_domain);

/**
 * msi_parent_init_dev_msi_info - Delegate initialization of device MSI info down
 *				  in the domain hierarchy
 * @dev:		The device for which the domain should be created
 * @domain:		The domain in the hierarchy this op is being called on
 * @msi_parent_domain:	The IRQ_DOMAIN_FLAG_MSI_PARENT domain for the child to
 *			be created
 * @msi_child_info:	The MSI domain info of the IRQ_DOMAIN_FLAG_MSI_DEVICE
 *			domain to be created
 *
 * Return: true on success, false otherwise
 *
 * This is the most complex problem of per device MSI domains and the
 * underlying interrupt domain hierarchy:
 *
 * The device domain to be initialized requests the broadest feature set
 * possible and the underlying domain hierarchy puts restrictions on it.
 *
 * That's trivial for a simple parent->child relationship, but it gets
 * interesting with an intermediate domain: root->parent->child.  The
 * intermediate 'parent' can expand the capabilities which the 'root'
 * domain is providing. So that creates a classic hen and egg problem:
 * Which entity is doing the restrictions/expansions?
 *
 * One solution is to let the root domain handle the initialization that's
 * why there is the @domain and the @msi_parent_domain pointer.
 */
/*
 * 沿 MSI domain 层级向父级委托 per-device 子 domain 的能力初始化。
 * @dev 是目标设备；@domain 是当前委托层；@msi_parent_domain 是最终承载子层的
 * MSI-parent；@msi_child_info 是可原位收缩/扩展的子 domain 能力输出。成功返回
 * true，缺少父级或回调视为层级构造错误并告警返回 false。
 *
 * 子层模板先请求最宽能力，底层层级再施加限制。简单 parent->child 可直接完成；
 * root->parent->child 中间层可能在 root 能力上再扩展，形成“由谁最终裁决”的循环
 * 依赖。这里把请求逐层交给 parent，允许最终由根层结合当前 domain 与目标
 * msi_parent_domain 统一决定，避免中间层越权假设硬件能力。
 */
bool msi_parent_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				  struct irq_domain *msi_parent_domain,
				  struct msi_domain_info *msi_child_info)
{
	struct irq_domain *parent = domain->parent;

	if (WARN_ON_ONCE(!parent || !parent->msi_parent_ops ||
			 !parent->msi_parent_ops->init_dev_msi_info))
		return false;

	return parent->msi_parent_ops->init_dev_msi_info(dev, parent, msi_parent_domain,
							 msi_child_info);
}

/**
 * msi_create_device_irq_domain - Create a device MSI interrupt domain
 * @dev:		Pointer to the device
 * @domid:		Domain id
 * @template:		MSI domain info bundle used as template
 * @hwsize:		Maximum number of MSI table entries (0 if unknown or unlimited)
 * @domain_data:	Optional pointer to domain specific data which is set in
 *			msi_domain_info::data
 * @chip_data:		Optional pointer to chip specific data which is set in
 *			msi_domain_info::chip_data
 *
 * Return: True on success, false otherwise
 *
 * There is no firmware node required for this interface because the per
 * device domains are software constructs which are actually closer to the
 * hardware reality than any firmware can describe them.
 *
 * The domain name and the irq chip name for a MSI device domain are
 * composed by: "$(PREFIX)$(CHIPNAME)-$(DEVNAME)"
 *
 * $PREFIX:   Optional prefix provided by the underlying MSI parent domain
 *	      via msi_parent_ops::prefix. If that pointer is NULL the prefix
 *	      is empty.
 * $CHIPNAME: The name of the irq_chip in @template
 * $DEVNAME:  The name of the device
 *
 * This results in understandable chip names and hardware interrupt numbers
 * in e.g. /proc/interrupts
 *
 * PCI-MSI-0000:00:1c.0     0-edge  Parent domain has no prefix
 * IR-PCI-MSI-0000:00:1c.4  0-edge  Same with interrupt remapping prefix 'IR-'
 *
 * IR-PCI-MSIX-0000:3d:00.0 0-edge  Hardware interrupt numbers reflect
 * IR-PCI-MSIX-0000:3d:00.0 1-edge  the real MSI-X index on that device
 * IR-PCI-MSIX-0000:3d:00.0 2-edge
 *
 * On IMS domains the hardware interrupt number is either a table entry
 * index or a purely software managed index but it is guaranteed to be
 * unique.
 *
 * The domain pointer is stored in @dev::msi::data::__irqdomains[]. All
 * subsequent operations on the domain depend on the domain id.
 *
 * The domain is automatically freed when the device is removed via devres
 * in the context of @dev::msi::data freeing, but it can also be
 * independently removed via @msi_remove_device_irq_domain().
 */
/*
 * 从只读 @template 为 @dev 的 @domid 创建独立 per-device MSI domain。
 * @hwsize 为表项容量，0 表示未知/不限；@domain_data 与 @chip_data 只借给新 bundle
 * 中的 info/chip。成功返回 true，失败返回 false 且自动清理本次临时资源。
 *
 * 阶段 1：验证设备的全局 domain 是 MSI parent、domid 合法，深拷贝整个 template；
 * 让 info/chip/ops/alloc_data 全部指向同一 bundle 内部，从而由 domain 独占其寿命。
 * 名称按“父前缀 + chip 名 + 设备名”生成，使 /proc/interrupts 可辨认；MSI-X 的
 * hwirq 对应真实表索引，IMS 则为硬件表项或保证唯一的软件索引。
 *
 * 阶段 2：wired-to-MSI 必须借用设备真实 fwnode 才能由固件查找匹配；PCI/MSI 等
 * 只在设备上下文按 domid 使用，故分配命名 fwnode。`__free` 变量在任何早退时
 * 自动释放 bundle/命名节点。
 *
 * 阶段 3：建立设备 MSI 数据并持其 mutex，拒绝重复槽位；由 parent 回调约束子层
 * 能力，再创建并发布 MSI_DEVICE domain。写入 __domains[domid].domain 是设备管理
 * 路径的发布点。prepare 失败会先撤销槽位、移除 domain，随后作用域清理临时对象。
 *
 * 阶段 4：prepare 成功后 domain 已长期借用 bundle 和可选命名 fwnode，
 * retain_and_null_ptr() 明确取消作用域自动清理，把所有权转交 remove/devres 路径。
 */
bool msi_create_device_irq_domain(struct device *dev, unsigned int domid,
				  const struct msi_domain_template *template,
				  unsigned int hwsize, void *domain_data,
				  void *chip_data)
{
	struct irq_domain *domain, *parent = dev->msi.domain;
	const struct msi_parent_ops *pops;
	struct fwnode_handle *fwnode;

	if (!irq_domain_is_msi_parent(parent))
		return false;

	if (domid >= MSI_MAX_DEVICE_IRQDOMAINS)
		return false;

	/* 阶段一：复制模板并把所有内部指针重定向到新 bundle。 */
	struct msi_domain_template *bundle __free(kfree) =
		kmemdup(template, sizeof(*bundle), GFP_KERNEL);
	if (!bundle)
		return false;

	bundle->info.hwsize = hwsize;
	bundle->info.chip = &bundle->chip;
	bundle->info.ops = &bundle->ops;
	bundle->info.data = domain_data;
	bundle->info.chip_data = chip_data;
	bundle->info.alloc_data = &bundle->alloc_info;
	bundle->info.dev = dev;

	/* 名称同时成为 irq_chip 名，便于 /proc/interrupts 标识设备实例。 */
	pops = parent->msi_parent_ops;
	snprintf(bundle->name, sizeof(bundle->name), "%s%s-%s",
		 pops->prefix ? : "", bundle->chip.name, dev_name(dev));
	bundle->chip.name = bundle->name;

	/*
	 * Using the device firmware node is required for wire to MSI
	 * device domains so that the existing firmware results in a domain
	 * match.
	 * All other device domains like PCI/MSI use the named firmware
	 * node as they are not guaranteed to have a fwnode. They are never
	 * looked up and always handled in the context of the device.
	 */
	/*
	 * wired-to-MSI 使用设备真实 fwnode，才能与已有固件中断描述匹配。PCI/MSI 等
	 * 设备 domain 不保证有真实节点，也从不全局查找，因此使用独立命名 fwnode。
	 */
	struct fwnode_handle *fwnode_alloced __free(irq_domain_free_fwnode) = NULL;

	if (!(bundle->info.flags & MSI_FLAG_USE_DEV_FWNODE))
		fwnode = fwnode_alloced = irq_domain_alloc_named_fwnode(bundle->name);
	else
		fwnode = dev->fwnode;

	if (!fwnode)
		return false;

	/* 阶段二：建立设备容器并在锁内确认目标 domid 尚未发布。 */
	if (msi_setup_device_data(dev))
		return false;

	guard(msi_descs_lock)(dev);
	if (WARN_ON_ONCE(msi_get_device_domain(dev, domid)))
		return false;

	if (!pops->init_dev_msi_info(dev, parent, parent, &bundle->info))
		return false;

	/* 阶段三：创建 domain，先发布到设备槽，再执行可能依赖该槽的 prepare。 */
	domain = __msi_create_irq_domain(fwnode, &bundle->info, IRQ_DOMAIN_FLAG_MSI_DEVICE, parent);
	if (!domain)
		return false;

	dev->msi.data->__domains[domid].domain = domain;

	if (msi_domain_prepare_irqs(domain, dev, hwsize, &bundle->alloc_info)) {
		/* prepare 失败必须先撤销设备槽的可见性，再销毁未完成 domain。 */
		dev->msi.data->__domains[domid].domain = NULL;
		irq_domain_remove(domain);
		return false;
	}

	/* @bundle and @fwnode_alloced are now in use. Prevent cleanup */
	/* domain 已取得长期使用权，清空 cleanup 指针把释放责任转交移除路径。 */
	retain_and_null_ptr(bundle);
	retain_and_null_ptr(fwnode_alloced);
	return true;
}

/**
 * msi_remove_device_irq_domain - Free a device MSI interrupt domain
 * @dev:	Pointer to the device
 * @domid:	Domain id
 */
/*
 * 移除并释放 @dev 的 @domid per-device MSI domain。
 * 持设备 MSI mutex 查找并确认 MSI_DEVICE 类型；先清空 domain 槽阻止后续管理入口
 * 取得它，再调用 provider teardown。随后保存 domain 的 fwnode，移除 irq_domain
 * 以释放其引用，再把该指针交给 irq_domain_free_fwnode()；后者只真正释放
 * irqchip 专用节点，对设备真实 fwnode 会告警并返回。最后由仍独立存活的 info
 * 恢复并释放整个 template bundle。
 *
 * 调用者必须已释放该 domain 的所有 IRQ 和描述符；本函数对空槽/非 device domain
 * 幂等返回。bundle 不嵌在 irq_domain 内，所以可在 irq_domain_remove() 后由
 * container_of(info, ...) 回收；任何外部借用都必须在此之前终止。
 */
void msi_remove_device_irq_domain(struct device *dev, unsigned int domid)
{
	struct fwnode_handle *fwnode = NULL;
	struct msi_domain_info *info;
	struct irq_domain *domain;

	guard(msi_descs_lock)(dev);
	domain = msi_get_device_domain(dev, domid);
	if (!domain || !irq_domain_is_msi_device(domain))
		return;

	dev->msi.data->__domains[domid].domain = NULL;
	info = domain->host_data;

	info->ops->msi_teardown(domain, info->alloc_data);

	if (irq_domain_is_msi_device(domain))
		fwnode = domain->fwnode;
	irq_domain_remove(domain);
	irq_domain_free_fwnode(fwnode);
	kfree(container_of(info, struct msi_domain_template, info));
}

/**
 * msi_match_device_irq_domain - Match a device irq domain against a bus token
 * @dev:	Pointer to the device
 * @domid:	Domain id
 * @bus_token:	Bus token to match against the domain bus token
 *
 * Return: True if device domain exists and bus tokens match.
 */
/*
 * 在设备锁下检查 @domid 是否存在 MSI_DEVICE domain，且其 info->bus_token 等于
 * @bus_token。返回纯布尔快照，不泄露 domain 指针或取得引用；空槽、类型不符、
 * token 不同均返回 false。
 */
bool msi_match_device_irq_domain(struct device *dev, unsigned int domid,
				 enum irq_domain_bus_token bus_token)
{
	struct msi_domain_info *info;
	struct irq_domain *domain;

	guard(msi_descs_lock)(dev);
	domain = msi_get_device_domain(dev, domid);
	if (domain && irq_domain_is_msi_device(domain)) {
		info = domain->host_data;
		return info->bus_token == bus_token;
	}
	return false;
}

/*
 * 调用 MSI provider 的 msi_prepare()，为即将分配的 @nvec 个向量填充 @arg。
 * @arg 是调用者拥有的输入输出缓冲区；回调可获取临时资源，成功后必须最终与
 * msi_teardown() 配对。返回 0 或 provider 错误，调用环境持设备 MSI mutex。
 */
static int msi_domain_prepare_irqs(struct irq_domain *domain, struct device *dev,
				   int nvec, msi_alloc_info_t *arg)
{
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;

	return ops->msi_prepare(domain, dev, nvec, arg);
}

/*
 * Carefully check whether the device can use reservation mode. If
 * reservation mode is enabled then the early activation will assign a
 * dummy vector to the device. If the PCI/MSI device does not support
 * masking of the entry then this can result in spurious interrupts when
 * the device driver is not absolutely careful. But even then a malfunction
 * of the hardware could result in a spurious interrupt on the dummy vector
 * and render the device unusable. If the entry can be masked then the core
 * logic will prevent the spurious interrupt and reservation mode can be
 * used. For now reservation mode is restricted to PCI/MSI.
 */
/*
 * 谨慎判断设备能否使用 MSI 向量 reservation mode。
 * 该模式在早期激活时先给设备一个哑向量，直到 request_irq() 才占用真实向量。
 * 若 PCI/MSI 表项不能 mask，驱动疏忽或硬件故障可能在哑向量上产生杂散中断，
 * 甚至使设备不可用；可 mask 时核心可阻止投递，才允许预留。
 *
 * 当前仅接受 PCI MSI/MSI-X/VMD 总线 token，并要求 MUST_REACTIVATE、没有 NO_MASK，
 * 且首个描述符证明 MSI-X 或普通 MSI 的 can_mask 能力。设备同类描述符能力一致，
 * 因而检查第一项足够。调用者持 MSI mutex；返回 true/false，不改变状态。
 */
static bool msi_check_reservation_mode(struct irq_domain *domain,
				       struct msi_domain_info *info,
				       struct device *dev)
{
	struct msi_desc *desc;

	switch(domain->bus_token) {
	case DOMAIN_BUS_PCI_MSI:
	case DOMAIN_BUS_PCI_DEVICE_MSI:
	case DOMAIN_BUS_PCI_DEVICE_MSIX:
	case DOMAIN_BUS_VMD_MSI:
		break;
	default:
		return false;
	}

	if (!(info->flags & MSI_FLAG_MUST_REACTIVATE))
		return false;

	if (info->flags & MSI_FLAG_NO_MASK)
		return false;

	/*
	 * Checking the first MSI descriptor is sufficient. MSIX supports
	 * masking and MSI does so when the can_mask attribute is set.
	 */
	/* MSI-X 固有 mask 能力；普通 MSI 由 can_mask 标志声明，检查首项即可代表设备。 */
	desc = msi_first_desc(dev, MSI_DESC_ALL);
	return desc->pci.msi_attrib.is_msix || desc->pci.msi_attrib.can_mask;
}

/*
 * 把底层 IRQ 分配失败转换成 PCI/MSI 兼容返回语义。
 * 非 PCI token 或内核未启用 PCI_MSI 一律 -ENOSPC。PCI multi-MSI 描述符请求多个
 * 向量时返回 1，提示上层缩小数量重试；单向量失败若此前已有成功描述符则返回
 * 成功数量，否则 -ENOSPC。这里不回滚，外层 msi_domain_alloc_locked() 统一释放。
 */
static int msi_handle_pci_fail(struct irq_domain *domain, struct msi_desc *desc,
			       int allocated)
{
	switch(domain->bus_token) {
	case DOMAIN_BUS_PCI_MSI:
	case DOMAIN_BUS_PCI_DEVICE_MSI:
	case DOMAIN_BUS_PCI_DEVICE_MSIX:
	case DOMAIN_BUS_VMD_MSI:
		if (IS_ENABLED(CONFIG_PCI_MSI))
			break;
		fallthrough;
	default:
		return -ENOSPC;
	}

	/* Let a failed PCI multi MSI allocation retry */
	/* multi-MSI 允许上层改用单向量或更小的 2 次幂数量重试。 */
	if (desc->nvec_used > 1)
		return 1;

	/* If there was a successful allocation let the caller know */
	/* 单向量批次部分成功时返回已成功描述符数，否则报告空间不足。 */
	return allocated ? allocated : -ENOSPC;
}

/* 初始化 virq 时允许先绑定哑向量，等待 request_irq() 完成最终激活。 */
#define VIRQ_CAN_RESERVE	0x01
/* 分配阶段必须立即调用 irq_domain_activate_irq() 写入 MSI 消息。 */
#define VIRQ_ACTIVATE		0x02

/*
 * 完成一个新 MSI virq 的可预留属性、managed 状态和可选早期激活。
 * 无 reservation 时清除 CAN_RESERVE；若要求早激活的 managed IRQ 当前没有任何
 * 在线 CPU 可服务，则标记 MANAGED_SHUTDOWN 并推迟硬件激活。x86 reservation
 * 使用 catch-all 向量处理该情况，因此只在非 reserve 路径判断。
 *
 * 未设置 ACTIVATE 时完成纯状态初始化。需要激活则调用 irqdomain 两阶段协议；
 * reserve 成功后再次清 activated 位，使 request_irq() 仍会用最终真实向量激活。
 * 返回 0 或 activate 错误；调用者已建立完整 irq_data 链。
 */
static int msi_init_virq(struct irq_domain *domain, int virq, unsigned int vflags)
{
	struct irq_data *irqd = irq_domain_get_irq_data(domain, virq);
	int ret;

	if (!(vflags & VIRQ_CAN_RESERVE)) {
		irqd_clr_can_reserve(irqd);

		/*
		 * If the interrupt is managed but no CPU is available to
		 * service it, shut it down until better times. Note that
		 * we only do this on the !RESERVE path as x86 (the only
		 * architecture using this flag) deals with this in a
		 * different way by using a catch-all vector.
		 */
		/*
		 * managed IRQ 没有在线目标 CPU 时保持 shutdown，待 CPU/亲和性条件改善。
		 * reservation 路径由 x86 的 catch-all 向量处理，不在这里关闭。
		 */
		if ((vflags & VIRQ_ACTIVATE) &&
		    irqd_affinity_is_managed(irqd) &&
		    !cpumask_intersects(irq_data_get_affinity_mask(irqd),
					cpu_online_mask)) {
			    irqd_set_managed_shutdown(irqd);
			    return 0;
		    }
	}

	if (!(vflags & VIRQ_ACTIVATE))
		return 0;

	ret = irq_domain_activate_irq(irqd, vflags & VIRQ_CAN_RESERVE);
	if (ret)
		return ret;
	/*
	 * If the interrupt uses reservation mode, clear the activated bit
	 * so request_irq() will assign the final vector.
	 */
	/* 预留模式的早激活只装哑向量，清状态以便 request_irq() 再装最终向量。 */
	if (vflags & VIRQ_CAN_RESERVE)
		irqd_clr_activated(irqd);
	return 0;
}

/*
 * 为一次批量分配准备 msi_alloc_info_t。
 * 新式 per-device domain 在 info->alloc_data 保存模板，直接按值复制到输出 @arg；
 * 旧式 domain 没有模板时调用 msi_prepare() 动态填充。返回 0 或 prepare 错误。
 */
static int populate_alloc_info(struct irq_domain *domain, struct device *dev,
			       unsigned int nirqs, msi_alloc_info_t *arg)
{
	struct msi_domain_info *info = domain->host_data;

	/*
	 * If the caller has provided a template alloc info, use that. Once
	 * all users of msi_create_irq_domain() have been eliminated, this
	 * should be the only source of allocation information, and the
	 * prepare call below should be finally removed.
	 */
	/*
	 * 有模板时优先复制；待旧 msi_create_irq_domain() 用户消失后，模板将成为唯一
	 * 来源，动态 prepare 兼容路径即可移除。
	 */
	if (!info->alloc_data)
		return msi_domain_prepare_irqs(domain, dev, nirqs, arg);

	*arg = *info->alloc_data;
	return 0;
}

/*
 * 为 @ctrl 区间内尚未关联的 MSI 描述符建立 Linux IRQ 和硬件投递状态。
 * 调用者持设备 MSI mutex，描述符由 XArray 拥有。本函数可能留下部分成功结果；
 * 任何非零返回都由外层 msi_domain_alloc_locked() 调用统一 free 路径回滚。
 *
 * 阶段 1：从模板或 prepare 回调得到共享 alloc_info；根据 provider 标志决定是否
 * 必须在 PCI 开启 MSI 前早激活（否则设备可能锁存随机消息），并严格检查能否用
 * reservation 哑向量。
 *
 * 阶段 2：遍历闭区间，只处理 desc->irq==0 的对象。provider 可先按描述符调整 arg，
 * set_desc 再绑定当前 desc；随后让 irqdomain 分配 desc->nvec_used 个连续 virq 和
 * 完整 irq_data 层级。PCI 分配失败转换为“缩小 multi-MSI 重试/部分成功”语义。
 *
 * 阶段 3：逐向量把 msi_desc 及偏移写入 irq_data、复制设备名到 debugfs，并执行
 * managed/reservation/早激活状态机。全部向量成功后可发布 sysfs 文件，最后才把
 * allocated 计数加一。成功返回 0；prepare、irqdomain、activate、sysfs 或内部范围
 * 错误返回非零，调用者随后撤销本区间所有已关联 IRQ 和可选简单描述符。
 */
static int __msi_domain_alloc_irqs(struct device *dev, struct irq_domain *domain,
				   struct msi_ctrl *ctrl)
{
	struct xarray *xa = &dev->msi.data->__domains[ctrl->domid].store;
	struct msi_domain_info *info = domain->host_data;
	struct msi_domain_ops *ops = info->ops;
	unsigned int vflags = 0, allocated = 0;
	msi_alloc_info_t arg = { };
	struct msi_desc *desc;
	unsigned long idx;
	int i, ret, virq;

	ret = populate_alloc_info(domain, dev, ctrl->nirqs, &arg);
	if (ret)
		return ret;

	/*
	 * This flag is set by the PCI layer as we need to activate
	 * the MSI entries before the PCI layer enables MSI in the
	 * card. Otherwise the card latches a random msi message.
	 */
	/* PCI 层启用设备 MSI 前必须先写有效消息，否则硬件会锁存随机内容。 */
	if (info->flags & MSI_FLAG_ACTIVATE_EARLY)
		vflags |= VIRQ_ACTIVATE;

	/*
	 * Interrupt can use a reserved vector and will not occupy
	 * a real device vector until the interrupt is requested.
	 */
	/* 可安全 mask 的设备先用预留向量，直到 request_irq() 才占真实设备向量。 */
	if (msi_check_reservation_mode(domain, info, dev))
		vflags |= VIRQ_CAN_RESERVE;

	xa_for_each_range(xa, idx, desc, ctrl->first, ctrl->last) {
		if (!msi_desc_match(desc, MSI_DESC_NOTASSOCIATED))
			continue;

		/* This should return -ECONFUSED... */
		/* 未关联描述符数超过请求 nirqs 表示调用者控制块与仓库状态自相矛盾。 */
		if (WARN_ON_ONCE(allocated >= ctrl->nirqs))
			return -EINVAL;

		if (ops->prepare_desc)
			ops->prepare_desc(domain, &arg, desc);

		ops->set_desc(&arg, desc);

		virq = __irq_domain_alloc_irqs(domain, -1, desc->nvec_used,
					       dev_to_node(dev), &arg, false,
					       desc->affinity);
		if (virq < 0)
			return msi_handle_pci_fail(domain, desc, allocated);

		for (i = 0; i < desc->nvec_used; i++) {
			irq_set_msi_desc_off(virq, i, desc);
			irq_debugfs_copy_devname(virq + i, dev);
			ret = msi_init_virq(domain, virq + i, vflags);
			if (ret)
				return ret;
		}
		if (info->flags & MSI_FLAG_DEV_SYSFS) {
			ret = msi_sysfs_populate_desc(dev, desc);
			if (ret)
				return ret;
		}
		allocated++;
	}
	return 0;
}

/*
 * 按 domain 标志决定是否由核心为 @ctrl 区间预建单向量描述符。
 * 未设置 ALLOC_SIMPLE_MSI_DESCS 时描述符由 PCI 等调用点准备，直接成功；设置时
 * 调用批量创建 helper，返回其 0/错误。
 */
static int msi_domain_alloc_simple_msi_descs(struct device *dev,
					     struct msi_domain_info *info,
					     struct msi_ctrl *ctrl)
{
	if (!(info->flags & MSI_FLAG_ALLOC_SIMPLE_MSI_DESCS))
		return 0;

	return msi_domain_add_simple_msi_descs(dev, ctrl);
}

/*
 * 设备 MSI mutex 已持有时执行一次分配的内部调度。
 * 验证范围、取得 device domain、可选创建核心管理的简单描述符；随后若 provider
 * 有整域 domain_alloc_irqs 回调则完全委托，否则使用逐描述符通用实现。该层不做
 * 失败回滚，便于外层统一覆盖自定义和通用路径。
 */
static int __msi_domain_alloc_locked(struct device *dev, struct msi_ctrl *ctrl)
{
	struct msi_domain_info *info;
	struct msi_domain_ops *ops;
	struct irq_domain *domain;
	int ret;

	if (!msi_ctrl_valid(dev, ctrl))
		return -EINVAL;

	domain = msi_get_device_domain(dev, ctrl->domid);
	if (!domain)
		return -ENODEV;

	info = domain->host_data;

	/* 核心管理型 domain 先补齐描述符；调用点管理型 domain 保持原仓库。 */
	ret = msi_domain_alloc_simple_msi_descs(dev, info, ctrl);
	if (ret)
		return ret;

	ops = info->ops;
	/* 整域回调拥有优先权，否则进入通用的逐描述符 irqdomain 分配。 */
	if (ops->domain_alloc_irqs)
		return ops->domain_alloc_irqs(domain, dev, ctrl->nirqs);

	return __msi_domain_alloc_irqs(dev, domain, ctrl);
}

/*
 * 锁内 MSI 分配事务包装器。
 * 内部调度成功返回 0；任何错误立即对同一 @ctrl 调用 msi_domain_free_locked()，
 * 撤销已激活 IRQ、sysfs 及按标志由核心创建的描述符，然后返回原错误。
 */
static int msi_domain_alloc_locked(struct device *dev, struct msi_ctrl *ctrl)
{
	int ret = __msi_domain_alloc_locked(dev, ctrl);

	if (ret)
		msi_domain_free_locked(dev, ctrl);
	return ret;
}

/**
 * msi_domain_alloc_irqs_range_locked - Allocate interrupts from a MSI interrupt domain
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @domid:	Id of the interrupt domain to operate on
 * @first:	First index to allocate (inclusive)
 * @last:	Last index to allocate (inclusive)
 *
 * Must be invoked from within a msi_lock_descs() / msi_unlock_descs()
 * pair. Use this for MSI irqdomains which implement their own descriptor
 * allocation/free.
 *
 * Return: %0 on success or an error code.
 */
/*
 * 在调用者已持 MSI mutex 时，为 @domid 的 [@first,@last] 每个索引分配一个 IRQ。
 * 适用于 provider 自己管理描述符的 domain；nirqs 由闭区间长度计算。成功 0，
 * 失败由锁内事务完整回滚并返回错误。
 */
int msi_domain_alloc_irqs_range_locked(struct device *dev, unsigned int domid,
				       unsigned int first, unsigned int last)
{
	struct msi_ctrl ctrl = {
		.domid	= domid,
		.first	= first,
		.last	= last,
		.nirqs	= last + 1 - first,
	};

	return msi_domain_alloc_locked(dev, &ctrl);
}

/**
 * msi_domain_alloc_irqs_range - Allocate interrupts from a MSI interrupt domain
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @domid:	Id of the interrupt domain to operate on
 * @first:	First index to allocate (inclusive)
 * @last:	Last index to allocate (inclusive)
 *
 * Return: %0 on success or an error code.
 */
/*
 * 自行获取设备 MSI mutex 后分配指定闭区间 IRQ。
 * 参数及返回语义与 locked 版本相同；guard 确保所有早退均释放锁。
 */
int msi_domain_alloc_irqs_range(struct device *dev, unsigned int domid,
				unsigned int first, unsigned int last)
{

	guard(msi_descs_lock)(dev);
	return msi_domain_alloc_irqs_range_locked(dev, domid, first, last);
}
EXPORT_SYMBOL_GPL(msi_domain_alloc_irqs_range);

/**
 * msi_domain_alloc_irqs_all_locked - Allocate all interrupts from a MSI interrupt domain
 *
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @domid:	Id of the interrupt domain to operate on
 * @nirqs:	The number of interrupts to allocate
 *
 * This function scans all MSI descriptors of the MSI domain and allocates interrupts
 * for all unassigned ones. That function is to be used for MSI domain usage where
 * the descriptor allocation is handled at the call site, e.g. PCI/MSI[X].
 *
 * Return: %0 on success or an error code.
 */
/*
 * 在已持 MSI mutex 时扫描 @domid 完整硬件索引空间，为所有未关联描述符分配总计
 * @nirqs 个 Linux IRQ。描述符由 PCI/MSI-X 等调用点预先建立；成功返回 0，失败
 * 统一回滚。multi-MSI 可能一个描述符覆盖多个 nirqs。
 */
int msi_domain_alloc_irqs_all_locked(struct device *dev, unsigned int domid, int nirqs)
{
	struct msi_ctrl ctrl = {
		.domid	= domid,
		.first	= 0,
		.last	= msi_domain_get_hwsize(dev, domid) - 1,
		.nirqs	= nirqs,
	};

	return msi_domain_alloc_locked(dev, &ctrl);
}

/*
 * 在设备 MSI mutex 下创建一个单向量描述符，并在指定或任意空闲索引分配 IRQ。
 * @affdesc 可空且会深拷贝；@icookie 可空，非空时按值存入描述符供 IMS/wired-to-MSI
 * provider 使用。返回 msi_map：成功 index>=0 且 virq>0；失败 index 为负错误、virq=0。
 *
 * 描述符分配后其所有权传给 msi_insert_desc()；插入成功再把实际索引写入 ctrl 并
 * 建立 IRQ。后一步失败调用 msi_domain_free_locked()，同时撤销 IRQ 和由标志决定的
 * 描述符；因此返回时不会遗留半关联对象。
 */
static struct msi_map __msi_domain_alloc_irq_at(struct device *dev, unsigned int domid,
						unsigned int index,
						const struct irq_affinity_desc *affdesc,
						union msi_instance_cookie *icookie)
{
	struct msi_ctrl ctrl = { .domid	= domid, .nirqs = 1, };
	struct irq_domain *domain;
	struct msi_map map = { };
	struct msi_desc *desc;
	int ret;

	/* 固定或任意索引的选择都必须基于当前锁内可见的 device domain。 */
	domain = msi_get_device_domain(dev, domid);
	if (!domain) {
		map.index = -ENODEV;
		return map;
	}

	desc = msi_alloc_desc(dev, 1, affdesc);
	if (!desc) {
		map.index = -ENOMEM;
		return map;
	}

	if (icookie)
		desc->data.icookie = *icookie;

	/* 插入成功是描述符所有权转交给 XArray 的提交点。 */
	ret = msi_insert_desc(dev, desc, domid, index);
	if (ret) {
		map.index = ret;
		return map;
	}

	ctrl.first = ctrl.last = desc->msi_index;

	/* 只为刚发布的单槽建立 virq；失败通过共同释放路径恢复空槽。 */
	ret = __msi_domain_alloc_irqs(dev, domain, &ctrl);
	if (ret) {
		map.index = ret;
		msi_domain_free_locked(dev, &ctrl);
	} else {
		map.index = desc->msi_index;
		map.virq = desc->irq;
	}
	return map;
}

/**
 * msi_domain_alloc_irq_at - Allocate an interrupt from a MSI interrupt domain at
 *			     a given index - or at the next free index
 *
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @domid:	Id of the interrupt domain to operate on
 * @index:	Index for allocation. If @index == %MSI_ANY_INDEX the allocation
 *		uses the next free index.
 * @affdesc:	Optional pointer to an interrupt affinity descriptor structure
 * @icookie:	Optional pointer to a domain specific per instance cookie. If
 *		non-NULL the content of the cookie is stored in msi_desc::data.
 *		Must be NULL for MSI-X allocations
 *
 * This requires a MSI interrupt domain which lets the core code manage the
 * MSI descriptors.
 *
 * Return: struct msi_map
 *
 *	On success msi_map::index contains the allocated index number and
 *	msi_map::virq the corresponding Linux interrupt number
 *
 *	On failure msi_map::index contains the error code and msi_map::virq
 *	is %0.
 */
/*
 * 这是面向驱动的加锁入口：先取得设备 MSI 描述符锁，再把实际工作交给
 * __msi_domain_alloc_irq_at()。因此索引选择、描述符插入、Linux IRQ 分配以及
 * 失败回滚都在同一个串行化区间内，不会与同一设备上的描述符遍历或释放交错。
 *
 * 该接口只适用于由 MSI 核心管理描述符的 domain。@index 可以指定固定槽位，
 * 也可以用 MSI_ANY_INDEX 请求下一个空槽；@affdesc 的内容会被复制到新描述符，
 * @icookie 则保存到实例数据中，但 MSI-X 分配不得传入 cookie。
 *
 * 返回约定刻意把错误放在 map.index：成功时 index/virq 都有效，失败时 index
 * 为负错误码且 virq 保持 0，调用者不能只检查 virq 来判断结果。
 */
struct msi_map msi_domain_alloc_irq_at(struct device *dev, unsigned int domid, unsigned int index,
				       const struct irq_affinity_desc *affdesc,
				       union msi_instance_cookie *icookie)
{
	guard(msi_descs_lock)(dev);
	return __msi_domain_alloc_irq_at(dev, domid, index, affdesc, icookie);
}

/**
 * msi_device_domain_alloc_wired - Allocate a "wired" interrupt on @domain
 * @domain:	The domain to allocate on
 * @hwirq:	The hardware interrupt number to allocate for
 * @type:	The interrupt type
 *
 * This weirdness supports wire to MSI controllers like MBIGEN.
 *
 * @hwirq is the hardware interrupt number which is handed in from
 * irq_create_fwspec_mapping(). As the wire to MSI domain is sparse, but
 * sized in firmware, the hardware interrupt number cannot be used as MSI
 * index. For the underlying irq chip the MSI index is irrelevant and
 * all it needs is the hardware interrupt number.
 *
 * To handle this the MSI index is allocated with MSI_ANY_INDEX and the
 * hardware interrupt number is stored along with the type information in
 * msi_desc::cookie so the underlying interrupt chip and domain code can
 * retrieve it.
 *
 * Return: The Linux interrupt number (> 0) or an error code
 */
/*
 * “wired-to-MSI” 控制器把传统线中断转换成 MSI。固件给出的 @hwirq 通常是稀疏
 * 编号，不能直接充当紧凑的 MSI 描述符索引，所以这里用 MSI_ANY_INDEX 分配一个
 * 任意空槽，并把 @type 放入 cookie 高 32 位、@hwirq 放入低 32 位，留给下层
 * irqchip/domain 在初始化 irq_data 时恢复真实硬件参数。
 *
 * 入口先验证 domain 确实绑定设备且带 DOMAIN_BUS_WIRED_TO_MSI 标记；持有设备
 * 描述符锁后还会再次确认它仍是默认设备 MSI domain，防止把描述符插入错误的
 * domain 槽。成功返回正的 Linux virq，失败直接返回分配阶段记录的负错误码。
 */
int msi_device_domain_alloc_wired(struct irq_domain *domain, unsigned int hwirq,
				  unsigned int type)
{
	unsigned int domid = MSI_DEFAULT_DOMAIN;
	union msi_instance_cookie icookie = { };
	struct device *dev = domain->dev;
	struct msi_map map = { };

	if (WARN_ON_ONCE(!dev || domain->bus_token != DOMAIN_BUS_WIRED_TO_MSI))
		return -EINVAL;

	icookie.value = ((u64)type << 32) | hwirq;

	guard(msi_descs_lock)(dev);
	if (WARN_ON_ONCE(msi_get_device_domain(dev, domid) != domain))
		map.index = -EINVAL;
	else
		map = __msi_domain_alloc_irq_at(dev, domid, MSI_ANY_INDEX, NULL, &icookie);
	return map.index >= 0 ? map.virq : map.index;
}

/*
 * 释放 @ctrl 覆盖范围内已经关联 Linux IRQ 的描述符。
 *
 * 调用者必须持有设备 MSI 描述符锁，因而 XArray 遍历、desc->irq 状态变化以及
 * 并发分配/释放互斥。每个描述符可能代表连续的 nvec_used 个向量；必须先逐个
 * 停用已经激活的 irq_data，使控制器不再接收该消息，再由 irqdomain 释放整段
 * virq。随后移除可选 sysfs 表示，最后把 desc->irq 清零，才重新把描述符标记为
 * “未关联”。这里不一定删除描述符本身：是否擦除 XArray 条目由外层依据
 * MSI_FLAG_FREE_MSI_DESCS 决定。
 */
static void __msi_domain_free_irqs(struct device *dev, struct irq_domain *domain,
				   struct msi_ctrl *ctrl)
{
	struct xarray *xa = &dev->msi.data->__domains[ctrl->domid].store;
	struct msi_domain_info *info = domain->host_data;
	struct irq_data *irqd;
	struct msi_desc *desc;
	unsigned long idx;
	int i;

	xa_for_each_range(xa, idx, desc, ctrl->first, ctrl->last) {
		/* Only handle MSI entries which have an interrupt associated */
		/* 只处理已经关联中断的 MSI 条目；预建但未分配的描述符保持不变。 */
		if (!msi_desc_match(desc, MSI_DESC_ASSOCIATED))
			continue;

		/* Make sure all interrupts are deactivated */
		/* 先确保所有向量都已停用，避免释放 irq_data 后硬件仍向其投递。 */
		for (i = 0; i < desc->nvec_used; i++) {
			irqd = irq_domain_get_irq_data(domain, desc->irq + i);
			if (irqd && irqd_is_activated(irqd))
				irq_domain_deactivate_irq(irqd);
		}

		irq_domain_free_irqs(desc->irq, desc->nvec_used);
		if (info->flags & MSI_FLAG_DEV_SYSFS)
			msi_sysfs_remove_desc(dev, desc);
		desc->irq = 0;
	}
}

/*
 * 在已持有设备 MSI 描述符锁的条件下释放一个索引区间。
 *
 * 无效范围或不存在的设备 domain 直接返回。domain 提供 domain_free_irqs()
 * 时，由实现方承担该设备的 IRQ 释放；否则使用通用路径按描述符停用并释放。
 * IRQ 资源释放完成后，只有设置 MSI_FLAG_FREE_MSI_DESCS 的 domain 才继续从
 * XArray 移除并销毁描述符；其余 domain 保留描述符供后续再次分配。
 */
static void msi_domain_free_locked(struct device *dev, struct msi_ctrl *ctrl)
{
	struct msi_domain_info *info;
	struct msi_domain_ops *ops;
	struct irq_domain *domain;

	if (!msi_ctrl_valid(dev, ctrl))
		return;

	domain = msi_get_device_domain(dev, ctrl->domid);
	if (!domain)
		return;

	info = domain->host_data;
	ops = info->ops;

	/* provider 整域释放与通用逐描述符释放二选一，不能重复执行。 */
	if (ops->domain_free_irqs)
		ops->domain_free_irqs(domain, dev);
	else
		__msi_domain_free_irqs(dev, domain, ctrl);

	/* IRQ 关联全部撤销后，才允许按策略销毁描述符对象。 */
	if (info->flags & MSI_FLAG_FREE_MSI_DESCS)
		msi_domain_free_descs(dev, ctrl);
}

/**
 * msi_domain_free_irqs_range_locked - Free a range of interrupts from a MSI interrupt domain
 *				       associated to @dev with msi_lock held
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are freed
 * @domid:	Id of the interrupt domain to operate on
 * @first:	First index to free (inclusive)
 * @last:	Last index to free (inclusive)
 */
/*
 * 释放与 @dev 关联的 MSI domain 中从 @first 到 @last（两端都包含）的中断。
 * 调用者已经持有 msi_lock_descs() 对应的设备描述符锁；本函数只组装控制范围并
 * 进入共同释放核心，不重复加锁。范围中的空槽或未关联描述符由底层安全跳过。
 */
void msi_domain_free_irqs_range_locked(struct device *dev, unsigned int domid,
				       unsigned int first, unsigned int last)
{
	struct msi_ctrl ctrl = {
		.domid	= domid,
		.first	= first,
		.last	= last,
	};
	msi_domain_free_locked(dev, &ctrl);
}

/**
 * msi_domain_free_irqs_range - Free a range of interrupts from a MSI interrupt domain
 *				associated to @dev
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are freed
 * @domid:	Id of the interrupt domain to operate on
 * @first:	First index to free (inclusive)
 * @last:	Last index to free (inclusive)
 */
/*
 * 这是普通调用者使用的区间释放入口。它取得设备 MSI 描述符锁，再调用 locked
 * 版本释放闭区间 [@first, @last]，从而把验证、IRQ 停用、irqdomain 释放和可选
 * 描述符销毁保持在同一个临界区内。
 */
void msi_domain_free_irqs_range(struct device *dev, unsigned int domid,
				unsigned int first, unsigned int last)
{
	guard(msi_descs_lock)(dev);
	msi_domain_free_irqs_range_locked(dev, domid, first, last);
}
EXPORT_SYMBOL_GPL(msi_domain_free_irqs_all);

/**
 * msi_domain_free_irqs_all_locked - Free all interrupts from a MSI interrupt domain
 *				     associated to a device
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are freed
 * @domid:	The id of the domain to operate on
 *
 * Must be invoked from within a msi_lock_descs() / msi_unlock_descs()
 * pair. Use this for MSI irqdomains which implement their own vector
 * allocation.
 */
/*
 * 释放设备在 @domid 中的全部 MSI 中断。调用者必须位于
 * msi_lock_descs()/msi_unlock_descs() 临界区内；这尤其供自行实现向量分配的
 * MSI irqdomain 使用。实现把 domain 的硬件表大小换算为闭区间
 * [0, hwsize - 1]，然后复用区间释放路径。
 */
void msi_domain_free_irqs_all_locked(struct device *dev, unsigned int domid)
{
	msi_domain_free_irqs_range_locked(dev, domid, 0,
					  msi_domain_get_hwsize(dev, domid) - 1);
}

/**
 * msi_domain_free_irqs_all - Free all interrupts from a MSI interrupt domain
 *			      associated to a device
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are freed
 * @domid:	The id of the domain to operate on
 */
/*
 * 普通的“全部释放”入口：内部取得设备 MSI 描述符锁，并调用 locked 版本覆盖
 * domain 的完整索引空间。这样调用者无需自行管理锁，同时与并发描述符操作互斥。
 */
void msi_domain_free_irqs_all(struct device *dev, unsigned int domid)
{
	guard(msi_descs_lock)(dev);
	msi_domain_free_irqs_all_locked(dev, domid);
}

/**
 * msi_device_domain_free_wired - Free a wired interrupt in @domain
 * @domain:	The domain to free the interrupt on
 * @virq:	The Linux interrupt number to free
 *
 * This is the counterpart of msi_device_domain_alloc_wired() for the
 * weird wired to MSI converting domains.
 */
/*
 * 这是 msi_device_domain_alloc_wired() 的对称释放路径，用于把线中断转换成
 * MSI 的特殊 domain。先由 @virq 找到所属描述符，并检查设备、描述符及 domain
 * 类型；加锁后再次确认 @domain 仍是设备默认 MSI domain，最后只释放该描述符
 * 的单个 MSI 索引。IRQ 关联和可选描述符删除仍由通用 locked 路径统一处理。
 */
void msi_device_domain_free_wired(struct irq_domain *domain, unsigned int virq)
{
	struct msi_desc *desc = irq_get_msi_desc(virq);
	struct device *dev = domain->dev;

	if (WARN_ON_ONCE(!dev || !desc || domain->bus_token != DOMAIN_BUS_WIRED_TO_MSI))
		return;

	guard(msi_descs_lock)(dev);
	if (WARN_ON_ONCE(msi_get_device_domain(dev, MSI_DEFAULT_DOMAIN) != domain))
		return;
	msi_domain_free_irqs_range_locked(dev, MSI_DEFAULT_DOMAIN, desc->msi_index,
					  desc->msi_index);
}

/**
 * msi_get_domain_info - Get the MSI interrupt domain info for @domain
 * @domain:	The interrupt domain to retrieve data from
 *
 * Return: the pointer to the msi_domain_info stored in @domain->host_data.
 */
/*
 * 返回 @domain->host_data 中保存的 msi_domain_info 指针。该指针由 domain
 * 拥有，本函数不增加引用也不复制内容；调用者只能在 domain 生命周期内把它
 * 当作借用对象使用。
 */
struct msi_domain_info *msi_get_domain_info(struct irq_domain *domain)
{
	return (struct msi_domain_info *)domain->host_data;
}

/**
 * msi_device_has_isolated_msi - True if the device has isolated MSI
 * @dev: The device to check
 *
 * Isolated MSI means that HW modeled by an irq_domain on the path from the
 * initiating device to the CPU will validate that the MSI message specifies an
 * interrupt number that the device is authorized to trigger. This must block
 * devices from triggering interrupts they are not authorized to trigger.
 * Currently authorization means the MSI vector is one assigned to the device.
 *
 * This is interesting for securing VFIO use cases where a rouge MSI (eg created
 * by abusing a normal PCI MemWr DMA) must not allow the VFIO userspace to
 * impact outside its security domain, eg userspace triggering interrupts on
 * kernel drivers, a VM triggering interrupts on the hypervisor, or a VM
 * triggering interrupts on another VM.
 */
/*
 * “隔离 MSI”表示从发起设备到 CPU 的 irq_domain 链上，至少有一层硬件会验证
 * MSI 消息中的向量确实授权给该设备；当前授权模型就是“该向量已经分配给此
 * 设备”。这种硬件约束用于阻止设备伪造未获授权的中断。
 *
 * 这对 VFIO 的安全边界尤其重要：恶意设备可把普通 PCI Memory Write DMA
 * 伪装成 MSI，隔离检查必须阻止它触发内核驱动、宿主机或其他虚拟机的中断，
 * 避免影响自身安全域之外的执行环境。
 *
 * 检查从设备的 MSI domain 向父 domain 逐层上溯；任意一层设置
 * IRQ_DOMAIN_FLAG_ISOLATED_MSI 即可确认隔离。若 domain 链未声明该能力，则
 * 交给体系结构的 arch_is_isolated_msi() 提供平台级兜底判断。
 */
bool msi_device_has_isolated_msi(struct device *dev)
{
	struct irq_domain *domain = dev_get_msi_domain(dev);

	for (; domain; domain = domain->parent)
		if (domain->flags & IRQ_DOMAIN_FLAG_ISOLATED_MSI)
			return true;
	return arch_is_isolated_msi();
}
EXPORT_SYMBOL_GPL(msi_device_has_isolated_msi);
