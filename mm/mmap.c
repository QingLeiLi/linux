// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/mmap.c
 *
 * Written by obz.
 *
 * Address space accounting code	<alan@lxorguk.ukuu.org.uk>
 */
/*
 * 本文件实现 MMU 系统的用户虚拟地址空间入口与记账：brk/mmap/munmap、空闲地址
 * 选择、栈扩展、进程退出拆映射、特殊 VMA 安装以及 fork 时的 VMA 复制。核心对象
 * 是受 mmap_lock 保护的 mm/VMA/Maple Tree；成功路径发布地址范围，失败路径必须
 * 撤销 file、策略、anon_vma、页表与 overcommit 记账，不能把半初始化 VMA 暴露出去。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* 基础内存、VMA、共享内存及 mmap ABI 定义。 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
/* 权限、文件、人格、LSM 与 hugetlb/shmem 后端。 */
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
/* shmem/profile/export/mount/policy 补齐共享匿名映射和策略记账接口。 */
#include <linux/shmem_fs.h>
#include <linux/profile.h>
#include <linux/export.h>
#include <linux/mount.h>
#include <linux/mempolicy.h>
/* 反向映射、MMU notifier、审计/trace 与 fork 辅助子系统。 */
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/mmdebug.h>
#include <linux/perf_event.h>
#include <linux/audit.h>
/* khugepaged/uprobes/notifier/hotplug/printk 参与 fork、热插拔和诊断。 */
#include <linux/khugepaged.h>
#include <linux/uprobes.h>
#include <linux/notifier.h>
#include <linux/memory.h>
#include <linux/printk.h>
#include <linux/userfaultfd_k.h>
/* 参数、pkey、OOM、调度 mm、KSM 与 memfd seal 支持。 */
#include <linux/moduleparam.h>
#include <linux/pkeys.h>
#include <linux/oom.h>
#include <linux/sched/mm.h>
#include <linux/ksm.h>
#include <linux/memfd.h>

#include <linux/uaccess.h>
/* 体系结构 cache/TLB/mmu_context 接口决定发布与拆页表的顺序。 */
#include <asm/cacheflush.h>
#include <asm/tlb.h>
#include <asm/mmu_context.h>

#define CREATE_TRACE_POINTS
#include <trace/events/mmap.h>

#include "internal.h"

#ifndef arch_mmap_check
/* 未提供 arch hook 的体系结构对 mmap 范围/flags 不增加额外拒绝。 */
#define arch_mmap_check(addr, len, flags)	(0)
#endif

#ifdef CONFIG_HAVE_ARCH_MMAP_RND_BITS
/* 原生进程 ASLR 位数：min 固定，max 启动后只读，当前值是高频读取 sysctl。 */
const int mmap_rnd_bits_min = CONFIG_ARCH_MMAP_RND_BITS_MIN;
int mmap_rnd_bits_max __ro_after_init = CONFIG_ARCH_MMAP_RND_BITS_MAX;
int mmap_rnd_bits __read_mostly = CONFIG_ARCH_MMAP_RND_BITS;
#endif
#ifdef CONFIG_HAVE_ARCH_MMAP_RND_COMPAT_BITS
/* compat 进程使用独立的随机位数上下界和当前 sysctl 值。 */
const int mmap_rnd_compat_bits_min = CONFIG_ARCH_MMAP_RND_COMPAT_BITS_MIN;
const int mmap_rnd_compat_bits_max = CONFIG_ARCH_MMAP_RND_COMPAT_BITS_MAX;
int mmap_rnd_compat_bits __read_mostly = CONFIG_ARCH_MMAP_RND_COMPAT_BITS;
#endif

static bool ignore_rlimit_data;
/* 启动/模块参数：仅放宽 RLIMIT_DATA，仍保留地址空间和 overcommit 等其他门禁。 */
core_param(ignore_rlimit_data, ignore_rlimit_data, bool, 0644);

/* Update vma->vm_page_prot to reflect vma->vm_flags. */
/* 根据最新 vm_flags 重算页表保护；VMA 修改路径在更改标志后调用。 */
/*
 * 入参 vma 是仍存活的借用 VMA；调用者负责稳定其 flags。函数无返回值和 ownership
 * 变化，最终以 WRITE_ONCE 发布 vm_page_prot，供不持 mmap_lock 的撤保护路径读取。
 */
void vma_set_page_prot(struct vm_area_struct *vma)
{
	/* 先在局部快照上计算，写通知映射需临时去掉 SHARED 后再生成只读保护。 */
	vm_flags_t vm_flags = vma->vm_flags;
	pgprot_t vm_page_prot;

	vm_page_prot = vm_pgprot_modify(vma->vm_page_prot, vm_flags);
	if (vma_wants_writenotify(vma, vm_page_prot)) {
		vm_flags &= ~VM_SHARED;
		vm_page_prot = vm_pgprot_modify(vm_page_prot, vm_flags);
	}
	/* remove_protection_ptes reads vma->vm_page_prot without mmap_lock */
	/* remove_protection_ptes 无 mmap_lock 读取该字段，单次原子发布避免撕裂。 */
	WRITE_ONCE(vma->vm_page_prot, vm_page_prot);
}

/*
 * check_brk_limits() - Use platform specific check of range & verify mlock
 * limits.
 * @addr: The address to check
 * @len: The size of increase.
 *
 * Return: 0 on success.
 */
/*
 * 检查 brk 新增长度：先用 MAP_FIXED 走体系结构地址合法性检查，再核对未来 mlock
 * 是否超过 RLIMIT_MEMLOCK。addr/len 均为页对齐字节范围，不取得对象引用；成功返回
 * 0，地址检查原样返回 errno，锁页额度不足返回 -EAGAIN。调用者持 mmap 写锁。
 */
static int check_brk_limits(unsigned long addr, unsigned long len)
{
	unsigned long mapped_addr;

	/* MAP_FIXED 查询只验证区间能否使用，不在此创建 VMA。 */
	mapped_addr = get_unmapped_area(NULL, addr, len, 0, MAP_FIXED);
	if (IS_ERR_VALUE(mapped_addr))
		return mapped_addr;

	return mlock_future_ok(current->mm,
			      current->mm->def_flags & VM_LOCKED, len)
		? 0 : -EAGAIN;
}

SYSCALL_DEFINE1(brk, unsigned long, brk)
{
	/* 用户堆边界事务：写锁内校验下限/rlimit，按增缩修改 VMA，失败返回并恢复原边界。 */
	/* brk 是用户请求的字节边界；new/oldbrk 是页对齐提交边界，origbrk 用于失败回滚。 */
	unsigned long newbrk, oldbrk, origbrk;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *brkvma, *next = NULL;
	unsigned long min_brk;
	bool populate = false;
	LIST_HEAD(uf);
	struct vma_iterator vmi;

	/* 可中断写锁串行化 mm->brk、VMA 拓扑和后续拆/建映射。 */
	if (mmap_write_lock_killable(mm))
		return -EINTR;

	origbrk = mm->brk;

	min_brk = mm->start_brk;
#ifdef CONFIG_COMPAT_BRK
	/*
	 * CONFIG_COMPAT_BRK can still be overridden by setting
	 * randomize_va_space to 2, which will still cause mm->start_brk
	 * to be arbitrarily shifted
	 */
	/* 兼容布局可用 end_data 作下界；启用强随机化时仍尊重随机后的 start_brk。 */
	if (!current->brk_randomized)
		min_brk = mm->end_data;
#endif
	if (brk < min_brk)
		goto out;

	/*
	 * Check against rlimit here. If this check is done later after the test
	 * of oldbrk with newbrk then it can escape the test and let the data
	 * segment grow beyond its set limit the in case where the limit is
	 * not page aligned -Ram Gupta
	 */
	/* 必须在“页对齐后未变化”的快路径前检查原始字节 brk，防止绕过 RLIMIT_DATA。 */
	if (check_data_rlimit(rlimit(RLIMIT_DATA), brk, mm->start_brk,
			      mm->end_data, mm->start_data))
		goto out;

	newbrk = PAGE_ALIGN(brk);
	oldbrk = PAGE_ALIGN(mm->brk);
	if (oldbrk == newbrk) {
		/* 同页调整只更新精确字节值，不改变 VMA/页表。 */
		mm->brk = brk;
		goto success;
	}

	/* Always allow shrinking brk. */
	/* 收缩不消耗新资源，但仍须确认目标属于当前 brk VMA 而非其他映射。 */
	if (brk <= mm->brk) {
		/* Search one past newbrk */
		/* 从新页边界查到旧页边界，定位覆盖待删除尾部的 brk VMA。 */
		vma_iter_init(&vmi, mm, newbrk);
		brkvma = vma_find(&vmi, oldbrk);
		if (!brkvma || brkvma->vm_start >= oldbrk)
			goto out; /* mapping intersects with an existing non-brk vma. */
		/* 找不到合法 brk VMA 表示与非 brk 映射相交，走统一恢复出口。 */
		/*
		 * mm->brk must be protected by write mmap_lock.
		 * do_vmi_align_munmap() will drop the lock on success,  so
		 * update it before calling do_vma_munmap().
		 */
		/* munmap 成功会代为解锁，故先在锁内提交 brk；失败出口再恢复 origbrk。 */
		mm->brk = brk;
		if (do_vmi_align_munmap(&vmi, brkvma, mm, newbrk, oldbrk, &uf,
					/* unlock = */ true))
			goto out;

		goto success_unlocked;
	}

	/* 扩张先过体系结构、mlock 和空闲区检查，再创建新的 brk 区间。 */
	if (check_brk_limits(oldbrk, newbrk - oldbrk))
		goto out;

	/*
	 * Only check if the next VMA is within the stack_guard_gap of the
	 * expansion area
	 */
	/* 只在相邻 VMA 进入 guard gap 时拒绝，避免堆增长侵入栈保护区。 */
	vma_iter_init(&vmi, mm, oldbrk);
	next = vma_find(&vmi, newbrk + PAGE_SIZE + stack_guard_gap);
	if (next && newbrk + PAGE_SIZE > vm_start_gap(next))
		goto out;

	brkvma = vma_prev_limit(&vmi, mm->start_brk);
	/* Ok, looks good - let it rip. */
	/* 前驱可供 do_brk_flags 合并；负返回保持旧拓扑并进入统一回滚。 */
	if (do_brk_flags(&vmi, brkvma, oldbrk, newbrk - oldbrk,
			 EMPTY_VMA_FLAGS) < 0)
		goto out;

	mm->brk = brk;
	if (mm->def_flags & VM_LOCKED)
		populate = true;

success:
	/* 普通成功仍持写锁；收缩成功已由 munmap 解锁，二者在完成 userfaultfd 后汇合。 */
	mmap_write_unlock(mm);
success_unlocked:
	userfaultfd_unmap_complete(mm, &uf);
	if (populate)
		mm_populate(oldbrk, newbrk - oldbrk);
	return brk;

out:
	/* 所有拒绝路径恢复精确 brk 并解锁，系统调用按传统 ABI 返回原 brk 而非 errno。 */
	mm->brk = origbrk;
	mmap_write_unlock(mm);
	return origbrk;
}

/*
 * If a hint addr is less than mmap_min_addr change hint to be as
 * low as possible but still greater than mmap_min_addr
 */
/*
 * 非固定 mmap hint 的规范化 helper：页对齐非零 hint，低于安全下界时抬到
 * PAGE_ALIGN(mmap_min_addr)。返回候选地址，不检查空闲性；do_mmap 随后交给选址器。
 */
static inline unsigned long round_hint_to_min(unsigned long hint)
{
	hint &= PAGE_MASK;
	if (((void *)hint != NULL) &&
	    (hint < mmap_min_addr))
		return PAGE_ALIGN(mmap_min_addr);
	return hint;
}

