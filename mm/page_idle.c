// SPDX-License-Identifier: GPL-2.0
/*
 * 本文件实现 PFN 位图形式的 idle page tracking：用户写 1 建立观察起点，等待工作负载
 * 运行后再读出仍未访问的 LRU folio。页表 young、设备 MMU notifier、folio idle/young
 * 和 reclaim 补偿共同构成协议；sysfs 扫描只尽力观测，不固定整个地址空间快照。
 */
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
/* 下列头文件提供热插拔 PFN 校验、folio/LRU、rmap、notifier 与 idle flag 原语。 */
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/page_ext.h>
#include <linux/page_idle.h>

#include "internal.h"

/* sysfs bitmap 以本机字节序 u64 为传输单元：每 8 字节精确表示连续 64 个 PFN。 */
#define BITMAP_CHUNK_SIZE	sizeof(u64)
#define BITMAP_CHUNK_BITS	(BITMAP_CHUNK_SIZE * BITS_PER_BYTE)

/*
 * Idle page tracking only considers user memory pages, for other types of
 * pages the idle flag is always unset and an attempt to set it is silently
 * ignored.
 *
 * We treat a page as a user memory page if it is on an LRU list, because it is
 * always safe to pass such a page to rmap_walk(), which is essential for idle
 * page tracking. With such an indicator of user pages we can skip isolated
 * pages, but since there are not usually many of them, it will hardly affect
 * the overall result.
 *
 * This function tries to get a user memory page by pfn as described above.
 */
/*
 * Idle tracking 只考虑用户内存页；其他页的 idle 始终视为未设置，写入会静默忽略。
 * 这里用“folio 在 LRU 上”作为用户页的可操作判据，因为这种页可以安全交给 rmap_walk()。
 * 代价是暂时被 isolate 的用户页也会跳过，但通常数量很少，对整体工作集估计影响有限。
 *
 * 业务背景：bitmap read/write 在把 PFN 转成对象后，需要跨越热移除、compound 拆分和
 * LRU 隔离竞态；本 helper 取得稳定 folio 引用并二次验证。
 * 入参：pfn 是要查询的物理页帧号，没有任何预先持有的 page 引用。
 * 出参/返回：成功返回持有引用的 LRU folio，调用者必须 folio_put()；离线页、tail 页、
 * 非 LRU 页或竞态失败返回 NULL，无 ownership 遗留。
 * 注意事项：folio_try_get() 不睡眠；引用只保证生命周期，LRU 状态仍需取得后复核。
 */
static struct folio *page_idle_get_folio(unsigned long pfn)
{
	struct page *page = pfn_to_online_page(pfn);
	struct folio *folio;

	/* pfn_to_online_page 同时过滤无 memmap/离线 PFN；huge folio 只由 head PFN 表示。 */
	if (!page || PageTail(page))
		return NULL;

	/* 先确认 LRU 候选并尝试取得引用，避免对象在后续 rmap 操作前释放。 */
	folio = page_folio(page);
	if (!folio_test_lru(folio) || !folio_try_get(folio))
		return NULL;
	/* 取得引用窗口内可能发生 compound 拆分或 LRU 隔离，二次检查失败便归还引用。 */
	if (unlikely(page_folio(page) != folio || !folio_test_lru(folio))) {
		folio_put(folio);
		folio = NULL;
	}
	return folio;
}

/*
 * page_idle_clear_pte_refs_one() - 清除一个 folio 在某 VMA 中所有映射的 accessed/young。
 *
 * 业务背景：rmap_walk() 对每个反向映射调用本函数；只有把 CPU PTE/PMD 与设备 MMU
 * notifier 的 young 状态都清掉，后续访问才能被 idle tracking 重新观察。
 * 入参：folio 是已持有引用且由外层锁定的对象；vma/addr 是 rmap 提供的借用映射起点；
 * arg 本回调不使用，ownership 均不变化。
 * 出参/返回：始终返回 true 让 rmap 继续遍历；若发现任一引用，清 folio idle 并设置
 * folio young 作为 reclaim 补偿。无 errno。
 * 注意事项：运行在 rmap/页表锁协议内，不可睡眠；MMU notifier 与 CPU 页表状态必须同清。
 */
