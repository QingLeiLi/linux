// SPDX-License-Identifier: GPL-2.0-only
/*
 * PSCI CPU idle 驱动与层级电源域协同学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 【主路径】
 * device_initcall 创建 faux device -> probe 为每个 present CPU：
 *   校验 enable-method="psci"
 *   -> 解析 DT idle states 和 arm,psci-suspend-param
 *   -> 可选把 CPU attach 到 genpd 层级
 *   -> 注册 per-CPU cpuidle_driver
 *
 * 运行期普通状态：
 *   cpuidle core -> psci_enter_idle_state()
 *   -> CPU_PM/RCU 包装 -> psci_cpu_suspend_enter(state) -> PSCI firmware
 *
 * OSI 层级最深状态：
 *   genpd runtime PM/s2idle 选择共享 domain state
 *   -> psci_set_domain_state() 写当前 CPU per-CPU 覆盖值
 *   -> __psci_enter_domain_idle_state() 用共享 state 替代 CPU state
 *   -> 固件按 OS-initiated 决策关闭 CPU/cluster 等电源域
 *
 * 【核心对象】
 * 每 CPU psci_cpuidle_data 持有 state 数组和可选 PM-domain device；数组由
 * faux device 的 devres 管理，驱动注册期间只读。psci_domain_state 是一次
 * idle 尝试的 per-CPU 临时选择，返回后必须清零，不能泄漏到下一次进入。
 *
 * idle 热路径在当前 CPU、禁止普通迁移的 cpuidle 上下文执行，per-CPU 状态
 * 代替锁；CPU hotplug 和 syscore 回调负责同步 domain device 的 active/
 * suspended 状态。PREEMPT_RT 不允许普通 idle 热路径执行可能睡眠的 runtime
 * PM 层级操作，因此仅在 system suspend/s2idle 使用 domain states。
 *
 * 本文件不解析/创建 genpd provider（由 cpuidle-psci-domain.c 完成），也不
 * 保存 CPU 寄存器；最终 retention/power-down 差异由通用 PSCI 层处理。
 */
/*
 * PSCI CPU idle driver.
 *
 * Copyright (C) 2019 ARM Ltd.
 * Author: Lorenzo Pieralisi <lorenzo.pieralisi@arm.com>
 */

/* 统一给日志添加 "CPUidle PSCI: " 前缀。 */
#define pr_fmt(fmt) "CPUidle PSCI: " fmt

#include <linux/cpuhotplug.h>
#include <linux/cpu_cooling.h>
#include <linux/cpuidle.h>
#include <linux/cpumask.h>
#include <linux/cpu_pm.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/device/faux.h>
#include <linux/psci.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscore_ops.h>

#include <asm/cpuidle.h>
#include <trace/events/power.h>

#include "cpuidle-psci.h"
#include "dt_idle_states.h"
#include "dt_idle_genpd.h"

/*
 * 每 CPU 的稳定 idle 配置。
 *
 * psci_states[i] 与该 CPU cpuidle_driver.states[i] 一一对应，元素是提交固件
 * 的 u32 power-state；index 0 固定为 WFI/0。dev 是 CPU attach 到层级 genpd
 * 后得到的 faux device，NULL 表示不使用 OSI topology。初始化者发布后热路径
 * 只读；资源随父 faux device devres 回收。
 */
struct psci_cpuidle_data {
	u32 *psci_states;
	struct device *dev;
};

/*
 * 一次 OSI domain idle 选择的当前 CPU 临时状态。
 *
 * pd/state_idx 用于失败时修正对应 genpd state 的 rejected 统计；state 是
 * 最终覆盖 CPU state 的 PSCI 编码，0 为“本次没有共享状态”。genpd power_off
 * 在当前 CPU 写入，idle 返回后清除，无跨 CPU ownership。
 */
struct psci_cpuidle_domain_state {
	struct generic_pm_domain *pd;
	unsigned int state_idx;
	u32 state;
};

/*
 * per-CPU 稳定配置使用 READ_MOSTLY 优化布局；临时 domain state 频繁写，不
 * 使用只读布局。psci_cpuidle_use_syscore 表示至少建立过层级 topology，决定
 * 是否注册全局 syscore suspend/resume 桥接。
 */
