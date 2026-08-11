// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006 Thomas Gleixner
 *
 * This file contains driver APIs to the irq subsystem.
 */
/*
 * 本文件实现驱动面向 IRQ 子系统的管理 API。它覆盖 request/free、嵌套
 * disable/enable、亲和性、线程化 action、NMI 与 irqchip 状态查询；chip.c 负责硬件
 * flow，本文件负责把驱动请求转换为安全的 action/线程/引用生命周期。
 *
 * 主要所有权链：request_*() 分配 irqaction 并取得 module/PM/resource 所有权，
 * __setup_irq() 在 request_mutex+desc->lock 协议下发布到 action 链；free_*() 先摘链，
 * 再等待 hardirq、irq_work 和线程全部退出，最后逆序释放资源。亲和性通知另用 kref
 * 保护异步 work，不能与 irqaction 或 desc 生命周期混为一谈。
 */

/* 本文件所有 pr_* 日志统一添加 genirq: 前缀，便于定位通用 IRQ 管理层。 */
#define pr_fmt(fmt) "genirq: " fmt

#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/sched/task.h>
#include <linux/sched/isolation.h>
#include <uapi/linux/sched/types.h>
#include <linux/task_work.h>

#include "internals.h"

#if defined(CONFIG_IRQ_FORCED_THREADING) && !defined(CONFIG_PREEMPT_RT)
/*
 * force_irqthreads_key 是默认关闭的 jump label；threadirqs 启动参数启用后，允许线程化
 * 的普通 primary handler 会被改造成 IRQ kthread 执行。PREEMPT_RT 有自身线程化模型，
 * 因而不构建此独立开关。
 */
DEFINE_STATIC_KEY_FALSE(force_irqthreads_key);

/*
 * setup_forced_irqthreads() - 响应 threadirqs 早期启动参数
 *
 * @arg: early_param 提供但未消费的可空参数字符串；该选项不需要值。
 * 启用静态键并返回 0 表示解析成功。仅启动期单线程调用，不分配内存、不需锁；状态在
 * init 后保持只读，后续 irq_setup_forced_threading() 据此改写新 action。
 */
static int __init setup_forced_irqthreads(char *arg)
{
	static_branch_enable(&force_irqthreads_key);
	return 0;
}
/* 注册无值 early parameter：threadirqs。 */
early_param("threadirqs", setup_forced_irqthreads);
#endif
/* 非 forced-threading 构建或 PREEMPT_RT 下，force_irqthreads() 由配置相关实现决定。 */

#ifdef CONFIG_SMP
/*
 * synchronize_irqwork() - 等待该 IRQ 已排队或正在执行的跨 CPU redirect irq_work
 *
 * @desc: 输入输出的非 NULL、存活描述符借用指针。
 * irq_work_sync() 同步 desc->redirect.work，返回时没有旧重定向回调再进入该 desc。
 * 无返回值；可忙等，调用者不能持 work 完成所需的锁，通常用于可阻塞的释放路径。
 */
static inline void synchronize_irqwork(struct irq_desc *desc)
{
	/* Synchronize pending or on the fly redirect work */
	/* 同步尚在 pending 或已经飞行中的 redirect work。 */
	irq_work_sync(&desc->redirect.work);
}
#else
/*
 * synchronize_irqwork() - UP 配置下的 redirect work 同步空实现
 *
 * @desc: 未消费的描述符借用指针。UP 没有跨 CPU 重定向 work，故无返回值、无副作用。
 */
static inline void synchronize_irqwork(struct irq_desc *desc) { }
#endif

/* __irq_get_irqchip_state() 前置声明，供同步路径可选查询硬件 ACTIVE 状态。 */
static int __irq_get_irqchip_state(struct irq_data *d, enum irqchip_irq_state which, bool *state);

/*
 * __synchronize_hardirq() - 等待描述符的软件执行窗口及可选硬件 in-flight 状态结束
 *
 * @desc: 非 NULL、存活描述符；调用者不得持 handler 完成所需资源。
 * @sync_chip: true 时在软件 INPROGRESS 清零后进一步查询 IRQCHIP_STATE_ACTIVE。
 *
 * 函数不可睡眠但会忙等：先无锁观察 INPROGRESS，再以 desc->lock 复核；若请求且 chip
 * 支持 state 查询，还把硬件 active 写回 inprogress 并循环。返回：无。它不等待
 * threaded handler，且 sync_chip=false 可谨慎从 IRQ 上下文使用。
 */
static void __synchronize_hardirq(struct irq_desc *desc, bool sync_chip)
{
	/* irqd 是内嵌状态借用；inprogress 是每轮锁内确认的软件/硬件活动快照。 */
	struct irq_data *irqd = irq_desc_get_irq_data(desc);
	bool inprogress;

	do {
		/*
		 * Wait until we're out of the critical section.  This might
		 * give the wrong answer due to the lack of memory barriers.
		 */
		/*
		 * 先等软件临界区结束；由于这段无锁且没有内存屏障，观察值可能错误，
		 * 所以它只是降低加锁频率，不能作为最终完成证明。
		 */
		while (irqd_irq_inprogress(&desc->irq_data))
			cpu_relax();

		/* Ok, that indicated we're done: double-check carefully. */
		/* 无锁观察显示结束后，必须在 desc 锁下谨慎复核。 */
		guard(raw_spinlock_irqsave)(&desc->lock);
		inprogress = irqd_irq_inprogress(&desc->irq_data);

		/*
		 * If requested and supported, check at the chip whether it
		 * is in flight at the hardware level, i.e. already pending
		 * in a CPU and waiting for service and acknowledge.
		 */
		/*
		 * 若调用者要求且软件已空闲，再询问 chip 中断是否已到达某 CPU、仍在
		 * 等待服务/ack。这样关闭路径可覆盖尚未进入 flow handler 的硬件事件。
		 */
		if (!inprogress && sync_chip) {
			/*
			 * Ignore the return code. inprogress is only updated
			 * when the chip supports it.
			 */
			/*
			 * 忽略查询 errno；只有层级中存在支持该状态的 chip 时，helper 才会
			 * 更新 inprogress。缺失能力保留 false，按软件同步完成处理。
			 */
			__irq_get_irqchip_state(irqd, IRQCHIP_STATE_ACTIVE,
						&inprogress);
		}
		/* Oops, that failed? */
		/* 若锁内复核或硬件查询仍显示 active，就回到循环重新等待。 */
	} while (inprogress);
}

/**
 * synchronize_hardirq - wait for pending hard IRQ handlers (on other CPUs)
 * @irq: interrupt number to wait for
 *
 * This function waits for any pending hard IRQ handlers for this interrupt
 * to complete before returning. If you use this function while holding a
 * resource the IRQ handler may need you will deadlock. It does not take
 * associated threaded handlers into account.
 *
 * Do not use this for shutdown scenarios where you must be sure that all
 * parts (hardirq and threaded handler) have completed.
 *
 * Returns: false if a threaded handler is active.
 *
 * This function may be called - with care - from IRQ context.
 *
 * It does not check whether there is an interrupt in flight at the
 * hardware level, but not serviced yet, as this might deadlock when called
 * with interrupts disabled and the target CPU of the interrupt is the
 * current CPU.
 */
/*
 * 等待逻辑 IRQ @irq 在其他 CPU 上的 pending/running hardirq handler 完成；
 * 若调用者持有 handler 所需资源会死锁。它不等待关联的 threaded handler，因此不能
 * 用于必须确认全部硬件与线程部分退出的 shutdown。
 *
 * 找不到 desc 时返回 true。找到后只同步软件 INPROGRESS，不查询“已飞向当前 CPU 但尚未
 * 服务”的硬件状态，以避免本地中断关闭时自等死；最后返回 threads_active==0，false
 * 表示仍有线程活动。函数忙等、不睡眠，可在严格审查后从 IRQ 上下文调用。
 */
bool synchronize_hardirq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc) {
		__synchronize_hardirq(desc, false);
		return !atomic_read(&desc->threads_active);
	}

	return true;
}
EXPORT_SYMBOL(synchronize_hardirq);

/*
 * __synchronize_irq() - 完整等待 redirect、hardirq、硬件 active 与 IRQ 线程
 *
 * @desc: 非 NULL、存活描述符；调用者处于可调度上下文且不持 handler 所需资源。
 * 先同步 redirect irq_work，再以 sync_chip=true 等待硬件/软件 hardirq，最后睡眠等待
 * threads_active 归零。无返回值；每一阶段都在下一阶段前建立“不会再新增旧工作”的边界。
 */
static void __synchronize_irq(struct irq_desc *desc)
{
	synchronize_irqwork(desc);
	__synchronize_hardirq(desc, true);

	/*
	 * We made sure that no hardirq handler is running. Now verify that no
	 * threaded handlers are active.
	 */
	/* hardirq 已确认退出，现在还必须验证所有 threaded handler 均不活跃。 */
	wait_event(desc->wait_for_threads, !atomic_read(&desc->threads_active));
}

/**
 * synchronize_irq - wait for pending IRQ handlers (on other CPUs)
 * @irq: interrupt number to wait for
 *
 * This function waits for any pending IRQ handlers for this interrupt to
 * complete before returning. If you use this function while holding a
 * resource the IRQ handler may need you will deadlock.
 *
 * Can only be called from preemptible code as it might sleep when
 * an interrupt thread is associated to @irq.
 *
 * It optionally makes sure (when the irq chip supports that method)
 * that the interrupt is not pending in any CPU and waiting for
 * service.
 */
/*
 * 等待 @irq 的全部 pending handler 完成；持有 handler 所需资源会死锁。
 * 因 threaded handler 可能使 wait_event 睡眠，只能从可抢占进程上下文调用。若 chip
 * 支持，还确认没有事件已 pending 到 CPU、等待服务。
 *
 * 无返回值；IRQ 不存在时为空操作。函数只建立执行完成屏障，不禁用线路、不摘 action，
 * 所以调用者通常先阻止新事件或在 free/disable 协议内使用。
 */
void synchronize_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc)
		__synchronize_irq(desc);
}
EXPORT_SYMBOL(synchronize_irq);

#ifdef CONFIG_SMP
/*
 * irq_default_affinity 是新 IRQ 未获专用策略时采用的全局 CPU 目标掩码。irqdesc.c 在
 * 启动期分配/初始化，proc 接口可更新内容；每个 desc 建立时复制位图，不借用其指针。
 * cpumask_var_t 的存储在系统寿命内有效。
 */
cpumask_var_t irq_default_affinity;

/*
 * __irq_can_set_affinity() - 检查描述符是否具备通用 CPU 亲和性设置能力
 *
 * @desc: 可空、只读借用描述符。
 * desc 存在、状态允许 balancing、chip 存在且实现 irq_set_affinity 时返回 true；否则
 * false。不检查 managed 用户权限、目标掩码或 CPU 在线性，不睡眠、无副作用。
 */
static bool __irq_can_set_affinity(struct irq_desc *desc)
{
	if (!desc || !irqd_can_balance(&desc->irq_data) ||
	    !desc->irq_data.chip || !desc->irq_data.chip->irq_set_affinity)
		return false;
	return true;
}

/**
 * irq_can_set_affinity - Check if the affinity of a given irq can be set
 * @irq:	Interrupt to check
 *
 */
/*
 * 检查逻辑 IRQ @irq 的 affinity 是否可设置。返回 1/0，直接复用内部能力
 * 判定；找不到 IRQ 也返回 0。函数不加锁，适合在已有描述符生命周期或配置稳定条件下
 * 做能力快照，结果不能保证下一次设置时 chip/state 未变化。
 */
int irq_can_set_affinity(unsigned int irq)
{
	return __irq_can_set_affinity(irq_to_desc(irq));
}

/**
 * irq_can_set_affinity_usr - Check if affinity of a irq can be set from user space
 * @irq:	Interrupt to check
 *
 * Like irq_can_set_affinity() above, but additionally checks for the
 * AFFINITY_MANAGED flag.
 */
/*
 * 与 irq_can_set_affinity() 相同，但额外拒绝 affinity-managed IRQ，防止
 * 用户空间覆盖内核为 MSI 等自动管理的目标。@irq 是逻辑号；返回 bool，不加锁、不睡眠。
 * 内核内部仍可通过受控路径调整 managed affinity。
 */
bool irq_can_set_affinity_usr(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return __irq_can_set_affinity(desc) &&
		!irqd_affinity_is_managed(&desc->irq_data);
}

/**
 * irq_set_thread_affinity - Notify irq threads to adjust affinity
 * @desc:	irq descriptor which has affinity changed
 *
 * Just set IRQTF_AFFINITY and delegate the affinity setting to the
 * interrupt thread itself. We can not call set_cpus_allowed_ptr() here as
 * we hold desc->lock and this code can be called from hard interrupt
 * context.
 */
/*
 * 亲和性改变后只给 IRQ 线程置 IRQTF_AFFINITY，并把任务唤醒，让线程自己
 * 调整 CPU 允许集合。当前持 desc->lock，且可能来自 hardirq，不能在这里直接调用可能
 * 获取调度器锁/睡眠的 set_cpus_allowed_ptr()。
 *
 * @desc 是非 NULL、锁内描述符。函数遍历稳定 action 链，同时处理 primary 对应线程和
 * forced-threading secondary 线程；仅有 thread 的节点被标记。无返回值，不等待迁移完成。
 */
static void irq_set_thread_affinity(struct irq_desc *desc)
{
	/* action 是 desc 锁保护下的链节点借用指针。 */
	struct irqaction *action;

	for_each_action_of_desc(desc, action) {
		if (action->thread) {
			set_bit(IRQTF_AFFINITY, &action->thread_flags);
			wake_up_process(action->thread);
		}
		if (action->secondary && action->secondary->thread) {
			set_bit(IRQTF_AFFINITY, &action->secondary->thread_flags);
			wake_up_process(action->secondary->thread);
		}
	}
}

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
/*
 * irq_validate_effective_affinity() - 诊断 chip 成功设置后未发布实际生效掩码
 *
 * @data: 只读非 NULL irq_data，chip 与 effective_affinity 存储已稳定。
 * 掩码非空直接返回；为空时按全系统只告警一次并打印 chip 名称/IRQ。无返回值、不修复
 * 掩码、不睡眠；它验证 irq_set_affinity 回调契约，而不把诊断失败转成设置 errno。
 */
static void irq_validate_effective_affinity(struct irq_data *data)
{
	/* m/chip 均为 irq_data/domain 生命周期内的只读借用。 */
	const struct cpumask *m = irq_data_get_effective_affinity_mask(data);
	struct irq_chip *chip = irq_data_get_irq_chip(data);

	if (!cpumask_empty(m))
		return;
	pr_warn_once("irq_chip %s did not update eff. affinity mask of irq %u\n",
		     chip->name, data->irq);
}
#else
/*
 * irq_validate_effective_affinity() - 未存储独立 effective mask 时的空实现
 *
 * @data: 未消费的 irq_data 借用指针。无返回值、无副作用，查询会回退请求 affinity。
 */
static inline void irq_validate_effective_affinity(struct irq_data *data) { }
#endif

/*
 * __tmp_mask 是 irq_do_set_affinity() 的每 CPU 临时 cpumask，避免亲和性热路径动态分配。
 * 调用者须处于不可迁移并串行当前 CPU 使用的上下文（通常 desc irqsave 锁）；返回前不
 * 泄露指针，多个 CPU 可各自并行使用自己的实例。
 */
static DEFINE_PER_CPU(struct cpumask, __tmp_mask);

/*
 * irq_do_set_affinity() - 选择可编程 CPU 掩码并直接调用当前 irq_chip
 *
 * @data: 输入输出的非 NULL irq_data，其 desc/chip/affinity 生命周期已稳定。
 * @mask: 只读非 NULL 请求掩码；成功后通常复制到 desc 的“请求 affinity”。
 * @force: true 时绕过在线 CPU 过滤，专供 CPU hotplug 等受控场景。
 *
 * 缺少 chip 回调或非 force 且没有在线目标返回 -EINVAL；否则原样处理 chip 返回：
 * IRQ_SET_MASK_OK/OK_DONE 复制原始请求 mask，OK_NOCOPY 信任 chip 已更新 common_data
 * affinity，三者
 * 都验证 effective mask、通知 IRQ 线程迁移并归一为 0；其他错误原样返回。函数不设置
 * IRQD_AFFINITY_SET，不排 pending work，调用者通常持 desc->lock 且不可迁移。
 */
int irq_do_set_affinity(struct irq_data *data, const struct cpumask *mask, bool force)
{
	/* tmp_mask 是当前 CPU 私有暂存；desc/chip/prog_mask 均为本次调用期借用。 */
	struct cpumask *tmp_mask = this_cpu_ptr(&__tmp_mask);
	struct irq_desc *desc = irq_data_to_desc(data);
	struct irq_chip *chip = irq_data_get_irq_chip(data);
	const struct cpumask  *prog_mask;
	int ret;

	if (!chip || !chip->irq_set_affinity)
		return -EINVAL;

	/*
	 * If this is a managed interrupt and housekeeping is enabled on
	 * it check whether the requested affinity mask intersects with
	 * a housekeeping CPU. If so, then remove the isolated CPUs from
	 * the mask and just keep the housekeeping CPU(s). This prevents
	 * the affinity setter from routing the interrupt to an isolated
	 * CPU to avoid that I/O submitted from a housekeeping CPU causes
	 * interrupts on an isolated one.
	 *
	 * If the masks do not intersect or include online CPU(s) then
	 * keep the requested mask. The isolated target CPUs are only
	 * receiving interrupts when the I/O operation was submitted
	 * directly from them.
	 *
	 * If all housekeeping CPUs in the affinity mask are offline, the
	 * interrupt will be migrated by the CPU hotplug code once a
	 * housekeeping CPU which belongs to the affinity mask comes
	 * online.
	 */
	/*
	 * managed IRQ 启用 housekeeping 隔离策略时，先求请求 mask 与
	 * housekeeping 集合的交集。如果其中有在线 housekeeping CPU，就去掉 isolated CPU，
	 * 避免 housekeeping CPU 提交的 I/O 把完成中断路由到隔离 CPU。
	 *
	 * 若两集合不相交，或交集中的 housekeeping CPU 全离线，则保留原始请求；这样隔离
	 * CPU 直接提交的 I/O 仍可在自身收中断。若当前所有合适 housekeeping CPU 离线，
	 * CPU hotplug 会在其中一颗上线时迁移 IRQ。tmp_mask 只作值运算，不改变 @mask。
	 */
	if (irqd_affinity_is_managed(data) &&
	    housekeeping_enabled(HK_TYPE_MANAGED_IRQ)) {
		const struct cpumask *hk_mask;

		hk_mask = housekeeping_cpumask(HK_TYPE_MANAGED_IRQ);

		cpumask_and(tmp_mask, mask, hk_mask);
		if (!cpumask_intersects(tmp_mask, cpu_online_mask))
			prog_mask = mask;
		else
			prog_mask = tmp_mask;
	} else {
		prog_mask = mask;
	}

	/*
	 * Make sure we only provide online CPUs to the irqchip,
	 * unless we are being asked to force the affinity (in which
	 * case we do as we are told).
	 */
	/*
	 * 通常只把 prog_mask 中在线 CPU 交给 irqchip；force 模式则完全尊重
	 * 调用者并传原始 @mask，即使目标尚未 online。非 force 的在线交集为空时拒绝。
	 */
	cpumask_and(tmp_mask, prog_mask, cpu_online_mask);
	if (!force && !cpumask_empty(tmp_mask))
		ret = chip->irq_set_affinity(data, tmp_mask, force);
	else if (force)
		ret = chip->irq_set_affinity(data, mask, force);
	else
		ret = -EINVAL;

	/*
	 * OK/OK_DONE 表示核心负责保存请求 mask；NOCOPY 表示 chip 已处理其所需副本。
	 * 三种成功都需要让 IRQ kthread 稍后更新调度 affinity。
	 */
	switch (ret) {
	case IRQ_SET_MASK_OK:
	case IRQ_SET_MASK_OK_DONE:
		cpumask_copy(desc->irq_common_data.affinity, mask);
		fallthrough;
	case IRQ_SET_MASK_OK_NOCOPY:
		irq_validate_effective_affinity(data);
		irq_set_thread_affinity(desc);
		ret = 0;
	}

	return ret;
}

