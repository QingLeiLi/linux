// SPDX-License-Identifier: GPL-2.0
/*
 * 本文件是构建期的“类型布局转接器”，不参与内核运行时执行。Kbuild 使用目标
 * 架构编译器把它编译成 rq-offsets.s，再从 DEFINE() 产生的特殊汇编文本中提取
 * RQ_nr_pinned，生成 include/generated/rq-offsets.h。
 *
 * 之所以不能在 include/linux/sched.h 中直接写 offsetof(struct rq, nr_pinned)，
 * 是因为公共头文件在那里只前置声明了 struct rq；完整布局属于调度器内部的
 * sched.h。这里在能够看到完整结构体的位置计算偏移，再以普通整数宏跨越这条
 * 可见性边界。偏移随目标体系结构、配置和结构布局变化，每次相关依赖改变都必须
 * 重新生成，不能把某个平台的数值硬编码进源码。
 */

/*
 * 定义 COMPILE_OFFSETS 后，include/linux/sched.h 不再反向包含尚未生成的
 * rq-offsets.h，并把依赖该偏移的迁移辅助函数替换为空实现，从而打破
 * “生成偏移需要 sched.h、sched.h 又需要生成偏移”的构建环。
 */
#define COMPILE_OFFSETS

/* DEFINE() 把“符号、常量值、原 C 表达式”编码为可由 Kbuild 的 sed 规则识别的 .ascii 行。 */
#include <linux/kbuild.h>
/* 提供目标内核的基础类型；偏移必须按目标 ABI 而不是构建主机 ABI 计算。 */
#include <linux/types.h>
/* 提供 struct rq 的完整内部布局以及其中的 nr_pinned 字段。 */
#include "sched.h"

/*
 * main() - 向生成的汇编文件发布 rq->nr_pinned 的目标架构字节偏移
 *
 * 宏观位置：顶层 Kbuild 的 prepare 阶段 → 编译 rq-offsets.c 为 rq-offsets.s
 * → scripts/Makefile.lib:filechk_offsets() 提取标记 → 生成 rq-offsets.h
 * → include/linux/sched.h:this_rq_pinned() 以 percpu rq 基址加偏移访问该字段。
 *
 * 入参：无。该函数只为让目标编译器实例化常量表达式而存在，不会被链接进
 * vmlinux，也不会在内核或构建主机上作为程序运行；因此没有锁、并发上下文、
 * 睡眠、对象 ownership 或运行时失败路径。
 *
 * 返回：源码形式返回 0 以构成完整 C 函数，但构建流程不消费此返回值。真正的
 * 输出副作用是编译器在 rq-offsets.s 中保留名为 RQ_nr_pinned 的 DEFINE 标记；
 * 若 struct rq 布局改变，offsetof() 会在重新编译时自动给出新的字节数。
 */
int main(void)
{
	/*
	 * offsetof() 在编译期求出 nr_pinned 相对 struct rq 起始地址的字节偏移；
	 * DEFINE() 不创建 C 变量，而用立即数约束把结果写成特殊 .ascii 标记。
	 */
	DEFINE(RQ_nr_pinned, offsetof(struct rq, nr_pinned));

	/* 此返回只满足 C 函数语法；偏移已经通过上面的汇编标记交给 Kbuild。 */
	return 0;
}
