// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt)  "irq: " fmt

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/topology.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/fs.h>

#include "proc.h"

/*
 * IRQ domain 把控制器看到的硬件中断号（hwirq）转换为 Linux 全局 IRQ 号
 * （virq），并把每一级控制器的 irq_chip、私有数据和层级关系保存在
 * irq_data 链中。本文件同时负责 domain 的创建/发布/销毁、正反向映射，
 * 以及层级 domain 的分配、激活和回收。
 *
 * 阅读时应区分三种同步范围：
 * 1. irq_domain_mutex 保护全局 domain 链表及其查找；
 * 2. domain->root->mutex 串行化同一层级树中的映射和 irq_data 链变更；
 * 3. 已发布的反向映射允许中断路径在 RCU 读侧无睡眠地查询。
 * 因而“内存已经分配”不等于“中断路径已经可见”，发布映射通常是一次
 * 单独且有顺序要求的提交动作。
 */

/* 所有已发布 domain 的全局注册表；链表只借用对象，移除后才允许释放 domain。 */
static LIST_HEAD(irq_domain_list);
/* 串行化全局 domain 链表、名称/debugfs 更新及其匹配遍历的可睡眠互斥锁。 */
static DEFINE_MUTEX(irq_domain_mutex);

/* 未显式指定 domain 时的兼容性回退入口；其生命周期由平台初始化代码约束。 */
static struct irq_domain *irq_default_domain;

/* 层级分配事务的锁内实现；条件编译分支在文件后部提供真实版本或拒绝桩。 */
static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity);
/* 根据构建配置和操作表设置 domain 的层级属性。 */
static void irq_domain_check_hierarchy(struct irq_domain *domain);
/* 按 domain 类型选择单个映射的通用或 MSI 专用释放协议。 */
static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq);

/*
 * 为没有 OF/ACPI/软件节点的中断控制器构造的轻量固件节点。
 * fwnode 嵌入对象以接入统一固件节点接口；name 由本对象拥有，parent 和 pa
 * 只借用调用者提供的指针，释放本对象时不会释放后二者。
 */
struct irqchip_fwid {
	struct fwnode_handle	fwnode;
	struct fwnode_handle	*parent;
	unsigned int		type;
	char			*name;
	phys_addr_t		*pa;
};

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
/* 已发布 domain 时创建其 debugfs 状态文件。 */
static void debugfs_add_domain_dir(struct irq_domain *d);
/* 移除 domain 时先删除持有其私有指针的 debugfs 文件。 */
static void debugfs_remove_domain_dir(struct irq_domain *d);
#else
/* debugfs 支持关闭时，domain 发布路径保留为零成本空操作。 */
static inline void debugfs_add_domain_dir(struct irq_domain *d) { }
/* debugfs 支持关闭时，domain 移除路径保留为零成本空操作。 */
static inline void debugfs_remove_domain_dir(struct irq_domain *d) { }
#endif

/*
 * 从嵌入的 fwnode 还原 irqchip_fwid 并返回其名称。
 * 返回值仍归 irqchip_fwid 所有，调用者只能借用，不能释放或长期越过节点寿命。
 */
static const char *irqchip_fwnode_get_name(const struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid = container_of(fwnode, struct irqchip_fwid, fwnode);

	return fwid->name;
}

/*
 * 返回创建节点时记录的父固件节点。这里不增引用；父子节点的存活顺序由
 * 创建 irq domain 的控制器驱动负责保证。
 */
static struct fwnode_handle *irqchip_fwnode_get_parent(const struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid = container_of(fwnode, struct irqchip_fwid, fwnode);

	return fwid->parent;
}

/* irqchip 专用 fwnode 只暴露稳定名称和父节点两项身份操作。 */
const struct fwnode_operations irqchip_fwnode_ops = {
	.get_name = irqchip_fwnode_get_name,
	.get_parent = irqchip_fwnode_get_parent,
};
EXPORT_SYMBOL_GPL(irqchip_fwnode_ops);

/**
 * __irq_domain_alloc_fwnode - Allocate a fwnode_handle suitable for
 *                           identifying an irq domain
 * @type:	Type of irqchip_fwnode. See linux/irqdomain.h
 * @id:		Optional user provided id if name != NULL
 * @name:	Optional user provided domain name
 * @pa:		Optional user-provided physical address
 * @parent:	Optional parent fwnode_handle
 *
 * Allocate a struct irqchip_fwid, and return a pointer to the embedded
 * fwnode_handle (or NULL on failure).
 *
 * Note: The types IRQCHIP_FWNODE_NAMED and IRQCHIP_FWNODE_NAMED_ID are
 * solely to transport name information to irqdomain creation code. The
 * node is not stored. For other types the pointer is kept in the irq
 * domain struct.
 */
/*
 * 分配一个可标识 IRQ domain 的 fwnode_handle。
 * @type：irqchip_fwnode 类型，定义见 linux/irqdomain.h。
 * @id：当名称非空时可由调用者提供的可选编号。
 * @name：可选的 domain 名称。
 * @pa：可选的控制器物理地址；这里只保存借用指针。
 * @parent：可选的父固件节点；这里只保存借用指针。
 *
 * 成功时返回新分配 irqchip_fwid 中嵌入的 fwnode，失败返回 NULL。命名类型
 * 只借此节点把名称传给 domain 创建代码，并不把节点长期存入 domain；其他
 * 类型则可由 domain 持有这个节点。调用者最终必须用 irq_domain_free_fwnode()
 * 对称释放。
 *
 * 执行阶段：先分别分配对象和名称；任一失败便同时回收，避免半初始化对象
 * 泄漏；随后写入只读身份信息，最后用 fwnode_init() 发布节点操作表。该函数
 * 使用 GFP_KERNEL，必须处于可睡眠上下文。
 */
struct fwnode_handle *__irq_domain_alloc_fwnode(unsigned int type, int id,
						const char *name,
						phys_addr_t *pa,
						struct fwnode_handle *parent)
{
	struct irqchip_fwid *fwid;
	char *n;

	fwid = kzalloc_obj(*fwid);

	switch (type) {
	case IRQCHIP_FWNODE_NAMED:
		n = kasprintf(GFP_KERNEL, "%s", name);
		break;
	case IRQCHIP_FWNODE_NAMED_ID:
		n = kasprintf(GFP_KERNEL, "%s-%d", name, id);
		break;
	default:
		n = kasprintf(GFP_KERNEL, "irqchip@%pa", pa);
		break;
	}

	if (!fwid || !n) {
		kfree(fwid);
		kfree(n);
		return NULL;
	}

	fwid->type = type;
	fwid->name = n;
	fwid->pa = pa;
	fwid->parent = parent;
	fwnode_init(&fwid->fwnode, &irqchip_fwnode_ops);
	return &fwid->fwnode;
}
EXPORT_SYMBOL_GPL(__irq_domain_alloc_fwnode);

/**
 * irq_domain_free_fwnode - Free a non-OF-backed fwnode_handle
 * @fwnode: fwnode_handle to free
 *
 * Free a fwnode_handle allocated with irq_domain_alloc_fwnode.
 */
/*
 * 释放由 irq_domain_alloc_fwnode() 家族创建的非 OF 固件节点。
 * @fwnode：待释放节点；NULL 或非 irqchip 节点只告警/返回。
 *
 * 本函数先验证节点来源，再由嵌入成员恢复外层对象，按“名称在前、容器在后”
 * 的顺序释放。它不释放 parent 和 pa，因为这两个字段只是借用关系。调用前
 * 必须确保所有 domain 及并发读者都已停止引用该节点。
 */
void irq_domain_free_fwnode(struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid;

	if (!fwnode || WARN_ON(!is_fwnode_irqchip(fwnode)))
		return;

	fwid = container_of(fwnode, struct irqchip_fwid, fwnode);
	kfree(fwid->name);
	kfree(fwid);
}
EXPORT_SYMBOL_GPL(irq_domain_free_fwnode);

/*
 * 从调用者给出的基础字符串生成 domain 私有名称。
 * DOMAIN_BUS_ANY 保留原名称，其余总线类型把 token 追加为后缀，使同一控制器
 * 上面向不同总线语义的 domain 可区分。成功后设置 IRQ_DOMAIN_NAME_ALLOCATED，
 * 从而把名称所有权交给 domain 的销毁路径；失败返回 -ENOMEM，domain 尚未取得
 * 名称所有权。使用 GFP_KERNEL，调用环境必须可睡眠。
 */
static int alloc_name(struct irq_domain *domain, char *base, enum irq_domain_bus_token bus_token)
{
	if (bus_token == DOMAIN_BUS_ANY)
		domain->name = kasprintf(GFP_KERNEL, "%s", base);
	else
		domain->name = kasprintf(GFP_KERNEL, "%s-%d", base, bus_token);
	if (!domain->name)
		return -ENOMEM;

	domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
	return 0;
}

/*
 * 根据真实固件节点路径、可选业务后缀和总线 token 生成 domain 名称。
 * @suffix 只在同一设备节点创建多个 domain 时用于消除重名；空后缀不会额外
 * 插入分隔符。名称由 domain 拥有，并用 IRQ_DOMAIN_NAME_ALLOCATED 标记。
 *
 * 固件路径可能含有“/”，而 debugfs 会把它解释为目录分隔符；因此这里把
 * “/”替换成“:”。这只改变调试展示名称，不改变 fwnode 身份或匹配关系。
 */
static int alloc_fwnode_name(struct irq_domain *domain, const struct fwnode_handle *fwnode,
			     enum irq_domain_bus_token bus_token, const char *suffix)
{
	const char *sep = suffix ? "-" : "";
	const char *suf = suffix ? : "";
	char *name;

	if (bus_token == DOMAIN_BUS_ANY)
		name = kasprintf(GFP_KERNEL, "%pfw%s%s", fwnode, sep, suf);
	else
		name = kasprintf(GFP_KERNEL, "%pfw%s%s-%d", fwnode, sep, suf, bus_token);
	if (!name)
		return -ENOMEM;

	/*
	 * fwnode paths contain '/', which debugfs is legitimately unhappy
	 * about. Replace them with ':', which does the trick and is not as
	 * offensive as '\'...
	 */
	/*
	 * fwnode 路径包含“/”，debugfs 会合理地拒绝把它当普通文件名使用。
	 * 将其替换为“:”即可规避目录语义，也比使用反斜杠更易读。
	 */
	domain->name = strreplace(name, '/', ':');
	domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
	return 0;
}

/*
 * 在没有可用固件身份和既有名称时生成进程内唯一的兜底名称。
 * 原子递增编号允许并发创建者不借助全局 domain 锁也不会获得相同编号；总线
 * token 仍按需成为后缀。编号只保证本次内核运行期间不重复，不是稳定 ABI。
 */
static int alloc_unknown_name(struct irq_domain *domain, enum irq_domain_bus_token bus_token)
{
	static atomic_t unknown_domains;
	int id = atomic_inc_return(&unknown_domains);

	if (bus_token == DOMAIN_BUS_ANY)
		domain->name = kasprintf(GFP_KERNEL, "unknown-%d", id);
	else
		domain->name = kasprintf(GFP_KERNEL, "unknown-%d-%d", id, bus_token);
	if (!domain->name)
		return -ENOMEM;

	domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
	return 0;
}

/*
 * 为尚未发布的 domain 选择名称和所有权策略。
 *
 * 选择顺序如下：
 * 1. irqchip 专用 fwnode：命名类型复制其传递的名称，其他类型通常借用节点名；
 * 2. OF、ACPI 或软件节点：根据完整固件路径分配名称；
 * 3. 调用者已预置 domain->name：直接保留；
 * 4. 最后生成 unknown-N 兜底名称。
 *
 * 仅真实设备节点支持 name_suffix。irqchip-fwnode 的命名类型本身已承载显式
 * 名称，再接受 suffix 会造成含义和所有权不清，故返回 -EINVAL。凡经 alloc_*
 * 分配的名称都会设置 IRQ_DOMAIN_NAME_ALLOCATED，销毁路径据此决定是否 kfree。
 */
static int irq_domain_set_name(struct irq_domain *domain, const struct irq_domain_info *info)
{
	enum irq_domain_bus_token bus_token = info->bus_token;
	const struct fwnode_handle *fwnode = info->fwnode;

	if (is_fwnode_irqchip(fwnode)) {
		const struct irqchip_fwid *fwid = container_of(fwnode, struct irqchip_fwid, fwnode);

		/*
		 * The name_suffix is only intended to be used to avoid a name
		 * collision when multiple domains are created for a single
		 * device and the name is picked using a real device node.
		 * (Typical use-case is regmap-IRQ controllers for devices
		 * providing more than one physical IRQ.) There should be no
		 * need to use name_suffix with irqchip-fwnode.
		 */
		/*
		 * name_suffix 只用于同一真实设备节点创建多个 domain 时避免名称冲突，
		 * 典型场景是一个 regmap-IRQ 设备提供多个物理 IRQ。irqchip-fwnode
		 * 已有自己的命名方式，不应再叠加 name_suffix。
		 */
		if (info->name_suffix)
			return -EINVAL;

		switch (fwid->type) {
		case IRQCHIP_FWNODE_NAMED:
		case IRQCHIP_FWNODE_NAMED_ID:
			return alloc_name(domain, fwid->name, bus_token);
		default:
			domain->name = fwid->name;
			if (bus_token != DOMAIN_BUS_ANY)
				return alloc_name(domain, fwid->name, bus_token);
		}

	} else if (is_of_node(fwnode) || is_acpi_device_node(fwnode) || is_software_node(fwnode)) {
		return alloc_fwnode_name(domain, fwnode, bus_token, info->name_suffix);
	}

	if (domain->name)
		return 0;

	if (fwnode)
		pr_err("Invalid fwnode type for irqdomain\n");
	return alloc_unknown_name(domain, bus_token);
}