/*
 * 判断新增 bytes 的锁页 VMA 是否可接受：mm 只借用，is_vma_locked 表示该范围最终
 * 是否锁页。非锁页或 CAP_IPC_LOCK 快速成功；否则现有 locked_vm 加新增页不得超过
 * RLIMIT_MEMLOCK。返回布尔、无记账副作用，调用者仍负责实际锁页。
 */
bool mlock_future_ok(const struct mm_struct *mm, bool is_vma_locked,
		     unsigned long bytes)
{
	/* mm 为借用地址空间，bytes 是新增锁页字节；无锁页需求或有能力时快速允许。 */
	unsigned long locked_pages, limit_pages;

	if (!is_vma_locked || capable(CAP_IPC_LOCK))
		return true;

	/* 将新增字节与现有 locked_vm 都换成页数，对比当前进程 MEMLOCK 限额。 */
	locked_pages = bytes >> PAGE_SHIFT;
	locked_pages += mm->locked_vm;

	limit_pages = rlimit(RLIMIT_MEMLOCK);
	limit_pages >>= PAGE_SHIFT;

	return locked_pages <= limit_pages;
}

/*
 * 根据 inode 类型给文件 mmap 计算最大字节位置：常规/块/socket 用 LFS 上限，支持
 * 无符号 offset 的特殊驱动返回 0 哨兵，其余限制 ULONG_MAX。file/inode 只借用。
 */
static inline u64 file_mmap_size_max(struct file *file, struct inode *inode)
{
	/* 常规文件、块设备和 socket 使用 LFS 上限；其他驱动按 offset 能力选择边界。 */
	if (S_ISREG(inode->i_mode))
		return MAX_LFS_FILESIZE;

	if (S_ISBLK(inode->i_mode))
		return MAX_LFS_FILESIZE;

	if (S_ISSOCK(inode->i_mode))
		return MAX_LFS_FILESIZE;

	/* Special "we do even unsigned file positions" case */
	/* FOP_UNSIGNED_OFFSET 表示驱动自行接受完整无符号偏移，0 作为“不设上限”哨兵。 */
	if (file->f_op->fop_flags & FOP_UNSIGNED_OFFSET)
		return 0;

	/* Yes, random drivers might want more. But I'm tired of buggy drivers */
	/* 其余特殊文件保守限制到 ULONG_MAX，避免页偏移和长度组合溢出。 */
	return ULONG_MAX;
}

static inline bool file_mmap_ok(struct file *file, struct inode *inode,
				unsigned long pgoff, unsigned long len)
{
	/* 校验 [pgoff*PAGE_SIZE, +len) 未超过类型上限；只读输入，返回布尔且无副作用。 */
	/* maxsize=0 表示无显式上限；否则以先减 len 的方式避免 pgoff+len 溢出。 */
	u64 maxsize = file_mmap_size_max(file, inode);

	if (maxsize && len > maxsize)
		return false;
	maxsize -= len;
	if (pgoff > maxsize >> PAGE_SHIFT)
		return false;
	return true;
}

/**
 * do_mmap() - Perform a userland memory mapping into the current process
 * address space of length @len with protection bits @prot, mmap flags @flags
 * (from which VMA flags will be inferred), and any additional VMA flags to
 * apply @vm_flags. If this is a file-backed mapping then the file is specified
 * in @file and page offset into the file via @pgoff.
 *
 * This function does not perform security checks on the file and assumes, if
 * @uf is non-NULL, the caller has provided a list head to track unmap events
 * for userfaultfd @uf.
 *
 * It also simply indicates whether memory population is required by setting
 * @populate, which must be non-NULL, expecting the caller to actually perform
 * this task itself if appropriate.
 *
 * This function will invoke architecture-specific (and if provided and
 * relevant, file system-specific) logic to determine the most appropriate
 * unmapped area in which to place the mapping if not MAP_FIXED.
 *
 * Callers which require userland mmap() behaviour should invoke vm_mmap(),
 * which is also exported for module use.
 *
 * Those which require this behaviour less security checks, userfaultfd and
 * populate behaviour, and who handle the mmap write lock themselves, should
 * call this function.
 *
 * Note that the returned address may reside within a merged VMA if an
 * appropriate merge were to take place, so it doesn't necessarily specify the
 * start of a VMA, rather only the start of a valid mapped range of length
 * @len bytes, rounded down to the nearest page size.
 *
 * The caller must write-lock current->mm->mmap_lock.
 *
 * @file: An optional struct file pointer describing the file which is to be
 * mapped, if a file-backed mapping.
 * @addr: If non-zero, hints at (or if @flags has MAP_FIXED set, specifies) the
 * address at which to perform this mapping. See mmap (2) for details. Must be
 * page-aligned.
 * @len: The length of the mapping. Will be page-aligned and must be at least 1
 * page in size.
 * @prot: Protection bits describing access required to the mapping. See mmap
 * (2) for details.
 * @flags: Flags specifying how the mapping should be performed, see mmap (2)
 * for details.
 * @vm_flags: VMA flags which should be set by default, or 0 otherwise.
 * @pgoff: Page offset into the @file if file-backed, should be 0 otherwise.
 * @populate: A pointer to a value which will be set to 0 if no population of
 * the range is required, or the number of bytes to populate if it is. Must be
 * non-NULL. See mmap (2) for details as to under what circumstances population
 * of the range occurs.
 * @uf: An optional pointer to a list head to track userfaultfd unmap events
 * should unmapping events arise. If provided, it is up to the caller to manage
 * this.
 *
 * Returns: Either an error, or the address at which the requested mapping has
 * been performed.
 */
/*
 * 中文契约：do_mmap 是持 mmap 写锁调用的核心映射事务。file 可空且只借用；addr/len
 * 为字节 hint/长度，prot/flags 是用户协议，vm_flags 是调用者附加标志，pgoff 是页偏移；
 * populate 非空输出需预取的字节数，uf 可空并由调用者最终完成 userfaultfd 事件。
 * 成功返回映射起址（可能位于合并 VMA 内），失败返回编码 errno；本函数校验/选址后由
 * mmap_region 发布或替换 VMA，不负责实际 populate，也不取得 file 的持久调用者引用。
 */
unsigned long do_mmap(struct file *file, unsigned long addr,
			unsigned long len, unsigned long prot,
			unsigned long flags, vm_flags_t vm_flags,
			unsigned long pgoff, unsigned long *populate,
			struct list_head *uf)
{
	/* mm 来自 current；pkey 仅在纯执行映射上尝试分配，populate 先清零保证失败输出。 */
	struct mm_struct *mm = current->mm;
	int pkey = 0;

	*populate = 0;

	mmap_assert_write_locked(mm);

	if (!len)
		return -EINVAL;

	/*
	 * Does the application expect PROT_READ to imply PROT_EXEC?
	 *
	 * (the exception is when the underlying filesystem is noexec
	 *  mounted, in which case we don't add PROT_EXEC.)
	 */
	/* READ_IMPLIES_EXEC 兼容人格把可读提升为可执行，但 noexec 挂载必须阻止提升。 */
	if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC))
		if (!(file && path_noexec(&file->f_path)))
			prot |= PROT_EXEC;

	/* force arch specific MAP_FIXED handling in get_unmapped_area */
	/* NOREPLACE 也需走体系结构 fixed 约束，稍后另查交集以返回 -EEXIST。 */
	if (flags & MAP_FIXED_NOREPLACE)
		flags |= MAP_FIXED;

	if (!(flags & MAP_FIXED))
		addr = round_hint_to_min(addr);

	/* Careful about overflows.. */
	/* 页对齐回绕为零表示长度溢出；原始零长度与溢出分别返回 EINVAL/ENOMEM。 */
	len = PAGE_ALIGN(len);
	if (!len)
		return -ENOMEM;

	/* offset overflow? */
	/* pgoff 加页数回绕会越过文件页偏移空间，映射尚未发布即可直接拒绝。 */
	if ((pgoff + (len >> PAGE_SHIFT)) < pgoff)
		return -EOVERFLOW;

	/* Too many mappings? */
	/* map_count 门禁预留并发/后续拆分余量，真正发布仍由 mmap_region 完成。 */
	if (mm->map_count > get_sysctl_max_map_count())
		return -ENOMEM;

	/*
	 * addr is returned from get_unmapped_area,
	 * There are two cases:
	 * 1> MAP_FIXED == false
	 *	unallocated memory, no need to check sealing.
	 * 1> MAP_FIXED == true
	 *	sealing is checked inside mmap_region when
	 *	do_vmi_munmap is called.
	 */
	/* 非 fixed 地址必为空闲；fixed 覆盖的 seal 检查延后到 mmap_region 的 munmap 事务。 */

	/* 纯 execute-only 映射尽量使用专属 protection key，失败则退回 pkey 0。 */
	if (prot == PROT_EXEC) {
		pkey = execute_only_pkey(mm);
		if (pkey < 0)
			pkey = 0;
	}

	/* Do simple checking here so the lower-level routines won't have
	 * to. we assume access permissions have been handled by the open
	 * of the memory object, so we don't do any here.
	 */
	/* 把用户 prot/flags、mm 缺省和 MAY 权限合成为内部 VMA flags。 */
	vm_flags |= calc_vm_prot_bits(prot, pkey) | calc_vm_flag_bits(file, flags) |
			mm->def_flags | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;

	/* Obtain the address to map to. we verify (or select) it and ensure
	 * that it represents a valid section of the address space.
	 */
	/* 文件/体系结构/THP 选址器返回最终页对齐地址或编码 errno，尚未修改 VMA 树。 */
	addr = __get_unmapped_area(file, addr, len, pgoff, flags, vm_flags);
	if (IS_ERR_VALUE(addr))
		return addr;

	/* NOREPLACE 与任何现存 VMA 相交都失败，不能像 MAP_FIXED 那样替换。 */
	if (flags & MAP_FIXED_NOREPLACE) {
		if (find_vma_intersection(mm, addr, addr + len))
			return -EEXIST;
	}

	/* 显式锁页先查能力，再把新增页数与 locked_vm/MEMLOCK 限额合并核算。 */
	if (flags & MAP_LOCKED)
		if (!can_do_mlock())
			return -EPERM;

	if (!mlock_future_ok(mm, vm_flags & VM_LOCKED, len))
		return -EAGAIN;

	/* 文件映射分支还需核对文件大小、共享写权限、挂载属性、mmap op 与 seals。 */
	if (file) {
		struct inode *inode = file_inode(file);
		unsigned long flags_mask;
		int err;

		if (!file_mmap_ok(file, inode, pgoff, len))
			return -EOVERFLOW;

		/* legacy mask 是所有文件缺省支持集，FOP_MMAP_SYNC 显式扩展严格协议。 */
		flags_mask = LEGACY_MAP_MASK;
		if (file->f_op->fop_flags & FOP_MMAP_SYNC)
			flags_mask |= MAP_SYNC;

		switch (flags & MAP_TYPE) {
		case MAP_SHARED:
			/*
			 * Force use of MAP_SHARED_VALIDATE with non-legacy
			 * flags. E.g. MAP_SYNC is dangerous to use with
			 * MAP_SHARED as you don't know which consistency model
			 * you will get. We silently ignore unsupported flags
			 * with MAP_SHARED to preserve backward compatibility.
			 */
			/* 旧 MAP_SHARED 静默丢弃非传统 flag；需严格语义的 flag 必须用 VALIDATE。 */
			flags &= LEGACY_MAP_MASK;
			fallthrough;
		case MAP_SHARED_VALIDATE:
			/* VALIDATE 拒绝文件不支持的位；可写共享映射还要求写 fd 且不能是 swapfile。 */
			if (flags & ~flags_mask)
				return -EOPNOTSUPP;
			if (prot & PROT_WRITE) {
				if (!(file->f_mode & FMODE_WRITE))
					return -EACCES;
				if (IS_SWAPFILE(file->f_mapping->host))
					return -ETXTBSY;
			}

			/*
			 * Make sure we don't allow writing to an append-only
			 * file..
			 */
			/* append-only 文件不能通过共享可写映射绕过追加语义。 */
			if (IS_APPEND(inode) && (file->f_mode & FMODE_WRITE))
				return -EACCES;

			vm_flags |= VM_SHARED | VM_MAYSHARE;
			if (!(file->f_mode & FMODE_WRITE))
				vm_flags &= ~(VM_MAYWRITE | VM_SHARED);
			fallthrough;
		case MAP_PRIVATE:
			/* 私有和共享最终都要求可读 fd、可执行挂载许可及文件 mmap 能力。 */
			if (!(file->f_mode & FMODE_READ))
				return -EACCES;
			if (path_noexec(&file->f_path)) {
				if (vm_flags & VM_EXEC)
					return -EPERM;
				vm_flags &= ~VM_MAYEXEC;
			}

			/* 没有 mmap 实现或与栈增长标志组合都不是合法文件 VMA。 */
			if (!can_mmap_file(file))
				return -ENODEV;
			if (vm_flags & (VM_GROWSDOWN|VM_GROWSUP))
				return -EINVAL;
			break;

		default:
			return -EINVAL;
		}

		/*
		 * Check to see if we are violating any seals and update VMA
		 * flags if necessary to avoid future seal violations.
		 */
		/* memfd seal 既可拒绝当前映射，也可收紧未来允许的 VMA flags。 */
		err = memfd_check_seals_mmap(file, &vm_flags);
		if (err)
			return (unsigned long)err;
	} else {
		/* 匿名映射只接受 shared/private/droppable；pgoff 不再表示文件位置。 */
		switch (flags & MAP_TYPE) {
		case MAP_SHARED:
			if (vm_flags & (VM_GROWSDOWN|VM_GROWSUP))
				return -EINVAL;
			/*
			 * Ignore pgoff.
			 */
			/* 匿名 shared 后端由 shmem 建立，用户给出的文件页偏移无意义。 */
			pgoff = 0;
			vm_flags |= VM_SHARED | VM_MAYSHARE;
			break;
		case MAP_DROPPABLE:
			if (VM_DROPPABLE == VM_NONE)
				return -EOPNOTSUPP;
			/*
			 * A locked or stack area makes no sense to be droppable.
			 *
			 * Also, since droppable pages can just go away at any time
			 * it makes no sense to copy them on fork or dump them.
			 *
			 * And don't attempt to combine with hugetlb for now.
			 */
			/* droppable 不能锁页、作栈或 hugetlb，并在回收/fork/core 时采用易失语义。 */
			/* 两组互斥检查分别来自用户 flags 与已合成的内部 VMA flags。 */
			if (flags & (MAP_LOCKED | MAP_HUGETLB))
			        return -EINVAL;
			if (vm_flags & (VM_GROWSDOWN | VM_GROWSUP))
			        return -EINVAL;

			vm_flags |= VM_DROPPABLE;

			/*
			 * If the pages can be dropped, then it doesn't make
			 * sense to reserve them.
			 */
			/* 页面可随时丢弃，因此跳过提交量预留。 */
			vm_flags |= VM_NORESERVE;

			/*
			 * Likewise, they're volatile enough that they
			 * shouldn't survive forks or coredumps.
			 */
			/* fork 清零且 core dump 跳过，防止把易失内容固化到后续状态。 */
			vm_flags |= VM_WIPEONFORK | VM_DONTDUMP;
			fallthrough;
		case MAP_PRIVATE:
			/*
			 * Set pgoff according to addr for anon_vma.
			 */
			/* 匿名私有映射用虚拟页号作 pgoff，支持相邻 VMA 合并的一致索引。 */
			pgoff = addr >> PAGE_SHIFT;
			break;
		default:
			return -EINVAL;
		}
	}

	/*
	 * Set 'VM_NORESERVE' if we should not account for the
	 * memory use of this mapping.
	 */
	/* NORESERVE 只在非严格 overcommit 或 hugetlb 显式请求时转为内部标志。 */
	if (flags & MAP_NORESERVE) {
		/* We honor MAP_NORESERVE if allowed to overcommit */
		/* 非 OVERCOMMIT_NEVER 模式允许普通映射推迟承诺记账。 */
		if (sysctl_overcommit_memory != OVERCOMMIT_NEVER)
			vm_flags |= VM_NORESERVE;

		/* hugetlb applies strict overcommit unless MAP_NORESERVE */
		/* hugetlb 默认严格预留，显式 NORESERVE 才跳过。 */
		if (file && is_file_hugepages(file))
			vm_flags |= VM_NORESERVE;
	}

	/* 最终提交点：mmap_region 建立/合并 VMA 并处理 fixed 覆盖；此前均可无状态返回。 */
	addr = mmap_region(file, addr, len, vm_flags, pgoff, uf);
	if (!IS_ERR_VALUE(addr) &&
	    ((vm_flags & VM_LOCKED) ||
	     (flags & (MAP_POPULATE | MAP_NONBLOCK)) == MAP_POPULATE))
		*populate = len;
	/* 调用者在解锁和完成 userfaultfd 后按 populate 实际预取页面。 */
	return addr;
}

