// SPDX-License-Identifier: GPL-2.0
/*
 * KFENCE guarded object allocator and fault handling.
 *
 * Copyright (C) 2020, Google LLC.
 */
/*
 * 中文学习注释：本文件是 KFENCE 的核心实现，已读区域覆盖 public 头文件
 * include/linux/kfence.h 所声明接口的真实状态转换点。
 *
 * 已读主线：
 *   - 启动/运行期参数控制 kfence_enabled 与 kfence_sample_interval；
 *   - kfence_allocation_key + kfence_allocation_gate 把普通 slab fast path
 *     限流到极少数 __kfence_alloc() 慢路径；
 *   - kfence_metadata 记录每个 KFENCE 对象的 cache、地址、大小、状态和栈；
 *   - free/page fault/shutdown_cache 路径都围绕 metadata 状态机判断对象是否
 *     活动、已释放、RCU 延迟释放或 zombie。
 *
 * 并发边界：
 *   全局开关用 READ_ONCE()/WRITE_ONCE() 避免编译器合并读写；metadata 的发布
 *   用 release/acquire 配对，单对象状态修改由 meta->lock 序列化；采样机会用
 *   atomic gate 竞争，避免多个 CPU 在同一窗口同时消耗 KFENCE pool。
 */

#define pr_fmt(fmt) "kfence: " fmt

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/debugfs.h>
#include <linux/hash.h>
#include <linux/irq_work.h>
#include <linux/jhash.h>
#include <linux/kasan-enabled.h>
#include <linux/kcsan-checks.h>
#include <linux/kfence.h>
#include <linux/kmemleak.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/log2.h>
#include <linux/memblock.h>
#include <linux/moduleparam.h>
#include <linux/nodemask.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/reboot.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include <asm/kfence.h>

#include "kfence.h"

/* Disables KFENCE on the first warning assuming an irrecoverable error. */
/*
 * 中文翻译与补充：KFENCE_WARN_ON() 是“发现内部不变量破坏就关闭 KFENCE”的保护宏。
 * 入参 @cond 是需要验证的内部条件；返回值等同 WARN_ON(cond)。如果触发，宏会把
 * kfence_enabled 写成 false，并记录 disabled_by_warn，避免后续继续用可能已经
 * 损坏的 metadata/pool 生成更多误导性报告。WRITE_ONCE() 只保证单次可见写入，
 * 不提供锁语义；其它路径通过 READ_ONCE() 观察这个运行期开关。
 */
#define KFENCE_WARN_ON(cond)                                                   \
	({                                                                     \
		const bool __cond = WARN_ON(cond);                             \
		if (unlikely(__cond)) {                                        \
			WRITE_ONCE(kfence_enabled, false);                     \
			disabled_by_warn = true;                               \
		}                                                              \
		__cond;                                                        \
	})

/* === Data ================================================================= */

bool kfence_enabled __read_mostly;
static bool disabled_by_warn __read_mostly;
/*
 * kfence_enabled 是运行期总开关：true 表示分配采样和 fault 诊断可以工作；
 * false 表示 public helper 应安全退化。__read_mostly 说明正常运行时读多写少，
 * 适合放到读多写少区域减少 cacheline 干扰。
 *
 * disabled_by_warn 只由 KFENCE 内部严重告警置位，用来区分“用户主动关闭/调整采样”
 * 和“内部不变量已坏，不允许 late re-enable”。这不是引用计数，也不保护 metadata。
 */

unsigned long kfence_sample_interval __read_mostly = CONFIG_KFENCE_SAMPLE_INTERVAL;
EXPORT_SYMBOL_GPL(kfence_sample_interval); /* Export for test modules. */
/*
 * kfence_sample_interval 的单位和定时器解释在 KFENCE timer 路径中使用。
 * 导出给 GPL 模块主要服务测试和调试模块；普通分配器不直接依赖该变量，而是通过
 * static key 与 allocation gate 获得“是否尝试采样”的低成本答案。
 */

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "kfence."

static int kfence_enable_late(void);
/*
 * param_set_sample_interval() - 写入 kfence.sample_interval 参数。
 *
 * 入参：
 *   @val: 用户通过 boot/module/debug 参数传入的字符串，借用，只在解析期间读取；
 *   @kp:  内核参数描述，@kp->arg 指向 kfence_sample_interval，不取得所有权。
 *
 * 返回：
 *   0       表示参数已接受，必要时已经关闭或重新启用 KFENCE；
 *   -EINVAL 表示请求与当前 KASAN HW tags 或 WARN 后禁用状态冲突；
 *   其它负 errno 来自 kstrtoul() 解析失败。
 *
 * 副作用：
 *   写 kfence_sample_interval，可能 WRITE_ONCE(kfence_enabled, false)，也可能在
 *   系统启动后调用 kfence_enable_late() 重新建立采样工作。该函数处于参数写入
 *   路径，不是分配 fast path，允许执行打印和较重的状态切换。
 */
static int param_set_sample_interval(const char *val, const struct kernel_param *kp)
{
	unsigned long num;
	int ret = kstrtoul(val, 0, &num);

	/*
	 * 阶段 1：先把字符串变成无符号长整型。解析失败时没有任何 KFENCE 状态改变，
	 * 调用者可把 errno 直接反馈给参数写入者。
	 */
	if (ret < 0)
		return ret;

	/* Using 0 to indicate KFENCE is disabled. */
	/*
	 * 中文翻译与补充：参数值 0 表示关闭 KFENCE。若当前仍启用，先打印一次状态
	 * 变化并用 WRITE_ONCE() 发布关闭；后续分配路径 READ_ONCE(kfence_enabled)
	 * 会停止真正分配，但已存在 KFENCE 对象仍由 free/fault 路径识别处理。
	 */
	if (!num && READ_ONCE(kfence_enabled)) {
		pr_info("disabled\n");
		WRITE_ONCE(kfence_enabled, false);
	}

	/*
	 * 阶段 2：拒绝与硬件 tag-based KASAN 同时启用。两者都试图接管内存错误
	 * 检测语义，KFENCE 的 guard-page 模型无法保证在该配置下提供一致诊断。
	 */
	if (num && kasan_hw_tags_enabled()) {
		pr_info("disabled as KASAN HW tags are enabled\n");
		return -EINVAL;
	}

	/*
	 * 阶段 3：提交新的 interval 值。这里直接写 kp->arg 指向的 unsigned long，
	 * 参数框架保证类型匹配；写入后若系统已启动且 KFENCE 当前关闭，再走 late
	 * enable 路径尝试重新排定 gate 定时器。
	 */
	*((unsigned long *)kp->arg) = num;

	if (num && !READ_ONCE(kfence_enabled) && system_state != SYSTEM_BOOTING)
		return disabled_by_warn ? -EINVAL : kfence_enable_late();
	return 0;
}

/*
 * param_get_sample_interval() - 读取 kfence.sample_interval 参数。
 *
 * 入参：
 *   @buffer: 参数框架提供的输出缓冲区，本函数写入文本形式的 interval；
 *   @kp:     参数描述，读取其中保存的 unsigned long。
 *
 * 返回：
 *   写入 @buffer 的字节数，或 param_get_ulong() 的返回值。若 KFENCE 已关闭，
 *   即使保存的 interval 非 0，也向用户呈现 "0\n"，表达“当前不会采样分配”。
 */
static int param_get_sample_interval(char *buffer, const struct kernel_param *kp)
{
	/*
	 * 读路径以 kfence_enabled 为准，而不是只回显 kfence_sample_interval。
	 * 这样 WARN 后禁用、KASAN 冲突或用户关闭都会向外暴露为实际不可用状态。
	 */
	if (!READ_ONCE(kfence_enabled))
		return sprintf(buffer, "0\n");

	return param_get_ulong(buffer, kp);
}

static const struct kernel_param_ops sample_interval_param_ops = {
	.set = param_set_sample_interval,
	.get = param_get_sample_interval,
};
module_param_cb(sample_interval, &sample_interval_param_ops, &kfence_sample_interval, 0600);

/* Pool usage% threshold when currently covered allocations are skipped. */
/*
 * 中文翻译与补充：当当前已分配 KFENCE 对象占 pool 的比例超过该阈值时，
 * __kfence_alloc() 会开始用 Counting Bloom filter 跳过“已有覆盖”的分配来源。
 * 入参来自 module_param，单位是百分比；它影响覆盖多样性，而不是 pool 的真实容量。
 */
static unsigned long kfence_skip_covered_thresh __read_mostly = 75;
module_param_named(skip_covered_thresh, kfence_skip_covered_thresh, ulong, 0644);

/* Allocation burst count: number of excess KFENCE allocations per sample. */
/*
 * 中文翻译与补充：kfence_burst 控制每个采样周期内允许额外放出的 KFENCE 分配数。
 * 它与 kfence_allocation_gate 一起决定“一个 timer tick 可以让多少个分配进入慢路径”。
 */
static unsigned int kfence_burst __read_mostly;
module_param_named(burst, kfence_burst, uint, 0644);

/* If true, use a deferrable timer. */
/*
 * 中文翻译与补充：kfence_deferrable 为 true 时，采样定时器可延后到 CPU 非空闲时
 * 执行，减少空闲系统被 KFENCE 周期性唤醒的功耗代价；代价是采样时间不再严格。
 */
