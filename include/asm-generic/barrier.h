/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Generic barrier definitions.
 *
 * It should be possible to use these on really simple architectures,
 * but it serves more as a starting point for new ports.
 *
 * Copyright (C) 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
/*
 * 通用内存屏障定义。
 *
 * 这些实现理论上可直接用于顺序性很强的简单体系结构，但更重要的角色是新架构
 * 移植的起点：体系结构先定义带 __ 前缀的原始硬件屏障，本文件再包上 KCSAN
 * 插桩并补齐统一 API；未提供的能力才逐级回退。版权与作者信息见上方原文。
 *
 * 层次地图：
 *   barrier()       只阻止编译器重排，不保证 CPU、设备或宿主机观察顺序；
 *   mb/rmb/wmb      面向 CPU 与设备的严格屏障，UP 与设备交互时仍可能需要；
 *   smp_*           面向同一内核中的 CPU 间同步，非 SMP 构建可降为编译器屏障；
 *   dma_*           面向 CPU 与 DMA 设备共享内存；
 *   virt_*          客体即使是 UP，也按 SMP 宿主可能并行处理共享状态来排序。
 * 所有宏都不管理对象生命周期或锁 ownership；正确性取决于写端与读端选择语义
 * 匹配的屏障，并用 READ_ONCE()/WRITE_ONCE() 保证相应单次访问。
 */
#ifndef __ASM_GENERIC_BARRIER_H
#define __ASM_GENERIC_BARRIER_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>
#include <linux/kcsan-checks.h>
#include <asm/rwonce.h>

#ifndef nop
#define nop()	asm volatile ("nop")
#endif

/*
 * nop() 发出一条体系结构空操作指令。它不读写 C 对象、无返回值且不睡眠；
 * 因为没有 "memory" clobber，它也不是编译器或 CPU 内存屏障。体系结构可提前
 * 定义更合适的形式，通用版本主要供短暂延迟、补丁槽或指令布局用途。
 */

/*
 * Architectures that want generic instrumentation can define __ prefixed
 * variants of all barriers.
 */
/*
 * 希望复用通用插桩的体系结构可定义所有带 __ 前缀的原始屏障。本层检测这些
 * 实现后生成无前缀公共接口：先通知 KCSAN，再执行真实屏障，从而同时满足硬件
 * 顺序，并让弱内存检测插桩认识这个屏障边界。
 */

#ifdef __mb
#define mb()	do { kcsan_mb(); __mb(); } while (0)
#endif

#ifdef __rmb
#define rmb()	do { kcsan_rmb(); __rmb(); } while (0)
#endif

#ifdef __wmb
#define wmb()	do { kcsan_wmb(); __wmb(); } while (0)
#endif

/*
 * __mb/__rmb/__wmb 分别提供完整、读、写硬件顺序；存在时生成 mb/rmb/wmb。
 * 三者无入参和返回值，不睡眠：KCSAN 调用只描述同步边，__* 才承担实际硬件
 * 约束。do/while(0) 保证宏在 if/else 中像单条语句使用。
 */

#ifdef __dma_mb
#define dma_mb()	do { kcsan_mb(); __dma_mb(); } while (0)
#endif

#ifdef __dma_rmb
#define dma_rmb()	do { kcsan_rmb(); __dma_rmb(); } while (0)
#endif

#ifdef __dma_wmb
#define dma_wmb()	do { kcsan_wmb(); __dma_wmb(); } while (0)
#endif

/*
 * __dma_mb/__dma_rmb/__dma_wmb 是体系结构为一致性 DMA 共享内存提供的原始
 * 完整/读/写屏障；公共 dma_* 同样先让 KCSAN 建模。它们排序的是 CPU 与设备
 * 对 DMA 内存的观察，不能替代 MMIO accessor、cache maintenance 或设备协议
 * 要求的 doorbell 顺序；是否需要这些额外动作由体系结构和驱动契约决定。
 */

/*
 * Force strict CPU ordering. And yes, this is required on UP too when we're
 * talking to devices.
 *
 * Fall back to compiler barriers if nothing better is provided.
 */
/*
 * 强制严格 CPU 次序；与设备通信时，即使单处理器构建也需要硬件顺序。若体系
 * 结构没有更强实现，只能退回编译器屏障。这一兜底适合确实不乱序的简单架构，
 * 新架构若硬件会乱序却沿用它，会导致驱动或共享内存协议在运行时失效。
 */

