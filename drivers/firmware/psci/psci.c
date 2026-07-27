// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM PSCI 固件接口发现、分派与电源管理学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 【文件职责】
 * PSCI（Power State Coordination Interface）把 CPU 启停、CPU/系统 suspend、
 * Trusted OS 迁移、整机复位和关机等操作定义为标准固件调用。本文件负责：
 *
 *   DT/ACPI 描述
 *      -> 选择 SMC（进入安全监控器）或 HVC（进入 hypervisor）conduit
 *      -> 探测 PSCI 版本与 PSCI_FEATURES
 *      -> 发布 psci_ops 操作表和可选能力
 *      -> 注册 restart、poweroff、hibernate、suspend、debugfs 接口
 *
 * 运行期调用者（SMP/CPU hotplug、cpuidle、system suspend、reboot）只依赖
 * psci_ops，不需要知道底层是 SMC 还是 HVC，也不需要区分 PSCI 0.1 的 DT
 * 私有 function ID 与 0.2+ 的标准 ID。
 *
 * 本文件不实现固件中的电源状态机，也不保存/恢复具体 CPU 寄存器；上下文
 * 丢失路径由体系结构 cpu_suspend()/cpu_resume 完成，固件负责真正改变电源
 * 域。SMCCC helper 只建立同步调用边界，不意味着普通内核锁或内存所有权转移。
 *
 * 【核心对象与生命周期】
 * - invoke_psci_fn：SMC/HVC 的统一四参数调用入口，初始化成功后只读。
 * - psci_ops：对内核其他子系统发布的稳定操作表；0.1 按 DT 属性逐项填充，
 *   0.2+ 一次性安装标准包装函数。
 * - psci_cpu_suspend_feature 与 *_supported：PSCI_FEATURES 的启动期快照，
 *   供 idle/suspend/reboot 路径无锁读取。
 * - resident_cpu：单处理器 Trusted OS 的驻留逻辑 CPU；CPU hotplug 用它阻止
 *   会被固件 DENIED、且在 CPU_OFF 路径中可能成为致命结果的下线请求。
 *
 * 初始化在启动 CPU 上串行完成，早于上述运行期消费者注册/启用，因此这些
 * 全局对象不需要运行期写锁。SMC/HVC 本身可能跨异常级或进入 hypervisor；
 * CPU_OFF、SYSTEM_OFF、SYSTEM_RESET、成功的 context-losing suspend 通常
 * 不从原执行点返回，返回路径主要承载固件拒绝或恢复后的状态。
 *
 * 【方案权衡】
 * 统一操作表隔离了固件版本、位宽和 conduit 差异，调用路径短且无需动态
 * 分配；代价是固件错误常只能在同步返回值中发现，部分不可返回操作无法由
 * 内核回滚。PSCI 0.1 缺少版本/能力发现，只能信任 DT function ID；0.2+
 * 能主动探测并按版本启用功能，但仍必须兼容不完全符合规范的 hypervisor。
 */
/*
 *
 * Copyright (C) 2015 ARM Limited
 */

/* 为本文件所有 pr_*() 日志加 "psci: " 前缀，不改变日志级别。 */
#define pr_fmt(fmt) "psci: " fmt

#include <linux/acpi.h>
#include <linux/arm-smccc.h>
#include <linux/cpuidle.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/linkage.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/printk.h>
#include <linux/psci.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/suspend.h>

#include <uapi/linux/psci.h>

#include <asm/cpuidle.h>
#include <asm/cputype.h>
#include <asm/hypervisor.h>
#include <asm/system_misc.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>

/*
 * While a 64-bit OS can make calls with SMC32 calling conventions, for some
 * calls it is necessary to use SMC64 to pass or return 64-bit values.
 * For such calls PSCI_FN_NATIVE(version, name) will choose the appropriate
 * (native-width) function ID.
 */
/*
 * 64 位内核仍可发起 SMC32/HVC32，但 CPU_ON、CPU_SUSPEND 等带物理地址或
 * affinity 参数的调用必须用 native-width ID，才能完整传递/返回 64 位值。
 * 该宏只在编译期选择 function ID，不决定 conduit；SMC/HVC 由后文函数指针
 * 分派。32 位内核选择普通 ID，64 位内核选择 FN64 ID。
 */
#ifdef CONFIG_64BIT
#define PSCI_FN_NATIVE(version, name)	PSCI_##version##_FN64_##name
#else
#define PSCI_FN_NATIVE(version, name)	PSCI_##version##_FN_##name
#endif

/*
 * The CPU any Trusted OS is resident on. The trusted OS may reject CPU_OFF
 * calls to its resident CPU, so we must avoid issuing those. We never migrate
 * a Trusted OS even if it claims to be capable of migration -- doing so will
 * require cooperation with a Trusted OS driver.
 */
/*
 * 单处理器 Trusted OS 的驻留 CPU。
 *
 * -1 表示未发现驻留限制；psci_init_migrate() 根据 MIGRATE_INFO_* 的 MPIDR
 * 结果转换并写入逻辑 CPU 号。之后 CPU hotplug 与 checker 只读。内核即使
 * 得知 TOS 可以迁移也不主动调用 MIGRATE，因为安全 OS 状态迁移还需要对应
 * 驱动协作；直接下线驻留 CPU 可能让 CPU_OFF 返回 DENIED。
 */
static int resident_cpu = -1;

/*
 * 向体系结构 SMP、cpuidle 和 KVM 等消费者发布的 PSCI 操作表。
 *
 * 静态零初始化表示“该操作不受支持”；初始化路径在启动 CPU 上按固件版本
 * 填充，之后不再修改。函数指针不持有模块/对象引用，本驱动和固件接口在
 * 内核运行期始终存在。
 */
struct psci_operations psci_ops;

/* 当前固件调用通道；NONE 表示尚未从 DT/ACPI 完成选择。初始化后只读。 */
static enum arm_smccc_conduit psci_conduit = SMCCC_CONDUIT_NONE;

/*
 * 查询某逻辑 CPU 是否承载不可由本文件迁移的 Trusted OS。
 *
 * @cpu：Linux 逻辑 CPU 号，不是 MPIDR；可为任意 int。
 * 返回 true 仅当它等于 resident_cpu。无副作用、不睡眠；初始化完成后的
 * 运行期只读不需要锁。
 */
bool psci_tos_resident_on(int cpu)
{
	return cpu == resident_cpu;
}

/*
 * 底层 PSCI conduit 的统一函数类型：a0 是 function ID，a1-a3 是前三个
 * PSCI 参数，返回值取 SMCCC a0。unsigned long 与 native SMCCC 寄存器宽度
 * 一致；PSCI 负状态码以二进制补码返回，具体 wrapper 再解释。
 */
typedef unsigned long (psci_fn)(unsigned long, unsigned long,
				unsigned long, unsigned long);

/*
 * 当前已选择的 SMC/HVC 调用函数。get_set_conduit_method()/ACPI init 在首次
 * PSCI 调用前发布；其后所有路径只读。NULL 表示 PSCI 尚未初始化，debugfs
 * 等晚期入口会据此跳过。
 */
static psci_fn *invoke_psci_fn;

/*
 * PSCI 0.1 没有标准 function ID，DT 解析出的四个 ID 保存在这里。未出现的
 * 属性保持 0，且对应 psci_ops 回调也保持 NULL，消费者据此判断不支持。
 */
static struct psci_0_1_function_ids psci_0_1_function_ids;

/*
 * 返回 PSCI 0.1 function-ID 快照。
 *
 * 无入参；按值复制结构体，不暴露内部可写地址，也不转移所有权。主要供 KVM
 * 等代码复用宿主 DT 协议。初始化完成后调用无需锁。
 */
struct psci_0_1_function_ids get_psci_0_1_function_ids(void)
{
	return psci_0_1_function_ids;
}