static DEFINE_PER_CPU_READ_MOSTLY(struct psci_cpuidle_data, psci_cpuidle_data);
static DEFINE_PER_CPU(struct psci_cpuidle_domain_state, psci_domain_state);
static bool psci_cpuidle_use_syscore;

/*
 * 记录 genpd 为当前 CPU 选择的共享 domain power state。
 *
 * @pd：触发 power_off 的 generic_pm_domain 借用指针，生命周期覆盖 idle。
 * @state_idx：pd->states[] 下标，供失败统计。
 * @state：提交 PSCI 的共享 power-state 编码，非零表示有效。
 *
 * 调用者是 OSI domain 的 psci_pd_power_off()，运行在即将 idle 的当前 CPU；
 * this_cpu_ptr 要求调用协议阻止迁移。函数只写当前 CPU 三字段，无锁、不睡眠。
 */
void psci_set_domain_state(struct generic_pm_domain *pd, unsigned int state_idx,
			   u32 state)
{
	struct psci_cpuidle_domain_state *ds = this_cpu_ptr(&psci_domain_state);

	ds->pd = pd;
	ds->state_idx = state_idx;
	ds->state = state;
}

/*
 * 清除当前 CPU 的一次性 domain-state 有效标志。
 *
 * 只需把 state 置 0；pd/state_idx 在 state==0 时不可读。调用者必须位于目标
 * CPU 或具备等价 per-CPU 访问保证。无返回、无睡眠。
 */
static inline void psci_clear_domain_state(void)
{
	__this_cpu_write(psci_domain_state.state, 0);
}

/*
 * 进入可能包含共享 CPU power domain 的 PSCI idle state。
 *
 * @dev：当前 CPU 的 cpuidle_device 借用指针。
 * @drv：当前 CPU driver，接口要求但本实现不直接读取。
 * @idx：请求的 cpuidle state 下标。
 * @s2idle：true 使用 system s2idle 的直接 genpd suspend/resume；false 使用
 *          runtime PM 引用计数。
 *
 * 返回 idx 表示成功进入，-1 表示 CPU PM 或 PSCI 失败。函数先 cpu_pm_enter，
 * 再让 genpd 选择层级状态；power_off callback 可能写 ds->state，若非零便
 * 覆盖单 CPU state。固件返回后逆序恢复 genpd/cpu_pm、修正 rejected 统计并
 * 清除临时 state。
 *
 * __cpuidle 标记该热路径的特殊执行段；普通 idle 不得睡眠。PREEMPT_RT 不把
 * 该函数安装为普通 .enter，只用于允许的 s2idle/system suspend 场景。
 */
static __cpuidle int __psci_enter_domain_idle_state(struct cpuidle_device *dev,
						    struct cpuidle_driver *drv, int idx,
						    bool s2idle)
{
	struct psci_cpuidle_data *data = this_cpu_ptr(&psci_cpuidle_data);
	u32 *states = data->psci_states;
	struct device *pd_dev = data->dev;
	struct psci_cpuidle_domain_state *ds;
	u32 state = states[idx];
	int ret;

	/* 阶段 1：通知 CPU PM clients 保存本 CPU 低功耗前状态；失败尚未改 genpd。 */
	ret = cpu_pm_enter();
	if (ret)
		return -1;

	/* Do runtime PM to manage a hierarchical CPU toplogy. */
	/*
	 * s2idle 已在全局 suspend 流程中，直接驱动 genpd；普通 idle 通过 runtime
	 * PM put 让 governor 判断 domain 是否能关。两者的 power_off 都可能设置
	 * 当前 CPU domain override。
	 */
	if (s2idle)
		dev_pm_genpd_suspend(pd_dev);
	else
		pm_runtime_put_sync_suspend(pd_dev);

	ds = this_cpu_ptr(&psci_domain_state);
	/* 非零共享 state 比单 CPU state 更深，按 OSI 协议由 OS 明确提交。 */
	if (ds->state)
		state = ds->state;

	trace_psci_domain_idle_enter(dev->cpu, state, s2idle);
	ret = psci_cpu_suspend_enter(state) ? -1 : idx;
	trace_psci_domain_idle_exit(dev->cpu, state, s2idle);

	/* 阶段 3：无论 PSCI 成败都逆序恢复 domain device 与 CPU PM clients。 */
	if (s2idle)
		dev_pm_genpd_resume(pd_dev);
	else
		pm_runtime_get_sync(pd_dev);

	cpu_pm_exit();

	/* Correct domain-idlestate statistics if we failed to enter. */
	/* 固件拒绝共享 state 时，补记该 genpd state 被 reject，而非算作驻留成功。 */
	if (ret == -1 && ds->state)
		pm_genpd_inc_rejected(ds->pd, ds->state_idx);

	/* Clear the domain state to start fresh when back from idle. */
	/* state 是一次性通信槽；遗漏清零会让下一次错误复用旧 domain 决策。 */
	psci_clear_domain_state();
	return ret;
}

