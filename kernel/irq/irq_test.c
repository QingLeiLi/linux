// SPDX-License-Identifier: LGPL-2.1+

#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/nodemask.h>
#include <kunit/test.h>

#include "internals.h"

/*
 * 本 KUnit 套件用最小 fake irq_chip 构造真实 irq_desc/action 生命周期，验证 disable
 * depth、在 disabled 状态 free/re-request、managed shutdown/reactivate，以及 CPU1
 * offline/online 后 depth 的保持。测试会调用真实 IRQ 与 CPU hotplug 核心，只适合隔离
 * 的 KUnit 环境；fake chip 不模拟硬件失败、寄存器或事件并发。
 */

/* fake action 恒报告已处理；测试只关心管理状态，不生成真实中断。 */
static irqreturn_t noop_handler(int irq, void *data)
{
	return IRQ_HANDLED;
}

/* fake chip 的无副作用 void 回调。 */
static void noop(struct irq_data *data) { }
/* fake startup 恒成功，使核心可以进入 STARTED 状态。 */
static unsigned int noop_ret(struct irq_data *data) { return 0; }

/* fake affinity 编程只把目标复制为 effective mask，不访问硬件并恒返回成功。 */
static int noop_affinity(struct irq_data *data, const struct cpumask *dest,
			 bool force)
{
	irq_data_update_effective_affinity(data, dest);

	return 0;
}

/*
 * 所有测试共享的 fake irq_chip：提供启动/关闭/屏蔽/确认等最小能力，set_affinity
 * 维护 effective 状态，并声明 SKIP_SET_WAKE。静态对象永久存活，各 desc 只借用指针。
 */
static struct irq_chip fake_irq_chip = {
	.name           = "fake",
	.irq_startup    = noop_ret,
	.irq_shutdown   = noop,
	.irq_enable     = noop,
	.irq_disable    = noop,
	.irq_ack        = noop,
	.irq_mask       = noop,
	.irq_unmask     = noop,
	.irq_set_affinity = noop_affinity,
	.flags          = IRQCHIP_SKIP_SET_WAKE,
};

/*
 * 为单个测试分配一个未绑定 domain 的动态 irq_desc，并安装 fake chip 与
 * handle_simple_irq。@affd 可选地把描述符初始化为 managed affinity。任何分配/查找
 * 失败用 KUNIT_ASSERT 终止当前测试；成功清 NOREQUEST，使 request_irq() 可安装 action，
 * 返回 virq。
 *
 * 部分架构默认给 IRQ 设置 NOREQUEST|NOPROBE；当前代码只清 NOREQUEST，保留
 * NOPROBE，因为测试无需旧式自动探测。调用者最终只 free_irq()，本套件当前没有配对
 * irq_free_descs()，因此每个成功 setup 的动态描述符会保留到测试环境结束。
 */
static int irq_test_setup_fake_irq(struct kunit *test, struct irq_affinity_desc *affd)
{
	struct irq_desc *desc;
	int virq;

	virq = irq_domain_alloc_descs(-1, 1, 0, NUMA_NO_NODE, affd);
	KUNIT_ASSERT_GE(test, virq, 0);

	irq_set_chip_and_handler(virq, &fake_irq_chip, handle_simple_irq);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	/* On some architectures, IRQs are NOREQUEST | NOPROBE by default. */
	/* 某些架构的 IRQ 默认同时禁止 request 与 probe。 */
	irq_settings_clr_norequest(desc);

	return virq;
}

/*
 * 基本 depth 配对测试：申请后 IRQ 自动启动且 depth=0；一次 disable_irq() 变为 1，
 * 一次 enable_irq() 恢复 0，最后 free action。ASSERT 用于后续不可继续的 setup/request，
 * EXPECT 收集状态偏差但继续完成清理。
 */
static void irq_disable_depth_test(struct kunit *test)
{
	struct irq_desc *desc;
	int virq, ret;

	virq = irq_test_setup_fake_irq(test, NULL);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	disable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	enable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	free_irq(virq, NULL);
}

/*
 * 验证在 depth=1 时直接 free 最后 action 的语义。free_irq() 关闭线路但不会把既有禁用
 * 嵌套伪装成 enabled，故先期望 depth>=1；同一 desc 再 request 时核心启动路径应规范化
 * 到 depth=0。最后再次 free，防止 action 越过测试；动态 desc 本身仍由 setup 边界保留。
 */
static void irq_free_disabled_test(struct kunit *test)
{
	struct irq_desc *desc;
	int virq, ret;

	virq = irq_test_setup_fake_irq(test, NULL);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	disable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	free_irq(virq, NULL);
	KUNIT_EXPECT_GE(test, desc->depth, 1);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	free_irq(virq, NULL);
}