#ifndef mb
#define mb()	barrier()
#endif

#ifndef rmb
#define rmb()	mb()
#endif

#ifndef wmb
#define wmb()	mb()
#endif

/*
 * mb() 默认采用 compiler barrier；rmb()/wmb() 再保守复用完整屏障。三者均
 * 无入参/返回，既不读取条件也不等待。体系结构通常覆盖这些定义来发出真实
 * fence；调用者不得因通用源码写着 barrier() 就推断目标架构没有硬件栅栏。
 */

#ifndef dma_mb
#define dma_mb()	mb()
#endif

#ifndef dma_rmb
#define dma_rmb()	rmb()
#endif

#ifndef dma_wmb
#define dma_wmb()	wmb()
#endif

/*
 * 未提供 DMA 专用屏障时，dma_mb/rmb/wmb 分别复用普通完整/读/写屏障。这是
 * “普通屏障足以排序 DMA 可见内存”的保守接口映射，不表示普通缓存内存自动
 * 变为 DMA coherent，也不完成所有权在 CPU 与设备之间的显式同步。
 */

#ifndef __smp_mb
#define __smp_mb()	mb()
#endif

#ifndef __smp_rmb
#define __smp_rmb()	rmb()
#endif

#ifndef __smp_wmb
#define __smp_wmb()	wmb()
#endif

/*
 * __smp_mb/rmb/wmb 是不含 KCSAN 包装的 CPU 间原始层，默认映射到普通屏障。
 * virt_* 与公共 smp_* 会复用它们，因此体系结构覆盖时必须保持相同的完整/读/
 * 写排序契约。这里的双下划线表示“内部原语”，不是“更强屏障”。
 */

#ifdef CONFIG_SMP

#ifndef smp_mb
#define smp_mb()	do { kcsan_mb(); __smp_mb(); } while (0)
#endif

#ifndef smp_rmb
#define smp_rmb()	do { kcsan_rmb(); __smp_rmb(); } while (0)
#endif

#ifndef smp_wmb
#define smp_wmb()	do { kcsan_wmb(); __smp_wmb(); } while (0)
#endif

/*
 * SMP 构建中的 smp_mb/rmb/wmb 先向 KCSAN 发出完整/读/写同步事件，再调用
 * 体系结构 __smp_*。它们用于 CPU 之间共享普通内存的协议；每个屏障必须在
 * 对端有能够建立所需顺序的配对访问或屏障，单独插入并不会消除数据竞争。
 */

#else	/* !CONFIG_SMP */

#ifndef smp_mb
#define smp_mb()	barrier()
#endif

#ifndef smp_rmb
#define smp_rmb()	barrier()
#endif

#ifndef smp_wmb
#define smp_wmb()	barrier()
#endif

/*
 * 非 SMP 构建没有另一个内核 CPU 与本 CPU 并行观察普通内存，所以 smp_*
 * 退化为编译器屏障，保留源级顺序而不发硬件 fence。该优化不适用于设备和
 * SMP 宿主；相应场景必须使用 mb()/dma_*() 或 virt_*()。
 */

#endif	/* CONFIG_SMP */

#ifndef __smp_store_mb
#define __smp_store_mb(var, value)  do { WRITE_ONCE(var, value); __smp_mb(); } while (0)
#endif

/*
 * __smp_store_mb(@var, @value) 先用 WRITE_ONCE 对 @var 做一次编译器可见的
 * 单次写，随后执行完整屏障，因此“该写”先于屏障后的所有访问被观察。@var
 * 应是体系结构能够按协议原子访问的标量左值，@value 求值一次；宏无返回值。
 * WRITE_ONCE 本身不锁住变量，非自然对齐/非原生字长数据也不能仅凭本宏假定
 * 无撕裂，多个写者仍需各自的并发协议。
 */

#ifndef __smp_mb__before_atomic
#define __smp_mb__before_atomic()	__smp_mb()
#endif

#ifndef __smp_mb__after_atomic
#define __smp_mb__after_atomic()	__smp_mb()
#endif

/*
 * __smp_mb__before_atomic()/__smp_mb__after_atomic() 为“原子 RMW 本身没有所需
 * 顺序”时补上完整屏障，分别约束原子操作之前或之后的普通访问。通用实现最
 * 保守地使用 __smp_mb()；体系结构可利用原子指令自带语义给出更轻实现。
 * 它们必须紧邻目标原子操作使用，不能把远处无关原子操作误当作配对边界。
 */

