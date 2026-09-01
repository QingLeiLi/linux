// SPDX-License-Identifier: GPL-2.0
/*
 *	linux/mm/mincore.c
 *
 * Copyright (C) 1994-2006  Linus Torvalds
 */

/*
 * The mincore() system call.
 */
/*
 * 本文件实现 mincore(2) 的“虚拟地址范围 -> 每页驻留字节”查询。它只生成
 * 调用瞬间的观察结果，不固定 folio；除被 mlock 等机制锁住的页外，返回用户态前
 * 驻留状态就可能改变。下层回调把普通页表、HugeTLB、swap 和文件页缓存统一成
 * 每个基础页一个字节，系统调用入口负责参数检查、分批和 copy_to_user()。
 */
#include <linux/pagemap.h>
#include <linux/gfp.h>
#include <linux/pagewalk.h>
#include <linux/mman.h>
#include <linux/syscalls.h>
/* 页表 walker 与 mincore UAPI/系统调用声明提供入口分派和逐级回调框架。 */
#include <linux/swap.h>
#include <linux/leafops.h>
#include <linux/shmem_fs.h>
#include <linux/hugetlb.h>
#include <linux/pgtable.h>
/* swap、softleaf、shmem、HugeTLB 与通用页表 helper 覆盖各类叶子项语义。 */

#include <linux/uaccess.h>
#include "swap.h"
#include "internal.h"

/*
 * mincore_hugetlb() - 把一段 HugeTLB 映射展开成基础页粒度的驻留结果
 *
 * 业务背景：walk_page_range() 遇到 VM_HUGETLB VMA 时调用本回调；用户 ABI 仍要求
 * 每个 PAGE_SIZE 返回一个字节，所以一个 huge PTE 的判定要复制到整个 hugepage 范围。
 * 入参：pte 是 walker 找到的 huge PTE 指针，可能为空；hmask 描述 hugepage 掩码，
 * 本实现无需直接使用；addr/end 是左闭右开的虚拟地址范围；walk 是借用的遍历上下文，
 * 其中 mm/vma 在 mmap 读锁下稳定，private 指向当前输出游标。
 * 出参/返回：总是返回 0 让 walker 继续；写入 [addr, end) 对应的 vec 字节，并把
 * walk->private 推进到区间末尾。函数不取得长期对象引用。
 * 注意事项：CONFIG_HUGETLB_PAGE 下用 huge PTE 锁与并发 fault/unmap/迁移串行；锁只
 * 稳定页表项，不保证返回后页面继续驻留。关闭 HugeTLB 时该回调不应可达，BUG() 用于
 * 暴露错误的配置分派；函数不睡眠。
 */
static int mincore_hugetlb(pte_t *pte, unsigned long hmask, unsigned long addr,
			unsigned long end, struct mm_walk *walk)
{
#ifdef CONFIG_HUGETLB_PAGE
	/* present 是整段 hugepage 的快照；vec 是仅在本次回调内借用的输出游标。 */
	unsigned char present;
	unsigned char *vec = walk->private;
	spinlock_t *ptl;

	/*
	 * 先锁住该 hstate/mm 对应的 huge PTE，再读取页表项，避免并发 fault 或
	 * unmap 使“检查”和整段填充针对不同的页表状态。
	 */
	ptl = huge_pte_lock(hstate_vma(walk->vma), walk->mm, pte);

	/*
	 * Hugepages under user process are always in RAM and never
	 * swapped out, but theoretically it needs to be checked.
	 */
	/*
	 * 用户进程中的 HugeTLB 页通常常驻 RAM、不会换出，但仍必须检查空项和
	 * marker：前者表示尚未建立映射，后者只是 fault 元数据，都不能报告驻留。
	 */
	if (!pte) {
		present = 0;
	} else {
		/* ptep 是锁保护下的值快照，不持有它所映射 folio 的引用。 */
		const pte_t ptep = huge_ptep_get(walk->mm, addr, pte);

		if (huge_pte_none(ptep) || pte_is_marker(ptep))
			present = 0;
		else
			present = 1;
	}

	/*
	 * ABI 使用基础页粒度，因此把同一 huge PTE 的结果复制给区间内每个页，
	 * 再发布新游标；完成填充后才解锁，保证本段来自一次一致的 PTE 观察。
	 */
	for (; addr != end; vec++, addr += PAGE_SIZE)
		*vec = present;
	walk->private = vec;
	spin_unlock(ptl);
#else
	/* walker 不应在禁用 HugeTLB 的构建中选择 hugetlb_entry。 */
	BUG();
#endif
	return 0;
}