#ifdef CONFIG_GENERIC_PENDING_IRQ
/*
 * irq_set_affinity_pending() - 保存暂时无法编程的亲和性请求
 *
 * @data: 输入输出的非 NULL irq_data，调用者持 desc->lock。
 * @dest: 只读非 NULL目标掩码，内容会复制到 desc->pending_mask。
 * 置 SETAFFINITY_PENDING、复制值并返回 0；不调用 chip、不通知线程，稍后的中断/迁移
 * 安全点负责应用。内部存储归 desc 所有，调用者返回后可复用 @dest。
 */
static inline int irq_set_affinity_pending(struct irq_data *data,
					   const struct cpumask *dest)
{
	struct irq_desc *desc = irq_data_to_desc(data);

	irqd_set_move_pending(data);
	irq_copy_pending(desc, dest);
	return 0;
}
#else
/*
 * irq_set_affinity_pending() - 无通用 pending 存储时拒绝延迟亲和性
 *
 * @data: 未消费的 irq_data 借用指针。
 * @dest: 未消费的目标掩码借用指针。
 * 返回 -EBUSY，表示底层忙状态不能转换为稍后执行；无副作用、不睡眠。
 */
static inline int irq_set_affinity_pending(struct irq_data *data,
					   const struct cpumask *dest)
{
	return -EBUSY;
}
#endif

/*
 * irq_try_set_affinity() - 尝试直接编程，必要时转为 generic pending 请求
 *
 * @data: 输入输出的非 NULL、锁内 irq_data。
 * @dest: 只读非 NULL 请求掩码。
 * @force: 是否强制设置离线目标。
 * 先返回 irq_do_set_affinity() 结果；只有 -EBUSY 且非 force 时调用 pending helper，配置
 * 支持时转成 0，关闭时仍为 -EBUSY。其他错误/成功原样返回。
 */
static int irq_try_set_affinity(struct irq_data *data,
				const struct cpumask *dest, bool force)
{
	int ret = irq_do_set_affinity(data, dest, force);

	/*
	 * In case that the underlying vector management is busy and the
	 * architecture supports the generic pending mechanism then utilize
	 * this to avoid returning an error to user space.
	 */
	/*
	 * 底层向量管理正忙且架构支持通用 pending 时，记录请求而不向用户空间
	 * 返回错误；force 调用要求即时语义，不能在此静默转换。
	 */
	if (ret == -EBUSY && !force)
		ret = irq_set_affinity_pending(data, dest);
	return ret;
}

/*
 * irq_set_affinity_deactivated() - 为“仅激活态可编程”的 chip 缓存 affinity
 *
 * @data: 输入输出的非 NULL irq_data。
 * @mask: 只读非 NULL 请求掩码。
 * 仅层级 domain、当前未 activated 且 IRQD_AFFINITY_ON_ACTIVATE 时处理：复制 requested
 * 与 effective affinity，置 AFFINITY_SET 并返回 true，完全不调用 chip。其他情况返回
 * false 且无副作用，让调用者走即时/延迟编程。调用者持 desc->lock，不睡眠。
 */
static bool irq_set_affinity_deactivated(struct irq_data *data,
					 const struct cpumask *mask)
{
	struct irq_desc *desc = irq_data_to_desc(data);

	/*
	 * Handle irq chips which can handle affinity only in activated
	 * state correctly
	 *
	 * If the interrupt is not yet activated, just store the affinity
	 * mask and do not call the chip driver at all. On activation the
	 * driver has to make sure anyway that the interrupt is in a
	 * usable state so startup works.
	 */
	/*
	 * 某些 irqchip 只有 activated 时才能设置 affinity。若尚未激活，就只保存
	 * mask，不调用驱动；后续 activation 本就必须把 IRQ 建到可 startup 的正确目标。
	 */
	if (!IS_ENABLED(CONFIG_IRQ_DOMAIN_HIERARCHY) ||
	    irqd_is_activated(data) || !irqd_affinity_on_activate(data))
		return false;

	cpumask_copy(desc->irq_common_data.affinity, mask);
	irq_data_update_effective_affinity(data, mask);
	irqd_set(data, IRQD_AFFINITY_SET);
	return true;
}

/**
 * irq_affinity_schedule_notify_work - Schedule work to notify about affinity change
 * @desc:  Interrupt descriptor whose affinity changed
 */
/*
 * 为 affinity 已改变的 @desc 调度通知 work。调用者必须持 desc->lock，且
 * desc->affinity_notify 非 NULL。先为本次调度取得 kref；schedule_work() 成功时该引用
 * 转交 work，若 work 已在 pending/running 而返回 false，就立即 release 多取的引用。
 * 无返回值、不睡眠，多个变化可合并为一次回调，通知读取的是执行时最新掩码。
 */
void irq_affinity_schedule_notify_work(struct irq_desc *desc)
{
	lockdep_assert_held(&desc->lock);

	kref_get(&desc->affinity_notify->kref);
	if (!schedule_work(&desc->affinity_notify->work)) {
		/* Work was already scheduled, drop our extra ref */
		/* work 已调度时没有新的执行者消费引用，故立刻归还本次额外 kref。 */
		kref_put(&desc->affinity_notify->kref, desc->affinity_notify->release);
	}
}

/*
 * irq_set_affinity_locked() - 在 desc 锁内落实或排队一次亲和性修改
 *
 * @data: 输入输出的非 NULL irq_data；调用者持对应 desc->lock。
 * @mask: 只读非 NULL 请求掩码。
 * @force: 是否绕过在线 CPU 检查。
 *
 * 缺 chip 回调返回 -EINVAL。未激活缓存路径直接返回 0；否则能在进程上下文移动且没有
 * 旧 pending 时尝试立即设置，不能立即移动则覆盖 pending mask。随后无论直接设置是否
 * 返回错误，都会调度已注册 notifier 并置 AFFINITY_SET；返回即时路径结果，排队路径为
 * 0。函数不睡眠，但 notifier work 异步运行。
 */
int irq_set_affinity_locked(struct irq_data *data, const struct cpumask *mask,
			    bool force)
{
	/* chip/desc 是同一 IRQ 生命周期内借用；ret 仅传递即时尝试的结果。 */
	struct irq_chip *chip = irq_data_get_irq_chip(data);
	struct irq_desc *desc = irq_data_to_desc(data);
	int ret = 0;

	if (!chip || !chip->irq_set_affinity)
		return -EINVAL;

	/* 阶段 1：未激活且只能在激活期编程时，只缓存请求，不触碰硬件。 */
	if (irq_set_affinity_deactivated(data, mask))
		return 0;

	/*
	 * 阶段 2：chip 允许当前上下文迁移且没有旧请求时立即尝试；否则仅覆盖
	 * desc 拥有的 pending mask，留给安全的屏蔽/迁移窗口提交。
	 */
	if (irq_can_move_pcntxt(data) && !irqd_is_setaffinity_pending(data)) {
		ret = irq_try_set_affinity(data, mask, force);
	} else {
		irqd_set_move_pending(data);
		irq_copy_pending(desc, mask);
	}

	/*
	 * 阶段 3：发布“调用者设置过 affinity”的软件状态，并异步通知观察者。
	 * notifier 合并事件且读取执行时的最新值，因此这里不把 @mask 借给 work。
	 */
	if (desc->affinity_notify)
		irq_affinity_schedule_notify_work(desc);

	irqd_set(data, IRQD_AFFINITY_SET);

	return ret;
}

/**
 * irq_update_affinity_desc - Update affinity management for an interrupt
 * @irq:	The interrupt number to update
 * @affinity:	Pointer to the affinity descriptor
 *
 * This interface can be used to configure the affinity management of
 * interrupts which have been allocated already.
 *
 * There are certain limitations on when it may be used - attempts to use it
 * for when the kernel is configured for generic IRQ reservation mode (in
 * config GENERIC_IRQ_RESERVATION_MODE) will fail, as it may conflict with
 * managed/non-managed interrupt accounting. In addition, attempts to use it on
 * an interrupt which is already started or which has already been configured
 * as managed will also fail, as these mean invalid init state or double init.
 */
/*
 * 为已经分配的 @irq 更新 affinity management 描述 @affinity。reservation
 * mode 可能与 managed/non-managed 计数冲突，返回 -EOPNOTSUPP；IRQ 已 STARTED 或已是
 * managed 表示初始化时机无效/重复，返回 -EBUSY。
 *
 * 函数在可睡眠配置上下文取得 bus/desc 锁。若此前 activated，先 deactivate 以撤销
 * 旧建立结果；按 is_managed 设置 MANAGED 与初始 SHUTDOWN，复制 mask，再恢复原激活态。
 * 成功返回 0，描述符不存在返回 -EINVAL。reactivate 返回值未传播，调用方应在启动前
 * 保证 domain 激活不会失败；@affinity 只读借用，内部保存位图副本。
 */
int irq_update_affinity_desc(unsigned int irq, struct irq_affinity_desc *affinity)
{
	/*
	 * Supporting this with the reservation scheme used by x86 needs
	 * some more thought. Fail it for now.
	 */
	/* x86 reservation scheme 下的正确协同尚需设计，当前明确拒绝。 */
	if (IS_ENABLED(CONFIG_GENERIC_IRQ_RESERVATION_MODE))
		return -EOPNOTSUPP;

	scoped_irqdesc_get_and_buslock(irq, 0) {
		struct irq_desc *desc = scoped_irqdesc;
		bool activated;

		/* Requires the interrupt to be shut down */
		/* 更新管理属性要求 IRQ 尚未 startup/当前已 shutdown。 */
		if (irqd_is_started(&desc->irq_data))
			return -EBUSY;

		/* Interrupts which are already managed cannot be modified */
		/* 已 managed 的 IRQ 不允许二次改变管理描述。 */
		if (irqd_affinity_is_managed(&desc->irq_data))
			return -EBUSY;
		/*
		 * Deactivate the interrupt. That's required to undo
		 * anything an earlier activation has established.
		 */
		/* 先 deactivate，撤销较早 activation 已建立的硬件/domain 状态。 */
		activated = irqd_is_activated(&desc->irq_data);
		if (activated)
			irq_domain_deactivate_irq(&desc->irq_data);

		if (affinity->is_managed) {
			irqd_set(&desc->irq_data, IRQD_AFFINITY_MANAGED);
			irqd_set(&desc->irq_data, IRQD_MANAGED_SHUTDOWN);
		}

		cpumask_copy(desc->irq_common_data.affinity, &affinity->mask);

		/* Restore the activation state */
		/* 更新完成后仅在入口原本 activated 时恢复同一生命周期状态。 */
		if (activated)
			irq_domain_activate_irq(&desc->irq_data, false);
		return 0;
	}
	return -EINVAL;
}

/*
 * __irq_set_affinity() - 按逻辑号查找并在 irqsave desc 锁下设置 affinity
 *
 * @irq: 目标逻辑 IRQ。
 * @mask: 只读非 NULL请求掩码。
 * @force: 是否允许离线目标。
 * desc 不存在返回 -EINVAL；否则返回 irq_set_affinity_locked() 结果。guard 精确恢复本地
 * 中断状态，函数不取得跨调用引用，IRQ 注册生命周期由调用者保证。
 */
static int __irq_set_affinity(unsigned int irq, const struct cpumask *mask,
			      bool force)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc)
		return -EINVAL;

	guard(raw_spinlock_irqsave)(&desc->lock);
	return irq_set_affinity_locked(irq_desc_get_irq_data(desc), mask, force);
}

/**
 * irq_set_affinity - Set the irq affinity of a given irq
 * @irq:	Interrupt to set affinity
 * @cpumask:	cpumask
 *
 * Fails if cpumask does not contain an online CPU
 */
/*
 * 把 @irq 的 affinity 设置为 @cpumask；若掩码不含在线 CPU 则失败。
 * 函数使用非 force 路径，返回 0、-EINVAL、-EBUSY 或 chip 错误；可能在底层忙时成功
 * 排为 pending。@cpumask 内容被复制或在回调内消费，不被长期持有。
 */
int irq_set_affinity(unsigned int irq, const struct cpumask *cpumask)
{
	return __irq_set_affinity(irq, cpumask, false);
}
EXPORT_SYMBOL_GPL(irq_set_affinity);

/**
 * irq_force_affinity - Force the irq affinity of a given irq
 * @irq:	Interrupt to set affinity
 * @cpumask:	cpumask
 *
 * Same as irq_set_affinity, but without checking the mask against
 * online cpus.
 *
 * Solely for low level cpu hotplug code, where we need to make per
 * cpu interrupts affine before the cpu becomes online.
 */
/*
 * 与 irq_set_affinity() 相同，但不以 cpu_online_mask 过滤目标；仅供低层 CPU
 * hotplug 在目标 CPU 上线前预设 per-CPU IRQ。@cpumask 必须由调用者保证合理；返回底层
 * 设置结果，普通驱动/用户路径不得用它绕过在线性检查。
 */
int irq_force_affinity(unsigned int irq, const struct cpumask *cpumask)
{
	return __irq_set_affinity(irq, cpumask, true);
}
EXPORT_SYMBOL_GPL(irq_force_affinity);

/*
 * __irq_apply_affinity_hint() - 发布用户可见 affinity hint，并可选实际应用
 *
 * @irq: 目标全局逻辑 IRQ。
 * @m: 可空、长期存活的只读 cpumask 指针；NULL 清除 hint。核心只保存指针、不复制。
 * @setaffinity: true 且 @m 非 NULL 时额外调用普通 affinity 设置。
 *
 * 描述符不存在/类型不符返回 -EINVAL；hint 发布成功返回 0。实际 __irq_set_affinity()
 * 的错误被忽略，故返回 0 只证明 hint 已保存，不证明硬件目标已改变。调用者必须在清除
 * hint 前保持 @m 存活，并自行处理“建议”与“实际 affinity”可能不同。
 */
int __irq_apply_affinity_hint(unsigned int irq, const struct cpumask *m, bool setaffinity)
{
	int ret = -EINVAL;

	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {
		scoped_irqdesc->affinity_hint = m;
		ret = 0;
	}

	if (!ret && m && setaffinity)
		__irq_set_affinity(irq, m, false);
	return ret;
}
EXPORT_SYMBOL_GPL(__irq_apply_affinity_hint);

/*
 * irq_affinity_notify() - 在 workqueue 进程上下文交付最新 affinity 快照
 *
 * @work: 嵌入 irq_affinity_notify 的 work；调度时已为本次执行持有一份 kref。
 * 函数还原 notify、查 desc，并分配临时 cpumask。若描述符消失或分配失败，跳过回调；
 * 否则在 desc 锁下优先复制 pending 目标，没有 pending 才复制当前 requested affinity，
 * 出锁后调用用户 notify()，再释放临时存储。所有出口都 put work 引用，可能触发 release。
 * 回调可睡眠，但不得假设 IRQ/desc 在回调后仍存在；传入 cpumask 只在回调期间有效。
 */
static void irq_affinity_notify(struct work_struct *work)
{
	/* notify 由 kref 保活；desc 是按 irq 查得的借用；cpumask 是本次 work owned 临时存储。 */
	struct irq_affinity_notify *notify = container_of(work, struct irq_affinity_notify, work);
	struct irq_desc *desc = irq_to_desc(notify->irq);
	cpumask_var_t cpumask;

	/* 阶段 1：为跨越 desc 锁和外部回调边界建立本次 work 私有的值快照。 */
	if (!desc || !alloc_cpumask_var(&cpumask, GFP_KERNEL))
		goto out;

	scoped_guard(raw_spinlock_irqsave, &desc->lock) {
		if (irq_move_pending(&desc->irq_data))
			irq_get_pending(cpumask, desc);
		else
			cpumask_copy(cpumask, desc->irq_common_data.affinity);
	}

	/* 阶段 2：锁外交付快照，允许用户回调睡眠，也避免回调反向取得 desc 锁。 */
	notify->notify(notify, cpumask);

	free_cpumask_var(cpumask);
out:
	/*
	 * 阶段 3：无论是否取得快照都归还调度时的 work 引用；这可能成为最后
	 * 一份引用并在当前 workqueue 上调用 release()。
	 */
	kref_put(&notify->kref, notify->release);
}

/**
 * irq_set_affinity_notifier - control notification of IRQ affinity changes
 * @irq:	Interrupt for which to enable/disable notification
 * @notify:	Context for notification, or %NULL to disable
 *		notification.  Function pointers must be initialised;
 *		the other fields will be initialised by this function.
 *
 * Must be called in process context.  Notification may only be enabled
 * after the IRQ is allocated and must be disabled before the IRQ is freed
 * using free_irq().
 */
/*
 * 控制 @irq 的 affinity change 通知。@notify 非 NULL 表示启用，调用者已填写
 * notify/release 函数，其余字段由本函数初始化；NULL 表示禁用。只能在进程上下文调用，
 * 必须在 IRQ 分配后启用，并在 free_irq() 前禁用。
 *
 * IRQ 不存在或为 NMI 返回 -EINVAL。安装时初始化 irq、base kref 和 work，再在 desc 锁
 * 下原子替换指针。旧 notifier 若存在，cancel_work_sync() 等待运行回调；取消 pending
 * work 时归还其 work 引用，随后再归还安装持有的 base 引用。最终 release 可能在本函数
 * 或 work 上下文执行；调用者不得提前释放对象。成功返回 0。
 */