#ifndef __smp_store_release
#define __smp_store_release(p, v)					\
do {									\
	compiletime_assert_atomic_type(*p);				\
	__smp_mb();							\
	WRITE_ONCE(*p, v);						\
} while (0)
#endif

/*
 * __smp_store_release(@p, @v) 发布一个标量状态：先用编译期断言拒绝可能撕裂的
 * 非原子大小类型，再以完整屏障保证此前所有读写不会越过发布点，最后通过
 * WRITE_ONCE 写入 *p。@p 是借用的非 NULL 指针，@v 求值一次；无返回值且不
 * 获取引用。对端通常以 smp_load_acquire(p) 读到该值，随后才可消费发布前
 * 初始化的数据。通用实现强于最低 release 要求，但不使并发写者自动安全。
 */

#ifndef __smp_load_acquire
#define __smp_load_acquire(p)						\
({									\
	__unqual_scalar_typeof(*p) ___p1 = READ_ONCE(*p);		\
	compiletime_assert_atomic_type(*p);				\
	__smp_mb();							\
	(typeof(*p))___p1;						\
})
#endif

/*
 * __smp_load_acquire(@p) 先用 READ_ONCE 取得 *p 并保存到去限定标量 ___p1，
 * 再检查类型可原子访问并执行完整屏障，保证随后的读写不能跑到这次观察之前。
 * 宏返回与 *p 相同类型的快照；@p 只借用、不延长对象生命周期。若读到发布端
 * smp_store_release() 写入的值，二者建立发布—获取顺序；未读到该值时不能
 * 凭空获得发布端初始化数据的可见性。
 */

#ifdef CONFIG_SMP

#ifndef smp_store_mb
#define smp_store_mb(var, value)  do { kcsan_mb(); __smp_store_mb(var, value); } while (0)
#endif

#ifndef smp_mb__before_atomic
#define smp_mb__before_atomic()	do { kcsan_mb(); __smp_mb__before_atomic(); } while (0)
#endif

#ifndef smp_mb__after_atomic
#define smp_mb__after_atomic()	do { kcsan_mb(); __smp_mb__after_atomic(); } while (0)
#endif

#ifndef smp_store_release
#define smp_store_release(p, v) do { kcsan_release(); __smp_store_release(p, v); } while (0)
#endif

#ifndef smp_load_acquire
#define smp_load_acquire(p) __smp_load_acquire(p)
#endif

/*
 * SMP 公共包装：smp_store_mb、原子前后屏障和 smp_store_release 先向 KCSAN
 * 报告相应同步，再执行内部实现；smp_load_acquire 的 READ_ONCE 路径已承担
 * 工具可见访问并直接复用内部宏。接口不睡眠，适用于进程、中断等原子上下文，
 * 但调用者仍须保证 @p 指向对象在整个访问窗口内存活。
 */

#else	/* !CONFIG_SMP */

#ifndef smp_store_mb
#define smp_store_mb(var, value)  do { WRITE_ONCE(var, value); barrier(); } while (0)
#endif

#ifndef smp_mb__before_atomic
#define smp_mb__before_atomic()	barrier()
#endif

#ifndef smp_mb__after_atomic
#define smp_mb__after_atomic()	barrier()
#endif

#ifndef smp_store_release
#define smp_store_release(p, v)						\
do {									\
	barrier();							\
	WRITE_ONCE(*p, v);						\
} while (0)
#endif

#ifndef smp_load_acquire
#define smp_load_acquire(p)						\
({									\
	__unqual_scalar_typeof(*p) ___p1 = READ_ONCE(*p);		\
	barrier();							\
	(typeof(*p))___p1;						\
})
#endif

/*
 * 非 SMP 构建保留同一源级契约但省去硬件跨 CPU fence：store_mb 是一次
 * WRITE_ONCE 后接 compiler barrier；原子前后接口均为 compiler barrier；
 * release 在写前、acquire 在读后放 barrier()。这仍阻止编译器跨发布/获取点
 * 移动访问。它们不排序设备或 SMP 宿主的访问，后两种场景不能使用该退化假设。
 */

#endif	/* CONFIG_SMP */