/*
 * 原始与 extended power-state 的合法位集合。
 *
 * 0.2 原格式包含 StateID、Type 和 affinity level；1.0 extended 格式扩大
 * StateID、把 Type 移到 bit 30，并移除原 affinity-level 字段。掩码用于
 * 拒绝 DT/ACPI 中携带保留位的状态值，不能混用。
 */
#define PSCI_0_2_POWER_STATE_MASK		\
				(PSCI_0_2_POWER_STATE_ID_MASK | \
				PSCI_0_2_POWER_STATE_TYPE_MASK | \
				PSCI_0_2_POWER_STATE_AFFL_MASK)

#define PSCI_1_0_EXT_POWER_STATE_MASK		\
				(PSCI_1_0_EXT_POWER_STATE_ID_MASK | \
				PSCI_1_0_EXT_POWER_STATE_TYPE_MASK)

/*
 * PSCI 1.0+ 启动期能力快照：
 * - psci_cpu_suspend_feature：PSCI_FEATURES(CPU_SUSPEND) 的非负位图，0 也
 *   表示函数存在但不支持 OSI/extended encoding。
 * - psci_system_reset2_supported：允许 warm/soft reboot 使用 SYSTEM_RESET2。
 * - psci_system_off2_hibernate_supported：SYSTEM_OFF2 声明 HIBERNATE_OFF。
 *
 * 固件不支持时保持静态零值；初始化后运行期只读。
 */
static u32 psci_cpu_suspend_feature;
static bool psci_system_reset2_supported;
static bool psci_system_off2_hibernate_supported;

/* 返回 CPU_SUSPEND feature bit 1，即固件是否采用 extended StateID 编码。 */
static inline bool psci_has_ext_power_state(void)
{
	return psci_cpu_suspend_feature &
				PSCI_1_0_FEATURES_CPU_SUSPEND_PF_MASK;
}

/*
 * 查询固件是否支持 OS-initiated suspend mode。
 *
 * 无入参；返回 CPU_SUSPEND feature bit 0。该能力只表示可切换 OSI，实际模式
 * 由 psci_set_osi_mode() 选择；初始化前/不支持时返回 false。
 */
bool psci_has_osi_support(void)
{
	return psci_cpu_suspend_feature & PSCI_1_0_OS_INITIATED;
}

/*
 * 判断 power-state 的 Type 位是否要求丢失 CPU 上下文。
 *
 * @state：按当前固件报告格式编码的 PSCI CPU_SUSPEND 参数。
 * 返回 true 表示 power-down/context-losing，必须走 cpu_suspend 保存上下文；
 * false 表示 standby/retention，可直接调用固件后从原位置继续。只读无副作用。
 */
static inline bool psci_power_state_loses_context(u32 state)
{
	/* mask 根据 feature 快照选择 bit 16（原格式）或 bit 30（extended）。 */
	const u32 mask = psci_has_ext_power_state() ?
					PSCI_1_0_EXT_POWER_STATE_TYPE_MASK :
					PSCI_0_2_POWER_STATE_TYPE_MASK;

	return state & mask;
}

/*
 * 校验一个 PSCI power-state 是否只使用当前格式允许的位。
 *
 * @state：通常来自 DT arm,psci-suspend-param 或 ACPI LPI，纯输入。
 * 返回 true 只说明保留位均为 0；StateID 是否由具体固件实现、状态层级是否
 * 可达，仍要在 CPU_SUSPEND 时由固件判断。无副作用、不睡眠。
 */
bool psci_power_state_is_valid(u32 state)
{
	/* 取反 valid_mask 后与 state 相交，可一次发现任何未知/保留位。 */
	const u32 valid_mask = psci_has_ext_power_state() ?
			       PSCI_1_0_EXT_POWER_STATE_MASK :
			       PSCI_0_2_POWER_STATE_MASK;

	return !(state & ~valid_mask);
}

/*
 * 通过 HVC 发起一次 PSCI/SMCCC 同步调用。
 *
 * @function_id、@arg0..@arg2 对应 SMCCC a0..a3，均为纯输入 native 寄存器值。
 * a4..a7 固定清零；res 是栈上返回寄存器快照，只取 a0 并原样返回。调用可能
 * 进入 hypervisor、改变 CPU/系统电源状态，成功时甚至不返回；本函数不翻译
 * PSCI errno、不睡眠，也不提供跨调用的普通锁。
 */
static __always_inline unsigned long
__invoke_psci_fn_hvc(unsigned long function_id,
		     unsigned long arg0, unsigned long arg1,
		     unsigned long arg2)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(function_id, arg0, arg1, arg2, 0, 0, 0, 0, &res);
	return res.a0;
}

/*
 * 通过 SMC 发起一次 PSCI/SMCCC 同步调用。
 *
 * 参数、返回和副作用与 HVC wrapper 相同，区别是异常进入 secure monitor。
 * __always_inline 保持热路径只剩架构 SMCCC 序列，res 生命周期仅限本次调用。
 */
static __always_inline unsigned long
__invoke_psci_fn_smc(unsigned long function_id,
		     unsigned long arg0, unsigned long arg1,
		     unsigned long arg2)
{
	struct arm_smccc_res res;

	arm_smccc_smc(function_id, arg0, arg1, arg2, 0, 0, 0, 0, &res);
	return res.a0;
}

/*
 * 把常见 PSCI 负状态码转换为 Linux errno。
 *
 * @errno：固件 a0 的有符号 PSCI 返回值；SUCCESS 转 0，NOT_SUPPORTED 转
 * -EOPNOTSUPP，参数/地址错误转 -EINVAL，DENIED 转 -EPERM。其余状态（包括
 * ALREADY_ON、ON_PENDING、INTERNAL_FAILURE、NOT_PRESENT、DISABLED）当前
 * 统一折叠为 -EINVAL，因此调用者若需要原始正/负协议状态必须绕过本 helper。
 * 函数纯计算、无副作用。
 */
static __always_inline int psci_to_linux_errno(int errno)
{
	switch (errno) {
	case PSCI_RET_SUCCESS:
		return 0;
	case PSCI_RET_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case PSCI_RET_INVALID_PARAMS:
	case PSCI_RET_INVALID_ADDRESS:
		return -EINVAL;
	case PSCI_RET_DENIED:
		return -EPERM;
	}

	return -EINVAL;
}

/*
 * PSCI 0.1 的固定版本报告。
 *
 * 0.1 没有 PSCI_VERSION 调用，DT compatible 本身就是版本依据，因此无固件
 * 调用、无失败，始终返回编码后的 0.1。
 */
static u32 psci_0_1_get_version(void)
{
	return PSCI_VERSION(0, 1);
}

/*
 * 读取 PSCI 0.2+ 固件版本。
 *
 * 无参数；invoke_psci_fn 必须已由 conduit 初始化。返回固件 a0 的原始 u32
 * 版本编码，正常值由 major:minor 组成；PSCI_VERSION 是 0.2 起的必选调用。
 */
static u32 psci_0_2_get_version(void)
{
	return invoke_psci_fn(PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0);
}

/*
 * 在 platform-coordinated（PC）与 OS-initiated（OSI）suspend 模式间切换。
 *
 * @enable：true 请求 OSI，由 OS/CPU PM domains 选择层级状态；false 请求 PC，
 *          由固件协调依赖 CPU 的组合状态。
 * 返回转换后的 Linux errno；失败时保留固件原模式并记录 FW_BUG 日志。调用
 * SET_SUSPEND_MODE 会改变固件全局协调策略，必须在建立/拆除 CPU PM domain
 * 拓扑的控制路径串行调用，不能在 idle 热路径并发切换。
 */
int psci_set_osi_mode(bool enable)
{
	/* suspend_mode 是传给固件 a1 的规范枚举；err 保存原始 PSCI 状态码。 */
	unsigned long suspend_mode;
	int err;

	/* 阶段 1：把内核布尔策略映射为 PSCI 标准模式编号。 */
	suspend_mode = enable ? PSCI_1_0_SUSPEND_MODE_OSI :
			PSCI_1_0_SUSPEND_MODE_PC;

	/* 阶段 2：同步提交固件全局状态；其余参数按规范为 0。 */
	err = invoke_psci_fn(PSCI_1_0_FN_SET_SUSPEND_MODE, suspend_mode, 0, 0);
	if (err < 0)
		pr_info(FW_BUG "failed to set %s mode: %d\n",
				enable ? "OSI" : "PC", err);
	return psci_to_linux_errno(err);
}

