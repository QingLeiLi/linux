// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017-2018 Bartosz Golaszewski <brgl@bgdev.pl>
 * Copyright (C) 2020 Bartosz Golaszewski <bgolaszewski@baylibre.com>
 */

#include <linux/cleanup.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irq_sim.h>
#include <linux/irq_work.h>
#include <linux/slab.h>

/*
 * 一个 simulator domain 共享的执行上下文。pending 以 hwirq 为索引聚合并发注入，HARD
 * irq_work 在硬中断上下文把它们映射成 virq 后调用 handle_simple_irq()；domain 指针、
 * 回调表副本和 user_data 由 create_full 发布，直到 remove_sim 才失效。
 */
struct irq_sim_work_ctx {
	struct irq_work		work;
	unsigned int		irq_count;
	unsigned long		*pending;
	struct irq_domain	*domain;
	struct irq_sim_ops	ops;
	void			*user_data;
};

/* 每个映射 IRQ 的 chip_data；enabled 由核心 mask/unmask 路径在 desc 锁域内维护。 */
struct irq_sim_irq_ctx {
	bool			enabled;
	struct irq_sim_work_ctx	*work_ctx;
};

/* 模拟 mask：只关闭后续 state 注入/查询，不主动清除已经置位的 pending bit。 */
static void irq_sim_irqmask(struct irq_data *data)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);

	irq_ctx->enabled = false;
}

/* 模拟 unmask：重新允许 PENDING state 查询和注入；不自动重放既有 pending。 */
static void irq_sim_irqunmask(struct irq_data *data)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);

	irq_ctx->enabled = true;
}

/*
 * 只接受 IRQ_TYPE_NONE 或 rising/falling/both edge 位组合；任何非 EDGE_BOTH 位返回
 * -EINVAL。成功把类型写入 irq_data 并返回 0。模拟器不实现 level 触发语义。
 */
static int irq_sim_set_type(struct irq_data *data, unsigned int type)
{
	/* We only support rising and falling edge trigger types. */
	/* 模拟器只支持上升沿和下降沿触发类型。 */
	if (type & ~IRQ_TYPE_EDGE_BOTH)
		return -EINVAL;

	irqd_set_trigger_type(data, type);

	return 0;
}

/*
 * 查询模拟 irqchip 状态。目前只识别 PENDING，其他类型返回 -EINVAL。IRQ enabled 时
 * 把对应 hwirq pending bit 写入 *@state；masked 时返回 0 但不写输出，这是当前回调的
 * 精确语义，调用者不能把成功等同于输出一定初始化。位图用原子 bitop 与 irq_work 并发。
 */
static int irq_sim_get_irqchip_state(struct irq_data *data,
				     enum irqchip_irq_state which, bool *state)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		if (irq_ctx->enabled)
			*state = test_bit(hwirq, irq_ctx->work_ctx->pending);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * 设置 PENDING 状态。只支持该 state，否则 -EINVAL；enabled 时用 assign_bit() 原子
 * 置/清对应 hwirq，置 true 还排队 HARD irq_work，清 false 不排队。masked 时请求被
 * 静默忽略但仍返回 0。核心状态 API 在 desc/bus 锁域调用，pending 位还与 work 并发。
 */
static int irq_sim_set_irqchip_state(struct irq_data *data,
				     enum irqchip_irq_state which, bool state)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	switch (which) {
	case IRQCHIP_STATE_PENDING:
		if (irq_ctx->enabled) {
			assign_bit(hwirq, irq_ctx->work_ctx->pending, state);
			if (state)
				irq_work_queue(&irq_ctx->work_ctx->work);
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * 首个 action 请求 simulator IRQ 时，把 domain/hwirq/user_data 交给可选 provider
 * callback。回调可拒绝请求并返回负 errno（例如 GPIO 模拟器无法把该 pin 锁为 IRQ）；
 * 无回调返回 0。work_ctx 与 user_data 均为借用对象，provider 必须覆盖 domain 生命周期。
 */
static int irq_sim_request_resources(struct irq_data *data)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);
	struct irq_sim_work_ctx *work_ctx = irq_ctx->work_ctx;
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	if (work_ctx->ops.irq_sim_irq_requested)
		return work_ctx->ops.irq_sim_irq_requested(work_ctx->domain,
							   hwirq,
							   work_ctx->user_data);

	return 0;
}

