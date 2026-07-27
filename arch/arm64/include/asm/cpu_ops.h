/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 CPU 启停协议操作表学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * ARM64 支持多种把 CPU 带入/带出内核的固件协议。设备树 cpu 节点的
 * enable-method 或 ACPI MADT 描述协议名称，cpu_ops.c 据此为每个逻辑 CPU
 * 选择一个静态 const struct cpu_operations：
 *
 *   固件描述 -> init_cpu_ops() 绑定操作表
 *       -> cpu_init() 读取每 CPU 启动参数
 *       -> cpu_prepare() 做一次性可启动检查/释放准备
 *       -> cpu_boot() 在控制 CPU 上发起启动
 *       -> secondary_start_kernel()
 *       -> cpu_postboot() 在新 CPU 上完成协议收尾
 *
 *   热下线：
 *       cpu_can_disable() 能力查询
 *       -> cpu_disable() 在目标 CPU 上做可失败、可取消检查
 *       -> 通用层越过 point-of-no-return
 *       -> cpu_die() 在目标 CPU 上永久离开内核
 *       -> cpu_kill() 在控制 CPU 上确认硬件/固件最终状态
 *
 * 操作表实现包括 PSCI、DT spin-table 和可选 ACPI parking protocol。表和函数
 * 指针均由静态存储拥有，per-CPU cpu_ops[] 只借用指针，并在初始化后
 * __ro_after_init；运行期读取无锁且没有引用计数。协议回调负责固件寄存器、
 * mailbox、cache 可见性和错误语义，通用 SMP 层负责 CPU mask、IPI、调度及
 * hotplug 状态机。
 */
/*
 * Copyright (C) 2013 ARM Ltd.
 */
#ifndef __ASM_CPU_OPS_H
#define __ASM_CPU_OPS_H

#include <linux/init.h>
#include <linux/threads.h>

/**
 * struct cpu_operations - Callback operations for hotplugging CPUs.
 *
 * @name:	Name of the property as appears in a devicetree cpu node's
 *		enable-method property. On systems booting with ACPI, @name
 *		identifies the struct cpu_operations entry corresponding to
 *		the boot protocol specified in the ACPI MADT table.
 * @cpu_init:	Reads any data necessary for a specific enable-method for a
 *		proposed logical id.
 * @cpu_prepare: Early one-time preparation step for a cpu. If there is a
 *		mechanism for doing so, tests whether it is possible to boot
 *		the given CPU.
 * @cpu_boot:	Boots a cpu into the kernel.
 * @cpu_postboot: Optionally, perform any post-boot cleanup or necessary
 *		synchronisation. Called from the cpu being booted.
 * @cpu_can_disable: Determines whether a CPU can be disabled based on
 *		mechanism-specific information.
 * @cpu_disable: Prepares a cpu to die. May fail for some mechanism-specific
 * 		reason, which will cause the hot unplug to be aborted. Called
 * 		from the cpu to be killed.
 * @cpu_die:	Makes a cpu leave the kernel. Must not fail. Called from the
 *		cpu being killed.
 * @cpu_kill:  Ensures a cpu has left the kernel. Called from another cpu.
 */