/*
 * ksys_mmap_pgoff 是系统调用公共包装：按 fd/匿名 hugetlb 构造借用 file，调整大页
 * 长度后交给 vm_mmap_pgoff（其负责安全检查与 mmap 锁）。成功返回地址、失败返回 errno；
 * 本函数持有的 fget/hugetlb 临时引用无论成败都在 out_fput 释放。
 */
unsigned long ksys_mmap_pgoff(unsigned long addr, unsigned long len,
			      unsigned long prot, unsigned long flags,
			      unsigned long fd, unsigned long pgoff)
{
	struct file *file = NULL;
	unsigned long retval;

	/* 文件路径先审计并稳定 fd；hugetlb 文件按自身 hstate 对齐长度。 */
	if (!(flags & MAP_ANONYMOUS)) {
		audit_mmap_fd(fd, flags);
		file = fget(fd);
		if (!file)
			return -EBADF;
		/* fd 指向 hugetlb 时采用文件 hstate；普通文件却带 HUGETLB flag 属协议冲突。 */
		if (is_file_hugepages(file)) {
			len = ALIGN(len, huge_page_size(hstate_file(file)));
		} else if (unlikely(flags & MAP_HUGETLB)) {
			retval = -EINVAL;
			goto out_fput;
		}
	} else if (flags & MAP_HUGETLB) {
		/* 匿名 hugetlb 根据 flag 选择 hstate，并创建只为本次映射服务的伪文件。 */
		struct hstate *hs;

		hs = hstate_sizelog((flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK);
		if (!hs)
			return -EINVAL;

		len = ALIGN(len, huge_page_size(hs));
		/*
		 * VM_NORESERVE is used because the reservations will be
		 * taken when vm_ops->mmap() is called
		 */
		/* 预留推迟到 hugetlb mmap 回调，故创建文件时带 VMA_NORESERVE。 */
		file = hugetlb_file_setup(HUGETLB_ANON_FILE, len,
				mk_vma_flags(VMA_NORESERVE_BIT),
				HUGETLB_ANONHUGE_INODE,
				(flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK);
		if (IS_ERR(file))
			return PTR_ERR(file);
	}

	/* 下层返回后不再需要包装层引用；已建 VMA 会持有自己的 file 引用。 */
	retval = vm_mmap_pgoff(file, addr, len, prot, flags, pgoff);
out_fput:
	if (file)
		fput(file);
	return retval;
}

SYSCALL_DEFINE6(mmap_pgoff, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags,
		unsigned long, fd, unsigned long, pgoff)
{
	/* 现代 mmap ABI 直接把页偏移参数交给公共包装，返回地址或编码 errno。 */
	return ksys_mmap_pgoff(addr, len, prot, flags, fd, pgoff);
}

#ifdef __ARCH_WANT_SYS_OLD_MMAP
struct mmap_arg_struct {
	/* 六字段是旧 ABI 的用户快照：五个 mmap 参数加字节 offset。 */
	unsigned long addr;
	unsigned long len;
	unsigned long prot;
	unsigned long flags;
	unsigned long fd;
	unsigned long offset;
};

SYSCALL_DEFINE1(old_mmap, struct mmap_arg_struct __user *, arg)
{
	/* arg 仅为用户指针；先复制到栈，校验页对齐后把字节 offset 转为 pgoff。 */
	struct mmap_arg_struct a;

	if (copy_from_user(&a, arg, sizeof(a)))
		return -EFAULT;
	if (offset_in_page(a.offset))
		return -EINVAL;

	return ksys_mmap_pgoff(a.addr, a.len, a.prot, a.flags, a.fd,
			       a.offset >> PAGE_SHIFT);
}
#endif /* __ARCH_WANT_SYS_OLD_MMAP */

/*
 * Determine if the allocation needs to ensure that there is no
 * existing mapping within it's guard gaps, for use as start_gap.
 */
/* shadow stack 选址需额外保留一页起始 guard；普通 VMA 返回 0。 */
static inline unsigned long stack_guard_placement(vm_flags_t vm_flags)
{
	if (vm_flags & VM_SHADOW_STACK)
		return PAGE_SIZE;

	return 0;
}

/*
 * Search for an unmapped address range.
 *
 * We are looking for a range that:
 * - does not intersect with any VMA;
 * - is contained within the [low_limit, high_limit) interval;
 * - is at least the desired size.
 * - satisfies (begin_addr & align_mask) == (align_offset & align_mask)
 */
/*
 * info 输入上下界、长度、方向、对齐和 guard 约束；根据 TOPDOWN 标志调用 Maple Tree
 * 空洞搜索，返回候选地址或编码 errno，并统一发 trace。调用者须保证 mm/VMA 视图稳定。
 */
unsigned long vm_unmapped_area(struct vm_unmapped_area_info *info)
{
	unsigned long addr;

	if (info->flags & VM_UNMAPPED_AREA_TOPDOWN)
		addr = unmapped_area_topdown(info);
	else
		addr = unmapped_area(info);

	trace_vm_unmapped_area(addr, info);
	return addr;
}

/* Get an address range which is currently unmapped.
 * For shmat() with addr=0.
 *
 * Ugly calling convention alert:
 * Return value with the low bits set means error value,
 * ie
 *	if (ret & ~PAGE_MASK)
 *		error = ret;
 *
 * This function "knows" that -ENOMEM has the bits set.
 */
/*
 * bottom-up 通用选址器：file 仅用于 hugetlb 对齐，addr 是可选 hint，len 为字节，
 * pgoff 在本实现不参与普通对齐，flags/vm_flags 决定 fixed 与 guard。fixed 原样返回；
 * hint 可用则快速返回，否则在 [mmap_base,mmap_end) 搜索，失败传递编码 errno。
 */
unsigned long
generic_get_unmapped_area(struct file *filp, unsigned long addr,
			  unsigned long len, unsigned long pgoff,
			  unsigned long flags, vm_flags_t vm_flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma, *prev;
	struct vm_unmapped_area_info info = {};
	const unsigned long mmap_end = arch_get_mmap_end(addr, len, flags);

	/* 长度连整个允许地址区都容不下时无需查树。 */
	if (len > mmap_end - mmap_min_addr)
		return -ENOMEM;

	if (flags & MAP_FIXED)
		return addr;

	/* hint 必须同时避开后一 VMA 的 start gap 与前一 VMA 的 end gap。 */
	if (addr) {
		addr = PAGE_ALIGN(addr);
		vma = find_vma_prev(mm, addr, &prev);
		if (mmap_end - len >= addr && addr >= mmap_min_addr &&
		    (!vma || addr + len <= vm_start_gap(vma)) &&
		    (!prev || addr >= vm_end_gap(prev)))
			return addr;
	}

	/* hint 失败后从 mm->mmap_base 向高地址搜索；hugetlb 再附加大页掩码。 */
	info.length = len;
	info.low_limit = mm->mmap_base;
	info.high_limit = mmap_end;
	info.start_gap = stack_guard_placement(vm_flags);
	if (filp && is_file_hugepages(filp))
		info.align_mask = huge_page_mask_align(filp);
	return vm_unmapped_area(&info);
}

#ifndef HAVE_ARCH_UNMAPPED_AREA
/* 无体系结构覆盖时，arch 入口完整转发到底向上通用策略。 */
unsigned long
arch_get_unmapped_area(struct file *filp, unsigned long addr,
		       unsigned long len, unsigned long pgoff,
		       unsigned long flags, vm_flags_t vm_flags)
{
	return generic_get_unmapped_area(filp, addr, len, pgoff, flags,
					 vm_flags);
}
#endif

/*
 * This mmap-allocator allocates new areas top-down from below the
 * stack's low limit (the base):
 */
/*
 * top-down 通用选址器优先从栈下方向低地址搜索；输入/ownership 与 bottom-up 相同。
 * fixed/hint 快速路径相同，主搜索仅在 -ENOMEM 时退化到底向上策略，避免大栈布局
 * 使本可满足的 mmap 失败；返回最终候选或编码 errno。
 */
unsigned long
generic_get_unmapped_area_topdown(struct file *filp, unsigned long addr,
				  unsigned long len, unsigned long pgoff,
				  unsigned long flags, vm_flags_t vm_flags)
{
	struct vm_area_struct *vma, *prev;
	struct mm_struct *mm = current->mm;
	struct vm_unmapped_area_info info = {};
	const unsigned long mmap_end = arch_get_mmap_end(addr, len, flags);

	/* requested length too big for entire address space */
	/* 请求大于整个允许区间，直接返回 -ENOMEM。 */
	if (len > mmap_end - mmap_min_addr)
		return -ENOMEM;

	if (flags & MAP_FIXED)
		return addr;

	/* requesting a specific address */
	/* 非零 hint 经页对齐后先尝试前后 guard 都满足的快速路径。 */
	if (addr) {
		addr = PAGE_ALIGN(addr);
		vma = find_vma_prev(mm, addr, &prev);
		if (mmap_end - len >= addr && addr >= mmap_min_addr &&
				(!vma || addr + len <= vm_start_gap(vma)) &&
				(!prev || addr >= vm_end_gap(prev)))
			return addr;
	}

	/* 主搜索上界由 arch mmap base 决定，并携带 shadow-stack/hugetlb 对齐要求。 */
	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	info.low_limit = PAGE_SIZE;
	info.high_limit = arch_get_mmap_base(addr, mm->mmap_base);
	info.start_gap = stack_guard_placement(vm_flags);
	if (filp && is_file_hugepages(filp))
		info.align_mask = huge_page_mask_align(filp);
	addr = vm_unmapped_area(&info);

	/*
	 * A failed mmap() very likely causes application failure,
	 * so fall back to the bottom-up function here. This scenario
	 * can happen with large stack limits and large mmap()
	 * allocations.
	 */
	/* 只有 top-down 明确返回 -ENOMEM 才改为 TASK_UNMAPPED_BASE 起的 bottom-up 重试。 */
	if (offset_in_page(addr)) {
		VM_BUG_ON(addr != -ENOMEM);
		info.flags = 0;
		info.low_limit = TASK_UNMAPPED_BASE;
		info.high_limit = mmap_end;
		addr = vm_unmapped_area(&info);
	}

	return addr;
}

#ifndef HAVE_ARCH_UNMAPPED_AREA_TOPDOWN
/* 无体系结构 top-down 覆盖时转发通用退化策略。 */
unsigned long
arch_get_unmapped_area_topdown(struct file *filp, unsigned long addr,
			       unsigned long len, unsigned long pgoff,
			       unsigned long flags, vm_flags_t vm_flags)
{
	return generic_get_unmapped_area_topdown(filp, addr, len, pgoff, flags,
						 vm_flags);
}
#endif

unsigned long mm_get_unmapped_area_vmflags(struct file *filp, unsigned long addr,
					   unsigned long len, unsigned long pgoff,
					   unsigned long flags, vm_flags_t vm_flags)
{
	/* MMF_TOPDOWN 是 mm 布局策略；选择对应 arch hook，返回值尚未通过 LSM 地址检查。 */
	if (mm_flags_test(MMF_TOPDOWN, current->mm))
		return arch_get_unmapped_area_topdown(filp, addr, len, pgoff,
						      flags, vm_flags);
	return arch_get_unmapped_area(filp, addr, len, pgoff, flags, vm_flags);
}

/*
 * 完整空闲地址分派器：file 可空借用，addr/len 为字节，pgoff 为页偏移，flags 与
 * vm_flags 分别是用户/内部策略。依次经过 arch、file/shmem/THP/布局和 LSM 检查，
 * 返回页对齐候选或编码 errno；只查询，不发布 VMA。
 */
unsigned long
__get_unmapped_area(struct file *file, unsigned long addr, unsigned long len,
		unsigned long pgoff, unsigned long flags, vm_flags_t vm_flags)
{
	/* get_area 可来自文件，匿名 shared 可来自 shmem，其余走 mm/arch 缺省策略。 */
	unsigned long (*get_area)(struct file *, unsigned long,
				  unsigned long, unsigned long, unsigned long)
				  = NULL;

	/* 第一阶段先让体系结构拒绝非法 flag/range，再防 TASK_SIZE 长度溢出。 */
	unsigned long error = arch_mmap_check(addr, len, flags);
	if (error)
		return error;

	/* Careful about overflows.. */
	/* len 大于整个用户空间会使后续 TASK_SIZE-len 下溢，提前返回 -ENOMEM。 */
	if (len > TASK_SIZE)
		return -ENOMEM;

	if (file) {
		if (file->f_op->get_unmapped_area)
			get_area = file->f_op->get_unmapped_area;
	} else if (flags & MAP_SHARED) {
		/*
		 * mmap_region() will call shmem_zero_setup() to create a file,
		 * so use shmem's get_unmapped_area in case it can be huge.
		 */
		/* 匿名 shared 稍后会变成 shmem 文件，提前用 shmem hook 保留大页对齐机会。 */
		get_area = shmem_get_unmapped_area;
	}

	/* Always treat pgoff as zero for anonymous memory. */
	/* 匿名地址选择不允许用户 pgoff 影响文件型对齐。 */
	if (!file)
		pgoff = 0;

	/* 第二阶段按文件/shmem、无 hint 的 PMD 对齐匿名 THP、普通 arch 策略三选一。 */
	if (get_area) {
		addr = get_area(file, addr, len, pgoff, flags);
	} else if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) && !file
		   && !addr /* no hint */
		   /* 无 hint 才能自由调整到 PMD 边界。 */
		   && IS_ALIGNED(len, PMD_SIZE)) {
		/* Ensures that larger anonymous mappings are THP aligned. */
		/* PMD 整倍长度优先 THP 对齐，提升后续折叠/大页 fault 成功率。 */
		addr = thp_get_unmapped_area_vmflags(file, addr, len,
						     pgoff, flags, vm_flags);
	} else {
		addr = mm_get_unmapped_area_vmflags(file, addr, len,
						    pgoff, flags, vm_flags);
	}
	/* 第三阶段统一验证错误、TASK_SIZE 边界、页对齐和 LSM mmap_min_addr 策略。 */
	if (IS_ERR_VALUE(addr))
		return addr;

	if (addr > TASK_SIZE - len)
		return -ENOMEM;
	if (offset_in_page(addr))
		return -EINVAL;

	error = security_mmap_addr(addr);
	return error ? error : addr;
}

