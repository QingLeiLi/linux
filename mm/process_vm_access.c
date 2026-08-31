// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * linux/mm/process_vm_access.c
 *
 * Copyright (C) 2010-2011 Christopher Yeoh <cyeoh@au1.ibm.com>, IBM Corp.
 */

/*
 * process_vm_readv()/writev() 在一次系统调用中连接“当前进程的本地 iovec”与“目标
 * 进程的远端 iovec”。本文件先导入并验证用户描述符，再经 ptrace 权限取得目标 mm，
 * 分批 pin 远端页并用 iov_iter 拷贝；已复制字节优先于后续错误返回，形成部分成功 ABI。
 */

#include <linux/compat.h>
#include <linux/mm.h>
#include <linux/uio.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/syscalls.h>

/**
 * process_vm_rw_pages - read/write pages from task specified
 * @pages: array of pointers to pages we want to copy
 * @offset: offset in page to start copying from/to
 * @len: number of bytes to copy
 * @iter: where to copy to/from locally
 * @vm_write: 0 means copy from, 1 means copy to
 * Returns 0 on success, error code otherwise
 */
/*
 * 在一批已 pin 的远端页与当前调用者的本地 iov_iter 之间执行实际字节复制。
 * 业务背景：process_vm_rw_single_vec() 负责地址翻页和 pin，本函数只处理页内偏移与
 * iterator 推进，是 readv/writev 数据真正跨进程移动的位置。
 * 入参：@pages 是借用的已 pin 页指针数组；@offset 是首页面内字节偏移，范围
 * 0..PAGE_SIZE-1；@len 是本批最多复制字节数；@iter 是输入输出本地 iterator，成功
 * 字节会推进它；@vm_write 为 0 时远端页→本地，为 1 时本地→远端页。
 * 出参/返回：全部请求或本地 iterator 耗尽返回 0；本地用户缓冲 fault 导致非预期短拷贝
 * 时返回 -EFAULT。无论返回值如何，iter 已保留实际推进量，不管理 pages pin ownership。
 * 注意事项：页必须在整个调用期间保持 pin；copy_page_*_iter 可能 fault/睡眠。错误不会
 * 回滚已复制字节，调用者据 iterator 差值实现部分成功语义。
 */
static int process_vm_rw_pages(struct page **pages,
			       unsigned offset,
			       size_t len,
			       struct iov_iter *iter,
			       int vm_write)
{
	/* Do the copy for each page */
	/* 逐页处理，直到本批 len 或本地 iterator 任一耗尽。 */
	while (len && iov_iter_count(iter)) {
		/* page 是当前借用页；copy 是本页可用上限，copied 是实际推进字节。 */
		struct page *page = *pages++;
		size_t copy = PAGE_SIZE - offset;
		size_t copied;

		/* 最后一页只复制远端向量尚需的尾部，不越过 @len。 */
		if (copy > len)
			copy = len;

		/* vm_write 从当前进程 iterator 取数据写远端页；read 方向相反。 */
		if (vm_write)
			copied = copy_page_from_iter(page, offset, copy, iter);
		else
			copied = copy_page_to_iter(page, offset, copy, iter);

		/* iterator helper 已推进实际 copied；相同数量从本批剩余量扣除。 */
		len -= copied;
		/* iterator 仍有目标却未完成本页，说明本地用户地址 fault，停止且不回滚。 */
		if (copied < copy && iov_iter_count(iter))
			return -EFAULT;
		/* 只有首个远端页可能非零偏移，后续页一律从页首开始。 */
		offset = 0;
	}
	return 0;
}

/* Maximum number of pages kmalloc'd to hold struct page's during copy */
/* 动态 page* 暂存数组最多占两页内核内存，限制单次系统调用的可靠性成本。 */
#define PVM_MAX_KMALLOC_PAGES 2

/* Maximum number of pages that can be stored at a time */
/* 由两页容量换算出的每批最大远端页数；大向量通过循环重复 pin/copy/unpin。 */
#define PVM_MAX_USER_PAGES (PVM_MAX_KMALLOC_PAGES * PAGE_SIZE / sizeof(struct page *))

/**
 * process_vm_rw_single_vec - read/write pages from task specified
 * @addr: start memory address of target process
 * @len: size of area to copy to/from
 * @iter: where to copy to/from locally
 * @process_pages: struct pages area that can store at least
 *  nr_pages_to_copy struct page pointers
 * @mm: mm for task
 * @task: task to read/write from
 * @vm_write: 0 means copy from, 1 means copy to
 * Returns 0 on success or on failure error code
 */
