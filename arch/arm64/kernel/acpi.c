// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 ACPI 平台适配学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 职责边界：本文件把体系结构无关 ACPI 核心接到 arm64 的启动策略、早期
 * 映射、EFI 内存属性、PSCI、firmware-first RAS 和 CPU 热插拔协议。它不
 * 解析 AML namespace，不建立 GIC/NUMA 拓扑，也不实现 ACPICA 表管理本身。
 *
 * 主调用链：
 *   setup_arch()
 *     -> acpi_table_upgrade()
 *     -> acpi_boot_table_init()
 *        -> ACPI/DT 选择 -> acpi_table_init() -> FADT 合规检查
 *        -> FACS 休眠签名、SPCR 控制台、可选 BGRT
 *   ACPICA OSL -> __acpi_map_table()/acpi_os_ioremap() -> arm64 页属性映射
 *   synchronous external abort/KVM -> apei_claim_sea() -> GHES -> irq_work
 *   ACPI processor/NUMA -> CPU UID 与逻辑 CPU 映射 helper
 *
 * 核心状态与生命周期：
 * - acpi_disabled/acpi_pci_disabled/acpi_noirq 初始都为 1；只有启动策略允许时
 *   enable_acpi() 才同时清零。初始化或 FADT 校验失败会恢复禁用，除非用户
 *   显式 acpi=force；该选择随后决定设备树是否展开以及 ACPI 子系统是否继续。
 * - early_param 只记录用户策略，真正提交发生在 acpi_boot_table_init()。
 * - ACPI 表早期映射使用 early_memremap 临时持有；永久阶段映射由通用 OSL
 *   缓存和引用管理，本文件只选择 arm64 页属性，不取得固件内存 ownership。
 * - EFI 描述符和 MADT GICC 都是借用元数据；CPU present mask、memblock NOMAP
 *   及 GHES/irq_work 状态才是本文件对外可见的体系结构副作用。
 *
 * 并发模型：启动选择、表校验和早期映射在 boot CPU 串行执行；CPU 热插拔
 * helper 由调用者持有 cpu_maps/cpus 写侧锁；SEA 路径运行在异常/NMI-like
 * 上下文，通过 DAIF 保存恢复、nmi_enter/exit 和 irq_work 延迟处理维持上下文
 * 约束。本文件不为这些外部协议另设私有锁。
 *
 * 方案权衡：arm64 优先沿 EFI 内存图选择精确页属性，避免 writable/executable
 * 别名和把普通 RAM 当 MMIO；代价是跨 EFI 区域映射被拒绝，并需为缺失描述符
 * 保留设备内存回退。ACPI 与 DT 互斥减少双重硬件描述冲突，acpi=force 则为
 * 有缺陷平台保留诊断/兼容入口，但可能让启动继续使用未完全验证的 ACPI。
 */
/*
 *  ARM64 Specific Low-Level ACPI Boot Support
 *
 *  Copyright (C) 2013-2014, Linaro Ltd.
 *	Author: Al Stone <al.stone@linaro.org>
 *	Author: Graeme Gregory <graeme.gregory@linaro.org>
 *	Author: Hanjun Guo <hanjun.guo@linaro.org>
 *	Author: Tomasz Nowicki <tomasz.nowicki@linaro.org>
 *	Author: Naresh Bhat <naresh.bhat@linaro.org>
 */

#define pr_fmt(fmt) "ACPI: " fmt

#include <linux/acpi.h>
#include <linux/arm-smccc.h>
#include <linux/cpumask.h>
#include <linux/efi.h>
#include <linux/efi-bgrt.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irq_work.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/smp.h>
#include <linux/serial_core.h>
#include <linux/suspend.h>
#include <linux/pgtable.h>

#include <acpi/ghes.h>
#include <acpi/processor.h>
#include <asm/cputype.h>
#include <asm/cpu_ops.h>
#include <asm/daifflags.h>
#include <asm/smp_plat.h>

/*
 * 三个全局禁用开关构成一组启动策略状态：enable_acpi()/disable_acpi() 必须
 * 同步更新它们，避免出现“解析 ACPI、却跳过 PCI 或 IRQ”这种半启用状态。
 * 初值 1 保证任何校验前默认走安全的 DT 路径；后续读者包括 ACPI core、
 * PCI 和中断初始化。变量在系统生命周期内有效，启动期写、之后以策略只读。
 */
int acpi_noirq = 1;		/* skip ACPI IRQ initialization */
/* 原注释表示：初始状态跳过 ACPI IRQ 初始化，待 enable_acpi() 后才允许。 */
int acpi_disabled = 1;
EXPORT_SYMBOL(acpi_disabled);

int acpi_pci_disabled = 1;	/* skip ACPI PCI scan and IRQ initialization */
/* 原注释表示：初始状态同时跳过 ACPI PCI 枚举及其 IRQ 初始化。 */
EXPORT_SYMBOL(acpi_pci_disabled);

/*
 * acpi= early parameter 的四个记忆位只记录命令行意图，不直接切换全局状态。
 * off 优先级最高；on 允许 ACPI 覆盖非空 DT；force 还允许初始化失败后继续；
 * nospcr 仅抑制把 SPCR 选为默认串口，不关闭 ACPI 表解析。
 */