unsigned long
mm_get_unmapped_area(struct file *file, unsigned long addr, unsigned long len,
		     unsigned long pgoff, unsigned long flags)
{
	/* 导出兼容入口不附加 VMA flags，供内核调用者取得经布局选择的候选地址。 */
	return mm_get_unmapped_area_vmflags(file, addr, len, pgoff, flags, 0);
}
EXPORT_SYMBOL(mm_get_unmapped_area);

/**
 * find_vma_intersection() - Look up the first VMA which intersects the interval
 * @mm: The process address space.
 * @start_addr: The inclusive start user address.
 * @end_addr: The exclusive end user address.
 *
 * Returns: The first VMA within the provided range, %NULL otherwise.  Assumes
 * start_addr < end_addr.
 */
/*
 * 在持 mmap_lock 的 mm Maple Tree 中查找首个与 [start_addr,end_addr) 相交的
 * 借用 VMA；无交集返回 NULL，不取得引用。调用者只能在锁/其他稳定机制有效期内使用。
 */
struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
					     unsigned long start_addr,
					     unsigned long end_addr)
{
	unsigned long index = start_addr;

	mmap_assert_locked(mm);
	return mt_find(&mm->mm_mt, &index, end_addr - 1);
}
EXPORT_SYMBOL(find_vma_intersection);

/**
 * find_vma() - Find the VMA for a given address, or the next VMA.
 * @mm: The mm_struct to check
 * @addr: The address
 *
 * Returns: The VMA associated with addr, or the next VMA.
 * May return %NULL in the case of no VMA at addr or above.
 */
/*
 * 返回包含 addr 的 VMA，否则返回其后第一项；树尾返回 NULL。mm/返回指针均借用，
 * 调用者必须已持 mmap_lock，返回后通常检查边界以区分“包含”和“下一项”。
 */
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	unsigned long index = addr;

	mmap_assert_locked(mm);
	return mt_find(&mm->mm_mt, &index, ULONG_MAX);
}
EXPORT_SYMBOL(find_vma);

/**
 * find_vma_prev() - Find the VMA for a given address, or the next vma and
 * set %pprev to the previous VMA, if any.
 * @mm: The mm_struct to check
 * @addr: The address
 * @pprev: The pointer to set to the previous VMA
 *
 * Note that RCU lock is missing here since the external mmap_lock() is used
 * instead.
 *
 * Returns: The VMA associated with @addr, or the next vma.
 * May return %NULL in the case of no vma at addr or above.
 */
/*
 * 与 find_vma 同时通过 pprev 输出严格前驱；mm 和输出 VMA 都只借用。外部 mmap_lock
 * 替代 RCU 稳定 Maple Tree，调用者用当前项/前驱判断空洞、guard 或栈扩展方向。
 */
struct vm_area_struct *
find_vma_prev(struct mm_struct *mm, unsigned long addr,
			struct vm_area_struct **pprev)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, addr);

	/* load 得到包含/相邻槽，prev 输出严格前驱；空槽再向后取下一 VMA。 */
	vma = vma_iter_load(&vmi);
	*pprev = vma_prev(&vmi);
	if (!vma)
		vma = vma_next(&vmi);
	return vma;
}

/* enforced gap between the expanding stack and other mappings. */
/* 栈与相邻映射的最小隔离字节数，缺省 256 页；启动参数在并发使用前一次性改写。 */
unsigned long stack_guard_gap = 256UL<<PAGE_SHIFT;

/* 早期启动解析十进制页数，完整消费字符串时发布为字节；始终返回“参数已处理”。 */
static int __init cmdline_parse_stack_guard_gap(char *p)
{
	unsigned long val;
	char *endptr;

	/* 只有十进制串无尾随字符时才接受，页数左移为字节 gap。 */
	val = simple_strtoul(p, &endptr, 10);
	if (!*endptr)
		stack_guard_gap = val << PAGE_SHIFT;

	return 1;
}
__setup("stack_guard_gap=", cmdline_parse_stack_guard_gap);

#ifdef CONFIG_STACK_GROWSUP
/* 向上增长配置：持 mmap 写锁把 address 纳入 vma，返回 expand_upwards 的 errno。 */
int expand_stack_locked(struct vm_area_struct *vma, unsigned long address)
{
	return expand_upwards(vma, address);
}

/*
 * 持 mmap 写锁查找包含 addr 的 VMA；未命中时仅尝试把前驱 GROWSUP 栈扩到页对齐地址。
 * 返回锁期借用 VMA，失败/无前驱返回 NULL；VM_LOCKED 新区间在成功后同步 populate。
 */
