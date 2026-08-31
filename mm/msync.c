// SPDX-License-Identifier: GPL-2.0
/*
 *	linux/mm/msync.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * The msync() system call.
 */
/*
 * 本文件实现 msync() 系统调用：把用户虚拟地址区间映射回共享文件区间，并在
 * MS_SYNC 模式调用文件系统 fsync 回调。它只遍历当前进程 VMA，不改变映射布局。
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/file.h>
#include <linux/syscalls.h>
#include <linux/sched.h>

/*
 * MS_SYNC syncs the entire file - including mappings.
 *
 * MS_ASYNC does not start I/O (it used to, up to 2.5.67).
 * Nor does it marks the relevant pages dirty (it used to up to 2.6.17).
 * Now it doesn't do anything, since dirty pages are properly tracked.
 *
 * The application may now run fsync() to
 * write out the dirty pages and wait on the writeout and check the result.
 * Or the application may run fadvise(FADV_DONTNEED) against the fd to start
 * async writeout immediately.
 * So by _not_ starting I/O in MS_ASYNC we provide complete flexibility to
 * applications.
 */
/*
 * MS_SYNC 会同步整个相关文件范围，包括 mmap 映射产生的脏数据。
 * MS_ASYNC 自 2.5.67 起不再启动 I/O，自 2.6.17 起也不再显式标脏；现代内核
 * 已正确追踪脏页，因此该模式本身无需动作。应用若要写出并等待、检查结果，
 * 可调用 fsync()；若只想立即触发异步写回，可对 fd 使用
 * fadvise(FADV_DONTNEED)。不在 MS_ASYNC 中擅自启动 I/O，反而把策略选择完整
 * 留给应用。MS_INVALIDATE 在此实现中主要负责拒绝锁定 VMA，而非主动丢页。
 */
/*
 * msync() - 同步或检查当前进程一段文件映射。
 *
 * 业务背景：用户修改 MAP_SHARED 映射后，可要求把对应文件字节区间提交给
 * 文件系统并等待完成；本函数负责地址校验、VMA 遍历、洞检测和地址到文件
 * offset 的转换，真正持久化由 vfs_fsync_range() 分派给具体文件系统。
 * 入参：@start 是用户虚拟起始地址，允许带体系结构地址 tag 但去 tag 后必须
 * 页对齐；@len 是字节长度，会向上按页取整；@flags 只能由 MS_ASYNC、
 * MS_INVALIDATE、MS_SYNC 组成，且 ASYNC/SYNC 互斥。三者均为纯输入。
 * 出参/返回：成功返回 0；非法 flag、未对齐或互斥模式返回 -EINVAL；区间溢出
 * 或覆盖未映射地址返回 -ENOMEM；INVALIDATE 遇 VM_LOCKED 返回 -EBUSY；同步
 * 文件失败则传播 vfs_fsync_range() errno。已完成的较早 VMA 不会回滚。
 * 注意事项：mmap_read_lock 稳定每轮 VMA，但同步 I/O 前会 get_file() 后解锁，
 * 因而可睡眠且 VMA 布局可能变化；重新加锁后必须从当前 @start 再查。文件引用
 * 跨越解锁/I/O 生命周期，fput() 后不再使用。MS_SYNC 只处理 file-backed、
 * VM_SHARED VMA，匿名或私有 VMA 无需文件同步。
 */
