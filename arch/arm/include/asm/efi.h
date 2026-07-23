/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM32 EFI 架构接口、运行时页表切换和 EFI stub 布局约束学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 内核侧通过 arm_efi_init() 发现固件，通过 efi_create_mapping() 构造独立
 * efi_mm；每次 Runtime Service 调用前后由 efi_virtmap_load/unload 切换页表。
 * stub 侧则使用 MAX_UNCOMP_KERNEL_SIZE、EFI_PHYS_ALIGN 和 initrd 上界选择内核
 * 解压目标，并发布 efi_arm_entry_state 供正式内核诊断固件 CPU 状态。
 *
 * 独立地址空间隔离了固件映射和普通进程，优点是不会永久污染 init_mm；代价是
 * 每次调用要禁止迁移并切换 MMU context。CPU_TTBR0_PAN 配置还要求在固件调用
 * 周围临时开放 TTBR0 访问，否则 runtime function pointer/数据不可达。
 */
/*
 * Copyright (C) 2015 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#ifndef __ASM_ARM_EFI_H
#define __ASM_ARM_EFI_H

#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/early_ioremap.h>
#include <asm/fixmap.h>
#include <asm/highmem.h>
#include <asm/mach/map.h>
#include <asm/mmu_context.h>
#include <asm/ptrace.h>
#include <asm/uaccess.h>

#ifdef CONFIG_EFI
/* 早期发现入口与 ARM 总入口；均只在 setup_arch 启动阶段调用。 */
void efi_init(void);
void arm_efi_init(void);

/* EFI runtime 描述符建表及 Memory Attributes Table 权限收紧回调。 */
int efi_create_mapping(struct mm_struct *mm, efi_memory_desc_t *md);
int efi_set_mapping_permissions(struct mm_struct *mm, efi_memory_desc_t *md, bool);

/* 通用 EFI 调用框架用这对钩子进入/退出专用 efi_mm；必须严格成对。 */
#define arch_efi_call_virt_setup()	efi_virtmap_load()
#define arch_efi_call_virt_teardown()	efi_virtmap_unload()

#ifdef CONFIG_CPU_TTBR0_PAN
/*
 * TTBR0 PAN 下的调用包装：保存当前 uaccess/PAN 状态，临时允许 TTBR0 访问，
 * 调用 runtime service 后恢复。_Generic 兼容返回 efi_status_t 与 void 的固件
 * 方法；void 方法完成调用后以 EFI_ABORTED 作为表达式占位结果。控制表达式不
 * 求值，因此被选中的 (p)->f(args) 只执行一次。宏不负责页表切换，外层 setup/
 * teardown 仍必须包围它。
 */
#undef arch_efi_call_virt
#define arch_efi_call_virt(p, f, args...) ({				\
	unsigned int flags = uaccess_save_and_enable();			\
	efi_status_t res = _Generic((p)->f(args),			\
			efi_status_t:	(p)->f(args),			\
			default:	((p)->f(args), EFI_ABORTED));	\
	uaccess_restore(flags);						\
	res;								\
})
#endif

/*
 * EFI 汇编调用边界需要保存/恢复的 CPSR 位：执行状态、大小端、异常屏蔽、Thumb
 * 和处理器模式。固件不得把这些调用者状态永久带回内核。
 */
#define ARCH_EFI_IRQ_FLAGS_MASK \
	(PSR_J_BIT | PSR_E_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT | \
	 PSR_T_BIT | MODE_MASK)

static inline void efi_set_pgd(struct mm_struct *mm)
{
	/* 复用 ARM ASID/context 切换主路径；NULL 表示没有显式 task 关联。 */
	check_and_switch_context(mm, NULL);
}

/* load 禁止迁移并安装 efi_mm；unload 恢复 current->active_mm 后重新允许抢占。 */
void efi_virtmap_load(void);
void efi_virtmap_unload(void);

#else
/* 无 EFI 构建把 setup_arch 调用折叠为空，不引入运行时代码。 */
#define arm_efi_init()
#endif /* CONFIG_EFI */

/* arch specific definitions used by the stub code */
/* 以下定义运行于 EFI stub/解压阶段，不能依赖正式内核页表或分配器。 */

/*
 * A reasonable upper bound for the uncompressed kernel size is 32 MBytes,
 * so we will reserve that amount of memory. We have no easy way to tell what
 * the actuall size of code + data the uncompressed kernel will use.
 * If this is insufficient, the decompressor will relocate itself out of the
 * way before performing the decompression.
 */
/* 预留 32 MiB 是空间上界而非最终 image_size；不足时解压器自行搬迁规避重叠。 */
#define MAX_UNCOMP_KERNEL_SIZE	SZ_32M

/*
 * phys-to-virt patching requires that the physical to virtual offset is a
 * multiple of 2 MiB. However, using an alignment smaller than TEXT_OFFSET
 * here throws off the memory allocation logic, so let's use the lowest power
 * of two greater than 2 MiB and greater than TEXT_OFFSET.
 */
/* 对齐同时满足 ARM phys-to-virt 指令 patch 编码和解压器 TEXT_OFFSET 布局。 */
#define EFI_PHYS_ALIGN		max(UL(SZ_2M), roundup_pow_of_two(TEXT_OFFSET))

/* on ARM, the initrd should be loaded in a lowmem region */
/*
 * 以 image_addr 所在 4 MiB 窗口为基准，把 initrd 最高地址限制在其后 512 MiB；
 * 返回物理字节地址上界，不分配内存，帮助 stub 保证正式内核能用 lowmem 访问。
 */
static inline unsigned long efi_get_max_initrd_addr(unsigned long image_addr)
{
	return round_down(image_addr, SZ_4M) + SZ_512M;
}

/*
 * EFI stub 安装到配置表的跨阶段快照。before 字段在进入 stub 时采集，after
 * 字段在 ExitBootServices() 后采集；CPSR 表示 SVC/HYP 等执行状态，SCTLR
 * 保存 MMU/cache 控制。表内存由 stub Boot Services pool 持有，正式内核只读。
 */
struct efi_arm_entry_state {
	u32	cpsr_before_ebs;
	u32	sctlr_before_ebs;
	u32	cpsr_after_ebs;
	u32	sctlr_after_ebs;
};

/*
 * capsule 更新前把 [addr, addr+size) 的 CPU D-cache 写回到固件可见内存。
 * addr 为内核 VA、size 为字节；调用者持有缓冲区，函数不改变其所有权。
 */
static inline void efi_capsule_flush_cache_range(void *addr, int size)
{
	__cpuc_flush_dcache_area(addr, size);
}

#endif /* _ASM_ARM_EFI_H */