static bool param_acpi_off __initdata;
static bool param_acpi_on __initdata;
static bool param_acpi_force __initdata;
static bool param_acpi_nospcr __initdata;

/*
 * parse_acpi - 解析 arm64 的 acpi= 启动策略并暂存选择。
 *
 * 调用关系：early_param 在启动命令行扫描期调用；结果由
 * acpi_boot_table_init() 一次性消费。
 * 入参：@arg 是参数框架借用的 NUL 结尾字符串，不可为 NULL；本函数不保存
 * 指针、不取得 ownership。启动 CPU 串行执行，可用普通字符串比较，不睡眠。
 * 返回：识别 off/on/force/nospcr 时返回 0 并设置对应 __initdata 标志；
 * NULL 或未知值返回 -EINVAL，由参数核心统一输出错误。多个标志若同时出现，
 * 此处不互相清除，最终优先级由启动决策显式处理。
 */
static int __init parse_acpi(char *arg)
{
	/* 入口校验尚未改变任何策略位，失败可直接交回参数核心。 */
	if (!arg)
		return -EINVAL;

	/* "acpi=off" disables both ACPI table parsing and interpreter */
	/* 完整含义：off 同时禁止 ACPI 表解析和解释器，后续决策中优先级最高。 */
	if (strcmp(arg, "off") == 0)
		param_acpi_off = true;
	else if (strcmp(arg, "on") == 0) /* prefer ACPI over DT */
		/* on 在 ACPI 与设备树并存时选择 ACPI，但不放宽表校验失败。 */
		param_acpi_on = true;
	else if (strcmp(arg, "force") == 0) /* force ACPI to be enabled */
		/* force 既选择 ACPI，也允许初始化失败后维持 enabled 状态。 */
		param_acpi_force = true;
	else if (strcmp(arg, "nospcr") == 0) /* disable SPCR as default console */
		/* nospcr 只关闭 SPCR 默认串口选择，不影响其他 ACPI 功能。 */
		param_acpi_nospcr = true;
	else
		return -EINVAL;	/* Core will print when we return error */
	/* 未知值由 early-param 核心打印，避免体系结构代码重复诊断。 */

	return 0;
}
early_param("acpi", parse_acpi);

/*
 * dt_is_stub - 判断固件设备树是否只含可与 ACPI 共存的占位节点。
 *
 * 调用关系：acpi_boot_table_init() 在没有 acpi=on/force 时用它决定是否允许
 * ACPI 接管。@initial_boot_params 是早期扁平 DT 借用缓冲区，调用期间稳定。
 * 函数只遍历根节点的直接子节点：/chosen 始终忽略；Xen 环境允许兼容
 * "xen,xen" 的 /hypervisor；发现任何其他节点立即返回 false。
 * 返回 true 表示 DT 不承载平台硬件描述，ACPI 可安全成为唯一枚举来源；
 * 无分配、无引用和外部副作用，启动期串行且不睡眠。
 */
static bool __init dt_is_stub(void)
{
	int node;

	/* 扁平树迭代器返回节点偏移，name 指针仍借用 initial_boot_params。 */
	fdt_for_each_subnode(node, initial_boot_params, 0) {
		const char *name = fdt_get_name(initial_boot_params, node, NULL);

		/* chosen 只承载 bootargs/stdout 等启动选择，不描述可枚举硬件。 */
		if (strcmp(name, "chosen") == 0)
			continue;
		/* Xen hypervisor 节点是 ACPI 客体环境的允许例外。 */
		if (strcmp(name, "hypervisor") == 0 &&
		    of_flat_dt_is_compatible(node, "xen,xen"))
			continue;

		/* 第一个真实硬件/平台节点即可证明 DT 非占位树。 */
		return false;
	}

	return true;
}

/*
 * __acpi_map_table() will be called before page_init(), so early_ioremap()
 * or early_memremap() should be called here to for ACPI table mapping.
 */
/*
 * 补充说明：__acpi_map_table - 为 ACPICA 早期表发现建立临时物理映射。
 *
 * 通用 OSL 在 acpi_permanent_mmap 尚未建立时调用。@phys 是物理字节地址，
 * @size 是映射字节数；二者均为纯输入，size 为 0 时无可映射对象。
 * 原注释说明调用早于 page_init()，因此必须使用 early_ioremap 或
 * early_memremap；arm64 选择后者。成功返回临时 __iomem 借用地址，调用者
 * 必须以同一 size 交给 __acpi_unmap_table()；失败/零长度返回 NULL。
 * 函数不取得物理内存 ownership，启动期串行，映射槽生命周期由 early map
 * 协议管理。
 */
void __init __iomem *__acpi_map_table(unsigned long phys, unsigned long size)
{
	if (!size)
		return NULL;

	return early_memremap(phys, size);
}

/*
 * __acpi_unmap_table - 释放早期 ACPI 表的临时映射槽。
 *
 * @map/@size 必须与成功的 __acpi_map_table() 配对；map 是借用的 __iomem
 * 地址，size 单位为字节。NULL 或零长度表示未取得资源，安全返回。
 * 返回 void；有效输入会解除虚拟映射但不释放或修改固件物理表。
 */
void __init __acpi_unmap_table(void __iomem *map, unsigned long size)
{
	if (!map || !size)
		return;

	early_memunmap(map, size);
}

