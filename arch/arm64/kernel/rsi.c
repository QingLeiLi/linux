// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 Realm Services Interface（RSI）启动与内存属性策略学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件把 Arm CCA Realm Monitor（RMM）提供的 RSI 接入 ARM64 内核：
 *
 *   setup_arch()
 *       -> arm64_rsi_init()
 *           -> 确认 SMC conduit 与 RSI ABI 版本
 *           -> 读取 Realm 的 IPA 位宽等配置
 *           -> 注册 ioremap 页属性策略
 *           -> 注册 set_memory_{en,de}crypted() 后端
 *           -> 把 memblock RAM 转为受保护的 RIPAS_RAM
 *           -> 启用 rsi_present static key，正式发布 Realm 状态
 *
 * 运行期主要消费者：
 *   - cc_platform_has()/is_realm_world() 查询机密计算能力；
 *   - ioremap 路径按 RIPAS 为映射选择 protected 或 NS shared IPA；
 *   - DMA/SWIOTLB 和 pageattr 路径在 private/shared 间转换页面；
 *   - arch_initcall 注册虚拟 platform_device，触发 Arm CCA guest 驱动。
 *
 * 核心状态由 RMM 维护：RIPAS_RAM/DEV 表示 Realm 侧受信范围，
 * RIPAS_EMPTY 表示由非可信 Host 模拟或共享的范围，RIPAS_DESTROYED 表示
 * Host 未经允许破坏了受保护内容。内核只缓存 Realm 配置和页表中的
 * NS_SHARED 编码，不在本文件复制一份逐页 RIPAS 数据库。
 *
 * 初始化在启动 CPU 上串行完成，没有专用锁。rsi_present 初始为 false，
 * 只有所有不可缺少的策略和 RAM 状态建立后才一次性启用；运行期查询通过
 * static branch 获得低开销。RSI SMC 是同步跨安全边界调用，不转移普通
 * struct page ownership，但会改变 RMM/Host 对 IPA 的解释，调用顺序属于
 * 安全协议，不能等同于普通 PTE 属性修改。
 */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#include <linux/jump_label.h>
#include <linux/memblock.h>
#include <linux/psci.h>
#include <linux/swiotlb.h>
#include <linux/cc_platform.h>
#include <linux/platform_device.h>

#include <asm/io.h>
#include <asm/mem_encrypt.h>
#include <asm/pgtable.h>
#include <asm/rsi.h>

/*
 * RMM 写回的当前 Realm 配置快照。
 *
 * struct realm_config 自带 4 KiB 对齐和 4 KiB 大小，满足
 * RSI_REALM_CONFIG 的 granule 要求。对象在内核整个运行期存在，但只在
 * arm64_rsi_init() 的早期单 CPU 阶段写入；随后本文件只读取 ipa_bits。
 * 它不代表对 RMM 对象的引用，也不需要释放。
 */
static struct realm_config config;

/*
 * 页表描述符中表示 Non-Secure/shared IPA 的地址位编码。
 *
 * RMM 报告的 ipa_bits 决定“top IPA bit”；arm64_rsi_init() 把该物理地址位
 * 转换为 PTE 编码后写入本变量。初值 0，在 rsi_present 发布前消费者通过
 * PROT_NS_SHARED 的 is_realm_world() 条件不会把 0 当作有效共享位。初始化后
 * 只读，导出给页表、DMA 和模块代码使用。
 */
unsigned long prot_ns_shared;
EXPORT_SYMBOL(prot_ns_shared);

/*
 * 全系统 Realm 就绪状态的 static key。
 *
 * 初始 false，让普通 ARM64 平台上的 is_realm_world() 被跳转标签优化为廉价
 * 常量路径；arm64_rsi_init() 仅在全部必要步骤成功后启用一次。RO 变体表达
 * 初始化后不再切回 false，导出后所有消费者共享同一发布状态。
 */
DEFINE_STATIC_KEY_FALSE_RO(rsi_present);
EXPORT_SYMBOL(rsi_present);

/*
 * cc_platform_has() - 向通用机密计算代码报告 ARM64 Realm 能力。
 *
 * @attr：调用者查询的 cc_attr 枚举值，纯输入，无 ownership。通用接口允许
 * 任意上下文调用；本实现只读取 static key，不持锁、不睡眠、没有副作用。
 *
 * 当前只把 CC_ATTR_MEM_ENCRYPT 映射为 is_realm_world()：RSI 完整初始化后
 * 返回 true，否则返回 false。其他属性即使在别的平台有含义，本实现也明确
 * 返回 false，避免用“运行在 Realm”推导未经实现声明的细粒度能力。
 */
