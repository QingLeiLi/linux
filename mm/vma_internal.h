/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vma_internal.h
 *
 * Headers required by vma.c, which can be substituted accordingly when testing
 * VMA functionality.
 */
/*
 * 本头文件集中列出 mm/vma.c 实现 VMA 拆分、合并、链接和修改时依赖的内部接口。
 * 测试构建可用替代头或桩实现隔离这些依赖，因此这里属于实现边界，不是供其他
 * 子系统调用的稳定 API；新增 vma.c 依赖时也应在此显式登记，保持测试与生产一致。
 */

#ifndef __MM_VMA_INTERNAL_H
#define __MM_VMA_INTERNAL_H

/*
 * 第一组提供文件后备、位操作、错误处理和基础 VFS 对象。vma.c 借用 file、
 * address_space 等对象来维护文件映射；引用及错误指针规则由这些公共头定义。
 */
#include <linux/backing-dev.h>
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/cacheflush.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/huge_mm.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_inline.h>

/*
 * 第二组提供 VMA 使用的 KSM、khugepaged、链表与 maple tree。maple tree 是
 * mm_struct 中 VMA 的发布/查找索引；修改者仍须遵守 mmap_lock，而包含头文件
 * 本身既不取得锁，也不延长任何 VMA 的生命周期。
 */
#include <linux/kernel.h>
#include <linux/ksm.h>
#include <linux/khugepaged.h>
#include <linux/list.h>
#include <linux/maple_tree.h>
#include <linux/mempolicy.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mm_types.h>

/*
 * 第三组给出 mmap 标志、锁断言、MMU 上下文及同步原语。它们让 vma.c 能区分
 * 只读查找与结构修改，并在页表、VMA 边界和体系结构地址空间状态之间保持顺序。
 */
#include <linux/mman.h>
#include <linux/mmap_lock.h>
#include <linux/mmdebug.h>
#include <linux/mmu_context.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/perf_event.h>
#include <linux/personality.h>
#include <linux/pfn.h>

/*
 * 第四组连接 RCU、反向映射、进程信号、LSM、shmem、swap、uprobes 与
 * userfaultfd；VMA 改动会通知这些观察者或更新其辅助状态，不能只修改 maple tree。
 */
#include <linux/rcupdate.h>
#include <linux/rmap.h>
#include <linux/rwsem.h>
#include <linux/sched/signal.h>
#include <linux/security.h>
#include <linux/shmem_fs.h>
#include <linux/swap.h>
#include <linux/uprobes.h>
#include <linux/userfaultfd_k.h>
#include <linux/pgtable.h>

/*
 * 体系结构头提供 current 与 TLB 收集/失效契约；最后的 mm/internal.h 则暴露
 * 仅供内存管理内部共享的 helper。这里的依赖方向说明 vma.c 位于通用 MM 层，
 * 但提交 VMA 变化后仍要由架构接口完成页表可见性和 TLB 一致性维护。
 */
#include <asm/current.h>
#include <asm/tlb.h>

#include "internal.h"

/* 结束本文件的防重复包含范围；该注释不改变任何条件编译分支。 */
#endif	/* __MM_VMA_INTERNAL_H */