/*
 * CPU_SUSPEND 的公共原始调用包装。
 *
 * @fn：0.1 DT function ID 或 0.2+ native-width 标准 ID。
 * @state：PSCI power-state 编码，决定 retention/power-down 与 StateID。
 * @entry_point：context-losing 状态恢复入口的物理地址；retention 路径可为 0。
 *
 * 成功 standby 会从固件调用返回；成功 power-down 通常从 entry_point 恢复，
 * 不沿本次 C 调用栈返回。固件拒绝时返回 Linux errno。函数不保存 CPU 上下文，
 * 调用者必须在 power-down 前经 cpu_suspend() 建立恢复现场。
 */
static __always_inline int
__psci_cpu_suspend(u32 fn, u32 state, unsigned long entry_point)
{
	/* err 是固件 a0 原始有符号状态；第三个 PSCI 参数未使用。 */
	int err;

	err = invoke_psci_fn(fn, state, entry_point, 0);
	return psci_to_linux_errno(err);
}

/* PSCI 0.1 CPU_SUSPEND：只把 DT 解析出的私有 ID 固定后复用公共契约。 */
static __always_inline int
psci_0_1_cpu_suspend(u32 state, unsigned long entry_point)
{
	return __psci_cpu_suspend(psci_0_1_function_ids.cpu_suspend,
				  state, entry_point);
}

/*
 * PSCI 0.2+ CPU_SUSPEND：按内核位宽选择能容纳恢复物理地址的标准 ID。
 * @state/@entry_point 与公共 helper 相同，返回 Linux errno。
 */
static __always_inline int
psci_0_2_cpu_suspend(u32 state, unsigned long entry_point)
{
	return __psci_cpu_suspend(PSCI_FN_NATIVE(0_2, CPU_SUSPEND),
				  state, entry_point);
}

/*
 * CPU_OFF 的公共包装。
 *
 * @fn：版本对应 function ID；@state 是实现可选使用的 power-state 建议。
 * 成功会关闭当前 CPU，不应返回；只有失败才回到调用者并转为 Linux errno。
 * 本函数必须在待关闭 CPU 自身执行，不迁移任务、不保存恢复上下文。
 */
static int __psci_cpu_off(u32 fn, u32 state)
{
	/* err 只有固件拒绝/异常返回时才对后续 C 控制流可见。 */
	int err;

	err = invoke_psci_fn(fn, state, 0, 0);
	return psci_to_linux_errno(err);
}

/* PSCI 0.1 CPU_OFF：使用 DT 私有 ID；参数/返回契约同 __psci_cpu_off()。 */
static int psci_0_1_cpu_off(u32 state)
{
	return __psci_cpu_off(psci_0_1_function_ids.cpu_off, state);
}

/* PSCI 0.2+ CPU_OFF：该调用没有 native 64 位变体，使用标准 SMC32 ID。 */
static int psci_0_2_cpu_off(u32 state)
{
	return __psci_cpu_off(PSCI_0_2_FN_CPU_OFF, state);
}

/*
 * CPU_ON 的公共包装。
 *
 * @fn：版本对应 function ID。
 * @cpuid：目标 CPU 的物理 affinity/MPIDR 值，不是 Linux 逻辑 CPU 号。
 * @entry_point：目标 CPU 上电后开始执行的物理地址。
 *
 * 调用只向固件提交异步上电请求；返回 SUCCESS 不保证目标已执行到内核入口，
 * 后续 SMP handshake 负责确认。失败转为 Linux errno，不分配或持有 CPU 对象。
 */
static int __psci_cpu_on(u32 fn, unsigned long cpuid, unsigned long entry_point)
{
	/* context_id 参数固定 0；Linux 的每 CPU 启动信息由自身启动协议提供。 */
	int err;

	err = invoke_psci_fn(fn, cpuid, entry_point, 0);
	return psci_to_linux_errno(err);
}

/* PSCI 0.1 CPU_ON：使用 DT 私有 function ID。 */
static int psci_0_1_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
	return __psci_cpu_on(psci_0_1_function_ids.cpu_on, cpuid, entry_point);
}

/* PSCI 0.2+ CPU_ON：native ID 保证 64 位 MPIDR/entry point 不被截断。 */
static int psci_0_2_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
	return __psci_cpu_on(PSCI_FN_NATIVE(0_2, CPU_ON), cpuid, entry_point);
}

/*
 * Trusted OS MIGRATE 的公共包装。
 *
 * @fn：版本对应 ID；@cpuid：目标物理 CPU affinity。返回 Linux errno。
 * 该调用要求 TOS/驱动共同管理状态，本文件正常不会主动使用 migrate 回调，
 * 但保留在 psci_ops 供兼容消费者使用。
 */
static int __psci_migrate(u32 fn, unsigned long cpuid)
{
	int err;

	err = invoke_psci_fn(fn, cpuid, 0, 0);
	return psci_to_linux_errno(err);
}

/* PSCI 0.1 MIGRATE：使用 DT 私有 function ID。 */
static int psci_0_1_migrate(unsigned long cpuid)
{
	return __psci_migrate(psci_0_1_function_ids.migrate, cpuid);
}

/* PSCI 0.2+ MIGRATE：native ID 可承载完整物理 affinity。 */
static int psci_0_2_migrate(unsigned long cpuid)
{
	return __psci_migrate(PSCI_FN_NATIVE(0_2, MIGRATE), cpuid);
}

/*
 * 查询某 affinity 层级的电源状态。
 *
 * @target_affinity：目标 CPU/cluster 的 MPIDR affinity 值。
 * @lowest_affinity_level：要观察的最低层级。
 * 返回原始 PSCI 值：ON/OFF/ON_PENDING 是非负协议状态，错误是负 PSCI 状态。
 * 不能经过 psci_to_linux_errno()，否则会丢失调用者轮询 CPU_OFF 所需的状态。
 */
static int psci_affinity_info(unsigned long target_affinity,
		unsigned long lowest_affinity_level)
{
	return invoke_psci_fn(PSCI_FN_NATIVE(0_2, AFFINITY_INFO),
			      target_affinity, lowest_affinity_level, 0);
}

/*
 * 查询 Trusted OS 是 MP、可迁移 UP、不可迁移 UP，还是不支持该接口。
 * 无参数，返回原始 PSCI 类型/负状态，供初始化和 debugfs 精确分类。
 */
static int psci_migrate_info_type(void)
{
	return invoke_psci_fn(PSCI_0_2_FN_MIGRATE_INFO_TYPE, 0, 0, 0);
}

/*
 * 查询单处理器 Trusted OS 当前驻留的物理 CPU。
 *
 * 仅在 MIGRATE_INFO_TYPE 返回 UP 类型后调用；返回 native-width MPIDR/affinity
 * 原值，不转成 Linux errno，由调用者校验 MPIDR_HWID_BITMASK。
 */
static unsigned long psci_migrate_info_up_cpu(void)
{
	return invoke_psci_fn(PSCI_FN_NATIVE(0_2, MIGRATE_INFO_UP_CPU),
			      0, 0, 0);
}

/*
 * 发布 PSCI 的 SMCCC conduit。
 *
 * @conduit：必须是 HVC 或 SMC。函数把 invoke_psci_fn 指向对应 always-inline
 * wrapper，并保存 psci_conduit 供 arm_smccc_version_init()/KVM 使用。
 *
 * 非法值 WARN；代码仍记录该枚举，但不会安装有效函数指针，因此所有调用者
 * 必须只从已验证的 DT/ACPI 分支进入。启动期一次写入、运行期只读，无锁。
 */
