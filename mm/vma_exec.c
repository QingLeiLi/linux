// SPDX-License-Identifier: GPL-2.0-only

/*
 * Functions explicitly implemented for exec functionality which however are
 * explicitly VMA-only logic.
 */
/*
 * 本文件放置 exec 专用、但实现边界完全属于 VMA 子系统的操作：先在新 mm 的
 * STACK_TOP_MAX 建立临时匿名栈，待二进制格式和随机化确定最终地址后，再连同
 * 页表向低地址搬移。拆出文件可避免 fs/exec.c 直接操作 maple tree/VMA 协议。
 */

#include "vma_internal.h"
#include "vma.h"

/*
 * Relocate a VMA downwards by shift bytes. There cannot be any VMAs between
 * this VMA and its relocated range, which will now reside at [vma->vm_start -
 * shift, vma->vm_end - shift).
 *
 * This function is almost certainly NOT what you want for anything other than
 * early executable temporary stack relocation.
 */
/*
 * 将 VMA 向低地址移动 @shift 字节；当前 VMA 与目标区间之间不得存在其他 VMA，
 * 新区间为 [vm_start-shift, vm_end-shift)。该接口几乎只适合 exec 早期临时栈
 * 重定位，普通 mremap/栈扩展不应复用，因为其失败恢复语义依赖整个新进程清理。
 */
/*
 * relocate_vma_down() - 把 exec 临时栈的 VMA 和页表整体向低地址搬移。
 *
 * 业务背景：create_init_stack_vma() 在最终栈地址未知时先占 STACK_TOP_MAX；
 * setup_arg_pages() 持 mmap 写锁确定 stack_shift 后调用本函数完成最终定位。
 * 入参：@vma 是新 mm 中借用的临时栈 VMA，调用者持 mmap_write_lock；@shift
 * 是向低地址移动的字节数，须页对齐、不得使地址下溢，且路径中无其他 VMA。
 * 出参/返回：成功返回 vma_shrink() 的 0，VMA 与页表覆盖新区间；目标被占用
 * 返回 -EFAULT，扩展或页表移动失败返回 -ENOMEM。页表部分移动失败不回滚，
 * 由 exec 失败后的 mm teardown 清理。
 * 注意事项：可分配内存并睡眠；vma_expand()/vma_shrink() 修改 maple tree，
 * move_page_tables() 改写页表，TLB gather 延迟释放旧页表。仅能用于尚未提交给
 * current 的 exec mm，否则这种非事务性失败会暴露破碎映射。
 */
