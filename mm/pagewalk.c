// SPDX-License-Identifier: GPL-2.0
#include <linux/pagewalk.h>
#include <linux/highmem.h>
#include <linux/sched.h>
#include <linux/hugetlb.h>
#include <linux/mmu_context.h>
#include <linux/swap.h>

#include <asm/tlbflush.h>

#include "internal.h"

/*
 * 本文件把 VMA/内核/地址空间的范围请求下降为 PGD→…→PTE 回调；mm_walk
 * 由调用者持有，ops 是借用操作表。各层只在调用者约定的锁与 VMA 生命周期
 * 内读取页表，install_pte 是受限的内部能力，不能从导出安全接口进入。
 */
/*
 * We want to know the real level where a entry is located ignoring any
 * folding of levels which may be happening. For example if p4d is folded then
 * a missing entry found at level 1 (p4d) is actually at level 0 (pgd).
 */
/*
 * real_depth() - 把硬件折叠的页表层级还原为逻辑缺页层级。
 * 业务背景：pte_hole 回调需要报告真实 PGD/P4D/PUD/PMD 深度而非折叠别名。
 * 入参：depth 是当前下降层级。出参/返回：返回折叠校正后的 0..3 深度。
 * 注意事项：只依赖编译期 PTRS_PER_*；不访问页表、不睡眠，配置决定结果。
 */
static int real_depth(int depth)
{
	if (depth == 3 && PTRS_PER_PMD == 1)
		depth = 2;
	if (depth == 2 && PTRS_PER_PUD == 1)
		depth = 1;
	if (depth == 1 && PTRS_PER_P4D == 1)
		depth = 0;
	return depth;
}

/*
 * walk_pte_range_inner() - 在已建立的 PTE 映射/锁窗口内逐项分派或安装。
 * 业务背景：这是 page-walk 的叶层，向 ops 回调交付每页区间。
 * 入参：pte 指向当前可用页表项，addr/end 为半开区间，walk 借用回调状态。
 * 出参/返回：0 完成，回调 errno 立即停止；install 时把 new_pte 发布到 mm。
 * 注意事项：调用者负责 PTE map/PTL 生命周期；install_pte 与 pte_entry 二选一，
 * 新建 PTE 后必须更新架构 MMU cache，不能跳过该可见性步骤。
 */
static int walk_pte_range_inner(pte_t *pte, unsigned long addr,
				unsigned long end, struct mm_walk *walk)
{
	const struct mm_walk_ops *ops = walk->ops;
	int err = 0;

	/* 每轮覆盖一个 PAGE_SIZE；结束判断在递增前避免 end 溢出。 */
	for (;;) {
		/* 空 PTE 且内部安装者存在时创建；否则原样交给观察回调。 */
		if (ops->install_pte && pte_none(ptep_get(pte))) {
			pte_t new_pte;

			err = ops->install_pte(addr, addr + PAGE_SIZE, &new_pte,
					       walk);
			if (err)
				break;

			/* set_pte_at 是真实发布点；随后 cache hook 让需要的软件 TLB 的架构可见。 */
			set_pte_at(walk->mm, addr, pte, new_pte);
			/* Non-present before, so for arches that need it. */
			if (!WARN_ON_ONCE(walk->no_vma))
				update_mmu_cache(walk->vma, addr, pte);
		} else {
			err = ops->pte_entry(pte, addr, addr + PAGE_SIZE, walk);
			if (err)
				break;
		}
		if (addr >= end - PAGE_SIZE)
			/* 半开区间最后一页已处理，不能先 addr+=PAGE_SIZE 再比较而发生溢出。 */
			break;
		addr += PAGE_SIZE;
		pte++;
	}
	return err;
}

/*
 * walk_pte_range() - 取得适合当前 walk 模式的 PTE 映射与锁，再调用叶层。
 * 业务背景：普通 VMA 需要 PTL 防回收，内核/无 VMA walk 只能使用相应映射方式。
 * 入参：pmd/addr/end 指定已下降范围，walk 携带 mm、vma 与回调。出参：errno。
 * 注意事项：pte=NULL 时把 ACTION_AGAIN 交给上层重试；map/unmap、lock/unlock
 * 必须成对，不能把内核页表的无锁访问误用到可能被用户路径回收的页表。
 */
static int walk_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pte_t *pte;
	int err = 0;
	spinlock_t *ptl;

	/* 无 VMA 模式可能访问 firmware/内核异常页表，选择不会施加用户校验的 helper。 */
	if (walk->no_vma) {
		/*
		 * pte_offset_map() might apply user-specific validation.
		 * Indeed, on x86_64 the pmd entries set up by init_espfix_ap()
		 * fit its pmd_bad() check (_PAGE_NX set and _PAGE_RW clear),
		 * and CONFIG_EFI_PGT_DUMP efi_mm goes so far as to walk them.
		 */
		if (walk->mm == &init_mm || addr >= TASK_SIZE)
			pte = pte_offset_kernel(pmd, addr);
		else
			pte = pte_offset_map(pmd, addr);
		if (pte) {
			/* 映射成功才进入叶层；用户 PTE 映射必须在回调后 unmap。 */
			err = walk_pte_range_inner(pte, addr, end, walk);
			if (walk->mm != &init_mm && addr < TASK_SIZE)
				pte_unmap(pte);
		}
	} else {
			/* 非安装回调只借用当前 PTE/PTL，正负返回值由调用者语义决定。 */
		/* 正常 VMA 路径由 PTL 稳定 PTE 页与条目，回调仅在这把锁持有期间执行。 */
		pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
		if (pte) {
			/* map_lock 同时取得 PTL；unmap_unlock 是该路径唯一释放点。 */
			err = walk_pte_range_inner(pte, addr, end, walk);
			pte_unmap_unlock(pte, ptl);
		}
	}
	if (!pte)
		/* PTE 页被并发回收/无法映射时，让上层从安全层级重新开始。 */
		walk->action = ACTION_AGAIN;
	return err;
}

/*
 * walk_pmd_range() - 遍历一个 PUD 下的 PMD，处理空洞、巨大映射和 PTE 下降。
 * 业务背景：中间层按 ops 的回调需求决定分配、报告 hole 或继续下降。
 * 入参：pud 与地址半开范围为借用页表位置；walk 是可由回调修改 action 的状态。
 * 出参/返回：0 或回调/分配错误。注意事项：PUD 可能并发拆分，ACTION_AGAIN
 * 要重读当前 PMD；只有 VMA 路径可 split huge PMD，内核路径不能擅自拆页表。
 */