/*
 * 最后一个 action 释放时调用可选 provider 回调，归还 request_resources 取得的外围
 * 状态。返回 void；domain/hwirq/user_data 与请求阶段完全配对，回调不得保留即将销毁的
 * simulator 私有指针。
 */
static void irq_sim_release_resources(struct irq_data *data)
{
	struct irq_sim_irq_ctx *irq_ctx = irq_data_get_irq_chip_data(data);
	struct irq_sim_work_ctx *work_ctx = irq_ctx->work_ctx;
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	if (work_ctx->ops.irq_sim_irq_released)
		work_ctx->ops.irq_sim_irq_released(work_ctx->domain, hwirq,
						   work_ctx->user_data);
}

/* simulator 的共享 irq_chip；每个映射通过 chip_data 找到独立 enabled 与共享 work。 */
static struct irq_chip irq_sim_irqchip = {
	.name			= "irq_sim",
	.irq_mask		= irq_sim_irqmask,
	.irq_unmask		= irq_sim_irqunmask,
	.irq_set_type		= irq_sim_set_type,
	.irq_get_irqchip_state	= irq_sim_get_irqchip_state,
	.irq_set_irqchip_state	= irq_sim_set_irqchip_state,
	.irq_request_resources	= irq_sim_request_resources,
	.irq_release_resources	= irq_sim_release_resources,
};

/*
 * HARD irq_work 消费共享 pending bitmap。按 hwirq 从低到高查位，先原子清 pending，
 * 再经 domain 反向映射取得 virq 并调用 handle_simple_irq()；只有已映射、enabled IRQ
 * 能通过本 chip 置位，因此正常路径保证 mapping/desc 存在。remove_sim() 用
 * irq_work_sync() 阻止上下文在 bitmap/work_ctx 释放后继续运行。
 *
 * offset 在一次 callback 中单调不回绕：同位或更高位并发置入可被本轮继续看见，较低
 * offset 依赖 irq_work_queue() 在 callback 运行期间重新排队后由下一轮从 0 扫描。当前
 * while 条件观察整张位图而 find_next_bit() 无“到末尾后回绕”防线，这是并发低位注入
 * 必须注意的现有实现边界。
 */
static void irq_sim_handle_irq(struct irq_work *work)
{
	struct irq_sim_work_ctx *work_ctx;
	unsigned int offset = 0;
	int irqnum;

	work_ctx = container_of(work, struct irq_sim_work_ctx, work);

	while (!bitmap_empty(work_ctx->pending, work_ctx->irq_count)) {
		offset = find_next_bit(work_ctx->pending,
				       work_ctx->irq_count, offset);
		clear_bit(offset, work_ctx->pending);
		irqnum = irq_find_mapping(work_ctx->domain, offset);
		handle_simple_irq(irq_to_desc(irqnum));
	}
}

/*
 * 为 domain 的一个 hwirq->virq 映射分配 irq_sim_irq_ctx，并配置共享 chip、私有
 * chip_data 与 handle_simple_irq。清除 NOREQUEST/NOAUTOEN 使模拟 IRQ 可像普通 IRQ
 * 被申请并自动启动，同时设置 NOPROBE 禁止旧式自动探测扰动。成功发布 work_ctx 借用
 * 并返回 0；分配失败 -ENOMEM，irqdomain 外层负责回滚尚未发布的映射。
 */
static int irq_sim_domain_map(struct irq_domain *domain,
			      unsigned int virq, irq_hw_number_t hw)
{
	struct irq_sim_work_ctx *work_ctx = domain->host_data;
	struct irq_sim_irq_ctx *irq_ctx;

	irq_ctx = kzalloc_obj(*irq_ctx);
	if (!irq_ctx)
		return -ENOMEM;

	irq_set_chip(virq, &irq_sim_irqchip);
	irq_set_chip_data(virq, irq_ctx);
	irq_set_handler(virq, handle_simple_irq);
	irq_modify_status(virq, IRQ_NOREQUEST | IRQ_NOAUTOEN, IRQ_NOPROBE);
	irq_ctx->work_ctx = work_ctx;

	return 0;
}

