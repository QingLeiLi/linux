// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/fs/exec.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/**
 * exec.c - 进程执行（execve系统调用实现）
 *
 * 【核心功能】
 * 实现execve()系统调用，用于加载和执行新程序。
 * 这是Unix/Linux进程模型的核心：fork()创建进程，exec()加载程序。
 *
 * 【历史演进】
 * - 1991-1992: Linus最初实现
 * - #!脚本支持由tytso实现
 * - 1991.12.01: 需求加载（demand-loading）实现 - 只读取头部到内存，
 *   可执行文件的inode放入current->executable，页错误完成实际加载
 * - 1993.07: Eric Youngdale改为使用mmap，current->executable仅用于procfs
 *   使用分派表支持多种二进制格式（ELF、a.out、脚本等）
 */

/*
 * #!-checking implemented by tytso.
 */
/**
 * #!检查由tytso实现
 *
 * 【脚本支持】
 * 支持#!/bin/bash这样的shebang行，内核识别并调用解释器
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 *
 * Demand loading changed July 1993 by Eric Youngdale.   Use mmap instead,
 * current->executable is only used by the procfs.  This allows a dispatch
 * table to check for several different types  of binary formats.  We keep
 * trying until we recognize the file or we run out of supported binary
 * formats.
 */
/**
 * 需求加载于1991年12月1日实现 - 无需将任何内容读入内存，只需头部。
 * 可执行文件的inode放入"current->executable"，页错误完成实际加载。简洁。
 *
 * 我可以再次自豪地说，Linux经受住了变更的考验：
 * 完全实现需求加载只花了不到2小时的工作。
 *
 * 需求加载于1993年7月由Eric Youngdale修改。改用mmap，
 * current->executable仅用于procfs。这允许分派表检查多种不同类型的
 * 二进制格式。我们不断尝试，直到识别文件或用尽所有支持的二进制格式。
 *
 * 【技术细节】
 * - 需求加载（demand-loading）：延迟加载，只在访问时加载页面
 * - mmap：内存映射，将文件直接映射到进程地址空间
 * - 分派表：支持ELF、a.out、脚本、Misc等多种格式
 */

#include <linux/kernel_read_file.h>  /* 内核文件读取 */
#include <linux/slab.h>              /* 内存分配 */
#include <linux/file.h>              /* 文件操作 */
#include <linux/fdtable.h>           /* 文件描述符表 */
#include <linux/mm.h>                /* 内存管理 */
#include <linux/stat.h>              /* 文件状态 */
#include <linux/fcntl.h>             /* 文件控制 */
#include <linux/swap.h>              /* 交换分区 */
#include <linux/string.h>            /* 字符串操作 */
#include <linux/init.h>              /* 初始化宏 */
#include <linux/sched/mm.h>          /* 进程内存管理 */
#include <linux/sched/coredump.h>    /* 核心转储 */
#include <linux/sched/exec_state.h>  /* 执行状态 */
#include <linux/sched/signal.h>      /* 信号处理 */
#include <linux/sched/numa_balancing.h> /* NUMA均衡 */
#include <linux/sched/task.h>        /* 任务管理 */
#include <linux/pagemap.h>           /* 页缓存 */
#include <linux/perf_event.h>        /* 性能事件 */
#include <linux/highmem.h>           /* 高端内存 */
#include <linux/spinlock.h>          /* 自旋锁 */
#include <linux/key.h>               /* 密钥管理 */
#include <linux/personality.h>       /* 执行域（personality） */
#include <linux/binfmts.h>           /* 二进制格式 */
#include <linux/utsname.h>           /* 系统名称 */
#include <linux/pid_namespace.h>     /* PID命名空间 */
#include <linux/module.h>            /* 模块支持 */
#include <linux/namei.h>             /* 路径查找 */
#include <linux/mount.h>             /* 挂载点 */
#include <linux/security.h>          /* 安全模块（LSM） */
#include <linux/syscalls.h>          /* 系统调用 */
#include <linux/tsacct_kern.h>       /* 任务统计 */
#include <linux/cn_proc.h>           /* 进程连接器 */
#include <linux/audit.h>             /* 审计 */
#include <linux/kmod.h>              /* 内核模块加载 */
#include <linux/fsnotify.h>          /* 文件系统通知 */
#include <linux/fs_struct.h>         /* 文件系统结构 */
#include <linux/oom.h>               /* OOM killer */
#include <linux/compat.h>            /* 兼容层（32位） */
#include <linux/vmalloc.h>           /* 虚拟内存分配 */
#include <linux/io_uring.h>          /* io_uring异步I/O */
#include <linux/syscall_user_dispatch.h> /* 系统调用用户分派 */
#include <linux/coredump.h>          /* 核心转储 */
#include <linux/time_namespace.h>    /* 时间命名空间 */
#include <linux/user_events.h>       /* 用户事件 */
#include <linux/rseq.h>              /* 可重启序列 */
#include <linux/ksm.h>               /* 内核同页合并 */

#include <linux/uaccess.h>           /* 用户空间访问 */
#include <asm/mmu_context.h>         /* MMU上下文 */
#include <asm/tlb.h>                 /* TLB管理 */

#include <trace/events/task.h>       /* 任务跟踪事件 */
#include "internal.h"

#include <trace/events/sched.h>      /* 调度跟踪事件 */

/* For vma exec functions. */
/* 用于VMA执行函数 */
#include "../mm/internal.h"

static int bprm_creds_from_file(struct linux_binprm *bprm);

/**
 * suid_dumpable - 控制SUID进程的核心转储行为
 *
 * 【安全考虑】
 * 0 = 禁止SUID/SGID进程核心转储（默认，防止泄露敏感信息）
 * 1 = 允许核心转储，但文件所有者改为root
 * 2 = 允许核心转储，保持原进程的uid/gid
 *
 * 可通过 /proc/sys/fs/suid_dumpable 配置
 */
int suid_dumpable = 0;

/**
 * formats - 已注册的二进制格式链表
 *
 * 【二进制格式】
 * 内核支持多种可执行文件格式：
 * - ELF（Executable and Linkable Format）：现代Linux标准
 * - a.out：古老的Unix格式
 * - script：#!脚本
 * - misc：其他格式（通过binfmt_misc注册）
 */
static LIST_HEAD(formats);

/**
 * binfmt_lock - 保护formats链表的读写锁
 */
static DEFINE_RWLOCK(binfmt_lock);

/**
 * __register_binfmt - 注册二进制格式处理器
 * @fmt: 二进制格式结构
 * @insert: 是否插入到链表头部（而非尾部）
 *
 * 【注册机制】
 * 将新的二进制格式处理器添加到formats链表。
 * insert=1: 插入链表头（高优先级）
 * insert=0: 插入链表尾（低优先级）
 *
 * 【为什么需要优先级】
 * 执行文件时按顺序尝试各格式，优先级影响识别顺序。
 * 例如，#!脚本需要在ELF之前检查。
 */
void __register_binfmt(struct linux_binfmt * fmt, int insert)
{
	write_lock(&binfmt_lock);
	insert ? list_add(&fmt->lh, &formats) :
		 list_add_tail(&fmt->lh, &formats);
	write_unlock(&binfmt_lock);
}

EXPORT_SYMBOL(__register_binfmt);

/**
 * unregister_binfmt - 注销二进制格式处理器
 * @fmt: 二进制格式结构
 *
 * 【模块卸载】
 * 从formats链表中移除二进制格式处理器。
 * 通常在模块卸载时调用。
 */
void unregister_binfmt(struct linux_binfmt * fmt)
{
	write_lock(&binfmt_lock);
	list_del(&fmt->lh);
	write_unlock(&binfmt_lock);
}

EXPORT_SYMBOL(unregister_binfmt);

/**
 * put_binfmt - 减少二进制格式模块引用计数
 * @fmt: 二进制格式结构
 *
 * 【模块引用计数】
 * 当不再需要某个格式处理器时调用，允许模块卸载。
 */
static inline void put_binfmt(struct linux_binfmt * fmt)
{
	module_put(fmt->module);
}

/**
 * path_noexec - 检查路径是否禁止执行
 * @path: 文件路径
 *
 * 返回值：true=禁止执行，false=允许执行
 *
 * 【执行权限检查】
 * 检查挂载点是否设置了noexec标志，或文件系统是否禁止执行。
 * 这是安全机制的一部分，例如/tmp可能被挂载为noexec防止执行恶意脚本。
 */
bool path_noexec(const struct path *path)
{
	/* If it's an anonymous inode make sure that we catch any shenanigans. */
	/* 如果是匿名inode，确保捕获任何不当行为 */
	VFS_WARN_ON_ONCE(IS_ANON_FILE(d_inode(path->dentry)) &&
			 !(path->mnt->mnt_sb->s_iflags & SB_I_NOEXEC));
	return (path->mnt->mnt_flags & MNT_NOEXEC) ||
	       (path->mnt->mnt_sb->s_iflags & SB_I_NOEXEC);
}

#ifdef CONFIG_MMU
/*
 * The nascent bprm->mm is not visible until exec_mmap() but it can
 * use a lot of memory, account these pages in current->mm temporary
 * for oom_badness()->get_mm_rss(). Once exec succeeds or fails, we
 * change the counter back via acct_arg_size(0).
 */
/**
 * 新生的bprm->mm在exec_mmap()之前不可见，但它可能使用大量内存。
 * 临时在current->mm中统计这些页面，供oom_badness()->get_mm_rss()使用。
 * 一旦exec成功或失败，我们通过acct_arg_size(0)改回计数器。
 *
 * 【为什么要统计】
 * 在exec期间，新进程的内存空间正在构建中，但OOM killer需要准确的
 * 内存使用信息来决定杀死哪个进程。临时统计到当前进程避免遗漏。
 */
/**
 * acct_arg_size - 统计参数页面数量
 * @bprm: 二进制程序参数结构
 * @pages: 当前页面数
 *
 * 【参数传递内存】
 * execve()的参数（argv/envp）需要复制到新进程的栈区。
 * 此函数跟踪使用了多少页面，用于OOM评估。
 */
static void acct_arg_size(struct linux_binprm *bprm, unsigned long pages)
{
	struct mm_struct *mm = current->mm;
	long diff = (long)(pages - bprm->vma_pages);

	if (!mm || !diff)
		return;

	bprm->vma_pages = pages;
	add_mm_counter(mm, MM_ANONPAGES, diff);
}

/**
 * get_arg_page - 获取参数页面
 * @bprm: 二进制程序参数结构
 * @pos: 页面位置（地址）
 * @write: 是否需要写权限
 *
 * 返回值：页面指针，失败返回NULL
 *
 * 【参数传递机制】
 * 为execve()的参数分配和获取页面。参数字符串需要从旧进程
 * 复制到新进程的地址空间。
 */
static struct page *get_arg_page(struct linux_binprm *bprm, unsigned long pos,
		int write)
{
	struct page *page;
	struct vm_area_struct *vma = bprm->vma;
	struct mm_struct *mm = bprm->mm;
	int ret;

	/*
	 * Avoid relying on expanding the stack down in GUP (which
	 * does not work for STACK_GROWSUP anyway), and just do it
	 * ahead of time.
	 */
	/**
	 * 避免依赖GUP中向下扩展栈（对STACK_GROWSUP无效），
	 * 提前完成扩展。
	 *
	 * 【栈扩展】
	 * 大多数架构栈向下增长，但某些架构（如PA-RISC）栈向上增长。
	 * 这里显式处理栈扩展，而不是依赖get_user_pages的副作用。
	 */
	if (!mmap_read_lock_maybe_expand(mm, vma, pos, write))
		return NULL;

	/*
	 * We are doing an exec().  'current' is the process
	 * doing the exec and 'mm' is the new process's mm.
	 */
	/**
	 * 我们正在执行exec()。'current'是执行exec的进程，
	 * 'mm'是新进程的mm。
	 *
	 * 【双重mm】
	 * exec期间存在两个mm：
	 * - current->mm: 旧进程的内存空间（仍在使用）
	 * - bprm->mm: 新进程的内存空间（正在构建）
	 */
	ret = get_user_pages_remote(mm, pos, 1,
			write ? FOLL_WRITE : 0,
			&page, NULL);
	mmap_read_unlock(mm);
	if (ret <= 0)
		return NULL;

	if (write)
		acct_arg_size(bprm, vma_pages(vma));

	return page;
}

/**
 * put_arg_page - 释放参数页面
 * @page: 页面指针
 *
 * 减少页面引用计数
 */
static void put_arg_page(struct page *page)
{
	put_page(page);
}

/**
 * free_arg_pages - 释放所有参数页面
 * @bprm: 二进制程序参数结构
 *
 * 【CONFIG_MMU版本】
 * 有MMU时，页面由mm管理，无需显式释放
 */
static void free_arg_pages(struct linux_binprm *bprm)
{
}

/**
 * flush_arg_page - 刷新参数页面的缓存
 * @bprm: 二进制程序参数结构
 * @pos: 页面位置
 * @page: 页面指针
 *
 * 【缓存一致性】
 * 确保CPU缓存与内存一致，对于某些架构（如ARM）必需
 */
static void flush_arg_page(struct linux_binprm *bprm, unsigned long pos,
		struct page *page)
{
	flush_cache_page(bprm->vma, pos, page_to_pfn(page));
}

/**
 * valid_arg_len - 验证参数长度是否有效
 * @bprm: 二进制程序参数结构
 * @len: 参数长度
 *
 * 返回值：true=有效，false=超长
 *
 * 【参数长度限制】
 * 单个参数字符串不能超过MAX_ARG_STRLEN（通常32页）
 */
static bool valid_arg_len(struct linux_binprm *bprm, long len)
{
	return len <= MAX_ARG_STRLEN;
}

#else

/**
 * 【无MMU系统的实现】
 *
 * 无MMU的嵌入式系统（如某些ARM Cortex-M）没有虚拟内存管理单元，
 * 需要不同的参数传递实现。
 */

/**
 * acct_arg_size - 统计参数页面（无MMU版本）
 *
 * 无MMU系统不需要统计，空实现
 */
static inline void acct_arg_size(struct linux_binprm *bprm, unsigned long pages)
{
}

/**
 * get_arg_page - 获取参数页面（无MMU版本）
 * @bprm: 二进制程序参数结构
 * @pos: 页面位置
 * @write: 是否需要写权限
 *
 * 返回值：页面指针，失败返回NULL
 *
 * 【无MMU实现】
 * 直接从bprm->page数组分配页面，无需复杂的虚拟地址映射
 */