/*
 * 分配并初始化一个尚未对全局查询者可见的 irq_domain。
 *
 * 输入约束首先排除线性 revmap 与 direct/nomap 模式混用，并要求 direct_max
 * 覆盖完整 hwirq 空间。对象尾部按 info->size 一次性分配线性 revmap 数组，
 * NUMA 节点取自 OF 节点；随后确定名称、取得 fwnode 引用，并初始化 radix tree、
 * 操作表和映射边界。
 *
 * 返回成功对象时，调用者拥有唯一清理责任，但它还未进入 irq_domain_list，
 * 中断查找路径也看不到它。错误以 ERR_PTR 返回；名称设置失败时仅需释放对象，
 * 因为该阶段尚未取得 fwnode 引用。GFP_KERNEL 和名称分配决定本函数可睡眠。
 */
static struct irq_domain *__irq_domain_create(const struct irq_domain_info *info)
{
	struct irq_domain *domain;
	int err;

	if (WARN_ON((info->size && info->direct_max) ||
		    (!IS_ENABLED(CONFIG_IRQ_DOMAIN_NOMAP) && info->direct_max) ||
		    (info->direct_max && info->direct_max != info->hwirq_max)))
		return ERR_PTR(-EINVAL);

	domain = kzalloc_node(struct_size(domain, revmap, info->size),
			      GFP_KERNEL, of_node_to_nid(to_of_node(info->fwnode)));
	if (!domain)
		return ERR_PTR(-ENOMEM);

	err = irq_domain_set_name(domain, info);
	if (err) {
		kfree(domain);
		return ERR_PTR(err);
	}

	domain->fwnode = fwnode_handle_get(info->fwnode);
	fwnode_dev_initialized(domain->fwnode, true);

	/* Fill structure */
	/* 填充其余结构字段；此时对象仍处于未发布状态。 */
	INIT_RADIX_TREE(&domain->revmap_tree, GFP_KERNEL);
	domain->ops = info->ops;
	domain->host_data = info->host_data;
	domain->bus_token = info->bus_token;
	domain->hwirq_max = info->hwirq_max;

	if (info->direct_max)
		domain->flags |= IRQ_DOMAIN_FLAG_NO_MAP;

	domain->revmap_size = info->size;

	/*
	 * Hierarchical domains use the domain lock of the root domain
	 * (innermost domain).
	 *
	 * For non-hierarchical domains (as for root domains), the root
	 * pointer is set to the domain itself so that &domain->root->mutex
	 * always points to the right lock.
	 */
	/*
	 * 层级 domain 共用根 domain（最内层控制器）的互斥锁。
	 * 非层级 domain 和根 domain 都先把 root 指向自身，从而让
	 * &domain->root->mutex 始终是正确的串行化锁；子 domain 稍后实例化时
	 * 再改为父层级的 root。
	 */
	mutex_init(&domain->mutex);
	domain->root = domain;

	irq_domain_check_hierarchy(domain);

	return domain;
}

/*
 * 把已完整初始化的 domain 发布到 debugfs 和全局注册表。
 * irq_domain_mutex 使调试目录与链表可见性同步推进，避免查找者观察到尚未完成
 * 注册的对象。调用者在此之后不能再按“私有半成品”方式直接释放 domain，而
 * 必须走 irq_domain_remove() 撤销发布。
 */
static void __irq_domain_publish(struct irq_domain *domain)
{
	mutex_lock(&irq_domain_mutex);
	debugfs_add_domain_dir(domain);
	list_add(&domain->link, &irq_domain_list);
	mutex_unlock(&irq_domain_mutex);

	pr_debug("Added domain %s\n", domain->name);
}

/*
 * 释放一个已经撤销发布或从未发布的 domain 底层存储。
 * 先撤销 fwnode 的设备初始化状态并归还引用；名称仅在
 * IRQ_DOMAIN_NAME_ALLOCATED 置位时由 domain 拥有。调用者必须已清除映射、
 * generic chip、链表/debugfs 项以及所有并发引用，本函数本身不做这些收尾。
 */
static void irq_domain_free(struct irq_domain *domain)
{
	fwnode_dev_initialized(domain->fwnode, false);
	fwnode_handle_put(domain->fwnode);
	if (domain->flags & IRQ_DOMAIN_NAME_ALLOCATED)
		kfree(domain->name);
	kfree(domain);
}

/*
 * 在稀疏 IRQ 配置下，为固定 virq 区间尽力预分配 irq_desc。
 * 非稀疏配置的描述符静态存在，直接返回。分配失败只记录信息而不使 domain
 * 实例化失败，因为兼容路径允许描述符已经由架构或更早阶段预分配；真正关联
 * 时仍会检验每个 virq 是否存在。
 */
static void irq_domain_instantiate_descs(const struct irq_domain_info *info)
{
	if (!IS_ENABLED(CONFIG_SPARSE_IRQ))
		return;

	if (irq_alloc_descs(info->virq_base, info->virq_base, info->size,
			    of_node_to_nid(to_of_node(info->fwnode))) < 0) {
		pr_info("Cannot allocate irq_descs @ IRQ%d, assuming pre-allocated\n",
			info->virq_base);
	}
}

/*
 * 完成 domain 从“私有对象”到“已发布且可选预关联”的完整实例化事务。
 *
 * 顺序不可随意交换：先创建基础对象并接入父层级的 root 锁；再分配 generic
 * chip、执行控制器 init 回调；只有这些可能失败的准备都成功后才发布到全局
 * 链表。发布后可按兼容模式预分配 irq_desc，并建立固定 virq/hwirq 映射。
 *
 * generic chip 或 init 失败时按逆序撤销已取得资源，domain 从未发布，因此不会
 * 被并发查找者看见。注意预关联函数本身按项报告失败而不回滚整个 domain，这
 * 保留了旧式控制器“部分描述符预先存在”的兼容语义。
 */
static struct irq_domain *__irq_domain_instantiate(const struct irq_domain_info *info,
						   bool cond_alloc_descs, bool force_associate)
{
	struct irq_domain *domain;
	int err;

	domain = __irq_domain_create(info);
	if (IS_ERR(domain))
		return domain;

	domain->flags |= info->domain_flags;
	domain->exit = info->exit;
	domain->dev = info->dev;

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	if (info->parent) {
		domain->root = info->parent->root;
		domain->parent = info->parent;
	}
#endif

	if (info->dgc_info) {
		err = irq_domain_alloc_generic_chips(domain, info->dgc_info);
		if (err)
			goto err_domain_free;
	}

	if (info->init) {
		err = info->init(domain);
		if (err)
			goto err_domain_gc_remove;
	}

	__irq_domain_publish(domain);

	if (cond_alloc_descs && info->virq_base > 0)
		irq_domain_instantiate_descs(info);

	/*
	 * Legacy interrupt domains have a fixed Linux interrupt number
	 * associated. Other interrupt domains can request association by
	 * providing a Linux interrupt number > 0.
	 */
	/*
	 * 传统 interrupt domain 具有固定 Linux IRQ 号；其他 domain 也可通过提供
	 * 大于 0 的 Linux IRQ 起始号请求预关联。动态映射 domain 则把关联推迟到
	 * 首次解析具体中断时。
	 */
	if (force_associate || info->virq_base > 0) {
		irq_domain_associate_many(domain, info->virq_base, info->hwirq_base,
					  info->size - info->hwirq_base);
	}

	return domain;

err_domain_gc_remove:
	if (info->dgc_info)
		irq_domain_remove_generic_chips(domain);
err_domain_free:
	irq_domain_free(domain);
	return ERR_PTR(err);
}

/**
 * irq_domain_instantiate() - Instantiate a new irq domain data structure
 * @info: Domain information pointer pointing to the information for this domain
 *
 * Return: A pointer to the instantiated irq domain or an ERR_PTR value.
 */
/*
 * 根据 @info 实例化新的 IRQ domain。
 * 返回已发布 domain 的指针，失败返回 ERR_PTR。此通用入口不预分配描述符，也
 * 不强制预关联；调用者后续可按固件中断描述动态创建映射。
 */
struct irq_domain *irq_domain_instantiate(const struct irq_domain_info *info)
{
	return __irq_domain_instantiate(info, false, false);
}
EXPORT_SYMBOL_GPL(irq_domain_instantiate);

/**
 * irq_domain_remove() - Remove an irq domain.
 * @domain: domain to remove
 *
 * This routine is used to remove an irq domain. The caller must ensure
 * that all mappings within the domain have been disposed of prior to
 * use, depending on the revmap type.
 */
/*
 * 从系统中移除并释放 @domain。
 * 调用者必须事先销毁该 domain 的全部映射；本函数只对仍非空的 radix revmap
 * 发出警告，并不会替调用者安全回收活跃 IRQ。
 *
 * 阶段顺序为：先让控制器 exit 回调停止其私有功能；在 irq_domain_mutex 下删除
 * debugfs 和全局链表项、必要时清空默认 domain；随后移除由 domain 拥有的
 * generic chips，最后归还 fwnode、名称和对象。释放后任何缓存的 domain 指针
 * 都失效，因此设备拆卸路径还必须先排空所有并发使用者。
 */
void irq_domain_remove(struct irq_domain *domain)
{
	if (domain->exit)
		domain->exit(domain);

	mutex_lock(&irq_domain_mutex);
	debugfs_remove_domain_dir(domain);

	WARN_ON(!radix_tree_empty(&domain->revmap_tree));

	list_del(&domain->link);

	/*
	 * If the going away domain is the default one, reset it.
	 */
	/* 若被移除的是默认 domain，同时清空兼容性回退指针，避免悬空引用。 */
	if (unlikely(irq_default_domain == domain))
		irq_set_default_domain(NULL);

	mutex_unlock(&irq_domain_mutex);

	if (domain->flags & IRQ_DOMAIN_FLAG_DESTROY_GC)
		irq_domain_remove_generic_chips(domain);

	pr_debug("Removed domain %s\n", domain->name);
	irq_domain_free(domain);
}
EXPORT_SYMBOL_GPL(irq_domain_remove);

/*
 * 更新已发布 domain 的总线 token，并同步重建带 token 后缀的展示名称。
 * 全局互斥锁把 token、名称所有权和 debugfs 文件的变化组成一个事务。名称分配
 * 失败时 token 已更新而旧名称保留，这是允许的降级状态：映射语义已改变，但
 * 调试名称不会造成内存或悬空引用问题。成功后新名称归 domain 所有。
 *
 * 该函数会在锁内以 GFP_KERNEL 分配并操作 debugfs，只能在可睡眠上下文调用。
 */
void irq_domain_update_bus_token(struct irq_domain *domain,
				 enum irq_domain_bus_token bus_token)
{
	char *name;

	if (domain->bus_token == bus_token)
		return;

	mutex_lock(&irq_domain_mutex);

	domain->bus_token = bus_token;

	name = kasprintf(GFP_KERNEL, "%s-%d", domain->name, bus_token);
	if (!name) {
		mutex_unlock(&irq_domain_mutex);
		return;
	}

	debugfs_remove_domain_dir(domain);

	if (domain->flags & IRQ_DOMAIN_NAME_ALLOCATED)
		kfree(domain->name);
	else
		domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;

	domain->name = name;
	debugfs_add_domain_dir(domain);

	mutex_unlock(&irq_domain_mutex);
}
EXPORT_SYMBOL_GPL(irq_domain_update_bus_token);

/**
 * irq_domain_create_simple() - Register an irq_domain and optionally map a range of irqs
 * @fwnode: firmware node for the interrupt controller
 * @size: total number of irqs in mapping
 * @first_irq: first number of irq block assigned to the domain,
 *	pass zero to assign irqs on-the-fly. If first_irq is non-zero, then
 *	pre-map all of the irqs in the domain to virqs starting at first_irq.
 * @ops: domain callbacks
 * @host_data: Controller private data pointer
 *
 * Allocates an irq_domain, and optionally if first_irq is positive then also
 * allocate irq_descs and map all of the hwirqs to virqs starting at first_irq.
 *
 * This is intended to implement the expected behaviour for most
 * interrupt controllers. If device tree is used, then first_irq will be 0 and
 * irqs get mapped dynamically on the fly. However, if the controller requires
 * static virq assignments (non-DT boot) then it will set that up correctly.
 */
/*
 * 注册一个适合多数控制器的简单 domain，并可选地预映射一段 IRQ。
 * @fwnode：中断控制器固件节点；@size：hwirq 空间大小；@first_irq：固定 virq
 * 起点，0 表示按需动态分配；@ops：domain 回调；@host_data：控制器私有数据。
 *
 * first_irq 大于 0 时会尽力分配 irq_desc，并把从 hwirq 0 开始的整个区间映射
 * 到该 virq 起点；设备树平台通常传 0，由后续固件解析按需建图。失败对旧接口
 * 表示为 NULL，而不是内部实例化函数使用的 ERR_PTR。
 */
