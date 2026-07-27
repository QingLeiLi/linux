/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 异常表汇编生成协议学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 本头文件是“可能 fault 的指令”与 arch/arm64/mm/extable.c 修复分发器
 * 之间的编码契约。汇编/uaccess helper 通过这里的宏向 __ex_table 写入：
 *
 *   insn 相对偏移 | fixup 相对偏移 | 16-bit type | 16-bit data
 *
 * 链接器把各翻译单元的 section 合并，启动/模块加载代码排序，异常处理器
 * 再按 fault PC 查表并根据 type/data 改写 pt_regs。此文件只生成元数据，
 * 不在正常路径执行指令，也不拥有运行期对象。
 *
 * `__ASSEMBLER__` 分支供独立 .S 文件直接发出 assembler directive；
 * 另一分支生成 C inline-asm 字符串。两套宏必须产生完全相同的 12-byte
 * 表项布局。type 数值和 data 位域也是生成端与 C handler 的内部 ABI，
 * 任一侧单独变化都会导致错误寄存器被改写或错误 fixup 被选择。
 *
 * 相对 32-bit insn/fixup 避免绝对地址重定位并缩小表项；16-bit type/data
 * 让常见恢复策略共享 handler。代价是链接距离必须落在编码范围内，而且
 * 排序搬移表项时必须由 asm/extable.h 的 swap hook 重算相对偏移。
 */
#ifndef __ASM_ASM_EXTABLE_H
#define __ASM_ASM_EXTABLE_H

#include <linux/bits.h>
#include <asm/gpr-num.h>

/*
 * type 字段的 arm64 修复策略编号：
 *
 * NONE                    保留的无处理类型；总分发不会把它当作可恢复项。
 * BPF                     交给 BPF JIT 专用 handler 解释 fixup 位域。
 * UACCESS_ERR_ZERO        用户访问失败时写 -EFAULT、可选清零寄存器。
 * KACCESS_ERR_ZERO        内核访问变体，复用同一寄存器修复 handler。
 * UACCESS_CPY             copy 双向访问，仅用户地址一侧 fault 可修复。
 * LOAD_UNALIGNED_ZEROPAD  跨页非对齐读取时保留可读字节、故障侧补零。
 *
 * 这些常量没有运行期存储，必须与 fixup_exception() 的 switch 保持一致。
 */
#define EX_TYPE_NONE			0
#define EX_TYPE_BPF			1
#define EX_TYPE_UACCESS_ERR_ZERO	2
#define EX_TYPE_KACCESS_ERR_ZERO	3
#define EX_TYPE_UACCESS_CPY		4
#define EX_TYPE_LOAD_UNALIGNED_ZEROPAD	5

/* Data fields for EX_TYPE_UACCESS_ERR_ZERO */
/*
 * UACCESS/KACCESS ERR_ZERO 的 data 用两个 5-bit 字段保存 AArch64 通用
 * 寄存器编号：bits[4:0] 写入 -EFAULT，bits[9:5] 写入 0。编号 31 表示
 * XZR/WZR，pt_regs_write_reg() 会忽略写入，从而让薄包装表达“无需该输出”。
 */
#define EX_DATA_REG_ERR_SHIFT	0
#define EX_DATA_REG_ERR		GENMASK(4, 0)
#define EX_DATA_REG_ZERO_SHIFT	5
#define EX_DATA_REG_ZERO	GENMASK(9, 5)

/* Data fields for EX_TYPE_LOAD_UNALIGNED_ZEROPAD */
/*
 * ZEROPAD 的 data 同样打包两个 5-bit 编号：DATA 是恢复值目标寄存器，
 * ADDR 是原始未对齐地址寄存器。handler 用二者读取异常现场并写回结果。
 */
#define EX_DATA_REG_DATA_SHIFT	0
#define EX_DATA_REG_DATA	GENMASK(4, 0)
#define EX_DATA_REG_ADDR_SHIFT	5
#define EX_DATA_REG_ADDR	GENMASK(9, 5)

/* Data fields for EX_TYPE_UACCESS_CPY */
/*
 * copy 表项只需一位记录“uaccess 半边是否为写”。异常 ESR 的 WnR 位必须
 * 与之相等，才能证明 fault 来自用户指针而非内核缓冲区。
 */
#define EX_DATA_UACCESS_WRITE	BIT(0)

#ifdef __ASSEMBLER__

