// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2002 Richard Henderson
 * Copyright (C) 2001 Rusty Russell, 2002, 2010 Rusty Russell IBM.
 * Copyright (C) 2023 Luis Chamberlain <mcgrof@kernel.org>
 * Copyright (C) 2024 Mike Rapoport IBM.
 */

/* 统一日志前缀，便于把启动校验或分配失败归属到 execmem。 */
#define pr_fmt(fmt) "execmem: " fmt

#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/execmem.h>
#include <linux/maple_tree.h>
#include <linux/set_memory.h>
#include <linux/moduleloader.h>
#include <linux/text-patching.h>

/* 下面依赖 vmalloc、Maple Tree、页权限和 text patch 的公共协议。 */
#include <asm/tlbflush.h>

#include "internal.h"

/*
 * 架构在启动期交出的各类可执行内存窗口；初始化完成后改为只读，随后
 * 分配路径只能借用它，不能在并发加载模块时重新定义地址和权限策略。
 */
static struct execmem_info *execmem_info __ro_after_init;
/* 未提供架构钩子时使用的兜底配置，其范围在 __execmem_init() 中建立。 */
static struct execmem_info default_execmem_info __ro_after_init;

#ifdef CONFIG_MMU
/*
 * execmem_vmalloc() - 在架构允许的代码窗口创建一段虚拟连续内存
 *
 * 业务背景：模块、ftrace 和 BPF 不能随意使用 vmalloc 地址；本函数是
 * execmem_alloc() 的非缓存后端，先满足架构距离/对齐/页权限限制。
 * 入参：range 是启动后只读的借用策略；size 是已页对齐字节数；pgprot 和
 * vm_flags 是本次映射权限/VM 行为。出参/返回：成功返回新映射的唯一持有者，
 * 失败返回 NULL。注意事项：可睡眠；KASAN shadow 失败时必须撤销映射。
 */
static void *execmem_vmalloc(struct execmem_range *range, size_t size,
			     pgprot_t pgprot, unsigned long vm_flags)
{
	bool kasan = range->flags & EXECMEM_KASAN_SHADOW;
	gfp_t gfp_flags = GFP_KERNEL | __GFP_NOWARN;
	unsigned int align = range->alignment;
	unsigned long start = range->start;
	unsigned long end = range->end;
	void *p;

	/* KASAN 自己追踪这段 shadow，避免 kmemleak 将其误报为泄漏。 */
	if (kasan)
		vm_flags |= VM_DEFER_KMEMLEAK;

	/* 首选窗口通常满足模块可达性或架构 text 位置约束。 */
	p = __vmalloc_node_range(size, align, start, end, gfp_flags,
				 pgprot, vm_flags, NUMA_NO_NODE,
				 __builtin_return_address(0));
	/* 首选耗尽才退到架构声明的次级窗口，不能反过来破坏近距离假设。 */
	if (!p && range->fallback_start) {
		start = range->fallback_start;
		end = range->fallback_end;
		/* 后备范围同样保留 alignment、GFP 和调用点，以便诊断与首选路径一致。 */
		p = __vmalloc_node_range(size, align, start, end, gfp_flags,
					 pgprot, vm_flags, NUMA_NO_NODE,
					 __builtin_return_address(0));
	}

	if (!p) {
		pr_warn_ratelimited("unable to allocate memory\n");
		return NULL;
	}

	/* 映射成功不等于 KASAN 元数据成功；失败仍由本层拥有 p 并负责 vfree。 */
	if (kasan && (kasan_alloc_module_shadow(p, size, GFP_KERNEL) < 0)) {
		vfree(p);
		return NULL;
	}

	return p;
}

/*
 * execmem_vmap() - 仅预留模块数据所需的虚拟区间描述符
 *
 * 业务背景：模块装载器随后自行填页；此处只让数据段落入与 text 协调的
 * EXECMEM_MODULE_DATA 窗口。入参：size 为字节数。出参/返回：成功返回
 * vm_struct 持有给调用者，失败为 NULL。注意事项：CONFIG_MMU 才有映射空间。
 */
struct vm_struct *execmem_vmap(size_t size)
{
	struct execmem_range *range = &execmem_info->ranges[EXECMEM_MODULE_DATA];
	struct vm_struct *area;

