/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * PSCI/SMCCC 数值 ABI 学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本 UAPI 头文件只定义跨内核、KVM 与用户空间共享的常量：function ID、
 * CPU_SUSPEND power-state 位域、版本/feature 编码及固件返回码。它不声明
 * Linux 内核操作表，也不发起 SMC/HVC。
 *
 * function ID 的高位编码调用约定和 32/64 位宽，低位选择具体 PSCI 功能；
 * power-state 必须按固件通过 PSCI_FEATURES 报告的 original/extended 格式
 * 解释。所有值属于固件 ABI，不能随 Linux 内部实现任意重编号。
 */
/*
 * ARM Power State and Coordination Interface (PSCI) header
 *
 * This header holds common PSCI defines and macros shared
 * by: ARM kernel, ARM64 kernel, KVM ARM/ARM64 and user space.
 *
 * Copyright (C) 2014 Linaro Ltd.
 * Author: Anup Patel <anup.patel@linaro.org>
 */
/*
 * 原注释说明该头文件由 ARM/ARM64 内核、KVM 和用户空间共同使用，因此新增
 * 版本时必须保持数值兼容，而不能只考虑当前宿主内核。
 */

#ifndef _UAPI_LINUX_PSCI_H
#define _UAPI_LINUX_PSCI_H

/*
 * PSCI v0.1 interface
 *
 * The PSCI v0.1 function numbers are implementation defined.
 *
 * Only PSCI return values such as: SUCCESS, NOT_SUPPORTED,
 * INVALID_PARAMS, and DENIED defined below are applicable
 * to PSCI v0.1.
 */
/*
 * PSCI 0.1 的 function number 由平台 DT 提供，没有统一编号；只有 SUCCESS、
 * NOT_SUPPORTED、INVALID_PARAMS、DENIED 这组基础返回语义可跨实现依赖。
 */

/* PSCI v0.2 interface */
/*
 * 0.2 标准 function ID 构造：
 * - 0x84 前缀表示 SMC32 fast call、标准服务 owner；
 * - bit30 置位形成对应 SMC64 ID；
 * - n 是 PSCI 规范分配的功能序号。
 */
#define PSCI_0_2_FN_BASE			0x84000000
#define PSCI_0_2_FN(n)				(PSCI_0_2_FN_BASE + (n))
#define PSCI_0_2_64BIT				0x40000000
#define PSCI_0_2_FN64_BASE			\
					(PSCI_0_2_FN_BASE + PSCI_0_2_64BIT)
#define PSCI_0_2_FN64(n)			(PSCI_0_2_FN64_BASE + (n))

/*
 * PSCI 0.2 的 32 位标准功能序号：
 * VERSION 查询版本；CPU_SUSPEND/OFF/ON 管理 CPU；AFFINITY_INFO 查询电源
 * 状态；MIGRATE_* 描述/迁移 Trusted OS；SYSTEM_OFF/RESET 控制整机。
 */
#define PSCI_0_2_FN_PSCI_VERSION		PSCI_0_2_FN(0)
#define PSCI_0_2_FN_CPU_SUSPEND			PSCI_0_2_FN(1)
#define PSCI_0_2_FN_CPU_OFF			PSCI_0_2_FN(2)
#define PSCI_0_2_FN_CPU_ON			PSCI_0_2_FN(3)
#define PSCI_0_2_FN_AFFINITY_INFO		PSCI_0_2_FN(4)
#define PSCI_0_2_FN_MIGRATE			PSCI_0_2_FN(5)
#define PSCI_0_2_FN_MIGRATE_INFO_TYPE		PSCI_0_2_FN(6)
#define PSCI_0_2_FN_MIGRATE_INFO_UP_CPU		PSCI_0_2_FN(7)
#define PSCI_0_2_FN_SYSTEM_OFF			PSCI_0_2_FN(8)
#define PSCI_0_2_FN_SYSTEM_RESET		PSCI_0_2_FN(9)

/*
 * 需要承载 native-width affinity 或物理地址的 64 位变体。CPU_OFF、
 * VERSION、SYSTEM_OFF/RESET 等参数无需 64 位地址，因此没有对应 FN64。
 */
#define PSCI_0_2_FN64_CPU_SUSPEND		PSCI_0_2_FN64(1)
#define PSCI_0_2_FN64_CPU_ON			PSCI_0_2_FN64(3)
#define PSCI_0_2_FN64_AFFINITY_INFO		PSCI_0_2_FN64(4)
#define PSCI_0_2_FN64_MIGRATE			PSCI_0_2_FN64(5)
#define PSCI_0_2_FN64_MIGRATE_INFO_UP_CPU	PSCI_0_2_FN64(7)