/*
 * __ASM_EXTABLE_RAW - 在独立汇编中发出一个原始 arm64 异常表项。
 *
 * @insn/@fixup 是当前汇编单元内的标签表达式；@type/@data 是可由
 * assembler 求值的常量。宏临时切换到只分配、不执行的 __ex_table，
 * 以 4-byte 对齐写两个相对当前位置的 .long，再写两个 .short，最后恢复
 * 原 section。生成过程无运行期副作用；字段基址必须与 C 结构布局一致。
 */
#define __ASM_EXTABLE_RAW(insn, fixup, type, data)	\
	.pushsection	__ex_table, "a";		\
	.align		2;				\
	.long		((insn) - .);			\
	.long		((fixup) - .);			\
	.short		(type);				\
	.short		(data);				\
	.popsection;

/*
 * EX_DATA_REG - 把汇编寄存器名字转换为 data 位域。
 *
 * @reg 选择 ERR/ZERO/DATA/ADDR 字段，@gpr 是 xN/wN/xzr/wzr 名称。
 * asm/gpr-num.h 生成的 .L__gpr_num_* 常量提供架构寄存器号，再左移到
 * 对应字段。该宏只参与汇编期常量计算。
 */
#define EX_DATA_REG(reg, gpr)	\
	(.L__gpr_num_##gpr << EX_DATA_REG_##reg##_SHIFT)

/*
 * _ASM_EXTABLE_UACCESS_ERR_ZERO - 生成用户访问的“错误码 + 清零”表项。
 *
 * @insn 是可能 fault 的用户访问指令，@fixup 是恢复标签；@err 指定接收
 * -EFAULT 的寄存器，@zero 指定同时清零的寄存器。handler 根据编码修改
 * pt_regs 后跳到 fixup；传 wzr 可选择性丢弃某个输出。
 */
#define _ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, err, zero)		\
	__ASM_EXTABLE_RAW(insn, fixup, 					\
			  EX_TYPE_UACCESS_ERR_ZERO,			\
			  (						\
			    EX_DATA_REG(ERR, err) |			\
			    EX_DATA_REG(ZERO, zero)			\
			  ))

/*
 * 用户访问薄包装：ERR 固定只写错误寄存器，UACCESS 把 err/zero 都设为
 * wzr，表示只需要跳到 fixup。两者不改变底层表项布局和恢复类型。
 */
#define _ASM_EXTABLE_UACCESS_ERR(insn, fixup, err)			\
	_ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, err, wzr)

#define _ASM_EXTABLE_UACCESS(insn, fixup)				\
	_ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, wzr, wzr)

/*
 * Create an exception table entry for uaccess `insn`, which will branch to `fixup`
 * when an unhandled fault is taken.
 */
/*
 * 为 uaccess 指令 insn 创建异常表项；发生未被普通缺页流程解决的 fault
 * 时，arm64 fixup 会把异常 PC 改到 fixup。该 assembler macro 只是给
 * .S 调用者提供小写接口，最终仍展开为上面的 UACCESS 表项。
 */
	.macro          _asm_extable_uaccess, insn, fixup
	_ASM_EXTABLE_UACCESS(\insn, \fixup)
	.endm

/*
 * Create an exception table entry for `insn` if `fixup` is provided. Otherwise
 * do nothing.
 */
/*
 * 仅当调用者提供非空 fixup 文本时为 insn 生成表项；空参数时不产生任何
 * section 数据。该条件宏让 cache maintenance 等汇编 helper 同时支持
 * “允许 fault 并恢复”和“没有恢复点”两种实例。
 */
	.macro		_cond_uaccess_extable, insn, fixup
	.ifnc			\fixup,
	_asm_extable_uaccess	\insn, \fixup
	.endif
	.endm

/*
 * copy 专用 assembler 宏额外编码 uaccess_is_write，供 fault handler
 * 把 ESR 访问方向与用户半边方向匹配；insn/fixup 仍是汇编标签。
 */
	.macro		_asm_extable_uaccess_cpy, insn, fixup, uaccess_is_write
	__ASM_EXTABLE_RAW(\insn, \fixup, EX_TYPE_UACCESS_CPY, \uaccess_is_write)
	.endm

#else /* __ASSEMBLER__ */
/*
 * 以下分支供 C inline asm 使用：宏返回字符串片段而非直接汇编 directive，
 * 但产生的 section、对齐和字段顺序必须与上面的 .S 分支一致。
 */

#include <linux/stringify.h>

