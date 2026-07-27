// SPDX-License-Identifier: GPL-2.0
/*
 * PSCI CPU 层级电源域与 OSI 模式学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件把 PSCI DT 节点下的 power-domain 子节点转换为 generic PM domains，
 * 建立 CPU/cluster 等 master-subdomain 拓扑，并在固件支持 OS-initiated
 * suspend 时切换 OSI：
 *
 *   core_initcall 注册 platform driver
 *   -> probe 遍历 #power-domain-cells 子节点
 *   -> dt_idle_pd_alloc 解析 domain idle states
 *   -> pm_genpd_init + OF provider 发布
 *   -> dt_idle_pd_init_topology 建立层级
 *   -> psci_set_osi_mode(true/false)
 *
 * OSI 下 genpd governor 选择共享 domain state，psci_pd_power_off() 不直接
 * 调固件，而把 state 写入当前 CPU 的 per-CPU 临时槽；随后 cpuidle 主驱动
 * 在同一次 idle 进入中把它提交给 PSCI。这样避免 domain callback 与 CPU
 * suspend 分成两个固件事务。
 *
 * provider list 记录成功发布对象的 DT 引用和回滚顺序。probe/删除由设备模型
 * 串行，链表无需额外锁；idle 热路径只读已发布 genpd，并使用 genpd/per-CPU
 * 协议同步。PREEMPT_RT 禁止普通 runtime idle 关闭 domain，但保留 system
 * suspend 使用。
 */
/*
 * PM domains for CPUs via genpd - managed by cpuidle-psci.
 *
 * Copyright (C) 2019 Linaro Ltd.
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *
 */

/* 为本文件日志添加 "CPUidle PSCI: " 前缀。 */
#define pr_fmt(fmt) "CPUidle PSCI: " fmt

#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/psci.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "cpuidle-psci.h"
#include "dt_idle_genpd.h"

/*
 * 一个已发布 PSCI genpd provider 的回滚记录。
 *
 * link 嵌入全局链表；node 是通过 of_node_get 持有的 DT 引用。genpd 本体由
 * OF provider/通用 DT idle helper 管理，删除时通过 node 反查并释放。
 */
struct psci_pd_provider {
	struct list_head link;
	struct device_node *node;
};

/* 成功 provider 的发布顺序链表；probe 失败时反向删除以尊重层级依赖。 */
static LIST_HEAD(psci_pd_providers);

/*
 * OSI genpd power_off 回调：把选中的共享 state 传给当前 CPU idle 路径。
 *
 * @pd：正在逻辑关断的 CPU PM domain，借用指针。pd->state_idx 已由 governor
 * 选择；state->data 指向 dt_idle_pd_alloc 保存的 u32 PSCI state。
 *
 * 没有 data 表示该状态无需 PSCI override，返回 0。有效时调用
 * psci_set_domain_state 记录 pd/index/state；不在此处调用固件。回调运行在
 * 当前 idle CPU 的 genpd 路径，per-CPU 写入要求不得迁移。始终返回 0。
 */
static int psci_pd_power_off(struct generic_pm_domain *pd)
{
	struct genpd_power_state *state = &pd->states[pd->state_idx];
	u32 *pd_state;

	if (!state->data)
		return 0;

	/* OSI mode is enabled, set the corresponding domain state. */
	/* 延迟到 CPU suspend 统一提交，保证 CPU 与共享 domain 状态是一个事务。 */
	pd_state = state->data;
	psci_set_domain_state(pd, pd->state_idx, *pd_state);

	return 0;
}

/*
 * 从一个 DT 子节点创建并发布 PSCI generic PM domain。
 *
 * @np：带 #power-domain-cells 的节点借用引用。
 * @use_osi：固件是否支持 OSI；true 安装 power_off，false 把 domain 固定
 *           ALWAYS_ON，仅保留拓扑描述。
 *
 * 阶段：分配/解析 pd -> 分配 provider 记录 -> 设置 IRQ_SAFE/CPU_DOMAIN 与
 * RT/OSI flags -> 选择 governor -> pm_genpd_init -> OF provider 发布 ->
 * 持有 node 引用并加入全局链表。
 *
 * 返回 0/负 errno。每个失败标签只撤销已取得资源并逆序落下：provider 发布
 * 失败先 remove genpd，再释放记录和 pd；成功后 ownership 由 provider list
 * 记录，psci_pd_remove() 统一回收。
 */