struct vm_area_struct *find_extend_vma_locked(struct mm_struct *mm, unsigned long addr)
{
	/* 查包含项；否则只可扩张前驱向上栈，成功后锁页 VMA 立即预 fault 新区间。 */
	struct vm_area_struct *vma, *prev;

	/* 页对齐后先查现有覆盖，再尝试扩张前驱。 */
	addr &= PAGE_MASK;
	vma = find_vma_prev(mm, addr, &prev);
	if (vma && (vma->vm_start <= addr))
		return vma;
	if (!prev)
		return NULL;
	/* 扩张失败不改变返回 ownership；成功的 prev 仍只在锁内借用。 */
	if (expand_stack_locked(prev, addr))
		return NULL;
	if (prev->vm_flags & VM_LOCKED)
		populate_vma_page_range(prev, addr, prev->vm_end, NULL);
	return prev;
}
#else
/* 向下增长配置：持 mmap 写锁把 address 纳入 vma，返回 expand_downwards 的 errno。 */
int expand_stack_locked(struct vm_area_struct *vma, unsigned long address)
{
	return expand_downwards(vma, address);
}

/*
 * 持 mmap 写锁查找包含 addr 或其后一 VMA；未包含时尝试把后一 GROWSDOWN 栈向下扩。
 * 返回锁期借用 VMA，失败返回 NULL；锁页 VMA 对新增前缀同步 populate。
 */
struct vm_area_struct *find_extend_vma_locked(struct mm_struct *mm, unsigned long addr)
{
	/* 查包含/后一项；只有后一项可向下扩张，VM_LOCKED 成功后预 fault 新前缀。 */
	struct vm_area_struct *vma;
	unsigned long start;

	/* 页对齐后查包含/后一项；没有后一项即无可向下扩张的栈。 */
	addr &= PAGE_MASK;
	vma = find_vma(mm, addr);
	if (!vma)
		return NULL;
	if (vma->vm_start <= addr)
		return vma;
	start = vma->vm_start;
	/* 保存旧 start 供锁页范围使用，扩张失败直接返回 NULL。 */
	if (expand_stack_locked(vma, addr))
		return NULL;
	if (vma->vm_flags & VM_LOCKED)
		populate_vma_page_range(vma, addr, start, NULL);
	return vma;
}
#endif

#if defined(CONFIG_STACK_GROWSUP)

/* 配置宏把不支持的增长方向固定为 -EFAULT，使公共升级逻辑无需条件分支。 */
#define vma_expand_up(vma,addr) expand_upwards(vma, addr)
#define vma_expand_down(vma, addr) (-EFAULT)

#else

/* 向下增长配置镜像上述分派。 */
#define vma_expand_up(vma,addr) (-EFAULT)
#define vma_expand_down(vma, addr) expand_downwards(vma, addr)

#endif

/*
 * expand_stack(): legacy interface for page faulting. Don't use unless
 * you have to.
 *
 * This is called with the mm locked for reading, drops the lock, takes
 * the lock for writing, tries to look up a vma again, expands it if
 * necessary, and downgrades the lock to reading again.
 *
 * If no vma is found or it can't be expanded, it returns NULL and has
 * dropped the lock.
 */
/*
 * page fault 兼容接口在入口持读锁：先释放读锁、可中断取得写锁并重新查树，按配置
 * 尝试向上前驱或向下当前 VMA。成功把写锁降级回读锁并返回借用 VMA；失败返回 NULL
 * 且不持任何 mmap 锁。重新查找防止锁升级窗口内 VMA 被并发改变。
 */
struct vm_area_struct *expand_stack(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma, *prev;

	/* 锁升级不是原子的：释放读锁后取得写锁，故必须重新查询 VMA。 */
	mmap_read_unlock(mm);
	if (mmap_write_lock_killable(mm))
		return NULL;

	/* 当前包含项优先；否则按配置尝试前驱向上或当前项向下。 */
	vma = find_vma_prev(mm, addr, &prev);
	if (vma && vma->vm_start <= addr)
		goto success;

	if (prev && !vma_expand_up(prev, addr)) {
		vma = prev;
		goto success;
	}

	if (vma && !vma_expand_down(vma, addr))
		goto success;

	/* 两个方向都失败：释放写锁，遵守 NULL 时无锁的后置条件。 */
	mmap_write_unlock(mm);
	return NULL;

success:
	/* 成功把写锁降级为读锁，返回 VMA 可在该读锁下继续使用。 */
	mmap_write_downgrade(mm);
	return vma;
}

/* do_munmap() - Wrapper function for non-maple tree aware do_munmap() calls.
 * @mm: The mm_struct
 * @start: The start address to munmap
 * @len: The length to be munmapped.
 * @uf: The userfaultfd list_head
 *
 * Return: 0 on success, error otherwise.
 */
/*
 * 中文契约：把旧式 mm/start/len 调用包装成从 start 定位的 VMA iterator，再交给
 * do_vmi_munmap。mm 与 uf 都是借用；调用者持 mmap 写锁，函数不代为解锁。成功
 * 返回 0 并完成范围拆除，失败返回 errno，userfaultfd 事件仍由上层最终完成。
 */
int do_munmap(struct mm_struct *mm, unsigned long start, size_t len,
	      struct list_head *uf)
{
	VMA_ITERATOR(vmi, mm, start);

	return do_vmi_munmap(&vmi, mm, start, len, uf, false);
}

int vm_munmap(unsigned long start, size_t len)
{
	/* 内核调用包装让 __vm_munmap 自行加锁，但不使用系统调用式地址解标签。 */
	return __vm_munmap(start, len, false);
}
EXPORT_SYMBOL(vm_munmap);

SYSCALL_DEFINE2(munmap, unsigned long, addr, size_t, len)
{
	/* 用户地址先移除体系结构 tag，再由 __vm_munmap 获取写锁并完成 userfaultfd。 */
	addr = untagged_addr(addr);
	return __vm_munmap(addr, len, true);
}


/*
 * Emulation of deprecated remap_file_pages() syscall.
 */
/*
 * 兼容旧 remap_file_pages ABI：只接受共享文件 VMA和 prot=0，先在读锁下取得 file
 * 引用/权限快照，锁外做 LSM，再在写锁下逐项重验证连续 VMA，最终用 MAP_FIXED 的
 * do_mmap 重建页偏移。成功归一化为 0；所有失败归还 file 并释放锁，populate 在锁外做。
 */
SYSCALL_DEFINE5(remap_file_pages, unsigned long, start, unsigned long, size,
		unsigned long, prot, unsigned long, pgoff, unsigned long, flags)
{
	/* populate 是 do_mmap 输出；ret 默认 EINVAL，file 引用跨越 LSM 与写锁重验证。 */
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long populate = 0;
	unsigned long ret = -EINVAL;
	struct file *file;
	vm_flags_t vm_flags;

	/* 每系统仅警告一次旧 ABI 使用，不影响本次兼容执行。 */
	pr_warn_once("%s (%d) uses deprecated remap_file_pages() syscall. See Documentation/mm/remap_file_pages.rst.\n",
		     current->comm, current->pid);

	/* 旧接口不允许调用者改变保护；地址/长度向下页对齐并拒绝空或回绕区间。 */
	if (prot)
		return ret;
	start = start & PAGE_MASK;
	size = size & PAGE_MASK;

	if (start + size <= start)
		return ret;

	/* Does pgoff wrap? */
	/* 文件页偏移加映射页数不得回绕。 */
	if (pgoff + (size >> PAGE_SHIFT) < pgoff)
		return ret;

	if (mmap_read_lock_killable(mm))
		return -EINTR;

	/*
	 * Look up VMA under read lock first so we can perform the security
	 * without holding locks (which can be problematic). We reacquire a
	 * write lock later and check nothing changed underneath us.
	 */
	/* 读锁只用于稳定初始 VMA并取得 file 引用；LSM 可能睡眠，必须在解锁后调用。 */
	vma = vma_lookup(mm, start);

	if (!vma || !(vma->vm_flags & VM_SHARED)) {
		mmap_read_unlock(mm);
		return -EINVAL;
	}

	/* 从原共享 VMA 派生 prot，并把允许的 NONBLOCK 与固定/共享/populate 组合。 */
	prot |= vma->vm_flags & VM_READ ? PROT_READ : 0;
	prot |= vma->vm_flags & VM_WRITE ? PROT_WRITE : 0;
	prot |= vma->vm_flags & VM_EXEC ? PROT_EXEC : 0;

	flags &= MAP_NONBLOCK;
	flags |= MAP_SHARED | MAP_FIXED | MAP_POPULATE;
	if (vma->vm_flags & VM_LOCKED)
		flags |= MAP_LOCKED;

	/* Save vm_flags used to calculate prot and flags, and recheck later. */
	/* 保存 flags/file 快照跨越解锁窗口；写锁阶段必须逐项重验防 TOCTOU。 */
	vm_flags = vma->vm_flags;
	file = get_file(vma->vm_file);

	mmap_read_unlock(mm);

	/* Call outside mmap_lock to be consistent with other callers. */
	/* LSM 拒绝时只需 fput；尚未修改任何 VMA。 */
	ret = security_mmap_file(file, prot, flags);
	if (ret) {
		fput(file);
		return ret;
	}

	ret = -EINVAL;

	/* OK security check passed, take write lock + let it rip. */
	/* 安全检查通过后取得可中断写锁，重新定位 start 对应 VMA。 */
	if (mmap_write_lock_killable(mm)) {
		fput(file);
		return -EINTR;
	}

	vma = vma_lookup(mm, start);

	if (!vma)
		goto out;

	/* Make sure things didn't change under us. */
	/* flags 或 file 任一变化都使此前权限结论失效，统一走 out。 */
	if (vma->vm_flags != vm_flags)
		goto out;
	if (vma->vm_file != file)
		goto out;

	/* 跨多 VMA 时必须逐段证明同一文件/flags 且无空洞。 */
	if (start + size > vma->vm_end) {
		VMA_ITERATOR(vmi, mm, vma->vm_end);
		struct vm_area_struct *next, *prev = vma;

		for_each_vma_range(vmi, next, start + size) {
			/* hole between vmas ? */
			/* 多 VMA 覆盖必须地址连续且 file/flags 完全相同，不能跨空洞或语义边界。 */
			if (next->vm_start != prev->vm_end)
				goto out;

			if (next->vm_file != vma->vm_file)
				goto out;

			if (next->vm_flags != vma->vm_flags)
				goto out;

			if (start + size <= next->vm_end)
				break;

			prev = next;
		}

		if (!next)
			goto out;
	}

	/* 重验证成功后 fixed 重映射指定 pgoff；do_mmap 可能产生待预取字节数。 */
	ret = do_mmap(vma->vm_file, start, size,
			prot, flags, 0, pgoff, &populate, NULL);
out:
	/* 写锁与临时 file 引用统一释放；成功映射的 populate 在锁外执行。 */
	mmap_write_unlock(mm);
	fput(file);
	if (populate)
		mm_populate(ret, populate);
	if (!IS_ERR_VALUE(ret))
		ret = 0;
	return ret;
}

/*
 * 内核 brk 映射入口：addr/request 是字节范围，is_exec 选择执行 VMA。函数自行取得
 * current mm 写锁，先移除重叠区再建立匿名 VMA；返回 0 或 errno。成功后完成 uf，
 * VM_LOCKED 时锁外 populate；失败解锁并保留对应下层返回值。
 */