static int walk_pmd_range(pud_t *pud, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pud_t pudval = pudp_get(pud);
	/* pudval 是无锁快照，仅用于检测不能下降的 leaf/缺项；其余访问由下层协议复核。 */
	pmd_t *pmd;
	unsigned long next;
	const struct mm_walk_ops *ops = walk->ops;
	bool has_handler = ops->pte_entry;
	bool has_install = ops->install_pte;
	int err = 0;
	int depth = real_depth(3);

	/*
	 * For PTE handling, pte_offset_map_lock() takes care of checking
	 * whether there actually is a page table. But it also has to be
	 * very careful about concurrent page table reclaim.
	 *
	 * Similarly, we have to be careful here - a PUD entry that points
	 * to a PMD table cannot go away, so we can just walk it. But if
	 * it's something else, we need to ensure we didn't race something,
	 * so need to retry.
	 *
	 * A pertinent example of this is a PUD refault after PUD split -
	 * we will need to split again or risk accessing invalid memory.
	 */
	/* 非 table 的 PUD 不能安全下降；上层会重新验证，避免 PUD split 后 UAF。 */
	if (!pud_present(pudval) || pud_leaf(pudval)) {
		walk->action = ACTION_AGAIN;
		return 0;
	}

	pmd = pmd_offset(pud, addr);
	/* 每轮处理一个 PMD 边界，callback 可通过 action 请求重试或剪枝。 */
	do {
again:
		next = pmd_addr_end(addr, end);
		/* 空 PMD：内部安装者分配下级表；观察者只能收到 hole 回调。 */
		if (pmd_none(*pmd)) {
			/* 分配成功后继续同一 PMD；hole 回调成功也只跳过当前条目。 */
			if (has_install)
				err = __pte_alloc(walk->mm, pmd);
			else if (ops->pte_hole)
				err = ops->pte_hole(addr, next, depth, walk);
			if (err)
				break;
			if (!has_install)
				continue;
		}

		walk->action = ACTION_SUBTREE;
		/* 默认下降，回调可改为 AGain/CONTINUE 改写该默认控制流。 */

		/*
		 * This implies that each ->pmd_entry() handler
		 * needs to know about pmd_trans_huge() pmds
		 */
		if (ops->pmd_entry)
			/* PMD 回调可能观察 huge 状态并修改 action，返回后必须先检查 errno。 */
			err = ops->pmd_entry(pmd, addr, next, walk);
		if (err)
			break;

		if (walk->action == ACTION_AGAIN)
			/* ACTION_AGAIN 表示 callback 或并发结构变化要求重新读取同一 PMD。 */
			goto again;
		if (walk->action == ACTION_CONTINUE)
			continue;

		if (!has_handler) { /* No handlers for lower page tables. */
			/* 没有 lower consumer 时仅允许 install；否则该子树无可观察工作。 */
			/* 纯安装时保留现有 huge PMD，避免无消费者却拆分大页。 */
			if (!has_install)
				continue; /* Nothing to do. */
			/*
			 * We are ONLY installing, so avoid unnecessarily
			 * splitting a present huge page.
			 */
			if (pmd_present(*pmd) && pmd_trans_huge(*pmd))
				continue;
		}

		/* 有 VMA 时 split helper 协调 THP；无 VMA 的 leaf 只能跳过以保护内核映射。 */
		if (walk->vma)
			split_huge_pmd(walk->vma, pmd, addr);
		else if (pmd_leaf(*pmd) || !pmd_present(*pmd))
			continue; /* Nothing to do. */

		err = walk_pte_range(pmd, addr, next, walk);
		/* 子层完成后再次检查 action，因为 PTE 映射失败可请求重试父层。 */
		if (err)
			break;

		if (walk->action == ACTION_AGAIN)
			goto again;

	} while (pmd++, addr = next, addr != end);

	return err;
}

/*
 * walk_pud_range() - 在 P4D 范围内执行与 PMD 层对称的空洞、回调与下降协议。
 * 业务背景：统一五级/折叠架构的递归遍历。入参：p4d、地址范围和 walk 均借用。
 * 出参/返回：0 或错误；注意事项：回调可改变 action，巨大 PUD 仅在 VMA 允许
 * 的情况下拆分，避免安装下级 PTE 时破坏现有大页映射。
 */
static int walk_pud_range(p4d_t *p4d, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pud_t *pud;
	/* has_handler 汇总所有低层消费者，决定是否有必要 materialize/下降页表。 */
	unsigned long next;
	const struct mm_walk_ops *ops = walk->ops;
	bool has_handler = ops->pmd_entry || ops->pte_entry;
	bool has_install = ops->install_pte;
	int err = 0;
	int depth = real_depth(2);

	pud = pud_offset(p4d, addr);
	/* PUD 循环与 PMD 一样以 addr_end 保证不跨越当前表项边界。 */
	do {
 again:
		next = pud_addr_end(addr, end);
		if (pud_none(*pud)) {
			/* PUD 空洞与 PMD 同理：安装下级或向 pte_hole 报告逻辑深度。 */
			if (has_install)
				err = __pmd_alloc(walk->mm, pud, addr);
			else if (ops->pte_hole)
				err = ops->pte_hole(addr, next, depth, walk);
			if (err)
				break;
			if (!has_install)
				continue;
		}

		walk->action = ACTION_SUBTREE;
		/* 每个 PUD 回调前重置 action，防止上一条目的指令泄漏到本条目。 */

		if (ops->pud_entry)
			/* PUD 回调同样先于下降，以便消费者拦截或剪枝该子树。 */
			err = ops->pud_entry(pud, addr, next, walk);
		if (err)
			break;

		if (walk->action == ACTION_AGAIN)
			goto again;
		if (walk->action == ACTION_CONTINUE)
			/* CONTINUE 跳过本 PUD 的下级表，但仍推进到 next。 */
			continue;

		if (!has_handler) { /* No handlers for lower page tables. */
			/* 仅安装路径不拆 present huge PUD，保持原有巨大映射性能与语义。 */
			if (!has_install)
				continue; /* Nothing to do. */
			/*
			 * We are ONLY installing, so avoid unnecessarily
			 * splitting a present huge page.
			 */
			if (pud_present(*pud) && pud_trans_huge(*pud))
				continue;
		}

		if (walk->vma)
			/* VMA 巨页拆分由 helper 与 fault/迁移协议协调，不能直接改条目。 */
			split_huge_pud(walk->vma, pud, addr);
		else if (pud_leaf(*pud) || !pud_present(*pud))
			continue; /* Nothing to do. */

		err = walk_pmd_range(pud, addr, next, walk);
		if (err)
			break;

		if (walk->action == ACTION_AGAIN)
			goto again;
	} while (pud++, addr = next, addr != end);
	/* 循环正常结束时所有已下降子树均已返回；err 保留第一个失败原因。 */

	return err;
}

/* P4D 结束后不持任何子层锁；锁只由 walk_pte_range/hugetlb 路径局部取得。 */
/*
 * walk_p4d_range() - 处理 PGD 下 P4D 条目并按回调需求下降至 PUD。
 * 业务背景：为五级页表提供与折叠架构兼容的递归中继。入参均为借用定位。
 * 出参/返回：回调或分配错误，成功为 0。注意事项：bad/none 条目只能安装或
 * 报 hole；callback 失败立即停止，不能继续使用可能已改变的 action 状态。
 */
