/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Linux 内核 PSCI 操作契约学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本头文件位于固件实现与 ARM/ARM64 SMP、cpuidle、KVM 消费者之间：声明
 * psci_ops 操作表、power-state 辅助接口以及 DT/ACPI 初始化入口。结构体只
 * 保存静态函数指针，不拥有固件或 CPU 对象；drivers/firmware/psci/psci.c
 * 在启动期填充，之后运行期消费者无锁只读。
 *
 * CONFIG stub 让调用代码不必散布条件编译：关闭 PSCI/ACPI 时返回“无操作/
 * 不存在”，不会发起 SMC/HVC。这里是内核私有契约；稳定的数值 ABI 定义在
 * include/uapi/linux/psci.h。
 */
/*
 *
 * Copyright (C) 2015 ARM Limited
 */

#ifndef __LINUX_PSCI_H
#define __LINUX_PSCI_H

#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/types.h>

/*
 * CPU_SUSPEND power-state 的 Type 语义值：STANDBY 保留 CPU 上下文并从固件
 * 调用处返回；POWER_DOWN 丢失上下文，调用者必须提供物理恢复入口。
 */
#define PSCI_POWER_STATE_TYPE_STANDBY		0
#define PSCI_POWER_STATE_TYPE_POWER_DOWN	1

/* 查询逻辑 CPU 是否承载不可自动迁移的 Trusted OS；纯查询。 */
bool psci_tos_resident_on(int cpu);

/*
 * 公共运行期接口组：
 * - psci_cpu_suspend_enter：按 state Type 选择 retention/cpu_suspend 路径。
 * - psci_power_state_is_valid：只校验当前编码格式的合法位。
 * - psci_set_osi_mode：切换 PC/OSI 固件协调模式，返回 Linux errno。
 * - psci_has_osi_support：读取启动期 CPU_SUSPEND feature 快照。
 */
int psci_cpu_suspend_enter(u32 state);
bool psci_power_state_is_valid(u32 state);
int psci_set_osi_mode(bool enable);
bool psci_has_osi_support(void);

/*
 * 内核 PSCI 操作表。
 *
 * 各回调在初始化成功后保持静态：
 * - get_version：返回 major:minor 编码。
 * - cpu_suspend：state + 恢复物理入口；power-down 成功不沿原调用点返回。
 * - cpu_off：关闭当前 CPU，成功不返回。
 * - cpu_on：目标物理 affinity + 启动物理入口，成功仅表示异步请求接受。
 * - migrate：请求单处理器 Trusted OS 迁移到物理 CPU。
 * - affinity_info：返回 ON/OFF/ON_PENDING 或原始负 PSCI 状态。
 * - migrate_info_type：返回 TOS UP/MP 类型或原始负状态。
 *
 * NULL 字段就是“不支持”，消费者必须在调用前检查。函数指针没有引用计数，
 * 因为提供者编入内核且不卸载。
 */
struct psci_operations {
	u32 (*get_version)(void);
	int (*cpu_suspend)(u32 state, unsigned long entry_point);
	int (*cpu_off)(u32 state);
	int (*cpu_on)(unsigned long cpuid, unsigned long entry_point);
	int (*migrate)(unsigned long cpuid);
	int (*affinity_info)(unsigned long target_affinity,
			unsigned long lowest_affinity_level);
	int (*migrate_info_type)(void);
};

/* 启动期发布、运行期只读的全局 PSCI 操作表。 */
extern struct psci_operations psci_ops;

/*
 * PSCI 0.1 的实现自定义 function ID 快照。四字段来自 DT 同名属性；缺失属性
 * 保持 0 且 psci_ops 对应回调为 NULL。0.2+ 使用标准 ID，不依赖此结构。
 */
struct psci_0_1_function_ids {
	u32 cpu_suspend;
	u32 cpu_on;
	u32 cpu_off;
	u32 migrate;
};

/* 按值返回 0.1 ID 快照，不暴露内部可写存储。 */
struct psci_0_1_function_ids get_psci_0_1_function_ids(void);

#if defined(CONFIG_ARM_PSCI_FW)
/* 从 DT PSCI compatible/method 初始化 conduit 和操作表；返回 0/负 errno。 */
int __init psci_dt_init(void);
#else
/* 未编入 PSCI 固件支持时，调用点保持可编译且视为无需初始化。 */
static inline int psci_dt_init(void) { return 0; }
#endif

#if defined(CONFIG_ARM_PSCI_FW) && defined(CONFIG_ACPI)
/*
 * ACPI 路径接口：psci_acpi_init 发布 PSCI 0.2+；present/use_hvc 分别读取
 * ACPI 平台标志。初始化查询带 __init，conduit 查询可供后续代码读取。
 */
int __init psci_acpi_init(void);
bool __init acpi_psci_present(void);
bool acpi_psci_use_hvc(void);
#else
/* 缺少 PSCI 或 ACPI 时明确返回“不存在/不用 HVC”，不触碰固件状态。 */
static inline int psci_acpi_init(void) { return 0; }
static inline bool acpi_psci_present(void) { return false; }
static inline bool acpi_psci_use_hvc(void) {return false; }
#endif

#endif /* __LINUX_PSCI_H */