/*
 * PSCI 1.0 在 0.2 序号空间继续追加：FEATURES 做能力发现，CPU_FREEZE/
 * DEFAULT_SUSPEND/NODE_HW_STATE 管理电源，SYSTEM_SUSPEND 管整机休眠，
 * SET_SUSPEND_MODE 切换 PC/OSI，STAT_* 提供驻留统计。
 */
#define PSCI_1_0_FN_PSCI_FEATURES		PSCI_0_2_FN(10)
#define PSCI_1_0_FN_CPU_FREEZE			PSCI_0_2_FN(11)
#define PSCI_1_0_FN_CPU_DEFAULT_SUSPEND		PSCI_0_2_FN(12)
#define PSCI_1_0_FN_NODE_HW_STATE		PSCI_0_2_FN(13)
#define PSCI_1_0_FN_SYSTEM_SUSPEND		PSCI_0_2_FN(14)
#define PSCI_1_0_FN_SET_SUSPEND_MODE		PSCI_0_2_FN(15)
#define PSCI_1_0_FN_STAT_RESIDENCY		PSCI_0_2_FN(16)
#define PSCI_1_0_FN_STAT_COUNT			PSCI_0_2_FN(17)

/* 1.1/1.3 新增 warm/vendor reset、内存保护与 hibernate system-off。 */
#define PSCI_1_1_FN_SYSTEM_RESET2		PSCI_0_2_FN(18)
#define PSCI_1_1_FN_MEM_PROTECT			PSCI_0_2_FN(19)
#define PSCI_1_1_FN_MEM_PROTECT_CHECK_RANGE	PSCI_0_2_FN(20)
#define PSCI_1_3_FN_SYSTEM_OFF2			PSCI_0_2_FN(21)

/* 上述带地址/计数参数功能的 64 位调用约定 ID。 */
#define PSCI_1_0_FN64_CPU_DEFAULT_SUSPEND	PSCI_0_2_FN64(12)
#define PSCI_1_0_FN64_NODE_HW_STATE		PSCI_0_2_FN64(13)
#define PSCI_1_0_FN64_SYSTEM_SUSPEND		PSCI_0_2_FN64(14)
#define PSCI_1_0_FN64_STAT_RESIDENCY		PSCI_0_2_FN64(16)
#define PSCI_1_0_FN64_STAT_COUNT		PSCI_0_2_FN64(17)

#define PSCI_1_1_FN64_SYSTEM_RESET2		PSCI_0_2_FN64(18)
#define PSCI_1_1_FN64_MEM_PROTECT_CHECK_RANGE	PSCI_0_2_FN64(20)
#define PSCI_1_3_FN64_SYSTEM_OFF2		PSCI_0_2_FN64(21)

/* PSCI v0.2 power state encoding for CPU_SUSPEND function */
/*
 * original power-state：
 * bits[15:0] StateID；bit16 Type（0 standby，1 power-down）；
 * bits[25:24] 最低 affinity level。其余位必须为 0。
 */
#define PSCI_0_2_POWER_STATE_ID_MASK		0xffff
#define PSCI_0_2_POWER_STATE_ID_SHIFT		0
#define PSCI_0_2_POWER_STATE_TYPE_SHIFT		16
#define PSCI_0_2_POWER_STATE_TYPE_MASK		\
				(0x1 << PSCI_0_2_POWER_STATE_TYPE_SHIFT)
#define PSCI_0_2_POWER_STATE_AFFL_SHIFT		24
#define PSCI_0_2_POWER_STATE_AFFL_MASK		\
				(0x3 << PSCI_0_2_POWER_STATE_AFFL_SHIFT)

/* PSCI extended power state encoding for CPU_SUSPEND function */
/*
 * extended 格式把 StateID 扩为 bits[27:0]，Type 移到 bit30，不再携带 original
 * affinity-level 字段。是否采用该格式由 PSCI_FEATURES(CPU_SUSPEND) bit1 决定。
 */
#define PSCI_1_0_EXT_POWER_STATE_ID_MASK	0xfffffff
#define PSCI_1_0_EXT_POWER_STATE_ID_SHIFT	0
#define PSCI_1_0_EXT_POWER_STATE_TYPE_SHIFT	30
#define PSCI_1_0_EXT_POWER_STATE_TYPE_MASK	\
				(0x1 << PSCI_1_0_EXT_POWER_STATE_TYPE_SHIFT)

