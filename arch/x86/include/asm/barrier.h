/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_BARRIER_H
#define _ASM_X86_BARRIER_H

#include <asm/alternative.h>
#include <asm/nops.h>

/*
 * x86 内存序实现层：这里把架构指令封装成 asm-generic/barrier.h 所需的
 * __mb/__smp_* 等原始接口。x86 的普通 WB 内存模型较强，但设备、WC 内存、
 * 非临时写、投机执行和跨 CPU 原子协议仍需要显式屏障；“x86 强内存序”不能
 * 作为删除这些接口的理由。
 */

/*
 * Force strict CPU ordering.
 * And yes, this might be required on UP too when we're talking
 * to devices.
 */
/*
 * 强制严格 CPU 次序；与设备交互时，即使 UP 内核也可能需要。CONFIG_X86_32
 * 直接定义公共 mb/rmb/wmb：ALTERNATIVE 在启动时按 XMM2 能力选择旧 CPU 可用
 * 的锁定栈操作或 mfence/lfence/sfence。64 位则提供带 __ 前缀的原始实现，
 * 由 asm-generic 层增加 KCSAN 包装。"memory" 与 "cc" clobber 同时约束
 * 编译器内存访问和条件码；宏无入参、返回值，也不管理被排序对象的生命周期。
 */

#ifdef CONFIG_X86_32
#define mb() asm volatile(ALTERNATIVE("lock addl $0,-4(%%esp)", "mfence", \
				      X86_FEATURE_XMM2) ::: "memory", "cc")
#define rmb() asm volatile(ALTERNATIVE("lock addl $0,-4(%%esp)", "lfence", \
				       X86_FEATURE_XMM2) ::: "memory", "cc")
#define wmb() asm volatile(ALTERNATIVE("lock addl $0,-4(%%esp)", "sfence", \
				       X86_FEATURE_XMM2) ::: "memory", "cc")
#else
#define __mb()	asm volatile("mfence":::"memory")
#define __rmb()	asm volatile("lfence":::"memory")
#define __wmb()	asm volatile("sfence" ::: "memory")
#endif

/**
 * array_index_mask_nospec() - generate a mask that is ~0UL when the
 * 	bounds check succeeds and 0 otherwise
 * @index: array element index
 * @size: number of elements in array
 *
 * Returns:
 *     0 - (index < size)
 */
/*
 * array_index_mask_nospec() - 为投机安全数组索引生成全零或全一掩码
 * @idx: 待检查的无符号数组索引，只求值一次。
 * @sz: 元素个数，只求值一次；必须与 @idx 采用可正确比较的整数类型。
 *
 * cmp 设置标志，sbb reg,reg 把“发生借位”扩展成 ~0UL，否则得到 0。调用者把
 * mask 与索引相与，使错误索引在投机窗口也不能选择越界地址。返回值只编码
 * idx < sz，不检查实际数组、对象生命周期或访问权限；整个宏不睡眠。
 */
#define array_index_mask_nospec(idx,sz) ({	\
	typeof((idx)+(sz)) __idx = (idx);	\
	typeof(__idx) __sz = (sz);		\
	unsigned long __mask;			\
	asm volatile ("cmp %1,%2; sbb %0,%0"	\
			:"=r" (__mask)		\
			:ASM_INPUT_G (__sz),	\
			 "r" (__idx)		\
			:"cc");			\
	__mask; })

/* Prevent speculative execution past this barrier. */
/*
 * 阻止投机执行越过此点。ALTERNATIVE 仅在 CPU 具备 LFENCE_RDTSC 语义时修补
 * 为 lfence，否则为空；它用于已验证条件后的推测执行边界，不替代普通的数据
 * 可见性屏障，也不修复缺失的边界检查。
 */
#define barrier_nospec() alternative("", "lfence", X86_FEATURE_LFENCE_RDTSC)

#define __dma_rmb()	barrier()
#define __dma_wmb()	barrier()

/*
 * x86 对一致性 DMA 内存的读/写次序由硬件模型保证，因此 DMA rmb/wmb 只需
 * compiler barrier 防止编译器重排。它们不负责刷新非一致性缓存，也不覆盖
 * MMIO、WC 或设备 doorbell 的额外规则。
 */

#define __smp_mb()	asm volatile("lock addl $0,-4(%%" _ASM_SP ")" ::: "memory", "cc")

#define __smp_rmb()	dma_rmb()
#define __smp_wmb()	barrier()
#define __smp_store_mb(var, value) do { (void)xchg(&var, value); } while (0)

/*
 * __smp_mb() 用带 lock 前缀的栈上零加法获得完整序列化，而不改变有效数据；
 * __smp_rmb/wmb 利用 x86 的强 load/load 与 store/store 次序，仅保留所需的
 * DMA/编译器约束。__smp_store_mb() 用隐含 lock 的 xchg 原子写入 @var，令写
 * 前后都具备完整屏障语义。所有接口均不可睡眠，调用者仍需保证访问地址有效。
 */

#define __smp_store_release(p, v)					\
do {									\
	compiletime_assert_atomic_type(*p);				\
	barrier();							\
	WRITE_ONCE(*p, v);						\
} while (0)

#define __smp_load_acquire(p)						\
({									\
	typeof(*p) ___p1 = READ_ONCE(*p);				\
	compiletime_assert_atomic_type(*p);				\
	barrier();							\
	___p1;								\
})

/*
 * x86 的普通存储/加载已经满足 release/acquire 所需硬件次序，因此实现只在
 * release 写前、acquire 读后放 compiler barrier，并用 WRITE_ONCE/READ_ONCE
 * 保证一次访问。compiletime_assert_atomic_type() 拒绝非原生原子宽度；@p 是
 * 借用且必须非 NULL，宏不获取引用。对端只有在 acquire 读到 release 发布值时
 * 才能据此消费此前数据。
 */

/* Atomic operations are already serializing on x86 */
/* x86 原子 RMW 已自带所需序列化，所以原子操作前后的附加 SMP 屏障为空。 */
#define __smp_mb__before_atomic()	do { } while (0)
#define __smp_mb__after_atomic()	do { } while (0)

/* Writing to CR3 provides a full memory barrier in switch_mm(). */
/*
 * switch_mm() 写 CR3 已提供完整内存屏障，因此切换地址空间后的通用补充屏障
 * 可以为空；这个结论只适用于确实执行相应 x86 切换协议的位置。
 */
#define smp_mb__after_switch_mm()	do { } while (0)

#include <asm-generic/barrier.h>

#endif /* _ASM_X86_BARRIER_H */
