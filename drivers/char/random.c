// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Linux 内核源码学习注释，生成模型：GPT-5 Codex，日期：2026-07-28。
 *
 * 本文件实现内核通用随机数子系统的核心路径：外部事件、硬件 RNG、启动参数、
 * 中断/输入/磁盘时序等来源先被混入 input_pool；在可信熵位数达到阈值后，
 * input_pool 派生出 base_crng 密钥；随后每 CPU crng 和批量整数缓存使用
 * ChaCha20 扩展出供内核、getrandom(2)、/dev/random、/dev/urandom 和
 * sysctl 接口消费的随机字节。
 *
 * 阅读地图：
 *   初始化与等待：rng_is_initialized()、wait_for_random_bytes()、
 *     execute_with_initialized_rng() 负责 CRNG_READY 状态观察、阻塞等待和通知。
 *   CRNG 扩展器：crng_reseed()、crng_make_state()、_get_random_bytes()、
 *     get_random_bytes_user() 把熵池输出转换成快速密钥擦除的 ChaCha20 流。
 *   熵池：mix_pool_bytes() 只混入数据；credit_init_bits() 才增加“可计入初始化”
 *     的位数；extract_entropy() 以 BLAKE2s/HKDF-like 方式从池中提取种子。
 *   熵采集：启动、PM、硬件 RNG、VM fork、中断、输入和磁盘路径分别提供
 *     不同质量的数据，只有受信任或有估算依据的来源会 credit。
 *   用户接口：getrandom(2) 是现代首选接口，/dev/random 等价于阻塞读，
 *     /dev/urandom 等价于 GRND_INSECURE 的非阻塞早期读。
 *   sysctl：主要保留历史 ABI，部分 knob 可写但不再改变 RNG 行为。
 *
 * 并发模型：
 *   base_crng.lock 保护全局 CRNG key、generation 和 crng_init 单调状态转换；
 *   input_pool.lock 保护 BLAKE2s 哈希上下文，init_bits 通过 cmpxchg 单调累加；
 *   per-CPU crng 和 batched_entropy 使用 local_lock，避免同一 CPU 上中断/抢占
 *   路径并发消费同一密钥或缓存槽；等待者通过 crng_init_wait、fasync 和
 *   notifier 在 CRNG_READY 发布后被唤醒。
 *
 * 关键安全不变量：
 *   混入数据不等于计入熵；credit 只发生在调用者明确知道质量或代码有保守估算时。
 *   CRNG_READY 之后读取路径不会回退为未初始化；generation 变化通知 per-CPU
 *   缓存必须重新取种；临时密钥材料使用 memzero_explicit()/chacha_zeroize_state()
 *   清除，防止长时间残留破坏前向安全。
 */
/*
 * Copyright (C) 2017-2024 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 * Copyright Matt Mackall <mpm@selenic.com>, 2003, 2004, 2005
 * Copyright Theodore Ts'o, 1994, 1995, 1996, 1997, 1998, 1999. All rights reserved.
 *
 * This driver produces cryptographically secure pseudorandom data. It is divided
 * into roughly six sections, each with a section header:
 *
 *   - Initialization and readiness waiting.
 *   - Fast key erasure RNG, the "crng".
 *   - Entropy accumulation and extraction routines.
 *   - Entropy collection routines.
 *   - Userspace reader/writer interfaces.
 *   - Sysctl interface.
 *
 * The high level overview is that there is one input pool, into which
 * various pieces of data are hashed. Prior to initialization, some of that
 * data is then "credited" as having a certain number of bits of entropy.
 * When enough bits of entropy are available, the hash is finalized and
 * handed as a key to a stream cipher that expands it indefinitely for
 * various consumers. This key is periodically refreshed as the various
 * entropy collectors, described below, add data to the input pool.
 */
/*
 * 上述英文总览说明：本驱动生成密码学安全的伪随机数据，并按六个章节组织：
 * 初始化/等待、快速密钥擦除 CRNG、熵积累与提取、熵采集、用户态接口和 sysctl。
 * 宏观设计是“一个输入池 + 一个可扩展输出器”：所有来源先被哈希进 input_pool；
 * 初始化前，只有部分来源会被 credit 为具有若干位熵；达到阈值后，哈希结果被
 * 提交为流密码密钥，流密码再为不同消费者无限扩展随机字节。后续采集器继续
 * 混入数据，并通过周期性 reseed 刷新密钥。
 *
 * 学习时要区分三件事：
 *   1. mix：改变哈希状态，让不同机器/不同时间的池状态分叉；
 *   2. credit：增加 init_bits，推动 CRNG_EMPTY → CRNG_EARLY → CRNG_READY；
 *   3. extract/reseed：把池状态转换成 CRNG key，并让输出路径观察到新 generation。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/percpu.h>
#include <linux/ptrace.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/ratelimit.h>
#include <linux/syscalls.h>
#include <linux/completion.h>
#include <linux/uuid.h>
#include <linux/uaccess.h>
#include <linux/suspend.h>
#include <linux/siphash.h>
#include <linux/sched/isolation.h>
#include <crypto/chacha.h>
#include <crypto/blake2s.h>
#include <vdso/datapage.h>
#include <asm/archrandom.h>
#include <asm/processor.h>
#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/io.h>

/*********************************************************************
 *
 * Initialization and readiness waiting.
 *
 * Much of the RNG infrastructure is devoted to various dependencies
 * being able to wait until the RNG has collected enough entropy and
 * is ready for safe consumption.
 *
 *********************************************************************/
/*
 * 本节处理“什么时候随机数可被安全消费”的状态发布问题。早期启动时很多内核代码
 * 已经需要随机数，但真正足够的熵可能尚未到达，因此这里同时提供：
 *   - 低成本查询：crng_ready()/rng_is_initialized()；
 *   - 可中断等待：wait_for_random_bytes()；
 *   - 一次性通知：execute_with_initialized_rng()、random_ready_notifier；
 *   - 用户态异步通知：fasync 和 poll。
 */

/*
 * crng_init is protected by base_crng->lock, and only increases
 * its value (from empty->early->ready).
 */
/*
 * crng_init 由 base_crng.lock 保护，只允许单调前进。它不是“池里有什么数据”
 * 的完整描述，而是消费端可见的安全等级承诺：
 *   CRNG_EMPTY：最多只有少量不可计入或不足量的输入，安全消费者应等待；
 *   CRNG_EARLY：已有半池熵，允许早期 CRNG key 初始化，但仍未对外宣布就绪；
 *   CRNG_READY：达到 POOL_READY_BITS，getrandom(0)、/dev/random 和等待者可继续。
 * __read_mostly 表示就绪后读远多于写；实际快路径还会通过 static key 把
 * crng_ready() 优化成几乎零开销。
 */
static enum {
	CRNG_EMPTY = 0, /* Little to no entropy collected */
	CRNG_EARLY = 1, /* At least POOL_EARLY_BITS collected */
	CRNG_READY = 2  /* Fully initialized with POOL_READY_BITS collected */
} crng_init __read_mostly = CRNG_EMPTY;
static DEFINE_STATIC_KEY_FALSE(crng_is_ready);
#define crng_ready() (static_branch_likely(&crng_is_ready) || crng_init >= CRNG_READY)
/* Various types of waiters for crng_init->CRNG_READY transition. */
/*
 * 等待者分三类：睡眠等待队列服务 wait_for_random_bytes()/poll；fasync 服务
 * /dev/random 的 SIGIO；notifier 服务内核子系统注册回调。它们都围绕同一个
 * 发布边界：_credit_init_bits() 首次把 init_bits 推到 POOL_READY_BITS。
 */
static DECLARE_WAIT_QUEUE_HEAD(crng_init_wait);
static struct fasync_struct *fasync;
static ATOMIC_NOTIFIER_HEAD(random_ready_notifier);

/* Control how we warn userspace. */
/*
 * /dev/urandom 在未初始化阶段仍允许读，但会警告。ratelimit 避免早期用户空间
 * 大量读取时刷屏；ratelimit_disable 是模块参数，允许调试时关闭抑制。
 */
static struct ratelimit_state urandom_warning =
	RATELIMIT_STATE_INIT_FLAGS("urandom_warning", HZ, 3, RATELIMIT_MSG_ON_RELEASE);
static int ratelimit_disable __read_mostly = 0;
module_param_named(ratelimit_disable, ratelimit_disable, int, 0644);
MODULE_PARM_DESC(ratelimit_disable, "Disable random ratelimit suppression");

/*
 * Returns whether or not the input pool has been seeded and thus guaranteed
 * to supply cryptographically secure random numbers. This applies to: the
 * /dev/urandom device, the get_random_bytes function, and the get_random_{u8,
 * u16,u32,u64,long} family of functions.
 *
 * Returns: true if the input pool has been seeded.
 *          false if the input pool has not been seeded.
 */
/*
 * rng_is_initialized() - 查询 RNG 是否达到密码学安全消费状态
 *
 * 调用位置：内核调用者在决定是否等待、是否推迟操作、或是否走降级路径前使用。
 * 入参：无。
 * 锁/上下文：不获取睡眠锁，可在普通上下文和较敏感路径中低成本调用。
 * 返回：true 表示 CRNG_READY 已发布；false 表示调用者若需要安全保证应等待
 * wait_for_random_bytes() 或改用允许不安全早期输出的接口。
 * 副作用：无；只是观察 static key 或 crng_init 状态。
 */
bool rng_is_initialized(void)
{
	return crng_ready();
}
EXPORT_SYMBOL(rng_is_initialized);

/*
 * crng_set_ready() - 打开 crng_ready() 的 static key 快路径
 *
 * 调用位置：_credit_init_bits() 在 workqueue 可用时排队执行，random_init() 也会在
 * early 阶段已就绪但 static key 尚未开启时直接调用。
 * 入参：@work 可为 NULL；本函数不使用该参数，只满足 workqueue 回调签名。
 * 上下文：__cold 路径，只在初始化完成附近执行；static_branch_enable() 会修改
 * 跳转标签，之后大量 crng_ready() 读者不再反复检查普通变量。
 * 返回：无直接返回值；副作用是全局 static key 从 false 切换为 true。
 */
static void __cold crng_set_ready(struct work_struct *work)
{
	static_branch_enable(&crng_is_ready);
}

/* Used by wait_for_random_bytes(), and considered an entropy collector, below. */
/*
 * try_to_generate_entropy() 的前置声明服务 wait_for_random_bytes()：等待者不能只是
 * 睡眠，还会主动运行基于定时器抖动的熵采集尝试，从而避免在缺少外部事件的早期
 * 启动阶段永久等待。
 */
static void try_to_generate_entropy(void);

/*
 * Wait for the input pool to be seeded and thus guaranteed to supply
 * cryptographically secure random numbers. This applies to: the /dev/urandom
 * device, the get_random_bytes function, and the get_random_{u8,u16,u32,u64,
 * long} family of functions. Using any of these functions without first
 * calling this function forfeits the guarantee of security.
 *
 * Returns: 0 if the input pool has been seeded.
 *          -ERESTARTSYS if the function was interrupted by a signal.
 */
/*
 * wait_for_random_bytes() - 等待 CRNG_READY 并为安全随机数建立前置保证
 *
 * 调用位置：需要阻塞等待安全随机数的内核路径，以及 getrandom(2) 的阻塞模式。
 * 入参：无。
 * 锁/上下文：必须允许睡眠；内部会等待 crng_init_wait，且 try_to_generate_entropy()
 * 可能分配 cpumask、调度 timer 并 schedule()。
 * 阶段：
 *   1. 若尚未 ready，主动尝试从计时抖动生成熵；
 *   2. 以 1 秒 timeout 等待 CRNG_READY，timeout 后重新尝试；
 *   3. 信号中断时向调用者返回可重启错误。
 * 返回：0 表示 ready 已观察到；负值通常为 -ERESTARTSYS；正 wait 返回被折叠为 0。
 * 成功副作用：无额外引用或锁转移；只是保证返回时后续 get_random_bytes() 等接口
 * 满足“已初始化后使用”的安全前提。
 */
int wait_for_random_bytes(void)
{
	while (!crng_ready()) {
		int ret;

		/*
		 * 等待前先主动推进熵收集：如果系统有可用 cycle counter，
		 * try_to_generate_entropy() 会用跨 CPU timer 抖动把 init_bits
		 * 推向 ready；如果不可行，则很快返回，再进入等待队列。
		 */
		try_to_generate_entropy();
		ret = wait_event_interruptible_timeout(crng_init_wait, crng_ready(), HZ);
		if (ret)
			return ret > 0 ? 0 : ret;
	}
	return 0;
}
EXPORT_SYMBOL(wait_for_random_bytes);

/*
 * Add a callback function that will be invoked when the crng is initialised,
 * or immediately if it already has been. Only use this is you are absolutely
 * sure it is required. Most users should instead be able to test
 * `rng_is_initialized()` on demand, or make use of `get_random_bytes_wait()`.
 */
/*
 * execute_with_initialized_rng() - 注册或立即执行 CRNG_READY 回调
 *
 * 调用位置：少数内核子系统需要“RNG 就绪后立刻做一次事”时使用。多数调用者
 * 更应按需查询 rng_is_initialized() 或直接等待。
 * @nb: 调用者提供的 notifier_block；调用者保持其存储期有效，注册后由
 * random_ready_notifier 链表借用，不取得额外对象所有权。
 * 锁/上下文：获取 random_ready_notifier.lock 并关闭本地中断，避免与 ready
 * 发布路径的 atomic_notifier_call_chain()/注册操作并发破坏链表。
 * 返回：0 表示已经立即调用或注册成功；负 errno 来自 raw_notifier_chain_register()。
 * 竞态处理：持锁后再次检查 crng_ready()，保证“检查 ready”和“挂入链表”之间
 * 不会漏掉一次 ready 发布。
 */
int __cold execute_with_initialized_rng(struct notifier_block *nb)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&random_ready_notifier.lock, flags);
	if (crng_ready())
		/*
		 * 已 ready 时在锁内同步调用，调用者不会再进入链表，也无需等待
		 * 未来事件。回调不能假设自己由 workqueue 执行。
		 */
		nb->notifier_call(nb, 0, NULL);
	else
		ret = raw_notifier_chain_register((struct raw_notifier_head *)&random_ready_notifier.head, nb);
	spin_unlock_irqrestore(&random_ready_notifier.lock, flags);
	return ret;
}

/*********************************************************************
 *
 * Fast key erasure RNG, the "crng".
 *
 * These functions expand entropy from the entropy extractor into
 * long streams for external consumption using the "fast key erasure"
 * RNG described at <https://blog.cr.yp.to/20170723-random.html>.
 *
 * There are a few exported interfaces for use by other drivers:
 *
 *	void get_random_bytes(void *buf, size_t len)
 *	u8 get_random_u8()
 *	u16 get_random_u16()
 *	u32 get_random_u32()
 *	u32 get_random_u32_below(u32 ceil)
 *	u32 get_random_u32_above(u32 floor)
 *	u32 get_random_u32_inclusive(u32 floor, u32 ceil)
 *	u64 get_random_u64()
 *	unsigned long get_random_long()
 *
 * These interfaces will return the requested number of random bytes
 * into the given buffer or as a return value. This is equivalent to
 * a read from /dev/urandom. The u8, u16, u32, u64, long family of
 * functions may be higher performance for one-off random integers,
 * because they do a bit of buffering and do not invoke reseeding
 * until the buffer is emptied.
 *
 *********************************************************************/
