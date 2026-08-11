// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2008 Advanced Micro Devices, Inc.
 *
 * Author: Joerg Roedel <joerg.roedel@amd.com>
 */

#define pr_fmt(fmt)	"DMA-API: " fmt
/* 本文件所有 pr_*() 日志统一使用 "DMA-API: " 前缀，便于从系统日志定位调试器。 */

#include <linux/sched/task_stack.h>
#include <linux/scatterlist.h>
#include <linux/dma-map-ops.h>
#include <linux/sched/task.h>
#include <linux/stacktrace.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/swiotlb.h>
#include <asm/sections.h>
#include "debug.h"

/*
 * DMA 调试 entry 哈希表参数：共 16384 个桶，以 DMA 地址右移 13 位后的低 14 位
 * 选桶。右移跳过 8 KiB 内偏移，使同一映射附近的地址倾向落入同一桶；MASK 要求
 * HASH_SIZE 为二次幂。
 */
#define HASH_SIZE       16384ULL
#define HASH_FN_SHIFT   13
#define HASH_FN_MASK    (HASH_SIZE - 1)

/* 默认预分配 65536 个 entry，降低启动后首批 DMA 操作触发原子扩容的概率。 */
#define PREALLOC_DMA_DEBUG_ENTRIES (1 << 16)
/* If the pool runs out, add this many new entries at once */
/* 原注释翻译：对象池耗尽时，一次新增这么多个 entry；数量恰好铺满一页。 */
#define DMA_DEBUG_DYNAMIC_ENTRIES (PAGE_SIZE / sizeof(struct dma_debug_entry))

/*
 * 每个 entry 所代表的 DMA API 资源类别：single 用于单段 sync 查询，sg 表示散列表，
 * coherent/noncoherent 表示两类分配，phy 表示物理 map。它们用于 API 配对诊断。
 */
enum {
	dma_debug_single,
	dma_debug_sg,
	dma_debug_coherent,
	dma_debug_noncoherent,
	dma_debug_phy,
};

/*
 * 映射错误检查状态：NOT_APPLICABLE 表示该 API 无此要求，NOT_CHECKED 表示驱动尚未
 * 调用 dma_mapping_error()，CHECKED 表示已观察到对应检查。
 */
enum map_err_types {
	MAP_ERR_CHECK_NOT_APPLICABLE,
	MAP_ERR_NOT_CHECKED,
	MAP_ERR_CHECKED,
};

/* 每个 entry 最多保存 5 个创建现场返回地址，控制诊断价值与对象体积的平衡。 */
#define DMA_DEBUG_STACKTRACE_ENTRIES 5

/**
 * struct dma_debug_entry - track a dma_map* or dma_alloc_coherent mapping
 * @list: node on pre-allocated free_entries list
 * @dev: 'dev' argument to dma_map_{page|single|sg} or dma_alloc_coherent
 * @dev_addr: dma address
 * @size: length of the mapping
 * @type: single, page, sg, coherent
 * @direction: enum dma_data_direction
 * @sg_call_ents: 'nents' from dma_map_sg
 * @sg_mapped_ents: 'mapped_ents' from dma_map_sg
 * @paddr: physical start address of the mapping
 * @map_err_type: track whether dma_mapping_error() was checked
 * @attrs: dma attributes
 * @stack_len: number of backtrace entries in @stack_entries
 * @stack_entries: stack of backtrace history
 */
/*
 * struct dma_debug_entry - 一次 DMA map/alloc 的调试镜像
 * @list: entry 空闲时挂 free_entries，活动时挂所属哈希桶；同一时刻只能属于一张链表
 * @dev: 发起 DMA 操作的设备借用指针，设备解绑前用于泄漏诊断
 * @dev_addr: 设备所见 DMA 起始地址，也是哈希和配对查找的主键
 * @size: 映射或分配长度，单位字节
 * @type: dma_debug_* 资源类别
 * @direction: DMA 数据方向，用于校验 unmap/sync 配对
 * @sg_call_ents: 驱动传给 dma_map_sg() 的原始 SG 项数
 * @sg_mapped_ents: 后端合并后返回、具有有效 DMA 字段的 SG 项数
 * @paddr: CPU 物理起始地址，用于 cacheline 重叠和非法区域检查
 * @map_err_type: dma_mapping_error() 检查状态
 * @attrs: map/alloc 时的 DMA 属性快照
 * @stack_len: CONFIG_STACKTRACE 下 @stack_entries 的有效数量
 * @stack_entries: 创建映射时的有限调用栈
 *
 * 原 kernel-doc 描述了上述字段。本对象只记录真实 DMA 操作的镜像，不拥有设备、
 * 页面或 DMA 映射；真实操作即使未能分配调试 entry 也不会被回滚。活动阶段受桶锁
 * 与 radix_lock 管理，释放后回到 free_entries 对象池并可被清零复用。
 */
struct dma_debug_entry {
	struct list_head list;
	struct device    *dev;
	u64              dev_addr;
	u64              size;
	int              type;
	int              direction;
	int		 sg_call_ents;
	int		 sg_mapped_ents;
	phys_addr_t	 paddr;
	enum map_err_types map_err_type;
	unsigned long	 attrs;
#ifdef CONFIG_STACKTRACE
	unsigned int	stack_len;
	unsigned long	stack_entries[DMA_DEBUG_STACKTRACE_ENTRIES];
#endif
} ____cacheline_aligned_in_smp;

/* match_fn 是桶查找谓词：比较查询 entry 与活动候选，返回主键/包含关系是否成立。 */
typedef bool (*match_fn)(struct dma_debug_entry *, struct dma_debug_entry *);

/*
 * struct hash_bucket - 一组具有相同地址哈希值的活动 entry
 * @list: 活动 dma_debug_entry 链表
 * @lock: IRQ-safe 地保护 @list 以及持锁查找到的 entry 字段
 */
struct hash_bucket {
	struct list_head list;
	spinlock_t lock;
};

/* Hash list to save the allocated dma addresses */
/* 原注释翻译：保存已分配/映射 DMA 地址的哈希桶数组；每桶有独立锁以降低竞争。 */
static struct hash_bucket dma_entry_hash[HASH_SIZE];
/* List of pre-allocated dma_debug_entry's */
/* 原注释翻译：预分配 dma_debug_entry 的空闲链表；其中对象由整页分配且不单独释放。 */
static LIST_HEAD(free_entries);
/* Lock for the list above */
/* 原注释翻译：保护上方 free_entries 及其数量/最低水位统计的 IRQ-safe 自旋锁。 */
static DEFINE_SPINLOCK(free_entries_lock);

/* Global disable flag - will be set in case of an error */
/* 原注释翻译：全局禁用标记；对象池或 cacheline 账本发生致命错误时置位。 */
static bool global_disable __read_mostly;

/* Early initialization disable flag, set at the end of dma_debug_init */
/* 原注释翻译：dma_debug_init() 完成核心初始化时置位；此前所有 hook 都应旁路。 */
static bool dma_debug_initialized __read_mostly;

/*
 * dma_debug_disabled - 判断调试 hook 当前是否必须旁路
 *
 * 无参数。显式/故障禁用，或核心尚未初始化时返回 true；完全可用时返回 false。
 * 该快速路径无锁且只读 read-mostly 标志，允许极少量并发状态变化造成一次漏记；
 * 调试器不能为追求精确而改变真实 DMA API 的并发与可用性。
 */
static inline bool dma_debug_disabled(void)
{
	return global_disable || !dma_debug_initialized;
}

/* Global error count */
/* 原注释翻译：累计发现的 DMA API 错误数；允许无锁更新丢计数，仅用于诊断。 */
static u32 error_count;

/* Global error show enable*/
/* 原注释翻译：非零时显示全部错误，不消耗 show_num_errors 配额。 */
static u32 show_all_errors __read_mostly;
/* Number of errors to show */
/* 原注释翻译：未开启全部显示时，还允许输出到日志的错误数量；默认只显示一个。 */
static u32 show_num_errors = 1;

/*
 * num_free_entries: 当前空闲 entry 数；min_free_entries: 历史最低空闲水位；
 * nr_total_entries: 对象池累计容量。三者由 free_entries_lock 保护写入，但 debugfs
 * 可无锁观察瞬时值，因此只提供诊断近似而非一致快照。
 */
static u32 num_free_entries;
static u32 min_free_entries;
static u32 nr_total_entries;

/* number of preallocated entries requested by kernel cmdline */
/* 原注释翻译：命令行请求的启动期预分配 entry 数，默认 65536。 */
static u32 nr_prealloc_entries = PREALLOC_DMA_DEBUG_ENTRIES;

/* per-driver filter related state */
/* 原注释翻译：以下状态把错误输出限制到指定驱动；账本记录本身仍覆盖全部驱动。 */

/* 驱动过滤名称含终止 NUL 的最大存储长度。 */
#define NAME_MAX_LEN	64

/* 过滤名称由 driver_name_lock 保护；current_driver 缓存首次匹配的 driver 借用指针。 */
static char                  current_driver_name[NAME_MAX_LEN] __read_mostly;
static struct device_driver *current_driver                    __read_mostly;

static DEFINE_RWLOCK(driver_name_lock);

/* 三张字符串表把内部枚举安全转换为 debugfs/日志中的稳定可读名称。 */
static const char *const maperr2str[] = {
	[MAP_ERR_CHECK_NOT_APPLICABLE] = "dma map error check not applicable",
	[MAP_ERR_NOT_CHECKED] = "dma map error not checked",
	[MAP_ERR_CHECKED] = "dma map error checked",
};

static const char *type2name[] = {
	[dma_debug_single] = "single",
	[dma_debug_sg] = "scatter-gather",
	[dma_debug_coherent] = "coherent",
	[dma_debug_noncoherent] = "noncoherent",
	[dma_debug_phy] = "phy",
};

static const char *dir2name[] = {
	[DMA_BIDIRECTIONAL]	= "DMA_BIDIRECTIONAL",
	[DMA_TO_DEVICE]		= "DMA_TO_DEVICE",
	[DMA_FROM_DEVICE]	= "DMA_FROM_DEVICE",
	[DMA_NONE]		= "DMA_NONE",
};

/*
 * The access to some variables in this macro is racy. We can't use atomic_t
 * here because all these variables are exported to debugfs. Some of them even
 * writeable. This is also the reason why a lock won't help much. But anyway,
 * the races are no big deal. Here is why:
 *
 *   error_count: the addition is racy, but the worst thing that can happen is
 *                that we don't count some errors
 *   show_num_errors: the subtraction is racy. Also no big deal because in
 *                    worst case this will result in one warning more in the
 *                    system log than the user configured. This variable is
 *                    writeable via debugfs.
 */
/*
 * 原注释翻译：err_printk() 使用的 error_count 与 show_num_errors 存在有意接受的竞争。
 * 它们通过 debugfs 暴露，部分还可写，使用 atomic_t 或局部锁也无法给用户提供完整
 * 事务快照。最坏结果只是少计一次错误或多打印一条警告，不影响 DMA 正确性。
 */
/*
 * dump_entry_trace - 输出创建映射时保存的有限调用栈
 * @entry: 发生错误的活动 entry；允许为 NULL
 *
 * CONFIG_STACKTRACE 开启且 @entry 有效时打印其 @stack_len 个地址；否则无操作。
 * 函数不修改 entry、不获取锁，调用方须以桶锁或“entry 尚未对外并发可见”等方式
 * 保证其仍存活。日志输出本身不改变调试账本；无返回值。
 */
static inline void dump_entry_trace(struct dma_debug_entry *entry)
{
#ifdef CONFIG_STACKTRACE
	if (entry) {
		pr_warn("Mapped at:\n");
		stack_trace_print(entry->stack_entries, entry->stack_len, 0);
	}
#endif
}

/*
 * driver_filter - 判断某设备的错误是否符合当前驱动过滤器
 * @dev: 发生错误的设备；允许为 NULL
 *
 * 过滤名为空立即返回 true。已有 @current_driver 缓存时只接受 dev->driver 与之相同
 * 的设备；设备或 driver 为空返回 false。首次匹配时在 driver_name_lock 读锁和关 IRQ
 * 区间内比较名称，并把 @current_driver 缓存为借用指针。该赋值发生在读锁内，多个
 * 首次匹配者可能写入同一候选缓存；这是只影响过滤显示的可接受竞争。局部 @drv 是
 * 候选驱动，@flags 保存 IRQ 状态，@ret 保存匹配结果。
 *
 * 返回值只控制日志输出，不控制 error_count 增长或 entry 记录。缓存不取得
 * device_driver 引用，仅在用户通过 filter_write() 改写/关闭过滤器时于写锁下清空；
 * 快速路径只比较指针而不解引用缓存对象。
 */