static struct page *get_arg_page(struct linux_binprm *bprm, unsigned long pos,
		int write)
{
	struct page *page;

	page = bprm->page[pos / PAGE_SIZE];
	if (!page && write) {
		page = alloc_page(GFP_HIGHUSER|__GFP_ZERO);
		if (!page)
			return NULL;
		bprm->page[pos / PAGE_SIZE] = page;
	}

	return page;
}

/**
 * put_arg_page - 释放参数页面（无MMU版本）
 * @page: 页面指针
 *
 * 无MMU版本空实现，页面通过free_arg_pages批量释放
 */
static void put_arg_page(struct page *page)
{
}

/**
 * free_arg_page - 释放单个参数页面（无MMU版本）
 * @bprm: 二进制程序参数结构
 * @i: 页面索引
 */
static void free_arg_page(struct linux_binprm *bprm, int i)
{
	if (bprm->page[i]) {
		__free_page(bprm->page[i]);
		bprm->page[i] = NULL;
	}
}

/**
 * free_arg_pages - 释放所有参数页面（无MMU版本）
 * @bprm: 二进制程序参数结构
 *
 * 遍历并释放所有分配的参数页面
 */
static void free_arg_pages(struct linux_binprm *bprm)
{
	int i;

	for (i = 0; i < MAX_ARG_PAGES; i++)
		free_arg_page(bprm, i);
}

/**
 * flush_arg_page - 刷新参数页面缓存（无MMU版本）
 *
 * 无MMU系统无需缓存刷新，空实现
 */
static void flush_arg_page(struct linux_binprm *bprm, unsigned long pos,
		struct page *page)
{
}

/**
 * valid_arg_len - 验证参数长度（无MMU版本）
 * @bprm: 二进制程序参数结构
 * @len: 参数长度
 *
 * 返回值：true=有效，false=超长
 *
 * 【无MMU限制】
 * 无MMU系统参数长度不能超过bprm->p（剩余空间）
 */
static bool valid_arg_len(struct linux_binprm *bprm, long len)
{
	return len <= bprm->p;
}

#endif /* CONFIG_MMU */

/*
 * Create a new mm_struct and populate it with a temporary stack
 * vm_area_struct.  We don't have enough context at this point to set the stack
 * flags, permissions, and offset, so we use temporary values.  We'll update
 * them later in setup_arg_pages().
 */
/**
 * 创建新的mm_struct并填充临时的栈vm_area_struct。
 * 此时我们没有足够的上下文来设置栈的标志、权限和偏移，
 * 所以使用临时值。稍后在setup_arg_pages()中更新它们。
 *
 * 【exec的内存管理】
 * exec需要创建全新的进程地址空间，包括代码段、数据段、栈等。
 * 此函数初始化新的mm_struct和初始栈。
 */
/**
 * bprm_mm_init - 初始化二进制程序的内存管理结构
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【关键步骤】
 * 1. 分配新的mm_struct
 * 2. 保存用户命名空间
 * 3. 保存栈大小限制（RLIMIT_STACK）
 * 4. 创建初始栈VMA（有MMU）或设置栈指针（无MMU）
 */
static int bprm_mm_init(struct linux_binprm *bprm)
{
	int err;
	struct mm_struct *mm = NULL;

	bprm->mm = mm = mm_alloc();
	err = -ENOMEM;
	if (!mm)
		goto err;

	/* Staged for would_dump() narrowing; consumed by begin_new_exec(). */
	/* 为would_dump()缩小范围暂存；由begin_new_exec()消费 */
	bprm->user_ns = get_user_ns(current_user_ns());

	/* Save current stack limit for all calculations made during exec. */
	/* 保存当前栈限制，供exec期间的所有计算使用 */
	task_lock(current->group_leader);
	bprm->rlim_stack = current->signal->rlim[RLIMIT_STACK];
	task_unlock(current->group_leader);

#ifndef CONFIG_MMU
	/**
	 * 无MMU系统：直接设置栈指针到最大值
	 * MAX_ARG_PAGES个页面减去一个指针大小
	 */
	bprm->p = PAGE_SIZE * MAX_ARG_PAGES - sizeof(void *);
#else
	/**
	 * 有MMU系统：创建初始栈VMA
	 * 这是一个临时的栈区域，用于存放参数和环境变量
	 */
	err = create_init_stack_vma(bprm->mm, &bprm->vma, &bprm->p);
	if (err)
		goto err;
#endif

	return 0;

err:
	if (mm) {
		bprm->mm = NULL;
		mmdrop(mm);
	}

	return err;
}

/**
 * struct user_arg_ptr - 用户空间参数指针包装器
 *
 * 【兼容性处理】
 * 支持32位程序在64位内核上运行（compat模式）。
 * 32位指针和64位指针大小不同，需要区分处理。
 */
struct user_arg_ptr {
#ifdef CONFIG_COMPAT
	bool is_compat;  /* 是否是32位兼容模式 */
#endif
	union {
		const char __user *const __user *native;  /* 64位原生指针 */
#ifdef CONFIG_COMPAT
		const compat_uptr_t __user *compat;       /* 32位兼容指针 */
#endif
	} ptr;
};

/**
 * get_user_arg_ptr - 从用户空间获取参数指针
 * @argv: 用户参数指针包装器
 * @nr: 参数索引
 *
 * 返回值：用户空间字符串指针，失败返回ERR_PTR
 *
 * 【兼容性】
 * 根据is_compat标志选择正确的指针类型：
 * - 32位模式：compat_uptr_t（4字节）
 * - 64位模式：char *（8字节）
 */
static const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr)
{
	const char __user *native;

#ifdef CONFIG_COMPAT
	if (unlikely(argv.is_compat)) {
		compat_uptr_t compat;

		if (get_user(compat, argv.ptr.compat + nr))
			return ERR_PTR(-EFAULT);

		return compat_ptr(compat);
	}
#endif

	if (get_user(native, argv.ptr.native + nr))
		return ERR_PTR(-EFAULT);

	return native;
}

/*
 * count() counts the number of strings in array ARGV.
 */
/**
 * count()统计数组ARGV中的字符串数量
 */
/**
 * count - 统计参数数量
 * @argv: 用户参数指针
 * @max: 最大参数数量限制
 *
 * 返回值：参数数量，失败返回负错误码
 *
 * 【参数数组】
 * argv/envp是以NULL结尾的指针数组，类似char *argv[]。
 * 此函数遍历数组直到遇到NULL。
 */
static int count(struct user_arg_ptr argv, int max)
{
	int i = 0;

	if (argv.ptr.native != NULL) {
		for (;;) {
			const char __user *p = get_user_arg_ptr(argv, i);

			if (!p)
				break;

			if (IS_ERR(p))
				return -EFAULT;

			if (i >= max)
				return -E2BIG;  /* 参数太多 */
			++i;

			if (fatal_signal_pending(current))
				return -ERESTARTNOHAND;  /* 收到致命信号，中断 */
			cond_resched();  /* 可能需要调度，避免长时间占用CPU */
		}
	}
	return i;
}

/**
 * count_strings_kernel - 统计内核参数数量
 * @argv: 内核空间参数数组
 *
 * 返回值：参数数量，失败返回负错误码
 *
 * 【内核调用exec】
 * 内核自己也可能调用exec（如init进程），参数直接在内核空间，
 * 无需用户空间访问的复杂性。
 */
static int count_strings_kernel(const char *const *argv)
{
	int i;

	if (!argv)
		return 0;

	for (i = 0; argv[i]; ++i) {
		if (i >= MAX_ARG_STRINGS)
			return -E2BIG;
		if (fatal_signal_pending(current))
			return -ERESTARTNOHAND;
		cond_resched();
	}
	return i;
}

/**
 * bprm_set_stack_limit - 设置栈空间限制
 * @bprm: 二进制程序参数结构
 * @limit: 限制大小（字节）
 *
 * 返回值：0=成功，-E2BIG=参数太多超出限制
 *
 * 【栈空间管理】
 * 参数和环境变量存放在栈上，需要确保不超出栈大小限制。
 * bprm->argmin标记最低可用栈地址。
 */
static inline int bprm_set_stack_limit(struct linux_binprm *bprm,
				       unsigned long limit)
{
#ifdef CONFIG_MMU
	/* Avoid a pathological bprm->p. */
	/* 避免病态的bprm->p值 */
	if (bprm->p < limit)
		return -E2BIG;
	bprm->argmin = bprm->p - limit;
#endif
	return 0;
}

/**
 * bprm_hit_stack_limit - 检查是否触及栈限制
 * @bprm: 二进制程序参数结构
 *
 * 返回值：true=超出限制，false=未超出
 */
static inline bool bprm_hit_stack_limit(struct linux_binprm *bprm)
{
#ifdef CONFIG_MMU
	return bprm->p < bprm->argmin;
#else
	return false;
#endif
}

/*
 * Calculate bprm->argmin from:
 * - _STK_LIM
 * - ARG_MAX
 * - bprm->rlim_stack.rlim_cur
 * - bprm->argc
 * - bprm->envc
 * - bprm->p
 */
/**
 * 从以下因素计算bprm->argmin：
 * - _STK_LIM（栈限制常量）
 * - ARG_MAX（参数最大大小）
 * - bprm->rlim_stack.rlim_cur（进程栈资源限制）
 * - bprm->argc（参数数量）
 * - bprm->envc（环境变量数量）
 * - bprm->p（当前栈指针）
 */
/**
 * bprm_stack_limits - 计算并设置栈空间限制
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，-E2BIG=参数太多
 *
 * 【栈空间分配策略】
 * 需要在以下需求间平衡：
 * 1. argv/envp字符串需要空间
 * 2. binfmt加载器需要栈空间工作
 * 3. 程序本身需要合理的栈空间
 */
static int bprm_stack_limits(struct linux_binprm *bprm)
{
	unsigned long limit, ptr_size;

	/*
	 * Limit to 1/4 of the max stack size or 3/4 of _STK_LIM
	 * (whichever is smaller) for the argv+env strings.
	 * This ensures that:
	 *  - the remaining binfmt code will not run out of stack space,
	 *  - the program will have a reasonable amount of stack left
	 *    to work from.
	 */
	/**
	 * argv+env字符串限制为max栈大小的1/4或_STK_LIM的3/4（取较小值）。
	 * 这确保：
	 *  - 剩余的binfmt代码不会耗尽栈空间
	 *  - 程序有合理的栈空间可用
	 *
	 * 【为什么3/4和1/4】
	 * - 3/4 _STK_LIM: 为参数留足空间，但保留1/4给程序
	 * - 1/4 rlim_cur: 如果用户设置了很大的栈限制，参数也不能占太多
	 */
	limit = _STK_LIM / 4 * 3;
	limit = min(limit, bprm->rlim_stack.rlim_cur / 4);
	/*
	 * We've historically supported up to 32 pages (ARG_MAX)
	 * of argument strings even with small stacks
	 */
	/**
	 * 历史上即使栈很小，我们也支持最多32页（ARG_MAX）的参数字符串
	 *
	 * 【兼容性】
	 * ARG_MAX = 32 * PAGE_SIZE = 128KB（4KB页面）
	 * 即使进程栈限制很小，也要保证这个最小值
	 */
	limit = max_t(unsigned long, limit, ARG_MAX);
	/* Reject totally pathological counts. */
	/* 拒绝完全病态的计数 */
	if (bprm->argc < 0 || bprm->envc < 0)
		return -E2BIG;
	/*
	 * We must account for the size of all the argv and envp pointers to
	 * the argv and envp strings, since they will also take up space in
	 * the stack. They aren't stored until much later when we can't
	 * signal to the parent that the child has run out of stack space.
	 * Instead, calculate it here so it's possible to fail gracefully.
	 *
	 * In the case of argc = 0, make sure there is space for adding a
	 * empty string (which will bump argc to 1), to ensure confused
	 * userspace programs don't start processing from argv[1], thinking
	 * argc can never be 0, to keep them from walking envp by accident.
	 * See do_execveat_common().
	 */
	/**
	 * 必须计算所有argv和envp指针的大小，因为它们也会占用栈空间。
	 * 指针数组要到很晚才存储，那时无法通知父进程子进程栈空间不足。
	 * 所以在这里计算，以便优雅地失败。
	 *
	 * 对于argc=0的情况，确保有空间添加空字符串（会将argc增加到1），
	 * 以确保困惑的用户空间程序不会从argv[1]开始处理，认为argc永远
	 * 不为0，防止它们意外遍历envp。见do_execveat_common()。
	 *
	 * 【指针数组开销】
	 * 除了字符串本身，还需要存储指向它们的指针数组：
	 * char *argv[argc+1];  // +1是NULL结尾
	 * char *envp[envc+1];
	 * 在64位系统上，每个指针8字节
	 */
	if (check_add_overflow(max(bprm->argc, 1), bprm->envc, &ptr_size) ||
	    check_mul_overflow(ptr_size, sizeof(void *), &ptr_size))
		return -E2BIG;
	if (limit <= ptr_size)
		return -E2BIG;
	limit -= ptr_size;

	return bprm_set_stack_limit(bprm, limit);
}

/*
 * 'copy_strings()' copies argument/environment strings from the old
 * processes's memory to the new process's stack.  The call to get_user_pages()
 * ensures the destination page is created and not swapped out.
 */
/**
 * copy_strings()从旧进程的内存复制参数/环境字符串到新进程的栈。
 * 调用get_user_pages()确保目标页面被创建且未被换出。
 *
 * 【为什么要复制】
 * exec会完全替换进程地址空间，旧内存会被释放，所以必须先把
 * 参数复制到新的栈空间。
 */
/**
 * copy_strings - 复制参数/环境字符串到新进程栈
 * @argc: 字符串数量
 * @argv: 用户空间字符串指针数组
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【复制过程】
 * 从后往前复制（argc-1到0），因为栈向下增长。
 * 使用页面缓存避免频繁映射/解映射。
 */
static int copy_strings(int argc, struct user_arg_ptr argv,
			struct linux_binprm *bprm)
{
	struct page *kmapped_page = NULL;
	char *kaddr = NULL;
	unsigned long kpos = 0;
	int ret;