/*
 * 本节是输出扩展器。input_pool 只负责把不可预测输入压缩成种子；真正高吞吐
 * 输出由 CRNG 完成：
 *   base_crng 是全局根密钥和 generation 源；
 *   每 CPU crng 从 base_crng 派生本地 key，减少全局锁竞争；
 *   每次输出都执行 fast key erasure：先产出 ChaCha block，再用 block 前半
 *   覆盖旧 key，从而即使之后 key 泄露，也难以恢复此前输出。
 */

enum {
	CRNG_RESEED_START_INTERVAL = HZ,
	CRNG_RESEED_INTERVAL = 60 * HZ
};

/*
 * base_crng - 全局 CRNG 根状态
 *
 * @key: 从 input_pool 提取出的全局根密钥。只有持有 base_crng.lock 的 reseed
 *       或 per-CPU 更新路径可以读写。
 * @generation: 单调增长的版本号；每 CPU crng 和 batched_entropy 通过比较它
 *       判断本地缓存是否过期。ULONG_MAX 保留为 per-CPU 初始无效值。
 * @lock: 保护 key、generation 和 crng_init 的一致性。持锁区必须短，避免
 *       随机数热路径在全局锁上长期排队。
 */
static struct {
	u8 key[CHACHA_KEY_SIZE] __aligned(__alignof__(long));
	unsigned long generation;
	spinlock_t lock;
} base_crng = {
	.lock = __SPIN_LOCK_UNLOCKED(base_crng.lock)
};

/*
 * struct crng - 每 CPU ChaCha20 扩展器状态
 *
 * @key: 本 CPU 当前输出密钥，从 base_crng 通过 fast key erasure 派生。
 * @generation: 该 key 对应的 base_crng.generation；不匹配时必须先刷新。
 * @lock: local_lock 保护同一 CPU 上进程上下文、软中断/硬中断等并发消费者，
 *        防止两个路径同时使用并擦除同一个 per-CPU key。
 */
struct crng {
	u8 key[CHACHA_KEY_SIZE];
	unsigned long generation;
	local_lock_t lock;
};

static DEFINE_PER_CPU(struct crng, crngs) = {
	.generation = ULONG_MAX,
	.lock = INIT_LOCAL_LOCK(crngs.lock),
};

/*
 * Return the interval until the next reseeding, which is normally
 * CRNG_RESEED_INTERVAL, but during early boot, it is at an interval
 * proportional to the uptime.
 */
/*
 * crng_reseed_interval() - 计算下一次自动 reseed 的延迟
 *
 * 调用位置：crng_reseed() 安排 delayed work，以及硬件 RNG 写入节流。
 * 入参：无。
 * 锁/上下文：不睡眠；early_boot 使用 READ_ONCE/WRITE_ONCE 是为了避免并发
 * 读取/写入普通 bool 的数据竞争告警，不提供复杂同步语义。
 * 返回：早期启动阶段返回从 1 秒开始、随 uptime 增长的间隔；系统运行足够久后
 * 固定为 60 秒。这样早期熵变化更频繁地进入 CRNG，稳定后降低开销。
 */
static unsigned int crng_reseed_interval(void)
{
	static bool early_boot = true;

	if (unlikely(READ_ONCE(early_boot))) {
		time64_t uptime = ktime_get_seconds();
		/*
		 * early_boot 是一次性退火开关：uptime 足够大后写成 false，
		 * 之后所有 CPU 都走固定间隔。这里不需要锁，因为多次写 false
		 * 等价，最坏只是某个调用多用一次早期间隔。
		 */
		if (uptime >= CRNG_RESEED_INTERVAL / HZ * 2)
			WRITE_ONCE(early_boot, false);
		else
			return max_t(unsigned int, CRNG_RESEED_START_INTERVAL,
				     (unsigned int)uptime / 2 * HZ);
	}
	return CRNG_RESEED_INTERVAL;
}

/* Used by crng_reseed() and crng_make_state() to extract a new seed from the input pool. */
/*
 * extract_entropy() 从 input_pool 派生 seed。这里前置声明是为了让 CRNG 层在
 * 文件物理顺序上先描述输出器，再在下一节给出熵池实现；调用者获得的是写入
 * @buf 的字节，不取得 input_pool 内部状态所有权。
 */
static void extract_entropy(void *buf, size_t len);

/* This extracts a new crng key from the input pool. */
/*
 * crng_reseed() - 从 input_pool 提取新根密钥并发布新 generation
 *
 * 调用位置：初始化 ready、周期性 delayed work、PM resume、VM fork 和 ioctl
 * RNDRESEEDCRNG 等路径。
 * @work: delayed work 回调参数；直接调用时可为 NULL，函数不依赖其内容。
 * 锁/上下文：extract_entropy() 会获取 input_pool.lock；随后获取 base_crng.lock。
 * 直接调用者必须处于可执行这些短临界区的上下文。函数本身不把任何引用转移给调用者。
 * 阶段：
 *   1. 先安排下一次 reseed，避免本次处理较晚导致周期继续后移；
 *   2. 在栈上提取新 key；
 *   3. 持 base_crng.lock 提交 key 和 generation；
 *   4. 如启用 vDSO getrandom，用 release store 发布 generation；
 *   5. 清除栈上 key。
 * 返回：无直接返回值；副作用是全局输出密钥更新，per-CPU 缓存通过 generation
 * 观察到过期并在下次使用时刷新。
 */
static void crng_reseed(struct work_struct *work)
{
	static DECLARE_DELAYED_WORK(next_reseed, crng_reseed);
	unsigned long flags;
	unsigned long next_gen;
	u8 key[CHACHA_KEY_SIZE];

	/* Immediately schedule the next reseeding, so that it fires sooner rather than later. */
	/*
	 * 英文注释说明“立刻安排下一次 reseed”。这样周期以本次开始时间为基准，
	 * 不会因为 extract_entropy() 或锁竞争延迟而不断漂移。system_dfl_wq 在
	 * very early boot 可能尚未创建，因此需要判空。
	 */
	if (likely(system_dfl_wq))
		queue_delayed_work(system_dfl_wq, &next_reseed, crng_reseed_interval());

	extract_entropy(key, sizeof(key));

	/*
	 * We copy the new key into the base_crng, overwriting the old one,
	 * and update the generation counter. We avoid hitting ULONG_MAX,
	 * because the per-cpu crngs are initialized to ULONG_MAX, so this
	 * forces new CPUs that come online to always initialize.
	 */
	/*
	 * 提交阶段必须在 base_crng.lock 下完成：key 与 generation 是一个原子
	 * 逻辑版本。若先更新 generation 再拷贝 key，其他 CPU 可能认为本地缓存
	 * 已过期并派生到半更新 key；若不跳过 ULONG_MAX，新上线 CPU 的初始哨兵
	 * 可能误判为已经同步。
	 */
	spin_lock_irqsave(&base_crng.lock, flags);
	memcpy(base_crng.key, key, sizeof(base_crng.key));
	next_gen = base_crng.generation + 1;
	if (next_gen == ULONG_MAX)
		++next_gen;
	WRITE_ONCE(base_crng.generation, next_gen);

	/* base_crng.generation's invalid value is ULONG_MAX, while
	 * vdso_k_rng_data->generation's invalid value is 0, so add one to the
	 * former to arrive at the latter. Use smp_store_release so that this
	 * is ordered with the write above to base_crng.generation. Pairs with
	 * the smp_rmb() before the syscall in the vDSO code.
	 *
	 * Cast to unsigned long for 32-bit architectures, since atomic 64-bit
	 * operations are not supported on those architectures. This is safe
	 * because base_crng.generation is a 32-bit value. On big-endian
	 * architectures it will be stored in the upper 32 bits, but that's okay
	 * because the vDSO side only checks whether the value changed, without
	 * actually using or interpreting the value.
	 */
	/*
	 * vDSO getrandom 在用户态缓存 RNG 状态，需要知道内核 generation 是否变化。
	 * smp_store_release() 保证内核先完成 base_crng.generation/key 的更新，再让
	 * vDSO 读者看到新的 vdso generation；用户态侧的屏障在进入 syscall 前配对，
	 * 避免看到“版本已变”却仍基于旧状态做判断。
	 */
	if (IS_ENABLED(CONFIG_VDSO_GETRANDOM))
		smp_store_release((unsigned long *)&vdso_k_rng_data->generation, next_gen + 1);

	/*
	 * ready 状态也在 base_crng.lock 下单调提交。static branch 可能稍后由
	 * crng_set_ready() 打开；在此之前 crng_ready() 仍可通过 crng_init >=
	 * CRNG_READY 观察到就绪，不会丢失语义。
	 */
	if (!static_branch_likely(&crng_is_ready))
		crng_init = CRNG_READY;
	spin_unlock_irqrestore(&base_crng.lock, flags);
	memzero_explicit(key, sizeof(key));
}

/*
 * This generates a ChaCha block using the provided key, and then
 * immediately overwrites that key with half the block. It returns
 * the resultant ChaCha state to the user, along with the second
 * half of the block containing 32 bytes of random data that may
 * be used; random_data_len may not be greater than 32.
 *
 * The returned ChaCha state contains within it a copy of the old
 * key value, at index 4, so the state should always be zeroed out
 * immediately after using in order to maintain forward secrecy.
 * If the state cannot be erased in a timely manner, then it is
 * safer to set the random_data parameter to &chacha_state->x[4]
 * so that this function overwrites it before returning.
 */
/*
 * crng_fast_key_erasure() - 用一个 ChaCha block 同时产出随机数并擦除旧 key
 *
 * @key: 输入/输出参数。入口是调用者持有的 32 字节 CRNG key；返回时已被
 *       first_block 前半覆盖。调用者必须已通过 base_crng.lock 或 local_lock
 *       排除并发使用同一 key。
 * @chacha_state: 输出参数。返回后包含可继续生成后续 block 的 ChaCha 状态；
 *       其中 x[4..11] 曾保存旧 key，调用者用完必须 zeroize。
 * @random_data: 输出缓冲区，可与 chacha_state->x[4] 重叠以立刻覆盖旧 key 副本。
 * @random_data_len: 本次直接返回的字节数，范围 0..32。
 * 返回：无直接返回值；副作用是 key 被不可逆推进，random_data 被填充。
 * 安全意图：攻击者若稍后获得当前 key，只能推断之后输出，不能恢复已经用来
 * 覆盖 key 的旧 block 和更早输出。
 */
static void crng_fast_key_erasure(u8 key[CHACHA_KEY_SIZE],
				  struct chacha_state *chacha_state,
				  u8 *random_data, size_t random_data_len)
{
	u8 first_block[CHACHA_BLOCK_SIZE];

	BUG_ON(random_data_len > 32);

	/*
	 * 构造 ChaCha20 初始状态：常量、32 字节 key、全零 counter/nonce。这里
	 * 不是为网络协议生成带 nonce 的流，而是把 ChaCha 当作内部 PRF 使用。
	 */
	chacha_init_consts(chacha_state);
	memcpy(&chacha_state->x[4], key, CHACHA_KEY_SIZE);
	memset(&chacha_state->x[12], 0, sizeof(u32) * 4);
	chacha20_block(chacha_state, first_block);

	/*
	 * first_block 前 32 字节成为下一代 key，后 32 字节可作为本次随机输出。
	 * 这就是 fast key erasure 的提交点；旧 key 不再保留在 @key 指向的存储。
	 */
	memcpy(key, first_block, CHACHA_KEY_SIZE);
	memcpy(random_data, first_block + CHACHA_KEY_SIZE, random_data_len);
	memzero_explicit(first_block, sizeof(first_block));
}

/*
 * This function returns a ChaCha state that you may use for generating
 * random data. It also returns up to 32 bytes on its own of random data
 * that may be used; random_data_len may not be greater than 32.
 */
/*
 * crng_make_state() - 为调用者准备可继续输出的 ChaCha 状态
 *
 * 调用位置：_get_random_bytes() 和 get_random_bytes_user() 的每次读取起点。
 * @chacha_state: 输出参数；返回后调用者拥有该栈上状态，负责尽快清零。
 * @random_data: 输出参数；接收 fast key erasure 首 block 后半最多 32 字节。
 * @random_data_len: 需要直接写入 random_data 的字节数，范围 0..32。
 * 锁/上下文：可能获取 base_crng.lock 和本 CPU crng local_lock；不睡眠。
 * 主要路径：
 *   - 未 ready：直接在 base_crng 上做早期 fast key erasure，必要时先从熵池提取；
 *   - ready 且 per-CPU generation 过期：持全局锁派生新 per-CPU key；
 *   - ready 且本地新鲜：只持 local_lock 使用本地 key，避免全局竞争。
 * 返回：无直接返回值；输出 chacha_state/random_data，并推进被使用的 key。
 */
static void crng_make_state(struct chacha_state *chacha_state,
			    u8 *random_data, size_t random_data_len)
{
	unsigned long flags;
	struct crng *crng;

	BUG_ON(random_data_len > 32);

	/*
	 * For the fast path, we check whether we're ready, unlocked first, and
	 * then re-check once locked later. In the case where we're really not
	 * ready, we do fast key erasure with the base_crng directly, extracting
	 * when crng_init is CRNG_EMPTY.
	 */
	/*
	 * 未 ready 早期路径必须谨慎：这里提供的是“尽力而为”的输出，用于
	 * GRND_INSECURE、早期内核调用或等待过程中的内部推进，不向阻塞安全接口
	 * 承诺已经有足量熵。加锁后重查 crng_ready()，避免检查和取锁之间 ready
	 * 已发布却仍按早期路径消耗 base_crng。
	 */
	if (!crng_ready()) {
		bool ready;

		spin_lock_irqsave(&base_crng.lock, flags);
		ready = crng_ready();
		if (!ready) {
			if (crng_init == CRNG_EMPTY)
				/*
				 * CRNG_EMPTY 时 base_crng.key 尚未初始化为熵池输出。
				 * 在同一把 base_crng.lock 下提取并使用，保证多个早期
				 * 消费者不会同时从同一个初始 key 派生。
				 */
				extract_entropy(base_crng.key, sizeof(base_crng.key));
			crng_fast_key_erasure(base_crng.key, chacha_state,
					      random_data, random_data_len);
		}
		spin_unlock_irqrestore(&base_crng.lock, flags);
		if (!ready)
			return;
	}

	local_lock_irqsave(&crngs.lock, flags);
	crng = raw_cpu_ptr(&crngs);

	/*
	 * If our per-cpu crng is older than the base_crng, then it means
	 * somebody reseeded the base_crng. In that case, we do fast key
	 * erasure on the base_crng, and use its output as the new key
	 * for our per-cpu crng. This brings us up to date with base_crng.
	 */
	/*
	 * 本地 generation 不匹配表示根密钥已经 reseed。刷新时同时持有本地
	 * local_lock 和 base_crng.lock：前者排除同 CPU 消费者，后者稳定全局
	 * key/generation 对。派生过程也会擦除 base_crng.key，使全局 key 前进。
	 */
	if (unlikely(crng->generation != READ_ONCE(base_crng.generation))) {
		spin_lock(&base_crng.lock);
		crng_fast_key_erasure(base_crng.key, chacha_state,
				      crng->key, sizeof(crng->key));
		crng->generation = base_crng.generation;
		spin_unlock(&base_crng.lock);
	}

	/*
	 * Finally, when we've made it this far, our per-cpu crng has an up
	 * to date key, and we can do fast key erasure with it to produce
	 * some random data and a ChaCha state for the caller. All other
	 * branches of this function are "unlikely", so most of the time we
	 * should wind up here immediately.
	 */
	/*
	 * 热路径只操作本 CPU key：生成调用者的 chacha_state 和最多 32 字节直接输出，
	 * 同时把 per-CPU key 推进到下一代。local_unlock 后其他本 CPU 路径看到的
	 * 已经是擦除后的 key。
	 */
	crng_fast_key_erasure(crng->key, chacha_state, random_data, random_data_len);
	local_unlock_irqrestore(&crngs.lock, flags);
}