/* PSCI v0.2 affinity level state returned by AFFINITY_INFO */
/* 查询结果：目标已开、已关或上电请求仍在处理中；这些不是 Linux errno。 */
#define PSCI_0_2_AFFINITY_LEVEL_ON		0
#define PSCI_0_2_AFFINITY_LEVEL_OFF		1
#define PSCI_0_2_AFFINITY_LEVEL_ON_PENDING	2

/* PSCI v0.2 multicore support in Trusted OS returned by MIGRATE_INFO_TYPE */
/*
 * TOS 类型：UP_MIGRATE 为单处理器且可迁移；UP_NO_MIGRATE 为单处理器不可
 * 迁移；MP 表示多处理器安全 OS，无单一驻留 CPU 约束。
 */
#define PSCI_0_2_TOS_UP_MIGRATE			0
#define PSCI_0_2_TOS_UP_NO_MIGRATE		1
#define PSCI_0_2_TOS_MP				2

/* PSCI v1.1 reset type encoding for SYSTEM_RESET2 */
/* 0 是架构 warm reset；bit31 起是厂商自定义 reset 类型命名空间。 */
#define PSCI_1_1_RESET_TYPE_SYSTEM_WARM_RESET	0
#define PSCI_1_1_RESET_TYPE_VENDOR_START	0x80000000U

/* PSCI v1.3 hibernate type for SYSTEM_OFF2 */
/* feature bit0 表示 SYSTEM_OFF2 支持 HIBERNATE_OFF。 */
#define PSCI_1_3_OFF_TYPE_HIBERNATE_OFF		BIT(0)

/* PSCI version decoding (independent of PSCI version) */
/*
 * 版本统一编码为 high16 major + low16 minor。MAJOR/MINOR 宏只提取字段；
 * PSCI_VERSION 对输入裁剪后组合，亦被 SMCCC 版本复用。
 */
#define PSCI_VERSION_MAJOR_SHIFT		16
#define PSCI_VERSION_MINOR_MASK			\
		((1U << PSCI_VERSION_MAJOR_SHIFT) - 1)
#define PSCI_VERSION_MAJOR_MASK			~PSCI_VERSION_MINOR_MASK
#define PSCI_VERSION_MAJOR(ver)			\
		(((ver) & PSCI_VERSION_MAJOR_MASK) >> PSCI_VERSION_MAJOR_SHIFT)
#define PSCI_VERSION_MINOR(ver)			\
		((ver) & PSCI_VERSION_MINOR_MASK)
#define PSCI_VERSION(maj, min)						\
	((((maj) << PSCI_VERSION_MAJOR_SHIFT) & PSCI_VERSION_MAJOR_MASK) | \
	 ((min) & PSCI_VERSION_MINOR_MASK))

/* PSCI features decoding (>=1.0) */
/*
 * CPU_SUSPEND feature：bit1 选择 extended power-state format；bit0 表示
 * SET_SUSPEND_MODE 支持 OS-initiated 模式。PC=0、OSI=1 是提交给固件的枚举。
 */
#define PSCI_1_0_FEATURES_CPU_SUSPEND_PF_SHIFT	1
#define PSCI_1_0_FEATURES_CPU_SUSPEND_PF_MASK	\
			(0x1 << PSCI_1_0_FEATURES_CPU_SUSPEND_PF_SHIFT)

#define PSCI_1_0_OS_INITIATED			BIT(0)
#define PSCI_1_0_SUSPEND_MODE_PC		0
#define PSCI_1_0_SUSPEND_MODE_OSI		1

/* PSCI return values (inclusive of all PSCI versions) */
/*
 * 固件返回码均经 a0 返回：0 成功，负值依次表示功能缺失、参数/权限、异步
 * CPU 状态、内部故障、目标不存在/禁用或恢复地址非法。调用者不能把
 * ALREADY_ON/ON_PENDING 与通用 -EINVAL 混淆，除非上层明确选择折叠语义。
 */
#define PSCI_RET_SUCCESS			0
#define PSCI_RET_NOT_SUPPORTED			-1
#define PSCI_RET_INVALID_PARAMS			-2
#define PSCI_RET_DENIED				-3
#define PSCI_RET_ALREADY_ON			-4
#define PSCI_RET_ON_PENDING			-5
#define PSCI_RET_INTERNAL_FAILURE		-6
#define PSCI_RET_NOT_PRESENT			-7
#define PSCI_RET_DISABLED			-8
#define PSCI_RET_INVALID_ADDRESS		-9

#endif /* _UAPI_LINUX_PSCI_H */