	area = __get_vm_area_node(size, range->alignment, PAGE_SHIFT, VM_ALLOC,
				  range->start, range->end, NUMA_NO_NODE,
				  GFP_KERNEL, __builtin_return_address(0));
	/* 数据窗口也遵循同一后备策略；这里只预留 VA，尚未建立实际页映射。 */
	if (!area && range->fallback_start)
		area = __get_vm_area_node(size, range->alignment, PAGE_SHIFT, VM_ALLOC,
					  range->fallback_start, range->fallback_end,
					  NUMA_NO_NODE, GFP_KERNEL, __builtin_return_address(0));

	return area;
}
#else
/* NOMMU 没有可选择的虚拟窗口，退化为平台 vmalloc 实现。 */
static void *execmem_vmalloc(struct execmem_range *range, size_t size,
			     pgprot_t pgprot, unsigned long vm_flags)
{
	return vmalloc(size);
}
#endif /* CONFIG_MMU */

#ifdef CONFIG_ARCH_HAS_EXECMEM_ROX
/*
 * ROX cache 同时维护“借出”和“可复用”两棵区间树；mutex 保护树、payload
 * 的 pending 位以及 pending_free_cnt，tree 本身显式复用这把外部锁。
 */
struct execmem_cache {
	struct mutex mutex;
	struct maple_tree busy_areas;
	struct maple_tree free_areas;
	unsigned int pending_free_cnt;	/* protected by mutex */
/* pending_free_cnt 记录已移交 worker、仍留在 busy tree 的项数。 */
};

/* delay to schedule asynchronous free if fast path free fails */
/* 快路径的非重试元数据分配失败后，等待短暂延迟再由可睡眠 worker 重试。 */
#define FREE_DELAY	(msecs_to_jiffies(10))

/* mark entries in busy_areas that should be freed asynchronously */
/* 页对齐地址的空闲低位编码 pending 状态，避免另分配一份延迟释放对象。 */
#define PENDING_FREE_MASK	(1 << (PAGE_SHIFT - 1))

static struct execmem_cache execmem_cache = {
	.mutex = __MUTEX_INITIALIZER(execmem_cache.mutex),
	.busy_areas = MTREE_INIT_EXT(busy_areas, MT_FLAGS_LOCK_EXTERN,
				     execmem_cache.mutex),
	.free_areas = MTREE_INIT_EXT(free_areas, MT_FLAGS_LOCK_EXTERN,
				     execmem_cache.mutex),
};

/* 将 Maple Tree 当前闭区间转换为字节长度；调用者持有对应树的外部 mutex。 */
static inline unsigned long mas_range_len(struct ma_state *mas)
{
	return mas->last - mas->index + 1;
}

/*
 * execmem_set_direct_map_valid() - 成批开关缓存大页在 direct map 中的可访问性
 *
 * 业务背景：ROX 缓存复用 text 页时，direct map 不能成为绕开 W^X 的旁路。
 * 入参：vm 是仍由缓存拥有的 vmalloc 区，valid 决定允许或撤销 direct map。
 * 出参/返回：0 表示全部完成，错误时已恢复此前页。注意事项：按 vm 的大页
 * order 操作；部分失败必须反向恢复，否则同一映射会处在混合安全状态。
 */
static int execmem_set_direct_map_valid(struct vm_struct *vm, bool valid)
{
	unsigned int nr = (1 << get_vm_area_page_order(vm));
	unsigned int updated = 0;
	int err = 0;

	/* 每次覆盖一个可用的大页粒度，记录已提交数量供失败回滚。 */
	for (int i = 0; i < vm->nr_pages; i += nr) {
		err = set_direct_map_valid_noflush(vm->pages[i], nr, valid);
		if (err)
			goto err_restore;
		updated += nr;
	}

	return 0;

	/* 当前页失败时，已改页必须恢复为调用前的 valid 值。 */
err_restore:
	for (int i = 0; i < updated; i += nr)
		set_direct_map_valid_noflush(vm->pages[i], nr, !valid);

	return err;
}

