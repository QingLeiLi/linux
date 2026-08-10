/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/barrier.h
 *
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_BARRIER_H
#define __ASM_BARRIER_H

#ifndef __ASSEMBLER__

#include <linux/kasan-checks.h>

#include <asm/alternative-macros.h>

/*
 * arm64 屏障与等待指令适配层：向通用内核提供事件等待、指令同步、CPU/DMA
 * 内存屏障、投机抑制以及 acquire/release 单次访问。ARM64 是弱内存序架构，
 * 屏障的共享域（sy/osh/ish）和访问类型（ld/st）决定真正保护的观察者；选择
 * 过弱会产生跨 CPU/设备不可见，选择过强则增加流水线与互连开销。
 */

#define __nops(n)	".rept	" #n "\nnop\n.endr\n"
#define nops(n)		asm volatile(__nops(n))

/* __nops(@n) 生成 @n 条汇编 nop 文本，nops() 将其作为不可删除的内联汇编发出。 */

#define sev()		asm volatile("sev" : : : "memory")
#define wfe()		asm volatile("wfe" : : : "memory")
#define wfet(val)	asm volatile("msr s0_3_c1_c0_0, %0"	\
				     : : "r" (val) : "memory")
#define wfi()		asm volatile("wfi" : : : "memory")
#define wfit(val)	asm volatile("msr s0_3_c1_c0_1, %0"	\
				     : : "r" (val) : "memory")

/*
 * sev() 向处理器系统发送事件，唤醒以 wfe() 等待的 CPU；wfe/wfi 分别等待
 * 事件/中断，wfet/wfit 通过系统寄存器接口携带超时值。@val 只求值一次。
 * "memory" clobber 阻止编译器跨等待点移动访问，但事件本身不发布共享数据；
 * 锁协议仍须在 sev 前和唤醒后使用匹配的 release/acquire。
 */

#define isb()		asm volatile("isb" : : : "memory")
#define dmb(opt)	asm volatile("dmb " #opt : : : "memory")
#define dsb(opt)	asm volatile("dsb " #opt : : : "memory")

/*
 * isb() 刷新指令流水线，使此前系统状态变更影响后续取指；dmb(@opt) 只保证
 * 指定域/类型的内存访问观察顺序；dsb(@opt) 还等待此前访问完成。@opt 是汇编
 * token 而非运行时值。三者都不睡眠、不取得对象所有权，不能互相随意替换。
 */

#define psb_csync()	asm volatile("hint #17" : : : "memory")
#define __tsb_csync()	asm volatile("hint #18" : : : "memory")
#define csdb()		asm volatile("hint #20" : : : "memory")

/*
 * psb_csync()/__tsb_csync() 分别用于性能监控/跟踪同步，csdb() 建立条件选择
 * 后的数据值投机边界。这些以 HINT 编码的指令在不支持的 CPU 上可安全退化，
 * 支持的 CPU 则赋予架构规定的同步语义。
 */

/*
 * Data Gathering Hint:
 * This instruction prevents merging memory accesses with Normal-NC or
 * Device-GRE attributes before the hint instruction with any memory accesses
 * appearing after the hint instruction.
 */
/*
 * Data Gathering Hint：阻止提示之前、具有 Normal-NC 或 Device-GRE 属性的
 * 内存访问与提示之后的访问合并。它划分 write-combining/聚合批次，不等同于
 * 等待全部访问完成的 dsb，也不单独提供跨 CPU 的发布—获取关系。
 */
#define dgh()		asm volatile("hint #6" : : : "memory")

#define spec_bar()	asm volatile(ALTERNATIVE("dsb nsh\nisb\n",		\
						 SB_BARRIER_INSN"nop\n",	\
						 ARM64_HAS_SB))

#define gsb_ack()	asm volatile(GSB_ACK_BARRIER_INSN : : : "memory")
#define gsb_sys()	asm volatile(GSB_SYS_BARRIER_INSN : : : "memory")

/*
 * spec_bar() 按 ARM64_HAS_SB 能力在 dsb nsh+isb 与专用 SB 指令之间启动时替换，
 * 阻止投机越界；gsb_ack()/gsb_sys() 发出 Guarded Control Stack 的确认/系统域
 * 屏障。它们服务控制流/投机协议，不应被当作普通数据 dmb 的同义词。
 */

#ifdef CONFIG_ARM64_PSEUDO_NMI
#define pmr_sync()						\
	do {							\
		asm volatile(					\
		ALTERNATIVE_CB("dsb sy",			\
			       ARM64_HAS_GIC_PRIO_RELAXED_SYNC,	\
			       alt_cb_patch_nops)		\
		);						\
	} while(0)