SYSCALL_DEFINE3(msync, unsigned long, start, size_t, len, int, flags)
{
	/* 变量地图：end 是尾后地址；mm/vma 是当前地址空间及借用扫描游标。 */
	unsigned long end;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	/* unmapped_error 记住曾遇到洞；error 保存当前硬错误或 fsync 结果。 */
	int unmapped_error = 0;
	int error = -EINVAL;

	/* 阶段 1：去除地址 tag，再验证 flag 组合与页对齐入口契约。 */
	start = untagged_addr(start);

	if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
		goto out;
	if (offset_in_page(start))
		goto out;
	if ((flags & MS_ASYNC) && (flags & MS_SYNC))
		goto out;
	/* 阶段 2：页对齐长度并以无符号回绕检测尾后地址溢出。 */
	error = -ENOMEM;
	len = (len + ~PAGE_MASK) & PAGE_MASK;
	end = start + len;
	if (end < start)
		goto out;
	/* 空区间在参数合法时立即成功，不需要取得 mmap_lock。 */
	error = 0;
	if (end == start)
		goto out;
	/*
	 * If the interval [start,end) covers some unmapped address ranges,
	 * just ignore them, but return -ENOMEM at the end. Besides, if the
	 * flag is MS_ASYNC (w/o MS_INVALIDATE) the result would be -ENOMEM
	 * anyway and there is nothing left to do, so return immediately.
	 */
	/*
	 * 若 [start,end) 含未映射洞，继续处理其余已映射部分，但最终返回 -ENOMEM。
	 * 唯一例外是纯 MS_ASYNC：它既不触发 I/O，也不 invalidate，继续扫描不会
	 * 增加副作用，因此一发现洞即可直接以 -ENOMEM 结束。
	 */
	/* 阶段 3：读锁稳定 VMA 索引，find_vma 返回首个 vm_end 大于 start 的 VMA。 */
	mmap_read_lock(mm);
	vma = find_vma(mm, start);
	for (;;) {
		/* file 是跨解锁前临时借用；fstart/fend 是闭区间文件字节偏移。 */
		struct file *file;
		loff_t fstart, fend;

		/* Still start < end. */
		/* 循环不变量仍是 start < end；无后继 VMA 表示剩余区间全部未映射。 */
		error = -ENOMEM;
		if (!vma)
			goto out_unlock;
		/* Here start < vma->vm_end. */
		/* 此处 start < vm_end；若 start 在 vm_start 前，先处理映射洞。 */
		if (start < vma->vm_start) {
			if (flags == MS_ASYNC)
				goto out_unlock;
			/* 跳过洞继续处理后续 VMA，同时保存最终 -ENOMEM。 */
			start = vma->vm_start;
			if (start >= end)
				goto out_unlock;
			unmapped_error = -ENOMEM;
		}
		/* Here vma->vm_start <= start < vma->vm_end. */
		/* 现在 start 位于当前 VMA；锁定页不能被 INVALIDATE 语义干扰。 */
		if ((flags & MS_INVALIDATE) &&
				(vma->vm_flags & VM_LOCKED)) {
			error = -EBUSY;
			goto out_unlock;
		}
		/* 阶段 4：把本段虚拟偏移换算为 VMA 对应的闭区间文件偏移。 */
		file = vma->vm_file;
		fstart = (start - vma->vm_start) +
			 ((loff_t)vma->vm_pgoff << PAGE_SHIFT);
		fend = fstart + (min(end, vma->vm_end) - start) - 1;
		/* 先保存下一扫描地址，避免同步期间 VMA 被拆分、合并或删除。 */
		start = vma->vm_end;
		if ((flags & MS_SYNC) && file &&
				(vma->vm_flags & VM_SHARED)) {
			/*
			 * 同步慢路径：get_file 稳定 file 生命周期，然后释放 mmap 读锁再做
			 * 可能长时间睡眠的 filesystem fsync。datasync=1 仍会提交访问数据
			 * 所必需的元数据；失败立即返回，已同步的前段不会撤销。
			 */
			get_file(file);
			mmap_read_unlock(mm);
			error = vfs_fsync_range(file, fstart, fend, 1);
			fput(file);
			if (error || start >= end)
				goto out;
			/* 未结束时重新取得锁并按保存的 start 重建 VMA 游标，禁止复用旧 vma。 */
			mmap_read_lock(mm);
			vma = find_vma(mm, start);
		} else {
			/* 快路径：ASYNC、匿名、私有或无 MS_SYNC 时不发 I/O，只推进扫描。 */
			if (start >= end) {
				error = 0;
				goto out_unlock;
			}
			vma = find_vma(mm, vma->vm_end);
		}
	}
out_unlock:
	/* 所有仍持 mmap 读锁的正常/错误出口在这里成对释放。 */
	mmap_read_unlock(mm);
out:
	/* 当前硬错误优先；否则把先前遇到映射洞的 -ENOMEM 延迟返回。 */
	return error ? : unmapped_error;
}
