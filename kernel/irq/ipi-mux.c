// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Multiplex several virtual IPIs over a single HW IPI.
 *
 * Copyright The Asahi Linux Contributors
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#define pr_fmt(fmt) "ipi-mux: " fmt
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <linux/smp.h>

/*
 * 本文件在一个“向指定 CPU 触发父硬件 IPI”的回调之上复用多个虚拟 IPI。每个
 * CPU 用 bits 记录待处理虚拟编号，用 enable 记录允许分派的编号；发送只需置位，
 * 从 0->1 且当前 enabled 时才触发一次父 IPI，重复发送自然合并。
 *
 * send 与 unmask 分别执行“写 pending -> 读 enable”和“写 enable -> 读 pending”，
 * 两侧屏障消除双方都漏发父 IPI 的窗口。接收端原子领取 enabled pending，保留
 * masked 位供以后 unmask 重触发。domain/per-CPU 状态是一次创建、永久存活的全局
 * 单例；快速路径不取全局锁，依赖创建完成后才向外发布 virq。
 */

/*
 * 单个 CPU 的虚拟 IPI 状态。
 * @enable：本 CPU 已解屏蔽的虚拟 IPI 位图，只由本 CPU 的 mask/unmask 修改。
 * @bits：发送者可并发置位、本 CPU 接收路径领取的 pending 位图。
 * 两者用 atomic_t 是为了跨 CPU 原子读改写和下面显式内存序，不转移所有权。
 */
struct ipi_mux_cpu {
	atomic_t			enable;
	atomic_t			bits;
};

/* 所有 CPU 的 mux 位图，创建成功后永久分配。 */
static struct ipi_mux_cpu __percpu *ipi_mux_pcpu;
/* 虚拟 IPI 的线性 irq_domain，也是“已创建”发布标志。 */
static struct irq_domain *ipi_mux_domain;
/* provider 提供的父硬件 IPI 触发回调，创建成功后只读。 */
static void (*ipi_mux_send)(unsigned int cpu);

/*
 * 在当前 CPU 屏蔽虚拟 IPI @d。
 * irqchip 回调上下文保证操作的是本 CPU per-CPU 状态；原子清 enable 位，不删除
 * 已 pending 的 bits，因此屏蔽期间到达的事件会保留到以后 unmask。无返回值。
 */
static void ipi_mux_mask(struct irq_data *d)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);

	atomic_andnot(BIT(irqd_to_hwirq(d)), &icpu->enable);
}

/*
 * 在当前 CPU 解屏蔽虚拟 IPI @d，并补发可能被屏蔽滞留的事件。
 * 先原子设置 enable 位，再用完整屏障约束随后读取 bits；若对应 pending 已存在，
 * 立即向当前 CPU 触发父 IPI。它与发送方“先置 bits、屏障、再读 enable”的镜像
 * 序列配对，保证竞态时至少发送方看到 enabled，或本函数看到 pending，不会漏发。
 */
static void ipi_mux_unmask(struct irq_data *d)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);
	u32 ibit = BIT(irqd_to_hwirq(d));

	atomic_or(ibit, &icpu->enable);

	/*
	 * The atomic_or() above must complete before the atomic_read()
	 * below to avoid racing ipi_mux_send_mask().
	 */
	/* enable 置位必须先于 pending 检查，避免与发送方互相读到旧值。 */
	smp_mb__after_atomic();

	/* If a pending IPI was unmasked, raise a parent IPI immediately. */
	/* 若屏蔽期间已有 pending，立刻补触发当前 CPU 的父 IPI。 */
	if (atomic_read(&icpu->bits) & ibit)
		ipi_mux_send(smp_processor_id());
}

/*
 * 把虚拟 IPI @d 发送给 @mask 中的每个 CPU。
 * 对每个目标原子 OR pending 位并取得旧值；release 语义把调用者在发送前的共享
 * 内存写发布到接收 CPU，接收端 fetch_andnot 与之配对。随后完整屏障再读 enable，
 * 只有该位原先未 pending 且当前 enabled 时触发父 IPI：已有 pending 说明已有一次
 * 父通知足以合并处理，masked 则留给未来 unmask 补发。
 *
 * 本函数可被多 CPU 并发调用；所有共享状态均为目标 CPU 的原子位图。provider
 * 回调无错误返回，本函数也不报告失败，调用者保证 @mask 和 hwirq 合法。
 */