struct irq_domain *irq_domain_create_simple(struct fwnode_handle *fwnode,
					    unsigned int size,
					    unsigned int first_irq,
					    const struct irq_domain_ops *ops,
					    void *host_data)
{
	struct irq_domain_info info = {
		.fwnode		= fwnode,
		.size		= size,
		.hwirq_max	= size,
		.virq_base	= first_irq,
		.ops		= ops,
		.host_data	= host_data,
	};
	struct irq_domain *domain = __irq_domain_instantiate(&info, true, false);

	return IS_ERR(domain) ? NULL : domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_simple);

/*
 * 创建具有固定 hwirq 与 virq 起点的传统 domain。
 * 与 simple 入口不同，它把 first_hwirq 纳入 revmap 尺寸，并强制关联指定区间；
 * 适用于没有动态固件映射的旧平台。描述符应由架构预先准备，函数不会条件分配
 * irq_desc。失败返回 NULL，成功对象由调用者最终用 irq_domain_remove() 销毁。
 */
struct irq_domain *irq_domain_create_legacy(struct fwnode_handle *fwnode,
					 unsigned int size,
					 unsigned int first_irq,
					 irq_hw_number_t first_hwirq,
					 const struct irq_domain_ops *ops,
					 void *host_data)
{
	struct irq_domain_info info = {
		.fwnode		= fwnode,
		.size		= first_hwirq + size,
		.hwirq_max	= first_hwirq + size,
		.hwirq_base	= first_hwirq,
		.virq_base	= first_irq,
		.ops		= ops,
		.host_data	= host_data,
	};
	struct irq_domain *domain = __irq_domain_instantiate(&info, false, true);

	return IS_ERR(domain) ? NULL : domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_legacy);

/**
 * irq_find_matching_fwspec() - Locates a domain for a given fwspec
 * @fwspec: FW specifier for an interrupt
 * @bus_token: domain-specific data
 */
/*
 * 根据固件中断描述 @fwspec 和总线 token 查找已发布 domain。
 * 在 irq_domain_mutex 下遍历全局链表，优先让 domain 的 select 回调处理完整
 * fwspec；否则退到传统 match 回调，最后才比较 fwnode 身份和 token。
 * DOMAIN_BUS_ANY 是通配请求，其他 token 必须精确匹配。
 *
 * 返回的是借用指针，不增加引用。调用方所处的设备/初始化生命周期必须保证
 * domain 不会并发移除；回调运行在互斥锁内，允许睡眠但不得递归修改 domain
 * 全局链表，否则会自锁。
 */
struct irq_domain *irq_find_matching_fwspec(struct irq_fwspec *fwspec,
					    enum irq_domain_bus_token bus_token)
{
	struct irq_domain *h, *found = NULL;
	struct fwnode_handle *fwnode = fwspec->fwnode;
	int rc;

	/*
	 * We might want to match the legacy controller last since
	 * it might potentially be set to match all interrupts in
	 * the absence of a device node. This isn't a problem so far
	 * yet though...
	 *
	 * bus_token == DOMAIN_BUS_ANY matches any domain, any other
	 * values must generate an exact match for the domain to be
	 * selected.
	 */
	/*
	 * 理想情况下应最后匹配传统控制器，因为缺少设备节点时它可能通配所有中断；
	 * 当前尚未因此出现问题。DOMAIN_BUS_ANY 可匹配任意 domain，其他 token 值
	 * 必须精确匹配后才能选中。
	 */
	mutex_lock(&irq_domain_mutex);
	list_for_each_entry(h, &irq_domain_list, link) {
		if (h->ops->select && bus_token != DOMAIN_BUS_ANY)
			rc = h->ops->select(h, fwspec, bus_token);
		else if (h->ops->match)
			rc = h->ops->match(h, to_of_node(fwnode), bus_token);
		else
			rc = ((fwnode != NULL) && (h->fwnode == fwnode) &&
			      ((bus_token == DOMAIN_BUS_ANY) ||
			       (h->bus_token == bus_token)));

		if (rc) {
			found = h;
			break;
		}
	}
	mutex_unlock(&irq_domain_mutex);
	return found;
}
EXPORT_SYMBOL_GPL(irq_find_matching_fwspec);

/**
 * irq_set_default_domain() - Set a "default" irq domain
 * @domain: default domain pointer
 *
 * For convenience, it's possible to set a "default" domain that will be used
 * whenever NULL is passed to irq_create_mapping(). It makes life easier for
 * platforms that want to manipulate a few hard coded interrupt numbers that
 * aren't properly represented in the device-tree.
 */
/*
 * 设置兼容性的“默认”IRQ domain。
 * 当 irq_create_mapping() 收到 NULL domain 时会回退到这里，主要服务于无法在
 * 设备树中正确表达的少量硬编码中断。此函数不取得引用也不加锁，平台初始化与
 * 拆卸代码必须自行保证赋值时序和对象寿命；现代驱动应始终传递明确 domain。
 */
void irq_set_default_domain(struct irq_domain *domain)
{
	pr_debug("Default domain set to @0x%p\n", domain);

	irq_default_domain = domain;
}
EXPORT_SYMBOL_GPL(irq_set_default_domain);

/**
 * irq_get_default_domain() - Retrieve the "default" irq domain
 *
 * Returns: the default domain, if any.
 *
 * Modern code should never use this. This should only be used on
 * systems that cannot implement a firmware->fwnode mapping (which
 * both DT and ACPI provide).
 */
/*
 * 返回当前默认 domain；未设置时返回 NULL。
 * 返回值是无引用的快照，只适用于不能实现 firmware 到 fwnode 映射的旧系统；
 * DT 和 ACPI 均已有正式映射，现代代码不应依赖该全局回退入口。
 */
struct irq_domain *irq_get_default_domain(void)
{
	return irq_default_domain;
}
EXPORT_SYMBOL_GPL(irq_get_default_domain);

/*
 * 判断 domain 是否使用 hwirq == virq 的 direct/nomap 模式。
 * 标志只有在内核启用 CONFIG_IRQ_DOMAIN_NOMAP 时才生效，避免配置关闭时误跳过
 * 必需的反向映射维护。
 */
static bool irq_domain_is_nomap(struct irq_domain *domain)
{
	return IS_ENABLED(CONFIG_IRQ_DOMAIN_NOMAP) &&
	       (domain->flags & IRQ_DOMAIN_FLAG_NO_MAP);
}

/*
 * 在持有层级根锁时撤销一个 hwirq 的反向映射。
 * nomap 无独立表项；小 hwirq 位于内嵌线性数组，通过 rcu_assign_pointer()
 * 发布 NULL；大 hwirq 位于 radix tree。调用者还需负责先停止 IRQ、清理 irq_data
 * 及等待必要的并发读者，本辅助函数只修改索引。
 */
static void irq_domain_clear_mapping(struct irq_domain *domain,
				     irq_hw_number_t hwirq)
{
	lockdep_assert_held(&domain->root->mutex);

	if (irq_domain_is_nomap(domain))
		return;

	if (hwirq < domain->revmap_size)
		rcu_assign_pointer(domain->revmap[hwirq], NULL);
	else
		radix_tree_delete(&domain->revmap_tree, hwirq);
}

/*
 * 在持有层级根锁时发布 hwirq 到 irq_data 的反向映射。
 * 线性区使用 RCU 指针，使中断路径可无锁读取；超出线性区的键写入 radix tree。
 * 调用者必须先完整初始化 irq_data，发布之后读者即可沿其 domain/chip 状态使用。
 */
static void irq_domain_set_mapping(struct irq_domain *domain,
				   irq_hw_number_t hwirq,
				   struct irq_data *irq_data)
{
	/*
	 * This also makes sure that all domains point to the same root when
	 * called from irq_domain_insert_irq() for each domain in a hierarchy.
	 */
	/*
	 * irq_domain_insert_irq() 会为层级中的每个 domain 调用这里；根锁断言也确保
	 * 这些 domain 都指向同一个 root，否则无法用一把锁串行化整条 irq_data 链。
	 */
	lockdep_assert_held(&domain->root->mutex);

	if (irq_domain_is_nomap(domain))
		return;

	if (hwirq < domain->revmap_size)
		rcu_assign_pointer(domain->revmap[hwirq], irq_data);
	else
		radix_tree_insert(&domain->revmap_tree, hwirq, irq_data);
}

/*
 * 解除非层级 domain 中一个 virq 与 hwirq 的关联。
 *
 * 在根锁下先置 IRQ_NOREQUEST 阻止新申请，再移除 chip/handler；synchronize_irq()
 * 等待已经开始的处理完成后才调用控制器 unmap。其后的 smp_mb() 保证驱动取消
 * 映射产生的状态更新先于核心清空 irq_data 与反向映射对其他 CPU 可见。
 * mapcount 和 revmap 最后提交，返回后该 virq 的 irq_desc 仍存在但不再属于 domain。
 * 本函数可睡眠，且调用者必须保证不是从该 IRQ 自身的处理上下文进入。
 */
static void irq_domain_disassociate(struct irq_domain *domain, unsigned int irq)
{
	struct irq_data *irq_data = irq_get_irq_data(irq);
	irq_hw_number_t hwirq;

	if (WARN(!irq_data || irq_data->domain != domain,
		 "virq%i doesn't exist; cannot disassociate\n", irq))
		return;

	hwirq = irq_data->hwirq;

	mutex_lock(&domain->root->mutex);

	irq_set_status_flags(irq, IRQ_NOREQUEST);

	/* remove chip and handler */
	/* 移除 irq_chip 和流控处理函数，令后续进入者无法再调用控制器操作。 */
	irq_set_chip_and_handler(irq, NULL, NULL);

	/* Make sure it's completed */
	/* 等待所有已经在途的该 IRQ 处理完成。 */
	synchronize_irq(irq);

	/* Tell the PIC about it */
	/* 通知物理中断控制器撤销硬件侧映射。 */
	if (domain->ops->unmap)
		domain->ops->unmap(domain, irq);
	smp_mb();

	irq_data->domain = NULL;
	irq_data->hwirq = 0;
	domain->mapcount--;

	/* Clear reverse map for this hwirq */
	/* 最后清除从 hwirq 回查 irq_data 的入口。 */
	irq_domain_clear_mapping(domain, hwirq);

	mutex_unlock(&domain->root->mutex);
}

/*
 * 在已持有 domain 根锁的条件下，把现有 irq_desc 关联到一个 hwirq。
 * 先验证范围、描述符存在且尚未关联，再临时写入 irq_data 供控制器 map 回调配置
 * chip/handler。回调失败会恢复 domain/hwirq；-EPERM 表示固件保护，按预期静默。
 *
 * 成功路径先增加 mapcount、发布反向映射，最后清除 IRQ_NOREQUEST 允许申请，
 * 因而外部使用者不会看到“可申请但 revmap 尚未建立”的半成品状态。
 */
static int irq_domain_associate_locked(struct irq_domain *domain, unsigned int virq,
				       irq_hw_number_t hwirq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);
	int ret;

	if (WARN(hwirq >= domain->hwirq_max,
		 "error: hwirq 0x%x is too large for %s\n", (int)hwirq, domain->name))
		return -EINVAL;
	if (WARN(!irq_data, "error: virq%i is not allocated", virq))
		return -EINVAL;
	if (WARN(irq_data->domain, "error: virq%i is already associated", virq))
		return -EINVAL;

	irq_data->hwirq = hwirq;
	irq_data->domain = domain;
	if (domain->ops->map) {
		ret = domain->ops->map(domain, virq, hwirq);
		if (ret != 0) {
			/*
			 * If map() returns -EPERM, this interrupt is protected
			 * by the firmware or some other service and shall not
			 * be mapped. Don't bother telling the user about it.
			 */
			/*
			 * map() 返回 -EPERM 表示该中断受固件或其他服务保护，不允许映射；
			 * 这是预期拒绝，因此不再向用户输出噪声日志。
			 */
			if (ret != -EPERM) {
				pr_info("%s didn't like hwirq-0x%lx to VIRQ%i mapping (rc=%d)\n",
				       domain->name, hwirq, virq, ret);
			}
			irq_data->domain = NULL;
			irq_data->hwirq = 0;
			return ret;
		}
	}

	domain->mapcount++;
	irq_domain_set_mapping(domain, hwirq, irq_data);

	irq_clear_status_flags(virq, IRQ_NOREQUEST);

	return 0;
}

/*
 * 把已分配 virq 关联到 @domain 中的 @hwirq。
 * 公共入口只负责取得层级根锁并调用锁内实现；成功后控制器 map 已执行、revmap
 * 已发布且 IRQ 可申请。失败保持描述符未关联，错误码原样返回。函数可睡眠。
 */