	while (argc-- > 0) {
		const char __user *str;
		int len;
		unsigned long pos;

		ret = -EFAULT;
		str = get_user_arg_ptr(argv, argc);
		if (IS_ERR(str))
			goto out;

		len = strnlen_user(str, MAX_ARG_STRLEN);
		if (!len)
			goto out;

		ret = -E2BIG;
		if (!valid_arg_len(bprm, len))
			goto out;

		/* We're going to work our way backwards. */
		/* 我们将反向工作（从后往前） */
		pos = bprm->p;
		str += len;
		bprm->p -= len;
		if (bprm_hit_stack_limit(bprm))
			goto out;

		while (len > 0) {
			int offset, bytes_to_copy;

			if (fatal_signal_pending(current)) {
				ret = -ERESTARTNOHAND;
				goto out;
			}
			cond_resched();

			offset = pos % PAGE_SIZE;
			if (offset == 0)
				offset = PAGE_SIZE;

			bytes_to_copy = offset;
			if (bytes_to_copy > len)
				bytes_to_copy = len;

			offset -= bytes_to_copy;
			pos -= bytes_to_copy;
			str -= bytes_to_copy;
			len -= bytes_to_copy;

			if (!kmapped_page || kpos != (pos & PAGE_MASK)) {
				struct page *page;

				page = get_arg_page(bprm, pos, 1);
				if (!page) {
					ret = -E2BIG;
					goto out;
				}

				if (kmapped_page) {
					flush_dcache_page(kmapped_page);
					kunmap_local(kaddr);
					put_arg_page(kmapped_page);
				}
				kmapped_page = page;
				kaddr = kmap_local_page(kmapped_page);
				kpos = pos & PAGE_MASK;
				flush_arg_page(bprm, kpos, kmapped_page);
			}
			if (copy_from_user(kaddr+offset, str, bytes_to_copy)) {
				ret = -EFAULT;
				goto out;
			}
		}
	}
	ret = 0;
out:
	if (kmapped_page) {
		flush_dcache_page(kmapped_page);
		kunmap_local(kaddr);
		put_arg_page(kmapped_page);
	}
	return ret;
}

/*
 * Copy and argument/environment string from the kernel to the processes stack.
 */
/**
 * 从内核复制参数/环境字符串到进程栈
 */
/**
 * copy_string_kernel - 复制内核字符串到进程栈
 * @arg: 内核空间字符串
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【内核调用exec】
 * 当内核调用exec（如启动init）时，参数在内核空间，
 * 直接使用memcpy而非copy_from_user。
 */
int copy_string_kernel(const char *arg, struct linux_binprm *bprm)
{
	int len = strnlen(arg, MAX_ARG_STRLEN) + 1 /* terminating NUL */;
	unsigned long pos = bprm->p;

	if (len == 0)
		return -EFAULT;
	if (!valid_arg_len(bprm, len))
		return -E2BIG;

	/* We're going to work our way backwards. */
	/* 我们将反向工作 */
	arg += len;
	bprm->p -= len;
	if (bprm_hit_stack_limit(bprm))
		return -E2BIG;

	while (len > 0) {
		unsigned int bytes_to_copy = min(len,
				min_not_zero(offset_in_page(pos), PAGE_SIZE));
		struct page *page;

		pos -= bytes_to_copy;
		arg -= bytes_to_copy;
		len -= bytes_to_copy;

		page = get_arg_page(bprm, pos, 1);
		if (!page)
			return -E2BIG;
		flush_arg_page(bprm, pos & PAGE_MASK, page);
		memcpy_to_page(page, offset_in_page(pos), arg, bytes_to_copy);
		put_arg_page(page);
	}

	return 0;
}
EXPORT_SYMBOL(copy_string_kernel);

/**
 * copy_strings_kernel - 复制多个内核字符串到进程栈
 * @argc: 字符串数量
 * @argv: 内核空间字符串数组
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 */
static int copy_strings_kernel(int argc, const char *const *argv,
			       struct linux_binprm *bprm)
{
	while (argc-- > 0) {
		int ret = copy_string_kernel(argv[argc], bprm);
		if (ret < 0)
			return ret;
		if (fatal_signal_pending(current))
			return -ERESTARTNOHAND;
		cond_resched();
	}
	return 0;
}

#ifdef CONFIG_MMU

/*
 * Finalizes the stack vm_area_struct. The flags and permissions are updated,
 * the stack is optionally relocated, and some extra space is added.
 */
/**
 * 完成栈vm_area_struct。更新标志和权限，可选地重定位栈，并添加额外空间。
 *
 * 【栈设置】
 * 这是exec的最后阶段之一，将临时栈VMA转换为最终的栈配置。
 */
/**
 * setup_arg_pages - 设置参数页面和最终栈
 * @bprm: 二进制程序参数结构
 * @stack_top: 栈顶地址
 * @executable_stack: 栈可执行性标志
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【关键操作】
 * 1. 计算栈大小和位置（可能随机化）
 * 2. 设置栈权限（可执行/不可执行）
 * 3. 重定位栈到最终位置
 * 4. 设置栈扩展区域
 */
int setup_arg_pages(struct linux_binprm *bprm,
		    unsigned long stack_top,
		    int executable_stack)
{
	int ret;
	unsigned long stack_shift;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = bprm->vma;
	struct vm_area_struct *prev = NULL;
	vm_flags_t vm_flags;
	unsigned long stack_base;
	unsigned long stack_size;
	unsigned long stack_expand;
	unsigned long rlim_stack;
	struct mmu_gather tlb;
	struct vma_iterator vmi;

#ifdef CONFIG_STACK_GROWSUP
	/**
	 * 【向上增长的栈】
	 * 某些架构（如PA-RISC）的栈向上增长
	 */
	/* Limit stack size */
	/* 限制栈大小 */
	stack_base = bprm->rlim_stack.rlim_max;

	stack_base = calc_max_stack_size(stack_base);

	/* Add space for stack randomization. */
	/* 为栈随机化添加空间 */
	if (current->flags & PF_RANDOMIZE)
		stack_base += (STACK_RND_MASK << PAGE_SHIFT);

	/* Make sure we didn't let the argument array grow too large. */
	/* 确保参数数组没有增长过大 */
	if (vma->vm_end - vma->vm_start > stack_base)
		return -ENOMEM;

	stack_base = PAGE_ALIGN(stack_top - stack_base);

	stack_shift = vma->vm_start - stack_base;
	mm->arg_start = bprm->p - stack_shift;
	bprm->p = vma->vm_end - stack_shift;
#else
	/**
	 * 【向下增长的栈】
	 * 大多数架构（x86、ARM等）的栈向下增长
	 */
	stack_top = arch_align_stack(stack_top);
	stack_top = PAGE_ALIGN(stack_top);

	if (unlikely(stack_top < mmap_min_addr) ||
	    unlikely(vma->vm_end - vma->vm_start >= stack_top - mmap_min_addr))
		return -ENOMEM;

	stack_shift = vma->vm_end - stack_top;

	bprm->p -= stack_shift;
	mm->arg_start = bprm->p;
#endif

	bprm->exec -= stack_shift;

	if (mmap_write_lock_killable(mm))
		return -EINTR;

	vm_flags = VM_STACK_FLAGS;

	/*
	 * Adjust stack execute permissions; explicitly enable for
	 * EXSTACK_ENABLE_X, disable for EXSTACK_DISABLE_X and leave alone
	 * (arch default) otherwise.
	 */
	/**
	 * 调整栈执行权限；对EXSTACK_ENABLE_X显式启用，
	 * 对EXSTACK_DISABLE_X禁用，否则保持原样（架构默认）。
	 *
	 * 【可执行栈】
	 * 现代系统通常禁止栈执行（NX/DEP保护），防止栈溢出攻击。
	 * 但某些程序（如含蹦床trampolines的GCC嵌套函数）需要可执行栈。
	 * ELF的GNU_STACK段标记栈是否需要可执行。
	 */
	if (unlikely(executable_stack == EXSTACK_ENABLE_X))
		vm_flags |= VM_EXEC;
	else if (executable_stack == EXSTACK_DISABLE_X)
		vm_flags &= ~VM_EXEC;
	vm_flags |= mm->def_flags;
	vm_flags |= VM_STACK_INCOMPLETE_SETUP;

	vma_iter_init(&vmi, mm, vma->vm_start);

	tlb_gather_mmu(&tlb, mm);
	ret = mprotect_fixup(&vmi, &tlb, vma, &prev, vma->vm_start, vma->vm_end,
			vm_flags);
	tlb_finish_mmu(&tlb);

	if (ret)
		goto out_unlock;
	BUG_ON(prev != vma);

	if (unlikely(vm_flags & VM_EXEC)) {
		pr_warn_once("process '%pD4' started with executable stack\n",
			     bprm->file);
	}

	/* Move stack pages down in memory. */
	/* 在内存中向下移动栈页面 */
	if (stack_shift) {
		/*
		 * During bprm_mm_init(), we create a temporary stack at STACK_TOP_MAX.  Once
		 * the binfmt code determines where the new stack should reside, we shift it to
		 * its final location.
		 */
		/**
		 * 在bprm_mm_init()期间，我们在STACK_TOP_MAX处创建临时栈。
		 * 一旦binfmt代码确定新栈应该位于何处，我们将其移动到最终位置。
		 *
		 * 【为什么需要移动】
		 * 初始化时不知道最终栈位置（可能需要随机化），所以先创建临时栈，
		 * 等确定位置后再移动。
		 */
		ret = relocate_vma_down(vma, stack_shift);
		if (ret)
			goto out_unlock;
	}

	/* mprotect_fixup is overkill to remove the temporary stack flags */
	/* mprotect_fixup用于移除临时栈标志有些过头 */
	vm_flags_clear(vma, VM_STACK_INCOMPLETE_SETUP);

	stack_expand = 131072UL; /* randomly 32*4k (or 2*64k) pages */
	                         /* 随意设为32*4k（或2*64k）页面 = 128KB */
	stack_size = vma->vm_end - vma->vm_start;
	/*
	 * Align this down to a page boundary as expand_stack
	 * will align it up.
	 */
	/**
	 * 向下对齐到页面边界，因为expand_stack会向上对齐。
	 */
	rlim_stack = bprm->rlim_stack.rlim_cur & PAGE_MASK;

	stack_expand = min(rlim_stack, stack_size + stack_expand);

#ifdef CONFIG_STACK_GROWSUP
	stack_base = vma->vm_start + stack_expand;
#else
	stack_base = vma->vm_end - stack_expand;
#endif
	current->mm->start_stack = bprm->p;
	ret = expand_stack_locked(vma, stack_base);
	if (ret)
		ret = -EFAULT;

out_unlock:
	mmap_write_unlock(mm);
	return ret;
}
EXPORT_SYMBOL(setup_arg_pages);

#else

/*
 * Transfer the program arguments and environment from the holding pages
 * onto the stack. The provided stack pointer is adjusted accordingly.
 */
/**
 * 将程序参数和环境从暂存页面传输到栈上。
 * 相应地调整提供的栈指针。
 *
 * 【无MMU版本】
 * 无MMU系统的参数传输更简单，直接从bprm->page数组复制到栈。
 */
/**
 * transfer_args_to_stack - 传输参数到栈（无MMU版本）
 * @bprm: 二进制程序参数结构
 * @sp_location: 栈指针位置（输入/输出）
 *
 * 返回值：0=成功，负数=错误码
 */
int transfer_args_to_stack(struct linux_binprm *bprm,
			   unsigned long *sp_location)
{
	unsigned long index, stop, sp;
	int ret = 0;

	stop = bprm->p >> PAGE_SHIFT;
	sp = *sp_location;

	for (index = MAX_ARG_PAGES - 1; index >= stop; index--) {
		unsigned int offset = index == stop ? bprm->p & ~PAGE_MASK : 0;
		char *src = kmap_local_page(bprm->page[index]) + offset;
		sp -= PAGE_SIZE - offset;
		if (copy_to_user((void *) sp, src, PAGE_SIZE - offset) != 0)
			ret = -EFAULT;
		kunmap_local(src);
		if (ret)
			goto out;
	}

	bprm->exec += *sp_location - MAX_ARG_PAGES * PAGE_SIZE;
	*sp_location = sp;

out:
	return ret;
}
EXPORT_SYMBOL(transfer_args_to_stack);

#endif /* CONFIG_MMU */

/*
 * On success, caller must call do_close_execat() on the returned
 * struct file to close it.
 */
/**
 * 成功时，调用者必须对返回的struct file调用do_close_execat()来关闭它。
 */
/**
 * do_open_execat - 打开可执行文件
 * @fd: 文件描述符（AT_FDCWD表示当前目录）
 * @name: 文件名
 * @flags: 打开标志
 *
 * 返回值：文件指针，失败返回ERR_PTR
 *
 * 【安全检查】
 * 1. 检查文件是否在noexec挂载点
 * 2. 确保文件是普通文件
 * 3. 阻止对可执行文件的写访问（防止TOCTOU攻击）
 */
static struct file *do_open_execat(int fd, struct filename *name, int flags)
{
	int err;
	struct file *file __free(fput) = NULL;
	struct open_flags open_exec_flags = {
		.open_flag = O_LARGEFILE | O_RDONLY | __FMODE_EXEC,
		.acc_mode = MAY_EXEC,
		.intent = LOOKUP_OPEN,
		.lookup_flags = LOOKUP_FOLLOW,
	};

	if ((flags &
	     ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_EXECVE_CHECK)) != 0)
		return ERR_PTR(-EINVAL);
	if (flags & AT_SYMLINK_NOFOLLOW)
		open_exec_flags.lookup_flags &= ~LOOKUP_FOLLOW;

	file = do_file_open(fd, name, &open_exec_flags);
	if (IS_ERR(file))
		return file;

	if (path_noexec(&file->f_path))
		return ERR_PTR(-EACCES);

	/*
	 * In the past the regular type check was here. It moved to may_open() in
	 * 633fb6ac3980 ("exec: move S_ISREG() check earlier"). Since then it is
	 * an invariant that all non-regular files error out before we get here.
	 */
	/**
	 * 过去常规类型检查在这里。它在633fb6ac3980中移到了may_open()。
	 * 从那时起，所有非常规文件都会在到达这里之前报错，这是一个不变量。
	 *
	 * 【为什么只能是普通文件】
	 * 只有常规文件可以exec，不能exec目录、设备、管道等。
	 */
	if (WARN_ON_ONCE(!S_ISREG(file_inode(file)->i_mode)))
		return ERR_PTR(-EACCES);

	err = exe_file_deny_write_access(file);
	if (err)
		return ERR_PTR(err);

	return no_free_ptr(file);
}

/**
 * open_exec - Open a path name for execution
 *
 * @name: path name to open with the intent of executing it.
 *
 * Returns ERR_PTR on failure or allocated struct file on success.
 *
 * As this is a wrapper for the internal do_open_execat(), callers
 * must call exe_file_allow_write_access() before fput() on release. Also see
 * do_close_execat().
 */