/*
 * acpi_psci_present - 查询 FADT 是否声明 PSCI 启动协议。
 *
 * PSCI 初始化和 arm64 CPU enable-method 选择在 ACPI 表已建立后调用。无入参，
 * 仅读取 ACPICA 保存的 FADT arm_boot_flags，不取得表引用、不睡眠。
 * 返回 true 表示 ACPI_FADT_PSCI_COMPLIANT 已置位，后续使用 PSCI 而不是
 * parking protocol；false 表示需要其他 CPU 启动方法。
 */
bool __init acpi_psci_present(void)
{
	return acpi_gbl_FADT.arm_boot_flags & ACPI_FADT_PSCI_COMPLIANT;
}

/* Whether HVC must be used instead of SMC as the PSCI conduit */
/*
 * 原注释说明此函数判断 PSCI conduit 是否必须用 HVC 代替 SMC。
 * acpi_psci_use_hvc - 返回 FADT 的 PSCI_USE_HVC 策略位。
 *
 * 无入参、无副作用；PSCI 与 SDEI 初始化读取该结果。只有 PSCI compliant
 * 路径才应把该位解释为 conduit 选择，true 进入虚拟化 HVC 调用，false
 * 使用 SMC。读取的是 ACPICA 长期保存的 FADT 副本，不需要 get/put 表引用。
 */
bool acpi_psci_use_hvc(void)
{
	return acpi_gbl_FADT.arm_boot_flags & ACPI_FADT_PSCI_USE_HVC;
}

/*
 * acpi_fadt_sanity_check() - Check FADT presence and carry out sanity
 *			      checks on it
 *
 * Return 0 on success,  <0 on failure
 */
/*
 * 补充说明：acpi_fadt_sanity_check - 验证 arm64 启动所需的 FADT 最低契约。
 *
 * 调用关系：acpi_boot_table_init() 在初始表目录建立后调用；通过后其他代码
 * 才能依赖 PSCI/GIC/SMP 启动字段。无入参，启动 CPU 串行，可执行表映射。
 * 阶段：持有第一张 FADT 引用；检查版本至少 5.1（旧版本仅在存在 ARM boot
 * flags 时兼容继续）；检查 HW_REDUCED；最后无条件 put。
 * 返回 0 表示可继续 ACPI；-ENODEV 表示 FADT 不存在；-EINVAL 表示版本/ARM
 * flags 或 hardware-reduced 契约不满足。失败不保留引用，日志是额外副作用。
 */
static int __init acpi_fadt_sanity_check(void)
{
	/*
	 * 变量地图：table 是 acpi_get_table() 持有的通用表引用；fadt 是同一
	 * 映射的借用类型视图；status 保存 ACPICA 结果；ret 跨越统一释放出口。
	 */
	struct acpi_table_header *table;
	struct acpi_table_fadt *fadt;
	acpi_status status;
	int ret = 0;

	/*
	 * FADT is required on arm64; retrieve it to check its presence
	 * and carry out revision and ACPI HW reduced compliance tests
	 */
	/*
	 * 原注释含义：arm64 强制要求 FADT；这里取得表是为了证明其存在并执行
	 * revision 与 reduced-hardware 合规检查。成功后 table 带一份必须 put
	 * 的引用；失败时没有资源需要清理。
	 */
	status = acpi_get_table(ACPI_SIG_FADT, 0, &table);
	if (ACPI_FAILURE(status)) {
		const char *msg = acpi_format_exception(status);

		pr_err("Failed to get FADT table, %s\n", msg);
		return -ENODEV;
	}

	fadt = (struct acpi_table_fadt *)table;

	/*
	 * Revision in table header is the FADT Major revision, and there
	 * is a minor revision of FADT which was introduced by ACPI 5.1,
	 * we only deal with ACPI 5.1 or newer revision to get GIC and SMP
	 * boot protocol configuration data.
	 */
	/*
	 * 原注释含义：header.revision 是 FADT 主版本，minor_revision 从 5.1
	 * 才存在；本实现只对 5.1+ 的 GIC/SMP boot protocol 数据作可靠解释。
	 * 兼容例外要求旧表至少实际提供 arm_boot_flags，否则 ret 置为 -EINVAL。
	 */
	if (table->revision < 5 ||
	   (table->revision == 5 && fadt->minor_revision < 1)) {
		pr_err(FW_BUG "Unsupported FADT revision %d.%d, should be 5.1+\n",
		       table->revision, fadt->minor_revision);

		if (!fadt->arm_boot_flags) {
			ret = -EINVAL;
			goto out;
		}
		pr_err("FADT has ARM boot flags set, assuming 5.1\n");
	}

	if (!(fadt->flags & ACPI_FADT_HW_REDUCED)) {
		/*
		 * arm64 没有 ACPI legacy fixed hardware 寄存器模型，必须使用
		 * hardware-reduced profile；失败状态在统一 out 路径释放表引用。
		 */
		pr_err("FADT not ACPI hardware reduced compliant\n");
		ret = -EINVAL;
	}

out:
	/*
	 * acpi_get_table() creates FADT table mapping that
	 * should be released after parsing and before resuming boot
	 */
	/*
	 * 原注释含义：get 创建/持有了 FADT 映射，解析结束、恢复启动前必须释放。
	 * 此标签覆盖正常、版本失败和 HW_REDUCED 失败，确保恰好一次 put。
	 */
	acpi_put_table(table);
	return ret;
}