/*
 * 处理一个远端 iovec：按固定上限循环 pin 目标 mm 的页、复制并立即 unpin。
 * 业务背景：远端向量可能跨很多页，不能长期 pin 或为全部页无限分配指针数组；本函数
 * 位于权限已获准的 core 循环与逐页 copy helper 之间，限定资源峰值。
 * 入参：@addr/@len 是远端用户虚拟起点和字节长度；@iter 是输入输出的本地 iterator；
 * @process_pages 是容量至少 min(本向量页数, PVM_MAX_USER_PAGES) 的输出暂存数组；@mm
 * 是持有 mm_users 引用的目标地址空间；@task 是对应借用任务（当前实现不直接使用）；
 * @vm_write 为 1 时要求远端页可写，否则读取远端页。
 * 出参/返回：完整处理或本地 iterator 耗尽返回 0；无法 pin 或本地 copy fault 返回
 * -EFAULT。iter 保留已复制进度，每批所有成功 pin 的页均在返回前 unpin，写方向标脏。
 * 注意事项：可睡眠；每次先持 mmap_read_lock，GUP 可能通过 @locked=0 自行释放它，
 * 调用者只能在 locked 仍为 1 时解锁。远端 mm 生命周期由外层 mmput() 配对。
 */
static int process_vm_rw_single_vec(unsigned long addr,
				    unsigned long len,
				    struct iov_iter *iter,
				    struct page **process_pages,
				    struct mm_struct *mm,
				    struct task_struct *task,
				    int vm_write)
{
	/* pa 是当前页对齐远端地址；start_offset 只描述首批首页的页内偏移。 */
	unsigned long pa = addr & PAGE_MASK;
	unsigned long start_offset = addr - pa;
	/* nr_pages 是本向量尚待覆盖页数；rc 一旦非零就终止后续批次。 */
	unsigned long nr_pages;
	ssize_t rc = 0;
	/* flags 传给远端 GUP；写远端时补 FOLL_WRITE 触发权限/COW 语义。 */
	unsigned int flags = 0;

	/* Work out address and page range required */
	/* 空远端向量不访问 addr，也不推进本地 iterator，直接成功。 */
	if (len == 0)
		return 0;
	/* 首尾地址换算为包含式页数；后续每批至多 PVM_MAX_USER_PAGES。 */
	nr_pages = (addr + len - 1) / PAGE_SIZE - addr / PAGE_SIZE + 1;

	/* writev 必须以写意图 pin，确保只读 VMA 被拒绝并正确处理私有映射 COW。 */
	if (vm_write)
		flags |= FOLL_WRITE;

	/* 阶段 1：资源有界的 pin → copy → unpin 循环；本地长度耗尽也是正常结束。 */
	while (!rc && nr_pages && iov_iter_count(iter)) {
		/* pinned_pages 先是本批请求量，调用后改为实际 pin 数；locked 跟踪 mmap 锁。 */
		int pinned_pages = min_t(unsigned long, nr_pages, PVM_MAX_USER_PAGES);
		int locked = 1;
		size_t bytes;

		/*
		 * Get the pages we're interested in.  We must
		 * access remotely because task/mm might not
		 * current/current->mm
		 */
		/*
		 * 页属于 @task/@mm 而非 current/current->mm，必须使用 remote GUP。调用前
		 * 持 mmap 读锁；GUP 遇到可重试 fault 时可能释放锁并把 locked 清零。
		 */
		mmap_read_lock(mm);
		pinned_pages = pin_user_pages_remote(mm, pa, pinned_pages,
						     flags, process_pages,
						     &locked);
		/* 只有 GUP 没有代为释放时才由本层配对 unlock，避免双重解锁。 */
		if (locked)
			mmap_read_unlock(mm);
		/* API 的具体负 errno 在此统一折叠为 syscall 的远端地址 -EFAULT。 */
		if (pinned_pages <= 0)
			return -EFAULT;

		/* 计算本批从首偏移后可访问的字节，并裁到远端向量剩余 len。 */
		bytes = pinned_pages * PAGE_SIZE - start_offset;
		if (bytes > len)
			bytes = len;

		/* 阶段 2：页 pin 稳定物理 backing，实际 copy 推进本地 iterator。 */
		rc = process_vm_rw_pages(process_pages,
					 start_offset, bytes, iter,
					 vm_write);
		/*
		 * 地址游标按本批覆盖范围前移。即使 copy 中途 fault，rc 已阻止下一轮；
		 * 外层不靠这里的 len 计算部分成功，而是比较 iterator 前后 count。
		 */
		len -= bytes;
		start_offset = 0;
		nr_pages -= pinned_pages;
		pa += pinned_pages * PAGE_SIZE;

		/* If vm_write is set, the pages need to be made dirty: */
		/* 写远端时 unpin 前统一标脏，保证页内容变化能被回写；读方向只解除 pin。 */
		unpin_user_pages_dirty_lock(process_pages, pinned_pages,
					    vm_write);
	}

	/* 本批所有 pin 已释放；返回仅表达是否应停止下一远端 iovec。 */
	return rc;
}