/**
 * open_exec - 打开路径名用于执行
 *
 * @name: 要打开并执行的路径名
 *
 * 返回值：成功返回分配的struct file，失败返回ERR_PTR
 *
 * 作为内部do_open_execat()的包装器，调用者必须在释放时
 * 先调用exe_file_allow_write_access()再fput()。另见do_close_execat()。
 */
struct file *open_exec(const char *name)
{
	CLASS(filename_kernel, filename)(name);
	return do_open_execat(AT_FDCWD, filename, 0);
}
EXPORT_SYMBOL(open_exec);

#if defined(CONFIG_BINFMT_FLAT) || defined(CONFIG_BINFMT_ELF_FDPIC)
/**
 * read_code - 读取代码段到用户空间
 * @file: 可执行文件
 * @addr: 用户空间地址
 * @pos: 文件偏移
 * @len: 读取长度
 *
 * 返回值：读取的字节数，负数=错误码
 *
 * 【刷新指令缓存】
 * 某些架构（如ARM）有独立的指令缓存和数据缓存，
 * 加载代码后需要刷新指令缓存，否则CPU可能执行旧指令。
 */
ssize_t read_code(struct file *file, unsigned long addr, loff_t pos, size_t len)
{
	ssize_t res = vfs_read(file, (void __user *)addr, len, &pos);
	if (res > 0)
		flush_icache_user_range(addr, addr + len);
	return res;
}
EXPORT_SYMBOL(read_code);
#endif

/*
 * Maps the mm_struct mm into the current task struct.
 * On success, this function returns with exec_update_lock
 * held for writing. The replaced address space is stashed in
 * bprm->old_mm for setup_new_exec() to release outside the lock.
 */
/**
 * 将mm_struct映射到当前任务结构。
 * 成功时，此函数返回时持有exec_update_lock写锁。
 * 被替换的地址空间暂存在bprm->old_mm中，供setup_new_exec()在锁外释放。
 *
 * 【exec的地址空间切换】
 * 这是exec最关键的一步：用新程序的地址空间替换旧程序的地址空间。
 * 之后进程仍在运行，但看到的是全新的内存布局。
 */
/**
 * exec_mmap - 切换到新的地址空间
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【关键步骤】
 * 1. 通知父进程不再关注旧VM
 * 2. 获取exec_update_lock（防止并发exec）
 * 3. 切换mm_struct
 * 4. 激活新地址空间
 */
static int exec_mmap(struct linux_binprm *bprm)
{
	struct task_exec_state *exec_state __free(put_task_exec_state) = NULL;
	struct mm_struct *mm = bprm->mm;
	struct task_struct *tsk;
	struct mm_struct *old_mm, *active_mm;
	int ret;

	exec_state = alloc_task_exec_state(bprm->user_ns);
	if (!exec_state)
		return -ENOMEM;

	/* Notify parent that we're no longer interested in the old VM */
	/* 通知父进程我们不再关注旧VM */
	tsk = current;
	old_mm = current->mm;
	exec_mm_release(tsk, old_mm);

	ret = down_write_killable(&tsk->signal->exec_update_lock);
	if (ret)
		return ret;

	if (old_mm) {
		/*
		 * If there is a pending fatal signal perhaps a signal
		 * whose default action is to create a coredump get
		 * out and die instead of going through with the exec.
		 */
		/**
		 * 如果有待处理的致命信号，也许是默认动作为创建coredump的信号，
		 * 那么退出并终止而不是继续exec。
		 *
		 * 【信号处理】
		 * exec期间如果收到致命信号（如SIGKILL），应该立即终止，
		 * 而不是完成exec然后再处理信号。
		 */
		ret = mmap_read_lock_killable(old_mm);
		if (ret) {
			up_write(&tsk->signal->exec_update_lock);
			return ret;
		}
	}

	task_lock(tsk);
	membarrier_exec_mmap(mm);

	local_irq_disable();
	active_mm = tsk->active_mm;
	tsk->active_mm = mm;
	tsk->mm = mm;
	mm_init_cid(mm, tsk);
	exec_state = task_exec_state_replace(tsk, exec_state);
	/*
	 * This prevents preemption while active_mm is being loaded and
	 * it and mm are being updated, which could cause problems for
	 * lazy tlb mm refcounting when these are updated by context
	 * switches. Not all architectures can handle irqs off over
	 * activate_mm yet.
	 */
	/**
	 * 这防止在加载active_mm以及更新它和mm时发生抢占，
	 * 这可能在上下文切换更新它们时导致lazy tlb mm引用计数问题。
	 * 并非所有架构都能处理activate_mm期间关中断。
	 *
	 * 【临界区保护】
	 * 切换地址空间是非常关键的操作，必须原子完成，
	 * 否则进程可能处于不一致状态。
	 */
	if (!IS_ENABLED(CONFIG_ARCH_WANT_IRQS_OFF_ACTIVATE_MM))
		local_irq_enable();
	activate_mm(active_mm, mm);
	if (IS_ENABLED(CONFIG_ARCH_WANT_IRQS_OFF_ACTIVATE_MM))
		local_irq_enable();
	lru_gen_add_mm(mm);
	task_unlock(tsk);
	lru_gen_use_mm(mm);
	if (old_mm) {
		mmap_read_unlock(old_mm);
		BUG_ON(active_mm != old_mm);
		/* Defer teardown to setup_new_exec(), outside the exec locks. */
		/* 延迟拆除到setup_new_exec()，在exec锁外 */
		bprm->old_mm = old_mm;
		return 0;
	}
	mmdrop_lazy_tlb(active_mm);
	return 0;
}

/* Release the address space replaced by exec, outside the exec locks. */
/**
 * 释放被exec替换的地址空间，在exec锁外。
 */
/**
 * exec_mm_put_old - 释放旧的地址空间
 * @old_mm: 旧的mm_struct
 *
 * 【延迟释放】
 * 在exec锁内切换地址空间，但延迟到锁外再释放旧地址空间，
 * 避免长时间持有锁。
 */
static void exec_mm_put_old(struct mm_struct *old_mm)
{
	setmax_mm_hiwater_rss(&current->signal->maxrss, old_mm);
	mm_update_next_owner(old_mm);
	mmput(old_mm);
}

/**
 * de_thread - 脱离线程组
 * @tsk: 当前任务
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【多线程exec】
 * 如果进程是多线程的，exec只在调用线程执行，其他线程被杀死。
 * exec后的进程变成单线程，成为新的线程组领导者。
 *
 * 【为什么要杀死其他线程】
 * exec替换整个地址空间，其他线程的栈、代码都会消失，
 * 它们无法继续运行，必须被终止。
 */
static int de_thread(struct task_struct *tsk)
{
	struct signal_struct *sig = tsk->signal;
	struct sighand_struct *oldsighand = tsk->sighand;
	spinlock_t *lock = &oldsighand->siglock;

	if (thread_group_empty(tsk))
		goto no_thread_group;

	/*
	 * Kill all other threads in the thread group.
	 */
	/**
	 * 杀死线程组中的所有其他线程。
	 */
	spin_lock_irq(lock);
	if ((sig->flags & SIGNAL_GROUP_EXIT) || sig->group_exec_task) {
		/*
		 * Another group action in progress, just
		 * return so that the signal is processed.
		 */
		/**
		 * 另一个组操作正在进行，只需返回以便处理信号。
		 *
		 * 【并发exec/exit】
		 * 如果有其他线程正在exec或整个进程组正在退出，
		 * 当前exec应该失败，让信号处理完成。
		 */
		spin_unlock_irq(lock);
		return -EAGAIN;
	}

	sig->group_exec_task = tsk;
	sig->notify_count = zap_other_threads(tsk);
	if (!thread_group_leader(tsk))
		sig->notify_count--;

	while (sig->notify_count) {
		__set_current_state(TASK_KILLABLE);
		spin_unlock_irq(lock);
		schedule();
		if (__fatal_signal_pending(tsk))
			goto killed;
		spin_lock_irq(lock);
	}
	spin_unlock_irq(lock);

	/*
	 * At this point all other threads have exited, all we have to
	 * do is to wait for the thread group leader to become inactive,
	 * and to assume its PID:
	 */
	/**
	 * 此时所有其他线程已退出，我们要做的就是等待线程组领导者
	 * 变为非活动状态，并接管其PID：
	 *
	 * 【PID继承】
	 * 如果非领导线程调用exec，它必须接管领导者的PID，
	 * 因为PID代表整个进程，不能改变。
	 */
	if (!thread_group_leader(tsk)) {
		struct task_struct *leader = tsk->group_leader;

		for (;;) {
			cgroup_threadgroup_change_begin(tsk);
			write_lock_irq(&tasklist_lock);
			/*
			 * Do this under tasklist_lock to ensure that
			 * exit_notify() can't miss ->group_exec_task
			 */
			/**
			 * 在tasklist_lock下执行，确保exit_notify()
			 * 不会错过->group_exec_task
			 */
			sig->notify_count = -1;
			if (likely(leader->exit_state))
				break;
			__set_current_state(TASK_KILLABLE);
			write_unlock_irq(&tasklist_lock);
			cgroup_threadgroup_change_end(tsk);
			schedule();
			if (__fatal_signal_pending(tsk))
				goto killed;
		}

		/*
		 * The only record we have of the real-time age of a
		 * process, regardless of execs it's done, is start_time.
		 * All the past CPU time is accumulated in signal_struct
		 * from sister threads now dead.  But in this non-leader
		 * exec, nothing survives from the original leader thread,
		 * whose birth marks the true age of this process now.
		 * When we take on its identity by switching to its PID, we
		 * also take its birthdate (always earlier than our own).
		 */
		/**
		 * 无论进程执行了多少次exec，我们唯一记录进程实时年龄的是start_time。
		 * 所有过去的CPU时间都从现已死亡的姊妹线程累积在signal_struct中。
		 * 但在这个非领导者exec中，原始领导线程没有任何遗留，
		 * 而其诞生标志着这个进程的真实年龄。
		 * 当我们通过切换到其PID来接管其身份时，
		 * 我们也接管其出生日期（总是早于我们自己的）。
		 *
		 * 【进程年龄】
		 * start_time是进程创建时间，即使exec多次也不变。
		 * 非领导线程exec时必须继承领导者的start_time，
		 * 保持进程年龄的一致性。
		 */
		tsk->start_time = leader->start_time;
		tsk->start_boottime = leader->start_boottime;

		BUG_ON(!same_thread_group(leader, tsk));
		/*
		 * An exec() starts a new thread group with the
		 * TGID of the previous thread group. Rehash the
		 * two threads with a switched PID, and release
		 * the former thread group leader:
		 */
		/**
		 * exec()启动一个新线程组，具有前一个线程组的TGID。
		 * 用切换的PID重新散列两个线程，并释放前线程组领导者：
		 */

		/* Become a process group leader with the old leader's pid.
		 * The old leader becomes a thread of the this thread group.
		 */
		/**
		 * 用旧领导者的pid成为进程组领导者。
		 * 旧领导者成为此线程组的线程。
		 *
		 * 【PID交换】
		 * exchange_tids交换两个线程的PID，让exec线程获得领导者的PID。
		 * transfer_pid转移TGID、PGID、SID，保持进程组和会话不变。
		 */
		exchange_tids(tsk, leader);
		transfer_pid(leader, tsk, PIDTYPE_TGID);
		transfer_pid(leader, tsk, PIDTYPE_PGID);
		transfer_pid(leader, tsk, PIDTYPE_SID);

		list_replace_rcu(&leader->tasks, &tsk->tasks);
		list_replace_init(&leader->sibling, &tsk->sibling);

		tsk->group_leader = tsk;
		leader->group_leader = tsk;

		tsk->exit_signal = SIGCHLD;
		leader->exit_signal = -1;

		BUG_ON(leader->exit_state != EXIT_ZOMBIE);
		leader->exit_state = EXIT_DEAD;
		/*
		 * We are going to release_task()->ptrace_unlink() silently,
		 * the tracer can sleep in do_wait(). EXIT_DEAD guarantees
		 * the tracer won't block again waiting for this thread.
		 */
		/**
		 * 我们将静默调用release_task()->ptrace_unlink()，
		 * 跟踪器可能在do_wait()中睡眠。EXIT_DEAD保证
		 * 跟踪器不会再次阻塞等待此线程。
		 */
		if (unlikely(leader->ptrace))
			__wake_up_parent(leader, leader->parent);
		write_unlock_irq(&tasklist_lock);
		cgroup_threadgroup_change_end(tsk);

		release_task(leader);
	}

	sig->group_exec_task = NULL;
	sig->notify_count = 0;

no_thread_group:
	/* we have changed execution domain */
	/* 我们已经改变了执行域 */
	tsk->exit_signal = SIGCHLD;

	BUG_ON(!thread_group_leader(tsk));
	return 0;

killed:
	/* protects against exit_notify() and __exit_signal() */
	/* 防止exit_notify()和__exit_signal() */
	read_lock(&tasklist_lock);
	sig->group_exec_task = NULL;
	sig->notify_count = 0;
	read_unlock(&tasklist_lock);
	return -EAGAIN;
}


/*
 * This function makes sure the current process has its own signal table,
 * so that flush_signal_handlers can later reset the handlers without
 * disturbing other processes.  (Other processes might share the signal
 * table via the CLONE_SIGHAND option to clone().)
 */
/**
 * 此函数确保当前进程有自己的信号表，
 * 以便flush_signal_handlers稍后可以重置处理程序而不干扰其他进程。
 * （其他进程可能通过clone()的CLONE_SIGHAND选项共享信号表。）
 *
 * 【信号处理器unshare】
 * exec会重置所有信号处理器到默认值，但如果信号表被共享，
 * 需要先复制一份，避免影响共享进程。
 */
/**
 * unshare_sighand - 取消共享信号处理器表
 * @me: 当前任务
 *
 * 返回值：0=成功，负数=错误码
 */
static int unshare_sighand(struct task_struct *me)
{
	struct sighand_struct *oldsighand = me->sighand;

	if (refcount_read(&oldsighand->count) != 1) {
		struct sighand_struct *newsighand;
		/*
		 * This ->sighand is shared with the CLONE_SIGHAND
		 * but not CLONE_THREAD task, switch to the new one.
		 */
		/**
		 * 此->sighand与CLONE_SIGHAND但非CLONE_THREAD任务共享，
		 * 切换到新的。
		 */
		newsighand = kmem_cache_alloc(sighand_cachep, GFP_KERNEL);
		if (!newsighand)
			return -ENOMEM;

		refcount_set(&newsighand->count, 1);

		write_lock_irq(&tasklist_lock);
		spin_lock(&oldsighand->siglock);
		memcpy(newsighand->action, oldsighand->action,
		       sizeof(newsighand->action));
		rcu_assign_pointer(me->sighand, newsighand);
		spin_unlock(&oldsighand->siglock);
		write_unlock_irq(&tasklist_lock);

		__cleanup_sighand(oldsighand);
	}
	return 0;
}