static bool page_idle_clear_pte_refs_one(struct folio *folio,
					struct vm_area_struct *vma,
					unsigned long addr, void *arg)
{
	DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, addr, 0);
	bool referenced = false;

	/* pvmw 在同一 VMA 内逐个发现 PTE 或 PMD 映射，并代管相应页表锁。 */
	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte) {
			/*
			 * For PTE-mapped THP, one sub page is referenced,
			 * the whole THP is referenced.
			 *
			 * PFN swap PTEs, such as device-exclusive ones, that
			 * actually map pages are "old" from a CPU perspective.
			 * The MMU notifier takes care of any device aspects.
			 */
			/*
			 * PTE 映射 THP 时任一 subpage 被引用都视为整个 THP 被引用。device-exclusive
			 * 等 PFN swap PTE 从 CPU 角度本来就是 old；设备侧访问状态由 MMU notifier
			 * 负责清理。present PTE 才执行 CPU test-and-clear，notifier 则始终通知。
			 */
			if (likely(pte_present(ptep_get(pvmw.pte))))
				referenced |= ptep_test_and_clear_young(vma, addr, pvmw.pte);
			referenced |= mmu_notifier_clear_young(vma->vm_mm, addr, addr + PAGE_SIZE);
		} else if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)) {
			/* PMD 映射的 THP 以 PMD_SIZE 通知设备，并聚合整个大页的引用状态。 */
			pmd_t pmdval = pmdp_get(pvmw.pmd);

			if (likely(pmd_present(pmdval)))
				referenced |= pmdp_test_and_clear_young(vma, addr, pvmw.pmd);
			referenced |= mmu_notifier_clear_young(vma->vm_mm, addr, addr + PMD_SIZE);
		} else {
			/* unexpected pmd-mapped page? */
			/* THP 配置关闭却遇到 PMD 映射违反预期，只告警一次并继续 rmap。 */
			WARN_ON_ONCE(1);
		}
	}

	/* 任何映射曾 young 都说明观察窗口内页非 idle。 */
	if (referenced) {
		folio_clear_idle(folio);
		/*
		 * We cleared the referenced bit in a mapping to this page. To
		 * avoid interference with page reclaim, mark it young so that
		 * folio_referenced() will return > 0.
		 */
		/*
		 * 我们为 idle 检测清除了该页某个映射的 referenced 位；为避免干扰 reclaim，
		 * 同时设置 folio young，使 folio_referenced() 仍返回大于 0，不把活跃页误回收。
		 */
		folio_set_young(folio);
	}
	return true;
}

/*
 * page_idle_clear_pte_refs() - 尽力遍历并清除 folio 的所有映射引用位。
 * 业务背景：bitmap 写入在建立新观察窗口前调用，读取也在报告 idle 前复核潜在 PTE 访问。
 * 入参：folio 是调用者持有引用的 LRU folio，本函数不接管引用。
 * 出参/返回：无直接返回值；成功时 rmap 回调可能更新页表、MMU notifier、idle/young；
 * 未映射、无 mapping 或锁竞争时无副作用地跳过。
 * 注意事项：使用 trylock 避免 sysfs 长扫描阻塞；持 folio 锁期间 rmap_walk，结束必解锁。
 */
static void page_idle_clear_pte_refs(struct folio *folio)
{
	/*
	 * Since rwc.try_lock is unused, rwc is effectively immutable, so we
	 * can make it static to save some cycles and stack.
	 */
	/*
	 * rwc.try_lock 未使用，所以控制结构初始化后事实上不可变；设为 static 可减少每次
	 * bitmap 扫描的栈空间和初始化开销。rmap_one 清引用，anon_lock 稳定匿名映射链。
	 */
	static struct rmap_walk_control rwc = {
		.rmap_one = page_idle_clear_pte_refs_one,
		.anon_lock = folio_lock_anon_vma_read,
	};

	/* 无页表映射或无合法 mapping 时不存在需要 rmap 清理的 PTE/PMD。 */
	if (!folio_mapped(folio) || !folio_raw_mapping(folio))
		return;

	/* 锁竞争时选择跳过，避免一次大 bitmap I/O 长时间等待热点页。 */
	if (!folio_trylock(folio))
		return;

	rmap_walk(folio, &rwc);
	folio_unlock(folio);
}