/* 将可执行映射暂时降为不可执行再赋予写权限，供生成/擦除指令使用。 */
static int execmem_force_rw(void *ptr, size_t size)
{
	unsigned int nr = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long addr = (unsigned long)ptr;
	int ret;

	ret = set_memory_nx(addr, nr);
	if (ret)
		return ret;

	return set_memory_rw(addr, nr);
}

/* 恢复 R+X；调用者在完成所有指令写入及 cache 同步后才可重新发布。 */
int execmem_restore_rox(void *ptr, size_t size)
{
	unsigned int nr = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long addr = (unsigned long)ptr;

	return set_memory_rox(addr, nr);
}

/*
 * execmem_cache_clean() - 归还已经重新合并为完整 PMD 的空闲缓存块
 * 业务背景：缓存保留零碎 text 区以便快速复用，只有完整大页才值得真正释放。
 * 入参：work 是全局清理 work；出参/返回：无。注意事项：mutex 稳定两棵树，
 * vfree 前恢复 direct map；workqueue 上下文可睡眠。
 */
static void execmem_cache_clean(struct work_struct *work)
{
	struct maple_tree *free_areas = &execmem_cache.free_areas;
	struct mutex *mutex = &execmem_cache.mutex;
	MA_STATE(mas, free_areas, 0, ULONG_MAX);
	void *area;

	/* tree 的区间与 payload 同时变化，遍历和摘除必须在同一把 mutex 下。 */
	mutex_lock(mutex);
	/* 每个 free 区仅当起点和长度都 PMD 对齐时才能安全恢复为整体大页映射。 */
	mas_for_each(&mas, area, ULONG_MAX) {
		size_t size = mas_range_len(&mas);

		/* 仅完整、对齐的大页块离开缓存；零碎块保留以服务后续小请求。 */
		if (IS_ALIGNED(size, PMD_SIZE) &&
		    IS_ALIGNED(mas.index, PMD_SIZE)) {
			struct vm_struct *vm = find_vm_area(area);

			/* 先恢复合法 direct map，再摘 tree 项并 vfree，避免释放后再解引用 vm。 */
			execmem_set_direct_map_valid(vm, true);
			mas_store_gfp(&mas, NULL, GFP_KERNEL);
			vfree(area);
		}
	}
	mutex_unlock(mutex);
}

static DECLARE_WORK(execmem_cache_clean_work, execmem_cache_clean);

/*
 * execmem_cache_add_locked() - 把归还区间与相邻空闲区合并后登记
 * 业务背景：合并避免缓存碎片导致大块分配退化。入参：ptr/size 是调用者已
 * 独占的字节区间，gfp_mask 控制 Maple 节点分配；返回 tree 更新 errno。
 * 注意事项：调用者必须持有 execmem_cache.mutex，登记成功后 free tree 持有区间。
 */
static int execmem_cache_add_locked(void *ptr, size_t size, gfp_t gfp_mask)
{
	struct maple_tree *free_areas = &execmem_cache.free_areas;
	unsigned long addr = (unsigned long)ptr;
	MA_STATE(mas, free_areas, addr - 1, addr + 1);
	unsigned long lower, upper;
	void *area = NULL;

	/* 初值是刚归还区间；两侧命中后把边界向外扩展。 */
	lower = addr;
	upper = addr + size - 1;

	/* 向左、向右各探测一次；只有严格相邻才能保持区间无重叠不变量。 */
	/* 左侧区间若相邻则把新合并区的下界延展到其起点。 */
	area = mas_walk(&mas);
	if (area && mas.last == addr - 1)
		lower = mas.index;

	area = mas_next(&mas, ULONG_MAX);
	if (area && mas.index == addr + size)
		upper = mas.last;

	mas_set_range(&mas, lower, upper);
	return mas_store_gfp(&mas, (void *)lower, gfp_mask);
}

/* 检查候选空闲区起点及请求长度仍在本类型允许的首选或后备窗口中。 */
static bool within_range(struct execmem_range *range, struct ma_state *mas,
			 size_t size)
{
	unsigned long addr = mas->index;

	if (addr >= range->start && addr + size < range->end)
		return true;

	/* 首选不命中才考虑后备，且上界使用同一请求 size 排除跨界切块。 */
	if (range->fallback_start &&
	    addr >= range->fallback_start && addr + size < range->fallback_end)
		return true;

	return false;
}