/*
 * mincore_swap() - 判断 swap/softleaf 条目是否已有可立即使用的内存副本
 *
 * 业务背景：普通非 present PTE 与 tmpfs XArray exceptional entry 最终都汇入这里；
 * mincore 的“in core”语义要求 swap cache 中存在且 uptodate，而不是仅有磁盘槽位。
 * 入参：entry 是纯输入的 softleaf/swap 编码，调用者保证其来自受保护的 PTE 或有效
 * shmem XArray 项；shmem 区分无 swap 类型的特殊条目在页表和 tmpfs 中的不同含义。
 * 出参/返回：返回 1 表示无需 I/O 即有最新内存内容，0 表示不驻留或条目无效；若查到
 * folio，本函数取得的临时引用会在返回前释放，不向调用者转移 ownership。
 * 注意事项：tmpfs 的 lockless mapping 查询必须用 get_swap_device() 阻止 swapoff；
 * 页表路径已由 PTL 稳定条目且 swap 设备生命周期稳定，避免额外引用。函数不睡眠，
 * 返回只是一刻的快照。
 */
static unsigned char mincore_swap(swp_entry_t entry, bool shmem)
{
	/* si 仅在 shmem 路径临时持有；folio 若非 NULL 则带一份待释放引用。 */
	struct swap_info_struct *si;
	struct folio *folio = NULL;
	unsigned char present = 0;

	/*
	 * Shmem mapping may contain swapin error entries, which are
	 * absent. Page table may contain migration or hwpoison
	 * entries which are always uptodate.
	 */
	/*
	 * tmpfs 可能保存 swapin-error exceptional entry，它代表内容不可用；页表中的
	 * migration/hwpoison 等非 swap softleaf 则已有内存页或确定的内存状态，按驻留处理。
	 */
	if (!softleaf_is_swap(entry))
		return !shmem;

	/* 真正的 swap entry 在无 CONFIG_SWAP 构建中不应出现，保守返回“不驻留”。 */
	if (!IS_ENABLED(CONFIG_SWAP)) {
		WARN_ON(1);
		return 0;
	}

	/*
	 * Shmem mapping lookup is lockless, so we need to grab the swap
	 * device. mincore page table walk locks the PTL, and the swap
	 * device is stable, avoid touching the si for better performance.
	 */
	/*
	 * tmpfs 的 XArray 查找不持 PTL，先增加 swap 设备 users 引用，防止 swapoff
	 * 在 swap_cache_get_folio() 使用 entry 时回收并复用设备；页表路径已有同步保证。
	 */
	if (shmem) {
		si = get_swap_device(entry);
		if (!si)
			return 0;
	}
	/* 命中时获得未锁 folio 引用；这里只读取 uptodate 标志，不要求锁定内容。 */
	folio = swap_cache_get_folio(entry);
	if (shmem)
		put_swap_device(si);
	/* The swap cache space contains either folio, shadow or NULL */
	/*
	 * swap cache 槽可能是 folio、仅用于回收历史的 XArray value（shadow）或空；
	 * 只有真实且 uptodate 的 folio 才能在不发起 swapin I/O 的情况下访问。
	 */
	if (folio && !xa_is_value(folio)) {
		present = folio_test_uptodate(folio);
		folio_put(folio);
	}

	return present;
}

/*
 * Later we can get more picky about what "in core" means precisely.
 * For now, simply check to see if the page is in the page cache,
 * and is up to date; i.e. that no page-in operation would be required
 * at this time if an application were to map and access this page.
 */
/*
 * 上述“in core”当前定义为页缓存中存在最新 folio，因而应用映射并访问时无需 page-in；
 * 它不是“曾经缓存过”或“有后备存储”的判断，未来实现仍可能收紧此定义。
 */