static bool driver_filter(struct device *dev)
{
	struct device_driver *drv;
	unsigned long flags;
	bool ret;

	/* driver filter off */
	/* 原注释翻译：过滤器关闭，所有设备错误都允许显示。 */
	if (likely(!current_driver_name[0]))
		return true;

	/* driver filter on and initialized */
	/* 原注释翻译：过滤器已解析到具体 driver，只接受完全相同的驱动对象。 */
	if (current_driver && dev && dev->driver == current_driver)
		return true;

	/* driver filter on, but we can't filter on a NULL device... */
	/* 原注释翻译：过滤器开启时无法判断 NULL 设备属于哪个驱动，因此拒绝显示。 */
	if (!dev)
		return false;

	if (current_driver || !current_driver_name[0])
		return false;

	/* driver filter on but not yet initialized */
	/* 原注释翻译：过滤名已设置但尚未缓存 driver，尝试由当前设备完成首次解析。 */
	drv = dev->driver;
	if (!drv)
		return false;

	/* lock to protect against change of current_driver_name */
	/* 原注释翻译：读锁防止比较过程中 debugfs 改写当前过滤名称。 */
	read_lock_irqsave(&driver_name_lock, flags);

	ret = false;
	if (drv->name &&
	    strncmp(current_driver_name, drv->name, NAME_MAX_LEN - 1) == 0) {
		current_driver = drv;
		ret = true;
	}

	read_unlock_irqrestore(&driver_name_lock, flags);

	return ret;
}

/*
 * err_printk - 记录一个 DMA API 错误，并按过滤器/配额选择是否 WARN
 * @dev: 相关设备，可为 NULL；@entry: 相关映射镜像，可为 NULL
 * @format/@arg: 具体错误正文及格式参数
 *
 * 每次调用都递增 error_count；只有 driver_filter() 通过且显示开关允许时才输出设备
 * 名称、正文和创建栈。非“显示全部”模式下消耗一份 show_num_errors 配额，即使该次
 * 因驱动过滤而未打印也会消耗。宏无返回值，允许统计竞争，不能改变真实 DMA 操作。
 */
#define err_printk(dev, entry, format, arg...) do {			\
		error_count += 1;					\
		if (driver_filter(dev) &&				\
		    (show_all_errors || show_num_errors > 0)) {		\
			WARN(1, pr_fmt("%s %s: ") format,		\
			     dev ? dev_driver_string(dev) : "NULL",	\
			     dev ? dev_name(dev) : "NULL", ## arg);	\
			dump_entry_trace(entry);			\
		}							\
		if (!show_all_errors && show_num_errors > 0)		\
			show_num_errors -= 1;				\
	} while (0);

/*
 * Hash related functions
 *
 * Every DMA-API request is saved into a struct dma_debug_entry. To
 * have quick access to these structs they are stored into a hash.
 */
/*
 * 原注释翻译：每个 DMA API 请求都保存为 dma_debug_entry；为快速按设备 DMA 地址
 * 找回配对记录，活动 entry 被组织到哈希表中。以下 helper 的锁契约是：get 取得并
 * 返回锁定桶，put 配对释放；桶内查找/增删均由调用者持有对应桶锁。
 */
/*
 * hash_fn - 由 entry 的 DMA 地址计算桶索引
 * @entry: 至少已填写 dev_addr 的引用 entry 或活动 entry
 *
 * 原注释说明哈希基于 DMA 地址。当前代码右移 HASH_FN_SHIFT（13）后应用 16383
 * 掩码，得到 [0, HASH_SIZE) 索引。函数无副作用、无锁、不可睡眠；返回 int 桶号。
 */
static int hash_fn(struct dma_debug_entry *entry)
{
	/*
	 * Hash function is based on the dma address.
	 * We use bits 20-27 here as the index into the hash
	 */
	/*
	 * 原注释翻译：哈希函数基于 DMA 地址，并取其中一段位作为索引。注释中的历史
	 * 位号与当前 SHIFT/MASK 常量并不完全一致，学习时应以实际表达式为准。
	 */
	return (entry->dev_addr >> HASH_FN_SHIFT) & HASH_FN_MASK;
}

/*
 * Request exclusive access to a hash bucket for a given dma_debug_entry.
 */
/*
 * get_hash_bucket - 计算并独占锁定 entry 对应的哈希桶
 * @entry: 提供 dev_addr 的查询/活动 entry
 * @flags: 输出加锁前的本地 IRQ 状态，供 put_hash_bucket() 恢复
 *
 * 局部 @idx 是哈希桶号，@__flags 接收 spin_lock_irqsave() 状态；成功总是返回已锁定
 * bucket，不会失败或睡眠。稀疏注解 __acquires 声明锁所有权转移给调用者。
 */
static struct hash_bucket *get_hash_bucket(struct dma_debug_entry *entry,
					   unsigned long *flags)
	__acquires(&dma_entry_hash[idx].lock)
{
	int idx = hash_fn(entry);
	unsigned long __flags;

	spin_lock_irqsave(&dma_entry_hash[idx].lock, __flags);
	*flags = __flags;
	return &dma_entry_hash[idx];
}

/*
 * Give up exclusive access to the hash bucket
 */
/*
 * put_hash_bucket - 释放由 get_hash_bucket() 取得的桶锁并恢复 IRQ
 * @bucket: 当前调用者独占的桶
 * @flags: 对应 get 时保存的 IRQ 状态
 *
 * 无返回值。__releases 帮助静态锁检查；参数必须严格配对，不能释放错误桶或重复释放。
 */
static void put_hash_bucket(struct hash_bucket *bucket,
			    unsigned long flags)
	__releases(&bucket->lock)
{
	spin_unlock_irqrestore(&bucket->lock, flags);
}

/*
 * exact_match - 判断两个 entry 是否具有相同设备与 DMA 起始地址
 * @a: 查询 entry；@b: 候选活动 entry
 *
 * 两个主键字段都相等返回 true，否则 false。大小、类型、方向等由后续 best-fit
 * 评分处理；函数只读输入、无锁要求，但调用者通常持有 @b 所在桶锁。
 */
static bool exact_match(struct dma_debug_entry *a, struct dma_debug_entry *b)
{
	return ((a->dev_addr == b->dev_addr) &&
		(a->dev == b->dev)) ? true : false;
}

/*
 * containing_match - 判断候选映射是否完整包含查询子区间
 * @a: 待 sync 等操作引用的子区间
 * @b: 哈希表中的原始映射候选
 *
 * 设备不同立即 false；同设备且 b.start <= a.start、b.end >= a.end 时返回 true。
 * 使用半开区间端点相加比较，调用契约要求地址加长度不溢出。只读、无副作用。
 */
static bool containing_match(struct dma_debug_entry *a,
			     struct dma_debug_entry *b)
{
	if (a->dev != b->dev)
		return false;

	if ((b->dev_addr <= a->dev_addr) &&
	    ((b->dev_addr + b->size) >= (a->dev_addr + a->size)))
		return true;

	return false;
}

/*
 * Search a given entry in the hash bucket list
 */
/*
 * __hash_bucket_find - 在已锁定桶中按匹配谓词选择唯一最佳 entry
 * @bucket: 调用者已持锁的哈希桶
 * @ref: 描述本次 unmap/sync 的查询镜像
 * @match: exact_match 或 containing_match 谓词
 *
 * 遍历中 @entry 是候选，@matches 统计通过主谓词的数量，@match_lvl 按 size/type/
 * direction/sg_call_ents 四项相等数评分，@last_lvl 与 @ret 保存当前最佳候选。四项
 * 全等立即返回 perfect-fit。若最终只有一个主匹配则返回它；多个但无 perfect-fit
 * 返回 NULL，宁可报告无法配对也不误删任一真实映射。
 *
 * 函数不获取/释放桶锁，不改变链表；返回的是受桶锁保护的借用指针。调用者在解锁
 * 后不得继续访问该 entry。
 */
static struct dma_debug_entry *__hash_bucket_find(struct hash_bucket *bucket,
						  struct dma_debug_entry *ref,
						  match_fn match)
{
	struct dma_debug_entry *entry, *ret = NULL;
	int matches = 0, match_lvl, last_lvl = -1;

	list_for_each_entry(entry, &bucket->list, list) {
		if (!match(ref, entry))
			continue;

		/*
		 * Some drivers map the same physical address multiple
		 * times. Without a hardware IOMMU this results in the
		 * same device addresses being put into the dma-debug
		 * hash multiple times too. This can result in false
		 * positives being reported. Therefore we implement a
		 * best-fit algorithm here which returns the entry from
		 * the hash which fits best to the reference value
		 * instead of the first-fit.
		 */
		/*
		 * 原注释翻译：无 IOMMU 时，同一物理地址被重复映射可能产生相同 DMA 地址，
		 * 若直接返回首项会制造误报。因此比较四个附加字段，优先接受完全匹配；无法
		 * 在多个非完全候选间唯一判定时放弃匹配。
		 */
		matches += 1;
		match_lvl = 0;
		entry->size         == ref->size         ? ++match_lvl : 0;
		entry->type         == ref->type         ? ++match_lvl : 0;
		entry->direction    == ref->direction    ? ++match_lvl : 0;
		entry->sg_call_ents == ref->sg_call_ents ? ++match_lvl : 0;

		if (match_lvl == 4) {
			/* perfect-fit - return the result */
			/* 原注释翻译：四项完全吻合，直接返回完美匹配。 */
			return entry;
		} else if (match_lvl > last_lvl) {
			/*
			 * We found an entry that fits better then the
			 * previous one or it is the 1st match.
			 */
			/* 原注释翻译：当前候选比先前候选更贴近查询，或它是第一个候选。 */
			last_lvl = match_lvl;
			ret      = entry;
		}
	}

	/*
	 * If we have multiple matches but no perfect-fit, just return
	 * NULL.
	 */
	/* 原注释翻译：存在多个候选却没有完美匹配时返回 NULL，避免任意选择。 */
	ret = (matches == 1) ? ret : NULL;

	return ret;
}

/*
 * bucket_find_exact - 在已锁定桶中按设备与 DMA 起始地址查找
 * @bucket: 已锁定桶；@ref: 查询镜像
 *
 * 转交 __hash_bucket_find(..., exact_match)，继承唯一/完美匹配与借用生命周期契约。
 */
static struct dma_debug_entry *bucket_find_exact(struct hash_bucket *bucket,
						 struct dma_debug_entry *ref)
{
	return __hash_bucket_find(bucket, ref, exact_match);
}

/*
 * bucket_find_contain - 向低地址桶回溯查找完整包含 ref 的映射
 * @bucket: 输入为 ref 起始地址对应的已锁定桶；输出为最终仍锁定的桶
 * @ref: sync 子区间查询镜像
 * @flags: 输入/输出当前桶锁配对的 IRQ 状态
 *
 * @entry 保存查找结果，@index 是逐次减去 8 KiB 的临时地址，@limit 防止在地址零
 * 附近下溢并把扫描限制到 HASH_SIZE 个桶；循环 @i 是回溯次数。每次未命中先释放
 * 旧桶，再锁前一地址对应桶。命中返回受当前输出桶锁保护的 entry；完全失败返回
 * NULL，但最后一个 *@bucket 仍保持锁定，调用者必须统一 put_hash_bucket()。
 */
static struct dma_debug_entry *bucket_find_contain(struct hash_bucket **bucket,
						   struct dma_debug_entry *ref,
						   unsigned long *flags)
{

	struct dma_debug_entry *entry, index = *ref;
	int limit = min(HASH_SIZE, (index.dev_addr >> HASH_FN_SHIFT) + 1);

	for (int i = 0; i < limit; i++) {
		entry = __hash_bucket_find(*bucket, ref, containing_match);

		if (entry)
			return entry;

		/*
		 * Nothing found, go back a hash bucket
		 */
		/* 原注释翻译：当前桶没有包含映射，释放它并向更低 DMA 地址的桶回退。 */
		put_hash_bucket(*bucket, *flags);
		index.dev_addr -= (1 << HASH_FN_SHIFT);
		*bucket = get_hash_bucket(&index, flags);
	}

	return NULL;
}

/*
 * Add an entry to a hash bucket
 */
