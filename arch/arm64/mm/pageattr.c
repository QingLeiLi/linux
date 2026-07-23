// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 内核页表属性动态修改学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件为模块、vmalloc、debug-pagealloc、KFENCE 与 Realm 提供 RO/RW、
 * X/NX、valid/invalid 以及共享/私有状态切换。核心链路是：验证范围只由
 * 一个普通 vmalloc 区覆盖 -> 必要时同步修改线性映射别名 -> 清理延迟
 * vmap 别名 -> walk 内核页表逐 leaf 改位 -> 按硬件规则刷新 TLB。
 *
 * 页表 walk 在调用者保证映射稳定的启动/内核映射管理上下文运行；
 * lazy MMU mode 批量延后昂贵同步。Realm 转换是 break-before-make 三阶段：
 * 先 invalid 映射、请求 RMM 改物理归属、再 valid；中途失败故意保持页面
 * 不可访问并由调用者泄漏，优先避免 host/Realm 对同一页产生冲突别名。
 */
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mem_encrypt.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/pagewalk.h>

#include <asm/cacheflush.h>
#include <asm/pgtable-prot.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>
#include <asm/kfence.h>

/* 一次页表 walk 要置位与清位的掩码；由 walk->private 借用，生命周期仅本次调用。 */
/*
 * clear_mask 指定从每个叶子先移除的属性，set_mask 指定随后加入的属性；二者
 * 可以包含物理上 alias 的架构位，故不能合并成无序赋值。对象通常在调用栈上，
 * walker 只借指针、不取得所有权，walk 返回后不再访问。
 */
struct page_change_data {
	pgprot_t set_mask;
	pgprot_t clear_mask;
};

/*
 * 对一个 leaf 描述符应用 clear-then-set。val 是原始 PTE/PMD/PUD 位值，
 * walk->private 必须指向 page_change_data；返回新位值。先清后置是硬性
 * 顺序，因为 PTE_NG/PTE_PRESENT_INVALID 等不同语义位可能物理 alias，
 * 反序会把刚设置的目标状态再次清掉。
 */
static ptval_t set_pageattr_masks(ptval_t val, struct mm_walk *walk)
{
	struct page_change_data *masks = walk->private;

	/*
	 * Some users clear and set bits which alias each other (e.g. PTE_NG and
	 * PTE_PRESENT_INVALID). It is therefore important that we always clear
	 * first then set.
	 */
	/* clear_mask 与 set_mask 可以包含同一物理位，最终明确以 set 状态为准。 */
	val &= ~(pgprot_val(masks->clear_mask));
	val |= (pgprot_val(masks->set_mask));

	return val;
}

/*
 * pagewalk 的 PUD leaf 回调。pud 指向当前表项，[addr,next) 必须完整覆盖
 * 一个 PUD_SIZE block，否则拒绝部分修改 block；非 leaf 留给下一层 walk。
 * 修改成功后 ACTION_CONTINUE 告诉 walker 不再下钻此 leaf。返回 0/-EINVAL。
 */
static int pageattr_pud_entry(pud_t *pud, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	pud_t val = pudp_get(pud);

	if (pud_leaf(val)) {
		if (WARN_ON_ONCE((next - addr) != PUD_SIZE))
			return -EINVAL;
		val = __pud(set_pageattr_masks(pud_val(val), walk));
		set_pud(pud, val);
		walk->action = ACTION_CONTINUE;
	}

	return 0;
}

/* PMD block 版本，契约同 PUD：只允许整块更新，防止无 BBM 的部分 block 修改。 */
static int pageattr_pmd_entry(pmd_t *pmd, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	pmd_t val = pmdp_get(pmd);

	if (pmd_leaf(val)) {
		if (WARN_ON_ONCE((next - addr) != PMD_SIZE))
			return -EINVAL;
		val = __pmd(set_pageattr_masks(pmd_val(val), walk));
		set_pmd(pmd, val);
		walk->action = ACTION_CONTINUE;
	}

	return 0;
}

/*
 * 最底层 PTE 回调，可逐页直接改掩码。pte 指针由内核页表 walker 保证有效，
 * addr/next 在本实现不需读取；__set_pte 只写描述符，批量同步由外层负责。
 */
