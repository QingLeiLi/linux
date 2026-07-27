// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 ACPI Parking Protocol implementation
 *
 * Authors: Lorenzo Pieralisi <lorenzo.pieralisi@arm.com>
 *	    Mark Salter <msalter@redhat.com>
 */

/*
 * ACPI Parking Protocol 用一个由固件描述的逐 CPU 信箱启动次级 CPU。
 *
 * 固件先让次级 CPU 停在自己的 parking loop 中，并把信箱 cpu_id 初始化为
 * ~0U。内核启动该 CPU 时：
 *
 *   1. 从 MADT 的 GICC 结构取得信箱物理地址、协议版本和 GIC CPU ID；
 *   2. 向 entry_point 写入 secondary_entry 的物理地址；
 *   3. 最后向 cpu_id 写入目标 GIC CPU ID，并发送 wakeup IPI；
 *   4. 固件观察到有效 cpu_id 后跳到 entry_point，随后按协议清零
 *      entry_point；
 *   5. 新 CPU 进入内核后由 cpu_postboot() 检查固件是否完成清零。
 *
 * 这里的数据以 Linux 逻辑 CPU 号索引，但传给固件的是 MADT 给出的 GIC
 * CPU interface ID。信箱是固件与内核共同拥有的协议内存，所有字段的字节序
 * 都由协议规定为小端，不能依赖内核自身的本地字节序。
 */

#include <linux/acpi.h>
#include <linux/mm.h>
#include <linux/types.h>

#include <asm/cpu_ops.h>

/*
 * 固件可见的信箱 ABI。字段布局和宽度属于 parking protocol，不能为了
 * 内核方便而重排。
 */
struct parking_protocol_mailbox {
	/*
	 * ~0U 表示信箱空闲；内核写入 GIC CPU ID 后，固件据此识别启动请求。
	 */
	__le32 cpu_id;
	/* 协议保留字段，内核既不读取也不修改。 */
	__le32 reserved;
	/*
	 * 次级 CPU 的物理入口。固件完成取用后应清零，postboot 会验证这一
	 * 状态转换。
	 */
	__le64 entry_point;
};

/*
 * 一个逻辑 CPU 在内核侧的 parking protocol 状态。
 *
 * mailbox_addr/version/gic_cpu_id 在 early ACPI MADT 枚举期间写入，之后只读；
 * mailbox 是 cpu_boot() 成功映射后留下的 __iomem 指针，供该目标 CPU 的
 * postboot 回调读取。这个文件没有对应的 iounmap 成功路径，所以该小映射
 * 在当前实现中持续保留；失败路径则在返回前解除映射。
 */
struct cpu_mailbox_entry {
	/* 已映射的固件信箱；在成功执行 cpu_boot() 前不可解引用。 */
	struct parking_protocol_mailbox __iomem *mailbox;
	/* MADT 提供的信箱物理地址，零表示没有可用信箱。 */
	phys_addr_t mailbox_addr;
	/* MADT parking protocol 版本，零表示未声明该协议。 */
	u8 version;
	/* 写给固件的 GIC CPU interface ID。 */
	u8 gic_cpu_id;
};

/*
 * 以 Linux 逻辑 CPU 号为索引的静态协议表。
 *
 * 表项先由启动 CPU 在 ACPI 枚举阶段填充；bring-up 时启动 CPU 更新 mailbox
 * 指针，目标 CPU 只在自己已经启动后的 postboot 中读取对应表项。该流程
 * 没有并发修改同一个表项的正常路径，因此这里不需要独立的锁。
 */
static struct cpu_mailbox_entry cpu_mailbox_entries[NR_CPUS];

/*
 * 把一条 ACPI MADT Generic Interrupt Controller CPU Interface（GICC）
 * 描述复制到内核的逐 CPU 表。
 *
 * 调用者仍拥有并管理 p 指向的 ACPI 表内存；本函数只复制启动所需的
 * 标量，不保留 p。此函数带 __init，因为它只在早期 MADT 枚举期间使用。
 */