int irq_domain_associate(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq)
{
	int ret;

	mutex_lock(&domain->root->mutex);
	ret = irq_domain_associate_locked(domain, virq, hwirq);
	mutex_unlock(&domain->root->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(irq_domain_associate);

/*
 * 按相同偏移批量关联连续 virq 和 hwirq 区间。
 * 每项独立获取根锁并建立映射，单项失败只由 associate 路径记录，不回滚先前
 * 成功项，也不向调用者返回汇总错误；因此它适合启动期兼容映射，不是原子事务。
 */
void irq_domain_associate_many(struct irq_domain *domain, unsigned int irq_base,
			       irq_hw_number_t hwirq_base, int count)
{
	struct device_node *of_node;
	int i;

	of_node = irq_domain_get_of_node(domain);
	pr_debug("%s(%s, irqbase=%i, hwbase=%i, count=%i)\n", __func__,
		of_node_full_name(of_node), irq_base, (int)hwirq_base, count);

	for (i = 0; i < count; i++)
		irq_domain_associate(domain, irq_base + i, hwirq_base + i);
}
EXPORT_SYMBOL_GPL(irq_domain_associate_many);

#ifdef CONFIG_IRQ_DOMAIN_NOMAP
/**
 * irq_create_direct_mapping() - Allocate an irq for direct mapping
 * @domain: domain to allocate the irq for or NULL for default domain
 *
 * This routine is used for irq controllers which can choose the hardware
 * interrupt numbers they generate. In such a case it's simplest to use
 * the linux irq as the hardware interrupt number. It still uses the linear
 * or radix tree to store the mapping, but the irq controller can optimize
 * the revmap path by using the hwirq directly.
 */
/*
 * 为可自行选择硬件中断号的控制器建立 direct 映射。
 * @domain 为 NULL 时使用默认 domain；新分配的 virq 同时作为 hwirq，因此必须
 * 小于 domain->hwirq_max。描述符分配或关联失败都会立即释放描述符并返回 0。
 *
 * 这种模式仍可保留线性/radix 反向映射，但控制器在快速路径上可直接把 hwirq
 * 当作 Linux IRQ 使用。返回 0 表示失败，成功值的释放责任交给映射拥有者。
 */
unsigned int irq_create_direct_mapping(struct irq_domain *domain)
{
	struct device_node *of_node;
	unsigned int virq;

	if (domain == NULL)
		domain = irq_default_domain;

	of_node = irq_domain_get_of_node(domain);
	virq = irq_alloc_desc_from(1, of_node_to_nid(of_node));
	if (!virq) {
		pr_debug("create_direct virq allocation failed\n");
		return 0;
	}
	if (virq >= domain->hwirq_max) {
		pr_err("ERROR: no free irqs available below %lu maximum\n",
			domain->hwirq_max);
		irq_free_desc(virq);
		return 0;
	}
	pr_debug("create_direct obtained virq %d\n", virq);

	if (irq_domain_associate(domain, virq, virq)) {
		irq_free_desc(virq);
		return 0;
	}

	return virq;
}
EXPORT_SYMBOL_GPL(irq_create_direct_mapping);
#endif

/*
 * 在根锁已持有时为单个 hwirq 分配 irq_desc 并建立非层级关联。
 * affinity 只参与描述符的初始亲和性布局；关联失败必须对称释放新描述符。
 * 返回新 virq，任何失败返回 0。调用者必须先确认不存在既有映射。
 */
static unsigned int irq_create_mapping_affinity_locked(struct irq_domain *domain,
						       irq_hw_number_t hwirq,
						       const struct irq_affinity_desc *affinity)
{
	struct device_node *of_node = irq_domain_get_of_node(domain);
	int virq;

	pr_debug("irq_create_mapping(0x%p, 0x%lx)\n", domain, hwirq);

	/* Allocate a virtual interrupt number */
	/* 分配一个 Linux 虚拟中断号及其 irq_desc。 */
	virq = irq_domain_alloc_descs(-1, 1, hwirq, of_node_to_nid(of_node),
				      affinity);
	if (virq <= 0) {
		pr_debug("-> virq allocation failed\n");
		return 0;
	}

	if (irq_domain_associate_locked(domain, virq, hwirq)) {
		irq_free_desc(virq);
		return 0;
	}

	pr_debug("irq %lu on domain %s mapped to virtual irq %u\n",
		hwirq, of_node_full_name(of_node), virq);

	return virq;
}

/**
 * irq_create_mapping_affinity() - Map a hardware interrupt into linux irq space
 * @domain: domain owning this hardware interrupt or NULL for default domain
 * @hwirq: hardware irq number in that domain space
 * @affinity: irq affinity
 *
 * Only one mapping per hardware interrupt is permitted. Returns a linux
 * irq number.
 * If the sense/trigger is to be specified, set_irq_type() should be called
 * on the number returned from that call.
 */
/*
 * 把 @domain 空间中的 @hwirq 映射到 Linux IRQ，并应用可选初始亲和性。
 * 每个 hwirq 只允许一个映射；返回现有或新建 virq，失败返回 0。若 domain 为
 * NULL 则尝试默认 domain，两者都为空会告警。触发类型不由本函数设置，调用者
 * 应在返回的 virq 上另行调用 set_irq_type()。
 *
 * 根锁把“检查既有映射—分配—关联”串成原子过程，防止两个并发创建者为同一
 * hwirq 分配不同 virq。函数涉及互斥锁和内存分配，只能在可睡眠上下文调用。
 */
unsigned int irq_create_mapping_affinity(struct irq_domain *domain,
					 irq_hw_number_t hwirq,
					 const struct irq_affinity_desc *affinity)
{
	int virq;

	/* Look for default domain if necessary */
	/* 调用者未指定时使用旧平台的默认 domain。 */
	if (domain == NULL)
		domain = irq_default_domain;
	if (domain == NULL) {
		WARN(1, "%s(, %lx) called with NULL domain\n", __func__, hwirq);
		return 0;
	}

	mutex_lock(&domain->root->mutex);

	/* Check if mapping already exists */
	/* 锁内复查，确保同一 hwirq 的映射唯一。 */
	virq = irq_find_mapping(domain, hwirq);
	if (virq) {
		pr_debug("existing mapping on virq %d\n", virq);
		goto out;
	}

	virq = irq_create_mapping_affinity_locked(domain, hwirq, affinity);
out:
	mutex_unlock(&domain->root->mutex);

	return virq;
}
EXPORT_SYMBOL_GPL(irq_create_mapping_affinity);

/*
 * 把统一固件中断描述转换为 domain 内部 hwirq 与触发类型。
 * 层级 domain 优先使用可理解通用 irq_fwspec 的 translate 回调；传统 OF
 * domain 使用 xlate 回调。两者都没有时把第一个参数当作中断线编号，触发类型
 * 保持调用者预置值。回调只做语义翻译，不创建映射或取得资源。
 */
static int irq_domain_translate(struct irq_domain *d,
				struct irq_fwspec *fwspec,
				irq_hw_number_t *hwirq, unsigned int *type)
{
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	if (d->ops->translate)
		return d->ops->translate(d, fwspec, hwirq, type);
#endif
	if (d->ops->xlate)
		return d->ops->xlate(d, to_of_node(fwspec->fwnode),
				     fwspec->param, fwspec->param_count,
				     hwirq, type);

	/* If domain has no translation, then we assume interrupt line */
	/* domain 未提供翻译器时，约定第一个单元就是硬件中断线编号。 */
	*hwirq = fwspec->param[0];
	return 0;
}

/*
 * 把 OF phandle 参数转换为固件无关的 irq_fwspec 表示。
 * fwnode 和参数仅按值写入目标结构，不取得设备节点引用；调用者必须保证 np
 * 在后续查找和翻译期间存活，并保证 count 不超过 fwspec 参数数组容量。
 */
void of_phandle_args_to_fwspec(struct device_node *np, const u32 *args,
			       unsigned int count, struct irq_fwspec *fwspec)
{
	int i;

	fwspec->fwnode = of_fwnode_handle(np);
	fwspec->param_count = count;

	for (i = 0; i < count; i++)
		fwspec->param[i] = args[i];
}
EXPORT_SYMBOL_GPL(of_phandle_args_to_fwspec);

/*
 * 为固件中断描述选择拥有它的 domain。
 * 有 fwnode 时先精确寻找有线中断 domain，再兼容 DOMAIN_BUS_ANY；无 fwnode
 * 时只能回退默认 domain。返回值是借用指针，其寿命受设备初始化/拆卸时序约束。
 */
static struct irq_domain *fwspec_to_domain(struct irq_fwspec *fwspec)
{
	struct irq_domain *domain;

	if (fwspec->fwnode) {
		domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_WIRED);
		if (!domain)
			domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_ANY);
	} else {
		domain = irq_default_domain;
	}

	return domain;
}

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
/*
 * 查询 domain 对一个 fwspec 的层级扩展信息。
 * 输出结构总是先清零；未找到 domain 或没有 get_fwspec_info 回调并非错误，返回
 * 0 表示没有额外信息。回调负责填充父级规格等控制器相关内容并返回错误码。
 */
int irq_populate_fwspec_info(struct irq_fwspec *fwspec, struct irq_fwspec_info *info)
{
	struct irq_domain *domain = fwspec_to_domain(fwspec);

	memset(info, 0, sizeof(*info));

	if (!domain || !domain->ops->get_fwspec_info)
		return 0;

	return domain->ops->get_fwspec_info(fwspec, info);
}
#endif

/*
 * 根据固件中断描述查找/创建唯一 Linux IRQ 映射。
 *
 * 处理阶段：选择 domain；翻译 hwirq 与触发类型并过滤非法类型位；在层级根锁
 * 下复查既有映射；若不存在，则按 domain 类型走 MSI 设备分配、层级分配或普通
 * 描述符关联；最后把触发类型记录到最外层 irq_data。返回 virq，失败返回 0。
 *
 * 已有映射可以复用，但触发类型必须为空或一致；若旧映射尚未设置类型，可在锁
 * 内补上，否则拒绝冲突，避免两个固件消费者用不同电气语义共享同一中断线。
 * MSI 设备分配会临时释放 root 锁，因为其内部有自己的分配/锁顺序；重新加锁后
 * 才继续公共提交路径。函数可能分配内存并获取互斥锁，只能在可睡眠上下文调用。
 */
unsigned int irq_create_fwspec_mapping(struct irq_fwspec *fwspec)
{
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_domain *domain;
	struct irq_data *irq_data;
	irq_hw_number_t hwirq;
	int virq;

	domain = fwspec_to_domain(fwspec);
	if (!domain) {
		pr_warn("no irq domain found for %s !\n",
			of_node_full_name(to_of_node(fwspec->fwnode)));
		return 0;
	}

	if (irq_domain_translate(domain, fwspec, &hwirq, &type))
		return 0;

	/*
	 * WARN if the irqchip returns a type with bits
	 * outside the sense mask set and clear these bits.
	 */
	/* irqchip 若返回感知掩码之外的类型位则告警，并剔除这些非法位。 */
	if (WARN_ON(type & ~IRQ_TYPE_SENSE_MASK))
		type &= IRQ_TYPE_SENSE_MASK;

	mutex_lock(&domain->root->mutex);

	/*
	 * If we've already configured this interrupt,
	 * don't do it again, or hell will break loose.
	 */
	/* 已配置的 hwirq 必须复用原映射，重复执行控制器配置会破坏硬件状态。 */
	virq = irq_find_mapping(domain, hwirq);
	if (virq) {
		/*
		 * If the trigger type is not specified or matches the
		 * current trigger type then we are done so return the
		 * interrupt number.
		 */
		/* 未指定新类型或新旧类型一致时，直接返回现有 virq。 */
		if (type == IRQ_TYPE_NONE || type == irq_get_trigger_type(virq))
			goto out;

		/*
		 * If the trigger type has not been set yet, then set
		 * it now and return the interrupt number.
		 */
		/* 现有映射尚无触发类型时，可用本次固件信息补齐后返回。 */
		if (irq_get_trigger_type(virq) == IRQ_TYPE_NONE) {
			irq_data = irq_get_irq_data(virq);
			if (!irq_data) {
				virq = 0;
				goto out;
			}

			irqd_set_trigger_type(irq_data, type);
			goto out;
		}

		pr_warn("type mismatch, failed to map hwirq-%lu for %s!\n",
			hwirq, of_node_full_name(to_of_node(fwspec->fwnode)));
		virq = 0;
		goto out;
	}

	if (irq_domain_is_hierarchy(domain)) {
		if (irq_domain_is_msi_device(domain)) {
			mutex_unlock(&domain->root->mutex);
			virq = msi_device_domain_alloc_wired(domain, hwirq, type);
			mutex_lock(&domain->root->mutex);
		} else
			virq = irq_domain_alloc_irqs_locked(domain, -1, 1, NUMA_NO_NODE,
							    fwspec, false, NULL);
		if (virq <= 0) {
			virq = 0;
			goto out;
		}
	} else {
		/* Create mapping */
		/* 非层级 domain 分配描述符并建立普通关联。 */
		virq = irq_create_mapping_affinity_locked(domain, hwirq, NULL);
		if (!virq)
			goto out;
	}

	irq_data = irq_get_irq_data(virq);
	if (WARN_ON(!irq_data)) {
		virq = 0;
		goto out;
	}

	/* Store trigger type */
	/* 映射建立完成后，在 irq_data 中保存固件声明的触发类型。 */
	irqd_set_trigger_type(irq_data, type);
out:
	mutex_unlock(&domain->root->mutex);

	return virq;
}
EXPORT_SYMBOL_GPL(irq_create_fwspec_mapping);

/*
 * OF 专用薄封装：把 phandle 参数转换成 irq_fwspec，再走统一映射创建流程。
 * 返回语义与 irq_create_fwspec_mapping() 相同，成功为 virq，失败为 0；本函数
 * 不持有 OF 节点的额外引用。
 */
unsigned int irq_create_of_mapping(struct of_phandle_args *irq_data)
{
	struct irq_fwspec fwspec;

	of_phandle_args_to_fwspec(irq_data->np, irq_data->args,
				  irq_data->args_count, &fwspec);

	return irq_create_fwspec_mapping(&fwspec);
}
EXPORT_SYMBOL_GPL(irq_create_of_mapping);

/**
 * irq_dispose_mapping() - Unmap an interrupt
 * @virq: linux irq number of the interrupt to unmap
 */