int irq_set_affinity_notifier(unsigned int irq, struct irq_affinity_notify *notify)
{
	/* desc 是 IRQ 分配生命周期内借用；old_notify 在锁内取出后由 kref 保证清理期存活。 */
	struct irq_desc *desc = irq_to_desc(irq);
	struct irq_affinity_notify *old_notify;

	/* The release function is promised process context */
	/* API 保证 release 在进程上下文执行，因此这里明确检查可睡眠条件。 */
	might_sleep();

	if (!desc || irq_is_nmi(desc))
		return -EINVAL;

	/* Complete initialisation of *notify */
	/* 补全调用者提供 notifier 对象中由 IRQ 核心拥有的字段。 */
	if (notify) {
		notify->irq = irq;
		kref_init(&notify->kref);
		INIT_WORK(&notify->work, irq_affinity_notify);
	}

	scoped_guard(raw_spinlock_irq, &desc->lock) {
		old_notify = desc->affinity_notify;
		desc->affinity_notify = notify;
	}

	if (old_notify) {
		if (cancel_work_sync(&old_notify->work)) {
			/* Pending work had a ref, put that one too */
			/* 成功取消的 pending work 原本持有执行引用，取消者必须代为 put。 */
			kref_put(&old_notify->kref, old_notify->release);
		}
		kref_put(&old_notify->kref, old_notify->release);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(irq_set_affinity_notifier);

#ifndef CONFIG_AUTO_IRQ_AFFINITY
/*
 * Generic version of the affinity autoselector.
 */
/*
 * 原文意为“通用 affinity 自动选择器”。
 * irq_setup_affinity() - 为尚未建立有效目标的 IRQ 选择启动 affinity
 *
 * @desc: 输入输出的非 NULL、锁内描述符。
 * 不可设置 affinity 的 per-CPU/no-balance/无回调 IRQ 返回 0。其余 IRQ 在全局 mask_lock
 * 下复用静态临时 mask：优先保留仍含在线 CPU 的 managed/用户设置，否则回退全局默认；
 * 与 online 集合求交，空时兜底所有 online CPU，再尽量收窄到 desc NUMA node。最后返回
 * irq_do_set_affinity() 结果。函数不设置 AFFINITY_SET，调用方决定启动状态语义。
 */
int irq_setup_affinity(struct irq_desc *desc)
{
	/* set 是选中策略的借用指针；node 是 NUMA 提示，NUMA_NO_NODE 表示不收窄。 */
	struct cpumask *set = irq_default_affinity;
	int node = irq_desc_get_node(desc);

	/* 静态 mask 被所有 IRQ 共用，mask_lock 同时保证不可迁移的临时计算区。 */
	static DEFINE_RAW_SPINLOCK(mask_lock);
	static struct cpumask mask;

	/* Excludes PER_CPU and NO_BALANCE interrupts */
	/* 能力 helper 会排除 PER_CPU 和 NO_BALANCE IRQ。 */
	if (!__irq_can_set_affinity(desc))
		return 0;

	guard(raw_spinlock)(&mask_lock);
	/*
	 * Preserve the managed affinity setting and a userspace affinity
	 * setup, but make sure that one of the targets is online.
	 */
	/*
	 * 保留 managed 或用户已设置的 affinity，但必须至少有一个目标在线；
	 * 全部离线时清 AFFINITY_SET，让本次选择回退默认集合。
	 */
	if (irqd_affinity_is_managed(&desc->irq_data) ||
	    irqd_has_set(&desc->irq_data, IRQD_AFFINITY_SET)) {
		if (cpumask_intersects(desc->irq_common_data.affinity,
				       cpu_online_mask))
			set = desc->irq_common_data.affinity;
		else
			irqd_clear(&desc->irq_data, IRQD_AFFINITY_SET);
	}

	cpumask_and(&mask, cpu_online_mask, set);
	if (cpumask_empty(&mask))
		cpumask_copy(&mask, cpu_online_mask);

	if (node != NUMA_NO_NODE) {
		const struct cpumask *nodemask = cpumask_of_node(node);

		/* make sure at least one of the cpus in nodemask is online */
		/* 只有当前候选与节点掩码确有交集时才收窄，避免得到空目标。 */
		if (cpumask_intersects(&mask, nodemask))
			cpumask_and(&mask, &mask, nodemask);
	}
	return irq_do_set_affinity(&desc->irq_data, &mask, false);
}
#else
/* Wrapper for ALPHA specific affinity selector magic */
/*
 * 原文意为“ALPHA 特定 affinity 选择魔法的包装器”。启用 AUTO_IRQ_AFFINITY 时，
 * irq_setup_affinity() 把 @desc 的逻辑号交给架构 irq_select_affinity()；返回架构结果，
 * 不在通用层操作掩码或锁。@desc 为非 NULL 借用对象。
 */
int irq_setup_affinity(struct irq_desc *desc)
{
	return irq_select_affinity(irq_desc_get_irq(desc));
}
#endif /* CONFIG_AUTO_IRQ_AFFINITY */
#endif /* CONFIG_SMP */
/* UP 配置不构建 affinity 管理路径；中断只能在唯一 CPU 上处理。 */


/**
 * irq_set_vcpu_affinity - Set vcpu affinity for the interrupt
 * @irq:	interrupt number to set affinity
 * @vcpu_info:	vCPU specific data or pointer to a percpu array of vCPU
 *		specific data for percpu_devid interrupts
 *
 * This function uses the vCPU specific data to set the vCPU affinity for
 * an irq. The vCPU specific data is passed from outside, such as KVM. One
 * example code path is as below: KVM -> IOMMU -> irq_set_vcpu_affinity().
 */
/*
 * 用外部虚拟化组件（如 KVM）提供的 @vcpu_info 设置 @irq 的 vCPU 目标；
 * percpu_devid IRQ 时它可指向按 CPU 排列的一组 vCPU 私有数据，典型调用链为
 * KVM -> IOMMU -> 本函数。
 *
 * 函数在 desc bus lock 与 raw lock 保护下，从叶子 irq_data 沿 parent_data 向上寻找首个
 * 实现 irq_set_vcpu_affinity 的 irqchip，并把该层 data 与不透明指针原样交给回调。回调
 * 返回值原样传播；层级中无能力返回 -ENOSYS，逻辑 IRQ 无效返回 -EINVAL。核心不复制、
 * 不解释 @vcpu_info，其有效期与并发更新规则由调用者和 chip 回调约定。
 */
int irq_set_vcpu_affinity(unsigned int irq, void *vcpu_info)
{
	scoped_irqdesc_get_and_lock(irq, 0) {
		/* data 逐层借用；chip 始终对应当前 data，找到能力后才离开循环。 */
		struct irq_desc *desc = scoped_irqdesc;
		struct irq_data *data;
		struct irq_chip *chip;

		data = irq_desc_get_irq_data(desc);
		/* 阶段 1：沿叶子到根逐层寻找真正理解 @vcpu_info 的第一个 chip。 */
		do {
			chip = irq_data_get_irq_chip(data);
			if (chip && chip->irq_set_vcpu_affinity)
				break;

			data = irqd_get_parent_data(data);
		} while (data);

		if (!data)
			return -ENOSYS;
		/* 阶段 2：仍在描述符锁域内调用选中层，返回值与副作用完全由 chip 定义。 */
		return chip->irq_set_vcpu_affinity(data, vcpu_info);
	}
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(irq_set_vcpu_affinity);

/*
 * __disable_irq() - 增加一次描述符的软件禁用嵌套深度
 *
 * @desc: 输入输出的非 NULL 描述符；调用者已持 desc->lock，并按需要持 chip bus lock。
 * 仅从 depth 0 变为 1 时调用 irq_disable() 落实硬件/状态关闭；后续嵌套只递增计数。
 * 无返回值、不等待已在执行的 handler。每次调用必须由一次 __enable_irq() 配平。
 */
void __disable_irq(struct irq_desc *desc)
{
	if (!desc->depth++)
		irq_disable(desc);
}

/*
 * __disable_irq_nosync() - 按逻辑号加锁执行一次非同步禁用
 *
 * @irq: 全局逻辑 IRQ。
 * desc 存在时在 bus lock 与 desc 锁下调用 __disable_irq() 并返回 0；不存在/类型检查失败
 * 返回 -EINVAL。它只阻止新的正常分发，不等待当前 hardirq 或线程退出。
 */
static int __disable_irq_nosync(unsigned int irq)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {
		__disable_irq(scoped_irqdesc);
		return 0;
	}
	return -EINVAL;
}

/**
 * disable_irq_nosync - disable an irq without waiting
 * @irq: Interrupt to disable
 *
 * Disable the selected interrupt line.  Disables and Enables are
 * nested.
 * Unlike disable_irq(), this function does not ensure existing
 * instances of the IRQ handler have completed before returning.
 *
 * This function may be called from IRQ context.
 */
/*
 * 禁用 @irq，disable/enable 采用可嵌套计数；与 disable_irq() 不同，返回前不
 * 保证既有 handler 已完成。因此可从 IRQ 上下文调用，但调用者若要拆资源，必须另行完整
 * synchronize。公开 API 不返回查找错误；无效 IRQ 只导致内部失败。
 */
void disable_irq_nosync(unsigned int irq)
{
	__disable_irq_nosync(irq);
}
EXPORT_SYMBOL(disable_irq_nosync);

/**
 * disable_irq - disable an irq and wait for completion
 * @irq: Interrupt to disable
 *
 * Disable the selected interrupt line.  Enables and Disables are nested.
 *
 * This function waits for any pending IRQ handlers for this interrupt to
 * complete before returning. If you use this function while holding a
 * resource the IRQ handler may need you will deadlock.
 *
 * Can only be called from preemptible code as it might sleep when an
 * interrupt thread is associated to @irq.
 *
 */
/*
 * 嵌套禁用 @irq，并等待所有 pending/running hardirq 与 threaded handler
 * 完成。等待可能睡眠，只能从可抢占进程上下文调用；持有 handler 需要的资源会死锁。
 * 若 IRQ 无效，内部禁用失败后不做同步。无返回值，之后仍须以 enable_irq() 配平。
 */
void disable_irq(unsigned int irq)
{
	might_sleep();
	if (!__disable_irq_nosync(irq))
		synchronize_irq(irq);
}
EXPORT_SYMBOL(disable_irq);

/**
 * disable_hardirq - disables an irq and waits for hardirq completion
 * @irq: Interrupt to disable
 *
 * Disable the selected interrupt line.  Enables and Disables are nested.
 *
 * This function waits for any pending hard IRQ handlers for this interrupt
 * to complete before returning. If you use this function while holding a
 * resource the hard IRQ handler may need you will deadlock.
 *
 * When used to optimistically disable an interrupt from atomic context the
 * return value must be checked.
 *
 * Returns: false if a threaded handler is active.
 *
 * This function may be called - with care - from IRQ context.
 */
/*
 * 先嵌套禁用 @irq，再只等待 hardirq 部分完成，不等待 IRQ 线程。返回 true
 * 表示禁用成功且此刻没有活动线程；false 既可能表示 IRQ 无效，也可能表示线程仍活跃，
 * 调用者必须检查返回值后才能在原子上下文乐观地拆除 hardirq 专用资源。它可谨慎用于
 * IRQ 上下文，但不能持 hard handler 所需资源。
 */
bool disable_hardirq(unsigned int irq)
{
	if (!__disable_irq_nosync(irq))
		return synchronize_hardirq(irq);
	return false;
}
EXPORT_SYMBOL_GPL(disable_hardirq);

/**
 * disable_nmi_nosync - disable an nmi without waiting
 * @irq: Interrupt to disable
 *
 * Disable the selected interrupt line. Disables and enables are nested.
 *
 * The interrupt to disable must have been requested through request_nmi.
 * Unlike disable_nmi(), this function does not ensure existing
 * instances of the IRQ handler have completed before returning.
 */
/*
 * 对由 request_nmi() 建立的 @irq 增加一次 NMI 禁用深度，不等待正在执行的
 * NMI handler。实现复用通用 nosync 路径；调用者必须用 enable_nmi() 配平，并在释放前
 * 使用具备相应同步保证的 NMI 生命周期接口。
 */
void disable_nmi_nosync(unsigned int irq)
{
	disable_irq_nosync(irq);
}

/*
 * __enable_irq() - 撤销一次嵌套禁用并在最后一级重新启动 IRQ
 *
 * @desc: 输入输出的非 NULL、desc 锁内描述符。
 * depth==0 是不配平 enable，告警且不改状态；depth==1 时若 suspend 也告警，否则设置
 * NOPROBE 并以 FORCE+RESEND 调用 irq_startup()，兼容 NOAUTOEN 首次启用及 S3 恢复中的
 * managed shutdown IRQ；depth>1 仅递减。无返回值，startup 错误不向本层传播。
 */
void __enable_irq(struct irq_desc *desc)
{
	switch (desc->depth) {
	case 0:
 err_out:
		WARN(1, KERN_WARNING "Unbalanced enable for IRQ %d\n",
		     irq_desc_get_irq(desc));
		break;
	case 1: {
		if (desc->istate & IRQS_SUSPENDED)
			goto err_out;
		/* Prevent probing on this irq: */
		/* 显式启用后禁止再把该线路当作自动探测候选。 */
		irq_settings_set_noprobe(desc);
		/*
		 * Call irq_startup() not irq_enable() here because the
		 * interrupt might be marked NOAUTOEN so irq_startup()
		 * needs to be invoked when it gets enabled the first time.
		 * This is also required when __enable_irq() is invoked for
		 * a managed and shutdown interrupt from the S3 resume
		 * path.
		 *
		 * If it was already started up, then irq_startup() will
		 * invoke irq_enable() under the hood.
		 */
		/*
		 * 这里必须调用 irq_startup() 而非 irq_enable()。NOAUTOEN IRQ 在首次
		 * enable 时尚未 startup；S3 resume 也可能在 managed+shutdown 状态进入此路径。
		 * 若 IRQ 早已启动，irq_startup() 内部会退化为 irq_enable()。
		 */
		irq_startup(desc, IRQ_RESEND, IRQ_START_FORCE);
		break;
	}
	default:
		desc->depth--;
	}
}

/**
 * enable_irq - enable handling of an irq
 * @irq: Interrupt to enable
 *
 * Undoes the effect of one call to disable_irq().  If this matches the
 * last disable, processing of interrupts on this IRQ line is re-enabled.
 *
 * This function may be called from IRQ context only when
 * desc->irq_data.chip->bus_lock and desc->chip->bus_sync_unlock are NULL !
 */
/*
 * 撤销 @irq 的一次 disable；只有最后一层配平时才重新允许处理中断。函数取得
 * chip bus lock 和 desc 锁，故仅当 chip 没有可能睡眠的 bus_lock/bus_sync_unlock 时才
 * 可从 IRQ 上下文调用。未 setup/request、没有 chip 时告警；无返回值。
 */
void enable_irq(unsigned int irq)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {
		struct irq_desc *desc = scoped_irqdesc;

		if (WARN(!desc->irq_data.chip, "enable_irq before setup/request_irq: irq %u\n", irq))
			return;
		__enable_irq(desc);
	}
}
EXPORT_SYMBOL(enable_irq);

/**
 * enable_nmi - enable handling of an nmi
 * @irq: Interrupt to enable
 *
 * The interrupt to enable must have been requested through request_nmi.
 * Undoes the effect of one call to disable_nmi(). If this matches the last
 * disable, processing of interrupts on this IRQ line is re-enabled.
 */
/*
 * 撤销 request_nmi() 所建 @irq 的一次 NMI disable；最后一级重新启用线路。
 * 它复用 enable_irq() 的锁与 depth 规则，调用者必须保证目标确为 NMI 且上下文满足 chip
 * bus-lock 限制。
 */
void enable_nmi(unsigned int irq)
{
	enable_irq(irq);
}

/*
 * set_irq_wake_real() - 把 wake enable/disable 真正下发给 irqchip
 *
 * @irq: 已验证且描述符存活的逻辑 IRQ。
 * @on: 非零启用、零禁用。
 * IRQCHIP_SKIP_SET_WAKE 表示核心可视为成功而无需硬件回调；有 irq_set_wake 时返回其
 * 结果，否则返回 -ENXIO。调用者持 bus/desc 锁；本函数不修改 wake_depth 或核心状态位。
 */
static int set_irq_wake_real(unsigned int irq, unsigned int on)
{
	struct irq_desc *desc = irq_to_desc(irq);
	int ret = -ENXIO;

	if (irq_desc_get_chip(desc)->flags &  IRQCHIP_SKIP_SET_WAKE)
		return 0;

	if (desc->irq_data.chip->irq_set_wake)
		ret = desc->irq_data.chip->irq_set_wake(&desc->irq_data, on);

	return ret;
}

/**
 * irq_set_irq_wake - control irq power management wakeup
 * @irq:	interrupt to control
 * @on:	enable/disable power management wakeup
 *
 * Enable/disable power management wakeup mode, which is disabled by
 * default.  Enables and disables must match, just as they match for
 * non-wakeup mode support.
 *
 * Wakeup mode lets this IRQ wake the system from sleep states like
 * "suspend to RAM".
 *
 * Note: irq enable/disable state is completely orthogonal to the
 * enable/disable state of irq wake. An irq can be disabled with
 * disable_irq() and still wake the system as long as the irq has wake
 * enabled. If this does not hold, then the underlying irq chip and the
 * related driver need to be investigated.
 */
/*
 * 以嵌套引用计数控制 @irq 的系统唤醒能力，默认关闭；wake 模式允许该 IRQ
 * 从 suspend-to-RAM 等睡眠态唤醒系统。wake 状态与普通 irq enable 状态完全正交：线路
 * 即使被 disable_irq() 禁用，只要 wake 已启用仍应能唤醒，否则应检查 irqchip/驱动。
 *
 * 在 bus/desc 锁下，on 只在 wake_depth 0->1 时调用硬件，off 只在 1->0 时调用；硬件
 * 失败会回滚计数。成功边界同步 IRQD_WAKEUP_STATE。不配平的 off 告警但返回 0；NMI 或
 * 无效 IRQ 返回 -EINVAL，chip 不支持时首次转换返回 -ENXIO。
 */
int irq_set_irq_wake(unsigned int irq, unsigned int on)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {
		struct irq_desc *desc = scoped_irqdesc;
		int ret = 0;

		/* Don't use NMIs as wake up interrupts please */
		/* NMI 不允许作为由此通用接口管理的系统唤醒中断。 */
		if (irq_is_nmi(desc))
			return -EINVAL;

		/*
		 * wakeup-capable irqs can be shared between drivers that
		 * don't need to have the same sleep mode behaviors.
		 */
		/*
		 * 可唤醒 IRQ 可能被多个驱动共享，而各驱动的睡眠策略不同，因此用
		 * wake_depth 汇总各调用方引用，不能让一次关闭覆盖其他调用方的启用。
		 */
		if (on) {
			/* 启用只在 0->1 边界下发；失败把引用计数和硬件状态一起回滚。 */
			if (desc->wake_depth++ == 0) {
				ret = set_irq_wake_real(irq, on);
				if (ret)
					desc->wake_depth = 0;
				else
					irqd_set(&desc->irq_data, IRQD_WAKEUP_STATE);
			}
		} else {
			/* 禁用只在 1->0 边界下发；不配平只告警，硬件失败则恢复一份引用。 */
			if (desc->wake_depth == 0) {
				WARN(1, "Unbalanced IRQ %d wake disable\n", irq);
			} else if (--desc->wake_depth == 0) {
				ret = set_irq_wake_real(irq, on);
				if (ret)
					desc->wake_depth = 1;
				else
					irqd_clear(&desc->irq_data, IRQD_WAKEUP_STATE);
			}
		}
		return ret;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(irq_set_irq_wake);

/*
 * Internal function that tells the architecture code whether a
 * particular irq has been exclusively allocated or is available
 * for driver use.
 */
/*
 * 供架构代码判断 @irq 是否已被独占分配，或仍可供驱动申请。
 * 在 desc 锁下，只有 settings 允许 request，且 action 链为空，或新 @irqflags 与现有首个
 * action 都声明 IRQF_SHARED 时返回 true；否则（含无效 IRQ）false。该函数只做瞬时检查，
 * 不预留 IRQ，真正申请仍须由 __setup_irq() 在 request_mutex 下解决竞态。
 */
bool can_request_irq(unsigned int irq, unsigned long irqflags)
{
	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {
		struct irq_desc *desc = scoped_irqdesc;

		if (irq_settings_can_request(desc)) {
			if (!desc->action || irqflags & desc->action->flags & IRQF_SHARED)
				return true;
		}
	}
	return false;
}

/*
 * __irq_set_trigger() - 在锁内让 irqchip 设置触发方式并同步核心状态
 *
 * @desc: 输入输出的非 NULL 描述符；调用者持 desc->lock，且按 chip 要求持 bus lock。
 * @flags: 含 IRQ_TYPE_* 的请求，函数只保留 IRQ_TYPE_SENSE_MASK。
 * 无 set_type 能力时打印调试信息并按兼容策略返回 0。IRQCHIP_SET_TYPE_MASKED 要求回调期
 * mask：函数只在入口未 masked 时临时 mask，并仅在入口未 disabled 时于末尾 unmask。
 * chip 成功码归一为 0，并同步 irq_data trigger/level 与 desc settings；错误原样返回并
 * 记录日志。NOCOPY 要求 chip 已更新 irq_data trigger，核心从中回读。
 */
int __irq_set_trigger(struct irq_desc *desc, unsigned long flags)
{
	/* chip 为 desc 生命周期内借用；unmask 记录是否需恢复入口的运行状态。 */
	struct irq_chip *chip = desc->irq_data.chip;
	int ret, unmask = 0;

	if (!chip || !chip->irq_set_type) {
		/*
		 * IRQF_TRIGGER_* but the PIC does not support multiple
		 * flow-types?
		 */
		/* 请求了 IRQF_TRIGGER_*，但该 PIC 可能根本不支持多种 flow type。 */
		pr_debug("No set_type function for IRQ %d (%s)\n",
			 irq_desc_get_irq(desc),
			 chip ? (chip->name ? : "unknown") : "unknown");
		return 0;
	}

	/* 阶段 1：按 chip 契约建立 set_type 回调所需的临时 masked 状态。 */
	if (chip->flags & IRQCHIP_SET_TYPE_MASKED) {
		if (!irqd_irq_masked(&desc->irq_data))
			mask_irq(desc);
		if (!irqd_irq_disabled(&desc->irq_data))
			unmask = 1;
	}

	/* Mask all flags except trigger mode */
	/* 丢弃非触发模式位，避免把 action flags 误传给 irqchip。 */
	flags &= IRQ_TYPE_SENSE_MASK;
	ret = chip->irq_set_type(&desc->irq_data, flags);

	/*
	 * 阶段 2：把 chip 的三类成功约定归一化。普通成功由核心写 trigger，
	 * NOCOPY 则从 chip 已更新的 irq_data 回读；二者最终同步 desc 的 level 属性。
	 */
	switch (ret) {
	case IRQ_SET_MASK_OK:
	case IRQ_SET_MASK_OK_DONE:
		irqd_clear(&desc->irq_data, IRQD_TRIGGER_MASK);
		irqd_set(&desc->irq_data, flags);
		fallthrough;

	case IRQ_SET_MASK_OK_NOCOPY:
		flags = irqd_get_trigger_type(&desc->irq_data);
		irq_settings_set_trigger_mask(desc, flags);
		irqd_clear(&desc->irq_data, IRQD_LEVEL);
		irq_settings_clr_level(desc);
		if (flags & IRQ_TYPE_LEVEL_MASK) {
			irq_settings_set_level(desc);
			irqd_set(&desc->irq_data, IRQD_LEVEL);
		}

		ret = 0;
		break;
	default:
		pr_err("Setting trigger mode %lu for irq %u failed (%pS)\n",
		       flags, irq_desc_get_irq(desc), chip->irq_set_type);
	}
	/* 阶段 3：只恢复入口时本来可运行的线路，不能意外启用已 disabled 的 IRQ。 */
	if (unmask)
		unmask_irq(desc);
	return ret;
}

#ifdef CONFIG_HARDIRQS_SW_RESEND
/*
 * irq_set_parent() - 记录软件 resend 使用的父逻辑 IRQ
 *
 * @irq: 要更新的子 IRQ。
 * @parent_irq: 父逻辑号；仅保存整数，不取得描述符引用。
 * desc 存在时在锁下赋值并返回 0，否则 -EINVAL。该关系供 HARDIRQS_SW_RESEND 路径追踪，
 * 不会建立 irqdomain 层级或改变硬件路由，调用者负责父号生命周期一致性。
 */
int irq_set_parent(int irq, int parent_irq)
{
	scoped_irqdesc_get_and_lock(irq, 0) {
		scoped_irqdesc->parent_irq = parent_irq;
		return 0;
	}
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(irq_set_parent);
#endif

/*
 * Default primary interrupt handler for threaded interrupts. Is
 * assigned as primary handler when request_threaded_irq is called
 * with handler == NULL. Useful for oneshot interrupts.
 */
/*
 * request_threaded_irq() 未提供 primary handler 时采用的默认 hardirq 入口，
 * 尤其适合 oneshot。它不处理设备，只返回 IRQ_WAKE_THREAD，请核心唤醒 action->thread_fn；
 * @irq/@dev_id 均不消费，调用于 hardirq 上下文。
 */
static irqreturn_t irq_default_primary_handler(int irq, void *dev_id)
{
	return IRQ_WAKE_THREAD;
}

/*
 * Primary handler for nested threaded interrupts. Should never be
 * called.
 */
/*
 * nested threaded IRQ 的哨兵 primary handler，正常情况下父线程应直接执行其
 * thread_fn，绝不进入此函数。若被错误调用则告警并返回 IRQ_NONE，暴露层级配置错误。
 */
static irqreturn_t irq_nested_primary_handler(int irq, void *dev_id)
{
	WARN(1, "Primary handler called for nested irq %d\n", irq);
	return IRQ_NONE;
}

/*
 * irq_forced_secondary_handler() - forced threading 辅助 action 的硬中断哨兵
 *
 * secondary action 只承载原 thread_fn 并由主 IRQ 线程显式唤醒，不应作为 action 链上的
 * primary 被分发；若发生则告警并返回 IRQ_NONE。@dev_id 未消费。
 */
static irqreturn_t irq_forced_secondary_handler(int irq, void *dev_id)
{
	WARN(1, "Secondary action handler called for irq %d\n", irq);
	return IRQ_NONE;
}

#ifdef CONFIG_SMP
/*
 * Check whether we need to change the affinity of the interrupt thread.
 */
/*
 * 检查当前 IRQ 线程是否收到 affinity 更新请求。
 * @desc/@action 是当前线程对应的存活借用对象。test_and_clear 取得一次请求；分配临时
 * cpumask 失败时重新置位供下次重试。成功时在 desc 锁下复制 effective affinity，出锁后
 * 调 set_cpus_allowed_ptr(current)，避免调度器操作与 desc 锁形成锁依赖。无返回值，
 * 设置失败不传播；临时 mask 由本函数分配并释放。
 */
static void irq_thread_check_affinity(struct irq_desc *desc, struct irqaction *action)
{
	/* mask 是可睡眠分配的本次调用私有位图，不能在 desc 锁外借用内部 affinity 指针。 */
	cpumask_var_t mask;

	if (!test_and_clear_bit(IRQTF_AFFINITY, &action->thread_flags))
		return;

	__set_current_state(TASK_RUNNING);

	/*
	 * In case we are out of memory we set IRQTF_AFFINITY again and
	 * try again next time
	 */
	/* 内存不足时恢复 IRQTF_AFFINITY，让线程下一次被唤醒时重试。 */
	if (!alloc_cpumask_var(&mask, GFP_KERNEL)) {
		set_bit(IRQTF_AFFINITY, &action->thread_flags);
		return;
	}

	scoped_guard(raw_spinlock_irq, &desc->lock) {
		const struct cpumask *m;

		m = irq_data_get_effective_affinity_mask(&desc->irq_data);
		cpumask_copy(mask, m);
	}

	set_cpus_allowed_ptr(current, mask);
	free_cpumask_var(mask);
}
#else
/* UP 没有可迁移目标，线程 affinity 检查为空操作。 */
static inline void irq_thread_check_affinity(struct irq_desc *desc, struct irqaction *action) { }
#endif

/*
 * irq_wait_for_interrupt() - IRQ kthread 的可中断等待与停止仲裁循环
 *
 * @desc: 线程所属描述符，在线程退出前由 free 协议保活。
 * @action: 线程私有 action，IRQTF_* 位在 hardirq/free/本线程间原子同步。
 * 每轮先设 TASK_INTERRUPTIBLE 并应用 affinity。停止请求到达时仍优先消费一次已置位的
 * RUNTHREAD，返回 0 让 handler 最后执行；没有工作则恢复 RUNNING 并返回 -1 退出。普通
 * RUNTHREAD 返回 0，否则 schedule()。函数可睡眠，返回时当前任务总为 TASK_RUNNING。
 */
static int irq_wait_for_interrupt(struct irq_desc *desc,
				  struct irqaction *action)
{
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		irq_thread_check_affinity(desc, action);

		if (kthread_should_stop()) {
			/* may need to run one last time */
			/* stop 与最后一次唤醒竞态时，已记账的工作仍必须执行并配平。 */
			if (test_and_clear_bit(IRQTF_RUNTHREAD,
					       &action->thread_flags)) {
				__set_current_state(TASK_RUNNING);
				return 0;
			}
			__set_current_state(TASK_RUNNING);
			return -1;
		}

		if (test_and_clear_bit(IRQTF_RUNTHREAD,
				       &action->thread_flags)) {
			__set_current_state(TASK_RUNNING);
			return 0;
		}
		schedule();
	}
}

/*
 * Oneshot interrupts keep the irq line masked until the threaded
 * handler finished. unmask if the interrupt has not been disabled and
 * is marked MASKED.
 */
/*
 * oneshot IRQ 在线程 handler 完成前保持线路 masked；当该共享 IRQ 的最后一个
 * oneshot 线程结束，且 IRQ 未 disabled、仍 marked MASKED 时才 unmask。
 *
 * @desc/@action 在 IRQ 线程生命周期内有效。非 oneshot 或 forced secondary 直接返回。
 * 函数按 chip bus lock -> desc raw lock 顺序串行硬件与 threads_oneshot；若 hardirq 仍在
 * INPROGRESS，则释放两锁忙等重试，避免跨 CPU 的 hard handler 再次 mask 后永久不解锁。
 * RUNTHREAD 又被置位时保留本 action 的 bit。无返回值，可能短暂忙等，不睡眠。
 */
static void irq_finalize_oneshot(struct irq_desc *desc,
				 struct irqaction *action)
{
	if (!(desc->istate & IRQS_ONESHOT) ||
	    action->handler == irq_forced_secondary_handler)
		return;
again:
	chip_bus_lock(desc);
	raw_spin_lock_irq(&desc->lock);

	/*
	 * Implausible though it may be we need to protect us against
	 * the following scenario:
	 *
	 * The thread is faster done than the hard interrupt handler
	 * on the other CPU. If we unmask the irq line then the
	 * interrupt can come in again and masks the line, leaves due
	 * to IRQS_INPROGRESS and the irq line is masked forever.
	 *
	 * This also serializes the state of shared oneshot handlers
	 * versus "desc->threads_oneshot |= action->thread_mask;" in
	 * irq_wake_thread(). See the comment there which explains the
	 * serialization.
	 */
	/*
	 * 即使少见，也必须防止本线程比另一 CPU 上的 hard handler 更早完成。
	 * 若此时解 mask，新事件可进入、再次 mask，却因 IRQS_INPROGRESS 提前退出，使线路永久
	 * masked。此锁序也与 irq_wake_thread() 设置 threads_oneshot 串行，详见该处说明。
	 */
	if (unlikely(irqd_irq_inprogress(&desc->irq_data))) {
		raw_spin_unlock_irq(&desc->lock);
		chip_bus_sync_unlock(desc);
		cpu_relax();
		goto again;
	}

	/*
	 * Now check again, whether the thread should run. Otherwise
	 * we would clear the threads_oneshot bit of this thread which
	 * was just set.
	 */
	/* 锁内再查 RUNTHREAD，避免清掉刚被新一次唤醒重新设置的 oneshot bit。 */
	if (test_bit(IRQTF_RUNTHREAD, &action->thread_flags))
		goto out_unlock;

	desc->threads_oneshot &= ~action->thread_mask;

	if (!desc->threads_oneshot && !irqd_irq_disabled(&desc->irq_data) &&
	    irqd_irq_masked(&desc->irq_data))
		unmask_threaded_irq(desc);

out_unlock:
	raw_spin_unlock_irq(&desc->lock);
	chip_bus_sync_unlock(desc);
}

/*
 * Interrupts explicitly requested as threaded interrupts want to be
 * preemptible - many of them need to sleep and wait for slow busses to
 * complete.
 */
/*
 * 显式申请的 threaded handler 应保持可抢占，因为它常需睡眠等待慢速总线。
 * 函数在 IRQ kthread 进程上下文调用 action->thread_fn；IRQ_HANDLED 时累加无处理检测所用
 * threads_handled，随后完成 oneshot 收尾并原样返回 handler 结果。
 */
static irqreturn_t irq_thread_fn(struct irq_desc *desc,	struct irqaction *action)
{
	irqreturn_t ret = action->thread_fn(action->irq, action->dev_id);

	if (ret == IRQ_HANDLED)
		atomic_inc(&desc->threads_handled);

	irq_finalize_oneshot(desc, action);
	return ret;
}

/*
 * Interrupts which are not explicitly requested as threaded
 * interrupts rely on the implicit bh/preempt disable of the hard irq
 * context. So we need to disable bh here to avoid deadlocks and other
 * side effects.
 */
/*
 * 被强制线程化、但原本按 hardirq 编写的 handler 依赖 hardirq 隐含的 BH/抢占
 * 关闭语义；在线程中必须模拟它以避免死锁或副作用。所有配置都关闭 BH；非 PREEMPT_RT
 * 还在调用期关闭本地 IRQ，RT 由其线程化模型提供所需语义。返回 irq_thread_fn() 结果，
 * 并严格逆序恢复上下文状态。
 */
static irqreturn_t irq_forced_thread_fn(struct irq_desc *desc, struct irqaction *action)
{
	irqreturn_t ret;

	local_bh_disable();
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		local_irq_disable();
	ret = irq_thread_fn(desc, action);
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		local_irq_enable();
	local_bh_enable();
	return ret;
}

/*
 * wake_threads_waitq() - 完成一次已记账的 IRQ 线程执行
 *
 * @desc: 存活描述符。原子递减 threads_active；若降为 0，唤醒 synchronize/free 及 ready
 * 等待者。无返回值。调用者必须与成功置 RUNTHREAD 时的递增严格一一对应，避免下溢。
 */
void wake_threads_waitq(struct irq_desc *desc)
{
	if (atomic_dec_and_test(&desc->threads_active))
		wake_up(&desc->wait_for_threads);
}

/*
 * irq_thread_dtor() - 异常退出 IRQ kthread 时修复执行记账和 oneshot 状态
 *
 * @unused: task_work 回调接口参数，未消费；回调只能在 current 正退出时运行。
 * action 从 kthread_data() 取回。若还有 RUNTHREAD，说明对应 threads_active 尚未配平，
 * 清位并递减/唤醒；随后调用 oneshot 收尾，防止 desc->threads_oneshot 遗留。正常由
 * __free_irq()+kthread_stop() 退出会在 irq_thread() 中取消此 task_work，不走本错误路径。
 */
static void irq_thread_dtor(struct callback_head *unused)
{
	struct task_struct *tsk = current;
	struct irq_desc *desc;
	struct irqaction *action;

	if (WARN_ON_ONCE(!(current->flags & PF_EXITING)))
		return;

	action = kthread_data(tsk);

	pr_err("exiting task \"%s\" (%d) is an active IRQ thread (irq %d)\n",
	       tsk->comm, tsk->pid, action->irq);


	desc = irq_to_desc(action->irq);
	/*
	 * If IRQTF_RUNTHREAD is set, we need to decrement
	 * desc->threads_active and wake possible waiters.
	 */
	/* RUNTHREAD 尚在表示存在未配平的 threads_active，必须递减并唤醒等待者。 */
	if (test_and_clear_bit(IRQTF_RUNTHREAD, &action->thread_flags))
		wake_threads_waitq(desc);

	/* Prevent a stale desc->threads_oneshot */
	/* 异常退出也要清理本线程的 oneshot 位，避免线路永久保持 masked。 */
	irq_finalize_oneshot(desc, action);
}

/*
 * irq_wake_secondary() - 从主 forced IRQ 线程唤醒其辅助线程
 *
 * @desc: action 所属存活描述符。
 * @action: 拥有 secondary 的主 action。
 * secondary 缺失表示构造/返回值契约错误，告警后返回；否则在 desc 锁下调用内部唤醒，
 * 由其原子设置 RUNTHREAD、threads_oneshot 并增加 threads_active。无返回值、不睡眠。
 */
static void irq_wake_secondary(struct irq_desc *desc, struct irqaction *action)
{
	struct irqaction *secondary = action->secondary;

	if (WARN_ON_ONCE(!secondary))
		return;

	guard(raw_spinlock_irq)(&desc->lock);
	__irq_wake_thread(desc, secondary);
}

/*
 * Internal function to notify that a interrupt thread is ready.
 */
/*
 * IRQ kthread 进入主函数后设置 IRQTF_READY，并唤醒 wait_for_threads 上等待
 * setup 完成的申请者。@action 的线程位是发布标志，@desc waitqueue 只负责通知。
 */
static void irq_thread_set_ready(struct irq_desc *desc,
				 struct irqaction *action)
{
	set_bit(IRQTF_READY, &action->thread_flags);
	wake_up(&desc->wait_for_threads);
}

/*
 * Internal function to wake up a interrupt thread and wait until it is
 * ready.
 */
/*
 * 若 @action 及其 thread 存在，唤醒新建 kthread 并睡眠等待 IRQTF_READY。
 * 用于发布 action 前确认线程初始化已开始，避免后续 affinity/free 与尚未运行的线程竞态。
 * 必须在可睡眠上下文调用，且不得持阻止线程启动的锁；无返回值。
 */
static void wake_up_and_wait_for_irq_thread_ready(struct irq_desc *desc,
						  struct irqaction *action)
{
	if (!action || !action->thread)
		return;

	wake_up_process(action->thread);
	wait_event(desc->wait_for_threads,
		   test_bit(IRQTF_READY, &action->thread_flags));
}

/*
 * Interrupt handler thread
 */
/*
 * 原文意为“中断处理线程”。@data 是在线程整个寿命内由 action 所有者保活的 irqaction。
 * 入口发布 READY，按普通/secondary 选择 FIFO 策略，再按 forced 标志选择是否模拟 hardirq
 * 上下文。注册退出 task_work 处理异常自退；循环等待 RUNTHREAD、执行 handler，若返回
 * IRQ_WAKE_THREAD 再唤醒 secondary，并配平 threads_active。kthread_stop 正常退出前，
 * free 路径已 synchronize_hardirq，故取消 dtor 时不应残留 RUNTHREAD/oneshot bit。返回 0。
 */
static int irq_thread(void *data)
{
	/* on_exit_work 属于当前栈但 task_work 在退出前取消/执行；action/desc 由 free 协议保活。 */
	struct callback_head on_exit_work;
	struct irqaction *action = data;
	struct irq_desc *desc = irq_to_desc(action->irq);
	irqreturn_t (*handler_fn)(struct irq_desc *desc,
			struct irqaction *action);

	/* 阶段 1：先发布 READY，再确定调度策略和本线程实际执行的包装函数。 */
	irq_thread_set_ready(desc, action);

	if (action->handler == irq_forced_secondary_handler)
		sched_set_fifo_secondary(current);
	else
		sched_set_fifo(current);

	if (force_irqthreads() && test_bit(IRQTF_FORCED_THREAD,
					   &action->thread_flags))
		handler_fn = irq_forced_thread_fn;
	else
		handler_fn = irq_thread_fn;

	/* 阶段 2：安装异常退出兜底；正常 free 会在退出前取消这个 task_work。 */
	init_task_work(&on_exit_work, irq_thread_dtor);
	task_work_add(current, &on_exit_work, TWA_NONE);

	/*
	 * 阶段 3：每次成功领取 RUNTHREAD 只执行一次并配平一次 threads_active；
	 * handler 要求唤醒 secondary 时，由后者建立自己独立的活动计数。
	 */
	while (!irq_wait_for_interrupt(desc, action)) {
		irqreturn_t action_ret;

		action_ret = handler_fn(desc, action);
		if (action_ret == IRQ_WAKE_THREAD)
			irq_wake_secondary(desc, action);

		wake_threads_waitq(desc);
	}

	/*
	 * This is the regular exit path. __free_irq() is stopping the
	 * thread via kthread_stop() after calling
	 * synchronize_hardirq(). So neither IRQTF_RUNTHREAD nor the
	 * oneshot mask bit can be set.
	 */
	/*
	 * 这是正常退出。__free_irq() 先 synchronize_hardirq()，再 kthread_stop()，
	 * 所以此刻既不应有 RUNTHREAD，也不应残留本线程的 oneshot mask bit。
	 */
	task_work_cancel_func(current, irq_thread_dtor);
	return 0;
}

/**
 * irq_wake_thread - wake the irq thread for the action identified by dev_id
 * @irq:	Interrupt line
 * @dev_id:	Device identity for which the thread should be woken
 */
/*
 * 在 @irq 的 action 链中按 @dev_id 找到设备，并唤醒其 IRQ 线程。无效 IRQ
 * 或 percpu_devid 类型直接返回；在 desc irqsave 锁下只处理首个匹配 action，且必须确有
 * thread。无返回值，适用于 primary handler 之外需要显式触发线程处理的受控路径；
 * @dev_id 必须与 request 时身份完全相同。
 */
void irq_wake_thread(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action;

	if (!desc || WARN_ON(irq_settings_is_per_cpu_devid(desc)))
		return;

	/*
	 * desc 锁同时稳定 action 链并与 hardirq 的线程唤醒记账串行；匹配后只把
	 * 工作提交给已有 thread，action 和 @dev_id 的所有权均不改变。
	 */
	guard(raw_spinlock_irqsave)(&desc->lock);
	for_each_action_of_desc(desc, action) {
		if (action->dev_id == dev_id) {
			if (action->thread)
				__irq_wake_thread(desc, action);
			break;
		}
	}
}
EXPORT_SYMBOL_GPL(irq_wake_thread);

/*
 * irq_setup_forced_threading() - 必要时把普通 hardirq action 改造成线程化结构
 *
 * @new: 尚未发布、由申请路径独占的 irqaction；函数可改 flags/handler/thread_fn/secondary。
 * 全局未强制，或 NO_THREAD/PERCPU/已 ONESHOT，或本来就是默认 primary 的显式线程 IRQ
 * 时保持不变返回 0。其余 action 增加 ONESHOT，把原 handler 移到 thread_fn，并用默认
 * primary 唤醒线程。若原本同时有真实 primary 与 thread_fn，则分配 secondary 保存原
 * thread_fn，由主线程返回 IRQ_WAKE_THREAD 时唤醒。分配失败返回 -ENOMEM，调用者释放。
 */
static int irq_setup_forced_threading(struct irqaction *new)
{
	if (!force_irqthreads())
		return 0;
	if (new->flags & (IRQF_NO_THREAD | IRQF_PERCPU | IRQF_ONESHOT))
		return 0;

	/*
	 * No further action required for interrupts which are requested as
	 * threaded interrupts already
	 */
	/* 已经按 threaded IRQ 形式申请的 action 无需再次改写。 */
	if (new->handler == irq_default_primary_handler)
		return 0;

	new->flags |= IRQF_ONESHOT;

	/*
	 * Handle the case where we have a real primary handler and a
	 * thread handler. We force thread them as well by creating a
	 * secondary action.
	 */
	/*
	 * 若驱动同时提供真实 primary 与 thread handler，两者都要线程化；为原
	 * thread_fn 新建 secondary action，主 action 的线程先运行原 primary，再按返回值唤醒它。
	 */
	if (new->handler && new->thread_fn) {
		/* Allocate the secondary action */
		/* 分配并初始化只属于 @new 的辅助 action，失败尚未发布，直接回滚。 */
		new->secondary = kzalloc_obj(struct irqaction);
		if (!new->secondary)
			return -ENOMEM;
		new->secondary->handler = irq_forced_secondary_handler;
		new->secondary->thread_fn = new->thread_fn;
		new->secondary->dev_id = new->dev_id;
		new->secondary->irq = new->irq;
		new->secondary->name = new->name;
	}
	/* Deal with the primary handler */
	/* 把原 primary 移入主 IRQ 线程，并用只负责唤醒的默认 hardirq handler 替换。 */
	set_bit(IRQTF_FORCED_THREAD, &new->thread_flags);
	new->thread_fn = new->handler;
	new->handler = irq_default_primary_handler;
	return 0;
}

/*
 * irq_request_resources() - 请求 irqchip 对该描述符所需的外围资源
 *
 * @desc: 已有 chip、尚在申请序列中的描述符。
 * 有 irq_request_resources 回调则原样返回其结果，否则视为成功。资源所有权在成功后转给
 * IRQ/action 生命周期，并由 irq_release_resources() 对称归还；调用者持 request_mutex。
 */
static int irq_request_resources(struct irq_desc *desc)
{
	struct irq_data *d = &desc->irq_data;
	struct irq_chip *c = d->chip;

	return c->irq_request_resources ? c->irq_request_resources(d) : 0;
}

/*
 * irq_release_resources() - 对称释放 irqchip 申请阶段取得的资源
 *
 * @desc: 最后 action 已移除、chip/data 仍存活的描述符。
 * 回调存在时调用，无返回值；必须只对成功 request 的生命周期调用一次。
 */
static void irq_release_resources(struct irq_desc *desc)
{
	struct irq_data *d = &desc->irq_data;
	struct irq_chip *c = d->chip;

	if (c->irq_release_resources)
		c->irq_release_resources(d);
}

/*
 * irq_supports_nmi() - 检查描述符能否安全切换为 NMI
 *
 * @desc: 已 setup chip 的只读借用描述符。
 * 层级 domain 中只允许 root irqchip 直接管理的 IRQ；存在可能睡眠的 bus lock 也拒绝；
 * 最终要求 IRQCHIP_SUPPORTS_NMI。返回 bool，无锁、无副作用，调用者保证配置稳定。
 */
static bool irq_supports_nmi(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	/* Only IRQs directly managed by the root irqchip can be set as NMI */
	/* 只有根 irqchip 直接管理、没有 parent_data 的 IRQ 才可设置为 NMI。 */
	if (d->parent_data)
		return false;
#endif
	/* Don't support NMIs for chips behind a slow bus */
	/* 慢总线 chip 的 lock/unlock 可能睡眠，与 NMI 上下文不兼容。 */
	if (d->chip->irq_bus_lock || d->chip->irq_bus_sync_unlock)
		return false;

	return d->chip->flags & IRQCHIP_SUPPORTS_NMI;
}

/*
 * irq_nmi_setup() - 让 irqchip 为 NMI 交付模式做专用建立
 *
 * @desc: 申请路径持锁保护的描述符。回调存在则原样返回结果，否则 -EINVAL；成功后必须
 * 由 irq_nmi_teardown() 对称撤销。本函数本身不设置 desc 的 NMI 状态位。
 */
static int irq_nmi_setup(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);
	struct irq_chip *c = d->chip;

	return c->irq_nmi_setup ? c->irq_nmi_setup(d) : -EINVAL;
}

/*
 * irq_nmi_teardown() - 撤销 irqchip 的 NMI 专用建立
 *
 * @desc: 仍存活且此前 NMI setup 成功的描述符。回调存在时调用；无返回值，核心随后继续
 * 普通资源释放。调用者保证没有 NMI handler 正在运行。
 */
static void irq_nmi_teardown(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);
	struct irq_chip *c = d->chip;

	if (c->irq_nmi_teardown)
		c->irq_nmi_teardown(d);
}