/*
 * hash_bucket_add - 把新活动 entry 追加到桶链表尾部
 * @bucket: 调用者已独占锁定的目标桶
 * @entry: 已填充且当前不在其他链表中的 entry
 *
 * 无返回值；只改变链表关系，不处理 cacheline 树或对象 ownership。
 */
static void hash_bucket_add(struct hash_bucket *bucket,
			    struct dma_debug_entry *entry)
{
	list_add_tail(&entry->list, &bucket->list);
}

/*
 * Remove entry from a hash bucket list
 */
/*
 * hash_bucket_del - 从当前活动桶摘除 entry
 * @entry: 调用者在其桶锁下找到的活动 entry
 *
 * 无返回值。摘除后对象尚未回到 free-list，调用者须先释放桶锁，再调用
 * dma_entry_free() 按既定锁序移除 cacheline 记录并归还对象。
 */
static void hash_bucket_del(struct dma_debug_entry *entry)
{
	list_del(&entry->list);
}

/*
 * For each mapping (initial cacheline in the case of
 * dma_alloc_coherent/dma_map_page, initial cacheline in each page of a
 * scatterlist, or the cacheline specified in dma_map_single) insert
 * into this tree using the cacheline as the key. At
 * dma_unmap_{single|sg|page} or dma_free_coherent delete the entry.  If
 * the entry already exists at insertion time add a tag as a reference
 * count for the overlapping mappings.  For now, the overlap tracking
 * just ensures that 'unmaps' balance 'maps' before marking the
 * cacheline idle, but we should also be flagging overlaps as an API
 * violation.
 *
 * Memory usage is mostly constrained by the maximum number of available
 * dma-debug entries in that we need a free dma_debug_entry before
 * inserting into the tree.  In the case of dma_map_page and
 * dma_alloc_coherent there is only one dma_debug_entry and one
 * dma_active_cacheline entry to track per event.  dma_map_sg(), on the
 * other hand, consumes a single dma_debug_entry, but inserts 'nents'
 * entries into the tree.
 *
 * Use __GFP_NOWARN because the printk from an OOM, to netconsole, could end
 * up right back in the DMA debugging code, leading to a deadlock.
 */
/*
 * 原注释翻译与展开：除按 DMA 地址保存 entry 外，调试器还用物理 cacheline 编号为
 * key 建立 radix tree。map/alloc 时插入代表性 cacheline，unmap/free 时删除；同一
 * key 重复映射时用 radix tag 的各个位编码重叠计数，使 map/unmap 数量平衡后才把
 * cacheline 标为空闲。SG 会消耗多个活动 key，而可跟踪规模首先受 free entry 数量
 * 限制。原文所称“SG 只消耗一个 dma_debug_entry”属于历史实现描述；当前
 * debug_dma_map_sg() 会为每个 mapped entry 分配一个调试对象，应以代码为准。
 * radix 分配使用 GFP_ATOMIC|__GFP_NOWARN，避免 OOM printk 经 netconsole 的 DMA
 * 路径递归回调本调试器并死锁。
 */
/*
 * dma_active_cacheline: 物理 cacheline 编号到首个 dma_debug_entry 的 radix tree；
 * radix_lock: 保护树节点、tag 重叠计数及查找/增删。树不拥有 entry，对象归还前
 * 必须先通过 active_cacheline_remove() 平衡记录。
 */
static RADIX_TREE(dma_active_cacheline, GFP_ATOMIC | __GFP_NOWARN);
static DEFINE_SPINLOCK(radix_lock);
/* tag 位全 1 是可表示的最大额外重叠数；其余宏完成页内 cacheline 编号换算。 */
#define ACTIVE_CACHELINE_MAX_OVERLAP ((1 << RADIX_TREE_MAX_TAGS) - 1)
#define CACHELINE_PER_PAGE_SHIFT (PAGE_SHIFT - L1_CACHE_SHIFT)
#define CACHELINES_PER_PAGE (1 << CACHELINE_PER_PAGE_SHIFT)

/*
 * to_cacheline_number - 把 entry 物理起始地址编码为全局 cacheline 序号
 * @entry: 已填写 paddr 的映射镜像
 *
 * 物理页号乘每页 cacheline 数，再加页内偏移右移 L1_CACHE_SHIFT 的索引。返回的
 * phys_addr_t 是序号而非字节地址；函数只读、无锁、无副作用。
 */
static phys_addr_t to_cacheline_number(struct dma_debug_entry *entry)
{
	return ((entry->paddr >> PAGE_SHIFT) << CACHELINE_PER_PAGE_SHIFT) +
		(offset_in_page(entry->paddr) >> L1_CACHE_SHIFT);
}

/*
 * active_cacheline_read_overlap - 从 radix tag 读取额外重叠映射计数
 * @cln: cacheline 序号
 *
 * 局部 @overlap 累加结果，@i 从高到低遍历 tag 位；置位 tag 被还原到对应整数位。
 * 返回 [0, MAX]。调用者必须持有 radix_lock，函数不改变树。
 */
static int active_cacheline_read_overlap(phys_addr_t cln)
{
	int overlap = 0, i;

	for (i = RADIX_TREE_MAX_TAGS - 1; i >= 0; i--)
		if (radix_tree_tag_get(&dma_active_cacheline, cln, i))
			overlap |= 1 << i;
	return overlap;
}

/*
 * active_cacheline_set_overlap - 把整数重叠计数写回 radix tag
 * @cln: 已存在或正在处理的 cacheline key
 * @overlap: 期望的额外重叠数
 *
 * 超过可表示上限或小于零时不改 tag，直接返回越界值，供调用者识别溢出/最终删除；
 * 合法时由局部 @i 逐位 set/clear tag 并返回原值。调用者必须持 radix_lock。
 */
static int active_cacheline_set_overlap(phys_addr_t cln, int overlap)
{
	int i;

	if (overlap > ACTIVE_CACHELINE_MAX_OVERLAP || overlap < 0)
		return overlap;

	for (i = RADIX_TREE_MAX_TAGS - 1; i >= 0; i--)
		if (overlap & 1 << i)
			radix_tree_tag_set(&dma_active_cacheline, cln, i);
		else
			radix_tree_tag_clear(&dma_active_cacheline, cln, i);

	return overlap;
}

/*
 * active_cacheline_inc_overlap - 为重复 key 增加一份额外映射计数
 * @cln: 发生 -EEXIST 的 cacheline 序号
 * @is_cache_clean: 新映射是否声明无需进行常规 cacheline 冲突诊断
 *
 * 局部 @overlap 读取旧值、前置递增并尝试写回。超过 tag 容量时，非 cache-clean
 * 映射触发一次性泄漏警告；clean 映射抑制该警告。无返回值，调用者持 radix_lock。
 */
static void active_cacheline_inc_overlap(phys_addr_t cln, bool is_cache_clean)
{
	int overlap = active_cacheline_read_overlap(cln);

	overlap = active_cacheline_set_overlap(cln, ++overlap);

	/* If we overflowed the overlap counter then we're potentially
	 * leaking dma-mappings.
	 */
	/* 原注释翻译：重叠计数溢出意味着 map/unmap 可能不平衡，存在 DMA 映射泄漏。 */
	WARN_ONCE(!is_cache_clean && overlap > ACTIVE_CACHELINE_MAX_OVERLAP,
		  pr_fmt("exceeded %d overlapping mappings of cacheline %pa\n"),
		  ACTIVE_CACHELINE_MAX_OVERLAP, &cln);
}

/*
 * active_cacheline_dec_overlap - 减少一份额外重叠映射计数
 * @cln: 待释放映射的 cacheline 序号
 *
 * 返回递减后的值：非负表示树中仍应保留 key，-1 表示原计数为零、当前是最后一份
 * 映射，调用者应删除 radix 节点。调用者必须持 radix_lock。
 */
static int active_cacheline_dec_overlap(phys_addr_t cln)
{
	int overlap = active_cacheline_read_overlap(cln);

	return active_cacheline_set_overlap(cln, --overlap);
}

/*
 * active_cacheline_insert - 登记映射起始物理 cacheline 并检测重叠
 * @entry: 即将发布的活动 DMA 镜像
 * @overlap_cache_clean: 输出既有重叠映射是否声明 cache-clean 属性
 *
 * @cln 是物理 cacheline key，@is_cache_clean 由 IGNORE_CACHELINES 或
 * REQUIRE_COHERENT 属性导出，@flags 保存 IRQ 状态，@rc 是 radix 插入结果。设备只
 * 读内存的 DMA_TO_DEVICE 不会让 CPU 随后读取陈旧数据，直接返回 0 且不建树。
 *
 * 其余方向在 radix_lock 下插入：成功返回 0；key 已存在时增加重叠计数，查询首个
 * entry 的属性写入输出并返回 -EEXIST；节点分配失败返回 -ENOMEM。树只借用 @entry，
 * 调用者随后仍须把它留在活动哈希中，最终以 remove 配对。
 */
static int active_cacheline_insert(struct dma_debug_entry *entry,
				   bool *overlap_cache_clean)
{
	phys_addr_t cln = to_cacheline_number(entry);
	bool is_cache_clean = entry->attrs &
			      (DMA_ATTR_DEBUGGING_IGNORE_CACHELINES |
			       DMA_ATTR_REQUIRE_COHERENT);
	unsigned long flags;
	int rc;

	*overlap_cache_clean = false;

	/* If the device is not writing memory then we don't have any
	 * concerns about the cpu consuming stale data.  This mitigates
	 * legitimate usages of overlapping mappings.
	 */
	/*
	 * 原注释翻译：设备不写内存时，CPU 不会因重叠映射读到陈旧数据；跳过跟踪也可
	 * 避免对合法的只读重叠用法产生误报。
	 */
	if (entry->direction == DMA_TO_DEVICE)
		return 0;

	spin_lock_irqsave(&radix_lock, flags);
	rc = radix_tree_insert(&dma_active_cacheline, cln, entry);
	if (rc == -EEXIST) {
		struct dma_debug_entry *existing;

		active_cacheline_inc_overlap(cln, is_cache_clean);
		existing = radix_tree_lookup(&dma_active_cacheline, cln);
		/* A lookup failure here after we got -EEXIST is unexpected. */
		/* 原注释翻译：插入报告 key 已存在后，紧接着查不到该 key 属于内部异常。 */
		WARN_ON(!existing);
		if (existing)
			*overlap_cache_clean =
				existing->attrs &
				(DMA_ATTR_DEBUGGING_IGNORE_CACHELINES |
				 DMA_ATTR_REQUIRE_COHERENT);
	}
	spin_unlock_irqrestore(&radix_lock, flags);

	return rc;
}

/*
 * active_cacheline_remove - 平衡一次映射的 cacheline 跟踪记录
 * @entry: 已从活动哈希摘除、但尚未归还 free-list 的镜像
 *
 * DMA_TO_DEVICE 与 insert 对称地无操作。其他方向在 radix_lock 下递减额外重叠数；
 * 结果小于零表示最后一份映射，删除 radix key，否则保留节点。@cln 是 key，@flags
 * 保存 IRQ 状态。无返回值，必须在 entry 内存可复用前调用。
 */
static void active_cacheline_remove(struct dma_debug_entry *entry)
{
	phys_addr_t cln = to_cacheline_number(entry);
	unsigned long flags;

	/* ...mirror the insert case */
	/* 原注释翻译：与插入时的 DMA_TO_DEVICE 旁路严格对称。 */
	if (entry->direction == DMA_TO_DEVICE)
		return;

	spin_lock_irqsave(&radix_lock, flags);
	/* since we are counting overlaps the final put of the
	 * cacheline will occur when the overlap count is 0.
	 * active_cacheline_dec_overlap() returns -1 in that case
	 */
	/*
	 * 原注释翻译：重叠计数归零时仍有首份映射；再释放一次得到 -1，才是删除 key
	 * 的最终 put。
	 */
	if (active_cacheline_dec_overlap(cln) < 0)
		radix_tree_delete(&dma_active_cacheline, cln);
	spin_unlock_irqrestore(&radix_lock, flags);
}

/*
 * Dump mappings entries on kernel space for debugging purposes
 */
/*
 * debug_dma_dump_mappings - 把当前活动 DMA 镜像输出到内核日志
 * @dev: 只显示该设备；NULL 表示显示全部设备
 *
 * @idx 遍历全部桶，@bucket/@entry 是当前桶与记录，@flags 保存 IRQ，@cln 是物理
 * cacheline 序号。每桶在自旋锁内输出一致的 entry 字段，解锁后 cond_resched()，
 * 避免扫描 16384 桶长期独占 CPU。函数可睡眠/调度，不用于原子上下文；仅观察账本，
 * 不改变 entry 生命周期。
 */
