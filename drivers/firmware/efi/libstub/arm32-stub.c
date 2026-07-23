// SPDX-License-Identifier: GPL-2.0
/*
 * ARM32 EFI stub CPU 能力检查、状态交接与解压目标选址学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 此文件运行在正式内核启动之前，只能使用 EFI Boot Services 和少量 CP15 指令。
 * check_platform_features() 记录进入 stub 时的 CPSR/SCTLR，把状态结构安装为 EFI
 * 配置表，并在 LPAE 内核上验证 CPU 能力；ExitBootServices() 后再补写 after 字段，
 * 供 arch/arm/kernel/efi.c 读取。handle_kernel_image() 则预留解压后内核窗口，
 * 计算满足 phys-to-virt patch 的对齐基址并归还未使用的首尾页。
 *
 * 配置表把 bootloader 阶段证据跨越 ExitBootServices 交给内核，代价是必须严格
 * 管理 pool 所有权：安装失败或后续能力检查失败时撤表并释放；成功后内存留给
 * 内核启动阶段只读消费。选址采用“较大临时分配后裁剪”，用少量空间换取对齐成功。
 */
/*
 * Copyright (C) 2013 Linaro Ltd;  <roy.franz@linaro.org>
 */
#include <linux/efi.h>
#include <asm/efi.h>

#include "efistub.h"

/* 私有配置表 GUID；正式内核用同一 LINUX_EFI_ARM_CPU_STATE_TABLE_GUID 查找。 */
static efi_guid_t cpu_state_guid = LINUX_EFI_ARM_CPU_STATE_TABLE_GUID;

/* Boot Services pool 中的跨阶段状态对象；安装成功后配置表持有其可发现性。 */
struct efi_arm_entry_state *efi_entry_state;

/*
 * 读取当前异常级/处理器模式的 CPSR 和对应 SCTLR。
 * cpsr/sctlr 均为必填输出指针。HYP_MODE 使用 CP15 opc1=4 读取 HSCTLR，SVC 等
 * 非 HYP 模式使用普通 SCTLR；函数不改变寄存器，也不调用 Boot Services。
 */
static void get_cpu_state(u32 *cpsr, u32 *sctlr)
{
	asm("mrs %0, cpsr" : "=r"(*cpsr));
	if ((*cpsr & MODE_MASK) == HYP_MODE)
		asm("mrc p15, 4, %0, c1, c0, 0" : "=r"(*sctlr));
	else
		asm("mrc p15, 0, %0, c1, c0, 0" : "=r"(*sctlr));
}

/*
 * EFI stub 平台准入检查及状态表发布事务。
 *
 * 成功返回 EFI_SUCCESS，efi_entry_state 已安装到配置表且 before 字段有效；
 * pool 分配/安装失败返回固件错误。非 LPAE 内核无需额外 CPU 检查；LPAE 构建
 * 要求 ID_MMFR0.VMSA 编码至少 5，否则返回 EFI_UNSUPPORTED，并按“先撤配置表、
 * 再释放 pool”顺序回滚，避免固件留下悬空表指针。
 */
efi_status_t check_platform_features(void)
{
	efi_status_t status;
	u32 cpsr, sctlr;
	int block;

	/* 在任何 EFI 调用前取快照，尽量反映 firmware 交给 stub 的原始状态。 */
	get_cpu_state(&cpsr, &sctlr);

	efi_info("Entering in %s mode with MMU %sabled\n",
		 ((cpsr & MODE_MASK) == HYP_MODE) ? "HYP" : "SVC",
		 (sctlr & 1) ? "en" : "dis");

	/* EFI_LOADER_DATA pool 在 ExitBootServices 前归 stub，成功安装后留到内核接管。 */
	status = efi_bs_call(allocate_pool, EFI_LOADER_DATA,
			     sizeof(*efi_entry_state),
			     (void **)&efi_entry_state);
	if (status != EFI_SUCCESS) {
		efi_err("allocate_pool() failed\n");
		return status;
	}

	efi_entry_state->cpsr_before_ebs = cpsr;
	efi_entry_state->sctlr_before_ebs = sctlr;

	/* 发布动作放在字段初始化之后，固件/内核发现表时不会看到未初始化 before 值。 */
	status = efi_bs_call(install_configuration_table, &cpu_state_guid,
			     efi_entry_state);
	if (status != EFI_SUCCESS) {
		efi_err("install_configuration_table() failed\n");
		goto free_state;
	}

	/* non-LPAE kernels can run anywhere */
	/* short-descriptor 内核不要求 CPU 提供 LPAE 页表格式，可直接完成准入。 */
	if (!IS_ENABLED(CONFIG_ARM_LPAE))
		return EFI_SUCCESS;

	/* LPAE kernels need compatible hardware */
	/* ID_MMFR0 低 nibble 是 VMSA 支持级别；小于 5 不满足 ARM LPAE 内核要求。 */
	block = cpuid_feature_extract(CPUID_EXT_MMFR0, 0);
	if (block < 5) {
		efi_err("This LPAE kernel is not supported by your CPU\n");
		status = EFI_UNSUPPORTED;
		goto drop_table;
	}
	return EFI_SUCCESS;

drop_table:
	/* 先让配置表不再可发现 state，再释放 backing，防止悬空 GUID 条目。 */
	efi_bs_call(install_configuration_table, &cpu_state_guid, NULL);
free_state:
	efi_bs_call(free_pool, efi_entry_state);
	return status;
}

