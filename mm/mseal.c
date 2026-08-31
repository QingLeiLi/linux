// SPDX-License-Identifier: GPL-2.0
/*
 *  Implement mseal() syscall.
 *
 *  Copyright (c) 2023,2024 Google, Inc.
 *
 *  Author: Jeff Xu <jeffxu@chromium.org>
 */
/* 本文件实现 64 位 MMU 构建中的 mseal()，把 VMA 元数据变更永久封死。 */

#include <linux/mempolicy.h>
#include <linux/minmax.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include "internal.h"

/*
 * mseal() disallows an input range which contain unmapped ranges (VMA holes).
 *
 * It disallows unmapped regions from start to end whether they exist at the
 * start, in the middle, or at the end of the range, or any combination thereof.
 *
 * This is because after sealing a range, there's nothing to stop memory mapping
 * of ranges in the remaining gaps later, meaning that the user might then
 * wrongly consider the entirety of the mseal()'d range to be sealed when it
 * in fact isn't.
 */
/*
 * mseal() 拒绝包含未映射区间（VMA 洞）的输入。无论洞位于开头、中间、末尾或
 * 多处都必须失败：洞在调用后仍可被 mmap 成不同属性，若只密封现有 VMA，用户
 * 会误以为整个请求范围都已受到保护。第一遍完整性检查因此也是常见错误下的
 * 原子性边界——确认无洞后才开始不可撤销地设置 seal。
 */

/*
 * Does the [start, end) range contain any unmapped memory?
 *
 * We ensure that:
 * - start is part of a valid VMA.
 * - end is part of a valid VMA.
 * - no gap (unallocated memory) exists between start and end.
 */
/*
 * 检查 [start,end) 是否含未映射内存：start 与 end 覆盖到的地址都必须属于
 * 有效 VMA，并且两端之间不得出现任何未分配空洞。
 */
/*
 * range_contains_unmapped() - 验证待密封半开区间由连续 VMA 完整覆盖。
 * 业务背景：do_mseal() 的第一遍只验证、不修改，避免明确的地址错误造成部分密封。
 * 入参：@mm 是当前进程借用地址空间；@start/@end 是页对齐半开虚拟地址，
 * start < end。函数只读 VMA tree，不接管任何对象。
 * 出参/返回：发现首洞或最后 VMA 未覆盖 end 时返回 true，完整覆盖返回 false。
 * 注意事项：调用者必须持 @mm 的 mmap 写锁；iterator 当前以 current->mm 构造，
 * 因而本函数仅能以 current->mm 作为 @mm 调用。扫描不分配、不睡眠。
 */
static bool range_contains_unmapped(struct mm_struct *mm,
		unsigned long start, unsigned long end)
{
	/* vma 是借用游标；prev_end 是已验证连续覆盖的尾后地址。 */
	struct vm_area_struct *vma;
	unsigned long prev_end = start;
	VMA_ITERATOR(vmi, current->mm, start);

	/* 每个 VMA 起点若越过 prev_end，就证明两段之间存在不可密封的洞。 */
	for_each_vma_range(vmi, vma, end) {
		if (vma->vm_start > prev_end)
			return true;

		prev_end = vma->vm_end;
	}

	/* 没有更多 VMA 但连续覆盖尚未到 end，表示尾部是洞。 */
	return prev_end < end;
}

/*
 * mseal_apply() - 第二遍逐段设置 VMA_SEALED_BIT，必要时拆分或合并 VMA。
 *
 * 业务背景：range_contains_unmapped() 已保证范围连续；本函数用
 * vma_modify_flags() 把边界对齐到请求子区间，再以 vma_start_write() 协议提交
 * seal。已有 seal 的区间保持不变，使重复 mseal 幂等。
 * 入参：@mm 是持 mmap 写锁的 current 地址空间；@start/@end 是完整映射的半开区间。
 * 出参/返回：全部成功返回 0；split/merge/Maple Tree 失败返回 PTR_ERR。失败前
 * 已设置的 seal 不回滚，因为 seal 没有逆操作，调用者可能观察到部分更新。
 * 注意事项：可因 VMA 元数据分配而睡眠；@prev 与 iterator 必须随 merge/split
 * 结果推进。VMA_SEALED_BIT 是单向状态，后续 munmap/mremap/mprotect 等检查它。
 */