/* 普通 runtime-idle wrapper：固定 s2idle=false，其他契约同核心函数。 */
static int psci_enter_domain_idle_state(struct cpuidle_device *dev,
					struct cpuidle_driver *drv, int idx)
{
	return __psci_enter_domain_idle_state(dev, drv, idx, false);
}

/* suspend-to-idle wrapper：固定 s2idle=true，绕过 runtime PM 引用路径。 */
static int psci_enter_s2idle_domain_idle_state(struct cpuidle_device *dev,
					       struct cpuidle_driver *drv,
					       int idx)
{
	return __psci_enter_domain_idle_state(dev, drv, idx, true);
}

/*
 * CPU hotplug online 回调：恢复该 CPU 的 genpd device active 引用。
 *
 * @cpu 是正在上线 CPU，AP hotplug 状态在目标 CPU 执行，故 __this_cpu_read
 * 读取对应配置。无 domain device 时无操作。非 RT 用 runtime PM get；RT
 * 直接 resume genpd，避免不适合该上下文的 runtime PM 路径。始终返回 0。
 */
static int psci_idle_cpuhp_up(unsigned int cpu)
{
	struct device *pd_dev = __this_cpu_read(psci_cpuidle_data.dev);

	if (pd_dev) {
		if (!IS_ENABLED(CONFIG_PREEMPT_RT))
			pm_runtime_get_sync(pd_dev);
		else
			dev_pm_genpd_resume(pd_dev);
	}

	return 0;
}

/*
 * CPU hotplug offline 回调：挂起 domain device 并清理临时选择。
 *
 * @cpu 是当前下线 CPU；配置访问与 online 对称。成功后下一次上线从无旧
 * domain state 开始。helper 返回值当前不传播，函数始终返回 0。
 */
static int psci_idle_cpuhp_down(unsigned int cpu)
{
	struct device *pd_dev = __this_cpu_read(psci_cpuidle_data.dev);

	if (pd_dev) {
		if (!IS_ENABLED(CONFIG_PREEMPT_RT))
			pm_runtime_put_sync(pd_dev);
		else
			dev_pm_genpd_suspend(pd_dev);

		/* Clear domain state to start fresh at next online. */
		/* 防止 hotplug 跨代保留上一次 idle 的共享 state。 */
		psci_clear_domain_state();
	}

	return 0;
}

/*
 * system-wide suspend/resume 时切换所有 CPU domain devices。
 *
 * @suspend：true 对每个存在的 dev 调 genpd suspend；false 调 resume，并
 * 修正用户态已 offline CPU 的 runtime status。cpu 遍历 possible CPUs，
 * dev 为每 CPU稳定借用指针；cleared 保证只清一次当前执行 CPU 的临时槽。
 *
 * syscore 阶段设备 PM 已冻结，函数不与普通 idle 并发；不分配、无返回。
 */
static void psci_idle_syscore_switch(bool suspend)
{
	bool cleared = false;
	struct device *dev;
	int cpu;

	for_each_possible_cpu(cpu) {
		dev = per_cpu_ptr(&psci_cpuidle_data, cpu)->dev;

		if (dev && suspend) {
			dev_pm_genpd_suspend(dev);
		} else if (dev) {
			dev_pm_genpd_resume(dev);

			/* Account for userspace having offlined a CPU. */
			/* offline CPU 的 device 仍可能标 suspended，resume 后校正为 active。 */
			if (pm_runtime_status_suspended(dev))
				pm_runtime_set_active(dev);

			/* Clear domain state to re-start fresh. */
			/* __this_cpu 操作的是执行 syscore 回调的 CPU，只需执行一次。 */
			if (!cleared) {
				psci_clear_domain_state();
				cleared = true;
			}
		}
	}
}

