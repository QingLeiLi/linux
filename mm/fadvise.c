// SPDX-License-Identifier: GPL-2.0
/*
 * mm/fadvise.c
 *
 * Copyright (C) 2002, Linus Torvalds
 *
 * 11Jan2003	Andrew Morton
 *		Initial version.
 */
/*
 * 本实现由 Andrew Morton 于 2003-01-11 提交初版；文件把 fadvise 系统调用、
 * 文件系统可选回调与通用 page-cache/readahead 建议语义串成一条调用链。
 */

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
/* 前组提供 file/inode/page-cache 核心类型；后组提供 bdi、advice、writeback 与 ABI。 */
#include <linux/backing-dev.h>
#include <linux/fadvise.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/swap.h>

#include <asm/unistd.h>

#include "internal.h"

/*
 * POSIX_FADV_WILLNEED could set PG_Referenced, and POSIX_FADV_NOREUSE could
 * deactivate the pages and clear PG_Referenced.
 */
/*
 * POSIX_FADV_WILLNEED 的预读可能设置 PG_Referenced；POSIX_FADV_NOREUSE 则会
 * 通过 FMODE_NOREUSE 让相关映射不参与 recency 更新，从而可能使页面退活并清除
 * PG_Referenced。这些 advice 是回收/预读提示，不是数据持久化或立即回收保证。
 */

/*
 * generic_fadvise() - 为普通 page-cache 文件实现 POSIX fadvise 提示。
 *
 * 业务背景：vfs_fadvise() 在文件系统没有专用 ->fadvise 回调时进入这里；函数按
 * advice 调整 file 级预读模式、触发范围预读或尽力回收干净缓存页。
 * 入参：@file 是调用期间持有引用的文件借用指针，不可为 NULL；@offset/@len 是
 * byte 范围，offset/len 不得为负，len=0 表示延伸到可表示末端；@advice 必须是
 * POSIX_FADV_* 已知值。
 * 出参/返回：成功或有效但被 DAX/noop bdi 忽略时返回 0；FIFO 返回 -ESPIPE，非法
 * 范围/mapping/advice 返回 -EINVAL。预读、flush 和失效的内部失败多按提示语义忽略。
 * 注意事项：可睡眠；file->f_lock 只保护 FMODE_* 竞态，page cache/folio 生命周期
 * 由各 helper 自身同步。DONTNEED 不保证丢弃 dirty、mapped、locked 或正在 I/O 的页。
 */