/* Maximum number of entries for process pages array
   which lives on stack */
/* 小远端向量优先用 16 项栈数组，避免 kmalloc；更大者才启用有上限的动态缓冲。 */
#define PVM_MAX_PP_ARRAY_COUNT 16

/**
 * process_vm_rw_core - core of reading/writing pages from task specified
 * @pid: PID of process to read/write from/to
 * @iter: where to copy to/from locally
 * @rvec: iovec array specifying where to copy to/from in the other process
 * @riovcnt: size of rvec array
 * @flags: currently unused
 * @vm_write: 0 if reading from other process, 1 if writing to other process
 *
 * Returns the number of bytes read/written or error code. May
 *  return less bytes than expected if an error occurs during the copying
 *  process.
 */
/*
 * 获取目标任务/mm 权限与生命周期，并依次执行全部远端 iovec。
 * 业务背景：系统调用入口只负责导入用户数组；本核心用真实凭据执行 ptrace attach 级
 * 权限检查，并统一管理 task/mm 引用和 page* 暂存空间，是安全边界与部分成功汇合点。
 * 入参：@pid 是调用者 PID namespace 中的目标 PID；@iter 是已验证的本地输入输出
 * iterator；@rvec 是内核中的远端 iovec 借用数组，@riovcnt 为项数；@flags 当前必须
 * 为 0 且本层不使用；@vm_write 为 0 读远端、1 写远端。
 * 出参/返回：成功返回复制字节数；零长度返回 0；复制前可返回 -ENOMEM/-ESRCH、mm_access
 * 的权限/信号错误（-EACCES 映射为 -EPERM）或 -EFAULT。只要复制过至少一字节，就返回
 * 正字节数并屏蔽随后错误。所有 task/mm 引用与动态数组在出口释放。
 * 注意事项：可睡眠；mm_access 与 mmput 配对、find_get_task_by_vpid 与 put_task_struct
 * 配对。权限检查采用 PTRACE_MODE_ATTACH_REALCREDS，目标 exec/exit 竞态由 mm 引用稳定。
 */