/*
 * This is unlocked -- the string will always be NUL-terminated, but
 * may show overlapping contents if racing concurrent reads.
 */
/**
 * 这是无锁的 -- 字符串总是以NUL结尾，
 * 但如果与并发读取竞争，可能显示重叠的内容。
 */
/**
 * __set_task_comm - 设置任务命令名
 * @tsk: 任务结构
 * @buf: 新命令名
 * @exec: 是否为exec调用
 *
 * 【进程名】
 * comm是进程名，显示在ps、top等工具中。
 * exec时会更新为新程序的名称。
 */
void __set_task_comm(struct task_struct *tsk, const char *buf, bool exec)
{
	size_t len = strnlen(buf, sizeof(tsk->comm) - 1);

	trace_task_rename(tsk, buf);
	memcpy(tsk->comm, buf, len);
	memset(&tsk->comm[len], 0, sizeof(tsk->comm) - len);
	perf_event_comm(tsk, exec);
}

/*
 * Calling this is the point of no return. None of the failures will be
 * seen by userspace since either the process is already taking a fatal
 * signal (via de_thread() or coredump), or will have SEGV raised
 * (after exec_mmap()) by search_binary_handler (see below).
 */
/**
 * 调用此函数是不归路。用户空间看不到任何失败，
 * 因为进程要么已经接收到致命信号（通过de_thread()或coredump），
 * 要么将由search_binary_handler（见下文）在exec_mmap()后引发SEGV。
 *
 * 【不归点】
 * 这是exec的关键转折点，之后的错误无法返回给用户空间，
 * 只能通过信号杀死进程。
 */
/**
 * begin_new_exec - 开始新程序执行
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【关键步骤】
 * 1. 计算新凭证（UID/GID/capabilities）
 * 2. 设置不归点标志
 * 3. 脱离线程组（de_thread）
 * 4. 取消共享信号处理器
 * 5. 重置信号和文件描述符
 */
int begin_new_exec(struct linux_binprm * bprm)
{
	struct task_struct *me = current;
	int retval;

	/* Once we are committed compute the creds */
	/* 一旦我们提交就计算凭证 */
	retval = bprm_creds_from_file(bprm);
	if (retval)
		return retval;

	/*
	 * This tracepoint marks the point before flushing the old exec where
	 * the current task is still unchanged, but errors are fatal (point of
	 * no return). The later "sched_process_exec" tracepoint is called after
	 * the current task has successfully switched to the new exec.
	 */
	/**
	 * 此跟踪点标记刷新旧exec之前的点，此时当前任务仍未改变，
	 * 但错误是致命的（不归点）。稍后的"sched_process_exec"跟踪点
	 * 在当前任务成功切换到新exec后调用。
	 */
	trace_sched_prepare_exec(current, bprm);

	/*
	 * Ensure all future errors are fatal.
	 */
	/**
	 * 确保所有未来的错误都是致命的。
	 */
	bprm->point_of_no_return = true;

	/* Make this the only thread in the thread group */
	/* 使这成为线程组中的唯一线程 */
	retval = de_thread(me);
	if (retval)
		goto out;
	/* see the comment in check_unsafe_exec() */
	/* 参见check_unsafe_exec()中的注释 */
	current->fs->in_exec = 0;
	/*
	 * Cancel any io_uring activity across execve
	 */
	/**
	 * 取消execve期间的任何io_uring活动
	 *
	 * 【io_uring清理】
	 * io_uring是异步I/O框架，exec时必须取消所有待处理操作，
	 * 因为它们引用的内存会被释放。
	 */
	io_uring_task_cancel();

	/* Ensure the files table is not shared. */
	/* 确保文件表不共享 */
	retval = unshare_files();
	if (retval)
		goto out;

	/*
	 * Must be called _before_ exec_mmap() as bprm->mm is
	 * not visible until then. Doing it here also ensures
	 * we don't race against replace_mm_exe_file().
	 */
	/**
	 * 必须在exec_mmap()_之前_调用，因为bprm->mm直到那时才可见。
	 * 在此处执行也确保我们不会与replace_mm_exe_file()竞争。
	 *
	 * 【可执行文件引用】
	 * mm->exe_file指向当前运行的可执行文件，用于/proc/pid/exe符号链接。
	 */
	retval = set_mm_exe_file(bprm->mm, bprm->file);
	if (retval)
		goto out;

	/* If the binary is not readable then enforce mm->dumpable=0 */
	/* 如果二进制文件不可读，则强制mm->dumpable=0 */
	/**
	 * 【coredump控制】
	 * 如果可执行文件不可读（如setuid程序），禁止生成coredump，
	 * 防止泄露敏感信息。
	 */
	would_dump(bprm, bprm->file);
	if (bprm->have_execfd)
		would_dump(bprm, bprm->executable);

	/*
	 * Release all of the old mmap stuff
	 */
	/**
	 * 释放所有旧的mmap内容
	 *
	 * 【地址空间切换】
	 * exec_mmap是最关键的步骤，用新程序的地址空间替换旧的。
	 * 之后进程看到的是全新的内存布局。
	 */
	acct_arg_size(bprm, 0);
	retval = exec_mmap(bprm);
	if (retval)
		goto out;

	bprm->mm = NULL;

	retval = exec_task_namespaces();
	if (retval)
		goto out_unlock;

#ifdef CONFIG_POSIX_TIMERS
	/**
	 * 【定时器清理】
	 * exec后清除所有POSIX定时器，因为它们属于旧程序，
	 * 新程序会创建自己的定时器。
	 */
	spin_lock_irq(&me->sighand->siglock);
	posix_cpu_timers_exit(me);
	spin_unlock_irq(&me->sighand->siglock);
	exit_itimers(me);
	flush_itimer_signals();
#endif

	/*
	 * Make the signal table private.
	 */
	/**
	 * 使信号表私有。
	 */
	retval = unshare_sighand(me);
	if (retval)
		goto out_unlock;

	me->flags &= ~(PF_RANDOMIZE | PF_FORKNOEXEC |
					PF_NOFREEZE | PF_NO_SETAFFINITY);
	flush_thread();
	me->personality &= ~bprm->per_clear;

	clear_syscall_work_syscall_user_dispatch(me);

	/*
	 * We have to apply CLOEXEC before we change whether the process is
	 * dumpable (in setup_new_exec) to avoid a race with a process in userspace
	 * trying to access the should-be-closed file descriptors of a process
	 * undergoing exec(2).
	 */
	/**
	 * 我们必须在改变进程是否可dump（在setup_new_exec中）之前应用CLOEXEC，
	 * 以避免与用户空间中试图访问正在exec(2)的进程的应关闭文件描述符的进程竞争。
	 *
	 * 【CLOEXEC】
	 * 带FD_CLOEXEC标志的文件描述符在exec时自动关闭。
	 * 这防止新程序继承不需要的文件描述符，避免安全漏洞。
	 */
	do_close_on_exec(me->files);

	if (bprm->secureexec) {
		/* Make sure parent cannot signal privileged process. */
		/* 确保父进程不能向特权进程发信号 */
		me->pdeath_signal = 0;

		/*
		 * For secureexec, reset the stack limit to sane default to
		 * avoid bad behavior from the prior rlimits. This has to
		 * happen before arch_pick_mmap_layout(), which examines
		 * RLIMIT_STACK, but after the point of no return to avoid
		 * needing to clean up the change on failure.
		 */
		/**
		 * 对于secureexec，将栈限制重置为合理默认值，
		 * 以避免来自先前rlimits的不良行为。这必须在arch_pick_mmap_layout()之前发生，
		 * 它检查RLIMIT_STACK，但在不归点之后以避免失败时需要清理更改。
		 *
		 * 【secureexec】
		 * setuid/setgid程序执行时，为安全重置资源限制，
		 * 防止非特权用户通过修改rlimit攻击特权程序。
		 */
		if (bprm->rlim_stack.rlim_cur > _STK_LIM)
			bprm->rlim_stack.rlim_cur = _STK_LIM;
	}

	me->sas_ss_sp = me->sas_ss_size = 0;

	/*
	 * Figure out dumpability. Note that this checking only of current
	 * is wrong, but userspace depends on it. This should be testing
	 * bprm->secureexec instead.
	 */
	/**
	 * 确定可dump性。注意，仅检查current是错误的，
	 * 但用户空间依赖于它。这应该改为测试bprm->secureexec。
	 *
	 * 【dumpable标志】
	 * 控制进程是否可以生成coredump和被ptrace。
	 * setuid/setgid程序通常禁止dump，防止泄露敏感信息。
	 */
	if (bprm->interp_flags & BINPRM_FLAGS_ENFORCE_NONDUMP ||
	    !(uid_eq(current_euid(), current_uid()) &&
	      gid_eq(current_egid(), current_gid())))
		task_exec_state_set_dumpable(suid_dumpable);
	else
		task_exec_state_set_dumpable(TASK_DUMPABLE_OWNER);

	perf_event_exec();

	/*
	 * If the original filename was empty, alloc_bprm() made up a path
	 * that will probably not be useful to admins running ps or similar.
	 * Let's fix it up to be something reasonable.
	 */
	/**
	 * 如果原始文件名为空，alloc_bprm()编造了一个路径，
	 * 对于运行ps或类似工具的管理员来说可能没有用。
	 * 让我们将其修正为合理的内容。
	 */
	if (bprm->comm_from_dentry) {
		/*
		 * Hold RCU lock to keep the name from being freed behind our back.
		 * Use acquire semantics to make sure the terminating NUL from
		 * __d_alloc() is seen.
		 *
		 * Note, we're deliberately sloppy here. We don't need to care about
		 * detecting a concurrent rename and just want a terminated name.
		 */
		/**
		 * 持有RCU锁以防止名称在我们背后被释放。
		 * 使用acquire语义确保看到来自__d_alloc()的终止NUL。
		 *
		 * 注意，我们在此故意马虎。我们不需要关心检测并发重命名，
		 * 只想要一个终止的名称。
		 */
		rcu_read_lock();
		__set_task_comm(me, smp_load_acquire(&bprm->file->f_path.dentry->d_name.name),
				true);
		rcu_read_unlock();
	} else {
		__set_task_comm(me, kbasename(bprm->filename), true);
	}

	/* An exec changes our domain. We are no longer part of the thread
	   group */
	/**
	 * exec改变了我们的域。我们不再是线程组的一部分
	 *
	 * 【self_exec_id】
	 * 每次exec都增加self_exec_id，用于检测进程是否已exec。
	 * 某些操作（如ptrace）需要确保目标进程未exec。
	 */
	WRITE_ONCE(me->self_exec_id, me->self_exec_id + 1);
	flush_signal_handlers(me, 0);

	retval = set_cred_ucounts(bprm->cred);
	if (retval < 0)
		goto out_unlock;

	/*
	 * install the new credentials for this executable
	 */
	/**
	 * 为此可执行文件安装新凭证
	 *
	 * 【凭证切换】
	 * 这是setuid/setgid生效的时刻，进程获得新的UID/GID和capabilities。
	 */
	security_bprm_committing_creds(bprm);

	commit_creds(bprm->cred);
	bprm->cred = NULL;

	/*
	 * Disable monitoring for regular users
	 * when executing setuid binaries. Must
	 * wait until new credentials are committed
	 * by commit_creds() above
	 */
	/**
	 * 执行setuid二进制文件时禁用常规用户的监控。
	 * 必须等到上面的commit_creds()提交新凭证
	 *
	 * 【perf安全】
	 * setuid程序执行时禁用perf监控，防止非特权用户
	 * 通过perf窥探特权进程的行为。
	 */
	if (task_exec_state_get_dumpable(me) != TASK_DUMPABLE_OWNER)
		perf_event_exit_task(me);
	/*
	 * cred_guard_mutex must be held at least to this point to prevent
	 * ptrace_attach() from altering our determination of the task's
	 * credentials; any time after this it may be unlocked.
	 */
	/**
	 * cred_guard_mutex必须至少持有到此点，
	 * 以防止ptrace_attach()改变我们对任务凭证的确定；
	 * 此后任何时候都可以解锁。
	 */
	security_bprm_committed_creds(bprm);

	/* Pass the opened binary to the interpreter. */
	/* 将打开的二进制文件传递给解释器 */
	/**
	 * 【脚本解释器】
	 * 如果是脚本（如#!/bin/sh），将脚本文件作为FD传递给解释器，
	 * 避免TOCTOU攻击（脚本在打开和执行之间被修改）。
	 */
	if (bprm->have_execfd) {
		retval = FD_ADD(0, bprm->executable);
		if (retval < 0)
			goto out_unlock;
		bprm->executable = NULL;
		bprm->execfd = retval;
	}
	return 0;

out_unlock:
	up_write(&me->signal->exec_update_lock);
	if (!bprm->cred)
		mutex_unlock(&me->signal->cred_guard_mutex);

out:
	return retval;
}
EXPORT_SYMBOL(begin_new_exec);

/**
 * would_dump - 检查文件是否可读并设置dump标志
 * @bprm: 二进制程序参数结构
 * @file: 要检查的文件
 *
 * 【可读性检查】
 * 如果可执行文件不可读（如只有执行权限的setuid程序），
 * 设置BINPRM_FLAGS_ENFORCE_NONDUMP标志，禁止coredump。
 */
void would_dump(struct linux_binprm *bprm, struct file *file)
{
	struct inode *inode = file_inode(file);
	struct mnt_idmap *idmap = file_mnt_idmap(file);
	if (inode_permission(idmap, inode, MAY_READ) < 0) {
		struct user_namespace *old, *user_ns;
		bprm->interp_flags |= BINPRM_FLAGS_ENFORCE_NONDUMP;

		/* Ensure bprm->user_ns contains the executable. */
		/* 确保bprm->user_ns包含可执行文件 */
		user_ns = old = bprm->user_ns;
		while ((user_ns != &init_user_ns) &&
		       !privileged_wrt_inode_uidgid(user_ns, idmap, inode))
			user_ns = user_ns->parent;

		if (old != user_ns) {
			bprm->user_ns = get_user_ns(user_ns);
			put_user_ns(old);
		}
	}
}
EXPORT_SYMBOL(would_dump);