/*
 * _get_random_bytes() - 内核内部随机字节生成核心
 *
 * @buf: 输出缓冲区，调用者拥有并保证可写；本函数不保留指针。
 * @len: 请求字节数，可为 0；无上限语义但大输出会循环生成多个 ChaCha block。
 * 调用位置：get_random_bytes()、batched entropy refill 和未 ready 的整数接口。
 * 锁/上下文：不睡眠；内部短暂获取 CRNG 相关锁。
 * 返回：无直接返回值；副作用是填充 @buf，并推进 CRNG key/state。
 * 安全边界：本函数不等待 CRNG_READY，调用者若需要安全保证必须先调用
 * wait_for_random_bytes() 或来自 getrandom 阻塞路径。
 */
static void _get_random_bytes(void *buf, size_t len)
{
	struct chacha_state chacha_state;
	u8 tmp[CHACHA_BLOCK_SIZE];
	size_t first_block_len;

	if (!len)
		return;

	/*
	 * 首 block 后半最多 32 字节由 crng_make_state() 直接给出；剩余部分使用
	 * 返回的 detached ChaCha state 继续扩展。这样一次调用开始时就完成 key
	 * erasure，不必等全部用户缓冲区写完。
	 */
	first_block_len = min_t(size_t, 32, len);
	crng_make_state(&chacha_state, buf, first_block_len);
	len -= first_block_len;
	buf += first_block_len;

	while (len) {
		/*
		 * 尾部不足一个 block 时先写到栈上 tmp，再只复制调用者需要的长度。
		 * tmp 可能包含额外随机字节，不能留在栈上，因此随后显式清零。
		 */
		if (len < CHACHA_BLOCK_SIZE) {
			chacha20_block(&chacha_state, tmp);
			memcpy(buf, tmp, len);
			memzero_explicit(tmp, sizeof(tmp));
			break;
		}

		/*
		 * 整 block 可直接输出到调用者缓冲区。x[12] 是 ChaCha counter 低位；
		 * 溢出时手动推进高位 x[13]，避免长输出重复 block。
		 */
		chacha20_block(&chacha_state, buf);
		if (unlikely(chacha_state.x[12] == 0))
			++chacha_state.x[13];
		len -= CHACHA_BLOCK_SIZE;
		buf += CHACHA_BLOCK_SIZE;
	}

	chacha_zeroize_state(&chacha_state);
}

/*
 * This returns random bytes in arbitrary quantities. The quality of the
 * random bytes is as good as /dev/urandom. In order to ensure that the
 * randomness provided by this function is okay, the function
 * wait_for_random_bytes() should be called and return 0 at least once
 * at any point prior.
 */
/*
 * get_random_bytes() - 导出的内核随机字节接口
 *
 * @buf: 调用者提供的可写内核缓冲区；本函数只写入，不取得所有权。
 * @len: 需要填充的字节数；0 表示空操作。
 * 调用位置：内核子系统常用 API，语义等价于读取 /dev/urandom。
 * 返回：无直接返回值。调用者若需要“已初始化后安全输出”的保证，必须在任意
 * 先前时刻成功等待 wait_for_random_bytes()；本 wrapper 自身不阻塞等待。
 */
void get_random_bytes(void *buf, size_t len)
{
	_get_random_bytes(buf, len);
}
EXPORT_SYMBOL(get_random_bytes);

/*
 * get_random_bytes_user() - 将随机字节复制到 iov_iter 描述的用户/迭代器目标
 *
 * @iter: 输入/输出参数；入口描述目标缓冲区和剩余长度，返回时推进已复制位置。
 * 调用位置：getrandom(2)、/dev/random 和 /dev/urandom 的读路径。
 * 上下文：copy_to_iter() 可能触发缺页并睡眠；因此函数先执行 key erasure，
 * 避免用户态让复制长时间卡住时旧 key 副本留在 chacha_state 中。
 * 返回：成功返回复制字节数，可能小于请求长度；若一个字节也未复制则返回 -EFAULT。
 * 副作用：推进 CRNG 状态，清除临时 block 和 chacha_state。
 */
static ssize_t get_random_bytes_user(struct iov_iter *iter)
{
	struct chacha_state chacha_state;
	u8 block[CHACHA_BLOCK_SIZE];
	size_t ret = 0, copied;

	if (unlikely(!iov_iter_count(iter)))
		return 0;

	/*
	 * Immediately overwrite the ChaCha key at index 4 with random
	 * bytes, in case userspace causes copy_to_iter() below to sleep
	 * forever, so that we still retain forward secrecy in that case.
	 */
	/*
	 * 将 random_data 指向 chacha_state.x[4]：crng_fast_key_erasure()
	 * 返回前会用随机输出覆盖 state 中保存旧 key 的位置。即使后续 copy_to_iter()
	 * 被用户缺页拖住，栈上的旧 key 也已经不再可见。
	 */
	crng_make_state(&chacha_state, (u8 *)&chacha_state.x[4],
			CHACHA_KEY_SIZE);
	/*
	 * However, if we're doing a read of len <= 32, we don't need to
	 * use chacha_state after, so we can simply return those bytes to
	 * the user directly.
	 */
	/*
	 * 小读可以直接复制 x[4] 中的 32 字节候选输出，并在 out_zero_chacha 统一
	 * 清理。copy_to_iter() 会按 iter 剩余长度推进，不会把超出请求的字节暴露给用户。
	 */
	if (iov_iter_count(iter) <= CHACHA_KEY_SIZE) {
		ret = copy_to_iter(&chacha_state.x[4], CHACHA_KEY_SIZE, iter);
		goto out_zero_chacha;
	}

	for (;;) {
		/*
		 * 大读按 ChaCha block 流式输出。每页边界检查信号并 cond_resched()，
		 * 防止巨大读请求在内核态长时间占用 CPU，同时保留“已复制多少就返回多少”
		 * 的 Unix read 语义。
		 */
		chacha20_block(&chacha_state, block);
		if (unlikely(chacha_state.x[12] == 0))
			++chacha_state.x[13];

		copied = copy_to_iter(block, sizeof(block), iter);
		ret += copied;
		if (!iov_iter_count(iter) || copied != sizeof(block))
			break;

		BUILD_BUG_ON(PAGE_SIZE % sizeof(block) != 0);
		if (ret % PAGE_SIZE == 0) {
			if (signal_pending(current))
				break;
			cond_resched();
		}
	}

	memzero_explicit(block, sizeof(block));
out_zero_chacha:
	chacha_zeroize_state(&chacha_state);
	return ret ? ret : -EFAULT;
}

/*
 * Batched entropy returns random integers. The quality of the random
 * number is as good as /dev/urandom. In order to ensure that the randomness
 * provided by this function is okay, the function wait_for_random_bytes()
 * should be called and return 0 at least once at any point prior.
 */
/*
 * 批量整数接口用宏生成 u8/u16/u32/u64 四套 per-CPU 缓存。它优化的是
 * “偶发取一个整数”的热路径：一次 refill 生成 1.5 个 ChaCha block 的随机整数，
 * 后续调用只从本 CPU 数组取一个槽位，并立即把槽位置零。generation 不匹配时
 * 丢弃旧缓存，避免 reseed 后继续消费旧代随机数。
 */

#define DEFINE_BATCHED_ENTROPY(type)						\
struct batch_ ##type {								\
	/*									\
	 * We make this 1.5x a ChaCha block, so that we get the			\
	 * remaining 32 bytes from fast key erasure, plus one full		\
	 * block from the detached ChaCha state. We can increase		\
	 * the size of this later if needed so long as we keep the		\
	 * formula of (integer_blocks + 0.5) * CHACHA_BLOCK_SIZE.		\
	 */									\
	/*									\
	 * entropy[] 是本 CPU 的随机整数缓存；数组大小选择为 1.5 个		\
	 * ChaCha block，使 fast key erasure 直接给出的 32 字节和随后	\
	 * 一个完整 block 都被利用。lock 保护同 CPU 并发消费者；		\
	 * generation 记录 refill 时的 base_crng 版本；position 是下一个	\
	 * 可消费槽位，UINT_MAX 表示尚未填充或已被热插拔失效。		\
	 */									\
	type entropy[CHACHA_BLOCK_SIZE * 3 / (2 * sizeof(type))];		\
	local_lock_t lock;							\
	unsigned long generation;						\
	unsigned int position;							\
};										\
										\
