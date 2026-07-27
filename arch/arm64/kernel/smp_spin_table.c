// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spin Table SMP initialisation
 *
 * Copyright (C) 2013 ARM Ltd.
 */

/*
 * spin-table 是 arm64 设备树启动次级 CPU 的一种固件协作协议。
 *
 * 与 PSCI 由固件提供“启动 CPU”调用不同，spin-table 依赖两个连续阶段：
 *
 *   1. cpu_prepare():
 *      把内核的 secondary_holding_pen 物理地址写入该 CPU 的
 *      cpu-release-addr。固件或引导加载程序中正在轮询这个地址的 CPU
 *      看到新入口后跳入内核，并在 holding pen 中等待。
 *
 *   2. cpu_boot():
 *      把目标 CPU 的 MPIDR 硬件 ID 写入共享的
 *      secondary_holding_pen_release。holding pen 中只有 MPIDR 匹配的
 *      CPU 会继续进入 secondary_startup，其余 CPU 继续等待。
 *
 * cpu-release-addr 是“固件等待入口”的逐 CPU 信箱，而
 * secondary_holding_pen_release 是“内核 holding pen 放行条件”的全局信箱。
 * 两处写入之后都要把数据推进到一致性点并执行 SEV：SEV 只负责唤醒
 * WFE 中的 CPU，真正传递状态的是信箱中的内存值。
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/mm.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/io.h>
#include <asm/smp_plat.h>

/*
 * 次级 CPU 的早期汇编等待入口，定义在 head.S 中。这里必须把它的物理
 * 地址交给尚未建立内核虚拟地址环境的固件/CPU。
 */
extern void secondary_holding_pen(void);

/*
 * holding pen 的共享放行字。head.S 在 MMU 关闭或尚未进入普通内核执行
 * 环境时直接读取它，因此放在专供该阶段读取的 .mmuoff.data.read 段。
 *
 * 初值 INVALID_HWID 不会匹配任何合法 MPIDR，所以系统启动之初不会误放行
 * 某个 CPU。volatile 只阻止编译器缓存或合并访问；对未加入硬件一致性域的
 * 次级 CPU，可见性仍依赖 write_pen_release() 中显式的缓存维护。
 */
volatile unsigned long __section(".mmuoff.data.read")
secondary_holding_pen_release = INVALID_HWID;

/*
 * 从各 CPU 设备树节点的 cpu-release-addr 属性复制出的物理地址。
 *
 * 数组下标是 Linux 逻辑 CPU 号，元素为零表示没有可用的 release 信箱。
 * 设备树节点引用只在 cpu_init() 内短暂持有，因此这里保存数值而不保存
 * device_node 指针。初始化和 prepare 都发生在 CPU bring-up 的串行控制
 * 路径上，不需要额外加锁。
 */
static phys_addr_t cpu_release_addr[NR_CPUS];

/*
 * Write secondary_holding_pen_release in a way that is guaranteed to be
 * visible to all observers, irrespective of whether they're taking part
 * in coherency or not.  This is necessary for the hotplug code to work
 * reliably.
 */
/*
 * 发布新的 holding-pen 放行值。
 *
 * 写普通内存后执行到 PoC（Point of Coherency）的 clean+invalidate，确保
 * 即使观察者尚未参与缓存一致性，也能从外部可见位置重新取得新值。
 * 调用者随后还会执行 SEV，唤醒可能停在 WFE 的次级 CPU。
 */
static void write_pen_release(u64 val)
{
	/* 缓存维护接口使用 [start, end) 地址区间。 */
	void *start = (void *)&secondary_holding_pen_release;
	unsigned long size = sizeof(secondary_holding_pen_release);

	/* 先发布目标 MPIDR，再把包含该值的缓存范围推进到 PoC。 */
	secondary_holding_pen_release = val;
	dcache_clean_inval_poc((unsigned long)start, (unsigned long)start + size);
}


/*
 * 为一个逻辑 CPU 解析 spin-table 的固件信箱。
 *
 * 成功后 cpu_release_addr[cpu] 持有该 CPU 正在轮询的物理地址；
 * 失败时让上层把这个 CPU 视为无法通过本方法启动。此阶段只解析
 * 元数据，不映射也不写信箱。
 */
static int smp_spin_table_cpu_init(unsigned int cpu)
{
	struct device_node *dn;
	int ret;

	/* of_get_cpu_node() 返回带引用的节点，所有退出路径都要相应 put。 */
	dn = of_get_cpu_node(cpu, NULL);
	if (!dn)
		return -ENODEV;

	/*
	 * Determine the address from which the CPU is polling.
	 */
	/*
	 * 属性值是物理地址。解析器负责按设备树单元格式转换为 CPU 本地的
	 * u64 表示；后续 ioremap_cache() 才建立访问它所需的虚拟映射。
	 */
	ret = of_property_read_u64(dn, "cpu-release-addr",
				   &cpu_release_addr[cpu]);
	if (ret)
		pr_err("CPU %d: missing or invalid cpu-release-addr property\n",
		       cpu);

	/* 数值已经复制到静态数组，不再需要持有设备树节点。 */
	of_node_put(dn);

	/* 原样返回属性解析错误，使 CPU ops 初始化路径能够拒绝该 CPU。 */
	return ret;
}