/*
 * mincore_page() - 查询文件 mapping 中一个页索引的即时驻留状态
 *
 * 业务背景：页表项为空/为 marker 时，匿名映射必然不驻留，但文件映射仍可能在 page
 * cache 中；尤其 tmpfs 换出页不会把 swap entry 安装到每个映射 PTE，所以必须查 mapping。
 * 入参：mapping 是 VMA 文件持有的 address_space 借用指针，在 mmap 读锁和文件生命周期内
 * 有效；index 是文件内 PAGE_SIZE 单位页索引。
 * 出参/返回：返回 1 表示找到 uptodate folio（或 tmpfs swap cache 命中），否则返回 0；
 * filemap_get_entry() 取得的真实 folio 引用由本函数释放，exceptional value 无引用可释放。
 * 注意事项：XArray 查找与回收并发，因此结果可能立刻过时；本函数不锁 folio、不触发 I/O，
 * 也不把任何对象 ownership 交给调用者。
 */
static unsigned char mincore_page(struct address_space *mapping, pgoff_t index)
{
	/* present 初始为“不驻留”；folio 也可能承载 XArray value 编码而非真实指针。 */
	unsigned char present = 0;
	struct folio *folio;

	/*
	 * When tmpfs swaps out a page from a file, any process mapping that
	 * file will not get a swp_entry_t in its pte, but rather it is like
	 * any other file mapping (ie. marked !present and faulted in with
	 * tmpfs's .fault). So swapped out tmpfs mappings are tested here.
	 */
	/*
	 * tmpfs 文件换出后，各进程 PTE 与普通文件洞一样是 !present，真正 swap entry
	 * 留在 shmem mapping 中并由 .fault 处理；所以仅查看调用进程 PTE 会漏报缓存命中。
	 */
	folio = filemap_get_entry(mapping, index);
	if (folio) {
		/* XArray value 是 shadow 或 swap 编码；只有 shmem 的 swap 编码值得继续查询。 */
		if (xa_is_value(folio)) {
			if (shmem_mapping(mapping))
				return mincore_swap(radix_to_swp_entry(folio),
						    true);
			else
				return 0;
		}
		/* 真实 folio 命中带引用；取 uptodate 快照后立即配对 folio_put()。 */
		present = folio_test_uptodate(folio);
		folio_put(folio);
	}

	return present;
}

/*
 * __mincore_unmapped_range() - 为没有 present PTE 的区间生成驻留字节
 *
 * 业务背景：pte_hole 回调和 marker/none PTE 都调用这个核心 helper。页表“未映射”
 * 不等于文件内容“不在内存”：文件 VMA 还要按 pgoff 查询 page cache；匿名 VMA 才能
 * 直接填 0。
 * 入参：addr/end 是同一 VMA 内页对齐的左闭右开虚拟区间；vma 是 mmap 读锁保护下的
 * 借用对象；vec 是至少可写 nr=(end-addr)/PAGE_SIZE 字节的内核缓冲区。
 * 出参/返回：依次写满 vec[0..nr)，并返回写入的字节/页数 nr；不推进外层 walk 游标，
 * 不转移 VMA、file、mapping 或 folio ownership。
 * 注意事项：文件查找可能看到与页表观察不同瞬间的 page-cache 状态，这是 mincore ABI
 * 允许的陈旧快照；函数不持 PTE 锁，可调用 lockless filemap 查询，但不发起 I/O。
 */
static int __mincore_unmapped_range(unsigned long addr, unsigned long end,
				struct vm_area_struct *vma, unsigned char *vec)
{
	/* nr 是本区间的基础页数；i 是 vec 与文件 pgoff 同步前进的页下标。 */
	unsigned long nr = (end - addr) >> PAGE_SHIFT;
	int i;

	if (vma->vm_file) {
		/* pgoff 把首个虚拟地址换算为文件 mapping 的 PAGE_SIZE 索引。 */
		pgoff_t pgoff;

		pgoff = linear_page_index(vma, addr);
		/* 文件 PTE 即使为空，uptodate folio 或 tmpfs swap cache 仍可报告驻留。 */
		for (i = 0; i < nr; i++, pgoff++)
			vec[i] = mincore_page(vma->vm_file->f_mapping, pgoff);
	} else {
		/* 匿名区间没有文件 page cache 可补查，空 PTE 就表示当前无驻留页。 */
		for (i = 0; i < nr; i++)
			vec[i] = 0;
	}
	return nr;
}