static DEFINE_PER_CPU(struct batch_ ##type, batched_entropy_ ##type) = {	\
	.lock = INIT_LOCAL_LOCK(batched_entropy_ ##type.lock),			\
	.position = UINT_MAX							\
};										\
										\
type get_random_ ##type(void)							\
{										\
	type ret;								\
	unsigned long flags;							\
	struct batch_ ##type *batch;						\
	unsigned long next_gen;							\
										\
	if  (!crng_ready()) {							\
		/*								\
		 * 未 ready 时不能使用长期批量缓存，否则早期低熵输出会被	\
		 * 缓存在 per-CPU 数组里延后消费。直接生成一个整数并返回，	\
		 * 安全语义仍与 get_random_bytes() 一样由调用者负责等待。	\
		 */								\
		_get_random_bytes(&ret, sizeof(ret));				\
		return ret;							\
	}									\
										\
	local_lock_irqsave(&batched_entropy_ ##type.lock, flags);		\
	batch = raw_cpu_ptr(&batched_entropy_##type);				\
										\
	next_gen = READ_ONCE(base_crng.generation);				\
	if (batch->position >= ARRAY_SIZE(batch->entropy) ||			\
	    next_gen != batch->generation) {					\
		/*								\
		 * 缓存耗尽或根 generation 变化时 refill。_get_random_bytes()	\
		 * 会推进 CRNG；position 重置后，本 CPU 后续调用按槽位消费。	\
		 */								\
		_get_random_bytes(batch->entropy, sizeof(batch->entropy));	\
		batch->position = 0;						\
		batch->generation = next_gen;					\
	}									\
										\
	ret = batch->entropy[batch->position];					\
	/* 已消费槽位立即清零，避免旧随机整数在 per-CPU 内存中长期残留。 */	\
	batch->entropy[batch->position] = 0;					\
	++batch->position;							\
	local_unlock_irqrestore(&batched_entropy_ ##type.lock, flags);		\
	return ret;								\
}										\
EXPORT_SYMBOL(get_random_ ##type);

DEFINE_BATCHED_ENTROPY(u8)
DEFINE_BATCHED_ENTROPY(u16)
DEFINE_BATCHED_ENTROPY(u32)
DEFINE_BATCHED_ENTROPY(u64)

/*
 * __get_random_u32_below() - 返回 [0, ceil) 范围内的无偏随机数
 *
 * @ceil: 上界，语义为开区间右端；0 是内部兼容特例，返回完整 u32 随机数。
 * 调用位置：include/linux/random.h 中 get_random_u32_below() 的变量上界慢路径。
 * 锁/上下文：通过 get_random_u32() 使用批量缓存；不睡眠。
 * 返回：ceil 非 0 时返回均匀分布在 0..ceil-1 的值；ceil 为 0 时返回任意 u32。
 * 算法重点：用 32x32→64 乘法的高 32 位做缩放，并在低 32 位落入不可整除
 * 尾区时拒绝重采样，避免简单取模导致小值概率偏高。
 */
u32 __get_random_u32_below(u32 ceil)
{
	/*
	 * This is the slow path for variable ceil. It is still fast, most of
	 * the time, by doing traditional reciprocal multiplication and
	 * opportunistically comparing the lower half to ceil itself, before
	 * falling back to computing a larger bound, and then rejecting samples
	 * whose lower half would indicate a range indivisible by ceil. The use
	 * of `-ceil % ceil` is analogous to `2^32 % ceil`, but is computable
	 * in 32-bits.
	 */
	/*
	 * 英文注释说明：这是变量 ceil 的慢路径，但通常仍很快。乘法高半部分给出
	 * 缩放结果；低半部分用于判断本次样本是否落在会造成除不尽偏差的区域。
	 * `-ceil % ceil` 在无符号 32 位下等价于 2^32 % ceil，用于计算拒绝边界。
	 */
	u32 rand = get_random_u32();
	u64 mult;

	/*
	 * This function is technically undefined for ceil == 0, and in fact
	 * for the non-underscored constant version in the header, we build bug
	 * on that. But for the non-constant case, it's convenient to have that
	 * evaluate to being a straight call to get_random_u32(), so that
	 * get_random_u32_inclusive() can work over its whole range without
	 * undefined behavior.
	 */
	/*
	 * ceil==0 对“低于 0”没有数学意义，但为了让 inclusive wrapper 能覆盖
	 * [0, U32_MAX] 全范围，这里把它定义为直接返回完整随机 u32。
	 */
	if (unlikely(!ceil))
		return rand;

	mult = (u64)ceil * rand;
	if (unlikely((u32)mult < ceil)) {
		u32 bound = -ceil % ceil;
		/*
		 * 低半部分小于 bound 表示该样本落入 2^32 不能被 ceil 整除的尾部；
		 * 丢弃这些样本后，高半部分才是无偏的。循环通常很少执行。
		 */
		while (unlikely((u32)mult < bound))
			mult = (u64)ceil * get_random_u32();
	}
	return mult >> 32;
}
EXPORT_SYMBOL(__get_random_u32_below);

#ifdef CONFIG_SMP
/*
 * This function is called when the CPU is coming up, with entry
 * CPUHP_RANDOM_PREPARE, which comes before CPUHP_WORKQUEUE_PREP.
 */
/*
 * random_prepare_cpu() - CPU 热插拔 prepare 阶段失效本 CPU RNG 缓存
 *
 * @cpu: 即将上线的 CPU 编号。
 * 调用位置：CPUHP_RANDOM_PREPARE，早于该 CPU workqueue prepare。
 * 上下文：CPU hotplug 串行化路径；直接写目标 CPU 的 per-CPU 存储。
 * 返回：0 表示准备成功。
 * 副作用：把 per-CPU crng generation 设为 ULONG_MAX，并把批量缓存 position
 * 设为 UINT_MAX，强制上线后的第一次消费重新从 base_crng 派生新状态。
 */
int __cold random_prepare_cpu(unsigned int cpu)
{
	/*
	 * When the cpu comes back online, immediately invalidate both
	 * the per-cpu crng and all batches, so that we serve fresh
	 * randomness.
	 */
	per_cpu_ptr(&crngs, cpu)->generation = ULONG_MAX;
	per_cpu_ptr(&batched_entropy_u8, cpu)->position = UINT_MAX;
	per_cpu_ptr(&batched_entropy_u16, cpu)->position = UINT_MAX;
	per_cpu_ptr(&batched_entropy_u32, cpu)->position = UINT_MAX;
	per_cpu_ptr(&batched_entropy_u64, cpu)->position = UINT_MAX;
	return 0;
}
#endif


/**********************************************************************
 *
 * Entropy accumulation and extraction routines.
 *
 * Callers may add entropy via:
 *
 *     static void mix_pool_bytes(const void *buf, size_t len)
 *
 * After which, if added entropy should be credited:
 *
 *     static void credit_init_bits(size_t bits)
 *
 * Finally, extract entropy via:
 *
 *     static void extract_entropy(void *buf, size_t len)
 *
 **********************************************************************/
/*
 * 本节维护 input_pool。它不是传统“把熵字节排队再读出”的池，而是一个 BLAKE2s
 * 哈希状态：
 *   mix_pool_bytes() 只改变哈希状态，不改变安全等级；
 *   _credit_init_bits() 以保守上限增加 init_bits，并在阈值跨越时发布状态；
 *   extract_entropy() final 当前 hash 得到 seed，同时用 next_key 重新 key 化 hash，
 *   让后续输入进入新的 PRF key。
 */

enum {
	POOL_BITS = BLAKE2S_HASH_SIZE * 8,
	POOL_READY_BITS = POOL_BITS, /* When crng_init->CRNG_READY */
	POOL_EARLY_BITS = POOL_READY_BITS / 2 /* When crng_init->CRNG_EARLY */
};
/*
 * POOL_BITS 等于 BLAKE2s 输出大小 256 位。READY 阈值取满池，EARLY 阈值取半池。
 * 这些值不是说池只能吸收 256 位数据，而是说初始化 credit 最多累加到 256 位；
 * 哈希状态仍会持续混入后续所有输入。
 */

/*
 * input_pool - 熵输入哈希状态与初始化 credit 计数
 *
 * @hash: BLAKE2s 上下文；所有 mix/extract 都围绕它更新。
 * @lock: 保护 hash 的内部链式状态，防止并发 update/final/init_key 交错。
 * @init_bits: 已被 credit 的初始化熵位数，单调饱和到 POOL_BITS；通过 cmpxchg
 *       更新，使多个采集路径并发 credit 时不会丢失增量。
 */
static struct {
	struct blake2s_ctx hash;
	spinlock_t lock;
	unsigned int init_bits;
} input_pool = {
	.hash.h = { BLAKE2S_IV0 ^ (0x01010000 | BLAKE2S_HASH_SIZE),
		    BLAKE2S_IV1, BLAKE2S_IV2, BLAKE2S_IV3, BLAKE2S_IV4,
		    BLAKE2S_IV5, BLAKE2S_IV6, BLAKE2S_IV7 },
	.hash.outlen = BLAKE2S_HASH_SIZE,
	.lock = __SPIN_LOCK_UNLOCKED(input_pool.lock),
};

static void _mix_pool_bytes(const void *buf, size_t len)
{
	blake2s_update(&input_pool.hash, buf, len);
}

/*
 * This function adds bytes into the input pool. It does not
 * update the initialization bit counter; the caller should call
 * credit_init_bits if this is appropriate.
 */
/*
 * mix_pool_bytes() - 将任意字节混入 input_pool，但不计入熵位数
 *
 * @buf: 输入数据，调用期间只借用；可以是设备标识、时间戳、硬件 RNG 输出等。
 * @len: 输入字节数；0 是无效果更新。
 * 锁/上下文：获取 input_pool.lock 并关闭本地中断，保护 BLAKE2s 状态。
 * 返回：无直接返回值；副作用是改变未来 extract_entropy() 的输出。
 * 关键边界：混入可以让不同机器/启动状态分叉，但不会推进 CRNG_READY；只有调用者
 * 随后明确 credit_init_bits()，才表示这些输入被信任为有熵贡献。
 */
static void mix_pool_bytes(const void *buf, size_t len)
{
	unsigned long flags;

	spin_lock_irqsave(&input_pool.lock, flags);
	_mix_pool_bytes(buf, len);
	spin_unlock_irqrestore(&input_pool.lock, flags);
}

/*
 * This is an HKDF-like construction for using the hashed collected entropy
 * as a PRF key, that's then expanded block-by-block.
 */
/*
 * extract_entropy() - 从 input_pool 派生输出字节并重新 key 化输入池
 *
 * @buf: 输出缓冲区，调用者拥有；本函数写入 len 字节。
 * @len: 需要派生的字节数，可以超过 BLAKE2S_HASH_SIZE，此时按 counter 多块扩展。
 * 调用位置：crng_reseed() 获取新 base_crng.key；早期 crng_make_state() 也可能
 * 在 CRNG_EMPTY 时用它初始化 base_crng.key。
 * 锁/上下文：短暂持 input_pool.lock 完成 final 和 init_key；可在持 base_crng.lock
 * 的早期路径中被调用，但不能睡眠。
 * 阶段：
 *   1. 采集硬件 seed/arch random/周期计数作为本次提取的额外 salt；
 *   2. final 当前 hash 得到 seed；
 *   3. 用 seed 和 RDSEED||0 派生 next_key，并用 next_key 重新初始化 input_pool.hash；
 *   4. 用 seed 和递增 counter 扩展输出；
 *   5. 清除 seed、next_key 和 block。
 * 安全含义：提取不会“清空”所有历史输入，而是把旧 hash 结果变成 PRF key，并让
 * 后续 input_pool 使用新的隐藏 key，降低状态泄露后的回溯能力。
 */
static void extract_entropy(void *buf, size_t len)
{
	unsigned long flags;
	u8 seed[BLAKE2S_HASH_SIZE], next_key[BLAKE2S_HASH_SIZE];
	struct {
		unsigned long rdseed[32 / sizeof(long)];
		size_t counter;
	} block;
	size_t i, longs;

	for (i = 0; i < ARRAY_SIZE(block.rdseed);) {
		/*
		 * 提取时额外混入架构随机源或周期计数。这里不 credit，只把它作为
		 * PRF 输入的一部分；即使硬件随机源质量未知，也能让输出随平台状态分叉。
		 */
		longs = arch_get_random_seed_longs(&block.rdseed[i], ARRAY_SIZE(block.rdseed) - i);
		if (longs) {
			i += longs;
			continue;
		}
		longs = arch_get_random_longs(&block.rdseed[i], ARRAY_SIZE(block.rdseed) - i);
		if (longs) {
			i += longs;
			continue;
		}
		block.rdseed[i++] = random_get_entropy();
	}

	spin_lock_irqsave(&input_pool.lock, flags);

	/* seed = HASHPRF(last_key, entropy_input) */
	/*
	 * 英文公式表示：当前 BLAKE2s hash 的最终输出 seed 是“上一轮 hash key”
	 * 和累计 entropy_input 的 PRF 结果。final 后旧 hash 上下文不可继续 update，
	 * 必须在下面 init_key。
	 */
	blake2s_final(&input_pool.hash, seed);

	/* next_key = HASHPRF(seed, RDSEED || 0) */
	/*
	 * next_key 用 seed 和 block.counter=0 的块派生，随后立刻作为 input_pool.hash
	 * 的新 key。这样本次提取后继续混入的新输入不会仍使用旧 hash key。
	 */
	block.counter = 0;
	blake2s(seed, sizeof(seed), (const u8 *)&block, sizeof(block), next_key, sizeof(next_key));
	blake2s_init_key(&input_pool.hash, BLAKE2S_HASH_SIZE, next_key, sizeof(next_key));

	spin_unlock_irqrestore(&input_pool.lock, flags);
	memzero_explicit(next_key, sizeof(next_key));

	while (len) {
		i = min_t(size_t, len, BLAKE2S_HASH_SIZE);
		/* output = HASHPRF(seed, RDSEED || ++counter) */
		/*
		 * 输出扩展从 counter=1 开始，避免与 next_key 的 counter=0 域重叠。
		 * 每块最多 BLAKE2S_HASH_SIZE 字节，直到填满调用者缓冲区。
		 */
		++block.counter;
		blake2s(seed, sizeof(seed), (const u8 *)&block, sizeof(block), buf, i);
		len -= i;
		buf += i;
	}

	memzero_explicit(seed, sizeof(seed));
	memzero_explicit(&block, sizeof(block));
}

#define credit_init_bits(bits) if (!crng_ready()) _credit_init_bits(bits)
/*
 * credit_init_bits() 是快速门禁宏：ready 之后 init_bits 已经达到上限，再 credit
 * 没有状态意义，直接跳过 _credit_init_bits() 可避免热路径争用。宏只应包裹
 * 单个语句使用；当前文件所有调用点都遵守这个形态。
 */

/*
 * _credit_init_bits() - 单调增加初始化熵计数并处理阈值跨越
 *
 * @bits: 调用者声称可计入初始化的熵位数；会被截断到 POOL_BITS。
 * 调用位置：可信 CPU/bootloader/hwrng 输入，以及中断/输入/磁盘/定时器估算路径。
 * 锁/并发：init_bits 用 try_cmpxchg 饱和累加，避免多个 CPU 同时 credit 丢失；
 * CRNG_EARLY 转换需要 base_crng.lock；READY 转换通过 crng_reseed() 提交 key。
 * 返回：无直接返回值。
 * 副作用：
 *   - 跨越 POOL_EARLY_BITS：初始化 base_crng.key 并设置 CRNG_EARLY；
 *   - 跨越 POOL_READY_BITS：reseed、发布 ready、通知内核/用户等待者和 vDSO。
 */
static void __cold _credit_init_bits(size_t bits)
{
	static DECLARE_WORK(set_ready, crng_set_ready);
	unsigned int new, orig, add;
	unsigned long flags;
	int m;

	if (!bits)
		return;

	add = min_t(size_t, bits, POOL_BITS);

	/*
	 * init_bits 是饱和单调计数。try_cmpxchg 失败时 orig 会被更新为当前值，
	 * 循环重新计算 new；这样并发 credit 既不会倒退，也不会超过 POOL_BITS。
	 */
	orig = READ_ONCE(input_pool.init_bits);
	do {
		new = min_t(unsigned int, POOL_BITS, orig + add);
	} while (!try_cmpxchg(&input_pool.init_bits, &orig, new));

	if (orig < POOL_READY_BITS && new >= POOL_READY_BITS) {
		crng_reseed(NULL); /* Sets crng_init to CRNG_READY under base_crng.lock. */
		/*
		 * READY 是全系统发布点。crng_reseed() 已在 base_crng.lock 下设置
		 * CRNG_READY；后续动作把这个状态传播给不同观察者：static key、notifier、
		 * vDSO、等待队列、fasync 和日志。
		 */
		if (system_dfl_wq)
			queue_work(system_dfl_wq, &set_ready);
		atomic_notifier_call_chain(&random_ready_notifier, 0, NULL);
		if (IS_ENABLED(CONFIG_VDSO_GETRANDOM))
			WRITE_ONCE(vdso_k_rng_data->is_ready, true);
		wake_up_interruptible(&crng_init_wait);
		kill_fasync(&fasync, SIGIO, POLL_IN);
		pr_notice("crng init done\n");
		m = ratelimit_state_get_miss(&urandom_warning);
		if (m)
			pr_notice("%d urandom warning(s) missed due to ratelimiting\n", m);
	} else if (orig < POOL_EARLY_BITS && new >= POOL_EARLY_BITS) {
		spin_lock_irqsave(&base_crng.lock, flags);
		/* Check if crng_init is CRNG_EMPTY, to avoid race with crng_reseed(). */
		/*
		 * 英文注释说明：加锁后检查 CRNG_EMPTY 是为了避免与并发 ready reseed
		 * 竞争。若另一路已经完成 crng_reseed()，这里不能把状态倒回 CRNG_EARLY。
		 */
		if (crng_init == CRNG_EMPTY) {
			extract_entropy(base_crng.key, sizeof(base_crng.key));
			crng_init = CRNG_EARLY;
		}
		spin_unlock_irqrestore(&base_crng.lock, flags);
	}
}


/**********************************************************************
 *
 * Entropy collection routines.
 *
 * The following exported functions are used for pushing entropy into
 * the above entropy accumulation routines:
 *
 *	void add_device_randomness(const void *buf, size_t len);
 *	void add_hwgenerator_randomness(const void *buf, size_t len, size_t entropy, bool sleep_after);
 *	void add_bootloader_randomness(const void *buf, size_t len);
 *	void add_vmfork_randomness(const void *unique_vm_id, size_t len);
 *	void add_interrupt_randomness(int irq);
 *	void add_input_randomness(unsigned int type, unsigned int code, unsigned int value);
 *	void add_disk_randomness(struct gendisk *disk);
 *
 * add_device_randomness() adds data to the input pool that
 * is likely to differ between two devices (or possibly even per boot).
 * This would be things like MAC addresses or serial numbers, or the
 * read-out of the RTC. This does *not* credit any actual entropy to
 * the pool, but it initializes the pool to different values for devices
 * that might otherwise be identical and have very little entropy
 * available to them (particularly common in the embedded world).
 *
 * add_hwgenerator_randomness() is for true hardware RNGs, and will credit
 * entropy as specified by the caller. If the entropy pool is full it will
 * block until more entropy is needed.
 *
 * add_bootloader_randomness() is called by bootloader drivers, such as EFI
 * and device tree, and credits its input depending on whether or not the
 * command line option 'random.trust_bootloader' is set.
 *
 * add_vmfork_randomness() adds a unique (but not necessarily secret) ID
 * representing the current instance of a VM to the pool, without crediting,
 * and then force-reseeds the crng so that it takes effect immediately.
 *
 * add_interrupt_randomness() uses the interrupt timing as random
 * inputs to the entropy pool. Using the cycle counters and the irq source
 * as inputs, it feeds the input pool roughly once a second or after 64
 * interrupts, crediting 1 bit of entropy for whichever comes first.
 *
 * add_input_randomness() uses the input layer interrupt timing, as well
 * as the event type information from the hardware.
 *
 * add_disk_randomness() uses what amounts to the seek time of block
 * layer request events, on a per-disk_devt basis, as input to the
 * entropy pool. Note that high-speed solid state drives with very low
 * seek times do not make for good sources of entropy, as their seek
 * times are usually fairly consistent.
 *
 * The last two routines try to estimate how many bits of entropy
 * to credit. They do this by keeping track of the first and second
 * order deltas of the event timings.
 *
 **********************************************************************/
/*
 * 本节是 input_pool 的生产者集合。必须区分“唯一但公开的数据”和“不可预测数据”：
 * device ID、VM generation ID、boot 参数能让不同实例状态分叉，但不一定秘密，通常只 mix；
 * hwrng/bootloader 在配置允许时可 credit；中断、输入和磁盘基于时序 delta 做保守估算。
 */

static bool trust_cpu __initdata = true;
static bool trust_bootloader __initdata = true;
/*
 * trust_cpu/trust_bootloader 只在 __init 阶段使用，控制是否相信 CPU 硬件 RNG 或
 * bootloader seed 的熵声明。__initdata 表示初始化结束后内存可释放；运行期路径
 * 不再读取这些变量。
 */
/*
 * parse_trust_cpu() - 解析 random.trust_cpu= 命令行布尔值
 *
 * @arg: early_param 传入的字符串，通常是 0/1、true/false 等 kstrtobool()
 *       支持的形式。
 * 返回：0 表示解析成功；负 errno 表示参数非法。
 * 副作用：写入 __initdata 变量 trust_cpu，影响 random_init_early() 是否把
 * CPU 硬件随机数获取到的位数 credit 到 input_pool。
 */
static int __init parse_trust_cpu(char *arg)
{
	return kstrtobool(arg, &trust_cpu);
}
/*
 * parse_trust_bootloader() - 解析 random.trust_bootloader= 命令行布尔值
 *
 * @arg: early_param 传入的布尔字符串。
 * 返回：0 表示解析成功；负 errno 表示参数非法。
 * 副作用：写入 trust_bootloader，影响 add_bootloader_randomness() 是否按 seed
 * 长度 credit 熵；即使为 false，bootloader seed 仍会被 mix。
 */
static int __init parse_trust_bootloader(char *arg)
{
	return kstrtobool(arg, &trust_bootloader);
}
/*
 * early_param 在命令行解析早期生效，允许 random.trust_cpu=0 或
 * random.trust_bootloader=0 在 random_init_early()/add_bootloader_randomness()
 * credit 前改变策略。解析函数只转换布尔值，不直接修改熵池。
 */
early_param("random.trust_cpu", parse_trust_cpu);
early_param("random.trust_bootloader", parse_trust_bootloader);

/*
 * random_pm_notification() - suspend/resume 时混入时间状态并必要时 reseed
 *
 * @nb: notifier_block，当前实现不使用；由 PM core 传入。
 * @action: PM 事件类型，作为输入混入，使不同 suspend/resume 阶段状态分叉。
 * @data: PM core 附带数据，当前实现不使用。
 * 调用位置：register_pm_notifier() 注册后的电源管理通知链。
 * 锁/上下文：持 input_pool.lock 混入时间戳；ready 且恢复路径时可直接 crng_reseed()。
 * 返回：0 表示不阻止 PM 事件。
 * 副作用：不 credit 熵，只 mix 时间和 action；resume 后若 CRNG_READY，刷新输出 key。
 */
static int random_pm_notification(struct notifier_block *nb, unsigned long action, void *data)
{
	unsigned long flags, entropy = random_get_entropy();

	/*
	 * Encode a representation of how long the system has been suspended,
	 * in a way that is distinct from prior system suspends.
	 */
	/*
	 * 英文注释说明：三个时钟域共同编码 suspend 经过了多久，并让每次 suspend
	 * 与之前不同。ktime_get/boottime/real 分别覆盖单调时间、含休眠时间和墙钟。
	 */
	ktime_t stamps[] = { ktime_get(), ktime_get_boottime(), ktime_get_real() };

	spin_lock_irqsave(&input_pool.lock, flags);
	_mix_pool_bytes(&action, sizeof(action));
	_mix_pool_bytes(stamps, sizeof(stamps));
	_mix_pool_bytes(&entropy, sizeof(entropy));
	spin_unlock_irqrestore(&input_pool.lock, flags);

	if (crng_ready() && (action == PM_RESTORE_PREPARE ||
	    (action == PM_POST_SUSPEND && !IS_ENABLED(CONFIG_PM_AUTOSLEEP) &&
	     !IS_ENABLED(CONFIG_PM_USERSPACE_AUTOSLEEP)))) {
		/*
		 * resume 后立即 reseed 的目的是让 suspend 期间积累的时间差尽快影响
		 * 输出流。autosleep 配置下避免在频繁自动睡眠路径中过度 reseed。
		 */
		crng_reseed(NULL);
		pr_notice("crng reseeded on system resumption\n");
	}
	return 0;
}

static struct notifier_block pm_notifier = { .notifier_call = random_pm_notification };

/*
 * random_init_early - RNG 最早期初始化（时钟子系统就绪前）
 *
 * 调用时机：start_kernel() 极早期，时钟计数器不可用，中断尚未使能。
 * 此阶段能获取的熵源非常有限，主要依赖：
 *   1. 编译时潜在熵（LATENT_ENTROPY_PLUGIN）
 *   2. CPU 硬件随机数生成器（RDRAND/RDSEED 或 ARM64 的 mrs rndrrs）
 *   3. 内核版本信息（utsname）和命令行参数
 *
 * 整个 RNG 子系统的状态机：
 *   CRNG_EMPTY  (0) → 几乎没有熵，/dev/urandom 会发出警告
 *   CRNG_EARLY  (1) → 积累了 POOL_EARLY_BITS（=128 bits），可用于早期加密
 *   CRNG_READY  (2) → 积累了 POOL_READY_BITS（=256 bits），完全初始化
 *
 * 所有熵数据都通过 _mix_pool_bytes() 混入 input_pool（一个 BLAKE2s 哈希状态）。
 */
void __init random_init_early(const char *command_line)
{
	unsigned long entropy[BLAKE2S_BLOCK_SIZE / sizeof(long)];
	size_t i, longs, arch_bits;

#if defined(LATENT_ENTROPY_PLUGIN)
	/* GCC 插件 LATENT_ENTROPY_PLUGIN 在编译时用随机字节填充 compiletime_seed，
	 * 每次编译生成不同的值，使不同编译版本的内核初始熵池状态不同。
	 * __latent_entropy 标记告诉插件在此处注入随机数据，
	 * __initconst 确保数据在 init 完成后被释放。 */
	static const u8 compiletime_seed[BLAKE2S_BLOCK_SIZE] __initconst __latent_entropy;
	_mix_pool_bytes(compiletime_seed, sizeof(compiletime_seed));
#endif

	/* 尽量从 CPU 硬件随机数获取熵，填满 entropy[] 数组（一个 BLAKE2s 块大小）。
	 * arch_bits 追踪成功从硬件获取的位数，失败的槽位不计入熵贡献。
	 *
	 * 优先使用 arch_get_random_seed_longs()（对应 x86 RDSEED / arm64 RNDRRS）：
	 *   从真实物理熵源（热噪声等）获取，质量最高，但可能暂时不可用（返回 0）。
	 *
	 * 退而使用 arch_get_random_longs()（对应 x86 RDRAND / arm64 RNDR）：
	 *   从 DRBG（确定性随机位生成器）获取，基于硬件种子，质量略低于 RDSEED。
	 *
	 * 若两者都返回 0（硬件不支持或暂时不可用），该槽位跳过，arch_bits 减少，
	 * 不将全零数据当作熵（避免熵池被低质量数据污染）。 */
	for (i = 0, arch_bits = sizeof(entropy) * 8; i < ARRAY_SIZE(entropy);) {
		longs = arch_get_random_seed_longs(entropy, ARRAY_SIZE(entropy) - i);
		if (longs) {
			_mix_pool_bytes(entropy, sizeof(*entropy) * longs);
			i += longs;
			continue;
		}
		longs = arch_get_random_longs(entropy, ARRAY_SIZE(entropy) - i);
		if (longs) {
			_mix_pool_bytes(entropy, sizeof(*entropy) * longs);
			i += longs;
			continue;
		}
		/* 此槽位无法从硬件获取，不计入熵贡献。 */
		arch_bits -= sizeof(*entropy) * 8;
		++i;
	}

	/* 混入内核版本信息（uts_namespace：hostname/sysname/release/version/machine）。
	 * 不同系统的 utsname 不同，防止不同机器的熵池初始状态相同。 */
	_mix_pool_bytes(init_utsname(), sizeof(*(init_utsname())));

	/* 混入内核命令行参数（如 root=、console=、KASLR 偏移等）。
	 * 命令行在不同启动中可能不同，提供额外的区分度。 */
	_mix_pool_bytes(command_line, strlen(command_line));

	/* Reseed if already seeded by earlier phases.
	 *
	 * 某些架构（如 EFI stub）在更早阶段已向熵池注入了足够的熵，
	 * 若此时 crng 已就绪，立即重新播种以利用新混入的数据。
	 *
	 * 否则，若配置信任 CPU 硬件随机数（trust_cpu=true，默认开启），
	 * 将本轮从硬件获取的 arch_bits 位计入熵池，
	 * 若累积到 POOL_READY_BITS（256位）则触发 crng_ready 状态转换。
	 *
	 * trust_cpu 可通过命令行 random.trust_cpu=0 禁用，
	 * 适用于不信任 CPU 硬件随机数实现的安全敏感场景。 */
	if (crng_ready())
		crng_reseed(NULL);
	else if (trust_cpu)
		_credit_init_bits(arch_bits);
}

/*
 * random_init - RNG 第二阶段初始化（时钟子系统就绪后）
 *
 * 调用时机：time_init() 之后，中断使能之前。
 * 此时时钟计数器可用，可以获取高精度时间戳作为额外熵源。
 *
 * 与 random_init_early() 的分工：
 *   early：依赖 CPU 硬件随机数，在时钟不可用时运行
 *   本函数：依赖时间戳熵，补充 early 阶段可能遗漏的熵
 */
void __init random_init(void)
{
	/* random_get_entropy()：读取 CPU 周期计数器（如 x86 TSC、arm64 CNTVCT_EL0）
	 * 作为熵源。时钟计数器值在每次启动时不同，提供时间相关的随机性。
	 * 若无时钟计数器（某些嵌入式 CPU），返回 0，后面有 WARN 提示。 */
	unsigned long entropy = random_get_entropy();

	/* ktime_get_real()：读取当前墙钟时间（UTC 时间戳）。
	 * 与 random_get_entropy() 的 CPU 周期计数不同，这是人类可读的时间，
	 * 两者结合提供更多维度的时间随机性。 */
	ktime_t now = ktime_get_real();

	/* 将时间戳混入熵池，增加初始状态的不可预测性。 */
	_mix_pool_bytes(&now, sizeof(now));
	_mix_pool_bytes(&entropy, sizeof(entropy));

	/* add_latent_entropy()：若启用了 LATENT_ENTROPY_PLUGIN，将编译时注入到
	 * latent_entropy 全局变量中的随机值混入熵池（与 random_init_early 中
	 * compiletime_seed 不同，这个值在内核运行期间会被各处代码路径修改）。
	 * 未启用插件时此调用为空操作。 */
	add_latent_entropy();

	/*
	 * If we were initialized by the cpu or bootloader before workqueues
	 * are initialized, then we should enable the static branch here.
	 *
	 * workqueue 初始化之前，_credit_init_bits() 无法通过 queue_work() 调度
	 * crng_set_ready 工作项来开启 crng_is_ready 静态分支。
	 * 现在 workqueue 已就绪，若 crng_init 已达到 CRNG_READY 但静态分支
	 * 尚未开启（crng_is_ready 仍为 false），在此补充开启。
	 * crng_is_ready 静态分支开启后，crng_ready() 查询变为单条 NOP，零开销。
	 */
	if (!static_branch_likely(&crng_is_ready) && crng_init >= CRNG_READY)
		crng_set_ready(NULL);

	/* Reseed if already seeded by earlier phases.
	 * 若 early 阶段已完成初始化，利用新混入的时间戳熵立即重新播种，
	 * 刷新 crng（ChaCha20 流密码密钥），进一步增强随机数质量。 */
	if (crng_ready())
		crng_reseed(NULL);

	/* 注册电源管理通知回调 random_pm_notification：
	 * 系统 suspend/resume 时将休眠时长、恢复时间戳混入熵池，
	 * 防止 resume 后 RNG 状态与 suspend 前相同（时间流逝是额外熵源）。
	 * resume 后若 crng 已就绪，还会主动重新播种以刷新密钥。 */
	WARN_ON(register_pm_notifier(&pm_notifier));

	/* 若 entropy=0，说明既没有 CPU 周期计数器也没有备用定时器，
	 * 系统将严重缺乏时序熵，RNG 质量会受到影响，发出内核警告。
	 * 某些极简嵌入式系统可能遇到此情况。 */
	WARN(!entropy, "Missing cycle counter and fallback timer; RNG "
		       "entropy collection will consequently suffer.");
}

/*
 * Add device- or boot-specific data to the input pool to help
 * initialize it.
 *
 * None of this adds any entropy; it is meant to avoid the problem of
 * the entropy pool having similar initial state across largely
 * identical devices.
 */
/*
 * add_device_randomness() - 混入设备或本次启动特有数据但不 credit
 *
 * @buf: 调用者提供的设备/启动相关字节，只在调用期间借用。
 * @len: 输入长度。
 * 调用位置：驱动可用 MAC、序列号、RTC 等“可能唯一但不一定秘密”的数据调用。
 * 锁/上下文：获取 input_pool.lock；不睡眠。
 * 返回：无直接返回值。
 * 副作用：混入 random_get_entropy() 和 buf，使相同镜像的不同设备池状态不同；
 * init_bits 不变，因此不会让 RNG 因公开序列号而错误宣布 ready。
 */
void add_device_randomness(const void *buf, size_t len)
{
	unsigned long entropy = random_get_entropy();
	unsigned long flags;

	spin_lock_irqsave(&input_pool.lock, flags);
	_mix_pool_bytes(&entropy, sizeof(entropy));
	_mix_pool_bytes(buf, len);
	spin_unlock_irqrestore(&input_pool.lock, flags);
}
EXPORT_SYMBOL(add_device_randomness);

/*
 * Interface for in-kernel drivers of true hardware RNGs. Those devices
 * may produce endless random bits, so this function will sleep for
 * some amount of time after, if the sleep_after parameter is true.
 */
/*
 * add_hwgenerator_randomness() - 硬件 RNG 驱动向核心 RNG 注入数据
 *
 * @buf: 硬件 RNG 产生的字节，调用期间借用。
 * @len: 输入长度。
 * @entropy: 驱动声称可 credit 的熵位数；0 表示只 mix 不推进 ready。
 * @sleep_after: true 表示调用者愿意在池已满或无需继续初始化时被节流。
 * 调用位置：hwrng kthread/驱动持续上报硬件随机数。
 * 返回：无直接返回值。
 * 副作用：mix 输入、按 entropy credit；ready 后或无 credit 时可睡眠一个 reseed
 * 间隔，避免硬件 RNG 无限写入造成 CPU/锁压力。
 */
void add_hwgenerator_randomness(const void *buf, size_t len, size_t entropy, bool sleep_after)
{
	mix_pool_bytes(buf, len);
	credit_init_bits(entropy);

	/*
	 * Throttle writing to once every reseed interval, unless we're not yet
	 * initialized or no entropy is credited.
	 */
	/*
	 * 未初始化且 entropy>0 时不节流，让硬件 RNG 尽快推动 ready；ready 后继续
	 * 高频输入收益有限，按 reseed interval 睡眠即可。kthread_should_stop()
	 * 允许 hwrng 线程退出时不再睡眠。
	 */
	if (sleep_after && !kthread_should_stop() && (crng_ready() || !entropy))
		schedule_timeout_interruptible(crng_reseed_interval());
}
EXPORT_SYMBOL_GPL(add_hwgenerator_randomness);

/*
 * Handle random seed passed by bootloader, and credit it depending
 * on the command line option 'random.trust_bootloader'.
 */
/*
 * add_bootloader_randomness() - 处理 bootloader/固件传入的随机种子
 *
 * @buf: bootloader 提供的 seed，调用期间借用。
 * @len: seed 字节数；trust_bootloader 为 true 时按 len*8 credit。
 * 调用位置：EFI、device tree 等早期启动代码。
 * 上下文：__init 阶段；可能在 random_init_early()/random_init() 前后发生。
 * 返回：无直接返回值。
 * 安全边界：不信任 bootloader 时仍 mix 以分叉状态，但不 credit，避免固件可预测
 * 或复用 seed 时错误宣布 CRNG_READY。
 */
void __init add_bootloader_randomness(const void *buf, size_t len)
{
	mix_pool_bytes(buf, len);
	if (trust_bootloader)
		credit_init_bits(len * 8);
}

#if IS_ENABLED(CONFIG_VMGENID)
static BLOCKING_NOTIFIER_HEAD(vmfork_chain);
/*
 * vmfork_chain 通知依赖 VM generation ID 的子系统：虚拟机 fork/克隆后，内核 RNG
 * 会先混入新的唯一 ID 并 reseed，随后通知其他使用随机状态的组件重建自身状态。
 * blocking notifier 允许回调睡眠，因此只能在非原子上下文调用。
 */

/*
 * Handle a new unique VM ID, which is unique, not secret, so we
 * don't credit it, but we do immediately force a reseed after so
 * that it's used by the crng posthaste.
 */
/*
 * add_vmfork_randomness() - VM fork 后混入新的唯一实例 ID 并尽快 reseed
 *
 * @unique_vm_id: hypervisor/VMGENID 提供的新实例标识；唯一但不保证秘密。
 * @len: 标识长度。
 * 调用位置：VMGENID 驱动检测到虚拟机克隆、快照恢复导致 ID 变化时。
 * 返回：无直接返回值。
 * 副作用：只 mix 不 credit；若 CRNG_READY，则立即 reseed 让新 ID 影响输出；
 * 随后调用 vmfork_chain 通知其他订阅者。
 */
void __cold add_vmfork_randomness(const void *unique_vm_id, size_t len)
{
	add_device_randomness(unique_vm_id, len);
	if (crng_ready()) {
		crng_reseed(NULL);
		pr_notice("crng reseeded due to virtual machine fork\n");
	}
	blocking_notifier_call_chain(&vmfork_chain, 0, NULL);
}
#if IS_MODULE(CONFIG_VMGENID)
EXPORT_SYMBOL_GPL(add_vmfork_randomness);
#endif

/*
 * register_random_vmfork_notifier() - 订阅 VM fork 后 RNG 已重播种事件
 *
 * @nb: 调用者提供的 notifier_block；注册成功后由 vmfork_chain 借用其存储，
 *      调用者必须保证 unregister 前对象仍有效。
 * 返回：blocking_notifier_chain_register() 的结果。
 * 上下文：可睡眠 notifier 链，适合需要在 VM 克隆后重建随机派生状态的子系统。
 */
int __cold register_random_vmfork_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&vmfork_chain, nb);
}
EXPORT_SYMBOL_GPL(register_random_vmfork_notifier);

/*
 * unregister_random_vmfork_notifier() - 取消 VM fork notifier 订阅
 *
 * @nb: 先前注册的 notifier_block。
 * 返回：blocking_notifier_chain_unregister() 的结果。
 * 副作用：从 vmfork_chain 摘除回调；返回后新的 VM fork 通知不会再调用它。
 */
int __cold unregister_random_vmfork_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&vmfork_chain, nb);
}
EXPORT_SYMBOL_GPL(unregister_random_vmfork_notifier);
#endif

struct fast_pool {
	unsigned long pool[4];
	unsigned long last;
	unsigned int count;
	struct timer_list mix;
};
/*
 * struct fast_pool - 每 CPU 中断快速混合池
 *
 * @pool: HalfSipHash/SipHash 内部状态。中断路径只做很少轮数的混合，降低开销；
 *        真正进入 input_pool 由定时器回调批量完成。
 * @last: 上次把 fast_pool 混入 input_pool 的 jiffies，用于一秒节流。
 * @count: 自上次混入以来累计中断数；最高位 MIX_INFLIGHT 表示已有 timer 在途。
 * @mix: 绑定到该 CPU 的 timer，用于把 fast_pool 快照提交到全局 input_pool。
 */

static void mix_interrupt_randomness(struct timer_list *work);

static DEFINE_PER_CPU(struct fast_pool, irq_randomness) = {
#ifdef CONFIG_64BIT
#define FASTMIX_PERM SIPHASH_PERMUTATION
	.pool = { SIPHASH_CONST_0, SIPHASH_CONST_1, SIPHASH_CONST_2, SIPHASH_CONST_3 },
#else
#define FASTMIX_PERM HSIPHASH_PERMUTATION
	.pool = { HSIPHASH_CONST_0, HSIPHASH_CONST_1, HSIPHASH_CONST_2, HSIPHASH_CONST_3 },
#endif
	.mix = __TIMER_INITIALIZER(mix_interrupt_randomness, 0)
};
/*
 * 每 CPU 初始化常量来自 SipHash/HalfSipHash 固定 IV。固定 key 本身不提供安全性，
 * 这里只用于把高频中断输入快速扩散，后续还会进入带 key 的 BLAKE2s input_pool。
 */

/*
 * This is [Half]SipHash-1-x, starting from an empty key. Because
 * the key is fixed, it assumes that its inputs are non-malicious,
 * and therefore this has no security on its own. s represents the
 * four-word SipHash state, while v represents a two-word input.
 */
/*
 * fast_mix() - 在中断热路径中低成本扩散两个 word 输入
 *
 * @s: 输入/输出 SipHash 状态，通常是本 CPU fast_pool.pool。
 * @v1: 第一部分输入，常用周期计数。
 * @v2: 第二部分输入，常用 irq/IP/event 编码。
 * 上下文：可在硬中断中调用；不加锁，依赖 per-CPU 和本地中断执行顺序。
 * 返回：无直接返回值；副作用是更新 @s。
 * 安全边界：英文注释强调固定 key 意味着它本身不是密码学安全 MAC；它只是快速
 * 预混合，真正安全压缩发生在 input_pool 的 BLAKE2s。
 */
static void fast_mix(unsigned long s[4], unsigned long v1, unsigned long v2)
{
	s[3] ^= v1;
	FASTMIX_PERM(s[0], s[1], s[2], s[3]);
	s[0] ^= v1;
	s[3] ^= v2;
	FASTMIX_PERM(s[0], s[1], s[2], s[3]);
	s[0] ^= v2;
}

#ifdef CONFIG_SMP
/*
 * This function is called when the CPU has just come online, with
 * entry CPUHP_AP_RANDOM_ONLINE, just after CPUHP_AP_WORKQUEUE_ONLINE.
 */
/*
 * random_online_cpu() - CPU online 后清理可能遗留的中断混合在途标志
 *
 * @cpu: 刚上线的 CPU。
 * 调用位置：CPUHP_AP_RANDOM_ONLINE，workqueue 已对该 CPU online。
 * 返回：0 表示成功。
 * 副作用：清零 fast_pool.count，同时清除 MIX_INFLIGHT；这会丢弃下线期间
 * 无法正确归属的计数，保证新一轮中断从干净状态累计。
 */
int __cold random_online_cpu(unsigned int cpu)
{
	/*
	 * During CPU shutdown and before CPU onlining, add_interrupt_
	 * randomness() may schedule mix_interrupt_randomness(), and
	 * set the MIX_INFLIGHT flag. However, because the worker can
	 * be scheduled on a different CPU during this period, that
	 * flag will never be cleared. For that reason, we zero out
	 * the flag here, which runs just after workqueues are onlined
	 * for the CPU again. This also has the effect of setting the
	 * irq randomness count to zero so that new accumulated irqs
	 * are fresh.
	 */
	per_cpu_ptr(&irq_randomness, cpu)->count = 0;
	return 0;
}
#endif

static void mix_interrupt_randomness(struct timer_list *work)
{
	struct fast_pool *fast_pool = container_of(work, struct fast_pool, mix);
	/*
	 * The size of the copied stack pool is explicitly 2 longs so that we
	 * only ever ingest half of the siphash output each time, retaining
	 * the other half as the next "key" that carries over. The entropy is
	 * supposed to be sufficiently dispersed between bits so on average
	 * we don't wind up "losing" some.
	 */
	/*
	 * 英文注释说明：只复制两个 word，相当于每次只把 SipHash 输出的一半提交给
	 * input_pool，另一半留在 fast_pool.pool 中继续作为下一轮“key-like”状态。
	 * 这样既批量提交中断噪声，又保留跨批次扩散。
	 */
	unsigned long pool[2];
	unsigned int count;

	/* Check to see if we're running on the wrong CPU due to hotplug. */
	/*
	 * timer 可能因 CPU hotplug 在错误 CPU 上执行。关闭本地中断后比较 per-CPU
	 * 地址，若不匹配则不能读取/清零别的 CPU fast_pool，直接返回。
	 */
	local_irq_disable();
	if (fast_pool != this_cpu_ptr(&irq_randomness)) {
		local_irq_enable();
		return;
	}

	/*
	 * Copy the pool to the stack so that the mixer always has a
	 * consistent view, before we reenable irqs again.
	 */
	/*
	 * 复制并清零 count 必须在本地中断关闭期间完成，防止 add_interrupt_randomness()
	 * 同时修改 pool/count。重新开中断后再拿 input_pool.lock，避免长时间关中断。
	 */
	memcpy(pool, fast_pool->pool, sizeof(pool));
	count = fast_pool->count;
	fast_pool->count = 0;
	fast_pool->last = jiffies;
	local_irq_enable();

	mix_pool_bytes(pool, sizeof(pool));
	/*
	 * 每 64 次中断最多 credit 1 bit，且至少 credit 1 bit，以便低频中断也能
	 * 推动早期初始化；上限是本次提交的 pool 位数，避免过度估算。
	 */
	credit_init_bits(clamp_t(unsigned int, (count & U16_MAX) / 64, 1, sizeof(pool) * 8));

	memzero_explicit(pool, sizeof(pool));
}

void add_interrupt_randomness(int irq)
{
	enum { MIX_INFLIGHT = 1U << 31 };
	unsigned long entropy = random_get_entropy();
	struct fast_pool *fast_pool = this_cpu_ptr(&irq_randomness);
	struct pt_regs *regs = get_irq_regs();
	unsigned int new_count;

	/*
	 * 中断路径只做 per-CPU fast_mix，输入包括周期计数、irq 号和指令地址。
	 * 这里不能拿全局 input_pool.lock，否则高频中断会造成严重锁竞争和延迟。
	 */
	fast_mix(fast_pool->pool, entropy,
		 (regs ? instruction_pointer(regs) : _RET_IP_) ^ swab(irq));
	new_count = ++fast_pool->count;

	if (new_count & MIX_INFLIGHT)
		return;

	/*
	 * 未达到 1024 次且距离上次提交不足 1 秒时继续留在 fast_pool 中累计。
	 * 这是吞吐与新鲜度的折中：频繁中断按批提交，低频系统最多约一秒提交一次。
	 */
	if (new_count < 1024 && !time_is_before_jiffies(fast_pool->last + HZ))
		return;

	fast_pool->count |= MIX_INFLIGHT;
	if (!timer_pending(&fast_pool->mix)) {
		/*
		 * 将 timer 固定加到当前 CPU，确保回调读取的是同一个 per-CPU fast_pool。
		 * MIX_INFLIGHT 防止重复排队；random_online_cpu() 会处理 hotplug 遗留位。
		 */
		fast_pool->mix.expires = jiffies;
		add_timer_on(&fast_pool->mix, raw_smp_processor_id());
	}
}
EXPORT_SYMBOL_GPL(add_interrupt_randomness);

/* There is one of these per entropy source */
/*
 * 每个时序熵源一个 timer_rand_state，用于保存事件时间差的一阶、二阶历史。
 * 这不是随机输出缓存，而是估算“本次时间变化有多少不可预测性”的状态。
 */
struct timer_rand_state {
	unsigned long last_time;
	long last_delta, last_delta2;
};

/*
 * This function adds entropy to the entropy "pool" by using timing
 * delays. It uses the timer_rand_state structure to make an estimate
 * of how many bits of entropy this call has added to the pool. The
 * value "num" is also added to the pool; it should somehow describe
 * the type of event that just happened.
 */
/*
 * add_timer_randomness() - 基于事件时序 delta 混入并保守 credit 熵
 *
 * @state: 当前熵源的历史时间差状态；调用者持有其存储期。
 * @num: 描述事件类别的编码，如输入事件 type/code/value 或磁盘 devt。
 * 调用位置：input 层和 block 层事件；硬中断中调用时转入 fast_pool。
 * 锁/上下文：非硬中断路径获取 input_pool.lock；硬中断路径只改 per-CPU fast_pool。
 * 返回：无直接返回值。
 * 估算模型：计算 jiffies delta、一阶差分、二阶差分，再取三者绝对值最小值；
 * 这是保守做法，避免把稳定周期性事件误判为高熵。
 */
static void add_timer_randomness(struct timer_rand_state *state, unsigned int num)
{
	unsigned long entropy = random_get_entropy(), now = jiffies, flags;
	long delta, delta2, delta3;
	unsigned int bits;

	/*
	 * If we're in a hard IRQ, add_interrupt_randomness() will be called
	 * sometime after, so mix into the fast pool.
	 */
	/*
	 * 硬中断中不能走可能较重的 input_pool.lock 路径；先混入本 CPU fast_pool，
	 * 后续 add_interrupt_randomness()/timer 回调会按自己的规则提交和 credit。
	 */
	if (in_hardirq()) {
		fast_mix(this_cpu_ptr(&irq_randomness)->pool, entropy, num);
	} else {
		spin_lock_irqsave(&input_pool.lock, flags);
		_mix_pool_bytes(&entropy, sizeof(entropy));
		_mix_pool_bytes(&num, sizeof(num));
		spin_unlock_irqrestore(&input_pool.lock, flags);
	}

	if (crng_ready())
		return;

	/*
	 * Calculate number of bits of randomness we probably added.
	 * We take into account the first, second and third-order deltas
	 * in order to make our estimate.
	 */
	/*
	 * delta 是本次事件间隔；delta2 表示间隔变化；delta3 表示变化的变化。
	 * 取最小绝对值意味着只要任何一阶表现出规律性，就降低本次 credit。
	 * READ_ONCE/WRITE_ONCE 防止并发事件源读写这些普通字段时出现编译器撕裂
	 * 或 KCSAN 数据竞争噪声；它们不提供锁语义，因此估算仍是近似的。
	 */
	delta = now - READ_ONCE(state->last_time);
	WRITE_ONCE(state->last_time, now);

	delta2 = delta - READ_ONCE(state->last_delta);
	WRITE_ONCE(state->last_delta, delta);

	delta3 = delta2 - READ_ONCE(state->last_delta2);
	WRITE_ONCE(state->last_delta2, delta2);

	if (delta < 0)
		delta = -delta;
	if (delta2 < 0)
		delta2 = -delta2;
	if (delta3 < 0)
		delta3 = -delta3;
	if (delta > delta2)
		delta = delta2;
	if (delta > delta3)
		delta = delta3;

	/*
	 * delta is now minimum absolute delta. Round down by 1 bit
	 * on general principles, and limit entropy estimate to 11 bits.
	 */
	/*
	 * fls(delta >> 1) 相当于取 log2 并主动少算 1 bit；最大 11 bit 是硬上限，
	 * 防止单次事件因偶然大间隔被过度 credit。
	 */
	bits = min(fls(delta >> 1), 11);

	/*
	 * As mentioned above, if we're in a hard IRQ, add_interrupt_randomness()
	 * will run after this, which uses a different crediting scheme of 1 bit
	 * per every 64 interrupts. In order to let that function do accounting
	 * close to the one in this function, we credit a full 64/64 bit per bit,
	 * and then subtract one to account for the extra one added.
	 */
	/*
	 * 硬中断路径不直接 _credit_init_bits(bits)，而是折算到 fast_pool.count。
	 * mix_interrupt_randomness() 后续按“每 64 次 1 bit”的规则 credit，因此这里
	 * 增加 bits*64 并减去已有的一次计数，使两个估算体系大致对齐。
	 */
	if (in_hardirq())
		this_cpu_ptr(&irq_randomness)->count += max(1u, bits * 64) - 1;
	else
		_credit_init_bits(bits);
}

/*
 * add_input_randomness() - 将输入事件类别和值的时序混入 RNG
 *
 * @type: input 子系统事件类型，如 EV_KEY/EV_REL。
 * @code: 类型内事件编码，如按键码或轴编号。
 * @value: 事件值；重复值被视为 autorepeat 一类低价值输入而忽略。
 * 调用位置：input 事件路径。
 * 返回：无直接返回值。
 * 副作用：更新 input_timer_state，并通过 add_timer_randomness() mix/credit
 * 估算出的时序熵。last_value 是全局轻量过滤器，不区分设备，因此只用于
 * 降低明显重复事件影响，不作为安全边界。
 */
void add_input_randomness(unsigned int type, unsigned int code, unsigned int value)
{
	static unsigned char last_value;
	static struct timer_rand_state input_timer_state = { INITIAL_JIFFIES };

	/* Ignore autorepeat and the like. */
	/*
	 * 输入设备长按自动重复会产生高度可预测的 value 序列。只比较 last_value
	 * 是一个轻量过滤，避免把明显重复事件当成新时序来源。
	 */
	if (value == last_value)
		return;

	last_value = value;
	add_timer_randomness(&input_timer_state,
			     (type << 4) ^ code ^ (code >> 4) ^ value);
}
EXPORT_SYMBOL_GPL(add_input_randomness);

#ifdef CONFIG_BLOCK
/*
 * add_disk_randomness() - 将块设备请求时序作为熵源
 *
 * @disk: 产生请求事件的磁盘；可为 NULL。disk->random 在 rand_initialize_disk()
 *        分配，缺失时表示该设备不参与此来源。
 * 返回：无直接返回值。
 * 注意：SSD 等低延迟设备 seek time 规律性强，函数仍会保守估算 credit，而不是
 * 盲目信任每个 I/O 都有高熵。
 */
void add_disk_randomness(struct gendisk *disk)
{
	if (!disk || !disk->random)
		return;
	/* First major is 1, so we get >= 0x200 here. */
	/*
	 * devt 编码作为事件类别混入，避免不同磁盘共享同一个 timer_rand_state 时
	 * 语义混淆；真正的不可预测性仍主要来自请求到达时间差。
	 */
	add_timer_randomness(disk->random, 0x100 + disk_devt(disk));
}
EXPORT_SYMBOL_GPL(add_disk_randomness);

void __cold rand_initialize_disk(struct gendisk *disk)
{
	struct timer_rand_state *state;

	/*
	 * If kzalloc returns null, we just won't use that entropy
	 * source.
	 */
	/*
	 * 英文注释说明：分配失败只是不使用该磁盘作为时序熵源，不影响块设备本身。
	 * state 的所有权转移给 disk->random，释放由磁盘生命周期对应路径负责。
	 */
	state = kzalloc_obj(struct timer_rand_state);
	if (state) {
		state->last_time = INITIAL_JIFFIES;
		disk->random = state;
	}
}
#endif

struct entropy_timer_state {
	unsigned long entropy;
	struct timer_list timer;
	atomic_t samples;
	unsigned int samples_per_bit;
};
/*
 * struct entropy_timer_state - wait_for_random_bytes() 主动造熵的临时状态
 *
 * @entropy: 当前 CPU 读到的周期计数样本，主循环和 timer 回调都会混入。
 * @timer: 栈上 timer，用于在其他 timer CPU 上制造调度/中断抖动。
 * @samples: timer 回调样本计数；每 samples_per_bit 次 credit 1 bit。
 * @samples_per_bit: 启动前探测出的“多少样本约等于 1 bit”的保守比例。
 * 生命周期：对象位于 try_to_generate_entropy() 栈上，timer_setup_on_stack()
 * 后必须在函数退出前 timer_delete_sync()/timer_destroy_on_stack()。
 */

/*
 * Each time the timer fires, we expect that we got an unpredictable jump in
 * the cycle counter. Even if the timer is running on another CPU, the timer
 * activity will be touching the stack of the CPU that is generating entropy.
 *
 * Note that we don't re-arm the timer in the timer itself - we are happy to be
 * scheduled away, since that just makes the load more complex, but we do not
 * want the timer to keep ticking unless the entropy loop is running.
 *
 * So the re-arming always happens in the entropy loop itself.
 */
/*
 * entropy_timer() - 主动造熵循环的 timer 回调
 *
 * @timer: 嵌入 entropy_timer_state 的栈上 timer。
 * 上下文：timer softirq；不能睡眠。只读取周期计数、mix 到 input_pool，并按样本
 * 计数偶尔 credit 1 bit。
 * 返回：无直接返回值。
 * 设计原因：timer 回调运行在可能不同的 CPU/时刻，与等待线程的 schedule() 交错，
 * 让 cycle counter 差异包含调度、tick、cache 和中断扰动。
 */
static void __cold entropy_timer(struct timer_list *timer)
{
	struct entropy_timer_state *state = container_of(timer, struct entropy_timer_state, timer);
	unsigned long entropy = random_get_entropy();

	mix_pool_bytes(&entropy, sizeof(entropy));
	if (atomic_inc_return(&state->samples) % state->samples_per_bit == 0)
		credit_init_bits(1);
}

/*
 * If we have an actual cycle counter, see if we can generate enough entropy
 * with timing noise.
 */
/*
 * try_to_generate_entropy() - 在等待 RNG ready 时主动利用计时抖动采集熵
 *
 * 调用位置：wait_for_random_bytes() 和 /dev/urandom 早期机会性初始化。
 * 入参：无。
 * 上下文：必须允许睡眠；会分配 cpumask、使用栈上 timer、schedule()，并可被信号中断。
 * 返回：无直接返回值；可能通过 credit_init_bits() 推进 CRNG_READY，也可能因硬件
 * 无可用 cycle counter 或信号到达而无效果返回。
 * 阶段：
 *   1. 快速采样 NUM_TRIAL_SAMPLES 次，确认 cycle counter 有足够变化；
 *   2. 建立栈上 timer 和 timer CPU 集合；
 *   3. 循环把 timer 安排到其他/合适 CPU，当前线程 schedule() 制造交错；
 *   4. ready、信号或资源失败时同步删除 timer 并销毁栈上对象。
 */
static void __cold try_to_generate_entropy(void)
{
	enum { NUM_TRIAL_SAMPLES = 8192, MAX_SAMPLES_PER_BIT = HZ / 15 };
	u8 stack_bytes[sizeof(struct entropy_timer_state) + SMP_CACHE_BYTES - 1];
	struct entropy_timer_state *stack = PTR_ALIGN((void *)stack_bytes, SMP_CACHE_BYTES);
	unsigned int i, num_different = 0;
	unsigned long last = random_get_entropy();
	cpumask_var_t timer_cpus;
	int cpu = -1;

	for (i = 0; i < NUM_TRIAL_SAMPLES - 1; ++i) {
		/*
		 * 先做探测，避免在没有真实 cycle counter 的平台上进入昂贵循环。
		 * num_different 越小，samples_per_bit 越大；超过上限则认为不可用。
		 */
		stack->entropy = random_get_entropy();
		if (stack->entropy != last)
			++num_different;
		last = stack->entropy;
	}
	stack->samples_per_bit = DIV_ROUND_UP(NUM_TRIAL_SAMPLES, num_different + 1);
	if (stack->samples_per_bit > MAX_SAMPLES_PER_BIT)
		return;

	/*
	 * timer 位于栈上，因此从 setup 到 destroy 的所有路径都必须确保没有回调
	 * 仍在运行。out 标签统一做 timer_delete_sync()/timer_destroy_on_stack()。
	 */
	atomic_set(&stack->samples, 0);
	timer_setup_on_stack(&stack->timer, entropy_timer, 0);
	if (!alloc_cpumask_var(&timer_cpus, GFP_KERNEL))
		goto out;

	while (!crng_ready() && !signal_pending(current)) {
		/*
		 * Check !timer_pending() and then ensure that any previous callback has finished
		 * executing by checking timer_delete_sync_try(), before queueing the next one.
		 */
		/*
		 * 英文注释说明：先确认 timer 未挂起，再用 timer_delete_sync_try()
		 * 确认旧回调没有仍在执行，之后才重新 add_timer_on()。这是栈上 timer
		 * 的生命周期要求，否则函数栈帧退出或重用时可能 UAF。
		 */
		if (!timer_pending(&stack->timer) && timer_delete_sync_try(&stack->timer) >= 0) {
			unsigned int num_cpus;

			/*
			 * Preemption must be disabled here, both to read the current CPU number
			 * and to avoid scheduling a timer on a dead CPU.
			 */
			/*
			 * 关抢占后 smp_processor_id() 稳定，且选择 timer CPU 到 add_timer_on()
			 * 之间当前 CPU 不会迁移；同时降低把 timer 投递到刚下线 CPU 的竞态窗口。
			 */
			preempt_disable();

			/* Only schedule callbacks on timer CPUs that are online. */
			/*
			 * 优先使用 housekeeping timer CPU，避免 NOHZ_FULL/隔离 CPU 被这个
			 * 主动造熵循环打扰；若配置异常没有 timer housekeeping CPU，则退回
			 * 所有 online CPU 保证能继续推进。
			 */
			cpumask_and(timer_cpus, housekeeping_cpumask(HK_TYPE_TIMER), cpu_online_mask);
			num_cpus = cpumask_weight(timer_cpus);
			/* In very bizarre case of misconfiguration, fallback to all online. */
			if (unlikely(num_cpus == 0)) {
				*timer_cpus = *cpu_online_mask;
				num_cpus = cpumask_weight(timer_cpus);
			}

			/* Basic CPU round-robin, which avoids the current CPU. */
			/*
			 * 简单轮转并尽量避开当前 CPU，让 timer 回调和等待线程运行在不同 CPU，
			 * 增加调度和跨 CPU 时序抖动；单 CPU 系统则只能选择自己。
			 */
			do {
				cpu = cpumask_next(cpu, timer_cpus);
				if (cpu >= nr_cpu_ids)
					cpu = cpumask_first(timer_cpus);
			} while (cpu == smp_processor_id() && num_cpus > 1);

			/* Expiring the timer at `jiffies` means it's the next tick. */
			/*
			 * expires=jiffies 表示尽快在下一个 tick 触发，而不是等待额外延迟。
			 * add_timer_on() 后 timer 生命周期由 out 清理路径负责同步收尾。
			 */
			stack->timer.expires = jiffies;

			add_timer_on(&stack->timer, cpu);

			preempt_enable();
		}
		mix_pool_bytes(&stack->entropy, sizeof(stack->entropy));
		schedule();
		stack->entropy = random_get_entropy();
	}
	mix_pool_bytes(&stack->entropy, sizeof(stack->entropy));

	free_cpumask_var(timer_cpus);
out:
	timer_delete_sync(&stack->timer);
	timer_destroy_on_stack(&stack->timer);
}


/**********************************************************************
 *
 * Userspace reader/writer interfaces.
 *
 * getrandom(2) is the primary modern interface into the RNG and should
 * be used in preference to anything else.
 *
 * Reading from /dev/random has the same functionality as calling
 * getrandom(2) with flags=0. In earlier versions, however, it had
 * vastly different semantics and should therefore be avoided, to
 * prevent backwards compatibility issues.
 *
 * Reading from /dev/urandom has the same functionality as calling
 * getrandom(2) with flags=GRND_INSECURE. Because it does not block
 * waiting for the RNG to be ready, it should not be used.
 *
 * Writing to either /dev/random or /dev/urandom adds entropy to
 * the input pool but does not credit it.
 *
 * Polling on /dev/random indicates when the RNG is initialized, on
 * the read side, and when it wants new entropy, on the write side.
 *
 * Both /dev/random and /dev/urandom have the same set of ioctls for
 * adding entropy, getting the entropy count, zeroing the count, and
 * reseeding the crng.
 *
 **********************************************************************/
/*
 * 本节是 ABI 层。现代语义下：
 *   getrandom(flags=0) 和 /dev/random 读都会等待 CRNG_READY；
 *   getrandom(GRND_INSECURE) 和 /dev/urandom 允许未 ready 输出，但会警告；
 *   写 /dev/random 或 /dev/urandom 只 mix，不 credit；
 *   ioctl 中只有具备 CAP_SYS_ADMIN 的 RNDADDENTROPY/RNDADDTOENTCNT 能 credit。
 */

/*
 * getrandom(2) - 用户态主随机数系统调用
 *
 * @ubuf: 用户态输出缓冲区；成功复制时被写入随机字节。
 * @len: 请求字节数；0 返回 0。
 * @flags: GRND_NONBLOCK、GRND_RANDOM、GRND_INSECURE 的组合；其他位非法。
 * 调用位置：体系结构 syscall wrapper 进入本函数。
 * 上下文：进程上下文，可睡眠；阻塞模式会等待 CRNG_READY，复制用户页也可能睡眠。
 * 返回：成功返回复制字节数；-EINVAL 表示 flag 组合非法；-EAGAIN 表示非阻塞且未 ready；
 * wait_for_random_bytes()/import_ubuf()/get_random_bytes_user() 的错误原样向上传递。
 * 语义边界：GRND_RANDOM 现在不再提供旧版 /dev/random 的“熵耗尽阻塞”模型；它与
 * flags=0 一样等待初始化。GRND_INSECURE 明确选择早期不安全输出。
 */
SYSCALL_DEFINE3(getrandom, char __user *, ubuf, size_t, len, unsigned int, flags)
{
	struct iov_iter iter;
	int ret;

	if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
		return -EINVAL;

	/*
	 * Requesting insecure and blocking randomness at the same time makes
	 * no sense.
	 */
	/*
	 * 英文注释说明：同时要求 GRND_INSECURE 和 GRND_RANDOM 没有语义。前者表示
	 * 不等待 ready 的早期输出，后者历史上表示 /dev/random 风格阻塞接口。
	 */
	if ((flags & (GRND_INSECURE | GRND_RANDOM)) == (GRND_INSECURE | GRND_RANDOM))
		return -EINVAL;

	if (!crng_ready() && !(flags & GRND_INSECURE)) {
		/*
		 * 安全模式在 ready 前必须等待。NONBLOCK 把“尚未 ready”转换为 -EAGAIN；
		 * 阻塞模式则进入 wait_for_random_bytes()，可被信号中断。
		 */
		if (flags & GRND_NONBLOCK)
			return -EAGAIN;
		ret = wait_for_random_bytes();
		if (unlikely(ret))
			return ret;
	}

	ret = import_ubuf(ITER_DEST, ubuf, len, &iter);
	if (unlikely(ret))
		return ret;
	/*
	 * import_ubuf() 只建立 iov_iter，不复制数据；真正的随机生成和用户页复制
	 * 在 get_random_bytes_user() 中完成，并可能返回短复制长度。
	 */
	return get_random_bytes_user(&iter);
}

/*
 * random_poll() - /dev/random poll/select 就绪状态
 *
 * @file: 被 poll 的字符设备文件。
 * @wait: poll table；用于把调用者挂到 crng_init_wait。
 * 返回：ready 时报告可读；未 ready 时报告可写，提示用户可写入更多 seed。
 * 副作用：poll_wait() 只注册等待关系，不直接睡眠；ready 发布时
 * wake_up_interruptible(&crng_init_wait) 唤醒 poller。
 */
static __poll_t random_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &crng_init_wait, wait);
	return crng_ready() ? EPOLLIN | EPOLLRDNORM : EPOLLOUT | EPOLLWRNORM;
}