static void set_conduit(enum arm_smccc_conduit conduit)
{
	switch (conduit) {
	case SMCCC_CONDUIT_HVC:
		invoke_psci_fn = __invoke_psci_fn_hvc;
		break;
	case SMCCC_CONDUIT_SMC:
		invoke_psci_fn = __invoke_psci_fn_smc;
		break;
	default:
		WARN(1, "Unexpected PSCI conduit %d\n", conduit);
	}

	psci_conduit = conduit;
}

/*
 * 从 PSCI DT 节点解析并发布 "method" conduit。
 *
 * @np：匹配到的 PSCI device_node 借用引用，不能为空；本函数不 put、不保存。
 * 属性 "hvc" 选择 hypervisor call，"smc" 选择 secure monitor call。
 *
 * 返回 0 后 invoke_psci_fn 可安全使用；缺属性返回 -ENXIO，未知字符串返回
 * -EINVAL，失败前不改变已有 conduit。method 指针借用 FDT 属性存储，仅在
 * 比较期间使用。启动上下文可调用 OF helper，不涉及运行期并发。
 */
static int get_set_conduit_method(const struct device_node *np)
{
	/* method 是 OF 属性内部 NUL 结尾字符串的借用指针，不需释放。 */
	const char *method;

	pr_info("probing for conduit method from DT.\n");

	if (of_property_read_string(np, "method", &method)) {
		pr_warn("missing \"method\" property\n");
		return -ENXIO;
	}

	if (!strcmp("hvc", method)) {
		set_conduit(SMCCC_CONDUIT_HVC);
	} else if (!strcmp("smc", method)) {
		set_conduit(SMCCC_CONDUIT_SMC);
	} else {
		pr_warn("invalid \"method\" property: %s\n", method);
		return -EINVAL;
	}
	return 0;
}

/*
 * restart notifier：把内核重启请求提交给 PSCI 固件。
 *
 * @nb：psci_sys_reset_nb 的借用指针；@action/@data 是 reboot notifier 通用
 * 参数，本实现实际依据全局 reboot_mode 选择 reset 类型，不消费另外两项。
 *
 * warm/soft 且已探测 RESET2 时请求 architectural SYSTEM_WARM_RESET；其他
 * 情况使用 0.2 SYSTEM_RESET。成功通常不返回；若固件异常返回，NOTIFY_DONE
 * 允许 restart chain 继续尝试其他 handler。原子 notifier 上下文不可睡眠。
 */
static int psci_sys_reset(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	if ((reboot_mode == REBOOT_WARM || reboot_mode == REBOOT_SOFT) &&
	    psci_system_reset2_supported) {
		/*
		 * reset_type[31] = 0 (architectural)
		 * reset_type[30:0] = 0 (SYSTEM_WARM_RESET)
		 * cookie = 0 (ignored by the implementation)
		 */
		/*
		 * reset_type bit31=0 表示架构定义，低 31 位 0 是 warm reset；
		 * cookie 对该类型无意义，传 0 保持跨固件兼容。
		 */
		invoke_psci_fn(PSCI_FN_NATIVE(1_1, SYSTEM_RESET2), 0, 0, 0);
	} else {
		invoke_psci_fn(PSCI_0_2_FN_SYSTEM_RESET, 0, 0, 0);
	}

	return NOTIFY_DONE;
}

/*
 * PSCI restart handler 描述符。priority=129 让它按 reboot notifier 的降序
 * 优先级参与竞争；对象静态存续，无注册后的释放需求。
 */
static struct notifier_block psci_sys_reset_nb = {
	.notifier_call = psci_sys_reset,
	.priority = 129,
};

/*
 * legacy pm_power_off 回调：调用 PSCI SYSTEM_OFF。
 *
 * 无入参/返回；成功整机断电不返回，异常固件返回时调用者继续其兜底流程。
 * 回调可能处于关机晚期，不能分配或睡眠。
 */
static void psci_sys_poweroff(void)
{
	invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF, 0, 0, 0);
}

#ifdef CONFIG_HIBERNATION
/* 以下 SYSTEM_OFF2 集成只在内核支持 hibernation 时编译。 */

/*
 * sys-off handler：仅在真正进入 hibernation 时请求 PSCI SYSTEM_OFF2。
 *
 * @data：sys-off 框架上下文借用指针，本实现不读取。handler 注册在通用
 * POWER_OFF 模式，因此先用 system_entering_hibernation() 筛选；普通关机
 * 返回 NOTIFY_DONE，不抢占其他 firmware handler。
 *
 * 成功 HIBERNATE_OFF 不返回；固件异常返回后仍以 NOTIFY_DONE 允许链继续。
 */
static int psci_sys_hibernate(struct sys_off_data *data)
{
	/*
	 * If no hibernate type is specified SYSTEM_OFF2 defaults to selecting
	 * HIBERNATE_OFF.
	 *
	 * There are hypervisors in the wild that do not align with the spec and
	 * reject calls that explicitly provide a hibernate type. For
	 * compatibility with these nonstandard implementations, pass 0 as the
	 * type.
	 */
	/*
	 * 规范允许省略 hibernate type，此时默认 HIBERNATE_OFF。现实中有 hypervisor
	 * 会错误拒绝显式 type bit，故传 0 兼容两者；这不是请求普通 SYSTEM_OFF。
	 */
	if (system_entering_hibernation())
		invoke_psci_fn(PSCI_FN_NATIVE(1_3, SYSTEM_OFF2), 0, 0, 0);
	return NOTIFY_DONE;
}

/*
 * 在 subsys initcall 阶段按能力注册 hibernate power-off handler。
 *
 * 无参数；若 SYSTEM_OFF2 未报告 HIBERNATE_OFF，返回 0 且无副作用。支持时
 * 以比 EFI shutdown 更高的 firmware priority 注册静态回调；注册返回值当前
 * 未保存，函数始终返回 0。注册框架持有回调指针，不转移动态对象所有权。
 */
static int __init psci_hibernate_init(void)
{
	if (psci_system_off2_hibernate_supported) {
		/* Higher priority than EFI shutdown, but only for hibernate */
		/* 优先级仅决定 handler 顺序；回调自身仍用 hibernation 状态二次筛选。 */
		register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
					 SYS_OFF_PRIO_FIRMWARE + 2,
					 psci_sys_hibernate, NULL);
	}
	return 0;
}
/* 能力在早期 psci_probe() 建立，subsys_initcall 此时才消费并注册框架入口。 */
subsys_initcall(psci_hibernate_init);
#endif

/*
 * 查询某 PSCI/SMCCC function ID 的 feature。
 *
 * @psci_func_id：要探测的标准 function ID。返回固件原始值：NOT_SUPPORTED
 * 为负，其他错误保持原码，成功为该函数专属 feature 位图（可能为 0）。
 * 仅适用于 PSCI 1.0+；调用者必须先按版本守卫。
 */
static int psci_features(u32 psci_func_id)
{
	return invoke_psci_fn(PSCI_1_0_FN_PSCI_FEATURES,
			      psci_func_id, 0, 0);
}

#ifdef CONFIG_DEBUG_FS
/* 以下代码只提供只读诊断，不参与 PSCI 功能可用性或电源状态决策。 */

/*
 * 把 PSCI function ID 与可读名称组成静态表项。PSCI_ID 使用 32 位 ID，
 * PSCI_ID_NATIVE 按内核位宽选择 ID；宏本身不发起固件调用。
 */