/*
 * ExitBootServices() 成功后的补充快照。check_platform_features() 成功是前置条件，
 * 因而 efi_entry_state 已分配且仍有效。函数只写 after 字段，不再调用已失效的
 * Boot Services；正式内核随后可比较固件交接前后的 CPU 模式/MMU 状态。
 */
void efi_handle_post_ebs_state(void)
{
	get_cpu_state(&efi_entry_state->cpsr_after_ebs,
		      &efi_entry_state->sctlr_after_ebs);
}

/*
 * 为 ARM 解压后内核选择并预留物理窗口。
 *
 * image_addr/image_size、reserve_addr/reserve_size 是输出指针；image 和
 * image_handle 属于跨架构 stub ABI，本实现无需读取。先申请
 * MAX_UNCOMP_KERNEL_SIZE+EFI_PHYS_ALIGN 字节，再选择对齐 kernel_base，保留从
 * kernel_base+slack 开始的 32 MiB，释放前后多余页。成功时 image_addr 指向
 * kernel_base+TEXT_OFFSET、image_size=0（实际解压大小此时未知），reserve_* 描述
 * 必须保留的 backing；分配失败保持输出未承诺并返回 EFI 错误。
 */
efi_status_t handle_kernel_image(unsigned long *image_addr,
				 unsigned long *image_size,
				 unsigned long *reserve_addr,
				 unsigned long *reserve_size,
				 efi_loaded_image_t *image,
				 efi_handle_t image_handle)
{
	const int slack = TEXT_OFFSET - 5 * PAGE_SIZE;
	int alloc_size = MAX_UNCOMP_KERNEL_SIZE + EFI_PHYS_ALIGN;
	unsigned long alloc_base, kernel_base;
	efi_status_t status;

	/*
	 * Allocate space for the decompressed kernel as low as possible.
	 * The region should be 16 MiB aligned, but the first 'slack' bytes
	 * are not used by Linux, so we allow those to be occupied by the
	 * firmware.
	 */
	/*
	 * 原注释中的“16 MiB”是历史表述；实际对齐由 EFI_PHYS_ALIGN 定义，当前规则是
	 * max(2 MiB, roundup_pow_of_two(TEXT_OFFSET))，应以配置计算值而非固定 16 MiB
	 * 理解下面的取整。
	 */
	status = efi_low_alloc_above(alloc_size, EFI_PAGE_SIZE, &alloc_base, 0x0);
	if (status != EFI_SUCCESS) {
		efi_err("Unable to allocate memory for uncompressed kernel.\n");
		return status;
	}

	if ((alloc_base % EFI_PHYS_ALIGN) > slack) {
		/*
		 * More than 'slack' bytes are already occupied at the base of
		 * the allocation, so we need to advance to the next 16 MiB block.
		 */
		/* 同样应把“16 MiB block”理解为下一个 EFI_PHYS_ALIGN 边界。 */
		kernel_base = round_up(alloc_base, EFI_PHYS_ALIGN);
		efi_info("Free memory starts at 0x%lx, setting kernel_base to 0x%lx\n",
			 alloc_base, kernel_base);
	} else {
		kernel_base = round_down(alloc_base, EFI_PHYS_ALIGN);
	}

	/* reserve 起点距最终 image_addr 固定 5 页，为解压器头部/启动布局保留前导空间。 */
	*reserve_addr = kernel_base + slack;
	*reserve_size = MAX_UNCOMP_KERNEL_SIZE;

	/* now free the parts that we will not use */
	/* 裁掉对齐过量分配的前缀和尾部；保留区所有权继续由 EFI loader memory 持有。 */
	if (*reserve_addr > alloc_base) {
		efi_bs_call(free_pages, alloc_base,
			    (*reserve_addr - alloc_base) / EFI_PAGE_SIZE);
		alloc_size -= *reserve_addr - alloc_base;
	}
	efi_bs_call(free_pages, *reserve_addr + MAX_UNCOMP_KERNEL_SIZE,
		    (alloc_size - MAX_UNCOMP_KERNEL_SIZE) / EFI_PAGE_SIZE);

	/* 解压器从 image_addr 放置正式内核；0 表示 stub 尚不知道最终展开字节数。 */
	*image_addr = kernel_base + TEXT_OFFSET;
	*image_size = 0;

	efi_debug("image addr == 0x%lx, reserve_addr == 0x%lx\n",
		  *image_addr, *reserve_addr);

	return EFI_SUCCESS;
}