static int mseal_apply(struct mm_struct *mm,
		unsigned long start, unsigned long end)
{
	/* vma/prev 是当前及前驱借用指针，供 vma_modify_flags 判断 merge 邻接关系。 */
	struct vm_area_struct *vma, *prev;
	VMA_ITERATOR(vmi, mm, start);

	/* We know there are no gaps so this will be non-NULL. */
	/* 第一遍已证明无洞，因此 start 所在 VMA 必然存在；定位其前驱以支持合并。 */
	vma = vma_iter_load(&vmi);
	prev = vma_prev(&vmi);
	if (start > vma->vm_start)
		prev = vma;

	/* 逐 VMA 截取与请求的交集，curr_start/curr_end 始终构成半开区间。 */
	for_each_vma_range(vmi, vma, end) {
		const unsigned long curr_start = max(vma->vm_start, start);
		const unsigned long curr_end = min(vma->vm_end, end);

		/* 已密封 VMA 是幂等快路径；未密封时才可能 split/merge。 */
		if (!vma_test(vma, VMA_SEALED_BIT)) {
			/* vma_flags 是本段现有标志的值副本，追加 seal 后交给重塑 helper。 */
			vma_flags_t vma_flags = vma->flags;

			vma_flags_set(&vma_flags, VMA_SEALED_BIT);

			/*
			 * vma_modify_flags 只准备恰好覆盖交集的 VMA，可能拆分/合并并返回新指针；
			 * 它不执行最终 flag 写入。错误直接返回，先前段的 seal 保持不可撤销。
			 */
			vma = vma_modify_flags(&vmi, prev, vma, curr_start,
					       curr_end, &vma_flags);
			if (IS_ERR(vma))
				return PTR_ERR(vma);
			/* 写协议稳定目标 VMA，然后才真正发布 SEALED 位。 */
			vma_start_write(vma);
			vma_set_flags(vma, VMA_SEALED_BIT);
		}

		/* prev 必须使用变形后的返回 VMA，供下一轮保持树邻接关系。 */
		prev = vma;
	}

	return 0;
}

/*
 * mseal(2) seals the VM's meta data from
 * selected syscalls.
 *
 * addr/len: VM address range.
 *
 *  The address range by addr/len must meet:
 *   start (addr) must be in a valid VMA.
 *   end (addr + len) must be in a valid VMA.
 *   no gap (unallocated memory) between start and end.
 *   start (addr) must be page aligned.
 *
 *  len: len will be page aligned implicitly.
 *
 *   Below VMA operations are blocked after sealing.
 *   1> Unmapping, moving to another location, and shrinking
 *	the size, via munmap() and mremap(), can leave an empty
 *	space, therefore can be replaced with a VMA with a new
 *	set of attributes.
 *   2> Moving or expanding a different vma into the current location,
 *	via mremap().
 *   3> Modifying a VMA via mmap(MAP_FIXED).
 *   4> Size expansion, via mremap(), does not appear to pose any
 *	specific risks to sealed VMAs. It is included anyway because
 *	the use case is unclear. In any case, users can rely on
 *	merging to expand a sealed VMA.
 *   5> mprotect and pkey_mprotect.
 *   6> Some destructive madvice() behavior (e.g. MADV_DONTNEED)
 *      for anonymous memory, when users don't have write permission to the
 *	memory. Those behaviors can alter region contents by discarding pages,
 *	effectively a memset(0) for anonymous memory.
 *
 *  flags: reserved.
 *
 * return values:
 *  zero: success.
 *  -EINVAL:
 *   invalid input flags.
 *   start address is not page aligned.
 *   Address range (start + len) overflow.
 *  -ENOMEM:
 *   addr is not a valid address (not allocated).
 *   end (start + len) is not a valid address.
 *   a gap (unallocated memory) between start and end.
 *  -EPERM:
 *  - In 32 bit architecture, sealing is not supported.
 * Note:
 *  user can call mseal(2) multiple times, adding a seal on an
 *  already sealed memory is a no-action (no error).
 *
 *  unseal() is not supported.
 */
