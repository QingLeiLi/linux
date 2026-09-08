/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vma.h
 *
 * Core VMA manipulation API implemented in vma.c.
 */
/*
 * 学习提示：本头文件把 VMA 变更拆成“准备描述符、Maple Tree 更新、
 * 页表解除映射、反向映射/文件链维护”几层。调用者通常已经持有 mmap
 * 写锁；声明只给出接口，真正的提交顺序与失败回滚位于 vma.c。
 */
#ifndef __MM_VMA_H
#define __MM_VMA_H

/*
 * VMA lock generalization
 */
/*
 * vma_prepare 是一次 VMA 边界调整的事务清单：vma/adj_next 是被改对象，
 * file/mapping/anon_vma 保存索引归属，insert/remove/remove2 描述提交时
 * 要增删的 VMA。skip_vma_uprobe 让特定合并路径避免重复通知 uprobe。
 * 该对象只在持锁的修改窗口内有效，不拥有这些指针指向对象的引用。
 */
struct vma_prepare {
	struct vm_area_struct *vma;
	struct vm_area_struct *adj_next;
	struct file *file;
	struct address_space *mapping;
	struct anon_vma *anon_vma;
	/* 上述指针锁定索引归属；以下三项是提交阶段的增删集合。 */
	struct vm_area_struct *insert;
	struct vm_area_struct *remove;
	struct vm_area_struct *remove2;

	bool skip_vma_uprobe :1;
};

struct unlink_vma_file_batch {
	/* count 是当前缓存项数；vmas 仅暂存待从文件 interval tree 脱链的对象。 */
	int count;
	struct vm_area_struct *vmas[8];
};

/*
 * vma munmap operation
 */
/*
 * vma_munmap_struct 汇总一次 munmap 的查找结果和记账增量。start/end 是
 * 用户请求对齐后的 VMA 区间，unmap_start/unmap_end 是实际清 PTE 的边界；
 * prev/next 固定删除区间两侧。调用链先准备这些值，再修改树、清页表并
 * 更新 mm 计数；unlock 决定成功返回前是否释放 mmap 写锁。
 */
struct vma_munmap_struct {
	struct vma_iterator *vmi;
	struct vm_area_struct *vma;     /* The first vma to munmap */
	struct vm_area_struct *prev;    /* vma before the munmap area */
	struct vm_area_struct *next;    /* vma after the munmap area */
	struct list_head *uf;           /* Userfaultfd list_head */
	unsigned long start;            /* Aligned start addr (inclusive) */
	unsigned long end;              /* Aligned end addr (exclusive) */
	unsigned long unmap_start;      /* Unmap PTE start */
	unsigned long unmap_end;        /* Unmap PTE end */
	/* 上述为清页表范围；以下字段累计待删除 VMA 和 mm 统计差值。 */
	int vma_count;                  /* Number of vmas that will be removed */
	bool unlock;                    /* Unlock after the munmap */
	bool clear_ptes;                /* If there are outstanding PTE to be cleared */
	/* 2 byte hole */
	unsigned long nr_pages;         /* Number of pages being removed */
	unsigned long locked_vm;        /* Number of locked pages */
	unsigned long nr_accounted;     /* Number of VM_ACCOUNT pages */
	/* exec/stack/data 分别是从 mm 对应分类计数中扣除的页数。 */
	unsigned long exec_vm;
	unsigned long stack_vm;
	unsigned long data_vm;
};

enum vma_merge_state {
	/* 初始态：尚未决定能否合并。 */
	VMA_MERGE_START,
	/* 合并在需要新元数据时遇到内存不足。 */
	VMA_MERGE_ERROR_NOMEM,
	/* 邻接属性不兼容；保持原布局并让调用者走非合并路径。 */
	VMA_MERGE_NOMERGE,
	/* 合并已提交，target 指向最终保留的 VMA。 */
	VMA_MERGE_SUCCESS,
};

/*
 * Describes a VMA merge operation and is threaded throughout it.
 *
 * Any of the fields may be mutated by the merge operation, so no guarantees are
 * made to the contents of this structure after a merge operation has completed.
 */
/*
 * 译注：该结构贯穿一次 VMA 合并，操作期间任何字段都可能被改写，完成后
 * 不能把其初值当作事实。mm/vmi 给出受写锁保护的地址空间与游标；三邻居
 * 描述候选布局，target 是最终幸存者。start/end/pgoff 会从请求范围扩展为
 * 合并后的总范围，file/anon_vma/policy/UFFD/name 则必须逐项兼容。
 */
struct vma_merge_struct {
	struct mm_struct *mm;
	struct vma_iterator *vmi;
	/*
	 * Adjacent VMAs, any of which may be NULL if not present:
	 *
	 * |------|--------|------|
	 * | prev | middle | next |
	 * |------|--------|------|
	 *
	 * middle may not yet exist in the case of a proposed new VMA being
	 * merged, or it may be an existing VMA.
	 *
	 * next may be assigned by the caller.
	 */
	/*
	 * 译注：prev/middle/next 是地址相邻的三个位置，缺失者为 NULL；新建
	 * 范围尚无 middle，而修改范围以现有 VMA 为 middle，next 也可由调用者预置。
	 */
	struct vm_area_struct *prev;
	struct vm_area_struct *middle;
	struct vm_area_struct *next;
	/* This is the VMA we ultimately target to become the merged VMA. */
	/* 译注：这是最终扩展并保留在树中的目标 VMA。 */
	struct vm_area_struct *target;
	/*
	 * Initially, the start, end, pgoff fields are provided by the caller
	 * and describe the proposed new VMA range, whether modifying an
	 * existing VMA (which will be 'middle'), or adding a new one.
	 *
	 * During the merge process these fields are updated to describe the new
	 * range _including those VMAs which will be merged_.
	 */
	/*
	 * 译注：start/end/pgoff 初始描述提议区间；探测到可合并邻居后会改写为
	 * 包含所有参与 VMA 的最终范围，因此完成后不能用它们恢复原请求。
	 */
	unsigned long start;
	unsigned long end;
	pgoff_t pgoff;