static int walk_p4d_range(pgd_t *pgd, unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	p4d_t *p4d;
	/* depth 经 real_depth 校正，确保折叠层的 hole 仍报告逻辑层级。 */
	/* p4d/next 仅在本循环持有，has_install 决定空项是否能被 materialize。 */
	unsigned long next;
	const struct mm_walk_ops *ops = walk->ops;
	bool has_handler = ops->pud_entry || ops->pmd_entry || ops->pte_entry;
	bool has_install = ops->install_pte;
	int err = 0;
	int depth = real_depth(1);

	/* 从当前地址计算首 P4D，循环由 p4d_addr_end 裁剪为单条目区间。 */
	p4d = p4d_offset(pgd, addr);
	/* 首条目与 addr 对齐；每轮尾部递增 p4d 和 addr，二者必须同步推进。 */
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d)) {
			/* clear_bad 后该条目按空洞处理；不能把坏项继续向下解引用。 */
			if (has_install)
				err = __pud_alloc(walk->mm, p4d, addr);
			else if (ops->pte_hole)
				err = ops->pte_hole(addr, next, depth, walk);
			if (err)
				break;
			if (!has_install)
				continue;
		}
		if (ops->p4d_entry) {
			/* P4D 是可选层；配置折叠时 helpers 仍保持统一递归接口。 */
			err = ops->p4d_entry(p4d, addr, next, walk);
			if (err)
				break;
		}
		if (has_handler || has_install)
			/* 无低层回调且不安装时不下降，减少不必要页表访问。 */
			err = walk_pud_range(p4d, addr, next, walk);
		if (err)
			break;
	} while (p4d++, addr = next, addr != end);

	return err;
}

/*
 * walk_pgd_range() - 选择根 PGD 并启动用户或内核页表的逐级遍历。
 * 业务背景：所有非 HugeTLB range 最终从此根层进入递归。入参：addr/end 为
 * 半开地址范围，walk 可指定替代 pgd。出参/返回：0 或下层错误。
 * 注意事项：walk->pgd 用于特殊内核页表；ops 的 install 能分配下级表，安全
 * wrapper 会在进入前拒绝该能力，避免外部调用修改页表。
 */
static int walk_pgd_range(unsigned long addr, unsigned long end,
			  struct mm_walk *walk)
{
	pgd_t *pgd;
	/* root 层没有更高父项可重试，bad entry 清除后按普通空洞处理。 */
	/* has_handler 汇总所有子层回调，防止纯观察空操作无谓下降完整页表树。 */
	unsigned long next;
	const struct mm_walk_ops *ops = walk->ops;
	bool has_handler = ops->p4d_entry || ops->pud_entry || ops->pmd_entry ||
		ops->pte_entry;
	bool has_install = ops->install_pte;
	int err = 0;

	/* 调试/内核调用可提供非 mm->pgd 的根；普通用户 walk 从 mm 根开始。 */
	if (walk->pgd)
		/* 自定义 pgd 由调试/架构调用者借用，偏移仍按当前虚拟 addr 计算。 */
		pgd = walk->pgd + pgd_index(addr);
	else
		pgd = pgd_offset(walk->mm, addr);
	do {
		/* PGD 每轮覆盖一个 root entry，空/bad 项先清理再决定安装或 hole。 */
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd)) {
			/* 根条目为空时只有内部安装或 hole 回调两种合法结果。 */
			if (has_install)
				err = __p4d_alloc(walk->mm, pgd, addr);
			else if (ops->pte_hole)
				err = ops->pte_hole(addr, next, 0, walk);
			if (err)
				break;
			if (!has_install)
				continue;
		}
		if (ops->pgd_entry) {
			/* 根层回调可在进入 P4D 前统计或中止；错误不得部分继续。 */
			err = ops->pgd_entry(pgd, addr, next, walk);
			if (err)
				break;
		}
		if (has_handler || has_install)
			err = walk_p4d_range(pgd, addr, next, walk);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);
	/* 根层遍历不自行获取 mmap 锁，依赖进入 walker 前的调用者锁断言。 */

	return err;
}

#ifdef CONFIG_HUGETLB_PAGE
/* HugeTLB 分支将普通多级页表 walk 替换为按 hstate 大小的逻辑 entry walk。 */
/*
 * hugetlb_entry_end() - 返回不跨越一个逻辑 HugeTLB entry 的终点。
 * 业务背景：HugeTLB 页大小由 hstate 决定，不能沿用固定 PMD/PUD 边界。
 * 入参：h 借用 hstate，addr/end 为半开范围；返回 min(huge boundary,end)。
 * 注意事项：只做算术，不取得锁；调用者必须先稳定 VMA/hstate 生命周期。
 */
static unsigned long hugetlb_entry_end(struct hstate *h, unsigned long addr,
				       unsigned long end)
{
	unsigned long boundary = (addr & huge_page_mask(h)) + huge_page_size(h);
	/* 对齐向下后加一 huge page 得到当前逻辑 entry 尾；min 保留调用者 end。 */

	return min(boundary, end);
}

/*
 * walk_hugetlb_range() - 在 hugetlb VMA 的读锁下按逻辑 huge entry 分派回调。
 * 业务背景：HugeTLB 具有独立页表/共享规则，不能经通用 PTE 下降路径处理。
 * 入参：walk->vma 必为 HugeTLB VMA；addr/end 是范围。出参：0 或回调 errno。
 * 注意事项：hugetlb_vma_lock_read 覆盖 walk；缺项以 depth=-1 报 hole；关闭
 * CONFIG_HUGETLB_PAGE 时下方桩不访问参数并始终成功。
 */
static int walk_hugetlb_range(unsigned long addr, unsigned long end,
			      struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	/* hstate、hmask、sz 从此 VMA 派生，读锁前不得跨 VMA 保存为长期引用。 */
	struct hstate *h = hstate_vma(vma);
	unsigned long next;
	unsigned long hmask = huge_page_mask(h);
	unsigned long sz = huge_page_size(h);
	pte_t *pte;
	const struct mm_walk_ops *ops = walk->ops;
	int err = 0;
	/* callback 接收的 pte 仅在 hugetlb VMA read lock 下有效，不能跨函数保存。 */

	/* 读锁阻止 VMA 内 huge PTE 结构在回调期间被拆除或重组。 */
	hugetlb_vma_lock_read(vma);
	/* 每个逻辑 huge entry 独立查找；不存在时仍允许 pte_hole 观察空洞。 */
	do {
		next = hugetlb_entry_end(h, addr, end);
		pte = hugetlb_walk(vma, addr & hmask, sz);
		/* hugetlb_walk 返回物理 PTE 指针，hugetlb VMA 锁保证回调窗口有效。 */
		if (pte)
			/* 有 PTE 时回调在同一 hugetlb VMA 读锁窗口执行；空项则只通知 hole。 */
			err = ops->hugetlb_entry(pte, hmask, addr, next, walk);
		else if (ops->pte_hole)
			err = ops->pte_hole(addr, next, -1, walk);
		if (err)
			break;
	} while (addr = next, addr != end);
	hugetlb_vma_unlock_read(vma);

	return err;
}

#else /* CONFIG_HUGETLB_PAGE */
/* 配置桩不读取 addr/end/walk，编译器可消除无 HugeTLB 内核中的所有相关工作。 */
/* 关闭 HugeTLB 配置时不存在可遍历的此类 VMA；保留同签名桩简化上层分支。 */
static int walk_hugetlb_range(unsigned long addr, unsigned long end,
			      struct mm_walk *walk)
{
	return 0;
}

#endif /* CONFIG_HUGETLB_PAGE */

/*
 * Decide whether we really walk over the current vma on [@start, @end)
 * or skip it via the returned value. Return 0 if we do walk over the
 * current vma, and return 1 if we skip the vma. Negative values means
 * error, where we abort the current walk.
 */