/*
 * page_idle_bitmap_read() - 读取 /sys/kernel/mm/page_idle/bitmap 的 PFN idle 位图。
 *
 * 业务背景：用户先写 1 标记工作集页为 idle，等待观测窗口后读取；读取会清理/复核页表
 * young 位，仅仍保持 idle 的 LRU folio 对应 bit 为 1。
 * 入参：file/kobj/attr 是 sysfs 借用上下文且本实现不使用；buf 是 count 字节输出缓冲区；
 * pos/count 以字节计，必须 8 字节对齐，pos*8 是首 PFN。
 * 出参/返回：成功返回实际填充字节数；起点超 max_pfn 返回 0（EOF）；未对齐返回 -EINVAL。
 * 注意事项：按本机字节序 u64 输出；每个 folio 引用均在循环内释放，cond_resched 可睡眠；
 * huge folio 只报告 head PFN，非 LRU/离线/锁竞争页不会获得新的稳定性保证。
 */
static ssize_t page_idle_bitmap_read(struct file *file, struct kobject *kobj,
				     const struct bin_attribute *attr, char *buf,
				     loff_t pos, size_t count)
{
	u64 *out = (u64 *)buf;
	struct folio *folio;
	unsigned long pfn, end_pfn;
	int bit;

	/* ABI 只接受完整 u64，防止一个 64-PFN chunk 被部分覆盖或解释。 */
	if (pos % BITMAP_CHUNK_SIZE || count % BITMAP_CHUNK_SIZE)
		return -EINVAL;

	/* 位图文件的每个字节表示 8 PFN，因此字节 offset 乘 8 得首 PFN。 */
	pfn = pos * BITS_PER_BYTE;
	if (pfn >= max_pfn)
		return 0;

	/* 将请求裁剪到在线地址空间的全局 PFN 上界。 */
	end_pfn = pfn + count * BITS_PER_BYTE;
	if (end_pfn > max_pfn)
		end_pfn = max_pfn;

	/* 每逢 64-PFN 边界清零输出 word，之后仅 OR 入经复核仍 idle 的位。 */
	for (; pfn < end_pfn; pfn++) {
		bit = pfn % BITMAP_CHUNK_BITS;
		if (!bit)
			*out = 0ULL;
		folio = page_idle_get_folio(pfn);
		if (folio) {
			if (folio_test_idle(folio)) {
				/*
				 * The page might have been referenced via a
				 * pte, in which case it is not idle. Clear
				 * refs and recheck.
				 */
				/*
				 * folio 虽带 idle 标志，仍可能经某个 PTE 被访问；先清除 refs，回调会在
				 * 发现 young 时清 idle，因此必须二次 test 后才能向用户报告 1。
				 */
				page_idle_clear_pte_refs(folio);
				if (folio_test_idle(folio))
					*out |= 1ULL << bit;
			}
			/* 当前 PFN 判断结束，归还 page_idle_get_folio() 取得的引用。 */
			folio_put(folio);
		}
		if (bit == BITMAP_CHUNK_BITS - 1)
			out++;
		/* 大范围读取主动让出 CPU；当前位置没有持 folio/页表锁。 */
		cond_resched();
	}
	return (char *)out - buf;
}