int vm_brk_flags(unsigned long addr, unsigned long request, bool is_exec)
{
	/* 内核 brk helper：addr/request 为字节范围，is_exec 仅决定新 VMA 的 EXEC 标志。 */
	const vma_flags_t vma_flags = is_exec ?
		mk_vma_flags(VMA_EXEC_BIT) : EMPTY_VMA_FLAGS;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = NULL;
	unsigned long len;
	int ret;
	bool populate;
	LIST_HEAD(uf);
	VMA_ITERATOR(vmi, mm, addr);

	/* 对齐溢出返回 -ENOMEM，零长度是无状态成功。 */
	len = PAGE_ALIGN(request);
	if (len < request)
		return -ENOMEM;
	if (!len)
		return 0;

	if (mmap_write_lock_killable(mm))
		return -EINTR;

	/* 写锁内先核对范围/锁页额度，再清除重叠映射并在同一 iterator 位置建 brk。 */
	ret = check_brk_limits(addr, len);
	if (ret)
		goto limits_failed;

	/* munmap 可能生成 uf 事件；成功后 iterator 定位到可插入位置。 */
	ret = do_vmi_munmap(&vmi, mm, addr, len, &uf, 0);
	if (ret)
		goto munmap_failed;

	/* 前驱供 do_brk_flags 尝试合并，返回值决定是否可 populate。 */
	vma = vma_prev(&vmi);
	ret = do_brk_flags(&vmi, vma, addr, len, vma_flags);
	populate = ((mm->def_flags & VM_LOCKED) != 0);
	mmap_write_unlock(mm);
	userfaultfd_unmap_complete(mm, &uf);
	/* userfaultfd 完成与锁页 populate 均在解锁后，避免长时间占用 mmap_lock。 */
	if (populate && !ret)
		mm_populate(addr, len);
	return ret;

munmap_failed:
limits_failed:
	/* 两个前置失败都仍持写锁且没有待完成的成功 uf 事务。 */
	mmap_write_unlock(mm);
	return ret;
}

/*
 * 对已从外部可见树隔离的 [vma,end] 前缀逐项 close/free，返回 VM_ACCOUNT 页数供
 * 调用者统一 vm_unacct_memory。mm/vmi/vma 均借用，要求 mmap 写锁且范围连续有效。
 */
static
unsigned long tear_down_vmas(struct mm_struct *mm, struct vma_iterator *vmi,
		struct vm_area_struct *vma, unsigned long end)
{
	/* 调用者已使树不可达并持写锁；返回需归还的 VM_ACCOUNT 页数。 */
	unsigned long nr_accounted = 0;
	int count = 0;

	/* iterator 从首项尾端继续，count 最终应等于尚存 map_count。 */
	mmap_assert_write_locked(mm);
	vma_iter_set(vmi, vma->vm_end);
	/* 逐 VMA 累加承诺、标记 detached、执行 close/free，并允许调度防止长时间霸占 CPU。 */
	do {
		if (vma->vm_flags & VM_ACCOUNT)
			nr_accounted += vma_pages(vma);
		vma_mark_detached(vma);
		remove_vma(vma);
		count++;
		cond_resched();
		vma = vma_next(vmi);
	} while (vma && vma->vm_end <= end);

	VM_WARN_ON_ONCE(count != mm->map_count);
	return nr_accounted;
}

/* Release all mmaps. */
/*
 * 释放 mm 的全部映射：最后用户退出后先通知外部观察者并在读锁下拆页表，再设置
 * OOM_SKIP、切写锁、使 Maple Tree 对 RCU 读者不可达，最后关闭/释放 VMA 和归还
 * overcommit。mm 由调用者持有，函数不释放 mm 本身；完成后其 VMA 树已销毁。
 */
void exit_mmap(struct mm_struct *mm)
{
	struct mmu_gather tlb;
	struct vm_area_struct *vma;
	unsigned long nr_accounted = 0;
	VMA_ITERATOR(vmi, mm, 0);
	struct unmap_desc unmap;

	/* mm's last user has gone, and its about to be pulled down */
	/* mm 已无用户，先让 MMU notifier 停止二级页表等外部消费者。 */
	mmu_notifier_release(mm);

	mmap_read_lock(mm);
	arch_exit_mmap(mm);

	vma = vma_next(&vmi);
	if (!vma) {
		/* Can happen if dup_mmap() received an OOM */
		/* fork 失败可能留下空树；跳过页表遍历，改持写锁直接销毁元数据。 */
		mmap_read_unlock(mm);
		mmap_write_lock(mm);
		goto destroy;
	}

	unmap_all_init(&unmap, &vmi, vma);
	flush_cache_mm(mm);
	tlb_gather_mmu_fullmm(&tlb, mm);
	/* update_hiwater_rss(mm) here? but nobody should be looking */
	/* 最后用户已退出，无需再更新供用户观察的 RSS 高水位。 */
	/* Use ULONG_MAX here to ensure all VMAs in the mm are unmapped */
	/* ULONG_MAX 覆盖整棵树，确保不存在尾部 VMA 遗漏。 */
	unmap_vmas(&tlb, &unmap);
	mmap_read_unlock(mm);

	/*
	 * Set MMF_OOM_SKIP to hide this task from the oom killer/reaper
	 * because the memory has been already freed.
	 */
	/* 页表已拆除后设置 OOM_SKIP，防止 reaper 对同一 mm 重复回收。 */
	mm_flags_set(MMF_OOM_SKIP, mm);
	mmap_write_lock(mm);
	unmap.mm_wr_locked = true;
	mt_clear_in_rcu(&mm->mm_mt);
	unmap_pgtable_init(&unmap, &vmi);
	free_pgtables(&tlb, &unmap);
	tlb_finish_mmu(&tlb);

	/*
	 * Walk the list again, actually closing and freeing it, with preemption
	 * enabled, without holding any MM locks besides the unreachable
	 * mmap_write_lock.
	 */
	/* 树已对外不可达，写锁仅维持销毁不变量；启用抢占逐项执行 close/free。 */
	nr_accounted = tear_down_vmas(mm, &vmi, vma, ULONG_MAX);

destroy:
	__mt_destroy(&mm->mm_mt);
	trace_exit_mmap(mm);
	mmap_write_unlock(mm);
	vm_unacct_memory(nr_accounted);
}

/*
 * Return true if the calling process may expand its vm space by the passed
 * number of pages
 */
/*
 * 判断新增 npages 是否同时满足 RLIMIT_AS 和数据映射的 RLIMIT_DATA。mm/vma_flags
 * 只借用，页数单位不变；允许返回 true，不修改记账；拒绝返回 false。ignore_rlimit_data
 * 只放宽数据限额，Valgrind 的零软限额兼容分支仍受硬限额约束。
 */
bool may_expand_vm(struct mm_struct *mm, const vma_flags_t *vma_flags,
		   unsigned long npages)
{
	if (mm->total_vm + npages > rlimit(RLIMIT_AS) >> PAGE_SHIFT)
		return false;

	if (is_data_mapping_vma_flags(vma_flags) &&
	    mm->data_vm + npages > rlimit(RLIMIT_DATA) >> PAGE_SHIFT) {
		/* Workaround for Valgrind */
		/* 软 DATA 限额为零时，允许未越过 hard limit 的 Valgrind 式布局。 */
		if (rlimit(RLIMIT_DATA) == 0 &&
		    mm->data_vm + npages <= rlimit_max(RLIMIT_DATA) >> PAGE_SHIFT)
			return true;

		/* 首次越限记录进程、请求后字节数和软限额，提示可选启动参数。 */
		pr_warn_once("%s (%d): VmData %lu exceed data ulimit %lu. Update limits%s.\n",
			     current->comm, current->pid,
			     (mm->data_vm + npages) << PAGE_SHIFT,
			     rlimit(RLIMIT_DATA),
			     ignore_rlimit_data ? "" : " or use boot option ignore_rlimit_data");

		if (!ignore_rlimit_data)
			return false;
	}

	return true;
}

/*
 * 在 VMA 发布/撤销事务中按 npages（可正可负）更新 mm->total_vm，并按 flags 将同一
 * 数量归入 exec_vm、stack_vm 或 data_vm。mm 借用、无返回；调用者以 mmap 写锁或
 * 尚未发布 mm 保证子计数一致，WRITE_ONCE 只为 total_vm 的无锁观察提供单次发布。
 */
void vm_stat_account(struct mm_struct *mm, vm_flags_t flags, long npages)
{
	/* 在 VMA 发布/删除事务内增减总页数，并按互斥分类更新 exec/stack/data 子计数。 */
	WRITE_ONCE(mm->total_vm, READ_ONCE(mm->total_vm)+npages);

	if (is_exec_mapping(flags))
		mm->exec_vm += npages;
	else if (is_stack_mapping(flags))
		mm->stack_vm += npages;
	else if (is_data_mapping(flags))
		mm->data_vm += npages;
}

static vm_fault_t special_mapping_fault(struct vm_fault *vmf);

/*
 * Close hook, called for unmap() and on the old vma for mremap().
 *
 * Having a close hook prevents vma merging regardless of flags.
 */
/* 特殊 VMA 被 munmap 或旧 mremap 关闭时转发可选 close；vma/sm 均为借用。 */
static void special_mapping_close(struct vm_area_struct *vma)
{
	const struct vm_special_mapping *sm = vma->vm_private_data;

	if (sm->close)
		sm->close(sm, vma);
}

static const char *special_mapping_name(struct vm_area_struct *vma)
{
	/* /proc 等查询从稳定的 vm_private_data 返回静态/长期存活名称，不转移字符串 ownership。 */
	return ((struct vm_special_mapping *)vma->vm_private_data)->name;
}

static int special_mapping_mremap(struct vm_area_struct *new_vma)
{
	/* 只允许 current->mm 内的特殊 VMA 重映射，再转发可选回调；无回调视为成功。 */
	struct vm_special_mapping *sm = new_vma->vm_private_data;

	if (WARN_ON_ONCE(current->mm != new_vma->vm_mm))
		return -EFAULT;

	if (sm->mremap)
		return sm->mremap(sm, new_vma);

	return 0;
}

static int special_mapping_split(struct vm_area_struct *vma, unsigned long addr)
{
	/*
	 * Forbid splitting special mappings - kernel has expectations over
	 * the number of pages in mapping. Together with VM_DONTEXPAND
	 * the size of vma should stay the same over the special mapping's
	 * lifetime.
	 */
	/* 特殊映射的页数组与长度有内核约定，任何 split 都返回 -EINVAL 保持一体。 */
	return -EINVAL;
}

static const struct vm_operations_struct special_mapping_vmops = {
	.close = special_mapping_close,
	.fault = special_mapping_fault,
	.mremap = special_mapping_mremap,
	.name = special_mapping_name,
	/* vDSO code relies that VVAR can't be accessed remotely */
	/* VVAR 等特殊页禁止 access 回调，远程访问必须失败而不能绕过其可见性约束。 */
	.access = NULL,
	.may_split = special_mapping_split,
};

static vm_fault_t special_mapping_fault(struct vm_fault *vmf)
{
	/* vmf/vma 借用；优先交给 spec fault，否则按 pgoff 在线性 page* 哨兵数组中定位。 */
	struct vm_area_struct *vma = vmf->vma;
	pgoff_t pgoff;
	struct page **pages;
	struct vm_special_mapping *sm = vma->vm_private_data;

	/* 自定义回调完全决定 fault 结果；静态页数组路径则为命中页取得一份引用。 */
	if (sm->fault)
		return sm->fault(sm, vmf->vma, vmf);

	/* pages 是 NULL 终止数组；每递减一次 pgoff 就前进一个页指针。 */
	pages = sm->pages;

	for (pgoff = vmf->pgoff; pgoff && *pages; ++pages)
		pgoff--;

	/* 找到页后 get_page 的引用交给 fault core；越过 NULL 哨兵返回 SIGBUS。 */
	if (*pages) {
		struct page *page = *pages;
		get_page(page);
		vmf->page = page;
		return 0;
	}

	return VM_FAULT_SIGBUS;
}