/* Barriers for virtual machine guests when talking to an SMP host */
/* 虚拟机客体与 SMP 宿主通信所用的屏障。 */
#define virt_mb() do { kcsan_mb(); __smp_mb(); } while (0)
#define virt_rmb() do { kcsan_rmb(); __smp_rmb(); } while (0)
#define virt_wmb() do { kcsan_wmb(); __smp_wmb(); } while (0)
#define virt_store_mb(var, value) do { kcsan_mb(); __smp_store_mb(var, value); } while (0)
#define virt_mb__before_atomic() do { kcsan_mb(); __smp_mb__before_atomic(); } while (0)
#define virt_mb__after_atomic()	do { kcsan_mb(); __smp_mb__after_atomic(); } while (0)
#define virt_store_release(p, v) do { kcsan_release(); __smp_store_release(p, v); } while (0)
#define virt_load_acquire(p) __smp_load_acquire(p)

/*
 * virt_mb/rmb/wmb 以及 store/atomic/release/acquire 变体始终调用 __smp_*
 * 原始层，即使客体内核自身按 UP 构建也不退化为 barrier()：宿主的多个 CPU
 * 仍可能并行处理共享 vring、steal-time 等状态。参数、返回及原子类型限制与
 * 对应 smp_* 相同；KCSAN 包装让检测器看到同步，但对象生命周期仍由虚拟化
 * 协议管理。把 virt_* 错换为 UP smp_* 可能只在虚拟机压力下暴露乱序。
 */

/**
 * smp_acquire__after_ctrl_dep() - Provide ACQUIRE ordering after a control dependency
 *
 * A control dependency provides a LOAD->STORE order, the additional RMB
 * provides LOAD->LOAD order, together they provide LOAD->{LOAD,STORE} order,
 * aka. (load)-ACQUIRE.
 *
 * Architectures that do not do load speculation can have this be barrier().
 */
/*
 * smp_acquire__after_ctrl_dep() - 在控制依赖之后补齐 ACQUIRE 排序
 *
 * 控制依赖本身提供 LOAD→STORE 顺序，额外的读屏障提供 LOAD→LOAD 顺序，合并
 * 后得到 LOAD→{LOAD,STORE}，也就是一次 load-acquire。无入参/返回，不睡眠；
 * 必须紧跟“读取值并据此选择控制流”的成功分支，不能用于没有真实控制依赖的
 * 数据依赖。不会推测 load 的体系结构可覆盖为 barrier()，通用实现用 smp_rmb()。
 */
#ifndef smp_acquire__after_ctrl_dep
#define smp_acquire__after_ctrl_dep()		smp_rmb()
#endif

/**
 * smp_cond_load_relaxed() - (Spin) wait for cond with no ordering guarantees
 * @ptr: pointer to the variable to wait on
 * @cond_expr: boolean expression to wait for
 *
 * Equivalent to using READ_ONCE() on the condition variable.
 *
 * Due to C lacking lambda expressions we load the value of *ptr into a
 * pre-named variable @VAL to be used in @cond.
 */
/*
 * smp_cond_load_relaxed() - 自旋等待条件成立，不提供额外内存顺序
 * @ptr: 指向被轮询标量的借用指针；循环期间必须保持有效且可并发读取。
 * @cond_expr: 使用预命名变量 VAL 表达的布尔退出条件；每轮在更新 VAL 后求值。
 *
 * 语义等价于反复 READ_ONCE(*ptr)：宏先把 @ptr 求值一次保存到 __PTR，每轮把
 * 观察值写入去限定标量 VAL；条件为假时 cpu_relax() 给体系结构降低忙等功耗/
 * 总线压力的机会。C 没有 lambda，才把 VAL 暴露给调用点表达式。返回使条件
 * 首次为真的那个值，并转换回 *ptr 类型；不超时、不调度、不获取引用，也不
 * 保证返回后能看到生产者在状态写入前的其他数据。调用者必须确保最终会有写者，
 * 或在 @cond_expr 中加入超时/取消条件，并确认当前上下文允许持续自旋。
 */
#ifndef smp_cond_load_relaxed
#define smp_cond_load_relaxed(ptr, cond_expr) ({		\
	typeof(ptr) __PTR = (ptr);				\
	__unqual_scalar_typeof(*ptr) VAL;			\
	for (;;) {						\
		VAL = READ_ONCE(*__PTR);			\
		if (cond_expr)					\
			break;					\
		cpu_relax();					\
	}							\
	(typeof(*ptr))VAL;					\
})
#endif