/*
 * 验证 managed IRQ 的 shutdown/deactivate 与软件 disable depth 独立。仅 SMP 执行：以
 * CPU_MASK_ALL managed affinity 创建并申请 IRQ，确认 ACTIVATED/STARTED/MANAGED 后先
 * disable 到 depth=1；desc 锁内强制 shutdown/deactivate，再显式 activate 与 managed
 * startup。硬件生命周期恢复后 depth 必须仍为 1，最终 enable 才回到 0。
 */
static void irq_shutdown_depth_test(struct kunit *test)
{
	struct irq_desc *desc;
	struct irq_data *data;
	int virq, ret;
	struct irq_affinity_desc affinity = {
		.is_managed = 1,
		.mask = CPU_MASK_ALL,
	};

	if (!IS_ENABLED(CONFIG_SMP))
		kunit_skip(test, "requires CONFIG_SMP for managed shutdown");

	virq = irq_test_setup_fake_irq(test, &affinity);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	data = irq_desc_get_irq_data(desc);
	KUNIT_ASSERT_PTR_NE(test, data, NULL);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_TRUE(test, irqd_is_activated(data));
	KUNIT_EXPECT_TRUE(test, irqd_is_started(data));
	KUNIT_EXPECT_TRUE(test, irqd_affinity_is_managed(data));

	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	disable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	scoped_guard(raw_spinlock_irqsave, &desc->lock)
		irq_shutdown_and_deactivate(desc);

	KUNIT_EXPECT_FALSE(test, irqd_is_activated(data));
	KUNIT_EXPECT_FALSE(test, irqd_is_started(data));

	KUNIT_EXPECT_EQ(test, irq_activate(desc), 0);
#ifdef CONFIG_SMP
	irq_startup_managed(desc);
#endif

	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	enable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	free_irq(virq, NULL);
}

/*
 * 验证 managed IRQ 固定到 CPU1 时的真实 hotplug 路径。仅在 SMP、CPU1 存在/可热插拔/
 * 已在线时执行；申请并 disable 后调用 remove_cpu(1)，期望下线路径关闭/迁移资源但保留
 * depth>=1，再 add_cpu(1) 并确认 depth 仍为 1。最后 enable 应重新得到
 * ACTIVATED/STARTED 且 depth=0。
 *
 * remove/add 使用 KUNIT_EXPECT，失败会记录但继续，因此测试环境若 add_cpu() 失败可能把
 * CPU1 留在 offline；该用例应在可牺牲的 KUnit 环境运行，不应当作无副作用单元测试。
 */
static void irq_cpuhotplug_test(struct kunit *test)
{
	struct irq_desc *desc;
	struct irq_data *data;
	int virq, ret;
	struct irq_affinity_desc affinity = {
		.is_managed = 1,
	};

	if (!IS_ENABLED(CONFIG_SMP))
		kunit_skip(test, "requires CONFIG_SMP for CPU hotplug");
	if (!get_cpu_device(1))
		kunit_skip(test, "requires more than 1 CPU for CPU hotplug");
	if (!cpu_is_hotpluggable(1))
		kunit_skip(test, "CPU 1 must be hotpluggable");
	if (!cpu_online(1))
		kunit_skip(test, "CPU 1 must be online");

	cpumask_copy(&affinity.mask, cpumask_of(1));

	virq = irq_test_setup_fake_irq(test, &affinity);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	data = irq_desc_get_irq_data(desc);
	KUNIT_ASSERT_PTR_NE(test, data, NULL);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_TRUE(test, irqd_is_activated(data));
	KUNIT_EXPECT_TRUE(test, irqd_is_started(data));
	KUNIT_EXPECT_TRUE(test, irqd_affinity_is_managed(data));

	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	disable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	KUNIT_EXPECT_EQ(test, remove_cpu(1), 0);
	KUNIT_EXPECT_GE(test, desc->depth, 1);
	KUNIT_EXPECT_EQ(test, add_cpu(1), 0);

	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	enable_irq(virq);
	KUNIT_EXPECT_TRUE(test, irqd_is_activated(data));
	KUNIT_EXPECT_TRUE(test, irqd_is_started(data));
	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	free_irq(virq, NULL);
}

/* 四个测试按声明顺序运行；空哨兵终止 KUnit case 数组。 */
static struct kunit_case irq_test_cases[] = {
	KUNIT_CASE(irq_disable_depth_test),
	KUNIT_CASE(irq_free_disabled_test),
	KUNIT_CASE(irq_shutdown_depth_test),
	KUNIT_CASE(irq_cpuhotplug_test),
	{}
};

/* KUnit suite 元数据，引用永久静态 case 数组。 */
static struct kunit_suite irq_test_suite = {
	.name = "irq_test_cases",
	.test_cases = irq_test_cases,
};

/* 注册套件，并声明模块描述与 GPL 许可证以满足使用的 GPL-only IRQ API。 */
kunit_test_suite(irq_test_suite);
MODULE_DESCRIPTION("IRQ unit test suite");
MODULE_LICENSE("GPL");
