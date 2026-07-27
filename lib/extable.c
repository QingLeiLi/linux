// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 通用异常表排序与单表查找学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 本文件为 kernel/extable.c 和模块加载器提供与体系结构无关的容器算法：
 *
 *   未排序表 -> sort_extable() -> 按故障指令绝对地址升序排列
 *                                  |
 *                                  +-> search_extable() 二分查找
 *                                  +-> trim_init_extable() 裁掉模块 init 表项
 *
 * 架构仍负责定义 struct exception_table_entry、把表项编码进 __ex_table，
 * 以及命中后的寄存器/fixup 语义。本文件只要求能够把表项转换为真实 insn
 * 地址，并在交换相对表项时保持所有相对目标不变。
 *
 * 内建表在早期启动、模块表在 relocation 完成且对外发布前原地排序；之后
 * fault 路径只读表并可并发二分查找。排序者独占可写数组，查询者只借用
 * 指针，均不取得表项所有权。模块 init 完成后，trim 只缩小模块记录的
 * 有效窗口，实际 init 内存仍由模块释放路径统一回收。
 *
 * 相对表项节省重定位和存储成本，但交换元素不能做普通字节交换：
 * 字段基址一变，相对偏移也必须补偿。通用 sort 通过架构 swap hook
 * 解决该问题，代价是每种额外相对字段都必须纳入交换协议，否则排序会
 * 静默破坏 fixup。
 */
/*
 * Derived from arch/ppc/mm/extable.c and arch/i386/mm/extable.c.
 *
 * Copyright (C) 2004 Paul Mackerras, IBM Corp.
 */
/*
 * 本通用实现派生自早期 PowerPC 与 i386 的异常表代码；原版权如上。
 */

#include <linux/bsearch.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <linux/extable.h>

#ifndef ARCH_HAS_RELATIVE_EXTABLE
/*
 * 绝对地址表项直接读取 insn 字段。x 是表内借用指针，宏只取值，
 * 不改变表项，也不涉及生命周期。
 */
#define ex_to_insn(x)	((x)->insn)
#else
/*
 * ex_to_insn() - 把相对 insn 字段还原为实际故障指令地址。
 *
 * @x 是有效表项的借用只读指针，不可为 NULL。相对格式保存
 * “目标地址 - insn 字段自身地址”，所以必须以 &x->insn 为基址相加。
 * 返回 unsigned long 虚拟地址；无副作用、不睡眠，排序和查找比较器都会
 * 调用。ARCH_HAS_RELATIVE_EXTABLE=n 时由上面的直接字段宏提供同一语义。
 */
static inline unsigned long ex_to_insn(const struct exception_table_entry *x)
{
	return (unsigned long)&x->insn + x->insn;
}
#endif

#ifndef ARCH_HAS_RELATIVE_EXTABLE
/*
 * 绝对地址表项可让通用 sort 使用默认字节交换，因此回调为 NULL。
 */
#define swap_ex		NULL
#else
/*
 * swap_ex() - 交换两个相对地址表项并补偿字段基址变化。
 *
 * @a/@b 是 sort() 传入的两个可写表项地址，借用且属于同一数组；
 * @size 是通用交换回调要求的表项字节数，本实现由静态结构布局决定，
 * 无需读取。delta 是 b 相对 a 的字节距离。
 *
 * 普通结构体交换会让原相对值以新字段地址为基准，从而指向错误位置。
 * 本函数先保存 a，再把 b 搬到 a 并给偏移加 delta，把 a 搬到 b 并减
 * delta；架构 hook 同步处理 fixup/type/data 等附加字段。无失败返回，
 * 调用者必须独占尚未发布的表，不需要锁且不会睡眠。
 */