/*
 * execmem_cache_alloc_locked() - 从 free tree 切出一段并发布到 busy tree
 * 业务背景：busy tree 是已借给调用者的 ownership 账本，free tree 只含可复用块。
 * 入参：range 限制地址窗口，size 为页对齐请求；返回借出的地址或 NULL。
 * 注意事项：mutex 已持有；任一 tree 写入失败时不能丢失原区间。
 */
static void *execmem_cache_alloc_locked(struct execmem_range *range, size_t size)
{
	struct maple_tree *free_areas = &execmem_cache.free_areas;
	struct maple_tree *busy_areas = &execmem_cache.busy_areas;
	MA_STATE(mas_free, free_areas, 0, ULONG_MAX);
	MA_STATE(mas_busy, busy_areas, 0, ULONG_MAX);
	unsigned long addr, last, area_size = 0;
	void *area, *ptr = NULL;
	int err;

	/* 首次适配：跳过过小或落在错误架构窗口的空闲区。 */
	mas_for_each(&mas_free, area, ULONG_MAX) {
		area_size = mas_range_len(&mas_free);

		if (area_size >= size && within_range(range, &mas_free, size))
			break;
	}

	/* 遍历结束仍不足，保持所有 tree 不变并让上层选择扩容。 */
	if (area_size < size)
		return NULL;

	addr = mas_free.index;
	last = mas_free.last;

	/* insert allocated size to busy_areas at range [addr, addr + size) */
	/* 将 [addr, addr + size) 转为 busy；此后 free 路径才能按地址找到它。 */
	mas_set_range(&mas_busy, addr, addr + size - 1);
	err = mas_store_gfp(&mas_busy, (void *)addr, GFP_KERNEL);
	if (err)
		return NULL;

	/* 先摘掉完整旧 free 区，再把未借部分以新 payload 放回。 */
	mas_store_gfp(&mas_free, NULL, GFP_KERNEL);
	/* 有剩余时重建尾部 free 区；失败则撤销 busy 插入，不返回幽灵分配。 */
	if (area_size > size) {
		void *ptr = (void *)(addr + size);

		/*
		 * re-insert remaining free size to free_areas at range
		 * [addr + size, last]
		 */
		/* 原英文说明的尾部区继承原 last，payload 指向该新空闲区起点。 */
		mas_set_range(&mas_free, addr + size, last);
		err = mas_store_gfp(&mas_free, ptr, GFP_KERNEL);
		if (err) {
			mas_store_gfp(&mas_busy, NULL, GFP_KERNEL);
			return NULL;
		}
	}
	ptr = (void *)addr;

	return ptr;
}

/* 快速路径包装：cleanup guard 在返回时释放 mutex，返回地址的 ownership 不受影响。 */
static void *__execmem_cache_alloc(struct execmem_range *range, size_t size)
{
	guard(mutex)(&execmem_cache.mutex);

	return execmem_cache_alloc_locked(range, size);
}

/*
 * execmem_cache_populate_alloc() - 分配新的 ROX 大块、加固后填充缓存
 * 业务背景：free tree 无可用区时才扩容，优先 PMD 大小以保留大页映射优势。
 * 入参：range 为借用策略，size 为本次请求；返回已从新块切出的 busy 地址。
 * 注意事项：权限转换、tree 发布和失败回滚的次序不能交换，过程可睡眠。
 */