/**
 * setup_new_exec - 设置新程序执行环境
 * @bprm: 二进制程序参数结构
 *
 * 【最后收尾】
 * 在begin_new_exec()之后调用，完成最后的设置：
 * 1. 选择mmap布局（栈位置、堆位置）
 * 2. 设置任务大小
 * 3. 释放exec锁
 * 4. 释放旧地址空间
 */
void setup_new_exec(struct linux_binprm * bprm)
{
	/* Setup things that can depend upon the personality */
	/* 设置可能依赖于personality的内容 */
	struct task_struct *me = current;

	arch_pick_mmap_layout(me->mm, &bprm->rlim_stack);

	arch_setup_new_exec();

	/* Set the new mm task size. We have to do that late because it may
	 * depend on TIF_32BIT which is only updated in flush_thread() on
	 * some architectures like powerpc
	 */
	/**
	 * 设置新mm任务大小。我们必须延迟执行，因为它可能
	 * 依赖于TIF_32BIT，该标志仅在某些架构（如powerpc）的flush_thread()中更新
	 *
	 * 【TASK_SIZE】
	 * 用户空间可用的最大虚拟地址。32位进程和64位进程不同。
	 */
	me->mm->task_size = TASK_SIZE;
	up_write(&me->signal->exec_update_lock);
	mutex_unlock(&me->signal->cred_guard_mutex);

	/* The exec locks are dropped: release the old address space now. */
	/* exec锁已释放：现在释放旧地址空间 */
	if (bprm->old_mm) {
		exec_mm_put_old(bprm->old_mm);
		bprm->old_mm = NULL;
	}
}
EXPORT_SYMBOL(setup_new_exec);

/* Runs immediately before start_thread() takes over. */
/* 在start_thread()接管之前立即运行 */
/**
 * finalize_exec - 完成exec最后步骤
 * @bprm: 二进制程序参数结构
 *
 * 【启动前最后一步】
 * 在跳转到新程序入口点之前，应用栈资源限制。
 */
void finalize_exec(struct linux_binprm *bprm)
{
	/* Store any stack rlimit changes before starting thread. */
	/* 在启动线程前存储任何栈rlimit更改 */
	task_lock(current->group_leader);
	current->signal->rlim[RLIMIT_STACK] = bprm->rlim_stack;
	task_unlock(current->group_leader);
}
EXPORT_SYMBOL(finalize_exec);

/*
 * Prepare credentials and lock ->cred_guard_mutex.
 * setup_new_exec() commits the new creds and drops the lock.
 * Or, if exec fails before, free_bprm() should release ->cred
 * and unlock.
 */
/**
 * 准备凭证并锁定->cred_guard_mutex。
 * setup_new_exec()提交新凭证并释放锁。
 * 或者，如果exec之前失败，free_bprm()应释放->cred并解锁。
 */
/**
 * prepare_bprm_creds - 准备exec凭证
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【凭证准备】
 * 锁定cred_guard_mutex防止ptrace干扰，准备新凭证结构。
 * 新凭证会根据可执行文件的setuid/setgid位进行调整。
 */
static int prepare_bprm_creds(struct linux_binprm *bprm)
{
	if (mutex_lock_interruptible(&current->signal->cred_guard_mutex))
		return -ERESTARTNOINTR;

	bprm->cred = prepare_exec_creds();
	if (likely(bprm->cred))
		return 0;

	mutex_unlock(&current->signal->cred_guard_mutex);
	return -ENOMEM;
}

/* Matches do_open_execat() */
/* 与do_open_execat()配对 */
/**
 * do_close_execat - 关闭可执行文件
 * @file: 要关闭的文件
 *
 * 【写权限恢复】
 * 打开可执行文件时会拒绝写入（exe_file_deny_write_access），
 * 关闭时恢复写权限。
 */
static void do_close_execat(struct file *file)
{
	if (!file)
		return;
	exe_file_allow_write_access(file);
	fput(file);
}

/**
 * free_bprm - 释放bprm结构
 * @bprm: 要释放的二进制程序参数结构
 *
 * 【清理资源】
 * exec失败时清理所有分配的资源：
 * - mm：新地址空间
 * - user_ns：用户命名空间
 * - arg_pages：参数和环境变量页
 * - cred：新凭证
 * - old_mm：旧地址空间
 * - file：可执行文件
 */
static void free_bprm(struct linux_binprm *bprm)
{
	if (bprm->mm) {
		acct_arg_size(bprm, 0);
		mmput(bprm->mm);
	}
	if (bprm->user_ns)
		put_user_ns(bprm->user_ns);
	free_arg_pages(bprm);
	if (bprm->cred) {
		/* in case exec fails before de_thread() succeeds */
		/* 如果exec在de_thread()成功之前失败 */
		current->fs->in_exec = 0;
		mutex_unlock(&current->signal->cred_guard_mutex);
		abort_creds(bprm->cred);
	}
	/* exec swapped the mm but failed before setup_new_exec() freed it */
	/* exec交换了mm但在setup_new_exec()释放它之前失败 */
	if (bprm->old_mm)
		exec_mm_put_old(bprm->old_mm);
	do_close_execat(bprm->file);
	if (bprm->executable)
		fput(bprm->executable);
	/* If a binfmt changed the interp, free it. */
	/* 如果binfmt更改了interp，释放它 */
	if (bprm->interp != bprm->filename)
		kfree(bprm->interp);
	kfree(bprm->fdpath);
	kfree(bprm);
}

/**
 * alloc_bprm - 分配bprm结构
 * @fd: 文件描述符（-1表示使用filename）
 * @filename: 可执行文件名
 * @flags: 标志
 *
 * 返回值：bprm结构指针，失败返回ERR_PTR
 */
static struct linux_binprm *alloc_bprm(int fd, struct filename *filename, int flags)
{
	struct linux_binprm *bprm;
	struct file *file;
	int retval = -ENOMEM;

	file = do_open_execat(fd, filename, flags);
	if (IS_ERR(file))
		return ERR_CAST(file);

	bprm = kzalloc_obj(*bprm);
	if (!bprm) {
		do_close_execat(file);
		return ERR_PTR(-ENOMEM);
	}

	bprm->file = file;

	if (fd == AT_FDCWD || filename->name[0] == '/') {
		bprm->filename = filename->name;
	} else {
		if (filename->name[0] == '\0') {
			bprm->fdpath = kasprintf(GFP_KERNEL, "/dev/fd/%d", fd);
			bprm->comm_from_dentry = 1;
		} else {
			bprm->fdpath = kasprintf(GFP_KERNEL, "/dev/fd/%d/%s",
						  fd, filename->name);
		}
		if (!bprm->fdpath)
			goto out_free;

		/*
		 * Record that a name derived from an O_CLOEXEC fd will be
		 * inaccessible after exec.  This allows the code in exec to
		 * choose to fail when the executable is not mmaped into the
		 * interpreter and an open file descriptor is not passed to
		 * the interpreter.  This makes for a better user experience
		 * than having the interpreter start and then immediately fail
		 * when it finds the executable is inaccessible.
		 */
		/**
		 * 记录从O_CLOEXEC fd派生的名称在exec后将无法访问。
		 * 这允许exec中的代码选择在可执行文件未mmap到解释器
		 * 且未向解释器传递打开的文件描述符时失败。
		 * 这比让解释器启动然后立即发现可执行文件无法访问而失败
		 * 提供更好的用户体验。
		 *
		 * 【fexecve安全】
		 * 通过文件描述符执行（fexecve）时，如果FD设置了CLOEXEC，
		 * 需要提前检测，避免解释器找不到文件。
		 */
		if (get_close_on_exec(fd))
			bprm->interp_flags |= BINPRM_FLAGS_PATH_INACCESSIBLE;

		bprm->filename = bprm->fdpath;
	}
	bprm->interp = bprm->filename;

	/*
	 * At this point, security_file_open() has already been called (with
	 * __FMODE_EXEC) and access control checks for AT_EXECVE_CHECK will
	 * stop just after the security_bprm_creds_for_exec() call in
	 * bprm_execve().  Indeed, the kernel should not try to parse the
	 * content of the file with exec_binprm() nor change the calling
	 * thread, which means that the following security functions will not
	 * be called:
	 * - security_bprm_check()
	 * - security_bprm_creds_from_file()
	 * - security_bprm_committing_creds()
	 * - security_bprm_committed_creds()
	 */
	/**
	 * 此时，security_file_open()已被调用（带__FMODE_EXEC），
	 * AT_EXECVE_CHECK的访问控制检查将在bprm_execve()中的
	 * security_bprm_creds_for_exec()调用后立即停止。
	 * 实际上，内核不应尝试用exec_binprm()解析文件内容，
	 * 也不应改变调用线程，这意味着不会调用以下安全函数：
	 * - security_bprm_check()
	 * - security_bprm_creds_from_file()
	 * - security_bprm_committing_creds()
	 * - security_bprm_committed_creds()
	 *
	 * 【AT_EXECVE_CHECK】
	 * 只检查是否允许执行，不真正执行。用于预检查权限。
	 */
	bprm->is_check = !!(flags & AT_EXECVE_CHECK);

	retval = bprm_mm_init(bprm);
	if (!retval)
		return bprm;

out_free:
	free_bprm(bprm);
	return ERR_PTR(retval);
}

DEFINE_CLASS(bprm, struct linux_binprm *, if (!IS_ERR(_T)) free_bprm(_T),
	alloc_bprm(fd, name, flags), int fd, struct filename *name, int flags)

/**
 * bprm_change_interp - 更改解释器路径
 * @interp: 新解释器路径
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【脚本解释器链】
 * 如#!/usr/bin/env python，env是第一个解释器，python是第二个。
 * binfmt会多次调用此函数更新解释器路径。
 */
int bprm_change_interp(const char *interp, struct linux_binprm *bprm)
{
	/* If a binfmt changed the interp, free it first. */
	/* 如果binfmt更改了interp，先释放它 */
	if (bprm->interp != bprm->filename)
		kfree(bprm->interp);
	bprm->interp = kstrdup(interp, GFP_KERNEL);
	if (!bprm->interp)
		return -ENOMEM;
	return 0;
}
EXPORT_SYMBOL(bprm_change_interp);

/*
 * determine how safe it is to execute the proposed program
 * - the caller must hold ->cred_guard_mutex to protect against
 *   PTRACE_ATTACH or seccomp thread-sync
 */
/**
 * 确定执行提议的程序有多安全
 * - 调用者必须持有->cred_guard_mutex以防止PTRACE_ATTACH或seccomp线程同步
 */
/**
 * check_unsafe_exec - 检查exec安全性
 * @bprm: 二进制程序参数结构
 *
 * 【安全检查】
 * 检测可能导致特权提升漏洞的情况：
 * - LSM_UNSAFE_PTRACE：正在被调试
 * - LSM_UNSAFE_NO_NEW_PRIVS：设置了no_new_privs
 * - LSM_UNSAFE_SHARE：与其他进程共享文件系统
 */
static void check_unsafe_exec(struct linux_binprm *bprm)
{
	struct task_struct *p = current, *t;
	unsigned n_fs;

	if (p->ptrace)
		bprm->unsafe |= LSM_UNSAFE_PTRACE;

	/*
	 * This isn't strictly necessary, but it makes it harder for LSMs to
	 * mess up.
	 */
	/**
	 * 这并不是严格必要的，但它使LSM更难出错。
	 */
	if (task_no_new_privs(current))
		bprm->unsafe |= LSM_UNSAFE_NO_NEW_PRIVS;

	/*
	 * If another task is sharing our fs, we cannot safely
	 * suid exec because the differently privileged task
	 * will be able to manipulate the current directory, etc.
	 * It would be nice to force an unshare instead...
	 *
	 * Otherwise we set fs->in_exec = 1 to deny clone(CLONE_FS)
	 * from another sub-thread until de_thread() succeeds, this
	 * state is protected by cred_guard_mutex we hold.
	 */
	/**
	 * 如果另一个任务正在共享我们的fs，我们无法安全地执行suid exec，
	 * 因为具有不同特权的任务将能够操作当前目录等。
	 * 强制unshare会更好...
	 *
	 * 否则我们设置fs->in_exec = 1以拒绝来自另一个子线程的clone(CLONE_FS)，
	 * 直到de_thread()成功，此状态由我们持有的cred_guard_mutex保护。
	 *
	 * 【文件系统共享风险】
	 * 如果setuid程序执行时，其他进程共享相同的文件系统（当前目录、根目录），
	 * 其他进程可以通过修改当前目录攻击特权进程。
	 */
	n_fs = 1;
	read_seqlock_excl(&p->fs->seq);
	rcu_read_lock();
	for_other_threads(p, t) {
		if (t->fs == p->fs)
			n_fs++;
	}
	rcu_read_unlock();

	/* "users" and "in_exec" locked for copy_fs() */
	/* "users"和"in_exec"为copy_fs()锁定 */
	if (p->fs->users > n_fs)
		bprm->unsafe |= LSM_UNSAFE_SHARE;
	else
		p->fs->in_exec = 1;
	read_sequnlock_excl(&p->fs->seq);
}

/**
 * bprm_fill_uid - 填充UID/GID根据setuid/setgid位
 * @bprm: 二进制程序参数结构
 * @file: 可执行文件
 *
 * 【setuid/setgid机制】
 * 如果可执行文件设置了setuid位，进程将以文件所有者的UID运行。
 * 如果设置了setgid位，进程将以文件组的GID运行。
 * 这是Unix/Linux特权提升的核心机制（如/bin/passwd）。
 */