int generic_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	/*
	 * inode/mapping/bdi 是 file 引用保护下的借用对象；endbyte 是闭区间 byte 末端，
	 * start/end_index 是页索引闭区间，nrpages 仅供 WILLNEED 使用。
	 */
	struct inode *inode;
	struct address_space *mapping;
	struct backing_dev_info *bdi;
	loff_t endbyte;			/* inclusive */
	/* endbyte 保存包含式末字节；后续页区间换算均据此避免 off-by-one。 */
	pgoff_t start_index;
	pgoff_t end_index;
	unsigned long nrpages;

	/* FIFO 不具备可定位 byte 范围，POSIX 要求以 -ESPIPE 拒绝。 */
	inode = file_inode(file);
	if (S_ISFIFO(inode->i_mode))
		return -ESPIPE;

	/* 普通建议需要 address_space；负 offset/len 在任何页索引换算前拒绝。 */
	mapping = file->f_mapping;
	if (!mapping || len < 0 || offset < 0)
		return -EINVAL;

	bdi = inode_to_bdi(mapping->host);

	/* DAX 绕过 page cache，noop bdi 也没有有效预读/回写后端：只校验 advice。 */
	if (IS_DAX(inode) || (bdi == &noop_backing_dev_info)) {
		switch (advice) {
		case POSIX_FADV_NORMAL:
			/* NORMAL 是恢复默认策略的有效提示，但本分支没有缓存状态可改。 */
		case POSIX_FADV_RANDOM:
			/* RANDOM 在无 page cache/readahead 后端时接受但忽略。 */
		case POSIX_FADV_SEQUENTIAL:
			/* SEQUENTIAL 同样没有可调整的预读窗口。 */
		case POSIX_FADV_WILLNEED:
			/* WILLNEED 无法为 DAX/noop mapping 发起通用 page-cache 预读。 */
		case POSIX_FADV_NOREUSE:
			/* NOREUSE 不改变这类 mapping 的回收 recency。 */
		case POSIX_FADV_DONTNEED:
			/* DONTNEED 也没有通用 page cache 可失效。 */
			/* no bad return value, but ignore advice */
			/* 这些都是合法 advice，不能报错，只需明确忽略其效果。 */
			break;
		default:
			/* 未知 advice 即使在无缓存后端也必须拒绝，保持 ABI 参数校验。 */
			return -EINVAL;
		}
		return 0;
	}

	/*
	 * Careful about overflows. Len == 0 means "as much as possible".  Use
	 * unsigned math because signed overflows are undefined and UBSan
	 * complains.
	 */
	/*
	 * 必须防止溢出。len==0 表示“尽可能多”；用无符号加法是因为有符号溢出
	 * 未定义且 UBSan 会报告。和超过 loff_t 正上限或发生无符号回绕时统一饱和到
	 * LLONG_MAX；否则减 1 把 [offset, offset+len) 转成包含式末字节。
	 */
	endbyte = (u64)offset + (u64)len;
	if (!len || endbyte < len)
		endbyte = LLONG_MAX;
	else
		endbyte--;		/* inclusive */
	/* 非零有限长度转换为闭区间，因此末字节是 offset+len-1。 */

	switch (advice) {
	case POSIX_FADV_NORMAL:
		/* 恢复 bdi 默认预读窗口，并在 f_lock 下同时清除 RANDOM/NOREUSE 模式。 */
		file->f_ra.ra_pages = bdi->ra_pages;
		spin_lock(&file->f_lock);
		file->f_mode &= ~(FMODE_RANDOM | FMODE_NOREUSE);
		spin_unlock(&file->f_lock);
		break;
	case POSIX_FADV_RANDOM:
		/* 标记随机访问；readahead 路径据此禁用常规顺序预读或走强制读取行为。 */
		spin_lock(&file->f_lock);
		file->f_mode |= FMODE_RANDOM;
		spin_unlock(&file->f_lock);
		break;
	case POSIX_FADV_SEQUENTIAL:
		/* 顺序模式把默认预读窗口加倍并清 RANDOM；不清 NOREUSE，两个提示可叠加。 */
		file->f_ra.ra_pages = bdi->ra_pages * 2;
		spin_lock(&file->f_lock);
		file->f_mode &= ~FMODE_RANDOM;
		spin_unlock(&file->f_lock);
		break;
	case POSIX_FADV_WILLNEED:
		/* First and last PARTIAL page! */
		/* WILLNEED 覆盖首尾部分页：任何与 byte 范围相交的 page 都纳入预读。 */
		start_index = offset >> PAGE_SHIFT;
		end_index = endbyte >> PAGE_SHIFT;

		/* Careful about overflow on the "+1" */
		/* end-start+1 也可能回绕为 0；此时用 ULONG_MAX 表示尽可能多的页。 */
		nrpages = end_index - start_index + 1;
		if (!nrpages)
			nrpages = ~0UL;

		/* 预读是尽力异步填充提示；helper 的内部短读/分配失败不转成 fadvise 错误。 */
		force_page_cache_readahead(mapping, file, start_index, nrpages);
		break;
	case POSIX_FADV_NOREUSE:
		/* 后续映射访问不更新 recency，帮助一次性数据更快成为回收候选。 */
		spin_lock(&file->f_lock);
		file->f_mode |= FMODE_NOREUSE;
		spin_unlock(&file->f_lock);
		break;
	case POSIX_FADV_DONTNEED:
		/* 先尽力启动 dirty folio 回写，但不等待完整性提交，也忽略 flush errno。 */
		filemap_flush_range(mapping, offset, endbyte);

		/*
		 * First and last FULL page! Partial pages are deliberately
		 * preserved on the expectation that it is better to preserve
		 * needed memory than to discard unneeded memory.
		 */
		/*
		 * 首尾只处理完整页。刻意保留部分页，因为保留少量可能仍需的数据优于
		 * 为丢弃无用部分而连同同页有用字节一起逐出。
		 */
		start_index = (offset+(PAGE_SIZE-1)) >> PAGE_SHIFT;
		end_index = (endbyte >> PAGE_SHIFT);
		/*
		 * The page at end_index will be inclusively discarded according
		 * by invalidate_mapping_pages(), so subtracting 1 from
		 * end_index means we will skip the last page.  But if endbyte
		 * is page aligned or is at the end of file, we should not skip
		 * that page - discarding the last page is safe enough.
		 */
		/*
		 * invalidate_mapping_pages() 会包含式处理 end_index，所以对末字节没有落在
		 * 页尾的普通范围先减 1，跳过末尾部分页；若范围恰好以页尾结束，或末字节
		 * 正是文件 EOF，则丢弃该末页是安全的，不应跳过。
		 */
		if ((endbyte & ~PAGE_MASK) != ~PAGE_MASK &&
				endbyte != inode->i_size - 1) {
			/* First page is tricky as 0 - 1 = -1, but pgoff_t
			 * is unsigned, so the end_index >= start_index
			 * check below would be true and we'll discard the whole
			 * file cache which is not what was asked.
			 */
			/*
			 * 首页索引尤其棘手：pgoff_t 是无符号，0-1 会回绕成最大值，使下面
			 * end_index>=start_index 错判为真并丢弃整个文件缓存，这并非请求范围。
			 */
			if (end_index == 0)
				break;

			end_index--;
		}

		/* 只有至少存在一张完整页时才进入尽力失效；部分页范围直接成功返回。 */
		if (end_index >= start_index) {
			/* 首次尝试不能逐出的 folio 数，决定是否支付全 CPU drain 代价。 */
			unsigned long nr_failed = 0;

			/*
			 * It's common to FADV_DONTNEED right after
			 * the read or write that instantiates the
			 * pages, in which case there will be some
			 * sitting on the local LRU cache. Try to
			 * avoid the expensive remote drain and the
			 * second cache tree walk below by flushing
			 * them out right away.
			 */
			/*
			 * FADV_DONTNEED 常紧跟刚实例化页面的 read/write，本 CPU 的 folio 可能
			 * 仍停在本地 LRU 批缓存。先做便宜的本地 drain，可避免昂贵的远端 drain
			 * 和第二次 page-cache 树遍历。
			 */
			lru_add_drain();

			/* 第一遍逐出 clean/unmapped/unlocked folio，并统计暂时失败项。 */
			mapping_try_invalidate(mapping, start_index, end_index,
					&nr_failed);

			/*
			 * The failures may be due to the folio being
			 * in the LRU cache of a remote CPU. Drain all
			 * caches and try again.
			 */
			/*
			 * 失败可能只是 folio 仍在远端 CPU 的 LRU 批缓存；此时 drain 所有 CPU
			 * 后再遍历一次。第二遍仍是尽力失效，剩余 busy/dirty 页不会变成 errno。
			 */
			if (nr_failed) {
				lru_add_drain_all();
				invalidate_mapping_pages(mapping, start_index,
						end_index);
			}
		}
		break;
	default:
		/* 普通 page-cache 后端同样拒绝所有未知 advice。 */
		return -EINVAL;
	}
	return 0;
}

