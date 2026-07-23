/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 内核 FDPIC ELF 装载规划对象与跨 32/64 位 loadmap 类型适配学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * FDPIC 不假设整个 ELF 按单一 load bias 映射：每个 PT_LOAD 的实际地址由
 * loadmap 描述，函数指针也可能是“代码地址+GOT”描述符。binfmt_elf_fdpic 在
 * 解析 header 后用 elf_fdpic_params 聚合文件引用、各类用户 VA、栈请求和布局
 * flags；架构 helper 可修改 load_addr/arrangement，mapper 再分配 VMA并填写
 * loadmap，最终通过入口寄存器把主程序与解释器 loadmap 交给用户态。
 *
 * 单一参数对象让多阶段装载共享状态且便于错误回滚；代价是字段随阶段逐步有效，
 * 读者必须结合 PRESENT、EXECUTABLE 和 ARRANGEMENT flags 判断当前不变量。
 */
/* FDPIC ELF load map
 *
 * Copyright (C) 2003 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#ifndef _LINUX_ELF_FDPIC_H
#define _LINUX_ELF_FDPIC_H

#include <uapi/linux/elf-fdpic.h>

/* 按当前 ELF_CLASS 把通用名称绑定到对应位宽的 UAPI loadmap/segment 格式。 */
#if ELF_CLASS == ELFCLASS32
#define Elf_Sword			Elf32_Sword
#define elf_fdpic_loadseg		elf32_fdpic_loadseg
#define elf_fdpic_loadmap		elf32_fdpic_loadmap
#define ELF_FDPIC_LOADMAP_VERSION	ELF32_FDPIC_LOADMAP_VERSION
#else
#define Elf_Sword			Elf64_Sxword
#define elf_fdpic_loadmap		elf64_fdpic_loadmap
#define elf_fdpic_loadseg		elf64_fdpic_loadseg
#define ELF_FDPIC_LOADMAP_VERSION	ELF64_FDPIC_LOADMAP_VERSION
#endif

/*
 * binfmt binary parameters structure
 */
/*
 * 一个可执行文件或解释器的完整装载计划与结果。
 *
 * hdr/phdrs 是内核侧解析副本；loadmap 是稍后复制给用户态的段映射表。elfhdr_addr、
 * ph_addr、map_addr、entry_addr、dynamic_addr 均为映射完成后的用户虚拟地址；
 * stack_size 来自 PT_GNU_STACK；load_addr 是架构/通用布局器给 mapper 的选址提示。
 * phdrs/loadmap 的分配与释放由 binfmt_elf_fdpic 主流程拥有，本结构只保存指针。
 */
struct elf_fdpic_params {
	struct elfhdr			hdr;		/* ref copy of ELF header */
	struct elf_phdr			*phdrs;		/* ref copy of PT_PHDR table */
	struct elf_fdpic_loadmap	*loadmap;	/* loadmap to be passed to userspace */
	unsigned long			elfhdr_addr;	/* mapped ELF header user address */
	unsigned long			ph_addr;	/* mapped PT_PHDR user address */
	unsigned long			map_addr;	/* mapped loadmap user address */
	unsigned long			entry_addr;	/* mapped entry user address */
	unsigned long			stack_size;	/* stack size requested (PT_GNU_STACK) */
	unsigned long			dynamic_addr;	/* mapped PT_DYNAMIC user address */
	unsigned long			load_addr;	/* user address at which to map binary */
	unsigned long			flags;
/* 低 4 位互斥表示 PT_LOAD 相对布局约束；其余位描述栈策略、对象角色和有效性。 */
#define ELF_FDPIC_FLAG_ARRANGEMENT	0x0000000f	/* PT_LOAD arrangement flags */
/* 各段可任意选址，最灵活但函数/数据间固定相对位移不成立。 */
#define ELF_FDPIC_FLAG_INDEPENDENT	0x00000000	/* PT_LOADs can be put anywhere */
/* 必须尊重文件 p_vaddr，适合地址已由链接器固定的对象。 */
#define ELF_FDPIC_FLAG_HONOURVADDR	0x00000001	/* PT_LOAD.vaddr must be honoured */
/* 所有段使用同一位移，保留链接时段间差值。 */
#define ELF_FDPIC_FLAG_CONSTDISP	0x00000002	/* PT_LOADs require constant
							 * displacement */
/* mapper 应把各 PT_LOAD 紧邻排列，减少洞但仍由内核选择整体位置。 */
#define ELF_FDPIC_FLAG_CONTIGUOUS	0x00000003	/* PT_LOADs should be contiguous */
/* EXEC_STACK/NOEXEC_STACK 来自 PT_GNU_STACK；两者未置表示文件未明确声明。 */
#define ELF_FDPIC_FLAG_EXEC_STACK	0x00000010	/* T if stack to be executable */
#define ELF_FDPIC_FLAG_NOEXEC_STACK	0x00000020	/* T if stack not to be executable */
/* EXECUTABLE 区分主程序与解释器，PRESENT 表示对应对象确实存在并已参与规划。 */
#define ELF_FDPIC_FLAG_EXECUTABLE	0x00000040	/* T if this object is the executable */
#define ELF_FDPIC_FLAG_PRESENT		0x80000000	/* T if this object is present */
};

#ifdef CONFIG_MMU
/*
 * 架构布局钩子在实际 map 前调整主程序/解释器 load_addr、栈顶和 brk 起点。
 * 四个参数均为借用的可写规划对象/输出指针，无所有权转移、无失败返回。
 */
extern void elf_fdpic_arch_lay_out_mm(struct elf_fdpic_params *exec_params,
				      struct elf_fdpic_params *interp_params,
				      unsigned long *start_stack,
				      unsigned long *start_brk);
#endif

#endif /* _LINUX_ELF_FDPIC_H */