/*
 * setup_irq_thread() - 创建但暂不运行一个 action 对应的 IRQ kthread
 *
 * @new: 尚未发布的 action；成功后 new->thread 持有 task_struct 引用并置 AFFINITY 请求。
 * @irq: 用于线程名和 action 身份的逻辑号。
 * @secondary: false 创建 irq/N-name，true 创建 irq/N-s-name。
 * kthread_create 失败返回 PTR_ERR；成功取得额外 task 引用，先绑定 cpu_possible_mask 阻止
 * cpuset/housekeeping 单独重绑，实际 effective affinity 待线程启动后自行应用。返回 0；
 * 调用者负责唤醒、停止以及 put_task_struct。
 */
static int
setup_irq_thread(struct irqaction *new, unsigned int irq, bool secondary)
{
	struct task_struct *t;

	if (!secondary) {
		t = kthread_create(irq_thread, new, "irq/%d-%s", irq,
				   new->name);
	} else {
		t = kthread_create(irq_thread, new, "irq/%d-s-%s", irq,
				   new->name);
	}

	if (IS_ERR(t))
		return PTR_ERR(t);

	/*
	 * We keep the reference to the task struct even if
	 * the thread dies to avoid that the interrupt code
	 * references an already freed task_struct.
	 */
	/*
	 * 即使线程意外死亡，也保留 task_struct 引用，避免中断路径访问已释放对象；
	 * 最终由 action 释放路径 put。
	 */
	new->thread = get_task_struct(t);

	/*
	 * The affinity can not be established yet, but it will be once the
	 * interrupt is enabled. Delay and defer the actual setting to the
	 * thread itself once it is ready to run. In the meantime, prevent
	 * it from ever being re-affined directly by cpuset or
	 * housekeeping. The proper way to do it is to re-affine the whole
	 * vector.
	 */
	/*
	 * 此时 IRQ 尚未 enable，无法确定实际 affinity；先把线程绑到全部 possible
	 * CPU，并阻止 cpuset/housekeeping 独立重新绑定。正确调整单位是整个中断向量，真正
	 * affinity 在 IRQ 启用、线程可运行后由线程自身完成。
	 */
	kthread_bind_mask(t, cpu_possible_mask);

	/*
	 * Ensure the thread adjusts the affinity once it reaches the
	 * thread function.
	 */
	/* 预置请求位，保证线程一进入等待循环就按 desc 的 effective mask 调整。 */
	set_bit(IRQTF_AFFINITY, &new->thread_flags);

	return 0;
}