/*
 * acpi_boot_table_init() called from setup_arch(), always.
 *	1. find RSDP and get its address, and then find XSDT
 *	2. extract all tables and checksums them all
 *	3. check ACPI FADT revision
 *	4. check ACPI FADT HW reduced flag
 *
 * We can parse ACPI boot-time tables such as MADT after
 * this function is called.
 *
 * On return ACPI is enabled if either:
 *
 * - ACPI tables are initialized and sanity checks passed
 * - acpi=force was passed in the command line and ACPI was not disabled
 *   explicitly through acpi=off command line parameter
 *
 * ACPI is disabled on function return otherwise
 */
/*
 * 补充说明：acpi_boot_table_init - 提交 arm64 的 ACPI/DT 启动选择并初始化表。
 *
 * 调用关系：setup_arch() 在 paging_init() 和 acpi_table_upgrade() 后始终调用；
 * 返回后 acpi_disabled 决定是否 unflatten DT，ACPI 子系统再继续解析 MADT 等表。
 * 无入参、无直接返回值；启动 CPU 串行，可执行早期映射但尚无普通设备并发。
 * 原注释列出的阶段是：定位 RSDP/XSDT、提取并校验表、验证 FADT 版本和
 * HW_REDUCED；通过后可解析 MADT 等 boot-time 表。
 * 成功将三个禁用开关保持为 0，并处理 FACS/SPCR/BGRT；策略拒绝或普通失败
 * 将其恢复为 1。acpi=force 只绕过初始化失败后的 disable，不能覆盖显式 off。
 */
void __init acpi_boot_table_init(void)
{
	/*
	 * Enable ACPI instead of device tree unless
	 * - ACPI has been disabled explicitly (acpi=off), or
	 * - the device tree is not empty (it has more than just a /chosen node,
	 *   and a /hypervisor node when running on Xen)
	 *   and ACPI has not been [force] enabled (acpi=on|force)
	 */
	/*
	 * 原注释完整决策：默认只有“未显式 off 且 DT 为空壳”才选 ACPI；非空 DT
	 * 表示固件已提供另一套硬件描述，除非 on/force 显式覆盖。该阶段尚未调用
	 * enable_acpi()，直接跳到 done 仍保持三个全局开关的安全初值。
	 */
	if (param_acpi_off ||
	    (!param_acpi_on && !param_acpi_force && !dt_is_stub()))
		goto done;

	/*
	 * ACPI is disabled at this point. Enable it in order to parse
	 * the ACPI tables and carry out sanity checks
	 */
	/*
	 * 原注释含义：此刻 ACPI 仍禁用；先同时清零三个开关，才能让通用表解析
	 * 和 FADT 校验工作。该发布只是“试运行”状态，失败时仍可撤销。
	 */
	enable_acpi();

	/*
	 * If ACPI tables are initialized and FADT sanity checks passed,
	 * leave ACPI enabled and carry on booting; otherwise disable ACPI
	 * on initialization error.
	 * If acpi=force was passed on the command line it forces ACPI
	 * to be enabled even if its initialization failed.
	 */
	/*
	 * 原注释含义：表初始化与 FADT 检查都成功才自然保持 enabled；逻辑 OR
	 * 具有短路语义，表目录失败时不会再访问 FADT。普通失败调用 disable_acpi()
	 * 原子式恢复三开关；force 则保留 enabled 供平台继续启动和诊断。
	 */
	if (acpi_table_init() || acpi_fadt_sanity_check()) {
		pr_err("Failed to init ACPI tables\n");
		if (!param_acpi_force)
			disable_acpi();
	}

done:
	/*
	 * 收尾按最终策略分流。禁用 ACPI 时，若早期控制台原本请求 SPCR，则改从
	 * DT /chosen 恢复 stdout；启用时才允许读取 FACS、SPCR 和 BGRT。
	 */
	if (acpi_disabled) {
		if (earlycon_acpi_spcr_enable)
			early_init_dt_scan_chosen_stdout();
	} else {
#ifdef CONFIG_HIBERNATION
		/*
		 * FACS hardware_signature 用于休眠镜像与当前固件状态匹配。FACS 只在
		 * CONFIG_HIBERNATION 下临时 get；存在时复制标量后立即 put，不把表
		 * 指针带入运行期。表缺失不改变 ACPI 启用结果。
		 */
		struct acpi_table_header *facs = NULL;
		acpi_get_table(ACPI_SIG_FACS, 1, &facs);
		if (facs) {
			swsusp_hardware_signature =
				((struct acpi_table_facs *)facs)->hardware_signature;
			acpi_put_table(facs);
		}
#endif

		/*
		 * For varying privacy and security reasons, sometimes need
		 * to completely silence the serial console output, and only
		 * enable it when needed.
		 * But there are many existing systems that depend on this
		 * behaviour, use acpi=nospcr to disable console in ACPI SPCR
		 * table as default serial console.
		 */
		/*
		 * 原注释含义：出于隐私/安全，有些系统需要默认静默串口、按需启用；
		 * 但既有平台依赖 SPCR 默认控制台行为，因此用 acpi=nospcr 选择退出。
		 * 第一个参数保留 earlycon 请求，第二个参数控制默认串口选择。
		 */
		acpi_parse_spcr(earlycon_acpi_spcr_enable,
			!param_acpi_nospcr);

		/* BGRT 仅在配置启用时解析启动图像信息，不影响 ACPI 主初始化成败。 */
		if (IS_ENABLED(CONFIG_ACPI_BGRT))
			acpi_table_parse(ACPI_SIG_BGRT, acpi_parse_bgrt);
	}
}

