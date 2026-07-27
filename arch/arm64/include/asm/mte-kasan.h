/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 MTE/KASAN 后端学习导读
 *
 * 中文学习注释生成模型：OpenAI GPT-5 Codex（2026-07-27）。
 * 源码分析基线：doc/lql 分支，commit f8f7ac7435bf。
 * 宏观学习入口：doc/09 linux-memory-management-internals.md。
 *
 * 本文件把 KASAN HW_TAGS 的通用架构钩子落实为 Arm Memory Tagging
 * Extension 指令和系统寄存器操作：查询 fault 模式、控制 PSTATE.TCO、
 * 读取/生成 tag，以及批量设置内存 allocation tag。它不分配内存、不管理
 * 对象生命周期，也不决定 KASAN 策略；调用者必须保证 CPU 支持 MTE、
 * 地址和长度按 16-byte granule 对齐，并负责并发排他。
 *
 * CONFIG_KASAN_HW_TAGS 控制 KASAN 异步模式静态键；CONFIG_ARM64_MTE
 * 控制指令实现是否存在。关闭 MTE 时保留同签名桩函数。热路径采用 static
 * key 和 DC 块指令减少分支与逐 granule 操作，代价是严格的前置条件。
 */
/*
 * Copyright (C) 2020 ARM Ltd.
 */
#ifndef __ASM_MTE_KASAN_H
#define __ASM_MTE_KASAN_H

#include <asm/compiler.h>
#include <asm/cputype.h>
#include <asm/mte-def.h>

#ifndef __ASSEMBLER__

#include <linux/types.h>

#ifdef CONFIG_KASAN_HW_TAGS

/* Whether the MTE asynchronous mode is enabled. */
/*
 * 该静态键表示 KASAN 当前使用 async 或 asymmetric fault 模式。默认
 * false，启动期在模式确定后切换；更新由初始化路径串行完成，
 * 运行期读者无需加锁。
 */
DECLARE_STATIC_KEY_FALSE(mte_async_or_asymm_mode);

/*
 * 查询是否需要异步模式特殊处理。无入参；任意原子上下文可调用，
 * 不睡眠。
 * 返回静态键布尔值，无副作用。
 */
static inline bool system_uses_mte_async_or_asymm_mode(void)
{
	return static_branch_unlikely(&mte_async_or_asymm_mode);
}

#else /* CONFIG_KASAN_HW_TAGS */

/* 未编译 KASAN_HW_TAGS 时固定返回 false；无入参、无副作用且不睡眠。 */
static inline bool system_uses_mte_async_or_asymm_mode(void)
{
	return false;
}

#endif /* CONFIG_KASAN_HW_TAGS */

#ifdef CONFIG_ARM64_MTE

/*
 * The Tag Check Flag (TCF) mode for MTE is per EL, hence TCF0
 * affects EL0 and TCF affects EL1 irrespective of which TTBR is
 * used.
 * The kernel accesses TTBR0 usually with LDTR/STTR instructions
 * when UAO is available, so these would act as EL0 accesses using
 * TCF0.
 * However futex.h code uses exclusives which would be executed as
 * EL1, this can potentially cause a tag check fault even if the
 * user disables TCF0.
 *
 * To address the problem we set the PSTATE.TCO bit in uaccess_enable()
 * and reset it in uaccess_disable().
 *
 * The Tag check override (TCO) bit disables temporarily the tag checking
 * preventing the issue.
 */
/*
 * TCF0 管理 EL0 tag check，TCF 管理 EL1，与使用哪个 TTBR 无关。普通
 * uaccess 的 LDTR/STTR 被视为 EL0 访问，但 futex exclusive 指令作为 EL1
 * 访问，即使用户关闭 TCF0 也可能触发 tag fault。
 *
 * uaccess_enable() 因此置 PSTATE.TCO，uaccess_disable() 再清除。TCO=1
 * 临时抑制 tag checking；它不修改地址 tag 或内存 allocation tag。
 */
/*
 * 清除当前 CPU 的 TCO。无入参/返回、不睡眠；支持 MTE HW_TAGS 时恢复
 * tag check，否则 alternative 保持 nop。只修改本 CPU PSTATE。
 */
static inline void mte_disable_tco(void)
{
	asm volatile(ALTERNATIVE("nop", SET_PSTATE_TCO(0),
				 ARM64_MTE, CONFIG_KASAN_HW_TAGS));
}

/*
 * 设置当前 CPU 的 TCO，临时抑制 tag check。无入参/返回、不睡眠；
 * 调用者必须与 mte_disable_tco() 成对使用。
 */
static inline void mte_enable_tco(void)
{
	asm volatile(ALTERNATIVE("nop", SET_PSTATE_TCO(1),
				 ARM64_MTE, CONFIG_KASAN_HW_TAGS));
}

/*
 * These functions disable tag checking only if in MTE async mode
 * since the sync mode generates exceptions synchronously and the
 * nofault or load_unaligned_zeropad can handle them.
 */