/*
 * mincore_unmapped_range() - 适配 mm_walk 的 pte_hole 回调签名
 *
 * 业务背景：pagewalk 在缺失的页表层级调用 pte_hole；本 wrapper 复用
 * __mincore_unmapped_range() 并维护 walk->private 输出游标。
 * 入参：addr/end 是 hole 的页对齐区间；depth 是页表缺洞层级但本判定无需区分，故标记
 * __always_unused；walk 借用当前 mm/VMA，并令 private 指向下一个待写字节。
 * 出参/返回：返回 0 继续遍历；写入对应结果并把 private 前移实际页数，无对象 ownership
 * 变化，也没有独立失败码。
 * 注意事项：调用者持 mmap 读锁稳定 VMA；该回调可执行 page-cache 查询但不发起阻塞 I/O。
 */
static int mincore_unmapped_range(unsigned long addr, unsigned long end,
				   __always_unused int depth,
				   struct mm_walk *walk)
{
	/* helper 返回写入量，直接累加到 unsigned-char 输出游标。 */
	walk->private += __mincore_unmapped_range(addr, end,
						  walk->vma, walk->private);
	return 0;
}

/*
 * mincore_pte_range() - 扫描一个 PMD 覆盖范围内的普通/透明大页 PTE 驻留状态
 *
 * 业务背景：mincore_walk_ops 的 pmd_entry 在 pagewalk 下降到 PTE 层时调用本函数；
 * 它把 THP、present PTE、hole/marker 和 swap softleaf 统一写入逐基础页 vec。
 * 入参：pmd 是当前 PMD 借用指针；addr/end 是该回调负责的页对齐区间；walk 借用
 * mm/VMA，并以 private 传入可写输出游标。调用者持 mmap 读锁。
 * 出参/返回：正常返回 0 并把 private 推进 nr 字节；若 PTE 映射窗口暂时建立失败，设置
 * ACTION_AGAIN 要求 walker 重试当前层级、保持输出游标不变后返回 0。无直接 errno。
 * 注意事项：THP 路径持 PMD 锁，普通路径持 PTE 锁，与 split/fault/unmap 串行；锁内只做
 * 不睡眠的状态检查。锁保证本批页表读取稳定，却不固定物理 folio，结果仍可能过时。
 */
static int mincore_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			struct mm_walk *walk)
{
	/*
	 * ptl 对应 PMD 或 PTE 锁；vma/vec 是借用指针；nr 是输出总页数；
	 * step 允许体系结构一次确认连续 PTE 批，i 用于填充该批。
	 */
	spinlock_t *ptl;
	struct vm_area_struct *vma = walk->vma;
	pte_t *ptep;
	unsigned char *vec = walk->private;
	int nr = (end - addr) >> PAGE_SHIFT;
	int step, i;

	/*
	 * 快速路径：若 PMD 当前是锁定的透明大页，整个回调区间都有 present 映射，
	 * 直接填 1；释放 PMD 锁后统一推进游标。
	 */
	ptl = pmd_trans_huge_lock(pmd, vma);
	if (ptl) {
		memset(vec, 1, nr);
		spin_unlock(ptl);
		goto out;
	}

	/*
	 * 慢速路径：映射并锁住 PTE 页。并发页表拆除可能让映射失败，此时不能消费
	 * vec，ACTION_AGAIN 让 pagewalk 从稳定层级重新判断。
	 */
	ptep = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	if (!ptep) {
		walk->action = ACTION_AGAIN;
		return 0;
	}
	/* 每轮在同一 PTL 下取得 PTE 值快照，并按体系结构 batch hint 推进。 */
	for (; addr != end; ptep += step, addr += step * PAGE_SIZE) {
		pte_t pte = ptep_get(ptep);

		step = 1;
		/* We need to do cache lookup too for markers */
		/* marker 不是驻留映射，但文件 backing 仍可能在 page cache，故与 none 一并补查。 */
		if (pte_none(pte) || pte_is_marker(pte))
			__mincore_unmapped_range(addr, addr + PAGE_SIZE,
						 vma, vec);
		else if (pte_present(pte)) {
			/* batch 是架构保证连续同权限、同驻留性质的 PTE 数，默认退化为 1。 */
			unsigned int batch = pte_batch_hint(ptep, pte);

			if (batch > 1) {
				/* max_nr 防止最后一批越过本回调的 end 与输出缓冲区。 */
				unsigned int max_nr = (end - addr) >> PAGE_SHIFT;

				step = min_t(unsigned int, batch, max_nr);
			}

			for (i = 0; i < step; i++)
				vec[i] = 1;
		} else { /* pte is a swap entry */
			/* 非 present 且非空/marker 的 softleaf 由 swap/migration/hwpoison 语义判定。 */
			const softleaf_t entry = softleaf_from_pte(pte);

			*vec = mincore_swap(entry, false);
		}
		vec += step;
	}
	/* ptep 已越过最后一项，减一传回仍位于映射窗口内的指针并成对解锁/unmap。 */
	pte_unmap_unlock(ptep - 1, ptl);
out:
	/* 两条成功路径都恰好生成 nr 个字节；随后允许长范围查询主动让出 CPU。 */
	walk->private += nr;
	cond_resched();
	return 0;
}