	union {
		/* Temporary while VMA flags are being converted. */
		/* 译注：旧位图向类型安全 vma_flags 转换期间共用同一存储。 */
		vm_flags_t vm_flags;
		vma_flags_t vma_flags;
	};
	struct file *file;
	struct anon_vma *anon_vma;
	struct mempolicy *policy;
	struct vm_userfaultfd_ctx uffd_ctx;
	struct anon_vma_name *anon_name;
	enum vma_merge_state state;

	/* If copied from (i.e. mremap()'d) the VMA from which we are copying. */
	/* 译注：mremap 复制场景记录源 VMA，用于判断 anon_vma 等共享条件。 */
	struct vm_area_struct *copied_from;

	/* Flags which callers can use to modify merge behaviour: */
	/* 译注：以下位由调用者设定，控制合并策略而非描述结果。 */

	/*
	 * If we can expand, simply do so. We know there is nothing to merge to
	 * the right. Does not reset state upon failure to merge. The VMA
	 * iterator is assumed to be positioned at the previous VMA, rather than
	 * at the gap.
	 */
	/* 译注：just_expand 已知右侧无候选，只扩展当前 VMA，失败不重置状态。 */
	bool just_expand :1;

	/*
	 * If a merge is possible, but an OOM error occurs, give up and don't
	 * execute the merge, returning NULL.
	 */
	/* 译注：give_up_on_oom 把合并分配失败降级为“不合并”，供尽力路径使用。 */
	bool give_up_on_oom :1;

	/*
	 * If set, skip uprobe_mmap upon merged vma.
	 */
	/* 译注：合并后的 mmap uprobe 通知由更外层负责时设置此位。 */
	bool skip_vma_uprobe :1;

	/* Internal flags set during merge process: */
	/* 译注：以下内部位是准备阶段生成、提交阶段消费的一次性动作清单。 */

	/*
	 * Internal flag indicating the merge increases vmg->middle->vm_start
	 * (and thereby, vmg->prev->vm_end).
	 */
	/* 译注：提交时上移 middle 起点，同时扩展 prev 末端。 */
	bool __adjust_middle_start :1;
	/*
	 * Internal flag indicating the merge decreases vmg->next->vm_start
	 * (and thereby, vmg->middle->vm_end).
	 */
	/* 译注：提交时下移 next 起点，同时扩展 middle 末端。 */
	bool __adjust_next_start :1;
	/*
	 * Internal flag used during the merge operation to indicate we will
	 * remove vmg->middle.
	 */
	/* 译注：middle 会被幸存 VMA 吞并，提交后须脱链并释放。 */
	bool __remove_middle :1;
	/*
	 * Internal flag used during the merge operation to indicate we will
	 * remove vmg->next.
	 */
	/* 译注：next 会被幸存 VMA 吞并，提交后须脱链并释放。 */
	bool __remove_next :1;

};

struct unmap_desc {
	/*
	 * unmap_desc 把 VMA 树扫描范围与可释放页表范围分开。mas/first 定位
	 * 首个受影响 VMA；pg_* 限制页表释放，vma_* 与 tree_* 控制 Maple
	 * Tree 遍历和复位。mm_wr_locked 记录 free_pgtables 可依赖的锁状态。
	 */
	struct  ma_state *mas;        /* the maple state point to the first vma */
	struct vm_area_struct *first; /* The first vma */
	unsigned long pg_start;       /* The first pagetable address to free (floor) */
	unsigned long pg_end;         /* The last pagetable address to free (ceiling) */
	unsigned long vma_start;      /* The min vma address */
	unsigned long vma_end;        /* The max vma address */
	/* tree_end 是扫描上限，tree_reset 是阶段切换时重新定位的位置。 */
	unsigned long tree_end;       /* Maximum for the vma tree search */
	unsigned long tree_reset;     /* Where to reset the vma tree walk */
	bool mm_wr_locked;            /* If the mmap write lock is held */
};

/*
 * unmap_all_init() - Initialize unmap_desc to remove all vmas, point the
 * pg_start and pg_end to a safe location.
 */
/* 译注：初始化“删除全部 VMA”描述符，并把页表首尾设为架构安全范围。 */
static inline void unmap_all_init(struct unmap_desc *unmap,
		struct vma_iterator *vmi, struct vm_area_struct *vma)
{
	/*
	 * 业务背景：进程退出要清空全部用户 VMA；入参给出描述符、游标和首 VMA。
	 * 出参：初始化全用户页表范围且不宣称持有写锁；返回 void。
	 * 注意：只建立遍历状态，不在此处删除树节点或释放页表。
	 */
	unmap->mas = &vmi->mas;
	unmap->first = vma;
	unmap->pg_start = FIRST_USER_ADDRESS;
	unmap->pg_end = USER_PGTABLES_CEILING;
	unmap->vma_start = 0;
	unmap->vma_end = ULONG_MAX;
	/* 退出路径覆盖整棵用户 VMA 树，并从首 VMA 末端开始下一阶段。 */
	unmap->tree_end = ULONG_MAX;
	unmap->tree_reset = vma->vm_end;
	unmap->mm_wr_locked = false;
}

/*
 * unmap_pgtable_init() - Initialize unmap_desc to remove all page tables within
 * the user range.
 *
 * ARM can have mappings outside of vmas.
 * See: e2cdef8c847b4 ("[PATCH] freepgt: free_pgtables from FIRST_USER_ADDRESS")
 *
 * ARM LPAE uses page table mappings beyond the USER_PGTABLES_CEILING
 * See: CONFIG_ARM_LPAE in arch/arm/include/asm/pgtable.h
 */