/*
 * mseal(2) 阻止选定内存管理系统调用修改 VMA 元数据。@addr/@len 描述虚拟地址
 * 范围：start 与 end 都须落在有效 VMA，中间无洞，start 页对齐；len 由内核
 * 隐式向上页对齐。密封后禁止：munmap/mremap 造成的取消映射、移动、收缩；
 * mremap 把其他 VMA 移入或扩大到这里；MAP_FIXED 覆盖；即使扩张风险较小也
 * 暂时禁止的 mremap 扩张；mprotect/pkey_mprotect；以及对不可写匿名内存会
 * 丢弃内容、效果类似清零的破坏性 madvise。flags 预留，当前必须为 0。
 *
 * 成功返回 0。非法 flags、start 未对齐、长度或地址溢出返回 -EINVAL；首尾
 * 未映射或中间有洞返回 -ENOMEM；32 位架构不提供本实现。重复密封已密封范围
 * 是成功的无操作，且不存在 unseal。明确输入错误在第一遍检查前不修改范围；
 * 少见的 split/merge 或 VMA 数量上限错误可能发生在第二遍并留下部分密封。
 */
/*
 * do_mseal() - 校验用户范围并在 mmap 写锁内执行两遍式 VMA 密封。
 * 业务背景：sys_mseal() 的核心实现，先排除可预见错误，再完整性扫描，最后
 * 设置不可逆 SEALED 位，保护代码、只读数据等安全关键映射的属性。
 * 入参：@start 是可带 tag 的用户地址；@len_in 是原始字节数；@flags 当前必须
 * 为 0。均为纯输入，len 向上页对齐但不回写用户。
 * 出参/返回：0 成功/空区间；-EINVAL 表示 flags、对齐或溢出错误；-ENOMEM 表示
 * VMA 洞；-EINTR 表示 killable mmap 锁等待中断；另传播 vma_modify_flags errno。
 * 注意事项：可睡眠；锁保护 VMA 集合不被并发 mmap 修改。第一遍后第二遍内部
 * 失败允许部分密封，已置位 VMA 无法回滚，调用者必须按返回值处理。
 */
int do_mseal(unsigned long start, size_t len_in, unsigned long flags)
{
	/* len 是页对齐长度；end 是尾后地址；ret 沿统一解锁出口传播。 */
	size_t len;
	int ret = 0;
	unsigned long end;
	struct mm_struct *mm = current->mm;

	/* Verify flags not set. */
	/* flags 尚未定义任何位，非零值一律拒绝以保留 ABI 扩展空间。 */
	if (flags)
		return -EINVAL;

	/* 去除体系结构 tag 后，起点仍必须页对齐。 */
	start = untagged_addr(start);
	if (!PAGE_ALIGNED(start))
		return -EINVAL;

	len = PAGE_ALIGN(len_in);
	/* Check to see whether len was rounded up from small -ve to zero. */
	/* 非零 size_t 若 PAGE_ALIGN 回绕成 0，表示接近 SIZE_MAX 的伪负长度溢出。 */
	if (len_in && !len)
		return -EINVAL;

	/* 再检查 start+len 的无符号回绕；空范围则保持幂等成功。 */
	end = start + len;
	if (end < start)
		return -EINVAL;

	if (end == start)
		return 0;

	/* 阶段 2：killable 写锁稳定 VMA tree，并允许等待信号安全中止且无修改。 */
	if (mmap_write_lock_killable(mm))
		return -EINTR;

	/* 第一遍只验证完整覆盖，所有洞类错误在任何 seal 发布之前失败。 */
	if (range_contains_unmapped(mm, start, end)) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Second pass, this should success, unless there are errors
	 * from vma_modify_flags, e.g. merge/split error, or process
	 * reaching the max supported VMAs, however, those cases shall
	 * be rare.
	 */
	/*
	 * 第二遍通常成功；只有 vma_modify_flags 的 merge/split 分配失败或进程达到
	 * VMA 数量上限等少见情况才失败，此时早先已密封的段不会回滚。
	 */
	ret = mseal_apply(mm, start, end);

out:
	/* 所有取得写锁后的出口在此成对释放；ret 精确返回第一/第二遍结果。 */
	mmap_write_unlock(mm);
	return ret;
}

/*
 * mseal() - 系统调用 ABI 包装，转交 do_mseal()。
 * 业务背景：体系结构 syscall 表进入此层，核心 VMA/锁协议集中在 do_mseal()。
 * 入参：@start/@len/@flags 与 do_mseal 相同，均来自用户寄存器且为纯输入。
 * 出参/返回：原样返回 do_mseal() 的 0 或负 errno，无额外副作用。
 * 注意事项：仅 64 位 MMU 构建链接本文件；包装自身不持锁，可能在下层睡眠。
 */
SYSCALL_DEFINE3(mseal, unsigned long, start, size_t, len, unsigned long,
		flags)
{
	return do_mseal(start, len, flags);
}