static void bprm_fill_uid(struct linux_binprm *bprm, struct file *file)
{
	/* Handle suid and sgid on files */
	/* 处理文件上的suid和sgid */
	struct mnt_idmap *idmap;
	struct inode *inode = file_inode(file);
	unsigned int mode;
	vfsuid_t vfsuid;
	vfsgid_t vfsgid;
	int err;

	/* 【检查挂载点是否允许setuid】 */
	if (!mnt_may_suid(file->f_path.mnt))
		return;  /* 挂载时指定了nosuid选项，禁止setuid */

	/* 【no_new_privs标志检查】进程设置了no_new_privs，禁止获取新特权 */
	if (task_no_new_privs(current))
		return;

	/* 【快速路径】检查是否设置了setuid/setgid位 */
	mode = READ_ONCE(inode->i_mode);
	if (!(mode & (S_ISUID|S_ISGID)))
		return;  /* 未设置setuid/setgid位，无需处理 */

	idmap = file_mnt_idmap(file);

	/* Be careful if suid/sgid is set */
	/* 如果设置了suid/sgid则要小心 */
	inode_lock(inode);

	/* Atomically reload and check mode/uid/gid now that lock held. */
	/* 持锁后原子地重新加载并检查mode/uid/gid */
	/* 【TOCTOU防护】防止检查与使用之间的竞争条件 */
	mode = inode->i_mode;
	vfsuid = i_uid_into_vfsuid(idmap, inode);
	vfsgid = i_gid_into_vfsgid(idmap, inode);
	err = inode_permission(idmap, inode, MAY_EXEC);
	inode_unlock(inode);

	/* Did the exec bit vanish out from under us? Give up. */
	/* 执行位是否在我们脚下消失了？放弃。*/
	if (err)
		return;

	/* We ignore suid/sgid if there are no mappings for them in the ns */
	/* 如果在命名空间中没有映射，我们忽略suid/sgid */
	/* 【用户命名空间】setuid/setgid的UID/GID必须在当前用户命名空间中有映射 */
	if (!vfsuid_has_mapping(bprm->cred->user_ns, vfsuid) ||
	    !vfsgid_has_mapping(bprm->cred->user_ns, vfsgid))
		return;

	/* 【处理setuid位】 */
	if (mode & S_ISUID) {
		bprm->per_clear |= PER_CLEAR_ON_SETID;  /* 清除个性化标志 */
		bprm->cred->euid = vfsuid_into_kuid(vfsuid);  /* 设置有效UID为文件所有者 */
	}

	/* 【处理setgid位】需要同时满足setgid和组可执行 */
	if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
		bprm->per_clear |= PER_CLEAR_ON_SETID;
		bprm->cred->egid = vfsgid_into_kgid(vfsgid);  /* 设置有效GID为文件组 */
	}
}

/*
 * Compute brpm->cred based upon the final binary.
 */
/**
 * 计算基于最终二进制文件的brpm->cred
 *
 * bprm_creds_from_file - 从文件计算凭证
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【fexecve特殊处理】
 * 如果是fexecve（通过文件描述符执行），使用bprm->executable而非bprm->file。
 * bprm->execfd_creds标志指示应该从哪个文件获取凭证。
 */
static int bprm_creds_from_file(struct linux_binprm *bprm)
{
	/* Compute creds based on which file? */
	/* 基于哪个文件计算凭证？ */
	struct file *file = bprm->execfd_creds ? bprm->executable : bprm->file;

	bprm_fill_uid(bprm, file);
	return security_bprm_creds_from_file(bprm, file);
}

/*
 * Fill the binprm structure from the inode.
 * Read the first BINPRM_BUF_SIZE bytes
 *
 * This may be called multiple times for binary chains (scripts for example).
 */
/**
 * 从inode填充binprm结构
 * 读取前BINPRM_BUF_SIZE字节
 *
 * 这个函数可能被多次调用用于二进制链（例如脚本）。
 *
 * prepare_binprm - 准备二进制程序参数
 * @bprm: 二进制程序参数结构
 *
 * 返回值：读取的字节数，负数=错误码
 *
 * 【为什么要读取文件头】
 * 内核需要识别文件格式（ELF、脚本等），读取前128字节（BINPRM_BUF_SIZE）
 * 足以识别大部分格式：
 * - ELF魔数：0x7f 'E' 'L' 'F'
 * - 脚本：#!/bin/sh
 * - a.out、Java class等
 *
 * 【多次调用】
 * 对于脚本，第一次调用读取脚本头（#!/bin/sh），识别为脚本后，
 * 打开解释器（/bin/sh），第二次调用读取解释器的ELF头。
 */
static int prepare_binprm(struct linux_binprm *bprm)
{
	loff_t pos = 0;

	memset(bprm->buf, 0, BINPRM_BUF_SIZE);  /* 清空缓冲区 */
	return kernel_read(bprm->file, bprm->buf, BINPRM_BUF_SIZE, &pos);  /* 读取文件头 */
}

/*
 * Arguments are '\0' separated strings found at the location bprm->p
 * points to; chop off the first by relocating brpm->p to right after
 * the first '\0' encountered.
 */
/**
 * 参数是'\0'分隔的字符串，位于bprm->p指向的位置；
 * 通过将brpm->p重定位到遇到的第一个'\0'之后来截掉第一个参数。
 *
 * remove_arg_zero - 移除argv[0]
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【用途】
 * 用于脚本解释器。当执行脚本时，argv[0]是脚本文件名，
 * 但解释器需要它变成解释器名称。此函数移除原argv[0]，
 * 为解释器路径腾出空间。
 *
 * 【实现】
 * 扫描从bprm->p开始的字符串，找到第一个'\0'，
 * 然后将bprm->p移动到'\0'之后，跳过该参数。
 * 可能跨越多个页面。
 */
int remove_arg_zero(struct linux_binprm *bprm)
{
	unsigned long offset;
	char *kaddr;
	struct page *page;

	if (!bprm->argc)
		return 0;  /* 没有参数，直接返回 */

	do {
		offset = bprm->p & ~PAGE_MASK;  /* 页内偏移 */
		page = get_arg_page(bprm, bprm->p, 0);  /* 获取参数页 */
		if (!page)
			return -EFAULT;
		kaddr = kmap_local_page(page);  /* 映射页面到内核地址空间 */

		/* 扫描页面直到找到'\0'或到达页尾 */
		for (; offset < PAGE_SIZE && kaddr[offset];
				offset++, bprm->p++)
			;

		kunmap_local(kaddr);  /* 解除映射 */
		put_arg_page(page);
	} while (offset == PAGE_SIZE);  /* 如果到达页尾但未找到'\0'，继续下一页 */

	bprm->p++;  /* 跳过'\0' */
	bprm->argc--;  /* 参数计数减一 */

	return 0;
}
EXPORT_SYMBOL(remove_arg_zero);

/*
 * cycle the list of binary formats handler, until one recognizes the image
 */
/**
 * 循环遍历二进制格式处理器列表，直到有一个识别该映像
 *
 * search_binary_handler - 搜索并执行二进制格式处理器
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【二进制格式处理器（binfmt）】
 * Linux支持多种可执行文件格式，每种格式有一个处理器：
 * - binfmt_elf: ELF格式（现代Linux默认）
 * - binfmt_script: 脚本格式（#!/bin/sh）
 * - binfmt_misc: 其他格式（通过/proc/sys/fs/binfmt_misc配置）
 * - binfmt_flat: 嵌入式系统的扁平格式
 *
 * 【执行流程】
 * 1. prepare_binprm()读取文件头
 * 2. security_bprm_check()安全检查
 * 3. 遍历formats列表，尝试每个处理器
 * 4. 第一个识别该格式的处理器执行load_binary()
 * 5. 如果是脚本，load_binary()会递归调用本函数处理解释器
 */
static int search_binary_handler(struct linux_binprm *bprm)
{
	struct linux_binfmt *fmt;
	int retval;

	retval = prepare_binprm(bprm);  /* 读取文件头 */
	if (retval < 0)
		return retval;

	retval = security_bprm_check(bprm);  /* LSM安全检查 */
	if (retval)
		return retval;

	read_lock(&binfmt_lock);  /* 读锁保护格式列表 */
	list_for_each_entry(fmt, &formats, lh) {  /* 遍历所有注册的格式处理器 */
		if (!try_module_get(fmt->module))  /* 增加模块引用计数 */
			continue;  /* 模块正在卸载，跳过 */
		read_unlock(&binfmt_lock);  /* 释放锁以允许处理器操作 */

		retval = fmt->load_binary(bprm);  /* 调用格式处理器的load_binary() */

		read_lock(&binfmt_lock);  /* 重新获取锁 */
		put_binfmt(fmt);  /* 减少模块引用计数 */
		/* 【成功条件】
		 * 1. bprm->point_of_no_return已设置：已过不归点，成功或失败都要返回
		 * 2. retval != -ENOEXEC：不是"格式不匹配"错误
		 */
		if (bprm->point_of_no_return || (retval != -ENOEXEC)) {
			read_unlock(&binfmt_lock);
			return retval;  /* 找到了匹配的处理器，返回结果 */
		}
	}
	read_unlock(&binfmt_lock);

	return -ENOEXEC;  /* 没有处理器识别该格式 */
}

/* binfmt handlers will call back into begin_new_exec() on success. */
/* binfmt处理器在成功时会回调begin_new_exec() */
/**
 * exec_binprm - 执行二进制程序
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【递归深度限制】
 * 脚本可以嵌套：#!/bin/sh脚本可以调用另一个#!/bin/sh脚本。
 * 为防止无限递归，限制最大深度（BINPRM_MAX_RECURSION）。
 *
 * 【PID跟踪】
 * 保存old_pid和old_vpid，用于检测exec过程中是否发生了PID变化。
 * 在多线程exec中，非领导者线程可能会接管领导者的PID。
 */
static int exec_binprm(struct linux_binprm *bprm)
{
	pid_t old_pid, old_vpid;
	int ret, depth;

	/* Need to fetch pid before load_binary changes it */
	/* 需要在load_binary改变它之前获取pid */
	old_pid = current->pid;
	rcu_read_lock();
	old_vpid = task_pid_nr_ns(current, task_active_pid_ns(current->parent));  /* 虚拟PID */
	rcu_read_unlock();

	/* This allows 5 levels of binfmt rewrites before failing hard. */
	/* 这允许5层binfmt重写，超过则失败 */
	/* 【递归处理脚本链】循环处理脚本解释器链 */
	for (depth = 0;; depth++) {
		struct file *exec;
		if (depth > 5)
			return -ELOOP;  /* 超过最大深度，防止无限递归 */

		ret = search_binary_handler(bprm);  /* 搜索并执行格式处理器 */
		if (ret < 0)
			return ret;
		if (!bprm->interpreter)
			break;  /* 不是脚本，或已到达最终解释器，退出循环 */

		/* 【处理脚本解释器】
		 * 如果是脚本（如#!/bin/sh），search_binary_handler会设置bprm->interpreter
		 * 指向解释器文件。需要切换到解释器继续执行。
		 */
		exec = bprm->file;
		bprm->file = bprm->interpreter;  /* 切换到解释器 */
		bprm->interpreter = NULL;

		exe_file_allow_write_access(exec);  /* 允许写入旧文件（不再执行它） */
		if (unlikely(bprm->have_execfd)) {  /* fexecve特殊处理 */
			if (bprm->executable) {
				fput(exec);
				return -ENOEXEC;  /* 不允许多次设置executable */
			}
			bprm->executable = exec;  /* 保存原始可执行文件 */
		} else
			fput(exec);  /* 释放旧文件 */
	}

	/* 【exec成功，发送通知】 */
	audit_bprm(bprm);  /* 审计日志 */
	trace_sched_process_exec(current, old_pid, bprm);  /* tracepoint */
	ptrace_event(PTRACE_EVENT_EXEC, old_vpid);  /* ptrace事件 */
	proc_exec_connector(current);  /* 进程连接器通知 */
	return 0;
}

/**
 * bprm_execve - 执行二进制程序的主函数
 * @bprm: 二进制程序参数结构
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【执行流程】
 * 1. 准备凭证（prepare_bprm_creds）
 * 2. 检查不安全执行状态（check_unsafe_exec）
 * 3. 调度器优化（sched_exec）- 将任务迁移到更优CPU
 * 4. 安全模块初始化凭证（security_bprm_creds_for_exec）
 * 5. 执行二进制程序（exec_binprm）
 * 6. 清理和通知
 *
 * 【in_execve标志】
 * current->in_execve在exec期间为1，用于检测exec进行中的状态。
 */
static int bprm_execve(struct linux_binprm *bprm)
{
	int retval;

	retval = prepare_bprm_creds(bprm);  /* 准备新凭证结构 */
	if (retval)
		return retval;

	/*
	 * Check for unsafe execution states before exec_binprm(), which
	 * will call back into begin_new_exec(), into bprm_creds_from_file(),
	 * where setuid-ness is evaluated.
	 */
	/**
	 * 在exec_binprm()之前检查不安全的执行状态，它将回调到begin_new_exec()，
	 * 再到bprm_creds_from_file()，在那里评估setuid特性。
	 */
	check_unsafe_exec(bprm);  /* 检查ptrace、fs共享等安全问题 */
	current->in_execve = 1;  /* 标记正在执行exec */
	sched_mm_cid_before_execve(current);  /* 调度器mm_cid处理 */

	sched_exec();  /* 调度器exec优化：可能迁移到更优CPU */

	/* Set the unchanging part of bprm->cred */
	/* 设置bprm->cred的不变部分 */
	retval = security_bprm_creds_for_exec(bprm);  /* LSM设置凭证 */
	if (retval || bprm->is_check)
		goto out;  /* AT_EXECVE_CHECK模式，只检查不执行 */

	retval = exec_binprm(bprm);  /* 执行二进制程序 */
	if (retval < 0)
		goto out;

	/* 【exec成功后清理】 */
	sched_mm_cid_after_execve(current);  /* 调度器清理 */
	rseq_execve(current);  /* 可重启序列（restartable sequences）处理 */
	/* execve succeeded */
	/* execve成功 */
	current->in_execve = 0;  /* 清除exec标志 */
	user_events_execve(current);  /* 用户事件通知 */
	acct_update_integrals(current);  /* 更新进程统计信息 */
	task_numa_free(current, false);  /* 释放NUMA相关资源 */
	return retval;

out:
	/*
	 * If past the point of no return ensure the code never
	 * returns to the userspace process.  Use an existing fatal
	 * signal if present otherwise terminate the process with
	 * SIGSEGV.
	 */
	/**
	 * 如果已经过了不归点，确保代码永远不会返回到用户空间进程。
	 * 如果有现有的致命信号则使用它，否则用SIGSEGV终止进程。
	 *
	 * 【不归点后的错误处理】
	 * 过了不归点后，旧程序的地址空间、信号处理器等已被破坏，
	 * 无法返回用户空间继续执行。必须杀死进程。
	 */
	if (bprm->point_of_no_return && !fatal_signal_pending(current))
		force_fatal_sig(SIGSEGV);  /* 强制发送SIGSEGV信号 */

	sched_mm_cid_after_execve(current);
	rseq_force_update();
	current->in_execve = 0;

	return retval;
}

/**
 * do_execveat_common - execveat系统调用的通用实现
 * @fd: 文件描述符（用于fexecve），或AT_FDCWD表示使用路径名
 * @filename: 可执行文件的文件名结构
 * @argv: 参数数组指针
 * @envp: 环境变量数组指针
 * @flags: 标志位（AT_EMPTY_PATH、AT_SYMLINK_NOFOLLOW等）
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【RLIMIT_NPROC延迟检查】
 * RLIMIT_NPROC限制了用户可以创建的进程数。理想情况下应该在
 * setuid()时检查，但很多程序不检查setuid()返回值，所以内核
 * 在setuid()时只标记PF_NPROC_EXCEEDED，在exec时真正拒绝。
 *
 * 【执行流程】
 * 1. 检查RLIMIT_NPROC限制
 * 2. 分配并初始化bprm结构（CLASS宏实现自动清理）
 * 3. 计数和复制argv和envp
 * 4. 设置栈限制
 * 5. 调用bprm_execve()执行
 */