/*
 * page_idle_bitmap_write() - 按输入位把对应用户 folio 标记为新的 idle 观察起点。
 *
 * 业务背景：写 1 先尽力清除现有 PTE/PMD young，再设置 folio idle；写 0 不清除已有
 * idle，因而写入语义是与当前 bitmap 做 OR，而不是整块替换。
 * 入参：file/kobj/attr 为未使用的 sysfs 借用上下文；buf 是 count 字节本机序 u64 输入；
 * pos/count 以字节计且须 8 字节对齐。
 * 出参/返回：成功返回实际消费字节数；未对齐返回 -EINVAL；起点越过 max_pfn 返回 -ENXIO。
 * 注意事项：只处理置 1 位，非用户/离线/tail PFN 静默忽略；循环可调度，folio 引用闭环。
 */
static ssize_t page_idle_bitmap_write(struct file *file, struct kobject *kobj,
				      const struct bin_attribute *attr, char *buf,
				      loff_t pos, size_t count)
{
	const u64 *in = (u64 *)buf;
	struct folio *folio;
	unsigned long pfn, end_pfn;
	int bit;

	/* 与 read 使用同一 u64 对齐 ABI；越界起点在写路径是错误而非 EOF。 */
	if (pos % BITMAP_CHUNK_SIZE || count % BITMAP_CHUNK_SIZE)
		return -EINVAL;

	pfn = pos * BITS_PER_BYTE;
	if (pfn >= max_pfn)
		return -ENXIO;

	/* 末尾跨过 max_pfn 时只消费可表示的 PFN 前缀。 */
	end_pfn = pfn + count * BITS_PER_BYTE;
	if (end_pfn > max_pfn)
		end_pfn = max_pfn;

	/* 每个输入 word 的 bit i 对应该 64-PFN chunk 中的第 i 页。 */
	for (; pfn < end_pfn; pfn++) {
		bit = pfn % BITMAP_CHUNK_BITS;
		if ((*in >> bit) & 1) {
			/* 只为可稳定取得的 LRU folio 建立观察窗口。 */
			folio = page_idle_get_folio(pfn);
			if (folio) {
				/* 先清旧访问证据再置 idle；后续访问应重新产生 young。 */
				page_idle_clear_pte_refs(folio);
				folio_set_idle(folio);
				folio_put(folio);
			}
		}
		if (bit == BITMAP_CHUNK_BITS - 1)
			in++;
		/* 当前迭代无引用/锁后允许调度，避免长写独占 CPU。 */
		cond_resched();
	}
	return (char *)in - buf;
}

/* bitmap 二进制属性权限 0600，仅特权读写；size=0 表示由回调按 max_pfn 动态裁剪。 */
static const struct bin_attribute page_idle_bitmap_attr =
		__BIN_ATTR(bitmap, 0600,
			   page_idle_bitmap_read, page_idle_bitmap_write, 0);

/* attribute 数组以 NULL 终止，元素均为静态只读对象，不涉及引用计数。 */
static const struct bin_attribute *const page_idle_bin_attrs[] = {
	&page_idle_bitmap_attr,
	NULL,
};

/* 组名 page_idle 形成 /sys/kernel/mm/page_idle/，其下当前只有 bitmap。 */
static const struct attribute_group page_idle_attr_group = {
	.bin_attrs = page_idle_bin_attrs,
	.name = "page_idle",
};

/*
 * page_idle_init() - 在 mm_kobj 下注册 page_idle sysfs 属性组。
 * 业务背景：subsys_initcall 在内存子系统与 sysfs 核心就绪后发布用户态 idle tracking ABI。
 * 入参：无。
 * 出参/返回：成功返回 0；sysfs_create_group() 失败记录错误并原样返回负 errno，无残留组。
 * 注意事项：仅启动期调用一次，可睡眠；成功后 sysfs 核心借用静态 attribute/group 对象。
 */
static int __init page_idle_init(void)
{
	int err;

	/* sysfs 注册是对用户可见的发布点，失败不伪装成功。 */
	err = sysfs_create_group(mm_kobj, &page_idle_attr_group);
	if (err) {
		pr_err("page_idle: register sysfs failed\n");
		return err;
	}
	return 0;
}

/* 在内存子系统初始化阶段发布接口，早于普通用户态开始使用。 */
subsys_initcall(page_idle_init);