/*
 * valid_percpu_irqaction() - 验证新的 percpu action 能否与已有链共存
 *
 * @old: 非 NULL 的现有 action 链首，调用者持有阻止链修改的锁。
 * @new: 尚未发布的新 action。
 * 只有 new->affinity 与每个 old->affinity 均不相交，且 percpu_dev_id 指针均不同才返回
 * true；任一冲突返回 false。函数只借用指针、不修改链，规则保证每 CPU 上设备身份唯一。
 */
static bool valid_percpu_irqaction(struct irqaction *old, struct irqaction *new)
{
	do {
		if (cpumask_intersects(old->affinity, new->affinity) ||
		    old->percpu_dev_id == new->percpu_dev_id)
			return false;

		old = old->next;
	} while (old);

	return true;
}

/*
 * Internal function to register an irqaction - typically used to
 * allocate special interrupts that are part of the architecture.
 *
 * Locking rules:
 *
 * desc->request_mutex	Provides serialization against a concurrent free_irq()
 *   chip_bus_lock	Provides serialization for slow bus operations
 *     desc->lock	Provides serialization against hard interrupts
 *
 * chip_bus_lock and desc->lock are sufficient for all other management and
 * interrupt related functions. desc->request_mutex solely serializes
 * request/free_irq().
 */
/*
 * 内部注册 irqaction，常用于架构特殊中断。锁序严格为 request_mutex（串行
 * request/free 和 action/线程所有权）-> chip_bus_lock（慢总线事务）-> desc->lock（与
 * hardirq 及普通管理操作串行）；后两把锁足以保护其他管理路径，request_mutex 专用于
 * 申请/释放的长生命周期事务。
 *
 * @irq/@desc 标识目标；@new 是调用者已分配、尚未发布的 action。成功时函数取得 desc
 * owner 模块引用，必要时创建并启动线程、取得首 action 的 chip 资源、把 action 发布到
 * desc 链、安装 PM/proc 状态并返回 0；从此 @new 归 IRQ 核心，直到 free。失败时停止已建
 * 线程、释放仅由本次首 action 取得的资源和模块引用，但不释放 @new/secondary 内存，交
 * 给调用者。返回 -EINVAL/-ENOSYS/-ENODEV/-ENOMEM/-EBUSY 或底层回调错误。
 */