#else
#define pmr_sync()	do {} while (0)
#endif

/*
 * Pseudo-NMI 构建中，pmr_sync() 在修改 GIC priority mask register 后执行同步；
 * 若 CPU 具备 relaxed sync 能力，ALTERNATIVE_CB 可把 dsb sy 替换为 nop。
 * 未启用 Pseudo-NMI 时没有该状态转换，宏为空。
 */

#define __mb()		dsb(sy)
#define __rmb()		dsb(ld)
#define __wmb()		dsb(st)

#define __dma_mb()	dmb(osh)
#define __dma_rmb()	dmb(oshld)
#define __dma_wmb()	dmb(oshst)

/*
 * __mb/rmb/wmb 用 system 域 dsb，既排序又等待相应全部/读/写访问完成；DMA
 * 版本用 outer-shareable 域 dmb，对 CPU 与外部共享设备排序而不要求同样的
 * 完成语义。asm-generic 会在这些原始接口外增加公共 API 与 KCSAN 插桩。
 */

#define io_stop_wc()	dgh()

#define tsb_csync()								\
	do {									\
		/*								\
		 * CPUs affected by Arm Erratum 2054223 or 2067961 needs	\
		 * another TSB to ensure the trace is flushed. The barriers	\
		 * don't have to be strictly back to back, as long as the	\
		 * CPU is in trace prohibited state.				\
		 */								\
		if (cpus_have_final_cap(ARM64_WORKAROUND_TSB_FLUSH_FAILURE))	\
			__tsb_csync();						\
		__tsb_csync();							\
	} while (0)

/*
 * tsb_csync() 刷新 trace buffer。受 Arm erratum 2054223/2067961 影响的 CPU
 * 需要额外执行一次 TSB；两条无需严格相邻，只要期间 CPU 保持禁止 trace 的
 * 状态。cpus_have_final_cap() 在能力最终确定后选择修复分支，宏无返回值。
 */

/*
 * Generate a mask for array_index__nospec() that is ~0UL when 0 <= idx < sz
 * and 0 otherwise.
 */
/*
 * 为 array_index__nospec() 生成掩码：0 <= @idx < @sz 时返回 ~0UL，否则返回 0。
 * 两个参数都是纯输入数值，无对象 ownership；函数不可睡眠。cmp+sbc 把无符号
 * 比较进位扩展成整字掩码，随后的 csdb() 阻止 CPU 投机使用尚未落实的掩码。
 */
#define array_index_mask_nospec array_index_mask_nospec
static inline unsigned long array_index_mask_nospec(unsigned long idx,
						    unsigned long sz)
{
	unsigned long mask;

	asm volatile(
	"	cmp	%1, %2\n"
	"	sbc	%0, xzr, xzr\n"
	: "=r" (mask)
	: "r" (idx), "Ir" (sz)
	: "cc");

	csdb();
	return mask;
}

/*
 * Ensure that reads of the counter are treated the same as memory reads
 * for the purposes of ordering by subsequent memory barriers.
 *
 * This insanity brought to you by speculative system register reads,
 * out-of-order memory accesses, sequence locks and Thomas Gleixner.
 *
 * https://lore.kernel.org/r/alpine.DEB.2.21.1902081950260.1662@nanos.tec.linutronix.de/
 */
/*
 * 保证系统计数器读取在后续内存屏障看来与普通内存读取相同。ARM64 可能投机
 * 读取系统寄存器，若它与乱序内存访问、seqcount 重试组合，屏障未必约束该值。
 * 这里从 @val 制造零值地址依赖并对 sp 做一次无结果加载，迫使计数值进入普通
 * 内存依赖链；这种看似反常的方案源于 Thomas Gleixner 对该竞态的分析，链接
 * 见原文。@val 只求值一次，宏不改变计数值或内存内容。
 */
#define arch_counter_enforce_ordering(val) do {				\
	u64 tmp, _val = (val);						\
									\
	asm volatile(							\
	"	eor	%0, %1, %1\n"					\
	"	add	%0, sp, %0\n"					\
	"	ldr	xzr, [%0]"					\
	: "=r" (tmp) : "r" (_val));					\
} while (0)

#define __smp_mb()	dmb(ish)
#define __smp_rmb()	dmb(ishld)
#define __smp_wmb()	dmb(ishst)

/* ARM64 CPU 间屏障使用 inner-shareable 域：完整、只读、只写分别选 ish/ishld/ishst。 */