static int pageattr_pte_entry(pte_t *pte, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	pte_t val = __ptep_get(pte);

	val = __pte(set_pageattr_masks(pte_val(val), walk));
	__set_pte(pte, val);

	return 0;
}

/* 三层 leaf 修改操作表；未提供的层由通用 walker 负责下钻。 */
static const struct mm_walk_ops pageattr_ops = {
	.pud_entry	= pageattr_pud_entry,
	.pmd_entry	= pageattr_pmd_entry,
	.pte_entry	= pageattr_pte_entry,
};

/*
 * true 表示内核镜像只读页在线性映射别名中也保持只读。启动参数可调整，
 * init 后冻结；它同时决定是否必须把 linear map 拆到页粒度。
 */
bool rodata_full __ro_after_init = true;

/*
 * 返回当前系统能否安全逐页改变 direct map。rodata_full、debug-pagealloc、
 * 晚初始化 KFENCE 或 Realm 任一启用，都要求初始化阶段已避免大 block
 * linear mapping。结果无副作用；false 时 direct-map helper 成功空操作，
 * 因为事后拆 live block 会产生 TLB 冲突。
 */
bool can_set_direct_map(void)
{
	/*
	 * rodata_full, DEBUG_PAGEALLOC and a Realm guest all require linear
	 * map to be mapped at page granularity, so that it is possible to
	 * protect/unprotect single pages.
	 *
	 * KFENCE pool requires page-granular mapping if initialized late.
	 *
	 * Realms need to make pages shared/protected at page granularity.
	 */
	/* 这些条件必须与 mmu.c 建立线性映射时的 block/page 决策保持一致。 */
	return rodata_full || debug_pagealloc_enabled() ||
		arm64_kfence_can_set_direct_map() || is_realm_world();
}

/*
 * 底层范围修改器。start/size 是内核虚拟字节范围，set_mask/clear_mask 是
 * 原始 PTE 属性。先请求 split_kernel_leaf_mapping 消除相交 block/contiguous
 * 映射，再在 lazy MMU mode 中无锁遍历内核页表。返回 0 或 split/walk
 * 错误；调用者负责范围映射生命周期和所需 TLB flush。
 */
static int update_range_prot(unsigned long start, unsigned long size,
			     pgprot_t set_mask, pgprot_t clear_mask)
{
	struct page_change_data data;
	int ret;

	data.set_mask = set_mask;
	data.clear_mask = clear_mask;

	ret = split_kernel_leaf_mapping(start, start + size);
	/* split helper 完成安全 BBM；失败后尚未修改目标 leaf，可直接返回。 */
	if (WARN_ON_ONCE(ret))
		return ret;

	lazy_mmu_mode_enable();
	/* 批量修改期间延迟体系结构同步，disable 时统一提交，降低逐页开销。 */

	/*
	 * The caller must ensure that the range we are operating on does not
	 * partially overlap a block mapping, or a cont mapping. Any such case
	 * must be eliminated by splitting the mapping.
	 */
	/* lockless walker 依赖内核静态/vmalloc 映射不被并发拆除的外层约束。 */
	ret = walk_kernel_page_table_range_lockless(start, start + size,
						    &pageattr_ops, NULL, &data);
	lazy_mmu_mode_disable();

	return ret;
}

/*
 * 修改范围并按 TLB 可缓存性决定是否 flush。present-invalid -> valid 且
 * 没改其他位时，无效描述符不可能驻留 TLB，可省 TLBI；其他转换必须刷新
 * [start,start+size)，使所有 CPU 不再使用旧权限。返回 update 的错误码。
 */
static int __change_memory_common(unsigned long start, unsigned long size,
				  pgprot_t set_mask, pgprot_t clear_mask)
{
	int ret;

	ret = update_range_prot(start, size, set_mask, clear_mask);

	/*
	 * If the memory is being switched from present-invalid to valid without
	 * changing any other bits then a TLBI isn't required as a non-valid
	 * entry cannot be cached in the TLB.
	 */
	/* 该优化只匹配精确掩码，避免把同时改权限/共享性的情况误判为免刷新。 */
	if (pgprot_val(set_mask) != PTE_PRESENT_VALID_KERNEL ||
	    pgprot_val(clear_mask) != PTE_PRESENT_INVALID)
		flush_tlb_kernel_range(start, start + size);
	return ret;
}

