// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 早期页表范围映射学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件在 MMU 尚未开启或内核正式虚拟映射尚未建立时，把一段连续物理地址递归
 * 填入启动页表。调用链主要是 head.S -> create_init_idmap() -> map_range()，以及
 * early_map_kernel() -> map_kernel()/map_fdt() -> map_range()。它只写调用者预留的
 * 页表页，不分配普通内存、不刷新 TLB，也不发布最终 TTBR；屏障和 TTBR 切换由
 * 上层调用者完成。
 *
 * 核心所有权是不发生动态转移：@pte 指向调用者维护的“下一张空闲页表页”物理
 * 游标，@tbl 是当前层页表的借用视图。递归首次遇到空表项时消耗一页并推进游标；
 * 已存在的子表会被复用。启动阶段只有当前 CPU 修改这些尚未发布的表，不需要锁，
 * 但页表格式、块对齐和 contiguous hint 必须严格匹配硬件遍历规则。
 */
// Copyright 2023 Google LLC
// Author: Ard Biesheuvel <ardb@google.com>

#include <linux/types.h>
#include <linux/sizes.h>

#include <asm/memory.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>

#include "pi.h"

/**
 * map_range - Map a contiguous range of physical pages into virtual memory
 *
 * @pte:		Address of physical pointer to array of pages to
 *			allocate page tables from
 * @start:		Virtual address of the start of the range
 * @end:		Virtual address of the end of the range (exclusive)
 * @pa:			Physical address of the start of the range
 * @prot:		Access permissions of the range
 * @level:		Translation level for the mapping
 * @tbl:		The level @level page table to create the mappings in
 * @may_use_cont:	Whether the use of the contiguous attribute is allowed
 * @va_offset:		Offset between a physical page and its current mapping
 * 			in the VA space
 */
/*
 * map_range() - 在指定层级页表中建立一段连续 VA 到连续 PA 的早期映射。
 *
 * 调用关系：create_init_idmap()、map_kernel() 和 map_fdt() 使用本函数；函数按需
 * 递归到更细层级，直到能写块描述符或第 3 级页描述符。@pte 是输入输出物理游标，
 * 指向调用者预留页表池中的下一空闲页；函数不取得其所有权，但每创建一张子表就
 * 把它向后推进一页。@start/@end 是半开区间虚拟地址，@end 不包含在映射内；
 * @pa 是首字节物理地址；@prot 给出权限和属性；@level/@tbl 描述当前翻译层及其
 * 借用页表；@may_use_cont 决定能否设置连续项提示；@va_offset 用于把页表物理地址
 * 转换成当前可访问的虚拟别名，MMU 关闭的 idmap 构造中为 0。
 *
 * 入口处页表池、目标表和地址范围必须由调用者预留并保证容量充足；此极早期路径
 * 不能睡眠、没有锁。返回无直接值，也没有可返回错误；返回后描述符已写入内存但尚未通过
 * 屏障和 TTBR/TLB 协议对硬件发布；容量或参数错误不会得到优雅回滚，而会破坏早期
 * 页表，因此正确性依赖链接脚本尺寸预算和调用者的对齐约束。
 */