/**
 * smp_cond_load_acquire() - (Spin) wait for cond with ACQUIRE ordering
 * @ptr: pointer to the variable to wait on
 * @cond_expr: boolean expression to wait for
 *
 * Equivalent to using smp_load_acquire() on the condition variable but employs
 * the control dependency of the wait to reduce the barrier on many platforms.
 */
/*
 * smp_cond_load_acquire() - 自旋等待条件成立，并对成功观察提供 ACQUIRE 顺序
 * @ptr: 被轮询标量的借用指针，非 NULL，生命周期覆盖整个自旋区间。
 * @cond_expr: 使用 VAL 判断成功的表达式；求值次数和副作用规则同 relaxed 版本。
 *
 * 它先由 smp_cond_load_relaxed() 返回满足条件的 _val，再利用“是否退出循环”的
 * 控制依赖调用 smp_acquire__after_ctrl_dep()。效果等价于在条件变量上使用
 * smp_load_acquire()，但许多体系结构可把最终屏障做得更轻。若生产者以 release
 * 发布该条件值，成功返回后调用者可读取发布前数据；返回值是满足条件的快照，
 * 不锁定对象，也不保证后续再次读取条件仍不变化。
 */
#ifndef smp_cond_load_acquire
#define smp_cond_load_acquire(ptr, cond_expr) ({		\
	__unqual_scalar_typeof(*ptr) _val;			\
	_val = smp_cond_load_relaxed(ptr, cond_expr);		\
	smp_acquire__after_ctrl_dep();				\
	(typeof(*ptr))_val;					\
})
#endif

/*
 * pmem_wmb() ensures that all stores for which the modification
 * are written to persistent storage by preceding instructions have
 * updated persistent storage before any data  access or data transfer
 * caused by subsequent instructions is initiated.
 */
/*
 * pmem_wmb() 保证：此前指令已要求写入持久介质的所有修改，必须先真正更新
 * 持久存储，随后指令引起的数据访问或传输才可开始。它描述的是“持久化完成”
 * 与后续动作的次序，不只是其他 CPU 的缓存可见性。通用实现退回 wmb()；具有
 * 易失缓存刷新、平台持久域等特殊要求的体系结构必须覆盖它。宏无入参/返回，
 * 也不会替调用者发出此前所需的逐 cacheline flush。
 */
#ifndef pmem_wmb
#define pmem_wmb()	wmb()
#endif

/*
 * ioremap_wc() maps I/O memory as memory with write-combining attributes. For
 * this kind of memory accesses, the CPU may wait for prior accesses to be
 * merged with subsequent ones. In some situation, such wait is bad for the
 * performance. io_stop_wc() can be used to prevent the merging of
 * write-combining memory accesses before this macro with those after it.
 */
/*
 * ioremap_wc() 把 I/O 内存映射为 write-combining 属性，CPU 可等待并合并前后
 * 多次写。某些协议或性能场景不希望前一批 WC 写继续与后一批合并，io_stop_wc()
 * 就在两批之间建立“停止合并”边界。默认空操作表示该架构无需额外指令；需要
 * 显式 drain/stop 的体系结构应覆盖。它无入参/返回，不等价于通用完成屏障，
 * 是否还需 wmb()/MMIO readback 取决于设备文档。
 */
#ifndef io_stop_wc
#define io_stop_wc() do { } while (0)
#endif

/*
 * Architectures that guarantee an implicit smp_mb() in switch_mm()
 * can override smp_mb__after_switch_mm.
 */
/*
 * 若体系结构保证 switch_mm() 已隐含完整 SMP 屏障，可覆盖
 * smp_mb__after_switch_mm() 以避免重复 fence。通用实现保守执行 smp_mb()，
 * 用于排序地址空间切换前后的共享状态观察。宏无入参/返回，必须在完成
 * switch_mm() 后的协议位置调用；只有能证明隐含屏障覆盖相同顺序边的架构才能
 * 将其弱化，不能仅因页表切换“看起来很重”就删除。
 */
#ifndef smp_mb__after_switch_mm
# define smp_mb__after_switch_mm()	smp_mb()
#endif

#endif /* !__ASSEMBLY__ */
#endif /* __ASM_GENERIC_BARRIER_H */
