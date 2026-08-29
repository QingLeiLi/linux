// SPDX-License-Identifier: GPL-2.0-only
/*
 * rodata_test.c: functional test for mark_rodata_ro function
 *
 * (C) Copyright 2008 Intel Corporation
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 */
/*
 * 本文件在启动末期对 mark_rodata_ro() 做功能自检：不仅确认测试常量仍可读，
 * 还用不会令内核崩溃的探测写验证 .rodata 页表权限，并核对链接器给出的段边界
 * 是否按页对齐。版权与作者行保留原样；首行所说的正是这条验证链。
 */
#define pr_fmt(fmt) "rodata_test: " fmt

#include <linux/rodata_test.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <asm/sections.h>

/*
 * TEST_VALUE 是测试常量的编译期基准；static const 使 rodata_test_data 只在本文件
 * 可见并由链接器放入 .rodata。后续所有读取使用 READ_ONCE，避免编译器把“常量
 * 永远等于初值”折叠掉，从而真正访问目标地址。该对象没有运行期所有权转移。
 */
#define TEST_VALUE 0xC3
static const int rodata_test_data = TEST_VALUE;

/*
 * rodata_test() - 在内核最终收紧映射后验证 .rodata 的内容、写保护与边界。
 *
 * 业务背景：kernel_init() 的 mark_readonly() 依次执行 mark_rodata_ro()、
 * debug_checkwx() 和本函数；这里是架构页表实现与链接布局的启动期验收点，而不
 * 是设置权限的实现。任一检查失败只写日志并停止后续检查，启动仍继续。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数和 ownership 变化；通过 pr_err/pr_info
 * 把首个失败原因或“全部成功”写入内核日志。
 * 注意事项：调用前 .rodata 必须已完成最终映射；本函数运行在启动内核线程的
 * 可睡眠进程上下文，但自身不取锁。copy_to_kernel_nofault() 临时禁用 page fault
 * 并把保护异常转成 -EFAULT，不能改成普通写入，否则预期故障会令内核崩溃。
 */
void rodata_test(void)
{
	/* zero 是仅供探测写使用的栈上源值；写保护正确时目标常量绝不会接收它。 */
	int zero = 0;

	/* test 1: read the value */
	/*
	 * 测试 1：先读取基准值。若失败，说明更早的一轮测试或其他越界写已经破坏
	 * 状态；此时继续做写保护测试会混淆“旧损坏”和“本次探测”的责任边界。
	 */
	/* If this test fails, some previous testrun has clobbered the state */
	/*
	 * 如果本测试失败，某次先前的测试运行已经破坏了状态。READ_ONCE 强制从
	 * rodata_test_data 的实际地址取值，而不是复用编译器推导出的常量。
	 */
	if (unlikely(READ_ONCE(rodata_test_data) != TEST_VALUE)) {
		pr_err("test 1 fails (start data)\n");
		return;
	}

	/* test 2: write to the variable; this should fault */
	/*
	 * 测试 2：尝试把 zero 写入该常量，正确的只读映射应产生 fault。
	 * copy_to_kernel_nofault() 返回 0 反而表示写入成功，因而是安全属性失效；
	 * -EFAULT 才是这里期待的结果，并保证异常已在 helper 内被安全收束。
	 */
	if (!copy_to_kernel_nofault((void *)&rodata_test_data,
				(void *)&zero, sizeof(zero))) {
		pr_err("test data was not read only\n");
		return;
	}

	/* test 3: check the value hasn't changed */
	/*
	 * 测试 3：故障返回还不足以证明没有部分写入；重新读取整个 int，确认探测
	 * 前后内容一致，才把“写入被拒绝”与“数据未受损”两个保证同时闭环。
	 */
	if (unlikely(READ_ONCE(rodata_test_data) != TEST_VALUE)) {
		pr_err("test data was changed\n");
		return;
	}

	/* test 4: check if the rodata section is PAGE_SIZE aligned */
	/*
	 * 测试 4：分别验证 .rodata 起止链接符号按 PAGE_SIZE 对齐。页权限以页为
	 * 粒度；边界不对齐会让相邻可写数据共享同一页，无法只收紧 rodata 而不误伤。
	 */
	if (!PAGE_ALIGNED(__start_rodata)) {
		pr_err("start of .rodata is not page size aligned\n");
		return;
	}
	if (!PAGE_ALIGNED(__end_rodata)) {
		pr_err("end of .rodata is not page size aligned\n");
		return;
	}

	/* 四个阶段均通过后才发布成功日志；此前没有任何持久状态需要回滚。 */
	pr_info("all tests were successful\n");
}