/*
 * 译注：此阶段准备删除用户范围页表。ARM 可能存在无 VMA 映射，LPAE 还
 * 可能越过通常上限，因此必须按 FIRST_USER_ADDRESS/CEILING 的架构约定。
 */
static inline void unmap_pgtable_init(struct unmap_desc *unmap,
				      struct vma_iterator *vmi)
{
	/*
	 * 业务背景：VMA 脱链后进入页表释放阶段；入参沿用前一阶段描述符/游标。
	 * 出参：把游标复位至 tree_reset，并限制到架构用户页表边界；返回 void。
	 * 注意：ARM 可有 VMA 外映射，所以边界采用架构常量而非首尾 VMA。
	 */
	vma_iter_set(vmi, unmap->tree_reset);
	unmap->vma_start = FIRST_USER_ADDRESS;
	unmap->vma_end = USER_PGTABLES_CEILING;
	unmap->tree_end = USER_PGTABLES_CEILING;
}

#define UNMAP_STATE(name, _vmi, _vma, _vma_start, _vma_end, _prev, _next)      \
	/* 持 mmap 写锁构造；邻居决定安全页表边界。 */                 \
	struct unmap_desc name = {                                             \
		.mas = &(_vmi)->mas,                                           \
		.first = _vma,                                                 \
		.pg_start = _prev ? ((struct vm_area_struct *)_prev)->vm_end : \
			FIRST_USER_ADDRESS,                                    \
		.pg_end = _next ? ((struct vm_area_struct *)_next)->vm_start : \
			USER_PGTABLES_CEILING,                                 \
		/* 扫描范围可不同，避免释放邻区页表。 */             \
		.vma_start = _vma_start,                                       \
		.vma_end = _vma_end,                                           \
		.tree_end = _next ?                                            \
			((struct vm_area_struct *)_next)->vm_start :           \
			USER_PGTABLES_CEILING,                                 \
		.tree_reset = _vma->vm_end,                                    \
		.mm_wr_locked = true,                                          \
	}

static inline bool vmg_nomem(struct vma_merge_struct *vmg)
{
	/* 合并调用者用它区分“属性不兼容”和“本可合并但分配失败”。 */
	return vmg->state == VMA_MERGE_ERROR_NOMEM;
}

/* Assumes addr >= vma->vm_start. */
/* 译注：前置条件是 addr 不小于 VMA 起点，否则无符号差值会下溢。 */
static inline pgoff_t vma_pgoff_offset(struct vm_area_struct *vma,
				       unsigned long addr)
{
	/* addr 必须位于 VMA 内或其后；返回对应文件/匿名映射的页偏移。 */
	return vma->vm_pgoff + PHYS_PFN(addr - vma->vm_start);
}

#define VMG_STATE(name, mm_, vmi_, start_, end_, vma_flags_, pgoff_)	\
	/* 为新范围候选创建起始态，后续再补 file/policy 等合并属性。 */ \
	struct vma_merge_struct name = {				\
		.mm = mm_,						\
		.vmi = vmi_,						\
		.start = start_,					\
		.end = end_,						\
		.vma_flags = vma_flags_,				\
		.pgoff = pgoff_,					\
		.state = VMA_MERGE_START,				\
	}

#define VMG_VMA_STATE(name, vmi_, prev_, vma_, start_, end_)	\
	/* 从现有 VMA 快照候选；指针由 mmap 写锁保护。 */       \
	struct vma_merge_struct name = {			\
		.mm = vma_->vm_mm,				\
		.vmi = vmi_,					\
		.prev = prev_,					\
		.middle = vma_,					\
		.next = NULL,					\
		/* 范围和偏移来自待修改 VMA。 */              \
		.start = start_,				\
		.end = end_,					\
		.vm_flags = vma_->vm_flags,			\
		.pgoff = vma_pgoff_offset(vma_, start_),	\
		.file = vma_->vm_file,				\
		/* 还须匹配匿名链、策略、UFFD 和名称。 */        \
		.anon_vma = vma_->anon_vma,			\
		.policy = vma_policy(vma_),			\
		.uffd_ctx = vma_->vm_userfaultfd_ctx,		\
		.anon_name = anon_vma_name(vma_),		\
		.state = VMA_MERGE_START,			\
	}

#ifdef CONFIG_DEBUG_VM_MAPLE_TREE
/* 调试配置下遍历并校验 mm 的 VMA Maple Tree 一致性；失败会报告诊断。 */
void validate_mm(struct mm_struct *mm);
#else
/* 非调试构建为空操作，不求值 mm，也不产生运行时成本。 */
#define validate_mm(mm) do { } while (0)
#endif

__must_check int vma_expand(struct vma_merge_struct *vmg);
/* vma_expand 成功返回 0；失败返回负 errno，调用者必须检查且布局不应半提交。 */
__must_check int vma_shrink(struct vma_iterator *vmi,
		struct vm_area_struct *vma,
		unsigned long start, unsigned long end, pgoff_t pgoff);

static inline int vma_iter_store_gfp(struct vma_iterator *vmi,
			struct vm_area_struct *vma, gfp_t gfp)

{
	/*
	 * 业务背景：按指定 GFP 将 VMA 写入 Maple Tree；入参要求 VMA 边界稳定。
	 * 返回 0 后节点已附着；返回 -ENOMEM 时存储失败，调用者仍拥有未发布对象。
	 * 注意：游标范围不覆盖 vm_start 时先失效，避免沿用陈旧搜索路径。
	 */
	if (vmi->mas.status != ma_start &&
	    ((vmi->mas.index > vma->vm_start) || (vmi->mas.last < vma->vm_start)))
		vma_iter_invalidate(vmi);