/*
 * 把固件轮询入口改为内核 holding pen。
 *
 * 这是一次性的 CPU 准备阶段：映射只在本函数内存在，写入并完成缓存
 * 维护后立即解除；信箱内容则由固件持续观察。返回成功只表示入口已经
 * 发布，并不表示目标 CPU 已经进入 holding pen。
 */
static int smp_spin_table_cpu_prepare(unsigned int cpu)
{
	__le64 __iomem *release_addr;
	/*
	 * 固件在 MMU 外使用物理地址跳转，不能把 secondary_holding_pen 的
	 * 内核虚拟地址直接写入信箱。
	 */
	phys_addr_t pa_holding_pen = __pa_symbol(secondary_holding_pen);

	/* cpu_init() 未取得合法信箱时，不能尝试启动这个 CPU。 */
	if (!cpu_release_addr[cpu])
		return -ENODEV;

	/*
	 * The cpu-release-addr may or may not be inside the linear mapping.
	 * As ioremap_cache will either give us a new mapping or reuse the
	 * existing linear mapping, we can use it to cover both cases. In
	 * either case the memory will be MT_NORMAL.
	 */
	/*
	 * 信箱有可能由固件放在普通 RAM，也可能位于线性映射之外。
	 * ioremap_cache() 统一提供 Normal Memory 属性，避免同一物理页出现
	 * 冲突的缓存属性。
	 */
	release_addr = ioremap_cache(cpu_release_addr[cpu],
				     sizeof(*release_addr));
	if (!release_addr)
		return -ENOMEM;

	/*
	 * We write the release address as LE regardless of the native
	 * endianness of the kernel. Therefore, any boot-loaders that
	 * read this address need to convert this address to the
	 * boot-loader's endianness before jumping. This is mandated by
	 * the boot protocol.
	 */
	/*
	 * relaxed 写不额外提供设备访问排序；这里所需的跨一致性域可见性
	 * 由紧随其后的 PoC 缓存维护完成。协议规定槽位为小端，
	 * writeq_relaxed() 对 __le64 __iomem 目标写入相应的 64 位入口值。
	 */
	writeq_relaxed(pa_holding_pen, release_addr);
	dcache_clean_inval_poc((__force unsigned long)release_addr,
			    (__force unsigned long)release_addr +
				    sizeof(*release_addr));

	/*
	 * Send an event to wake up the secondary CPU.
	 */
	/*
	 * 唤醒所有处于 WFE 的观察者；只有轮询到自己 release 信箱变化的 CPU
	 * 才会跳到 holding pen。事件本身不替代上面的内存可见性处理。
	 */
	sev();

	/* 映射仅用于本次发布，信箱的物理内容不受解除映射影响。 */
	iounmap(release_addr);

	return 0;
}

/*
 * 从共享 holding pen 中放行指定 CPU。
 *
 * 此时目标 CPU 应已由 prepare 阶段引导到 secondary_holding_pen。汇编循环
 * 用自身经掩码处理的 MPIDR 与这里发布的 cpu_logical_map(cpu) 比较，因此
 * 广播 SEV 虽会唤醒所有等待者，却只有目标 CPU 能进入 secondary_startup。
 */
static int smp_spin_table_cpu_boot(unsigned int cpu)
{
	/*
	 * Update the pen release flag.
	 */
	/*
	 * Linux 逻辑 CPU 号不能直接作为固件硬件 ID，必须查逻辑到 MPIDR
	 * 的映射。
	 */
	write_pen_release(cpu_logical_map(cpu));

	/*
	 * Send an event, causing the secondaries to read pen_release.
	 */
	/* write_pen_release() 已发布数据；SEV 促使等待者重新读取放行字。 */
	sev();

	return 0;
}

/*
 * 注册给通用 arm64 CPU bring-up 框架的 spin-table 实现。
 *
 * 名称必须与设备树 CPU 节点的 enable-method 完全匹配。这里只提供初始化、
 * 准备和启动回调；没有 cpu_can_disable/cpu_die/cpu_kill 等热下线回调，
 * 因而通用 CPU ops 层不会把本实现当作完整的 CPU hotplug 后端。
 */
const struct cpu_operations smp_spin_table_ops = {
	.name		= "spin-table",
	.cpu_init	= smp_spin_table_cpu_init,
	.cpu_prepare	= smp_spin_table_cpu_prepare,
	.cpu_boot	= smp_spin_table_cpu_boot,
};