static bool kfence_deferrable __read_mostly = IS_ENABLED(CONFIG_KFENCE_DEFERRABLE);
module_param_named(deferrable, kfence_deferrable, bool, 0444);

/* If true, check all canary bytes on panic. */
/*
 * 中文翻译与补充：kfence_check_on_panic 决定 panic notifier 中是否扫描所有对象的
 * canary 字节。开启后 panic 路径能补充发现越界写证据，但会增加崩溃路径耗时。
 */
static bool kfence_check_on_panic __read_mostly;
module_param_named(check_on_panic, kfence_check_on_panic, bool, 0444);

/* The pool of pages used for guard pages and objects. */
/*
 * 中文翻译与补充：__kfence_pool 指向“对象页 + guard 页”交错排列的专用池。
 * 这个地址是 is_kfence_address() 的范围基准，也是 addr_to_metadata() 推导对象
 * 元数据的基础。普通 slab 绝不能把该 pool 页当作自己的 slab page 管理。
 */
char *__kfence_pool __read_mostly;
EXPORT_SYMBOL(__kfence_pool); /* Export for test modules. */
/*
 * 中文翻译与补充：导出给测试模块，便于构造落在 pool 内外的地址验证 KFENCE
 * 分流和 fault 处理；生产分配路径不应绕过 public helper 直接操作该符号。
 */

/*
 * Per-object metadata, with one-to-one mapping of object metadata to
 * backing pages (in __kfence_pool).
 */
/*
 * 中文翻译与补充：每个可分配 KFENCE 对象有一份 metadata，与 __kfence_pool 中的
 * backing page 一一对应。metadata 保存对象地址、请求大小、所属 cache、状态、
 * alloc/free 栈和对象锁；它是 free、fault report、shutdown_cache 共同读取的
 * 生命周期中心。
 */
static_assert(CONFIG_KFENCE_NUM_OBJECTS > 0);
struct kfence_metadata *kfence_metadata __read_mostly;

/*
 * If kfence_metadata is not NULL, it may be accessed by kfence_shutdown_cache().
 * So introduce kfence_metadata_init to initialize metadata, and then make
 * kfence_metadata visible after initialization is successful. This prevents
 * potential UAF or access to uninitialized metadata.
 */
/*
 * 中文翻译与补充：只要 kfence_metadata 非 NULL，kfence_shutdown_cache() 就可能
 * 并发扫描它。因此初始化阶段先写临时指针 kfence_metadata_init，完成全部对象
 * metadata 初始化后，再用 release 语义发布到 kfence_metadata。这样读侧 acquire
 * 看到非 NULL 时，可以相信数组内容已经初始化完成，避免 UAF 或读取半初始化字段。
 */
static struct kfence_metadata *kfence_metadata_init __read_mostly;

/* Freelist with available objects. */
/*
 * 中文翻译与补充：kfence_freelist 保存当前可重新分配的 KFENCE metadata/object。
 * kfence_freelist_lock 保护链表结构以及从 freelist 取出/放回对象的结构性修改；
 * 单个对象的 cache/state/栈等字段仍由各自 meta->lock 保护。
 */
DEFINE_RAW_SPINLOCK(kfence_freelist_lock); /* Lock protecting freelist. */
/*
 * 中文翻译与补充：该锁只保护 freelist 链接本身，不能替代 metadata 对象锁。
 * 取 freelist 锁期间禁止睡眠；对象状态转换需要在更细粒度的 meta->lock 下完成。
 */
static struct list_head kfence_freelist __guarded_by(&kfence_freelist_lock) = LIST_HEAD_INIT(kfence_freelist);

/*
 * The static key to set up a KFENCE allocation; or if static keys are not used
 * to gate allocations, to avoid a load and compare if KFENCE is disabled.
 */
/*
 * 中文翻译与补充：kfence_allocation_key 是 slab fast path 的静态分支入口。
 * 开启时允许 kfence_alloc() 继续检查 atomic gate；关闭时让普通分配接近零成本。
 * 若配置不使用 static keys gate，它仍用于避免在 KFENCE disabled 时做额外加载比较。
 */
DEFINE_STATIC_KEY_FALSE(kfence_allocation_key);

/* Gates the allocation, ensuring only one succeeds in a given period. */
/*
 * 中文翻译与补充：kfence_allocation_gate 是采样窗口门闩，确保一个周期内只有被
 * gate 允许的少量 CPU 能进入 __kfence_alloc() 并尝试真正拿对象。它是原子计数，
 * 只表达采样机会，不表达 pool 是否仍有容量。
 */
atomic_t kfence_allocation_gate = ATOMIC_INIT(1);

/*
 * A Counting Bloom filter of allocation coverage: limits currently covered
 * allocations of the same source filling up the pool.
 *
 * Assuming a range of 15%-85% unique allocations in the pool at any point in
 * time, the below parameters provide a probablity of 0.02-0.33 for false
 * positive hits respectively:
 *
 *	P(alloc_traces) = (1 - e^(-HNUM * (alloc_traces / SIZE)) ^ HNUM
 */
/*
 * 中文翻译与补充：alloc_covered 是按分配调用栈哈希计数的 Counting Bloom filter。
 * 当 pool 接近满时，KFENCE 优先把稀缺对象留给尚未覆盖过的调用来源，避免同一类
 * 长寿命对象反复占满 pool。Bloom filter 可能假阳性，因此“跳过 covered”只是
 * 覆盖率权衡，不是安全正确性条件。
 */
#define ALLOC_COVERED_HNUM	2
#define ALLOC_COVERED_ORDER	(const_ilog2(CONFIG_KFENCE_NUM_OBJECTS) + 2)
#define ALLOC_COVERED_SIZE	(1 << ALLOC_COVERED_ORDER)
#define ALLOC_COVERED_HNEXT(h)	hash_32(h, ALLOC_COVERED_ORDER)
#define ALLOC_COVERED_MASK	(ALLOC_COVERED_SIZE - 1)
static atomic_t alloc_covered[ALLOC_COVERED_SIZE];

/* Stack depth used to determine uniqueness of an allocation. */
/*
 * 中文翻译与补充：用于计算分配来源唯一性的最大栈深。只取前几层可以稳定地区分
 * 调用来源，同时避免深栈中的调度/中断噪声让同一来源被误判成许多不同来源。
 */
#define UNIQUE_ALLOC_STACK_DEPTH ((size_t)8)

/*
 * Randomness for stack hashes, making the same collisions across reboots and
 * different machines less likely.
 */
/*
 * 中文翻译与补充：stack_hash_seed 在启动后只读，用来扰动调用栈哈希。它减少不同
 * 机器或不同重启间固定碰撞的概率，让覆盖率过滤更接近“按来源均匀采样”。
 */
static u32 stack_hash_seed __ro_after_init;

/* Statistics counters for debugfs. */
/*
 * 中文翻译与补充：这些 counter 是 debugfs 观察 KFENCE 行为的统计口径。
 * ALLOCATED 是当前存量，其余多为累计事件；它们帮助区分“没触发采样”“pool 满”
 * “分配约束不兼容”和“覆盖率过滤”等不同退化原因。
 */
enum kfence_counter_id {
	KFENCE_COUNTER_ALLOCATED,
	KFENCE_COUNTER_ALLOCS,
	KFENCE_COUNTER_FREES,
	KFENCE_COUNTER_ZOMBIES,
	KFENCE_COUNTER_BUGS,
	KFENCE_COUNTER_SKIP_INCOMPAT,
	KFENCE_COUNTER_SKIP_CAPACITY,
	KFENCE_COUNTER_SKIP_COVERED,
	KFENCE_COUNTER_COUNT,
};
static atomic_long_t counters[KFENCE_COUNTER_COUNT];
static const char *const counter_names[] = {
	[KFENCE_COUNTER_ALLOCATED]	= "currently allocated",
	[KFENCE_COUNTER_ALLOCS]		= "total allocations",
	[KFENCE_COUNTER_FREES]		= "total frees",
	[KFENCE_COUNTER_ZOMBIES]	= "zombie allocations",
	[KFENCE_COUNTER_BUGS]		= "total bugs",
	[KFENCE_COUNTER_SKIP_INCOMPAT]	= "skipped allocations (incompatible)",
	[KFENCE_COUNTER_SKIP_CAPACITY]	= "skipped allocations (capacity)",
	[KFENCE_COUNTER_SKIP_COVERED]	= "skipped allocations (covered)",
};
static_assert(ARRAY_SIZE(counter_names) == KFENCE_COUNTER_COUNT);

/* === Internals ============================================================ */

static inline bool should_skip_covered(void)
{
	unsigned long thresh = (CONFIG_KFENCE_NUM_OBJECTS * kfence_skip_covered_thresh) / 100;

	/*
	 * 当前分配对象数超过阈值后才启用 covered 过滤。阈值按对象个数而非字节计算，
	 * 因为 KFENCE 每个对象占用一个专用对象页，size 差异不改变 pool 槽位压力。
	 */
	return atomic_long_read(&counters[KFENCE_COUNTER_ALLOCATED]) > thresh;
}