	__mas_set_range(&vmi->mas, vma->vm_start, vma->vm_end - 1);
	mas_store_gfp(&vmi->mas, vma, gfp);
	/* Maple Tree 把分配失败编码进 mas；附着标记只能在确认成功后发布。 */
	if (unlikely(mas_is_err(&vmi->mas)))
		return -ENOMEM;

	vma_mark_attached(vma);
	return 0;
}

/*
 * Temporary helper function for stacked mmap handlers which specify
 * f_op->mmap() but which might have an underlying file system which implements
 * f_op->mmap_prepare().
 */
/* 译注：兼容仍实现 mmap 的叠加处理器与已迁移到 mmap_prepare 的底层文件系统。 */
static inline void compat_set_vma_from_desc(struct vm_area_struct *vma,
		struct vm_area_desc *desc)
{
	/*
	 * 业务背景：兼容叠加文件同时存在旧 mmap 与新 mmap_prepare；入参是半成品
	 * VMA 和准备结果。出参写回可变字段与驱动字段；返回 void。
	 * 注意：文件变化必须经 vma_set_file 交接引用，不能直接覆盖 vm_file。
	 */
	/*
	 * Since we're invoking .mmap_prepare() despite having a partially
	 * established VMA, we must take care to handle setting fields
	 * correctly.
	 */
	/* 译注：在已有半成品 VMA 上调用底层 mmap_prepare，写字段时需格外谨慎。 */

	/* Mutable fields. Populated with initial state. */
	/* 译注：这些可变字段先从准备描述符覆盖为最终初始状态。 */
	vma->vm_pgoff = desc->pgoff;
	if (desc->vm_file != vma->vm_file)
		vma_set_file(vma, desc->vm_file);
	vma->flags = desc->vma_flags;
	vma->vm_page_prot = desc->page_prot;

	/* User-defined fields. */
	/* 译注：驱动提供的 ops/private_data 作为用户定义部分直接交给 VMA。 */
	vma->vm_ops = desc->vm_ops;
	vma->vm_private_data = desc->private_data;
}

int
do_vmi_align_munmap(struct vma_iterator *vmi, struct vm_area_struct *vma,
		    struct mm_struct *mm, unsigned long start,
		    unsigned long end, struct list_head *uf, bool unlock);
/*
 * do_vmi_align_munmap 接收已页对齐区间和定位到首 VMA 的游标，完成拆分、
 * 脱链、页表解除映射和 mm 记账；0 表示成功，负 errno 表示准备/提交失败。
 * unlock 为真时函数可能在成功路径释放 mmap 写锁，调用者不可再假定持锁。
 */

int do_vmi_munmap(struct vma_iterator *vmi, struct mm_struct *mm,
		  unsigned long start, size_t len, struct list_head *uf,
		  bool unlock);
/* do_vmi_munmap 额外校验并对齐 start/len，再进入上述核心路径。 */

/* remove_vma 释放单个已脱链 VMA 的文件、策略和对象生命期资源。 */
void remove_vma(struct vm_area_struct *vma);
/* unmap_region 消费 unmap_desc，在安全边界内清 PTE/页表并完成 TLB 协议。 */
void unmap_region(struct unmap_desc *unmap);

/**
 * vma_modify_flags() - Perform any necessary split/merge in preparation for
 * setting VMA flags to *@vm_flags in the range @start to @end contained within
 * @vma.
 * @vmi: Valid VMA iterator positioned at @vma.
 * @prev: The VMA immediately prior to @vma or NULL if @vma is the first.
 * @vma: The VMA containing the range @start to @end to be updated.
 * @start: The start of the range to update. May be offset within @vma.
 * @end: The exclusive end of the range to update, may be offset within @vma.
 * @vma_flags_ptr: A pointer to the VMA flags that the @start to @end range is
 * about to be set to. On merge, this will be updated to include sticky flags.
 *
 * IMPORTANT: The actual modification being requested here is NOT applied,
 * rather the VMA is perhaps split, perhaps merged to accommodate the change,
 * and the caller is expected to perform the actual modification.
 *
 * In order to account for sticky VMA flags, the @vma_flags_ptr parameter points
 * to the requested flags which are then updated so the caller, should they
 * overwrite any existing flags, correctly retains these.
 *
 * Returns: A VMA which contains the range @start to @end ready to have its
 * flags altered to *@vma_flags.
 */
/*
 * 译注：该接口只为属性更新整理 VMA 边界，并不写入最终 flags。成功返回
 * 覆盖目标区间的 VMA，且可能通过 vma_flags_ptr 加回 sticky 位；失败返回
 * ERR_PTR。调用者随后才在锁保护下写属性，不能把“准备成功”当作已生效。
 */
__must_check struct vm_area_struct *vma_modify_flags(struct vma_iterator *vmi,
		struct vm_area_struct *prev, struct vm_area_struct *vma,
		unsigned long start, unsigned long end, vma_flags_t *vma_flags_ptr);

/**
 * vma_modify_name() - Perform any necessary split/merge in preparation for
 * setting anonymous VMA name to @new_name in the range @start to @end contained
 * within @vma.
 * @vmi: Valid VMA iterator positioned at @vma.
 * @prev: The VMA immediately prior to @vma or NULL if @vma is the first.
 * @vma: The VMA containing the range @start to @end to be updated.
 * @start: The start of the range to update. May be offset within @vma.
 * @end: The exclusive end of the range to update, may be offset within @vma.
 * @new_name: The anonymous VMA name that the @start to @end range is about to
 * be set to.
 *
 * IMPORTANT: The actual modification being requested here is NOT applied,
 * rather the VMA is perhaps split, perhaps merged to accommodate the change,
 * and the caller is expected to perform the actual modification.
 *
 * Returns: A VMA which contains the range @start to @end ready to have its
 * anonymous VMA name changed to @new_name.
 */