void __init map_range(phys_addr_t *pte, u64 start, u64 end, phys_addr_t pa,
		      pgprot_t prot, int level, pte_t *tbl, bool may_use_cont,
		      u64 va_offset)
{
	/*
	 * 变量地图：cmask 是当前层可使用 contiguous hint 时的对齐掩码；protval 是去掉
	 * 描述符类型位后的属性模板；lshift/lmask 给出当前层单个表项覆盖范围。level=3
	 * 只能写页项，level=2 可写块项，较高层通常需要继续建立子表。
	 */
	u64 cmask = (level == 3) ? CONT_PTE_SIZE - 1 : U64_MAX;
	ptval_t protval = pgprot_val(prot) & ~PTE_TYPE_MASK;
	int lshift = (3 - level) * PTDESC_TABLE_SHIFT;
	u64 lmask = (PAGE_SIZE << lshift) - 1;

	start	&= PAGE_MASK;
	pa	&= PAGE_MASK;
	/* 起始 VA/PA 向下页对齐；末端在循环中向上对齐，因此覆盖所有相交页面。 */

	/* Advance tbl to the entry that covers start */
	/*
	 * 把 tbl 推进到覆盖 start 的当前层表项。取模把虚拟地址中的本层索引限制在
	 * 一张页表内；之后循环每完成一个本层覆盖块就递增 tbl。
	 */
	tbl += (start >> (lshift + PAGE_SHIFT)) % PTRS_PER_PTE;

	/*
	 * Set the right block/page bits for this level unless we are
	 * clearing the mapping
	 */
	/*
	 * 除非 protval 为 0（上层借此清映射），否则补上本层合法描述符类型：第 2 层
	 * 使用 block，第 3 层使用 page。类型位不能由通用权限模板预先固定。
	 */
	if (protval)
		protval |= (level == 2) ? PMD_TYPE_SECT : PTE_TYPE_PAGE;

	while (start < end) {
		/* next 截在当前表项覆盖边界或请求末端，确保每轮只处理一个可判定的块。 */
		u64 next = min((start | lmask) + 1, PAGE_ALIGN(end));

		if (level < 2 || (level == 2 && (start | next | pa) & lmask)) {
			/*
			 * This chunk needs a finer grained mapping. Create a
			 * table mapping if necessary and recurse.
			 */
			/*
			 * 当前片段不能用本层块项表达：较高层本来就只能指向下级表，或第 2 层
			 * 的 VA、PA、长度没有同时按块边界对齐。首次使用空项时从线性页表池
			 * 领取一页，写 table 描述符并推进 @pte；已存在时直接复用原有子表。
			 */
			if (pte_none(*tbl)) {
				*tbl = __pte(__phys_to_pte_val(*pte) |
					     PMD_TYPE_TABLE | PMD_TABLE_UXN);
				*pte += PTRS_PER_PTE * sizeof(pte_t);
			}
			/*
			 * __pte_to_phys() 取回子表物理地址，再加 va_offset 得到当前执行环境能
			 * 解引用的地址。递归共享同一 @pte 游标，保证各层按领取顺序不重叠。
			 */
			map_range(pte, start, next, pa, prot, level + 1,
				  (pte_t *)(__pte_to_phys(*tbl) + va_offset),
				  may_use_cont, va_offset);
		} else {
			/*
			 * Start a contiguous range if start and pa are
			 * suitably aligned
			 */
			/*
			 * VA 与 PA 同时按 contiguous 组对齐且调用者允许时，从组首项开始设置
			 * PTE_CONT。它是给硬件的连续映射提示，不改变每个表项独立存在的事实。
			 */
			if (((start | pa) & cmask) == 0 && may_use_cont)
				protval |= PTE_CONT;

			/*
			 * Clear the contiguous attribute if the remaining
			 * range does not cover a contiguous block
			 */
			/* 剩余范围不足完整连续组时清提示，避免硬件把不完整的一组当成整体。 */
			if ((end & ~cmask) <= start)
				protval &= ~PTE_CONT;

			/* Put down a block or page mapping */
			/*
			 * 这是本轮真正改变页表的提交点：把 PA 与权限/类型模板组合为描述符。
			 * 上层仍须执行写屏障，并在替换活动映射时遵守 break-before-make/TLB 协议。
			 */
			*tbl = __pte(__phys_to_pte_val(pa) | protval);
		}
		/* VA、PA 和表项指针保持同步前进，维持“连续区间映射到连续物理区间”不变量。 */
		pa += next - start;
		start = next;
		tbl++;
	}
}

/*
 * create_init_idmap() - 为启动汇编构造覆盖内核早期代码和数据的恒等映射。
 *
 * head.S 在 MMU 关闭时调用其位置无关别名 __pi_create_init_idmap()。@pg_dir 是
 * 调用者清零并预留的根页表物理地址；@clrmask 指定需要从默认权限中移除的位，
 * LPA2 重建路径用它消除含义会变化的 descriptor 位。函数不持锁、不能睡眠，
 * 也不获取页表所有权；根页后一页起的连续空间被当作页表池。
 *
 * 返回下一空闲页表页的物理地址，供调用者了解实际消耗；不存在 errno 失败出口。
 * 成功时只完成内存中的描述符构造，调用者必须随后执行 DSB 并把根地址装入 TTBR0。
 */
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptval_t clrmask)
{
	/* MMU 关闭时 C 指针数值就是可直接使用的物理地址；ptep 是页表池领取游标。 */
	phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE; /* MMU is off */
	pgprot_t text_prot = PAGE_KERNEL_ROX;
	pgprot_t data_prot = PAGE_KERNEL;

	pgprot_val(text_prot) &= ~clrmask;
	pgprot_val(data_prot) &= ~clrmask;
	/* 文本保持只读可执行，数据保持可读写不可执行；两者统一清除调用者要求的位。 */

	/* MMU is off; pointer casts to phys_addr_t are safe */
	/*
	 * MMU 关闭，所以链接地址、指针和物理地址可直接互转。先覆盖 _stext 到
	 * __initdata_begin 的早期文本，再覆盖到 _end 的数据；两段共享 ptep 游标，
	 * 后一段会复用边界处已创建的中间页表，而不会重复领取。
	 */
	map_range(&ptep, (u64)_stext, (u64)__initdata_begin,
		  (phys_addr_t)_stext, text_prot, IDMAP_ROOT_LEVEL,
		  (pte_t *)pg_dir, false, 0);
	map_range(&ptep, (u64)__initdata_begin, (u64)_end,
		  (phys_addr_t)__initdata_begin, data_prot, IDMAP_ROOT_LEVEL,
		  (pte_t *)pg_dir, false, 0);

	/* 返回值仅报告页表池末端；页表存储仍由静态启动区所有，后续由启动流程复用。 */
	return ptep;
}
