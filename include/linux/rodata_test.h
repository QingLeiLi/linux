/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rodata_test.h: functional test for mark_rodata_ro function
 * rodata_test.h: mark_rodata_ro 函数的功能测试
 *
 * (C) Copyright 2008 Intel Corporation
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 */

#ifndef _RODATA_TEST_H
#define _RODATA_TEST_H

/**
 * 文件目的 (File Purpose):
 * ============================================================================
 * 测试内核只读数据(.rodata)段的写保护是否正确生效。
 * 这是一个关键的安全特性测试。
 * Tests whether write protection for kernel read-only data (.rodata) section
 * is correctly enabled. This is a critical security feature test.
 *
 * 背景知识 (Background):
 * ============================================================================
 * 1. 内核内存布局包含多个段:
 *    .text   - 代码段(应该是只读+可执行)
 *    .rodata - 只读数据段(应该是只读+不可执行)
 *    .data   - 可读写数据段
 *    Kernel memory layout includes multiple sections:
 *    .text   - Code section (should be read-only + executable)
 *    .rodata - Read-only data section (should be read-only + non-executable)
 *    .data   - Read-write data section
 *
 * 2. 出于性能考虑，内核启动初期所有内存可能都是可写的，便于初始化。
 *    启动完成后调用 mark_rodata_ro() 将应该只读的段设置为只读。
 *    For performance reasons, all memory may be writable during early boot
 *    for easy initialization. After boot completes, mark_rodata_ro() sets
 *    sections that should be read-only to actually be read-only.
 *
 * 3. 如果 mark_rodata_ro() 没有正确工作，攻击者可能修改内核常量数据，
 *    例如函数指针表、安全策略常量等，造成安全漏洞。
 *    If mark_rodata_ro() doesn't work correctly, attackers could modify
 *    kernel constant data like function pointer tables, security policy
 *    constants, etc., creating security vulnerabilities.
 *
 * 为什么需要这个测试 (Why This Test Is Needed):
 * ============================================================================
 * - 不同CPU架构实现内存保护的方式不同
 * - 页表配置错误可能导致保护失效
 * - 编译器或链接器配置问题可能影响段布局
 * - Different CPU architectures implement memory protection differently
 * - Page table configuration errors could disable protection
 * - Compiler or linker configuration issues could affect section layout
 *
 * CONFIG_DEBUG_RODATA_TEST 配置项:
 * ============================================================================
 * 这是一个内核配置选项，位于 "Kernel hacking" -> "Memory Debugging"
 * 启用后，内核启动时会尝试写入 .rodata 段并验证是否触发保护异常
 * This is a kernel config option under "Kernel hacking" -> "Memory Debugging"
 * When enabled, kernel tries to write to .rodata section at boot and verifies
 * if protection fault is triggered
 *
 * 注意: 这是调试功能，生产环境通常不启用(有轻微性能开销)
 * Note: This is a debug feature, usually not enabled in production
 * (has minor performance overhead)
 */

#ifdef CONFIG_DEBUG_RODATA_TEST
/**
 * rodata_test - 执行只读数据段保护测试
 * Execute read-only data section protection test
 *
 * 测试流程 (Test Flow):
 * 1. 在 .rodata 段中定义一个测试变量
 * 2. 尝试写入该变量
 * 3. 如果写入成功(未触发异常) -> 测试失败，说明保护未生效
 * 4. 如果触发保护异常 -> 测试成功，说明 mark_rodata_ro() 工作正常
 *
 * 1. Define a test variable in the .rodata section
 * 2. Try to write to that variable
 * 3. If write succeeds (no fault triggered) -> test fails, protection not working
 * 4. If protection fault triggered -> test passes, mark_rodata_ro() works correctly
 *
 * 调用时机 (When Called):
 * 通常在 mark_rodata_ro() 调用之后，作为启动自检的一部分
 * Usually called after mark_rodata_ro(), as part of boot self-tests
 *
 * 返回值 (Return):
 * void - 测试结果通过 printk 输出到内核日志
 *        Test results are output to kernel log via printk
 *
 * 典型输出 (Typical Output):
 * 成功: "rodata_test: test passed"
 * 失败: "rodata_test: test failed: .rodata section is writable"
 * Success: "rodata_test: test passed"
 * Failure: "rodata_test: test failed: .rodata section is writable"
 */
void rodata_test(void);
#else
/**
 * rodata_test - 空操作(未启用 CONFIG_DEBUG_RODATA_TEST 时)
 * No-op when CONFIG_DEBUG_RODATA_TEST is not enabled
 *
 * 为什么定义为 static inline (Why static inline):
 * 1. 避免链接错误 - 调用代码可以无条件调用 rodata_test()
 * 2. 零开销 - 编译器会完全优化掉这个空函数
 * 3. 不污染符号表 - static 限制作用域在本编译单元
 *
 * 1. Avoids link errors - calling code can unconditionally call rodata_test()
 * 2. Zero overhead - compiler will completely optimize out this empty function
 * 3. Doesn't pollute symbol table - static limits scope to compilation unit
 *
 * 这是内核中常见的条件编译模式:
 * #ifdef CONFIG_XXX
 *   实际实现
 * #else
 *   static inline 空实现
 * #endif
 *
 * This is a common conditional compilation pattern in the kernel:
 * #ifdef CONFIG_XXX
 *   Actual implementation
 * #else
 *   static inline empty implementation
 * #endif
 */
static inline void rodata_test(void) {}
#endif

#endif /* _RODATA_TEST_H */