/*
 * 译注：该接口按新匿名名称的可合并性拆分/合并边界，返回待写名的 VMA；
 * 它不替调用者安装 new_name，错误用 ERR_PTR 表示，引用交接仍由调用者完成。
 */
__must_check struct vm_area_struct *vma_modify_name(struct vma_iterator *vmi,
		struct vm_area_struct *prev, struct vm_area_struct *vma,
		unsigned long start, unsigned long end,
		struct anon_vma_name *new_name);

/**
 * vma_modify_policy() - Perform any necessary split/merge in preparation for
 * setting NUMA policy to @new_pol in the range @start to @end contained
 * within @vma.
 * @vmi: Valid VMA iterator positioned at @vma.
 * @prev: The VMA immediately prior to @vma or NULL if @vma is the first.
 * @vma: The VMA containing the range @start to @end to be updated.
 * @start: The start of the range to update. May be offset within @vma.
 * @end: The exclusive end of the range to update, may be offset within @vma.
 * @new_pol: The NUMA policy that the @start to @end range is about to be set
 * to.
 *
 * IMPORTANT: The actual modification being requested here is NOT applied,
 * rather the VMA is perhaps split, perhaps merged to accommodate the change,
 * and the caller is expected to perform the actual modification.
 *
 * Returns: A VMA which contains the range @start to @end ready to have its
 * NUMA policy changed to @new_pol.
 */
/*
 * 译注：该接口只把目标区间整理成可独立设置 NUMA policy 的 VMA；成功后
 * 调用者再更换策略并处理引用，失败返回 ERR_PTR，原有区间语义应保持有效。
 */
__must_check struct vm_area_struct *vma_modify_policy(struct vma_iterator *vmi,
		   struct vm_area_struct *prev, struct vm_area_struct *vma,
		   unsigned long start, unsigned long end,
		   struct mempolicy *new_pol);

/**
 * vma_modify_flags_uffd() - Perform any necessary split/merge in preparation for
 * setting VMA flags to @vm_flags and UFFD context to @new_ctx in the range
 * @start to @end contained within @vma.
 * @vmi: Valid VMA iterator positioned at @vma.
 * @prev: The VMA immediately prior to @vma or NULL if @vma is the first.
 * @vma: The VMA containing the range @start to @end to be updated.
 * @start: The start of the range to update. May be offset within @vma.
 * @end: The exclusive end of the range to update, may be offset within @vma.
 * @vma_flags: The VMA flags that the @start to @end range is about to be set to.
 * @new_ctx: The userfaultfd context that the @start to @end range is about to
 * be set to.
 * @give_up_on_oom: If an out of memory condition occurs on merge, simply give
 * up on it and treat the merge as best-effort.
 *
 * IMPORTANT: The actual modification being requested here is NOT applied,
 * rather the VMA is perhaps split, perhaps merged to accommodate the change,
 * and the caller is expected to perform the actual modification.
 *
 * Returns: A VMA which contains the range @start to @end ready to have its VMA
 * flags changed to @vma_flags and its userfaultfd context changed to @new_ctx.
 */
/*
 * 译注：flags 与 UFFD 上下文必须作为同一合并判据。give_up_on_oom 允许把
 * 合并分配失败视作尽力失败；返回 VMA 后属性仍未安装，须由调用者提交。
 */
__must_check struct vm_area_struct *vma_modify_flags_uffd(struct vma_iterator *vmi,
		struct vm_area_struct *prev, struct vm_area_struct *vma,
		unsigned long start, unsigned long end, const vma_flags_t *vma_flags,
		struct vm_userfaultfd_ctx new_ctx, bool give_up_on_oom);

/*
 * vma_merge_new_range 尝试把尚不存在的新区间并入左右邻居。成功返回幸存
 * VMA，不能合并或按策略放弃时返回 NULL；vmg->state 区分 NOMERGE/OOM。
 */
__must_check struct vm_area_struct *vma_merge_new_range(struct vma_merge_struct *vmg);

/* 将既有 vma 向后扩 delta，并在可能时吞并邻居；返回最终 VMA 或错误/NULL。 */
__must_check struct vm_area_struct *vma_merge_extend(struct vma_iterator *vmi,
		  struct vm_area_struct *vma, unsigned long delta);

/* 初始化文件 VMA 批处理缓存；不取得 VMA 引用，调用者必须维持其生命期。 */
void unlink_file_vma_batch_init(struct unlink_vma_file_batch *vb);

/* 冲刷剩余批次并完成文件映射树脱链；调用后缓存重新为空。 */
void unlink_file_vma_batch_final(struct unlink_vma_file_batch *vb);

/* 加入待脱链 VMA，数组满时内部先冲刷；要求相关映射锁序由调用者满足。 */
void unlink_file_vma_batch_add(struct unlink_vma_file_batch *vb,
			       struct vm_area_struct *vma);

struct vm_area_struct *copy_vma(struct vm_area_struct **vmap,
	unsigned long addr, unsigned long len, pgoff_t pgoff,
	bool *need_rmap_locks);
/*
 * copy_vma 为移动/复制区间复用或复制 VMA。成功通过 vmap 返回目标并告知
 * 是否需 anon_vma 反向映射锁；失败返回 NULL，未把半成品发布到调用者。
 */

/* 查找可与 vma 共享 anon_vma 的邻居；返回借用指针，NULL 表示不存在。 */
struct anon_vma *find_mergeable_anon_vma(struct vm_area_struct *vma);

/* 判断共享可写映射是否需要软件 dirty/write-notify 跟踪。 */
bool vma_needs_dirty_tracking(struct vm_area_struct *vma);
/* 结合页保护判断 write fault 是否应承担首次写通知。 */
bool vma_wants_writenotify(struct vm_area_struct *vma, pgprot_t vm_page_prot);