void debug_dma_dump_mappings(struct device *dev)
{
	int idx;
	phys_addr_t cln;

	for (idx = 0; idx < HASH_SIZE; idx++) {
		struct hash_bucket *bucket = &dma_entry_hash[idx];
		struct dma_debug_entry *entry;
		unsigned long flags;

		spin_lock_irqsave(&bucket->lock, flags);
		list_for_each_entry(entry, &bucket->list, list) {
			if (!dev || dev == entry->dev) {
				cln = to_cacheline_number(entry);
				dev_info(entry->dev,
					 "%s idx %d P=%pa D=%llx L=%llx cln=%pa %s %s attrs=0x%lx\n",
					 type2name[entry->type], idx,
					 &entry->paddr, entry->dev_addr,
					 entry->size, &cln,
					 dir2name[entry->direction],
					 maperr2str[entry->map_err_type],
					 entry->attrs);
			}
		}
		spin_unlock_irqrestore(&bucket->lock, flags);

		cond_resched();
	}
}

/*
 * Dump mappings entries on user space via debugfs
 */
/*
 * dump_show - 为 debugfs dump 文件生成活动映射快照文本
 * @seq: seq_file 输出目标
 * @v: single_open/show 约定参数，本实现不使用
 *
 * 与内核日志版本类似地逐桶加锁，@idx/@bucket/@entry/@flags/@cln 含义相同；每条
 * 输出驱动、设备、类型、物理/DMA 地址、长度、cacheline、方向、错误检查状态与
 * attrs。返回 0。不同桶之间状态可变化，因此结果是逐桶一致而非全表原子快照。
 */
static int dump_show(struct seq_file *seq, void *v)
{
	int idx;
	phys_addr_t cln;

	for (idx = 0; idx < HASH_SIZE; idx++) {
		struct hash_bucket *bucket = &dma_entry_hash[idx];
		struct dma_debug_entry *entry;
		unsigned long flags;

		spin_lock_irqsave(&bucket->lock, flags);
		list_for_each_entry(entry, &bucket->list, list) {
			cln = to_cacheline_number(entry);
			seq_printf(seq,
				   "%s %s %s idx %d P=%pa D=%llx L=%llx cln=%pa %s %s attrs=0x%lx\n",
				   dev_driver_string(entry->dev),
				   dev_name(entry->dev),
				   type2name[entry->type], idx,
				   &entry->paddr, entry->dev_addr,
				   entry->size, &cln,
				   dir2name[entry->direction],
				   maperr2str[entry->map_err_type],
				   entry->attrs);
		}
		spin_unlock_irqrestore(&bucket->lock, flags);
	}
	return 0;
}

/* 生成 dump_open/read/llseek/release 文件操作表，最终由 dma_debug_fs_init() 发布。 */
DEFINE_SHOW_ATTRIBUTE(dump);

/*
 * Wrapper function for adding an entry to the hash.
 * This function takes care of locking itself.
 */
/*
 * add_dma_entry - 发布完整 entry 到地址哈希与 cacheline 账本
 * @entry: 调用者刚从对象池取得并填完字段的 entry
 *
 * 先锁定地址桶追加 entry，再解锁并登记物理 cacheline。@attrs 保存属性快照，
 * @overlap_cache_clean 接收旧映射属性，@bucket/@flags 管理桶锁，@rc 区分 radix
 * 成功、重叠和内存不足。radix -ENOMEM 会永久置 global_disable，但已入哈希的 entry
 * 不回滚，因为调试失败绝不能撤销真实 DMA 操作。
 *
 * -EEXIST 仅在新旧映射并非双方都声明 cache-clean、未跳过 CPU sync、体系结构真实
 * cacheline 不小于 L1，且不是已知 SWIOTLB 非对齐 bounce 情形时报告重叠错误。
 * 无返回值；发布后对象 ownership 转给调试账本，配对 unmap/free 才能归还。
 */
static void add_dma_entry(struct dma_debug_entry *entry)
{
	unsigned long attrs = entry->attrs;
	bool overlap_cache_clean;
	struct hash_bucket *bucket;
	unsigned long flags;
	int rc;

	bucket = get_hash_bucket(entry, &flags);
	hash_bucket_add(bucket, entry);
	put_hash_bucket(bucket, flags);

	rc = active_cacheline_insert(entry, &overlap_cache_clean);
	if (rc == -ENOMEM) {
		pr_err_once("cacheline tracking ENOMEM, dma-debug disabled\n");
		global_disable = true;
	} else if (rc == -EEXIST && !(attrs & DMA_ATTR_SKIP_CPU_SYNC) &&
		   !(attrs & (DMA_ATTR_DEBUGGING_IGNORE_CACHELINES |
			      DMA_ATTR_REQUIRE_COHERENT) &&
		     overlap_cache_clean) &&
		   dma_get_cache_alignment() >= L1_CACHE_BYTES &&
		   !(IS_ENABLED(CONFIG_DMA_BOUNCE_UNALIGNED_KMALLOC) &&
		     is_swiotlb_active(entry->dev))) {
		err_printk(entry->dev, entry,
			"cacheline tracking EEXIST, overlapping mappings aren't supported\n");
	}
}

/*
 * dma_debug_create_entries - 分配一页并把其中所有 entry 加入空闲池
 * @gfp: 页分配上下文；启动预分配用 GFP_KERNEL，原子扩容用 GFP_ATOMIC
 *
 * @entry 是整页基址，@i 遍历页内对象。分配失败返回 -ENOMEM；成功把每个 list 节点
 * 追加到 free_entries，增加当前空闲数和总容量并返回 0。函数自身不加锁：运行期
 * 调用者必须持 free_entries_lock，初始化期则依赖调试器尚未发布。整页不单独回收，
 * 对象池系统期常驻。
 */
static int dma_debug_create_entries(gfp_t gfp)
{
	struct dma_debug_entry *entry;
	int i;

	entry = (void *)get_zeroed_page(gfp);
	if (!entry)
		return -ENOMEM;

	for (i = 0; i < DMA_DEBUG_DYNAMIC_ENTRIES; i++)
		list_add_tail(&entry[i].list, &free_entries);

	num_free_entries += DMA_DEBUG_DYNAMIC_ENTRIES;
	nr_total_entries += DMA_DEBUG_DYNAMIC_ENTRIES;

	return 0;
}

/*
 * __dma_entry_alloc - 从非空 free-list 取出并清零一个 entry
 *
 * 局部 @entry 由链表首节点换算，摘链后 memset 清除上一生命周期全部镜像字段；随后
 * 递减空闲数并更新最低水位。返回由调用者独占的未发布对象。调用者必须已持有
 * free_entries_lock 且保证链表非空；函数不分配内存、不睡眠。
 */
static struct dma_debug_entry *__dma_entry_alloc(void)
{
	struct dma_debug_entry *entry;

	entry = list_entry(free_entries.next, struct dma_debug_entry, list);
	list_del(&entry->list);
	memset(entry, 0, sizeof(*entry));

	num_free_entries -= 1;
	if (num_free_entries < min_free_entries)
		min_free_entries = num_free_entries;

	return entry;
}

/*
 * This should be called outside of free_entries_lock scope to avoid potential
 * deadlocks with serial consoles that use DMA.
 */
/*
 * __dma_entry_alloc_check_leak - 在锁外报告对象池按初始容量倍数增长
 * @nr_entries: 扩容后的总 entry 数
 *
 * 原注释翻译：必须在 free_entries_lock 外调用，避免日志经使用 DMA 的串口控制台
 * 重新进入本调试器形成死锁。局部 @tmp 是总容量对初始请求数的余数；当本次增长
 * 跨过一个初始池倍数附近时打印容量和百分比。该计算隐含 nr_prealloc_entries 非零；
 * 当前命令行解析未强制此约束，传 0 会在首次扩容后形成取模/除零风险。无返回值。
 */
static void __dma_entry_alloc_check_leak(u32 nr_entries)
{
	u32 tmp = nr_entries % nr_prealloc_entries;

	/* Shout each time we tick over some multiple of the initial pool */
	/* 原注释翻译：每跨过初始池容量的一个整数倍，就输出一次增长提示。 */
	if (tmp < DMA_DEBUG_DYNAMIC_ENTRIES) {
		pr_info("dma_debug_entry pool grown to %u (%u00%%)\n",
			nr_entries,
			(nr_entries / nr_prealloc_entries));
	}
}

/* struct dma_entry allocator
 *
 * The next two functions implement the allocator for
 * struct dma_debug_entries.
 */
/*
 * 原注释翻译：本节实现 dma_debug_entry 的内部对象分配器；预分配/扩容使 DMA hook
 * 可在原子上下文取得调试对象，而不会因普通 slab 睡眠改变被观测路径。
 */
/*
 * dma_entry_alloc - 取得一个调试 entry，必要时以 GFP_ATOMIC 扩容
 *
 * @alloc_check_leak 标记是否发生扩容，@entry 接收对象，@flags 保存 IRQ 状态，
 * @nr_entries 暂存扩容后容量。全程先持 free_entries_lock；空池时整页扩容，失败则
 * 置 global_disable、解锁、报错并返回 NULL。成功从链表取对象后解锁，锁外输出增长
 * 提示，CONFIG_STACKTRACE 下再保存调用栈。
 *
 * 返回独占且已清零的 entry；调用者填充后必须 add_dma_entry()，若真实 DMA 操作已
 * 成功但这里返回 NULL，只放弃本次调试记录。函数不睡眠，可从 IRQ/原子 DMA 路径调用。
 */
static struct dma_debug_entry *dma_entry_alloc(void)
{
	bool alloc_check_leak = false;
	struct dma_debug_entry *entry;
	unsigned long flags;
	u32 nr_entries;

	spin_lock_irqsave(&free_entries_lock, flags);
	if (num_free_entries == 0) {
		if (dma_debug_create_entries(GFP_ATOMIC)) {
			global_disable = true;
			spin_unlock_irqrestore(&free_entries_lock, flags);
			pr_err("debugging out of memory - disabling\n");
			return NULL;
		}
		alloc_check_leak = true;
		nr_entries = nr_total_entries;
	}

	entry = __dma_entry_alloc();

	spin_unlock_irqrestore(&free_entries_lock, flags);

	if (alloc_check_leak)
		__dma_entry_alloc_check_leak(nr_entries);

#ifdef CONFIG_STACKTRACE
	entry->stack_len = stack_trace_save(entry->stack_entries,
					    ARRAY_SIZE(entry->stack_entries),
					    1);
#endif
	return entry;
}

/*
 * dma_entry_free - 从 cacheline 账本撤销 entry 并归还对象池
 * @entry: 已从地址哈希桶摘除且不再被其他路径访问的活动对象
 *
 * 先调用 active_cacheline_remove()，避免同时持 radix_lock 与 free_entries_lock；
 * 再以 IRQ-safe free-list 锁把对象插到链表头并增加空闲数。字段此时不清零，下次
 * __dma_entry_alloc() 才统一清理。无返回值、不睡眠；调用者必须保证只归还一次。
 */
static void dma_entry_free(struct dma_debug_entry *entry)
{
	unsigned long flags;

	active_cacheline_remove(entry);

	/*
	 * add to beginning of the list - this way the entries are
	 * more likely cache hot when they are reallocated.
	 */
	/* 原注释翻译：插到链表头，使刚释放对象下次优先复用，更可能仍在 CPU cache 中。 */
	spin_lock_irqsave(&free_entries_lock, flags);
	list_add(&entry->list, &free_entries);
	num_free_entries += 1;
	spin_unlock_irqrestore(&free_entries_lock, flags);
}

/*
 * DMA-API debugging init code
 *
 * The init code does two things:
 *   1. Initialize core data structures
 *   2. Preallocate a given number of dma_debug_entry structs
 */
/*
 * 原注释翻译：初始化阶段建立核心数据结构，并预分配命令行请求数量的调试 entry。
 * 以下 debugfs 代码同时提供运行期观察和驱动过滤控制。
 */

/*
 * filter_read - 读取当前 driver_filter 名称
 * @file: debugfs 文件对象，本实现不使用
 * @user_buf: 用户输出缓冲区
 * @count: 用户缓冲区容量
 * @ppos: 文件偏移，由 simple_read_from_buffer() 更新
 *
 * 过滤名为空返回 0。@buf 是带换行的稳定副本，@flags 保存 IRQ 状态，@len 是有效
 * 长度。不能在关 IRQ 的锁内 copy_to_user，因此先持读锁复制到栈缓冲，解锁后再执行
 * 可能 fault/睡眠的 simple_read_from_buffer()。返回实际字节数或错误。
 */