static void *execmem_cache_populate_alloc(struct execmem_range *range, size_t size)
{
	unsigned long vm_flags = VM_ALLOW_HUGE_VMAP;
	struct mutex *mutex = &execmem_cache.mutex;
	struct vm_struct *vm;
	size_t alloc_size;
	int err = -ENOMEM;
	void *p;

	/* 大页失败允许退化为精确大小，仍沿同一 execmem 地址/影子策略分配。 */
	alloc_size = round_up(size, PMD_SIZE);
	p = execmem_vmalloc(range, alloc_size, PAGE_KERNEL, vm_flags);
	if (!p) {
		alloc_size = size;
		p = execmem_vmalloc(range, alloc_size, PAGE_KERNEL, vm_flags);
	}

	/* 两种粒度均失败才向调用者报告资源耗尽。 */
	if (!p)
		return NULL;

	/* 找不到 vm_struct 属于内部不变量破坏，不能继续执行权限转换。 */
	vm = find_vm_area(p);
	if (!vm)
		goto err_free_mem;

	/* fill memory with instructions that will trap */
/* 先填陷阱指令：未初始化或已回收 text 即使被错误执行也会立即暴露。 */
	execmem_fill_trapping_insns(p, alloc_size);

	err = set_memory_rox((unsigned long)p, vm->nr_pages);
	if (err)
		goto err_free_mem;

	/*
	 * New memory blocks must be allocated and added to the cache
	 * as an atomic operation, otherwise they may be consumed
	 * by a parallel call to the execmem_cache_alloc function.
	 */
	/* 映射到缓存和从缓存借出必须原子化，避免并发分配看见半初始化新块。 */
	mutex_lock(mutex);
	err = execmem_cache_add_locked(p, alloc_size, GFP_KERNEL);
	if (err)
		goto err_reset_direct_map;

	p = execmem_cache_alloc_locked(range, size);

	mutex_unlock(mutex);

	return p;

	/* 此时新块尚未发布给调用者；撤销安全状态后释放全部映射。 */
err_reset_direct_map:
	mutex_unlock(mutex);
	execmem_set_direct_map_valid(vm, true);
err_free_mem:
	vfree(p);
	return NULL;
}

/* 先尝试无新映射的缓存快速路径，未命中才以 populate 扩容。 */
static void *execmem_cache_alloc(struct execmem_range *range, size_t size)
{
	void *p;

	p = __execmem_cache_alloc(range, size);
	if (p)
		return p;

	return execmem_cache_populate_alloc(range, size);
}

/* busy tree 的 payload 低位复用为延迟释放标记；页对齐地址保证该位可用。 */
static inline bool is_pending_free(void *ptr)
{
	return ((unsigned long)ptr & PENDING_FREE_MASK);
}

/* 标记而不改变地址本体，慢速 worker 会在持锁状态下清除此标记。 */
static inline void *pending_free_set(void *ptr)
{
	return (void *)((unsigned long)ptr | PENDING_FREE_MASK);
}

/* 恢复可传给权限和 vfree helper 的真实页对齐地址。 */
static inline void *pending_free_clear(void *ptr)
{
	return (void *)((unsigned long)ptr & ~PENDING_FREE_MASK);
}

/*
 * __execmem_cache_free() - 擦除已借 text 并由 busy tree 转交 free tree
 * 业务背景：复用前必须消除旧指令，且不能让 direct map 绕开 ROX。入参：mas
 * 精确指向 busy 槽，ptr 是其地址，gfp_mask 规定 tree 元数据分配策略。
 * 出参/返回：0 完成所有权转移，错误时 busy 项仍保留。注意事项：调用者持锁。
 */
static int __execmem_cache_free(struct ma_state *mas, void *ptr, gfp_t gfp_mask)
{
	size_t size = mas_range_len(mas);
	int err;

	/* 先 NX+RW，写陷阱后再 ROX，防止写窗口同时保持可执行。 */
	err = execmem_force_rw(ptr, size);
	if (err)
		return err;

	execmem_fill_trapping_insns(ptr, size);
	execmem_restore_rox(ptr, size);

	/* free tree 接管成功后，才允许删除 busy 账本项。 */
	err = execmem_cache_add_locked(ptr, size, gfp_mask);
	if (err)
		return err;

	mas_store_gfp(mas, NULL, gfp_mask);
	return 0;
}

static void execmem_cache_free_slow(struct work_struct *work);
static DECLARE_DELAYED_WORK(execmem_cache_free_work, execmem_cache_free_slow);

/*
 * execmem_cache_free_slow() - 在可重试上下文完成先前失败的缓存释放
 * 业务背景：原 free 允许 Noretry，不能因内存压力在调用者路径阻塞。
 * 入参：全局 delayed work；出参/返回：无。注意事项：mutex 保护 pending 计数
 * 与 busy payload 标记；仍失败就重新调度，清空后才开始合并大块清理。
 */