static int psci_pd_init(struct device_node *np, bool use_osi)
{
	/*
	 * pd 是动态 domain；pd_provider 是回滚记录；pd_gov 是静态 governor 借用；
	 * ret 默认 -ENOMEM，随后保存每个发布阶段的精确错误。
	 */
	struct generic_pm_domain *pd;
	struct psci_pd_provider *pd_provider;
	struct dev_power_governor *pd_gov;
	int ret = -ENOMEM;

	pd = dt_idle_pd_alloc(np, psci_dt_parse_state_node);
	if (!pd)
		goto out;

	pd_provider = kzalloc_obj(*pd_provider);
	if (!pd_provider)
		goto free_pd;

	pd->flags |= GENPD_FLAG_IRQ_SAFE | GENPD_FLAG_CPU_DOMAIN;

	/*
	 * Allow power off when OSI has been successfully enabled.
	 * On a PREEMPT_RT based configuration the domain idle states are
	 * supported, but only during system-wide suspend.
	 */
	/*
	 * OSI 允许 governor 逻辑 power-off 并设置共享 state。ACTIVE_WAKEUP 保持唤醒
	 * 语义；RT 配置以 RPM_ALWAYS_ON 禁止普通 runtime idle 关 domain，但
	 * system-wide suspend 仍可显式选择。PC 模式则始终不让 OS 关 domain。
	 */
	if (use_osi) {
		pd->power_off = psci_pd_power_off;
		pd->flags |= GENPD_FLAG_ACTIVE_WAKEUP;
		if (IS_ENABLED(CONFIG_PREEMPT_RT))
			pd->flags |= GENPD_FLAG_RPM_ALWAYS_ON;
	} else {
		pd->flags |= GENPD_FLAG_ALWAYS_ON;
	}

	/* Use governor for CPU PM domains if it has some states to manage. */
	/* 无 states 时不需要 governor 做驻留/延迟选择。 */
	pd_gov = pd->states ? &pm_domain_cpu_gov : NULL;

	/* 发布阶段 1：初始化 genpd 内部状态；false 表示初始为 active。 */
	ret = pm_genpd_init(pd, pd_gov, false);
	if (ret)
		goto free_pd_prov;

	/* 发布阶段 2：让 DT consumer 可通过 phandle 获取这个 domain。 */
	ret = of_genpd_add_provider_simple(np, pd);
	if (ret)
		goto remove_pd;

	/* provider 已全局可发现后才持有 node 并记账，供完整逆序删除。 */
	pd_provider->node = of_node_get(np);
	list_add(&pd_provider->link, &psci_pd_providers);

	pr_debug("init PM domain %s\n", pd->name);
	return 0;

remove_pd:
	/* genpd 已初始化但 OF provider 未发布成功。 */
	pm_genpd_remove(pd);
free_pd_prov:
	/* provider 记录尚未进入全局链表。 */
	kfree(pd_provider);
free_pd:
	/* 释放 dt_idle_pd_alloc 创建的 states/name/domain 存储。 */
	dt_idle_pd_free(pd);
out:
	pr_err("failed to init PM domain ret=%d %pOF\n", ret, np);
	return ret;
}

/*
 * 逆序删除本驱动创建的全部 PSCI providers/domains。
 *
 * 无参数/返回。safe_reverse 允许边遍历边删除，并先撤销后创建的子层对象。
 * 对每项：阻止新 OF 查找 -> 取回最后一个 genpd -> 释放 domain -> put DT
 * 引用 -> 摘链并释放记录。若 remove_last 返回错误指针，仍完成其余元数据清理。
 *
 * 仅在 probe 失败回滚调用，设备模型串行，链表无并发 reader。
 */