/*
 * 销毁 Linux IRQ @virq 的 domain 映射。
 * 0 或不存在的描述符视为空操作；存在描述符却没有 domain 表示核心状态损坏并
 * 告警。层级 domain 由其分配协议递归释放 irq_data/硬件资源，普通 domain 则
 * 先解除关联再释放 irq_desc。调用者必须已 free_irq() 并停止所有用户。
 */
void irq_dispose_mapping(unsigned int virq)
{
	struct irq_data *irq_data;
	struct irq_domain *domain;

	irq_data = virq ? irq_get_irq_data(virq) : NULL;
	if (!irq_data)
		return;

	domain = irq_data->domain;
	if (WARN_ON(domain == NULL))
		return;

	if (irq_domain_is_hierarchy(domain)) {
		irq_domain_free_one_irq(domain, virq);
	} else {
		irq_domain_disassociate(domain, virq);
		irq_free_desc(virq);
	}
}
EXPORT_SYMBOL_GPL(irq_dispose_mapping);

/**
 * __irq_resolve_mapping() - Find a linux irq from a hw irq number.
 * @domain: domain owning this hardware interrupt
 * @hwirq: hardware irq number in that domain space
 * @irq: optional pointer to return the Linux irq if required
 *
 * Returns the interrupt descriptor.
 */
/*
 * 从 @domain 中的 @hwirq 反向解析 irq_desc，并可选返回 Linux IRQ 号。
 * domain 为 NULL 时使用默认 domain；找不到映射返回 NULL，且不修改 @irq。
 *
 * nomap 模式直接以 hwirq 索引 irq_data 并再次核对硬件号。普通模式在 RCU
 * 读侧查询：小编号来自线性 revmap，大编号来自 radix tree。发布者在根锁下
 * 更新索引，RCU 保证这里取到的 irq_data 在转换为 desc 期间仍有效，因此该
 * 查询适合中断快速路径且不会睡眠；返回指针的更长寿命仍由 IRQ 核心规则保证。
 */
struct irq_desc *__irq_resolve_mapping(struct irq_domain *domain,
				       irq_hw_number_t hwirq,
				       unsigned int *irq)
{
	struct irq_desc *desc = NULL;
	struct irq_data *data;

	/* Look for default domain if necessary */
	/* 未显式给出 domain 时尝试旧平台默认值。 */
	if (domain == NULL)
		domain = irq_default_domain;
	if (domain == NULL)
		return desc;

	if (irq_domain_is_nomap(domain)) {
		if (hwirq < domain->hwirq_max) {
			data = irq_domain_get_irq_data(domain, hwirq);
			if (data && data->hwirq == hwirq)
				desc = irq_data_to_desc(data);
			if (irq && desc)
				*irq = hwirq;
		}

		return desc;
	}

	rcu_read_lock();
	/* Check if the hwirq is in the linear revmap. */
	/* 小 hwirq 查内嵌线性表，超出范围则查 radix tree。 */
	if (hwirq < domain->revmap_size)
		data = rcu_dereference(domain->revmap[hwirq]);
	else
		data = radix_tree_lookup(&domain->revmap_tree, hwirq);

	if (likely(data)) {
		desc = irq_data_to_desc(data);
		if (irq)
			*irq = data->irq;
	}

	rcu_read_unlock();
	return desc;
}
EXPORT_SYMBOL_GPL(__irq_resolve_mapping);

/**
 * irq_domain_xlate_onecell() - Generic xlate for direct one cell bindings
 * @d:		Interrupt domain involved in the translation
 * @ctrlr:	The device tree node for the device whose interrupt is translated
 * @intspec:	The interrupt specifier data from the device tree
 * @intsize:	The number of entries in @intspec
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree IRQ specifier translation function which works with one cell
 * bindings where the cell value maps directly to the hwirq number.
 */
/*
 * 翻译单单元设备树 IRQ 描述：第 0 单元直接作为 hwirq，触发类型为空。
 * @d 和 @ctrlr 对这种通用编码不参与计算；少于一个单元返回 -EINVAL。
 */
int irq_domain_xlate_onecell(struct irq_domain *d, struct device_node *ctrlr,
			     const u32 *intspec, unsigned int intsize,
			     unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))
		return -EINVAL;
	*out_hwirq = intspec[0];
	*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_onecell);

/**
 * irq_domain_xlate_twocell() - Generic xlate for direct two cell bindings
 * @d:		Interrupt domain involved in the translation
 * @ctrlr:	The device tree node for the device whose interrupt is translated
 * @intspec:	The interrupt specifier data from the device tree
 * @intsize:	The number of entries in @intspec
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree IRQ specifier translation function which works with two cell
 * bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 */
/*
 * 翻译双单元设备树 IRQ 描述：通常第 0 单元是 hwirq，第 1 单元是 Linux IRQ
 * 类型标志。这里先构造统一 fwspec，再复用 irq_domain_translate_twocell() 的
 * 边界检查与掩码逻辑，避免 OF 与非 OF 调用产生不同语义。
 */
int irq_domain_xlate_twocell(struct irq_domain *d, struct device_node *ctrlr,
			const u32 *intspec, unsigned int intsize,
			irq_hw_number_t *out_hwirq, unsigned int *out_type)
{
	struct irq_fwspec fwspec;

	of_phandle_args_to_fwspec(ctrlr, intspec, intsize, &fwspec);
	return irq_domain_translate_twocell(d, &fwspec, out_hwirq, out_type);
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_twocell);

/**
 * irq_domain_xlate_twothreecell() - Generic xlate for direct two or three cell bindings
 * @d:		Interrupt domain involved in the translation
 * @ctrlr:	The device tree node for the device whose interrupt is translated
 * @intspec:	The interrupt specifier data from the device tree
 * @intsize:	The number of entries in @intspec
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree interrupt specifier translation function for two or three
 * cell bindings, where the cell values map directly to the hardware
 * interrupt number and the type specifier.
 */
/*
 * 翻译双单元或三单元 OF 中断描述。
 * 先将 OF 参数包装为通用 fwspec，再由固件无关翻译器解释两种布局；输出 hwirq
 * 和经过感知掩码过滤的触发类型，非法单元数返回 -EINVAL。
 */
int irq_domain_xlate_twothreecell(struct irq_domain *d, struct device_node *ctrlr,
				  const u32 *intspec, unsigned int intsize,
				  irq_hw_number_t *out_hwirq, unsigned int *out_type)
{
	struct irq_fwspec fwspec;

	of_phandle_args_to_fwspec(ctrlr, intspec, intsize, &fwspec);

	return irq_domain_translate_twothreecell(d, &fwspec, out_hwirq, out_type);
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_twothreecell);

/**
 * irq_domain_xlate_onetwocell() - Generic xlate for one or two cell bindings
 * @d:		Interrupt domain involved in the translation
 * @ctrlr:	The device tree node for the device whose interrupt is translated
 * @intspec:	The interrupt specifier data from the device tree
 * @intsize:	The number of entries in @intspec
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree IRQ specifier translation function which works with either one
 * or two cell bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 *
 * Note: don't use this function unless your interrupt controller explicitly
 * supports both one and two cell bindings.  For the majority of controllers
 * the _onecell() or _twocell() variants above should be used.
 */
/*
 * 翻译显式兼容一单元和双单元绑定的 OF 中断描述。
 * 第 0 单元始终是 hwirq；存在第 1 单元时取其感知类型位，否则类型为 NONE。
 * 只有控制器绑定明确允许两种格式时才能使用；多数控制器应选语义更严格的
 * onecell 或 twocell 版本，避免错误固件被悄悄接受。
 */
int irq_domain_xlate_onetwocell(struct irq_domain *d,
				struct device_node *ctrlr,
				const u32 *intspec, unsigned int intsize,
				unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))
		return -EINVAL;
	*out_hwirq = intspec[0];
	if (intsize > 1)
		*out_type = intspec[1] & IRQ_TYPE_SENSE_MASK;
	else
		*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_onetwocell);

/* 简单 domain 的通用操作表，接受明确支持的一单元/双单元 OF 编码。 */
const struct irq_domain_ops irq_domain_simple_ops = {
	.xlate = irq_domain_xlate_onetwocell,
};
EXPORT_SYMBOL_GPL(irq_domain_simple_ops);

/**
 * irq_domain_translate_onecell() - Generic translate for direct one cell
 * bindings
 * @d:		Interrupt domain involved in the translation
 * @fwspec:	The firmware interrupt specifier to translate
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 */
/*
 * 翻译固件无关的单参数中断描述：param[0] 直接成为 hwirq，类型为 NONE。
 * 参数不足返回 -EINVAL；@d 不参与通用格式的计算。
 */
int irq_domain_translate_onecell(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(fwspec->param_count < 1))
		return -EINVAL;
	*out_hwirq = fwspec->param[0];
	*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_onecell);

/**
 * irq_domain_translate_twocell() - Generic translate for direct two cell
 * bindings
 * @d:		Interrupt domain involved in the translation
 * @fwspec:	The firmware interrupt specifier to translate
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Device Tree IRQ specifier translation function which works with two cell
 * bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 */
/*
 * 翻译固件无关的双参数中断描述：param[0] 是 hwirq，param[1] 是 IRQ 类型。
 * 类型只保留 IRQ_TYPE_SENSE_MASK 中的电气感知位；参数不足返回 -EINVAL。
 * 尽管历史英文说明提到设备树，本接口接收通用 fwspec，也可供 ACPI/软件节点用。
 */
int irq_domain_translate_twocell(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(fwspec->param_count < 2))
		return -EINVAL;
	*out_hwirq = fwspec->param[0];
	*out_type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_twocell);

/**
 * irq_domain_translate_twothreecell() - Generic translate for direct two or three cell
 * bindings
 * @d:		Interrupt domain involved in the translation
 * @fwspec:	The firmware interrupt specifier to translate
 * @out_hwirq:	Pointer to storage for the hardware interrupt number
 * @out_type:	Pointer to storage for the interrupt type
 *
 * Firmware interrupt specifier translation function for two or three cell
 * specifications, where the parameter values map directly to the hardware
 * interrupt number and the type specifier.
 */
/*
 * 翻译通用双参数或三参数固件中断描述。
 * 双参数布局取 param[0]/param[1]，三参数布局跳过常用于控制器子类型的
 * param[0]，取 param[1]/param[2]；类型都只保留感知位。其他长度返回 -EINVAL。
 */
int irq_domain_translate_twothreecell(struct irq_domain *d, struct irq_fwspec *fwspec,
				      unsigned long *out_hwirq, unsigned int *out_type)
{
	if (fwspec->param_count == 2) {
		*out_hwirq = fwspec->param[0];
		*out_type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
		return 0;
	}

	if (fwspec->param_count == 3) {
		*out_hwirq = fwspec->param[1];
		*out_type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
		return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_twothreecell);

/*
 * 为 domain 映射分配连续 irq_desc 区间。
 * virq >= 0 要求从指定位置精确分配；负值表示动态选择，此时用 hwirq 对当前
 * IRQ 空间取模作为局部性提示，并避开保留的 IRQ 0。提示位置失败后再从 1 开始
 * 回退搜索。返回正的起始 virq，失败返回底层负错误码或 0。
 */
int irq_domain_alloc_descs(int virq, unsigned int cnt, irq_hw_number_t hwirq,
			   int node, const struct irq_affinity_desc *affinity)
{
	unsigned int hint;

	if (virq >= 0) {
		virq = __irq_alloc_descs(virq, virq, cnt, node, THIS_MODULE,
					 affinity);
	} else {
		hint = hwirq % irq_get_nr_irqs();
		if (hint == 0)
			hint++;
		virq = __irq_alloc_descs(-1, hint, cnt, node, THIS_MODULE,
					 affinity);
		if (virq <= 0 && hint > 1) {
			virq = __irq_alloc_descs(-1, 1, cnt, node, THIS_MODULE,
						 affinity);
		}
	}

	return virq;
}

/**
 * irq_domain_reset_irq_data - Clear hwirq, chip and chip_data in @irq_data
 * @irq_data:	The pointer to irq_data
 */
/*
 * 把一层 irq_data 的硬件绑定恢复为未配置状态。
 * hwirq 归零、chip 改为 no_irq_chip、chip_data 清空；irq、common、domain 和父链
 * 不在这里修改，便于层级分配失败路径按各自职责回滚。
 */
void irq_domain_reset_irq_data(struct irq_data *irq_data)
{
	irq_data->hwirq = 0;
	irq_data->chip = &no_irq_chip;
	irq_data->chip_data = NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_reset_irq_data);

#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
/*
 * 发布一个层级 IRQ 的全部反向映射。
 * 调用者持有共享 root mutex；从最外层 irq_data 沿 parent_data 遍历，为每个
 * domain 增加 mapcount 并发布 hwirq 索引。所有层级可查后才清除 IRQ_NOREQUEST，
 * 防止用户在父层映射尚未就绪时申请 IRQ。
 */
static void irq_domain_insert_irq(int virq)
{
	struct irq_data *data;

	for (data = irq_get_irq_data(virq); data; data = data->parent_data) {
		struct irq_domain *domain = data->domain;

		domain->mapcount++;
		irq_domain_set_mapping(domain, data->hwirq, data);
	}

	irq_clear_status_flags(virq, IRQ_NOREQUEST);
}

/*
 * 撤销一个层级 IRQ 在所有 domain 中的可见性。
 * 先禁止新申请并移除最外层 chip/handler，等待在途中断结束；内存屏障确保这些
 * 停用动作先于各层 mapcount 递减和 revmap 清除。调用者持有共享 root mutex，
 * 本函数只撤销发布，不释放 irq_data 链或 irq_desc。
 */
static void irq_domain_remove_irq(int virq)
{
	struct irq_data *data;

	irq_set_status_flags(virq, IRQ_NOREQUEST);
	irq_set_chip_and_handler(virq, NULL, NULL);
	synchronize_irq(virq);
	smp_mb();

	for (data = irq_get_irq_data(virq); data; data = data->parent_data) {
		struct irq_domain *domain = data->domain;
		irq_hw_number_t hwirq = data->hwirq;

		domain->mapcount--;
		irq_domain_clear_mapping(domain, hwirq);
	}
}

/*
 * 在 @child 后面分配并接入一层父 domain 的 irq_data。
 * 新节点与子节点共享 Linux IRQ 号和 irq_common_data，但拥有独立的 domain、
 * hwirq、chip 与 chip_data。对象按 IRQ 所在 NUMA 节点分配；成功后由整条层级
 * 链拥有，失败返回 NULL 且不修改 child->parent_data。
 */
static struct irq_data *irq_domain_insert_irq_data(struct irq_domain *domain,
						   struct irq_data *child)
{
	struct irq_data *irq_data;

	irq_data = kzalloc_node(sizeof(*irq_data), GFP_KERNEL,
				irq_data_get_node(child));
	if (irq_data) {
		child->parent_data = irq_data;
		irq_data->irq = child->irq;
		irq_data->common = child->common;
		irq_data->domain = domain;
	}

	return irq_data;
}

/*
 * 从给定节点开始释放动态分配的 irq_data 父链。
 * 最外层嵌在 irq_desc 中，绝不能传入这里；调用者必须先把链从仍存活的子节点
 * 断开，并保证映射、控制器资源及并发读者都已撤销。本函数只释放内存。
 */
static void __irq_domain_free_hierarchy(struct irq_data *irq_data)
{
	struct irq_data *tmp;

	while (irq_data) {
		tmp = irq_data;
		irq_data = irq_data->parent_data;
		kfree(tmp);
	}
}

/*
 * 释放一段 virq 的动态父级 irq_data 链，并重置最外层归属。
 * 对每个嵌入 irq_desc 的首节点，先摘下 parent_data、清空 domain，再释放父链；
 * irq_desc 本体由更外层分配/释放路径管理。调用前必须已撤销所有 revmap。
 */
static void irq_domain_free_irq_data(unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data, *tmp;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_get_irq_data(virq + i);
		tmp = irq_data->parent_data;
		irq_data->parent_data = NULL;
		irq_data->domain = NULL;

		__irq_domain_free_hierarchy(tmp);
	}
}