int relocate_vma_down(struct vm_area_struct *vma, unsigned long shift)
{
	/*
	 * The process proceeds as follows:
	 *
	 * 1) Use shift to calculate the new vma endpoints.
	 * 2) Extend vma to cover both the old and new ranges.  This ensures the
	 *    arguments passed to subsequent functions are consistent.
	 * 3) Move vma's page tables to the new range.
	 * 4) Free up any cleared pgd range.
	 * 5) Shrink the vma to cover only the new range.
	 */
	/*
	 * 流程为：计算新端点；先把 VMA 扩为覆盖新旧两段以维持 helper 参数一致；
	 * 搬页表；释放旧地址的空页表层级；最后把 VMA 收缩到新区间。
	 */

	/* 变量地图：mm 为所属地址空间；old/new_* 均是虚拟地址，length/shift 为字节。 */
	struct mm_struct *mm = vma->vm_mm;
	unsigned long old_start = vma->vm_start;
	unsigned long old_end = vma->vm_end;
	unsigned long length = old_end - old_start;
	unsigned long new_start = old_start - shift;
	unsigned long new_end = old_end - shift;
	/* vmi 从目标起点查邻接关系；vmg 描述临时扩展到 [new_start, old_end)。 */
	VMA_ITERATOR(vmi, mm, new_start);
	VMG_STATE(vmg, mm, &vmi, new_start, old_end, EMPTY_VMA_FLAGS,
		  vma->vm_pgoff);
	/* next 限制 free_pgd_range 上界；tlb 批量失效/释放；pmc 描述页表搬移。 */
	struct vm_area_struct *next;
	struct mmu_gather tlb;
	PAGETABLE_MOVE(pmc, vma, vma, old_start, new_start, length);

	/* 防御算术异常；有效向下移动应保持 new_start <= new_end。 */
	BUG_ON(new_start > new_end);

	/*
	 * ensure there are no vmas between where we want to go
	 * and where we are
	 */
	/* 确认目标起点之后遇到的就是 @vma，否则中间 VMA 会被扩展覆盖。 */
	if (vma != vma_next(&vmi))
		return -EFAULT;

	vma_iter_prev_range(&vmi);
	/*
	 * cover the whole range: [new_start, old_end)
	 */
	/* 先回到目标前的 iterator 位置，再把现有 VMA 临时扩张为新旧范围并集。 */
	vmg.target = vma;
	if (vma_expand(&vmg))
		return -ENOMEM;

	/*
	 * move the page tables downwards, on failure we rely on
	 * process cleanup to remove whatever mess we made.
	 */
	/*
	 * 页表向低地址移动；for_stack 告诉通用搬移器采用栈场景约束。若只移动了
	 * 部分 length，VMA 已扩张且页表可能分散，不能局部回滚，只能让 exec 清理 mm。
	 */
	pmc.for_stack = true;
	if (length != move_page_tables(&pmc))
		return -ENOMEM;

	/* 阶段 4：收集旧范围残留页表，完成后统一 TLB flush 与页表页释放。 */
	tlb_gather_mmu(&tlb, mm);
	next = vma_next(&vmi);
	if (new_end > old_start) {
		/*
		 * when the old and new regions overlap clear from new_end.
		 */
		/* 新旧区间重叠时 [new_end, old_end) 才是纯旧尾部，从 new_end 开始清理。 */
		free_pgd_range(&tlb, new_end, old_end, new_end,
			next ? next->vm_start : USER_PGTABLES_CEILING);
	} else {
		/*
		 * otherwise, clean from old_start; this is done to not touch
		 * the address space in [new_end, old_start) some architectures
		 * have constraints on va-space that make this illegal (IA64) -
		 * for the others its just a little faster.
		 */
		/*
		 * 不重叠时仅清理 [old_start,old_end)，故意避开空洞 [new_end,old_start)；
		 * IA64 等架构禁止触碰某些 VA 空洞，其他架构也因此少做无用扫描。
		 */
		free_pgd_range(&tlb, old_start, old_end, new_end,
			next ? next->vm_start : USER_PGTABLES_CEILING);
	}
	tlb_finish_mmu(&tlb);

	vma_prev(&vmi);
	/* Shrink the vma to just the new range */
	/* 最后把临时并集收缩到新区间；此返回值直接成为调用者看到的最终结果。 */
	return vma_shrink(&vmi, vma, new_start, new_end, vma->vm_pgoff);
}

/*
 * Establish the stack VMA in an execve'd process, located temporarily at the
 * maximum stack address provided by the architecture.
 *
 * We later relocate this downwards in relocate_vma_down().
 *
 * This function is almost certainly NOT what you want for anything other than
 * early executable initialisation.
 *
 * On success, returns 0 and sets *vmap to the stack VMA and *top_mem_p to the
 * maximum addressable location in the stack (that is capable of storing a
 * system word of data).
 */
/*
 * 为 execve 的新进程建立临时栈 VMA，初始放在架构提供的最大栈地址；稍后由
 * relocate_vma_down() 向低地址移动。该函数同样只适用于 exec 早期初始化。
 * 成功返回 0，*@vmap 获得栈 VMA 的借用指针，*@top_mem_p 获得可存放一个
 * machine word 的最高栈地址。
 */