static int do_execveat_common(int fd, struct filename *filename,
			      struct user_arg_ptr argv,
			      struct user_arg_ptr envp,
			      int flags)
{
	int retval;

	/*
	 * We move the actual failure in case of RLIMIT_NPROC excess from
	 * set*uid() to execve() because too many poorly written programs
	 * don't check setuid() return code.  Here we additionally recheck
	 * whether NPROC limit is still exceeded.
	 */
	/**
	 * 我们将RLIMIT_NPROC超出时的实际失败从set*uid()移到execve()，
	 * 因为太多写得不好的程序不检查setuid()返回码。这里我们额外
	 * 重新检查NPROC限制是否仍然超出。
	 */
	if ((current->flags & PF_NPROC_EXCEEDED) &&
	    is_rlimit_overlimit(current_ucounts(), UCOUNT_RLIMIT_NPROC, rlimit(RLIMIT_NPROC)))
		return -EAGAIN;  /* 进程数超限 */

	/* We're below the limit (still or again), so we don't want to make
	 * further execve() calls fail. */
	/* 我们低于限制（仍然或再次），所以我们不想让后续的execve()调用失败 */
	current->flags &= ~PF_NPROC_EXCEEDED;  /* 清除超限标志 */

	CLASS(bprm, bprm)(fd, filename, flags);  /* 分配并初始化bprm，自动清理 */
	if (IS_ERR(bprm))
		return PTR_ERR(bprm);

	/* 【计数参数】 */
	retval = count(argv, MAX_ARG_STRINGS);
	if (retval < 0)
		return retval;
	bprm->argc = retval;

	/* 【计数环境变量】 */
	retval = count(envp, MAX_ARG_STRINGS);
	if (retval < 0)
		return retval;
	bprm->envc = retval;

	/* 【设置栈限制】 */
	retval = bprm_stack_limits(bprm);
	if (retval < 0)
		return retval;

	/* 【复制文件名到参数栈】 */
	retval = copy_string_kernel(bprm->filename, bprm);
	if (retval < 0)
		return retval;
	bprm->exec = bprm->p;  /* 记录文件名在栈中的位置 */

	/* 【复制环境变量】 */
	retval = copy_strings(bprm->envc, envp, bprm);
	if (retval < 0)
		return retval;

	/* 【复制参数】 */
	retval = copy_strings(bprm->argc, argv, bprm);
	if (retval < 0)
		return retval;

	/*
	 * When argv is empty, add an empty string ("") as argv[0] to
	 * ensure confused userspace programs that start processing
	 * from argv[1] won't end up walking envp. See also
	 * bprm_stack_limits().
	 */
	/**
	 * 当argv为空时，添加一个空字符串("")作为argv[0]，
	 * 以确保从argv[1]开始处理的困惑的用户空间程序
	 * 不会最终遍历envp。另见bprm_stack_limits()。
	 *
	 * 【argv[0]保护】
	 * 某些程序假定argv[0]总是存在，从argv[1]开始处理参数。
	 * 如果argv为空，它们会误读envp为参数。添加空字符串防止这个问题。
	 */
	if (bprm->argc == 0) {
		retval = copy_string_kernel("", bprm);  /* 添加空字符串 */
		if (retval < 0)
			return retval;
		bprm->argc = 1;

		pr_warn_once("process '%s' launched '%s' with NULL argv: empty string added\n",
			     current->comm, bprm->filename);  /* 警告：异常用法 */
	}

	return bprm_execve(bprm);  /* 执行程序 */
}

/**
 * kernel_execve - 内核线程执行新程序
 * @kernel_filename: 内核空间的文件名字符串
 * @argv: 参数数组（内核空间）
 * @envp: 环境变量数组（内核空间）
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【用途】
 * 用于内核线程转换为用户空间进程。例如：
 * - init进程启动：kernel_init()调用此函数执行/sbin/init
 * - kthreadd生成用户空间辅助进程
 * - call_usermodehelper()执行用户空间程序
 *
 * 【与用户空间execve的区别】
 * - argv/envp在内核空间，不需要copy_from_user
 * - 不允许内核线程调用（PF_KTHREAD检查）
 * - 参数必须非空（WARN_ON_ONCE检查）
 */
int kernel_execve(const char *kernel_filename,
		  const char *const *argv, const char *const *envp)
{
	int retval;

	/* It is non-sense for kernel threads to call execve */
	/* 内核线程调用execve是没有意义的 */
	if (WARN_ON_ONCE(current->flags & PF_KTHREAD))
		return -EINVAL;  /* 内核线程不能exec */

	CLASS(filename_kernel, filename)(kernel_filename);  /* 从内核字符串创建filename */
	CLASS(bprm, bprm)(AT_FDCWD, filename, 0);  /* 分配bprm结构 */
	if (IS_ERR(bprm))
		return PTR_ERR(bprm);

	/* 【计数内核argv】 */
	retval = count_strings_kernel(argv);
	if (WARN_ON_ONCE(retval == 0))
		return -EINVAL;  /* 参数不能为空 */
	if (retval < 0)
		return retval;
	bprm->argc = retval;

	/* 【计数内核envp】 */
	retval = count_strings_kernel(envp);
	if (retval < 0)
		return retval;
	bprm->envc = retval;

	/* 【设置栈限制】 */
	retval = bprm_stack_limits(bprm);
	if (retval < 0)
		return retval;

	/* 【复制文件名】 */
	retval = copy_string_kernel(bprm->filename, bprm);
	if (retval < 0)
		return retval;
	bprm->exec = bprm->p;

	/* 【复制环境变量】内核空间版本 */
	retval = copy_strings_kernel(bprm->envc, envp, bprm);
	if (retval < 0)
		return retval;

	/* 【复制参数】内核空间版本 */
	/* 【复制参数】内核空间版本 */
	retval = copy_strings_kernel(bprm->argc, argv, bprm);
	if (retval < 0)
		return retval;

	return bprm_execve(bprm);  /* 执行程序 */
}

/**
 * set_binfmt - 设置当前进程的二进制格式处理器
 * @new: 新的二进制格式处理器
 *
 * 【模块引用计数管理】
 * - 释放旧格式处理器的模块引用（module_put）
 * - 获取新格式处理器的模块引用（__module_get）
 * - 防止模块在使用期间被卸载
 *
 * 【何时调用】
 * 在load_binary()成功识别并加载二进制文件后调用，
 * 标记进程使用的可执行文件格式（ELF、脚本等）。
 */
void set_binfmt(struct linux_binfmt *new)
{
	struct mm_struct *mm = current->mm;

	if (mm->binfmt)
		module_put(mm->binfmt->module);  /* 释放旧格式的模块引用 */

	mm->binfmt = new;  /* 设置新格式 */
	if (new)
		__module_get(new->module);  /* 获取新格式的模块引用 */
}
EXPORT_SYMBOL(set_binfmt);

/**
 * native_arg - 创建本地架构的用户参数指针
 * @p: 用户空间指针数组
 *
 * 返回值：user_arg_ptr结构
 *
 * 【用途】
 * 用于本地架构（非compat）的系统调用，封装用户空间指针。
 */
static inline struct user_arg_ptr native_arg(const char __user *const __user *p)
{
	return (struct user_arg_ptr){.ptr.native = p};
}

/**
 * SYSCALL_DEFINE3(execve, ...) - execve系统调用
 * @filename: 可执行文件路径（用户空间指针）
 * @argv: 参数数组（用户空间指针）
 * @envp: 环境变量数组（用户空间指针）
 *
 * 返回值：成功不返回（进程已被替换），失败返回负错误码
 *
 * 【系统调用】
 * execve(2)是替换当前进程为新程序的标准POSIX系统调用。
 * 成功时不返回，因为调用者的代码已被新程序替换。
 */
SYSCALL_DEFINE3(execve,
		const char __user *, filename,
		const char __user *const __user *, argv,
		const char __user *const __user *, envp)
{
	CLASS(filename, name)(filename);  /* 从用户空间获取文件名 */
	return do_execveat_common(AT_FDCWD, name,
				  native_arg(argv), native_arg(envp), 0);
}

/**
 * SYSCALL_DEFINE5(execveat, ...) - execveat系统调用
 * @fd: 目录文件描述符，或AT_FDCWD表示当前目录
 * @filename: 相对于fd的文件路径，或空表示fd本身（fexecve）
 * @argv: 参数数组
 * @envp: 环境变量数组
 * @flags: 标志位（AT_EMPTY_PATH、AT_SYMLINK_NOFOLLOW、AT_EXECVE_CHECK）
 *
 * 返回值：成功不返回，失败返回负错误码
 *
 * 【execveat vs execve】
 * execveat()是execve()的扩展版本，支持：
 * - 相对于目录fd的路径解析
 * - AT_EMPTY_PATH + fd：实现fexecve（通过fd执行）
 * - AT_SYMLINK_NOFOLLOW：不跟随符号链接
 * - AT_EXECVE_CHECK：只检查权限，不执行
 */
SYSCALL_DEFINE5(execveat,
		int, fd, const char __user *, filename,
		const char __user *const __user *, argv,
		const char __user *const __user *, envp,
		int, flags)
{
	CLASS(filename_uflags, name)(filename, flags);  /* 根据flags获取文件名 */
	return do_execveat_common(fd, name,
				  native_arg(argv), native_arg(envp), flags);
}

#ifdef CONFIG_COMPAT  /* 兼容模式：32位程序在64位内核上运行 */

/**
 * compat_arg - 创建兼容模式的用户参数指针
 * @p: 兼容模式的用户空间指针数组
 *
 * 返回值：user_arg_ptr结构，标记为compat模式
 *
 * 【兼容模式】
 * 64位内核运行32位程序时，指针大小不同（32位 vs 64位）。
 * compat_arg标记指针为32位格式，确保正确读取用户空间数据。
 */
static inline struct user_arg_ptr compat_arg(const compat_uptr_t __user *p)
{
	return (struct user_arg_ptr){.is_compat = true, .ptr.compat = p};
}

/**
 * COMPAT_SYSCALL_DEFINE3(execve, ...) - 兼容模式的execve系统调用
 * @filename: 可执行文件路径（32位指针）
 * @argv: 参数数组（32位指针数组）
 * @envp: 环境变量数组（32位指针数组）
 *
 * 返回值：成功不返回，失败返回负错误码
 *
 * 【32位兼容】
 * 在64位内核上运行32位程序时调用此版本。
 * 处理32位/64位指针大小差异。
 */
COMPAT_SYSCALL_DEFINE3(execve, const char __user *, filename,
	const compat_uptr_t __user *, argv,
	const compat_uptr_t __user *, envp)
{
	CLASS(filename, name)(filename);
	return do_execveat_common(AT_FDCWD, name,
				  compat_arg(argv), compat_arg(envp), 0);
}

/**
 * COMPAT_SYSCALL_DEFINE5(execveat, ...) - 兼容模式的execveat系统调用
 * @fd: 目录文件描述符
 * @filename: 文件路径（32位指针）
 * @argv: 参数数组（32位指针数组）
 * @envp: 环境变量数组（32位指针数组）
 * @flags: 标志位
 *
 * 返回值：成功不返回，失败返回负错误码
 */
COMPAT_SYSCALL_DEFINE5(execveat, int, fd,
		       const char __user *, filename,
		       const compat_uptr_t __user *, argv,
		       const compat_uptr_t __user *, envp,
		       int,  flags)
{
	CLASS(filename_uflags, name)(filename, flags);
	return do_execveat_common(fd, name,
				  compat_arg(argv), compat_arg(envp), flags);
}
#endif  /* CONFIG_COMPAT */

#ifdef CONFIG_SYSCTL

/**
 * proc_dointvec_minmax_coredump - 处理suid_dumpable sysctl写入
 * @table: sysctl表项
 * @write: 1=写入，0=读取
 * @buffer: 用户缓冲区
 * @lenp: 缓冲区长度
 * @ppos: 文件位置
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【suid_dumpable】
 * 控制setuid程序是否可以生成coredump：
 * - 0（SUID_DUMP_DISABLE）：禁止（默认，安全）
 * - 1（SUID_DUMP_USER）：允许，但只有用户可读
 * - 2（SUID_DUMP_ROOT）：允许，root可读
 *
 * 【安全考虑】
 * setuid程序的coredump可能包含敏感数据（密码、密钥）。
 * 改变此设置时，通知set_dumpable()更新现有进程的dumpable标志。
 */
static int proc_dointvec_minmax_coredump(const struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	int error, old = READ_ONCE(suid_dumpable);  /* 保存旧值 */

	error = proc_dointvec_minmax(table, write, buffer, lenp, ppos);  /* 标准处理 */

	if (!error && write && (old != READ_ONCE(suid_dumpable)))
		validate_coredump_safety();  /* 值改变，重新验证现有进程的coredump安全性 */
	return error;
}

/**
 * fs_exec_sysctls - exec相关的sysctl配置表
 *
 * 【suid_dumpable】
 * /proc/sys/fs/suid_dumpable
 * - 可读写（0644）
 * - 取值范围：0-2（extra1=0, extra2=2）
 * - 处理函数：proc_dointvec_minmax_coredump
 */
static const struct ctl_table fs_exec_sysctls[] = {
	{
		.procname	= "suid_dumpable",
		.data		= &suid_dumpable,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax_coredump,
		.extra1		= SYSCTL_ZERO,  /* 最小值：0 */
		.extra2		= SYSCTL_TWO,   /* 最大值：2 */
	},
};

/**
 * init_fs_exec_sysctls - 初始化exec相关的sysctl
 *
 * 返回值：0（总是成功）
 *
 * 【初始化时机】
 * fs_initcall()在启动时调用，注册/proc/sys/fs/下的sysctl条目。
 */
static int __init init_fs_exec_sysctls(void)
{
	register_sysctl_init("fs", fs_exec_sysctls);  /* 注册到/proc/sys/fs/ */
	return 0;
}

fs_initcall(init_fs_exec_sysctls);  /* 文件系统初始化阶段调用 */
#endif /* CONFIG_SYSCTL */

#ifdef CONFIG_EXEC_KUNIT_TEST
#include "tests/exec_kunit.c"  /* 单元测试代码 */
#endif
