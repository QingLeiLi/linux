/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM32 ELF ABI 常量、core dump 寄存器布局和 binfmt 架构钩子学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 通用 ELF/FDPIC 装载器包含本头文件后，通过这里把“ELF 文件”翻译成 ARM ABI：
 * e_flags/relocation 编号决定文件语义，elf_check_arch()/SET_PERSONALITY 决定
 * 能否执行及任务模式，ELF_PLAT_INIT/FDPIC_PLAT_INIT 构造入口寄存器，core dump
 * 类型和 ELF_CLASS/DATA/ARCH 则固定调试器看到的文件格式。
 *
 * 这些宏是内核与工具链、动态链接器、调试器共同遵守的 ABI，数值不能随意整理；
 * 同一 bit 在不同 EABI 版本可能复用，必须先看 EF_ARM_EABI_MASK 再解释低位。
 */
#ifndef __ASMARM_ELF_H
#define __ASMARM_ELF_H

#include <asm/auxvec.h>
#include <asm/hwcap.h>

/*
 * ELF register definitions..
 */
/* pt_regs/user_fp 是异常现场与用户浮点保存区，core dump 直接按其 ABI 布局导出。 */
#include <asm/ptrace.h>
#include <asm/user.h>

struct task_struct;

typedef unsigned long elf_greg_t;
typedef unsigned long elf_freg_t[3];

/* 通用寄存器集恰好覆盖一个 pt_regs，调试器据此恢复 r0-r15/CPSR 等现场。 */
#define ELF_NGREG (sizeof (struct pt_regs) / sizeof(elf_greg_t))
typedef elf_greg_t elf_gregset_t[ELF_NGREG];

typedef struct user_fp elf_fpregset_t;

/* e_flags 高字节标识 EABI 版本；0 表示历史 OABI，其低位规则由 arch elf.c 校验。 */
#define EF_ARM_EABI_MASK	0xff000000
#define EF_ARM_EABI_UNKNOWN	0x00000000
#define EF_ARM_EABI_VER1	0x01000000
#define EF_ARM_EABI_VER2	0x02000000
#define EF_ARM_EABI_VER3	0x03000000
#define EF_ARM_EABI_VER4	0x04000000
#define EF_ARM_EABI_VER5	0x05000000

/*
 * 下列文件特性位的意义依赖 ABI 版本；例如 0x10 在 ABI2 是 map-symbol 顺序，
 * 在 OABI 则表示浮点参数进寄存器。装载器不能脱离版本单独解释某一位。
 */
#define EF_ARM_BE8		0x00800000	/* ABI 4,5 */
#define EF_ARM_LE8		0x00400000	/* ABI 4,5 */
#define EF_ARM_MAVERICK_FLOAT	0x00000800	/* ABI 0 */
#define EF_ARM_VFP_FLOAT	0x00000400	/* ABI 0 */
#define EF_ARM_SOFT_FLOAT	0x00000200	/* ABI 0 */
#define EF_ARM_OLD_ABI		0x00000100	/* ABI 0 */
#define EF_ARM_NEW_ABI		0x00000080	/* ABI 0 */
#define EF_ARM_ALIGN8		0x00000040	/* ABI 0 */
#define EF_ARM_PIC		0x00000020	/* ABI 0 */
#define EF_ARM_MAPSYMSFIRST	0x00000010	/* ABI 2 */
#define EF_ARM_APCS_FLOAT	0x00000010	/* ABI 0, floats in fp regs */
#define EF_ARM_DYNSYMSUSESEGIDX	0x00000008	/* ABI 2 */
#define EF_ARM_APCS_26		0x00000008	/* ABI 0 */
#define EF_ARM_SYMSARESORTED	0x00000004	/* ABI 1,2 */
#define EF_ARM_INTERWORK	0x00000004	/* ABI 0 */
#define EF_ARM_HASENTRY		0x00000002	/* All */
#define EF_ARM_RELEXEC		0x00000001	/* All */

/* ARM 状态重定位编号：由模块/用户 ELF relocation 记录使用，数值来自 ARM ABI。 */
#define R_ARM_NONE		0
#define R_ARM_PC24		1
#define R_ARM_ABS32		2
#define R_ARM_REL32		3
#define R_ARM_CALL		28
#define R_ARM_JUMP24		29
#define R_ARM_TARGET1		38
#define R_ARM_V4BX		40
#define R_ARM_PREL31		42
#define R_ARM_MOVW_ABS_NC	43
#define R_ARM_MOVT_ABS		44
#define R_ARM_MOVW_PREL_NC	45
#define R_ARM_MOVT_PREL		46
#define R_ARM_ALU_PC_G0_NC	57
#define R_ARM_ALU_PC_G1_NC	59
#define R_ARM_LDR_PC_G2		63

/* Thumb 指令编码对应的调用、跳转及 MOVW/MOVT 重定位，不能与 ARM 编码混用。 */
#define R_ARM_THM_CALL		10
#define R_ARM_THM_JUMP24	30
#define R_ARM_THM_MOVW_ABS_NC	47
#define R_ARM_THM_MOVT_ABS	48
#define R_ARM_THM_MOVW_PREL_NC	49
#define R_ARM_THM_MOVT_PREL	50