static void swap_ex(void *a, void *b, int size)
{
	/*
	 * x/y 是 sort 数组中的可写借用表项，tmp 保存交换期间 a 的完整旧值；
	 * delta 使用内核允许的 void 指针字节算术，单位为 byte。
	 */
	struct exception_table_entry *x = a, *y = b, tmp;
	int delta = b - a;

	/*
	 * 阶段 1：交换 insn，并让两个新相对值继续解析到各自原
	 * 目标地址。
	 */
	tmp = *x;
	x->insn = y->insn + delta;
	y->insn = tmp.insn - delta;

#ifdef swap_ex_entry_fixup
	/*
	 * 架构若还有相对 fixup 或伴随元数据，必须在这里完成成组交换；
	 * hook 接收新 x/y、旧 a 快照和同一字节 delta。
	 */
	swap_ex_entry_fixup(x, y, tmp, delta);
#else
	/*
	 * 默认格式只有另一个相对 fixup 字段；采用与 insn 相同的基址补偿。
	 */
	x->fixup = y->fixup + delta;
	y->fixup = tmp.fixup - delta;
#endif
}
#endif /* ARCH_HAS_RELATIVE_EXTABLE */
/* 相对格式提供定制交换；绝对格式保持通用 sort 的普通交换策略。 */

/*
 * The exception table needs to be sorted so that the binary
 * search that we use to find entries in it works properly.
 * This is used both for the kernel exception table and for
 * the exception tables of modules that get loaded.
 */
/*
 * 异常表必须排序，二分查找才能正确工作；该要求同时适用于内建
 * 内核表和每个已加载模块的表。排序键是表项解析后的实际故障
 * 指令地址，而不是相对编码的原始整数。
 */
/*
 * cmp_ex_sort() - 比较两个表项的实际故障指令地址。
 *
 * @a/@b 是 sort() 借用的只读表项，均非 NULL。返回 1、-1、0
 * 分别表示 a 在 b 之后、之前或键相等；无副作用、不会睡眠。使用关系
 * 比较而非地址相减，避免 unsigned long 差值截断到 int 造成溢出和
 * 错误排序。
 */
static int cmp_ex_sort(const void *a, const void *b)
{
	/* x/y 仅在本次比较回调内借用，不取得表项引用或所有权。 */
	const struct exception_table_entry *x = a, *y = b;

	/* avoid overflow */
	/* 直接比较完整地址，避免用差值作为 int 返回时溢出。 */
	if (ex_to_insn(x) > ex_to_insn(y))
		return 1;
	if (ex_to_insn(x) < ex_to_insn(y))
		return -1;
	return 0;
}

/*
 * sort_extable() - 原地排序 [start, finish) 异常表。
 *
 * @start 是首表项，@finish 是同一数组的尾后指针；允许二者相等表示空表，
 * 调用者保留 ownership 并保证整个区间可写、尚无并发查询者。内建调用点
 * 位于早期启动，模块调用点位于 relocation 后、发布前，均允许 sort()
 * 使用栈和普通 CPU 时间但不涉及新的长期资源。
 *
 * 返回：无直接返回值。完成后按 ex_to_insn() 升序排列；相对格式通过
 * swap_ex 保持每个 insn/fixup 的绝对目标不变。接口不报告分配失败，
 * 因为内核 sort 在现有数组内完成。
 */
void sort_extable(struct exception_table_entry *start,
		  struct exception_table_entry *finish)
{
	/*
	 * finish-start 得到元素数，sizeof 给出步长；比较器定义排序键，
	 * swap_ex 在相对格式下接管交换，否则 NULL 选择默认交换。
	 */
	sort(start, finish - start, sizeof(struct exception_table_entry),
	     cmp_ex_sort, swap_ex);
}

#ifdef CONFIG_MODULES
/*
 * If the exception table is sorted, any referring to the module init
 * will be at the beginning or the end.
 */
/*
 * 表按指令地址排序，而模块 init text 是连续地址区间，因此所有引用 init
 * 的表项只会聚集在表首或表尾；无需扫描/搬动中间的 core text 表项。
 */