void __init acpi_set_mailbox_entry(int cpu,
				   struct acpi_madt_generic_interrupt *p)
{
	struct cpu_mailbox_entry *cpu_entry = &cpu_mailbox_entries[cpu];

	/* parked_address 指向该 CPU 的固件信箱，而不是内核入口本身。 */
	cpu_entry->mailbox_addr = p->parked_address;
	/* 版本与地址共同决定这个 CPU 是否能选择 parking-protocol。 */
	cpu_entry->version = p->parking_version;
	/* 固件协议使用 GICC interface number 标识要释放的 CPU。 */
	cpu_entry->gic_cpu_id = p->cpu_interface_number;
}

/*
 * 判断 ACPI 为指定 CPU 提供的数据是否足以选择 parking protocol。
 *
 * arm64 的 acpi_get_enable_method() 在系统没有选择 PSCI 时调用这里；地址
 * 或版本任一为零都会使 CPU ops 匹配失败。这里只验证 MADT 的基本存在性，
 * 信箱当前是否为 ~0U 要到真正启动时映射后才能检查。
 */
bool acpi_parking_protocol_valid(int cpu)
{
	struct cpu_mailbox_entry *cpu_entry = &cpu_mailbox_entries[cpu];

	return cpu_entry->mailbox_addr && cpu_entry->version;
}

/*
 * CPU ops 的逐 CPU 初始化回调。
 *
 * MADT 数据已由 acpi_set_mailbox_entry() 提前保存，这里无需再次解析 ACPI
 * 表；保留回调主要用于接入统一的 cpu_operations 生命周期并输出调试信息。
 */
static int acpi_parking_protocol_cpu_init(unsigned int cpu)
{
	pr_debug("%s: ACPI parked addr=%llx\n", __func__,
		  cpu_mailbox_entries[cpu].mailbox_addr);

	return 0;
}

/*
 * parking protocol 没有类似 spin-table 的预发布阶段：信箱必须等到每次
 * cpu_boot() 时才填写入口和 CPU ID，因此 prepare 回调为空操作。
 */
static int acpi_parking_protocol_cpu_prepare(unsigned int cpu)
{
	return 0;
}

/*
 * 构造并提交一个 parking protocol 启动请求。
 *
 * 成功返回表示信箱已经写好且 wakeup IPI 已发出，并不保证次级 CPU 已经
 * 执行内核代码；通用 SMP bring-up 层会继续等待其上线握手。失败时：
 *
 *   -EIO   无法以要求的设备属性映射信箱；
 *   -ENXIO 固件没有把 cpu_id 初始化为协议要求的空闲值。
 */