static void execmem_cache_free_slow(struct work_struct *work)
{
	struct maple_tree *busy_areas = &execmem_cache.busy_areas;
	MA_STATE(mas, busy_areas, 0, ULONG_MAX);
	void *area;

	guard(mutex)(&execmem_cache.mutex);

	/* 可能是旧 work 已排队但最后一项已被别的路径处理，直接结束。 */
	if (!execmem_cache.pending_free_cnt)
		return;

	/* 只消费带标记的 busy 项；普通 busy 区仍由其真实使用者拥有。 */
	/* 成功释放一个项才递减计数，失败项保留标记供下一轮重试。 */
	mas_for_each(&mas, area, ULONG_MAX) {
		if (!is_pending_free(area))
			continue;

		area = pending_free_clear(area);
		if (__execmem_cache_free(&mas, area, GFP_KERNEL))
			continue;

		execmem_cache.pending_free_cnt--;
	}

	/* 有遗留项保持 delayed work 存活；全部完成后再触发可能释放大页的 clean。 */
	if (execmem_cache.pending_free_cnt)
		schedule_delayed_work(&execmem_cache_free_work, FREE_DELAY);
	else
		schedule_work(&execmem_cache_clean_work);
}

/*
 * execmem_cache_free() - 尝试把调用者归还的 ROX 地址放回缓存
 * 业务背景：用 busy tree 判断地址是否来自本缓存，避免接管普通 execmem 映射。
 * 入参：ptr 是调用者不再使用的地址；返回 true 表示缓存已接管（含延迟路径）。
 * 注意事项：mutex 串行化查找和状态转移；false 时外层必须 vfree。
 */
static bool execmem_cache_free(void *ptr)
{
	struct maple_tree *busy_areas = &execmem_cache.busy_areas;
	unsigned long addr = (unsigned long)ptr;
	MA_STATE(mas, busy_areas, addr, addr);
	void *area;
	int err;

	guard(mutex)(&execmem_cache.mutex);

	/* 精确地址查 busy tree；不存在说明它不是 ROX cache 所有的映射。 */
	area = mas_walk(&mas);
	if (!area)
		return false;

	/* 快路径不可为 metadata 回收反复重试；失败改标记并转移给 worker。 */
	err = __execmem_cache_free(&mas, area, GFP_KERNEL | __GFP_NORETRY);
	if (err) {
		/*
		 * mas points to exact slot we've got the area from, nothing
		 * else can modify the tree because of the mutex, so there
		 * won't be any allocations in mas_store_gfp() and it will just
		 * change the pointer.
		 */
		/* mutex 保证 mas 仍精确指向该槽，故只改 payload 不会覆盖并发分配。 */
		area = pending_free_set(area);
		mas_store_gfp(&mas, area, GFP_KERNEL);
		execmem_cache.pending_free_cnt++;
		schedule_delayed_work(&execmem_cache_free_work, FREE_DELAY);
		return true;
	}

	schedule_work(&execmem_cache_clean_work);

	return true;
}

#else /* CONFIG_ARCH_HAS_EXECMEM_ROX */
/*
 * when ROX cache is not used the permissions defined by architectures for
 * execmem ranges that are updated before use (e.g. EXECMEM_MODULE_TEXT) must
 * be writable anyway
 */
/* 无 ROX cache 的架构本就以可写权限交付更新前的可执行范围，无需切权限。 */
static inline int execmem_force_rw(void *ptr, size_t size)
{
	return 0;
}

/* 配置桩：上层据 NULL 回退到普通 vmalloc 后端。 */
static void *execmem_cache_alloc(struct execmem_range *range, size_t size)
{
	return NULL;
}

/* 配置桩：false 使 execmem_free() 直接归还 vmalloc 映射。 */
static bool execmem_cache_free(void *ptr)
{
	return false;
}
#endif /* CONFIG_ARCH_HAS_EXECMEM_ROX */

/*
 * execmem_alloc() - 按子系统类型分配可执行或与之耦合的内存
 * 业务背景：模块加载、探针、ftrace/BPF 共享 API，但各架构对位置和 W^X 有不同限制。
 * 入参：type 选择启动期配置的范围，size 为请求字节数。出参/返回：成功返回调用者
 * 持有的 untagged 地址，失败为 NULL。注意事项：可睡眠；调用者最终必须 execmem_free。
 */