/*
 * can_do_mincore() - 判断是否允许向当前调用者披露 VMA 的真实缓存驻留信息
 *
 * 业务背景：mincore 历史上可被用作共享 page-cache 侧信道；do_mincore() 在真正
 * walk 前调用这里，将匿名/可写文件映射与不可披露的共享文件映射区分开。
 * 入参：vma 是 current->mm 中、由 mmap 读锁保护的借用 VMA；函数不取得 file/inode 引用。
 * 出参/返回：匿名 VMA 返回 true；无 file 的非匿名特殊 VMA 返回 false；文件 VMA 仅当
 * 调用者是 inode owner/具备能力或当前 file 通过 MAY_WRITE 权限检查时返回 true。
 * 注意事项：权限检查可能经过 LSM；调用者允许睡眠并持 mmap 读锁。false 不是系统调用
 * 失败，而是要求上层用全 1 模糊结果，避免泄露其他主体共享文件的 page-cache 状态。
 */
static inline bool can_do_mincore(struct vm_area_struct *vma)
{
	/* 匿名页只属于该 mm 的可见范围，不暴露跨主体共享文件缓存。 */
	if (vma_is_anonymous(vma))
		return true;
	/* 既非匿名又无 vm_file 的特殊映射无法建立安全的文件权限依据。 */
	if (!vma->vm_file)
		return false;
	/*
	 * Reveal pagecache information only for non-anonymous mappings that
	 * correspond to the files the calling process could (if tried) open
	 * for writing; otherwise we'd be including shared non-exclusive
	 * mappings, which opens a side channel.
	 */
	/*
	 * 只向本可拥有/管理 inode，或假如尝试便能以 MAY_WRITE 打开的调用者披露真实
	 * page-cache 信息；否则共享且非独占的映射会形成跨进程缓存侧信道。
	 */
	return file_owner_or_capable(vma->vm_file) ||
	       file_permission(vma->vm_file, MAY_WRITE) == 0;
}

/*
 * mincore_walk_ops 描述 pagewalk 的间接分派：PMD 回调处理普通 PTE/THP，pte_hole
 * 回调查文件缓存或填 0，hugetlb_entry 处理 HugeTLB。PGWALK_RDLOCK 声明 walker
 * 需要读侧页表锁语义；各回调共享并推进 private 中的输出游标，表本身只读且全局常驻。
 */
static const struct mm_walk_ops mincore_walk_ops = {
	.pmd_entry		= mincore_pte_range,
	.pte_hole		= mincore_unmapped_range,
	.hugetlb_entry		= mincore_hugetlb,
	.walk_lock		= PGWALK_RDLOCK,
};

/*
 * Do a chunk of "sys_mincore()". We've already checked
 * all the arguments, we hold the mmap semaphore: we should
 * just return the amount of info we're asked for.
 */
/*
 * 本函数处理 sys_mincore() 的一个连续块；入口参数已经验证且持有 mmap 锁，所以只需
 * 找到起始 VMA、限制到其末尾并返回实际生成的页数。旧称 mmap semaphore 即当前
 * mmap_read_lock；分块允许系统调用跨越多个 VMA 时在外层逐段重建查找状态。
 */