static int acpi_parking_protocol_cpu_boot(unsigned int cpu)
{
	struct cpu_mailbox_entry *cpu_entry = &cpu_mailbox_entries[cpu];
	struct parking_protocol_mailbox __iomem *mailbox;
	u32 cpu_id;

	/*
	 * Map mailbox memory with attribute device nGnRE (ie ioremap -
	 * this deviates from the parking protocol specifications since
	 * the mailboxes are required to be mapped nGnRnE; the attribute
	 * discrepancy is harmless insofar as the protocol specification
	 * is concerned).
	 * If the mailbox is mistakenly allocated in the linear mapping
	 * by FW ioremap will fail since the mapping will be prevented
	 * by the kernel (it clashes with the linear mapping attributes
	 * specifications).
	 */
	/*
	 * ioremap() 返回的 __iomem 指针只能通过 MMIO accessor 访问。若固件
	 * 错把信箱放进已用 Normal Memory 属性建立线性映射的 RAM，内核拒绝
	 * 为同一物理页创建冲突属性，因而这里会失败。
	 */
	mailbox = ioremap(cpu_entry->mailbox_addr, sizeof(*mailbox));
	if (!mailbox)
		return -EIO;

	/* relaxed 读取足以取得协议状态；此处尚未向信箱提交任何新数据。 */
	cpu_id = readl_relaxed(&mailbox->cpu_id);
	/*
	 * Check if firmware has set-up the mailbox entry properly
	 * before kickstarting the respective cpu.
	 */
	/*
	 * 非 ~0U 表示信箱不处于可提交状态。此时没有修改固件内存，只需
	 * 释放临时映射并把协议状态错误返回给通用启动路径。
	 */
	if (cpu_id != ~0U) {
		iounmap(mailbox);
		return -ENXIO;
	}

	/*
	 * stash the mailbox address mapping to use it for further FW
	 * checks in the postboot method
	 */
	/*
	 * 目标 CPU 上线后还要读取同一信箱，因此成功路径不能在这里
	 * iounmap。先保存指针、再发送唤醒 IPI，使 postboot 只需按自身
	 * 逻辑 CPU 号取回映射；该实现后续没有显式解除此映射。
	 */
	cpu_entry->mailbox = mailbox;

	/*
	 * We write the entry point and cpu id as LE regardless of the
	 * native endianness of the kernel. Therefore, any boot-loaders
	 * that read this address need to convert this address to the
	 * Boot-Loader's endianness before jumping.
	 */
	/*
	 * 先写 entry_point，最后写 cpu_id。后者是固件观察的有效标志，因此该
	 * 顺序保证固件接收请求时入口已经就绪；随后的 wakeup IPI 通知 parked
	 * CPU 重新检查信箱。
	 *
	 * secondary_entry 的物理地址用于尚未进入内核虚拟地址空间的 CPU。
	 */
	writeq_relaxed(__pa_symbol(secondary_entry),
		       &mailbox->entry_point);
	writel_relaxed(cpu_entry->gic_cpu_id, &mailbox->cpu_id);

	/*
	 * 向指定逻辑 CPU 发送体系结构唤醒 IPI。IPI 是“去检查信箱”的
	 * 通知，入口和身份仍由上面的共享信箱传递。
	 */
	arch_send_wakeup_ipi(cpu);

	return 0;
}

/*
 * 在新启动的 CPU 上验证固件完成了协议交接。
 *
 * cpu_operations 调度此回调时当前 CPU 已有稳定的逻辑编号，因此可以用
 * smp_processor_id() 找到启动 CPU 预先保存的映射。这里的 WARN 是协议
 * 健康检查：入口未清零说明固件没有完成规范要求的确认，但 CPU 已经
 * 运行，因而不再有可返回给 bring-up 调用者的错误码。
 */
static void acpi_parking_protocol_cpu_postboot(void)
{
	int cpu = smp_processor_id();
	struct cpu_mailbox_entry *cpu_entry = &cpu_mailbox_entries[cpu];
	struct parking_protocol_mailbox __iomem *mailbox = cpu_entry->mailbox;
	u64 entry_point;

	/* mailbox 由成功的 cpu_boot() 保存；postboot 不会出现在失败路径上。 */
	entry_point = readq_relaxed(&mailbox->entry_point);
	/*
	 * Check if firmware has cleared the entry_point as expected
	 * by the protocol specification.
	 */
	/* 只报告固件违约，不改写信箱，以免掩盖诊断现场。 */
	WARN_ON(entry_point);
}

/*
 * 注册 ACPI enable-method 名称对应的 CPU 操作。
 *
 * ACPI 选择逻辑返回 "parking-protocol" 后，通用 cpu_ops 匹配该 name。
 * postboot 用于验证固件清零入口；没有 cpu_can_disable/cpu_die/cpu_kill，
 * 因此该实现不向通用层提供完整的 CPU 热下线协议。
 */
const struct cpu_operations acpi_parking_protocol_ops = {
	.name		= "parking-protocol",
	.cpu_init	= acpi_parking_protocol_cpu_init,
	.cpu_prepare	= acpi_parking_protocol_cpu_prepare,
	.cpu_boot	= acpi_parking_protocol_cpu_boot,
	.cpu_postboot	= acpi_parking_protocol_cpu_postboot
};
