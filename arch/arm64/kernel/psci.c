// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 PSCI SMP 启停与 CPU hotplug 学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 通用 drivers/firmware/psci/psci.c 负责选择 SMC/HVC、探测版本并发布
 * psci_ops；本文件把该固件操作表接入 ARM64 cpu_operations：
 *
 *   CPU 上线：cpu_prepare() 校验 CPU_ON -> cpu_boot() 传 MPIDR 与
 *             secondary_entry 物理地址 -> 次级 CPU 完成内核握手
 *   CPU 下线：cpu_disable() 排除 TOS 驻留 CPU -> 目标 CPU cpu_die()
 *             调 CPU_OFF -> 控制 CPU cpu_kill() 轮询 AFFINITY_INFO
 *
 * psci_ops 在启动期发布后只读；本文件不拥有 CPU 对象，也不负责固件内部
 * 电源状态。CPU_ON 成功只表示请求已接受，CPU_OFF 成功不返回。hotplug 的
 * 并发由 CPU core 状态机串行化，AFFINITY_INFO 轮询用于跨 CPU/固件边界确认
 * 最终硬件状态，而不是普通锁或引用计数。
 */
/*
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/* 统一给本文件日志添加 "psci: " 前缀。 */
#define pr_fmt(fmt) "psci: " fmt

#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/mm.h>

#include <uapi/linux/psci.h>

#include <asm/cpu_ops.h>
#include <asm/errno.h>
#include <asm/smp_plat.h>

/*
 * cpu_operations.cpu_init 的 PSCI 占位入口。
 *
 * @cpu：待初始化逻辑 CPU 号。PSCI 没有每 CPU 软件初始化工作，返回 0；
 * 真正能力由全局 psci_ops 和 prepare 阶段检查。启动期调用，无副作用。
 */
static int __init cpu_psci_cpu_init(unsigned int cpu)
{
	return 0;
}

/*
 * 在启动某 CPU 前确认固件提供 CPU_ON。
 *
 * @cpu：Linux 逻辑 CPU 号，仅用于诊断。返回 0 表示可继续，未发布 cpu_on
 * 则返回 -ENODEV。此处不发起固件调用、不取得 CPU 引用。
 */
static int __init cpu_psci_cpu_prepare(unsigned int cpu)
{
	if (!psci_ops.cpu_on) {
		pr_err("no cpu_on method, not booting CPU%d\n", cpu);
		return -ENODEV;
	}

	return 0;
}

/*
 * 向 PSCI 固件提交次级 CPU 上电请求。
 *
 * @cpu：目标 Linux 逻辑 CPU。cpu_logical_map() 转为固件识别的 MPIDR；
 * secondary_entry 经 __pa_symbol 转为目标 CPU 的物理启动入口。
 *
 * 返回 psci_ops.cpu_on 的 Linux errno。SUCCESS 只表示异步请求被固件接受，
 * 后续 secondary CPU handshake 才证明上线。-EPERM 常对应固件 DENIED，保留
 * 给上层处理而不重复打印；其他错误记录日志。
 */
static int cpu_psci_cpu_boot(unsigned int cpu)
{
	/* 两个局部量分别固定恢复物理入口和固件调用结果。 */
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
	int err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);
	if (err && err != -EPERM)
		pr_err("failed to boot CPU%d (%d)\n", cpu, err);

	return err;
}

#ifdef CONFIG_HOTPLUG_CPU
/* 以下回调组成 CPU hotplug 下线路径，只在启用 HOTPLUG_CPU 时存在。 */

/*
 * 判断某 CPU 是否允许下线。
 *
 * @cpu：逻辑 CPU 号。Trusted OS 驻留 CPU 必须保持在线，否则 CPU_OFF 可能
 * 被拒绝并让目标 CPU 停在不可恢复的下线尾段。纯查询、无副作用。
 */
static bool cpu_psci_cpu_can_disable(unsigned int cpu)
{
	return !psci_tos_resident_on(cpu);
}