/**
 * irq_domain_disconnect_hierarchy - Mark the first unused level of a hierarchy
 * @domain:	IRQ domain from which the hierarchy is to be disconnected
 * @virq:	IRQ number where the hierarchy is to be trimmed
 *
 * Marks the @virq level belonging to @domain as disconnected.
 * Returns -EINVAL if @virq doesn't have a valid irq_data pointing
 * to @domain.
 *
 * Its only use is to be able to trim levels of hierarchy that do not
 * have any real meaning for this interrupt, and that the driver marks
 * as such from its .alloc() callback.
 */
/*
 * 把 @virq 在 @domain 对应层标记为“与实际硬件不连接”。
 * 该标记仅供 domain 的 alloc 回调声明某些内层层级对当前 IRQ 没有真实意义，
 * 后续 irq_domain_trim_hierarchy() 会验证并裁掉它们。找不到对应 irq_data 返回
 * -EINVAL；成功把 chip 设置为 ERR_PTR(-ENOTCONN)，不立即释放任何节点。
 */
int irq_domain_disconnect_hierarchy(struct irq_domain *domain,
				    unsigned int virq)
{
	struct irq_data *irqd;

	irqd = irq_domain_get_irq_data(domain, virq);
	if (!irqd)
		return -EINVAL;

	irqd->chip = ERR_PTR(-ENOTCONN);
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_disconnect_hierarchy);

/*
 * 验证并裁剪由 -ENOTCONN 标记的无意义内层 irq_data 尾链。
 * 最外层必须有有效 irqchip；标记前各层必须有 chip，标记后不得重新出现有效
 * chip，且唯一合法错误指针是 -ENOTCONN。这样能区分驱动的明确裁剪请求与分配
 * 回调留下的半初始化/错误链。
 *
 * 无标记直接成功；有标记则在最后一个有效节点处断链并释放其后的动态节点。
 * 此时映射尚未发布，所以不需要撤销 revmap，但调用者必须仍持有分配事务的
 * root mutex，防止链被并发观察或修改。
 */
static int irq_domain_trim_hierarchy(unsigned int virq)
{
	struct irq_data *tail, *irqd, *irq_data;

	irq_data = irq_get_irq_data(virq);
	tail = NULL;

	/* The first entry must have a valid irqchip */
	/* 嵌在 irq_desc 中的最外层必须对应实际可处理中断的 irqchip。 */
	if (IS_ERR_OR_NULL(irq_data->chip))
		return -EINVAL;

	/*
	 * Validate that the irq_data chain is sane in the presence of
	 * a hierarchy trimming marker.
	 */
	/* 遇到裁剪标记时验证整条 irq_data 链仍具有单调、可解释的层级结构。 */
	for (irqd = irq_data->parent_data; irqd; irq_data = irqd, irqd = irqd->parent_data) {
		/* Can't have a valid irqchip after a trim marker */
		/* 标记之后不能再次出现有效 irqchip，否则无法确定该裁掉哪一段。 */
		if (irqd->chip && tail)
			return -EINVAL;

		/* Can't have an empty irqchip before a trim marker */
		/* 标记之前也不能留下未初始化的空 irqchip。 */
		if (!irqd->chip && !tail)
			return -EINVAL;

		if (IS_ERR(irqd->chip)) {
			/* Only -ENOTCONN is a valid trim marker */
			/* 仅 disconnect_hierarchy() 写入的 -ENOTCONN 可作为裁剪标记。 */
			if (PTR_ERR(irqd->chip) != -ENOTCONN)
				return -EINVAL;

			tail = irq_data;
		}
	}

	/* No trim marker, nothing to do */
	/* 驱动未请求裁剪，保留完整层级。 */
	if (!tail)
		return 0;

	pr_info("IRQ%d: trimming hierarchy from %s\n",
		virq, tail->parent_data->domain->name);

	/* Sever the inner part of the hierarchy...  */
	/* 在最后一个有效层之后断链，并释放更内侧的无意义层级。 */
	irqd = tail;
	tail = tail->parent_data;
	irqd->parent_data = NULL;
	__irq_domain_free_hierarchy(tail);

	return 0;
}

/*
 * 为一段 virq 构造与 @domain 层级同形的 irq_data 链。
 * 每个 irq_desc 已内嵌最外层 irq_data，先把它归属给目标 domain；随后沿 parent
 * domain 向内逐层分配节点。任一分配失败会清理当前及此前 virq 的全部父链，并
 * 清空最外层 domain，保证调用者看不到部分构造成功的区间。
 */
static int irq_domain_alloc_irq_data(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	struct irq_domain *parent;
	int i;

	/* The outermost irq_data is embedded in struct irq_desc */
	/* 最外层 irq_data 属于 irq_desc 本体，只有其父级节点需要动态分配。 */
	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_get_irq_data(virq + i);
		irq_data->domain = domain;

		for (parent = domain->parent; parent; parent = parent->parent) {
			irq_data = irq_domain_insert_irq_data(parent, irq_data);
			if (!irq_data) {
				irq_domain_free_irq_data(virq, i + 1);
				return -ENOMEM;
			}
		}
	}

	return 0;
}

/**
 * irq_domain_get_irq_data - Get irq_data associated with @virq and @domain
 * @domain:	domain to match
 * @virq:	IRQ number to get irq_data
 */
/*
 * 在 @virq 的 irq_data 父链中查找属于 @domain 的那一层。
 * 找到返回借用指针，否则返回 NULL。函数不加锁；调用者必须位于层级仍稳定的
 * 分配/释放事务中，或依赖 IRQ 生命周期保证父链不会并发改变。
 */
struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
					 unsigned int virq)
{
	struct irq_data *irq_data;

	for (irq_data = irq_get_irq_data(virq); irq_data;
	     irq_data = irq_data->parent_data)
		if (irq_data->domain == domain)
			return irq_data;

	return NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_get_irq_data);

/**
 * irq_domain_set_hwirq_and_chip - Set hwirq and irqchip of @virq at @domain
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number
 * @hwirq:	The hwirq number
 * @chip:	The associated interrupt chip
 * @chip_data:	The associated chip data
 */
/*
 * 设置 @virq 在指定 @domain 层的 hwirq、irq_chip 和控制器私有数据。
 * 找不到对应层返回 -ENOENT。NULL chip 被规范化为 no_irq_chip，避免后续核心
 * 路径解引用空指针；同时通知 /proc IRQ 展示逻辑更新已见过的 chip 名称。
 * 此函数通常由 domain alloc 回调在映射发布前调用。
 */
int irq_domain_set_hwirq_and_chip(struct irq_domain *domain, unsigned int virq,
				  irq_hw_number_t hwirq,
				  const struct irq_chip *chip,
				  void *chip_data)
{
	struct irq_data *irq_data = irq_domain_get_irq_data(domain, virq);

	if (!irq_data)
		return -ENOENT;

	irq_data->hwirq = hwirq;
	irq_data->chip = (struct irq_chip *)(chip ? chip : &no_irq_chip);
	irq_data->chip_data = chip_data;

	irq_proc_update_chip(chip);
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_set_hwirq_and_chip);

/**
 * irq_domain_set_info - Set the complete data for a @virq in @domain
 * @domain:		Interrupt domain to match
 * @virq:		IRQ number
 * @hwirq:		The hardware interrupt number
 * @chip:		The associated interrupt chip
 * @chip_data:		The associated interrupt chip data
 * @handler:		The interrupt flow handler
 * @handler_data:	The interrupt flow handler data
 * @handler_name:	The interrupt handler name
 */
/*
 * 一次设置 domain 层硬件身份，以及 irq_desc 最外层的流控处理信息。
 * 先调用 irq_domain_set_hwirq_and_chip() 写入 hwirq/chip/chip_data，再安装 handler
 * 和 handler_data。该辅助接口返回 void，因此调用者必须确保 @domain/@virq
 * 对应层已经存在；否则第一步失败不会阻止后两步修改描述符。
 */
void irq_domain_set_info(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq, const struct irq_chip *chip,
			 void *chip_data, irq_flow_handler_t handler,
			 void *handler_data, const char *handler_name)
{
	irq_domain_set_hwirq_and_chip(domain, virq, hwirq, chip, chip_data);
	__irq_set_handler(virq, handler, 0, handler_name);
	irq_set_handler_data(virq, handler_data);
}
EXPORT_SYMBOL(irq_domain_set_info);

/**
 * irq_domain_free_irqs_common - Clear irq_data and free the parent
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number to start with
 * @nr_irqs:	The number of irqs to free
 */
/*
 * 供 domain free 回调复用的通用回收：重置本层 irq_data，再递归释放父 domain。
 * 对不存在于本 domain 的 virq 跳过本层重置，但仍把整个区间传给父级回收；
 * 实际对象内存和 revmap 由更外层公共释放流程处理。
 */
void irq_domain_free_irqs_common(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_domain_get_irq_data(domain, virq + i);
		if (irq_data)
			irq_domain_reset_irq_data(irq_data);
	}
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_common);

/**
 * irq_domain_free_irqs_top - Clear handler and handler data, clear irqdata and free parent
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number to start with
 * @nr_irqs:	The number of irqs to free
 */
/*
 * 回收层级最外层 domain 时使用的完整通用辅助函数。
 * 先清空每个 irq_desc 的 handler_data 和流控 handler，防止残留回调引用驱动
 * 数据；再重置本层 irq_data 并递归释放父级硬件资源。revmap 与描述符由调用它的
 * 公共释放事务稍后处理。
 */
void irq_domain_free_irqs_top(struct irq_domain *domain, unsigned int virq,
			      unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_set_handler_data(virq + i, NULL);
		irq_set_handler(virq + i, NULL);
	}
	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_top);

/*
 * 调用层级 domain 的 free 回调释放指定区间硬件资源。
 * 没有 free 回调即无工作；为避免把被裁剪掉的层级交给驱动，只对仍能在该
 * domain 找到 irq_data 的 virq 逐个调用，并固定 nr_irqs=1。回调应自行递归父级。
 */
static void irq_domain_free_irqs_hierarchy(struct irq_domain *domain,
					   unsigned int irq_base,
					   unsigned int nr_irqs)
{
	unsigned int i;

	if (!domain->ops->free)
		return;

	for (i = 0; i < nr_irqs; i++) {
		if (irq_domain_get_irq_data(domain, irq_base + i))
			domain->ops->free(domain, irq_base + i, 1);
	}
}

/*
 * 调用目标层级 domain 的 alloc 回调分配控制器资源。
 * 缺少 alloc 表示该 domain 不支持层级分配，返回 -ENOSYS；参数 @arg 的结构和
 * 父级递归方式完全由具体 domain 协议定义。
 */
static int irq_domain_alloc_irqs_hierarchy(struct irq_domain *domain, unsigned int irq_base,
					   unsigned int nr_irqs, void *arg)
{
	if (!domain->ops->alloc) {
		pr_debug("domain->ops->alloc() is NULL\n");
		return -ENOSYS;
	}

	return domain->ops->alloc(domain, irq_base, nr_irqs, arg);
}