/* syscore suspend 适配器：忽略框架 data，切到 suspend 并返回成功。 */
static int psci_idle_syscore_suspend(void *data)
{
	psci_idle_syscore_switch(true);
	return 0;
}

/* syscore resume 适配器：忽略 data，恢复所有 domain devices。 */
static void psci_idle_syscore_resume(void *data)
{
	psci_idle_syscore_switch(false);
}

/* syscore 回调表静态存续，框架只借用。 */
static const struct syscore_ops psci_idle_syscore_ops = {
	.suspend = psci_idle_syscore_suspend,
	.resume = psci_idle_syscore_resume,
};

/* 可注册的 syscore 对象，封装上面的 ops，无动态 ownership。 */
static struct syscore psci_idle_syscore = {
	.ops = &psci_idle_syscore_ops,
};

/*
 * 若存在层级 topology，注册一次 syscore suspend/resume 桥接。
 * 无参数/返回；必须在 probe 完成 per-CPU 初始化后调用。
 */
static void psci_idle_init_syscore(void)
{
	if (psci_cpuidle_use_syscore)
		register_syscore(&psci_idle_syscore);
}

/*
 * 注册 CPUHP_AP_CPU_PM_STARTING 状态的上下线回调。
 *
 * nocalls 不为已经在线 CPU 补调用，避免重复改变 probe 已建立的 PM 状态；
 * 以后 hotplug 才触发 up/down。err 仅用于日志，不使整个 cpuidle probe 失败。
 */
static void psci_idle_init_cpuhp(void)
{
	int err;

	err = cpuhp_setup_state_nocalls(CPUHP_AP_CPU_PM_STARTING,
					"cpuidle/psci:online",
					psci_idle_cpuhp_up,
					psci_idle_cpuhp_down);
	if (err)
		pr_warn("Failed %d while setup cpuhp state\n", err);
}

/*
 * 非层级 PSCI idle 热路径。
 *
 * @dev/@drv 是当前 CPU cpuidle 对象；@idx 是 state 下标。读取当前 CPU
 * psci_states[idx]，再由 CPU_PM_CPU_IDLE_ENTER_PARAM_RCU 包装 CPU PM 通知、
 * RCU idle/context 约束并调用 psci_cpu_suspend_enter。成功返回 idx，失败
 * 返回宏约定的错误。__cpuidle 路径不可睡眠。
 */
static __cpuidle int psci_enter_idle_state(struct cpuidle_device *dev,
					   struct cpuidle_driver *drv, int idx)
{
	u32 *state = __this_cpu_read(psci_cpuidle_data.psci_states);

	return CPU_PM_CPU_IDLE_ENTER_PARAM_RCU(psci_cpu_suspend_enter, idx, state[idx]);
}

/* DT idle-state matcher：arm,idle-state 节点由通用解析器绑定到 PSCI enter。 */
static const struct of_device_id psci_idle_state_match[] = {
	{ .compatible = "arm,idle-state",
	  .data = psci_enter_idle_state },
	{ },
};

/*
 * 从一个 DT idle-state 节点解析 PSCI suspend 参数。
 *
 * @np：节点借用引用；@state：u32 输出槽，成功写入，失败内容不可使用。
 * 缺属性传播 OF errno；保留位不符合当前 original/extended 格式返回 -EINVAL。
 * 校验只检查编码合法位，具体 StateID 支持仍由固件决定。
 */
int psci_dt_parse_state_node(struct device_node *np, u32 *state)
{
	int err = of_property_read_u32(np, "arm,psci-suspend-param", state);

	if (err) {
		pr_warn("%pOF missing arm,psci-suspend-param property\n", np);
		return err;
	}

	if (!psci_power_state_is_valid(*state)) {
		pr_warn("Invalid PSCI power state %#x\n", *state);
		return -EINVAL;
	}

	return 0;
}

