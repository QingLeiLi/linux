/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PGALLOC_TRACK_H
#define _LINUX_PGALLOC_TRACK_H

#if defined(CONFIG_MMU)
/*
 * p4d_alloc_track() - 获取 address 对应的 P4D，并记录是否新建了 PGD 下级表。
 * 业务背景：vmalloc/ioremap/apply_to_page_range 从顶层向下建立页表；架构稍后根据
 * 累积的 pgtbl_mod_mask 判断是否要把内核映射同步到其他页表。
 * 入参：mm 是被修改地址空间的借用指针；pgd 是 address 所在 PGD entry 的输入输出
 * 借用指针；address 是虚拟字节地址；mod_mask 是不可为 NULL 的输入输出位图，
 * 调用前可含其他层修改位，函数只做 OR、不清除旧状态，均不转移 ownership。
 * 出参/返回：已有或成功创建下级表时返回 address 对应的 P4D 借用指针；分配失败
 * 返回 NULL。只有本次填充空 PGD 时才置 PGTBL_PGD_MODIFIED。
 * 注意事项：__p4d_alloc() 可能分配页表并失败/睡眠，调用者须处于允许建立页表的
 * 上下文并负责外层映射同步；折叠 P4D 的架构仍保持同一接口和位图契约。
 */
static inline p4d_t *p4d_alloc_track(struct mm_struct *mm, pgd_t *pgd,
				     unsigned long address,
				     pgtbl_mod_mask *mod_mask)
{
	/* 快路径直接复用已有下级表；unlikely 提示空 PGD 通常不是稳态。 */
	if (unlikely(pgd_none(*pgd))) {
		/* 慢路径先创建并发布下级表；失败时位图保持调用前状态。 */
		if (__p4d_alloc(mm, pgd, address))
			return NULL;
		/* 位记录的是父级 entry 被填充，供批次结束时决定架构同步范围。 */
		*mod_mask |= PGTBL_PGD_MODIFIED;
	}

	/* 无论复用还是新建，统一返回 address 在该 P4D 表中的 entry。 */
	return p4d_offset(pgd, address);
}

/*
 * pud_alloc_track() - 获取 address 对应的 PUD，并跟踪 P4D entry 的新建。
 * 业务背景：它是五级页表下降链的 P4D→PUD 阶段，调用者依据累计位图在整段映射
 * 完成后一次性执行必要的 arch_sync_kernel_mappings()。
 * 入参：mm 是地址空间借用指针；p4d 是待检查父 entry 的输入输出借用指针；address
 * 是虚拟字节地址；mod_mask 是不可为 NULL 的累积输入输出位图，只增添位。
 * 出参/返回：成功返回对应 PUD entry 的借用指针；__pud_alloc() 失败返回 NULL；
 * 仅本次填充空 P4D 时置 PGTBL_P4D_MODIFIED，不取得页表长期引用。
 * 注意事项：分配可睡眠且可能返回 -ENOMEM（在此折成 NULL）；页表层折叠时 helper
 * 可退化为无分配偏移计算，但外部契约不变。同步与失败清理由上层负责。
 */
static inline pud_t *pud_alloc_track(struct mm_struct *mm, p4d_t *p4d,
				     unsigned long address,
				     pgtbl_mod_mask *mod_mask)
{
	/* 已有 PUD 表是常见快路径；仅空父 entry 才进入分配阶段。 */
	if (unlikely(p4d_none(*p4d))) {
		if (__pud_alloc(mm, p4d, address))
			return NULL;
		/* 成功发布 PUD 表后再记位，避免把失败尝试误报成可见页表修改。 */
		*mod_mask |= PGTBL_P4D_MODIFIED;
	}

	/* 返回值受 mm/页表生命周期保护，本函数不额外持有引用。 */
	return pud_offset(p4d, address);
}

/*
 * pmd_alloc_track() - 获取 address 对应的 PMD，并跟踪 PUD entry 的新建。
 * 业务背景：vmalloc 和通用页表遍历在继续建立 huge PMD 或 PTE 表前调用本层，
 * 用 PGTBL_PUD_MODIFIED 告知架构父层指针发生过变化。
 * 入参：mm 是地址空间借用指针；pud 是父 entry 的输入输出借用指针；address 为
 * 虚拟字节地址；mod_mask 是不可为 NULL 的累积输入输出位图，ownership 不改变。
 * 出参/返回：复用/创建成功返回 PMD entry 借用指针；分配失败返回 NULL；仅成功
 * 填充原本为空的 PUD 时 OR 入 PGTBL_PUD_MODIFIED。
 * 注意事项：可能睡眠和失败；调用者负责把 NULL 转换为 -ENOMEM，并在批次末依据
 * ARCH_PAGE_TABLE_SYNC_MASK 同步。折叠 PMD 的配置保留同样的调用形式。
 */
static inline pmd_t *pmd_alloc_track(struct mm_struct *mm, pud_t *pud,
				     unsigned long address,
				     pgtbl_mod_mask *mod_mask)
{
	/* 先检查父 entry，避免已有页表上的重复分配和无意义修改位。 */
	if (unlikely(pud_none(*pud))) {
		if (__pmd_alloc(mm, pud, address))
			return NULL;
		/* __pmd_alloc 成功后父 PUD 已对页表遍历者可见，此时提交修改位。 */
		*mod_mask |= PGTBL_PUD_MODIFIED;
	}

	/* offset helper 只计算 entry 地址，不转移新建页表页的 ownership。 */
	return pmd_offset(pud, address);
}
#endif /* CONFIG_MMU */
/* 上述三级分配/跟踪 helper 只在具有硬件 MMU 页表的配置中提供。 */

/*
 * pte_alloc_kernel_track() - 为内核映射获取 PTE entry 并跟踪 PMD 修改。
 * 业务背景：这是下降链的最后一级；vmalloc/apply_to_page_range 用它创建内核 PTE
 * 表，并把是否填充过 PMD 汇入与上层相同的 pgtbl_mod_mask。
 * 入参：pmd 是父 entry 的输入输出指针表达式；address 是虚拟字节地址；mask 是
 * 不可为 NULL 的输入输出位图指针。参数会在宏中求值，必须使用无副作用表达式。
 * 出参/返回：PMD 已存在或成功创建时返回对应 kernel PTE 借用指针；分配失败返回
 * NULL。只有成功创建空 PMD 的下级表时 OR 入 PGTBL_PMD_MODIFIED。
 * 注意事项：宏用短路逻辑和 GNU statement expression 保证“分配成功后才记位”；
 * __pte_alloc_kernel() 可能睡眠。调用者负责页表锁协议、NULL→-ENOMEM 和最终同步。
 */
#define pte_alloc_kernel_track(pmd, address, mask)			\
	((unlikely(pmd_none(*(pmd))) &&					\
	  (__pte_alloc_kernel(pmd) || ({*(mask)|=PGTBL_PMD_MODIFIED;0;})))?\
		NULL: pte_offset_kernel(pmd, address))

#endif /* _LINUX_PGALLOC_TRACK_H */
/* 结束 pgalloc-track.h 的防重复包含范围。 */