static int walk_page_test(unsigned long start, unsigned long end,
			struct mm_walk *walk)
{
	/* test_walk 优先拥有策略；其正值只表示跳过，不是最终 API 返回。 */
	struct vm_area_struct *vma = walk->vma;
	const struct mm_walk_ops *ops = walk->ops;

	if (ops->test_walk)
		/* callback 的返回直接传播给上层，默认 PFNMAP 过滤不再参与。 */
		return ops->test_walk(start, end, walk);

	/*
	 * vma(VM_PFNMAP) doesn't have any valid struct pages behind VM_PFNMAP
	 * range, so we don't walk over it as we do for normal vmas. However,
	 * Some callers are interested in handling hole range and they don't
	 * want to just ignore any single address range. Such users certainly
	 * define their ->pte_hole() callbacks, so let's delegate them to handle
	 * vma(VM_PFNMAP).
	 */
	if (vma->vm_flags & VM_PFNMAP) {
		/* depth=-1 表示 VMA 级 hole，而非某个具体页表层级的 none entry。 */
		/* PFNMAP 无 normal struct page，只有显式 hole 消费者可看到此范围。 */
		int err = 1;
		if (ops->pte_hole)
			err = ops->pte_hole(start, end, -1, walk);
		return err ? err : 1;
	}
	return 0;
}

/*
 * __walk_page_range() - 对一个已选定 VMA 执行 pre、页表/hugetlb、post 三阶段。
 * 业务背景：把 VMA 级回调与实际表遍历串为可回滚的协议。入参为该 VMA 内范围。
 * 出参/返回：下层 errno；post_vma 无返回且即使下层失败也承担提交/清理责任。
 * 注意事项：HugeTLB 不支持 install_pte；这是阻止通用安装破坏 huge PTE 语义的门。
 */
static int __walk_page_range(unsigned long start, unsigned long end,
			struct mm_walk *walk)
{
	/* walk 聚合 mm 与回调私有态；VMA 仅在每轮 find_vma 命中后临时赋值。 */
	int err = 0;
	/* pre/post 的调用次序构成 VMA 事务边界；post 无论下层是否报错都可收尾。 */
	struct vm_area_struct *vma = walk->vma;
	const struct mm_walk_ops *ops = walk->ops;
	bool is_hugetlb = is_vm_hugetlb_page(vma);

	/* We do not support hugetlb PTE installation. */
	if (ops->install_pte && is_hugetlb)
		/* 直接 -EINVAL 使调用者知道组合不支持，而不是静默跳过 huge VMA。 */
		/* HugeTLB logical PTE 可能跨物理项，通用 install 无法保证原子性。 */
		return -EINVAL;

	/* pre_vma 可拒绝本 VMA；拒绝时不得调用 post_vma，因为尚未建立该阶段。 */
	if (ops->pre_vma) {
		/* pre 成功后 post 必须配对调用；pre 失败则没有需要提交的本地状态。 */
		err = ops->pre_vma(start, end, walk);
		if (err)
			return err;
	}

	if (is_hugetlb) {
		/* hugetlb callback 缺失时不下降；普通 VMA 则从 PGD 正常递归。 */
		if (ops->hugetlb_entry)
			err = walk_hugetlb_range(start, end, walk);
	} else
		err = walk_pgd_range(start, end, walk);

	if (ops->post_vma)
		/* post 不能失败，调用者只接收之前保存的 err，避免清理错误覆盖主错误。 */
		ops->post_vma(walk);

	return err;
}

/* process_mm_walk_lock() - 断言调用者已按 ops 指定模式稳定 mm 的 VMA/页表关系。 */
static inline void process_mm_walk_lock(struct mm_struct *mm,
					enum page_walk_lock walk_lock)
{
	if (walk_lock == PGWALK_RDLOCK)
		/* read 模式只能查询；任何会安装/修改页表的 caller 都应走写锁协议。 */
		/* PGWALK_VMA_RDLOCK_VERIFY 不要求 mm write lock，其余写模式必须验证。 */
		/* read 模式只验证已有 mmap 锁，写/安装模式必须持 mmap write 锁。 */
		mmap_assert_locked(mm);
	else if (walk_lock != PGWALK_VMA_RDLOCK_VERIFY)
		mmap_assert_write_locked(mm);
}

/* process_vma_walk_lock() - 在 PER_VMA_LOCK 配置下取得或验证目标 VMA 写锁。 */
static inline void process_vma_walk_lock(struct vm_area_struct *vma,
					 enum page_walk_lock walk_lock)
{
#ifdef CONFIG_PER_VMA_LOCK
	/* 每种 enum 明确区分取得锁与验证既有锁；关闭配置时 mm 锁规则仍生效。 */
	switch (walk_lock) {
	/* 枚举每项对应取得、验证或由 mm helper 处理的锁责任，不能合并为泛化“已加锁”。 */
	case PGWALK_WRLOCK:
		/* 写模式主动升级/取得 VMA 写锁，随后允许下层安装或分裂操作。 */
		vma_start_write(vma);
		break;
	case PGWALK_WRLOCK_VERIFY:
		/* VERIFY 模式不重入加锁，仅断言调用者此前已建立正确排他关系。 */
		vma_assert_write_locked(vma);
		break;
	case PGWALK_VMA_RDLOCK_VERIFY:
		/* per-VMA 读锁只允许读取/验证，页表写入仍由其他模式约束。 */
		vma_assert_locked(vma);
		break;
	case PGWALK_RDLOCK:
		/* PGWALK_RDLOCK is handled by process_mm_walk_lock */
		break;
	}
#endif
}

/*
 * See the comment for walk_page_range(), this performs the heavy lifting of the
 * operation, only sets no restrictions on how the walk proceeds.
 *
 * We usually restrict the ability to install PTEs, but this functionality is
 * available to internal memory management code and provided in mm/internal.h.
 */
/*
 * walk_page_range_mm_unsafe() - 遍历 mm 的 VMA 间隙与映射，不限制 install_pte。
 * 业务背景：仅内部 MM 调用者可修改遍历期间页表。入参：mm、半开范围、ops/private。
 * 出参/返回：0 或回调 errno；正的 test_walk 控制值在内部消费为继续。
 * 注意事项：调用者必须已按 walk_lock 持锁；find_vma 游标先推进以防回调重入混淆。
 */