static int psci_dt_cpu_init_topology(struct cpuidle_driver *drv,
				     struct psci_cpuidle_data *data,
				     unsigned int state_count, int cpu)
{
	/* Currently limit the hierarchical topology to be used in OSI mode. */
	/*
	 * 层级 genpd 需要 OS 明确选择共享 state；固件只支持 PC 时保持普通 PSCI
	 * idle，不 attach domain，也不改 driver callbacks。
	 */
	if (!psci_has_osi_support())
		return 0;

	/*
	 * @drv 是当前 CPU driver；@data 是其 per-CPU 配置；@state_count 包含 WFI；
	 * @cpu 是逻辑号。attach 成功返回由 DT idle genpd 管理的 device 借用指针，
	 * 失败返回错误指针/NULL并转 errno。
	 */
	data->dev = dt_idle_attach_cpu(cpu, "psci");
	if (IS_ERR_OR_NULL(data->dev))
		return PTR_ERR_OR_ZERO(data->dev);

	psci_cpuidle_use_syscore = true;

	/*
	 * Using the deepest state for the CPU to trigger a potential selection
	 * of a shared state for the domain, assumes the domain states are all
	 * deeper states. On PREEMPT_RT the hierarchical topology is limited to
	 * s2ram and s2idle.
	 */
	/*
	 * 只替换最深 CPU state：domain states 假定比所有单 CPU state 更深，genpd
	 * governor 可在该入口选择共享层级。RT 普通 idle 不能走可能睡眠的 PM
	 * topology，仅安装 enter_s2idle；非 RT 同时安装普通 enter。
	 */
	drv->states[state_count - 1].enter_s2idle = psci_enter_s2idle_domain_idle_state;
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		drv->states[state_count - 1].enter = psci_enter_domain_idle_state;

	return 0;
}

/*
 * 为一个 CPU 建立 PSCI state 数组和可选 topology。
 *
 * @dev：devres owner；@drv：待完成的 per-CPU driver；@cpu_node：持有引用的
 * CPU DT 节点借用；@state_count：通用解析出的 DT idle state 数（不含 WFI）；
 * @cpu：逻辑 CPU。
 *
 * 分配 state_count+1 个 u32，index0 留给 WFI/0，依次解析每个 DT state。
 * 任一节点缺失/非法立即返回，devres 最终回收数组。全部成功后初始化 topology，
 * 最后发布 data->psci_states。返回 0/负 errno。
 */
static int psci_dt_cpu_init_idle(struct device *dev, struct cpuidle_driver *drv,
				 struct device_node *cpu_node,
				 unsigned int state_count, int cpu)
{
	int i, ret = 0;
	u32 *psci_states;
	struct device_node *state_node;
	struct psci_cpuidle_data *data = per_cpu_ptr(&psci_cpuidle_data, cpu);

	state_count++; /* Add WFI state too */
	/* index 0 的零初始化值就是不经 PSCI 的 WFI 占位 state。 */
	psci_states = devm_kcalloc(dev, state_count, sizeof(*psci_states),
				   GFP_KERNEL);
	if (!psci_states)
		return -ENOMEM;

	for (i = 1; i < state_count; i++) {
		/* DT state 序号比 driver index 少 1；get 返回引用，本轮必须 put。 */
		state_node = of_get_cpu_state_node(cpu_node, i - 1);
		if (!state_node)
			break;

		ret = psci_dt_parse_state_node(state_node, &psci_states[i]);
		of_node_put(state_node);

		if (ret)
			return ret;

		pr_debug("psci-power-state %#x index %d\n", psci_states[i], i);
	}

	if (i != state_count)
		return -ENODEV;

	/* Initialize optional data, used for the hierarchical topology. */
	/* topology 失败时不发布 psci_states，调用者可安全放弃该 CPU driver。 */
	ret = psci_dt_cpu_init_topology(drv, data, state_count, cpu);
	if (ret < 0)
		return ret;

	/* Idle states parsed correctly, store them in the per-cpu struct. */
	/* 发布点：此后 idle 热路径可按 index 无锁读取数组。 */
	data->psci_states = psci_states;
	return 0;
}

/*
 * 校验 PSCI CPU_SUSPEND 并解析某 CPU 的 DT idle 配置。
 *
 * @dev/@drv/@cpu/@state_count 透传到上面的构造函数。先要求 psci_ops.cpu_suspend
 * 已由固件 probe 发布，再取得 CPU node 引用；所有出口都 put。返回 0/负 errno，
 * 不在失败时发布半初始化 per-CPU state。
 */