static int
__setup_irq(unsigned int irq, struct irq_desc *desc, struct irqaction *new)
{
	/* old_ptr 是 action 链尾发布位置；thread_mask 汇总共享 oneshot 已占位。 */
	struct irqaction *old, **old_ptr;
	unsigned long flags, thread_mask = 0;
	int ret, nested, shared = 0;
	bool per_cpu_devid;

	if (!desc)
		return -EINVAL;

	if (desc->irq_data.chip == &no_irq_chip)
		return -ENOSYS;
	if (!try_module_get(desc->owner))
		return -ENODEV;

	per_cpu_devid = irq_settings_is_per_cpu_devid(desc);

	new->irq = irq;

	/*
	 * If the trigger type is not specified by the caller,
	 * then use the default for this interrupt.
	 */
	/* 申请者未指定触发类型时，继承 irq_data 中已配置的默认类型。 */
	if (!(new->flags & IRQF_TRIGGER_MASK))
		new->flags |= irqd_get_trigger_type(&desc->irq_data);

	/*
	 * IRQF_ONESHOT means the interrupt source in the IRQ chip will be
	 * masked until the threaded handled is done. If there is no thread
	 * handler then it makes no sense to have IRQF_ONESHOT.
	 */
	/*
	 * ONESHOT 会一直 mask 硬件源直到线程完成；没有 thread_fn 时该语义无意义，
	 * 因此告警（但保留后续统一校验/失败处理）。
	 */
	WARN_ON_ONCE(new->flags & IRQF_ONESHOT && !new->thread_fn);

	/*
	 * Check whether the interrupt nests into another interrupt
	 * thread.
	 */
	/* 判断此 IRQ 是否嵌套在另一个中断线程内，由父线程代为运行 thread_fn。 */
	nested = irq_settings_is_nested_thread(desc);
	if (nested) {
		if (!new->thread_fn) {
			ret = -EINVAL;
			goto out_mput;
		}
		/*
		 * Replace the primary handler which was provided from
		 * the driver for non nested interrupt handling by the
		 * dummy function which warns when called.
		 */
		/*
		 * nested 模式不会创建独立 kthread；把驱动为非嵌套场景提供的 primary
		 * 替换成告警哨兵，任何 hardirq 调用都表示层级分发错误。
		 */
		new->handler = irq_nested_primary_handler;
	} else {
		if (irq_settings_can_thread(desc)) {
			ret = irq_setup_forced_threading(new);
			if (ret)
				goto out_mput;
		}
	}

	/*
	 * Create a handler thread when a thread function is supplied
	 * and the interrupt does not nest into another interrupt
	 * thread.
	 */
	/*
	 * 仅非 nested 且提供 thread_fn 时创建独立主线程；forced 双 action 再创建
	 * secondary 线程。两者此时均未唤醒，失败可在发布前安全停止并回滚。
	 */
	if (new->thread_fn && !nested) {
		ret = setup_irq_thread(new, irq, false);
		if (ret)
			goto out_mput;
		if (new->secondary) {
			ret = setup_irq_thread(new->secondary, irq, true);
			if (ret)
				goto out_thread;
		}
	}

	/*
	 * Drivers are often written to work w/o knowledge about the
	 * underlying irq chip implementation, so a request for a
	 * threaded irq without a primary hard irq context handler
	 * requires the ONESHOT flag to be set. Some irq chips like
	 * MSI based interrupts are per se one shot safe. Check the
	 * chip flags, so we can avoid the unmask dance at the end of
	 * the threaded handler for those.
	 */
	/*
	 * 驱动通常不了解底层 chip；handler=NULL 的线程 IRQ 一般必须 ONESHOT，
	 * 防止线程清除设备前线路重新触发。但 MSI 等 IRQCHIP_ONESHOT_SAFE 源天然保证一次性，
	 * 可清掉软件 ONESHOT，省去线程末尾 mask/unmask 协调。
	 */
	if (desc->irq_data.chip->flags & IRQCHIP_ONESHOT_SAFE)
		new->flags &= ~IRQF_ONESHOT;

	/*
	 * Protects against a concurrent __free_irq() call which might wait
	 * for synchronize_hardirq() to complete without holding the optional
	 * chip bus lock and desc->lock. Also protects against handing out
	 * a recycled oneshot thread_mask bit while it's still in use by
	 * its previous owner.
	 */
	/*
	 * request_mutex 防止并发 __free_irq() 在不持 bus/desc 锁的同步阶段与本次
	 * 发布交错，也防止旧 oneshot 线程仍使用某 thread_mask bit 时该 bit 被新 action 复用。
	 */
	mutex_lock(&desc->request_mutex);

	/*
	 * Acquire bus lock as the irq_request_resources() callback below
	 * might rely on the serialization or the magic power management
	 * functions which are abusing the irq_bus_lock() callback,
	 */
	/*
	 * 资源申请回调可能依赖 bus 序列化，一些电源管理路径也借用了 irq_bus_lock
	 * 回调语义，因此在调用 irq_request_resources() 前先取得它。
	 */
	chip_bus_lock(desc);

	/* First installed action requests resources. */
	/* 仅 action 链从空变为非空的第一个处理器为整条 IRQ 请求 chip 资源。 */
	if (!desc->action) {
		ret = irq_request_resources(desc);
		if (ret) {
			pr_err("Failed to request resources for %s (irq %d) on irqchip %s\n",
			       new->name, irq, desc->irq_data.chip->name);
			goto out_bus_unlock;
		}
	}

	/*
	 * The following block of code has to be executed atomically
	 * protected against a concurrent interrupt and any of the other
	 * management calls which are not serialized via
	 * desc->request_mutex or the optional bus lock.
	 */
	/*
	 * 以下状态发布必须相对并发 hardirq 以及未由 request_mutex/bus lock 串行的
	 * 管理 API 原子执行，因此关闭本地 IRQ 并取得 desc->lock。
	 */
	raw_spin_lock_irqsave(&desc->lock, flags);
	old_ptr = &desc->action;
	old = *old_ptr;
	if (old) {
		/*
		 * Can't share interrupts unless both agree to and are
		 * the same type (level, edge, polarity). So both flag
		 * fields must have IRQF_SHARED set and the bits which
		 * set the trigger type must match. Also all must
		 * agree on ONESHOT.
		 * Interrupt lines used for NMIs cannot be shared.
		 */
		/*
		 * 共享要求新旧 action 都声明 SHARED、触发电平/边沿/极性一致，并对
		 * ONESHOT 达成一致；NMI 线路不可普通共享。percpu_devid 另按 affinity 和身份校验。
		 */
		unsigned int oldtype;

		if (irq_is_nmi(desc) && !per_cpu_devid) {
			pr_err("Invalid attempt to share NMI for %s (irq %d) on irqchip %s.\n",
				new->name, irq, desc->irq_data.chip->name);
			ret = -EINVAL;
			goto out_unlock;
		}

		if (per_cpu_devid && !valid_percpu_irqaction(old, new)) {
			pr_err("Overlapping affinities for %s (irq %d) on irqchip %s.\n",
				new->name, irq, desc->irq_data.chip->name);
			ret = -EINVAL;
			goto out_unlock;
		}

		/*
		 * If nobody did set the configuration before, inherit
		 * the one provided by the requester.
		 */
		/* 此前无人固定 irq_data 触发配置时，以本次请求类型作为比较基准并记录。 */
		if (irqd_trigger_type_was_set(&desc->irq_data)) {
			oldtype = irqd_get_trigger_type(&desc->irq_data);
		} else {
			oldtype = new->flags & IRQF_TRIGGER_MASK;
			irqd_set_trigger_type(&desc->irq_data, oldtype);
		}

		if (!((old->flags & new->flags) & IRQF_SHARED) ||
		    (oldtype != (new->flags & IRQF_TRIGGER_MASK)))
			goto mismatch;

		if ((old->flags & IRQF_ONESHOT) &&
		    (new->flags & IRQF_COND_ONESHOT))
			new->flags |= IRQF_ONESHOT;
		else if ((old->flags ^ new->flags) & IRQF_ONESHOT)
			goto mismatch;

		/* All handlers must agree on per-cpuness */
		/* 同一共享线路上的所有 handler 必须一致同意是否采用 per-CPU 语义。 */
		if ((old->flags & IRQF_PERCPU) !=
		    (new->flags & IRQF_PERCPU))
			goto mismatch;

		/* add new interrupt at end of irq queue */
		/* 沿 next 走到链尾，保持注册顺序追加新 action。 */
		do {
			/*
			 * Or all existing action->thread_mask bits,
			 * so we can find the next zero bit for this
			 * new action.
			 */
			/* 汇总所有现有 action 的 thread_mask，以寻找新 action 的首个空闲 bit。 */
			thread_mask |= old->thread_mask;
			old_ptr = &old->next;
			old = *old_ptr;
		} while (old);
		shared = 1;
	}

	/*
	 * Setup the thread mask for this irqaction for ONESHOT. For
	 * !ONESHOT irqs the thread mask is 0 so we can avoid a
	 * conditional in irq_wake_thread().
	 */
	/*
	 * 为 ONESHOT action 分配唯一线程位；非 ONESHOT 保持 0，使唤醒热路径无需
	 * 额外分支。该位标记的是共享线路中哪个线程尚未完成。
	 */
	if (new->flags & IRQF_ONESHOT) {
		/*
		 * Unlikely to have 32 resp 64 irqs sharing one line,
		 * but who knows.
		 */
		/* 虽极少有 32/64 个 action 共享线路，但位图用尽时必须以 -EBUSY 拒绝。 */
		if (thread_mask == ~0UL) {
			ret = -EBUSY;
			goto out_unlock;
		}
		/*
		 * The thread_mask for the action is or'ed to
		 * desc->thread_active to indicate that the
		 * IRQF_ONESHOT thread handler has been woken, but not
		 * yet finished. The bit is cleared when a thread
		 * completes. When all threads of a shared interrupt
		 * line have completed desc->threads_active becomes
		 * zero and the interrupt line is unmasked. See
		 * handle.c:irq_wake_thread() for further information.
		 *
		 * If no thread is woken by primary (hard irq context)
		 * interrupt handlers, then desc->threads_active is
		 * also checked for zero to unmask the irq line in the
		 * affected hard irq flow handlers
		 * (handle_[fasteoi|level]_irq).
		 *
		 * The new action gets the first zero bit of
		 * thread_mask assigned. See the loop above which or's
		 * all existing action->thread_mask bits.
		 */
		/*
		 * action->thread_mask 在唤醒时 OR 入 desc->threads_oneshot，在线程完成时
		 * 清除；共享线路所有位都清零后才可 unmask。若 hardirq 没唤醒任何线程，fasteoi/
		 * level flow handler 也检查该位图并解 mask。新 action 使用前面汇总位图的首个零位。
		 * 注意它与 atomic threads_active 含义不同：前者控制 oneshot 硬件 mask，后者供同步
		 * 等待线程执行次数归零。
		 */
		new->thread_mask = 1UL << ffz(thread_mask);

	} else if (new->handler == irq_default_primary_handler &&
		   !(desc->irq_data.chip->flags & IRQCHIP_ONESHOT_SAFE)) {
		/*
		 * The interrupt was requested with handler = NULL, so
		 * we use the default primary handler for it. But it
		 * does not have the oneshot flag set. In combination
		 * with level interrupts this is deadly, because the
		 * default primary handler just wakes the thread, then
		 * the irq lines is reenabled, but the device still
		 * has the level irq asserted. Rinse and repeat....
		 *
		 * While this works for edge type interrupts, we play
		 * it safe and reject unconditionally because we can't
		 * say for sure which type this interrupt really
		 * has. The type flags are unreliable as the
		 * underlying chip implementation can override them.
		 */
		/*
		 * handler=NULL 会采用只唤醒线程的默认 primary；若没有 ONESHOT，level
		 * 源会在线程清除设备前重新 enable，形成无休止重复中断。edge 虽可能可用，但核心
		 * 无法信任可能被 chip 覆盖的类型 flags，因此除天然 ONESHOT_SAFE 外一律拒绝。
		 */
		pr_err("Threaded irq requested with handler=NULL and !ONESHOT for %s (irq %d)\n",
		       new->name, irq);
		ret = -EINVAL;
		goto out_unlock;
	}

	if (!shared) {
		/* Setup the type (level, edge polarity) if configured: */
		/* 首 action 若显式给出触发类型，先让 chip 配置并同步 desc 状态。 */
		if (new->flags & IRQF_TRIGGER_MASK) {
			ret = __irq_set_trigger(desc,
						new->flags & IRQF_TRIGGER_MASK);

			if (ret)
				goto out_unlock;
		}

		/*
		 * Activate the interrupt. That activation must happen
		 * independently of IRQ_NOAUTOEN. request_irq() can fail
		 * and the callers are supposed to handle
		 * that. enable_irq() of an interrupt requested with
		 * IRQ_NOAUTOEN is not supposed to fail. The activation
		 * keeps it in shutdown mode, it merily associates
		 * resources if necessary and if that's not possible it
		 * fails. Interrupts which are in managed shutdown mode
		 * will simply ignore that activation request.
		 */
		/*
		 * domain activation 与 NOAUTOEN 无关，必须在 request 时完成资源关联并让
		 * 可能失败的错误返回申请者；NOAUTOEN 后续 enable_irq() 不应再承担可失败 activation。
		 * activation 仍保持 shutdown，不等同 startup；managed shutdown 会忽略该请求。
		 */
		ret = irq_activate(desc);
		if (ret)
			goto out_unlock;

		desc->istate &= ~(IRQS_AUTODETECT | IRQS_SPURIOUS_DISABLED | \
				  IRQS_ONESHOT | IRQS_WAITING);
		irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);

		if (new->flags & IRQF_PERCPU) {
			irqd_set(&desc->irq_data, IRQD_PER_CPU);
			irq_settings_set_per_cpu(desc);
			if (new->flags & IRQF_NO_DEBUG)
				irq_settings_set_no_debug(desc);
		}

		if (noirqdebug)
			irq_settings_set_no_debug(desc);

		if (new->flags & IRQF_ONESHOT)
			desc->istate |= IRQS_ONESHOT;

		/* Exclude IRQ from balancing if requested */
		/* NOBALANCING 同时写入 desc settings 与 irq_data 状态，排除自动均衡。 */
		if (new->flags & IRQF_NOBALANCING) {
			irq_settings_set_no_balancing(desc);
			irqd_set(&desc->irq_data, IRQD_NO_BALANCING);
		}

		if (!(new->flags & IRQF_NO_AUTOEN) &&
		    irq_settings_can_autoenable(desc)) {
			irq_startup(desc, IRQ_RESEND, IRQ_START_COND);
		} else if (!per_cpu_devid) {
			/*
			 * Shared interrupts do not go well with disabling
			 * auto enable. The sharing interrupt might request
			 * it while it's still disabled and then wait for
			 * interrupts forever.
			 */
			/*
			 * 共享 IRQ 不适合关闭自动 enable；后加入的共享者可能在整条线路仍禁用时
			 * 等待永远不会到来的中断。因此告警；非 percpu_devid 情况把嵌套 depth 规范为 1，
			 * 表示恰好一次 enable 即可启动。
			 */
			WARN_ON_ONCE(new->flags & IRQF_SHARED);
			/* Undo nested disables: */
			/* 丢弃此前累计的嵌套禁用深度，统一保留一层“尚未启用”。 */
			desc->depth = 1;
		}

	} else if (new->flags & IRQF_TRIGGER_MASK) {
		unsigned int nmsk = new->flags & IRQF_TRIGGER_MASK;
		unsigned int omsk = irqd_get_trigger_type(&desc->irq_data);

		if (nmsk != omsk)
			/* hope the handler works with current  trigger mode */
			/* 共享线路不能重配，只能警告并希望新 handler 兼容当前触发模式。 */
			pr_warn("irq %d uses trigger mode %u; requested %u\n",
				irq, omsk, nmsk);
	}

	/* 至此是核心发布点：desc 锁内把完整初始化的 @new 链入 hardirq 可见 action 链。 */
	*old_ptr = new;

	irq_pm_install_action(desc, new);

	/* Reset broken irq detection when installing new handler */
	/* 安装新 handler 后清零异常/无人处理统计，给新处理逻辑重新观察的机会。 */
	desc->irq_count = 0;
	desc->irqs_unhandled = 0;

	/*
	 * Check whether we disabled the irq via the spurious handler
	 * before. Reenable it and give it another chance.
	 */
	/*
	 * 若 spurious 检测曾禁用这条共享 IRQ，新 action 可能修复无人处理问题；清除
	 * 标志并撤销那一层禁用，让线路获得一次重新工作的机会。
	 */
	if (shared && (desc->istate & IRQS_SPURIOUS_DISABLED)) {
		desc->istate &= ~IRQS_SPURIOUS_DISABLED;
		__enable_irq(desc);
	}

	irq_proc_update_valid(desc);
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	chip_bus_sync_unlock(desc);
	mutex_unlock(&desc->request_mutex);

	wake_up_and_wait_for_irq_thread_ready(desc, new);
	wake_up_and_wait_for_irq_thread_ready(desc, new->secondary);
	/* 锁外启动线程并等待 READY 后再注册 proc，保证公开后线程控制面已可用。 */

	register_irq_proc(irq, desc);
	new->dir = NULL;
	register_handler_proc(irq, new);
	return 0;

mismatch:
	/* PROBE_SHARED 的试探性冲突保持安静；普通冲突打印新旧 flags/name 便于定位。 */
	if (!(new->flags & IRQF_PROBE_SHARED)) {
		pr_err("Flags mismatch irq %d. %08x (%s) vs. %08x (%s)\n",
		       irq, new->flags, new->name, old->flags, old->name);
#ifdef CONFIG_DEBUG_SHIRQ
		dump_stack();
#endif
	}
	ret = -EBUSY;

out_unlock:
	raw_spin_unlock_irqrestore(&desc->lock, flags);

	/* 若链仍为空，说明本次是首 action 且发布失败，归还刚申请的整条 IRQ 资源。 */
	if (!desc->action)
		irq_release_resources(desc);
out_bus_unlock:
	chip_bus_sync_unlock(desc);
	mutex_unlock(&desc->request_mutex);

out_thread:
	/* 尚未发布的线程可直接 stop；kthread_stop_put 同时归还 setup 持有的 task 引用。 */
	if (new->thread) {
		struct task_struct *t = new->thread;

		new->thread = NULL;
		kthread_stop_put(t);
	}
	if (new->secondary && new->secondary->thread) {
		struct task_struct *t = new->secondary->thread;

		new->secondary->thread = NULL;
		kthread_stop_put(t);
	}
out_mput:
	module_put(desc->owner);
	return ret;
}

/*
 * Internal function to unregister an irqaction - used to free
 * regular and special interrupts that are part of the architecture.
 */
/*
 * 内部注销普通或架构特殊 irqaction。
 * @desc: 存活描述符；@dev_id: 与注册时完全相同的 action 身份。
 * 函数只能在进程上下文执行。它按 request_mutex -> bus lock -> desc lock 找到并先摘除
 * action；最后一个 action 会 shutdown 线路。随后放开自旋/总线锁但保留 request_mutex，
 * 注销 proc、完整同步 hardirq/硬件 in-flight/线程，停止 kthread；最后 action 时再 deactivate
 * 并释放 chip 资源，归还 PM/module 引用。成功返回仍由调用者释放的 action；找不到返回
 * NULL。该顺序保证硬中断不再取得节点后，旧读者才退出，绝不在仍可执行时释放内存。
 */
static struct irqaction *__free_irq(struct irq_desc *desc, void *dev_id)
{
	/* action_ptr 是待摘链槽位；request_mutex 保证搜索至清理完成期间链不被 request 改写。 */
	unsigned irq = desc->irq_data.irq;
	struct irqaction *action, **action_ptr;
	unsigned long flags;

	WARN(in_interrupt(), "Trying to free IRQ %d from IRQ context!\n", irq);

	mutex_lock(&desc->request_mutex);
	chip_bus_lock(desc);
	raw_spin_lock_irqsave(&desc->lock, flags);

	/*
	 * There can be multiple actions per IRQ descriptor, find the right
	 * one based on the dev_id:
	 */
	/* 一个 desc 可挂多个共享 action，必须用 dev_id 精确找到调用者拥有的节点。 */
	action_ptr = &desc->action;
	for (;;) {
		action = *action_ptr;

		if (!action) {
			WARN(1, "Trying to free already-free IRQ %d\n", irq);
			raw_spin_unlock_irqrestore(&desc->lock, flags);
			chip_bus_sync_unlock(desc);
			mutex_unlock(&desc->request_mutex);
			return NULL;
		}

		if (action->dev_id == dev_id)
			break;
		action_ptr = &action->next;
	}

	/* Found it - now remove it from the list of entries: */
	/* desc 锁内先摘链，自此新的 hardirq 遍历不再看见该 action。 */
	*action_ptr = action->next;

	irq_pm_remove_action(desc, action);

	/* If this was the last handler, shut down the IRQ line: */
	/* 最后一个 handler 离开时关闭线路，阻止产生新的正常分发。 */
	if (!desc->action) {
		irq_settings_clr_disable_unlazy(desc);
		/* Only shutdown. Deactivate after synchronize_hardirq() */
		/* 这里仅 shutdown；必须等在途 hardirq 后才能撤销 domain activation。 */
		irq_shutdown(desc);
	}

#ifdef CONFIG_SMP
	/* make sure affinity_hint is cleaned up */
	/* free 前调用者本应清除 affinity hint；遗留时告警并防御性断开借用指针。 */
	if (WARN_ON_ONCE(desc->affinity_hint))
		desc->affinity_hint = NULL;
#endif

	irq_proc_update_valid(desc);
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	/*
	 * Drop bus_lock here so the changes which were done in the chip
	 * callbacks above are synced out to the irq chips which hang
	 * behind a slow bus (I2C, SPI) before calling synchronize_hardirq().
	 *
	 * Aside of that the bus_lock can also be taken from the threaded
	 * handler in irq_finalize_oneshot() which results in a deadlock
	 * because kthread_stop() would wait forever for the thread to
	 * complete, which is blocked on the bus lock.
	 *
	 * The still held desc->request_mutex() protects against a
	 * concurrent request_irq() of this irq so the release of resources
	 * and timing data is properly serialized.
	 */
	/*
	 * 同步前释放 bus lock，一方面把 I2C/SPI 等慢总线上的 shutdown 写操作真正
	 * 刷到 chip；另一方面 IRQ 线程可能在 irq_finalize_oneshot() 获取同一 bus lock，若
	 * free 持锁调用 kthread_stop 就会互等。仍持 request_mutex 足以阻止并发 request 复用
	 * IRQ，并串行后续资源和统计数据清理。
	 */
	chip_bus_sync_unlock(desc);

	unregister_handler_proc(irq, action);

	/*
	 * Make sure it's not being used on another CPU and if the chip
	 * supports it also make sure that there is no (not yet serviced)
	 * interrupt in flight at the hardware level.
	 */
	/* 等其他 CPU 上旧 handler、IRQ 线程以及 chip 可报告的未服务硬件事件全部退出。 */
	__synchronize_irq(desc);

#ifdef CONFIG_DEBUG_SHIRQ
	/*
	 * It's a shared IRQ -- the driver ought to be prepared for an IRQ
	 * event to happen even now it's being freed, so let's make sure that
	 * is so by doing an extra call to the handler ....
	 *
	 * ( We do this after actually deregistering it, to make sure that a
	 *   'real' IRQ doesn't run in parallel with our fake. )
	 */
	/*
	 * DEBUG_SHIRQ 下，共享驱动必须能容忍释放边界仍出现一次事件，故在真实 action
	 * 已摘除并同步后人工调用 handler；本地关 IRQ 防止测试调用所处 CPU 的中断干扰。
	 */
	if (action->flags & IRQF_SHARED) {
		local_irq_save(flags);
		action->handler(irq, dev_id);
		local_irq_restore(flags);
	}
#endif

	/*
	 * The action has already been removed above, but the thread writes
	 * its oneshot mask bit when it completes. Though request_mutex is
	 * held across this which prevents __setup_irq() from handing out
	 * the same bit to a newly requested action.
	 */
	/*
	 * action 虽已摘链，线程完成时仍会写自己的 oneshot bit；跨 stop 全程持有
	 * request_mutex，阻止 __setup_irq() 把该 bit 提前分配给新 action。
	 */
	if (action->thread) {
		kthread_stop_put(action->thread);
		if (action->secondary && action->secondary->thread)
			kthread_stop_put(action->secondary->thread);
	}

	/* Last action releases resources */
	/* 只有 action 链已空时，才释放整条 IRQ 由首 action 取得的共享 chip 资源。 */
	if (!desc->action) {
		/*
		 * Reacquire bus lock as irq_release_resources() might
		 * require it to deallocate resources over the slow bus.
		 */
		/* 资源释放可能通过慢总线访问硬件，因此按既定锁序重新取得 bus lock。 */
		chip_bus_lock(desc);
		/*
		 * There is no interrupt on the fly anymore. Deactivate it
		 * completely.
		 */
		/* 同步已证明无在途中断，现在可在 desc 锁下彻底 deactivate domain 资源。 */
		scoped_guard(raw_spinlock_irqsave, &desc->lock)
			irq_domain_deactivate_irq(&desc->irq_data);

		irq_release_resources(desc);
		chip_bus_sync_unlock(desc);
	}

	mutex_unlock(&desc->request_mutex);

	irq_chip_pm_put(&desc->irq_data);
	module_put(desc->owner);
	kfree(action->secondary);
	return action;
}

/**
 * free_irq - free an interrupt allocated with request_irq
 * @irq:	Interrupt line to free
 * @dev_id:	Device identity to free
 *
 * Remove an interrupt handler. The handler is removed and if the interrupt
 * line is no longer in use by any driver it is disabled.  On a shared IRQ
 * the caller must ensure the interrupt is disabled on the card it drives
 * before calling this function. The function does not return until any
 * executing interrupts for this IRQ have completed.
 *
 * This function must not be called from interrupt context.
 *
 * Returns the devname argument passed to request_irq.
 */
/*
 * 移除 request_irq/request_threaded_irq 建立的 @dev_id 对应 handler；若它是
 * 最后使用者则禁用线路。共享 IRQ 的驱动必须先在设备侧阻止自己的中断源，再调用本函数。
 * 函数等待所有正在执行的该 IRQ hard/thread handler 完成，不能从中断上下文调用。
 *
 * 普通 IRQ 才适用，percpu_devid 拒绝。成功释放 action 内存并返回注册时 @devname 的借用
 * 指针（该字符串生命周期仍由驱动保证）；失败返回 NULL。SMP 下 affinity notifier 必须
 * 事先禁用，遗留会告警并清指针，但不会代替正确的 kref/work 注销协议。
 */
const void *free_irq(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action;
	const char *devname;

	if (!desc || WARN_ON(irq_settings_is_per_cpu_devid(desc)))
		return NULL;

#ifdef CONFIG_SMP
	/* 阶段 1：先切断违约遗留的 notifier 借用，避免后续 affinity 变化再调度它。 */
	if (WARN_ON(desc->affinity_notify))
		desc->affinity_notify = NULL;
#endif

	/* 阶段 2：内部事务负责摘链、同步全部执行者并归还 action 之外的资源。 */
	action = __free_irq(desc, dev_id);

	if (!action)
		return NULL;

	/* 阶段 3：先保存仍由驱动拥有的名称指针，再释放核心拥有的 action 外壳。 */
	devname = action->name;
	kfree(action);
	return devname;
}
EXPORT_SYMBOL(free_irq);

/*
 * __cleanup_nmi() - 在 NMI 已禁用且同步责任满足后拆除唯一 action
 *
 * @irq/@desc: 已验证为 NMI 的逻辑号和存活描述符。
 * 在 desc irqsave 锁下调用 chip teardown、清 IRQS_NMI、摘唯一 action/PM 状态并 shutdown+
 * deactivate；锁外更新 proc、释放 action 和 chip 资源，归还 PM/module 引用。返回 devname
 * 借用指针或异常无 action 时 NULL。NMI 不共享且无 IRQ kthread，因此不走 __free_irq()。
 */