/*
 * write_pool_user() - 从用户 iov_iter 读取 seed 并混入 input_pool
 *
 * @iter: 输入/输出参数；入口描述用户源缓冲区，返回时推进已读取位置。
 * 调用位置：/dev/random、/dev/urandom 写，以及 ioctl RNDADDENTROPY。
 * 上下文：copy_from_iter() 可能睡眠；每页边界检查信号并 cond_resched()。
 * 返回：成功返回已混入字节数，可短写；若一个字节也未复制则返回 -EFAULT。
 * 安全边界：本函数只 mix，不 credit。RNDADDENTROPY 的调用者在确认全部写入后
 * 才单独 credit ent_count。
 */
static ssize_t write_pool_user(struct iov_iter *iter)
{
	u8 block[BLAKE2S_BLOCK_SIZE];
	ssize_t ret = 0;
	size_t copied;

	if (unlikely(!iov_iter_count(iter)))
		return 0;

	for (;;) {
		/*
		 * 分块复制到栈上 block，立即 mix 复制成功的部分。即使用户缓冲区中途
		 * fault，已经复制的字节也被保留为输入；是否接受短写由上层接口决定。
		 */
		copied = copy_from_iter(block, sizeof(block), iter);
		ret += copied;
		mix_pool_bytes(block, copied);
		if (!iov_iter_count(iter) || copied != sizeof(block))
			break;

		/*
		 * 大写入按页让出 CPU 并响应信号，避免特权进程一次写入巨大 seed 时
		 * 长时间占用内核态。返回值保留已成功混入的字节数。
		 */
		BUILD_BUG_ON(PAGE_SIZE % sizeof(block) != 0);
		if (ret % PAGE_SIZE == 0) {
			if (signal_pending(current))
				break;
			cond_resched();
		}
	}

	memzero_explicit(block, sizeof(block));
	return ret ? ret : -EFAULT;
}