void *execmem_alloc(enum execmem_type type, size_t size)
{
	struct execmem_range *range = &execmem_info->ranges[type];
	bool use_cache = range->flags & EXECMEM_ROX_CACHE;
	unsigned long vm_flags = VM_FLUSH_RESET_PERMS;
	pgprot_t pgprot = range->pgprot;
	void *p = NULL;

	/* 缓存树和页表以页为单位，先统一向上取整以保持 free 的区间账本一致。 */
	size = PAGE_ALIGN(size);

	if (use_cache)
		p = execmem_cache_alloc(range, size);
	else
		p = execmem_vmalloc(range, size, pgprot, vm_flags);

	return kasan_reset_tag(p);
}

/*
 * execmem_alloc_rw() - 取得可执行范围后临时交付可写版本
 * 业务背景：JIT、替换指令等需先生成内容；普通 alloc 的最终权限可能是 ROX。
 * 入参：type/size 同 execmem_alloc。出参/返回：成功转移地址给调用者，失败 NULL。
 * 注意事项：__free 自动在失败返回时 execmem_free；成功 no_free_ptr 转移责任。
 */
void *execmem_alloc_rw(enum execmem_type type, size_t size)
{
	void *p __free(execmem) = execmem_alloc(type, size);
	int err;

	/* cleanup class 在此出口自动归还 p，避免 force_rw 失败泄漏 ROX 缓存项。 */
	if (!p)
		return NULL;

	/* 成功后 no_free_ptr 取消自动 cleanup，把最终权限管理责任交给调用者。 */
	err = execmem_force_rw(p, size);
	if (err)
		return NULL;

	return no_free_ptr(p);
}

/*
 * execmem_free() - 归还一段此前取得的 execmem 地址
 * 业务背景：优先交给 ROX cache 复用，否则释放普通 vmalloc 映射。入参：ptr 为
 * 调用者已停止执行/访问的地址；出参/返回：无。注意事项：vmalloc 不能在中断中
 * 处理只读映射，因此该 API 只能在非中断上下文调用。
 */
void execmem_free(void *ptr)
{
	/*
	 * This memory may be RO, and freeing RO memory in an interrupt is not
	 * supported by vmalloc.
	 */
	/* 这不仅是调试告警：继续执行可能在 vmalloc 权限处理路径破坏上下文约束。 */
	WARN_ON(in_interrupt());

	if (!execmem_cache_free(ptr))
		vfree(ptr);
}

/* 查询启动配置是否要求 ROX cache；调用者据此决定是否必须自行恢复权限。 */
bool execmem_is_rox(enum execmem_type type)
{
	return !!(execmem_info->ranges[type].flags & EXECMEM_ROX_CACHE);
}

/*
 * execmem_validate() - 检查架构交出的默认可执行窗口是否可用
 * 业务背景：模块加载不能在半初始化或无权限的范围上继续；此处是启动期拒绝边界。
 * 入参：info 是 arch setup 或默认对象，调用期间独占。出参/返回：true 可继续初始化，
 * false 表示保留禁用状态。注意事项：无 ROX 支持时会就地降级 flags，之后才发布。
 */
static bool execmem_validate(struct execmem_info *info)
{
	struct execmem_range *r = &info->ranges[EXECMEM_DEFAULT];

	/* DEFAULT 是所有遗漏类型的模板，任一核心字段为零都会使后续分配无定义。 */
	/* 对齐、两端和页保护共同构成可分配窗口，缺一不可。 */
	if (!r->alignment || !r->start || !r->end || !pgprot_val(r->pgprot)) {
		pr_crit("Invalid parameters for execmem allocator, module loading will fail");
		return false;
	}

	/* 架构未实现 ROX 后端时禁止留下会调用配置桩的 cache 标志。 */
	/* 枚举每个类型而非只改 default，专属配置也可能独自设置了 cache 位。 */
	/* 配置关闭时仍逐个清理 flags，循环体只修改启动期私有 info。 */
	if (!IS_ENABLED(CONFIG_ARCH_HAS_EXECMEM_ROX)) {
		/* 每轮 i 对应一个 enum execmem_type，r 是该类型的可写启动配置。 */
		for (int i = EXECMEM_DEFAULT; i < EXECMEM_TYPE_MAX; i++) {
			r = &info->ranges[i];

			/* 只移除不可实现的 cache 位，保留 KASAN shadow 等独立策略。 */
			if (r->flags & EXECMEM_ROX_CACHE) {
				pr_warn_once("ROX cache is not supported\n");
				r->flags &= ~EXECMEM_ROX_CACHE;
			}
		}
	}

	return true;
}