static ssize_t filter_read(struct file *file, char __user *user_buf,
			   size_t count, loff_t *ppos)
{
	char buf[NAME_MAX_LEN + 1];
	unsigned long flags;
	int len;

	if (!current_driver_name[0])
		return 0;

	/*
	 * We can't copy to userspace directly because current_driver_name can
	 * only be read under the driver_name_lock with irqs disabled. So
	 * create a temporary copy first.
	 */
	/* 原注释翻译：关 IRQ 的读锁内不能直接复制到用户空间，必须先建立临时副本。 */
	read_lock_irqsave(&driver_name_lock, flags);
	len = scnprintf(buf, NAME_MAX_LEN + 1, "%s\n", current_driver_name);
	read_unlock_irqrestore(&driver_name_lock, flags);

	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

/*
 * filter_write - 设置或关闭 driver_filter
 * @file: debugfs 文件对象，本实现不使用
 * @userbuf: 用户输入；@count: 输入字节数；@ppos: 本实现不使用
 *
 * @buf 在无锁状态接收最多 NAME_MAX_LEN-1 字节，@len 是截断长度，用户复制失败返回
 * -EFAULT。随后在写锁/关 IRQ 区间内由 @i 提取第一个空白分隔 token。首字符非字母
 * 数字表示关闭过滤；否则替换名称。两种情况都清空 @current_driver 缓存，使后续
 * 错误重新按名称解析。成功返回原 @count。
 */
static ssize_t filter_write(struct file *file, const char __user *userbuf,
			    size_t count, loff_t *ppos)
{
	char buf[NAME_MAX_LEN];
	unsigned long flags;
	size_t len;
	int i;

	/*
	 * We can't copy from userspace directly. Access to
	 * current_driver_name is protected with a write_lock with irqs
	 * disabled. Since copy_from_user can fault and may sleep we
	 * need to copy to temporary buffer first
	 */
	/* 原注释翻译：用户复制可能缺页睡眠，先写临时缓冲，再进入关 IRQ 的写锁区。 */
	len = min(count, (size_t)(NAME_MAX_LEN - 1));
	if (copy_from_user(buf, userbuf, len))
		return -EFAULT;

	buf[len] = 0;

	write_lock_irqsave(&driver_name_lock, flags);

	/*
	 * Now handle the string we got from userspace very carefully.
	 * The rules are:
	 *         - only use the first token we got
	 *         - token delimiter is everything looking like a space
	 *           character (' ', '\n', '\t' ...)
	 *
	 */
	/* 原注释翻译：只采用首个 token，所有类似空格的字符均为分隔符。 */
	if (!isalnum(buf[0])) {
		/*
		 * If the first character userspace gave us is not
		 * alphanumerical then assume the filter should be
		 * switched off.
		 */
		/* 原注释翻译：首字符不是字母数字时，把本次写入解释为关闭过滤器。 */
		if (current_driver_name[0])
			pr_info("switching off dma-debug driver filter\n");
		current_driver_name[0] = 0;
		current_driver = NULL;
		goto out_unlock;
	}

	/*
	 * Now parse out the first token and use it as the name for the
	 * driver to filter for.
	 */
	/* 原注释翻译：截取首个 token，作为新的目标驱动名。 */
	for (i = 0; i < NAME_MAX_LEN - 1; ++i) {
		current_driver_name[i] = buf[i];
		if (isspace(buf[i]) || buf[i] == ' ' || buf[i] == 0)
			break;
	}
	current_driver_name[i] = 0;
	current_driver = NULL;

	pr_info("enable driver filter for driver [%s]\n",
		current_driver_name);

out_unlock:
	write_unlock_irqrestore(&driver_name_lock, flags);

	return count;
}

/* filter_fops 把 debugfs driver_filter 的读写连接到上述锁安全实现。 */
static const struct file_operations filter_fops = {
	.read  = filter_read,
	.write = filter_write,
	.llseek = default_llseek,
};

/*
 * dma_debug_fs_init - 建立 debugfs 的 dma-api 控制与观察节点
 *
 * @dentry 是目录句柄。发布禁用标志、错误统计/配额、entry 池水位、驱动过滤器和
 * dump；标量节点借用全局变量地址。debugfs 创建失败无需逐项回滚，本函数始终返回
 * 0。作为 core_initcall_sync 在可睡眠启动期执行，节点系统期存在。
 */
static int __init dma_debug_fs_init(void)
{
	struct dentry *dentry = debugfs_create_dir("dma-api", NULL);

	debugfs_create_bool("disabled", 0444, dentry, &global_disable);
	debugfs_create_u32("error_count", 0444, dentry, &error_count);
	debugfs_create_u32("all_errors", 0644, dentry, &show_all_errors);
	debugfs_create_u32("num_errors", 0644, dentry, &show_num_errors);
	debugfs_create_u32("num_free_entries", 0444, dentry, &num_free_entries);
	debugfs_create_u32("min_free_entries", 0444, dentry, &min_free_entries);
	debugfs_create_u32("nr_total_entries", 0444, dentry, &nr_total_entries);
	debugfs_create_file("driver_filter", 0644, dentry, NULL, &filter_fops);
	debugfs_create_file("dump", 0444, dentry, NULL, &dump_fops);

	return 0;
}
core_initcall_sync(dma_debug_fs_init);

/*
 * device_dma_allocations - 统计仍归属于设备的活动 DMA entry
 * @dev: 即将解绑或待检查设备
 * @out_entry: 每次命中时更新，最终输出一个泄漏样本；仅返回值大于零时有效
 *
 * @entry/@i 遍历每个桶，@flags 保护链表，@count 累计命中。逐桶加锁所得结果是
 * 诊断快照而非全表原子状态。返回命中数；输出 entry 在解锁后只是借用指针，设备
 * 解绑流程应已阻止新的驱动 DMA 操作，调试器不额外取得对象引用。
 */
static int device_dma_allocations(struct device *dev, struct dma_debug_entry **out_entry)
{
	struct dma_debug_entry *entry;
	unsigned long flags;
	int count = 0, i;

	for (i = 0; i < HASH_SIZE; ++i) {
		spin_lock_irqsave(&dma_entry_hash[i].lock, flags);
		list_for_each_entry(entry, &dma_entry_hash[i].list, list) {
			if (entry->dev == dev) {
				count += 1;
				*out_entry = entry;
			}
		}
		spin_unlock_irqrestore(&dma_entry_hash[i].lock, flags);
	}

	return count;
}

/*
 * dma_debug_device_change - 在设备驱动解绑时检查未释放 DMA 资源
 * @nb: 触发回调的 notifier_block，本实现不使用
 * @action: bus 通知事件
 * @data: 事件设备，转换为局部 @dev
 *
 * 调试禁用时直接返回 0。仅处理 BUS_NOTIFY_UNBOUND_DRIVER：@count 接收活动记录数，
 * @entry 接收一个样本；非零时报告泄漏总数及该样本地址、长度、方向和类型。其他
 * 事件忽略。函数始终返回 0，不阻止 bus 生命周期；只诊断，不自动回收真实映射。
 */
static int dma_debug_device_change(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;
	struct dma_debug_entry *entry;
	int count;

	if (dma_debug_disabled())
		return 0;

	switch (action) {
	case BUS_NOTIFY_UNBOUND_DRIVER:
		count = device_dma_allocations(dev, &entry);
		if (count == 0)
			break;
		err_printk(dev, entry, "device driver has pending "
				"DMA allocations while released from device "
				"[count=%d]\n"
				"One of leaked entries details: "
				"[device address=0x%016llx] [size=%llu bytes] "
				"[mapped with %s] [mapped as %s]\n",
			count, entry->dev_addr, entry->size,
			dir2name[entry->direction], type2name[entry->type]);
		break;
	default:
		break;
	}

	return 0;
}

/*
 * dma_debug_add_bus - 为一个 bus 注册设备解绑泄漏检查
 * @bus: 系统期 bus_type 借用指针
 *
 * 调试尚未启用或已禁用时不注册。局部 @nb 以 kzalloc_obj() 创建，失败只记录错误；
 * 成功设置回调后交给 bus notifier 链，调试器不保存指针也不注销，故对象随系统期
 * 注册常驻。bus_register_notifier() 的返回值在当前实现中被忽略；函数无返回值，
 * 在可睡眠的 bus 初始化上下文调用。
 */
void dma_debug_add_bus(const struct bus_type *bus)
{
	struct notifier_block *nb;

	if (dma_debug_disabled())
		return;

	nb = kzalloc_obj(struct notifier_block);
	if (nb == NULL) {
		pr_err("dma_debug_add_bus: out of memory\n");
		return;
	}

	nb->notifier_call = dma_debug_device_change;

	bus_register_notifier(bus, nb);
}

/*
 * dma_debug_init - 初始化哈希桶并预分配调试 entry
 *
 * @i 遍历 HASH_SIZE 个桶和预分配页，@nr_pages 是请求 entry 数向上换算的整页数。
 * 命令行已禁用时返回 0 且不置 initialized。否则先初始化每桶链表/锁，再以
 * GFP_KERNEL 尽量分配各页；单页失败不中断循环。达到请求量打印成功，部分成功打印
 * 警告并继续启用，完全失败置 global_disable 后返回 0。请求数为 0 时，0>=0 会被
 * 当作预分配达标并以空池启用，后续扩容将触发上述零除边界。
 *
 * 成功时设置最低水位和 dma_debug_initialized，之后各 DMA hook 才开始记账。作为
 * core_initcall 在单线程、可睡眠启动期执行；返回值始终为 0，调试器失败不阻断启动。
 */
static int dma_debug_init(void)
{
	int i, nr_pages;

	/* Do not use dma_debug_initialized here, since we really want to be
	 * called to set dma_debug_initialized
	 */
	/*
	 * 原注释翻译：这里不能用 dma_debug_disabled()（其中包含 initialized 判断），
	 * 因为本函数的职责正是从未初始化状态推进到 initialized。
	 */
	if (global_disable)
		return 0;

	for (i = 0; i < HASH_SIZE; ++i) {
		INIT_LIST_HEAD(&dma_entry_hash[i].list);
		spin_lock_init(&dma_entry_hash[i].lock);
	}

	nr_pages = DIV_ROUND_UP(nr_prealloc_entries, DMA_DEBUG_DYNAMIC_ENTRIES);
	for (i = 0; i < nr_pages; ++i)
		dma_debug_create_entries(GFP_KERNEL);
	if (num_free_entries >= nr_prealloc_entries) {
		pr_info("preallocated %d debug entries\n", nr_total_entries);
	} else if (num_free_entries > 0) {
		pr_warn("%d debug entries requested but only %d allocated\n",
			nr_prealloc_entries, nr_total_entries);
	} else {
		pr_err("debugging out of memory error - disabled\n");
		global_disable = true;

		return 0;
	}
	min_free_entries = num_free_entries;

	dma_debug_initialized = true;

	pr_info("debugging enabled by kernel config\n");
	return 0;
}
core_initcall(dma_debug_init);

/*
 * dma_debug_cmdline - 解析 dma_debug= 启停参数
 * @str: 参数值；NULL 返回 -EINVAL
 *
 * 前三个字符为 "off" 时置 global_disable 并记录日志；其他值不改变状态。返回 1
 * 表示参数已被 __setup 消费。由于采用 strncmp(..., 3)，任何以 off 开头的值都会禁用。
 */
static __init int dma_debug_cmdline(char *str)
{
	if (!str)
		return -EINVAL;

	if (strncmp(str, "off", 3) == 0) {
		pr_info("debugging disabled on kernel command line\n");
		global_disable = true;
	}

	return 1;
}

/*
 * dma_debug_entries_cmdline - 解析 dma_debug_entries= 预分配数量
 * @str: 数字参数及解析游标；NULL 返回 -EINVAL
 *
 * get_option() 成功时写入全局请求数；解析失败恢复 PREALLOC 默认值。返回 1 表示参数
 * 已消费。只修改启动期容量，不立即分配；当前实现接受数值 0，它会导致空池启用并
 * 在首次动态增长提示中触发除零风险，配置时应保持正数。
 */
static __init int dma_debug_entries_cmdline(char *str)
{
	if (!str)
		return -EINVAL;
	if (!get_option(&str, &nr_prealloc_entries))
		nr_prealloc_entries = PREALLOC_DMA_DEBUG_ENTRIES;
	return 1;
}

/* 两个 __setup 声明把早期参数分别连接到启停与容量解析函数。 */
__setup("dma_debug=", dma_debug_cmdline);
__setup("dma_debug_entries=", dma_debug_entries_cmdline);

/*
 * check_unmap - 校验 unmap/free 参数并消费对应活动 entry
 * @ref: 由具体 debug free/unmap hook 在栈上构造的查询镜像
 *
 * @bucket/@flags 管理 ref 地址对应的桶锁，@entry 是 exact/best-fit 结果。未找到时先
 * 解锁，再调用 dma_mapping_error() 区分“错误 DMA 地址”和“未记录的有效地址”；该
 * API 可能回调调试器，持锁调用会自死锁。找到后在锁内依次核对 size、API type、
 * coherent/noncoherent CPU 物理地址、SG 原始项数、direction、map-error 检查状态和
 * 必须配对一致的 attrs，所有差异只报告，不阻止后续摘除。
 *
 * 最终从桶链表删除并释放桶锁，再调用 dma_entry_free() 撤销 radix 记录并归还对象。
 * 锁外释放避免 bucket_lock 与 radix_lock 的 ABBA。函数无返回值；一次真实 unmap
 * 只消费一条最可信记录，即使调用参数存在错误，也不会自动影响真实 DMA 资源。
 */
static void check_unmap(struct dma_debug_entry *ref)
{
	struct dma_debug_entry *entry;
	struct hash_bucket *bucket;
	unsigned long flags;

	bucket = get_hash_bucket(ref, &flags);
	entry = bucket_find_exact(bucket, ref);

	if (!entry) {
		/* must drop lock before calling dma_mapping_error */
		/* 原注释翻译：调用 dma_mapping_error() 前必须释放桶锁，防止调试回调重入死锁。 */
		put_hash_bucket(bucket, flags);

		if (dma_mapping_error(ref->dev, ref->dev_addr)) {
			err_printk(ref->dev, NULL,
				   "device driver tries to free an "
				   "invalid DMA memory address\n");
		} else {
			err_printk(ref->dev, NULL,
				   "device driver tries to free DMA "
				   "memory it has not allocated [device "
				   "address=0x%016llx] [size=%llu bytes]\n",
				   ref->dev_addr, ref->size);
		}
		return;
	}

	if (ref->size != entry->size) {
		err_printk(ref->dev, entry, "device driver frees "
			   "DMA memory with different size "
			   "[device address=0x%016llx] [map size=%llu bytes] "
			   "[unmap size=%llu bytes]\n",
			   ref->dev_addr, entry->size, ref->size);
	}

	if (ref->type != entry->type) {
		err_printk(ref->dev, entry, "device driver frees "
			   "DMA memory with wrong function "
			   "[device address=0x%016llx] [size=%llu bytes] "
			   "[mapped as %s] [unmapped as %s]\n",
			   ref->dev_addr, ref->size,
			   type2name[entry->type], type2name[ref->type]);
	} else if ((entry->type == dma_debug_coherent ||
		    entry->type == dma_debug_noncoherent) &&
		   ref->paddr != entry->paddr) {
		err_printk(ref->dev, entry, "device driver frees "
			   "DMA memory with different CPU address "
			   "[device address=0x%016llx] [size=%llu bytes] "
			   "[cpu alloc address=0x%pa] "
			   "[cpu free address=0x%pa]",
			   ref->dev_addr, ref->size,
			   &entry->paddr,
			   &ref->paddr);
	}

	if (ref->sg_call_ents && ref->type == dma_debug_sg &&
	    ref->sg_call_ents != entry->sg_call_ents) {
		err_printk(ref->dev, entry, "device driver frees "
			   "DMA sg list with different entry count "
			   "[map count=%d] [unmap count=%d]\n",
			   entry->sg_call_ents, ref->sg_call_ents);
	}

	/*
	 * This may be no bug in reality - but most implementations of the
	 * DMA API don't handle this properly, so check for it here
	 */
	/*
	 * 原注释翻译：方向不一致未必在所有硬件上立刻出错，但多数 DMA API 实现不能
	 * 正确处理这种配对，因此调试器仍把它作为错误检查。
	 */
	if (ref->direction != entry->direction) {
		err_printk(ref->dev, entry, "device driver frees "
			   "DMA memory with different direction "
			   "[device address=0x%016llx] [size=%llu bytes] "
			   "[mapped with %s] [unmapped with %s]\n",
			   ref->dev_addr, ref->size,
			   dir2name[entry->direction],
			   dir2name[ref->direction]);
	}

	/*
	 * Drivers should use dma_mapping_error() to check the returned
	 * addresses of dma_map_single() and dma_map_page().
	 * If not, print this warning message. See Documentation/core-api/dma-api.rst.
	 */
	/*
	 * 原注释翻译：驱动必须用 dma_mapping_error() 检查 dma_map_single/page 的返回
	 * 地址；仍处于 NOT_CHECKED 说明在 unmap 前从未检查，按 DMA API 文档告警。
	 */
	if (entry->map_err_type == MAP_ERR_NOT_CHECKED) {
		err_printk(ref->dev, entry,
			   "device driver failed to check map error"
			   "[device address=0x%016llx] [size=%llu bytes] "
			   "[mapped as %s]",
			   ref->dev_addr, ref->size,
			   type2name[entry->type]);
	}

	/*
	 * This may be no bug in reality - but DMA API still expects
	 * that entry is unmapped with same attributes as it was mapped.
	 *
	 * DMA_ATTR_UNMAP_VALID lists the attributes that must be identical
	 * between map and unmap. Any attribute outside this set (e.g.
	 * DMA_ATTR_NO_WARN, DMA_ATTR_SKIP_CPU_SYNC) is allowed to differ.
	 */
	/*
	 * 原注释翻译：DMA API 要求 map/unmap 的关键属性一致。DMA_ATTR_UNMAP_VALID 只列
	 * 必须相同的集合；NO_WARN、SKIP_CPU_SYNC 等集合外属性允许不同，不参与比较。
	 */
#define DMA_ATTR_UNMAP_VALID                                               \
	(DMA_ATTR_NO_KERNEL_MAPPING | DMA_ATTR_FORCE_CONTIGUOUS |          \
	 DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT | DMA_ATTR_PRIVILEGED | \
	 DMA_ATTR_CC_SHARED)
	if ((ref->attrs & DMA_ATTR_UNMAP_VALID) !=
	    (entry->attrs & DMA_ATTR_UNMAP_VALID)) {
		err_printk(ref->dev, entry,
			   "device driver frees "
			   "DMA memory with different attributes "
			   "[device address=0x%016llx] [size=%llu bytes] "
			   "[mapped with 0x%lx] [unmapped with 0x%lx]\n",
			   ref->dev_addr, ref->size, entry->attrs, ref->attrs);
	}
#undef DMA_ATTR_UNMAP_VALID

	hash_bucket_del(entry);
	put_hash_bucket(bucket, flags);

	/*
	 * Free the entry outside of bucket_lock to avoid ABBA deadlocks
	 * between that and radix_lock.
	 */
	/* 原注释翻译：在桶锁外释放 entry，避免它与 radix_lock 构成 ABBA 死锁。 */
	dma_entry_free(entry);
}

/*
 * check_for_stack - 检测物理地址是否来自当前任务内核栈
 * @dev: 发起映射的设备
 * @phys: 待映射物理地址
 *
 * @stack_vm_area 描述 CONFIG_VMAP_STACK 下当前栈；@addr 是推导出的内核地址。直接
 * 映射栈路径先排除 highmem，再 phys_to_virt() 并用 object_is_on_stack() 判断。
 * vmalloc 栈路径由局部 @i 遍历 backing pages，比对 PFN 后组合当前栈基址、页索引和
 * 页内偏移，报告“可能地址”。只检查 current 的栈，命中只告警、不阻止 DMA 映射。
 */
static void check_for_stack(struct device *dev, phys_addr_t phys)
{
	void *addr;
	struct vm_struct *stack_vm_area = task_stack_vm_area(current);

	if (!stack_vm_area) {
		/* Stack is direct-mapped. */
		/* 原注释翻译：当前任务栈位于内核线性直接映射。 */
		if (PhysHighMem(phys))
			return;
		addr = phys_to_virt(phys);
		if (object_is_on_stack(addr))
			err_printk(dev, NULL, "device driver maps memory from stack [addr=%p]\n", addr);
	} else {
		/* Stack is vmalloced. */
		/* 原注释翻译：当前任务栈通过 vmalloc/vmap 组织，需要逐页反查物理 PFN。 */
		int i;

		for (i = 0; i < stack_vm_area->nr_pages; i++) {
			if (__phys_to_pfn(phys) !=
			    page_to_pfn(stack_vm_area->pages[i]))
				continue;

			addr = (u8 *)current->stack + i * PAGE_SIZE +
			       (phys % PAGE_SIZE);
			err_printk(dev, NULL, "device driver maps memory from stack [probable addr=%p]\n", addr);
			break;
		}
	}
}

/*
 * check_for_illegal_area - 检测 CPU 虚拟区间是否覆盖内核代码或只读数据
 * @dev: 发起映射的设备
 * @addr: 待 DMA 映射的 CPU 起始地址
 * @len: 区间字节数
 *
 * 与 [_stext,_etext) 或 [__start_rodata,__end_rodata) 相交即报告错误。无返回值，
 * 只诊断，不改变映射；调用方只对能安全转换为普通虚拟地址的内存使用它。
 */
static void check_for_illegal_area(struct device *dev, void *addr, unsigned long len)
{
	if (memory_intersects(_stext, _etext, addr, len) ||
	    memory_intersects(__start_rodata, __end_rodata, addr, len))
		err_printk(dev, NULL, "device driver maps memory from kernel text or rodata [addr=%p] [len=%lu]\n", addr, len);
}

/*
 * check_sync - 校验一次 DMA sync 是否落在已记录映射内且方向/SG 参数匹配
 * @dev: 执行 sync 的设备
 * @ref: sync 子区间查询镜像
 * @to_cpu: true 表示 sync_for_cpu，false 表示 sync_for_device
 *
 * @bucket/@flags 从 ref 起始桶开始并可能被 bucket_find_contain() 更新，@entry 是包含
 * 子区间的活动映射。未找到则报告未分配地址并统一解锁。命中后检查范围、direction
 * 和 SG 原始项数；原映射为 BIDIRECTIONAL 时方向语义检查可直接跳过。针对 CPU 与
 * device 的两个条件分别诊断与原 map ownership 不相容的同步方向。
 *
 * 所有路径都在 out 释放最终持有的桶锁；函数只读活动 entry，不消费记录，也不执行
 * 真实 cache 同步。调用者可在原子上下文使用，但日志路径可能较重。
 */
static void check_sync(struct device *dev,
		       struct dma_debug_entry *ref,
		       bool to_cpu)
{
	struct dma_debug_entry *entry;
	struct hash_bucket *bucket;
	unsigned long flags;

	bucket = get_hash_bucket(ref, &flags);

	entry = bucket_find_contain(&bucket, ref, &flags);

	if (!entry) {
		err_printk(dev, NULL, "device driver tries "
				"to sync DMA memory it has not allocated "
				"[device address=0x%016llx] [size=%llu bytes]\n",
				(unsigned long long)ref->dev_addr, ref->size);
		goto out;
	}

	if (ref->size > entry->size) {
		err_printk(dev, entry, "device driver syncs"
				" DMA memory outside allocated range "
				"[device address=0x%016llx] "
				"[allocation size=%llu bytes] "
				"[sync offset+size=%llu]\n",
				entry->dev_addr, entry->size,
				ref->size);
	}

	if (entry->direction == DMA_BIDIRECTIONAL)
		goto out;

	if (ref->direction != entry->direction) {
		err_printk(dev, entry, "device driver syncs "
				"DMA memory with different direction "
				"[device address=0x%016llx] [size=%llu bytes] "
				"[mapped with %s] [synced with %s]\n",
				(unsigned long long)ref->dev_addr, entry->size,
				dir2name[entry->direction],
				dir2name[ref->direction]);
	}

	if (to_cpu && !(entry->direction == DMA_FROM_DEVICE) &&
		      !(ref->direction == DMA_TO_DEVICE))
		err_printk(dev, entry, "device driver syncs "
				"device read-only DMA memory for cpu "
				"[device address=0x%016llx] [size=%llu bytes] "
				"[mapped with %s] [synced with %s]\n",
				(unsigned long long)ref->dev_addr, entry->size,
				dir2name[entry->direction],
				dir2name[ref->direction]);

	if (!to_cpu && !(entry->direction == DMA_TO_DEVICE) &&
		       !(ref->direction == DMA_FROM_DEVICE))
		err_printk(dev, entry, "device driver syncs "
				"device write-only DMA memory to device "
				"[device address=0x%016llx] [size=%llu bytes] "
				"[mapped with %s] [synced with %s]\n",
				(unsigned long long)ref->dev_addr, entry->size,
				dir2name[entry->direction],
				dir2name[ref->direction]);

	if (ref->sg_call_ents && ref->type == dma_debug_sg &&
	    ref->sg_call_ents != entry->sg_call_ents) {
		err_printk(ref->dev, entry, "device driver syncs "
			   "DMA sg list with different entry count "
			   "[map count=%d] [sync count=%d]\n",
			   entry->sg_call_ents, ref->sg_call_ents);
	}

out:
	put_hash_bucket(bucket, flags);
}

/*
 * check_sg_segment - 校验一个已映射 SG 段的设备长度与边界约束
 * @dev: DMA 设备，其 dma_parms 提供最大段长和边界掩码
 * @sg: 已具有 DMA address/len 的 SG 项
 *
 * @max_seg 是设备声明的最大段长；@start/@end 是闭区间 DMA 地址，@boundary 是不得
 * 跨越的边界掩码。原始 sg->length 超过 max_seg 时报告；映射后 [start,end] 跨边界
 * 也报告。函数假定 DMA 长度非零且端点加法不溢出，只诊断不修改 SG。
 */
static void check_sg_segment(struct device *dev, struct scatterlist *sg)
{
	unsigned int max_seg = dma_get_max_seg_size(dev);
	u64 start, end, boundary = dma_get_seg_boundary(dev);

	/*
	 * Either the driver forgot to set dma_parms appropriately, or
	 * whoever generated the list forgot to check them.
	 */
	/* 原注释翻译：可能是驱动未正确设置 dma_parms，或 SG 构造者忘了检查设备限制。 */
	if (sg->length > max_seg)
		err_printk(dev, NULL, "mapping sg segment longer than device claims to support [len=%u] [max=%u]\n",
			   sg->length, max_seg);
	/*
	 * In some cases this could potentially be the DMA API
	 * implementation's fault, but it would usually imply that
	 * the scatterlist was built inappropriately to begin with.
	 */
	/*
	 * 原注释翻译：少数情况下跨边界可能是 DMA 后端问题，但通常说明 scatterlist
	 * 在构建之初就没有遵守设备边界约束。
	 */
	start = sg_dma_address(sg);
	end = start + sg_dma_len(sg) - 1;
	if ((start ^ end) & ~boundary)
		err_printk(dev, NULL, "mapping sg segment across boundary [start=0x%016llx] [end=0x%016llx] [boundary=0x%016llx]\n",
			   start, end, boundary);
}

/*
 * debug_dma_map_single - 在执行 single map 前检查 CPU 地址类型
 * @dev: DMA 设备
 * @addr: 驱动提交的 CPU 虚拟起始地址
 * @len: 映射字节数
 *
 * 调试不可用时旁路。virt_addr_valid() 失败或地址属于 vmalloc 区分别报告，因为传统
 * dma_map_single() 需要可直接转换为页面/物理地址的线性映射内存。无返回值，只做
 * 前置诊断；实际映射及其 entry 由后续 debug_dma_map_phys() 记录。
 */
void debug_dma_map_single(struct device *dev, const void *addr,
			    unsigned long len)
{
	if (unlikely(dma_debug_disabled()))
		return;

	if (!virt_addr_valid(addr))
		err_printk(dev, NULL, "device driver maps memory from invalid area [addr=%p] [len=%lu]\n",
			   addr, len);

	if (is_vmalloc_addr(addr))
		err_printk(dev, NULL, "device driver maps memory from vmalloc area [addr=%p] [len=%lu]\n",
			   addr, len);
}

/* 导出前置地址检查 hook，供内核其他编译单元或模块化 DMA 调用路径使用。 */
EXPORT_SYMBOL(debug_dma_map_single);

/*
 * debug_dma_map_phys - 为成功的物理 DMA 映射建立活动调试 entry
 * @dev: DMA 设备；@phys: CPU 物理起始地址；@size: 映射字节数
 * @direction: DMA 数据方向；@dma_addr: 后端返回的设备地址；@attrs: 映射属性
 *
 * 先旁路禁用状态和实际映射错误，再由 @entry 取得原子对象并填写 phy 类型、双地址、
 * 长度、方向、attrs，且将 map_err_type 设为 NOT_CHECKED，要求驱动自己调用公开的
 * dma_mapping_error()。内部成功判定不等同于驱动完成检查。
 *
 * MMIO 属性下局部 @pfn 若指向有效且非 Reserved 的 RAM，就诊断误用
 * dma_map_resource()；普通内存则检查当前栈，非 highmem 还检查内核 text/rodata。
 * 最后 add_dma_entry() 转移调试对象 ownership。失败只漏记，不改变真实映射。
 */
void debug_dma_map_phys(struct device *dev, phys_addr_t phys, size_t size,
		int direction, dma_addr_t dma_addr, unsigned long attrs)
{
	struct dma_debug_entry *entry;

	if (unlikely(dma_debug_disabled()))
		return;

	if (dma_mapping_error(dev, dma_addr))
		return;

	entry = dma_entry_alloc();
	if (!entry)
		return;

	entry->dev       = dev;
	entry->type      = dma_debug_phy;
	entry->paddr	 = phys;
	entry->dev_addr  = dma_addr;
	entry->size      = size;
	entry->direction = direction;
	entry->map_err_type = MAP_ERR_NOT_CHECKED;
	entry->attrs     = attrs;

	if (attrs & DMA_ATTR_MMIO) {
		unsigned long pfn = PHYS_PFN(phys);

		if (pfn_valid(pfn) && !PageReserved(pfn_to_page(pfn)))
			err_printk(dev, entry,
				   "dma_map_resource called for RAM address %pa\n",
				   &phys);
	} else {
		check_for_stack(dev, phys);

		if (!PhysHighMem(phys))
			check_for_illegal_area(dev, phys_to_virt(phys), size);
	}

	add_dma_entry(entry);
}

/*
 * debug_dma_mapping_error - 标记驱动已检查一个 DMA 映射返回值
 * @dev: 被检查映射的设备
 * @dma_addr: 驱动传给 dma_mapping_error() 的设备地址
 *
 * @ref 只需主键字段，@bucket/@flags 保护地址桶，@entry 遍历 exact_match 候选。相同
 * 物理映射可能产生重复 DMA 地址，因此找到第一条仍为 NOT_CHECKED 的 entry 后改为
 * CHECKED 并停止；若无记录则无操作。函数不消费 entry，不判断真实错误结果。
 */
void debug_dma_mapping_error(struct device *dev, dma_addr_t dma_addr)
{
	struct dma_debug_entry ref;
	struct dma_debug_entry *entry;
	struct hash_bucket *bucket;
	unsigned long flags;

	if (unlikely(dma_debug_disabled()))
		return;

	ref.dev = dev;
	ref.dev_addr = dma_addr;
	bucket = get_hash_bucket(&ref, &flags);

	list_for_each_entry(entry, &bucket->list, list) {
		if (!exact_match(&ref, entry))
			continue;

		/*
		 * The same physical address can be mapped multiple
		 * times. Without a hardware IOMMU this results in the
		 * same device addresses being put into the dma-debug
		 * hash multiple times too. This can result in false
		 * positives being reported. Therefore we implement a
		 * best-fit algorithm here which updates the first entry
		 * from the hash which fits the reference value and is
		 * not currently listed as being checked.
		 */
		/*
		 * 原注释翻译：重复映射可能共享设备地址；为避免误把同一次检查应用到所有记录，
		 * 只更新第一个主键吻合且尚未标记 CHECKED 的 entry。
		 */
		if (entry->map_err_type == MAP_ERR_NOT_CHECKED) {
			entry->map_err_type = MAP_ERR_CHECKED;
			break;
		}
	}

	put_hash_bucket(bucket, flags);
}

/* 导出错误检查状态 hook，使公开 dma_mapping_error() 路径可更新调试镜像。 */
EXPORT_SYMBOL(debug_dma_mapping_error);

/*
 * debug_dma_unmap_phys - 校验并删除一条物理 DMA 映射记录
 * @dev: DMA 设备；@dma_addr: 待解除设备地址；@size: 解除长度
 * @direction: unmap 方向；@attrs: unmap 属性
 *
 * 栈上 @ref 填写 phy 类型和配对字段。调试禁用时旁路，否则交给 check_unmap() 完成
 * 最佳匹配、参数诊断、哈希/radix 删除和对象归还。无返回值，不执行真实 unmap。
 */
void debug_dma_unmap_phys(struct device *dev, dma_addr_t dma_addr, size_t size,
			  int direction, unsigned long attrs)
{
	struct dma_debug_entry ref = {
		.type           = dma_debug_phy,
		.dev            = dev,
		.dev_addr       = dma_addr,
		.size           = size,
		.direction      = direction,
		.attrs          = attrs,
	};

	if (unlikely(dma_debug_disabled()))
		return;
	check_unmap(&ref);
}

/*
 * debug_dma_map_sg - 检查 SG CPU backing 并记录后端返回的每个 DMA 段
 * @dev: DMA 设备；@sg: scatterlist 首项
 * @nents: 驱动输入项数；@mapped_ents: dma_map_sg() 返回的 DMA 段数
 * @direction: DMA 方向；@attrs: 映射属性
 *
 * @s/@i 遍历 SG，@entry 是每个 mapped 段的新调试对象。第一轮按 @nents 检查每个
 * 原始页面是否来自当前栈；非 highmem 还检查 text/rodata。第二轮按 @mapped_ents
 * 为具有有效 DMA 字段的段逐一填写 sg 类型、物理/DMA 地址、长度、两个项数、方向
 * 与属性，并通过 check_sg_segment() 校验设备限制后发布。
 *
 * 任一 entry 分配失败立即停止；分配器同时全局禁用调试，使后续 hook 旁路。函数
 * 只建立镜像，不拥有 SG/page，也不改变真实映射。
 */
void debug_dma_map_sg(struct device *dev, struct scatterlist *sg,
		      int nents, int mapped_ents, int direction,
		      unsigned long attrs)
{
	struct dma_debug_entry *entry;
	struct scatterlist *s;
	int i;

	if (unlikely(dma_debug_disabled()))
		return;

	for_each_sg(sg, s, nents, i) {
		check_for_stack(dev, sg_phys(s));
		if (!PageHighMem(sg_page(s)))
			check_for_illegal_area(dev, sg_virt(s), s->length);
	}

	for_each_sg(sg, s, mapped_ents, i) {
		entry = dma_entry_alloc();
		if (!entry)
			return;

		entry->type           = dma_debug_sg;
		entry->dev            = dev;
		entry->paddr	      = sg_phys(s);
		entry->size           = sg_dma_len(s);
		entry->dev_addr       = sg_dma_address(s);
		entry->direction      = direction;
		entry->sg_call_ents   = nents;
		entry->sg_mapped_ents = mapped_ents;
		entry->attrs          = attrs;

		check_sg_segment(dev, s);

		add_dma_entry(entry);
	}
}

/*
 * get_nr_mapped_entries - 从 SG 首条调试记录取回后端 mapped_ents
 * @dev: SG 所属设备（信息已包含在 @ref 中，形参本身不直接使用）
 * @ref: 第一项 SG 查询镜像
 *
 * @bucket/@flags 锁定精确地址桶，@entry 是匹配记录，@mapped_ents 默认 0；命中时
 * 读取创建阶段保存的 sg_mapped_ents。解锁后返回段数，0 表示无记录。
 */
static int get_nr_mapped_entries(struct device *dev,
				 struct dma_debug_entry *ref)
{
	struct dma_debug_entry *entry;
	struct hash_bucket *bucket;
	unsigned long flags;
	int mapped_ents;

	bucket       = get_hash_bucket(ref, &flags);
	entry        = bucket_find_exact(bucket, ref);
	mapped_ents  = 0;

	if (entry)
		mapped_ents = entry->sg_mapped_ents;
	put_hash_bucket(bucket, flags);

	return mapped_ents;
}

/*
 * debug_dma_unmap_sg - 校验并消费一次 SG 映射的各段调试记录
 * @dev: DMA 设备；@sglist: 原 scatterlist；@nelems: unmap 传入的原始项数
 * @dir: unmap 方向；@attrs: unmap 属性
 *
 * @s/@i 遍历原列表，@mapped_ents 初始为 0。每轮构造栈上 @ref；首项通过调试账本
 * 恢复 map 时后端返回段数，若非零则只处理 [0,mapped_ents)，避免读取合并后无效的
 * DMA 字段。找不到首项时 mapped_ents 保持 0，循环会让 check_unmap() 对各原始项
 * 报告未记录。每次 check_unmap() 诊断参数并消费一条 entry；不修改真实 SG。
 */
void debug_dma_unmap_sg(struct device *dev, struct scatterlist *sglist,
			int nelems, int dir, unsigned long attrs)
{
	struct scatterlist *s;
	int mapped_ents = 0, i;

	if (unlikely(dma_debug_disabled()))
		return;

	for_each_sg(sglist, s, nelems, i) {

		struct dma_debug_entry ref = {
			.type           = dma_debug_sg,
			.dev            = dev,
			.paddr		= sg_phys(s),
			.dev_addr       = sg_dma_address(s),
			.size           = sg_dma_len(s),
			.direction      = dir,
			.sg_call_ents   = nelems,
			.attrs          = attrs,
		};

		if (mapped_ents && i >= mapped_ents)
			break;

		if (!i)
			mapped_ents = get_nr_mapped_entries(dev, &ref);

		check_unmap(&ref);
	}
}

/*
 * virt_to_paddr - 把已验证的内核虚拟地址转换为精确物理地址
 * @virt: vmalloc/vmap 或线性映射地址
 *
 * 局部 @page 由 vmalloc_to_page() 或 virt_to_page() 取得，再以 page_to_phys() 加页内
 * 偏移返回物理地址。返回页是借用关系；调用方必须先保证地址有效且有 backing page。
 */
static phys_addr_t virt_to_paddr(void *virt)
{
	struct page *page;

	if (is_vmalloc_addr(virt))
		page = vmalloc_to_page(virt);
	else
		page = virt_to_page(virt);

	return page_to_phys(page) + offset_in_page(virt);
}

/*
 * debug_dma_alloc_coherent - 记录一次成功的 coherent 分配
 * @dev: 分配设备；@size: 字节数；@dma_addr: 设备地址
 * @virt: CPU 虚拟地址；@attrs: 分配属性
 *
 * 禁用、NULL 返回值或既非 vmalloc 也非有效线性地址时旁路。随后取得 @entry，填写
 * coherent 类型、设备、由 virt_to_paddr() 得到的物理地址、长度、DMA 地址、
 * BIDIRECTIONAL 方向和属性并发布。map_err_type 保持清零后的 NOT_APPLICABLE。
 * 调试对象只镜像真实分配，分配失败时不会创建；本函数不拥有缓冲区。
 */
void debug_dma_alloc_coherent(struct device *dev, size_t size,
			      dma_addr_t dma_addr, void *virt,
			      unsigned long attrs)
{
	struct dma_debug_entry *entry;

	if (unlikely(dma_debug_disabled()))
		return;

	if (unlikely(virt == NULL))
		return;

	/* handle vmalloc and linear addresses */
	/* 原注释翻译：仅处理具有有效 backing page 的 vmalloc 或线性映射地址。 */
	if (!is_vmalloc_addr(virt) && !virt_addr_valid(virt))
		return;

	entry = dma_entry_alloc();
	if (!entry)
		return;

	entry->type      = dma_debug_coherent;
	entry->dev       = dev;
	entry->paddr	 = virt_to_paddr(virt);
	entry->size      = size;
	entry->dev_addr  = dma_addr;
	entry->direction = DMA_BIDIRECTIONAL;
	entry->attrs     = attrs;

	add_dma_entry(entry);
}

/*
 * debug_dma_free_coherent - 校验并消费 coherent 分配记录
 * @dev: 原设备；@size: free 字节数；@virt: CPU 地址
 * @dma_addr: 设备地址；@attrs: free 属性
 *
 * 栈上 @ref 预填 coherent/BIDIRECTIONAL 和配对字段。先验证 @virt 可转换，再写入
 * paddr；之后若调试已禁用则旁路，否则 check_unmap() 同时核对 CPU 物理地址、长度、
 * 类型和 attrs 并归还 entry。无返回值，不释放真实缓冲区。
 */
void debug_dma_free_coherent(struct device *dev, size_t size, void *virt,
			     dma_addr_t dma_addr, unsigned long attrs)
{
	struct dma_debug_entry ref = {
		.type           = dma_debug_coherent,
		.dev            = dev,
		.dev_addr       = dma_addr,
		.size           = size,
		.direction      = DMA_BIDIRECTIONAL,
		.attrs          = attrs,
	};

	/* handle vmalloc and linear addresses */
	/* 原注释翻译：仅对 vmalloc 或有效线性地址构造物理地址查询键。 */
	if (!is_vmalloc_addr(virt) && !virt_addr_valid(virt))
		return;

	ref.paddr = virt_to_paddr(virt);

	if (unlikely(dma_debug_disabled()))
		return;

	check_unmap(&ref);
}

/*
 * debug_dma_sync_single_for_cpu - 校验单段映射把 ownership 同步回 CPU
 * @dev: DMA 设备；@dma_handle: sync 子区间设备起始地址
 * @size: 子区间字节数；@direction: 本次 sync 声明方向
 *
 * 构造 @ref（single 类型、设备、地址、长度、方向，SG 项数为 0），调试可用时调用
 * check_sync(..., true)。只校验账本和方向，不执行真实 cache 操作、不消费映射。
 */
void debug_dma_sync_single_for_cpu(struct device *dev, dma_addr_t dma_handle,
				   size_t size, int direction)
{
	struct dma_debug_entry ref;

	if (unlikely(dma_debug_disabled()))
		return;

	ref.type         = dma_debug_single;
	ref.dev          = dev;
	ref.dev_addr     = dma_handle;
	ref.size         = size;
	ref.direction    = direction;
	ref.sg_call_ents = 0;

	check_sync(dev, &ref, true);
}

/*
 * debug_dma_sync_single_for_device - 校验单段映射把 ownership 同步给设备
 * @dev: DMA 设备；@dma_handle: sync 子区间设备地址
 * @size: 子区间字节数；@direction: 本次 sync 方向
 *
 * 与 CPU 版本构造相同 @ref，但以 check_sync(..., false) 应用 device 方向规则。
 * 调试禁用时无操作；不消费 entry。
 */
void debug_dma_sync_single_for_device(struct device *dev,
				      dma_addr_t dma_handle, size_t size,
				      int direction)
{
	struct dma_debug_entry ref;

	if (unlikely(dma_debug_disabled()))
		return;

	ref.type         = dma_debug_single;
	ref.dev          = dev;
	ref.dev_addr     = dma_handle;
	ref.size         = size;
	ref.direction    = direction;
	ref.sg_call_ents = 0;

	check_sync(dev, &ref, false);
}

/*
 * debug_dma_sync_sg_for_cpu - 校验 SG 映射的 CPU ownership 同步
 * @dev: DMA 设备；@sg: 原 scatterlist；@nelems: 原始项数；@direction: sync 方向
 *
 * @s/@i 遍历，@mapped_ents 由第一项活动记录恢复；每项栈上 @ref 保存 sg 类型、双
 * 地址、DMA 长度、方向和原始项数。恢复结果为 0 或到达 mapped_ents 时立即停止；
 * 否则逐段 check_sync(..., true)。只读取映射后的 DMA 字段，不修改 SG 或 entry。
 */
void debug_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg,
			       int nelems, int direction)
{
	struct scatterlist *s;
	int mapped_ents = 0, i;

	if (unlikely(dma_debug_disabled()))
		return;

	for_each_sg(sg, s, nelems, i) {

		struct dma_debug_entry ref = {
			.type           = dma_debug_sg,
			.dev            = dev,
			.paddr		= sg_phys(s),
			.dev_addr       = sg_dma_address(s),
			.size           = sg_dma_len(s),
			.direction      = direction,
			.sg_call_ents   = nelems,
		};

		if (!i)
			mapped_ents = get_nr_mapped_entries(dev, &ref);

		if (i >= mapped_ents)
			break;

		check_sync(dev, &ref, true);
	}
}