/*
 * random_write_iter() - /dev/random 和 /dev/urandom 的写入口
 *
 * @kiocb: VFS I/O 控制块，当前不使用其中状态。
 * @iter: 用户源迭代器。
 * 返回：write_pool_user() 的字节数或错误。
 * 语义：两个设备写入都只向 input_pool 添加数据，不因普通 write 增加熵计数。
 */
static ssize_t random_write_iter(struct kiocb *kiocb, struct iov_iter *iter)
{
	return write_pool_user(iter);
}

/*
 * urandom_read_iter() - /dev/urandom 读入口，允许未 ready 早期输出
 *
 * @kiocb: VFS I/O 控制块。
 * @iter: 用户目标迭代器。
 * 返回：get_random_bytes_user() 的复制字节数或 -EFAULT。
 * 行为：未 ready 时机会性尝试造熵；若仍未 ready，按 ratelimit 打印警告，但不阻塞。
 */
static ssize_t urandom_read_iter(struct kiocb *kiocb, struct iov_iter *iter)
{
	static int maxwarn = 10;

	/*
	 * Opportunistically attempt to initialize the RNG on platforms that
	 * have fast cycle counters, but don't (for now) require it to succeed.
	 */
	/*
	 * 英文注释说明：有快速 cycle counter 的平台可能通过主动造熵完成初始化。
	 * /dev/urandom 不要求该尝试成功，因为其 ABI 允许早期不安全输出。
	 */
	if (!crng_ready())
		try_to_generate_entropy();

	if (!crng_ready()) {
		/*
		 * maxwarn 控制总警告次数，ratelimit 控制单位时间输出。miss 计数会在
		 * ready 发布时由 _credit_init_bits() 汇总打印，告诉用户有多少警告被抑制。
		 */
		if (!ratelimit_disable && maxwarn <= 0)
			ratelimit_state_inc_miss(&urandom_warning);
		else if (ratelimit_disable || __ratelimit(&urandom_warning)) {
			--maxwarn;
			pr_notice("%s: uninitialized urandom read (%zu bytes read)\n",
				  current->comm, iov_iter_count(iter));
		}
	}

	return get_random_bytes_user(iter);
}