/* 获取 mm 内所有 anon_vma 与文件 mapping 锁；失败返回负 errno并回滚已取锁。 */
int mm_take_all_locks(struct mm_struct *mm);
/* 与成功的 mm_take_all_locks 配对，释放全套锁和临时标记。 */
void mm_drop_all_locks(struct mm_struct *mm);

/* 建立 mmap 区域并发布到 mm；成功返回地址，失败返回编码后的负 errno。 */
unsigned long mmap_region(struct file *file, unsigned long addr,
		unsigned long len, vm_flags_t vm_flags, unsigned long pgoff,
		struct list_head *uf);

/* 扩展或建立 brk VMA；0 成功，负 errno 表示越界、冲突或分配失败。 */
int do_brk_flags(struct vma_iterator *vmi, struct vm_area_struct *brkvma,
		 unsigned long addr, unsigned long request,
		 vma_flags_t vma_flags);

/* 两种空洞搜索分别自低向高、自高向低；失败返回编码错误地址。 */
unsigned long unmapped_area(struct vm_unmapped_area_info *info);
unsigned long unmapped_area_topdown(struct vm_unmapped_area_info *info);

static inline bool vma_wants_manual_pte_write_upgrade(struct vm_area_struct *vma)
{
	/*
	 * 业务背景：批量改保护后判断是否还需逐 PTE 升级写权限；入参是目标 VMA。
	 * 返回 true 表示 fault/保护路径必须逐项处理；false 可保持统一只读策略。
	 * 注意：私有写映射必须保留 COW，共享映射则服从 write-notify 规则。
	 */
	/*
	 * We want to check manually if we can change individual PTEs writable
	 * if we can't do that automatically for all PTEs in a mapping. For
	 * private mappings, that's always the case when we have write
	 * permissions as we properly have to handle COW.
	 */
	/* 译注：不能整段自动升级时才逐项判断；私有映射的写权限始终受 COW 约束。 */
	if (vma->vm_flags & VM_SHARED)
		return vma_wants_writenotify(vma, vma->vm_page_prot);
	return !!(vma->vm_flags & VM_WRITE);
}

#ifdef CONFIG_MMU
static inline pgprot_t vm_pgprot_modify(pgprot_t oldprot, vm_flags_t vm_flags)
{
	/* 保留 oldprot 的架构缓存等属性，仅按新 VMA flags 改访问保护；返回新 pgprot。 */
	return pgprot_modify(oldprot, vm_get_page_prot(vm_flags));
}
#endif

static inline struct vm_area_struct *vma_prev_limit(struct vma_iterator *vmi,
						    unsigned long min)
{
	/* 向前找但不越过 min；返回借用 VMA 指针，找不到时为 NULL。 */
	return mas_prev(&vmi->mas, min);
}

/*
 * These three helpers classifies VMAs for virtual memory accounting.
 */
/* 译注：下列分类供 mm 的 exec_vm/stack_vm/data_vm 计数使用，类别可非穷尽。 */

/*
 * Executable code area - executable, not writable, not stack
 */
/* 译注：代码区必须可执行、不可写且不是栈。 */
static inline bool is_exec_mapping(vm_flags_t flags)
{
	/* 输入 flags，返回是否应计入 exec_vm；无状态修改。 */
	return (flags & (VM_EXEC | VM_WRITE | VM_STACK)) == VM_EXEC;
}

/*
 * Stack area (including shadow stacks)
 *
 * VM_GROWSUP / VM_GROWSDOWN VMAs are always private anonymous:
 * do_mmap() forbids all other combinations.
 */
/* 译注：栈含普通增长栈和 shadow stack；增长型 VMA 已由 do_mmap 限为私有匿名。 */
static inline bool is_stack_mapping(vm_flags_t flags)
{
	/* 输入 flags，返回是否应计入 stack_vm；VM_SHADOW_STACK 单独成立。 */
	return ((flags & VM_STACK) == VM_STACK) || (flags & VM_SHADOW_STACK);
}

/*
 * Data area - private, writable, not stack
 */
/* 译注：数据区是私有可写且非栈映射。 */
static inline bool is_data_mapping(vm_flags_t flags)
{
	/* 旧 vm_flags 位图版本；返回是否计入 data_vm。 */
	return (flags & (VM_WRITE | VM_SHARED | VM_STACK)) == VM_WRITE;
}

static inline bool is_data_mapping_vma_flags(const vma_flags_t *vma_flags)
{
	/* 类型安全 flags 版本，与 is_data_mapping 保持同一分类语义。 */
	return vma_flags_test(vma_flags, VMA_WRITE_BIT) &&
		!vma_flags_test_any(vma_flags, VMA_SHARED_BIT, VMA_STACK_BIT);
}

static inline void vma_iter_config(struct vma_iterator *vmi,
		unsigned long index, unsigned long last)
{
	/*
	 * 业务背景：为下一次 Maple Tree 操作配置半开区间 [index,last)。
	 * 入参要求 last > index；出参写入 mas 的闭区间末端，返回 void。
	 * 注意：这里只移动游标状态，不访问或修改 VMA 树。
	 */
	__mas_set_range(&vmi->mas, index, last - 1);
}

static inline void vma_iter_reset(struct vma_iterator *vmi)
{
	/* 将 Maple 状态复位到起始态；既不释放预分配节点，也不改变树。 */
	mas_reset(&vmi->mas);
}

static inline
struct vm_area_struct *vma_iter_prev_range_limit(struct vma_iterator *vmi, unsigned long min)
{
	/* 向前遍历覆盖区间且不越过 min，返回借用 VMA 或 NULL。 */
	return mas_prev_range(&vmi->mas, min);
}