static void ipi_mux_send_mask(struct irq_data *d, const struct cpumask *mask)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);
	u32 ibit = BIT(irqd_to_hwirq(d));
	unsigned long pending;
	int cpu;

	for_each_cpu(cpu, mask) {
		icpu = per_cpu_ptr(ipi_mux_pcpu, cpu);

		/*
		 * This sequence is the mirror of the one in ipi_mux_unmask();
		 * see the comment there. Additionally, release semantics
		 * ensure that the vIPI flag set is ordered after any shared
		 * memory accesses that precede it. This therefore also pairs
		 * with the atomic_fetch_andnot in ipi_mux_process().
		 */
		/*
		 * 这是 unmask 序列的镜像；release 还保证 pending 位发布在更早共享写之后，
		 * 供接收端领取时建立消息数据的先行关系。
		 */
		pending = atomic_fetch_or_release(ibit, &icpu->bits);

		/*
		 * The atomic_fetch_or_release() above must complete
		 * before the atomic_read() below to avoid racing with
		 * ipi_mux_unmask().
		 */
		/* pending 置位必须先于 enable 检查，避免双方都漏看对方的新状态。 */
		smp_mb__after_atomic();

		/*
		 * The flag writes must complete before the physical IPI is
		 * issued to another CPU. This is implied by the control
		 * dependency on the result of atomic_read() below, which is
		 * itself already ordered after the vIPI flag write.
		 */
		/* 控制依赖保证决定发送父 IPI 前，目标 pending 位已经对外可见。 */
		if (!(pending & ibit) && (atomic_read(&icpu->enable) & ibit))
			ipi_mux_send(cpu);
	}
}

/* 虚拟 IPI irqchip：本地 mask 状态和跨 CPU mask 发送都落到 per-CPU 原子位图协议。 */
static const struct irq_chip ipi_mux_chip = {
	.name		= "IPI Mux",
	.irq_mask	= ipi_mux_mask,
	.irq_unmask	= ipi_mux_unmask,
	.ipi_send_mask	= ipi_mux_send_mask,
};

/*
 * 为线性 mux domain 分配的一段 virq 初始化 per-CPU irq_data。
 * 每项 hwirq 使用区间内索引 i，设置 per-CPU devid，并绑定 ipi_mux_chip 与
 * handle_percpu_devid_irq；没有 chip_data/handler_data 所有权。成功始终返回 0，
 * 调用者持 irqdomain root mutex，失败清理由 irqdomain 外层负责。
 */
static int ipi_mux_domain_alloc(struct irq_domain *d, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_set_percpu_devid(virq + i);
		irq_domain_set_info(d, virq + i, i, &ipi_mux_chip, NULL,
				    handle_percpu_devid_irq, NULL, NULL);
	}

	return 0;
}

/* mux domain 的分配初始化由本文件完成，释放复用 irqdomain 顶层通用清理。 */
static const struct irq_domain_ops ipi_mux_domain_ops = {
	.alloc		= ipi_mux_domain_alloc,
	.free		= irq_domain_free_irqs_top,
};

/**
 * ipi_mux_process - Process multiplexed virtual IPIs
 */
/*
 * 在父硬件 IPI 的当前 CPU 中断处理程序里领取并分派虚拟 IPI。
 * enable 只由本 CPU mask/unmask 修改，硬中断上下文不会与本 CPU 的修改并发，因此
 * 无需额外排序读取。atomic_fetch_andnot(en) 只清除当前 enabled 的 pending 位，
 * masked 位继续留在 bits；该原子领取与发送方 release OR 配对，确保先前共享数据
 * 在调用虚拟 handler 前可见。随后按位调用 generic_handle_domain_irq()。
 *
 * 多个同类发送在位图中合并为一次分派；handler 若触发新发送，会留下下一轮 pending。
 * 函数无返回值，要求创建已完成且调用者确实位于父 IPI hardirq 上下文。
 */
void ipi_mux_process(void)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);
	irq_hw_number_t hwirq;
	unsigned long ipis;
	unsigned int en;

	/*
	 * Reading enable mask does not need to be ordered as long as
	 * this function is called from interrupt handler because only
	 * the CPU itself can change it's own enable mask.
	 */
	/* 只有当前 CPU 能改 enable，父 IPI hardirq 内可直接取得一致快照。 */
	en = atomic_read(&icpu->enable);

	/*
	 * Clear the IPIs we are about to handle. This pairs with the
	 * atomic_fetch_or_release() in ipi_mux_send_mask().
	 */
	/* 原子领取 enabled pending；masked bits 不清除，等待以后 unmask。 */
	ipis = atomic_fetch_andnot(en, &icpu->bits) & en;

	/* 每个置位 hwirq 映射到创建时同索引的虚拟 virq 并进入 per-CPU flow handler。 */
	for_each_set_bit(hwirq, &ipis, BITS_PER_TYPE(int))
		generic_handle_domain_irq(ipi_mux_domain, hwirq);
}