/*
 * random_read_iter() - /dev/random 读入口，等价于 getrandom(flags=0)
 *
 * @kiocb: VFS I/O 控制块；NOWAIT/NOIO 或 O_NONBLOCK 会把未 ready 转成 -EAGAIN。
 * @iter: 用户目标迭代器。
 * 返回：ready 后返回复制字节数；等待被信号中断返回 wait_for_random_bytes() 错误。
 * 语义：现代 /dev/random 不再按“熵池耗尽”反复阻塞，只等待一次 CRNG_READY。
 */
static ssize_t random_read_iter(struct kiocb *kiocb, struct iov_iter *iter)
{
	int ret;

	if (!crng_ready() &&
	    ((kiocb->ki_flags & (IOCB_NOWAIT | IOCB_NOIO)) ||
	     (kiocb->ki_filp->f_flags & O_NONBLOCK)))
		/*
		 * 非阻塞语义在进入 wait_for_random_bytes() 前处理，避免 VFS 调用者
		 * 被挂到等待队列或触发主动造熵。
		 */
		return -EAGAIN;

	ret = wait_for_random_bytes();
	if (ret != 0)
		return ret;
	return get_random_bytes_user(iter);
}

/*
 * random_ioctl() - /dev/random 与 /dev/urandom 的传统 ioctl 兼容入口
 *
 * @f: 设备文件，当前不使用。
 * @cmd: RNDGETENTCNT、RNDADDENTROPY 等命令。
 * @arg: 用户指针或整数参数，按命令解释。
 * 返回：0/字节语义成功，或 -EINVAL/-EPERM/-EFAULT/-ENODATA 等错误。
 * 权限：会改变 credit 或强制 reseed 的命令需要 CAP_SYS_ADMIN；只读熵计数不需要。
 * 兼容边界：RNDZAPENTCNT/RNDCLEARPOOL 保留 ABI 但不再清空池或降低安全状态。
 */