static int psci_cpu_init_idle(struct device *dev, struct cpuidle_driver *drv,
			      unsigned int cpu, unsigned int state_count)
{
	struct device_node *cpu_node;
	int ret;

	/*
	 * If the PSCI cpu_suspend function hook has not been initialized
	 * idle states must not be enabled, so bail out
	 */
	/* 没有固件 suspend 回调时启用 DT idle state 会在热路径 NULL 调用。 */
	if (!psci_ops.cpu_suspend)
		return -EOPNOTSUPP;

	cpu_node = of_cpu_device_node_get(cpu);
	if (!cpu_node)
		return -ENODEV;

	ret = psci_dt_cpu_init_idle(dev, drv, cpu_node, state_count, cpu);

	of_node_put(cpu_node);

	return ret;
}

/*
 * 撤销某 CPU 的可选层级 idle 附着。
 *
 * @cpu：逻辑 CPU。dt_idle_detach_cpu 接受 NULL/已附着 device 的当前协议，
 * 释放 topology 关联；全局 syscore 需求标志清零。psci_states 由 devres 管理，
 * 此处不释放。用于注册失败回滚。
 */
static void psci_cpu_deinit_idle(int cpu)
{
	struct psci_cpuidle_data *data = per_cpu_ptr(&psci_cpuidle_data, cpu);

	dt_idle_detach_cpu(data->dev);
	psci_cpuidle_use_syscore = false;
}

/*
 * 为单个 CPU 构造并注册 PSCI cpuidle_driver。
 *
 * @dev：faux device/devres owner；@cpu：逻辑 CPU。
 * 阶段：验证 CPU enable-method -> 分配 driver -> 初始化 WFI index0 -> 通用
 * DT parser 填性能字段 -> 建立 PSCI state/topology -> cpuidle_register ->
 * cooling 注册。
 *
 * 返回 0 或负 errno。cpuidle_register 失败时 detach topology；更早失败尚未
 * 发布 driver。drv/state 数组由 devres 持有，成功后 cpuidle core 借用。
 */
static int psci_idle_init_cpu(struct device *dev, int cpu)
{
	/*
	 * drv 为待发布对象；cpu_node 是短期 OF 引用；enable_method 借用属性字符串；
	 * ret 串联各阶段错误和通用 DT parser 返回的 state 数。
	 */
	struct cpuidle_driver *drv;
	struct device_node *cpu_node;
	const char *enable_method;
	int ret = 0;

	cpu_node = of_cpu_device_node_get(cpu);
	if (!cpu_node)
		return -ENODEV;

	/*
	 * Check whether the enable-method for the cpu is PSCI, fail
	 * if it is not.
	 */
	/* 只有由 PSCI 启动的 CPU 才能安全复用其固件 idle state。 */
	enable_method = of_get_property(cpu_node, "enable-method", NULL);
	if (!enable_method || (strcmp(enable_method, "psci")))
		ret = -ENODEV;

	of_node_put(cpu_node);
	if (ret)
		return ret;

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->name = "psci_idle";
	drv->owner = THIS_MODULE;
	drv->cpumask = (struct cpumask *)cpumask_of(cpu);

	/*
	 * PSCI idle states relies on architectural WFI to be represented as
	 * state index 0.
	 */
	/*
	 * cpuidle core 约定 index0 为最浅 WFI：1us 延迟/驻留是保守占位，UINT_MAX
	 * power_usage 表示未知。它仍走通用 enter 宏，但 state[0] 为 0。
	 */
	drv->states[0].enter = psci_enter_idle_state;
	drv->states[0].exit_latency = 1;
	drv->states[0].target_residency = 1;
	drv->states[0].power_usage = UINT_MAX;
	strscpy(drv->states[0].name, "WFI");
	strscpy(drv->states[0].desc, "ARM WFI");

	/*
	 * If no DT idle states are detected (ret == 0) let the driver
	 * initialization fail accordingly since there is no reason to
	 * initialize the idle driver if only wfi is supported, the
	 * default archictectural back-end already executes wfi
	 * on idle entry.
	 */
	/*
	 * dt_init_idle_driver 从 index1 开始填充并返回检测到的 DT state 数；若只有
	 * WFI，架构默认 idle 已足够，不注册一个无增益的独立 driver。
	 */
	ret = dt_init_idle_driver(drv, psci_idle_state_match, 1);
	if (ret <= 0)
		return ret ? : -ENODEV;

	/*
	 * Initialize PSCI idle states.
	 */
	/* ret 作为 DT state_count 传入，psci_cpu_init_idle 内部再加入 WFI。 */
	ret = psci_cpu_init_idle(dev, drv, cpu, ret);
	if (ret) {
		pr_err("CPU %d failed to PSCI idle\n", cpu);
		return ret;
	}

	ret = cpuidle_register(drv, NULL);
	if (ret)
		goto deinit;

	cpuidle_cooling_register(drv);

	return 0;
deinit:
	/* 到达时 topology 可能已 attach，但 cpuidle core 未持有成功注册。 */
	psci_cpu_deinit_idle(cpu);
	return ret;
}

