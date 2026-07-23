/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 cache maintenance 汇编原语学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * C 层把半开虚拟区间 [x0,x1) 交给这里，宏按运行时 cache-line 大小循环
 * 执行 DC/IC 指令。PoU（统一点）用于自修改代码的 D/I 一致性，PoC
 *（一致点）用于 CPU 与 DMA/其他观察者一致，PoP（持久化点）用于持久
 * 内存。IDC/DIC capability 可证明某些显式维护不需要，alternative 在
 * 启动时把分支改成最短硬件路径。
 *
 * `nosync` 变体只发 cache 指令，把最终 DSB 留给批量调用者；普通变体
 * 返回前完成屏障。用户地址版本临时打开 TTBR0/uaccess，并用异常表把
 * 页面 fault 转为 -EFAULT。所有函数只改变 cache 可见性，不修改页表、
 * 不分配内存，也不替代数据所有权协议所需的上层锁。
 */
/*
 * Cache maintenance
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/errno.h>
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/assembler.h>
#include <asm/cpufeature.h>
#include <asm/alternative.h>
#include <asm/asm-uaccess.h>

/*
 *	caches_clean_inval_pou_macro(start,end) [fixup]
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 *	- fixup   - optional label to branch to on user fault
 */
/*
 * 公共 D->I 同步宏：x0=start、x1=end，fixup 可为空或用户 fault 标签。
 * 无 IDC 时先 CVAU clean D 到 PoU；无 DIC 时再逐行 invalidate I。中间
 * x2..x5 为临时寄存器。IDC/DIC 路径仍保留 DSB/ISB，保证此前写入和后续
 * 取指顺序正确；\@ 生成每次宏展开唯一局部标签。
 */
.macro	caches_clean_inval_pou_macro, fixup
alternative_if ARM64_HAS_CACHE_IDC
	/* IDC 表示数据到 PoU 后 I-cache 自动一致，只需排序此前 store。 */
	dsb     ishst
	b       .Ldc_skip_\@
alternative_else_nop_endif
	mov     x2, x0
	mov     x3, x1
	dcache_by_line_op cvau, ish, x2, x3, x4, x5, \fixup
.Ldc_skip_\@:
alternative_if ARM64_HAS_CACHE_DIC
	/* DIC 表示无需显式 IC invalidate，但 ISB 仍刷新当前取指上下文。 */
	isb
	b	.Lic_skip_\@
alternative_else_nop_endif
	invalidate_icache_by_line x0, x1, x2, x3, \fixup
.Lic_skip_\@:
.endm

/*
 *	caches_clean_inval_pou(start,end)
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
/*
 * 内核地址版本：同步 [x0,x1) 后返回，无状态返回值。__pi 别名允许位置
 * 无关早期代码在重定位/alternative 完成前引用同一实现。
 */
SYM_FUNC_START(caches_clean_inval_pou)
	caches_clean_inval_pou_macro
	ret
SYM_FUNC_END(caches_clean_inval_pou)
SYM_FUNC_ALIAS(__pi_caches_clean_inval_pou, caches_clean_inval_pou)

/*
 *	caches_clean_inval_user_pou(start,end)
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
/*
 * 用户地址版本：x0/x1 为用户范围。临时启用 TTBR0 后运行同一宏；成功
 * x0=0，cache 指令访问 fault 时异常表跳 2f 并返回 -EFAULT。所有出口都
 * 经过标签 1 关闭 TTBR0，维持软件 PAN 不变量，不能从 fault 分支直接 ret。
 */
SYM_FUNC_START(caches_clean_inval_user_pou)
	uaccess_ttbr0_enable x2, x3, x4

	caches_clean_inval_pou_macro 2f
	mov	x0, xzr
1:
	uaccess_ttbr0_disable x1, x2
	ret
2:
	mov	x0, #-EFAULT
	b	1b
SYM_FUNC_END(caches_clean_inval_user_pou)

/*
 *	icache_inval_pou(start,end)
 *
 *	Ensure that the I cache is invalid within specified region.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
/*
 * 仅失效 [x0,x1) I-cache 到 PoU。DIC CPU 上无需逐行 IC，ISB 后返回；
 * 否则使用 x2/x3 临时寄存器循环。调用前数据侧必须已按协议 clean。
 */
SYM_FUNC_START(icache_inval_pou)
alternative_if ARM64_HAS_CACHE_DIC
	isb
	ret
alternative_else_nop_endif

	invalidate_icache_by_line x0, x1, x2, x3
	ret
SYM_FUNC_END(icache_inval_pou)

/*
 *	dcache_clean_inval_poc(start, end)
 *
 *	Ensure that any D-cache lines for the interval [start, end)
 *	are cleaned and invalidated to the PoC.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
/*
 * 对内核范围执行 DC CIVAC 到 PoC：先写回脏数据再失效本地副本，sy 域
 * 屏障使所有观察者在返回后看到结果。位置无关主体同时导出普通别名。
 */
SYM_FUNC_START(__pi_dcache_clean_inval_poc)
	dcache_by_line_op civac, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END(__pi_dcache_clean_inval_poc)