static long random_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int __user *p = (int __user *)arg;
	int ent_count;

	switch (cmd) {
	case RNDGETENTCNT:
		/* Inherently racy, no point locking. */
		/*
		 * 英文注释说明：读取 init_bits 天生有竞态，锁住也只能得到瞬时值。
		 * 用户态只能把它当作诊断信息，不能作为安全决策的强同步点。
		 */
		if (put_user(input_pool.init_bits, p))
			return -EFAULT;
		return 0;
	case RNDADDTOENTCNT:
		/*
		 * 只调整熵计数，不混入新数据。要求 CAP_SYS_ADMIN，因为错误 credit
		 * 会让系统过早宣布 CRNG_READY。
		 */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count, p))
			return -EFAULT;
		if (ent_count < 0)
			return -EINVAL;
		credit_init_bits(ent_count);
		return 0;
	case RNDADDENTROPY: {
		struct iov_iter iter;
		ssize_t ret;
		int len;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count, p++))
			return -EFAULT;
		if (ent_count < 0)
			return -EINVAL;
		if (get_user(len, p++))
			return -EFAULT;
		ret = import_ubuf(ITER_SOURCE, p, len, &iter);
		if (unlikely(ret))
			return ret;
		ret = write_pool_user(&iter);
		if (unlikely(ret < 0))
			return ret;
		/* Since we're crediting, enforce that it was all written into the pool. */
		/*
		 * 英文注释说明：因为随后要 credit，必须确认用户声明的 len 字节全部
		 * 已进入 input_pool。若允许短写仍 credit，会夸大实际注入数据质量。
		 */
		if (unlikely(ret != len))
			return -EFAULT;
		credit_init_bits(ent_count);
		return 0;
	}
	case RNDZAPENTCNT:
	case RNDCLEARPOOL:
		/* No longer has any effect. */
		/*
		 * 旧 ABI 曾用于清空/重置熵计数。当前实现保持 no-op，是为了避免用户态
		 * 误以为可以把已经 ready 的 CRNG 降级或擦除历史池状态。
		 */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return 0;
	case RNDRESEEDCRNG:
		/*
		 * 管理员可要求立即 reseed，但只有 ready 后才有完整安全语义；未 ready
		 * 返回 -ENODATA，避免把早期不足熵状态伪装成一次完整 reseed。
		 */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (!crng_ready())
			return -ENODATA;
		crng_reseed(NULL);
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * random_fasync() - 维护 /dev/random 的异步通知订阅
 *
 * @fd: 文件描述符。
 * @filp: VFS file。
 * @on: 非 0 表示注册，0 表示注销。
 * 返回：fasync_helper() 的结果。
 * 副作用：更新全局 fasync 链表；CRNG_READY 发布时 kill_fasync() 用它向用户态
 * 发送 SIGIO/POLL_IN。
 */
static int random_fasync(int fd, struct file *filp, int on)
{
	return fasync_helper(fd, filp, on, &fasync);
}

/*
 * random_fops/urandom_fops - 字符设备操作表
 *
 * 两者共享 write/ioctl/fasync/llseek/splice 语义；差异只在 read_iter：
 * /dev/random 阻塞等待 ready，/dev/urandom 允许早期不安全读并警告。
 * VFS open 字符设备后通过 file->f_op 间接分派到这些函数。
 */
const struct file_operations random_fops = {
	.read_iter = random_read_iter,
	.write_iter = random_write_iter,
	.poll = random_poll,
	.unlocked_ioctl = random_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.fasync = random_fasync,
	.llseek = noop_llseek,
	.splice_read = copy_splice_read,
	.splice_write = iter_file_splice_write,
};

const struct file_operations urandom_fops = {
	.read_iter = urandom_read_iter,
	.write_iter = random_write_iter,
	.unlocked_ioctl = random_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.fasync = random_fasync,
	.llseek = noop_llseek,
	.splice_read = copy_splice_read,
	.splice_write = iter_file_splice_write,
};


/********************************************************************
 *
 * Sysctl interface.
 *
 * These are partly unused legacy knobs with dummy values to not break
 * userspace and partly still useful things. They are usually accessible
 * in /proc/sys/kernel/random/ and are as follows:
 *
 * - boot_id - a UUID representing the current boot.
 *
 * - uuid - a random UUID, different each time the file is read.
 *
 * - poolsize - the number of bits of entropy that the input pool can
 *   hold, tied to the POOL_BITS constant.
 *
 * - entropy_avail - the number of bits of entropy currently in the
 *   input pool. Always <= poolsize.
 *
 * - write_wakeup_threshold - the amount of entropy in the input pool
 *   below which write polls to /dev/random will unblock, requesting
 *   more entropy, tied to the POOL_READY_BITS constant. It is writable
 *   to avoid breaking old userspaces, but writing to it does not
 *   change any behavior of the RNG.
 *
 * - urandom_min_reseed_secs - fixed to the value CRNG_RESEED_INTERVAL.
 *   It is writable to avoid breaking old userspaces, but writing
 *   to it does not change any behavior of the RNG.
 *
 ********************************************************************/
/*
 * sysctl 段主要是 /proc/sys/kernel/random ABI 兼容层。poolsize、entropy_avail
 * 暴露当前常量/计数；write_wakeup_threshold 和 urandom_min_reseed_secs 可写但
 * 写入被忽略；boot_id 是本次启动稳定 UUID；uuid 每次读取生成新 UUID。
 */

#ifdef CONFIG_SYSCTL

#include <linux/sysctl.h>

static int sysctl_random_min_urandom_seed = CRNG_RESEED_INTERVAL / HZ;
static int sysctl_random_write_wakeup_bits = POOL_READY_BITS;
static int sysctl_poolsize = POOL_BITS;
static u8 sysctl_bootid[UUID_SIZE];
/*
 * sysctl_* 变量是 proc handler 的 backing storage。部分变量不是 RNG 行为参数，
 * 而是为了旧用户态读取/写入路径仍存在而提供的虚拟值。
 */

/*
 * This function is used to return both the bootid UUID, and random
 * UUID. The difference is in whether table->data is NULL; if it is,
 * then a new UUID is generated and returned to the user.
 */
/*
 * proc_do_uuid() - 处理 boot_id 和 uuid 两个 sysctl 文件
 *
 * @table: sysctl 表项；table->data 为 NULL 表示 /uuid，每次生成新 UUID；
 *         非 NULL 表示 /boot_id，使用持久 sysctl_bootid。
 * @write: 非 0 表示用户写入；本接口拒绝写入。
 * @buf/@lenp/@ppos: proc sysctl 通用读写缓冲、长度和偏移参数，传给 proc_dostring()。
 * 返回：0 或 proc_dostring()/权限错误。
 * 并发：boot_id 第一次生成受 bootid_spinlock 保护，避免多个读者同时生成不同值。
 */
static int proc_do_uuid(const struct ctl_table *table, int write, void *buf,
			size_t *lenp, loff_t *ppos)
{
	u8 tmp_uuid[UUID_SIZE], *uuid;
	char uuid_string[UUID_STRING_LEN + 1];
	struct ctl_table fake_table = {
		.data = uuid_string,
		.maxlen = UUID_STRING_LEN
	};

	if (write)
		return -EPERM;

	uuid = table->data;
	if (!uuid) {
		/*
		 * /proc/sys/kernel/random/uuid 没有 backing storage：每次读取都在栈上
		 * 生成新 UUID，随后格式化成字符串返回。
		 */
		uuid = tmp_uuid;
		generate_random_uuid(uuid);
	} else {
		static DEFINE_SPINLOCK(bootid_spinlock);

		spin_lock(&bootid_spinlock);
		/*
		 * sysctl_bootid 初始为全零。这里以 uuid[8]==0 作为尚未初始化哨兵；
		 * 加锁保证本次启动内所有读者最终看到同一个 boot_id。
		 */
		if (!uuid[8])
			generate_random_uuid(uuid);
		spin_unlock(&bootid_spinlock);
	}

	snprintf(uuid_string, sizeof(uuid_string), "%pU", uuid);
	return proc_dostring(&fake_table, 0, buf, lenp, ppos);
}

/* The same as proc_dointvec, but writes don't change anything. */
/*
 * proc_do_rointvec() - 兼容“可写但写入无效果”的整数 sysctl
 *
 * @table/@write/@buf/@lenp/@ppos: proc sysctl 通用参数。
 * 返回：写入时直接返回 0；读取时调用 proc_dointvec()。
 * 用途：保留旧 ABI 中可写 knob 的表面行为，同时明确不再改变 RNG 内部策略。
 */
static int proc_do_rointvec(const struct ctl_table *table, int write, void *buf,
			    size_t *lenp, loff_t *ppos)
{
	return write ? 0 : proc_dointvec(table, 0, buf, lenp, ppos);
}

static const struct ctl_table random_table[] = {
	/*
	 * 表项按 /proc/sys/kernel/random/ 下的文件组织。data 指向 backing storage，
	 * proc_handler 决定读写语义；mode 0444 表示只读，0644 表示兼容写入口存在。
	 */
	{
		.procname	= "poolsize",
		.data		= &sysctl_poolsize,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "entropy_avail",
		.data		= &input_pool.init_bits,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "write_wakeup_threshold",
		.data		= &sysctl_random_write_wakeup_bits,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_do_rointvec,
	},
	{
		.procname	= "urandom_min_reseed_secs",
		.data		= &sysctl_random_min_urandom_seed,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_do_rointvec,
	},
	{
		.procname	= "boot_id",
		.data		= &sysctl_bootid,
		.mode		= 0444,
		.proc_handler	= proc_do_uuid,
	},
	{
		.procname	= "uuid",
		.mode		= 0444,
		.proc_handler	= proc_do_uuid,
	},
};

/*
 * random_init() is called before sysctl_init(),
 * so we cannot call register_sysctl_init() in random_init()
 */
/*
 * random_sysctls_init() - 在 sysctl 子系统就绪后注册 kernel/random 表
 *
 * 调用位置：device_initcall，晚于 random_init()。
 * 入参：无。
 * 返回：0；register_sysctl_init() 负责持有表项生命周期。
 * 设计原因：英文注释指出 random_init() 早于 sysctl_init()，因此不能在 RNG
 * 初始化函数中直接注册 sysctl，只能通过 initcall 延后。
 */
static int __init random_sysctls_init(void)
{
	register_sysctl_init("kernel/random", random_table);
	return 0;
}
device_initcall(random_sysctls_init);
#endif