int walk_page_range_mm_unsafe(struct mm_struct *mm, unsigned long start,
		unsigned long end, const struct mm_walk_ops *ops,
		void *private)
{
	/* walk 聚合 mm 与回调私有态；VMA 仅在每轮 find_vma 命中后临时赋值。 */
	int err = 0;
	unsigned long next;
	struct vm_area_struct *vma;
	struct mm_walk walk = {
		.ops		= ops,
		.mm		= mm,
		.private	= private,
	};
	/* 初始化后的 walk 尚无 vma；循环命中 VMA 后再发布该借用指针给回调。 */
	/* unsafe 允许内部 install_pte；公开 wrapper 在调用前已由 check_ops_safe 拦截。 */
	/* walk 只保存借用回调和 private；每次命中 VMA 后才填 mm/vma，避免间隙误用。 */

	if (start >= end)
		/* 参数错误在调用 callback 前返回，避免把空范围误报为 VMA hole。 */
		/* 空/倒置范围没有可安全遍历的地址，提前失败避免 addr_end 溢出路径。 */
		return -EINVAL;

	if (!walk.mm)
		/* NULL mm 与空范围同为调用契约错误；不进入 find_vma 以免解引用。 */
		/* mm 为 NULL 时无法找 VMA 或取得页表根，返回统一 -EINVAL。 */
		return -EINVAL;

	process_mm_walk_lock(walk.mm, ops->walk_lock);
	/* 锁验证成功后才允许把 VMA 地址传给 callback，保持 API 的生命周期承诺。 */

	/* 每轮分类为末尾空洞、两个 VMA 间隙或当前 VMA，再推进 start 到 next。 */
	vma = find_vma(walk.mm, start);
	do {
		if (!vma) { /* after the last vma */
			/* 尾部空洞没有 VMA/PTL，只有请求 hole 回调者可接收该区间。 */
			walk.vma = NULL;
			next = end;
			if (ops->pte_hole)
				err = ops->pte_hole(start, next, -1, &walk);
		} else if (start < vma->vm_start) { /* outside vma */
			/* VMA 前的间隙同样以 depth=-1 作为非页表空洞报告。 */
			walk.vma = NULL;
			next = min(end, vma->vm_start);
			if (ops->pte_hole)
				err = ops->pte_hole(start, next, -1, &walk);
		} else { /* inside vma */
			/* 进入映射后先锁/验证 VMA，再提前取得下一个游标以便回调结束后推进。 */
			process_vma_walk_lock(vma, ops->walk_lock);
			walk.vma = vma;
			next = min(end, vma->vm_end);
			vma = find_vma(mm, vma->vm_end);

			err = walk_page_test(start, next, &walk);
			if (err > 0) {
				/* 正值是内部 flow-control，清零后继续下一个 VMA，不泄露给调用者。 */
				/*
				 * positive return values are purely for
				 * controlling the pagewalk, so should never
				 * be passed to the callers.
				 */
				err = 0;
				continue;
			}
			if (err < 0)
				break;
			err = __walk_page_range(start, next, &walk);
		}
		if (err)
			break;
	} while (start = next, start < end);
	/* 成功时所有 VMA/间隙已覆盖；错误立即保留第一个 errno。 */
	return err;
}

/*
 * Determine if the walk operations specified are permitted to be used for a
 * page table walk.
 *
 * This check is performed on all functions which are parameterised by walk
 * operations and exposed in include/linux/pagewalk.h.
 *
 * Internal memory management code can use *_unsafe() functions to be able to
 * use all page walking operations.
 */
/* check_ops_safe() - 拒绝导出 walker 使用 install_pte 的写页表能力。 */
static bool check_ops_safe(const struct mm_walk_ops *ops)
{
	/*
	 * The installation of PTEs is solely under the control of memory
	 * management logic and subject to many subtle locking, security and
	 * cache considerations so we cannot permit other users to do so, and
	 * certainly not for exported symbols.
	 */
	if (ops->install_pte)
		/* 写 PTE 需 MM 内部锁/安全协议，外部 callback 无法满足这些前置条件。 */
		return false;

	return true;
}

/**
 * walk_page_range - walk page table with caller specific callbacks
 * @mm:		mm_struct representing the target process of page table walk
 * @start:	start address of the virtual address range
 * @end:	end address of the virtual address range
 * @ops:	operation to call during the walk
 * @private:	private data for callbacks' usage
 *
 * Recursively walk the page table tree of the process represented by @mm
 * within the virtual address range [@start, @end). During walking, we can do
 * some caller-specific works for each entry, by setting up pmd_entry(),
 * pte_entry(), and/or hugetlb_entry(). If you don't set up for some of these
 * callbacks, the associated entries/pages are just ignored.
 * The return values of these callbacks are commonly defined like below:
 *
 *  - 0  : succeeded to handle the current entry, and if you don't reach the
 *         end address yet, continue to walk.
 *  - >0 : succeeded to handle the current entry, and return to the caller
 *         with caller specific value.
 *  - <0 : failed to handle the current entry, and return to the caller
 *         with error code.
 *
 * Before starting to walk page table, some callers want to check whether
 * they really want to walk over the current vma, typically by checking
 * its vm_flags. walk_page_test() and @ops->test_walk() are used for this
 * purpose.
 *
 * If operations need to be staged before and committed after a vma is walked,
 * there are two callbacks, pre_vma() and post_vma(). Note that post_vma(),
 * since it is intended to handle commit-type operations, can't return any
 * errors.
 *
 * struct mm_walk keeps current values of some common data like vma and pmd,
 * which are useful for the access from callbacks. If you want to pass some
 * caller-specific data to callbacks, @private should be helpful.
 *
 * Locking:
 *   Callers of walk_page_range() and walk_page_vma() should hold @mm->mmap_lock,
 *   because these function traverse vma list and/or access to vma's data.
 */
/*
 * 业务背景：这是受限公开范围 walker；入参：mm 已持 mmap_lock，ops/private 为借用。
 * 出参/返回：0、回调正值或 errno。注意事项：先拒绝 install_pte，再进入 unsafe
 * 实现，避免普通驱动/调试代码借公共 API 改变进程页表。
 */
int walk_page_range(struct mm_struct *mm, unsigned long start,
		unsigned long end, const struct mm_walk_ops *ops,
		void *private)
{
	if (!check_ops_safe(ops))
		/* debug/VMA 包装和通用 walker 共用此检查，防止任一公共入口遗漏。 */
		/* 安全拒绝发生在建立 walk 之前，不会向不可信 ops 暴露页表状态。 */
		return -EINVAL;

	return walk_page_range_mm_unsafe(mm, start, end, ops, private);
}

/**
 * walk_kernel_page_table_range - walk a range of kernel pagetables.
 * @start:	start address of the virtual address range
 * @end:	end address of the virtual address range
 * @ops:	operation to call during the walk
 * @pgd:	pgd to walk if different from mm->pgd
 * @private:	private data for callbacks' usage
 *
 * Similar to walk_page_range() but can walk any page tables even if they are
 * not backed by VMAs. Because 'unusual' entries may be walked this function
 * will also not lock the PTEs for the pte_entry() callback. This is useful for
 * walking kernel pages tables or page tables for firmware.
 *
 * Note: Be careful to walk the kernel pages tables, the caller may be need to
 * take other effective approaches (mmap lock may be insufficient) to prevent
 * the intermediate kernel page tables belonging to the specified address range
 * from being freed (e.g. memory hot-remove).
 */
/*
 * 业务背景：供内核/firmware 页表观察者在 init_mm 锁保护下遍历无 VMA 范围。
 * 入参：start/end、ops、可选替代 pgd、private 均借用。出参：0 或回调 errno。
 * 注意事项：mmap_lock 对普通内核表足够，但 memory hot-remove 例外，调用者还
 * 必须稳定中间表；PTE callback 不自动持 PTL，不能假设条目不会被其他路径修改。
 */
int walk_kernel_page_table_range(unsigned long start, unsigned long end,
		const struct mm_walk_ops *ops, pgd_t *pgd, void *private)
{
	/*
	 * Kernel intermediate page tables are usually not freed, so the mmap
	 * read lock is sufficient. But there are some exceptions.
	 * E.g. memory hot-remove. In which case, the mmap lock is insufficient
	 * to prevent the intermediate kernel pages tables belonging to the
	 * specified address range from being freed. The caller should take
	 * other actions to prevent this race.
	 */
	mmap_assert_locked(&init_mm);
	/* init_mm read lock 只覆盖普通情形；热拔需要调用者另行阻止表页释放。 */

	return walk_kernel_page_table_range_lockless(start, end, ops, pgd,
						     private);
}