SYM_FUNC_ALIAS(dcache_clean_inval_poc, __pi_dcache_clean_inval_poc)

/*
 *	dcache_clean_pou(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoU.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
/*
 * clean D 到 PoU 但不 invalidate。IDC CPU 仅需 ishst DSB；否则逐行 CVAU
 * 并完成 inner-shareable 同步，典型消费者是随后执行的新写入代码。
 */
SYM_FUNC_START(dcache_clean_pou)
alternative_if ARM64_HAS_CACHE_IDC
	dsb	ishst
	ret
alternative_else_nop_endif
	dcache_by_line_op cvau, ish, x0, x1, x2, x3
	ret
SYM_FUNC_END(dcache_clean_pou)

/*
 * invalidate-to-PoC 的无最终屏障内部宏。边界非 cache-line 对齐时不能直接
 * IVAC 整行，否则会丢掉区间外同一行的脏字节，所以首尾 partial line 用
 * CIVAC 先写回再失效；完全覆盖的中间行用 IVAC。x2/x3 为 line-size/mask。
 */
.macro __dcache_inval_poc_nosync
	dcache_line_size x2, x3
	sub	x3, x2, #1
	tst	x1, x3				// end cache line aligned?
	bic	x1, x1, x3
	b.eq	1f
	dc	civac, x1			// clean & invalidate D / U line
1:	tst	x0, x3				// start cache line aligned?
	bic	x0, x0, x3
	b.eq	2f
	dc	civac, x0			// clean & invalidate D / U line
	b	3f
2:	dc	ivac, x0			// invalidate D / U line
3:	add	x0, x0, x2
	cmp	x0, x1
	b.lo	2b
.endm

/*
 *	dcache_inval_poc(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are invalidated. Any partial lines at the ends of the interval are
 *	also cleaned to PoC to prevent data loss.
 *
 *	- start   - kernel start address of region
 *	- end     - kernel end address of region
 */
/*
 * 完整 invalidate 接口：执行上面逐行算法后 DSB SY，返回时 PoC 已观察到
 * partial line 的保留数据，且目标 cache 行均无本地旧副本。
 */
SYM_FUNC_START(__pi_dcache_inval_poc)
	__dcache_inval_poc_nosync
	dsb	sy
	ret
SYM_FUNC_END(__pi_dcache_inval_poc)
SYM_FUNC_ALIAS(dcache_inval_poc, __pi_dcache_inval_poc)

/*
 *	dcache_inval_poc_nosync(start, end)
 *
 * 	Issue the instructions of D-cache lines for the interval [start, end)
 * 	for invalidation. Not necessarily cleaned to PoC till an explicit dsb
 *	sy is issued later
 *
 *	- start   - kernel start address of region
 *	- end     - kernel end address of region
 */
/*
 * 批量优化版本只发 DC 指令，不执行 DSB；调用者必须在所有范围完成后
 * 显式 `dsb sy`，在此之前不能把内存所有权交给设备或读取结果。
 */
SYM_FUNC_START(__pi_dcache_inval_poc_nosync)
	__dcache_inval_poc_nosync
	ret
SYM_FUNC_END(__pi_dcache_inval_poc_nosync)
SYM_FUNC_ALIAS(dcache_inval_poc_nosync, __pi_dcache_inval_poc_nosync)

/*
 *	dcache_clean_poc(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoC.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
/* clean [x0,x1) 到 PoC，保留 cache line 有效；用于 CPU 脏数据交给设备。 */
SYM_FUNC_START(__pi_dcache_clean_poc)
	dcache_by_line_op cvac, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END(__pi_dcache_clean_poc)
SYM_FUNC_ALIAS(dcache_clean_poc, __pi_dcache_clean_poc)

/*
 *	dcache_clean_poc_nosync(start, end)
 *
 * 	Issue the instructions of D-cache lines for the interval [start, end).
 * 	not necessarily cleaned to the PoC till an explicit dsb sy afterward.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
/* clean-to-PoC 的 nosync 版本；最终 `dsb sy` 是调用者不可省略的契约。 */
SYM_FUNC_START(__pi_dcache_clean_poc_nosync)
	dcache_by_line_op_nosync cvac, x0, x1, x2, x3
	ret
SYM_FUNC_END(__pi_dcache_clean_poc_nosync)
SYM_FUNC_ALIAS(dcache_clean_poc_nosync, __pi_dcache_clean_poc_nosync)

/*
 *	dcache_clean_pop(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoP.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
/*
 * clean 到持久化点 PoP。支持 DC CVAP 时逐行持久化；不支持 ARM64_HAS_DCPOP
 * 的 CPU 退化为 clean-to-PoC，因为该平台可声明 PoC 即其可用持久边界。
 * 返回前完成 SY 屏障，供 pmem API 建立掉电前写回顺序。
 */
SYM_FUNC_START(__pi_dcache_clean_pop)
	alternative_if_not ARM64_HAS_DCPOP
	b	dcache_clean_poc
	alternative_else_nop_endif
	dcache_by_line_op cvap, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END(__pi_dcache_clean_pop)
SYM_FUNC_ALIAS(dcache_clean_pop, __pi_dcache_clean_pop)