/*
 * __acpi_get_writethrough_mem_attribute - 为 EFI WT 属性选择 arm64 映射退化项。
 *
 * 无入参；由 ACPI/APEI 内存属性和 ioremap 路径调用。返回 pgprot_t，不建立
 * 映射、不取得资源。首次遇到时输出一次告警，随后统一退化为 Normal
 * Non-cacheable，避免仅为罕见 WT 语义占用一个有限的 MAIR 编码槽。
 */
static pgprot_t __acpi_get_writethrough_mem_attribute(void)
{
	/*
	 * Although UEFI specifies the use of Normal Write-through for
	 * EFI_MEMORY_WT, it is seldom used in practice and not implemented
	 * by most (all?) CPUs. Rather than allocate a MAIR just for this
	 * purpose, emit a warning and use Normal Non-cacheable instead.
	 */
	/*
	 * 原注释含义：UEFI 虽规定 EFI_MEMORY_WT 使用 Normal Write-through，
	 * 但实际 CPU 很少实现；与其为它单独分配 MAIR 项，本实现告警并使用
	 * Normal Non-cacheable。代价是放弃 WT 缓存收益，换取跨 CPU 可实现性。
	 */
	pr_warn_once("No MAIR allocation for EFI_MEMORY_WT; treating as Normal Non-cacheable\n");
	return __pgprot(PROT_NORMAL_NC);
}

/*
 * __acpi_get_mem_attribute - 把某物理地址的 EFI 能力位转换成 arm64 页属性。
 *
 * @addr 是待访问区域内的物理字节地址；调用者应保证 EFI memory map 已建立。
 * efi_mem_attributes() 返回该地址的能力位集合，本函数按 WB、WC、WT 的优先级
 * 选择最合适 Normal memory 属性；无任何已知缓存能力时返回最严格的
 * Device-nGnRnE。返回值只描述后续映射策略，不创建映射、无 ownership 转移。
 */
pgprot_t __acpi_get_mem_attribute(phys_addr_t addr)
{
	/*
	 * According to "Table 8 Map: EFI memory types to AArch64 memory
	 * types" of UEFI 2.5 section 2.3.6.1, each EFI memory type is
	 * mapped to a corresponding MAIR attribute encoding.
	 * The EFI memory attribute advises all possible capabilities
	 * of a memory region.
	 */
	/*
	 * 原注释含义：UEFI 2.5 2.3.6.1 表 8 规定 EFI 类型到 AArch64 MAIR
	 * 编码的映射；attribute 表示区域允许的全部能力而非唯一当前模式。
	 * 因而可能有多个 bit，下面按缓存能力从 WB 到 WT 选择。
	 */

	u64 attr;

	/* 只读取 EFI 描述，不持有 descriptor；映射生命周期尚未开始。 */
	attr = efi_mem_attributes(addr);
	if (attr & EFI_MEMORY_WB)
		return PAGE_KERNEL;
	if (attr & EFI_MEMORY_WC)
		return __pgprot(PROT_NORMAL_NC);
	if (attr & EFI_MEMORY_WT)
		return __acpi_get_writethrough_mem_attribute();
	return __pgprot(PROT_DEVICE_nGnRnE);
}

/*
 * acpi_os_ioremap - 按 EFI 内存类型为 ACPICA 建立安全的永久阶段映射。
 *
 * 调用关系：acpi_permanent_mmap 建立后，通用 ACPI OSL 的 acpi_map() 及
 * GHES/NVS 等路径调用；本函数只创建或选择底层映射。经 OSL 映射缓存调用时
 * 引用和延迟解除由 OSL 管理，直接调用者则必须遵守自身的 iounmap 配对契约。
 * @phys 是物理字节起点，@size 是字节长度；请求必须完整落在同一 EFI
 * descriptor 内，或完全不在 EFI map 中。函数可分配页表并返回 __iomem
 * 地址，调用上下文须允许 ioremap；不取得底层物理区域 ownership。
 * 返回 NULL 表示 EFI map 不可用、跨描述符、危险 RAM 映射或 ioremap 失败；
 * 成功可能复用线性映射，或以 RO/Normal/Device 属性创建新映射。
 */