/*
 * psci_idle_probe - Initializes PSCI cpuidle driver
 *
 * Initializes PSCI cpuidle driver for all present CPUs, if any CPU fails
 * to register cpuidle driver then rollback to cancel all CPUs
 * registration.
 */
/*
 * faux-device probe：为所有 present CPU 原子式地建立 cpuidle 服务。
 *
 * @fdev：提供 devres 生命周期的 faux device。逐 CPU 注册；任一失败则对此前
 * CPU 逆序 cpuidle_unregister + topology detach，保证不留下部分覆盖。全部
 * 成功后才注册全局 syscore/CPUHP 回调。
 *
 * 返回 0/首个 errno。cpu 是当前阶段/回滚边界；drv/dev 在回滚时从 per-CPU
 * cpuidle core 查询，均为借用指针。
 */
static int psci_cpuidle_probe(struct faux_device *fdev)
{
	int cpu, ret;
	struct cpuidle_driver *drv;
	struct cpuidle_device *dev;

	for_each_present_cpu(cpu) {
		ret = psci_idle_init_cpu(&fdev->dev, cpu);
		if (ret)
			goto out_fail;
	}

	/* 只有所有 CPU 已发布成功，辅助回调才可能安全访问完整 per-CPU 数据。 */
	psci_idle_init_syscore();
	psci_idle_init_cpuhp();
	return 0;

out_fail:
	/* cpu 指向失败项；--cpu 从最后一个成功 CPU 开始逆序撤销。 */
	while (--cpu >= 0) {
		dev = per_cpu(cpuidle_devices, cpu);
		drv = cpuidle_get_cpu_driver(dev);
		cpuidle_unregister(drv);
		psci_cpu_deinit_idle(cpu);
	}

	return ret;
}

/* faux device 操作表只提供 probe；对象静态存续。 */
static struct faux_device_ops psci_cpuidle_ops = {
	.probe = psci_cpuidle_probe,
};

/*
 * 快速判断 boot CPU DT 是否至少存在一个 arm,idle-state。
 *
 * 无参数；取 possible mask 首 CPU 的 node 和 index0 state node，使用
 * __free(device_node) 在作用域退出时自动 of_node_put。返回匹配布尔值，
 * 不保存引用。声明位置体现 cleanup class 的逆序自动释放 ownership。
 */
static bool __init dt_idle_state_present(void)
{
	struct device_node *cpu_node __free(device_node) =
			of_cpu_device_node_get(cpumask_first(cpu_possible_mask));
	if (!cpu_node)
		return false;

	struct device_node *state_node __free(device_node) =
			of_get_cpu_state_node(cpu_node, 0);
	if (!state_node)
		return false;

	return !!of_match_node(psci_idle_state_match, state_node);
}

/*
 * device initcall：按需创建 PSCI cpuidle faux device。
 *
 * 无 DT idle state 时返回 0、不创建对象。存在时 faux_device_create 触发上述
 * probe，并让 faux device/devres 持有所有 driver 分配；NULL 视为 -ENODEV。
 * 成功对象由 faux bus 管理，本文件无需保存 fdev。
 */
static int __init psci_idle_init(void)
{
	struct faux_device *fdev;

	if (!dt_idle_state_present())
		return 0;

	fdev = faux_device_create("psci-cpuidle", NULL, &psci_cpuidle_ops);
	if (!fdev) {
		pr_err("Failed to create psci-cpuidle device\n");
		return -ENODEV;
	}

	return 0;
}
/* PSCI 固件与设备模型可用后再创建每 CPU idle drivers。 */
device_initcall(psci_idle_init);