bool cc_platform_has(enum cc_attr attr)
{
	/* 只公开本实现能够由 rsi_present 就绪状态证明的通用内存保护属性。 */
	switch (attr) {
	case CC_ATTR_MEM_ENCRYPT:
		return is_realm_world();
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(cc_platform_has);

/*
 * rsi_version_matches() - 与 RMM 协商本内核要求的 RSI ABI 版本。
 *
 * 入参：无。由 arm64_rsi_init() 在任何依赖 RSI 的操作之前调用；入口已经
 * 确认 SMCCC conduit 是 SMC。函数不持锁、不分配内存，通过同步 SMC 与 RMM
 * 通信。
 *
 * ver_lower/ver_higher 接收 RMM 支持区间，编码均为 major:minor；ret 是 RSI/
 * SMCCC 状态。RMM 不实现版本调用时返回 false 且静默退化为非 Realm；
 * 实现存在但拒绝本内核版本时打印支持区间并返回 false；协商成功打印选用
 * 版本并返回 true。失败不发布任何全局 RSI 状态，也没有资源需要回滚。
 */
static bool rsi_version_matches(void)
{
	/*
	 * 变量地图：
	 *   ver_lower/ver_higher 是 RMM 写回的兼容版本区间；
	 *   ret 固定本次 SMC 的状态，后续分支不再重复跨边界调用。
	 */
	unsigned long ver_lower, ver_higher;
	unsigned long ret = rsi_request_version(RSI_ABI_VERSION,
						&ver_lower,
						&ver_higher);

	/*
	 * SMCCC_RET_NOT_SUPPORTED 表示连 RSI_ABI_VERSION function ID 都不可用，
	 * 常见于普通世界或不支持 CCA 的固件；此时输出寄存器不供诊断使用。
	 */
	if (ret == SMCCC_RET_NOT_SUPPORTED)
		return false;

	/*
	 * 其他非成功状态表示 RMM 能回答版本请求，但本内核请求不在可接受范围；
	 * 此时协议提供 lower/higher，日志同时展示请求值和支持区间。
	 */
	if (ret != RSI_SUCCESS) {
		pr_err("RME: RMM doesn't support RSI version %lu.%lu. Supported range: %lu.%lu-%lu.%lu\n",
		       RSI_ABI_VERSION_MAJOR, RSI_ABI_VERSION_MINOR,
		       RSI_ABI_VERSION_GET_MAJOR(ver_lower),
		       RSI_ABI_VERSION_GET_MINOR(ver_lower),
		       RSI_ABI_VERSION_GET_MAJOR(ver_higher),
		       RSI_ABI_VERSION_GET_MINOR(ver_higher));
		return false;
	}

	/* 协商成功后 lower 是本次接口选择所依据的版本，供启动日志确认 ABI。 */
	pr_info("RME: Using RSI version %lu.%lu\n",
		RSI_ABI_VERSION_GET_MAJOR(ver_lower),
		RSI_ABI_VERSION_GET_MINOR(ver_lower));

	return true;
}

/*
 * arm64_rsi_setup_memory() - 把固件交给内核的全部 memblock RAM确认为受保护 RAM。
 *
 * 入参：无。由 arm64_rsi_init() 在版本、配置和运行期策略注册成功后调用，
 * 但 rsi_present 尚未发布。隐式输入是 memblock.memory；for_each_mem_range()
 * 逐个返回 [start,end) 物理字节区间，包括其中的 reserved 子区间，因为这里
 * 处理的是 Realm IPA 状态而不是伙伴分配器可分配性。
 *
 * 函数在早期单 CPU、memblock 仍有效的上下文执行，不持锁；每个区间通过
 * 同步 RSI SMC 请求 RMM 设置 RIPAS_RAM。成功无直接返回值，保证所有遍历
 * 范围都处于 protected 状态。任一区间失败即 panic，因为部分建立后的
 * Realm 内存模型无法安全回滚或继续启动。
 */
static void __init arm64_rsi_setup_memory(void)
{
	/*
	 * 变量地图：
	 *   i         memblock 迭代游标，仅供宏维护扫描进度；
	 *   start/end 当前物理半开区间，单位字节，由 memblock 借出。
	 */
	u64 i;
	phys_addr_t start, end;

	/*
	 * Iterate over the available memory ranges and convert the state to
	 * protected memory. We should take extra care to ensure that we DO NOT
	 * permit any "DESTROYED" pages to be converted to "RAM".
	 *
	 * panic() is used because if the attempt to switch the memory to
	 * protected has failed here, then future accesses to the memory are
	 * simply going to be reflected as a SEA (Synchronous External Abort)
	 * which we can't handle.  Bailing out early prevents the guest limping
	 * on and dying later.
	 */
	/*
	 * 遍历全部可用内存范围并把状态转换为 protected memory。这里必须特别
	 * 小心，绝不能允许已处于 DESTROYED 的页面重新变成 RAM。
	 *
	 * safe helper 使用 RSI_NO_CHANGE_DESTROYED：如果 Host 已经未经 Realm
	 * 许可破坏某页，RMM 不会把它伪装成内容可信的 RAM。失败后继续访问该
	 * 范围只会产生内核无法处理的 SEA（同步外部异常），所以立即 panic 比
	 * 带着部分可用内存勉强启动、稍后随机死亡更可诊断也更安全。
	 */
	for_each_mem_range(i, &start, &end) {
		/*
		 * helper 可能把大范围拆成多次 RSI_IPA_STATE_SET，并验证 RMM 每次
		 * 返回的 top 单调前进且不越过 end；非零表示本区间未完整转换。
		 */
		if (rsi_set_memory_range_protected_safe(start, end)) {
			panic("Failed to set memory range to protected: %pa-%pa",
			      &start, &end);
		}
	}
}

/*
 * Check if a given PA range is Trusted (e.g., Protected memory, a Trusted Device
 * mapping, or an MMIO emulated in the Realm world).
 *
 * We can rely on the RIPAS value of the region to detect if a given region is
 * protected.
 *
 *  RIPAS_DEV - A trusted device memory or a trusted emulated MMIO (in the Realm
 *		world
 *  RIPAS_RAM - Memory (RAM), protected by the RMM guarantees. (e.g., Firmware
 *		reserved regions for data sharing).
 *
 *  RIPAS_DESTROYED is a special case of one of the above, where the host did
 *  something without our permission and as such we can't do anything about it.
 *
 * The only case where something is emulated by the untrusted hypervisor or is
 * backed by shared memory is indicated by RSI_RIPAS_EMPTY.
 */
/*
 * 判断给定物理区间是否属于 Realm 侧的受信/非共享地址空间。
 *
 * 受信范围包括 protected RAM、可信设备映射和 Realm 世界内模拟的 MMIO。
 * RIPAS 足以判定：RIPAS_DEV 表示可信设备内存或可信 Realm MMIO，
 * RIPAS_RAM 表示受 RMM 保证的 RAM（也可能是固件保留的共享数据区）。
 * RIPAS_DESTROYED 表示上述受保护对象曾被 Host 未经允许破坏，内容已不能
 * 使用，但它仍不能按普通 Host-shared EMPTY 地址建立 NS 别名。只有
 * RSI_RIPAS_EMPTY 明确表示由非可信 hypervisor 模拟或共享内存支撑。
 *
 * arm64_rsi_is_protected() - 查询 [base, base + size) 是否全部非 EMPTY。
 *
 * @base：物理/IPA 起始字节地址，纯输入，可不按 RSI granule 对齐。
 * @size：查询字节数，必须大于 0，且 base + size 不能回绕。
 *
 * 主要由 ioremap 属性 hook 和 arm64_is_protected_mmio() 调用。函数不持锁、
 * 不分配内存；每轮 rsi_ipa_state_get() 是同步 SMC，RMM 返回当前同一 RIPAS
 * 连续段的尾地址 top。返回 true 表示覆盖请求范围的所有 granules 均不是
 * EMPTY；范围非法、RSI 错误、RMM 不推进 top 或遇到 EMPTY 均返回 false，
 * 并对协议异常发出 WARN。
 */
bool arm64_rsi_is_protected(phys_addr_t base, size_t size)
{
	/*
	 * 变量地图：
	 *   ripas 当前 RMM 返回的连续段状态；
	 *   end   请求向上扩到 granule 边界后的排他末端；
	 *   top   当前同状态段的排他末端，也是下一轮 base。
	 */
	enum ripas ripas;
	phys_addr_t end, top;

	/* Overflow ? */
	/*
	 * 原注释询问是否发生溢出。<= 同时拒绝 size==0 和无符号加法回绕；
	 * WARN_ON 暴露调用者传入的无意义/危险区间，失败时不发起 RSI 查询。
	 */
	if (WARN_ON(base + size <= base))
		return false;

	/*
	 * RIPAS 以 4 KiB granule 为最小单位，必须检查与原请求相交的完整首尾
	 * granules。向下/向上对齐扩大查询不会误判，因为同一 granule 不能同时
	 * 具有两种 RIPAS。
	 */
	end = ALIGN(base + size, RSI_GRANULE_SIZE);
	base = ALIGN_DOWN(base, RSI_GRANULE_SIZE);

	/*
	 * RMM 可按状态连续段分批回答。每轮成功且 top 前进时才能缩小剩余区间；
	 * 任一 WARN 分支 break 后 base 仍小于 end，最终统一返回 false。
	 */
	while (base < end) {
		/* 查询 [base,end)，输出当前段 ripas 与可直接跳过的排他末端 top。 */
		if (WARN_ON(rsi_ipa_state_get(base, end, &ripas, &top)))
			break;
		/* 防御错误 RMM 响应，避免 top 不前进导致无限 SMC 循环。 */
		if (WARN_ON(top <= base))
			break;
		/* EMPTY 是唯一明确的 Host-shared/untrusted backing，立即判为非保护。 */
		if (ripas == RSI_RIPAS_EMPTY)
			break;
		base = top;
	}

	/* 只有游标覆盖到 end 才证明整个对齐后的范围均为非 EMPTY。 */
	return base >= end;
}
EXPORT_SYMBOL(arm64_rsi_is_protected);

/*
 * realm_ioremap_hook() - 为每次 ARM64 ioremap 选择 Realm/NS shared IPA 属性。
 *
 * @phys：拟映射物理区间的起始字节地址，纯输入。
 * @size：拟映射长度，单位字节，必须满足 ioremap 的范围约束。
 * @prot：借用的输入输出页属性指针，不可为 NULL；只修改当前 ioremap 栈上的
 * pgprot 副本，不保存指针、不取得引用。
 *
 * arm64_rsi_init() 把本函数注册为唯一 ioremap_prot_hook；随后
 * __ioremap_prot() 在建立页表前同步调用。查询范围为非 EMPTY 时清除
 * NS_SHARED 位，使用 Realm/protected IPA；EMPTY 或查询失败时设置 top IPA
 * bit，使用 Host 可见的 shared IPA。返回值恒为 0，因此本策略只选择属性，
 * 不直接拒绝映射；查询异常已经由 arm64_rsi_is_protected() 告警。
 *
 * 函数不分配、不睡眠，但会通过 RSI SMC 跨入 RMM；无 Linux 锁或 ownership
 * 转移。页表建立和失败回滚仍由通用 ioremap 路径负责。
 */
static int realm_ioremap_hook(phys_addr_t phys, size_t size, pgprot_t *prot)
{
	/*
	 * pgprot_encrypted/decrypted 在 ARM64 Realm 上分别清除/设置
	 * PROT_NS_SHARED；其余 cache、执行和设备内存属性保持不变。
	 */
	if (arm64_rsi_is_protected(phys, size))
		*prot = pgprot_encrypted(*prot);
	else
		*prot = pgprot_decrypted(*prot);

	return 0;
}

/*
 * arm64_rsi_init() - 探测 Realm 环境并提交 ARM64 RSI 全局运行状态。
 *
 * 调用关系：setup_arch() 在 PSCI 和 early_ioremap 初始化之后、SMP 与大量
 * 设备初始化之前调用。入参和直接返回值均无；失败采用静默/告警后返回，
 * 保持 rsi_present=false，使通用代码继续按非 Realm 平台工作。
 *
 * 入口是启动 CPU 的串行 __init 上下文，不持锁，SMC 与策略注册均可在这里
 * 执行。主要阶段为：验证 SMC conduit、协商 ABI、读取 Realm 配置、计算
 * shared IPA 页表位、注册 ioremap hook、注册内存转换后端、保护全部 RAM，
 * 最后以 static key 发布成功。
 *
 * 成功返回后 is_realm_world() 才对全系统为 true，prot_ns_shared、两个策略
 * 后端和 RAM RIPAS 已全部可用。注册接口没有注销能力；若 ioremap hook 已
 * 安装而后续 memory-enc 注册失败，当前代码会留下该 hook 但不启用
 * rsi_present。RAM 转换失败则 panic，不尝试从部分 RIPAS 状态回滚。
 */
void __init arm64_rsi_init(void)
{
	/*
	 * 阶段 1：RSI 规范使用 SMC 进入 RMM。若平台选择 HVC 或没有 conduit，
	 * 不能把普通固件环境误识别为 Realm，直接保留 static key 的 false 状态。
	 */
	if (arm_smccc_1_1_get_conduit() != SMCCC_CONDUIT_SMC)
		return;
	/* ABI 不存在或不兼容时，任何后续 RSI function ID 都不能安全调用。 */
	if (!rsi_version_matches())
		return;
	/*
	 * 阶段 2：让 RMM 把完整 Realm 配置写入 4 KiB 对齐对象。
	 *
	 * config 位于内核镜像，lm_alias() 取得指向同一物理存储的 linear-map
	 * 别名；rsi helper 再以 virt_to_phys() 生成 RMM 所需 IPA。传入的是短期
	 * 虚拟别名，RMM 实际接收物理缓冲区地址，不取得 config ownership。
	 * 失败表示后续连 IPA 位宽都未知。
	 */
	if (WARN_ON(rsi_get_realm_config(lm_alias(&config))))
		return;
	/*
	 * CCA 用 Realm IPA 的最高有效位区分 NS/shared 别名。BIT(ipa_bits - 1)
	 * 先得到物理地址位，__phys_to_pte_val() 再处理 ARM64 PTE 中地址位的实际
	 * 编码，包括相关地址宽度布局；结果供 pgprot_decrypted() 等宏使用。
	 */
	prot_ns_shared = __phys_to_pte_val(BIT(config.ipa_bits - 1));

	/*
	 * 阶段 3：在任何普通设备 ioremap 前安装全局属性策略。注册接口只允许
	 * 一个永久 hook；非零表示已有策略，当前初始化不能安全覆盖它。
	 */
	if (arm64_ioremap_prot_hook_register(realm_ioremap_hook))
		return;

	/*
	 * 将 set_memory_encrypted/decrypted 分派到 Realm 的三阶段转换：
	 * 先使 direct-map PTE 无效，再改变 RIPAS，最后恢复有效映射。重复注册
	 * 返回错误；这里没有反注册前一步 ioremap hook 的接口。
	 */
	if (realm_register_memory_enc_ops())
		return;

	/*
	 * 阶段 4：在向普通消费者宣告 Realm 之前，把固件交接的全部 RAM 建立为
	 * protected RIPAS。此步骤失败不可恢复，会在 helper 内 panic。
	 */
	arm64_rsi_setup_memory();

	/*
	 * 最终提交点：此前写入的配置、函数指针和 RMM 状态都已完成。static key
	 * 启用后，其他初始化路径开始强制 SWIOTLB、使用 NS_SHARED 地址并加载
	 * Realm 专用驱动；该状态没有关闭路径。
	 */
	static_branch_enable(&rsi_present);
}

/*
 * 供 Arm CCA guest 驱动匹配的静态虚拟 platform_device。
 *
 * 它没有 MMIO/IRQ 资源，只用名称 RSI_PDEV_NAME 形成 platform modalias；
 * id=PLATFORM_DEVID_NONE 表示不追加数字实例号。对象为静态存储期，注册成功
 * 后其内嵌 device 由 platform core 持有注册引用；底层存储始终是本文件的
 * 静态对象，本文件没有注销路径。
 */
static struct platform_device rsi_dev = {
	.name = RSI_PDEV_NAME,
	.id = PLATFORM_DEVID_NONE
};

/*
 * arm64_create_dummy_rsi_dev() - 在已确认 Realm 时发布 CCA guest 设备。
 *
 * 入参：无。arch_initcall 在 arm64_rsi_init() 完成之后调用；入口为可睡眠的
 * 启动期上下文，不持锁。is_realm_world() 为 false 时无副作用；为 true 时
 * platform_device_register() 把静态 rsi_dev 发布到 device model，驱动核心
 * 可匹配/自动加载名为 "arm-cca-dev" 的 attestation 驱动。
 *
 * 函数总返回 0：注册失败只记录错误，不让一个可选的 guest 服务阻止内核
 * 启动。成功后设备保持注册到系统结束；失败时设备没有进入 device model，
 * 当前代码只记录错误，不重试也不执行额外注销。
 */
static int __init arm64_create_dummy_rsi_dev(void)
{
	/* && 短路保证普通世界绝不会注册一个虚假的 CCA guest 设备。 */
	if (is_realm_world() &&
	    platform_device_register(&rsi_dev))
		pr_err("failed to register rsi platform device\n");
	return 0;
}

/*
 * 把虚拟设备发布安排在 arch initcall 阶段：此时 Realm static key 已提交，
 * device model 也可接受平台设备，而具体 guest 驱动可稍后匹配。
 */
arch_initcall(arm64_create_dummy_rsi_dev)