/*
 * CPU hotplug 下线准备检查。
 *
 * @cpu：待下线逻辑 CPU。CPU_OFF 缺失返回 -EOPNOTSUPP；TOS 驻留返回 -EPERM；
 * 否则返回 0。检查发生在仍可安全取消下线的控制阶段，不改变硬件状态。
 */
static int cpu_psci_cpu_disable(unsigned int cpu)
{
	/* Fail early if we don't have CPU_OFF support */
	/* 在任务迁移和不可返回的 die 阶段之前拒绝不支持的平台。 */
	if (!psci_ops.cpu_off)
		return -EOPNOTSUPP;

	/* Trusted OS will deny CPU_OFF */
	/* 与通用 PSCI 初始化探测出的 resident_cpu 约束配对。 */
	if (psci_tos_resident_on(cpu))
		return -EPERM;

	return 0;
}

/*
 * 在目标 CPU 自身执行最终 CPU_OFF。
 *
 * @cpu：正在下线的逻辑号，接口要求保留但本实现无需再转换。state 请求
 * power-down 类型；成功后 CPU 断电且函数不返回。若固件异常返回，上层已
 * 进入不可安全恢复的 die 阶段，因此这属于致命固件违约。
 */
static void cpu_psci_cpu_die(unsigned int cpu)
{
	/*
	 * There are no known implementations of PSCI actually using the
	 * power state field, pass a sensible default for now.
	 */
	/*
	 * 现有固件通常忽略 CPU_OFF state，但仍传入规范的 power-down Type，
	 * 不虚构 StateID/affinity level。
	 */
	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	psci_ops.cpu_off(state);
}

/*
 * 在控制 CPU 上确认目标 CPU 已真正断电。
 *
 * @cpu：刚执行 cpu_die 的逻辑 CPU。AFFINITY_INFO 不可用时返回 0，表示
 * 无法验证但不阻止 hotplug 完成；可用时最多轮询约 100ms。
 *
 * OFF 返回 0；超时返回 -ETIMEDOUT。err 保存固件原始 affinity 状态，
 * start/end 是 jiffies 时间窗。usleep_range 允许当前控制线程睡眠，避免
 * 忙等；目标 CPU 的电源转换与轮询天然存在竞态，因此必须重试。
 */
static int cpu_psci_cpu_kill(unsigned int cpu)
{
	int err;
	unsigned long start, end;

	if (!psci_ops.affinity_info)
		return 0;
	/*
	 * cpu_kill could race with cpu_die and we can
	 * potentially end up declaring this cpu undead
	 * while it is dying. So, try again a few times.
	 */
	/*
	 * cpu_die 与本轮询并行：过早读取可能仍是 ON/ON_PENDING，不能据一次结果
	 * 宣告“undead”。时间窗内重复读取直到观察到固件发布 OFF。
	 */

	start = jiffies;
	end = start + msecs_to_jiffies(100);
	do {
		err = psci_ops.affinity_info(cpu_logical_map(cpu), 0);
		if (err == PSCI_0_2_AFFINITY_LEVEL_OFF) {
			pr_info("CPU%d killed (polled %d ms)\n", cpu,
				jiffies_to_msecs(jiffies - start));
			return 0;
		}

		usleep_range(100, 1000);
	} while (time_before(jiffies, end));

	pr_warn("CPU%d may not have shut down cleanly (AFFINITY_INFO reports %d)\n",
			cpu, err);
	return -ETIMEDOUT;
}
#endif

/*
 * ARM64 PSCI cpu_operations 发布表。
 *
 * CPU framework 按 enable-method="psci" 选择该静态对象。init/prepare/boot
 * 处理上线；条件字段处理 hotplug 下线。表只借用上述静态函数，不需释放。
 */
const struct cpu_operations cpu_psci_ops = {
	.name		= "psci",
	.cpu_init	= cpu_psci_cpu_init,
	.cpu_prepare	= cpu_psci_cpu_prepare,
	.cpu_boot	= cpu_psci_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = cpu_psci_cpu_can_disable,
	.cpu_disable	= cpu_psci_cpu_disable,
	.cpu_die	= cpu_psci_cpu_die,
	.cpu_kill	= cpu_psci_cpu_kill,
#endif
};