/*
 * C 字符串版 __ASM_EXTABLE_RAW 与 assembler 版参数语义相同。#insn 等
 * 上层 stringify 会把 C 宏参数变成汇编表达式；每个换行结束一条 directive。
 */
#define __ASM_EXTABLE_RAW(insn, fixup, type, data)	\
	".pushsection	__ex_table, \"a\"\n"		\
	".align		2\n"				\
	".long		((" insn ") - .)\n"		\
	".long		((" fixup ") - .)\n"		\
	".short		(" type ")\n"			\
	".short		(" data ")\n"			\
	".popsection\n"

/*
 * C inline-asm 版 EX_DATA_REG 把寄存器 token 和 shift 常量拼入字符串；
 * __DEFINE_ASM_GPR_NUMS 必须同时生成 `.L__gpr_num_*` 符号供 assembler
 * 求值。
 */
#define EX_DATA_REG(reg, gpr)						\
	"((.L__gpr_num_" #gpr ") << " __stringify(EX_DATA_REG_##reg##_SHIFT) ")"

/*
 * C inline-asm 用户访问生成器。__DEFINE_ASM_GPR_NUMS 建立寄存器名映射；
 * 随后的 RAW 表项编码 err/zero 两个输出寄存器，不执行运行期 C 逻辑。
 */
#define _ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, err, zero)		\
	__DEFINE_ASM_GPR_NUMS						\
	__ASM_EXTABLE_RAW(#insn, #fixup, 				\
			  __stringify(EX_TYPE_UACCESS_ERR_ZERO),	\
			  "("						\
			    EX_DATA_REG(ERR, err) " | "			\
			    EX_DATA_REG(ZERO, zero)			\
			  ")")

/*
 * KACCESS 版本使用相同 data 位域，但 type 明确标记访问属于内核地址；
 * 当前 arm64 分发器让它复用 ERR_ZERO handler，分类者仍可区分来源。
 */
#define _ASM_EXTABLE_KACCESS_ERR_ZERO(insn, fixup, err, zero)		\
	__DEFINE_ASM_GPR_NUMS						\
	__ASM_EXTABLE_RAW(#insn, #fixup, 				\
			  __stringify(EX_TYPE_KACCESS_ERR_ZERO),	\
			  "("						\
			    EX_DATA_REG(ERR, err) " | "			\
			    EX_DATA_REG(ZERO, zero)			\
			  ")")

/*
 * 四个薄包装固定 zero/err 为 wzr：
 *
 * UACCESS_ERR/KACCESS_ERR 只保留 -EFAULT 输出；
 * UACCESS/KACCESS 两个输出都丢弃，只改变异常 PC。
 */
#define _ASM_EXTABLE_UACCESS_ERR(insn, fixup, err)			\
	_ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, err, wzr)

#define _ASM_EXTABLE_UACCESS(insn, fixup)				\
	_ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, wzr, wzr)

#define _ASM_EXTABLE_KACCESS_ERR(insn, fixup, err)			\
	_ASM_EXTABLE_KACCESS_ERR_ZERO(insn, fixup, err, wzr)

#define _ASM_EXTABLE_KACCESS(insn, fixup)				\
	_ASM_EXTABLE_KACCESS_ERR_ZERO(insn, fixup, wzr, wzr)

/*
 * _ASM_EXTABLE_LOAD_UNALIGNED_ZEROPAD - 生成非对齐跨页容错读取表项。
 *
 * @insn/@fixup 是 inline-asm 标签，@data 是接收补零结果的寄存器，
 * @addr 是保存原始地址的寄存器。生成器把两个编号写入 data，handler
 * 根据端序重建一个 unsigned long 并继续到 fixup。
 */
#define _ASM_EXTABLE_LOAD_UNALIGNED_ZEROPAD(insn, fixup, data, addr)		\
	__DEFINE_ASM_GPR_NUMS							\
	__ASM_EXTABLE_RAW(#insn, #fixup,					\
			  __stringify(EX_TYPE_LOAD_UNALIGNED_ZEROPAD),		\
			  "("							\
			    EX_DATA_REG(DATA, data) " | "			\
			    EX_DATA_REG(ADDR, addr)				\
			  ")")

#endif /* __ASSEMBLER__ */
/* 汇编源与 C inline asm 两种生成前端在此汇合为同一运行期表项 ABI。 */

#endif /* __ASM_ASM_EXTABLE_H */
/* 结束 arm64 异常表生成宏保护，防止重复定义类型与位域 ABI。 */
