// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ACPI 启动表管理学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 职责边界：本文件负责 Linux 启动期 ACPI 表的发现、临时持有、子表解析、
 * initrd/内建 initramfs 覆盖、固件内存保留及 MADT 诊断输出；它不负责 AML
 * 解释执行、ACPI namespace 构建，也不实现各体系结构使用 MADT/SRAT 等表
 * 建立 CPU、IRQ 或 NUMA 拓扑的具体策略。
 *
 * 主调用链：
 *   arch setup
 *     -> acpi_table_upgrade()：校验并搬运 initrd 覆盖表
 *     -> acpi_locate_initial_tables()
 *        -> acpi_initialize_tables()：从 RSDP/RSDT/XSDT 建立 initial_tables[]
 *     -> acpi_reserve_initial_tables()：阻止 memblock 覆盖固件表
 *     -> acpi_table_init_complete()
 *        -> acpi_table_initrd_scan()：安装未用于 override 的新增表
 *        -> check_multiple_madt()：确定 MADT 实例
 *   子系统 -> acpi_table_parse[_entries*]() -> handler -> acpi_put_table()
 *
 * 核心对象与生命周期：
 * - initial_tables[] 是 ACPICA 启动期表描述符存储，由
 *   acpi_initialize_tables() 填充，物理区随后由 memblock 保留；数组本身在
 *   initdata 回收前有效，表的映射/引用遵循 acpi_get_table()/put_table()。
 * - initrd 覆盖表先以 cpio_data 借用 initrd 内存，通过签名、长度、checksum
 *   和 lockdown 校验后复制到低端连续物理内存；acpi_tables_addr 发布该区域，
 *   acpi_initrd_installed 位图保证每份表只被 override 或 install 消费一次。
 * - handler 指针与表指针均是调用期间借用；解析入口在回调完成后配对
 *   acpi_put_table()，调用者不能把裸表指针带出该生命周期。
 *
 * 并发模型：发现、升级和初始保留都发生在单 CPU 启动期，依靠调用顺序而非
 * 运行期锁；acpi_initrd_installed 使用 test_and_set_bit() 原子领取表槽，
 * 明确阻止 override 与补充安装重复消费。进入 ACPICA 运行期后，表引用与映射
 * 生命周期由 ACPICA table manager 的锁和 get/put 协议负责。
 *
 * 方案权衡：启动时集中验证并复制 override 表，可在固件有缺陷时修复平台，
 * 代价是绕过固件信任边界、占用额外保留内存并 taint 内核；分块 early_memremap
 * 避免早期 fixmap 容量上限，但增加复制循环和边界处理复杂度。lockdown 模式
 * 选择安全优先，直接禁用 initrd 表覆盖。
 */
/*
 *  acpi_tables.c - ACPI Boot-Time Table Parsing
 *
 *  Copyright (C) 2001 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 */

/* Uncomment next line to get verbose printout */
/* 调试开关：解除下一行注释会让本文件的 pr_debug() 诊断在编译时生效。 */
/* #define DEBUG */
#define pr_fmt(fmt) "ACPI: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/irq.h>
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/memblock.h>
#include <linux/earlycpio.h>
#include <linux/initrd.h>
#include <linux/security.h>
#include <linux/kmemleak.h>
#include "internal.h"

#ifdef CONFIG_ACPI_CUSTOM_DSDT
#include CONFIG_ACPI_CUSTOM_DSDT_FILE
#endif

#define ACPI_MAX_TABLES		128

/*
 * MADT INTI flags 的两组 2-bit 文本映射：索引 0/1/2/3 分别对应 default、
 * 显式模式、保留值和相反模式。数组仅供诊断打印借用，字符串静态存活。
 */
static char *mps_inti_flags_polarity[] = { "dfl", "high", "res", "low" };
static char *mps_inti_flags_trigger[] = { "dfl", "edge", "res", "level" };

/*
 * ACPICA 启动期表描述符固定池。ACPI_MAX_TABLES 控制可登记上限；数组由
 * acpi_initialize_tables() 原地填充、由解析和保留路径读取，启动期串行，
 * 随 initdata 回收，不拥有固件表物理内存本身。
 */
static struct acpi_table_desc initial_tables[ACPI_MAX_TABLES] __initdata;

/*
 * 要使用的 MADT 实例号。默认 0；命令行可选择固件暴露的另一实例，
 * check_multiple_madt() 在不存在第二份 MADT 时强制恢复为 0。
 */
static int acpi_apic_instance __initdata_or_acpilib;

/*
 * Disable table checksum verification for the early stage due to the size
 * limitation of the current x86 early mapping implementation.
 */
/*
 * 补充说明：该启动期开关默认 false，由 early_param 选择性开启，并在
 * acpi_locate_initial_tables() 中一次性发布到 ACPICA 的全局 validation
 * 策略。它只规避“发现阶段映射整表”可能超过 x86 早期映射容量的问题，
 * 不改变 initrd override 路径对候选表始终执行 checksum 的安全校验。
 */
static bool acpi_verify_table_checksum __initdata_or_acpilib = false;

/*
 * acpi_table_print_madt_entry - 按 MADT 子表类型输出架构相关诊断信息。
 *
 * 调用关系：MADT 枚举/解析路径把已通过长度校验的子表交给本函数；结果只进入
 * 内核日志，不改变中断控制器或 CPU 拓扑。
 * 入参：@header 是调用期间借用的子表头，可为 NULL；具体结构体长度由上层
 * ACPI 子表遍历器保证，函数不取得引用、不释放内存。
 * 前置条件：表映射在整个调用期间有效；函数不持有本文件私有锁、不睡眠。
 * 核心过程：按 type 把共同前缀转换成对应 MADT 结构，提取 ID、地址、flags。
 * 返回与副作用：void；仅输出 debug/info/warn 日志，header 内容和 ownership
 * 均不变。NULL 静默返回，未知 type 输出告警。
 */