/*
 * do_mincore() - 为起始地址所在 VMA 生成至多 pages 个驻留字节
 *
 * 业务背景：mincore 系统调用每轮持 current->mm 的 mmap 读锁调用本函数；一个调用只
 * 处理一个 VMA 片段，使下一轮能发现洞或下一个 VMA，并保持 walker 的 VMA 前置条件。
 * 入参：addr 是页对齐用户虚拟地址；pages 是请求的最大基础页数；vec 是内核临时页中
 * 至少 pages 字节的输出缓冲区。三者均为纯输入/借用，current->mm 不转移 ownership。
 * 出参/返回：正数为实际写入页数；-ENOMEM 表示 addr 不属于任何 VMA；walker 的负 errno
 * 原样返回。不可披露文件映射返回对应页数并把 vec 填 1，不以错误暴露权限策略。
 * 注意事项：调用者必须持 mmap 读锁，故 VMA 边界与 file 指针稳定；函数可能经权限/LSM
 * 检查睡眠。返回后锁由外层释放，vec 只是一致性有限的快照。
 */
static long do_mincore(unsigned long addr, unsigned long pages, unsigned char *vec)
{
	/* vma 是锁内借用对象；end 截到 VMA/请求较早者；err 只承接 walker 状态。 */
	struct vm_area_struct *vma;
	unsigned long end;
	int err;

	/* 洞必须报告 -ENOMEM，不能跳到后续 VMA，否则 ABI 会掩盖未映射页。 */
	vma = vma_lookup(current->mm, addr);
	if (!vma)
		return -ENOMEM;
	/* pages 已由入口限制，计算当前 VMA 中本轮可以连续处理的右边界。 */
	end = min(vma->vm_end, addr + (pages << PAGE_SHIFT));
	if (!can_do_mincore(vma)) {
		/* 局部 pages 遮蔽形参，只表示 [addr,end) 实际输出长度。 */
		unsigned long pages = DIV_ROUND_UP(end - addr, PAGE_SIZE);

		/* 用“全部驻留”隐藏真实 page-cache 差异，同时保持成功和遍历进度。 */
		memset(vec, 1, pages);
		return pages;
	}
	/* walker 按页表类型写 vec；private 从 vec 起点逐回调推进。 */
	err = walk_page_range(vma->vm_mm, addr, end, &mincore_walk_ops, vec);
	if (err < 0)
		return err;
	/* walker 成功必须覆盖整个 VMA 片段，向外层返回消耗的页/字节数。 */
	return (end - addr) >> PAGE_SHIFT;
}

/*
 * The mincore(2) system call.
 *
 * mincore() returns the memory residency status of the pages in the
 * current process's address space specified by [addr, addr + len).
 * The status is returned in a vector of bytes.  The least significant
 * bit of each byte is 1 if the referenced page is in memory, otherwise
 * it is zero.
 *
 * Because the status of a page can change after mincore() checks it
 * but before it returns to the application, the returned vector may
 * contain stale information.  Only locked pages are guaranteed to
 * remain in memory.
 *
 * return values:
 *  zero    - success
 *  -EFAULT - vec points to an illegal address
 *  -EINVAL - addr is not a multiple of PAGE_SIZE
 *  -ENOMEM - Addresses in the range [addr, addr + len] are
 *		invalid for the address space of this process, or
 *		specify one or more pages which are not currently
 *		mapped
 *  -EAGAIN - A kernel resource was temporarily unavailable.
 */
/*
 * mincore(2) 查询 current 地址空间 [addr, addr+len) 中各基础页的即时驻留状态，
 * 每页写一个字节且最低位 1 表示当前在内存。检查后页面可立即被回收，所以只有锁页
 * 才保证结果返回时仍驻留。成功返回 0；vec 不可写返回 -EFAULT；addr 未页对齐返回
 * -EINVAL；用户范围含无效/未映射地址返回 -ENOMEM；临时页分配失败返回 -EAGAIN。
 */