/*
 * 对公开 set_memory_* API 做范围合法性与别名处理。addr 是 vmalloc 虚拟
 * 字节地址，numpages 为页数；只接受完全位于单个 VM_ALLOC 且未启用 huge
 * vmap 的区域。返回 0 或 -EINVAL/底层错误。rodata_full 下 RO 位还同步到
 * backing page 的线性别名，防止通过 direct map 绕过只读保护。
 */
static int change_memory_common(unsigned long addr, int numpages,
				pgprot_t set_mask, pgprot_t clear_mask)
{
	unsigned long start = addr;
	unsigned long size = PAGE_SIZE * numpages;
	unsigned long end = start + size;
	struct vm_struct *area;
	int ret;

	if (!PAGE_ALIGNED(addr)) {
		/* 告警并向下对齐以保持旧调用兼容，但范围长度仍按请求页数计算。 */
		start &= PAGE_MASK;
		end = start + size;
		WARN_ON_ONCE(1);
	}

	/*
	 * Kernel VA mappings are always live, and splitting live section
	 * mappings into page mappings may cause TLB conflicts. This means
	 * we have to ensure that changing the permission bits of the range
	 * we are operating on does not result in such splitting.
	 *
	 * Let's restrict ourselves to mappings created by vmalloc (or vmap).
	 * Disallow VM_ALLOW_HUGE_VMAP mappings to guarantee that only page
	 * mappings are updated and splitting is never needed.
	 *
	 * So check whether the [addr, addr + size) interval is entirely
	 * covered by precisely one VM area that has the VM_ALLOC flag set.
	 */
	/* kasan_reset_tag 后比较真实 VA，避免 tagged pointer 破坏边界判断。 */
	area = find_vm_area((void *)addr);
	if (!area ||
	    ((unsigned long)kasan_reset_tag((void *)end) >
	     (unsigned long)kasan_reset_tag(area->addr) + area->size) ||
	    ((area->flags & (VM_ALLOC | VM_ALLOW_HUGE_VMAP)) != VM_ALLOC))
		return -EINVAL;

	if (!numpages)
		/* 在访问 area->pages 和 flush alias 前接受空操作。 */
		return 0;

	/*
	 * If we are manipulating read-only permissions, apply the same
	 * change to the linear mapping of the pages that back this VM area.
	 */
	/* idx 把 vmalloc VA 偏移转换为 area->pages[] 下标，逐页修改 direct map。 */
	if (rodata_full && (pgprot_val(set_mask) == PTE_RDONLY ||
			    pgprot_val(clear_mask) == PTE_RDONLY)) {
		unsigned long idx = ((unsigned long)kasan_reset_tag((void *)start) -
				     (unsigned long)kasan_reset_tag(area->addr))
				    >> PAGE_SHIFT;
		for (; numpages; idx++, numpages--) {
			ret = __change_memory_common((u64)page_address(area->pages[idx]),
						     PAGE_SIZE, set_mask, clear_mask);
			if (ret)
				return ret;
		}
	}

	/*
	 * Get rid of potentially aliasing lazily unmapped vm areas that may
	 * have permissions set that deviate from the ones we are setting here.
	 */
	/* 先回收 lazy vmap 别名，防止同一页仍以旧权限从另一个 VA 可访问。 */
	vm_unmap_aliases();

	return __change_memory_common(start, size, set_mask, clear_mask);
}

/* 将 numpages 页设为只读；清 WRITE 后置 RDONLY，返回通用修改结果。 */
int set_memory_ro(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_RDONLY),
					__pgprot(PTE_WRITE));
}

/* 将 vmalloc 范围恢复可写；清 RDONLY 并置 WRITE。 */
int set_memory_rw(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_WRITE),
					__pgprot(PTE_RDONLY));
}

/* 禁止 privileged execute，并清可能与 BTI guard 相关的目标位。 */
int set_memory_nx(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_PXN),
					__pgprot(PTE_MAYBE_GP));
}