static ssize_t process_vm_rw_core(pid_t pid, struct iov_iter *iter,
				  const struct iovec *rvec,
				  unsigned long riovcnt,
				  unsigned long flags, int vm_write)
{
	/* task/mm 分别持有目标任务与地址空间引用；失败标签按取得顺序逆序释放。 */
	struct task_struct *task;
	/* 小批量 page* 用栈存储，process_pages 必要时改指向 kmalloc 缓冲。 */
	struct page *pp_stack[PVM_MAX_PP_ARRAY_COUNT];
	struct page **process_pages = pp_stack;
	struct mm_struct *mm;
	/* i 遍历远端向量；nr_pages 保存单个向量所需页数的最大值。 */
	unsigned long i;
	ssize_t rc = 0;
	unsigned long nr_pages = 0;
	unsigned long nr_pages_iov;
	/* iov_len 是当前远端段长度；total_len 从本地 iterator 初始剩余量开始。 */
	ssize_t iov_len;
	size_t total_len = iov_iter_count(iter);

	/*
	 * Work out how many pages of struct pages we're going to need
	 * when eventually calling get_user_pages
	 */
	/*
	 * 阶段 1：预扫远端向量，只求最大单段页数，用于选择 page* 暂存容量；不会 pin 页。
	 * 长度为零的段不参与计算，实际批处理仍受 PVM_MAX_USER_PAGES 限制。
	 */
	for (i = 0; i < riovcnt; i++) {
		iov_len = rvec[i].iov_len;
		if (iov_len > 0) {
			nr_pages_iov = ((unsigned long)rvec[i].iov_base
					+ iov_len - 1)
				/ PAGE_SIZE - (unsigned long)rvec[i].iov_base
				/ PAGE_SIZE + 1;
			nr_pages = max(nr_pages, nr_pages_iov);
		}
	}

	/* 全部远端段为空时，无需查任务或做权限检查，按零字节成功。 */
	if (nr_pages == 0)
		return 0;

	/* 阶段 2：超过栈容量时分配动态数组，但最多两页，超大段由 single_vec 分批复用。 */
	if (nr_pages > PVM_MAX_PP_ARRAY_COUNT) {
		/* For reliability don't try to kmalloc more than
		   2 pages worth */
		/* 为可靠性不尝试分配超过两页的 page* 数组，避免用户 iovec 制造大额内核分配。 */
		process_pages = kmalloc(min_t(size_t, PVM_MAX_KMALLOC_PAGES * PAGE_SIZE,
					      sizeof(struct page *)*nr_pages),
					GFP_KERNEL);

		/* 尚未取得 task/mm，分配失败可直接返回且没有资源需要回滚。 */
		if (!process_pages)
			return -ENOMEM;
	}

	/* Get process information */
	/* 阶段 3：按调用者 PID namespace 查找并持有 task；不存在/已消失返回 ESRCH。 */
	task = find_get_task_by_vpid(pid);
	if (!task) {
		rc = -ESRCH;
		goto free_proc_pages;
	}

	/*
	 * mm_access 在 exec_update_lock 下取得 mm_users 引用并执行 ptrace/LSM 权限检查；
	 * REALCREDS 防止调用者借 saved credentials 绕过跨进程内存访问策略。
	 */
	mm = mm_access(task, PTRACE_MODE_ATTACH_REALCREDS);
	if (IS_ERR(mm)) {
		rc = PTR_ERR(mm);
		/*
		 * Explicitly map EACCES to EPERM as EPERM is a more
		 * appropriate error code for process_vw_readv/writev
		 */
		/* syscall ABI 把底层 EACCES 明确改为更合适的“操作不允许”EPERM。 */
		if (rc == -EACCES)
			rc = -EPERM;
		goto put_task_struct;
	}

	/* 阶段 4：按远端向量顺序复制；本地 iterator 用尽或首个错误即停止。 */
	for (i = 0; i < riovcnt && iov_iter_count(iter) && !rc; i++)
		rc = process_vm_rw_single_vec(
			(unsigned long)rvec[i].iov_base, rvec[i].iov_len,
			iter, process_pages, mm, task, vm_write);

	/* copied = space before - space after */
	/* iterator 是唯一准确进度源，能反映页内 copy fault 前已经成功的字节。 */
	total_len -= iov_iter_count(iter);

	/* If we have managed to copy any data at all then
	   we return the number of bytes copied. Otherwise
	   we return the error code */
	/* POSIX 风格部分成功：只要有进度，正字节数优先于后续远端/本地 fault。 */
	if (total_len)
		rc = total_len;

	/* 正常/复制错误路径均释放 mm_users；页 pin 已由 single_vec 每批释放。 */
	mmput(mm);

put_task_struct:
	/* 到此 task 引用仍在，mm 或未取得或已释放。 */
	put_task_struct(task);

free_proc_pages:
	/* 栈数组不可释放；只有 process_pages 改指向动态缓冲时 kfree。 */
	if (process_pages != pp_stack)
		kfree(process_pages);
	return rc;
}

/**
 * process_vm_rw - check iovecs before calling core routine
 * @pid: PID of process to read/write from/to
 * @lvec: iovec array specifying where to copy to/from locally
 * @liovcnt: size of lvec array
 * @rvec: iovec array specifying where to copy to/from in the other process
 * @riovcnt: size of rvec array
 * @flags: currently unused
 * @vm_write: 0 if reading from other process, 1 if writing to other process
 *
 * Returns the number of bytes read/written or error code. May
 *  return less bytes than expected if an error occurs during the copying
 *  process.
 */
/*
 * 导入并校验本地/远端用户 iovec，然后调用权限与复制核心。
 * 业务背景：两个 syscall 共用本包装；本地向量需构造 iov_iter 并验证当前进程地址，
 * 远端向量只复制描述符，真正地址有效性由目标 mm 的 remote GUP 判断。
 * 入参：@pid 为目标 PID；@lvec/@liovcnt 是当前调用者的用户态 iovec 数组及项数；
 * @rvec/@riovcnt 是目标进程地址描述数组及项数（数组本身仍位于调用者内存）；@flags
 * 当前只接受 0；@vm_write 为 0 表示 remote→local，为 1 表示 local→remote。
 * 出参/返回：返回 core 的字节数/错误；非零 flags 为 -EINVAL，本地/远端描述符导入可返回
 * -EFAULT/-EINVAL/-ENOMEM。所有临时 iovec 动态内存在返回前释放，栈数组不被 kfree。
 * 注意事项：可睡眠；import_iovec 可能把 @iov_l 置 NULL 或改为动态数组，kfree(NULL)
 * 安全。方向必须从本地 iterator 视角选择，writev 的本地数据是 ITER_SOURCE。
 */