/*
 * debug_dma_sync_sg_for_device - 校验 SG 映射的 device ownership 同步
 * @dev: DMA 设备；@sg: 原列表；@nelems: 原始项数；@direction: sync 方向
 *
 * 遍历和 @mapped_ents 截止规则与 CPU 版本相同，区别是调用
 * check_sync(..., false)。调试禁用或首项记录缺失时不产生逐段检查。
 */
void debug_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
				  int nelems, int direction)
{
	struct scatterlist *s;
	int mapped_ents = 0, i;

	if (unlikely(dma_debug_disabled()))
		return;

	for_each_sg(sg, s, nelems, i) {

		struct dma_debug_entry ref = {
			.type           = dma_debug_sg,
			.dev            = dev,
			.paddr		= sg_phys(s),
			.dev_addr       = sg_dma_address(s),
			.size           = sg_dma_len(s),
			.direction      = direction,
			.sg_call_ents   = nelems,
		};
		if (!i)
			mapped_ents = get_nr_mapped_entries(dev, &ref);

		if (i >= mapped_ents)
			break;

		check_sync(dev, &ref, false);
	}
}

/*
 * debug_dma_alloc_pages - 记录 DMA API 分配的 noncoherent pages
 * @dev: 分配设备；@page: 连续页首页；@size: 字节数
 * @direction: DMA 方向；@dma_addr: 设备地址
 *
 * 调试可用时取得 @entry，填写 noncoherent 类型、page_to_phys() 地址、长度、设备
 * 地址和方向后发布。对象分配失败只漏记真实分配；entry 不持有 page 引用，调用者
 * 仍负责以 debug_dma_free_pages 对应的真实释放路径结束生命周期。
 */