/**
 * ipi_mux_create - Create virtual IPIs multiplexed on top of a single
 * parent IPI.
 * @nr_ipi:		number of virtual IPIs to create. This should
 *			be <= BITS_PER_TYPE(int)
 * @mux_send:		callback to trigger parent IPI for a particular CPU
 *
 * Returns first virq of the newly created virtual IPIs upon success
 * or <=0 upon failure
 */
/*
 * 创建一个永久的 IPI mux 单例，在父 IPI 之上提供 @nr_ipi 个虚拟 IPI。
 * @nr_ipi 不得超过 atomic_t 位宽，@mux_send 必须能向指定 CPU 触发父 IPI；已有
 * 全局 domain 时返回 -EEXIST，参数错误 -EINVAL，内存/对象创建失败 -ENOMEM。
 *
 * 阶段依次为：分配全零 per-CPU 位图；创建命名 fwnode；创建线性 domain；标记
 * IPI_SINGLE 并设置 IPI bus token；从 domain 分配全部 virq。全部成功后才先发布
 * ipi_mux_domain、再保存 send 回调，并返回首 virq。调用时序保证返回前没有快速路径
 * 使用这两个全局对象；成功后没有 destroy API，它们永久存活。
 *
 * 失败按 domain -> fwnode -> per-CPU 内存逆序清理。domain 分配 IRQ 失败时其内部
 * 已回滚 IRQ 资源，本层只移除 domain；每个标签只释放已取得资源并返回原 rc。
 */
int ipi_mux_create(unsigned int nr_ipi, void (*mux_send)(unsigned int cpu))
{
	struct fwnode_handle *fwnode;
	struct irq_domain *domain;
	int rc;

	/* domain 同时充当全局单例的已发布标志。 */
	if (ipi_mux_domain)
		return -EEXIST;

	if (BITS_PER_TYPE(int) < nr_ipi || !mux_send)
		return -EINVAL;

	/* 阶段一：为每个 possible CPU 准备 enable/pending 原子位图。 */
	ipi_mux_pcpu = alloc_percpu(typeof(*ipi_mux_pcpu));
	if (!ipi_mux_pcpu)
		return -ENOMEM;

	/* 阶段二：命名节点只为软件 domain 提供稳定身份和可读名称。 */
	fwnode = irq_domain_alloc_named_fwnode("IPI-Mux");
	if (!fwnode) {
		pr_err("unable to create IPI Mux fwnode\n");
		rc = -ENOMEM;
		goto fail_free_cpu;
	}

	/* 阶段三：建立尚未全局发布给 mux 快路径的线性 domain。 */
	domain = irq_domain_create_linear(fwnode, nr_ipi,
					  &ipi_mux_domain_ops, NULL);
	if (!domain) {
		pr_err("unable to add IPI Mux domain\n");
		rc = -ENOMEM;
		goto fail_free_fwnode;
	}

	domain->flags |= IRQ_DOMAIN_FLAG_IPI_SINGLE;
	irq_domain_update_bus_token(domain, DOMAIN_BUS_IPI);

	/* 阶段四：一次分配所有虚拟 virq，alloc 回调为每项绑定 per-CPU flow。 */
	rc = irq_domain_alloc_irqs(domain, nr_ipi, NUMA_NO_NODE, NULL);
	if (rc <= 0) {
		pr_err("unable to alloc IRQs from IPI Mux domain\n");
		goto fail_free_domain;
	}

	/* 所有资源就绪后提交单例；创建 API 的串行时序阻止半发布观察。 */
	ipi_mux_domain = domain;
	ipi_mux_send = mux_send;

	return rc;

fail_free_domain:
	/* IRQ 分配失败已由 irqdomain 内层回滚，此处销毁 domain 容器。 */
	irq_domain_remove(domain);
fail_free_fwnode:
	irq_domain_free_fwnode(fwnode);
fail_free_cpu:
	free_percpu(ipi_mux_pcpu);
	return rc;
}