/*
 * get_alloc_stack_hash() - 把分配调用栈压缩成覆盖率过滤使用的来源标识。
 *
 * 入参：
 *   @stack_entries: stack_trace_save() 填充的栈地址数组，借用；
 *   @num_entries:   数组中有效入口数，单位为栈帧个数。
 *
 * 返回：
 *   32 位哈希值，用于 alloc_covered Counting Bloom filter。
 *
 * 副作用：会就地过滤 IRQ 栈帧并限制参与哈希的深度，但不取得栈地址引用。
 */
static u32 get_alloc_stack_hash(unsigned long *stack_entries, size_t num_entries)
{
	/*
	 * 只取前 UNIQUE_ALLOC_STACK_DEPTH 层，把“同一分配来源”的定义固定在靠近
	 * allocator 的调用链上；filter_irq_stacks() 去掉中断栈噪声，避免同一逻辑
	 * 来源因中断上下文差异被拆成多个 covered 桶。
	 */
	num_entries = min(num_entries, UNIQUE_ALLOC_STACK_DEPTH);
	num_entries = filter_irq_stacks(stack_entries, num_entries);
	return jhash(stack_entries, num_entries * sizeof(stack_entries[0]), stack_hash_seed);
}

/*
 * Adds (or subtracts) count @val for allocation stack trace hash
 * @alloc_stack_hash from Counting Bloom filter.
 */
static void alloc_covered_add(u32 alloc_stack_hash, int val)
{
	int i;

	for (i = 0; i < ALLOC_COVERED_HNUM; i++) {
		atomic_add(val, &alloc_covered[alloc_stack_hash & ALLOC_COVERED_MASK]);
		alloc_stack_hash = ALLOC_COVERED_HNEXT(alloc_stack_hash);
	}
}

/*
 * Returns true if the allocation stack trace hash @alloc_stack_hash is
 * currently contained (non-zero count) in Counting Bloom filter.
 */
static bool alloc_covered_contains(u32 alloc_stack_hash)
{
	int i;

	for (i = 0; i < ALLOC_COVERED_HNUM; i++) {
		if (!atomic_read(&alloc_covered[alloc_stack_hash & ALLOC_COVERED_MASK]))
			return false;
		alloc_stack_hash = ALLOC_COVERED_HNEXT(alloc_stack_hash);
	}

	return true;
}

static bool kfence_protect(unsigned long addr)
{
	return !KFENCE_WARN_ON(!kfence_protect_page(ALIGN_DOWN(addr, PAGE_SIZE), true));
}

static bool kfence_unprotect(unsigned long addr)
{
	return !KFENCE_WARN_ON(!kfence_protect_page(ALIGN_DOWN(addr, PAGE_SIZE), false));
}

static inline unsigned long metadata_to_pageaddr(const struct kfence_metadata *meta)
	__must_hold(&meta->lock)
{
	unsigned long offset = (meta - kfence_metadata + 1) * PAGE_SIZE * 2;
	unsigned long pageaddr = (unsigned long)&__kfence_pool[offset];

	/* The checks do not affect performance; only called from slow-paths. */

	/* Only call with a pointer into kfence_metadata. */
	if (KFENCE_WARN_ON(meta < kfence_metadata ||
			   meta >= kfence_metadata + CONFIG_KFENCE_NUM_OBJECTS))
		return 0;

	/*
	 * This metadata object only ever maps to 1 page; verify that the stored
	 * address is in the expected range.
	 */
	if (KFENCE_WARN_ON(ALIGN_DOWN(meta->addr, PAGE_SIZE) != pageaddr))
		return 0;

	return pageaddr;
}

static inline bool kfence_obj_allocated(const struct kfence_metadata *meta)
{
	enum kfence_object_state state = READ_ONCE(meta->state);

	return state == KFENCE_OBJECT_ALLOCATED || state == KFENCE_OBJECT_RCU_FREEING;
}

/*
 * Update the object's metadata state, including updating the alloc/free stacks
 * depending on the state transition.
 */
static noinline void
metadata_update_state(struct kfence_metadata *meta, enum kfence_object_state next,
		      unsigned long *stack_entries, size_t num_stack_entries)
	__must_hold(&meta->lock)
{
	struct kfence_track *track =
		next == KFENCE_OBJECT_ALLOCATED ? &meta->alloc_track : &meta->free_track;

	lockdep_assert_held(&meta->lock);

	/* Stack has been saved when calling rcu, skip. */
	if (READ_ONCE(meta->state) == KFENCE_OBJECT_RCU_FREEING)
		goto out;

	if (stack_entries) {
		memcpy(track->stack_entries, stack_entries,
		       num_stack_entries * sizeof(stack_entries[0]));
	} else {
		/*
		 * Skip over 1 (this) functions; noinline ensures we do not
		 * accidentally skip over the caller by never inlining.
		 */
		num_stack_entries = stack_trace_save(track->stack_entries, KFENCE_STACK_DEPTH, 1);
	}
	track->num_stack_entries = num_stack_entries;
	track->pid = task_pid_nr(current);
	track->cpu = raw_smp_processor_id();
	track->ts_nsec = local_clock(); /* Same source as printk timestamps. */

out:
	/*
	 * Pairs with READ_ONCE() in
	 *	kfence_shutdown_cache(),
	 *	kfence_handle_page_fault().
	 */
	WRITE_ONCE(meta->state, next);
}

#ifdef CONFIG_KMSAN
#define check_canary_attributes noinline __no_kmsan_checks
#else
#define check_canary_attributes inline
#endif

/* Check canary byte at @addr. */
static check_canary_attributes bool check_canary_byte(u8 *addr)
{
	struct kfence_metadata *meta;
	enum kfence_fault fault;
	unsigned long flags;

	if (likely(*addr == KFENCE_CANARY_PATTERN_U8(addr)))
		return true;

	atomic_long_inc(&counters[KFENCE_COUNTER_BUGS]);

	meta = addr_to_metadata((unsigned long)addr);
	raw_spin_lock_irqsave(&meta->lock, flags);
	fault = kfence_report_error((unsigned long)addr, false, NULL, meta, KFENCE_ERROR_CORRUPTION);
	raw_spin_unlock_irqrestore(&meta->lock, flags);
	kfence_handle_fault(fault);

	return false;
}

static inline void set_canary(const struct kfence_metadata *meta)
{
	const unsigned long pageaddr = ALIGN_DOWN(meta->addr, PAGE_SIZE);
	unsigned long addr = pageaddr;

	/*
	 * The canary may be written to part of the object memory, but it does
	 * not affect it. The user should initialize the object before using it.
	 */
	for (; addr < meta->addr; addr += sizeof(u64))
		*((u64 *)addr) = KFENCE_CANARY_PATTERN_U64;

	addr = ALIGN_DOWN(meta->addr + meta->size, sizeof(u64));
	for (; addr - pageaddr < PAGE_SIZE; addr += sizeof(u64))
		*((u64 *)addr) = KFENCE_CANARY_PATTERN_U64;
}

static check_canary_attributes void
check_canary(const struct kfence_metadata *meta)
{
	const unsigned long pageaddr = ALIGN_DOWN(meta->addr, PAGE_SIZE);
	unsigned long addr = pageaddr;

	/*
	 * We'll iterate over each canary byte per-side until a corrupted byte
	 * is found. However, we'll still iterate over the canary bytes to the
	 * right of the object even if there was an error in the canary bytes to
	 * the left of the object. Specifically, if check_canary_byte()
	 * generates an error, showing both sides might give more clues as to
	 * what the error is about when displaying which bytes were corrupted.
	 */

	/* Apply to left of object. */
	for (; meta->addr - addr >= sizeof(u64); addr += sizeof(u64)) {
		if (unlikely(*((u64 *)addr) != KFENCE_CANARY_PATTERN_U64))
			break;
	}

	/*
	 * If the canary is corrupted in a certain 64 bytes, or the canary
	 * memory cannot be completely covered by multiple consecutive 64 bytes,
	 * it needs to be checked one by one.
	 */
	for (; addr < meta->addr; addr++) {
		if (unlikely(!check_canary_byte((u8 *)addr)))
			break;
	}

	/* Apply to right of object. */
	for (addr = meta->addr + meta->size; addr % sizeof(u64) != 0; addr++) {
		if (unlikely(!check_canary_byte((u8 *)addr)))
			return;
	}
	for (; addr - pageaddr < PAGE_SIZE; addr += sizeof(u64)) {
		if (unlikely(*((u64 *)addr) != KFENCE_CANARY_PATTERN_U64)) {

			for (; addr - pageaddr < PAGE_SIZE; addr++) {
				if (!check_canary_byte((u8 *)addr))
					return;
			}
		}
	}
}