/* 恢复可执行及配置允许的 BTI guard 位，同时清 PXN。 */
int set_memory_x(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(PTE_MAYBE_GP),
					__pgprot(PTE_PXN));
}

/*
 * 切换描述符 valid/invalid，addr 可为受控的 direct-map 范围，不经过 vmalloc
 * 验证。enable 非零恢复 PTE_PRESENT_VALID_KERNEL，否则置 PRESENT_INVALID。
 * numpages 单位为页；底层只在 invalid->valid 精确转换时省略 TLBI。
 */
int set_memory_valid(unsigned long addr, int numpages, int enable)
{
	if (enable)
		return __change_memory_common(addr, PAGE_SIZE * numpages,
					__pgprot(PTE_PRESENT_VALID_KERNEL),
					__pgprot(PTE_PRESENT_INVALID));
	else
		return __change_memory_common(addr, PAGE_SIZE * numpages,
					__pgprot(PTE_PRESENT_INVALID),
					__pgprot(PTE_PRESENT_VALID_KERNEL));
}

/*
 * 让单个 page 的 direct-map PTE 失效但不执行最终 TLB flush，供批量调用者
 * 合并 shootdown。无法逐页 direct-map 时成功空操作。page 生命周期不变。
 */
int set_direct_map_invalid_noflush(struct page *page)
{
	pgprot_t clear_mask = __pgprot(PTE_PRESENT_VALID_KERNEL);
	pgprot_t set_mask = __pgprot(PTE_PRESENT_INVALID);

	if (!can_set_direct_map())
		return 0;

	return update_range_prot((unsigned long)page_address(page),
				 PAGE_SIZE, set_mask, clear_mask);
}

/* 恢复单页 direct map 的 valid+write 默认状态；同样由调用者负责批量 TLBI。 */
int set_direct_map_default_noflush(struct page *page)
{
	pgprot_t set_mask = __pgprot(PTE_PRESENT_VALID_KERNEL | PTE_WRITE);
	pgprot_t clear_mask = __pgprot(PTE_PRESENT_INVALID | PTE_RDONLY);

	if (!can_set_direct_map())
		return 0;

	return update_range_prot((unsigned long)page_address(page),
				 PAGE_SIZE, set_mask, clear_mask);
}

/*
 * Realm guest 的共享/私有转换核心。addr 必须是 linear-map 页对齐范围，
 * numpages 为页数，encrypt=true 表示从 NS shared 变 protected/private。
 * 非 Realm 成功空操作；错误返回时映射可能已 invalid，调用者不得继续访问。
 * 三阶段顺序禁止重排：断开映射并 flush -> RSI 改 RIPAS/归属 -> 重建映射。
 */
static int __set_memory_enc_dec(unsigned long addr,
				int numpages,
				bool encrypt)
{
	unsigned long set_prot = 0, clear_prot = 0;
	phys_addr_t start, end;
	int ret;

	if (!is_realm_world())
		return 0;

	if (!__is_lm_address(addr))
		return -EINVAL;

	start = __virt_to_phys(addr);
	end = start + numpages * PAGE_SIZE;

	if (encrypt)
		clear_prot = PROT_NS_SHARED;
	else
		set_prot = PROT_NS_SHARED;

	/*
	 * Break the mapping before we make any changes to avoid stale TLB
	 * entries or Synchronous External Aborts caused by RIPAS_EMPTY
	 */
	/* invalidation 同时按目标方向预置/清除 NS_SHARED，但先不让 CPU 使用。 */
	ret = __change_memory_common(addr, PAGE_SIZE * numpages,
				     __pgprot(set_prot | PTE_PRESENT_INVALID),
				     __pgprot(clear_prot | PTE_PRESENT_VALID_KERNEL));

	if (ret)
		return ret;

	if (encrypt)
		/* RMM 接管 [start,end) 成为 protected，物理地址区间为半开范围。 */
		ret = rsi_set_memory_range_protected(start, end);
	else
		ret = rsi_set_memory_range_shared(start, end);

	if (ret)
		/* 不重建 valid：归属未知时保持不可访问比恢复潜在错误别名安全。 */
		return ret;

	return __change_memory_common(addr, PAGE_SIZE * numpages,
				      __pgprot(PTE_PRESENT_VALID_KERNEL),
				      __pgprot(PTE_PRESENT_INVALID));
}

