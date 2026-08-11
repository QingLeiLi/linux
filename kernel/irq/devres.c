// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/irq.h>

#include "internals.h"

/*
 * Device resource management aware IRQ request/free implementation.
 */
/*
 * 本文件为 IRQ 申请/释放提供 device resource management 感知封装。
 * 成功取得 IRQ、描述符范围、generic chip 设置或 irq_domain 后，把精确逆操作及其
 * 参数登记到设备 devres 栈；驱动解绑时按 LIFO 自动释放，显式提前释放必须走对应
 * devm 接口以同时摘除记录，不能直接调用底层 free/remove。
 */
/* 已成功申请 IRQ 的最小释放凭据；devres 核心拥有外壳，字段均为借用/数值副本。 */
struct irq_devres {
	unsigned int irq;
	void *dev_id;
};

/* devres 回调：消费记录中的 IRQ/dev_id 配对，free_irq() 同步并释放对应 action。 */
static void devm_irq_release(struct device *dev, void *res)
{
	struct irq_devres *this = res;

	free_irq(this->irq, this->dev_id);
}

/* 显式 devm_free_irq() 的匹配器；共享 IRQ 必须同时匹配逻辑号和身份 cookie。 */
static int devm_irq_match(struct device *dev, void *res, void *data)
{
	struct irq_devres *this = res, *match = data;

	return this->irq == match->irq && this->dev_id == match->dev_id;
}

/*
 * 统一 managed request 的错误报告。非负成功类别原样返回；负 errno 交给
 * dev_err_probe() 附带设备上下文、IRQ、两个 handler 和可选名字输出，并保留其返回
 * 语义（包括 deferred probe）。函数不改变已由内部申请路径决定的资源 ownership。
 */
static int devm_request_result(struct device *dev, int rc, unsigned int irq,
			       irq_handler_t handler, irq_handler_t thread_fn,
			       const char *devname)
{
	if (rc >= 0)
		return rc;

	return dev_err_probe(dev, rc, "request_irq(%u) %ps %ps %s\n",
			     irq, handler, thread_fn, devname ? : "");
}

/*
 * managed threaded IRQ 的共同实现。先分配尚未入设备栈的释放记录，再以调用者名字或
 * dev_name(dev) 申请 IRQ；申请失败只释放孤立记录，成功则填入 irq/dev_id 并通过
 * devres_add() 发布。发布后设备解绑会调用 devm_irq_release()，返回 0；分配/申请
 * 失败返回负 errno，且不会留下 devres 或已申请 IRQ。
 */