/* 文件系统可直接复用通用实现；导出不转移 file ownership。 */
EXPORT_SYMBOL(generic_fadvise);

/*
 * vfs_fadvise() - 在文件系统专用实现与通用实现之间分派。
 * 业务背景：系统调用、readahead 和 madvise WILLNEED 统一从此 VFS 边界进入，特定
 * 文件系统可用 file_operations::fadvise 扩展缓存或压缩等私有状态。
 * 入参：@file 是持有引用期间的借用文件，不可为 NULL；@offset/@len/@advice 原样
 * 传给所选实现，单位和范围语义由该实现按 fadvise 契约校验。
 * 出参/返回：原样返回文件系统回调或 generic_fadvise() 的 0/负 errno。
 * 注意事项：回调可能睡眠且负责自身锁；本层不重试、不改写结果、不持额外引用。
 */
int vfs_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	/* open 时确定的 operation table 优先；NULL 回调才落入通用 page-cache 语义。 */
	if (file->f_op->fadvise)
		return file->f_op->fadvise(file, offset, len, advice);

	return generic_fadvise(file, offset, len, advice);
}

/* 内核其他子系统和堆叠文件系统可调用这一分派边界。 */
EXPORT_SYMBOL(vfs_fadvise);

#ifdef CONFIG_ADVISE_SYSCALLS