/*
 * Use this function to walk the kernel page tables locklessly. It should be
 * guaranteed that the caller has exclusive access over the range they are
 * operating on - that there should be no concurrent access, for example,
 * changing permissions for vmalloc objects.
 */
/*
 * 业务背景：在调用者已经独占目标区间时避免取得 mmap_lock 的内核 walker。
 * 入参：范围、ops、pgd/private 为借用。出参：0 或错误。注意事项：exclusive
 * access 是外部前置条件；例如 vmalloc 权限变更并发会使无锁下降访问已释放表。
 */
int walk_kernel_page_table_range_lockless(unsigned long start, unsigned long end,
		const struct mm_walk_ops *ops, pgd_t *pgd, void *private)
{
	/* debug 模式显式 no_vma，因而 walk 不能从 VMA 获得范围或页表生命周期保证。 */
	struct mm_walk walk = {
		.ops		= ops,
		.mm		= &init_mm,
		.pgd		= pgd,
		.private	= private,
		.no_vma		= true
	};
	/* lockless walker 明确标记 no_vma，leaf helper 由此避免 user PTE 验证路径。 */

	if (start >= end)
		/* lockless 接口也拒绝空范围；exclusive access 仅是生命周期前提，不修正参数。 */
		return -EINVAL;
	if (!check_ops_safe(ops))
		return -EINVAL;

	return walk_pgd_range(start, end, &walk);
}

/**
 * walk_page_range_debug - walk a range of pagetables not backed by a vma
 * @mm:		mm_struct representing the target process of page table walk
 * @start:	start address of the virtual address range
 * @end:	end address of the virtual address range
 * @ops:	operation to call during the walk
 * @pgd:	pgd to walk if different from mm->pgd
 * @private:	private data for callbacks' usage
 *
 * Similar to walk_page_range() but can walk any page tables even if they are
 * not backed by VMAs. Because 'unusual' entries may be walked this function
 * will also not lock the PTEs for the pte_entry() callback.
 *
 * This is for debugging purposes ONLY.
 */
/*
 * 业务背景：调试工具可遍历没有 VMA 的指定 mm 页表。入参：mm/range/ops/pgd/private。
 * 出参：0 或错误；init_mm 转交内核 walker。注意事项：非 init_mm 必持 mmap 写锁，
 * 因 read lock 在 munmap 拆 VMA 后无法稳定裸页表；同样拒绝 install_pte。
 */
int walk_page_range_debug(struct mm_struct *mm, unsigned long start,
			  unsigned long end, const struct mm_walk_ops *ops,
			  pgd_t *pgd, void *private)
{
	/* 此 wrapper 预先绑定唯一 VMA，下面的边界检查是其锁覆盖范围的前提。 */
	struct mm_walk walk = {
		.ops		= ops,
		.mm		= mm,
		.pgd		= pgd,
		.private	= private,
		.no_vma		= true
	};

	/* For convenience, we allow traversal of kernel mappings. */
	if (mm == &init_mm)
		/* init_mm 没有普通 VMA 边界，转交专用内核 walker 保持锁约定一致。 */
		return walk_kernel_page_table_range(start, end, ops,
						    pgd, private);
	if (start >= end || !walk.mm)
		/* VMA unsafe 路径还要求 start/end 落在同一个 VMA，不能隐式跨边界。 */
		/* debug walker 对用户 mm 需要有效范围与 mm，随后断言 mmap write lock。 */
		return -EINVAL;
	if (!check_ops_safe(ops))
		return -EINVAL;

	/*
	 * The mmap lock protects the page walker from changes to the page
	 * tables during the walk.  However a read lock is insufficient to
	 * protect those areas which don't have a VMA as munmap() detaches
	 * the VMAs before downgrading to a read lock and actually tearing
	 * down PTEs/page tables. In which case, the mmap write lock should
	 * be held.
	 */
	mmap_assert_write_locked(mm);

	return walk_pgd_range(start, end, &walk);
}

/*
 * walk_page_range_vma_unsafe() - 在单一 VMA 内执行允许 install_pte 的内部遍历。
 * 入参：vma 已稳定，范围必须包含于 vma，ops/private 借用。出参：0 或 errno。
 * 注意事项：调用者已持 ops 指定的 mm/VMA 锁；越界拒绝避免错误下降到相邻 VMA。
 */
int walk_page_range_vma_unsafe(struct vm_area_struct *vma, unsigned long start,
		unsigned long end, const struct mm_walk_ops *ops, void *private)
{
	/* mapping walker 不预填 mm/vma；它们随 interval tree 的当前节点逐轮切换。 */
	struct mm_walk walk = {
		.ops		= ops,
		.mm		= vma->vm_mm,
		.vma		= vma,
		.private	= private,
	};

	if (start >= end || !walk.mm)
		return -EINVAL;
	if (start < vma->vm_start || end > vma->vm_end)
		/* 范围越界返回而不访问相邻 VMA，避免调用者以单 VMA 锁遍历多个映射。 */
		return -EINVAL;

	process_mm_walk_lock(walk.mm, ops->walk_lock);
	process_vma_walk_lock(vma, ops->walk_lock);
	return __walk_page_range(start, end, &walk);
}

/* 公开 VMA wrapper：先验证不含 install_pte，再转交内部 VMA walker。 */
int walk_page_range_vma(struct vm_area_struct *vma, unsigned long start,
			unsigned long end, const struct mm_walk_ops *ops,
			void *private)
{
	if (!check_ops_safe(ops))
		return -EINVAL;

	return walk_page_range_vma_unsafe(vma, start, end, ops, private);
}

/*
 * walk_page_vma() - 遍历整个 VMA 的受限公开接口。
 * 入参：vma、ops/private 借用。出参：0 或 errno。注意事项：必须先验证 mm 与
 * 操作表，并按 walk_lock 检查 mmap/VMA 锁；不提供页表安装能力。
 */
int walk_page_vma(struct vm_area_struct *vma, const struct mm_walk_ops *ops,
		void *private)
{
	struct mm_walk walk = {
		.ops		= ops,
		.mm		= vma->vm_mm,
		.vma		= vma,
		.private	= private,
	};

	if (!walk.mm)
		/* 全 VMA wrapper 从 vma->vm_mm 取上下文，空 mm 表明 VMA 尚未可遍历。 */
		return -EINVAL;
	if (!check_ops_safe(ops))
		return -EINVAL;

	process_mm_walk_lock(walk.mm, ops->walk_lock);
	process_vma_walk_lock(vma, ops->walk_lock);
	return __walk_page_range(vma->vm_start, vma->vm_end, &walk);
}