static void *kfence_guarded_alloc(struct kmem_cache *cache, size_t size, gfp_t gfp,
				  unsigned long *stack_entries, size_t num_stack_entries,
				  u32 alloc_stack_hash)
{
	struct kfence_metadata *meta = NULL;
	unsigned long flags;
	struct slab *slab;
	void *addr;
	const bool random_right_allocate = get_random_u32_below(2);
	const bool random_fault = CONFIG_KFENCE_STRESS_TEST_FAULTS &&
				  !get_random_u32_below(CONFIG_KFENCE_STRESS_TEST_FAULTS);

	/* Try to obtain a free object. */
	raw_spin_lock_irqsave(&kfence_freelist_lock, flags);
	if (!list_empty(&kfence_freelist)) {
		meta = list_entry(kfence_freelist.next, struct kfence_metadata, list);
		list_del_init(&meta->list);
	}
	raw_spin_unlock_irqrestore(&kfence_freelist_lock, flags);
	if (!meta) {
		atomic_long_inc(&counters[KFENCE_COUNTER_SKIP_CAPACITY]);
		return NULL;
	}

	if (unlikely(!raw_spin_trylock_irqsave(&meta->lock, flags))) {
		/*
		 * This is extremely unlikely -- we are reporting on a
		 * use-after-free, which locked meta->lock, and the reporting
		 * code via printk calls kmalloc() which ends up in
		 * kfence_alloc() and tries to grab the same object that we're
		 * reporting on. While it has never been observed, lockdep does
		 * report that there is a possibility of deadlock. Fix it by
		 * using trylock and bailing out gracefully.
		 */
		raw_spin_lock_irqsave(&kfence_freelist_lock, flags);
		/* Put the object back on the freelist. */
		list_add_tail(&meta->list, &kfence_freelist);
		raw_spin_unlock_irqrestore(&kfence_freelist_lock, flags);

		return NULL;
	}

	meta->addr = metadata_to_pageaddr(meta);
	/* Unprotect if we're reusing this page. */
	if (meta->state == KFENCE_OBJECT_FREED)
		kfence_unprotect(meta->addr);

	/*
	 * Note: for allocations made before RNG initialization, will always
	 * return zero. We still benefit from enabling KFENCE as early as
	 * possible, even when the RNG is not yet available, as this will allow
	 * KFENCE to detect bugs due to earlier allocations. The only downside
	 * is that the out-of-bounds accesses detected are deterministic for
	 * such allocations.
	 */
	if (random_right_allocate) {
		/* Allocate on the "right" side, re-calculate address. */
		meta->addr += PAGE_SIZE - size;
		meta->addr = ALIGN_DOWN(meta->addr, cache->align);
	}

	addr = (void *)meta->addr;

	/* Update remaining metadata. */
	metadata_update_state(meta, KFENCE_OBJECT_ALLOCATED, stack_entries, num_stack_entries);
	/* Pairs with READ_ONCE() in kfence_shutdown_cache(). */
	WRITE_ONCE(meta->cache, cache);
	meta->size = size;
	meta->alloc_stack_hash = alloc_stack_hash;
	raw_spin_unlock_irqrestore(&meta->lock, flags);

	alloc_covered_add(alloc_stack_hash, 1);

	/* Set required slab fields. */
	slab = virt_to_slab(addr);
	slab->slab_cache = cache;
	slab->objects = 1;

	/* Memory initialization. */
	set_canary(meta);

	/*
	 * We check slab_want_init_on_alloc() ourselves, rather than letting
	 * slab do the initialization, as otherwise it might overwrite KFENCE's
	 * redzone.
	 */
	if (unlikely(slab_want_init_on_alloc(gfp, cache)))
		memzero_explicit(addr, size);
	if (cache->ctor)
		cache->ctor(addr);

	if (random_fault)
		kfence_protect(meta->addr); /* Random "faults" by protecting the object. */

	atomic_long_inc(&counters[KFENCE_COUNTER_ALLOCATED]);
	atomic_long_inc(&counters[KFENCE_COUNTER_ALLOCS]);

	return addr;
}

static void kfence_guarded_free(void *addr, struct kfence_metadata *meta, bool zombie)
{
	struct kcsan_scoped_access assert_page_exclusive;
	u32 alloc_stack_hash;
	unsigned long flags;
	bool init;

	raw_spin_lock_irqsave(&meta->lock, flags);

	if (!kfence_obj_allocated(meta) || meta->addr != (unsigned long)addr) {
		enum kfence_fault fault;

		/* Invalid or double-free, bail out. */
		atomic_long_inc(&counters[KFENCE_COUNTER_BUGS]);
		fault = kfence_report_error((unsigned long)addr, false, NULL, meta,
					    KFENCE_ERROR_INVALID_FREE);
		raw_spin_unlock_irqrestore(&meta->lock, flags);
		kfence_handle_fault(fault);
		return;
	}

	/* Detect racy use-after-free, or incorrect reallocation of this page by KFENCE. */
	kcsan_begin_scoped_access((void *)ALIGN_DOWN((unsigned long)addr, PAGE_SIZE), PAGE_SIZE,
				  KCSAN_ACCESS_SCOPED | KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ASSERT,
				  &assert_page_exclusive);

	if (CONFIG_KFENCE_STRESS_TEST_FAULTS)
		kfence_unprotect((unsigned long)addr); /* To check canary bytes. */

	/* Restore page protection if there was an OOB access. */
	if (meta->unprotected_page) {
		memzero_explicit((void *)ALIGN_DOWN(meta->unprotected_page, PAGE_SIZE), PAGE_SIZE);
		kfence_protect(meta->unprotected_page);
		meta->unprotected_page = 0;
	}

	/* Mark the object as freed. */
	metadata_update_state(meta, KFENCE_OBJECT_FREED, NULL, 0);
	init = slab_want_init_on_free(meta->cache);
	alloc_stack_hash = meta->alloc_stack_hash;
	raw_spin_unlock_irqrestore(&meta->lock, flags);

	alloc_covered_add(alloc_stack_hash, -1);

	/* Check canary bytes for memory corruption. */
	check_canary(meta);

	/*
	 * Clear memory if init-on-free is set. While we protect the page, the
	 * data is still there, and after a use-after-free is detected, we
	 * unprotect the page, so the data is still accessible.
	 */
	if (!zombie && unlikely(init))
		memzero_explicit(addr, meta->size);

	/* Protect to detect use-after-frees. */
	kfence_protect((unsigned long)addr);

	kcsan_end_scoped_access(&assert_page_exclusive);
	if (!zombie) {
		/* Add it to the tail of the freelist for reuse. */
		raw_spin_lock_irqsave(&kfence_freelist_lock, flags);
		KFENCE_WARN_ON(!list_empty(&meta->list));
		list_add_tail(&meta->list, &kfence_freelist);
		raw_spin_unlock_irqrestore(&kfence_freelist_lock, flags);

		atomic_long_dec(&counters[KFENCE_COUNTER_ALLOCATED]);
		atomic_long_inc(&counters[KFENCE_COUNTER_FREES]);
	} else {
		/* See kfence_shutdown_cache(). */
		atomic_long_inc(&counters[KFENCE_COUNTER_ZOMBIES]);
	}
}

static void rcu_guarded_free(struct rcu_head *h)
{
	struct kfence_metadata *meta = container_of(h, struct kfence_metadata, rcu_head);

	kfence_guarded_free((void *)meta->addr, meta, false);
}

/*
 * Initialization of the KFENCE pool after its allocation.
 * Returns 0 on success; otherwise returns the address up to
 * which partial initialization succeeded.
 */
static unsigned long kfence_init_pool(void)
	__context_unsafe(/* constructor */)
{
	unsigned long addr, start_pfn;
	int i, rand;

	if (!arch_kfence_init_pool())
		return (unsigned long)__kfence_pool;

	addr = (unsigned long)__kfence_pool;
	start_pfn = PHYS_PFN(virt_to_phys(__kfence_pool));

	/*
	 * Set up object pages: they must have PGTY_slab set to avoid freeing
	 * them as real pages.
	 *
	 * We also want to avoid inserting kfence_free() in the kfree()
	 * fast-path in SLUB, and therefore need to ensure kfree() correctly
	 * enters __slab_free() slow-path.
	 */
	for (i = 0; i < KFENCE_POOL_SIZE / PAGE_SIZE; i++) {
		struct page *page;

		if (!i || (i % 2))
			continue;

		page = pfn_to_page(start_pfn + i);
		__SetPageSlab(page);
#ifdef CONFIG_MEMCG
		struct slab *slab = page_slab(page);
		slab->obj_exts = (unsigned long)&kfence_metadata_init[i / 2 - 1].obj_exts |
				 MEMCG_DATA_OBJEXTS;
#endif
	}

	/*
	 * Protect the first 2 pages. The first page is mostly unnecessary, and
	 * merely serves as an extended guard page. However, adding one
	 * additional page in the beginning gives us an even number of pages,
	 * which simplifies the mapping of address to metadata index.
	 */
	for (i = 0; i < 2; i++) {
		if (unlikely(!kfence_protect(addr)))
			return addr;

		addr += PAGE_SIZE;
	}

	for (i = 0; i < CONFIG_KFENCE_NUM_OBJECTS; i++) {
		struct kfence_metadata *meta = &kfence_metadata_init[i];

		/* Initialize metadata. */
		INIT_LIST_HEAD(&meta->list);
		raw_spin_lock_init(&meta->lock);
		meta->state = KFENCE_OBJECT_UNUSED;
		/* Use addr to randomize the freelist. */
		meta->addr = i;

		/* Protect the right redzone. */
		if (unlikely(!kfence_protect(addr + 2 * i * PAGE_SIZE + PAGE_SIZE)))
			goto reset_slab;
	}

	for (i = CONFIG_KFENCE_NUM_OBJECTS; i > 0; i--) {
		rand = get_random_u32_below(i);
		swap(kfence_metadata_init[i - 1].addr, kfence_metadata_init[rand].addr);
	}

	for (i = 0; i < CONFIG_KFENCE_NUM_OBJECTS; i++) {
		struct kfence_metadata *meta_1 = &kfence_metadata_init[i];
		struct kfence_metadata *meta_2 = &kfence_metadata_init[meta_1->addr];

		list_add_tail(&meta_2->list, &kfence_freelist);
	}
	for (i = 0; i < CONFIG_KFENCE_NUM_OBJECTS; i++) {
		kfence_metadata_init[i].addr = addr;
		addr += 2 * PAGE_SIZE;
	}

	/*
	 * Make kfence_metadata visible only when initialization is successful.
	 * Otherwise, if the initialization fails and kfence_metadata is freed,
	 * it may cause UAF in kfence_shutdown_cache().
	 */
	smp_store_release(&kfence_metadata, kfence_metadata_init);
	return 0;

reset_slab:
	addr += 2 * i * PAGE_SIZE;
	for (i = 0; i < KFENCE_POOL_SIZE / PAGE_SIZE; i++) {
		struct page *page;

		if (!i || (i % 2))
			continue;

		page = pfn_to_page(start_pfn + i);
#ifdef CONFIG_MEMCG
		struct slab *slab = page_slab(page);
		slab->obj_exts = 0;
#endif
		__ClearPageSlab(page);
	}

	return addr;
}