#define __smp_store_release(p, v)					\
do {									\
	typeof(p) __p = (p);						\
	union { __unqual_scalar_typeof(*p) __val; char __c[1]; } __u =	\
		{ .__val = (__force __unqual_scalar_typeof(*p)) (v) };	\
	compiletime_assert_atomic_type(*p);				\
	kasan_check_write(__p, sizeof(*p));				\
	switch (sizeof(*p)) {						\
	case 1:								\
		asm volatile ("stlrb %w1, %0"				\
				: "=Q" (*__p)				\
				: "rZ" (*(__u8 *)__u.__c)		\
				: "memory");				\
		break;							\
	case 2:								\
		asm volatile ("stlrh %w1, %0"				\
				: "=Q" (*__p)				\
				: "rZ" (*(__u16 *)__u.__c)		\
				: "memory");				\
		break;							\
	case 4:								\
		asm volatile ("stlr %w1, %0"				\
				: "=Q" (*__p)				\
				: "rZ" (*(__u32 *)__u.__c)		\
				: "memory");				\
		break;							\
	case 8:								\
		asm volatile ("stlr %x1, %0"				\
				: "=Q" (*__p)				\
				: "rZ" (*(__u64 *)__u.__c)		\
				: "memory");				\
		break;							\
	}								\
} while (0)

/*
 * __smp_store_release(@p, @v) 先固定指针与值，验证 *p 是 1/2/4/8 字节原子类型，
 * 让 KASAN 检查写范围，再按宽度选择 stlrb/stlrh/stlr。STLR 保证调用点此前的
 * 内存访问先于发布值被其他 CPU 观察。union 提供不丢失位模式的分宽视图；@p
 * 借用且必须有效，@v 只求值一次，无返回值，不序列化多个发布者。
 */

#define __smp_load_acquire(p)						\
({									\
	union { __unqual_scalar_typeof(*p) __val; char __c[1]; } __u;	\
	typeof(p) __p = (p);						\
	compiletime_assert_atomic_type(*p);				\
	kasan_check_read(__p, sizeof(*p));				\
	switch (sizeof(*p)) {						\
	case 1:								\
		asm volatile ("ldarb %w0, %1"				\
			: "=r" (*(__u8 *)__u.__c)			\
			: "Q" (*__p) : "memory");			\
		break;							\
	case 2:								\
		asm volatile ("ldarh %w0, %1"				\
			: "=r" (*(__u16 *)__u.__c)			\
			: "Q" (*__p) : "memory");			\
		break;							\
	case 4:								\
		asm volatile ("ldar %w0, %1"				\
			: "=r" (*(__u32 *)__u.__c)			\
			: "Q" (*__p) : "memory");			\
		break;							\
	case 8:								\
		asm volatile ("ldar %0, %1"				\
			: "=r" (*(__u64 *)__u.__c)			\
			: "Q" (*__p) : "memory");			\
		break;							\
	}								\
	(typeof(*p))__u.__val;						\
})

/*
 * __smp_load_acquire(@p) 固定借用指针、验证原子宽度并让 KASAN 检查读范围，
 * 再以 ldarb/ldarh/ldar 取得 1/2/4/8 字节快照。LDAR 保证后续访问不会越过
 * 本次读取；只有读到 release 发布值时才能消费发布前数据。返回 *p 原类型值，
 * 不增加引用，也不阻止对象随后变化或释放。
 */

#define smp_cond_load_relaxed(ptr, cond_expr)				\
({									\
	typeof(ptr) __PTR = (ptr);					\
	__unqual_scalar_typeof(*ptr) VAL;				\
	for (;;) {							\
		VAL = READ_ONCE(*__PTR);				\
		if (cond_expr)						\
			break;						\
		__cmpwait_relaxed(__PTR, VAL);				\
	}								\
	(typeof(*ptr))VAL;						\
})

#define smp_cond_load_acquire(ptr, cond_expr)				\
({									\
	typeof(ptr) __PTR = (ptr);					\
	__unqual_scalar_typeof(*ptr) VAL;				\
	for (;;) {							\
		VAL = smp_load_acquire(__PTR);				\
		if (cond_expr)						\
			break;						\
		__cmpwait_relaxed(__PTR, VAL);				\
	}								\
	(typeof(*ptr))VAL;						\
})

/*
 * 两个条件加载宏都把 @ptr 求值一次并循环观察 VAL。relaxed 版用 READ_ONCE，
 * acquire 版每轮用 smp_load_acquire；条件未满足时 __cmpwait_relaxed() 可利用
 * WFE/监视机制降低忙等流量，值变化后重试。@cond_expr 每轮求值且可引用 VAL；
 * 返回首个满足条件的快照，不超时、不调度，也不取得目标对象引用。
 */

#include <asm-generic/barrier.h>

#endif	/* __ASSEMBLER__ */

#endif	/* __ASM_BARRIER_H */