/**
 * walk_page_mapping - walk all memory areas mapped into a struct address_space.
 * @mapping: Pointer to the struct address_space
 * @first_index: First page offset in the address_space
 * @nr: Number of incremental page offsets to cover
 * @ops:	operation to call during the walk
 * @private:	private data for callbacks' usage
 *
 * This function walks all memory areas mapped into a struct address_space.
 * The walk is limited to only the given page-size index range, but if
 * the index boundaries cross a huge page-table entry, that entry will be
 * included.
 *
 * Also see walk_page_range() for additional information.
 *
 * Locking:
 *   This function can't require that the struct mm_struct::mmap_lock is held,
 *   since @mapping may be mapped by multiple processes. Instead
 *   @mapping->i_mmap_rwsem must be held. This might have implications in the
 *   callbacks, and it's up tho the caller to ensure that the
 *   struct mm_struct::mmap_lock is not needed.
 *
 *   Also this means that a caller can't rely on the struct
 *   vm_area_struct::vm_flags to be constant across a call,
 *   except for immutable flags. Callers requiring this shouldn't use
 *   this function.
 *
 * Return: 0 on success, negative error code on failure, positive number on
 * caller defined premature termination.
 */
/*
 * 业务背景：按文件页索引遍历所有映射该 address_space 的 VMA 的交集。
 * 入参：mapping 的 i_mmap_rwsem 已持有；first_index/nr 是页索引范围；ops/private 借用。
 * 出参：0、回调正终止值或 errno。注意事项：不能假设各 VMA 的 vm_flags 稳定，
 * 因为没有逐个 mmap_lock；先裁剪索引再转换地址以覆盖跨 huge entry 的区间。
 */
int walk_page_mapping(struct address_space *mapping, pgoff_t first_index,
		      pgoff_t nr, const struct mm_walk_ops *ops,
		      void *private)
{
	/* mapping walker 不预填 mm/vma；它们随 interval tree 的当前节点逐轮切换。 */
	struct mm_walk walk = {
		.ops		= ops,
		.private	= private,
	};
	struct vm_area_struct *vma;
	pgoff_t vba, vea, cba, cea;
	unsigned long start_addr, end_addr;
	int err = 0;
	/* 索引端点和虚拟地址端点分离保存，避免裁剪后用错 page-index 与 byte-address 单位。 */

	if (!check_ops_safe(ops))
		return -EINVAL;

	lockdep_assert_held(&mapping->i_mmap_rwsem);
	/* i_mmap 区间树稳定映射集合，但不冻结单个 VMA 可变 flags，回调须自行限制。 */
	vma_interval_tree_foreach(vma, &mapping->i_mmap, first_index,
				  first_index + nr - 1) {
		/* Clip to the vma */
		/* 先在 page-index 域取交集，随后按 vm_pgoff 换算成当前 VMA 虚拟地址。 */
		vba = vma->vm_pgoff;
		vea = vba + vma_pages(vma);
		cba = first_index;
		cba = max(cba, vba);
		cea = first_index + nr;
		cea = min(cea, vea);

		start_addr = ((cba - vba) << PAGE_SHIFT) + vma->vm_start;
		end_addr = ((cea - vba) << PAGE_SHIFT) + vma->vm_start;
		if (start_addr >= end_addr)
			/* huge entry/索引裁剪后可能为空，跳过而不调用回调。 */
			continue;

		walk.vma = vma;
		/* 该 VMA 的 mm 可被回调读取；mapping walk 不承诺 mmap_lock，因此不保存 flags。 */
		walk.mm = vma->vm_mm;

		err = walk_page_test(vma->vm_start, vma->vm_end, &walk);
		/* test 成功后才按裁剪后的 start_addr/end_addr 实际下降。 */
		if (err > 0) {
			/* mapping walker 的正值约定是提前正常终止，转换为 0 返回给调用者。 */
			err = 0;
			break;
		} else if (err < 0)
			break;

		err = __walk_page_range(start_addr, end_addr, &walk);
		/* 下层 errno 停止枚举，防止在调用者状态失败后继续访问其他 mm。 */
		if (err)
			break;
	}

	return err;
}

/**
 * folio_walk_start - walk the page tables to a folio
 * @fw: filled with information on success.
 * @vma: the VMA.
 * @addr: the virtual address to use for the page table walk.
 * @flags: flags modifying which folios to walk to.
 *
 * Walk the page tables using @addr in a given @vma to a mapped folio and
 * return the folio, making sure that the page table entry referenced by
 * @addr cannot change until folio_walk_end() was called.
 *
 * As default, this function returns only folios that are not special (e.g., not
 * the zeropage) and never returns folios that are supposed to be ignored by the
 * VM as documented by vm_normal_page(). If requested, zeropages will be
 * returned as well.
 *
 * If this function returns NULL it might either indicate "there is nothing" or
 * "there is nothing suitable".
 *
 * On success, @fw is filled and the function returns the folio while the PTL
 * is still held and folio_walk_end() must be called to clean up,
 * releasing any held locks. The returned folio must *not* be used after the
 * call to folio_walk_end(), unless a short-term folio reference is taken before
 * that call.
 *
 * @fw->page will correspond to the page that is effectively referenced by
 * @addr. However, for shared zeropages @fw->page is set to NULL. Note that
 * large folios might be mapped by multiple page table entries, and this
 * function will always only lookup a single entry as specified by @addr, which
 * might or might not cover more than a single page of the returned folio.
 *
 * This function must *not* be used as a naive replacement for
 * get_user_pages() / pin_user_pages(), especially not to perform DMA or
 * to carelessly modify page content. This function may *only* be used to grab
 * short-term folio references, never to grab long-term folio references.
 *
 * Using the page table entry pointers in @fw for reading or modifying the
 * entry should be avoided where possible: however, there might be valid
 * use cases.
 *
 * WARNING: Modifying page table entries in hugetlb VMAs requires a lot of care.
 * For example, PMD page table sharing might require prior unsharing. Also,
 * logical hugetlb entries might span multiple physical page table entries,
 * which *must* be modified in a single operation (set_huge_pte_at(),
 * huge_ptep_set_*, ...). Note that the page table entry stored in @fw might
 * not correspond to the first physical entry of a logical hugetlb entry.
 *
 * The mmap lock must be held in read mode.
 *
 * Return: folio pointer on success, otherwise NULL.
 */
/*
 * 业务背景：短期检查某虚拟地址当前映射的正常 folio，同时把对应 PTL 留给调用者。
 * 入参：fw 是成功时填充的输出状态；vma 已持 mmap read lock；addr 在 VMA 内；
 * flags 可请求零页。出参/返回：成功返回借用 folio 且 PTL 仍持有，失败 NULL。
 * 注意事项：必须调用 folio_walk_end() 释放 PTL/结束 pgtable walk；返回 folio
 * 不是长期引用，DMA/长期 pin 禁止使用此接口，若要跨 end 使用必须先 get。
 */