/*
 * 仅 async/asymm 模式需要围绕 nofault 或未对齐零填充访问切换 TCO；
 * sync 模式立即产生异常，现有异常表能够处理。两个 helper 均无
 * 入参/返回、不睡眠；静态键为 false 时无副作用。
 */
static inline void __mte_disable_tco_async(void)
{
	if (system_uses_mte_async_or_asymm_mode())
		mte_disable_tco();
}

static inline void __mte_enable_tco_async(void)
{
	if (system_uses_mte_async_or_asymm_mode())
		mte_enable_tco();
}

/*
 * These functions are meant to be only used from KASAN runtime through
 * the arch_*() interface defined in asm/memory.h.
 * These functions don't include system_supports_mte() checks,
 * as KASAN only calls them when MTE is supported and enabled.
 */
/*
 * 以下接口只允许 KASAN runtime 经 asm/memory.h 的 arch_* 钩子调用。
 * 它们不重复 system_supports_mte() 检查：KASAN 保证 MTE 已检测并启用，
 * 从而省去热路径分支；绕过前置条件可能在不支持 CPU 上触发异常。
 */

/*
 * 把指针 top-byte logical tag 转为 KASAN 0xF<x> 格式。ptr 仅作为借用
 * 地址数值读取，不解引用；任意上下文不睡眠。返回 u8 tag，无副作用。
 */
static inline u8 mte_get_ptr_tag(void *ptr)
{
	/* Note: The format of KASAN tags is 0xF<x> */
	/* KASAN tag 高半字节固定为 F，低半字节 x 来自 MTE logical tag。 */
	u8 tag = 0xF0 | (u8)(((u64)(ptr)) >> MTE_TAG_SHIFT);

	return tag;
}

/* Get allocation tag for the address. */
/*
 * 读取 addr 所在 MTE granule 的 allocation tag。addr 是非 NULL、可执行
 * LDG 的借用地址；函数不取得引用。LDG 把内存 tag 放入返回指针 tag 位，
 * 再转换为 KASAN 格式。不睡眠，不修改普通内存内容。
 */
static inline u8 mte_get_mem_tag(void *addr)
{
	asm(__MTE_PREAMBLE "ldg %0, [%0]"
		: "+r" (addr));

	return mte_get_ptr_tag(addr);
}

/* Generate a random tag. */
/*
 * 通过 IRG 生成 logical tag。无入参、不睡眠；addr 仅承接指令输出，
 * 并非待解引用对象。返回 KASAN 格式 tag，无内存副作用。
 */
static inline u8 mte_get_random_tag(void)
{
	void *addr;

	asm(__MTE_PREAMBLE "irg %0, %0"
		: "=r" (addr));

	return mte_get_ptr_tag(addr);
}

/*
 * 为 p 指向的一个 16-byte granule 写 allocation tag，并返回 p+16。
 * STG 不初始化普通数据。p 必须有效对齐；memory clobber 阻止编译器把
 * 周围内存访问跨过指令。无 errno，调用者负责范围与并发排他。
 */
static inline u64 __stg_post(u64 p)
{
	asm volatile(__MTE_PREAMBLE "stg %0, [%0], #16"
		     : "+r"(p)
		     :
		     : "memory");
	return p;
}

/*
 * 与 __stg_post() 相同，但 STZG 还把该 granule 普通数据清零。
 * 返回下一 granule 地址，具有内存写副作用且不睡眠。
 */
static inline u64 __stzg_post(u64 p)
{
	asm volatile(__MTE_PREAMBLE "stzg %0, [%0], #16"
		     : "+r"(p)
		     :
		     : "memory");
	return p;
}

/*
 * 用 DC GVA 为 p 所在整块设置 allocation tag。p 是带 tag 的有效块地址，
 * 块大小来自 DCZID_EL0；无返回、不睡眠，普通数据不清零。
 */
static inline void __dc_gva(u64 p)
{
	asm volatile(__MTE_PREAMBLE "dc gva, %0" : : "r"(p) : "memory");
}

/*
 * 与 __dc_gva() 相同，但 DC GZVA 同时清零块内普通数据。无返回值。
 */
static inline void __dc_gzva(u64 p)
{
	asm volatile(__MTE_PREAMBLE "dc gzva, %0" : : "r"(p) : "memory");
}

/*
 * Assign allocation tags for a region of memory based on the pointer tag.
 * Note: The address must be non-NULL and MTE_GRANULE_SIZE aligned and
 * size must be MTE_GRANULE_SIZE aligned.
 */
/*
 * 批量设置一段内存的 allocation tag。addr 是借用的非 NULL 起始地址，
 * addr/size 均须按 MTE_GRANULE_SIZE 对齐，size 单位字节；tag 是写入值，
 * init=true 时同时清零普通数据。调用者保证范围有效、MTE 已启用，并排除
 * 同时访问该范围 tag/data 的并发方。
 *
 * 函数不可睡眠、无返回值。头尾用 STG/STZG，中间完整大块在允许时用
 * DC GVA/GZVA。完成后整个 [addr, addr+size) tag 一致；无失败回滚接口。
 */