/*
 * 撤销一个 simulator 映射。调用者已确保 action/resource 不再使用该 IRQ；先恢复坏
 * handler，再由 irq_domain_reset_irq_data() 清 chip/domain 映射状态，最后释放该 virq
 * 独占的 irq_ctx。共享 work_ctx 由整个 domain 的 remove_sim() 另行释放。
 */
static void irq_sim_domain_unmap(struct irq_domain *domain, unsigned int virq)
{
	struct irq_sim_irq_ctx *irq_ctx;
	struct irq_data *irqd;

	irqd = irq_domain_get_irq_data(domain, virq);
	irq_ctx = irq_data_get_irq_chip_data(irqd);

	irq_set_handler(virq, NULL);
	irq_domain_reset_irq_data(irqd);
	kfree(irq_ctx);
}

/* simulator 线性 domain 的 map/unmap 生命周期表；host_data 指向共享 work_ctx。 */
static const struct irq_domain_ops irq_sim_domain_ops = {
	.map		= irq_sim_domain_map,
	.unmap		= irq_sim_domain_unmap,
};

/**
 * irq_domain_create_sim - Create a new interrupt simulator irq_domain and
 *                         allocate a range of dummy interrupts.
 *
 * @fwnode:     struct fwnode_handle to be associated with this domain.
 * @num_irqs:   Number of interrupts to allocate.
 *
 * On success: return a new irq_domain object.
 * On failure: a negative errno wrapped with ERR_PTR().
 */
/*
 * 原文契约：为 @fwnode 创建容量为 @num_irqs 的 simulator domain，成功返回新 domain，
 * 失败返回 ERR_PTR。该简化入口不给 provider callbacks/user_data，直接转 full 版本。
 * 原文“allocate a range of dummy interrupts”按当前线性 domain 实现应理解为建立 hwirq
 * 范围；virq/irq_desc 在调用者创建 mapping 时才取得，并非本函数一次性预分配。
 */
struct irq_domain *irq_domain_create_sim(struct fwnode_handle *fwnode,
					 unsigned int num_irqs)
{
	return irq_domain_create_sim_full(fwnode, num_irqs, NULL, NULL);
}
EXPORT_SYMBOL_GPL(irq_domain_create_sim);

/*
 * simulator 的完整创建事务。先分配零初始化 work_ctx 和 num_irqs 位 pending bitmap，
 * 再创建 host_data=work_ctx 的线性 domain；任一步失败由 __free(kfree/bitmap) 自动逆序
 * 清理并返回 ERR_PTR(-ENOMEM)。domain 成功后填写数量、HARD irq_work、bitmap、借用
 * user_data，并按值复制可选 ops，最后 no_free_ptr() 把两份内存所有权转交给返回对象。
 *
 * 成功 domain 必须由 irq_domain_remove_sim() 配对；ops 中函数指针及 user_data 指向的
 * 外部对象不会被深拷贝/持引用，调用者须保证它们覆盖所有 mapping、请求和最终拆卸。
 */
struct irq_domain *irq_domain_create_sim_full(struct fwnode_handle *fwnode,
					      unsigned int num_irqs,
					      const struct irq_sim_ops *ops,
					      void *data)
{
	struct irq_sim_work_ctx *work_ctx __free(kfree) =
				kzalloc_obj(*work_ctx);

	if (!work_ctx)
		return ERR_PTR(-ENOMEM);

	unsigned long *pending __free(bitmap) = bitmap_zalloc(num_irqs, GFP_KERNEL);
	if (!pending)
		return ERR_PTR(-ENOMEM);

	work_ctx->domain = irq_domain_create_linear(fwnode, num_irqs,
						    &irq_sim_domain_ops,
						    work_ctx);
	if (!work_ctx->domain)
		return ERR_PTR(-ENOMEM);

	work_ctx->irq_count = num_irqs;
	work_ctx->work = IRQ_WORK_INIT_HARD(irq_sim_handle_irq);
	work_ctx->pending = no_free_ptr(pending);
	work_ctx->user_data = data;

	if (ops)
		memcpy(&work_ctx->ops, ops, sizeof(*ops));

	return no_free_ptr(work_ctx)->domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_sim_full);

