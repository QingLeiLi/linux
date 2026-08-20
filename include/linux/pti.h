// SPDX-License-Identifier: GPL-2.0
/*
 * PTI (Page Table Isolation) 页表隔离头文件
 *
 * 【设计背景】
 * PTI 是针对 "Meltdown"（熔断，CVE-2017-5754）漏洞的内核缓解措施。
 * Meltdown 允许用户态程序通过 CPU 推测执行（speculative execution）侧信道
 * 读取内核内存。PTI 的解决方案是：用户态和内核态使用两套不同的页表，
 * 用户态的页表中不包含内核页的映射，从根本上切断侧信道路径。
 *
 * 【代价与权衡】
 * 启用 PTI 会带来性能开销（每次 syscall/中断需要切换 CR3 寄存器），
 * 现代 CPU（有 PCID 支持）可大幅降低此开销。
 * 该功能通过 CONFIG_MITIGATION_PAGE_TABLE_ISOLATION 编译选项控制。
 *
 * 【头文件结构说明】
 * 本文件是平台无关的包装层：
 *   - 若开启 PTI，则引入架构相关实现（asm/pti.h，如 x86 的具体实现）
 *   - 若未开启，则提供空的内联函数，保证调用代码无需 #ifdef 判断
 */
#ifndef _INCLUDE_PTI_H
#define _INCLUDE_PTI_H

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
/* 开启了页表隔离缓解措施，引入架构相关的具体实现（如 arch/x86/include/asm/pti.h） */
#include <asm/pti.h>
#else
/*
 * 未开启 PTI 时，提供空操作的内联函数（no-op stub）。
 * 设计原则：让调用方无需关心 PTI 是否启用，直接调用即可，
 * 编译器会将这些空函数优化掉，不产生任何代码。
 */

/*
 * pti_init - PTI 子系统早期初始化
 * 无参数，无返回值。
 * 在内核启动早期（setup_arch 阶段）调用，用于初始化双页表结构。
 * 未开启 PTI 时为空操作。
 */
static inline void pti_init(void) { }

/*
 * pti_finalize - PTI 子系统最终化（收尾）
 * 无参数，无返回值。
 * 在初始化后期调用，用于完成页表的最终设置（如标记内核 .text 等段）。
 * 未开启 PTI 时为空操作。
 */
static inline void pti_finalize(void) { }
#endif

#endif