static const void *__cleanup_nmi(unsigned int irq, struct irq_desc *desc)
{
	struct irqaction *action = NULL;
	const char *devname = NULL;

	/* 阶段 1：在 desc 锁内撤销 NMI 模式、摘除唯一 action 并关闭 domain 生命周期。 */
	scoped_guard(raw_spinlock_irqsave, &desc->lock) {
		irq_nmi_teardown(desc);

		desc->istate &= ~IRQS_NMI;

		if (!WARN_ON(desc->action == NULL)) {
			action = desc->action;
			irq_pm_remove_action(desc, action);
			devname = action->name;
		}
		desc->action = NULL;

		irq_settings_clr_disable_unlazy(desc);
		irq_shutdown_and_deactivate(desc);
	}

	/* 阶段 2：action 已不可达，锁外拆除 proc 和对象内存，再归还共享外围资源。 */
	irq_proc_update_valid(desc);

	if (action)
		unregister_handler_proc(irq, action);
	kfree(action);

	irq_release_resources(desc);

	irq_chip_pm_put(&desc->irq_data);
	module_put(desc->owner);

	return devname;
}

/*
 * free_nmi() - 释放 request_nmi() 建立的 NMI action
 *
 * @irq: NMI 逻辑号；@dev_id: API 对称参数，但 NMI 唯一 action 清理不靠它查找。
 * 无效、非 NMI 或 percpu_devid 返回 NULL。NMI 若仍 enable（depth==0）属于调用方违约，
 * 告警并执行 nosync disable 后清理；正确调用者应先 disable_nmi_nosync() 并自行保证所有
 * CPU 不再执行 handler。成功返回注册 devname，资源与 action 已释放。
 */
const void *free_nmi(unsigned int irq, void *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || WARN_ON(!irq_is_nmi(desc)))
		return NULL;

	if (WARN_ON(irq_settings_is_per_cpu_devid(desc)))
		return NULL;

	/* NMI still enabled */
	/* free 时 NMI 仍处于 depth==0 的启用状态，属于调用者生命周期违约。 */
	if (WARN_ON(desc->depth == 0))
		disable_nmi_nosync(irq);

	return __cleanup_nmi(irq, desc);
}

/**
 * request_threaded_irq - allocate an interrupt line
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 *		Primary handler for threaded interrupts.
 *		If handler is NULL and thread_fn != NULL
 *		the default primary handler is installed.
 * @thread_fn:	Function called from the irq handler thread
 *		If NULL, no irq thread is created
 * @irqflags:	Interrupt type flags
 * @devname:	An ascii name for the claiming device
 * @dev_id:	A cookie passed back to the handler function
 *
 * This call allocates interrupt resources and enables the interrupt line
 * and IRQ handling. From the point this call is made your handler function
 * may be invoked. Since your handler function must clear any interrupt the
 * board raises, you must take care both to initialise your hardware and to
 * set up the interrupt handler in the right order.
 *
 * If you want to set up a threaded irq handler for your device then you
 * need to supply @handler and @thread_fn. @handler is still called in hard
 * interrupt context and has to check whether the interrupt originates from
 * the device. If yes it needs to disable the interrupt on the device and
 * return IRQ_WAKE_THREAD which will wake up the handler thread and run
 * @thread_fn. This split handler design is necessary to support shared
 * interrupts.
 *
 * @dev_id must be globally unique. Normally the address of the device data
 * structure is used as the cookie. Since the handler receives this value
 * it makes sense to use it.
 *
 * If your interrupt is shared you must pass a non NULL dev_id as this is
 * required when freeing the interrupt.
 *
 * Flags:
 *
 *	IRQF_SHARED		Interrupt is shared
 *	IRQF_TRIGGER_*		Specify active edge(s) or level
 *	IRQF_ONESHOT		Run thread_fn with interrupt line masked
 */
/*
 * 申请并通常启用 @irq。@handler 在 hardirq 上下文运行；@thread_fn 非 NULL 时
 * 由 IRQ kthread 调用，handler 应确认事件来自本设备、在设备侧抑制源并返回
 * IRQ_WAKE_THREAD。handler=NULL+thread_fn 非 NULL 会安装默认 primary。此拆分是共享 IRQ
 * 能先判定归属、再睡眠处理的基础。
 *
 * @irqflags 描述共享、触发类型与 ONESHOT；@devname 是诊断/proc 用长期字符串；@dev_id
 * 原样回传且应全局唯一，共享 IRQ 必须非 NULL，free 时以它识别 action。调用前硬件及
 * handler 所依赖状态必须已初始化，因为成功发布后可立即进中断。成功返回 0，action、
 * module/PM/chip 资源归核心；失败返回 errno 并完整回滚本层分配。
 */
int request_threaded_irq(unsigned int irq, irq_handler_t handler,
			 irq_handler_t thread_fn, unsigned long irqflags,
			 const char *devname, void *dev_id)
{
	struct irqaction *action;
	struct irq_desc *desc;
	int retval;

	if (irq == IRQ_NOTCONNECTED)
		return -ENOTCONN;

	/*
	 * Sanity-check: shared interrupts must pass in a real dev-ID,
	 * otherwise we'll have trouble later trying to figure out
	 * which interrupt is which (messes up the interrupt freeing
	 * logic etc).
	 *
	 * Also shared interrupts do not go well with disabling auto enable.
	 * The sharing interrupt might request it while it's still disabled
	 * and then wait for interrupts forever.
	 *
	 * Also IRQF_COND_SUSPEND only makes sense for shared interrupts and
	 * it cannot be set along with IRQF_NO_SUSPEND.
	 */
	/*
	 * 共享 action 没有真实 dev_id 就无法在 free 时区分；共享与 NO_AUTOEN 组合
	 * 可能让后来申请者在禁用线路上永久等待。COND_SUSPEND 只对共享线路有意义，且不能与
	 * NO_SUSPEND 同时声明。任一非法组合直接返回 -EINVAL，不取得资源。
	 */
	if (((irqflags & IRQF_SHARED) && !dev_id) ||
	    ((irqflags & IRQF_SHARED) && (irqflags & IRQF_NO_AUTOEN)) ||
	    (!(irqflags & IRQF_SHARED) && (irqflags & IRQF_COND_SUSPEND)) ||
	    ((irqflags & IRQF_NO_SUSPEND) && (irqflags & IRQF_COND_SUSPEND)))
		return -EINVAL;

	desc = irq_to_desc(irq);
	if (!desc)
		return -EINVAL;

	if (!irq_settings_can_request(desc) ||
	    WARN_ON(irq_settings_is_per_cpu_devid(desc)))
		return -EINVAL;

	if (!handler) {
		if (!thread_fn)
			return -EINVAL;
		handler = irq_default_primary_handler;
	}

	action = kzalloc_obj(struct irqaction);
	/* action 由本函数拥有到 __setup_irq() 成功；成功后所有权转给 desc action 链。 */
	if (!action)
		return -ENOMEM;

	action->handler = handler;
	action->thread_fn = thread_fn;
	action->flags = irqflags;
	action->name = devname;
	action->dev_id = dev_id;

	retval = irq_chip_pm_get(&desc->irq_data);
	/* PM get 成功后，无论 setup 成败都必须由 free 或下方失败路径恰好 put 一次。 */
	if (retval < 0) {
		kfree(action);
		return retval;
	}

	retval = __setup_irq(irq, desc, action);

	if (retval) {
		/* __setup_irq() 保留 action/secondary 内存给调用者回收，但已自行回滚线程/module。 */
		irq_chip_pm_put(&desc->irq_data);
		kfree(action->secondary);
		kfree(action);
	}

#ifdef CONFIG_DEBUG_SHIRQ_FIXME
	if (!retval && (irqflags & IRQF_SHARED)) {
		/*
		 * It's a shared IRQ -- the driver ought to be prepared for it
		 * to happen immediately, so let's make sure....
		 * We disable the irq to make sure that a 'real' IRQ doesn't
		 * run in parallel with our fake.
		 */
		/*
		 * 调试配置下，共享 handler 应能容忍申请后立即发生事件，故禁用真实线路、
		 * 关闭本地 IRQ 后人工调用一次，再逆序恢复，以避免真实 IRQ 与测试 handler 并行。
		 */
		unsigned long flags;

		disable_irq(irq);
		local_irq_save(flags);

		handler(irq, dev_id);

		local_irq_restore(flags);
		enable_irq(irq);
	}
#endif
	return retval;
}
EXPORT_SYMBOL(request_threaded_irq);

/**
 * request_any_context_irq - allocate an interrupt line
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 *		Threaded handler for threaded interrupts.
 * @flags:	Interrupt type flags
 * @name:	An ascii name for the claiming device
 * @dev_id:	A cookie passed back to the handler function
 *
 * This call allocates interrupt resources and enables the interrupt line
 * and IRQ handling. It selects either a hardirq or threaded handling
 * method depending on the context.
 *
 * Returns: On failure, it returns a negative value. On success, it returns either
 * IRQC_IS_HARDIRQ or IRQC_IS_NESTED.
 */
/*
 * 按目标 desc 的上下文模型自动选择普通 hardirq 或 nested threaded 申请。
 * nested 时把 @handler 作为 thread_fn 交给 request_threaded_irq(handler=NULL)，成功返回
 * IRQC_IS_NESTED；否则调用 request_irq，成功返回 IRQC_IS_HARDIRQ。失败保留负 errno。
 * 其余参数及所有权与对应 request API 相同，调用者可据成功类别判断 handler 运行上下文。
 */
int request_any_context_irq(unsigned int irq, irq_handler_t handler,
			    unsigned long flags, const char *name, void *dev_id)
{
	struct irq_desc *desc;
	int ret;

	if (irq == IRQ_NOTCONNECTED)
		return -ENOTCONN;

	desc = irq_to_desc(irq);
	if (!desc)
		return -EINVAL;

	/* 阶段 1：desc 的 nested 属性决定 handler 实际运行在父 IRQ 线程还是 hardirq。 */
	if (irq_settings_is_nested_thread(desc)) {
		ret = request_threaded_irq(irq, NULL, handler,
					   flags, name, dev_id);
		return !ret ? IRQC_IS_NESTED : ret;
	}

	/* 阶段 2：普通描述符沿 request_irq() 建立 hardirq action，并转换成功类别。 */
	ret = request_irq(irq, handler, flags, name, dev_id);
	return !ret ? IRQC_IS_HARDIRQ : ret;
}
EXPORT_SYMBOL_GPL(request_any_context_irq);

/**
 * request_nmi - allocate an interrupt line for NMI delivery
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 *		Threaded handler for threaded interrupts.
 * @irqflags:	Interrupt type flags
 * @name:	An ascii name for the claiming device
 * @dev_id:	A cookie passed back to the handler function
 *
 * This call allocates interrupt resources and enables the interrupt line
 * and IRQ handling. It sets up the IRQ line to be handled as an NMI.
 *
 * An interrupt line delivering NMIs cannot be shared and IRQ handling
 * cannot be threaded.
 *
 * Interrupt lines requested for NMI delivering must produce per cpu
 * interrupts and have auto enabling setting disabled.
 *
 * @dev_id must be globally unique. Normally the address of the device data
 * structure is used as the cookie. Since the handler receives this value
 * it makes sense to use it.
 *
 * If the interrupt line cannot be used to deliver NMIs, function will fail
 * and return a negative value.
 */
/*
 * 申请并把 @irq 配置成 NMI 交付。NMI 不可共享、不可线程化或用于 IRQ polling，
 * 必须是 IRQF_PERCPU，且描述符/flags 要保证不会普通 auto-enable；chip 还必须通过根层级、
 * 无慢 bus lock 和 SUPPORTS_NMI 能力检查。@handler 非 NULL，@dev_id 应全局唯一并原样回传。
 *
 * 函数分配 action，强制加入 NO_THREAD/NOBALANCING，取得 PM 后复用 __setup_irq() 建立但
 * 保持禁用；再在 desc 锁下置 IRQS_NMI 并调用 chip nmi_setup。成功返回 0，调用者之后按
 * CPU/线路显式 enable；任一步失败逆序清理并返回 errno，nmi_setup 失败统一为 -EINVAL。
 */
int request_nmi(unsigned int irq, irq_handler_t handler,
		unsigned long irqflags, const char *name, void *dev_id)
{
	struct irqaction *action;
	struct irq_desc *desc;
	int retval;

	/* 阶段 1：在分配任何对象前拒绝断线号、共享/轮询、非 per-CPU 和空 handler。 */
	if (irq == IRQ_NOTCONNECTED)
		return -ENOTCONN;

	/* NMI cannot be shared, used for Polling */
	/* NMI 既不能共享，也不能使用条件挂起或 IRQ polling 语义。 */
	if (irqflags & (IRQF_SHARED | IRQF_COND_SUSPEND | IRQF_IRQPOLL))
		return -EINVAL;

	if (!(irqflags & IRQF_PERCPU))
		return -EINVAL;

	if (!handler)
		return -EINVAL;

	desc = irq_to_desc(irq);

	if (!desc || (irq_settings_can_autoenable(desc) &&
	    !(irqflags & IRQF_NO_AUTOEN)) ||
	    !irq_settings_can_request(desc) ||
	    WARN_ON(irq_settings_is_per_cpu_devid(desc)) ||
	    !irq_supports_nmi(desc))
		return -EINVAL;

	/* 阶段 2：构造尚未发布的 action；直到 __setup_irq() 成功前都由本函数拥有。 */
	action = kzalloc(sizeof(struct irqaction), GFP_KERNEL);
	if (!action)
		return -ENOMEM;

	action->handler = handler;
	action->flags = irqflags | IRQF_NO_THREAD | IRQF_NOBALANCING;
	action->name = name;
	action->dev_id = dev_id;

	retval = irq_chip_pm_get(&desc->irq_data);
	if (retval < 0)
		goto err_out;

	retval = __setup_irq(irq, desc, action);
	if (retval)
		goto err_irq_setup;

	/* 阶段 3：普通 action 建立完成后，在 desc 锁内提交不可共享的 NMI 身份和 chip 模式。 */
	scoped_guard(raw_spinlock_irqsave, &desc->lock) {
		/* Setup NMI state */
		/* 在 desc 锁内先发布 NMI 状态，再让 chip 建立 NMI 模式。 */
		desc->istate |= IRQS_NMI;
		retval = irq_nmi_setup(desc);
	}

	if (retval) {
		__cleanup_nmi(irq, desc);
		return -EINVAL;
	}
	return 0;

err_irq_setup:
	irq_chip_pm_put(&desc->irq_data);
err_out:
	kfree(action);

	return retval;
}

/*
 * enable_percpu_irq() - 在当前 CPU 上启用一个 percpu_devid IRQ
 *
 * @irq: 必须通过 PERCPU 类型检查的逻辑号。
 * @type: 可含触发类型；只保留 SENSE_MASK，NONE 时继承 irq_data 默认类型。
 * 函数在当前 CPU/desc 锁下必要时设置共享硬件触发类型，失败告警并保持未启用；成功调用
 * irq_percpu_enable(desc, smp_processor_id())，只改变当前 CPU 的 enabled 位和硬件状态。
 * 调用者必须处于不可迁移上下文，并用同 CPU disable_percpu_irq() 配平。
 */
void enable_percpu_irq(unsigned int irq, unsigned int type)
{
	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU) {
		struct irq_desc *desc = scoped_irqdesc;

		/*
		 * If the trigger type is not specified by the caller, then
		 * use the default for this interrupt.
		 */
		/* 调用者未指定 trigger 时，使用此 irq_data 已配置的默认类型。 */
		type &= IRQ_TYPE_SENSE_MASK;
		if (type == IRQ_TYPE_NONE)
			type = irqd_get_trigger_type(&desc->irq_data);

		if (type != IRQ_TYPE_NONE) {
			if (__irq_set_trigger(desc, type)) {
				WARN(1, "failed to set type for IRQ%d\n", irq);
				return;
			}
		}
		irq_percpu_enable(desc, smp_processor_id());
	}
}
EXPORT_SYMBOL_GPL(enable_percpu_irq);

/*
 * enable_percpu_nmi() - 在当前 CPU 启用 percpu NMI
 *
 * 参数、不可迁移要求和触发类型处理与 enable_percpu_irq() 相同；调用者须保证该 desc 已由
 * request_percpu_nmi() 配置为 NMI。无返回值，设置失败通过内部告警反映。
 */
void enable_percpu_nmi(unsigned int irq, unsigned int type)
{
	enable_percpu_irq(irq, type);
}

/**
 * irq_percpu_is_enabled - Check whether the per cpu irq is enabled
 * @irq:	Linux irq number to check for
 *
 * Must be called from a non migratable context. Returns the enable
 * state of a per cpu interrupt on the current cpu.
 */
/*
 * 查询 @irq 在当前 CPU 的 per-CPU enable 状态。必须从不可迁移上下文调用，
 * 否则取 smp_processor_id() 与测试位图之间可能换 CPU。有效 desc 在锁下测试
 * percpu_enabled；无效/类型不符返回 false。只读、无副作用。
 */
bool irq_percpu_is_enabled(unsigned int irq)
{
	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU)
		return cpumask_test_cpu(smp_processor_id(), scoped_irqdesc->percpu_enabled);
	return false;
}
EXPORT_SYMBOL_GPL(irq_percpu_is_enabled);

/*
 * disable_percpu_irq() - 仅在当前 CPU 禁用 percpu_devid IRQ
 *
 * @irq: PERCPU 类型逻辑号。锁内调用 irq_percpu_disable() 清当前 CPU enabled 位并下发
 * chip disable；无返回值。必须在不可迁移上下文调用，并在 free_percpu_irq() 前对 action
 * affinity 覆盖的每个 CPU 都完成禁用。
 */
void disable_percpu_irq(unsigned int irq)
{
	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU)
		irq_percpu_disable(scoped_irqdesc, smp_processor_id());
}
EXPORT_SYMBOL_GPL(disable_percpu_irq);

/*
 * disable_percpu_nmi() - 在当前 CPU 禁用 percpu NMI
 *
 * 复用 per-CPU IRQ 禁用状态机；调用者保证目标已配置为 NMI、当前上下文不可迁移。
 */
void disable_percpu_nmi(unsigned int irq)
{
	disable_percpu_irq(irq);
}

/*
 * Internal function to unregister a percpu irqaction.
 */
/*
 * 内部注销 @dev_id 对应的 percpu irqaction。
 * 调用者处于进程上下文，并按普通 percpu IRQ 需要持 chip bus lock。函数在 desc 锁下按
 * percpu_dev_id 找节点；若 action affinity 覆盖的任一 CPU 仍 enabled，则告警并返回 NULL，
 * 强制调用者先逐 CPU 禁用。成功摘链；最后 action 时清 IRQS_NMI 并更新 proc 有效性，
 * 锁外注销 handler proc、归还 PM/module 引用，返回 action 给调用者释放。线路资源由
 * percpu 描述符生命周期管理，此处不 shutdown/deactivate。
 */