static inline
struct vm_area_struct *vma_iter_next_range_limit(struct vma_iterator *vmi, unsigned long max)
{
	/* 向后遍历覆盖区间且不越过 max，返回借用 VMA 或 NULL。 */
	return mas_next_range(&vmi->mas, max);
}

static inline int vma_iter_area_lowest(struct vma_iterator *vmi, unsigned long min,
				       unsigned long max, unsigned long size)
{
	/* 在 [min,max) 自低向高找 size 大小空洞；0 成功并定位游标，负 errno 失败。 */
	return mas_empty_area(&vmi->mas, min, max - 1, size);
}

static inline int vma_iter_area_highest(struct vma_iterator *vmi, unsigned long min,
					unsigned long max, unsigned long size)
{
	/* 在 [min,max) 自高向低找空洞；返回约定同 lowest，max 转为闭区间传入。 */
	return mas_empty_area_rev(&vmi->mas, min, max - 1, size);
}

/*
 * VMA Iterator functions shared between nommu and mmap
 */
/* 译注：以下包装同时供 NOMMU 与 mmap 路径使用，游标状态规则来自 Maple Tree。 */
static inline int vma_iter_prealloc(struct vma_iterator *vmi,
		struct vm_area_struct *vma)
{
	/* 为随后存储 vma 预分配 Maple 节点；0 成功，负 errno 失败且尚未发布 VMA。 */
	return mas_preallocate(&vmi->mas, vma, GFP_KERNEL);
}

static inline void vma_iter_clear(struct vma_iterator *vmi)
{
	/* 用已经预分配的节点把当前配置区间清空；要求先成功 prealloc。 */
	mas_store_prealloc(&vmi->mas, NULL);
}

static inline struct vm_area_struct *vma_iter_load(struct vma_iterator *vmi)
{
	/* 在当前索引查找 VMA；返回树中借用指针或 NULL，不增加 VMA 引用。 */
	return mas_walk(&vmi->mas);
}

/* Store a VMA with preallocated memory */
/* 译注：使用预分配内存覆盖存储一个已经标记为 attached 的 VMA。 */
static inline void vma_iter_store_overwrite(struct vma_iterator *vmi,
					    struct vm_area_struct *vma)
{
	/*
	 * 业务背景：边界调整后覆盖 Maple Tree 中的 VMA 区间；入参要求预分配
	 * 已成功且 vma 已 attached。出参把 [vm_start,vm_end) 发布到树，返回 void。
	 * 注意：游标不覆盖 vm_start 时先失效重定位；调试配置额外报告错位状态。
	 */
	vma_assert_attached(vma);

#if defined(CONFIG_DEBUG_VM_MAPLE_TREE)
	/* 调试检查只诊断游标/VMA 范围矛盾，不改变正常配置的提交算法。 */
	if (MAS_WARN_ON(&vmi->mas, vmi->mas.status != ma_start &&
			vmi->mas.index > vma->vm_start)) {
		pr_warn("%lx > %lx\n store vma %lx-%lx\n into slot %lx-%lx\n",
			vmi->mas.index, vma->vm_start, vma->vm_start,
			vma->vm_end, vmi->mas.index, vmi->mas.last);
	}
	/* 第二个断言捕获游标末端尚未到达 VMA 起点的反向错位。 */
	if (MAS_WARN_ON(&vmi->mas, vmi->mas.status != ma_start &&
			vmi->mas.last <  vma->vm_start)) {
		pr_warn("%lx < %lx\nstore vma %lx-%lx\ninto slot %lx-%lx\n",
		       vmi->mas.last, vma->vm_start, vma->vm_start, vma->vm_end,
		       vmi->mas.index, vmi->mas.last);
	}
#endif

	if (vmi->mas.status != ma_start &&
	    ((vmi->mas.index > vma->vm_start) || (vmi->mas.last < vma->vm_start)))
		vma_iter_invalidate(vmi);

	/* 最后用半开 VMA 边界配置闭区间并消费预分配节点完成发布。 */
	__mas_set_range(&vmi->mas, vma->vm_start, vma->vm_end - 1);
	mas_store_prealloc(&vmi->mas, vma);
}

static inline void vma_iter_store_new(struct vma_iterator *vmi,
				      struct vm_area_struct *vma)
{
	/* 新 VMA 必须先标记 attached，再复用 overwrite 发布；调用者已持 mmap 写锁。 */
	vma_mark_attached(vma);
	vma_iter_store_overwrite(vmi, vma);
}

static inline unsigned long vma_iter_addr(struct vma_iterator *vmi)
{
	/* 返回当前 Maple 状态的闭区间起点。 */
	return vmi->mas.index;
}

static inline unsigned long vma_iter_end(struct vma_iterator *vmi)
{
	/* 把 Maple Tree 的闭区间 last 转回 VMA API 的半开区间末端。 */
	return vmi->mas.last + 1;
}

static inline
struct vm_area_struct *vma_iter_prev_range(struct vma_iterator *vmi)
{
	/* 无下界限制地取前一覆盖区间；返回借用 VMA 或 NULL。 */
	return mas_prev_range(&vmi->mas, 0);
}

/*
 * Retrieve the next VMA and rewind the iterator to end of the previous VMA, or
 * if no previous VMA, to index 0.
 */
/* 译注：取得 next 后把游标退到 prev 末端；没有 prev 时退到索引 0。 */
static inline
struct vm_area_struct *vma_iter_next_rewind(struct vma_iterator *vmi,
		struct vm_area_struct **pprev)
{
	/*
	 * 业务背景：删除/合并循环需同时知道两侧 VMA 并让下次遍历从前驱末端开始。
	 * 入参 pprev 可选；出参返回 next，并通过 pprev 返回 prev，二者均为借用指针。
	 * 注意：无 prev 时不能再跳一个 range，否则会越过刚取得的 next。
	 */
	struct vm_area_struct *next = vma_next(vmi);
	struct vm_area_struct *prev = vma_prev(vmi);