#define PSCI_ID(ver, _name) \
	{ .fn = PSCI_##ver##_FN_##_name, .name = #_name, }
#define PSCI_ID_NATIVE(ver, _name) \
	{ .fn = PSCI_FN_NATIVE(ver, _name), .name = #_name, }

/* A table of all optional functions */
/*
 * 所有可选 PSCI 功能的 debugfs 枚举表。
 *
 * fn 是传给 PSCI_FEATURES 的 ID，name 指向只读字符串字面量；数组静态存续，
 * 无动态 ownership。必选基础函数不列入，因为这里关注可选能力差异。
 */
static const struct {
	/* 两字段共同描述一次 feature probe，不在运行期修改。 */
	u32 fn;
	const char *name;
} psci_fn_ids[] = {
	PSCI_ID_NATIVE(0_2, MIGRATE),
	PSCI_ID(0_2, MIGRATE_INFO_TYPE),
	PSCI_ID_NATIVE(0_2, MIGRATE_INFO_UP_CPU),
	PSCI_ID(1_0, CPU_FREEZE),
	PSCI_ID_NATIVE(1_0, CPU_DEFAULT_SUSPEND),
	PSCI_ID_NATIVE(1_0, NODE_HW_STATE),
	PSCI_ID_NATIVE(1_0, SYSTEM_SUSPEND),
	PSCI_ID(1_0, SET_SUSPEND_MODE),
	PSCI_ID_NATIVE(1_0, STAT_RESIDENCY),
	PSCI_ID_NATIVE(1_0, STAT_COUNT),
	PSCI_ID_NATIVE(1_1, SYSTEM_RESET2),
	PSCI_ID(1_1, MEM_PROTECT),
	PSCI_ID_NATIVE(1_1, MEM_PROTECT_CHECK_RANGE),
	PSCI_ID_NATIVE(1_3, SYSTEM_OFF2),
};

/*
 * 生成 /sys/kernel/debug/psci 的一次性只读快照。
 *
 * @s：seq_file 输出对象的借用指针，只追加文本。
 * @data：single_open 私有数据；本文件传 NULL，故不使用。
 * 返回 0；seq_file 自己记录输出错误，不改变 PSCI 状态。
 *
 * 先报告 PSCI 版本；0.x 因无 PSCI_FEATURES 直接结束。1.0+ 再探测 SMCCC、
 * CPU_SUSPEND 编码/OSI、Trusted OS 类型和可选 ID。固件查询是同步只读调用，
 * 可进入 monitor/hypervisor，但不切换电源状态。全局操作表在启动期已冻结，
 * 多个 debugfs reader 无需本文件加锁。
 */
static int psci_debugfs_read(struct seq_file *s, void *data)
{
	/*
	 * feature 保存 PSCI_FEATURES 原始结果，type 保存 TOS 类型，i 遍历静态表，
	 * ver 在不同阶段复用为 PSCI 或 SMCCC 的 major:minor 编码。
	 */
	int feature, type, i;
	u32 ver;

	/* 阶段 1：debugfs 创建前已确认 get_version 非 NULL。 */
	ver = psci_ops.get_version();
	seq_printf(s, "PSCIv%d.%d\n",
		   PSCI_VERSION_MAJOR(ver),
		   PSCI_VERSION_MINOR(ver));

	/* PSCI_FEATURES is available only starting from 1.0 */
	/* 0.1/0.2 不能用未定义的 PSCI_FEATURES 试探，版本输出后直接返回。 */
	if (PSCI_VERSION_MAJOR(ver) < 1)
		return 0;

	/*
	 * 阶段 2：先问 PSCI 是否接受 SMCCC_VERSION ID；不接受则依据向后兼容
	 * 规则报告 SMCCC 1.0，避免直接执行未知 function。
	 */
	feature = psci_features(ARM_SMCCC_VERSION_FUNC_ID);
	if (feature != PSCI_RET_NOT_SUPPORTED) {
		ver = invoke_psci_fn(ARM_SMCCC_VERSION_FUNC_ID, 0, 0, 0);
		seq_printf(s, "SMC Calling Convention v%d.%d\n",
			   PSCI_VERSION_MAJOR(ver),
			   PSCI_VERSION_MINOR(ver));
	} else {
		seq_puts(s, "SMC Calling Convention v1.0 is assumed\n");
	}

	/* CPU_SUSPEND feature bit0=OSI、bit1=extended StateID。 */
	feature = psci_features(PSCI_FN_NATIVE(0_2, CPU_SUSPEND));
	if (feature < 0) {
		seq_printf(s, "PSCI_FEATURES(CPU_SUSPEND) error (%d)\n", feature);
	} else {
		seq_printf(s, "OSI is %ssupported\n",
			   (feature & BIT(0)) ? "" : "not ");
		seq_printf(s, "%s StateID format is used\n",
			   (feature & BIT(1)) ? "Extended" : "Original");
	}

	/*
	 * 阶段 3：只有 TOS UP 类型才查询驻留 MPIDR。cpuid 是本次物理 affinity，
	 * resident_cpu 是初始化时解析出的逻辑号，二者一起输出用于核对拓扑。
	 */
	type = psci_ops.migrate_info_type();
	if (type == PSCI_0_2_TOS_UP_MIGRATE ||
	    type == PSCI_0_2_TOS_UP_NO_MIGRATE) {
		unsigned long cpuid;

		seq_printf(s, "Trusted OS %smigrate capable\n",
			   type == PSCI_0_2_TOS_UP_NO_MIGRATE ? "not " : "");
		cpuid = psci_migrate_info_up_cpu();
		seq_printf(s, "Trusted OS resident on physical CPU 0x%lx (#%d)\n",
			   cpuid, resident_cpu);
	} else if (type == PSCI_0_2_TOS_MP) {
		seq_puts(s, "Trusted OS migration not required\n");
	} else {
		if (type != PSCI_RET_NOT_SUPPORTED)
			seq_printf(s, "MIGRATE_INFO_TYPE returned unknown type (%d)\n", type);
	}

	/* 阶段 4：跳过明确不支持项，保留其他错误与成功能力信息。 */
	for (i = 0; i < ARRAY_SIZE(psci_fn_ids); i++) {
		feature = psci_features(psci_fn_ids[i].fn);
		if (feature == PSCI_RET_NOT_SUPPORTED)
			continue;
		if (feature < 0)
			seq_printf(s, "PSCI_FEATURES(%s) error (%d)\n",
				   psci_fn_ids[i].name, feature);
		else
			seq_printf(s, "%s is supported\n", psci_fn_ids[i].name);
	}

	return 0;
}

/*
 * debugfs open 包装。
 *
 * @inode：debugfs inode 借用指针，本实现不使用其私有数据。
 * @f：待绑定 seq_file 的打开文件。返回 single_open 的 0/负 errno；成功后
 * seq_file 框架持有状态，并由 single_release 销毁。
 */
static int psci_debugfs_open(struct inode *inode, struct file *f)
{
	return single_open(f, psci_debugfs_read, NULL);
}

/*
 * debugfs 文件操作表。owner 保护打开期间的实现代码；其余操作委托单实例
 * seq_file。对象静态存续，debugfs dentry 只借用指针。
 */
static const struct file_operations psci_debugfs_ops = {
	.owner = THIS_MODULE,
	.open = psci_debugfs_open,
	.release = single_release,
	.read = seq_read,
	.llseek = seq_lseek
};

/*
 * late initcall：在 PSCI 初始化成功后创建 0444 的 debugfs 文件。
 *
 * 无参数。conduit 或 get_version 未发布时静默返回 0；否则创建根目录下
 * "psci"，PTR_ERR_OR_ZERO 把 dentry/错误指针转成 initcall 结果。成功 dentry
 * 由 debugfs 树持有，本文件无需保存或释放。
 */
static int __init psci_debugfs_init(void)
{
	if (!invoke_psci_fn || !psci_ops.get_version)
		return 0;

	return PTR_ERR_OR_ZERO(debugfs_create_file("psci", 0444, NULL, NULL,
						   &psci_debugfs_ops));
}
/* 晚于固件探测执行，只发布诊断视图。 */
late_initcall(psci_debugfs_init)
#endif

#ifdef CONFIG_CPU_IDLE
/*
 * CPU idle 集成：状态值由 cpuidle-psci 驱动解析，本文件依据 Type 位选择
 * retention 或 context-losing 协议。
 */

/*
 * cpu_suspend() 的 PSCI finisher。
 *
 * @state：cpu_suspend 透传的 PSCI state，低 32 位有效。
 * 体系结构已在进入 finisher 前保存可恢复上下文；本函数把 cpu_resume 的
 * 物理地址交给固件。失败返回 Linux errno；成功 power-down 后从 resume
 * 入口恢复，不沿原固件调用点继续。
 *
 * noinstr 禁止 tracing/KASAN 等依赖正常执行环境的插桩，因为这里处于
 * suspend 尾段。函数不分配、不睡眠。
 */
static noinstr int psci_suspend_finisher(unsigned long state)
{
	/* power_state 固定 PSCI u32 ABI；pa_cpu_resume 是固件可见恢复入口。 */
	u32 power_state = state;
	phys_addr_t pa_cpu_resume;

	pa_cpu_resume = __pa_symbol_nodebug((unsigned long)cpu_resume);

	return psci_ops.cpu_suspend(power_state, pa_cpu_resume);
}

/*
 * 进入一个 PSCI CPU idle state。
 *
 * @state：已经过格式校验的 PSCI power-state 编码。
 * 返回固件/cpu_suspend 的 0 或 Linux errno。
 *
 * retention 不丢 CPU 上下文：进入 context-tracking idle 区间，保存 IRQ
 * 上下文后直接以 entry_point=0 调固件，返回后逆序恢复。power-down 必须
 * 经 cpu_suspend() 保存寄存器/页表恢复现场，再由 finisher 提交恢复 PA。
 *
 * ARM64 cpu_suspend() 自己管理 ct_cpuidle_enter/exit，其他架构由本函数
 * 包围，避免重复切换。该路径处于 cpuidle 原子环境，不能睡眠；所有局部
 * 快照只属于当前 CPU 栈。
 */
int psci_cpu_suspend_enter(u32 state)
{
	/* ret 汇合两条 idle 路径的最终结果。 */
	int ret;

	/* retention 路径仍从同一 C 调用栈返回。 */
	if (!psci_power_state_loses_context(state)) {
		/* context 在 restore 前保存当前 CPU 的 IRQ 相关体系结构状态。 */
		struct arm_cpuidle_irq_context context;

		ct_cpuidle_enter();
		arm_cpuidle_save_irq_context(&context);
		ret = psci_ops.cpu_suspend(state, 0);
		arm_cpuidle_restore_irq_context(&context);
		ct_cpuidle_exit();
	} else {
		/*
		 * ARM64 cpu_suspend() wants to do ct_cpuidle_*() itself.
		 */
		/*
		 * context-losing 路径的顺序不可交换：先进入 idle context，再保存
		 * 可恢复现场，最后才允许固件关闭 CPU 电源。
		 */
		if (!IS_ENABLED(CONFIG_ARM64))
			ct_cpuidle_enter();

		ret = cpu_suspend(state, psci_suspend_finisher);

		if (!IS_ENABLED(CONFIG_ARM64))
			ct_cpuidle_exit();
	}

	return ret;
}
#endif

/*
 * cpu_suspend() 的整机 SYSTEM_SUSPEND finisher。
 *
 * @unused：通用 finisher 参数，本路径固定传 0。
 * 计算 cpu_resume 物理地址并调用 PSCI SYSTEM_SUSPEND。失败转 Linux errno；
 * 成功整机 suspend 后从 cpu_resume 恢复。运行于 suspend 尾段，不分配。
 */
static int psci_system_suspend(unsigned long unused)
{
	/* err 是固件原始状态；pa_cpu_resume 是 boot CPU 的固件恢复入口。 */
	int err;
	phys_addr_t pa_cpu_resume = __pa_symbol(cpu_resume);

	err = invoke_psci_fn(PSCI_FN_NATIVE(1_0, SYSTEM_SUSPEND),
			      pa_cpu_resume, 0, 0);
	return psci_to_linux_errno(err);
}

/*
 * platform_suspend_ops.enter：执行 suspend-to-RAM 的保存与固件提交。
 *
 * @state 已由 .valid 限定为 PM_SUSPEND_MEM，本函数不再区分。先标记恢复经
 * firmware，再由 cpu_suspend 保存上下文并调用 finisher。返回 0/负 errno。
 */
static int psci_system_suspend_enter(suspend_state_t state)
{
	pm_set_resume_via_firmware();

	return cpu_suspend(0, psci_system_suspend);
}

/*
 * platform_suspend_ops.begin：标记本轮进入路径由固件负责。
 *
 * @state 是已验证目标状态；无固件调用、无失败，返回 0。该全局 PM 标志供
 * 后续架构/PM core 选择匹配的 suspend 语义。
 */
static int psci_system_suspend_begin(suspend_state_t state)
{
	pm_set_suspend_via_firmware();

	return 0;
}

/*
 * PSCI SYSTEM_SUSPEND 的静态 PM 操作表。
 * valid 只接受 suspend-to-RAM；begin 标记 firmware 路径；enter 保存上下文
 * 并提交固件。suspend_set_ops() 发布后 PM core 只借用该对象。
 */
static const struct platform_suspend_ops psci_suspend_ops = {
	.valid          = suspend_valid_only_mem,
	.enter          = psci_system_suspend_enter,
	.begin          = psci_system_suspend_begin,
};

/*
 * 探测并缓存 PSCI 1.1 SYSTEM_RESET2 支持。
 *
 * 无参数/返回。除明确 NOT_SUPPORTED 外均置 true，保持当前实现对规范固件
 * feature 结果的信任；副作用供 restart notifier 选择 warm reset。
 */
static void __init psci_init_system_reset2(void)
{
	/* ret 是 PSCI_FEATURES(SYSTEM_RESET2) 的原始状态/位图。 */
	int ret;

	ret = psci_features(PSCI_FN_NATIVE(1_1, SYSTEM_RESET2));

	if (ret != PSCI_RET_NOT_SUPPORTED)
		psci_system_reset2_supported = true;
}

/*
 * 探测 PSCI 1.3 SYSTEM_OFF2 的 HIBERNATE_OFF 类型。
 *
 * 负 feature 结果均不支持；非负位图包含相应 bit 时缓存 true，供后续
 * hibernate initcall 注册 handler。无动态资源。
 */
static void __init psci_init_system_off2(void)
{
	/* ret 是 SYSTEM_OFF2 支持的 off-type 位图或负 PSCI 状态。 */
	int ret;

	ret = psci_features(PSCI_FN_NATIVE(1_3, SYSTEM_OFF2));
	if (ret < 0)
		return;

	if (ret & PSCI_1_3_OFF_TYPE_HIBERNATE_OFF)
		psci_system_off2_hibernate_supported = true;
}

/*
 * 按配置与 feature 发布整机 suspend 操作。
 *
 * CONFIG_SUSPEND 关闭时无操作；固件除明确 NOT_SUPPORTED 外即安装静态 ops。
 * suspend_set_ops() 是对 PM core 的全局发布，初始化后不再撤销。
 */
static void __init psci_init_system_suspend(void)
{
	/* ret 只用于区分明确 NOT_SUPPORTED 与当前实现接受的其他结果。 */
	int ret;

	if (!IS_ENABLED(CONFIG_SUSPEND))
		return;

	ret = psci_features(PSCI_FN_NATIVE(1_0, SYSTEM_SUSPEND));

	if (ret != PSCI_RET_NOT_SUPPORTED)
		suspend_set_ops(&psci_suspend_ops);
}

/*
 * 缓存 CPU_SUSPEND feature 位图。
 *
 * 明确 NOT_SUPPORTED 时保留零值；其他结果原样写入 u32。规范固件应返回
 * 非负 feature 位图；若错误地返回其他负码，二进制位也会被保存并可能被当作
 * feature，因此正确性依赖 PSCI_FEATURES 对必选 CPU_SUSPEND 的规范契约。
 */
static void __init psci_init_cpu_suspend(void)
{
	/* feature 是 CPU_SUSPEND 专属 feature 位图或原始负状态。 */
	int feature = psci_features(PSCI_FN_NATIVE(0_2, CPU_SUSPEND));

	if (feature != PSCI_RET_NOT_SUPPORTED)
		psci_cpu_suspend_feature = feature;
}

/*
 * Detect the presence of a resident Trusted OS which may cause CPU_OFF to
 * return DENIED (which would be fatal).
 */
/*
 * 探测单处理器 Trusted OS 的驻留 CPU，避免危险的 CPU_OFF。
 *
 * 无参数/返回。先读取 MIGRATE_INFO_TYPE：MP TOS 无驻留限制；不支持则无法
 * 得知但按无限制处理；只有两种 UP 类型继续读取驻留 MPIDR。物理 ID 必须只含
 * MPIDR_HWID_BITMASK，随后转换为 Linux 逻辑 CPU 并发布 resident_cpu。
 *
 * 无论 TOS 声称可迁移与否，本文件都只记录、不调用 MIGRATE；未知类型、非法
 * MPIDR 或拓扑中找不到该 CPU 时保持 -1。探测发生在 CPU hotplug 可用前，
 * 因此发布无需锁。
 */
static void __init psci_init_migrate(void)
{
	/*
	 * cpuid 是固件返回的物理 affinity；type 是原始 TOS 类型；cpu 是转换后的
	 * 逻辑号，-1 哨兵防止错误路径误禁止某个正常 CPU。
	 */
	unsigned long cpuid;
	int type, cpu = -1;

	/* 阶段 1：分类 TOS 拓扑；MP 和 NOT_SUPPORTED 均无需查询驻留 CPU。 */
	type = psci_ops.migrate_info_type();

	if (type == PSCI_0_2_TOS_MP) {
		pr_info("Trusted OS migration not required\n");
		return;
	}

	if (type == PSCI_RET_NOT_SUPPORTED) {
		pr_info("MIGRATE_INFO_TYPE not supported.\n");
		return;
	}

	/* 阶段 2：只接受规范定义的两种 UP 返回，未知负值/类型不参与映射。 */
	if (type != PSCI_0_2_TOS_UP_MIGRATE &&
	    type != PSCI_0_2_TOS_UP_NO_MIGRATE) {
		pr_err("MIGRATE_INFO_TYPE returned unknown type (%d)\n", type);
		return;
	}

	/*
	 * 阶段 3：MIGRATE_INFO_UP_CPU 返回 MPIDR affinity。保留位非零说明固件
	 * 数据不可信，不能交给 get_logical_index 做拓扑索引。
	 */
	cpuid = psci_migrate_info_up_cpu();
	if (cpuid & ~MPIDR_HWID_BITMASK) {
		pr_warn("MIGRATE_INFO_UP_CPU reported invalid physical ID (0x%lx)\n",
			cpuid);
		return;
	}

	/* 逻辑映射不存在时维持 -1，避免禁止一个错误 CPU 号。 */
	cpu = get_logical_index(cpuid);
	resident_cpu = cpu >= 0 ? cpu : -1;

	pr_info("Trusted OS resident on physical CPU 0x%lx\n", cpuid);
}

/*
 * 探测并初始化 ARM SMCCC 版本。
 *
 * 无参数/返回。基线假定 SMCCC 1.0；若 PSCI_FEATURES 接受 SMCCC_VERSION，
 * 再执行该调用。只有返回 >=1.1 才交给 arm_smccc_version_init() 发布版本和
 * conduit，使 Spectre workaround/KVM 等 SMCCC 消费者选择新接口。
 *
 * feature/ret 是固件原始值，ver 是最终报告值。无动态资源，初始化后 SMCCC
 * 全局版本由其子系统只读。
 */
static void __init psci_init_smccc(void)
{
	u32 ver = ARM_SMCCC_VERSION_1_0;
	int feature;

	feature = psci_features(ARM_SMCCC_VERSION_FUNC_ID);

	if (feature != PSCI_RET_NOT_SUPPORTED) {
		/* ret 仅在固件声明该 function ID 可调用后读取。 */
		u32 ret;
		ret = invoke_psci_fn(ARM_SMCCC_VERSION_FUNC_ID, 0, 0, 0);
		if (ret >= ARM_SMCCC_VERSION_1_1) {
			arm_smccc_version_init(ret, psci_conduit);
			ver = ret;
		}
	}

	/*
	 * Conveniently, the SMCCC and PSCI versions are encoded the
	 * same way. No, this isn't accidental.
	 */
	/*
	 * SMCCC 与 PSCI 都用高 16 位 major、低 16 位 minor，故可复用 PSCI_VERSION
	 * 解码宏；这是两个规范有意保持的 ABI 一致性，不是碰巧相同。
	 */
	pr_info("SMC Calling Convention v%d.%d\n",
		PSCI_VERSION_MAJOR(ver), PSCI_VERSION_MINOR(ver));

}

/*
 * 为 PSCI 0.2+ 安装标准操作表和整机电源回调。
 *
 * 无参数/返回。复合字面量整体赋值会同时清零未列字段，形成完整一致的
 * psci_ops 快照；所有 function ID 都由规范固定。随后注册静态 restart
 * notifier，并把 legacy pm_power_off 指向 SYSTEM_OFF。
 *
 * 这些是对运行期消费者的发布点，必须在版本校验后执行。注册返回值当前被
 * 忽略；对象均静态存续，无失败回滚或动态 ownership。
 */
static void __init psci_0_2_set_functions(void)
{
	pr_info("Using standard PSCI v0.2 function IDs\n");

	/*
	 * 在尚无并发消费者的启动阶段整体构造操作表；这表达版本集合的一致性，
	 * 但不依赖结构体赋值具备运行期原子性。
	 */
	psci_ops = (struct psci_operations){
		.get_version = psci_0_2_get_version,
		.cpu_suspend = psci_0_2_cpu_suspend,
		.cpu_off = psci_0_2_cpu_off,
		.cpu_on = psci_0_2_cpu_on,
		.migrate = psci_0_2_migrate,
		.affinity_info = psci_affinity_info,
		.migrate_info_type = psci_migrate_info_type,
	};

	register_restart_handler(&psci_sys_reset_nb);

	/* 发布 legacy 全局关机入口；成功 PSCI SYSTEM_OFF 不返回。 */
	pm_power_off = psci_sys_poweroff;
}

/*
 * Probe function for PSCI firmware versions >= 0.2
 */
/*
 * 在 conduit 已就绪后探测 PSCI 0.2+ 版本并启用相应能力。
 *
 * 无参数；PSCI_VERSION 是第一个固件调用。返回 0 表示基础操作表已发布，或
 * -EINVAL 表示 DT/ACPI 宣称 0.2+、固件却报告低于 0.2，失败时不发布 ops。
 *
 * 对所有 0.2+ 固件安装 CPU/系统基础函数并检测 TOS；major>=1 才能合法调用
 * PSCI_FEATURES，随后初始化 SMCCC、idle/suspend/reset/off2 和 KVM hyp
 * services。可选 feature 缺失不使整体 probe 失败。
 */
static int __init psci_probe(void)
{
	/* ver 是 PSCI_VERSION 返回的 ABI 编码，初始化过程中保持本地快照。 */
	u32 ver = psci_0_2_get_version();

	pr_info("PSCIv%d.%d detected in firmware.\n",
			PSCI_VERSION_MAJOR(ver),
			PSCI_VERSION_MINOR(ver));

	if (PSCI_VERSION_MAJOR(ver) == 0 && PSCI_VERSION_MINOR(ver) < 2) {
		pr_err("Conflicting PSCI version detected.\n");
		return -EINVAL;
	}

	/* 发布 0.2 必选/标准操作后，运行期 SMP 与关机路径才具备入口。 */
	psci_0_2_set_functions();

	/* TOS 探测依赖刚发布的 migrate_info_type。 */
	psci_init_migrate();

	/* PSCI_FEATURES 和后续 1.x 功能只在 major>=1 时存在。 */
	if (PSCI_VERSION_MAJOR(ver) >= 1) {
		psci_init_smccc();
		psci_init_cpu_suspend();
		psci_init_system_suspend();
		psci_init_system_reset2();
		psci_init_system_off2();
		kvm_init_hyp_services();
	}

	return 0;
}

/*
 * DT compatible 对应初始化函数的类型。
 * 参数是借用 device_node，返回 0/负 errno；函数指针存于 of match .data。
 */
typedef int (*psci_initcall_t)(const struct device_node *);

/*
 * PSCI init function for PSCI versions >=0.2
 *
 * Probe based on PSCI PSCI_VERSION function
 */
/*
 * PSCI 0.2+ DT 初始化入口。
 *
 * @np：匹配 "arm,psci-0.2"/"arm,psci-1.0" 的节点借用引用。
 * 先解析 method 并发布 conduit；失败原样返回且不调用固件。成功后通过标准
 * PSCI_VERSION 自描述探测实际版本，返回 psci_probe() 的结果。
 */
static int __init psci_0_2_init(const struct device_node *np)
{
	/* err 只承载 conduit 解析结果。 */
	int err;

	err = get_set_conduit_method(np);
	if (err)
		return err;

	/*
	 * Starting with v0.2, the PSCI specification introduced a call
	 * (PSCI_VERSION) that allows probing the firmware version, so
	 * that PSCI function IDs and version specific initialization
	 * can be carried out according to the specific version reported
	 * by firmware
	 */
	/*
	 * 0.2 引入 PSCI_VERSION 后，DT 不再逐项提供 function ID；内核可依据固件
	 * 实际版本选择标准 ID 和 feature 初始化，避免 compatible 与能力硬编码。
	 */
	return psci_probe();
}

/*
 * PSCI < v0.2 get PSCI Function IDs via DT.
 */
/*
 * PSCI 0.1 DT 初始化入口。
 *
 * @np：compatible="arm,psci" 的节点借用引用。0.1 没有 PSCI_VERSION/
 * PSCI_FEATURES，先选择 conduit，再逐项读取可选的 cpu_suspend/cpu_off/
 * cpu_on/migrate u32 function ID。每个成功属性同时更新私有 ID 槽和对应
 * psci_ops wrapper；缺失项保持 NULL，不使整个初始化失败。
 *
 * 返回 conduit 错误或 0。已成功发布的属性没有回滚，因为后续无会失败阶段。
 */
static int __init psci_0_1_init(const struct device_node *np)
{
	/* id 复用为当前 DT function ID；err 保存 method 解析结果。 */
	u32 id;
	int err;

	err = get_set_conduit_method(np);
	if (err)
		return err;

	pr_info("Using PSCI v0.1 Function IDs from DT\n");

	psci_ops.get_version = psci_0_1_get_version;

	/* 每组写入顺序先保存 ID、再发布 wrapper，避免 wrapper 看见未初始化 ID。 */
	if (!of_property_read_u32(np, "cpu_suspend", &id)) {
		psci_0_1_function_ids.cpu_suspend = id;
		psci_ops.cpu_suspend = psci_0_1_cpu_suspend;
	}

	if (!of_property_read_u32(np, "cpu_off", &id)) {
		psci_0_1_function_ids.cpu_off = id;
		psci_ops.cpu_off = psci_0_1_cpu_off;
	}

	if (!of_property_read_u32(np, "cpu_on", &id)) {
		psci_0_1_function_ids.cpu_on = id;
		psci_ops.cpu_on = psci_0_1_cpu_on;
	}

	if (!of_property_read_u32(np, "migrate", &id)) {
		psci_0_1_function_ids.migrate = id;
		psci_ops.migrate = psci_0_1_migrate;
	}

	return 0;
}

/*
 * compatible="arm,psci-1.0" 的增强 DT 初始化。
 *
 * @np 借用节点；先完整执行 0.2+ 探测。若 CPU_SUSPEND feature 声明 OSI，
 * 仅报告能力并显式保持默认 PC 模式；cpuidle power-domain 驱动建立好层级
 * 拓扑后才会按策略切换 OSI，避免 OS 尚不能协调 domain 时提前接管。
 *
 * 返回基础初始化错误或 0；psci_set_osi_mode(false) 的失败只记录日志，当前
 * 函数不把它升级为 probe 失败。
 */
static int __init psci_1_0_init(const struct device_node *np)
{
	int err;

	err = psci_0_2_init(np);
	if (err)
		return err;

	if (psci_has_osi_support()) {
		pr_info("OSI mode supported.\n");

		/* Default to PC mode. */
		/* PC 让固件协调多 CPU/cluster suspend，是拓扑驱动就绪前的安全基线。 */
		psci_set_osi_mode(false);
	}

	return 0;
}

/*
 * PSCI DT compatible -> 初始化函数映射表。
 *
 * 数组和函数指针位于 __initconst，启动探测后可回收；终止空项供 OF matcher
 * 停止。更具体 compatible 选择能执行对应版本策略的入口。
 */
static const struct of_device_id psci_of_match[] __initconst = {
	{ .compatible = "arm,psci",	.data = psci_0_1_init},
	{ .compatible = "arm,psci-0.2",	.data = psci_0_2_init},
	{ .compatible = "arm,psci-1.0",	.data = psci_1_0_init},
	{},
};

/*
 * Device Tree PSCI 顶层初始化。
 *
 * 无参数；在整棵树中查找首个匹配节点并同时取得 matched_np。成功查找返回
 * 带引用的 np，本函数在所有出口 put；matched_np 指向静态 match 表无需释放。
 *
 * 节点缺失/disabled 返回 -ENODEV。可用时把 .data 还原为 psci_initcall_t，
 * 调用版本入口并原样返回其结果。成功后 conduit/psci_ops 已发布，节点引用
 * 不再被操作表依赖。
 */
int __init psci_dt_init(void)
{
	/*
	 * np 是持有引用的 OF 节点；matched_np 是静态表借用项；init_fn 是版本
	 * 分派回调；ret 汇总最终初始化状态。
	 */
	struct device_node *np;
	const struct of_device_id *matched_np;
	psci_initcall_t init_fn;
	int ret;

	np = of_find_matching_node_and_match(NULL, psci_of_match, &matched_np);

	/* of_node_put(NULL) 安全，故缺失和 disabled 可共用清理出口。 */
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		return -ENODEV;
	}

	/* .data 在表中存的是函数地址；调用期间 np 引用仍由本函数持有。 */
	init_fn = (psci_initcall_t)matched_np->data;
	ret = init_fn(np);

	of_node_put(np);
	return ret;
}

#ifdef CONFIG_ACPI
/*
 * We use PSCI 0.2+ when ACPI is deployed on ARM64 and it's
 * explicitly clarified in SBBR
 */
/*
 * ARM64 ACPI/SBBR 明确要求 PSCI 0.2+，因此 ACPI 路径无需支持 0.1 私有 ID；
 * FADT/MADT 平台标志同时提供 PSCI 是否存在及 HVC/SMC conduit 选择。
 */
/*
 * ACPI PSCI 顶层初始化。
 *
 * 无参数。ACPI 未声明 PSCI 返回 -EOPNOTSUPP；否则按 ACPI 标志发布 HVC 或
 * SMC，并直接进入标准 psci_probe()。成功后契约与 DT 0.2+ 相同。
 *
 * ACPI 表已由架构代码解析为只读全局标志，本函数不持有表引用、不分配。
 */
int __init psci_acpi_init(void)
{
	if (!acpi_psci_present()) {
		pr_info("is not implemented in ACPI.\n");
		return -EOPNOTSUPP;
	}

	pr_info("probing for conduit method from ACPI.\n");

	/* ACPI 只提供二选一标志；false 按规范选择 SMC。 */
	if (acpi_psci_use_hvc())
		set_conduit(SMCCC_CONDUIT_HVC);
	else
		set_conduit(SMCCC_CONDUIT_SMC);

	return psci_probe();
}
#endif