static ssize_t process_vm_rw(pid_t pid,
			     const struct iovec __user *lvec,
			     unsigned long liovcnt,
			     const struct iovec __user *rvec,
			     unsigned long riovcnt,
			     unsigned long flags, int vm_write)
{
	/* 两个小数组覆盖常见 fast path；iov_l/iov_r 必要时接收动态导入结果。 */
	struct iovec iovstack_l[UIO_FASTIOV];
	struct iovec iovstack_r[UIO_FASTIOV];
	struct iovec *iov_l = iovstack_l;
	struct iovec *iov_r;
	/* iter 表示当前进程本地缓冲并累计实际复制进度；rc 贯穿 cleanup。 */
	struct iov_iter iter;
	ssize_t rc;
	/* 写远端时本地是数据源，读远端时本地是数据目的地。 */
	int dir = vm_write ? ITER_SOURCE : ITER_DEST;

	/* ABI 预留 flags 尚无定义；任何非零位都拒绝，避免未来语义歧义。 */
	if (flags != 0)
		return -EINVAL;

	/* Check iovecs */
	/* 阶段 1：导入本地 iovec、检查当前用户地址并初始化 iterator。 */
	rc = import_iovec(dir, lvec, liovcnt, UIO_FASTIOV, &iov_l, &iter);
	if (rc < 0)
		return rc;
	/* 本地总长度为零时无需读取远端描述符或检查目标权限。 */
	if (!iov_iter_count(&iter))
		goto free_iov_l;
	/*
	 * 阶段 2：复制远端 iovec 描述符，兼容 syscall 时转换 32 位布局；这里不对
	 * iov_base 做 current 的 access_ok，因为它属于目标进程地址空间。
	 */
	iov_r = iovec_from_user(rvec, riovcnt, UIO_FASTIOV, iovstack_r,
				in_compat_syscall());
	if (IS_ERR(iov_r)) {
		rc = PTR_ERR(iov_r);
		goto free_iov_l;
	}
	/* 阶段 3：描述符均稳定在内核内存，进入 task/mm 权限检查和实际复制。 */
	rc = process_vm_rw_core(pid, &iter, iov_r, riovcnt, flags, vm_write);
	/* iovec_from_user 只有超过 fast capacity 才返回动态数组。 */
	if (iov_r != iovstack_r)
		kfree(iov_r);
free_iov_l:
	/* import_iovec 约定栈 fast path 时 iov_l 为 NULL，故统一 kfree 安全。 */
	kfree(iov_l);
	return rc;
}

/*
 * 从 @pid 的远端向量读取到当前进程本地向量。
 * 业务背景：系统调用宏生成 ABI 包装，本体只固定 vm_write=0 并进入公共实现。
 * 入参：@pid、@lvec/@liovcnt、@rvec/@riovcnt、@flags 均保持用户 ABI 语义；两数组为
 * 借用用户指针。出参/返回：复制字节数、0 或公共路径 errno；不保留用户指针。
 * 注意事项：可睡眠，需 ptrace attach real-credentials 权限；可能部分成功。
 */
SYSCALL_DEFINE6(process_vm_readv, pid_t, pid, const struct iovec __user *, lvec,
		unsigned long, liovcnt, const struct iovec __user *, rvec,
		unsigned long, riovcnt,	unsigned long, flags)
{
	/* remote → local，因此公共层把本地 iterator 初始化为 ITER_DEST。 */
	return process_vm_rw(pid, lvec, liovcnt, rvec, riovcnt, flags, 0);
}

/*
 * 从当前进程本地向量写入 @pid 的远端向量。
 * 业务背景：与 process_vm_readv 共用安全边界，只把 vm_write 固定为 1，使远端 GUP
 * 请求 FOLL_WRITE 并在 unpin 时标脏。
 * 入参：@pid、两组用户 iovec/计数和 @flags 均为借用 ABI 输入。出参/返回：复制字节数、
 * 0 或公共 errno；可能部分成功，无输出参数 ownership。
 * 注意事项：可睡眠且需要 ptrace attach real-credentials 权限；写只读远端 VMA 会失败。
 */
SYSCALL_DEFINE6(process_vm_writev, pid_t, pid,
		const struct iovec __user *, lvec,
		unsigned long, liovcnt, const struct iovec __user *, rvec,
		unsigned long, riovcnt,	unsigned long, flags)
{
	/* local → remote，因此公共层把本地 iterator 初始化为 ITER_SOURCE。 */
	return process_vm_rw(pid, lvec, liovcnt, rvec, riovcnt, flags, 1);
}
