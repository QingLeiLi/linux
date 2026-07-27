// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 每 CPU 启停协议选择与发布学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件把固件提供的协议名称解析成 cpu_operations 指针：
 *
 *   DT:   cpu node "enable-method"
 *   ACPI: FADT PSCI 标志 / MADT parking protocol
 *              |
 *              v
 *       cpu_read_enable_method()
 *              -> cpu_get_ops()
 *              -> cpu_ops[logical_cpu]
 *
 * DT 支持 spin-table 与 PSCI；ACPI 支持 PSCI，以及配置允许时的 parking
 * protocol。选择阶段只绑定静态操作表，不执行 cpu_init/prepare/boot；
 * 后续 smp.c 按“枚举 -> 准备 -> 上线 -> 热下线”阶段调用表内回调。
 *
 * cpu_ops[] 在启动期由 boot CPU 单写，完成后成为 __ro_after_init；SMP 和
 * hotplug 路径无锁读取、只借用指针。支持表本身是 __initconst，初始化结束
 * 可回收，所以运行期绝不能再扫描它们，只能使用已发布到 cpu_ops[] 的静态
 * 操作表指针。
 *
 * boot CPU 已经由固件带入内核，不依赖 enable-method 完成当前启动，因此
 * DT/ACPI 都允许 CPU0 缺少协议而不产生伪告警；代价是 cpu_ops[0] 可能为
 * NULL，热插拔等运行期消费者必须检查。
 */
/*
 * CPU kernel entry/exit control
 *
 * Copyright (C) 2013 ARM Ltd.
 */
/*
 * 本文件负责选择 CPU 进入/退出内核所使用的固件协议；真正的 mailbox、
 * SMC、cache maintenance 和 CPU 状态转换由各 cpu_operations 实现完成。
 */

#include <linux/acpi.h>
#include <linux/cache.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/string.h>
#include <asm/acpi.h>
#include <asm/cpu_ops.h>
#include <asm/smp_plat.h>

/*
 * 可被选择的静态操作表定义：
 *   smp_spin_table_ops          DT spin-table；
 *   acpi_parking_protocol_ops   可选 ACPI parking protocol；
 *   cpu_psci_ops                DT/ACPI 都可使用的 PSCI。
 *
 * 本文件只借用这些 const 对象，不拥有或释放它们；生命周期覆盖整个内核。
 */
extern const struct cpu_operations smp_spin_table_ops;
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
extern const struct cpu_operations acpi_parking_protocol_ops;
#endif
extern const struct cpu_operations cpu_psci_ops;

/*
 * 每个逻辑 CPU 已绑定的操作表。
 *
 * 下标范围为 [0,NR_CPUS)，元素初始 NULL。init_cpu_ops() 在早期单 CPU 阶段
 * 写入；初始化完成后 __ro_after_init 使所在内存只读，既防止意外改写，也
 * 固化 get_cpu_ops() 无锁读取所依赖的“一次发布、永不替换”不变量。
 * 指针是借用关系，不需要引用计数。
 */
static const struct cpu_operations *cpu_ops[NR_CPUS] __ro_after_init;

/*
 * DT enable-method 的支持集合，以 NULL 哨兵结束。数组和元素指针均 const：
 * 初始化代码可推进扫描指针，但不能替换表项或修改操作表。__initconst 允许
 * 链接器在启动后回收该选择目录。
 */
static const struct cpu_operations *const dt_supported_cpu_ops[] __initconst = {
	&smp_spin_table_ops,
	&cpu_psci_ops,
	NULL,
};

/*
 * ACPI 启动协议目录。parking protocol 只有配置启用时才参与匹配；PSCI 始终
 * 保留为 ACPI 标准主路径。与 DT 表一样只在 __init 阶段可访问。
 */
static const struct cpu_operations *const acpi_supported_cpu_ops[] __initconst = {
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
	&acpi_parking_protocol_ops,
#endif
	&cpu_psci_ops,
	NULL,
};

/*
 * cpu_get_ops() - 在当前固件类型的支持目录中按协议名查找操作表。
 *
 * @name：借用的 NUL 结尾协议字符串，不可为 NULL；DT 路径通常指向
 * enable-method property，ACPI 路径通常是静态字符串。本函数不保存 name。
 *
 * acpi_disabled 决定扫描 DT 还是 ACPI 目录。匹配成功返回静态 const
 * cpu_operations 的借用指针，失败返回 NULL。函数不修改全局状态、不分配、
 * 不睡眠；仅限 __init，因为两个目录稍后会被回收。
 */
static const struct cpu_operations * __init cpu_get_ops(const char *name)
{
	/*
	 * ops 是“指向操作表指针的指针”，用于遍历 NULL 结尾目录；它自身只是
	 * 栈上游标，不改变目录元素。
	 */
	const struct cpu_operations *const *ops;

	/* 系统只选择一种固件描述来源，不会把两个目录混合或做跨表 fallback。 */
	ops = acpi_disabled ? dt_supported_cpu_ops : acpi_supported_cpu_ops;

	/* 每个 name 在对应目录中应唯一；首次 strcmp 相等项即为最终实现。 */
	while (*ops) {
		if (!strcmp(name, (*ops)->name))
			return *ops;

		ops++;
	}

	return NULL;
}