void debug_dma_alloc_pages(struct device *dev, struct page *page,
			   size_t size, int direction,
			   dma_addr_t dma_addr)
{
	struct dma_debug_entry *entry;

	if (unlikely(dma_debug_disabled()))
		return;

	entry = dma_entry_alloc();
	if (!entry)
		return;

	entry->type      = dma_debug_noncoherent;
	entry->dev       = dev;
	entry->paddr	 = page_to_phys(page);
	entry->size      = size;
	entry->dev_addr  = dma_addr;
	entry->direction = direction;

	add_dma_entry(entry);
}

/*
 * debug_dma_free_pages - 校验并消费 noncoherent pages 分配记录
 * @dev: 原分配设备；@page: 原首页；@size: 原字节数
 * @direction: free 方向；@dma_addr: 原设备地址
 *
 * 栈上 @ref 由 page_to_phys() 和所有配对字段组成。调试可用时 check_unmap() 核对
 * type、CPU 物理地址、大小、方向并删除调试记录。无返回值，不降低页面引用或执行
 * 真实释放。
 */
void debug_dma_free_pages(struct device *dev, struct page *page,
			  size_t size, int direction,
			  dma_addr_t dma_addr)
{
	struct dma_debug_entry ref = {
		.type           = dma_debug_noncoherent,
		.dev            = dev,
		.paddr		= page_to_phys(page),
		.dev_addr       = dma_addr,
		.size           = size,
		.direction      = direction,
	};

	if (unlikely(dma_debug_disabled()))
		return;

	check_unmap(&ref);
}

/*
 * dma_debug_driver_setup - 解析启动参数 dma_debug_driver=<name>
 * @str: __setup 提供的非 NULL 驱动名称字符串
 *
 * 局部 @i 最多复制 NAME_MAX_LEN-1 个字符；短字符串遇 NUL 停止，长字符串依靠全局
 * 数组末字节的 BSS 零值保持终止。启动期尚无并发 debugfs 写者，因此不取
 * driver_name_lock。非空名称打印启用提示，返回 1 表示参数已消费；具体 driver
 * 指针仍由首次错误事件惰性匹配。
 */
static int __init dma_debug_driver_setup(char *str)
{
	int i;

	for (i = 0; i < NAME_MAX_LEN - 1; ++i, ++str) {
		current_driver_name[i] = *str;
		if (*str == 0)
			break;
	}

	if (current_driver_name[0])
		pr_info("enable driver filter for driver [%s]\n",
			current_driver_name);


	return 1;
}

/* 把 dma_debug_driver= 注册到启动参数表；只影响错误显示过滤，不影响账本覆盖范围。 */
__setup("dma_debug_driver=", dma_debug_driver_setup);