/*
 * ksys_fadvise64_64() - 解析 fd 并调用 VFS fadvise 分派。
 * 业务背景：原生与 compat syscall wrapper 汇合在此，避免重复 fd 生命周期处理。
 * 入参：@fd 为当前进程描述符；@offset/@len 为 64 位 byte 范围；@advice 为 ABI 值。
 * 出参/返回：无效 fd 返回 -EBADF，否则原样返回 vfs_fadvise() 结果。
 * 注意事项：CLASS(fd) 通过 fdget() 稳定 file，并在离开作用域时自动 fdput；函数可睡眠。
 */
int ksys_fadvise64_64(int fd, loff_t offset, loff_t len, int advice)
{
	/* cleanup class 确保成功取得的 fd 引用在任意 return 路径自动释放。 */
	CLASS(fd, f)(fd);

	if (fd_empty(f))
		return -EBADF;

	return vfs_fadvise(fd_file(f), offset, len, advice);
}

/*
 * sys_fadvise64_64() - 原生四参数 64 位范围系统调用包装。
 * 业务背景：体系结构 syscall ABI 进入后只负责类型封送，语义集中在 ksys helper。
 * 入参：@fd、@offset、@len、@advice 均来自用户寄存器，范围单位 byte。
 * 出参/返回：原样返回 ksys_fadvise64_64() 的 0 或负 errno，无额外副作用。
 * 注意事项：SYSCALL_DEFINE 生成 ABI 包装；本函数本体可睡眠。
 */
SYSCALL_DEFINE4(fadvise64_64, int, fd, loff_t, offset, loff_t, len, int, advice)
{
	return ksys_fadvise64_64(fd, offset, len, advice);
}

#ifdef __ARCH_WANT_SYS_FADVISE64

/*
 * sys_fadvise64() - 为需要旧式 size_t len ABI 的架构提供包装。
 * 业务背景：offset 保持 loff_t，而无符号架构字长 len 扩展后汇入统一 64 位 helper。
 * 入参：@fd/@offset/@len/@advice 来自用户态；@len 受本架构 size_t 宽度限制。
 * 出参/返回：原样返回统一 helper 状态，无额外 ownership 或缓存副作用。
 * 注意事项：仅 __ARCH_WANT_SYS_FADVISE64 构建；实际校验仍由 generic/文件系统实现完成。
 */
SYSCALL_DEFINE4(fadvise64, int, fd, loff_t, offset, size_t, len, int, advice)
{
	return ksys_fadvise64_64(fd, offset, len, advice);
}

#endif

#if defined(CONFIG_COMPAT) && defined(__ARCH_WANT_COMPAT_FADVISE64_64)

/*
 * compat_sys_fadvise64_64() - 重组 32 位 ABI 拆分的两个 64 位范围参数。
 * 业务背景：compat_arg_u64_dual 把 offset/len 各拆为两个寄存器参数，glue 宏按架构
 * 端序重建 u64，再复用原生 ksys 路径。
 * 入参：@fd 与 @advice 为兼容整数；@offset/@len 各由双 32 位分量组成，单位 byte。
 * 出参/返回：原样返回 ksys helper 的 0/负 errno，无额外引用或状态。
 * 注意事项：仅 CONFIG_COMPAT 且架构请求该 ABI 时存在；重组先于负值/范围校验。
 */
COMPAT_SYSCALL_DEFINE6(fadvise64_64, int, fd, compat_arg_u64_dual(offset),
		       compat_arg_u64_dual(len), int, advice)
{
	return ksys_fadvise64_64(fd, compat_arg_u64_glue(offset),
				 compat_arg_u64_glue(len), advice);
}

#endif
#endif