static int __devm_request_threaded_irq(struct device *dev, unsigned int irq,
				       irq_handler_t handler,
				       irq_handler_t thread_fn,
				       unsigned long irqflags,
				       const char *devname, void *dev_id)
{
	struct irq_devres *dr;
	int rc;

	dr = devres_alloc(devm_irq_release, sizeof(struct irq_devres),
			  GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	if (!devname)
		devname = dev_name(dev);

	rc = request_threaded_irq(irq, handler, thread_fn, irqflags, devname,
				  dev_id);
	if (rc) {
		devres_free(dr);
		return rc;
	}

	dr->irq = irq;
	dr->dev_id = dev_id;
	devres_add(dev, dr);

	return 0;
}

/**
 * devm_request_threaded_irq - allocate an interrupt line for a managed device with error logging
 * @dev:	Device to request interrupt for
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the interrupt occurs
 * @thread_fn:	Function to be called in a threaded interrupt context. NULL
 *		for devices which handle everything in @handler
 * @irqflags:	Interrupt type flags
 * @devname:	An ascii name for the claiming device, dev_name(dev) if NULL
 * @dev_id:	A cookie passed back to the handler function
 *
 * Except for the extra @dev argument, this function takes the same
 * arguments and performs the same function as request_threaded_irq().
 * Interrupts requested with this function will be automatically freed on
 * driver detach.
 *
 * If an interrupt allocated with this function needs to be freed
 * separately, devm_free_irq() must be used.
 *
 * When the request fails, an error message is printed with contextual
 * information (device name, interrupt number, handler functions and
 * error code). Don't add extra error messages at the call sites.
 *
 * Return: 0 on success or a negative error number.
 */
/*
 * 原文契约：除额外 @dev 外与 request_threaded_irq() 参数和行为相同，成功资源在驱动
 * detach 时自动释放；若需提前释放必须调用 devm_free_irq()。失败会输出含设备、IRQ、
 * handler 与错误码的上下文日志，调用点不应重复报错。成功返回 0，失败返回负 errno。
 */
int devm_request_threaded_irq(struct device *dev, unsigned int irq,
			      irq_handler_t handler, irq_handler_t thread_fn,
			      unsigned long irqflags, const char *devname,
			      void *dev_id)
{
	int rc = __devm_request_threaded_irq(dev, irq, handler, thread_fn,
					     irqflags, devname, dev_id);

	return devm_request_result(dev, rc, irq, handler, thread_fn, devname);
}
EXPORT_SYMBOL(devm_request_threaded_irq);

/*
 * managed any-context IRQ 的共同实现。资源记录的取得、失败回滚和成功发布与 threaded
 * 版本相同；底层成功值不是固定 0，而是 IRQC_IS_HARDIRQ/IRQC_IS_NESTED，必须原样
 * 返回给调用者并仍登记同一 free_irq() 逆操作。
 */
static int __devm_request_any_context_irq(struct device *dev, unsigned int irq,
					  irq_handler_t handler,
					  unsigned long irqflags,
					  const char *devname, void *dev_id)
{
	struct irq_devres *dr;
	int rc;

	dr = devres_alloc(devm_irq_release, sizeof(struct irq_devres),
			  GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	if (!devname)
		devname = dev_name(dev);

	rc = request_any_context_irq(irq, handler, irqflags, devname, dev_id);
	if (rc < 0) {
		devres_free(dr);
		return rc;
	}

	dr->irq = irq;
	dr->dev_id = dev_id;
	devres_add(dev, dr);

	return rc;
}

/**
 * devm_request_any_context_irq - allocate an interrupt line for a managed device with error logging
 * @dev:	Device to request interrupt for
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the interrupt occurs
 * @irqflags:	Interrupt type flags
 * @devname:	An ascii name for the claiming device, dev_name(dev) if NULL
 * @dev_id:	A cookie passed back to the handler function
 *
 * Except for the extra @dev argument, this function takes the same
 * arguments and performs the same function as request_any_context_irq().
 * Interrupts requested with this function will be automatically freed on
 * driver detach.
 *
 * If an interrupt allocated with this function needs to be freed
 * separately, devm_free_irq() must be used.
 *
 * When the request fails, an error message is printed with contextual
 * information (device name, interrupt number, handler functions and
 * error code). Don't add extra error messages at the call sites.
 *
 * Return: IRQC_IS_HARDIRQ or IRQC_IS_NESTED on success, or a negative error
 * number.
 */
/*
 * 原文契约：与 request_any_context_irq() 相同但绑定 @dev 生命周期，detach 自动释放，
 * 提前释放必须使用 devm_free_irq()；失败由本层统一记录上下文，调用点不再重复日志。
 * 成功返回 IRQC_IS_HARDIRQ 或 IRQC_IS_NESTED，失败返回负 errno。
 */
int devm_request_any_context_irq(struct device *dev, unsigned int irq,
				 irq_handler_t handler, unsigned long irqflags,
				 const char *devname, void *dev_id)
{
	int rc = __devm_request_any_context_irq(dev, irq, handler, irqflags,
						devname, dev_id);

	return devm_request_result(dev, rc, irq, handler, NULL, devname);
}
EXPORT_SYMBOL(devm_request_any_context_irq);

/**
 *	devm_free_irq - free an interrupt
 *	@dev: device to free interrupt for
 *	@irq: Interrupt line to free
 *	@dev_id: Device identity to free
 *
 *	Except for the extra @dev argument, this function takes the
 *	same arguments and performs the same function as free_irq().
 *	This function instead of free_irq() should be used to manually
 *	free IRQs allocated with devm_request_irq().
 */
/*
 * 这是 managed IRQ 的显式提前释放入口，参数语义与 free_irq() 相同。函数以
 * irq+dev_id 从设备 devres 栈逆向查找最新匹配项，原子摘除后调用 release/free_irq
 * 并释放记录；找不到表示底层资源与 devres 账目不一致，以 WARN_ON 暴露，返回 void。
 */
void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
	struct irq_devres match_data = { irq, dev_id };

	WARN_ON(devres_release(dev, devm_irq_release, devm_irq_match,
			       &match_data));
}
EXPORT_SYMBOL(devm_free_irq);

/* 连续 irq_desc 区间的释放凭据；记录实际返回基址而非调用者的搜索起点。 */
struct irq_desc_devres {
	unsigned int from;
	unsigned int cnt;
};

/* devres 回调：把已登记的 [from, from+cnt) 描述符区间整体归还 IRQ 核心。 */
static void devm_irq_desc_release(struct device *dev, void *res)
{
	struct irq_desc_devres *this = res;

	irq_free_descs(this->from, this->cnt);
}

/**
 * __devm_irq_alloc_descs - Allocate and initialize a range of irq descriptors
 *			    for a managed device
 * @dev:	Device to allocate the descriptors for
 * @irq:	Allocate for specific irq number if irq >= 0
 * @from:	Start the search from this irq number
 * @cnt:	Number of consecutive irqs to allocate
 * @node:	Preferred node on which the irq descriptor should be allocated
 * @owner:	Owning module (can be NULL)
 * @affinity:	Optional pointer to an irq_affinity_desc array of size @cnt
 *		which hints where the irq descriptors should be allocated
 *		and which default affinities to use
 *
 * Returns the first irq number or error code.
 *
 * Note: Use the provided wrappers (devm_irq_alloc_desc*) for simplicity.
 */
/*
 * 原文契约：为 managed 设备分配并初始化 @cnt 个连续 irq_desc；@irq>=0 请求固定
 * 起点，否则从 @from 搜索，@node/@owner/@affinity 控制 NUMA、模块和逐项初始亲和性。
 * 通常应使用简化 wrapper。成功返回实际首 IRQ，并登记该基址与数量供 detach 自动
 * irq_free_descs()；失败返回负 errno，不留下记录或部分区间。
 *
 * devres 按逆序释放，调用者应在此记录之后登记依赖这些 desc 的 mappings/actions，
 * 使解绑时先拆上层用户再释放描述符；本函数本身只管理 desc 区间，不替代其依赖清理。
 */
int __devm_irq_alloc_descs(struct device *dev, int irq, unsigned int from,
			   unsigned int cnt, int node, struct module *owner,
			   const struct irq_affinity_desc *affinity)
{
	struct irq_desc_devres *dr;
	int base;

	dr = devres_alloc(devm_irq_desc_release, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	base = __irq_alloc_descs(irq, from, cnt, node, owner, affinity);
	if (base < 0) {
		devres_free(dr);
		return base;
	}

	dr->from = base;
	dr->cnt = cnt;
	devres_add(dev, dr);

	return base;
}
EXPORT_SYMBOL_GPL(__devm_irq_alloc_descs);

#ifdef CONFIG_GENERIC_IRQ_CHIP
/**
 * devm_irq_alloc_generic_chip - Allocate and initialize a generic chip
 *                               for a managed device
 * @dev:	Device to allocate the generic chip for
 * @name:	Name of the irq chip
 * @num_ct:	Number of irq_chip_type instances associated with this
 * @irq_base:	Interrupt base nr for this chip
 * @reg_base:	Register base address (virtual)
 * @handler:	Default flow handler associated with this chip
 *
 * Returns an initialized irq_chip_generic structure. The chip defaults
 * to the primary (index 0) irq_chip_type and @handler
 */
/*
 * 原文契约：为 managed 设备分配带 @num_ct 个 irq_chip_type 尾数组的零初始化 generic
 * chip，并用名字、IRQ 基址、寄存器基址和默认 flow handler 初始化；初始选用索引 0
 * 的 chip type。内存由 devm_kzalloc() 登记，detach 自动释放；成功返回 @gc，分配失败
 * 返回 NULL，未发布半初始化对象。
 */
struct irq_chip_generic *
devm_irq_alloc_generic_chip(struct device *dev, const char *name, int num_ct,
			    unsigned int irq_base, void __iomem *reg_base,
			    irq_flow_handler_t handler)
{
	struct irq_chip_generic *gc;

	gc = devm_kzalloc(dev, struct_size(gc, chip_types, num_ct), GFP_KERNEL);
	if (gc)
		irq_init_generic_chip(gc, name, num_ct,
				      irq_base, reg_base, handler);

	return gc;
}
EXPORT_SYMBOL_GPL(devm_irq_alloc_generic_chip);

/* generic-chip setup 的精确逆操作参数；flags 只影响建立阶段，remove 不需要保存。 */
struct irq_generic_chip_devres {
	struct irq_chip_generic *gc;
	u32 msk;
	unsigned int clr;
	unsigned int set;
};

/* devres 回调：按记录的 mask 与 settings 逆向调用 irq_remove_generic_chip()。 */
static void devm_irq_remove_generic_chip(struct device *dev, void *res)
{
	struct irq_generic_chip_devres *this = res;

	irq_remove_generic_chip(this->gc, this->msk, this->clr, this->set);
}

/**
 * devm_irq_setup_generic_chip - Setup a range of interrupts with a generic
 *                               chip for a managed device
 *
 * @dev:	Device to setup the generic chip for
 * @gc:		Generic irq chip holding all data
 * @msk:	Bitmask holding the irqs to initialize relative to gc->irq_base
 * @flags:	Flags for initialization
 * @clr:	IRQ_* bits to clear
 * @set:	IRQ_* bits to set
 *
 * Set up max. 32 interrupts starting from gc->irq_base. Note, this
 * initializes all interrupts to the primary irq_chip_type and its
 * associated handler.
 */
/*
 * 原文契约：对 @gc->irq_base 起最多 32 个、由 @msk 选择的 IRQ 安装 generic chip，
 * 全部先采用 primary irq_chip_type 及其 handler；@flags 控制初始化，@clr/@set 调整
 * desc 设置位。成功后登记 gc/msk/clr/set，detach 自动执行 irq_remove_generic_chip()。
 *
 * 先分配未发布的 devres，失败返回 -ENOMEM 且尚未 setup；底层 setup 为 void，完成后
 * 记录即可原子加入设备资源栈并返回 0。若 gc 本身也由上一接口 managed 分配，后登记
 * 的 setup 记录会先撤销 IRQ 绑定，再由较早的 devm_kzalloc 记录释放 gc 内存。
 */
int devm_irq_setup_generic_chip(struct device *dev, struct irq_chip_generic *gc,
				u32 msk, enum irq_gc_flags flags,
				unsigned int clr, unsigned int set)
{
	struct irq_generic_chip_devres *dr;

	dr = devres_alloc(devm_irq_remove_generic_chip,
			  sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	irq_setup_generic_chip(gc, msk, flags, clr, set);

	dr->gc = gc;
	dr->msk = msk;
	dr->clr = clr;
	dr->set = set;
	devres_add(dev, dr);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_irq_setup_generic_chip);
#endif /* CONFIG_GENERIC_IRQ_CHIP */

#ifdef CONFIG_IRQ_DOMAIN
/* devres 回调：记录槽拥有一个已实例化 domain，解绑时调用 irq_domain_remove()。 */
static void devm_irq_domain_remove(struct device *dev, void *res)
{
	struct irq_domain **domain = res;

	irq_domain_remove(*domain);
}

/**
 * devm_irq_domain_instantiate() - Instantiate a new irq domain data for a
 *                                 managed device.
 * @dev:	Device to instantiate the domain for
 * @info:	Domain information pointer pointing to the information for this
 *		domain
 *
 * Return: A pointer to the instantiated irq domain or an ERR_PTR value.
 */
/*
 * 原文契约：按 @info 为 managed 设备实例化 irq_domain，成功返回 domain 指针，失败
 * 返回 ERR_PTR。函数先分配一个保存 domain 指针的未发布 devres；实例化成功才写槽并
 * devres_add()，从而把 irq_domain_remove() 绑定到 detach，失败则仅释放空记录并原样
 * 返回错误指针。
 *
 * 返回指针由设备 devres 生命周期拥有，调用者借用它建立映射；依赖映射/handler 必须
 * 更晚登记或在解绑前撤销，使 LIFO 释放时先清空 domain 用户，再执行最终 remove。
 */
struct irq_domain *devm_irq_domain_instantiate(struct device *dev,
					       const struct irq_domain_info *info)
{
	struct irq_domain *domain;
	struct irq_domain **dr;

	dr = devres_alloc(devm_irq_domain_remove, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return ERR_PTR(-ENOMEM);

	domain = irq_domain_instantiate(info);
	if (!IS_ERR(domain)) {
		*dr = domain;
		devres_add(dev, dr);
	} else {
		devres_free(dr);
	}

	return domain;
}
EXPORT_SYMBOL_GPL(devm_irq_domain_instantiate);
#endif /* CONFIG_IRQ_DOMAIN */