void __iomem *acpi_os_ioremap(acpi_physical_address phys, acpi_size size)
{
	/*
	 * 变量地图：md 是 EFI map 当前借用描述符；region 记录包含起点的描述符；
	 * prot 从最保守 Device 属性起步，仅在 EFI/memblock 证据允许时放宽。
	 */
	efi_memory_desc_t *md, *region = NULL;
	pgprot_t prot;

	/* 没有 EFI memory map 就无法证明 RAM/MMIO 属性，拒绝猜测性映射。 */
	if (WARN_ON_ONCE(!efi_enabled(EFI_MEMMAP)))
		return NULL;

	/*
	 * 定位阶段：只接受整个半开区间 [phys, phys+size) 位于单一 descriptor。
	 * 跨区可能同时覆盖不同缓存/执行属性，单个 PTE 属性无法安全表达。
	 */
	for_each_efi_memory_desc(md) {
		u64 end = md->phys_addr + (md->num_pages << EFI_PAGE_SHIFT);

		if (phys < md->phys_addr || phys >= end)
			continue;

		if (phys + size > end) {
			pr_warn(FW_BUG "requested region covers multiple EFI memory regions\n");
			return NULL;
		}
		region = md;
		break;
	}

	/*
	 * It is fine for AML to remap regions that are not represented in the
	 * EFI memory map at all, as it only describes normal memory, and MMIO
	 * regions that require a virtual mapping to make them accessible to
	 * the EFI runtime services.
	 */
	/*
	 * 原注释含义：AML 映射完全不在 EFI map 中的区域是允许的，因为该 map
	 * 只保证列出普通内存，以及 EFI runtime 自身需要虚拟映射的 MMIO；普通
	 * 设备 MMIO 可以缺席。缺席时保持 Device-nGnRnE，禁止推测为可缓存 RAM。
	 */
	prot = __pgprot(PROT_DEVICE_nGnRnE);
	if (region) {
		/* EFI type 决定是否必须复用线性映射、只读映射或按 attribute 映射。 */
		switch (region->type) {
		case EFI_LOADER_CODE:
		case EFI_LOADER_DATA:
		case EFI_BOOT_SERVICES_CODE:
		case EFI_BOOT_SERVICES_DATA:
		case EFI_CONVENTIONAL_MEMORY:
		case EFI_PERSISTENT_MEMORY:
			if (memblock_is_map_memory(phys) ||
			    !memblock_is_region_memory(phys, size)) {
				/*
				 * 普通 RAM 已在线性映射中时再 ioremap 会制造属性别名；
				 * 区间若并非完整 memblock RAM，也不能按 override 例外放行。
				 */
				pr_warn(FW_BUG "requested region covers kernel memory @ %pa\n", &phys);
				return NULL;
			}
			/*
			 * Mapping kernel memory is permitted if the region in
			 * question is covered by a single memblock with the
			 * NOMAP attribute set: this enables the use of ACPI
			 * table overrides passed via initramfs, which are
			 * reserved in memory using arch_reserve_mem_area()
			 * below. As this particular use case only requires
			 * read access, fall through to the R/O mapping case.
			 */
			/*
			 * 原注释含义：只有同一 memblock 完整覆盖且带 NOMAP 的 kernel
			 * memory 才允许映射。这正是 initramfs ACPI override 经
			 * arch_reserve_mem_area() 保留的形态；该用例只读，因此显式
			 * fallthrough 到 RO，不能给固件表建立 writable 别名。
			 */
			fallthrough;

		case EFI_RUNTIME_SERVICES_CODE:
			/*
			 * This would be unusual, but not problematic per se,
			 * as long as we take care not to create a writable
			 * mapping for executable code.
			 */
			/*
			 * 原注释含义：AML 指向 runtime code 虽少见但并非天然错误；
			 * 关键约束是不得为 executable code 再建立 writable 映射。
			 */
			prot = PAGE_KERNEL_RO;
			break;

		case EFI_ACPI_RECLAIM_MEMORY:
			/*
			 * ACPI reclaim memory is used to pass firmware tables
			 * and other data that is intended for consumption by
			 * the OS only, which may decide it wants to reclaim
			 * that memory and use it for something else. We never
			 * do that, but we usually add it to the linear map
			 * anyway, in which case we should use the existing
			 * mapping.
			 */
			/*
			 * 原注释含义：ACPI reclaim 区承载供 OS 消费、理论上之后可回收的
			 * 表和数据；arm64 不回收它，但通常仍纳入线性映射。若已 map，
			 * 直接返回 __phys_to_virt()，避免创建不同属性的第二个别名。
			 */
			if (memblock_is_map_memory(phys))
				return (void __iomem *)__phys_to_virt(phys);
			fallthrough;

		default:
			/*
			 * 未在线性映射中的 EFI 区域按 capability 选属性；没有 WB/WC/WT
			 * 时保留 Device-nGnRnE。优先级与 __acpi_get_mem_attribute()
			 * 一致，WT 仍通过告警退化 helper。
			 */
			if (region->attribute & EFI_MEMORY_WB)
				prot = PAGE_KERNEL;
			else if (region->attribute & EFI_MEMORY_WC)
				prot = __pgprot(PROT_NORMAL_NC);
			else if (region->attribute & EFI_MEMORY_WT)
				prot = __acpi_get_writethrough_mem_attribute();
		}
	}
	/*
	 * 发布阶段：__ioremap_prot() 创建页表映射并把地址交给调用者；若来自
	 * 通用 OSL 缓存，后续由引用归零/RCU work 路径 iounmap；GHES/NVS 等
	 * 直接调用者自行配对释放。本函数不保存裸指针。
	 */
	return __ioremap_prot(phys, size, prot);
}

/*
 * Claim Synchronous External Aborts as a firmware first notification.
 *
 * Used by KVM and the arch do_sea handler.
 * @regs may be NULL when called from process context.
 */