static struct irqaction *__free_percpu_irq(unsigned int irq, void __percpu *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action, **action_ptr;

	WARN(in_interrupt(), "Trying to free IRQ %d from IRQ context!\n", irq);

	if (!desc)
		return NULL;

	scoped_guard(raw_spinlock_irqsave, &desc->lock) {
		/* 阶段 1：按 percpu_dev_id 搜索唯一 action；desc 锁稳定整条链。 */
		action_ptr = &desc->action;
		for (;;) {
			action = *action_ptr;

			if (!action) {
				WARN(1, "Trying to free already-free IRQ %d\n", irq);
				return NULL;
			}

			if (action->percpu_dev_id == dev_id)
				break;

			action_ptr = &action->next;
		}

		/* 阶段 2：目标 CPU 必须已全部禁用，handler 才不会再使用待释放 cookie。 */
		if (cpumask_intersects(desc->percpu_enabled, action->affinity)) {
			WARN(1, "percpu IRQ %d still enabled on CPU%d!\n", irq,
			     cpumask_first_and(desc->percpu_enabled, action->affinity));
			return NULL;
		}

		/* Found it - now remove it from the list of entries: */
		/* 所有目标 CPU 均已禁用后，desc 锁内摘除精确匹配的 action。 */
		*action_ptr = action->next;

		/* Demote from NMI if we killed the last action */
		/* 若移除最后 action，同时取消描述符的 NMI 身份并刷新 proc 可见性。 */
		if (!desc->action) {
			desc->istate &= ~IRQS_NMI;
			irq_proc_update_valid(desc);
		}
	}

	/* 阶段 3：action 已从并发查找域消失，锁外撤销控制面和注册期引用。 */
	unregister_handler_proc(irq, action);
	irq_chip_pm_put(&desc->irq_data);
	module_put(desc->owner);
	return action;
}

/**
 * free_percpu_irq - free an interrupt allocated with request_percpu_irq
 * @irq:	Interrupt line to free
 * @dev_id:	Device identity to free
 *
 * Remove a percpu interrupt handler. The handler is removed, but the
 * interrupt line is not disabled. This must be done on each CPU before
 * calling this function. The function does not return until any executing
 * interrupts for this IRQ have completed.
 *
 * This function must not be called from interrupt context.
 */
/*
 * 释放 request_percpu_irq*() 建立的 @dev_id action，但本函数不会替调用者禁用
 * 各 CPU 上的线路；必须先在 action 目标的每个 CPU 调 disable_percpu_irq()。不能从中断
 * 上下文调用。通过类型检查后，以 chip bus lock 包住内部摘链，并释放返回 action；无效
 * 或违约时为空操作/告警。API 要求返回前不再有执行中的对应 handler。
 */
void free_percpu_irq(unsigned int irq, void __percpu *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !irq_settings_is_per_cpu_devid(desc))
		return;

	chip_bus_lock(desc);
	kfree(__free_percpu_irq(irq, dev_id));
	chip_bus_sync_unlock(desc);
}
EXPORT_SYMBOL_GPL(free_percpu_irq);

/*
 * free_percpu_nmi() - 释放 percpu NMI action
 *
 * @irq/@dev_id 必须匹配 request_percpu_nmi()。要求调用者已在每个目标 CPU disable NMI；
 * 类型或 NMI 身份不符则返回/告警。NMI chip 不允许慢 bus lock，故直接调用内部摘链并释放
 * action。无返回值。
 */
void free_percpu_nmi(unsigned int irq, void __percpu *dev_id)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !irq_settings_is_per_cpu_devid(desc))
		return;

	if (WARN_ON(!irq_is_nmi(desc)))
		return;

	kfree(__free_percpu_irq(irq, dev_id));
}

/*
 * create_percpu_irqaction() - 构造尚未发布的 percpu action
 *
 * @handler/@devname/@dev_id: 处理函数、长期名称和 percpu cookie。
 * @flags: 额外 action flags；函数强制加入 PERCPU|NO_SUSPEND。
 * @affinity: 可空的长期存活目标掩码借用指针；NULL 使用 cpu_possible_mask。
 * 分配失败返回 NULL。非全 possible 掩码时加 SHARED，允许多个 action 以互不重叠 affinity
 * 分区共享同一 percpu_devid IRQ；对象在 setup 成功前归调用者，成功后归 desc 链。
 */
static
struct irqaction *create_percpu_irqaction(irq_handler_t handler, unsigned long flags,
					  const char *devname, const cpumask_t *affinity,
					  void __percpu *dev_id)
{
	struct irqaction *action;

	/* 阶段 1：把空 affinity 规范化为覆盖全部 possible CPU 的长期全局掩码。 */
	if (!affinity)
		affinity = cpu_possible_mask;

	/* 阶段 2：分配并填充仍由调用者拥有、尚不可被中断路径观察的 action。 */
	action = kzalloc_obj(struct irqaction);
	if (!action)
		return NULL;

	action->handler = handler;
	action->flags = flags | IRQF_PERCPU | IRQF_NO_SUSPEND;
	action->name = devname;
	action->percpu_dev_id = dev_id;
	action->affinity = affinity;

	/*
	 * We allow some form of sharing for non-overlapping affinity
	 * masks. Obviously, covering all CPUs prevents any sharing in
	 * the first place.
	 */
	/*
	 * 核心允许 affinity 互不重叠的有限共享；覆盖全部 possible CPU 的 action
	 * 必然与任何其他 action 相交，因此不标 SHARED，也从根本上排除共享。
	 */
	if (!cpumask_equal(affinity, cpu_possible_mask))
		action->flags |= IRQF_SHARED;

	return action;
}

/**
 * request_percpu_irq_affinity - allocate a percpu interrupt line
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 * @devname:	An ascii name for the claiming device
 * @affinity:	A cpumask describing the target CPUs for this interrupt
 * @dev_id:	A percpu cookie passed back to the handler function
 *
 * This call allocates interrupt resources, but doesn't enable the interrupt
 * on any CPU, as all percpu-devid interrupts are flagged with IRQ_NOAUTOEN.
 * It has to be done on each CPU using enable_percpu_irq().
 *
 * @dev_id must be globally unique. It is a per-cpu variable, and
 * the handler gets called with the interrupted CPU's instance of
 * that variable.
 */
/*
 * 为 @affinity 指定的 CPU 集合申请 percpu_devid IRQ action，但不在任何 CPU
 * 启用，因为该类 IRQ 固有 NOAUTOEN；调用者之后必须在每个目标 CPU 调 enable_percpu_irq()。
 * @dev_id 必须是非 NULL、全局唯一的 percpu 指针，handler 收到的是发生中断 CPU 对应实例。
 * @affinity 为 NULL 时覆盖全部 possible CPU；非空指针被 action 长期借用，须存活至 free。
 *
 * 函数验证 desc 类型/可申请性，构造 action、取得 chip PM 引用并交给 __setup_irq()。
 * 成功返回 0、所有权转给核心；失败返回 errno，并释放 action/PM 引用。
 */
int request_percpu_irq_affinity(unsigned int irq, irq_handler_t handler, const char *devname,
				const cpumask_t *affinity, void __percpu *dev_id)
{
	struct irqaction *action;
	struct irq_desc *desc;
	int retval;

	/* 阶段 1：先验证 percpu cookie 与描述符类型，尚不取得 PM 或 action 所有权。 */
	if (!dev_id)
		return -EINVAL;

	desc = irq_to_desc(irq);
	if (!desc || !irq_settings_can_request(desc) ||
	    !irq_settings_is_per_cpu_devid(desc))
		return -EINVAL;

	/* 阶段 2：构造 action 并取得 chip PM 引用；setup 成功后两者都转交 IRQ 核心。 */
	action = create_percpu_irqaction(handler, 0, devname, affinity, dev_id);
	if (!action)
		return -ENOMEM;

	retval = irq_chip_pm_get(&desc->irq_data);
	if (retval < 0) {
		kfree(action);
		return retval;
	}

	retval = __setup_irq(irq, desc, action);

	/* 阶段 3：setup 失败未发生所有权转移，按 PM 引用、action 的逆序回滚。 */
	if (retval) {
		irq_chip_pm_put(&desc->irq_data);
		kfree(action);
	}

	return retval;
}
EXPORT_SYMBOL_GPL(request_percpu_irq_affinity);

/**
 * request_percpu_nmi - allocate a percpu interrupt line for NMI delivery
 * @irq:	Interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 * @name:	An ascii name for the claiming device
 * @affinity:	A cpumask describing the target CPUs for this interrupt
 * @dev_id:	A percpu cookie passed back to the handler function
 *
 * This call allocates interrupt resources for a per CPU NMI. Per CPU NMIs
 * have to be setup on each CPU by calling prepare_percpu_nmi() before
 * being enabled on the same CPU by using enable_percpu_nmi().
 *
 * @dev_id must be globally unique. It is a per-cpu variable, and the
 * handler gets called with the interrupted CPU's instance of that
 * variable.
 *
 * Interrupt lines requested for NMI delivering should have auto enabling
 * setting disabled.
 *
 * If the interrupt line cannot be used to deliver NMIs, function
 * will fail returning a negative value.
 */
/*
 * 为 @affinity CPU 集合申请 percpu NMI action。这里只分配/发布资源，不做每
 * CPU chip setup；调用者须在每个目标 CPU 的不可抢占上下文依次 prepare_percpu_nmi()，
 * 再 enable_percpu_nmi()。@dev_id 是 percpu cookie，handler 收到当前 CPU 实例；调用者
 * 应保证其全局唯一并存活到 free。
 *
 * desc 必须是不可 auto-enable 的 percpu_devid IRQ，chip 支持 NMI。若已是 NMI，新申请
 * 不能覆盖全部 CPU，只允许与既有 action 不重叠 affinity 的分区共享。函数创建强制
 * NO_THREAD/NOBALANCING action、取得 PM 并经 __setup_irq() 发布，随后置 IRQS_NMI；成功
 * 返回 0，失败逆序释放并返回 errno。每 CPU setup 失败由 prepare 阶段单独报告。
 */
int request_percpu_nmi(unsigned int irq, irq_handler_t handler, const char *name,
		       const struct cpumask *affinity, void __percpu *dev_id)
{
	struct irqaction *action;
	struct irq_desc *desc;
	int retval;

	/* 阶段 1：验证 per-CPU 描述符具备 NMI 能力且不会被普通 auto-enable。 */
	if (!handler)
		return -EINVAL;

	desc = irq_to_desc(irq);

	if (!desc || !irq_settings_can_request(desc) ||
	    !irq_settings_is_per_cpu_devid(desc) ||
	    irq_settings_can_autoenable(desc) ||
	    !irq_supports_nmi(desc))
		return -EINVAL;

	/* The line cannot be NMI already if the new request covers all CPUs */
	/* 若线路已是 NMI，覆盖全部 possible CPU 的新 action 必然与现有目标冲突。 */
	if (irq_is_nmi(desc) &&
	    (!affinity || cpumask_equal(affinity, cpu_possible_mask)))
		return -EINVAL;

	/* 阶段 2：建立带目标 CPU 借用掩码的 action，并取得覆盖注册期的 chip PM 引用。 */
	action = create_percpu_irqaction(handler, IRQF_NO_THREAD | IRQF_NOBALANCING,
					 name, affinity, dev_id);
	if (!action)
		return -ENOMEM;

	retval = irq_chip_pm_get(&desc->irq_data);
	if (retval < 0)
		goto err_out;

	retval = __setup_irq(irq, desc, action);
	if (retval)
		goto err_irq_setup;

	/* 阶段 3：action 已发布后提交 NMI 身份；每 CPU 的 chip setup 仍留给 prepare API。 */
	scoped_guard(raw_spinlock_irqsave, &desc->lock)
		desc->istate |= IRQS_NMI;
	return 0;

err_irq_setup:
	irq_chip_pm_put(&desc->irq_data);
err_out:
	kfree(action);

	return retval;
}

/**
 * prepare_percpu_nmi - performs CPU local setup for NMI delivery
 * @irq: Interrupt line to prepare for NMI delivery
 *
 * This call prepares an interrupt line to deliver NMI on the current CPU,
 * before that interrupt line gets enabled with enable_percpu_nmi().
 *
 * As a CPU local operation, this should be called from non-preemptible
 * context.
 *
 * If the interrupt line cannot be used to deliver NMIs, function will fail
 * returning a negative value.
 */
/*
 * 在当前 CPU 为 @irq 执行 NMI 本地硬件建立，必须先于同 CPU 的
 * enable_percpu_nmi()。这是 CPU-local 操作，调用者必须关闭抢占以固定 smp_processor_id；
 * 函数也会告警可抢占调用。验证 PERCPU 与 IRQS_NMI 后，在 desc 锁下调用 chip nmi_setup，
 * 返回其结果；无效/非 NMI 返回 -EINVAL，失败另打印日志。成功后须由 teardown 配平。
 */
int prepare_percpu_nmi(unsigned int irq)
{
	int ret = -EINVAL;

	WARN_ON(preemptible());

	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU) {
		if (WARN(!irq_is_nmi(scoped_irqdesc),
			 "prepare_percpu_nmi called for a non-NMI interrupt: irq %u\n", irq))
			return -EINVAL;

		ret = irq_nmi_setup(scoped_irqdesc);
		if (ret)
			pr_err("Failed to setup NMI delivery: irq %u\n", irq);
	}
	return ret;
}

/**
 * teardown_percpu_nmi - undoes NMI setup of IRQ line
 * @irq: Interrupt line from which CPU local NMI configuration should be removed
 *
 * This call undoes the setup done by prepare_percpu_nmi().
 *
 * IRQ line should not be enabled for the current CPU.
 * As a CPU local operation, this should be called from non-preemptible
 * context.
 */
/*
 * 撤销当前 CPU 上 prepare_percpu_nmi() 完成的本地 NMI 配置。调用前该 CPU 的
 * 线路必须已 disable；同样要求不可抢占。验证 PERCPU/NMI 后在 desc 锁下调用 chip
 * nmi_teardown。无返回值，配置错误告警；应在 free_percpu_nmi() 前覆盖所有目标 CPU。
 */
void teardown_percpu_nmi(unsigned int irq)
{
	WARN_ON(preemptible());

	scoped_irqdesc_get_and_lock(irq, IRQ_GET_DESC_CHECK_PERCPU) {
		if (WARN_ON(!irq_is_nmi(scoped_irqdesc)))
			return;
		irq_nmi_teardown(scoped_irqdesc);
	}
}

/*
 * __irq_get_irqchip_state() - 沿 irqdomain 层级查询首个支持的硬件状态
 *
 * @data: 起始叶子 irq_data，调用期间层级稳定。
 * @which: 要查询的 IRQCHIP_STATE_* 阶段。
 * @state: 非 NULL 输出指针；只有支持回调实际调用时才由 chip 写入。
 * 从当前 data 向 parent 查找 irq_get_irqchip_state；遇到缺 chip 返回 -ENODEV，无任何层
 * 支持返回 -EINVAL，否则原样返回回调结果。函数本身不加锁，调用者持 bus/desc 或同步
 * 所需锁；层级关闭配置仅检查当前 data。
 */
static int __irq_get_irqchip_state(struct irq_data *data, enum irqchip_irq_state which, bool *state)
{
	struct irq_chip *chip;
	int err = -EINVAL;

	/* 阶段 1：从叶子向根寻找首个支持该状态查询契约的 irqchip。 */
	do {
		chip = irq_data_get_irq_chip(data);
		if (WARN_ON_ONCE(!chip))
			return -ENODEV;
		if (chip->irq_get_irqchip_state)
			break;
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
		data = data->parent_data;
#else
		data = NULL;
#endif
	} while (data);

	/* 阶段 2：只有找到回调才允许写 @state；缺能力时保留 -EINVAL 和原输出内容。 */
	if (data)
		err = chip->irq_get_irqchip_state(data, which, state);
	return err;
}

/**
 * irq_get_irqchip_state - returns the irqchip state of a interrupt.
 * @irq:	Interrupt line that is forwarded to a VM
 * @which:	One of IRQCHIP_STATE_* the caller wants to know about
 * @state:	a pointer to a boolean where the state is to be stored
 *
 * This call snapshots the internal irqchip state of an interrupt,
 * returning into @state the bit corresponding to stage @which
 *
 * This function should be called with preemption disabled if the interrupt
 * controller has per-cpu registers.
 */
/*
 * 快照 @irq 转发给 VM 等场景所需的内部 irqchip 状态，把 @which 对应布尔值
 * 写入 @state。函数取得 desc bus lock，并由内部 helper 沿层级寻找首个支持查询的 chip；
 * 返回 0 或 -EINVAL/-ENODEV/底层错误。若控制器寄存器是 per-CPU，调用者必须关闭抢占，
 * 保证回调期间访问同一 CPU bank。失败时不能假定 @state 已被写入。
 */
int irq_get_irqchip_state(unsigned int irq, enum irqchip_irq_state which, bool *state)
{
	/* 查找与慢总线串行成功后，层级 helper 负责选择回调并写输出；否则不触碰 @state。 */
	scoped_irqdesc_get_and_buslock(irq, 0) {
		struct irq_data *data = irq_desc_get_irq_data(scoped_irqdesc);

		return __irq_get_irqchip_state(data, which, state);
	}
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(irq_get_irqchip_state);

/**
 * irq_set_irqchip_state - set the state of a forwarded interrupt.
 * @irq:	Interrupt line that is forwarded to a VM
 * @which:	State to be restored (one of IRQCHIP_STATE_*)
 * @val:	Value corresponding to @which
 *
 * This call sets the internal irqchip state of an interrupt, depending on
 * the value of @which.
 *
 * This function should be called with migration disabled if the interrupt
 * controller has per-cpu registers.
 */
/*
 * 把转发中断 @irq 的内部 @which 状态恢复/设置为 @val。函数在 desc bus lock
 * 下从叶子 irq_data 向 parent 查找首个 irq_set_irqchip_state 回调并原样返回其结果；缺
 * chip 返回 -ENODEV，无支持层或无效 IRQ 返回 -EINVAL。per-CPU 寄存器控制器要求调用者
 * 禁止迁移，确保层级遍历和回调访问正确 CPU bank。核心不缓存该状态。
 */
int irq_set_irqchip_state(unsigned int irq, enum irqchip_irq_state which, bool val)
{
	scoped_irqdesc_get_and_buslock(irq, 0) {
		struct irq_data *data = irq_desc_get_irq_data(scoped_irqdesc);
		struct irq_chip *chip;

		/* 阶段 1：从叶子向根寻找首个可解释 @which 的设置回调。 */
		do {
			chip = irq_data_get_irq_chip(data);

			if (WARN_ON_ONCE(!chip))
				return -ENODEV;

			if (chip->irq_set_irqchip_state)
				break;

			data = irqd_get_parent_data(data);
		} while (data);

		/* 阶段 2：找到后在 bus lock 内提交硬件状态；无实现则统一落到 -EINVAL。 */
		if (data)
			return chip->irq_set_irqchip_state(data, which, val);
	}
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(irq_set_irqchip_state);

/**
 * irq_has_action - Check whether an interrupt is requested
 * @irq:	The linux irq number
 *
 * Returns: A snapshot of the current state
 */
/*
 * 返回 @irq 当前是否至少安装一个 action 的瞬时快照。RCU 读侧保护描述符查找
 * 与 action 可见性；无效 IRQ 返回 false。结果在解锁后可立即变化，不能据此取得 action
 * 所有权或代替 request/free 串行。
 */
bool irq_has_action(unsigned int irq)
{
	bool res;

	rcu_read_lock();
	res = irq_desc_has_action(irq_to_desc(irq));
	rcu_read_unlock();
	return res;
}
EXPORT_SYMBOL_GPL(irq_has_action);

/**
 * irq_check_status_bit - Check whether bits in the irq descriptor status are set
 * @irq:	The linux irq number
 * @bitmask:	The bitmask to evaluate
 *
 * Returns: True if one of the bits in @bitmask is set
 */
/*
 * 快照检查 @irq 的 status_use_accessors 中是否至少一个 @bitmask 位已设置。
 * RCU 保护描述符生命周期；无效 IRQ 返回 false。它直接读取状态快照，不持 desc 锁，适合
 * 能容忍并发变化的能力/状态探测，不可作为后续修改的排他条件。
 */
bool irq_check_status_bit(unsigned int irq, unsigned int bitmask)
{
	struct irq_desc *desc;
	bool res = false;

	rcu_read_lock();
	desc = irq_to_desc(irq);
	if (desc)
		res = !!(desc->status_use_accessors & bitmask);
	rcu_read_unlock();
	return res;
}
EXPORT_SYMBOL_GPL(irq_check_status_bit);