static struct vm_area_struct *__install_special_mapping(
	struct mm_struct *mm,
	unsigned long addr, unsigned long len,
	vm_flags_t vm_flags, void *priv,
	const struct vm_operations_struct *ops)
{
	/* 调用者持 mm 写锁；priv/ops 借用且须至少与 VMA 同寿命，返回拥有树中 VMA 或 ERR_PTR。 */
	int ret;
	struct vm_area_struct *vma;

	/* 先分配未发布 VMA，设置范围、不可扩展标志、保护和回调私有数据。 */
	vma = vm_area_alloc(mm);
	if (unlikely(vma == NULL))
		return ERR_PTR(-ENOMEM);

	/* 特殊映射 pgoff 从 0 开始，继承 mm 缺省但强制不可扩张。 */
	vma_set_range(vma, addr, addr + len, 0);
	vm_flags |= mm->def_flags | VM_DONTEXPAND;
	if (pgtable_supports_soft_dirty())
		vm_flags |= VM_SOFTDIRTY;
	vm_flags_init(vma, vm_flags & ~VM_LOCKED_MASK);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

	/* ops/priv 在发布前成对写入，fault/name/close 后续据此分派。 */
	vma->vm_ops = ops;
	vma->vm_private_data = priv;

	/* insert_vm_struct 是发布点；失败仅释放尚未发布的 VMA。 */
	ret = insert_vm_struct(mm, vma);
	if (ret)
		goto out;

	/* 发布成功后补记 mm 页数并通知 perf；返回指针由 mm 树持有。 */
	vm_stat_account(mm, vma->vm_flags, len >> PAGE_SHIFT);

	perf_event_mmap(vma);

	return vma;

out:
	/* insert 失败时 VMA 从未入树，直接释放对象即可，无统计或 perf 回滚。 */
	vm_area_free(vma);
	return ERR_PTR(ret);
}

bool vma_is_special_mapping(const struct vm_area_struct *vma,
	const struct vm_special_mapping *sm)
{
	/* 同时比较私有描述符和专用 vm_ops，避免仅指针碰巧相同造成误判。 */
	return vma->vm_private_data == sm &&
		vma->vm_ops == &special_mapping_vmops;
}

/*
 * Called with mm->mmap_lock held for writing.
 * Insert a new vma covering the given region, with the given flags.
 * Its pages are supplied by the given array of struct page *.
 * The array can be shorter than len >> PAGE_SHIFT if it's null-terminated.
 * The region past the last page supplied will always produce SIGBUS.
 * The array pointer and the pages it points to are assumed to stay alive
 * for as long as this mapping might exist.
 */
/*
 * 中文契约：持 mm 写锁安装由 spec 描述的特殊映射；addr/len 是字节区间，vm_flags
 * 为初始属性，spec 及其 pages/name/callback 必须比 VMA 活得更久。返回树中借用 VMA
 * 或 ERR_PTR；实际 fault 由 special_mapping_vmops 分派，数组尾后的访问产生 SIGBUS。
 */
struct vm_area_struct *_install_special_mapping(
	struct mm_struct *mm,
	unsigned long addr, unsigned long len,
	vm_flags_t vm_flags, const struct vm_special_mapping *spec)
{
	/* 公共包装固定使用 special_mapping_vmops，并把长期 spec 作为私有描述符。 */
	return __install_special_mapping(mm, addr, len, vm_flags, (void *)spec,
					&special_mapping_vmops);
}

#ifdef CONFIG_SYSCTL
#if defined(HAVE_ARCH_PICK_MMAP_LAYOUT) || \
		defined(CONFIG_ARCH_WANT_DEFAULT_TOPDOWN_MMAP_LAYOUT)
/* 0/1 sysctl 选择现代随机布局或兼容 legacy 布局，仅相关体系结构导出。 */
int sysctl_legacy_va_layout;
#endif

/*
 * 每个 ctl_table 项依次给出 proc 路径、被发布整数、宽度、权限、minmax 处理器和
 * extra1/extra2 边界；表为启动期静态只读描述，register_sysctl_init 不取得动态 ownership。
 */
static const struct ctl_table mmap_table[] = {
		{
				/* max_map_count：0644，非负，限制单 mm 的 VMA 数量。 */
				.procname       = "max_map_count",
				.data           = &sysctl_max_map_count,
				.maxlen         = sizeof(sysctl_max_map_count),
				.mode           = 0644,
				.proc_handler   = proc_dointvec_minmax,
				.extra1         = SYSCTL_ZERO,
		},
#if defined(HAVE_ARCH_PICK_MMAP_LAYOUT) || \
		defined(CONFIG_ARCH_WANT_DEFAULT_TOPDOWN_MMAP_LAYOUT)
		{
				/* legacy_va_layout：0644，非负布尔式布局选择。 */
				.procname       = "legacy_va_layout",
				.data           = &sysctl_legacy_va_layout,
				.maxlen         = sizeof(sysctl_legacy_va_layout),
				.mode           = 0644,
				.proc_handler   = proc_dointvec_minmax,
				.extra1         = SYSCTL_ZERO,
		},
#endif
#ifdef CONFIG_HAVE_ARCH_MMAP_RND_BITS
		{
				/* 原生 ASLR 位数：0600，仅管理员可写并受 arch min/max 夹限。 */
				.procname       = "mmap_rnd_bits",
				.data           = &mmap_rnd_bits,
				.maxlen         = sizeof(mmap_rnd_bits),
				.mode           = 0600,
				.proc_handler   = proc_dointvec_minmax,
				.extra1         = (void *)&mmap_rnd_bits_min,
				.extra2         = (void *)&mmap_rnd_bits_max,
		},
#endif
#ifdef CONFIG_HAVE_ARCH_MMAP_RND_COMPAT_BITS
		{
				/* compat ASLR 位数使用独立 arch 上下界，避免越过 32 位地址能力。 */
				.procname       = "mmap_rnd_compat_bits",
				.data           = &mmap_rnd_compat_bits,
				.maxlen         = sizeof(mmap_rnd_compat_bits),
				.mode           = 0600,
				.proc_handler   = proc_dointvec_minmax,
				.extra1         = (void *)&mmap_rnd_compat_bits_min,
				.extra2         = (void *)&mmap_rnd_compat_bits_max,
		},
#endif
};
#endif /* CONFIG_SYSCTL */

/*
 * initialise the percpu counter for VM, initialise VMA state.
 */
/* 启动期初始化全局 committed-as 计数、可选 vm sysctl 表和 VMA 子系统；失败属内核 BUG。 */
void __init mmap_init(void)
{
	int ret;

	ret = percpu_counter_init(&vm_committed_as, 0, GFP_KERNEL);
	VM_BUG_ON(ret);
#ifdef CONFIG_SYSCTL
	register_sysctl_init("vm", mmap_table);
#endif
	vma_state_init();
}

/*
 * Initialise sysctl_user_reserve_kbytes.
 *
 * This is intended to prevent a user from starting a single memory hogging
 * process, such that they cannot recover (kill the hog) in OVERCOMMIT_NEVER
 * mode.
 *
 * The default value is min(3% of free memory, 128MB)
 * 128MB is enough to recover with sshd/login, bash, and top/kill.
 */
/*
 * subsys init 根据当前空闲页计算普通用户恢复预留：free/32（约 3%）与 128 MiB
 * 上限取小值，写入 KiB sysctl 后返回 0。无动态资源和失败分支，热插拔 notifier 会复用。
 */
static int init_user_reserve(void)
{
	unsigned long free_kbytes;

	free_kbytes = K(global_zone_page_state(NR_FREE_PAGES));

	sysctl_user_reserve_kbytes = min(free_kbytes / 32, SZ_128K);
	return 0;
}
subsys_initcall(init_user_reserve);

/*
 * Initialise sysctl_admin_reserve_kbytes.
 *
 * The purpose of sysctl_admin_reserve_kbytes is to allow the sys admin
 * to log in and kill a memory hogging process.
 *
 * Systems with more than 256MB will reserve 8MB, enough to recover
 * with sshd, bash, and top in OVERCOMMIT_GUESS. Smaller systems will
 * only reserve 3% of free pages by default.
 */
/*
 * subsys init 计算管理员恢复预留：空闲内存约 3% 与 8 MiB 上限取小值，单位 KiB。
 * 发布到 sysctl 后返回 0；保证管理员仍有登录并终止内存失控进程的余量。
 */
static int init_admin_reserve(void)
{
	unsigned long free_kbytes;

	free_kbytes = K(global_zone_page_state(NR_FREE_PAGES));

	sysctl_admin_reserve_kbytes = min(free_kbytes / 32, SZ_8K);
	return 0;
}
subsys_initcall(init_admin_reserve);

/*
 * Reinititalise user and admin reserves if memory is added or removed.
 *
 * The default user reserve max is 128MB, and the default max for the
 * admin reserve is 8MB. These are usually, but not always, enough to
 * enable recovery from a memory hogging process using login/sshd, a shell,
 * and tools like top. It may make sense to increase or even disable the
 * reserve depending on the existence of swap or variations in the recovery
 * tools. So, the admin may have changed them.
 *
 * If memory is added and the reserves have been eliminated or increased above
 * the default max, then we'll trust the admin.
 *
 * If memory is removed and there isn't enough free memory, then we
 * need to reset the reserves.
 *
 * Otherwise keep the reserve set by the admin.
 */
/*
 * memory hotplug notifier：MEM_ONLINE 仅重算仍处于缺省范围的值，尊重管理员清零或
 * 调高；MEM_OFFLINE 在预留超过现有空闲 KiB 时收缩并记录日志；其他 action 无操作。
 * nb/data 未使用且只借用，回调总返回 NOTIFY_OK，不阻止热插拔事务。
 */
static int reserve_mem_notifier(struct notifier_block *nb,
			     unsigned long action, void *data)
{
	unsigned long tmp, free_kbytes;

	switch (action) {
	case MEM_ONLINE:
		/* Default max is 128MB. Leave alone if modified by operator. */
		/* 普通用户值在 (0,128MiB) 内仍视作自动值，按新增内存重算。 */
		tmp = sysctl_user_reserve_kbytes;
		if (tmp > 0 && tmp < SZ_128K)
			init_user_reserve();

		/* Default max is 8MB.  Leave alone if modified by operator. */
		/* 管理员值在 (0,8MiB) 内重算；0 或超上限视为显式配置。 */
		tmp = sysctl_admin_reserve_kbytes;
		if (tmp > 0 && tmp < SZ_8K)
			init_admin_reserve();

		break;
	case MEM_OFFLINE:
		/* 下线后只在预留已大于全部空闲内存时强制收敛，避免不可兑现。 */
		free_kbytes = K(global_zone_page_state(NR_FREE_PAGES));

		/* 两类预留分别比较并重算，互不覆盖管理员对另一项的设置。 */
		if (sysctl_user_reserve_kbytes > free_kbytes) {
			init_user_reserve();
			pr_info("vm.user_reserve_kbytes reset to %lu\n",
				sysctl_user_reserve_kbytes);
		}

		if (sysctl_admin_reserve_kbytes > free_kbytes) {
			init_admin_reserve();
			pr_info("vm.admin_reserve_kbytes reset to %lu\n",
				sysctl_admin_reserve_kbytes);
		}
		break;
	default:
		/* notifier 链的其他阶段不改变 sysctl。 */
		break;
	}
	return NOTIFY_OK;
}

static int __meminit init_reserve_notifier(void)
{
	/* 启动期注册默认优先级回调；注册失败只告警，不阻止启动。 */
	if (hotplug_memory_notifier(reserve_mem_notifier, DEFAULT_CALLBACK_PRI))
		pr_err("Failed registering memory add/remove notifier for admin reserve\n");

	/* notifier 注册失败不影响 mmap 核心初始化，保留启动可用性。 */
	return 0;
}
subsys_initcall(init_reserve_notifier);