/*
 * These are used to set parameters in the core dumps.
 */
/* core 文件固定为 ELF32/EM_ARM；ELF_DATA 随内核构建端序选择，供分析器解码。 */
#define ELF_CLASS	ELFCLASS32
#ifdef __ARMEB__
#define ELF_DATA	ELFDATA2MSB
#else
#define ELF_DATA	ELFDATA2LSB
#endif
#define ELF_ARCH	EM_ARM

/*
 * This yields a string that ld.so will use to load implementation
 * specific libraries for optimization.  This is more specific in
 * intent than poking at uname or /proc/cpuinfo.
 *
 * For now we just provide a fairly general string that describes the
 * processor family.  This could be made more specific later if someone
 * implemented optimisations that require it.  26-bit CPUs give you
 * "v1l" for ARM2 (no SWP) and "v2l" for anything else (ARM1 isn't
 * supported).  32-bit CPUs give you "v3[lb]" for anything based on an
 * ARM6 or ARM7 core and "armv4[lb]" for anything based on a StrongARM-1
 * core.
 */
/*
 * elf_platform 作为 AT_PLATFORM 传给 ld.so，表示粗粒度 CPU 家族和端序；长度
 * 上界 8 字节。它是库优化选择提示，不是安全能力检查，精确特性应使用 HWCAP。
 */
#define ELF_PLATFORM_SIZE 8
#define ELF_PLATFORM	(elf_platform)

extern char elf_platform[];

struct elf32_hdr;

/*
 * This is used to ensure we don't load something for the wrong architecture.
 */
/* binfmt 在提交新 mm 前调用实现文件中的校验器；0 会使装载返回 -ENOEXEC。 */
extern int elf_check_arch(const struct elf32_hdr *);
#define elf_check_arch elf_check_arch

/* FDPIC 用 OSABI=65 区分函数描述符 ABI；PIC 标志决定段是否要求常量位移。 */
#define ELFOSABI_ARM_FDPIC  65	/* ARM FDPIC platform */
#define elf_check_fdpic(x)  ((x)->e_ident[EI_OSABI] == ELFOSABI_ARM_FDPIC)
#define elf_check_const_displacement(x)  ((x)->e_flags & EF_ARM_PIC)
#define ELF_FDPIC_CORE_EFLAGS  0

#define vmcore_elf64_check_arch(x) (0)

/* ARM 的 READ_IMPLIES_EXEC 只依赖解析后的 EXSTACK_* 状态，不需再次读取 header。 */
extern int arm_elf_read_implies_exec(int);
#define elf_read_implies_exec(ex,stk) arm_elf_read_implies_exec(stk)

#define CORE_DUMP_USE_REGSET
/* ELF 文件页对齐由 ABI 固定为 4 KiB，不随内核可能使用的更大 PAGE_SIZE 改变。 */
#define ELF_EXEC_PAGESIZE	4096

/* This is the base location for PIE (ET_DYN with INTERP) loads. */
/* 非固定地址主程序从 4 MiB 附近布局，为低地址空洞和主程序/解释器错开留空间。 */
#define ELF_ET_DYN_BASE		0x400000UL

/* When the program starts, a1 contains a pointer to a function to be 
   registered with atexit, as per the SVR4 ABI.  A value of 0 means we 
   have no such handler.  */
/* ARM a1 即 r0；Linux 不提供该 SVR4 atexit 钩子，进入用户入口前明确清零。 */
#define ELF_PLAT_INIT(_r, load_addr)	(_r)->ARM_r0 = 0

/* FDPIC 入口 ABI：r7/r8 分别给主程序/解释器 loadmap，r9 指向动态段。 */
#define ELF_FDPIC_PLAT_INIT(_r, _exec_map_addr, _interp_map_addr, dynamic_addr) \
	do { \
		(_r)->ARM_r7 = _exec_map_addr; \
		(_r)->ARM_r8 = _interp_map_addr; \
		(_r)->ARM_r9 = dynamic_addr; \
	} while(0)

extern void elf_set_personality(const struct elf32_hdr *);
/* begin_new_exec() 后用新 header 更新 current 的 Linux/APCS/iWMMXt 执行语义。 */
#define SET_PERSONALITY(ex)	elf_set_personality(&(ex))

#ifdef CONFIG_MMU
#ifdef CONFIG_VDSO
/* 把当前 mm 的 VDSO ELF header 地址写入 AT_SYSINFO_EHDR 辅助向量供 libc 使用。 */
#define ARCH_DLINFO						\
do {								\
	NEW_AUX_ENT(AT_SYSINFO_EHDR,				\
		    (elf_addr_t)current->mm->context.vdso);	\
} while (0)
#endif
/* 通知通用装载器调用 arch_setup_additional_pages() 建立 VDSO 等架构附加映射。 */
#define ARCH_HAS_SETUP_ADDITIONAL_PAGES 1
struct linux_binprm;
int arch_setup_additional_pages(struct linux_binprm *, int);
#endif

#endif