/*
 * SYSCALL_DEFINE3(mincore) - 验证用户范围并分批导出逐页驻留向量
 *
 * 业务背景：这是用户 ABI 入口；它把任意长度请求切成最多 PAGE_SIZE 个结果字节的批次，
 * 每批短暂持 mmap 读锁调用 do_mincore()，再在锁外 copy_to_user()。
 * 入参：start 是待查询用户虚拟首地址，允许带架构地址 tag 但去 tag 后必须页对齐；len 是
 * 字节长度，尾部不足一页仍计一页；vec 是用户态输出数组，必须可写 ceil(len/PAGE_SIZE)
 * 字节。所有用户指针均为借用，内核不会在返回后保存。
 * 出参/返回：成功写完整个 vec 返回 0；-EINVAL/-ENOMEM/-EFAULT/-EAGAIN 的类别如上，
 * 若跨 VMA 查询中途失败，前面批次可能已写给用户，错误返回不回滚这些字节。
 * 注意事项：函数在进程上下文可睡眠；mmap 锁不跨 copy_to_user()，避免缺页时锁递归，
 * 因而各批可能来自不同时间点。临时页始终在统一出口释放。
 */
SYSCALL_DEFINE3(mincore, unsigned long, start, size_t, len,
		unsigned char __user *, vec)
{
	/* retval 保存本轮页数或最终 errno；pages 是尚未输出页数；tmp 是独占临时页。 */
	long retval;
	unsigned long pages;
	unsigned char *tmp;

	/* 架构 tag 不参与页表地址与对齐判断，先规范化成 current 可用地址。 */
	start = untagged_addr(start);

	/* Check the start address: needs to be page-aligned.. */
	/* 首地址必须按 PAGE_SIZE 对齐，否则 vec 中“每字节对应哪一页”没有合法 ABI 起点。 */
	if (unlikely(start & ~PAGE_MASK))
		return -EINVAL;

	/* ..and we need to be passed a valid user-space range */
	/* 先验证 [start,start+len) 属于用户地址空间；这里尚不要求每一页已有 VMA。 */
	if (!access_ok((void __user *) start, len))
		return -ENOMEM;

	/* This also avoids any overflows on PAGE_ALIGN */
	/*
	 * 用整除加余数计算 ceil(len/PAGE_SIZE)，不直接 PAGE_ALIGN(len)，从而避免
	 * len 接近上限时的向上对齐溢出；len==0 得到 pages==0 并成功返回。
	 */
	pages = len >> PAGE_SHIFT;
	pages += (offset_in_page(len)) != 0;

	/* vec 的可写长度以结果页数计；输入地址范围合法不代表输出指针也合法。 */
	if (!access_ok(vec, pages))
		return -EFAULT;

	/* 一页内核缓冲最多承载 PAGE_SIZE 个结果字节；分配失败映射为 ABI 的 -EAGAIN。 */
	tmp = (void *) __get_free_page(GFP_USER);
	if (!tmp)
		return -EAGAIN;

	/*
	 * 每轮只在页表/VMA 检查期间持 mmap 读锁。do_mincore() 可能在 VMA 末尾
	 * 提前返回正数，外层据此推进并在下一轮重新查找相邻 VMA或发现地址洞。
	 */
	retval = 0;
	while (pages) {
		/*
		 * Do at most PAGE_SIZE entries per iteration, due to
		 * the temporary buffer size.
		 */
		/* 临时页只有 PAGE_SIZE 字节，所以单批至多输出 PAGE_SIZE 个页状态。 */
		mmap_read_lock(current->mm);
		retval = do_mincore(start, min(pages, PAGE_SIZE), tmp);
		mmap_read_unlock(current->mm);

		/* 0 仅可能是无剩余工作，负值是洞或 walker 错误；两者都结束批循环。 */
		if (retval <= 0)
			break;
		/* 锁外复制允许用户缺页；失败保留已完成的早期批次并返回 -EFAULT。 */
		if (copy_to_user(vec, tmp, retval)) {
			retval = -EFAULT;
			break;
		}
		/* 正数 retval 同时是结果字节数和基础页数，三条游标必须同步推进。 */
		pages -= retval;
		vec += retval;
		start += retval << PAGE_SHIFT;
		/* 当前批成功后清零；若这正是最后一批，最终 ABI 返回成功 0。 */
		retval = 0;
	}
	/* 无论成功、地址洞还是用户复制失败，都释放唯一分配的临时页。 */
	free_page((unsigned long) tmp);
	return retval;
}