/*
 * Obtain a read lock on mm->mmap_lock, if the specified address is below the
 * start of the VMA, the intent is to perform a write, and it is a
 * downward-growing stack, then attempt to expand the stack to contain it.
 *
 * This function is intended only for obtaining an argument page from an ELF
 * image, and is almost certainly NOT what you want to use for any other
 * purpose.
 *
 * IMPORTANT - VMA fields are accessed without an mmap lock being held, so the
 * VMA referenced must not be linked in any user-visible tree, i.e. it must be a
 * new VMA being mapped.
 *
 * The function assumes that addr is either contained within the VMA or below
 * it, and makes no attempt to validate this value beyond that.
 *
 * Returns true if the read lock was obtained and a stack was perhaps expanded,
 * false if the stack expansion failed.
 *
 * On stack expansion the function temporarily acquires an mmap write lock
 * before downgrading it.
 */
/*
 * ELF 参数页专用锁 helper：new_vma 尚未发布，故可在无锁时读其 start/flags。普通读
 * 或地址已在范围内直接取读锁；向下增长写访问先取写锁扩栈，再降级为读锁。成功
 * 返回 true 且持读锁，失败返回 false 且不持锁；其他已发布 VMA 禁止调用。
 */
bool mmap_read_lock_maybe_expand(struct mm_struct *mm,
				 struct vm_area_struct *new_vma,
				 unsigned long addr, bool write)
{
	/* 无写意图或地址未落在起点下方，不需要改变 VMA 边界。 */
	if (!write || addr >= new_vma->vm_start) {
		mmap_read_lock(mm);
		return true;
	}

	/* 只有 GROWSDOWN 新 VMA 能为起点下方参数地址扩张。 */
	if (!(new_vma->vm_flags & VM_GROWSDOWN))
		return false;

	/* 写锁内扩张；成功降级保持调用者期望的读锁后置条件。 */
	mmap_write_lock(mm);
	if (expand_downwards(new_vma, addr)) {
		mmap_write_unlock(mm);
		return false;
	}

	mmap_write_downgrade(mm);
	return true;
}

__latent_entropy int dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	/*
	 * fork 的 mm 复制事务：oldmm 是已发布父地址空间，mm 是尚未发布的子对象。函数
	 * 先锁父再嵌套锁子，复制 Maple Tree 骨架并逐 VMA 替换为独立对象、策略、anon_vma、
	 * file 反向映射和页表。成功返回 0、两锁均释放并完成 userfaultfd；失败返回 errno，
	 * 只清理已初始化前缀、撤销承诺记账并标记子 mm 不稳定，调用者随后销毁整个 mm。
	 */
	struct vm_area_struct *mpnt, *tmp;
	int retval;
	unsigned long charge = 0;
	LIST_HEAD(uf);
	VMA_ITERATOR(vmi, mm, 0);

	/* 父写锁阻止复制期间 VMA 拓扑变化；flush/uprobes 在子发布前建立复制上下文。 */
	if (mmap_write_lock_killable(oldmm))
		return -EINTR;
	flush_cache_dup_mm(oldmm);
	uprobe_dup_mmap(oldmm, mm);
	/*
	 * Not linked in yet - no deadlock potential:
	 */
	/* 子 mm 尚不可见，不存在反向锁依赖；用嵌套类别记录父→子锁序。 */
	mmap_write_lock_nested(mm, SINGLE_DEPTH_NESTING);

	/* No ordering required: file already has been exposed. */
	/* exe_file 早已发布，只需复制其引用，无额外发布屏障。 */
	dup_mm_exe_file(mm, oldmm);

	/* 先复制四类页数快照；VM_DONTCOPY 分支稍后从子计数中扣除。 */
	mm->total_vm = oldmm->total_vm;
	mm->data_vm = oldmm->data_vm;
	mm->exec_vm = oldmm->exec_vm;
	mm->stack_vm = oldmm->stack_vm;

	/* Use __mt_dup() to efficiently build an identical maple tree. */
	/* 批量复制树节点只是地址索引骨架，叶中的父 VMA 指针必须逐项替换。 */
	retval = __mt_dup(&oldmm->mm_mt, &mm->mm_mt, GFP_KERNEL);
	if (unlikely(retval))
		goto out;

	/* 子树未完整前清除 RCU 可见标志；循环成功结束才重新发布。 */
	mt_clear_in_rcu(vmi.mas.tree);
	for_each_vma(vmi, mpnt) {
		struct file *file;

		/* 稳定父 VMA 写侧；DONTCOPY 项从子树清除并修正预复制的统计。 */
		retval = vma_start_write_killable(mpnt);
		if (retval < 0)
			goto loop_out;
		if (mpnt->vm_flags & VM_DONTCOPY) {
			/* 清除骨架叶可能分配/失败；成功后该父 VMA 不在子 mm 出现。 */
			retval = vma_iter_clear_gfp(&vmi, mpnt->vm_start,
						    mpnt->vm_end, GFP_KERNEL);
			if (retval)
				goto loop_out;

			vm_stat_account(mm, mpnt->vm_flags, -vma_pages(mpnt));
			continue;
		}
		/* VM_ACCOUNT 在分配子 VMA 前先取得 overcommit 承诺，失败由 fail_nomem 归还。 */
		charge = 0;
		if (mpnt->vm_flags & VM_ACCOUNT) {
			unsigned long len = vma_pages(mpnt);

			if (security_vm_enough_memory_mm(oldmm, len)) /* sic */
				/* 保留上游语义：承诺核算以 oldmm 为上下文，失败转统一 ENOMEM。 */
				goto fail_nomem;
			charge = len;
		}

		/* 复制 VMA 本体后依次复制 policy、绑定子 mm、登记 userfaultfd 与 anon_vma。 */
		tmp = vm_area_dup(mpnt);
		if (!tmp)
			goto fail_nomem;
		/* policy 复制失败仅拥有 tmp；成功后后续标签负责先 put policy。 */
		retval = vma_dup_policy(mpnt, tmp);
		if (retval)
			goto fail_nomem_policy;
		tmp->vm_mm = mm;
		retval = dup_userfaultfd(tmp, &uf);
		if (retval)
			goto fail_nomem_anon_vma_fork;
		if (tmp->vm_flags & VM_WIPEONFORK) {
			/*
			 * VM_WIPEONFORK gets a clean slate in the child.
			 * Don't prepare anon_vma until fault since we don't
			 * copy page for current vma.
			 */
			/* WIPEONFORK 子 VMA 从空内容开始，首次 fault 再延迟创建 anon_vma。 */
			tmp->anon_vma = NULL;
		} else if (anon_vma_fork(tmp, mpnt))
			goto fail_nomem_anon_vma_fork;
		/* fork 不继承实际锁页状态，避免子进程无条件消耗父的 memlock 承诺。 */
		vm_flags_clear(tmp, VM_LOCKED_MASK);
		/*
		 * Copy/update hugetlb private vma information.
		 */
		/* hugetlb 私有元数据含预留/所有权状态，需在发布子 VMA 前专门复制。 */
		if (is_vm_hugetlb_page(tmp))
			hugetlb_dup_vma_private(tmp);

		/*
		 * Link the vma into the MT. After using __mt_dup(), memory
		 * allocation is not necessary here, so it cannot fail.
		 */
		/* 以 tmp 替换预复制叶是发布到子树的单项步骤，不再分配 Maple 节点。 */
		vma_iter_bulk_store(&vmi, tmp);

		/* 从此 tmp 纳入子 map_count；随后 open/file 反向映射和页表仍可能失败。 */
		mm->map_count++;

		/* VMA open 回调建立后端私有引用；文件映射另取 file 引用并链接 i_mmap。 */
		if (tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);

		file = tmp->vm_file;
		if (file) {
			struct address_space *mapping = file->f_mapping;

			get_file(file);
			i_mmap_lock_write(mapping);
			if (vma_is_shared_maywrite(tmp))
				mapping_allow_writable(mapping);
			flush_dcache_mmap_lock(mapping);
			/* insert tmp into the share list, just after mpnt */
			/* 在 mapping 写锁下把子 VMA 插到父后，保持共享文件区间树顺序。 */
			vma_interval_tree_insert_after(tmp, mpnt,
					&mapping->i_mmap);
			flush_dcache_mmap_unlock(mapping);
			i_mmap_unlock_write(mapping);
		}

		/* 普通 VMA 复制页表/COW 引用；WIPEONFORK 故意保留空页表。 */
		if (!(tmp->vm_flags & VM_WIPEONFORK))
			retval = copy_page_range(tmp, mpnt);

		if (retval) {
			mpnt = vma_next(&vmi);
			goto loop_out;
		}
	}
	/* a new mm has just been created */
	/* 所有 VMA 完成后让体系结构复制 mm 上下文；成功才发布 RCU 树和子系统状态。 */
	retval = arch_dup_mmap(oldmm, mm);
loop_out:
	vma_iter_free(&vmi);
	if (!retval) {
		/* 完整树现在可供 RCU 路径观察，再通知 KSM 与 khugepaged 接管子 mm。 */
		mt_set_in_rcu(vmi.mas.tree);
		ksm_fork(mm, oldmm);
		khugepaged_fork(mm, oldmm);
	} else {
		unsigned long end;

		/*
		 * The entire maple tree has already been duplicated, but
		 * replacing the vmas failed at mpnt (which could be NULL if
		 * all were allocated but the last vma was not fully set up).
		 * Use the start address of the failure point to clean up the
		 * partially initialized tree.
		 */
		/* 失败点之前是可正常拆除的子 VMA，之后仍可能是指向父对象的未替换骨架。 */
		if (!mm->map_count) {
			/* zero vmas were written to the new tree. */
			/* 尚无子 VMA 发布，无需走 unmap/close。 */
			end = 0;
		} else if (mpnt) {
			/* partial tree failure */
			/* mpnt 起点是第一个未完成对象，只清理其前缀。 */
			end = mpnt->vm_start;
		} else {
			/* All vmas were written to the new tree */
			/* 最后一项后失败时，整个地址范围都是已初始化子 VMA。 */
			end = ULONG_MAX;
		}

		/* Hide mm from oom killer because the memory is being freed */
		/* 清理开始前设置 OOM_SKIP，避免 reaper 并发处理未发布子 mm。 */
		mm_flags_set(MMF_OOM_SKIP, mm);
		if (end) {
			vma_iter_set(&vmi, 0);
			tmp = vma_next(&vmi);
			UNMAP_STATE(unmap, &vmi, /* first = */ tmp,
				    /* vma_start = */ 0, /* vma_end = */ end,
				    /* prev = */ NULL, /* next = */ NULL);

			/*
			 * Don't iterate over vmas beyond the failure point for
			 * both unmap_vma() and free_pgtables().
			 */
			/* tree_end 把页表与 VMA 清理严格限制在已初始化前缀，不能触碰父指针叶。 */
			unmap.tree_end = end;
			flush_cache_mm(mm);
			unmap_region(&unmap);
			charge = tear_down_vmas(mm, &vmi, tmp, end);
			vm_unacct_memory(charge);
		}
		__mt_destroy(&mm->mm_mt);
		/*
		 * The mm_struct is going to exit, but the locks will be dropped
		 * first.  Set the mm_struct as unstable is advisable as it is
		 * not fully initialised.
		 */
		/* 锁即将释放但子 mm 仍由 fork 错误路径持有，UNSTABLE 告知观察者不可使用。 */
		mm_flags_set(MMF_UNSTABLE, mm);
	}
out:
	/* 统一按子→父逆序解锁；成功完成 uf，失败取消 uf，返回原始 errno。 */
	mmap_write_unlock(mm);
	flush_tlb_mm(oldmm);
	mmap_write_unlock(oldmm);
	if (!retval)
		dup_userfaultfd_complete(&uf);
	else
		dup_userfaultfd_fail(&uf);
	return retval;

fail_nomem_anon_vma_fork:
	/* tmp 尚未发布：按 policy→VMA→承诺的逆序释放，再进入前缀清理。 */
	mpol_put(vma_policy(tmp));
fail_nomem_policy:
	vm_area_free(tmp);
fail_nomem:
	retval = -ENOMEM;
	vm_unacct_memory(charge);
	goto loop_out;
}