void acpi_table_print_madt_entry(struct acpi_subtable_header *header)
{
	/* 校验阶段：NULL 不代表可解析子表，且尚未发生任何日志或解引用。 */
	if (!header)
		return;

	/*
	 * 分派阶段：header->type 决定共同前缀之后的结构布局。转换所得 p 都是
	 * 同一固件表映射内的借用视图，只在当前 case 中有效。
	 */
	switch (header->type) {

	/* x86 本地 APIC/x2APIC CPU 接口：打印固件 UID、硬件 ID 和 enabled 位。 */
	case ACPI_MADT_TYPE_LOCAL_APIC:
		{
			struct acpi_madt_local_apic *p =
			    (struct acpi_madt_local_apic *)header;
			pr_debug("LAPIC (acpi_id[0x%02x] lapic_id[0x%02x] %s)\n",
				 p->processor_id, p->id,
				 str_enabled_disabled(p->lapic_flags & ACPI_MADT_ENABLED));
		}
		break;

	/*
	 * x2APIC 使用 32-bit APIC ID 和独立 ACPI UID；enabled 位的来源仍是
	 * lapic_flags，因此这里只改变字段布局的解码方式，不改变可用性语义。
	 */
	case ACPI_MADT_TYPE_LOCAL_X2APIC:
		{
			struct acpi_madt_local_x2apic *p =
			    (struct acpi_madt_local_x2apic *)header;
			pr_debug("X2APIC (apic_id[0x%02x] uid[0x%02x] %s)\n",
				 p->local_apic_id, p->uid,
				 str_enabled_disabled(p->lapic_flags & ACPI_MADT_ENABLED));
		}
		break;

	/* I/O APIC 描述全局中断号空间的控制器地址和起始 GSI。 */
	case ACPI_MADT_TYPE_IO_APIC:
		{
			struct acpi_madt_io_apic *p =
			    (struct acpi_madt_io_apic *)header;
			pr_debug("IOAPIC (id[0x%02x] address[0x%08x] gsi_base[%d])\n",
				 p->id, p->address, p->global_irq_base);
		}
		break;

	/*
	 * ISA source override 把传统 bus IRQ 重定向到 GSI；flags 的低两位是
	 * polarity、随后两位是 trigger，任何更高保留位都单独告警。
	 */
	case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE:
		{
			struct acpi_madt_interrupt_override *p =
			    (struct acpi_madt_interrupt_override *)header;
			pr_info("INT_SRC_OVR (bus %d bus_irq %d global_irq %d %s %s)\n",
				p->bus, p->source_irq, p->global_irq,
				mps_inti_flags_polarity[p->inti_flags & ACPI_MADT_POLARITY_MASK],
				mps_inti_flags_trigger[(p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2]);
			if (p->inti_flags  &
			    ~(ACPI_MADT_POLARITY_MASK | ACPI_MADT_TRIGGER_MASK))
				pr_info("INT_SRC_OVR unexpected reserved flags: 0x%x\n",
					p->inti_flags  &
					~(ACPI_MADT_POLARITY_MASK | ACPI_MADT_TRIGGER_MASK));
		}
		break;

	/* NMI source 与本地 APIC/x2APIC NMI 都只解码触发、电平和目标 LINT。 */
	case ACPI_MADT_TYPE_NMI_SOURCE:
		{
			struct acpi_madt_nmi_source *p =
			    (struct acpi_madt_nmi_source *)header;
			pr_info("NMI_SRC (%s %s global_irq %d)\n",
				mps_inti_flags_polarity[p->inti_flags & ACPI_MADT_POLARITY_MASK],
				mps_inti_flags_trigger[(p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2],
				p->global_irq);
		}
		break;

	/* 本地 APIC NMI 把某个 ACPI processor ID 的 NMI 接到指定 LINT 引脚。 */
	case ACPI_MADT_TYPE_LOCAL_APIC_NMI:
		{
			struct acpi_madt_local_apic_nmi *p =
			    (struct acpi_madt_local_apic_nmi *)header;
			pr_info("LAPIC_NMI (acpi_id[0x%02x] %s %s lint[0x%x])\n",
				p->processor_id,
				mps_inti_flags_polarity[p->inti_flags & ACPI_MADT_POLARITY_MASK	],
				mps_inti_flags_trigger[(p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2],
				p->lint);
		}
		break;

	/*
	 * x2APIC NMI 以 32-bit UID 标识目标。先把 flags 两个位域保存为有界索引，
	 * 再访问四项文本表，避免冗长表达式掩盖 polarity/trigger 的独立含义。
	 */
	case ACPI_MADT_TYPE_LOCAL_X2APIC_NMI:
		{
			u16 polarity, trigger;
			struct acpi_madt_local_x2apic_nmi *p =
			    (struct acpi_madt_local_x2apic_nmi *)header;

			polarity = p->inti_flags & ACPI_MADT_POLARITY_MASK;
			trigger = (p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2;

			pr_info("X2APIC_NMI (uid[0x%02x] %s %s lint[0x%x])\n",
				p->uid,
				mps_inti_flags_polarity[polarity],
				mps_inti_flags_trigger[trigger],
				p->lint);
		}
		break;

	/* 64-bit LAPIC 地址覆盖旧 MADT 头中的 32-bit 默认地址。 */
	case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE:
		{
			struct acpi_madt_local_apic_override *p =
			    (struct acpi_madt_local_apic_override *)header;
			pr_info("LAPIC_ADDR_OVR (address[0x%llx])\n",
				p->address);
		}
		break;

	/* IA-64 SAPIC 兼容条目沿用相同的只读诊断分派。 */
	case ACPI_MADT_TYPE_IO_SAPIC:
		{
			struct acpi_madt_io_sapic *p =
			    (struct acpi_madt_io_sapic *)header;
			pr_debug("IOSAPIC (id[0x%x] address[%p] gsi_base[%d])\n",
				 p->id, (void *)(unsigned long)p->address,
				 p->global_irq_base);
		}
		break;

	/* Local SAPIC 额外携带 EID；enabled 仍由 lapic_flags 决定。 */
	case ACPI_MADT_TYPE_LOCAL_SAPIC:
		{
			struct acpi_madt_local_sapic *p =
			    (struct acpi_madt_local_sapic *)header;
			pr_debug("LSAPIC (acpi_id[0x%02x] lsapic_id[0x%02x] lsapic_eid[0x%02x] %s)\n",
				 p->processor_id, p->id, p->eid,
				 str_enabled_disabled(p->lapic_flags & ACPI_MADT_ENABLED));
		}
		break;

	/*
	 * 平台中断源条目同时携带 SAPIC 目标、向量和全局 IRQ；这里只完整打印
	 * 固件声明，真正的中断路由建立仍由体系结构解析回调负责。
	 */
	case ACPI_MADT_TYPE_INTERRUPT_SOURCE:
		{
			struct acpi_madt_interrupt_source *p =
			    (struct acpi_madt_interrupt_source *)header;
			pr_info("PLAT_INT_SRC (%s %s type[0x%x] id[0x%04x] eid[0x%x] iosapic_vector[0x%x] global_irq[0x%x]\n",
				mps_inti_flags_polarity[p->inti_flags & ACPI_MADT_POLARITY_MASK],
				mps_inti_flags_trigger[(p->inti_flags & ACPI_MADT_TRIGGER_MASK) >> 2],
				p->type, p->id, p->eid, p->io_sapic_vector,
				p->global_irq);
		}
		break;

	/* ARM GIC CPU interface 与 distributor 分别描述每 CPU 接口和全局控制器。 */
	case ACPI_MADT_TYPE_GENERIC_INTERRUPT:
		{
			struct acpi_madt_generic_interrupt *p =
				(struct acpi_madt_generic_interrupt *)header;
			pr_debug("GICC (acpi_id[0x%04x] address[%llx] MPIDR[0x%llx] %s)\n",
				 p->uid, p->base_address,
				 p->arm_mpidr,
				 str_enabled_disabled(p->flags & ACPI_MADT_ENABLED));

		}
		break;

	/* distributor 条目定义 GIC 全局控制器的 MMIO 基址和 GSI 起点。 */
	case ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR:
		{
			struct acpi_madt_generic_distributor *p =
				(struct acpi_madt_generic_distributor *)header;
			pr_debug("GIC Distributor (gic_id[0x%04x] address[%llx] gsi_base[%d])\n",
				 p->gic_id, p->base_address,
				 p->global_irq_base);
		}
		break;

	/*
	 * 多处理器唤醒 mailbox 的 reset_vector 仅在 V1+ 条目有效；旧版本保持 0，
	 * 避免读取版本定义之外的尾部字段。
	 */
	case ACPI_MADT_TYPE_MULTIPROC_WAKEUP:
		{
			struct acpi_madt_multiproc_wakeup *p =
				(struct acpi_madt_multiproc_wakeup *)header;
			u64 reset_vector = 0;

			if (p->version >= ACPI_MADT_MP_WAKEUP_VERSION_V1)
				reset_vector = p->reset_vector;

			pr_debug("MP Wakeup (version[%d], mailbox[%#llx], reset[%#llx])\n",
				 p->version, p->mailbox_address, reset_vector);
		}
		break;

	/* LoongArch CORE PIC 与 RISC-V INTC 条目打印各自 CPU/hart 标识及 enabled。 */
	case ACPI_MADT_TYPE_CORE_PIC:
		{
			struct acpi_madt_core_pic *p = (struct acpi_madt_core_pic *)header;

			pr_debug("CORE PIC (processor_id[0x%02x] core_id[0x%02x] %s)\n",
				 p->processor_id, p->core_id,
				 str_enabled_disabled(p->flags & ACPI_MADT_ENABLED));
		}
		break;

	/*
	 * RINTC 用 hart_id 关联 RISC-V hart，以 ACPI UID 关联设备命名空间 CPU；
	 * enabled 位决定该处理器接口是否应进入后续体系结构初始化候选集。
	 */
	case ACPI_MADT_TYPE_RINTC:
		{
			struct acpi_madt_rintc *p = (struct acpi_madt_rintc *)header;

			pr_debug("RISC-V INTC (acpi_uid[0x%04x] hart_id[0x%llx] %s)\n",
				 p->uid, p->hart_id,
				 str_enabled_disabled(p->flags & ACPI_MADT_ENABLED));
		}
		break;

	default:
		/* 未识别类型不阻止后续条目解析，只留下固件兼容性告警。 */
		pr_warn("Found unsupported MADT entry (type = 0x%x)\n",
			header->type);
		break;
	}
}

/*
 * acpi_table_parse_entries_array - 持有目标表并按规则数组同步遍历其子表。
 *
 * 调用关系：体系结构 MADT/SRAT 和 CEDT 等解析入口调用；内部借助 ACPICA
 * get/put 稳定表映射，再把实际边界检查和 handler 分派交给
 * acpi_parse_entries_array()。
 * 入参：@id 为 4 字节签名字符串借用，必须非 NULL；@table_size 是固定表头
 * 大小（字节），必须非零；@proc 指向 @proc_num 个借用处理规则；@max_entries
 * 为最多处理子表数，0 表示通用解析器的无限制语义。
 * 前置条件：ACPI 未禁用；调用者保证 proc/handler 在同步调用期间有效。
 * 返回与副作用：成功返回已处理条目数；-ENODEV 表示 ACPI 禁用或表不存在，
 * -EINVAL 表示 id/table_size 无效，其他负值来自通用解析器。函数不保留表
 * 或 proc ownership；handler 的业务副作用由调用者定义。
 */
int __init_or_acpilib acpi_table_parse_entries_array(
	char *id, unsigned long table_size, struct acpi_subtable_proc *proc,
	int proc_num, unsigned int max_entries)
{
	/*
	 * 变量地图：
	 *   table_header  acpi_get_table() 返回的持有表引用，所有出口前必须 put。
	 *   instance      MADT 使用命令行选择实例，其他签名固定取第一份。
	 *   count         通用子表解析器返回的处理数量或负 errno。
	 */
	struct acpi_table_header *table_header = NULL;
	int count;
	u32 instance = 0;

	/*
	 * 入口校验阶段：禁用 ACPI 与缺少目标表都归入 -ENODEV；空签名或零表头
	 * 大小属于调用契约错误，返回 -EINVAL。校验完成前尚未取得任何表引用。
	 * MADT 是唯一允许由 early_param 选择实例的签名，其他表保持 instance 0。
	 */
	if (acpi_disabled)
		return -ENODEV;

	if (!id)
		return -EINVAL;

	if (!table_size)
		return -EINVAL;

	if (!strncmp(id, ACPI_SIG_MADT, 4))
		instance = acpi_apic_instance;

	/*
	 * 获取阶段：acpi_get_table() 成功后 table_header 携带一次早期表引用，
	 * 即使解析器失败也必须由本函数 acpi_put_table() 配对释放。
	 */
	acpi_get_table(id, instance, &table_header);
	if (!table_header) {
		pr_debug("%4.4s not present\n", id);
		return -ENODEV;
	}

	/*
	 * 执行阶段：表映射和引用在全部同步 handler 返回前保持有效。通用解析器
	 * 负责检查子表长度、推进边界并按 proc.id 选择回调。
	 */
	count = acpi_parse_entries_array(id, table_size,
					 (union fw_table_header *)table_header,
					 0, proc, proc_num, max_entries);

	/* 释放阶段：归还 get 引用；调用者只能使用 handler 已复制/持有的结果。 */
	acpi_put_table(table_header);
	return count;
}

/*
 * __acpi_table_parse_entries - 单条 proc 规则到数组解析入口的内部适配层。
 *
 * @id/@table_size/@entry_id/@max_entries 均为纯输入；@handler 与
 * @handler_arg 二选一，可为空；@arg 是 handler_arg 同步调用期间的借用
 * 上下文，不转移 ownership。init/acpilib 上下文可调用，睡眠能力取决于
 * 下层表映射和 handler。
 * 返回：原样返回 acpi_table_parse_entries_array() 的计数或负 errno。
 * 副作用：仅栈上构造 proc，实际表引用与回调副作用由下层管理。
 */
static int __init_or_acpilib __acpi_table_parse_entries(
	char *id, unsigned long table_size, int entry_id,
	acpi_tbl_entry_handler handler, acpi_tbl_entry_handler_arg handler_arg,
	void *arg, unsigned int max_entries)
{
	struct acpi_subtable_proc proc = {
		.id		= entry_id,
		.handler	= handler,
		.handler_arg	= handler_arg,
		.arg		= arg,
	};

	return acpi_table_parse_entries_array(id, table_size, &proc, 1,
						max_entries);
}

/*
 * acpi_table_parse_cedt - 解析指定类型 CEDT 子表的带上下文回调包装。
 *
 * @id 选择 CEDT 子表类型；@handler_arg 为非 NULL 的同步回调；@arg 是调用
 * 期间借用的上下文，不转移 ownership。本层固定 CEDT 签名和表头大小，并
 * 使用 0 表示不限制处理条目数。
 * 返回处理数量或下层负 errno；除 handler 自身行为外无额外副作用。
 */
int __init_or_acpilib
acpi_table_parse_cedt(enum acpi_cedt_type id,
		      acpi_tbl_entry_handler_arg handler_arg, void *arg)
{
	return __acpi_table_parse_entries(ACPI_SIG_CEDT,
					  sizeof(struct acpi_table_cedt), id,
					  NULL, handler_arg, arg, 0);
}
EXPORT_SYMBOL_ACPI_LIB(acpi_table_parse_cedt);

/*
 * acpi_table_parse_entries - 解析任意 ACPI 表中指定类型子表的无上下文包装。
 *
 * @id 是 ACPI 表签名借用，@table_size 是固定表头大小，@entry_id 选择子表
 * 类型，@handler 是同步借用回调，@max_entries 是处理上限。参数与表引用均
 * 不被本函数长期持有。
 * 返回处理数量或下层负 errno；除 handler 自身行为外无额外副作用。
 */
int __init acpi_table_parse_entries(char *id, unsigned long table_size,
				    int entry_id,
				    acpi_tbl_entry_handler handler,
				    unsigned int max_entries)
{
	return __acpi_table_parse_entries(id, table_size, entry_id, handler,
					  NULL, NULL, max_entries);
}

/*
 * acpi_table_parse_madt - 固定 MADT 签名和头大小的子表解析入口。
 *
 * @id 是 MADT 子表类型；@handler 是同步借用的体系结构回调；@max_entries
 * 是处理上限。函数固定 MADT 签名和表头大小，并通过下层使用命令行所选
 * MADT 实例，不保留 handler 或表引用。
 * 返回处理数量或下层负 errno；除 handler 自身行为外无额外副作用。
 */
int __init acpi_table_parse_madt(enum acpi_madt_type id,
		      acpi_tbl_entry_handler handler, unsigned int max_entries)
{
	return acpi_table_parse_entries(ACPI_SIG_MADT,
					    sizeof(struct acpi_table_madt), id,
					    handler, max_entries);
}

/**
 * acpi_table_parse - find table with @id, run @handler on it
 * @id: table id to find
 * @handler: handler to run
 *
 * Scan the ACPI System Descriptor Table (STD) for a table matching @id,
 * run @handler on it.
 *
 * Return 0 if table found, -errno if not.
 */
/*
 * 补充说明：@id 是非 NULL 的 4 字节表签名借用；@handler 是非 NULL 同步回调，
 * 收到的 table 指针只在回调期间由本函数的 get 引用保证有效，不能无引用保存。
 * 启动期调用，无本文件私有锁；handler 是否睡眠由其自身契约决定。
 * 返回 0 表示已调用 handler；-EINVAL 表示参数无效，-ENODEV 表示 ACPI 禁用
 * 或表不存在。所有成功获取路径都在返回前 acpi_put_table()，不泄漏引用。
 */
int __init acpi_table_parse(char *id, acpi_tbl_table_handler handler)
{
	struct acpi_table_header *table = NULL;

	if (acpi_disabled)
		return -ENODEV;

	if (!id || !handler)
		return -EINVAL;

	/* 查找阶段：MADT 使用可选实例号，其他表固定请求 ACPICA 的 instance 0。 */
	if (strncmp(id, ACPI_SIG_MADT, 4) == 0)
		acpi_get_table(id, acpi_apic_instance, &table);
	else
		acpi_get_table(id, 0, &table);

	if (table) {
		/*
		 * 执行与释放：handler 在持有表引用时同步完成；随后立即 put，
		 * 因而 handler 若需长期使用内容必须自行复制或取得合适引用。
		 */
		handler(table);
		acpi_put_table(table);
		return 0;
	} else
		return -ENODEV;
}

/*
 * The BIOS is supposed to supply a single APIC/MADT,
 * but some report two.  Provide a knob to use either.
 * (don't you wish instance 0 and 1 were not the same?)
 */
/*
 * check_multiple_madt - 检测固件是否提供第二个可选择的 MADT。
 *
 * 入参：无。启动期串行、无本文件私有锁。函数请求 ACPICA instance 2：
 * 命中说明有多份 MADT，保留命令行选择并输出切换建议；未命中则强制实例 0。
 * 返回 void；成功获取的 table 引用在分支内 put，副作用是日志及可能更新
 * acpi_apic_instance，无可恢复失败资源。
 */
static void __init check_multiple_madt(void)
{
	struct acpi_table_header *table = NULL;

	/* get 返回的 table 是持有引用，不论仅用于探测也必须在命中分支归还。 */
	acpi_get_table(ACPI_SIG_MADT, 2, &table);
	if (table) {
		pr_warn("BIOS bug: multiple APIC/MADT found, using %d\n",
			acpi_apic_instance);
		pr_warn("If \"acpi_apic_instance=%d\" works better, "
			"notify linux-acpi@vger.kernel.org\n",
			acpi_apic_instance ? 0 : 2);
		acpi_put_table(table);

	} else
		acpi_apic_instance = 0;

	return;
}

/*
 * acpi_table_taint - 为不受信任的 ACPI 表覆盖设置全局 taint。
 *
 * @table 是被覆盖原表的非 NULL 借用指针，仅读取签名/OEM ID；调用者仍持有
 * ownership。函数可在 ACPICA 安装 override 的同步路径调用，不睡眠。
 * 返回 void；副作用是输出安全告警、设置 TAINT_OVERRIDDEN_ACPI_TABLE，
 * 并声明 lockdep 结论不再可靠。
 */
static void acpi_table_taint(struct acpi_table_header *table)
{
	pr_warn("Override [%4.4s-%8.8s], this is unsafe: tainting kernel\n",
		table->signature, table->oem_table_id);
	add_taint(TAINT_OVERRIDDEN_ACPI_TABLE, LOCKDEP_NOW_UNRELIABLE);
}

#ifdef CONFIG_ACPI_TABLE_UPGRADE
/*
 * acpi_tables_addr 是通过 memblock 分配、保存所有已校验 override 表的连续
 * 物理区起点，零表示尚未发布；all_tables_size 是该区有效字节总数。二者由
 * acpi_table_upgrade() 写，override/scan 路径只读，启动期串行且长期物理区
 * 被保留，变量本身遵循相应 init 生命周期。
 */
static u64 acpi_tables_addr;
static int all_tables_size;

/* Copied from acpica/tbutils.c:acpi_tb_checksum() */
/*
 * 补充说明：acpi_table_checksum - 计算 ACPI 规定的 8-bit 累加校验和。
 *
 * @buffer 是至少 @length 字节的借用输入缓冲区，非 NULL；@length 单位字节，
 * 允许为 0。函数只读、不睡眠、无副作用；返回所有字节模 256 的和，合法
 * ACPI 表返回 0，非零表示 checksum 失败。指针递增只改变局部副本。
 */
static u8 __init acpi_table_checksum(u8 *buffer, u32 length)
{
	u8 sum = 0;
	u8 *end = buffer + length;

	/* 扫描不取得缓冲区 ownership，end 固定为输入半开区间上界。 */
	while (buffer < end)
		sum = (u8) (sum + *(buffer++));
	return sum;
}

/* All but ACPI_SIG_RSDP and ACPI_SIG_FACS: */
/*
 * 补充说明：table_sigs 是允许从 initrd 覆盖/安装的普通 ACPI 表签名白名单。
 * RSDP 是发现入口、FACS 没有标准 ACPI header/checksum，故刻意排除。数组在
 * init 阶段只读，__nonstring_array 表示每项固定 4 字节而非 NUL 结尾字符串。
 */
static const char table_sigs[][ACPI_NAMESEG_SIZE] __nonstring_array __initconst = {
	ACPI_SIG_BERT, ACPI_SIG_BGRT, ACPI_SIG_CPEP, ACPI_SIG_ECDT,
	ACPI_SIG_EINJ, ACPI_SIG_ERST, ACPI_SIG_HEST, ACPI_SIG_MADT,
	ACPI_SIG_MSCT, ACPI_SIG_SBST, ACPI_SIG_SLIT, ACPI_SIG_SRAT,
	ACPI_SIG_ASF,  ACPI_SIG_BOOT, ACPI_SIG_DBGP, ACPI_SIG_DMAR,
	ACPI_SIG_HPET, ACPI_SIG_IBFT, ACPI_SIG_IVRS, ACPI_SIG_MCFG,
	ACPI_SIG_MCHI, ACPI_SIG_SLIC, ACPI_SIG_SPCR, ACPI_SIG_SPMI,
	ACPI_SIG_TCPA, ACPI_SIG_UEFI, ACPI_SIG_WAET, ACPI_SIG_WDAT,
	ACPI_SIG_WDDT, ACPI_SIG_WDRT, ACPI_SIG_DSDT, ACPI_SIG_FADT,
	ACPI_SIG_PSDT, ACPI_SIG_RSDT, ACPI_SIG_XSDT, ACPI_SIG_SSDT,
	ACPI_SIG_IORT, ACPI_SIG_NFIT, ACPI_SIG_HMAT, ACPI_SIG_PPTT,
	ACPI_SIG_NHLT, ACPI_SIG_AEST, ACPI_SIG_CEDT, ACPI_SIG_AGDI,
	ACPI_SIG_NBFT, ACPI_SIG_SWFT, ACPI_SIG_MPAM};

#define ACPI_HEADER_SIZE sizeof(struct acpi_table_header)

#define NR_ACPI_INITRD_TABLES 64
/*
 * acpi_initrd_files[] 在扫描 cpio 时暂存最多 64 个已校验文件的借用 data/size；
 * 数据最初仍属于 initrd。acpi_initrd_installed 位图按同一索引记录“已作为
 * override 或新增表消费”，test_and_set_bit() 使领取动作不可重复。
 */
static struct cpio_data __initdata acpi_initrd_files[NR_ACPI_INITRD_TABLES];
static DECLARE_BITMAP(acpi_initrd_installed, NR_ACPI_INITRD_TABLES);

/*
 * 一次 early_memremap 最多使用 NR_FIX_BTMAPS 个页槽；该字节上限连同首地址
 * 页内偏移共同约束每轮复制长度，防止耗尽启动期 fixmap 窗口。
 */
#define MAP_CHUNK_SIZE   (NR_FIX_BTMAPS << PAGE_SHIFT)

/*
 * acpi_table_upgrade - 校验 initrd ACPI 文件并搬到受保护的低端物理内存。
 *
 * 调用关系：体系结构 setup 在 acpi_initialize_tables() 之前调用；发布的
 * acpi_tables_addr 随后被 ACPICA physical override hook 和补充安装扫描使用。
 * 入参/出参：无显式参数和输出参数。
 * 前置条件：initramfs/initrd 地址已知、memblock 与 early_memremap 可用；
 * 启动 CPU 串行，无锁。security_locked_down() 在任何内存发布前执行。
 * 核心过程：选择 cpio 来源，逐文件校验 header/签名/长度/checksum，计算总长；
 * lockdown 通过后分配并保留连续物理区，再按 fixmap 容量分块复制。
 * 返回与副作用：void；无文件、无合法表、lockdown 或分配失败均安全返回。
 * 成功时写 all_tables_size/acpi_tables_addr/acpi_initrd_files，永久保留物理区；
 * 表尚未标为 installed，后续 override/scan 决定其最终用途。
 */
void __init acpi_table_upgrade(void)
{
	/*
	 * 变量地图：
	 *   data/size/offset 当前尚未扫描的 cpio 字节窗口及查找推进量。
	 *   file/table      当前 cpio 文件和其 ACPI header 借用视图。
	 *   table_nr        已接受文件数；total_offset 是复制目标内的字节偏移。
	 *   sig/no          白名单索引与文件槽索引。
	 */
	void *data;
	size_t size;
	int sig, no, table_nr = 0, total_offset = 0;
	long offset = 0;
	struct acpi_table_header *table;
	char cpio_path[32] = "kernel/firmware/acpi/";
	struct cpio_data file;

	/*
	 * 来源选择：内建 initramfs 和外部 initrd 生命周期不同，但都只作为
	 * 扫描期借用缓冲区；未配置/空来源在任何全局状态发布前返回。
	 */
	if (IS_ENABLED(CONFIG_ACPI_TABLE_OVERRIDE_VIA_BUILTIN_INITRD)) {
		data = __initramfs_start;
		size = __initramfs_size;
	} else {
		data = (void *)initrd_start;
		size = initrd_end - initrd_start;
	}

	if (data == NULL || size == 0)
		return;

	/*
	 * 发现与校验阶段：find_cpio_data() 返回当前路径下一个文件，并通过
	 * offset 告知下次扫描起点。只有四项校验全部通过才计入总大小和文件数组。
	 */
	for (no = 0; no < NR_ACPI_INITRD_TABLES; no++) {
		file = find_cpio_data(cpio_path, data, size, &offset);
		if (!file.data)
			break;

		data += offset;
		size -= offset;

		/* header 太短时不能安全读取 signature/length，拒绝但继续后续文件。 */
		if (file.size < sizeof(struct acpi_table_header)) {
			pr_err("ACPI OVERRIDE: Table smaller than ACPI header [%s%s]\n",
				cpio_path, file.name);
			continue;
		}

		table = file.data;

		/* 固定 4 字节比较，不要求固件 signature 具有字符串终止符。 */
		for (sig = 0; sig < ARRAY_SIZE(table_sigs); sig++)
			if (!memcmp(table->signature, table_sigs[sig], 4))
				break;

		if (sig >= ARRAY_SIZE(table_sigs)) {
			pr_err("ACPI OVERRIDE: Unknown signature [%s%s]\n",
				cpio_path, file.name);
			continue;
		}
		/*
		 * 文件大小必须与 header 声明完全相等；既拒绝截断，也拒绝尾随数据，
		 * 随后的 checksum 才不会越过 cpio 文件边界。
		 */
		if (file.size != table->length) {
			pr_err("ACPI OVERRIDE: File length does not match table length [%s%s]\n",
				cpio_path, file.name);
			continue;
		}
		if (acpi_table_checksum(file.data, table->length)) {
			pr_err("ACPI OVERRIDE: Bad table checksum [%s%s]\n",
				cpio_path, file.name);
			continue;
		}

		pr_info("%4.4s ACPI table found in initrd [%s%s][0x%x]\n",
			table->signature, cpio_path, file.name, table->length);

		/*
		 * 接受阶段只保存 initrd 借用地址并累计尺寸，尚未让 ACPICA 可见；
		 * table_nr 只对合法文件递增，因此数组连续无空洞。
		 */
		all_tables_size += table->length;
		acpi_initrd_files[table_nr].data = file.data;
		acpi_initrd_files[table_nr].size = file.size;
		table_nr++;
	}
	if (table_nr == 0)
		return;

	/*
	 * 安全边界：固件表覆盖能改变硬件描述与 AML 信任根。lockdown 拒绝时
	 * 不分配/发布新物理区，先前暂存的 initrd 借用会随启动内存正常回收。
	 */
	if (security_locked_down(LOCKDOWN_ACPI_TABLES)) {
		pr_notice("kernel is locked down, ignoring table override\n");
		return;
	}

	/*
	 * 资源获取：在平台允许的最大物理地址以下按页对齐分配连续区。零返回
	 * 表示失败，此时 acpi_tables_addr 仍是“未发布”哨兵，后续 hook 无操作。
	 */
	acpi_tables_addr =
		memblock_phys_alloc_range(all_tables_size, PAGE_SIZE,
					  0, ACPI_TABLE_UPGRADE_MAX_PHYS);
	if (!acpi_tables_addr) {
		WARN_ON(1);
		return;
	}
	/*
	 * Only calling e820_add_reserve does not work and the
	 * tables are invalid (memory got used) later.
	 * memblock_reserve works as expected and the tables won't get modified.
	 * But it's not enough on X86 because ioremap will
	 * complain later (used by acpi_os_map_memory) that the pages
	 * that should get mapped are not marked "reserved".
	 * Both memblock_reserve and e820__range_add (via arch_reserve_mem_area)
	 * works fine.
	 */
	arch_reserve_mem_area(acpi_tables_addr, all_tables_size);
	/*
	 * arch_reserve_mem_area() 补足体系结构资源图保留；与 memblock 物理占用
	 * 共同保证页不被分配，也避免 x86 ioremap 把目标判断为普通 RAM 冲突。
	 */

	kmemleak_ignore_phys(acpi_tables_addr);

	/*
	 * early_ioremap only can remap 256k one time. If we map all
	 * tables one time, we will hit the limit. Need to map chunks
	 * one by one during copying the same as that in relocate_initrd().
	 */
	/*
	 * 搬运阶段：每张表在目标区紧密排列。源仍是 initrd 借用地址；目标物理
	 * 区归 ACPI override 机制长期持有，不在本函数末尾释放。
	 */
	for (no = 0; no < table_nr; no++) {
		unsigned char *src_p = acpi_initrd_files[no].data;
		phys_addr_t size = acpi_initrd_files[no].size;
		phys_addr_t dest_addr = acpi_tables_addr + total_offset;
		phys_addr_t slop, clen;
		char *dest_p;

		total_offset += size;

		/*
		 * early_ioremap fixmap 槽有限，每轮映射不超过 MAP_CHUNK_SIZE，并把
		 * 首地址页内 slop 计入映射长度。unmap 后再推进源/目标/剩余长度，
		 * 保证任一时刻只持有一个临时映射。
		 */
		while (size) {
			slop = dest_addr & ~PAGE_MASK;
			clen = size;
			if (clen > MAP_CHUNK_SIZE - slop)
				clen = MAP_CHUNK_SIZE - slop;
			dest_p = early_memremap(dest_addr & PAGE_MASK,
						clen + slop);
			memcpy(dest_p + slop, src_p, clen);
			early_memunmap(dest_p, clen + slop);
			src_p += clen;
			dest_addr += clen;
			size -= clen;
		}
	}
}

/*
 * acpi_table_initrd_override - 为 ACPICA 正在安装的固件表寻找更高版本替代表。
 *
 * @existing_table 是 ACPICA 持有的原表借用指针，非 NULL；@address/@length 是
 * 非 NULL 输出参数，入口内容被清零，成功匹配时分别写替代表物理地址和字节数。
 * 函数扫描 acpi_tables_addr 中的 header 映射，不取得原表 ownership；启动
 * 安装期调用，临时映射可能使用早期映射设施。
 * 返回始终 AE_OK：无升级、格式损坏或版本不新都以零输出表示；成功要求
 * signature、OEM ID/table ID 相同、索引未被消费且 revision 更高。
 * 副作用：test_and_set_bit() 原子领取匹配索引，成功输出指向长期保留物理区。
 */
static acpi_status
acpi_table_initrd_override(struct acpi_table_header *existing_table,
			   acpi_physical_address *address, u32 *length)
{
	/*
	 * 变量地图：table_offset 是连续物理区字节偏移；table_index 与 installed
	 * 位图一一对应；table_length 在解除 header 临时映射后仍保存推进步长。
	 */
	int table_offset = 0;
	int table_index = 0;
	struct acpi_table_header *table;
	u32 table_length;

	/* 默认结果先发布为“无 override”，所有早退路径都保持这组安全输出。 */
	*length = 0;
	*address = 0;
	if (!acpi_tables_addr)
		return AE_OK;

	/*
	 * 扫描阶段只在剩余空间容得下完整 header 时映射。每轮先映射 header，
	 * 验证其 length 没越过总区间，再进行身份/版本选择。
	 */
	while (table_offset + ACPI_HEADER_SIZE <= all_tables_size) {
		table = acpi_os_map_memory(acpi_tables_addr + table_offset,
					   ACPI_HEADER_SIZE);
		if (table_offset + table->length > all_tables_size) {
			/*
			 * 搬运区自身应已校验；越界说明内部状态损坏。先解除映射，再
			 * WARN 并保持零输出，避免用损坏 length 继续扫描。
			 */
			acpi_os_unmap_memory(table, ACPI_HEADER_SIZE);
			WARN_ON(1);
			return AE_OK;
		}

		table_length = table->length;

		/* Only override tables matched */
		/*
		 * 身份匹配要求签名和两组 OEM 标识全部一致；不匹配表解除映射后
		 * 进入公共推进标签，不会被错误标为 installed。
		 */
		if (memcmp(existing_table->signature, table->signature, 4) ||
		    memcmp(table->oem_id, existing_table->oem_id,
			   ACPI_OEM_ID_SIZE) ||
		    memcmp(table->oem_table_id, existing_table->oem_table_id,
			   ACPI_OEM_TABLE_ID_SIZE)) {
			acpi_os_unmap_memory(table, ACPI_HEADER_SIZE);
			goto next_table;
		}
		/*
		 * Mark the table to avoid being used in
		 * acpi_table_initrd_scan() and check the revision.
		 */
		/*
		 * 领取与版本校验：原子置位确保同一 initrd 表只被一个路径消费。
		 * 已领取或 revision 不更新都拒绝；注意当前实现先置位再比较版本，
		 * 因而“版本不新”的候选也不会再被后续 install 扫描使用。
		 */
		if (test_and_set_bit(table_index, acpi_initrd_installed) ||
		    existing_table->oem_revision >= table->oem_revision) {
			acpi_os_unmap_memory(table, ACPI_HEADER_SIZE);
			goto next_table;
		}

		/*
		 * 成功发布：物理区已长期保留，输出无需额外引用；ACPICA 随后映射
		 * address/length。发布完解除仅用于读取 header 的临时映射。
		 */
		*length = table_length;
		*address = acpi_tables_addr + table_offset;
		pr_info("Table Upgrade: override [%4.4s-%6.6s-%8.8s]\n",
			table->signature, table->oem_id,
			table->oem_table_id);
		acpi_os_unmap_memory(table, ACPI_HEADER_SIZE);
		break;

next_table:
		/* 当前 header 已解除映射，使用保存的 table_length 安全推进下一项。 */
		table_offset += table_length;
		table_index++;
	}
	return AE_OK;
}

/*
 * acpi_table_initrd_scan - 安装未被 override 路径消费的 initrd ACPI 表。
 *
 * 无入参/返回值。启动期在 ACPICA 初始表建立后串行调用；扫描长期保留的
 * acpi_tables_addr，对每个未领取表原子置位后调用 acpi_install_physical_table()。
 * RSDT/XSDT 仅允许作为 root override，不能作为新增普通表安装。损坏长度
 * WARN 后终止扫描；安装状态由 ACPICA 管理，本函数不持有返回引用。
 */
static void __init acpi_table_initrd_scan(void)
{
	int table_offset = 0;
	int table_index = 0;
	u32 table_length;
	struct acpi_table_header *table;

	if (!acpi_tables_addr)
		return;

	/* 迭代和越界保护与 override 路径相同，每轮只临时映射 header。 */
	while (table_offset + ACPI_HEADER_SIZE <= all_tables_size) {
		table = acpi_os_map_memory(acpi_tables_addr + table_offset,
					   ACPI_HEADER_SIZE);
		if (table_offset + table->length > all_tables_size) {
			acpi_os_unmap_memory(table, ACPI_HEADER_SIZE);
			WARN_ON(1);
			return;
		}

		table_length = table->length;

		/* Skip RSDT/XSDT which should only be used for override */
		/* root 表控制全局目录，作为追加表安装会形成第二套不一致的根索引。 */
		if (ACPI_COMPARE_NAMESEG(table->signature, ACPI_SIG_RSDT) ||
		    ACPI_COMPARE_NAMESEG(table->signature, ACPI_SIG_XSDT)) {
			acpi_os_unmap_memory(table, ACPI_HEADER_SIZE);
			goto next_table;
		}
		/*
		 * Mark the table to avoid being used in
		 * acpi_table_initrd_override(). Though this is not possible
		 * because override is disabled in acpi_install_physical_table().
		 */
		/*
		 * 原子领取失败表示 override 或先前扫描已消费该槽；只解除映射并
		 * 推进，不重复调用 ACPICA 安装。
		 */
		if (test_and_set_bit(table_index, acpi_initrd_installed)) {
			acpi_os_unmap_memory(table, ACPI_HEADER_SIZE);
			goto next_table;
		}

		pr_info("Table Upgrade: install [%4.4s-%6.6s-%8.8s]\n",
			table->signature, table->oem_id,
			table->oem_table_id);
		/*
		 * 日志所需字段读取完即解除 header 映射；物理地址仍由保留区保证，
		 * 随后 ACPICA 自行映射、校验并登记整张表。
		 */
		acpi_os_unmap_memory(table, ACPI_HEADER_SIZE);
		acpi_install_physical_table(acpi_tables_addr + table_offset);
next_table:
		table_offset += table_length;
		table_index++;
	}
}
#else
/*
 * 未启用 ACPI_TABLE_UPGRADE 时的 override stub：@existing_table 仅为签名兼容
 * 的借用输入且不读取；@address/@table_length 必须非 NULL并被清零。返回
 * AE_OK 表示“平台 hook 正常但无替代表”，无副作用、不睡眠。
 */
static acpi_status
acpi_table_initrd_override(struct acpi_table_header *existing_table,
			   acpi_physical_address *address,
			   u32 *table_length)
{
	*table_length = 0;
	*address = 0;
	return AE_OK;
}

/*
 * 未启用升级时的 scan stub：无入参、无返回值、无副作用，供初始化主链
 * 无条件调用而无需扩散条件编译。
 */
static void __init acpi_table_initrd_scan(void)
{
}
#endif /* CONFIG_ACPI_TABLE_UPGRADE */

/*
 * acpi_os_physical_table_override - ACPICA OS 层物理表覆盖回调。
 *
 * @existing_table/@address/@table_length 的可空性、ownership 和输出语义完全
 * 遵循 ACPICA hook：指针由调用者借入，输出由 initrd helper 写入。返回
 * AE_OK；本薄包装不新增引用或副作用，只隔离配置相关实现。
 */
acpi_status
acpi_os_physical_table_override(struct acpi_table_header *existing_table,
				acpi_physical_address *address,
				u32 *table_length)
{
	return acpi_table_initrd_override(existing_table, address,
					  table_length);
}

#ifdef CONFIG_ACPI_CUSTOM_DSDT
/*
 * 两个 weakref 兼容不同内建 DSDT 生成器导出的符号名。符号不存在时地址为
 * NULL；存在时指向链接进内核的 AML 字节，不由本文件分配或释放。
 */
static void *amlcode __attribute__ ((weakref("AmlCode")));
static void *dsdt_amlcode __attribute__ ((weakref("dsdt_aml_code")));
#endif

/*
 * acpi_os_table_override - ACPICA OS 层逻辑表覆盖回调，支持内建 DSDT。
 *
 * @existing_table 是 ACPICA 原表借用指针；@new_table 是非 NULL 输出指针，
 * 成功无覆盖时写 NULL，有内建 DSDT 时写静态 AML header 借用地址。函数不
 * 转移或释放任一对象；可在 ACPICA 表安装同步路径调用，不睡眠。
 * 返回 AE_BAD_PARAMETER 表示任一参数为空；其他情况返回 AE_OK。实际覆盖
 * 会调用 acpi_table_taint()，使安全状态和 lockdep 可靠性发生可见变化。
 */
acpi_status acpi_os_table_override(struct acpi_table_header *existing_table,
		       struct acpi_table_header **new_table)
{
	if (!existing_table || !new_table)
		return AE_BAD_PARAMETER;

	/* 先发布无覆盖默认值，确保配置关闭或符号缺失时输出确定。 */
	*new_table = NULL;

#ifdef CONFIG_ACPI_CUSTOM_DSDT
	if (!strncmp(existing_table->signature, "DSDT", 4)) {
		/*
		 * 只允许替换 DSDT。先尝试现代 AmlCode 符号，再回退旧符号；
		 * weakref 不存在时得到 NULL，不会解引用未定义链接符号。
		 */
		*new_table = (struct acpi_table_header *)&amlcode;
		if (!(*new_table))
			*new_table = (struct acpi_table_header *)&dsdt_amlcode;
	}
#endif
	/* 只有真正发布替代表时才 taint；无覆盖路径不改变内核可信状态。 */
	if (*new_table != NULL)
		acpi_table_taint(existing_table);
	return AE_OK;
}

/*
 * acpi_locate_initial_tables()
 *
 * Get the RSDP, then find and checksum all the ACPI tables.
 *
 * result: initial_tables[] is initialized, and points to
 * a list of ACPI tables.
 */
/*
 * 补充说明：acpi_locate_initial_tables - 发现 RSDP 并建立 ACPICA 初始表目录。
 *
 * 无入参。体系结构 ACPI boot 在 memblock 保留之前调用；启动 CPU 串行，无
 * 本文件私有锁。函数先把命令行 checksum 策略发布给 ACPICA 全局开关，再把
 * 固定 initial_tables[] 交给 acpi_initialize_tables() 填充。
 * 返回 0 表示描述符目录可用；ACPICA 失败统一转换为 -EINVAL并打印原始
 * acpi_status 文本。成功副作用是 initial_tables[] 指向发现的固件/override
 * 表；这里只建立目录，不负责 memblock 保留物理区。
 */
int __init acpi_locate_initial_tables(void)
{
	acpi_status status;

	/*
	 * 策略发布：早期映射容量不足的平台默认关闭整表 checksum；显式参数可
	 * 在发现前开启。该开关影响 ACPICA 随后的 table validation。
	 */
	if (acpi_verify_table_checksum) {
		pr_info("Early table checksum verification enabled\n");
		acpi_gbl_enable_table_validation = TRUE;
	} else {
		pr_info("Early table checksum verification disabled\n");
		acpi_gbl_enable_table_validation = FALSE;
	}

	/*
	 * 发现阶段：第三个参数 0 表示使用调用者提供的静态数组而非动态 resize。
	 * 成功后数组条目由 ACPICA 管理，直到初始化生命周期结束。
	 */
	status = acpi_initialize_tables(initial_tables, ACPI_MAX_TABLES, 0);
	if (ACPI_FAILURE(status)) {
		const char *msg = acpi_format_exception(status);

		pr_warn("Failed to initialize tables, status=0x%x (%s)", status, msg);
		return -EINVAL;
	}

	return 0;
}

/*
 * acpi_reserve_initial_tables - 在 memblock 中保留所有已发现 ACPI 表物理区。
 *
 * 无入参/返回值。必须晚于 acpi_locate_initial_tables()、早于伙伴分配器接管；
 * 启动期串行，不睡眠。逐个读取 initial_tables[] 的物理 address/length，
 * 以首个零条目作为有效目录终点；memblock_reserve() 只改变分配可用性，不
 * 建立映射或取得表引用。成功后固件表不会被早期分配覆盖。
 */
void __init acpi_reserve_initial_tables(void)
{
	int i;

	/* 扫描固定池的有效前缀；ACPICA 以零 address/length 留下终止槽。 */
	for (i = 0; i < ACPI_MAX_TABLES; i++) {
		struct acpi_table_desc *table_desc = &initial_tables[i];
		u64 start = table_desc->address;
		u64 size = table_desc->length;

		if (!start || !size)
			break;

		pr_info("Reserving %4s table memory at [mem 0x%llx-0x%llx]\n",
			table_desc->signature.ascii, start, start + size - 1);

		/*
		 * 保留的是物理字节半开区间 [start,start+size)；描述符仍由 ACPICA
		 * 持有，reserve 不代表 Linux 取得固件表内容 ownership。
		 */
		memblock_reserve(start, size);
	}
}

/*
 * acpi_table_init_complete - 完成 override 新表安装和 MADT 实例选择。
 *
 * 无入参/返回值。初始表目录已经可用后调用，启动期串行、不睡眠。先安装
 * 未被物理 override 消费的 initrd 表，再探测多 MADT；顺序保证实例检查能
 * 看到升级后 ACPICA 表集合。副作用由两个子阶段产生，无额外资源持有。
 */
void __init acpi_table_init_complete(void)
{
	acpi_table_initrd_scan();
	check_multiple_madt();
}

/*
 * acpi_table_init - 非体系结构拆分路径使用的一站式 ACPI 表初始化入口。
 *
 * 无入参。先定位初始表，失败原样返回负 errno且不执行完成阶段；成功后安装
 * initrd 新表并检查 MADT，返回 0。启动期串行，资源 ownership 留给 ACPICA
 * 与 memblock；注意本包装不调用 acpi_reserve_initial_tables()，需要该步骤
 * 的体系结构使用拆分入口自行安排。
 */
int __init acpi_table_init(void)
{
	int ret;

	/* 准备失败时 initial_tables 不可依赖，必须阻止后续 scan/MADT 探测。 */
	ret = acpi_locate_initial_tables();
	if (ret)
		return ret;

	acpi_table_init_complete();

	return 0;
}

/*
 * acpi_parse_apic_instance - 解析 acpi_apic_instance= 的 MADT 实例选择。
 *
 * @str 是 early_param 借用的非持久字符串，可为 NULL；按 C 数字语法解析为
 * 全局 int。启动期串行、不睡眠。成功返回 0 并更新 acpi_apic_instance；
 * NULL 或转换失败返回 -EINVAL且保持旧值，成功时输出最终选择。
 */
static int __init acpi_parse_apic_instance(char *str)
{
	if (!str)
		return -EINVAL;

	if (kstrtoint(str, 0, &acpi_apic_instance))
		return -EINVAL;

	pr_notice("Shall use APIC/MADT table %d\n", acpi_apic_instance);

	return 0;
}
early_param("acpi_apic_instance", acpi_parse_apic_instance);

/*
 * acpi_force_table_verification_setup - 开启早期 ACPI 整表 checksum 校验。
 *
 * @s 是参数框架借用但忽略的字符串，可为 NULL；无输出 ownership。启动期
 * 串行、不睡眠，始终返回 0；副作用是把 acpi_verify_table_checksum 置 true，
 * 供 acpi_locate_initial_tables() 发布给 ACPICA。
 */
static int __init acpi_force_table_verification_setup(char *s)
{
	acpi_verify_table_checksum = true;

	return 0;
}
early_param("acpi_force_table_verification", acpi_force_table_verification_setup);

/*
 * acpi_force_32bit_fadt_addr - 强制 ACPICA 使用 FADT 的 32-bit 地址字段。
 *
 * @s 是借用且忽略的参数字符串，可为 NULL。启动期串行、不睡眠，始终返回
 * 0；副作用是记录日志并设置 acpi_gbl_use32_bit_fadt_addresses，适用于
 * 固件扩展 64-bit 字段错误而旧字段可用的兼容场景。
 */
static int __init acpi_force_32bit_fadt_addr(char *s)
{
	pr_info("Forcing 32 Bit FADT addresses\n");
	acpi_gbl_use32_bit_fadt_addresses = TRUE;

	return 0;
}
early_param("acpi_force_32bit_fadt_addr", acpi_force_32bit_fadt_addr);