static void psci_pd_remove(void)
{
	struct psci_pd_provider *pd_provider, *it;
	struct generic_pm_domain *genpd;

	list_for_each_entry_safe_reverse(pd_provider, it,
					 &psci_pd_providers, link) {
		of_genpd_del_provider(pd_provider->node);

		/* provider 已不可发现后，才能安全移除/释放 genpd。 */
		genpd = of_genpd_remove_last(pd_provider->node);
		if (!IS_ERR(genpd))
			kfree(genpd);

		of_node_put(pd_provider->node);
		list_del(&pd_provider->link);
		kfree(pd_provider);
	}
}

/* 只绑定 PSCI 1.0 节点，因为 OSI/标准 power-domain 描述从该代开始。 */
static const struct of_device_id psci_of_match[] = {
	{ .compatible = "arm,psci-1.0" },
	{}
};

/*
 * PSCI cpuidle domain platform-driver probe。
 *
 * @pdev：匹配 PSCI 1.0 节点的平台设备，of_node 为借用指针。
 * 遍历具有 #power-domain-cells 的子节点，为每项创建 provider；没有任何项
 * 返回 0，保留非层级 CPU idle。存在时再连接 master/subdomain topology，
 * 最后请求固件切换 use_osi 对应模式。
 *
 * 返回 0/负 errno。pd_count 标识是否需要 topology；ret 保存当前阶段错误。
 * provider 创建中途失败走 exit，保留当前代码已经创建的 providers；topology
 * 或模式切换失败则先移除 topology 再删除全部 providers。
 */
static int psci_cpuidle_domain_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	bool use_osi = psci_has_osi_support();
	int ret = 0, pd_count = 0;

	if (!np)
		return -ENODEV;

	/*
	 * Parse child nodes for the "#power-domain-cells" property and
	 * initialize a genpd/genpd-of-provider pair when it's found.
	 */
	/* scoped 迭代自动 put 每个 child 引用，continue/goto 均不会泄漏。 */
	for_each_child_of_node_scoped(np, node) {
		if (!of_property_present(node, "#power-domain-cells"))
			continue;

		ret = psci_pd_init(node, use_osi);
		if (ret)
			goto exit;

		pd_count++;
	}

	/* Bail out if not using the hierarchical CPU topology. */
	/* 零 provider 不是错误，主 cpuidle 驱动仍可使用单 CPU states。 */
	if (!pd_count)
		return 0;

	/* Link genpd masters/subdomains to model the CPU topology. */
	/* 所有 provider 可查找后才能按 DT phandle 建立父子关系。 */
	ret = dt_idle_pd_init_topology(np);
	if (ret)
		goto remove_pd;

	/* let's try to enable OSI. */
	/*
	 * use_osi=false 时也显式提交 PC，保证固件模式与创建出的 ALWAYS_ON domain
	 * 策略一致；切换失败不能留下 OS/固件认知不一致的 topology。
	 */
	ret = psci_set_osi_mode(use_osi);
	if (ret)
		goto remove_pd;

	pr_info("Initialized CPU PM domain topology using %s mode\n",
		use_osi ? "OSI" : "PC");
	return 0;

remove_pd:
	/* topology 可能已部分/全部连接，先拆关系再释放 provider/domain。 */
	dt_idle_pd_remove_topology(np);
	psci_pd_remove();
exit:
	pr_err("failed to create CPU PM domains ret=%d\n", ret);
	return ret;
}

/* 静态 platform driver：probe 建立 topology，无 remove（启动期常驻）。 */
static struct platform_driver psci_cpuidle_domain_driver = {
	.probe  = psci_cpuidle_domain_probe,
	.driver = {
		.name = "psci-cpuidle-domain",
		.of_match_table = psci_of_match,
	},
};

/*
 * core initcall：尽早注册 domain driver，使 provider/topology 在 PSCI cpuidle
 * device_initcall 建立每 CPU driver 前可用。返回 platform driver 注册 errno。
 */
static int __init psci_idle_init_domains(void)
{
	return platform_driver_register(&psci_cpuidle_domain_driver);
}
/* core_initcall 顺序是 domain provider 先于主 cpuidle device。 */
core_initcall(psci_idle_init_domains);