/*
 * 在共享 root mutex 下执行层级 IRQ 分配事务。
 *
 * 阶段依次为：取得或复用连续 irq_desc；构造每个 IRQ 的完整 irq_data 父链；
 * 调用最外层 alloc 回调（驱动可递归分配父级硬件资源）；验证并裁剪无意义层级；
 * 最后为每一层发布 revmap 并开放 IRQ_NOREQUEST。只有最后一步后映射才对外可用。
 *
 * 失败路径在尚未发布 revmap 时释放 irq_data 和新描述符。注意 alloc 回调若已
 * 获得硬件资源，按 domain API 契约应在返回错误前自行回滚；裁剪验证失败同样
 * 依赖驱动分配协议保持可释放状态。realloc=true 时复用旧描述符，不重新分配。
 */
static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity)
{
	int i, ret, virq;

	if (realloc && irq_base >= 0) {
		virq = irq_base;
	} else {
		virq = irq_domain_alloc_descs(irq_base, nr_irqs, 0, node,
					      affinity);
		if (virq < 0) {
			pr_debug("cannot allocate IRQ(base %d, count %d)\n",
				 irq_base, nr_irqs);
			return virq;
		}
	}

	if (irq_domain_alloc_irq_data(domain, virq, nr_irqs)) {
		pr_debug("cannot allocate memory for IRQ%d\n", virq);
		ret = -ENOMEM;
		goto out_free_desc;
	}

	ret = irq_domain_alloc_irqs_hierarchy(domain, virq, nr_irqs, arg);
	if (ret < 0)
		goto out_free_irq_data;

	for (i = 0; i < nr_irqs; i++) {
		ret = irq_domain_trim_hierarchy(virq + i);
		if (ret)
			goto out_free_irq_data;
	}

	for (i = 0; i < nr_irqs; i++)
		irq_domain_insert_irq(virq + i);

	return virq;

out_free_irq_data:
	irq_domain_free_irq_data(virq, nr_irqs);
out_free_desc:
	irq_free_descs(virq, nr_irqs);
	return ret;
}

/**
 * __irq_domain_alloc_irqs - Allocate IRQs from domain
 * @domain:	domain to allocate from
 * @irq_base:	allocate specified IRQ number if irq_base >= 0
 * @nr_irqs:	number of IRQs to allocate
 * @node:	NUMA node id for memory allocation
 * @arg:	domain specific argument
 * @realloc:	IRQ descriptors have already been allocated if true
 * @affinity:	Optional irq affinity mask for multiqueue devices
 *
 * Allocate IRQ numbers and initialized all data structures to support
 * hierarchy IRQ domains.
 * Parameter @realloc is mainly to support legacy IRQs.
 * Returns error code or allocated IRQ number
 *
 * The whole process to setup an IRQ has been split into two steps.
 * The first step, __irq_domain_alloc_irqs(), is to allocate IRQ
 * descriptor and required hardware resources. The second step,
 * irq_domain_activate_irq(), is to program the hardware with preallocated
 * resources. In this way, it's easier to rollback when failing to
 * allocate resources.
 */
/*
 * 从层级 @domain 分配 IRQ 号、irq_desc、irq_data 链和所需硬件资源。
 * @irq_base 非负时请求固定 IRQ；@realloc 表示描述符已经存在，主要兼容旧 IRQ；
 * @node 和 @affinity 控制描述符的 NUMA/亲和性初始布局，@arg 交给 domain 回调。
 * 成功返回起始 virq，失败返回负错误码。
 *
 * IRQ 建立被刻意拆成两步：本函数只预留编号和硬件资源；之后由
 * irq_domain_activate_irq() 真正编程控制器。分配失败因此可在硬件尚未启用时
 * 完整回滚。公共入口在整个事务外持有 root mutex，并允许 NULL domain 回退到
 * 默认 domain；涉及睡眠锁和内存分配，不能在原子/中断上下文调用。
 */
int __irq_domain_alloc_irqs(struct irq_domain *domain, int irq_base,
			    unsigned int nr_irqs, int node, void *arg,
			    bool realloc, const struct irq_affinity_desc *affinity)
{
	int ret;

	if (domain == NULL) {
		domain = irq_default_domain;
		if (WARN(!domain, "domain is NULL; cannot allocate IRQ\n"))
			return -EINVAL;
	}

	mutex_lock(&domain->root->mutex);
	ret = irq_domain_alloc_irqs_locked(domain, irq_base, nr_irqs, node, arg,
					   realloc, affinity);
	mutex_unlock(&domain->root->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(__irq_domain_alloc_irqs);

/* The irq_data was moved, fix the revmap to refer to the new location */
/*
 * irq_data 被复制到新地址后，修正其 domain 的反向映射指针。
 * 调用者持有 root mutex；nomap 无索引可修。线性表用 RCU 发布新地址，radix
 * tree 则定位既有 slot 原位替换，保持 hwirq 键和 mapcount 不变。
 */
static void irq_domain_fix_revmap(struct irq_data *d)
{
	void __rcu **slot;

	lockdep_assert_held(&d->domain->root->mutex);

	if (irq_domain_is_nomap(d->domain))
		return;

	/* Fix up the revmap. */
	/* 保留原映射键，只把值从旧 irq_data 地址更新为新地址。 */
	if (d->hwirq < d->domain->revmap_size) {
		/* Not using radix tree */
		/* 线性 revmap 区间不使用 radix tree。 */
		rcu_assign_pointer(d->domain->revmap[d->hwirq], d);
	} else {
		slot = radix_tree_lookup_slot(&d->domain->revmap_tree, d->hwirq);
		if (slot)
			radix_tree_replace_slot(&d->domain->revmap_tree, slot, d);
	}
}

/**
 * irq_domain_push_irq() - Push a domain in to the top of a hierarchy.
 * @domain:	Domain to push.
 * @virq:	Irq to push the domain in to.
 * @arg:	Passed to the irq_domain_ops alloc() function.
 *
 * For an already existing irqdomain hierarchy, as might be obtained
 * via a call to pci_enable_msix(), add an additional domain to the
 * head of the processing chain.  Must be called before request_irq()
 * has been called.
 */
/*
 * 在已有 IRQ domain 层级最外端压入一个新 domain。
 * 常见于 pci_enable_msix() 已产生基础层级后再叠加中断重映射/隔离层。@domain
 * 的 parent 必须正好等于当前最外层 domain，且必须在 request_irq() 之前调用。
 *
 * 实现把 irq_desc 内嵌的原 irq_data 完整复制到新分配父节点，再把内嵌位置改成
 * 新 domain 的空白层，调用其 alloc 填入 hwirq/chip。成功后先修正旧 domain
 * revmap 指向复制后的地址，再发布新 domain 映射。alloc 失败则从副本恢复原
 * irq_data 并释放副本，不改变原层级。
 *
 * desc->action 检查只用于捕获明显误用，并非无竞争证明；若为了严格无竞争而跨
 * alloc 回调持有描述符相关锁，可能与驱动锁序死锁。因此 API 依赖调用者在 IRQ
 * 尚未注册处理函数的生命周期阶段保证稳定。
 */
int irq_domain_push_irq(struct irq_domain *domain, int virq, void *arg)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);
	struct irq_data *parent_irq_data;
	struct irq_desc *desc;
	int rv = 0;

	/*
	 * Check that no action has been set, which indicates the virq
	 * is in a state where this function doesn't have to deal with
	 * races between interrupt handling and maintaining the
	 * hierarchy.  This will catch gross misuse.  Attempting to
	 * make the check race free would require holding locks across
	 * calls to struct irq_domain_ops->alloc(), which could lead
	 * to deadlock, so we just do a simple check before starting.
	 */
	/*
	 * action 为空说明 virq 尚未进入需要同时处理中断执行与层级维护竞争的状态。
	 * 该检查可发现明显误用，但并非无竞争；跨 alloc 回调持锁可能死锁，所以这里只
	 * 在开始前检查一次，真正的生命周期排他性由调用者保证。
	 */
	desc = irq_to_desc(virq);
	if (!desc)
		return -EINVAL;
	if (WARN_ON(desc->action))
		return -EBUSY;

	if (domain == NULL)
		return -EINVAL;

	if (WARN_ON(!irq_domain_is_hierarchy(domain)))
		return -EINVAL;

	if (!irq_data)
		return -EINVAL;

	if (domain->parent != irq_data->domain)
		return -EINVAL;

	parent_irq_data = kzalloc_node(sizeof(*parent_irq_data), GFP_KERNEL,
				       irq_data_get_node(irq_data));
	if (!parent_irq_data)
		return -ENOMEM;

	mutex_lock(&domain->root->mutex);

	/* Copy the original irq_data. */
	/* 把内嵌位置的原最外层完整复制成新 domain 的父节点。 */
	*parent_irq_data = *irq_data;

	/*
	 * Overwrite the irq_data, which is embedded in struct irq_desc, with
	 * values for this domain.
	 */
	/*
	 * 重写 irq_desc 内嵌 irq_data，使它代表新压入 domain；复制出的原数据成为
	 * parent_data，随后由新 domain 的 alloc 回调补齐硬件字段。
	 */
	irq_data->parent_data = parent_irq_data;
	irq_data->domain = domain;
	irq_data->mask = 0;
	irq_data->hwirq = 0;
	irq_data->chip = NULL;
	irq_data->chip_data = NULL;

	/* May (probably does) set hwirq, chip, etc. */
	/* 新 domain 的 alloc 回调通常会设置 hwirq、chip 和 chip_data。 */
	rv = irq_domain_alloc_irqs_hierarchy(domain, virq, 1, arg);
	if (rv) {
		/* Restore the original irq_data. */
		/* 分配失败时从副本原样恢复内嵌 irq_data。 */
		*irq_data = *parent_irq_data;
		kfree(parent_irq_data);
		goto error;
	}

	irq_domain_fix_revmap(parent_irq_data);
	irq_domain_set_mapping(domain, irq_data->hwirq, irq_data);
error:
	mutex_unlock(&domain->root->mutex);

	return rv;
}
EXPORT_SYMBOL_GPL(irq_domain_push_irq);

/**
 * irq_domain_pop_irq() - Remove a domain from the top of a hierarchy.
 * @domain:	Domain to remove.
 * @virq:	Irq to remove the domain from.
 *
 * Undo the effects of a call to irq_domain_push_irq().  Must be
 * called either before request_irq() or after free_irq().
 */
/*
 * 撤销 irq_domain_push_irq()，移除层级最外端的 @domain。
 * 只能在 request_irq() 前或 free_irq() 后调用；指定 domain 必须就是当前首层，
 * 且必须存在保存原状态的 parent_data。
 *
 * root mutex 下先清除新 domain revmap 并调用其 free 回调，再把父节点内容复制回
 * irq_desc 内嵌位置；随后修正恢复后 domain 的 revmap，解锁后释放临时父节点。
 * 与 push 相同，action 检查只捕获明显误用，生命周期排他性由调用者保证。
 */
int irq_domain_pop_irq(struct irq_domain *domain, int virq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);
	struct irq_data *parent_irq_data;
	struct irq_data *tmp_irq_data;
	struct irq_desc *desc;

	/*
	 * Check that no action is set, which indicates the virq is in
	 * a state where this function doesn't have to deal with races
	 * between interrupt handling and maintaining the hierarchy.
	 * This will catch gross misuse.  Attempting to make the check
	 * race free would require holding locks across calls to
	 * struct irq_domain_ops->free(), which could lead to
	 * deadlock, so we just do a simple check before starting.
	 */
	/*
	 * action 为空意味着无需与活跃中断处理并发维护层级。跨 free 回调持额外锁可能
	 * 死锁，因此这里只做开始前的误用检查，不把它当作完整同步机制。
	 */
	desc = irq_to_desc(virq);
	if (!desc)
		return -EINVAL;
	if (WARN_ON(desc->action))
		return -EBUSY;

	if (domain == NULL)
		return -EINVAL;

	if (!irq_data)
		return -EINVAL;

	tmp_irq_data = irq_domain_get_irq_data(domain, virq);

	/* We can only "pop" if this domain is at the top of the list */
	/* 只有位于 irq_data 链首的 domain 才能被弹出。 */
	if (WARN_ON(irq_data != tmp_irq_data))
		return -EINVAL;

	if (WARN_ON(irq_data->domain != domain))
		return -EINVAL;

	parent_irq_data = irq_data->parent_data;
	if (WARN_ON(!parent_irq_data))
		return -EINVAL;

	mutex_lock(&domain->root->mutex);

	irq_data->parent_data = NULL;

	irq_domain_clear_mapping(domain, irq_data->hwirq);
	irq_domain_free_irqs_hierarchy(domain, virq, 1);

	/* Restore the original irq_data. */
	/* 把压入前保存的原最外层数据恢复到 irq_desc 内嵌位置。 */
	*irq_data = *parent_irq_data;

	irq_domain_fix_revmap(irq_data);

	mutex_unlock(&domain->root->mutex);

	kfree(parent_irq_data);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_pop_irq);

/**
 * irq_domain_free_irqs - Free IRQ number and associated data structures
 * @virq:	base IRQ number
 * @nr_irqs:	number of IRQs to free
 */
/*
 * 释放一段层级 IRQ 的编号及全部关联数据结构。
 * 首项必须存在 domain 且最外层提供 free 回调。root mutex 下逐项撤销 revmap 和
 * 可申请状态，再让驱动递归释放硬件资源；解锁后销毁动态 irq_data 父链和连续
 * irq_desc。调用者必须已停用并释放中断处理函数，区间也必须属于同一 domain。
 */