static inline void mte_set_mem_tag_range(void *addr, size_t size, u8 tag,
					 bool init)
{
	/*
	 * curr 是当前带 tag 地址；dczid/bs/dzp 描述 DC 块能力；mask 为块内
	 * 偏移掩码；end1 是首块边界，end2 是尾块起点，end3 是范围
	 * 开区间末端。
	 */
	u64 curr, mask, dczid, dczid_bs, dczid_dzp, end1, end2, end3;

	/* Read DC G(Z)VA block size from the system register. */
	/*
	 * DCZID_EL0 低四位 BS 以 4 字节为基准编码块大小，DZP 表示
	 * 禁止块操作。
	 */
	dczid = read_cpuid(DCZID_EL0);
	dczid_bs = 4ul << (dczid & 0xf);
	dczid_dzp = (dczid >> 4) & 1;

	curr = (u64)__tag_set(addr, tag);
	mask = dczid_bs - 1;
	/* STG/STZG up to the end of the first block. */
	/* 头部逐 granule 处理，避免块指令越过输入范围起点。 */
	end1 = curr | mask;
	end3 = curr + size;
	/* DC GVA / GZVA in [end1, end2) */
	/* 中间完整块使用 DC，end2 之后的尾部再由逐 granule 指令收尾。 */
	end2 = end3 & ~mask;

	/*
	 * The following code uses STG on the first DC GVA block even if the
	 * start address is aligned - it appears to be faster than an alignment
	 * check + conditional branch. Also, if the range size is at least 2 DC
	 * GVA blocks, the first two loops can use post-condition to save one
	 * branch each.
	 */
	/*
	 * 即使起点块对齐，首块仍用 STG，实测比对齐检查加条件跳转更快。
	 * 范围至少两块时，头部和中段 do/while 各省一次入口分支；
	 * 小范围统一由最后的 STG 循环处理。局部宏通过两组函数参数在
	 * “仅 tag”和“tag+清零”指令族之间选择，函数末立即 undef，
	 * 避免污染包含者。
	 */
#define SET_MEMTAG_RANGE(stg_post, dc_gva)		\
	do {						\
		if (!dczid_dzp && size >= 2 * dczid_bs) {\
			do {				\
				curr = stg_post(curr);	\
			} while (curr < end1);		\
							\
			do {				\
				dc_gva(curr);		\
				curr += dczid_bs;	\
			} while (curr < end2);		\
		}					\
							\
		while (curr < end3)			\
			curr = stg_post(curr);		\
	} while (0)

	/* init 决定整条路径是否同时初始化普通数据。 */
	if (init)
		SET_MEMTAG_RANGE(__stzg_post, __dc_gzva);
	else
		SET_MEMTAG_RANGE(__stg_post, __dc_gva);
#undef SET_MEMTAG_RANGE
}

/*
 * KASAN MTE fault-mode 初始化入口，由实现文件在 CPU/系统初始化阶段定义。
 * sync、async、asymm 均无入参/返回并修改 MTE 检查模式；store_only 返回
 * 0 或 errno。调用者必须已确认 MTE 支持，初始化实现负责必要的 CPU 状态
 * 和静态键发布。
 */
void mte_enable_kernel_sync(void);
void mte_enable_kernel_async(void);
void mte_enable_kernel_asymm(void);
int mte_enable_kernel_store_only(void);

#else /* CONFIG_ARM64_MTE */

/*
 * CONFIG_ARM64_MTE=n 的兼容桩组：
 *
 * TCO 和 mode enable 的 void 函数均无动作；get_ptr/get_mem/get_random
 * 固定返回 0xFF；set_range 忽略借用地址、字节长度、tag 和 init，不写
 * 内存；store_only 返回 -EINVAL。所有桩均不睡眠、无 ownership 转移。
 */
static inline void mte_disable_tco(void)
{
}

static inline void mte_enable_tco(void)
{
}

static inline void __mte_disable_tco_async(void)
{
}

static inline void __mte_enable_tco_async(void)
{
}

static inline u8 mte_get_ptr_tag(void *ptr)
{
	return 0xFF;
}

static inline u8 mte_get_mem_tag(void *addr)
{
	return 0xFF;
}

static inline u8 mte_get_random_tag(void)
{
	return 0xFF;
}

static inline void mte_set_mem_tag_range(void *addr, size_t size,
						u8 tag, bool init)
{
}

static inline void mte_enable_kernel_sync(void)
{
}

static inline void mte_enable_kernel_async(void)
{
}

static inline void mte_enable_kernel_asymm(void)
{
}

static inline int mte_enable_kernel_store_only(void)
{
	return -EINVAL;
}

#endif /* CONFIG_ARM64_MTE */

#endif /* __ASSEMBLER__ */

#endif /* __ASM_MTE_KASAN_H  */