/*
 * create_init_stack_vma() - 在尚未提交的新 mm 中创建一页临时匿名栈。
 *
 * 业务背景：bprm_mm_init() 尚不知道 ELF/架构/ASLR 决定的最终栈顶，先在
 * STACK_TOP_MAX 建立带 INCOMPLETE_SETUP 标志的 VMA，供参数/环境复制使用。
 * 入参：@mm 是调用者持有的新地址空间，尚未发布给 current；@vmap 是非 NULL
 * 输出指针；@top_mem_p 是非 NULL 地址输出。函数借用 @mm，不接管其引用。
 * 出参/返回：成功返回 0，*@vmap 指向已插入 maple tree 的 VMA，*@top_mem_p
 * 为 vm_end-sizeof(void *)；分配失败 -ENOMEM，锁被信号中断 -EINTR，KSM 或
 * insert_vm_struct 失败传播其 errno。所有失败把 *@vmap 置 NULL 并释放 VMA。
 * 注意事项：可睡眠；内部取得 killable mmap 写锁，与 ksmd/VMA 修改串行。
 * insert 成功是发布到该私有 mm 的边界，随后设置 total_vm/stack_vm；错误路径
 * 必须按 KSM 注册、mmap 锁、VMA ownership 的逆序回滚。
 */
int create_init_stack_vma(struct mm_struct *mm, struct vm_area_struct **vmap,
			  unsigned long *top_mem_p)
{
	/* flags 描述临时栈权限/未完成状态；err 沿 cleanup 标签传播。 */
	unsigned long flags = VM_STACK_FLAGS | VM_STACK_INCOMPLETE_SETUP;
	int err;
	/* vma 初始由本函数独占，insert 成功后 ownership 交给 @mm。 */
	struct vm_area_struct *vma = vm_area_alloc(mm);

	if (!vma)
		return -ENOMEM;

	/* 匿名属性必须在发布前确定，后续 fault 才会建立 anon_vma/匿名页。 */
	vma_set_anonymous(vma);

	/* 阶段 2：以 killable 写锁串行 VMA tree 与 KSM mm 注册；中断时只释放 vma。 */
	if (mmap_write_lock_killable(mm)) {
		err = -EINTR;
		goto err_free;
	}

	/*
	 * Need to be called with mmap write lock
	 * held, to avoid race with ksmd.
	 */
	/* ksm_execve 必须在 mmap 写锁内，使新 mm 的 KSM 登记不与 ksmd 扫描竞态。 */
	err = ksm_execve(mm);
	if (err)
		goto err_ksm;

	/*
	 * Place the stack at the largest stack address the architecture
	 * supports. Later, we'll move this to an appropriate place. We don't
	 * use STACK_TOP because that can depend on attributes which aren't
	 * configured yet.
	 */
	/*
	 * 最终栈属性尚未配置，STACK_TOP 可能依赖这些属性，故临时使用架构绝对上限
	 * STACK_TOP_MAX。先建一页并保留 INCOMPLETE_SETUP，稍后再改权限和搬移。
	 */
	VM_WARN_ON_ONCE(VM_STACK_FLAGS & VM_STACK_INCOMPLETE_SETUP);
	vma->vm_end = STACK_TOP_MAX;
	vma->vm_start = vma->vm_end - PAGE_SIZE;
	/* soft-dirty 可用时把初始匿名映射标记为已写语义，再从 flags 派生页保护。 */
	if (pgtable_supports_soft_dirty())
		flags |= VM_SOFTDIRTY;
	vm_flags_init(vma, flags);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

	/* 阶段 4：插入 maple tree 是 VMA 对 @mm 可查找的发布点。 */
	err = insert_vm_struct(mm, vma);
	if (err)
		goto err;

	/* 新 mm 此时只有这一页栈；解锁后再把两个成功输出交给 exec 调用者。 */
	mm->stack_vm = mm->total_vm = 1;
	mmap_write_unlock(mm);
	*vmap = vma;
	*top_mem_p = vma->vm_end - sizeof(void *);
	return 0;

err:
	/* insert 失败但 KSM 登记已成功，先撤销 KSM 状态再落入通用清理。 */
	ksm_exit(mm);
err_ksm:
	/* 到达此处仍持 mmap 写锁；释放后才销毁未发布 VMA。 */
	mmap_write_unlock(mm);
err_free:
	/* 所有失败统一清空输出，防止 bprm cleanup 误用悬空 vma。 */
	*vmap = NULL;
	vm_area_free(vma);
	return err;
}