/*
 * trim_init_extable() - 从模块有效异常表窗口移除 init text 表项。
 *
 * @m 是模块加载器持有且可修改的 struct module 借用指针，不可为 NULL；
 * 调用时模块 init 已结束、module_mutex 已持有，init 内存尚未最终释放。
 * m->extable 指向已排序表，m->num_exentries 是当前有效元素数。
 *
 * 函数先向前移动 extable 并递减计数，再从尾部递减计数；不搬移、不释放
 * 表项存储。完成后动态查找不会再返回即将失效的 init fixup。返回：无
 * 直接返回值，无普通失败路径；init 存储仍由后续模块释放阶段回收。
 */
void trim_init_extable(struct module *m)
{
	/*trim the beginning*/
	/*
	 * 表首循环每次丢弃一个指向 init text 的连续前缀；先检查计数可防止
	 * 空表解引用，extable 指针始终指向剩余窗口首元素。
	 */
	while (m->num_exentries &&
	       within_module_init(ex_to_insn(&m->extable[0]), m)) {
		m->extable++;
		m->num_exentries--;
	}
	/*trim the end*/
	/*
	 * 表尾循环丢弃连续后缀，只缩短计数而不移动首指针。完成后
	 * 首尾都不是 init 地址；排序与 init 区间连续性保证中间也
	 * 不会残留。
	 */
	while (m->num_exentries &&
	       within_module_init(ex_to_insn(&m->extable[m->num_exentries - 1]),
				  m))
		m->num_exentries--;
}
#endif /* CONFIG_MODULES */
/* 模块关闭时不存在动态模块表，也不编译裁剪生命周期。 */

/*
 * cmp_ex_search() - 比较查找键与一个异常表项的实际指令地址。
 *
 * @key 指向 bsearch() 栈上的 unsigned long 指令地址，借用只读；
 * @elt 指向已排序表中的借用只读元素。返回正、负、零分别表示 key 更大、
 * 更小或相等。无副作用、不睡眠；关系比较避免差值转换为 int 时溢出。
 */
static int cmp_ex_search(const void *key, const void *elt)
{
	/*
	 * _elt 是候选表项；_key 复制目标地址，因此比较器不在回调外保存
	 * bsearch() 传入的临时指针。
	 */
	const struct exception_table_entry *_elt = elt;
	unsigned long _key = *(unsigned long *)key;

	/* avoid overflow */
	/* 同排序比较器一样，避免大地址差值被 int 截断。 */
	if (_key > ex_to_insn(_elt))
		return 1;
	if (_key < ex_to_insn(_elt))
		return -1;
	return 0;
}

/*
 * Search one exception table for an entry corresponding to the
 * given instruction address, and return the address of the entry,
 * or NULL if none is found.
 * We use a binary search, and thus we assume that the table is
 * already sorted.
 */
/*
 * 在给定异常表中查找与指令地址对应的表项；命中返回该表项地址，
 * 未命中返回 NULL。实现采用二分查找，因此调用者必须先建立升序
 * 不变量。
 */
/*
 * search_extable() - 对一张稳定的有序异常表执行只读二分查找。
 *
 * @base 是首元素借用 const 指针；num 是有效表项数，单位为 entry；
 * @value 是目标故障指令虚拟地址。num 为 0 时允许 base 不被解引用。
 * 调用者负责保证 [base, base+num) 已按 ex_to_insn() 排序，并在返回指针
 * 使用期间稳定动态表的存储期。
 *
 * 返回命中的借用 const 表项或 NULL；不修改表、不分配、不会睡眠，可在
 * fault 等原子上下文调用。函数只定位元数据，架构 handler 决定如何 fixup。
 */
const struct exception_table_entry *
search_extable(const struct exception_table_entry *base,
	       const size_t num,
	       unsigned long value)
{
	/*
	 * value 的地址只在 bsearch 调用期间作为 key；返回值直接指向 base
	 * 数组，没有新引用或 ownership 转移。
	 */
	return bsearch(&value, base, num,
		       sizeof(struct exception_table_entry), cmp_ex_search);
}