struct folio *folio_walk_start(struct folio_walk *fw,
		struct vm_area_struct *vma, unsigned long addr,
		folio_walk_flags_t flags)
{
	/* 这些局部指针先保存 lockless 快照，只有相应 PTL 下复读后才写入 fw。 */
	unsigned long entry_size;
	bool zeropage = false;
	struct page *page;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	pgd_t *pgdp;
	p4d_t *p4dp;
	/* fw 在成功前逐层写入当前 entry 指针/值；失败不得被调用者当作有效输出。 */

	/* 阶段 1：锁/pgtable walk begin 稳定 VMA，随后从 PGD 逐层进行无锁初查。 */
	mmap_assert_locked(vma->vm_mm);
	vma_pgtable_walk_begin(vma);

	if (WARN_ON_ONCE(addr < vma->vm_start || addr >= vma->vm_end))
		/* 地址越界时尚未取得 PTL，只需结束 pgtable walk。 */
		goto not_found;

	pgdp = pgd_offset(vma->vm_mm, addr);
	/* PGD/P4D/PUD 初查只定位；遇到 leaf 后才获得该层 PTL 并重读。 */
	if (pgd_none_or_clear_bad(pgdp))
		goto not_found;

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none_or_clear_bad(p4dp))
		goto not_found;

	pudp = pud_offset(p4dp, addr);
	pud = pudp_get(pudp);
	if (pud_none(pud))
		/* 缺项不是错误，表示该地址暂无适合的 folio。 */
		goto not_found;
	/* 阶段 2：若 PUD 是 huge/special leaf，取得 PUD PTL 后重读并记录 fw。 */
	if (IS_ENABLED(CONFIG_PGTABLE_HAS_HUGE_LEAVES) &&
	    (!pud_present(pud) || pud_leaf(pud))) {
		ptl = pud_lock(vma->vm_mm, pudp);
		/* 加锁后重读 pud，初查与锁获取之间可能已有 fault/split 改变条目。 */
		pud = pudp_get(pudp);

		entry_size = PUD_SIZE;
		fw->level = FW_LEVEL_PUD;
		fw->pudp = pudp;
		fw->pud = pud;

		/* PUD 锁下的三分支分别处理撤销、下级表和 leaf；失败前必须归还 PTL。 */
		if (pud_none(pud)) {
			/* 条目消失后释放 PTL 并统一失败，不使用锁前快照。 */
			spin_unlock(ptl);
			goto not_found;
		} else if (pud_present(pud) && !pud_leaf(pud)) {
			/* 非 leaf 的 PUD 只携带下级表，释放本层锁后由 pmd_table 继续。 */
			spin_unlock(ptl);
			goto pmd_table;
		} else if (pud_present(pud)) {
			/* leaf PUD 仅接受 normal 页；special/device 项无法转换为可返回 folio。 */
			page = vm_normal_page_pud(vma, addr, pud);
			if (page)
				goto found;
		}
		/* present leaf 未归属普通页也不能保留锁或 fw 状态，归入未找到结果。 */
		spin_unlock(ptl);
		goto not_found;
	}

/* PUD 不是 leaf 时继续下降；标签路径都先释放上一层 PTL，避免锁泄漏。 */
pmd_table:
	VM_WARN_ON_ONCE(!pud_present(pud) || pud_leaf(pud));
	pmdp = pmd_offset(pudp, addr);
	pmd = pmdp_get_lockless(pmdp);
	/* PMD 初读同样只用于选择路径；huge leaf 判定前必须在 PTL 下复核。 */
	if (pmd_none(pmd))
		goto not_found;
	/* 阶段 3：PMD leaf 同样在锁下验证、识别 normal/huge-zero 页或转入 PTE。 */
	if (IS_ENABLED(CONFIG_PGTABLE_HAS_HUGE_LEAVES) &&
	    (!pmd_present(pmd) || pmd_leaf(pmd))) {
		ptl = pmd_lock(vma->vm_mm, pmdp);
		/* PMD PTL 取得后更新 fw 的 level/指针/值，成功出口把锁交给调用者。 */
		pmd = pmdp_get(pmdp);

		entry_size = PMD_SIZE;
		fw->level = FW_LEVEL_PMD;
		fw->pmdp = pmdp;
		fw->pmd = pmd;

		if (pmd_none(pmd)) {
			/* PMD 被并发撤销时不保留 fw 指针；释放刚取得的 PTL 后统一失败。 */
			spin_unlock(ptl);
			goto not_found;
		} else if (pmd_present(pmd) && !pmd_leaf(pmd)) {
			/* 锁下发现已拆为下级表，丢弃当前 PTL 并按 PTE 路径重新取得正确锁。 */
			spin_unlock(ptl);
			goto pte_table;
		} else if (pmd_present(pmd)) {
			/* normal-page 转换排除 device/special 映射；这些映射不能伪装成普通 folio。 */
			page = vm_normal_page_pmd(vma, addr, pmd);
			if (page) {
				goto found;
			} else if ((flags & FW_ZEROPAGE) &&
				    is_huge_zero_pmd(pmd)) {
				page = pfn_to_page(pmd_pfn(pmd));
				zeropage = true;
				goto found;
			}
		}
		/* 未命中 normal/zero huge folio 时，此 PMD 的锁窗口在此结束。 */
		spin_unlock(ptl);
		goto not_found;
	}

/* 最终 PTE 路径以 pte_offset_map_lock 取得 PTL；其映射在成功时交给 fw。 */
pte_table:
	VM_WARN_ON_ONCE(!pmd_present(pmd) || pmd_leaf(pmd));
	ptep = pte_offset_map_lock(vma->vm_mm, pmdp, addr, &ptl);
	/* PTE map 失败通常代表表已回收；禁止使用 lockless PMD 快照继续解引用。 */
	if (!ptep)
		goto not_found;
	pte = ptep_get(ptep);

	entry_size = PAGE_SIZE;
	/* fw 记录 entry 层级及原始条目，folio_walk_end 据此选择正确的解锁/解除映射。 */
	fw->level = FW_LEVEL_PTE;
	fw->ptep = ptep;
	fw->pte = pte;

	/* PTE 成功路径仅接受可归属普通 folio 的映射，零页是经 flags 允许的例外。 */
	/* present PTE 才可转换 normal page；special PFN 默认拒绝，零页需显式 flag。 */
	if (pte_present(pte)) {
		/* 零页返回时 page 指针仅供 folio 返回，fw->page 必置 NULL 表示无专属页。 */
		page = vm_normal_page(vma, addr, pte);
		if (page)
			/* 普通页命中后进入统一成功出口，由调用者通过 folio_walk_end 归还 PTL。 */
			goto found;
		if ((flags & FW_ZEROPAGE) &&
		    is_zero_pfn(pte_pfn(pte))) {
			page = pfn_to_page(pte_pfn(pte));
			zeropage = true;
			goto found;
		}
	}
	/* 普通页和获准零页都未命中，先解除 PTE 映射及 PTL 再走失败出口。 */
	pte_unmap_unlock(ptep, ptl);
	/* PTE 未产生可返回 folio 时立即释放锁，不把短期保护泄漏给失败调用者。 */
/* 失败出口不遗留 PTE 映射或 VMA walk 保护；每个已锁分支先自行 unlock。 */
not_found:
	/* vma_pgtable_walk_end 与 begin 配对，通知并发页表变更者本次观察窗口结束。 */
	vma_pgtable_walk_end(vma);
	return NULL;
found:
	/* 成功出口故意保留 PTL；page 内偏移而非 folio 首页对应 addr 实际引用的子页。 */
	if (!zeropage)
		/* Note: Offset from the mapped page, not the folio start. */
		fw->page = page + ((addr & (entry_size - 1)) >> PAGE_SHIFT);
	else
		fw->page = NULL;
	fw->ptl = ptl;
	/* 返回 folio 前 ownership 仍属页表/调用者：仅 PTL 抑制条目变化，并未增加 ref。 */
	return page_folio(page);
}