static bool __init kfence_init_pool_early(void)
{
	unsigned long addr;

	if (!__kfence_pool)
		return false;

	addr = kfence_init_pool();

	if (!addr) {
		/*
		 * The pool is live and will never be deallocated from this point on.
		 * Ignore the pool object from the kmemleak phys object tree, as it would
		 * otherwise overlap with allocations returned by kfence_alloc(), which
		 * are registered with kmemleak through the slab post-alloc hook.
		 */
		kmemleak_ignore_phys(__pa(__kfence_pool));
		return true;
	}

	/*
	 * Only release unprotected pages, and do not try to go back and change
	 * page attributes due to risk of failing to do so as well. If changing
	 * page attributes for some pages fails, it is very likely that it also
	 * fails for the first page, and therefore expect addr==__kfence_pool in
	 * most failure cases.
	 */
	memblock_free((void *)addr, KFENCE_POOL_SIZE - (addr - (unsigned long)__kfence_pool));
	__kfence_pool = NULL;

	memblock_free(kfence_metadata_init, KFENCE_METADATA_SIZE);
	kfence_metadata_init = NULL;

	return false;
}

/* === DebugFS Interface ==================================================== */

static int stats_show(struct seq_file *seq, void *v)
{
	int i;

	seq_printf(seq, "enabled: %i\n", READ_ONCE(kfence_enabled));
	for (i = 0; i < KFENCE_COUNTER_COUNT; i++)
		seq_printf(seq, "%s: %ld\n", counter_names[i], atomic_long_read(&counters[i]));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

/*
 * debugfs seq_file operations for /sys/kernel/debug/kfence/objects.
 * start_object() and next_object() return the object index + 1, because NULL is used
 * to stop iteration.
 */
static void *start_object(struct seq_file *seq, loff_t *pos)
{
	if (*pos < CONFIG_KFENCE_NUM_OBJECTS)
		return (void *)((long)*pos + 1);
	return NULL;
}

static void stop_object(struct seq_file *seq, void *v)
{
}

static void *next_object(struct seq_file *seq, void *v, loff_t *pos)
{
	++*pos;
	if (*pos < CONFIG_KFENCE_NUM_OBJECTS)
		return (void *)((long)*pos + 1);
	return NULL;
}

static int show_object(struct seq_file *seq, void *v)
{
	struct kfence_metadata *meta = &kfence_metadata[(long)v - 1];
	unsigned long flags;

	raw_spin_lock_irqsave(&meta->lock, flags);
	kfence_print_object(seq, meta);
	raw_spin_unlock_irqrestore(&meta->lock, flags);
	seq_puts(seq, "---------------------------------\n");

	return 0;
}

static const struct seq_operations objects_sops = {
	.start = start_object,
	.next = next_object,
	.stop = stop_object,
	.show = show_object,
};
DEFINE_SEQ_ATTRIBUTE(objects);

static int kfence_debugfs_init(void)
{
	struct dentry *kfence_dir;

	if (!READ_ONCE(kfence_enabled))
		return 0;

	kfence_dir = debugfs_create_dir("kfence", NULL);
	debugfs_create_file("stats", 0444, kfence_dir, NULL, &stats_fops);
	debugfs_create_file("objects", 0400, kfence_dir, NULL, &objects_fops);
	return 0;
}

late_initcall(kfence_debugfs_init);

/* === Panic Notifier ====================================================== */

static void kfence_check_all_canary(void)
{
	int i;

	for (i = 0; i < CONFIG_KFENCE_NUM_OBJECTS; i++) {
		struct kfence_metadata *meta = &kfence_metadata[i];

		if (kfence_obj_allocated(meta))
			check_canary(meta);
	}
}

static int kfence_check_canary_callback(struct notifier_block *nb,
					unsigned long reason, void *arg)
{
	if (READ_ONCE(kfence_enabled))
		kfence_check_all_canary();
	return NOTIFY_OK;
}

static struct notifier_block kfence_check_canary_notifier = {
	.notifier_call = kfence_check_canary_callback,
};

/* === Allocation Gate Timer ================================================ */

static struct delayed_work kfence_timer;

#ifdef CONFIG_KFENCE_STATIC_KEYS
/* Wait queue to wake up allocation-gate timer task. */
static DECLARE_WAIT_QUEUE_HEAD(allocation_wait);

static int kfence_reboot_callback(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	/*
	 * Disable kfence to avoid static keys IPI synchronization during
	 * late shutdown/kexec
	 */
	WRITE_ONCE(kfence_enabled, false);
	/* Cancel any pending timer work */
	cancel_delayed_work(&kfence_timer);
	/*
	 * Wake up any blocked toggle_allocation_gate() so it can complete
	 * early while the system is still able to handle IPIs.
	 */
	wake_up(&allocation_wait);

	return NOTIFY_OK;
}

static struct notifier_block kfence_reboot_notifier = {
	.notifier_call = kfence_reboot_callback,
	.priority = INT_MAX, /* Run early to stop timers ASAP */
};

static void wake_up_kfence_timer(struct irq_work *work)
{
	wake_up(&allocation_wait);
}
static DEFINE_IRQ_WORK(wake_up_kfence_timer_work, wake_up_kfence_timer);
#endif

/*
 * Set up delayed work, which will enable and disable the static key. We need to
 * use a work queue (rather than a simple timer), since enabling and disabling a
 * static key cannot be done from an interrupt.
 *
 * Note: Toggling a static branch currently causes IPIs, and here we'll end up
 * with a total of 2 IPIs to all CPUs. If this ends up a problem in future (with
 * more aggressive sampling intervals), we could get away with a variant that
 * avoids IPIs, at the cost of not immediately capturing allocations if the
 * instructions remain cached.
 */
static void toggle_allocation_gate(struct work_struct *work)
{
	if (!READ_ONCE(kfence_enabled))
		return;

	atomic_set(&kfence_allocation_gate, -kfence_burst);
#ifdef CONFIG_KFENCE_STATIC_KEYS
	/* Enable static key, and await allocation to happen. */
	static_branch_enable(&kfence_allocation_key);

	wait_event_idle(allocation_wait,
			atomic_read(&kfence_allocation_gate) > 0 ||
			!READ_ONCE(kfence_enabled));

	/* Disable static key and reset timer. */
	static_branch_disable(&kfence_allocation_key);
#endif
	queue_delayed_work(system_dfl_wq, &kfence_timer,
			   msecs_to_jiffies(kfence_sample_interval));
}

/* === Public interface ===================================================== */

void __init kfence_alloc_pool_and_metadata(void)
{
	if (!kfence_sample_interval)
		return;

	/*
	 * If KASAN hardware tags are enabled, disable KFENCE, because it
	 * does not support MTE yet.
	 */
	if (kasan_hw_tags_enabled()) {
		pr_info("disabled as KASAN HW tags are enabled\n");
		if (__kfence_pool) {
			memblock_free(__kfence_pool, KFENCE_POOL_SIZE);
			__kfence_pool = NULL;
		}
		kfence_sample_interval = 0;
		return;
	}

	/*
	 * If the pool has already been initialized by arch, there is no need to
	 * re-allocate the memory pool.
	 */
	if (!__kfence_pool)
		__kfence_pool = memblock_alloc(KFENCE_POOL_SIZE, PAGE_SIZE);

	if (!__kfence_pool) {
		pr_err("failed to allocate pool\n");
		return;
	}

	/* The memory allocated by memblock has been zeroed out. */
	kfence_metadata_init = memblock_alloc(KFENCE_METADATA_SIZE, PAGE_SIZE);
	if (!kfence_metadata_init) {
		pr_err("failed to allocate metadata\n");
		memblock_free(__kfence_pool, KFENCE_POOL_SIZE);
		__kfence_pool = NULL;
	}
}

static void kfence_init_enable(void)
{
	if (!IS_ENABLED(CONFIG_KFENCE_STATIC_KEYS))
		static_branch_enable(&kfence_allocation_key);

	if (kfence_deferrable)
		INIT_DEFERRABLE_WORK(&kfence_timer, toggle_allocation_gate);
	else
		INIT_DELAYED_WORK(&kfence_timer, toggle_allocation_gate);

	if (kfence_check_on_panic)
		atomic_notifier_chain_register(&panic_notifier_list, &kfence_check_canary_notifier);

#ifdef CONFIG_KFENCE_STATIC_KEYS
	register_reboot_notifier(&kfence_reboot_notifier);
#endif

	WRITE_ONCE(kfence_enabled, true);
	queue_delayed_work(system_dfl_wq, &kfence_timer, 0);

	pr_info("initialized - using %lu bytes for %d objects at 0x%p-0x%p\n", KFENCE_POOL_SIZE,
		CONFIG_KFENCE_NUM_OBJECTS, (void *)__kfence_pool,
		(void *)(__kfence_pool + KFENCE_POOL_SIZE));
}

void __init kfence_init(void)
{
	stack_hash_seed = get_random_u32();

	/* Setting kfence_sample_interval to 0 on boot disables KFENCE. */
	if (!kfence_sample_interval)
		return;

	if (!kfence_init_pool_early()) {
		pr_err("%s failed\n", __func__);
		return;
	}

	kfence_init_enable();
}

static int kfence_init_late(void)
{
	const unsigned long nr_pages_pool = KFENCE_POOL_SIZE / PAGE_SIZE;
	const unsigned long nr_pages_meta = KFENCE_METADATA_SIZE / PAGE_SIZE;
	unsigned long addr = (unsigned long)__kfence_pool;
	unsigned long free_size = KFENCE_POOL_SIZE;
	int err = -ENOMEM;

#ifdef CONFIG_CONTIG_ALLOC
	struct page *pages;

	pages = alloc_contig_pages(nr_pages_pool, GFP_KERNEL | __GFP_SKIP_KASAN,
				   first_online_node, NULL);
	if (!pages)
		return -ENOMEM;

	__kfence_pool = page_to_virt(pages);
	pages = alloc_contig_pages(nr_pages_meta, GFP_KERNEL | __GFP_SKIP_KASAN,
				   first_online_node, NULL);
	if (pages)
		kfence_metadata_init = page_to_virt(pages);
#else
	if (nr_pages_pool > MAX_ORDER_NR_PAGES ||
	    nr_pages_meta > MAX_ORDER_NR_PAGES) {
		pr_warn("KFENCE_NUM_OBJECTS too large for buddy allocator\n");
		return -EINVAL;
	}

	__kfence_pool = alloc_pages_exact(KFENCE_POOL_SIZE,
					  GFP_KERNEL | __GFP_SKIP_KASAN);
	if (!__kfence_pool)
		return -ENOMEM;

	kfence_metadata_init = alloc_pages_exact(KFENCE_METADATA_SIZE,
						 GFP_KERNEL | __GFP_SKIP_KASAN);
#endif

	if (!kfence_metadata_init)
		goto free_pool;

	memzero_explicit(kfence_metadata_init, KFENCE_METADATA_SIZE);
	addr = kfence_init_pool();
	if (!addr) {
		kfence_init_enable();
		kfence_debugfs_init();
		return 0;
	}

	pr_err("%s failed\n", __func__);
	free_size = KFENCE_POOL_SIZE - (addr - (unsigned long)__kfence_pool);
	err = -EBUSY;

#ifdef CONFIG_CONTIG_ALLOC
	free_contig_range(page_to_pfn(virt_to_page((void *)kfence_metadata_init)),
			  nr_pages_meta);
free_pool:
	free_contig_range(page_to_pfn(virt_to_page((void *)addr)),
			  free_size / PAGE_SIZE);
#else
	free_pages_exact((void *)kfence_metadata_init, KFENCE_METADATA_SIZE);
free_pool:
	free_pages_exact((void *)addr, free_size);
#endif

	kfence_metadata_init = NULL;
	__kfence_pool = NULL;
	return err;
}

static int kfence_enable_late(void)
{
	if (!__kfence_pool)
		return kfence_init_late();

	WRITE_ONCE(kfence_enabled, true);
	queue_delayed_work(system_dfl_wq, &kfence_timer, 0);
	pr_info("re-enabled\n");
	return 0;
}

void kfence_shutdown_cache(struct kmem_cache *s)
{
	unsigned long flags;
	struct kfence_metadata *meta;
	int i;

	/* Pairs with release in kfence_init_pool(). */
	/*
	 * 中文翻译与补充：这里与 kfence_init_pool() 发布 metadata 的 release 写配对。
	 * 如果读到 NULL，说明 KFENCE metadata 尚未发布或初始化失败，shutdown_cache()
	 * 没有可扫描对象，直接保持普通 cache 销毁语义。acquire 保证一旦看到非 NULL，
	 * 后续读取 metadata 数组时不会看到初始化前的半成品字段。
	 */
	if (!smp_load_acquire(&kfence_metadata))
		return;

	/*
	 * 阶段 1：扫描仍属于 @s 的活动 KFENCE 对象。
	 *
	 * 变量地图：
	 *   flags  保存本 CPU 中断状态，供 meta->lock 的 irqsave/restore 成对恢复；
	 *   meta   当前槽位的 metadata，借用自全局数组，不取得额外引用；
	 *   i      KFENCE 对象槽位编号，范围是 [0, CONFIG_KFENCE_NUM_OBJECTS)。
	 *
	 * 目标不是把对象放回 freelist，而是把“cache 正在销毁但对象仍被外部持有”
	 * 转化成 zombie allocation，保留诊断能力并避免普通 slab cache 被整个泄漏。
	 */
	for (i = 0; i < CONFIG_KFENCE_NUM_OBJECTS; i++) {
		bool in_use;

		meta = &kfence_metadata[i];

		/*
		 * If we observe some inconsistent cache and state pair where we
		 * should have returned false here, cache destruction is racing
		 * with either kmem_cache_alloc() or kmem_cache_free(). Taking
		 * the lock will not help, as different critical section
		 * serialization will have the same outcome.
		 */
		/*
		 * 中文翻译与补充：先用 READ_ONCE() 做无锁预筛，避免每个 metadata 都取锁。
		 * 如果这里观察到 cache/state 组合短暂不一致，说明 cache 销毁正与并发
		 * kmem_cache_alloc()/kmem_cache_free() 交错。立即取锁也不能改变高层语义：
		 * 不同临界区排序仍可能让本轮销毁看到“刚释放”或“刚分配”的边界状态。
		 */
		if (READ_ONCE(meta->cache) != s || !kfence_obj_allocated(meta))
			continue;

		/*
		 * 对候选对象取 meta->lock 复核。锁保护 meta->cache 与 state 的组合不变量；
		 * in_use 是锁内稳定快照，出锁后只用于决定是否调用 guarded_free。
		 */
		raw_spin_lock_irqsave(&meta->lock, flags);
		in_use = meta->cache == s && kfence_obj_allocated(meta);
		raw_spin_unlock_irqrestore(&meta->lock, flags);

		if (in_use) {
			/*
			 * This cache still has allocations, and we should not
			 * release them back into the freelist so they can still
			 * safely be used and retain the kernel's default
			 * behaviour of keeping the allocations alive (leak the
			 * cache); however, they effectively become "zombie
			 * allocations" as the KFENCE objects are the only ones
			 * still in use and the owning cache is being destroyed.
			 *
			 * We mark them freed, so that any subsequent use shows
			 * more useful error messages that will include stack
			 * traces of the user of the object, the original
			 * allocation, and caller to shutdown_cache().
			 */
			/*
			 * 中文翻译与补充：cache 还有活动分配时，普通内核语义是销毁失败并让
			 * 对象继续存活；KFENCE 这里不把对象归还 freelist，而是用 zombie=true
			 * 标记为 freed/zombie。后续访问会触发 KFENCE 报告，报告能同时展示
			 * 使用者、原始分配点和当前 shutdown_cache() 调用点。
			 */
			kfence_guarded_free((void *)meta->addr, meta, /*zombie=*/true);
		}
	}

	/*
	 * 阶段 2：清理已经 freed 且仍指向 @s 的 metadata->cache。
	 *
	 * 第一轮负责把活动对象转成 zombie；第二轮负责切断 freed metadata 对即将销毁
	 * cache 的借用指针，避免 cache 内存释放后，后续报告路径再通过 meta->cache
	 * 访问已经无效的 kmem_cache。
	 */
	for (i = 0; i < CONFIG_KFENCE_NUM_OBJECTS; i++) {
		meta = &kfence_metadata[i];

		/* See above. */
		/*
		 * 中文翻译与补充：同样先做无锁预筛。这里要求 state 已经是 FREED，
		 * 因为只有不再活动的对象才能安全丢弃 cache 指针；活动对象必须保留
		 * 足够信息交给第一轮 zombie 报告。
		 */
		if (READ_ONCE(meta->cache) != s || READ_ONCE(meta->state) != KFENCE_OBJECT_FREED)
			continue;

		/*
		 * 锁内再次确认 cache/state，防止并发分配或释放在预筛后改变 metadata。
		 * 成功后 meta->cache=NULL 表示该 metadata 不再借用即将销毁的 cache。
		 */
		raw_spin_lock_irqsave(&meta->lock, flags);
		if (meta->cache == s && meta->state == KFENCE_OBJECT_FREED)
			meta->cache = NULL;
		raw_spin_unlock_irqrestore(&meta->lock, flags);
	}
}

void *__kfence_alloc(struct kmem_cache *s, size_t size, gfp_t flags)
{
	unsigned long stack_entries[KFENCE_STACK_DEPTH];
	size_t num_stack_entries;
	u32 alloc_stack_hash;
	int allocation_gate;
	/*
	 * 变量地图：
	 *   stack_entries      保存本次采样分配的调用栈，用于错误报告和覆盖率过滤；
	 *   num_stack_entries  有效栈帧数，单位为帧；
	 *   alloc_stack_hash   对调用来源的压缩标识，用于 alloc_covered；
	 *   allocation_gate    本 CPU 对采样窗口 gate 的竞争结果。
	 *
	 * 入参来自 kfence_alloc()：@s/@size/@flags 均为借用输入。返回 non-NULL 时
	 * 对象所有权交给普通 allocator 调用者；返回 NULL 时调用者必须继续普通分配。
	 */

	/*
	 * Perform size check before switching kfence_allocation_gate, so that
	 * we don't disable KFENCE without making an allocation.
	 */
	/*
	 * 中文翻译与补充：先检查 size，是为了不消耗采样 gate。KFENCE 一个对象最多
	 * 放在一个 PAGE_SIZE 对象页内；过大请求不兼容，但这不是普通分配失败，所以
	 * 只增加 skip 统计并返回 NULL。
	 */
	if (size > PAGE_SIZE) {
		atomic_long_inc(&counters[KFENCE_COUNTER_SKIP_INCOMPAT]);
		return NULL;
	}

	/*
	 * Skip allocations from non-default zones, including DMA. We cannot
	 * guarantee that pages in the KFENCE pool will have the requested
	 * properties (e.g. reside in DMAable memory).
	 */
	/*
	 * 中文翻译与补充：KFENCE pool 是预留/专用页区间，不能承诺 DMA、DMA32、
	 * 指定 node 或非默认 zone 的物理属性。遇到这类 GFP/cache 约束必须退回普通
	 * allocator，否则驱动可能拿到不满足硬件 DMA 可达性的内存。
	 */
	if ((flags & GFP_ZONEMASK) ||
	    ((flags & __GFP_THISNODE) && num_online_nodes() > 1) ||
	    (s->flags & (SLAB_CACHE_DMA | SLAB_CACHE_DMA32))) {
		atomic_long_inc(&counters[KFENCE_COUNTER_SKIP_INCOMPAT]);
		return NULL;
	}

	/*
	 * Skip allocations for this slab, if KFENCE has been disabled for
	 * this slab.
	 */
	/*
	 * 中文翻译与补充：SLAB_SKIP_KFENCE 是 cache 层面对 KFENCE 的显式拒绝。
	 * 典型原因是对象布局、构造函数或调试工具组合不适合 guard-page 采样。
	 * 返回 NULL 让调用者继续普通分配，不消耗对象所有权。
	 */
	if (s->flags & SLAB_SKIP_KFENCE)
		return NULL;

	/*
	 * 阶段 2：竞争本采样窗口。atomic_inc_return() 同时递增 gate 并返回递增后值；
	 * 只有看到 <= 1 的 CPU 可以继续，其它 CPU 表示窗口已被消耗，立即退回普通
	 * allocator。这里是 kfence_alloc() 中 atomic_read() 的真正提交点。
	 */
	allocation_gate = atomic_inc_return(&kfence_allocation_gate);
	if (allocation_gate > 1)
		return NULL;
#ifdef CONFIG_KFENCE_STATIC_KEYS
	/*
	 * waitqueue_active() is fully ordered after the update of
	 * kfence_allocation_gate per atomic_inc_return().
	 */
	/*
	 * 中文翻译与补充：waitqueue_active() 需要在 gate 更新之后观察，atomic_inc_return()
	 * 已提供这里需要的顺序。若 KFENCE timer 正等待 gate 被消费，当前 CPU 需要唤醒
	 * 它安排下一次采样窗口。
	 */
	if (allocation_gate == 1 && waitqueue_active(&allocation_wait)) {
		/*
		 * Calling wake_up() here may deadlock when allocations happen
		 * from within timer code. Use an irq_work to defer it.
		 */
		/*
		 * 中文翻译与补充：分配可能发生在 timer 代码内部，直接 wake_up() 可能与
		 * 定时器/等待队列路径形成死锁。irq_work 把唤醒推迟到更安全的上下文，
		 * 保留“通知 gate 已消费”的语义，同时避开当前分配栈上的锁依赖。
		 */
		irq_work_queue(&wake_up_kfence_timer_work);
	}
#endif

	/*
	 * gate 已经被消费后仍要检查运行期开关。若用户或 WARN 路径刚关闭 KFENCE，
	 * 本次采样窗口宁可空过，也不能继续修改 pool/metadata。
	 */
	if (!READ_ONCE(kfence_enabled))
		return NULL;

	/*
	 * 阶段 3：采集分配栈。栈既用于后续错误报告，也用于覆盖率过滤；这是慢路径
	 * 成本，因此必须放在 static key 与 gate 之后。
	 */
	num_stack_entries = stack_trace_save(stack_entries, KFENCE_STACK_DEPTH, 0);

	/*
	 * Do expensive check for coverage of allocation in slow-path after
	 * allocation_gate has already become non-zero, even though it might
	 * mean not making any allocation within a given sample interval.
	 *
	 * This ensures reasonable allocation coverage when the pool is almost
	 * full, including avoiding long-lived allocations of the same source
	 * filling up the pool (e.g. pagecache allocations).
	 */
	/*
	 * 中文翻译与补充：covered 检查故意放在 gate 已非零之后。如果因为 covered
	 * 返回 NULL，本采样周期可能没有实际 KFENCE 分配；这是为了换取 pool 紧张时
	 * 更广的调用来源覆盖，避免 pagecache 这类长寿命热点来源占满所有槽位。
	 */
	alloc_stack_hash = get_alloc_stack_hash(stack_entries, num_stack_entries);
	if (should_skip_covered() && alloc_covered_contains(alloc_stack_hash)) {
		atomic_long_inc(&counters[KFENCE_COUNTER_SKIP_COVERED]);
		return NULL;
	}

	/*
	 * 阶段 4：真正从 KFENCE freelist 取对象、设置保护页和 metadata。成功返回的
	 * 对象所有权交给上层 allocator；失败返回 NULL，普通分配路径继续承担本次请求。
	 */
	return kfence_guarded_alloc(s, size, flags, stack_entries, num_stack_entries,
				    alloc_stack_hash);
}

size_t kfence_ksize(const void *addr)
{
	const struct kfence_metadata *meta = addr_to_metadata((unsigned long)addr);
	/*
	 * @addr 是借用地址，不建立对象引用。addr_to_metadata() 只按 pool 地址范围
	 * 反推 metadata；返回 NULL 表示它不是当前 KFENCE 可识别对象。
	 */

	/*
	 * Read locklessly -- if there is a race with __kfence_alloc(), this is
	 * either a use-after-free or invalid access.
	 */
	/*
	 * 中文翻译与补充：这里无锁读取 meta->size。若调用者与重新分配并发，访问对象
	 * 本身已经属于 UAF/非法访问范畴；ksize 路径不试图用锁把对象“救活”，只给
	 * 合法 KFENCE 对象返回原始请求大小，普通对象返回 0 让调用者走 __ksize()。
	 */
	return meta ? meta->size : 0;
}

void *kfence_object_start(const void *addr)
{
	const struct kfence_metadata *meta = addr_to_metadata((unsigned long)addr);
	/*
	 * @addr 可以指向对象内部，例如 kvfree_rcu_cb() 收到嵌在对象内的 rcu_head。
	 * metadata->addr 才是 KFENCE 当初交给 allocator 的对象起点。
	 */

	/*
	 * Read locklessly -- if there is a race with __kfence_alloc(), this is
	 * either a use-after-free or invalid access.
	 */
	/*
	 * 中文翻译与补充：与 kfence_ksize() 一样，这里不获取生命周期引用。返回值只在
	 * 调用者已经处于释放/诊断协议中时有效；NULL 表示应回到普通 slab 对象定位。
	 */
	return meta ? (void *)meta->addr : NULL;
}

void __kfence_free(void *addr)
{
	struct kfence_metadata *meta = addr_to_metadata((unsigned long)addr);
	/*
	 * 入参 @addr 已由 kfence_free() 或回滚路径确认属于 KFENCE pool。meta 是借用
	 * metadata 指针，后续状态转换决定对象是立即 guarded_free，还是进入 RCU 延迟。
	 */

#ifdef CONFIG_MEMCG
	/*
	 * KFENCE 对象在释放时不应还挂着 memcg objcg 扩展；若仍存在，说明计费释放
	 * 协议被破坏。KFENCE_WARN_ON() 会关闭 KFENCE，避免继续使用不可信状态。
	 */
	KFENCE_WARN_ON(meta->obj_exts.objcg);
#endif
	/*
	 * If the objects of the cache are SLAB_TYPESAFE_BY_RCU, defer freeing
	 * the object, as the object page may be recycled for other-typed
	 * objects once it has been freed. meta->cache may be NULL if the cache
	 * was destroyed.
	 * Save the stack trace here so that reports show where the user freed
	 * the object.
	 */
	/*
	 * 中文翻译与补充：SLAB_TYPESAFE_BY_RCU 允许读侧在 RCU 宽限期内继续看到旧对象页，
	 * 但释放后该页可能被同 cache 复用为另一类型对象。KFENCE 因此不能立即把对象
	 * 重新放回 freelist，而是先在 meta->lock 下标成 RCU_FREEING，并用 call_rcu()
	 * 等待读侧结束后再真正 guarded_free。meta->cache 可能已被 shutdown_cache()
	 * 清成 NULL，此时不能再解引用 cache flags。
	 */
	if (unlikely(meta->cache && (meta->cache->flags & SLAB_TYPESAFE_BY_RCU))) {
		unsigned long flags;

		/*
		 * 锁内更新状态和 free stack，保证 fault/report 路径不会看到“已进入 RCU
		 * 延迟但状态仍像活动对象”的半更新组合。
		 */
		raw_spin_lock_irqsave(&meta->lock, flags);
		metadata_update_state(meta, KFENCE_OBJECT_RCU_FREEING, NULL, 0);
		raw_spin_unlock_irqrestore(&meta->lock, flags);
		/*
		 * call_rcu() 转移最终释放动作到 RCU 回调；当前释放者返回后不能再访问对象，
		 * 但 KFENCE metadata 会保留足够信息给宽限期内的 UAF 报告。
		 */
		call_rcu(&meta->rcu_head, rcu_guarded_free);
	} else {
		/*
		 * 非 TYPESAFE_BY_RCU cache 没有读侧延迟复用承诺，可以立即进入 guarded_free：
		 * 记录 free 栈、保护对象页并把槽位归还给 KFENCE freelist。
		 */
		kfence_guarded_free(addr, meta, false);
	}
}

bool kfence_handle_page_fault(unsigned long addr, bool is_write, struct pt_regs *regs)
{
	const int page_index = (addr - (unsigned long)__kfence_pool) / PAGE_SIZE;
	struct kfence_metadata *to_report = NULL;
	unsigned long unprotected_page = 0;
	enum kfence_error_type error_type;
	enum kfence_fault fault;
	unsigned long flags;
	/*
	 * 变量地图：
	 *   page_index       fault 地址在 KFENCE pool 中的页号，用奇偶区分 guard/object 页；
	 *   to_report        最终归因的 metadata，可能是左右相邻对象或当前对象；
	 *   unprotected_page 需要临时解除保护的 guard 页地址，0 表示对象页 UAF 场景；
	 *   error_type       报告分类，OOB/UAF/INVALID；
	 *   fault            报告策略结果，交给 kfence_handle_fault() 决定 panic/继续；
	 *   flags            保护 to_report->lock 的中断状态。
	 */

	/*
	 * 阶段 1：先确认 fault 是否属于 KFENCE pool。false 会把控制权交回架构普通
	 * page fault 处理；不能对 pool 外地址做 KFENCE unprotect 或 metadata 推导。
	 */
	if (!is_kfence_address((void *)addr))
		return false;

	/*
	 * 运行期关闭时仍可能有旧保护页触发 fault。此时 KFENCE 不再报告错误，而是解除
	 * 该页保护让访问继续，避免关闭调试设施后保留不可访问页导致系统反复 fault。
	 */
	if (!READ_ONCE(kfence_enabled)) /* If disabled at runtime ... */
		return kfence_unprotect(addr); /* ... unprotect and proceed. */

	atomic_long_inc(&counters[KFENCE_COUNTER_BUGS]);

	/*
	 * 阶段 2：按页号奇偶分类。KFENCE 对象页和 guard/redzone 页交错排列；落在
	 * redzone 的 fault 更像越界访问，需要在左右邻居中选择更接近的活动对象归因。
	 */
	if (page_index % 2) {
		/* This is a redzone, report a buffer overflow. */
		/*
		 * 中文翻译与补充：当前 fault 页是 redzone，先按 buffer overflow 方向处理。
		 * meta 是左右邻居的临时 metadata 候选；distance 用于选择离 fault 地址更近
		 * 的对象边界，结果只是诊断近似值，不参与内存安全决策。
		 */
		struct kfence_metadata *meta;
		int distance = 0;

		/*
		 * 先看左侧对象。如果左侧 metadata 存在且对象仍处于 allocated/RCU freeing
		 * 等可报告状态，就暂定它为 OOB 责任对象。
		 */
		meta = addr_to_metadata(addr - PAGE_SIZE);
		if (meta && kfence_obj_allocated(meta)) {
			to_report = meta;
			/* Data race ok; distance calculation approximate. */
			/*
			 * 中文翻译与补充：距离计算只用于报告“越界多少字节”，允许 data_race()
			 * 读取近似值。即使 size 与 free/realloc 并发变化，报告仍比静默继续更有用。
			 */
			distance = addr - data_race(meta->addr + meta->size);
		}

		/*
		 * 再看右侧对象。若左右都可能归因，选择距离更近的对象，让报告更可能指向
		 * 真正发生 OOB 的分配点。
		 */
		meta = addr_to_metadata(addr + PAGE_SIZE);
		if (meta && kfence_obj_allocated(meta)) {
			/* Data race ok; distance calculation approximate. */
			/*
			 * 中文翻译与补充：这里同样接受近似 data_race()。故障报告路径优先避免
			 * 死锁和递归 fault，而不是为了诊断距离去强行获取另一个对象锁。
			 */
			if (!to_report || distance > data_race(meta->addr) - addr)
				to_report = meta;
		}

		/*
		 * 没找到邻近对象时无法可靠归因为 OOB，跳到 out 生成 INVALID 报告。
		 */
		if (!to_report)
			goto out;

		/*
		 * 找到对象后设置错误类型和需要解除保护的 redzone 页。报告完成后 unprotect
		 * 允许当前访问继续，避免 fault handler 反复处理同一地址。
		 */
		error_type = KFENCE_ERROR_OOB;
		unprotected_page = addr;

		/*
		 * If the object was freed before we took the look we can still
		 * report this as an OOB -- the report will simply show the
		 * stacktrace of the free as well.
		 */
		/*
		 * 中文翻译与补充：即使对象在取锁前被释放，本次 fault 仍发生在 guard page，
		 * 作为 OOB 报告更贴近地址布局事实；free 栈会补充展示释放竞态的另一半证据。
		 */
	} else {
		/*
		 * 阶段 2 的对象页分支：fault 落在对象页而非 redzone，典型含义是对象已被
		 * KFENCE 重新保护后仍被访问，即 use-after-free 或非法访问。
		 */
		to_report = addr_to_metadata(addr);
		if (!to_report)
			goto out;

		error_type = KFENCE_ERROR_UAF;
		/*
		 * We may race with __kfence_alloc(), and it is possible that a
		 * freed object may be reallocated. We simply report this as a
		 * use-after-free, with the stack trace showing the place where
		 * the object was re-allocated.
		 */
		/*
		 * 中文翻译与补充：与 __kfence_alloc() 并发时，freed 对象可能已经重新分配。
		 * KFENCE 仍按 UAF 报告，因为访问保护页说明调用者使用了过期地址；报告中的
		 * realloc 栈能提示“旧指针碰上新对象”的竞态窗口。
		 */
	}

out:
	/*
	 * 阶段 3：在对象锁下固定报告快照。锁保护 unprotected_page、state、alloc/free
	 * track 等字段组合，使报告看到同一对象的一致诊断上下文。
	 */
	if (to_report) {
		raw_spin_lock_irqsave(&to_report->lock, flags);
		to_report->unprotected_page = unprotected_page;
		fault = kfence_report_error(addr, is_write, regs, to_report, error_type);
		raw_spin_unlock_irqrestore(&to_report->lock, flags);
	} else {
		/* This may be a UAF or OOB access, but we can't be sure. */
		/*
		 * 中文翻译与补充：没有 metadata 归因时仍报告 INVALID，表示地址落在 KFENCE
		 * pool 内但无法确认具体对象。调用者得到的是诊断事件，不是可恢复正确性的证明。
		 */
		fault = kfence_report_error(addr, is_write, regs, NULL, KFENCE_ERROR_INVALID);
	}

	/*
	 * 阶段 4：执行报告策略，例如按配置继续、panic 或其它处理。随后解除 fault
	 * 地址所在页保护，让当前访问能够完成，避免同一指令无限重复 page fault。
	 */
	kfence_handle_fault(fault);

	/*
	 * 中文翻译与补充：解除保护并让访问继续。返回值 true 表示 KFENCE 已消费该
	 * fault；unprotect 的结果同时告诉上层是否成功解除保护并可继续，不代表原访问
	 * 是合法内存行为。
	 */
	return kfence_unprotect(addr); /* Unprotect and let access proceed. */
}