/*
 * execmem_init_missing() - 用 DEFAULT 补齐架构未逐项指定的类型
 * 业务背景：架构只需描述差异范围，统一继承避免新增 enum 类型时配置遗漏。
 * 入参：启动期独占 info；出参/返回：无，原本 start 非零的专属配置保持不变。
 * 注意事项：模块数据特例强制 PAGE_KERNEL，不继承可能含执行权限的默认 pgprot。
 */
static void execmem_init_missing(struct execmem_info *info)
{
	struct execmem_range *default_range = &info->ranges[EXECMEM_DEFAULT];

	/* start 为零是“未定义”哨兵；复制整组字段维持首选/后备窗口的一致性。 */
	for (int i = EXECMEM_DEFAULT + 1; i < EXECMEM_TYPE_MAX; i++) {
		struct execmem_range *r = &info->ranges[i];

		/* 专属项有 start 即完整交给架构；零 start 才继承默认策略。 */
		/* 进入该分支时 r 未拥有专属 VA 窗口；以下字段一次性继承默认模板。 */
		if (!r->start) {
			/* text 可继承默认 pgprot，数据必须保持可写且不可执行的内核保护。 */
			if (i == EXECMEM_MODULE_DATA)
				r->pgprot = PAGE_KERNEL;
			else
				r->pgprot = default_range->pgprot;
			/* 其余范围、对齐和 flags 必须整体复制，否则同一类型会得到混合策略。 */
			r->alignment = default_range->alignment;
			r->start = default_range->start;
			r->end = default_range->end;
			r->flags = default_range->flags;
			r->fallback_start = default_range->fallback_start;
			r->fallback_end = default_range->fallback_end;
		}
	}
}

/* 默认弱钩子：无架构实现时返回 NULL，让公共初始化构造通用 vmalloc 策略。 */
struct execmem_info * __weak execmem_arch_setup(void)
{
	return NULL;
}

/*
 * __execmem_init() - 建立并发布整个 execmem 配置
 * 业务背景：这是启动期从架构策略到并发分配器只读配置的提交点。
 * 入参：无。出参/返回：无直接返回值；成功后 execmem_info 对所有分配者可见。
 * 注意事项：仅 init 上下文调用；验证失败不发布不完整策略，模块加载随后会失败。
 */
static void __init __execmem_init(void)
{
	struct execmem_info *info = execmem_arch_setup();

	/* 架构钩子优先；缺失时选择通用可执行 vmalloc 窗口而非猜测架构限制。 */
	if (!info) {
		info = execmem_info = &default_execmem_info;
		/* 默认范围覆盖通用 vmalloc 区，适用于没有额外可达性约束的架构。 */
		info->ranges[EXECMEM_DEFAULT].start = VMALLOC_START;
		info->ranges[EXECMEM_DEFAULT].end = VMALLOC_END;
		info->ranges[EXECMEM_DEFAULT].pgprot = PAGE_KERNEL_EXEC;
		/* 通用窗口不要求额外 text 对齐；具体架构钩子可覆盖该保守默认。 */
		info->ranges[EXECMEM_DEFAULT].alignment = 1;
	}

	/* 先校验必需模板，再补齐可选类型，避免把坏模板扩散到所有范围。 */
	if (!execmem_validate(info))
		return;

	execmem_init_missing(info);

	/* 此赋值是启动初始化完成后的发布点；之后配置由 __ro_after_init 固化。 */
	execmem_info = info;
}

#ifdef CONFIG_ARCH_WANTS_EXECMEM_LATE
/* 需要较晚架构状态的配置通过 core_initcall 在依赖就绪后发布。 */
static int __init execmem_late_init(void)
{
	__execmem_init();
	return 0;
}
core_initcall(execmem_late_init);
#else
/* 普通架构由早期公共入口完成同一初始化；无返回值，失败由 validate 日志表明。 */
void __init execmem_init(void)
{
	__execmem_init();
}
#endif