/*
 * struct cpu_operations - 某一种 ARM64 CPU enable/disable 协议的回调集合。
 *
 * 上述英文标题强调 hotplug，但当前接口同时服务系统启动时的 secondary CPU
 * bring-up。cpu_ops.c 根据 @name 选中静态对象，并把其指针保存到对应逻辑
 * CPU；结构体自身不拥有 CPU、固件句柄或动态资源，也没有析构回调。
 *
 * @name：不可为 NULL 的只读协议名。DT 下匹配 cpu 节点 enable-method；
 * ACPI 下匹配 MADT 指定启动协议对应的名称。字符串和操作表生命周期必须
 * 覆盖所有使用它的 CPU。
 *
 * @cpu_init：在枚举 secondary CPU、已有逻辑 CPU 号和硬件 ID 映射后调用，
 * 参数是逻辑 CPU 号。读取协议所需的每 CPU 数据，例如 spin-table 的
 * cpu-release-addr。返回 0 表示该 CPU 可继续枚举，负 errno 使其不进入
 * possible mask。所选启动协议必须提供该回调；它属于 __init 调用阶段。
 *
 * @cpu_prepare：控制 CPU 在 smp_prepare_cpus() 中对每个 possible secondary
 * 执行的一次性早期准备。可映射 mailbox、写启动入口、唤醒 bootloader
 * holding state，或验证固件具备 CPU_ON。返回 0 后 CPU 才可标记 present；
 * 失败使该 CPU 本次启动不可用。所选协议必须提供，允许使用初始化期资源。
 *
 * @cpu_boot：控制 CPU 每次上线时调用，参数是目标逻辑 CPU。负责向固件或
 * mailbox 提交 secondary_entry/硬件 ID 并触发唤醒。返回 0 只表示启动请求
 * 已接受；通用层仍等待目标 CPU handshake。负 errno 中止上线。字段可为
 * NULL，此时通用层返回 -EOPNOTSUPP。
 *
 * @cpu_postboot：可选，无参数。在 newly booted CPU 的
 * secondary_start_kernel() 中、CPU 正式 online 之前调用，用于确认 firmware
 * 清理 mailbox 或完成协议同步。返回类型为 void，不能通过该接口拒绝上线；
 * 回调不能保留临时启动映射之外的无效指针。
 *
 * CONFIG_HOTPLUG_CPU 下还有四个阶段：
 *
 * @cpu_can_disable：只读能力查询，参数是逻辑 CPU 号。可根据协议私有状态
 * 排除不能下线的 CPU（例如 Trusted OS 驻留 CPU）。返回 false 或缺失回调
 * 都使架构把该 CPU 视为不可 hotplug；不得在查询中启动不可逆转换。
 *
 * @cpu_disable：在将被移除的目标 CPU 上、越过 point-of-no-return 前调用。
 * 可执行协议检查；返回 0 允许继续，负 errno 取消热下线且 CPU 仍可运行。
 * 字段可为 NULL，表示没有额外准备，但前提是 cpu_die 存在。
 *
 * @cpu_die：在目标 CPU 已从 online mask 摘除、IRQ 已迁移且向 hotplug core
 * 报告 dead 后调用。必须完成 cache/固件要求并让 CPU 永久停止；成功不应
 * 返回，void 签名也没有可恢复错误通道。缺失时热下线在早期被拒绝；异常
 * 返回会落入通用层 BUG()。
 *
 * @cpu_kill：在另一个控制 CPU 上、目标执行 cpu_die 后调用，用固件状态或
 * 协议同步确认目标不再访问内核资源。返回 0 表示确认完成，负 errno 表示
 * 可能未干净关闭；字段缺失时通用层无法确认，只能假定目标已经死亡。
 *
 * 并发与 ownership：选择/写入 per-CPU ops 发生在启动期，运行期回调只读
 * 操作表。CPU hotplug core 串行化同一 CPU 的状态迁移；本结构不替具体实现
 * 提供锁、引用或内存屏障，各协议必须自行满足跨 CPU/固件的可见性约束。
 */
struct cpu_operations {
	/* 固件描述中的协议选择键；不由 cpu_ops[] 复制或释放。 */
	const char	*name;
	/* 以下四项按“读取参数 -> 一次准备 -> 发起启动 -> 目标 CPU 收尾”排序。 */
	int		(*cpu_init)(unsigned int);
	int		(*cpu_prepare)(unsigned int);
	int		(*cpu_boot)(unsigned int);
	void		(*cpu_postboot)(void);
#ifdef CONFIG_HOTPLUG_CPU
	/*
	 * 热下线字段只在启用 CPU hotplug 时进入结构布局；所有定义和消费者必须
	 * 在同一内核配置下编译，不能把该结构当作稳定用户 ABI。
	 */
	bool		(*cpu_can_disable)(unsigned int cpu);
	int		(*cpu_disable)(unsigned int cpu);
	void		(*cpu_die)(unsigned int cpu);
	int		(*cpu_kill)(unsigned int cpu);
#endif
};

/*
 * init_cpu_ops() - 为一个逻辑 CPU 选择并记录启动协议操作表。
 *
 * @cpu：逻辑 CPU 号，范围应为 0..NR_CPUS-1，纯输入。函数从 DT/ACPI 读取
 * enable method，在体系结构支持表中按 name 匹配，并把静态 ops 借用指针
 * 写入 cpu_ops[cpu]；它不调用 ops->cpu_init()。
 *
 * 仅在启动期调用，不持运行期锁。成功返回 0；缺少 enable method 返回
 * -ENODEV；名称存在但内核不支持时返回 -EOPNOTSUPP。失败不为该 CPU 发布
 * 可用操作表，后续枚举/启动路径据此跳过 CPU。
 */
int __init init_cpu_ops(int cpu);

/*
 * get_cpu_ops() - 读取逻辑 CPU 已绑定的只读操作表。
 *
 * @cpu：逻辑 CPU 号，调用者保证范围有效。返回借用指针或 NULL，不增加引用；
 * boot CPU 可能因固件未描述 enable method 而没有 ops。初始化完成后数组
 * __ro_after_init，函数可在 SMP/hotplug 路径无锁调用。指针有效到系统结束。
 */
extern const struct cpu_operations *get_cpu_ops(int cpu);

/*
 * init_bootcpu_ops() - 尝试为已经运行的 boot CPU 绑定协议表。
 *
 * 入参：无。setup_arch() 调用，固定处理逻辑 CPU0。boot CPU 不依赖该表完成
 * 当前启动，因此有意忽略 init_cpu_ops(0) 的返回值；若固件未给 boot CPU
 * enable method，后续需要 ops 的路径必须接受 NULL。返回类型为 void，
 * 唯一副作用是可能写入 cpu_ops[0]。
 */
static inline void __init init_bootcpu_ops(void)
{
	init_cpu_ops(0);
}

#endif /* ifndef __ASM_CPU_OPS_H */