/*
 * 补充说明：apei_claim_sea - 让 APEI/GHES 尝试认领同步外部异常（SEA）。
 *
 * 调用关系：用户态 SEA 的架构 fault handler 传入 @regs；KVM 在 IRQ enabled
 * 的进程上下文传 NULL。@regs 是异常帧借用指针，可空且不跨调用保存。
 * 函数先保存当前 CPU 的规范化 DAIF/PMR 状态，再在 ERRCTX 和 nmi_enter()
 * 包围下调用 GHES；若固件错误记录被成功消费，并且原上下文允许 IRQ，则
 * 切到 IRQ-off 进程上下文立即运行本 CPU irq_work，最后恢复原始 DAIF。
 * 返回 0 表示 GHES 已认领且延迟工作已处理；-ENOENT 表示配置关闭/无活动
 * 错误源/无记录；-EINPROGRESS 表示已排队但原上下文屏蔽 IRQ，不能立即完成；
 * 其他错误来自 GHES。副作用包括 GHES 错误记录、irq_work 和异常跟踪状态。
 */
int apei_claim_sea(struct pt_regs *regs)
{
	/*
	 * 变量地图：err 同时承载“未认领”默认值和 GHES 结果；
	 * return_to_irqs_enabled 描述返回点原本是否允许执行普通 IRQ 工作；
	 * current_flags 是同一 CPU 上最终必须恢复的 DAIF/PMR 逻辑快照。
	 */
	int err = -ENOENT;
	bool return_to_irqs_enabled;
	unsigned long current_flags;

	/* 配置关闭时没有 GHES SEA source，保持 -ENOENT 且不触碰异常状态。 */
	if (!IS_ENABLED(CONFIG_ACPI_APEI_GHES))
		return err;

	/* save_flags 只取规范化快照、不主动屏蔽；恢复必须发生在全部出口之前。 */
	current_flags = local_daif_save_flags();

	/* current_flags isn't useful here as daif doesn't tell us about pNMI */
	/*
	 * 原注释含义：current_flags 的 DAIF 位不足以单独表达优先级 NMI/PMR 状态。
	 * arch_local_save_flags() 使用通用 IRQ 语义判断普通中断是否原本开启。
	 */
	return_to_irqs_enabled = !irqs_disabled_flags(arch_local_save_flags());

	/*
	 * 真实异常入口以保存的 PSTATE 为准：当前 handler 的硬件屏蔽状态已被入口
	 * 代码改写，不能代表被 SEA 打断位置的 IRQ 状态。KVM NULL 路径沿用上值。
	 */
	if (regs)
		return_to_irqs_enabled = !regs_irqs_disabled(regs);

	/*
	 * SEA can interrupt SError, mask it and describe this as an NMI so
	 * that APEI defers the handling.
	 */
	/*
	 * 原注释含义：SEA 可能嵌套打断 SError；先切到屏蔽 A/I/F 的 ERRCTX，
	 * 再用 nmi_enter/exit 告知 lockdep、RCU 和 tracing 当前是 NMI-like
	 * 临界区。GHES 在该约束下只抓取/排队记录，不能做普通可睡眠处理。
	 */
	local_daif_restore(DAIF_ERRCTX);
	nmi_enter();
	err = ghes_notify_sea();
	nmi_exit();

	/*
	 * APEI NMI-like notifications are deferred to irq_work. Unless
	 * we interrupted irqs-masked code, we can do that now.
	 */
	/*
	 * 原注释含义：APEI 的 NMI-like 通知把后续处理排到 irq_work。只有被打断
	 * 位置原本允许 IRQ 时，当前函数才可在退出 NMI 后模拟一次 hardirq 立即
	 * 排空；否则必须保持调用者的 IRQ-off 不变量，不能擅自运行延迟工作。
	 */
	if (!err) {
		if (return_to_irqs_enabled) {
			/*
			 * DAIF_PROCCTX_NOIRQ 保持普通 IRQ/FIQ 屏蔽但退出 ERRCTX 的
			 * SError 屏蔽；__irq_enter/exit 为 irq_work 建立正确 hardirq
			 * 记账。工作完成前 err 保持 0，调用者可视为已完全认领。
			 */
			local_daif_restore(DAIF_PROCCTX_NOIRQ);
			__irq_enter();
			irq_work_run();
			__irq_exit();
		} else {
			/*
			 * 工作已由 GHES 排队，但当前不能完成。返回 -EINPROGRESS 明确
			 * 区分“固件未认领”和“已认领、等待未来 IRQ 时机”。
			 */
			pr_warn_ratelimited("APEI work queued but not completed\n");
			err = -EINPROGRESS;
		}
	}

	/* 对称恢复进入函数时的 DAIF/PMR 语义，不能把 ERRCTX 泄漏给调用者。 */
	local_daif_restore(current_flags);

	return err;
}

/*
 * arch_reserve_mem_area - 把 ACPI override 物理区标为不进入 arm64 线性映射。
 *
 * 通用 acpi_table_upgrade() 在 memblock 分配并保留复制区后调用。@addr 是
 * 物理字节起点，@size 是字节长度；本函数不取得区域 ownership，启动期由
 * memblock 协议串行调用。
 * memblock NOMAP 仍保留 struct page/Reserved 记账，并把该区从
 * memblock_is_map_memory() 的“普通可映射 RAM”视图排除，使后续
 * acpi_os_ioremap() 能识别这是完整 memblock RAM 上的受控 override 例外。
 * 当前调用点晚于 paging_init()，此 helper 只更新 memblock 元数据，不负责
 * 追溯拆除已建立的线性映射。返回 void，底层标记结果不向调用者传播。
 */