/**
 * irq_domain_remove_sim - Deinitialize the interrupt simulator domain: free
 *                         the interrupt descriptors and allocated memory.
 *
 * @domain:     The interrupt simulator domain to tear down.
 */
/*
 * 原文契约：拆除 simulator domain 并释放描述符/私有内存。结合 irq_domain_remove() 的
 * 当前契约，调用者必须先 dispose 全部 mappings/actions；本函数不会替仍活跃映射安全
 * 调用 unmap。先 irq_work_sync() 排空硬中断 callback，再释放 pending/work_ctx，最后
 * 从全局 domain 索引移除并释放 domain。
 *
 * 该顺序依赖“映射已空”：否则 irq_ctx->work_ctx/domain->host_data 会在 domain 仍可见时
 * 成为悬空指针。成功无返回值，调用后 @domain 及 ops/user_data 借用关系全部终止。
 */
void irq_domain_remove_sim(struct irq_domain *domain)
{
	struct irq_sim_work_ctx *work_ctx = domain->host_data;

	irq_work_sync(&work_ctx->work);
	bitmap_free(work_ctx->pending);
	kfree(work_ctx);

	irq_domain_remove(domain);
}
EXPORT_SYMBOL_GPL(irq_domain_remove_sim);

/* 设备 devres action 的适配器，把保存的 domain 交给完整 remove_sim 生命周期。 */
static void devm_irq_domain_remove_sim(void *data)
{
	struct irq_domain *domain = data;

	irq_domain_remove_sim(domain);
}

/**
 * devm_irq_domain_create_sim - Create a new interrupt simulator for
 *                              a managed device.
 *
 * @dev:        Device to initialize the simulator object for.
 * @fwnode:     struct fwnode_handle to be associated with this domain.
 * @num_irqs:   Number of interrupts to allocate
 *
 * On success: return a new irq_domain object.
 * On failure: a negative errno wrapped with ERR_PTR().
 */
/*
 * 原文契约：为 managed @dev 创建无扩展 callbacks 的 simulator domain；成功返回 domain，
 * detach 自动拆除，失败返回 ERR_PTR。简化入口把其余参数置 NULL 后转 full managed 版本。
 */
struct irq_domain *devm_irq_domain_create_sim(struct device *dev,
					      struct fwnode_handle *fwnode,
					      unsigned int num_irqs)
{
	return devm_irq_domain_create_sim_full(dev, fwnode, num_irqs,
					       NULL, NULL);
}
EXPORT_SYMBOL_GPL(devm_irq_domain_create_sim);

/*
 * managed 完整创建路径。先用非 managed full 版本取得 domain；失败原样返回。随后用
 * devm_add_action_or_reset() 登记 remove_sim：登记成功后设备拥有拆卸责任，登记失败时
 * helper 已立即执行 remove_sim 回滚，函数只返回 ERR_PTR(ret)，不能再次删除 domain。
 *
 * 调用者仍须让 mappings/actions 的 devres 清理记录晚于本 action（从而 LIFO 先释放），
 * 或在设备解绑前自行 dispose；ops/user_data 的借用生命周期同样必须覆盖 domain。
 */
struct irq_domain *
devm_irq_domain_create_sim_full(struct device *dev,
				struct fwnode_handle *fwnode,
				unsigned int num_irqs,
				const struct irq_sim_ops *ops,
				void *data)
{
	struct irq_domain *domain;
	int ret;

	domain = irq_domain_create_sim_full(fwnode, num_irqs, ops, data);
	if (IS_ERR(domain))
		return domain;

	ret = devm_add_action_or_reset(dev, devm_irq_domain_remove_sim, domain);
	if (ret)
		return ERR_PTR(ret);

	return domain;
}
EXPORT_SYMBOL_GPL(devm_irq_domain_create_sim_full);