/* Realm encrypt 包装：失败意味着页面状态不可安全判定，告警并要求上层泄漏。 */
static int realm_set_memory_encrypted(unsigned long addr, int numpages)
{
	int ret = __set_memory_enc_dec(addr, numpages, true);

	/*
	 * If the request to change state fails, then the only sensible cause
	 * of action for the caller is to leak the memory
	 */
	/* 泄漏牺牲容量但避免把可能仍属 Realm 的页重新分配给非安全使用者。 */
	WARN(ret, "Failed to encrypt memory, %d pages will be leaked",
	     numpages);

	return ret;
}

/* Realm decrypt/share 包装，参数/失败泄漏语义与 encrypt 对称。 */
static int realm_set_memory_decrypted(unsigned long addr, int numpages)
{
	int ret = __set_memory_enc_dec(addr, numpages, false);

	WARN(ret, "Failed to decrypt memory, %d pages will be leaked",
	     numpages);

	return ret;
}

/* 静态后端操作表生命周期覆盖运行期，注册层只借用该指针。 */
static const struct arm64_mem_crypt_ops realm_crypt_ops = {
	.encrypt = realm_set_memory_encrypted,
	.decrypt = realm_set_memory_decrypted,
};

/* 在确认 Realm 环境的启动路径注册唯一加密后端；返回 0 或 -EBUSY。 */
int realm_register_memory_enc_ops(void)
{
	return arm64_mem_crypt_ops_register(&realm_crypt_ops);
}

/*
 * 批量 direct-map valid 包装。page 是起始页，nr 为连续页数，valid 选择
 * 状态；名称虽为 noflush，内部 set_memory_valid 会按转换类型决定 TLBI。
 */
int set_direct_map_valid_noflush(struct page *page, unsigned nr, bool valid)
{
	unsigned long addr = (unsigned long)page_address(page);

	if (!can_set_direct_map())
		return 0;

	return set_memory_valid(addr, nr, valid);
}

#ifdef CONFIG_DEBUG_PAGEALLOC
/*
 * This is - apart from the return value - doing the same
 * thing as the new set_direct_map_valid_noflush() function.
 *
 * Unify? Explain the conceptual differences?
 */
/*
 * debug-pagealloc 的通用 MM 钩子，无返回值；enable 决定分配时映射、释放时
 * 失效。它忽略错误以符合调试钩子接口，而新 helper 返回状态供其他调用者
 * 处理，这也是两者尚不能机械合并的可观察差异。
 */
void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	if (!can_set_direct_map())
		return;

	set_memory_valid((unsigned long)page_address(page), numpages, enable);
}
#endif /* CONFIG_DEBUG_PAGEALLOC */

/*
 * This function is used to determine if a linear map page has been marked as
 * not-valid. Walk the page table and check the PTE_VALID bit.
 *
 * Because this is only called on the kernel linear map,  p?d_sect() implies
 * p?d_present(). When debug_pagealloc is enabled, sections mappings are
 * disabled.
 */
/*
 * 查询 page 的 linear-map 描述符是否 valid。逐级 READ_ONCE 防止编译器
 * 撕裂/重复读取并发页表项；遇到 block leaf 直接查其 valid，页粒度下钻
 * 到 PTE。只读诊断函数不获取页表锁，依赖 direct map 层级在启用逐页
 * 调试时不并发重构；返回 false 表示任一级不存在或最终 leaf invalid。
 */
bool kernel_page_present(struct page *page)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep;
	unsigned long addr = (unsigned long)page_address(page);

	pgdp = pgd_offset_k(addr);
	if (pgd_none(READ_ONCE(*pgdp)))
		return false;

	p4dp = p4d_offset(pgdp, addr);
	if (p4d_none(READ_ONCE(*p4dp)))
		return false;

	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud))
		return false;
	if (pud_leaf(pud))
		return pud_valid(pud);

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (pmd_none(pmd))
		return false;
	if (pmd_leaf(pmd))
		return pmd_valid(pmd);

	ptep = pte_offset_kernel(pmdp, addr);
	return pte_valid(__ptep_get(ptep));
}