void irq_domain_free_irqs(unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_get_irq_data(virq);
	struct irq_domain *domain;
	int i;

	if (WARN(!data || !data->domain || !data->domain->ops->free,
		 "NULL pointer, cannot free irq\n"))
		return;

	domain = data->domain;

	mutex_lock(&domain->root->mutex);
	for (i = 0; i < nr_irqs; i++)
		irq_domain_remove_irq(virq + i);
	irq_domain_free_irqs_hierarchy(domain, virq, nr_irqs);
	mutex_unlock(&domain->root->mutex);

	irq_domain_free_irq_data(virq, nr_irqs);
	irq_free_descs(virq, nr_irqs);
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs);

/*
 * 释放由固件映射入口创建的单个层级 IRQ。
 * MSI device domain 的有线中断具有专用账本和锁序，必须走 MSI 释放入口；其他
 * domain 使用通用层级释放。@domain 用于选择协议，virq 的实际归属仍由路径校验。
 */
static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq)
{
	if (irq_domain_is_msi_device(domain))
		msi_device_domain_free_wired(domain, virq);
	else
		irq_domain_free_irqs(virq, 1);
}

/**
 * irq_domain_alloc_irqs_parent - Allocate interrupts from parent domain
 * @domain:	Domain below which interrupts must be allocated
 * @irq_base:	Base IRQ number
 * @nr_irqs:	Number of IRQs to allocate
 * @arg:	Allocation data (arch/domain specific)
 */
/*
 * 供 domain alloc 回调向直接父 domain 请求同一段 IRQ 资源。
 * 没有父级返回 -ENOSYS；否则把参数原样交给父级 alloc，父级可继续递归。调用时
 * 公共分配事务已持有共享 root mutex，回调不得破坏该层级的锁顺序。
 */
int irq_domain_alloc_irqs_parent(struct irq_domain *domain,
				 unsigned int irq_base, unsigned int nr_irqs,
				 void *arg)
{
	if (!domain->parent)
		return -ENOSYS;

	return irq_domain_alloc_irqs_hierarchy(domain->parent, irq_base,
					       nr_irqs, arg);
}
EXPORT_SYMBOL_GPL(irq_domain_alloc_irqs_parent);

/**
 * irq_domain_free_irqs_parent - Free interrupts from parent domain
 * @domain:	Domain below which interrupts must be freed
 * @irq_base:	Base IRQ number
 * @nr_irqs:	Number of IRQs to free
 */
/*
 * 供 domain free 回调对称释放直接父 domain 的资源。
 * 根 domain 没有父级时为空操作；其余情况只调用父级层级释放回调，irq_data
 * 内存和描述符仍由最外层公共事务统一销毁。
 */
void irq_domain_free_irqs_parent(struct irq_domain *domain,
				 unsigned int irq_base, unsigned int nr_irqs)
{
	if (!domain->parent)
		return;

	irq_domain_free_irqs_hierarchy(domain->parent, irq_base, nr_irqs);
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_parent);

/*
 * 从最外层向内递归停用 irq_data 链对应的控制器。
 * 每层先执行自身 deactivate，再处理 parent_data，顺序与激活相反；这样先阻止
 * 外层继续转发，再逐步关闭靠近硬件的资源。缺失回调或空节点均安全跳过。
 */
static void __irq_domain_deactivate_irq(struct irq_data *irq_data)
{
	if (irq_data && irq_data->domain) {
		struct irq_domain *domain = irq_data->domain;

		if (domain->ops->deactivate)
			domain->ops->deactivate(domain, irq_data);
		if (irq_data->parent_data)
			__irq_domain_deactivate_irq(irq_data->parent_data);
	}
}

/*
 * 从最内层向外递归激活 irq_data 链对应的控制器。
 * 先激活 parent_data，保证底层路由/向量已准备，再激活当前外层。任一外层回调
 * 失败会从其父节点开始按停用顺序回滚此前成功层级；错误码向上传递。
 * @reserve 为真时回调只预留向量而不完成最终分配，具体语义由控制器实现。
 */
static int __irq_domain_activate_irq(struct irq_data *irqd, bool reserve)
{
	int ret = 0;

	if (irqd && irqd->domain) {
		struct irq_domain *domain = irqd->domain;

		if (irqd->parent_data)
			ret = __irq_domain_activate_irq(irqd->parent_data,
							reserve);
		if (!ret && domain->ops->activate) {
			ret = domain->ops->activate(domain, irqd, reserve);
			/* Rollback in case of error */
			/* 当前层激活失败时，撤销此前已经成功激活的所有父层。 */
			if (ret && irqd->parent_data)
				__irq_domain_deactivate_irq(irqd->parent_data);
		}
	}
	return ret;
}

/**
 * irq_domain_activate_irq - Call domain_ops->activate recursively to activate
 *			     interrupt
 * @irq_data:	Outermost irq_data associated with interrupt
 * @reserve:	If set only reserve an interrupt vector instead of assigning one
 *
 * This is the second step to call domain_ops->activate to program interrupt
 * controllers, so the interrupt could actually get delivered.
 */
/*
 * 递归调用各层 domain 的 activate 回调，使预分配 IRQ 真正可由硬件投递。
 * 这是两阶段建立协议的第二步：资源在 __irq_domain_alloc_irqs() 中预留，此处
 * 才编程控制器。已激活 irq_data 不重复执行；全部成功后设置 activated 状态位，
 * 失败保持未激活并返回错误。外部调用者负责与并发启停串行化。
 */
int irq_domain_activate_irq(struct irq_data *irq_data, bool reserve)
{
	int ret = 0;

	if (!irqd_is_activated(irq_data))
		ret = __irq_domain_activate_irq(irq_data, reserve);
	if (!ret)
		irqd_set_activated(irq_data);
	return ret;
}

/**
 * irq_domain_deactivate_irq - Call domain_ops->deactivate recursively to
 *			       deactivate interrupt
 * @irq_data: outermost irq_data associated with interrupt
 *
 * It calls domain_ops->deactivate to program interrupt controllers to disable
 * interrupt delivery.
 */
/*
 * 递归调用各层 domain 的 deactivate 回调，停止控制器投递该中断。
 * 仅 activated 状态置位时执行；停用完成后清除状态，使重复调用成为空操作。
 * 顺序从外向内，与激活相反，但不会释放预留资源，后续仍可再次激活。
 */
void irq_domain_deactivate_irq(struct irq_data *irq_data)
{
	if (irqd_is_activated(irq_data)) {
		__irq_domain_deactivate_irq(irq_data);
		irqd_clr_activated(irq_data);
	}
}

/*
 * 根据操作表识别层级 domain。
 * alloc 回调是层级分配协议的必需入口，因此其存在即设置 HIERARCHY 标志；该
 * 判断发生在 domain 创建阶段、发布之前，不需要并发同步。
 */
static void irq_domain_check_hierarchy(struct irq_domain *domain)
{
	/* Hierarchy irq_domains must implement callback alloc() */
	/* 层级 irq_domain 必须实现 alloc 回调。 */
	if (domain->ops->alloc)
		domain->flags |= IRQ_DOMAIN_FLAG_HIERARCHY;
}
#else	/* CONFIG_IRQ_DOMAIN_HIERARCHY */
/*
 * irq_domain_get_irq_data - Get irq_data associated with @virq and @domain
 * @domain:	domain to match
 * @virq:	IRQ number to get irq_data
 */
/*
 * 非层级配置下获取 @virq 唯一 irq_data，并核对其 domain。
 * 没有 parent_data 链可遍历；返回借用指针或 NULL，生命周期由 irq_desc 保证。
 */
struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
					 unsigned int virq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);

	return (irq_data && irq_data->domain == domain) ? irq_data : NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_get_irq_data);

/*
 * irq_domain_set_info - Set the complete data for a @virq in @domain
 * @domain:		Interrupt domain to match
 * @virq:		IRQ number
 * @hwirq:		The hardware interrupt number
 * @chip:		The associated interrupt chip
 * @chip_data:		The associated interrupt chip data
 * @handler:		The interrupt flow handler
 * @handler_data:	The interrupt flow handler data
 * @handler_name:	The interrupt handler name
 */
/*
 * 非层级配置下一次设置 virq 的 chip、流控 handler 及两类私有数据。
 * 此配置没有逐层 irq_data，@domain/@hwirq 不参与实现；调用者应在映射建立流程
 * 中保证它们与 irq_desc 的归属一致。
 */
void irq_domain_set_info(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq, const struct irq_chip *chip,
			 void *chip_data, irq_flow_handler_t handler,
			 void *handler_data, const char *handler_name)
{
	irq_set_chip_and_handler_name(virq, chip, handler, handler_name);
	irq_set_chip_data(virq, chip_data);
	irq_set_handler_data(virq, handler_data);
}

/* 层级支持关闭时不能执行层级分配；保留同名内部接口并统一返回 -EINVAL。 */
static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity)
{
	return -EINVAL;
}

/* 非层级构建无需识别或设置 HIERARCHY 标志。 */
static void irq_domain_check_hierarchy(struct irq_domain *domain) { }
/* 非层级构建不会由层级固件映射路径调用单 IRQ 释放辅助函数。 */
static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq) { }

#endif	/* CONFIG_IRQ_DOMAIN_HIERARCHY */

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
#include "debugfs.h"

/* debugfs 中“domains”目录；仅在 IRQ debugfs 初始化完成后非空。 */
static struct dentry *domain_dir;

/* 把 irq_domain->flags 的已知位映射为可读名称，供统一位图展示辅助函数使用。 */
static const struct irq_bit_descr irqdomain_flags[] = {
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_HIERARCHY),
	BIT_MASK_DESCR(IRQ_DOMAIN_NAME_ALLOCATED),
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_IPI_PER_CPU),
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_IPI_SINGLE),
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_MSI),
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_ISOLATED_MSI),
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_NO_MAP),
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_MSI_PARENT),
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_MSI_DEVICE),
	BIT_MASK_DESCR(IRQ_DOMAIN_FLAG_NONCORE),
};

/*
 * 把一个 domain 的名称、revmap 容量、已映射数和标志输出到 seq_file。
 * domain 可追加控制器私有调试内容；层级配置下再递归打印 parent，并增加缩进
 * 展示从外层到内层的拓扑。调用期间 domain 必须保持存活。
 */
static void irq_domain_debug_show_one(struct seq_file *m, struct irq_domain *d, int ind)
{
	seq_printf(m, "%*sname:   %s\n", ind, "", d->name);
	seq_printf(m, "%*ssize:   %u\n", ind + 1, "", d->revmap_size);
	seq_printf(m, "%*smapped: %u\n", ind + 1, "", d->mapcount);
	seq_printf(m, "%*sflags:  0x%08x\n", ind +1 , "", d->flags);
	irq_debug_show_bits(m, ind, d->flags, irqdomain_flags, ARRAY_SIZE(irqdomain_flags));
	if (d->ops && d->ops->debug_show)
		d->ops->debug_show(m, d, NULL, ind + 1);
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
	if (!d->parent)
		return;
	seq_printf(m, "%*sparent: %s\n", ind + 1, "", d->parent->name);
	irq_domain_debug_show_one(m, d->parent, ind + 4);
#endif
}

/*
 * debugfs 单文件的 show 入口。
 * 普通 domain 文件把 dentry 私有数据作为起点；“default”文件私有数据为 NULL，
 * 此时动态读取 irq_default_domain，未设置则输出空文件。
 */
static int irq_domain_debug_show(struct seq_file *m, void *p)
{
	struct irq_domain *d = m->private;

	/* Default domain? Might be NULL */
	/* 私有 domain 为空表示正在读取 default 文件；默认 domain 本身也可能为空。 */
	if (!d) {
		if (!irq_default_domain)
			return 0;
		d = irq_default_domain;
	}
	irq_domain_debug_show_one(m, d, 0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(irq_domain_debug);

/*
 * 为已命名 domain 在 debugfs/domains 下创建只读状态文件。
 * debugfs 尚未初始化或 domain 无名称时跳过；文件私有数据借用 domain 指针，
 * domain 移除路径必须先删除文件再释放对象。
 */
static void debugfs_add_domain_dir(struct irq_domain *d)
{
	if (!d->name || !domain_dir)
		return;
	debugfs_create_file(d->name, 0444, domain_dir, d,
			    &irq_domain_debug_fops);
}

/* 按 domain 名称查找并递归删除对应 debugfs 项，解除其私有指针引用。 */
static void debugfs_remove_domain_dir(struct irq_domain *d)
{
	debugfs_lookup_and_remove(d->name, domain_dir);
}

/*
 * 初始化 IRQ domain 的 debugfs 视图。
 * 创建 domains 目录及动态“default”文件，再在 irq_domain_mutex 下为初始化前
 * 已发布的所有 domain 补建文件；之后发布的新 domain 会在发布事务中自行添加。
 * __init 表明本入口只在 IRQ debugfs 初始化阶段调用一次。
 */
void __init irq_domain_debugfs_init(struct dentry *root)
{
	struct irq_domain *d;

	domain_dir = debugfs_create_dir("domains", root);

	debugfs_create_file("default", 0444, domain_dir, NULL,
			    &irq_domain_debug_fops);
	mutex_lock(&irq_domain_mutex);
	list_for_each_entry(d, &irq_domain_list, link)
		debugfs_add_domain_dir(d);
	mutex_unlock(&irq_domain_mutex);
}
#endif
