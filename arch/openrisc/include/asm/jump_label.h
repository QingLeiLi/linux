/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Chen Miao
 *
 * Based on arch/arm/include/asm/jump_label.h
 */
#ifndef __ASM_OPENRISC_JUMP_LABEL_H
#define __ASM_OPENRISC_JUMP_LABEL_H

#ifndef __ASSEMBLER__

#include <linux/types.h>
#include <asm/insn-def.h>

#define HAVE_JUMP_LABEL_BATCH

#define JUMP_LABEL_NOP_SIZE OPENRISC_INSN_SIZE

/**
 * JUMP_TABLE_ENTRY - Create a jump table entry
 * @key: Jump key identifier (typically a symbol address)
 * @label: Target label address
 *
 * This macro creates a jump table entry in the dedicated kernel section (__jump_table).
 * Each entry contains the following information:
 * 		Offset from current instruction to jump instruction (1b - .)
 * 		Offset from current instruction to target label (label - .)
 * 		Offset from current instruction to key identifier (key - .)
 */
/*
	// 向 __jump_table section 写入一个 jump_entry 条目, "aw" 表示该 section 可分配（a）且可写（w）
	".pushsection	__jump_table, \"aw\"	\n\t"	\
	// 将当前写入位置对齐到 4 字节边界，确保 jump_entry 结构体对齐，CPU 读取时不会跨 cache line
	".align 	4 			\n\t"	\
	// 1b 是数字标签，引用"向上（backward）最近的一个 1 标签
	// 1f 是引用"向下（forward）最近的一个 1 标签
	// 数字标签可以重复定义，不要求唯一
	// "." 是当前写入位置的地址
	// 1b - . 得到相对偏移，汇编器在编译时直接算出来, 代表NOP 指令相对当前位置的偏移（code 字段）
	// label - .：跳转目标相对当前位置的偏移（target 字段）
	".long 		1b - ., " label " - .	\n\t"	\
	// static_key 变量相对当前位置的偏移（key 字段）
	".long 		" key " - . 		\n\t"	\
	// 切回之前的 section
	".popsection				\n\t" 
*/
#define JUMP_TABLE_ENTRY(key, label)			\
	".pushsection	__jump_table, \"aw\"	\n\t"	\
	".align 	4 			\n\t"	\
	".long 		1b - ., " label " - .	\n\t"	\
	".long 		" key " - . 		\n\t"	\
	".popsection				\n\t"

#define ARCH_STATIC_BRANCH_ASM(key, label)		\
	".align		4			\n\t"	\
	"1: l.nop				\n\t"	\
	"    l.nop				\n\t"	\
	JUMP_TABLE_ENTRY(key, label)

static __always_inline bool arch_static_branch(struct static_key *const key,
					       const bool branch)
{
	asm goto (ARCH_STATIC_BRANCH_ASM("%0", "%l[l_yes]")
		  ::"i"(&((char *)key)[branch])::l_yes);

	return false;
l_yes:
	return true;
}

#define ARCH_STATIC_BRANCH_JUMP_ASM(key, label)		\
	".align		4			\n\t"	\
	"1: l.j	" label "			\n\t"	\
	"    l.nop				\n\t"	\
	JUMP_TABLE_ENTRY(key, label)

static __always_inline bool
arch_static_branch_jump(struct static_key *const key, const bool branch)
{
	asm goto (ARCH_STATIC_BRANCH_JUMP_ASM("%0", "%l[l_yes]")
		  ::"i"(&((char *)key)[branch])::l_yes);

	return false;
l_yes:
	return true;
}

#endif /* __ASSEMBLER__ */
#endif /* __ASM_OPENRISC_JUMP_LABEL_H */
