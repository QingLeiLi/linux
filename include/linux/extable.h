/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 通用异常表接口学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 本头文件连接三层实现：
 *
 *   生成层：架构汇编宏把“可能 fault 的指令 -> fixup”写入 __ex_table；
 *   容器层：lib/extable.c 负责表项排序和单表二分查找；
 *   分派层：kernel/extable.c 依次查询内建、模块和 BPF JIT 异常表。
 *
 * 它只声明通用查找/排序协议，不定义架构表项字段，也不执行 fault
 * 修复；struct exception_table_entry 的布局及 fixup 解释由
 * asm/extable.h 和架构 fault handler 决定。
 *
 * 内建表随内核全生命周期存在，模块/BPF 表随拥有者发布和回收。
 * 查询返回借用的 const 表项指针，不转移引用或所有权；动态表的使用者
 * 仍须满足对应模块执行期或 RCU 生命周期。排序发生在表发布前，正常
 * fault 路径只读有序表，因此无需为内建表查询加锁。
 *
 * CONFIG_MODULES 或 CONFIG_BPF_JIT 关闭时，内联 stub 把相应来源收敛为
 * NULL，使上层保持固定查找顺序而不散布条件编译。收益是调用者简单且
 * fast path 统一；代价是接口必须清楚区分永久内建表与动态表的生命周期。
 */
#ifndef _LINUX_EXTABLE_H
#define _LINUX_EXTABLE_H

#include <linux/stddef.h>	/* for NULL */
/* stddef 提供配置关闭时内联 stub 使用的 NULL。 */
#include <linux/types.h>

/*
 * module 与 exception_table_entry 只以指针形式出现在接口中，前向声明
 * 避免公共头文件强制引入完整模块和架构表项定义。
 */
struct module;
struct exception_table_entry;

/*
 * search_extable() - 在一张已排序异常表中二分查找指令地址。
 *
 * @base：表首元素的借用只读指针；num>0 时不可为 NULL，所有权不转移。
 * @num：从 base 起有效表项数，单位是 entry 而非字节。
 * @value：待匹配的故障指令虚拟地址。
 *
 * 返回匹配表项的借用 const 指针，未命中返回 NULL。函数不分配、
 * 不睡眠、不修改表；调用者必须在调用前保证表按架构实际指令地址
 * 升序排列，并在返回指针使用期间稳定动态表的生命周期。
 */
const struct exception_table_entry *
search_extable(const struct exception_table_entry *base,
	       const size_t num,
	       unsigned long value);

/*
 * sort_extable() - 原地按故障指令地址排序一个异常表半开区间。
 *
 * @start：首个可写表项；@finish：尾后指针，二者属于同一数组且
 * start<=finish。调用者仍拥有存储，并保证表尚未并发发布。无直接返回值；
 * 相对表项架构由专用 swap 回调在交换时重算偏移，排序后即可二分查找。
 */
void sort_extable(struct exception_table_entry *start,
		  struct exception_table_entry *finish);

/*
 * 启动/模块生命周期接口组：
 *
 * sort_main_extable() 无入参和直接返回值，在早期启动阶段必要时排序内建
 * __ex_table；完成后内建查询者可以依赖有序不变量。
 *
 * trim_init_extable(@m) 在模块 init 内存即将释放前，从已排序表的两端
 * 去掉指向 module init text 的表项。m 是已加载模块的借用可写对象，
 * CONFIG_MODULES=n 时没有调用者和实现。
 */
void sort_main_extable(void);
void trim_init_extable(struct module *m);

/* Given an address, look for it in the exception tables */
/*
 * 给定故障指令地址，在异常表集合中查找对应表项。
 *
 * search_exception_tables(@add) 依次查询内建、模块和 BPF 表；
 * 返回首个借用表项或 NULL。
 * search_kernel_exception_table(@addr) 只查内建表，适合明确排除动态
 * 代码的调用者。两个参数都是纯输入虚拟地址，函数均不执行 fixup、
 * 不修改寄存器现场，也不转移 ownership。
 */
const struct exception_table_entry *search_exception_tables(unsigned long add);
const struct exception_table_entry *
search_kernel_exception_table(unsigned long addr);

#ifdef CONFIG_MODULES
/* For extable.c to search modules' exception tables. */
/*
 * 供 kernel/extable.c 查询模块拥有的异常表。addr 是纯输入指令地址；
 * 返回当前模块执行/RCU 生命周期内借用的表项，未命中返回 NULL。
 */
const struct exception_table_entry *search_module_extables(unsigned long addr);
#else
/*
 * 未启用模块时固定返回 NULL。addr 被有意忽略，无副作用、不会睡眠；
 * 该 stub 让公共分派代码无需用 CONFIG_MODULES 包住每次调用。
 */
static inline const struct exception_table_entry *
search_module_extables(unsigned long addr)
{
	return NULL;
}
#endif /*CONFIG_MODULES*/
/*
 * 上述条件编译在有模块时连接动态模块索引，否则彻底移除该查询
 * 来源。
 */

#ifdef CONFIG_BPF_JIT
/*
 * 查询 BPF JIT 程序随附的异常表。addr 是纯输入 JIT 指令地址；返回当前
 * BPF 程序/RCU 生命周期内借用的表项，未命中返回 NULL。
 */
const struct exception_table_entry *search_bpf_extables(unsigned long addr);
#else
/*
 * 未启用 BPF JIT 时固定返回 NULL；不读取 addr，不分配、不会睡眠。
 */
static inline const struct exception_table_entry *
search_bpf_extables(unsigned long addr)
{
	return NULL;
}
#endif
/* CONFIG_BPF_JIT 只改变表来源是否存在，不改变上层查找和 NULL 语义。 */

#endif /* _LINUX_EXTABLE_H */
/* 结束通用异常表接口保护，避免同一翻译单元重复声明。 */