/*
 * cpu_read_enable_method() - 从 DT 或 ACPI 读取一个逻辑 CPU 的启动协议名。
 *
 * @cpu：逻辑 CPU 号，范围应为 0..NR_CPUS-1，纯输入。调用前
 * cpu_logical_map[cpu] 已建立，使 DT/ACPI helper 能定位固件 CPU 描述。
 *
 * 成功返回借用的 NUL 结尾字符串，不复制、不转移 ownership；DT property
 * 属于启动设备树，ACPI helper 返回协议静态名。失败返回 NULL。函数仅在
 * 启动期调用，不持运行期锁；DT 分支临时取得 device_node 引用，并在所有
 * 成功查找路径上用 of_node_put() 释放。
 *
 * CPU0 已经运行，缺少 enable method 是合法固件描述，相关分支避免告警；
 * secondary CPU 缺少协议则无法由内核启动，会记录诊断。
 */
static const char *__init cpu_read_enable_method(int cpu)
{
	/* enable_method 是短期借用字符串，只传给紧随其后的匹配逻辑。 */
	const char *enable_method;

	/* DT 和 ACPI 是互斥的系统级启动描述来源。 */
	if (acpi_disabled) {
		/*
		 * of_get_cpu_node() 根据逻辑映射定位 CPU 节点并取得引用。dn 仅在
		 * 本分支有效，属性查找完成后必须 put。
		 */
		struct device_node *dn = of_get_cpu_node(cpu, NULL);

		/* boot CPU 节点缺失值得单独诊断；secondary 枚举层会处理其不可用。 */
		if (!dn) {
			if (!cpu)
				pr_err("Failed to find device node for boot cpu\n");
			return NULL;
		}

		enable_method = of_get_property(dn, "enable-method", NULL);
		if (!enable_method) {
			/*
			 * The boot CPU may not have an enable method (e.g.
			 * when spin-table is used for secondaries).
			 * Don't warn spuriously.
			 */
			/*
			 * boot CPU 可以没有 enable-method，例如 spin-table 只描述
			 * secondary；此时不要产生误导告警。secondary 缺失则是真正
			 * 无法启动的固件问题，日志带节点路径。
			 */
			if (cpu != 0)
				pr_err("%pOF: missing enable-method property\n",
					dn);
		}
		/*
		 * 释放节点引用不会复制属性字符串；启动期静态 DT 的 property 存储
		 * 仍由设备树拥有，返回值只在随后的 init_cpu_ops() 匹配期间借用。
		 */
		of_node_put(dn);
	} else {
		/*
		 * ACPI helper 优先根据 FADT 选择 PSCI，否则检查该 CPU 的 parking
		 * protocol mailbox；返回静态协议名或 NULL，不产生引用。
		 */
		enable_method = acpi_get_enable_method(cpu);
		if (!enable_method) {
			/*
			 * In ACPI systems the boot CPU does not require
			 * checking the enable method since for some
			 * boot protocol (ie parking protocol) it need not
			 * be initialized. Don't warn spuriously.
			 */
			/*
			 * ACPI boot CPU 同样无需靠本表被启动；某些协议（如 parking
			 * protocol）甚至不要求初始化 CPU0。secondary 返回 NULL 才
			 * 记录“不支持的 ACPI enable-method”。
			 */
			if (cpu != 0)
				pr_err("Unsupported ACPI enable-method\n");
		}
	}

	return enable_method;
}
/*
 * Read a cpu's enable method and record it in cpu_ops.
 */
/*
 * 读取指定 CPU 的 enable method，并把匹配到的静态操作表发布到 cpu_ops。
 *
 * @cpu：逻辑 CPU 号，调用者保证范围有效。boot CPU 由
 * init_bootcpu_ops() 尝试绑定；secondary CPU 由 smp_cpu_setup() 在设置
 * possible mask 前调用。
 *
 * 函数运行于 __init 单写阶段，不持锁、不分配。成功返回 0，此后
 * get_cpu_ops(cpu) 返回有效借用指针；缺少协议返回 -ENODEV；协议字符串存在
 * 但不在当前 DT/ACPI 支持目录中返回 -EOPNOTSUPP 并记录名称。
 *
 * 本函数只完成选择和发布，不调用 cpu_init()。secondary 的每 CPU 协议数据
 * 由 smp_cpu_setup() 随后初始化；失败 CPU 不会进入 possible mask。
 */
int __init init_cpu_ops(int cpu)
{
	/* enable_method 的存储归 DT/ACPI 所有，只在本函数查找期间借用。 */
	const char *enable_method = cpu_read_enable_method(cpu);

	/* 没有名称就没有可匹配实现，cpu_ops[cpu] 保持其启动期初值 NULL。 */
	if (!enable_method)
		return -ENODEV;

	/*
	 * 实际发布点：cpu_get_ops() 只返回静态对象，赋值不转移 ownership。
	 * 未匹配时也显式写 NULL，避免消费者误用一个不存在的协议。
	 */
	cpu_ops[cpu] = cpu_get_ops(enable_method);
	if (!cpu_ops[cpu]) {
		pr_warn("Unsupported enable-method: %s\n", enable_method);
		return -EOPNOTSUPP;
	}

	return 0;
}

/*
 * get_cpu_ops() - 返回逻辑 CPU 已发布的启动/热插拔操作表。
 *
 * @cpu：逻辑 CPU 号，调用者必须保证在 [0,NR_CPUS) 内；本函数不做边界检查。
 * 返回静态 const 操作表的借用指针或 NULL，不增加引用、不允许调用者修改。
 *
 * 启动期写入完成后 cpu_ops[] 为 __ro_after_init，SMP/hotplug 路径可无锁读取。
 * NULL 常见于固件未描述 boot CPU 协议或 secondary 初始化失败；每个调用者
 * 必须按所在阶段决定拒绝操作还是采用退化行为。
 */
const struct cpu_operations *get_cpu_ops(int cpu)
{
	return cpu_ops[cpu];
}