void arch_reserve_mem_area(acpi_physical_address addr, size_t size)
{
	memblock_mark_nomap(addr, size);
}

#ifdef CONFIG_ACPI_HOTPLUG_CPU
/*
 * acpi_map_cpu - 把 ACPI processor 热添加结果发布到 CPU present mask。
 *
 * ACPI processor hot-add 在持有 cpu_maps_update_begin() 与 cpus_write_lock()
 * 的写侧协议下调用。@handle/@physid/@apci_id 是接口兼容输入，本 arm64 stub
 * 不读取；@pcpu 是调用者持有的输入输出逻辑 CPU ID 指针，必须非 NULL。
 * 本实现不能从硬件 ID 重新分配逻辑号：若 *pcpu 已是负 errno，原样返回；
 * 否则设置 cpu_present 并返回 0。present 发布后由调用者继续建立 per-CPU
 * processor 对象和注册 CPU；失败前没有 mask 变化，无资源 ownership 转移。
 */
int acpi_map_cpu(acpi_handle handle, phys_cpuid_t physid, u32 apci_id,
		 int *pcpu)
{
	/* If an error code is passed in this stub can't fix it */
	/*
	 * 原注释含义：调用者传入的逻辑 CPU 已经是错误码时，这个体系结构 stub
	 * 无法修复映射。保留负值也阻止 set_cpu_present() 使用非法下标。
	 */
	if (*pcpu < 0) {
		pr_warn_once("Unable to map CPU to valid ID\n");
		return *pcpu;
	}

	/* 发布点：从此 CPU 属于 present 集合，但尚不等于 online 或可调度。 */
	set_cpu_present(*pcpu, true);
	return 0;
}
EXPORT_SYMBOL(acpi_map_cpu);

/*
 * acpi_unmap_cpu - 从 present mask 撤销一个 ACPI 热插拔 CPU。
 *
 * ACPI processor 的 hot-add 回滚和移除路径在 CPU maps 写侧锁下调用。
 * @cpu 是有效逻辑 CPU 号；函数不验证范围、不改变 possible/online mask。
 * 返回固定 0；副作用是阻止后续流程把该 CPU 视为当前存在，无资源引用转移。
 */
int acpi_unmap_cpu(int cpu)
{
	set_cpu_present(cpu, false);
	return 0;
}
EXPORT_SYMBOL(acpi_unmap_cpu);
#endif /* CONFIG_ACPI_HOTPLUG_CPU */

/*
 * acpi_get_cpu_uid - 从 MADT GICC 映射取得逻辑 CPU 的 ACPI UID。
 *
 * PPTT、NUMA 和其他 ACPI 拓扑消费者调用。@cpu 是零起始逻辑 CPU 号，必须
 * 小于 nr_cpu_ids；@uid 是非 NULL 输出指针，成功时写 GICC uid，失败保持
 * 原内容。acpi_cpu_get_madt_gicc() 返回 MADT 长期数据的借用指针，本函数
 * 不保留或释放它，可并发只读且不睡眠。
 * 返回 0、-EINVAL（cpu 越界）或 -ENODEV（该 CPU 没有 GICC 映射）。
 */
int acpi_get_cpu_uid(unsigned int cpu, u32 *uid)
{
	struct acpi_madt_generic_interrupt *gicc;

	/* 先验证逻辑索引，避免下层按无效 CPU 访问 MADT 映射数组。 */
	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	/* GICC 只借用到标量复制完成；NULL 表示 MADT 未描述该逻辑 CPU。 */
	gicc = acpi_cpu_get_madt_gicc(cpu);
	if (!gicc)
		return -ENODEV;

	*uid = gicc->uid;
	return 0;
}
EXPORT_SYMBOL_GPL(acpi_get_cpu_uid);

/*
 * get_cpu_for_acpi_id - 反向查找给定 ACPI UID 对应的逻辑 CPU。
 *
 * arm64 SRAT GICC affinity 解析用它把 proximity domain 关联到 CPU。
 * @uid 是固件提供的 32-bit ACPI processor UID。函数遍历 [0,nr_cpu_ids)，
 * 通过 acpi_get_cpu_uid() 逐项读取；单个 CPU 缺少 GICC 时跳过，首个匹配
 * 立即返回逻辑 CPU 号。无匹配返回 -EINVAL，无输出参数、无状态副作用。
 * 这是启动/拓扑慢路径的线性查找，以简单性换取 O(nr_cpu_ids) 成本。
 */
int get_cpu_for_acpi_id(u32 uid)
{
	/* cpu_uid 仅在 helper 返回 0 时有效；ret 防止比较失败路径的旧栈值。 */
	u32 cpu_uid;
	int ret;

	/* UID 在 MADT/SRAT 协议中应唯一，首个成功匹配即可建立映射。 */
	for (int cpu = 0; cpu < nr_cpu_ids; cpu++) {
		ret = acpi_get_cpu_uid(cpu, &cpu_uid);
		if (ret == 0 && uid == cpu_uid)
			return cpu;
	}

	return -EINVAL;
}