	/*
	 * Consider the case where no previous VMA exists. We advance to the
	 * next VMA, skipping any gap, then rewind to the start of the range.
	 *
	 * If we were to unconditionally advance to the next range we'd wind up
	 * at the next VMA again, so we check to ensure there is a previous VMA
	 * to skip over.
	 */
	/* 译注：先跨过空洞取得 next，再按是否存在 prev 决定是否推进一个 range。 */
	if (prev)
		vma_iter_next_range(vmi);

	if (pprev)
		*pprev = prev;

	return next;
}

#ifdef CONFIG_64BIT
static inline bool vma_is_sealed(struct vm_area_struct *vma)
{
	/* 64 位把 VM_SEALED 编入 flags；返回该 VMA 是否禁止后续边界/权限变更。 */
	return (vma->vm_flags & VM_SEALED);
}
#else
static inline bool vma_is_sealed(struct vm_area_struct *vma)
{
	/* 非 64 位没有可用 flag 位，配置桩恒为 false 且不读取 vma。 */
	return false;
}
#endif

#if defined(CONFIG_STACK_GROWSUP)
/* 向上增长栈至 address；成功 0，失败负 errno，受 mmap 写锁和邻接 VMA 限制。 */
int expand_upwards(struct vm_area_struct *vma, unsigned long address);
#endif

/* 向下增长栈至 address；返回 0 或负 errno，并同步更新 VMA/anon_vma 记账。 */
int expand_downwards(struct vm_area_struct *vma, unsigned long address);

/* 内核 munmap 入口：校验 start/len；unlock 控制成功后是否释放 mmap 写锁。 */
int __vm_munmap(unsigned long start, size_t len, bool unlock);

/* 把调用者准备好的 VMA 插入 mm；成功 0，失败负 errno且所有权仍由调用者处理。 */
int insert_vm_struct(struct mm_struct *mm, struct vm_area_struct *vma);

/* vma_init.h, shared between CONFIG_MMU and nommu. */
/* 译注：以下对象生命期接口由 MMU/NOMMU 共用，实现在 vma_init.c。 */
/* 初始化 VMA slab/cache 等全局状态；仅启动阶段调用。 */
void __init vma_state_init(void);
/* 为 mm 分配并初始化 VMA；成功返回自有对象，失败 NULL。 */
struct vm_area_struct *vm_area_alloc(struct mm_struct *mm);
/* 复制 orig 的 VMA 元数据和所需引用；成功返回新对象，失败 NULL。 */
struct vm_area_struct *vm_area_dup(struct vm_area_struct *orig);
/* 释放尚未发布或已安全脱链的 VMA，并归还其对象存储。 */
void vm_area_free(struct vm_area_struct *vma);

/* vma_exec.c */
/* 译注：以下 exec 建栈接口位于 vma_exec.c，只有 MMU 构建存在。 */
#ifdef CONFIG_MMU
/* 创建初始用户栈 VMA；成功更新 vmap/top_mem_p，失败返回负 errno并清理半成品。 */
int create_init_stack_vma(struct mm_struct *mm, struct vm_area_struct **vmap,
			  unsigned long *top_mem_p);
/* 将栈 VMA 整体下移 shift；成功 0，失败负 errno且不留下半提交布局。 */
int relocate_vma_down(struct vm_area_struct *vma, unsigned long shift);
#endif

#ifdef CONFIG_MMU
/*
 * Denies creating a writable executable mapping or gaining executable permissions.
 *
 * This denies the following:
 *
 *	a)	mmap(PROT_WRITE | PROT_EXEC)
 *
 *	b)	mmap(PROT_WRITE)
 *		mprotect(PROT_EXEC)
 *
 *	c)	mmap(PROT_WRITE)
 *		mprotect(PROT_READ)
 *		mprotect(PROT_EXEC)
 *
 * But allows the following:
 *
 *	d)	mmap(PROT_READ | PROT_EXEC)
 *		mmap(PROT_READ | PROT_EXEC | PROT_BTI)
 *
 * This is only applicable if the user has set the Memory-Deny-Write-Execute
 * (MDWE) protection mask for the current process.
 *
 * @old specifies the VMA flags the VMA originally possessed, and @new the ones
 * we propose to set.
 *
 * Return: false if proposed change is OK, true if not ok and should be denied.
 */
/*
 * 译注：MDWE 禁止新建 W+X，也禁止原先不可执行的 VMA 后来获得 X；原本
 * RX 的映射可保留执行权限并增加非写属性。仅 current->mm 已设置 MDWE 时
 * 生效。返回 true 是拒绝提议，false 是此策略不阻止，非通用权限校验结果。
 */
static inline bool map_deny_write_exec(const vma_flags_t *old,
				       const vma_flags_t *new)
{
	/* If MDWE is disabled, we have nothing to deny. */
	/* 译注：进程未启用 MDWE 时不施加额外限制。 */
	if (!mm_flags_test(MMF_HAS_MDWE, current->mm))
		return false;

	/* If the new VMA is not executable, we have nothing to deny. */
	/* 译注：新状态不可执行时不可能违反“写后执行”约束。 */
	if (!vma_flags_test(new, VMA_EXEC_BIT))
		return false;

	/* Under MDWE we do not accept newly writably executable VMAs... */
	/* 译注：任何提议的可写且可执行状态都直接拒绝。 */
	if (vma_flags_test(new, VMA_WRITE_BIT))
		return true;

	/* ...nor previously non-executable VMAs becoming executable. */
	/* 译注：即使先去掉写权限，也不允许曾经不可执行的 VMA 再获得执行权。 */
	if (!vma_flags_test(old, VMA_EXEC_BIT))
		return true;

	return false;
}
#endif

#endif	/* __MM_VMA_H */
