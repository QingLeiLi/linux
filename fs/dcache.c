// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcache.c 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件实现 VFS 的目录项缓存（dcache）。dentry 表示“某个父目录下的一个
 * 名字”，把路径分量与 inode 连接起来；它缓存的是命名关系而不是文件内容。
 * 正 dentry 的 d_inode 指向实际 inode，负 dentry 的 d_inode 为 NULL，用来
 * 缓存“不存在”这一查找结果。具体文件系统仍负责从磁盘或远端服务器执行
 * ->lookup()、创建、删除和 rename，本文件负责通用对象、索引和并发协议。
 *
 * 顺序阅读可沿五条主线：
 *
 *   分配与发布：__d_alloc() -> d_alloc[_parallel]() -> d_add()/
 *                 d_instantiate() -> __d_rehash()
 *   路径查找：   d_hash_and_lookup() -> d_lookup()/__d_lookup_rcu()
 *   引用与销毁： dget()（头文件内联）-> dput() -> dentry_kill() ->
 *                 dentry_free() -> RCU 回调
 *   内存回收：   list_lru shrinker -> dentry_lru_isolate() ->
 *                 shrink_dentry_list()
 *   重命名：     d_move()/d_exchange() -> __d_move()，同时更新父子树、
 *                 名字、全局哈希和各 dentry 的序列计数。
 *
 * 核心生命周期是“分配但不可查找 -> 挂入父目录树 -> 可选绑定 inode ->
 * 加入全局哈希而可查找 -> 最后一个主动引用释放后进入 LRU 或立即淘汰 ->
 * 从哈希、inode alias 和父子树依次摘除 -> RCU 宽限期后释放”。引用计数只
 * 保证对象存活；d_lock/d_seq/rename_lock 分别保证字段更新、无锁快照验证和
 * 跨 dentry 的树形结构变更，三者不能互相替代。
 *
 * 并发设计的收益是普通路径查找可在 RCU-walk 下不写共享缓存、不取每个
 * dentry 的自旋锁；代价是 rename 允许读者得到可检测的旧快照，读者必须用
 * d_seq/rename_lock 重试，并且最终释放必须延迟到 RCU 读者退出。内存压力下
 * 只回收零引用对象；“在 LRU 上”不是已经死亡，而是可再次被查找并复活。
 */
/*
 * fs/dcache.c
 *
 * Complete reimplementation
 * (C) 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */

/*
 * Notes on the allocation strategy:
 *
 * The dcache is a master of the icache - whenever a dcache entry
 * exists, the inode will always exist. "iput()" is done either when
 * the dcache entry is deleted or garbage collected.
 */

/*
 * 分配策略要点：dcache 在生命周期上支配 icache。只要正 dentry 仍绑定
 * inode，dcache 就替该命名关系持有 inode 引用，因此 inode 不会先于 dentry
 * 消失；删除命名关系或回收 dentry 时，dentry_unlink_inode() 才通过 d_iput()
 * 或 iput() 交还这份引用。负 dentry 没有 inode，只缓存一次失败查找。
 */

#include <linux/ratelimit.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/fscrypt.h>
#include <linux/fsnotify.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/cache.h>
#include <linux/export.h>
#include <linux/security.h>
#include <linux/seqlock.h>
#include <linux/memblock.h>
#include <linux/bit_spinlock.h>
#include <linux/rculist_bl.h>
#include <linux/list_lru.h>
#include "internal.h"
#include "mount.h"

#include <asm/runtime-const.h>

/*
 * Usage:
 * dcache->d_inode->i_lock protects:
 *   - i_dentry, d_alias, d_inode of aliases
 * dcache_hash_bucket lock protects:
 *   - the dcache hash table
 * s_roots_lock protects:
 *   - the s_roots list (see __d_move()/dentry_unlist()/d_obtain_root())
 * dentry->d_sb->s_dentry_lru_lock protects:
 *   - the dcache lru lists and counters
 * d_lock protects:
 *   - d_flags
 *   - d_name
 *   - d_lru
 *   - d_count
 *   - d_unhashed()
 *   - d_parent and d_chilren
 *   - childrens' d_sib and d_parent
 *   - d_alias, d_inode
 *
 * Ordering:
 * dentry->d_inode->i_lock
 *   dentry->d_lock
 *     dentry->d_sb->s_dentry_lru_lock
 *     dcache_hash_bucket lock
 *     s_roots lock
 *
 * If there is an ancestor relationship:
 * dentry->d_parent->...->d_parent->d_lock
 *   ...
 *     dentry->d_parent->d_lock
 *       dentry->d_lock
 *
 * If no ancestor relationship:
 * arbitrary, since it's serialized on rename_lock
 */
/*
 * 上述锁表是整文件的锁序约束：inode->i_lock 保护同一 inode 的 alias 集合和
 * alias 的 d_inode 绑定；d_lock 保护单个 dentry 的名字、父子关系、引用计数
 * 及 LRU 状态；桶锁只保护哈希链本身；s_roots_lock 保护不在普通父子树中的
 * 次级根；每 superblock 的 LRU 锁保护回收链表。
 *
 * 正 dentry 淘汰需要同时改变 inode alias 与 dentry，故必须先 i_lock 后
 * d_lock。祖先锁从上到下取得，避免两个树操作形成 ABBA；无祖先关系时由
 * rename_lock 把拓扑修改串行化。锁序错误不仅可能死锁，还会让无锁 path walk
 * 看到名字、父指针和哈希位置来自不同版本。
 */

/*
 * 两个 sysctl 构成有理数 pressure/denom；shrinker 用该比例把通用扫描目标
 * 换算成 VFS cache 的回收力度。二者初始化后主要由 sysctl 写路径更新，
 * __read_mostly 使高频只读访问避免与其他频繁写变量共享 cache line。
 */
static int sysctl_vfs_cache_pressure __read_mostly = 100;
static int sysctl_vfs_cache_pressure_denom __read_mostly = 100;

/*
 * vfs_pressure_ratio() - 按当前 VFS cache pressure 缩放扫描量。
 * @val 是调用者给出的无符号基准数量；函数不持锁、不睡眠，只读取 sysctl
 * 配置并用 mult_frac() 降低先乘后除的溢出风险。返回缩放后的同单位数量，
 * 不取得或转移任何对象引用。
 */
unsigned long vfs_pressure_ratio(unsigned long val)
{
	return mult_frac(val, sysctl_vfs_cache_pressure, sysctl_vfs_cache_pressure_denom);
}
EXPORT_SYMBOL_GPL(vfs_pressure_ratio);

/*
 * rename_lock 是全局 dentry 拓扑序列锁。rename/move/exchange 写侧在改变名字、
 * parent 与哈希位置时推进序列；RCU 路径读者读取序列并在变化后重试。SMP 下
 * 独占缓存线减少读序列与无关变量的伪共享。它不持有任何 dentry 引用，也不
 * 替代 d_lock 对单对象字段的互斥保护。
 */
__cacheline_aligned_in_smp DEFINE_SEQLOCK(rename_lock);

/*
 * rename_lock 是全局树拓扑版本与慢路径串行点。写侧覆盖 rename/move/exchange
 * 等跨 dentry 更新；读侧通常先按 seqcount 乐观遍历，检测到并发写后再重试。
 * cacheline 对齐避免高频读序列值与无关数据发生伪共享。它不延长 dentry
 * 生命周期，读者仍需 RCU 或显式引用。
 */

EXPORT_SYMBOL(rename_lock);

/*
 * __dentry_cache 是 dcache_init() 创建后只读的 slab 描述符；所有 dentry 存储
 * 从这里分配并最终归还。dentry_cache 宏通过 runtime_const_ptr() 让初始化完成
 * 后的热路径把该指针特化为运行期常量；二者不承载单个对象的引用所有权。
 */
static struct kmem_cache *__dentry_cache __ro_after_init;
#define dentry_cache runtime_const_ptr(__dentry_cache)

/*
 * 三个常量 qstr 是无所有权的静态名字：空名用于内部占位，“/”用于匿名根，
 * “..”用于父目录语义。调用者不得释放其 name；hash_len 由 QSTR_INIT 固化。
 */
const struct qstr empty_name = QSTR_INIT("", 0);
EXPORT_SYMBOL(empty_name);
const struct qstr slash_name = QSTR_INIT("/", 1);
EXPORT_SYMBOL(slash_name);
const struct qstr dotdot_name = QSTR_INIT("..", 2);
EXPORT_SYMBOL(dotdot_name);

/*
 * This is the single most critical data structure when it comes
 * to the dcache: the hashtable for lookups. Somebody should try
 * to make this good - I've just made it work.
 *
 * This hash-function tries to avoid losing too many bits of hash
 * information, yet avoid using a prime hash-size or similar.
 *
 * Marking the variables "used" ensures that the compiler doesn't
 * optimize them away completely on architectures with runtime
 * constant infrastructure, this allows debuggers to see their
 * values. But updating these values has no effect on those arches.
 */

/*
 * 全局 dentry_hashtable 是普通查找的主索引，桶内由 hlist_bl 桶锁更新、由 RCU
 * 遍历；d_hash_shift 把 qstr 的 32 位 hash 映射到启动时确定的桶数。变量保留
 * __used 是为了让调试器在 runtime-constant 架构上仍能观察原值；运行期访问
 * 经 runtime_const_* 生成已特化的常量指令，初始化后的普通写入不会改变该
 * 指令中的值。二者在 init 后只读。
 */

static unsigned int d_hash_shift __ro_after_init __used;

static struct hlist_bl_head *dentry_hashtable __ro_after_init __used;

/* d_hash() 把 @hashlen 的 hash 高位映射为正式哈希桶；返回借用桶指针，无副作用。 */
static inline struct hlist_bl_head *d_hash(unsigned long hashlen)
{
	return runtime_const_ptr(dentry_hashtable) +
		runtime_const_shift_right_32(hashlen, d_hash_shift);
}

/*
 * in_lookup_hashtable 只存“某线程正在替父目录解析该名字”的临时 dentry，目的
 * 是合并并行 cache miss，避免多个线程同时调用文件系统 ->lookup()。它与正式
 * dentry_hashtable 分离；键同时混入 parent 地址和 name hash，发布完成后必须
 * 从临时表摘除并唤醒等待者。
 */

#define IN_LOOKUP_SHIFT 10
static struct hlist_bl_head in_lookup_hashtable[1 << IN_LOOKUP_SHIFT];

/* in_lookup_hash() 混合借用的 @parent 地址与 @hash，返回临时查找表的借用桶。 */
static inline struct hlist_bl_head *in_lookup_hash(const struct dentry *parent,
					unsigned int hash)
{
	hash += (unsigned long) parent / L1_CACHE_BYTES;
	return in_lookup_hashtable + hash_32(hash, IN_LOOKUP_SHIFT);
}

/*
 * dentry_stat_t 是 /proc/sys/fs/dentry-state 的 ABI 快照。前三个字段分别为
 * 总 dentry、零引用/LRU dentry、历史 age_limit；want_pages 与 dummy 保留兼容
 * 布局。nr_negative 统计 LRU 上的负 dentry。实际热路径使用 per-CPU 计数，
 * proc 读取时才汇总，避免每次分配/释放争用全局 cache line。
 */
struct dentry_stat_t {
	long nr_dentry;
	long nr_unused;
	long age_limit;		/* age in seconds */
	/* age_limit 的单位是秒，是保留的历史回收年龄字段。 */
	long want_pages;	/* pages requested by system */
	/* want_pages 表示系统请求回收的页数，保留在用户可见统计布局中。 */
	long nr_negative;	/* # of unused negative dentries */
	/* nr_negative 是公共 LRU 上未使用负 dentry 的近似数量。 */
	long dummy;		/* Reserved for future use */
	/* dummy 只为 ABI 将来扩展预留，当前不参与决策。 */
};

/*
 * nr_dentry、nr_dentry_unused、nr_dentry_negative 分别跟踪总量、可回收量和
 * 可回收负项；更新只要求近似统计，不作为生命周期正确性的依据。
 * dentry_negative_policy 控制删除时是否保留负项，sysctl 写入限制为 0/1。
 */
static DEFINE_PER_CPU(long, nr_dentry);
static DEFINE_PER_CPU(long, nr_dentry_unused);
static DEFINE_PER_CPU(long, nr_dentry_negative);
static int dentry_negative_policy;

#if defined(CONFIG_SYSCTL) && defined(CONFIG_PROC_FS)
/* Statistics gathering. */
/* 统计收集仅在 sysctl 与 procfs 同时启用时编译，其他配置不产生该接口。 */
/*
 * dentry_stat 是读取 /proc/sys/fs/dentry-state 时刷新的全局 ABI 快照；热路径不
 * 直接争用它。age_limit 保留历史默认值 45 秒，其余动态字段由 proc_nr_dentry()
 * 从 per-CPU 计数汇总。表项只读暴露，因此无需把它当生命周期同步依据。
 */
static struct dentry_stat_t dentry_stat = {
	.age_limit = 45,
};

/*
 * Here we resort to our own counters instead of using generic per-cpu counters
 * for consistency with what the vfs inode code does. We are expected to harvest
 * better code and performance by having our own specialized counters.
 *
 * Please note that the loop is done over all possible CPUs, not over all online
 * CPUs. The reason for this is that we don't want to play games with CPUs going
 * on and off. If one of them goes off, we will just keep their counters.
 *
 * glommer: See cffbc8a for details, and if you ever intend to change this,
 * please update all vfs counters to match.
 */
/*
 * 这里不用通用 percpu_counter，而与 VFS inode 统计保持自有 per-CPU 方案，以便
 * 编译器生成更直接的热路径更新。汇总必须遍历 possible CPU 而非 online CPU：
 * CPU 下线后它的槽仍保留历史计数，若只扫在线 CPU 会让总数随热插拔跳变。
 * 修改该规则时需同步所有 VFS 计数，提交 cffbc8a 记录了这一约定的背景。
 */
/*
 * 三个 get_nr_*() 契约相同：在可睡眠的 proc/sysctl 读取路径遍历所有
 * possible CPU（包括已经离线者），汇总对应 per-CPU 槽；负和因并发近似或
 * CPU 生命周期残值被钳为 0。无输入、无引用转移，返回当前近似总数。
 */
/*
 * get_nr_dentry() 汇总所有 possible CPU 的 dentry 总数槽并把并发近似产生的负和
 * 钳为 0。入参无，返回 long 近似计数；只读 per-CPU 数据、不睡眠、不改变对象。
 */
static long get_nr_dentry(void)
{
	int i;
	long sum = 0;
	for_each_possible_cpu(i)
		sum += per_cpu(nr_dentry, i);
	return sum < 0 ? 0 : sum;
}

/* get_nr_dentry_unused() 汇总所有 possible CPU 的未使用 dentry 近似计数并钳为非负。 */
static long get_nr_dentry_unused(void)
{
	int i;
	long sum = 0;
	for_each_possible_cpu(i)
		sum += per_cpu(nr_dentry_unused, i);
	return sum < 0 ? 0 : sum;
}

/* get_nr_dentry_negative() 汇总所有 possible CPU 的 LRU 负项近似计数并钳制为非负。 */
static long get_nr_dentry_negative(void)
{
	int i;
	long sum = 0;

	for_each_possible_cpu(i)
		sum += per_cpu(nr_dentry_negative, i);
	return sum < 0 ? 0 : sum;
}

/*
 * proc_nr_dentry() 刷新统计输出对象后调用通用只读 sysctl handler；参数遵守
 * ctl_table handler 契约，返回 0 或负 errno，并推进 @lenp/@ppos。
 */
/*
 * proc_nr_dentry() 在通用 sysctl handler 复制数据前刷新统计快照。table、buffer、
 * lenp、ppos 原样传给 proc_doulongvec_minmax()；该表项是只读的，但函数仍遵守
 * handler 通用签名。返回 0 或通用 handler 的负 errno，并通过 buffer/lenp/ppos
 * 产生用户可见副作用。
 */
static int proc_nr_dentry(const struct ctl_table *table, int write, void *buffer,
			  size_t *lenp, loff_t *ppos)
{
	dentry_stat.nr_dentry = get_nr_dentry();
	dentry_stat.nr_unused = get_nr_dentry_unused();
	dentry_stat.nr_negative = get_nr_dentry_negative();
	return proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
}

/*
 * fs_dcache_sysctls 描述 fs/dentry-state 只读统计和 fs/dentry-negative 0/1 策略。
 * 表与名字具有静态存储期，注册后由 sysctl 核心借用；handler 分别刷新快照或
 * 执行带上下界的整数读写。
 */
static const struct ctl_table fs_dcache_sysctls[] = {
	{
		.procname	= "dentry-state",
		.data		= &dentry_stat,
		.maxlen		= 6*sizeof(long),
		.mode		= 0444,
		.proc_handler	= proc_nr_dentry,
	},
	{
		.procname	= "dentry-negative",
		.data		= &dentry_negative_policy,
		.maxlen		= sizeof(dentry_negative_policy),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
};

/*
 * vm_dcache_sysctls 暴露回收压力的分子与分母；分子允许为 0，分母下限为 100，
 * 避免 vfs_pressure_ratio() 除零。注册后表由 sysctl 核心只读借用到系统生命周期。
 */
static const struct ctl_table vm_dcache_sysctls[] = {
	{
		.procname	= "vfs_cache_pressure",
		.data		= &sysctl_vfs_cache_pressure,
		.maxlen		= sizeof(sysctl_vfs_cache_pressure),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	},
	{
		.procname	= "vfs_cache_pressure_denom",
		.data		= &sysctl_vfs_cache_pressure_denom,
		.maxlen		= sizeof(sysctl_vfs_cache_pressure_denom),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ONE_HUNDRED,
	},
};

/* init_fs_dcache_sysctls() 在 fs_initcall 注册永久 vm/fs 表；无入参，恒返回 0。 */
/*
 * init_fs_dcache_sysctls() 在 fs_initcall 阶段注册 vm 与 fs 两组永久 sysctl 表；
 * 入参无、返回 0，注册失败由 register_sysctl_init() 的初始化期策略处理。
 */
static int __init init_fs_dcache_sysctls(void)
{
	register_sysctl_init("vm", vm_dcache_sysctls);
	register_sysctl_init("fs", fs_dcache_sysctls);
	return 0;
}
fs_initcall(init_fs_dcache_sysctls);
#endif

/*
 * Compare 2 name strings, return 0 if they match, otherwise non-zero.
 * The strings are both count bytes long, and count is non-zero.
 */
/*
 * 名字比较契约：两个缓冲区都至少有 tcount 个可读字节，且 tcount 非零；完全
 * 相等返回 0，否则返回非零。CONFIG_DCACHE_WORD_ACCESS 版本按机器字比较以减少
 * 分支，通用版本逐字节比较；二者都不读取名字的所有权，也不睡眠。
 */
#ifdef CONFIG_DCACHE_WORD_ACCESS

#include <asm/word-at-a-time.h>
/*
 * NOTE! 'cs' and 'scount' come from a dentry, so it has a
 * aligned allocation for this particular component. We don't
 * strictly need the load_unaligned_zeropad() safety, but it
 * doesn't hurt either.
 *
 * In contrast, 'ct' and 'tcount' can be from a pathname, and do
 * need the careful unaligned handling.
 */
/*
 * cs 来自 dentry 专门对齐的内联/外置名字，可直接按字读取；ct 可能指向用户
 * 路径解析产生的任意对齐地址，必须用 load_unaligned_zeropad()。末字即使越过
 * 逻辑长度，辅助函数也以零安全填充，再由 mask 只比较有效字节。
 */
/*
 * dentry_string_cmp() 的 word-at-a-time 实现比较两个借用缓冲区的 @tcount 字节，
 * @tcount 必须非零。完整机器字不等立即返回 1；尾字用掩码忽略长度外零填充位。
 * 全部相等返回 0。函数不取得引用、不写缓冲且不睡眠。
 */
static inline int dentry_string_cmp(const unsigned char *cs, const unsigned char *ct, unsigned tcount)
{
	unsigned long a,b,mask;

	for (;;) {
		a = read_word_at_a_time(cs);
		b = load_unaligned_zeropad(ct);
		if (tcount < sizeof(unsigned long))
			break;
		if (unlikely(a != b))
			return 1;
		cs += sizeof(unsigned long);
		ct += sizeof(unsigned long);
		tcount -= sizeof(unsigned long);
		if (!tcount)
			return 0;
	}
	mask = bytemask_from_count(tcount);
	return unlikely(!!((a ^ b) & mask));
}

#else

/* 通用 dentry_string_cmp() 对两个借用缓冲逐字节比较 @tcount 个字节。 */
static inline int dentry_string_cmp(const unsigned char *cs, const unsigned char *ct, unsigned tcount)
{
	do {
		if (*cs != *ct)
			return 1;
		cs++;
		ct++;
		tcount--;
	} while (tcount);
	return 0;
}

#endif

/* dentry_cmp() 默认比较借用 dentry 名字与候选缓冲；返回 0 相等，序列验证由上层负责。 */
/*
 * dentry_cmp() 是默认的 dentry 名字比较。@dentry 为借用对象，调用者负责其
 * RCU/引用生命周期；@ct 指向至少 @tcount 字节的候选名字。rename 可能让长度
 * 与指针来自不同瞬间，但 name 指针用 READ_ONCE 原子取得且存储以 NUL 结尾，
 * 因而不会越界；上层最终用 d_seq 拒绝不一致快照。返回 0 表示相等。
 */
static inline int dentry_cmp(const struct dentry *dentry, const unsigned char *ct, unsigned tcount)
{
	/*
	 * Be careful about RCU walk racing with rename:
	 * use 'READ_ONCE' to fetch the name pointer.
	 *
	 * NOTE! Even if a rename will mean that the length
	 * was not loaded atomically, we don't care. The
	 * RCU walk will check the sequence count eventually,
	 * and catch it. And we won't overrun the buffer,
	 * because we're reading the name pointer atomically,
	 * and a dentry name is guaranteed to be properly
	 * terminated with a NUL byte.
	 *
	 * End result: even if 'len' is wrong, we'll exit
	 * early because the data cannot match (there can
	 * be no NUL in the ct/tcount data)
	 */
	/*
	 * RCU-walk 可能与 rename 并发，必须用 READ_ONCE 取得单一 name 指针。长度即使
	 * 来自另一个版本也无妨：最终 d_seq 会发现竞态；名字存储总有 NUL 终止，
	 * 错误长度只会使比较提前失败，不会越过缓冲，候选路径本身不含 NUL。
	 */
	const unsigned char *cs = READ_ONCE(dentry->d_name.name);

	return dentry_string_cmp(cs, ct, tcount);
}

/*
 * long names are allocated separately from dentry and never modified.
 * Refcounted, freeing is RCU-delayed.  See take_dentry_name_snapshot()
 * for the reason why ->count and ->head can't be combined into a union.
 * dentry_string_cmp() relies upon ->name[] being word-aligned.
 */
/*
 * 长名字独立分配且发布后不可修改；count 保护 dentry 与名字快照的共享所有权，
 * 最后一份引用在 RCU 宽限期后释放。count 不能与 rcu_head 共用 union，因为
 * dentry 进入 RCU 回调时外部快照仍可能使用 count。name[] 按机器字对齐，满足
 * dentry_string_cmp() 的按字读取前提。
 */
struct external_name {
	atomic_t count;
	struct rcu_head head;
	unsigned char name[] __aligned(sizeof(unsigned long));
};

/*
 * external_name 承载超过 d_shortname 容量的不可变名字。count 同时覆盖 dentry
 * 自身和 name_snapshot 持有者；最后一份引用通过 RCU 延迟释放。head 不能与
 * count 复用，因为快照可能在 dentry 已进入 RCU 回收后仍需独立减少引用。
 */

/* external_name() 从嵌入的 name[] 地址恢复容器；只返回借用指针，不增引用。 */
static inline struct external_name *external_name(struct dentry *dentry)
{
	return container_of(dentry->d_name.name, struct external_name, name[0]);
}

/* __d_free() 是普通名字 dentry 的 RCU 析构回调；宽限期后归还 slab，无返回。 */
static void __d_free(struct rcu_head *head)
{
	struct dentry *dentry = container_of(head, struct dentry, d_rcu);

	kmem_cache_free(dentry_cache, dentry); 
}

/* __d_free_external() 先释放外置名字再释放 dentry；调用时二者均已不可达。 */
static void __d_free_external(struct rcu_head *head)
{
	struct dentry *dentry = container_of(head, struct dentry, d_rcu);
	kfree(external_name(dentry));
	kmem_cache_free(dentry_cache, dentry);
}

/* dname_external() 仅比较存储地址，真表示名字由 external_name 独立分配。 */
static inline int dname_external(const struct dentry *dentry)
{
	return dentry->d_name.name != dentry->d_shortname.string;
}

/*
 * take_dentry_name_snapshot() 为日志、审计等需要跨越 rename 的调用者取得稳定
 * 名字。@name 是调用者提供的输出对象；@dentry 是借用输入且须仍存活。短名
 * 直接复制到快照，长名用 inc-not-zero 获取外置存储引用；若 d_seq 表明期间
 * rename，则撤销本次快照并重试。成功后调用者拥有快照资源，必须配对调用
 * release_dentry_name_snapshot()。函数可能自旋重试但不睡眠。
 */
void take_dentry_name_snapshot(struct name_snapshot *name, struct dentry *dentry)
{
	unsigned seq;
	const unsigned char *s;

	rcu_read_lock();
retry:
	seq = read_seqcount_begin(&dentry->d_seq);
	s = READ_ONCE(dentry->d_name.name);
	name->name.hash_len = dentry->d_name.hash_len;
	name->name.name = name->inline_name.string;
	if (likely(s == dentry->d_shortname.string)) {
		name->inline_name = dentry->d_shortname;
	} else {
		struct external_name *p;
		p = container_of(s, struct external_name, name[0]);
		// get a valid reference
		/* 只在 count 非零时增引用，避免把已进入回收的外置名字重新复活。 */
		if (unlikely(!atomic_inc_not_zero(&p->count)))
			goto retry;
		name->name.name = s;
	}
	if (read_seqcount_retry(&dentry->d_seq, seq)) {
		release_dentry_name_snapshot(name);
		goto retry;
	}
	rcu_read_unlock();
}

EXPORT_SYMBOL(take_dentry_name_snapshot);

/* release_dentry_name_snapshot() 消耗 @name 持有的外置名字引用；短名无需释放。 */
/*
 * release_dentry_name_snapshot() 释放上述所有权：短名位于快照内部无需处理；
 * 长名减少 external_name 引用，最后一份通过 kfree_rcu 延迟释放。无返回，
 * 调用后 @name 中的 name 指针不可继续使用。
 */
void release_dentry_name_snapshot(struct name_snapshot *name)
{
	if (unlikely(name->name.name != name->inline_name.string)) {
		struct external_name *p;
		p = container_of(name->name.name, struct external_name, name[0]);
		if (unlikely(atomic_dec_and_test(&p->count)))
			kfree_rcu(p, head);
	}
}

EXPORT_SYMBOL(release_dentry_name_snapshot);

/*
 * __d_set_inode_and_type() 在调用者持锁时把 @inode 引用绑定给 @dentry，并以
 * release store 发布 @type_flags；无返回，输入 inode ownership 转入 dcache。
 */
/*
 * __d_set_inode_and_type() 在调用者已按协议持锁时把负 dentry 绑定为正 dentry。
 * 先写 d_inode，再以 release store 发布类型位，使无锁读者一旦观察到新类型就
 * 必然也能观察到 inode 初始化；@inode 引用的所有权已由调用者转交给 dcache。
 */
static inline void __d_set_inode_and_type(struct dentry *dentry,
					  struct inode *inode,
					  unsigned type_flags)
{
	unsigned flags;

	dentry->d_inode = inode;
	flags = READ_ONCE(dentry->d_flags);
	flags &= ~DCACHE_ENTRY_TYPE;
	flags |= type_flags;
	smp_store_release(&dentry->d_flags, flags);
}

/*
 * __d_clear_type_and_inode() 执行相反状态转换：先清公开类型，再令 d_inode 为
 * NULL。调用者持 d_lock 和 inode->i_lock；若该 dentry 正在真正 LRU 而非私有
 * shrink list 上，负项计数随转换增加。函数不释放 inode 引用，外层在解锁后
 * 调用 d_iput/iput，避免在自旋锁内进入可能复杂的文件系统回调。
 */
static inline void __d_clear_type_and_inode(struct dentry *dentry)
{
	unsigned flags = READ_ONCE(dentry->d_flags);

	flags &= ~DCACHE_ENTRY_TYPE;
	WRITE_ONCE(dentry->d_flags, flags);
	dentry->d_inode = NULL;
	/*
	 * The negative counter only tracks dentries on the LRU. Don't inc if
	 * d_lru is on another list.
	 */
	/*
	 * 负项统计只描述公共 LRU；若 d_lru 已被私有 shrink list 占用，就不能增加，
	 * 否则同一对象会在公共统计中凭空出现。
	 */
	if ((flags & (DCACHE_LRU_LIST|DCACHE_SHRINK_LIST)) == DCACHE_LRU_LIST)
		this_cpu_inc(nr_dentry_negative);
}

/*
 * DENTRY_WARN_ONCE() 在内部状态不变量失败时只告警一次，并打印对象地址与 flags，
 * 不改变 @dentry。D_FLAG_VERIFY() 专门核对 LRU_LIST/SHRINK_LIST 两位是否等于预期
 * @x；调用者须已持 d_lock，宏参数可能被求值多次，不能传入有副作用的表达式。
 */
#define DENTRY_WARN_ONCE(condition, dentry) \
	WARN_ONCE((condition), "dentry=%p d_flags=0x%x\n", (dentry), (dentry)->d_flags)
#define D_FLAG_VERIFY(dentry, x) \
	DENTRY_WARN_ONCE(((dentry)->d_flags & (DCACHE_LRU_LIST | DCACHE_SHRINK_LIST)) != (x), (dentry))

/*
 * dentry_free() 消耗已标死、负且脱离列表的 @dentry；NORCU 同步释放，普通对象
 * 排入 RCU 回调。返回后调用者不得访问该对象。
 */
/*
 * dentry_free() 是对象存储期的最后一道门：入口要求 dentry 已为负、lockref
 * 已标死且不在任何 LRU/shrink 链。外置名字先放引用；普通可被 RCU 查找过的
 * dentry 用 call_rcu 延迟 slab 释放，DCACHE_NORCU 对象从未发布给 RCU，允许
 * 立即释放。无直接返回，调用后不得访问 @dentry。
 */
static void dentry_free(struct dentry *dentry)
{
	DENTRY_WARN_ONCE(d_really_is_positive(dentry), dentry);
	DENTRY_WARN_ONCE(dentry->d_lockref.count >= 0, dentry);
	D_FLAG_VERIFY(dentry, 0);
	if (unlikely(dname_external(dentry))) {
		struct external_name *p = external_name(dentry);
		if (likely(atomic_dec_and_test(&p->count))) {
			call_rcu(&dentry->d_rcu, __d_free_external);
			return;
		}
	}
	/* if dentry was never visible to RCU, immediate free is OK */
	/* DCACHE_NORCU 保证从未被 RCU 查找者看见，因此无需等待宽限期即可释放。 */
	if (dentry->d_flags & DCACHE_NORCU)
		__d_free(&dentry->d_rcu);
	else
		call_rcu(&dentry->d_rcu, __d_free);
}

/*
 * Release the dentry's inode, using the filesystem
 * d_iput() operation if defined.
 */
/*
 * dentry_unlink_inode() 在 inode->i_lock 与 dentry->d_lock 均已持有时，将正
 * dentry 从 inode alias 链摘除并原子地变成负项；它负责释放这两把锁。随后
 * 通知删除事件并把 inode 引用交给文件系统 d_iput 或通用 iput。返回无；
 * dentry 本身仍存活，之后可作为负缓存或继续进入销毁路径。
 */
static void dentry_unlink_inode(struct dentry * dentry)
	__releases(dentry->d_lock)
	__releases(dentry->d_inode->i_lock)
{
	struct inode *inode = dentry->d_inode;

	raw_write_seqcount_begin(&dentry->d_seq);
	__d_clear_type_and_inode(dentry);
	__hlist_del(&dentry->d_alias);
	/*
	 * dentry becomes negative, so the space occupied by ->d_alias
	 * belongs to ->waiters now.
	 */
	/*
	 * dentry 变负后不再使用 d_alias 节点；同一 union 存储随状态转换改作 waiters
	 * 头指针，先置 NULL 防止把旧链表字节误认为等待者。
	 */
	dentry->waiters = NULL;
	raw_write_seqcount_end(&dentry->d_seq);
	spin_unlock(&dentry->d_lock);
	spin_unlock(&inode->i_lock);
	if (!inode->i_nlink)
		fsnotify_inoderemove(inode);
	if (dentry->d_op && dentry->d_op->d_iput)
		dentry->d_op->d_iput(dentry, inode);
	else
		iput(inode);
}

/*
 * The DCACHE_LRU_LIST bit is set whenever the 'd_lru' entry
 * is in use - which includes both the "real" per-superblock
 * LRU list _and_ the DCACHE_SHRINK_LIST use.
 *
 * The DCACHE_SHRINK_LIST bit is set whenever the dentry is
 * on the shrink list (ie not on the superblock LRU list).
 *
 * The per-cpu "nr_dentry_unused" counters are updated with
 * the DCACHE_LRU_LIST bit.
 *
 * The per-cpu "nr_dentry_negative" counters are only updated
 * when deleted from or added to the per-superblock LRU list, not
 * from/to the shrink list. That is to avoid an unneeded dec/inc
 * pair when moving from LRU to shrink list in select_collect().
 *
 * These helper functions make sure we always follow the
 * rules. d_lock must be held by the caller.
 */
/*
 * LRU 位语义：DCACHE_LRU_LIST 表示 d_lru 节点正被使用，既包括 superblock
 * 公共 LRU，也包括回收者私有 shrink list；DCACHE_SHRINK_LIST 进一步区分后者。
 * unused 计数随 d_lru 是否被使用而变，negative 只统计公共 LRU，因而从公共
 * LRU 搬到私有链时只减一次 negative，避免无意义的减后再加。以下 helper
 * 均要求调用者持 d_lock，并用 D_FLAG_VERIFY 检查状态机是否被破坏。
 */
/*
 * d_lru_add() 在调用者持有 @dentry->d_lock 且对象尚未占用 d_lru 时，将零引用
 * dentry 加入所属 superblock 的公共 LRU。它设置 LRU_LIST，递增 unused；负项再
 * 递增 negative。list_lru_add_obj() 若与状态机不符触发 WARN。无返回、不睡眠，
 * 对象仍可被并发查找重新取得引用，并未转入死亡状态。
 */
static void d_lru_add(struct dentry *dentry)
{
	D_FLAG_VERIFY(dentry, 0);
	dentry->d_flags |= DCACHE_LRU_LIST;
	this_cpu_inc(nr_dentry_unused);
	if (d_is_negative(dentry))
		this_cpu_inc(nr_dentry_negative);
	WARN_ON_ONCE(!list_lru_add_obj(
			&dentry->d_sb->s_dentry_lru, &dentry->d_lru));
}

/* d_lru_del() 把零引用 dentry 从公共 LRU 摘除并同步两个 per-CPU 统计。 */
static void d_lru_del(struct dentry *dentry)
{
	D_FLAG_VERIFY(dentry, DCACHE_LRU_LIST);
	dentry->d_flags &= ~DCACHE_LRU_LIST;
	this_cpu_dec(nr_dentry_unused);
	if (d_is_negative(dentry))
		this_cpu_dec(nr_dentry_negative);
	WARN_ON_ONCE(!list_lru_del_obj(
			&dentry->d_sb->s_dentry_lru, &dentry->d_lru));
}

/* d_shrink_del() 从回收者私有链摘除；之后该 dentry 可被杀死或单独释放。 */
static void d_shrink_del(struct dentry *dentry)
{
	D_FLAG_VERIFY(dentry, DCACHE_SHRINK_LIST | DCACHE_LRU_LIST);
	list_del_init(&dentry->d_lru);
	dentry->d_flags &= ~(DCACHE_SHRINK_LIST | DCACHE_LRU_LIST);
	this_cpu_dec(nr_dentry_unused);
}

/* d_shrink_add() 把持 d_lock 的零引用候选交给线程私有回收链，不会立即释放。 */
static void d_shrink_add(struct dentry *dentry, struct list_head *list)
{
	D_FLAG_VERIFY(dentry, 0);
	list_add(&dentry->d_lru, list);
	dentry->d_flags |= DCACHE_SHRINK_LIST | DCACHE_LRU_LIST;
	this_cpu_inc(nr_dentry_unused);
}

/*
 * These can only be called under the global LRU lock, ie during the
 * callback for freeing the LRU list. "isolate" removes it from the
 * LRU lists entirely, while shrink_move moves it to the indicated
 * private list.
 */
/*
 * 两个 isolate helper 运行在 list_lru 的全局/节点锁内。d_lru_isolate() 完全
 * 脱离 LRU；d_lru_shrink_move() 保留“d_lru 正被使用”状态但将节点移交给
 * 私有链。它们不能睡眠，调用者还须取得 d_lock 才能稳定 flags 与引用计数。
 */
static void d_lru_isolate(struct list_lru_one *lru, struct dentry *dentry)
{
	D_FLAG_VERIFY(dentry, DCACHE_LRU_LIST);
	dentry->d_flags &= ~DCACHE_LRU_LIST;
	this_cpu_dec(nr_dentry_unused);
	if (d_is_negative(dentry))
		this_cpu_dec(nr_dentry_negative);
	list_lru_isolate(lru, &dentry->d_lru);
}

/* d_lru_shrink_move() 在 LRU 锁与 d_lock 下把 @dentry 移到调用者私有 @list。 */
static void d_lru_shrink_move(struct list_lru_one *lru, struct dentry *dentry,
			      struct list_head *list)
{
	D_FLAG_VERIFY(dentry, DCACHE_LRU_LIST);
	dentry->d_flags |= DCACHE_SHRINK_LIST;
	if (d_is_negative(dentry))
		this_cpu_dec(nr_dentry_negative);
	list_lru_isolate_move(lru, &dentry->d_lru, list);
}

/* ___d_drop() 只从正式 hash 桶摘除 @dentry，不把它标成逻辑 unhashed。 */
/*
 * ___d_drop() 仅在桶锁下把节点从正式查找哈希链摘除，不把 pprev 设为 NULL；
 * __d_move() 借此暂时搬移节点。__d_drop() 还在 d_lock 下把对象标成 unhashed
 * 并使 d_seq 失效，适合真正取消可查找性。二者都不减少引用或释放对象。
 */
static void ___d_drop(struct dentry *dentry)
{
	struct hlist_bl_head *b = d_hash(dentry->d_name.hash);

	hlist_bl_lock(b);
	__hlist_bl_del(&dentry->d_hash);
	hlist_bl_unlock(b);
}

/*
 * __d_drop() 要求调用者持 @dentry->d_lock；若对象仍在正式哈希，就调用
 * ___d_drop() 摘链、把 pprev 清为 NULL，并使 d_seq 失效。无返回，不释放引用。
 */
void __d_drop(struct dentry *dentry)
{
	if (!d_unhashed(dentry)) {
		___d_drop(dentry);
		dentry->d_hash.pprev = NULL;
		write_seqcount_invalidate(&dentry->d_seq);
	}
}
EXPORT_SYMBOL(__d_drop);

/**
 * d_drop - drop a dentry
 * @dentry: dentry to drop
 *
 * d_drop() unhashes the entry from the parent dentry hashes, so that it won't
 * be found through a VFS lookup any more. Note that this is different from
 * deleting the dentry - d_delete will try to mark the dentry negative if
 * possible, giving a successful _negative_ lookup, while d_drop will
 * just make the cache lookup fail.
 *
 * d_drop() is used mainly for stuff that wants to invalidate a dentry for some
 * reason (NFS timeouts or autofs deletes).
 *
 * __d_drop requires dentry->d_lock
 *
 * ___d_drop doesn't mark dentry as "unhashed"
 * (dentry->d_hash.pprev will be LIST_POISON2, not NULL).
 */
/*
 * d_drop() 是对 __d_drop() 的加锁包装。@dentry 是借用且必须存活；函数可在
 * 原子上下文执行、不睡眠。返回无，副作用只是今后的 dcache lookup 不再命中，
 * 已持引用者仍可使用对象。它不同于 d_delete()：不会构造可命中的负缓存。
 */
void d_drop(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	__d_drop(dentry);
	spin_unlock(&dentry->d_lock);
}
EXPORT_SYMBOL(d_drop);

/*
 * completion_list 是 shrink_dcache_tree() 的栈上等待节点。等待线程拥有节点存储，
 * next 把多个等待者串入垂死 dentry->waiters，completion 在 dentry_unlist() 发布
 * “已脱离树”时完成；等待结束前节点不能离开调用栈或被其他用途复用。
 */
struct completion_list {
	struct completion_list *next;
	struct completion completion;
};

/*
 * completion_list 是栈上等待节点：shrink_dcache_tree() 把它单链到垂死 dentry
 * 的 waiters 字段，dentry_unlist() 在对象离开父子树时逐个 complete。节点由
 * 等待线程拥有，完成前 dentry 只借用其地址，因此不得提前离开调用栈。
 */

/*
 *  shrink_dcache_tree() needs to be notified when dentry in process of
 *  being evicted finally gets unlisted.  Such dentries are
 *	already with negative ->d_count
 *	already negative
 *	already not in in-lookup hash
 *	reachable only via ->d_sib.
 *
 *  Use ->waiters for a single-linked list of struct completion_list of
 *  waiters.
 */
/*
 * d_add_waiter() 要求 d_lock，若对象尚未标记 KILLED 就登记等待者并返回 true；
 * 已不可见则返回 false，调用者不必等待。d_complete_waiters() 同样在 d_lock
 * 下取走整链并完成所有栈节点，保证每个 tree shrinker 都能继续。
 */
static inline bool d_add_waiter(struct dentry *dentry, struct completion_list *p)
{
	if (unlikely(dentry->d_flags & DCACHE_DENTRY_KILLED))
		return false;
	init_completion(&p->completion);
	p->next = dentry->waiters;
	dentry->waiters = p;
	return true;
}

/* d_complete_waiters() 在 d_lock 下取走并完成全部栈上等待节点；无等待者为空操作。 */
static inline void d_complete_waiters(struct dentry *dentry)
{
	struct completion_list *v = dentry->waiters;
	if (unlikely(v)) {
		/* some shrink_dcache_tree() instances are waiting */
		/* 有一个或多个 shrink_dcache_tree() 正等待该对象从父子树彻底摘除。 */
		dentry->waiters = NULL;
		while (v) {
			struct completion *r = &v->completion;
			v = v->next;
			complete(r);
		}
	}
}

/* unlink_secondary_root() 在 s_roots_lock 下摘除借用的次级根，不改变显式引用。 */
/*
 * unlink_secondary_root() 从 superblock 的 s_roots 摘除匿名/次级根；调用者已
 * 持 d_lock，本函数短暂取得 s_roots_lock。它只改变根索引，不释放引用。
 */
static void unlink_secondary_root(struct dentry *dentry)
{
	spin_lock(&dentry->d_sb->s_roots_lock);
	hlist_del_init(&dentry->d_sib);
	spin_unlock(&dentry->d_sb->s_roots_lock);
}

/*
 * dentry_unlist() 是“从树上消失”的提交点，调用者持 d_lock，普通子项还持
 * parent->d_lock。它先置 KILLED 并唤醒等待者，再从 d_sib/s_roots 摘除。摘除
 * 后特意跳过可移动 cursor，保证并发 d_walk() 留在旧 d_sib.next 中的恢复点
 * 指向稳定普通节点，不会因目录游标 lseek 而漏扫后续兄弟。
 */
static inline void dentry_unlist(struct dentry *dentry)
{
	struct dentry *next;
	/*
	 * Inform d_walk() and shrink_dentry_list() that we are no longer
	 * attached to the dentry tree
	 */
	/*
	 * 先置 KILLED，向 d_walk() 与 shrink_dentry_list() 发布“已脱离命名树”的事实；
	 * 后者据此只做最终内存释放，不会再次执行 inode/hash 摘除。
	 */
	dentry->d_flags |= DCACHE_DENTRY_KILLED;
	d_complete_waiters(dentry);
	if (unlikely(hlist_unhashed(&dentry->d_sib)))
		return;
	if (unlikely(IS_ROOT(dentry))) {
		unlink_secondary_root(dentry); // secondary root goes away
		/* 次级根不在普通 parent children 链上，需从 superblock 根索引单独摘除。 */
		return;
	}
	__hlist_del(&dentry->d_sib);
	/*
	 * Cursors can move around the list of children.  While we'd been
	 * a normal list member, it didn't matter - ->d_sib.next would've
	 * been updated.  However, from now on it won't be and for the
	 * things like d_walk() it might end up with a nasty surprise.
	 * Normally d_walk() doesn't care about cursors moving around -
	 * ->d_lock on parent prevents that and since a cursor has no children
	 * of its own, we get through it without ever unlocking the parent.
	 * There is one exception, though - if we ascend from a child that
	 * gets killed as soon as we unlock it, the next sibling is found
	 * using the value left in its ->d_sib.next.  And if _that_
	 * pointed to a cursor, and cursor got moved (e.g. by lseek())
	 * before d_walk() regains parent->d_lock, we'll end up skipping
	 * everything the cursor had been moved past.
	 *
	 * Solution: make sure that the pointer left behind in ->d_sib.next
	 * points to something that won't be moving around.  I.e. skip the
	 * cursors.
	 */
	/*
	 * 目录 cursor 会在 children 链上移动。普通节点尚在链中时，删除会维护前驱的
	 * next；本节点摘除后，d_walk() 可能仍把这里残留的 next 当恢复点。如果它指向
	 * 随 lseek 移动的 cursor，恢复期间 cursor 越过的普通兄弟将被漏扫。因此沿
	 * 残留 next 跳过全部 cursor，使恢复点固定在不会自行移动的普通 dentry。
	 */
	while (dentry->d_sib.next) {
		next = hlist_entry(dentry->d_sib.next, struct dentry, d_sib);
		if (likely(!(next->d_flags & DCACHE_DENTRY_CURSOR)))
			break;
		dentry->d_sib.next = next->d_sib.next;
	}
}

/*
 * Prepare locking environment for killing a dentry.
 * Called under dentry->d_lock.  To proceed with eviction of a positive dentry
 * we need to get ->i_lock of the inode of that dentry as well.
 * However, ->i_lock nests outside of ->d_lock, so if trylock fails we might
 * have to drop and regain the latter.  Dentry state can change while its
 * ->d_lock is not held - it might end up getting killed, becoming busy,
 * negative, etc., so we need to be careful.
 *
 * For NORCU dentries memory safety relies upon having only one call of
 * lock_for_kill() in the entire lifetime of dentry and dentry_free() being
 * called only by the caller of lock_for_kill().  That this is NORCU-specific;
 * the crucial part is that refcounts of NORCU dentries never grow once having
 * dropped to zero.
 *
 * For normal dentries we can not assume that there won't be concurrent calls
 * of dentry_free() - dentry might end up being evicted by another thread
 * while we are dropping/retaking locks on the slow path.  Memory safety is
 * provided by keeping the RCU read-side critical area contiguous with
 * an explicit rcu_read_lock() scope bridging over the break in spinlock scopes.
 *
 * If dentry is busy (or busy dying, or already dead), unlock dentry
 * and return false.  Otherwise, return true and have that dentry's
 * inode (if any) locked in addition to dentry itself.
 */
/*
 * lock_for_kill() - 为零引用 dentry 组装最终淘汰锁环境。
 *
 * @dentry 由调用者借用且入口持 d_lock；成功返回 true 时仍持 d_lock，正项还
 * 持 inode->i_lock。若引用复活、对象已死或竞争失败，函数释放相关锁并返回
 * false。快速路径 trylock 遵守 i_lock -> d_lock 的既定顺序；失败时必须放掉
 * d_lock 再按序重取。普通 dentry 用连续 RCU 临界区桥接无锁窗口以防另一路
 * dentry_free() 释放内存；NORCU 的“引用到零后永不增长”不变量提供同等保证。
 * 函数只自旋不睡眠，并会在换锁后重新核对 d_inode 是否仍是同一个对象。
 */
static bool lock_for_kill(struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;

	if (unlikely(dentry->d_lockref.count)) {
		spin_unlock(&dentry->d_lock);
		return false;
	}

	if (!inode || likely(spin_trylock(&inode->i_lock)))
		return true;

	// Too bad - we need to drop ->d_lock and take locks in correct order.
	// To avoid breaking RCU read-side critical area when we drop ->d_lock,
	// take an explicit rcu_read_lock() while we are switching locks.
	/*
	 * trylock 失败后只能释放内层 d_lock，按 i_lock -> d_lock 的正规顺序重取；
	 * 显式 RCU 临界区跨过两段自旋锁作用域，防止换锁窗口中普通 dentry 被释放。
	 */
	rcu_read_lock();
	do {
		spin_unlock(&dentry->d_lock);
		spin_lock(&inode->i_lock);
		spin_lock(&dentry->d_lock);
		// make sure we'd locked the right inode - ->d_inode might've
		// changed while we were not holding ->d_lock
		/* 放锁窗口可能使 d_inode 改变，必须在两锁齐全后确认锁住的是当前 inode。 */
		if (likely(inode == dentry->d_inode))
			break;
		spin_unlock(&inode->i_lock);
		inode = dentry->d_inode;
	} while (inode);
	rcu_read_unlock();
	if (likely(!dentry->d_lockref.count))
		return true;
	if (inode)
		spin_unlock(&inode->i_lock);
	spin_unlock(&dentry->d_lock);
	return false;
}

/**
 * dentry_kill - evict a dentry
 * @dentry:	dentry to be evicted
 *
 * All dentry evictions are done by this function.  The reference we are
 * passed does not contribute to the refcount; the caller had either
 * already decremented the refcount or it had never held one in the
 * first place.  @dentry->d_lock is held by the caller and dropped
 * by dentry_kill(@dentry).
 *
 * We are guaranteed that nobody had called dentry_free(@dentry)
 * prior to the beginning of RCU read-side critical area we are in.
 *
 * Caller must not access @dentry after the call.
 *
 * If eviction of @dentry drops the last reference to its parent,
 * the reference to parent is returned to caller.  In that case
 * it is guaranteed to satisfy the requirements for dentry_kill()
 * argument - its ->d_lock is held and we are guaranteed that nobody
 * had passed it to dentry_free() prior to acquisition of its ->d_lock.
 * Otherwise %NULL is returned.
 *
 * If @dentry is idle and remains such after we assemble the full
 * locking environment for eviction (see lock_for_kill() for details)
 * we mark it doomed (->d_lockref.count < 0) and proceed to detaching
 * it from any filesystem objects.  Otherwise we drop ->d_lock and
 * return %NULL.
 *
 * Once @dentry is detached from the filesystem objects, we complete
 * detaching it from dentry tree. The parent, if any, gets locked
 * and its refcount is decremented; dentry is carefully removed from
 * the tree (see dentry_unlist() for details) and marked killed
 * (%DCACHE_DENTRY_KILLED set in ->d_flags).  At that point it's just
 * an inert chunk of memory, accessible only via RCU references
 * and possibly via a shrink list.  If it is not on any shrink lists,
 * we call dentry_free(), which schedules actual freeing of memory.
 * Othewise freeing is left to the owner of the shrink list in question.
 */
/*
 * dentry_kill() - 完成 dentry 从所有 VFS 索引到不可达内存的状态转换。
 *
 * 入口引用不计入 lockref，且持 d_lock；返回后调用者绝不能再访问原 dentry。
 * 函数依次标死、通知 ->d_prune、移出 LRU/哈希、解除 inode alias、调用
 * ->d_release、从父子树摘除，最后直接或延迟释放。若删除该子项使 parent 的
 * 引用恰好归零，则返回仍持 d_lock 的 parent，让 finish_dput() 以循环代替递归
 * 向上回收；否则返回 NULL。文件系统回调可能要求可调度上下文，所以本路径
 * 允许 cond_resched()，调用者须能睡眠。
 */
static struct dentry *dentry_kill(struct dentry *dentry)
{
	struct dentry *parent = NULL;
	bool can_free = true;

	if (unlikely(!lock_for_kill(dentry)))
		return NULL;

	/*
	 * The dentry is now unrecoverably dead to the world.
	 */
	/* lockref 标死后对象再也不能取得新引用，这是淘汰流程不可回滚的提交点。 */
	lockref_mark_dead(&dentry->d_lockref);
	/*
	 * count 变负是不可回滚的死亡标记：之后 lockref_get_not_dead() 不能再使
	 * 对象复活。只有建立全部锁环境并再次确认零引用后才能跨过此提交点。
	 */

	/*
	 * inform the fs via d_prune that this dentry is about to be
	 * unhashed and destroyed.
	 */
	/* 在通用层摘除状态前通知文件系统，使其释放依赖 dentry 仍完整可见的私有缓存。 */
	if (dentry->d_flags & DCACHE_OP_PRUNE)
		dentry->d_op->d_prune(dentry);

	if (dentry->d_flags & DCACHE_LRU_LIST) {
		if (!(dentry->d_flags & DCACHE_SHRINK_LIST))
			d_lru_del(dentry);
	}
	/* if it was on the hash then remove it */
	/* 无论正负，只要仍在正式哈希就先取消新 lookup 的可发现性。 */
	__d_drop(dentry);
	if (dentry->d_inode)
		dentry_unlink_inode(dentry);
	else
		spin_unlock(&dentry->d_lock);
	this_cpu_dec(nr_dentry);
	if (dentry->d_op && dentry->d_op->d_release)
		dentry->d_op->d_release(dentry);
	/* 到这里对象已脱离哈希和 inode，但仍由父子树或 shrink 私有链保活。 */

	cond_resched();
	/* now that it's negative, ->d_parent is stable */
	/* inode 已解除后不会再被 rename 成另一正项，因而可稳定读取 parent 并向上放引用。 */
	if (!IS_ROOT(dentry)) {
		parent = dentry->d_parent;
		spin_lock(&parent->d_lock);
	}
	spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
	dentry_unlist(dentry);
	if (dentry->d_flags & DCACHE_SHRINK_LIST)
		can_free = false;
	spin_unlock(&dentry->d_lock);
	if (likely(can_free))
		dentry_free(dentry);
	if (parent && --parent->d_lockref.count) {
		spin_unlock(&parent->d_lock);
		return NULL;
	}
	return parent;
}

/*
 * Decide if dentry is worth retaining.  Usually this is called with dentry
 * locked; if not locked, we are more limited and might not be able to tell
 * without a lock.  False in this case means "punt to locked path and recheck".
 *
 * In case we aren't locked, these predicates are not "stable". However, it is
 * sufficient that at some point after we dropped the reference the dentry was
 * hashed and the flags had the proper value. Other dentry users may have
 * re-gotten a reference to the dentry and change that, but our work is done -
 * we can leave the dentry around with a zero refcount.
 */
/*
 * retain_dentry() 判断最后一份主动引用释放后是否值得保留缓存。@locked 指明
 * 调用者是否持 d_lock：无锁路径只在无需修改状态时作肯定判断，任何不稳定或
 * 需要入 LRU/置 REFERENCED 的情形都返回 false，交给加锁路径复核。unhashed、
 * disconnected、DONTCACHE 或文件系统 d_delete 拒绝的对象必须淘汰；其余对象
 * 进入 LRU，已在 LRU 的对象获得一次“最近使用”机会。smp_rmb 与 lockref 的
 * 原子更新顺序配合，避免在引用归零后读取到过旧 flags。
 */
static inline bool retain_dentry(struct dentry *dentry, bool locked)
{
	unsigned int d_flags;

	smp_rmb();
	d_flags = READ_ONCE(dentry->d_flags);

	// Unreachable? Nobody would be able to look it up, no point retaining
	/* unhashed 对象无法被未来路径查找复用，留在 LRU 只会浪费内存。 */
	if (unlikely(d_unhashed(dentry)))
		return false;

	// Same if it's disconnected
	/* disconnected alias 不属于完整命名树，零引用后也不值得作为普通缓存保留。 */
	if (unlikely(d_flags & DCACHE_DISCONNECTED))
		return false;

	// ->d_delete() might tell us not to bother, but that requires
	// ->d_lock; can't decide without it
	/* 文件系统 d_delete 回调可否决缓存，但必须持 d_lock；无锁阶段只能转慢路径。 */
	if (unlikely(d_flags & DCACHE_OP_DELETE)) {
		if (!locked || dentry->d_op->d_delete(dentry))
			return false;
	}

	// Explicitly told not to bother
	/* DONTCACHE 是显式淘汰请求，最后引用释放时不得进入 LRU。 */
	if (unlikely(d_flags & DCACHE_DONTCACHE))
		return false;

	// At this point it looks like we ought to keep it.  We also might
	// need to do something - put it on LRU if it wasn't there already
	// and mark it referenced if it was on LRU, but not marked yet.
	// Unfortunately, both actions require ->d_lock, so in lockless
	// case we'd have to punt rather than doing those.
	/*
	 * 到此对象原则上可保留；首次入 LRU 或给已有 LRU 项置 REFERENCED 都会写受
	 * d_lock 保护的状态，所以无锁路径必须返回 false，由持锁慢路径完成。
	 */
	if (unlikely(!(d_flags & DCACHE_LRU_LIST))) {
		if (!locked)
			return false;
		d_lru_add(dentry);
	} else if (unlikely(!(d_flags & DCACHE_REFERENCED))) {
		if (!locked)
			return false;
		dentry->d_flags |= DCACHE_REFERENCED;
	}
	return true;
}

/*
 * d_mark_dontcache() 给借用 @inode 的所有 alias 置 DONTCACHE，并设置 inode 状态；
 * 入口无需锁、内部遵守 i_lock -> d_lock，无返回且不释放现有引用。
 */
/*
 * d_mark_dontcache() 遍历 @inode 的全部 alias，在 i_lock -> d_lock 顺序下设置
 * DCACHE_DONTCACHE，并设置 inode 的 I_DONTCACHE 状态。现有引用不受影响；每个
 * alias 在最后 dput 时将绕过 LRU 保留。@inode 为借用且调用期间必须存活，
 * 函数不睡眠、无返回。
 */
void d_mark_dontcache(struct inode *inode)
{
	struct dentry *de;

	spin_lock(&inode->i_lock);
	for_each_alias(de, inode) {
		spin_lock(&de->d_lock);
		de->d_flags |= DCACHE_DONTCACHE;
		spin_unlock(&de->d_lock);
	}
	inode_state_set(inode, I_DONTCACHE);
	spin_unlock(&inode->i_lock);
}

EXPORT_SYMBOL(d_mark_dontcache);

/*
 * Try to do a lockless dput(), and return whether that was successful.
 *
 * If unsuccessful, we return false, having already taken the dentry lock.
 * In that case refcount is guaranteed to be zero and we have already
 * decided that it's not worth keeping around.
 */
/*
 * fast_dput() - 乐观释放一份 dentry 引用。
 *
 * @dentry 是调用者拥有的一份引用。lockref_put_return() 在 RCU 保护下把锁与
 * 引用计数合并成一次原子快路径：剩余引用非零立即成功；归零但可无锁确认保留
 * 也成功；若锁竞争或需要改变 LRU/销毁状态，则取得 d_lock、重新检查复活/死亡
 * 竞态。返回 true 表示引用已经完整处理且不持锁；false 表示 count 为零、对象
 * 不应保留并把 d_lock 留给 finish_dput()。RCU 只防内存释放，不稳定字段值。
 */
static inline bool fast_dput(struct dentry *dentry)
{
	int ret;

	/*
	 * Try to decrement the lockref optimistically.
	 * RCU read lock held so that dentry is guaranteed to stay around
	 * even if the refcount goes down to zero.
	 */
	/* RCU 只保证原子减引用到零后内存仍在，是否保留/销毁仍由后续状态检查决定。 */
	rcu_read_lock();
	ret = lockref_put_return(&dentry->d_lockref);

	/*
	 * If the lockref_put_return() failed due to the lock being held
	 * by somebody else, the fast path has failed. We will need to
	 * get the lock, and then check the count again.
	 */
	/* lockref 与 d_lock 竞争时原子快路无法线性化，转为显式持锁并重新读取 count。 */
	if (unlikely(ret < 0)) {
		spin_lock(&dentry->d_lock);
		rcu_read_unlock();
		if (WARN_ON_ONCE(dentry->d_lockref.count <= 0)) {
			spin_unlock(&dentry->d_lock);
			return true;
		}
		dentry->d_lockref.count--;
		goto locked;
	}

	/*
	 * If we weren't the last ref, we're done.
	 */
	/* 返回值非零表示仍有其他主动引用，本次调用只需交还自己的一份。 */
	if (ret) {
		rcu_read_unlock();
		return true;
	}

	/*
	 * Can we decide that decrement of refcount is all we needed without
	 * taking the lock?  There's a very common case when it's all we need -
	 * dentry looks like it ought to be retained and there's nothing else
	 * to do.
	 */
	/* 常见零引用对象若已在合适 LRU 状态，无需写共享字段即可结束 dput。 */
	if (retain_dentry(dentry, false)) {
		rcu_read_unlock();
		return true;
	}

	/*
	 * Either not worth retaining or we can't tell without the lock.
	 * Get the lock, then.  We've already decremented the refcount to 0,
	 * but we'll need to re-check the situation after getting the lock.
	 */
	/* count 已为零；取锁后必须允许并发 lookup 在此前把它重新增为正数。 */
	spin_lock(&dentry->d_lock);
	rcu_read_unlock();

	/*
	 * Did somebody else grab a reference to it in the meantime, and
	 * we're no longer the last user after all? Alternatively, somebody
	 * else could have killed it and marked it dead. Either way, we
	 * don't need to do anything else.
	 */
	/* 锁下再次判断复活、死亡和保留条件；只有仍为零且不可保留才把锁交给销毁者。 */
locked:
	if (dentry->d_lockref.count || retain_dentry(dentry, true)) {
		spin_unlock(&dentry->d_lock);
		return true;
	}
	return false;
}

/* finish_dput() 接收持 d_lock 的零引用 @dentry，迭代销毁并可能向上处理 parent。 */
/*
 * finish_dput() 接收 fast_dput() 留下的零引用、持 d_lock 对象，循环调用
 * dentry_kill()；若父引用也降到零就继续向上。某一级在锁下重新满足保留条件
 * 时将其放入/标记 LRU 后解锁返回。循环避免深目录链导致内核栈递归溢出。
 */
static void finish_dput(struct dentry *dentry)
	__releases(dentry->d_lock)
{
	while ((dentry = dentry_kill(dentry)) != NULL) {
		if (retain_dentry(dentry, true)) {
			spin_unlock(&dentry->d_lock);
			return;
		}
	}
}

/* 
 * This is dput
 *
 * This is complicated by the fact that we do not want to put
 * dentries that are no longer on any hash chain on the unused
 * list: we'd much rather just get rid of them immediately.
 *
 * However, that implies that we have to traverse the dentry
 * tree upwards to the parents which might _also_ now be
 * scheduled for deletion (it may have been only waiting for
 * its last child to go away).
 *
 * This tail recursion is done by hand as we don't want to depend
 * on the compiler to always get this right (gcc generally doesn't).
 * Real recursion would eat up our stack space.
 */
/*
 * dput 的难点是 unhashed 零引用项不应进入 unused LRU，而应立即删除；删除一个
 * 子项又会交还它持有的 parent 引用，使祖先也可能恰好归零。实现用显式 while
 * 向上处理这条尾递归链，不依赖编译器尾调用优化，也避免深路径耗尽内核栈。
 */

/*
 * dput - release a dentry
 * @dentry: dentry to release 
 *
 * Release a dentry. This will drop the usage count and if appropriate
 * call the dentry unlink method as well as removing it from the queues and
 * releasing its resources. If the parent dentries were scheduled for release
 * they too may now get deleted.
 */
/*
 * dput() 是公开的引用释放入口。NULL 是允许的空操作；非 NULL 表示调用者转交
 * 恰好一份引用。函数要求可睡眠上下文，先走无锁常见路径，最后一份且不可缓存
 * 时进入同步销毁；销毁还可能级联释放祖先。无返回，调用后调用者不得再凭该
 * 引用访问对象。
 */
void dput(struct dentry *dentry)
{
	if (!dentry)
		return;
	might_sleep();
	if (likely(fast_dput(dentry)))
		return;
	finish_dput(dentry);
}
EXPORT_SYMBOL(dput);

/* d_make_discardable() 撤销 @dentry 的 PERSISTENT 钉住引用，并可能同步销毁它。 */
/*
 * d_make_discardable() 撤销 d_make_persistent() 额外建立的永久钉住引用。入口
 * 要求 PERSISTENT；清位并减引用后直接把持锁状态交给 finish_dput()。若仍有
 * 其他引用对象继续存活，否则可能在函数内销毁。无返回、调用后不拥有新引用。
 */
void d_make_discardable(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	WARN_ON(!(dentry->d_flags & DCACHE_PERSISTENT));
	dentry->d_flags &= ~DCACHE_PERSISTENT;
	dentry->d_lockref.count--;
	finish_dput(dentry);
}

EXPORT_SYMBOL(d_make_discardable);

/**
 * __move_to_shrink_list - try to place a dentry into a shrink list
 * @dentry:	dentry to try putting into shrink list
 * @list:	the list to put @dentry into.
 * Returns:	true @dentry had been placed into @list, false otherwise
 *
 * If @dentry is idle and not already include into a shrink list, move
 * it into @list and return %true; otherwise do nothing and return %false.
 *
 * Caller must be holding @dentry->d_lock.  There must have been no calls of
 * dentry_free(@dentry) prior to the beginning of the RCU read-side critical
 * area in which __move_to_shrink_list(@dentry, @list) is called.
 *
 * @list should be thread-private and eventually emptied by passing it to
 * shrink_dentry_list().
 */

/* 持 d_lock 尝试把零引用 @dentry 交给私有 @list；成功取得处置权返回 true。 */
/*
 * __move_to_shrink_list() 在 d_lock 与外层连续 RCU 生命周期保护下，尝试把零
 * 引用且尚未被其他回收者领取的 dentry 移到调用者私有 @list。true 表示当前
 * 回收者取得处置权；false 表示对象忙或已被领取。它不释放对象，也不睡眠。
 */
bool __move_to_shrink_list(struct dentry *dentry, struct list_head *list)
__must_hold(&dentry->d_lock)
{
	if (likely(!dentry->d_lockref.count &&
	    !(dentry->d_flags & DCACHE_SHRINK_LIST))) {
		if (dentry->d_flags & DCACHE_LRU_LIST)
			d_lru_del(dentry);
		d_shrink_add(dentry, list);
		return true;
	}
	return false;
}

EXPORT_SYMBOL(__move_to_shrink_list);

/* dput_to_list() 消耗一份 @dentry 引用，把慢路径对象移交调用者私有 @list。 */
/*
 * dput_to_list() 释放一份引用，但把需要销毁的对象批量交给 @list，而非当场
 * 递归回收。成功快路径不改列表；慢路径返回时不持 d_lock。调用者最终必须把
 * 私有链交给 shrink_dentry_list()，否则会泄漏处置中的对象。
 */
void dput_to_list(struct dentry *dentry, struct list_head *list)
{
	if (likely(fast_dput(dentry)))
		return;
	__move_to_shrink_list(dentry, list);
	spin_unlock(&dentry->d_lock);
}

/*
 * dget_parent() 为借用的 @dentry 取得其当前 parent 的持有引用。先在 RCU 下
 * 读取 d_seq、父指针并尝试 lockref_get_not_zero；rename 使序列变化则撤销引用。
 * 慢路径锁住候选 parent 并重新核对 dentry->d_parent，保证返回对象既未死又确为
 * 当前父。返回的引用必须 dput()；函数不返回 NULL，也不睡眠。
 */
struct dentry *dget_parent(struct dentry *dentry)
{
	int gotref;
	struct dentry *ret;
	unsigned seq;

	/*
	 * Do optimistic parent lookup without any
	 * locking.
	 */
	/* 先在 RCU 与 d_seq 下乐观读取 parent 并尝试非零增引用，常见无 rename 时免锁。 */
	rcu_read_lock();
	seq = raw_seqcount_begin(&dentry->d_seq);
	ret = READ_ONCE(dentry->d_parent);
	gotref = lockref_get_not_zero(&ret->d_lockref);
	rcu_read_unlock();
	if (likely(gotref)) {
		if (!read_seqcount_retry(&dentry->d_seq, seq))
			return ret;
		dput(ret);
	}

repeat:
	/*
	 * Don't need rcu_dereference because we re-check it was correct under
	 * the lock.
	 */
	/* 慢路虽直接读 parent，但随后锁住候选并重新比较，失败即循环，因此无需依赖值。 */
	rcu_read_lock();
	ret = dentry->d_parent;
	spin_lock(&ret->d_lock);
	if (unlikely(ret != dentry->d_parent)) {
		spin_unlock(&ret->d_lock);
		rcu_read_unlock();
		goto repeat;
	}
	rcu_read_unlock();
	BUG_ON(!ret->d_lockref.count);
	ret->d_lockref.count++;
	spin_unlock(&ret->d_lock);
	return ret;
}

EXPORT_SYMBOL(dget_parent);

/*
 * inode is a directory, inode->i_lock is held by the caller
 */
/* 要求 @inode->i_lock；目录若有唯一 alias，增引用返回，否则 NULL。 */
/*
 * __d_find_dir_alias() 要求 inode->i_lock，利用目录“一 inode 至多一个 alias”
 * 不变量返回首项并增引用；__d_find_any_alias() 适用于非目录，跳过无法安全增引
 * 的 NORCU/垂死 alias。均返回持有引用或 NULL，调用者负责 dput()。
 */
static struct dentry * __d_find_dir_alias(struct inode *inode)
{
	struct dentry *alias;

	if (hlist_empty(&inode->i_dentry))
		return NULL;
	alias = hlist_entry(inode->i_dentry.first, struct dentry, d_alias);
	lockref_get(&alias->d_lockref);
	return alias;
}

/*
 * __d_find_any_alias() 要求调用者持 @inode->i_lock，遍历 alias 并尝试取得一份
 * 不会从零复活的安全引用；成功返回持有引用，全部不可取或链为空时返回 NULL。
 */
static struct dentry * __d_find_any_alias(struct inode *inode)
{
	struct dentry *alias;

	if (hlist_empty(&inode->i_dentry))
		return NULL;
	for_each_alias(alias, inode)
		if (dget_alias_ilocked(alias))
			return alias;
	return NULL;
}

/**
 * d_find_any_alias - find any alias for a given inode
 * @inode: inode to find an alias for
 *
 * If any aliases exist for the given inode, take and return a
 * reference for one of them.  If no aliases exist, return %NULL.
 */
/* 为借用 @inode 返回任意可增引用 alias；无 alias 返回 NULL，结果须 dput。 */
/* d_find_any_alias() 是上述 helper 的 i_lock 包装；@inode 为借用，可返回任意 alias。 */
struct dentry *d_find_any_alias(struct inode *inode)
{
	struct dentry *de;

	spin_lock(&inode->i_lock);
	de = __d_find_any_alias(inode);
	spin_unlock(&inode->i_lock);
	return de;
}

EXPORT_SYMBOL(d_find_any_alias);

/* __d_find_alias() 要求 inode->i_lock，返回合适 alias 的持有引用或 NULL。 */
static struct dentry *__d_find_alias(struct inode *inode)
{
	struct dentry *alias;

	if (S_ISDIR(inode->i_mode))
		return __d_find_dir_alias(inode);

	for_each_alias(alias, inode) {
		spin_lock(&alias->d_lock);
 		if (!d_unhashed(alias)) {
			dget_dlock(alias);
			spin_unlock(&alias->d_lock);
			return alias;
		}
		spin_unlock(&alias->d_lock);
	}
	return NULL;
}

/**
 * d_find_alias - grab a hashed alias of inode
 * @inode: inode in question
 *
 * If inode has a hashed alias, or is a directory and has any alias,
 * acquire the reference to alias and return it. Otherwise return NULL.
 * Notice that if inode is a directory there can be only one alias and
 * it can be unhashed only if it has no children, or if it is the root
 * of a filesystem, or if the directory was renamed and d_revalidate
 * was the first vfs operation to notice.
 *
 * If the inode has an IS_ROOT, DCACHE_DISCONNECTED alias, then prefer
 * any other hashed alias over that one.
 */
/* 查找 @inode 的 hashed alias（目录可返回特殊唯一 alias），返回引用或 NULL。 */
/*
 * d_find_alias() 优先返回可哈希查找的持有引用；目录可返回唯一的 unhashed 根/
 * disconnected alias。非目录逐项取得 alias->d_lock 排除并发 d_drop。NULL 表示
 * 当前没有合适 alias，返回非 NULL 必须由调用者 dput()。
 */
struct dentry *d_find_alias(struct inode *inode)
{
	struct dentry *de = NULL;

	if (!hlist_empty(&inode->i_dentry)) {
		spin_lock(&inode->i_lock);
		de = __d_find_alias(inode);
		spin_unlock(&inode->i_lock);
	}
	return de;
}

EXPORT_SYMBOL(d_find_alias);

/*
 *  Caller MUST be holding rcu_read_lock() and be guaranteed
 *  that inode won't get freed until rcu_read_unlock().
 */
/* 调用者持 RCU 且稳定 @inode；返回不增引用的 alias 裸指针或 NULL。 */
/*
 * d_find_alias_rcu() 要求调用者已持 rcu_read_lock 且保证 inode 过临界区不释放。
 * 它在 i_lock 下返回裸借用指针，不增加 dentry 引用；I_FREEING 时 d_alias 存储
 * 可能与 i_rcu 复用，必须拒绝访问。指针只能在既定 RCU 契约内短暂使用。
 */
struct dentry *d_find_alias_rcu(struct inode *inode)
{
	struct hlist_head *l = &inode->i_dentry;
	struct dentry *de = NULL;

	spin_lock(&inode->i_lock);
	// ->i_dentry and ->i_rcu are colocated, but the latter won't be
	// used without having I_FREEING set, which means no aliases left
		/*
		 * i_dentry 与 i_rcu 复用相邻/重叠存储；只有置 I_FREEING 且 alias 已清空后
		 * 才会启用 i_rcu，所以先检查状态即可安全把当前存储解释成 alias 链。
		 */
	if (likely(!(inode_state_read(inode) & I_FREEING) && !hlist_empty(l))) {
		if (S_ISDIR(inode->i_mode)) {
			de = hlist_entry(l->first, struct dentry, d_alias);
		} else {
			hlist_for_each_entry(de, l, d_alias)
				if (!d_unhashed(de))
					break;
		}
	}
	spin_unlock(&inode->i_lock);
	return de;
}

/*
 *	Try to kill dentries associated with this inode.
 * WARNING: you must own a reference to inode.
 */
/* 在持有 @inode 引用时领取并销毁其零引用、非 NORCU alias；忙项保留。 */
/*
 * d_prune_aliases() 在持有一份 inode 引用的前提下领取其所有零引用、可 RCU
 * 回收的 alias 到私有链，释放 i_lock 后批量销毁。忙 alias 与 NORCU alias 被
 * 跳过；函数可睡眠，无返回，并不保证调用后 inode 完全没有 alias。
 */
void d_prune_aliases(struct inode *inode)
{
	LIST_HEAD(dispose);
	struct dentry *dentry;

	spin_lock(&inode->i_lock);
	for_each_alias(dentry, inode) {
		spin_lock(&dentry->d_lock);
		if (likely(!(dentry->d_flags & DCACHE_NORCU)))
			__move_to_shrink_list(dentry, &dispose);
		spin_unlock(&dentry->d_lock);
	}
	spin_unlock(&inode->i_lock);
	shrink_dentry_list(&dispose);
}

EXPORT_SYMBOL(d_prune_aliases);

/* shrink_kill() 消耗已锁定零引用 victim，并迭代处理随之归零的祖先。 */
static inline void shrink_kill(struct dentry *victim)
{
	while ((victim = dentry_kill(victim)) != NULL)
		;
}

/* shrink_dentry_list() 消耗并清空线程私有 @list，销毁或最终释放其中每个对象。 */
/*
 * shrink_dentry_list() 消耗调用者私有 shrink list。每项先在 d_lock 下移出链；
 * 已 KILLED 的对象只剩当前链所有权，可直接进入 dentry_free()，否则交给
 * dentry_kill() 完成全套摘除。返回时 @list 为空，函数可能调度且所有被领取
 * 对象均已释放或排入 RCU 回调。
 */
void shrink_dentry_list(struct list_head *list)
{
	while (!list_empty(list)) {
		struct dentry *dentry;

		dentry = list_entry(list->prev, struct dentry, d_lru);
		spin_lock(&dentry->d_lock);
		d_shrink_del(dentry);
		if (unlikely(dentry->d_flags & DCACHE_DENTRY_KILLED)) {
			spin_unlock(&dentry->d_lock);
			dentry_free(dentry);
			continue;
		}
		shrink_kill(dentry);
	}
}

EXPORT_SYMBOL(shrink_dentry_list);

/*
 * dentry_lru_isolate() 是持 LRU 锁的 shrinker 回调；trylock dentry 后返回
 * SKIP、REMOVED 或 ROTATE，并把可回收项移交 @arg 私有链。
 */
/*
 * dentry_lru_isolate() 是内存回收 list_lru 回调。由于入口已持 LRU 锁而正常
 * 锁序是 d_lock -> LRU 锁，只能 trylock 避免反序死锁。活跃项从 LRU 移除；
 * REFERENCED 零引用项清标记并旋到尾部获得第二次机会；其余项移入 freeable
 * 私有链。返回 LRU_SKIP/REMOVED/ROTATE 指示通用框架怎样处理节点。
 */
static enum lru_status dentry_lru_isolate(struct list_head *item,
		struct list_lru_one *lru, void *arg)
{
	struct list_head *freeable = arg;
	struct dentry	*dentry = container_of(item, struct dentry, d_lru);


	/*
	 * we are inverting the lru lock/dentry->d_lock here,
	 * so use a trylock. If we fail to get the lock, just skip
	 * it
	 */
	/* 当前已持 LRU 锁，与正常 d_lock -> LRU 顺序相反，只能 trylock；失败跳过防死锁。 */
	if (!spin_trylock(&dentry->d_lock))
		return LRU_SKIP;

	/*
	 * Referenced dentries are still in use. If they have active
	 * counts, just remove them from the LRU. Otherwise give them
	 * another pass through the LRU.
	 */
	/* 活跃引用项退出回收 LRU；零引用但 REFERENCED 的项清标志并获得下一轮机会。 */
	if (dentry->d_lockref.count) {
		d_lru_isolate(lru, dentry);
		spin_unlock(&dentry->d_lock);
		return LRU_REMOVED;
	}

	if (dentry->d_flags & DCACHE_REFERENCED) {
		dentry->d_flags &= ~DCACHE_REFERENCED;
		spin_unlock(&dentry->d_lock);

		/*
		 * The list move itself will be made by the common LRU code. At
		 * this point, we've dropped the dentry->d_lock but keep the
		 * lru lock. This is safe to do, since every list movement is
		 * protected by the lru lock even if both locks are held.
		 *
		 * This is guaranteed by the fact that all LRU management
		 * functions are intermediated by the LRU API calls like
		 * list_lru_add_obj and list_lru_del_obj. List movement in this file
		 * only ever occur through this functions or through callbacks
		 * like this one, that are called from the LRU API.
		 *
		 * The only exceptions to this are functions like
		 * shrink_dentry_list, and code that first checks for the
		 * DCACHE_SHRINK_LIST flag.  Those are guaranteed to be
		 * operating only with stack provided lists after they are
		 * properly isolated from the main list.  It is thus, always a
		 * local access.
		 */
		/*
		 * 这里释放 d_lock 但继续持 LRU 锁，由通用 list_lru 框架完成旋转。所有公共
		 * LRU 移动都经 list_lru API 或其持锁回调，所以单持 LRU 锁足够；只有已经
		 * 隔离到栈上私有链并以 SHRINK_LIST 标记的代码可绕过公共锁直接访问。
		 */
		return LRU_ROTATE;
	}

	d_lru_shrink_move(lru, dentry, freeable);
	spin_unlock(&dentry->d_lock);

	return LRU_REMOVED;
}

/**
 * prune_dcache_sb - shrink the dcache
 * @sb: superblock
 * @sc: shrink control, passed to list_lru_shrink_walk()
 *
 * Attempt to shrink the superblock dcache LRU by @sc->nr_to_scan entries. This
 * is done when we need more memory and called from the superblock shrinker
 * function.
 *
 * This function may fail to free any resources if all the dentries are in
 * use.
 */
/* 按 @sc 的预算扫描 @sb LRU 并在锁外销毁隔离项；返回回收框架计数。 */
/*
 * prune_dcache_sb() 是 superblock shrinker 的有界回收入口。@sb、@sc 均为借用；
 * sc 指定 NUMA/memcg 与最多扫描量。list_lru_shrink_walk() 只负责隔离，真正销毁
 * 在锁外由 shrink_dentry_list() 完成。返回框架认定已释放/隔离的数量；所有项
 * 忙时可返回 0，不能据此断言 dcache 为空。
 */
long prune_dcache_sb(struct super_block *sb, struct shrink_control *sc)
{
	LIST_HEAD(dispose);
	long freed;

	freed = list_lru_shrink_walk(&sb->s_dentry_lru, sc,
				     dentry_lru_isolate, &dispose);
	shrink_dentry_list(&dispose);
	return freed;
}

/*
 * dentry_lru_isolate_shrink() 用于卸载前强制清空：仍以 trylock 避免锁反转，但
 * 不做引用/REFERENCED 的第二次机会策略，直接把可锁项移到私有回收链。
 */
static enum lru_status dentry_lru_isolate_shrink(struct list_head *item,
		struct list_lru_one *lru, void *arg)
{
	struct list_head *freeable = arg;
	struct dentry	*dentry = container_of(item, struct dentry, d_lru);

	/*
	 * we are inverting the lru lock/dentry->d_lock here,
	 * so use a trylock. If we fail to get the lock, just skip
	 * it
	 */
	/* 卸载扫描同样反转 LRU/d_lock 顺序，必须 trylock；失败留待下一轮，不能阻塞。 */
	if (!spin_trylock(&dentry->d_lock))
		return LRU_SKIP;

	d_lru_shrink_move(lru, dentry, freeable);
	spin_unlock(&dentry->d_lock);

	return LRU_REMOVED;
}



/**
 * shrink_dcache_sb - shrink dcache for a superblock
 * @sb: superblock
 *
 * Shrink the dcache for the specified super block. This is used to free
 * the dcache before unmounting a file system.
 */
/* 卸载准备阶段反复清空 @sb 的公共 dentry LRU；不处理仍被引用的对象。 */
/*
 * shrink_dcache_sb() 反复按 1024 项隔离并销毁，直到该 superblock 的公共 LRU
 * 计数归零。它用于卸载准备，允许睡眠；只处理 LRU 零引用项，仍被使用的
 * dentry 不在此接口的处置范围。
 */
void shrink_dcache_sb(struct super_block *sb)
{
	do {
		LIST_HEAD(dispose);

		list_lru_walk(&sb->s_dentry_lru,
			dentry_lru_isolate_shrink, &dispose, 1024);
		shrink_dentry_list(&dispose);
	} while (list_lru_count(&sb->s_dentry_lru) > 0);
}

EXPORT_SYMBOL(shrink_dcache_sb);

/**
 * enum d_walk_ret - action to take during tree walk
 * @D_WALK_CONTINUE:	continue walk
 * @D_WALK_QUIT:	quit walk
 * @D_WALK_NORETRY:	quit when retry is needed
 * @D_WALK_SKIP:	skip this dentry and its children
 */
/*
 * 遍历回调动作：CONTINUE 继续并进入子树；QUIT 立即退出；NORETRY 在需要 rename
 * 重试时退出；SKIP 只跳过当前 dentry 及其全部孩子。
 */
enum d_walk_ret {
	D_WALK_CONTINUE,
	D_WALK_QUIT,
	D_WALK_NORETRY,
	D_WALK_SKIP,
};

/*
 * d_walk_ret 是回调对深度优先遍历器的控制协议：CONTINUE 进入子树，QUIT 立即
 * 结束，NORETRY 继续当前遍历但 rename 冲突时不重跑，SKIP 跳过当前子树。
 */

/**
 * d_walk - walk the dentry tree
 * @parent:	start of walk
 * @data:	data passed to @enter() and @finish()
 * @enter:	callback when first entering the dentry
 *
 * The @enter() callbacks are called with d_lock held.
 */
/* 从持有引用的 @parent 非递归遍历子树，在 d_lock 下调用 @enter，并处理 rename 重试。 */
/*
 * d_walk() 是非递归、可并发 rename 的 dentry 子树遍历器。@parent 必须由调用者
 * 持引用；@data 原样传给 @enter；回调在目标 d_lock 下执行且不得睡眠。通常先
 * 乐观 seqcount 遍历，拓扑变化时重试；回调返回 NORETRY 可放弃进度保证以避免
 * 重复副作用。函数无返回，不获取对象引用，锁只在内部短暂持有。
 */
static void d_walk(struct dentry *parent, void *data,
		   enum d_walk_ret (*enter)(void *, struct dentry *))
{
	struct dentry *this_parent, *dentry;
	unsigned seq = 0;
	enum d_walk_ret ret;
	bool retry = true;

again:
	/* 第一遍只读 rename_lock 序列；发生 rename 时按回调允许性升级后重走。 */
	read_seqbegin_or_lock(&rename_lock, &seq);
	this_parent = parent;
	spin_lock(&this_parent->d_lock);
	if (unlikely(this_parent->d_flags & DCACHE_DENTRY_CURSOR))
		goto out_unlock;

	ret = enter(data, this_parent);
	switch (ret) {
	case D_WALK_CONTINUE:
		break;
	case D_WALK_QUIT:
	case D_WALK_SKIP:
		goto out_unlock;
	case D_WALK_NORETRY:
		retry = false;
		break;
	}
repeat:
	/*
	 * 始终持当前 parent 的 d_lock 扫描 d_children；进入 child 前嵌套取得其
	 * d_lock。cursor 不是命名树成员，必须跳过。
	 */
	dentry = d_first_child(this_parent);
resume:
	hlist_for_each_entry_from(dentry, d_sib) {
		if (unlikely(dentry->d_flags & DCACHE_DENTRY_CURSOR))
			continue;

		spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);

		ret = enter(data, dentry);
		switch (ret) {
		case D_WALK_CONTINUE:
			break;
		case D_WALK_QUIT:
			spin_unlock(&dentry->d_lock);
			goto out_unlock;
		case D_WALK_NORETRY:
			retry = false;
			break;
		case D_WALK_SKIP:
			spin_unlock(&dentry->d_lock);
			continue;
		}

		if (!hlist_empty(&dentry->d_children)) {
			/* 把 child 的锁接力为下一层 parent 锁，避免递归和额外栈空间。 */
			spin_unlock(&this_parent->d_lock);
			spin_release(&dentry->d_lock.dep_map, _RET_IP_);
			this_parent = dentry;
			spin_acquire(&this_parent->d_lock.dep_map, 0, 1, _RET_IP_);
			goto repeat;
		}
		spin_unlock(&dentry->d_lock);
	}
	/*
	 * All done at this level ... ascend and resume the search.
	 */
	/* 当前层兄弟已处理完，沿 parent 上升并从刚离开的节点之后恢复深度优先扫描。 */
ascend:
	/*
	 * 一层完成后沿 d_parent 上升。解 child 锁到取 parent 锁的窗口以 RCU
	 * 保证指针存活，再用 rename_lock 序列确认没有走到旧父关系。
	 */
	if (this_parent != parent) {
		dentry = this_parent;
		this_parent = dentry->d_parent;

		rcu_read_lock();
		spin_unlock(&dentry->d_lock);
		spin_lock(&this_parent->d_lock);
		rcu_read_unlock();

		/* might go back up the wrong parent if we have had a rename. */
		/* 上升放锁期间若发生 rename，裸 parent 可能属于旧树，必须由序列检查发现。 */
		if (need_seqretry(&rename_lock, seq))
			goto rename_retry;
		/* go into the first sibling still alive */
		/* 从旧节点后继继续时跳过已 KILLED 兄弟，避免回调已脱树对象。 */
		hlist_for_each_entry_continue(dentry, d_sib) {
			if (likely(!(dentry->d_flags & DCACHE_DENTRY_KILLED))) {
				goto resume;
			}
		}
		goto ascend;
	}
	if (need_seqretry(&rename_lock, seq))
		goto rename_retry;

out_unlock:
	spin_unlock(&this_parent->d_lock);
	done_seqretry(&rename_lock, seq);
	return;

rename_retry:
	spin_unlock(&this_parent->d_lock);
	BUG_ON(seq & 1);
	if (!retry)
		return;
	seq = 1;
	goto again;
}

/*
 * check_mount 是 path_has_submounts() 传给 d_walk() 的栈上输入输出状态。mnt 是
 * 借用的目标挂载实例，mounted 初始为 0，回调发现当前命名空间的真实挂载点后
 * 置 1 并停止遍历；结构体不取得 mount/dentry 引用。
 */
struct check_mount {
	struct vfsmount *mnt;
	unsigned int mounted;
};

/* check_mount 把待检查的 vfsmount 与布尔结果带过 d_walk；均由栈上调用者拥有。 */

/* locks: mount_locked_reader && dentry->d_lock */
/* 在 mount 读锁与 d_lock 下检测当前 dentry 的真实挂载点，命中即终止遍历。 */
/*
 * path_check_mount() 在 mount_locked_reader 与 d_lock 下检查当前 dentry 是否真是
 * @mnt 命名空间中的挂载点；快速跳过未置 MOUNTED 的项，命中后置结果并终止。
 */
static enum d_walk_ret path_check_mount(void *data, struct dentry *dentry)
{
	struct check_mount *info = data;
	struct path path = { .mnt = info->mnt, .dentry = dentry };

	if (likely(!d_mountpoint(dentry)))
		return D_WALK_CONTINUE;
	if (__path_is_mountpoint(&path)) {
		info->mounted = 1;
		return D_WALK_QUIT;
	}
	return D_WALK_CONTINUE;
}

/**
 * path_has_submounts - check for mounts over a dentry in the
 *                      current namespace.
 * @parent: path to check.
 *
 * Return true if the parent or its subdirectories contain
 * a mount point in the current namespace.
 */
/* 检查借用 @parent 自身或后代是否有当前命名空间挂载点；返回布尔整数。 */
/*
 * path_has_submounts() 在 mount hash 读锁保护下遍历 @parent 子树。@parent 是
 * 借用 path；返回非零表示自身或后代存在当前 mount namespace 可见挂载点。
 * guard 离开作用域自动解锁，函数可因完整树扫描而耗时但不转移引用。
 */
int path_has_submounts(const struct path *parent)
{
	struct check_mount data = { .mnt = parent->mnt, .mounted = 0 };

	guard(mount_locked_reader)();
	d_walk(parent->dentry, &data, path_check_mount);

	return data.mounted;
}

EXPORT_SYMBOL(path_has_submounts);

/*
 * Called by mount code to set a mountpoint and check if the mountpoint is
 * reachable (e.g. NFS can unhash a directory dentry and then the complete
 * subtree can become unreachable).
 *
 * Only one of d_invalidate() and d_set_mounted() must succeed.  For
 * this reason take rename_lock and d_lock on dentry and ancestors.
 */
/* 与 invalidation 互斥地置 MOUNTED；成功 0，脱链 -ENOENT，已挂载 -EBUSY。 */
/*
 * d_set_mounted() 与 d_invalidate() 竞争地把 dentry 标为挂载点。独占读侧
 * rename_lock 稳定整条祖先链，再逐级确认没有 unhashed 祖先；目标已脱链返回
 * -ENOENT，已是挂载点返回 -EBUSY，首次置 DCACHE_MOUNTED 返回 0。@dentry 为
 * 借用，无引用转移；全程自旋锁范围内不睡眠。
 */
int d_set_mounted(struct dentry *dentry)
{
	struct dentry *p;
	int ret = -ENOENT;
	read_seqlock_excl(&rename_lock);
	for (p = dentry->d_parent; !IS_ROOT(p); p = p->d_parent) {
		/* Need exclusion wrt. d_invalidate() */
		/* 祖先 d_lock 与 rename_lock 共同排除 invalidate 正在取消可达性的窗口。 */
		spin_lock(&p->d_lock);
		if (unlikely(d_unhashed(p))) {
			spin_unlock(&p->d_lock);
			goto out;
		}
		spin_unlock(&p->d_lock);
	}
	spin_lock(&dentry->d_lock);
	if (!d_unlinked(dentry)) {
		ret = -EBUSY;
		if (!d_mountpoint(dentry)) {
			dentry->d_flags |= DCACHE_MOUNTED;
			ret = 0;
		}
	}
 	spin_unlock(&dentry->d_lock);
out:
	read_sequnlock_excl(&rename_lock);
	return ret;
}

/*
 * Search the dentry child list of the specified parent,
 * and move any unused dentries to the end of the unused
 * list for prune_dcache(). We descend to the next level
 * whenever the d_children list is non-empty and continue
 * searching.
 *
 * It returns zero iff there are no unused children,
 * otherwise  it returns the number of children moved to
 * the end of the unused list. This may not be the total
 * number of unused children, because select_parent can
 * drop the lock and return early due to latency
 * constraints.
 */
/*
 * 该选择过程深度优先扫描 parent 的 children，把零引用项移到回收链尾；遇到子树
 * 继续下降。返回/统计可能是分批近似值：为了调度延迟，取得部分候选后允许提前
 * 结束，调用者随后会再次进入，因此“非零”只表示本轮取得进展而非总数。
 */

/*
 * select_data 是 shrink_dcache_tree() 每轮遍历的栈上状态。start 是不可回收的
 * 根；第一阶段用 found 统计候选，第二阶段复用同一 union 的 victim 带回受 RCU
 * 保护的竞争对象；dispose 保存当前线程已领取、最终交给 shrink_dentry_list()
 * 的私有链。两个 union 成员不得在同一阶段同时解释。
 */
struct select_data {
	struct dentry *start;
	union {
		long found;
		struct dentry *victim;
	};
	struct list_head dispose;
};

/*
 * select_data 是子树回收遍历的栈上状态：start 防止回收根自身；found 记录候选
 * 数，victim 与其复用以在第二遍带回一个竞争对象；dispose 是当前线程取得处置
 * 权的私有链。union 两成员只在不同阶段使用。
 */

/*
 * select_collect() 在 d_walk 持 d_lock 时领取零引用后代。至少取得一项后即可
 * 因 need_resched 退出，保证大树扫描既有前进也不会长期霸占 CPU；返回
 * NORETRY 表示已经有不可重复的领取副作用，rename 冲突时不应整轮重做。
 */
static enum d_walk_ret select_collect(void *_data, struct dentry *dentry)
{
	struct select_data *data = _data;
	enum d_walk_ret ret = D_WALK_CONTINUE;

	if (data->start == dentry)
		goto out;

	if (dentry->d_lockref.count <= 0) {
		__move_to_shrink_list(dentry, &data->dispose);
		data->found++;
	}
	/*
	 * We can return to the caller if we have found some (this
	 * ensures forward progress). We'll be coming back to find
	 * the rest.
	 */
	/* 一旦 dispose 非空就已有可销毁工作；需要调度时退出，否则禁止 rename 重试后返回。 */
	if (!list_empty(&data->dispose))
		ret = need_resched() ? D_WALK_QUIT : D_WALK_NORETRY;
out:
	return ret;
}

/*
 * select_collect_umount() 在 d_walk 持 @dentry->d_lock 时撤销 PERSISTENT 钉住
 * 引用，再复用 select_collect() 领取零引用项；返回值沿用 d_walk_ret 协议。
 */
static enum d_walk_ret select_collect_umount(void *_data, struct dentry *dentry)
{
	if (dentry->d_flags & DCACHE_PERSISTENT) {
		dentry->d_flags &= ~DCACHE_PERSISTENT;
		dentry->d_lockref.count--;
	}
	return select_collect(_data, dentry);
}

/*
 * select_collect2() 处理第一遍发现却未能领取的零/负引用竞争项。若对象已在
 * shrink list 或正在死亡，它开启显式 RCU 临界区，把裸 victim 交回调用者并
 * 终止遍历；该 RCU 锁故意跨越 d_walk() 返回，必须由 shrink_dcache_tree()
 * 在取得 victim->d_lock 后配对解除。
 */
static enum d_walk_ret select_collect2(void *_data, struct dentry *dentry)
{
	struct select_data *data = _data;
	enum d_walk_ret ret = D_WALK_CONTINUE;

	if (data->start == dentry)
		goto out;

	if (dentry->d_lockref.count <= 0) {
		if (!__move_to_shrink_list(dentry, &data->dispose)) {
			/*
			 * We need an enter RCU read-side critical area that
			 * would extend past the return from d_walk() and
			 * we are in the scope of ->d_lock that will terminate
			 * before that, so we use rcu_read_lock() to bridge
			 * over to the scope of ->d_lock in d_walk() caller.
			 * The scope of rcu_read_lock() spans from here to
			 * paired rcu_read_unlock() in shrink_dcache_tree().
			 */
			/*
			 * d_walk 即将释放 d_lock，却要把裸 victim 带回调用者；显式 RCU 锁跨越
			 * 函数返回，直到 shrink_dcache_tree 取得 victim->d_lock 后才解除，桥接
			 * 两个锁作用域并保证期间内存不被释放。
			 */
			rcu_read_lock();
			data->victim = dentry;
			return D_WALK_QUIT;
		}
	}
	/*
	 * We can return to the caller if we have found some (this
	 * ensures forward progress). We'll be coming back to find
	 * the rest.
	 */
	/* 已隔离一批就可返回处理；后续循环会再找剩余项，兼顾前进性与调度延迟。 */
	if (!list_empty(&data->dispose))
		ret = need_resched() ? D_WALK_QUIT : D_WALK_NORETRY;
out:
	return ret;
}

/**
 * shrink_dcache_tree - prune dcache
 * @parent: parent of entries to prune
 * @for_umount: true if we want to unpin the persistent ones
 *
 * Prune the dcache to remove unused children of the parent dentry.
 */
/* 回收 @parent 的可回收后代；卸载模式还撤销 PERSISTENT，并等待垂死项 unlist。 */
/*
 * shrink_dcache_tree() 持有 @parent 的外部引用，反复遍历直到没有未使用后代。
 * @for_umount 为真时还撤销永久钉住。垂死对象可能仍挂在树上，本函数通过
 * completion 等待其 dentry_unlist 提交点，避免忙等和 UAF。函数可睡眠；返回
 * 只保证当时未发现可回收后代，并不阻止并发重新创建。
 */
static void shrink_dcache_tree(struct dentry *parent, bool for_umount)
{
	/* 每轮先批量领取普通零引用后代，锁外销毁；仍有竞争项才进入第二遍。 */
	for (;;) {
		struct completion_list wait;
		bool need_wait = false;
		struct select_data data = { .start = parent };

		INIT_LIST_HEAD(&data.dispose);
		d_walk(parent, &data,
			for_umount ? select_collect_umount : select_collect);

		if (!list_empty(&data.dispose)) {
			shrink_dentry_list(&data.dispose);
			continue;
		}

		cond_resched();
		if (!data.found)
			break;
		data.victim = NULL;
		d_walk(parent, &data, select_collect2);
		if (data.victim) {
			struct dentry *v = data.victim;
			/*
			 * select_collect2() has picked a dentry that was
			 * either dying or on a shrink list and arranged
			 * for it to be returned to us.  We are still in
			 * the RCU read-side critical area started there
			 * (rcu_read_lock() scope opened in select_collect2()),
			 * so dentry couldn't have been freed yet, but its
			 * state might've changed since we dropped ->d_lock
			 * on the way out.  Switch over to ->d_lock scope
			 * and recheck the dentry state.
			 */
			/*
			 * victim 是 dying 或已被别的 shrink list 领取的对象；select_collect2 开启的
			 * RCU 区间保证内存仍在，但放掉 d_lock 后状态可变。这里先取得 d_lock，再
			 * 解除 RCU 并复核，实现从生命周期保护到字段互斥的安全接力。
			 */
			spin_lock(&v->d_lock);
			rcu_read_unlock();

			if (unlikely(v->d_lockref.count < 0)) {
				// It's doomed; if it isn't dead yet, notify us
				// once it becomes invisible to d_walk().
				/* count 已负表示别的线程取得死亡权；若尚未 unlist，登记完成量等待提交。 */
				need_wait = d_add_waiter(v, &wait);
				spin_unlock(&v->d_lock);
			} else {
				/* 非负计数表示对象尚可由本线程尝试 kill；helper 会再核对忙状态。 */
				shrink_kill(v);
			}
		}
		shrink_dentry_list(&data.dispose);
		if (unlikely(need_wait))
			wait_for_completion(&wait.completion);
	}
}

/* shrink_dcache_parent() 是保留根自身的公开包装，回收其全部当前可回收后代。 */
void shrink_dcache_parent(struct dentry *parent)
{
	shrink_dcache_tree(parent, false);
}

EXPORT_SYMBOL(shrink_dcache_parent);

/* umount_check() 在 d_lock 下诊断卸载后仍忙的叶子，返回 CONTINUE 继续全树检查。 */
/*
 * umount_check() 是卸载后的诊断回调：有子项的节点把问题留给叶子报告；根仅有
 * 卸载者那一份引用合法；其他忙叶子触发 WARN，但继续遍历收集全部异常。
 */
static enum d_walk_ret umount_check(void *_data, struct dentry *dentry)
{
	/* it has busy descendents; complain about those instead */
	/* 非叶子的问题会在更具体的忙后代处报告，避免对同一引用链重复告警。 */
	if (!hlist_empty(&dentry->d_children))
		return D_WALK_CONTINUE;

	/* root with refcount 1 is fine */
	/* 根由当前卸载路径持有的唯一引用是预期状态，不构成泄漏。 */
	if (dentry == _data && dentry->d_lockref.count == 1)
		return D_WALK_CONTINUE;

	WARN(1, "BUG: Dentry %p{i=%llx,n=%pd} "
			" still in use (%d) [unmount of %s %s]\n",
		       dentry,
		       dentry->d_inode ?
		       dentry->d_inode->i_ino : (u64)0,
		       dentry,
		       dentry->d_lockref.count,
		       dentry->d_sb->s_type->name,
		       dentry->d_sb->s_id);
	return D_WALK_CONTINUE;
}

/*
 * do_one_tree() 消耗调用者对某个根的一份引用：撤销持久项并回收子树、诊断忙
 * 引用、取消根的哈希可见性，最后 dput 根。返回后不得再使用该引用。
 */
static void do_one_tree(struct dentry *dentry)
{
	shrink_dcache_tree(dentry, true);
	d_walk(dentry, dentry, umount_check);
	d_drop(dentry);
	dput(dentry);
}

/*
 * destroy the dentries attached to a superblock on unmounting
 */
/* 要求写持 s_umount；消耗主根和所有次级根，返回时 superblock 不再拥有 dentry 树。 */
/*
 * shrink_dcache_for_umount() 要求写持有 sb->s_umount，因此不会再有正常路径建立
 * 新树。它先清空并处理主 s_root，再逐个处理 s_roots 中的次级根；遇到已经
 * 标死但尚未 unlist 的根就登记 completion 等待，避免与淘汰线程重复释放。
 * 返回时 superblock 不再拥有任何 dentry 根；函数可睡眠。
 */
void shrink_dcache_for_umount(struct super_block *sb)
{
	struct dentry *dentry;

	rwsem_assert_held_write(&sb->s_umount);

	dentry = sb->s_root;
	sb->s_root = NULL;
	do_one_tree(dentry);

	for (;;) {
		spin_lock(&sb->s_roots_lock);
		dentry = hlist_entry_safe(sb->s_roots.first,
					  struct dentry, d_sib);
		if (!dentry) {
			spin_unlock(&sb->s_roots_lock);
			break;
		}
		rcu_read_lock();
		spin_unlock(&sb->s_roots_lock);
		spin_lock(&dentry->d_lock);
		rcu_read_unlock();
		if (unlikely(dentry->d_lockref.count < 0)) {
			struct completion_list wait;
			bool need_wait = d_add_waiter(dentry, &wait);

			spin_unlock(&dentry->d_lock);
			if (need_wait)
				wait_for_completion(&wait.completion);
		} else {
			dget_dlock(dentry);
			spin_unlock(&dentry->d_lock);
			do_one_tree(dentry);
		}
	}
}

/*
 * find_submount() 是 d_walk() 的早停回调。@_data 指向调用者的 dentry * 输出槽，
 * @dentry 是遍历器在已持 d_lock 条件下借用的当前节点。非挂载点返回
 * D_WALK_CONTINUE；首个挂载点用 dget_dlock() 取得需 dput() 的引用、写入输出槽，
 * 再返回 D_WALK_QUIT。函数只在自旋锁上下文运行，不睡眠；输出只在 QUIT 时有效。
 */
static enum d_walk_ret find_submount(void *_data, struct dentry *dentry)
{
	struct dentry **victim = _data;
	if (d_mountpoint(dentry)) {
		*victim = dget_dlock(dentry);
		return D_WALK_QUIT;
	}
	return D_WALK_CONTINUE;
}

/**
 * d_invalidate - detach submounts, prune dcache, and drop
 * @dentry: dentry to invalidate (aka detach, prune and drop)
 */
/* 取消 @dentry 哈希可见性，回收后代并拆除子挂载；输入引用仍归调用者。 */
/*
 * d_invalidate() 先把 @dentry 从名字哈希摘除，使新查找不能进入该子树；负项到此
 * 即完成。正项先回收普通后代，再反复寻找挂载点、detach_mounts() 并释放临时
 * 引用，最后若拆过挂载再清一次新暴露的后代。@dentry 本身的输入引用仍归
 * 调用者；函数可睡眠，返回后它 unhashed，但不保证已变负或被释放。
 */
void d_invalidate(struct dentry *dentry)
{
	bool had_submounts = false;
	spin_lock(&dentry->d_lock);
	if (d_unhashed(dentry)) {
		spin_unlock(&dentry->d_lock);
		return;
	}
	__d_drop(dentry);
	spin_unlock(&dentry->d_lock);

	/* Negative dentries can be dropped without further checks */
	/* 负项不绑定 inode，也不可能承载需要先清理的正目录子树，unhash 后即可返回。 */
	if (!dentry->d_inode)
		return;

	shrink_dcache_parent(dentry);
	for (;;) {
		struct dentry *victim = NULL;
		d_walk(dentry, &victim, find_submount);
		if (!victim) {
			if (had_submounts)
				shrink_dcache_parent(dentry);
			return;
		}
		had_submounts = true;
		detach_mounts(victim);
		dput(victim);
	}
}

EXPORT_SYMBOL(d_invalidate);

/**
 * __d_alloc - allocate a dcache entry
 * @sb: filesystem it will belong to
 * @name: qstr of the name
 *
 * Allocates a dentry. It returns %NULL if there is insufficient memory
 * available. On a success the dentry is returned. The name passed in is
 * copied and the copy passed in may be reused after this call.
 */
 
/* 在借用 @sb 上复制 @name 并构造 unhashed 负项；返回引用或 NULL，可睡眠。 */
/*
 * __d_alloc() 只构造独立、unhashed、负、IS_ROOT 形态的 dentry。@sb 是借用且
 * dentry 存活期间必须存活；@name 为借用并被完整复制，NULL 选择静态 slash_name。
 * 名字短则内嵌、长则外置引用计数存储；文件系统 d_init 可在发布前初始化私有
 * 状态。成功返回一份 lockref=1 的持有引用，失败返回 NULL 且已回滚全部分配。
 * 函数使用 GFP_KERNEL，允许睡眠，但返回对象尚不可被普通 lookup 发现。
 */
static struct dentry *__d_alloc(struct super_block *sb, const struct qstr *name)
{
	struct dentry *dentry;
	char *dname;
	int err;

	dentry = kmem_cache_alloc_lru(dentry_cache, &sb->s_dentry_lru,
				      GFP_KERNEL);
	/* slab 分配可睡眠，并把对象与该 superblock 的可回收 LRU 关联。 */
	if (!dentry)
		return NULL;

	/*
	 * We guarantee that the inline name is always NUL-terminated.
	 * This way the memcpy() done by the name switching in rename
	 * will still always have a NUL at the end, even if we might
	 * be overwriting an internal NUL character
	 */
	/*
	 * 内联缓冲最后一字节预先置 NUL；rename 的整块名字复制即使覆盖名字内部的
	 * NUL，也始终保留末尾终止符，使无锁比较不会越界。
	 */
	dentry->d_shortname.string[DNAME_INLINE_LEN-1] = 0;
	if (unlikely(!name)) {
		name = &slash_name;
		dname = dentry->d_shortname.string;
	} else if (name->len > DNAME_INLINE_LEN-1) {
		struct external_name *p;

		p = kmalloc_flex(*p, name, name->len + 1,
				 GFP_KERNEL_ACCOUNT | __GFP_RECLAIMABLE);
		if (!p) {
			kmem_cache_free(dentry_cache, dentry); 
			return NULL;
		}
		atomic_set(&p->count, 1);
		/* 初始引用属于 dentry；后续 name_snapshot 可独立增加该计数。 */
		dname = p->name;
	} else  {
		dname = dentry->d_shortname.string;
	}	

	dentry->__d_name.len = name->len;
	dentry->__d_name.hash = name->hash;
	memcpy(dname, name->name, name->len);
	dname[name->len] = 0;

	/* Make sure we always see the terminating NUL character */
	/* release store 发布名字指针，与无锁比较的 acquire/seqcount 顺序共同保证 NUL 可见。 */
	smp_store_release(&dentry->__d_name.name, dname); /* ^^^ */
	/* release 发布保证无锁按字比较一旦见到指针，就能见到结尾 NUL 与名字内容。 */

	dentry->d_flags = 0;
	lockref_init(&dentry->d_lockref);
	seqcount_spinlock_init(&dentry->d_seq, &dentry->d_lock);
	dentry->d_inode = NULL;
	dentry->d_parent = dentry;
	dentry->d_sb = sb;
	dentry->d_op = sb->__s_d_op;
	dentry->d_flags = sb->s_d_flags;
	dentry->d_fsdata = NULL;
	INIT_HLIST_BL_NODE(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_lru);
	INIT_HLIST_HEAD(&dentry->d_children);
	dentry->waiters = NULL;
	INIT_HLIST_NODE(&dentry->d_sib);

	if (dentry->d_op && dentry->d_op->d_init) {
		err = dentry->d_op->d_init(dentry);
		if (err) {
			/* 尚未挂树、哈希或计数，可按分配逆序同步回滚，无需 RCU。 */
			if (dname_external(dentry))
				kfree(external_name(dentry));
			kmem_cache_free(dentry_cache, dentry);
			return NULL;
		}
	}

	this_cpu_inc(nr_dentry);

	return dentry;
}

/**
 * d_alloc - allocate a dcache entry
 * @parent: parent of entry to allocate
 * @name: qstr of the name
 *
 * Allocates a dentry. It returns %NULL if there is insufficient memory
 * available. On a success the dentry is returned. The name passed in is
 * copied and the copy passed in may be reused after this call.
 */
/* 分配负项并挂入借用 @parent 的 children；尚不 rehash，返回引用或 NULL。 */
/*
 * d_alloc() 在 __d_alloc() 后取得 parent->d_lock，把新对象挂入 d_children 并
 * 为 d_parent 持有一份 parent 引用。新对象尚未 rehash/instantiate，因此无需
 * child 锁且外部仍不可按名字发现。成功返回调用者持有引用，失败 NULL；name
 * 已复制，调用者可立即复用。
 */
struct dentry *d_alloc(struct dentry * parent, const struct qstr *name)
{
	struct dentry *dentry = __d_alloc(parent->d_sb, name);
	if (!dentry)
		return NULL;
	spin_lock(&parent->d_lock);
	/*
	 * don't need child lock because it is not subject
	 * to concurrency here
	 */
	/* 新 child 尚未发布给任何查找者，只有当前线程持有，修改其 parent 无需 d_lock。 */
	dentry->d_parent = dget_dlock(parent);
	hlist_add_head(&dentry->d_sib, &parent->d_children);
	spin_unlock(&parent->d_lock);

	return dentry;
}

EXPORT_SYMBOL(d_alloc);

/* d_alloc_anon() 在借用 @sb 上分配 slash-name、unhashed 根形对象，返回引用或 NULL。 */
/* d_alloc_anon() 产生未挂父树的 slash-name 根形对象，返回持有引用或 NULL。 */
struct dentry *d_alloc_anon(struct super_block *sb)
{
	return __d_alloc(sb, NULL);
}

EXPORT_SYMBOL(d_alloc_anon);

/* d_alloc_cursor() 为借用 @parent 创建 NORCU 目录游标，成功返回必须 dput 的引用。 */
/*
 * d_alloc_cursor() 为 readdir/lseek 建立不参与名字查找的游标节点，标记 CURSOR
 * 与 NORCU，并单独持有 parent 引用。它不会加入 parent->d_children；返回对象
 * 只能按游标协议移动，最后由调用者 dput()。
 */
struct dentry *d_alloc_cursor(struct dentry * parent)
{
	struct dentry *dentry = d_alloc_anon(parent->d_sb);
	if (dentry) {
		dentry->d_flags |= DCACHE_DENTRY_CURSOR | DCACHE_NORCU;
		dentry->d_parent = dget(parent);
	}
	return dentry;
}

/**
 * d_alloc_pseudo - allocate a dentry (for lookup-less filesystems)
 * @sb: the superblock
 * @name: qstr of the name
 *
 * For a filesystem that just pins its dentries in memory and never
 * performs lookups at all, return an unhashed IS_ROOT dentry.
 * This is used for pipes, sockets et.al. - the stuff that should
 * never be anyone's children or parents.  Unlike all other
 * dentries, these will not have RCU delay between dropping the
 * last reference and freeing them.
 *
 * The only user is alloc_file_pseudo() and that's what should
 * be considered a public interface.  Don't use directly.
 */
/* 为无 lookup 的伪文件分配 unhashed NORCU 根项；返回引用或 NULL。 */
/*
 * d_alloc_pseudo() 服务 pipe/socket 等无 lookup 伪文件：返回 unhashed IS_ROOT、
 * NORCU dentry，并在文件系统未提供 d_op 时安装 simple_dname。成功为持有引用，
 * 失败 NULL；因从不发布给 RCU，最后 dput 可同步释放。公开调用边界是
 * alloc_file_pseudo()，普通文件系统不应直接使用。
 */
struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name)
{
	static const struct dentry_operations anon_ops = {
		.d_dname = simple_dname
	};
	struct dentry *dentry = __d_alloc(sb, name);
	if (likely(dentry)) {
		dentry->d_flags |= DCACHE_NORCU;
		/* d_op_flags(&anon_ops) is 0 */
		/* anon_ops 只有 d_dname，不对应任何 DCACHE_OP_* 热路径位，因此无需更新 flags。 */
		if (!dentry->d_op)
			dentry->d_op = &anon_ops;
	}
	return dentry;
}

/* d_alloc_name() 从 NUL 结尾字符串计算父相关 hash_len，再复用 d_alloc()。 */
struct dentry *d_alloc_name(struct dentry *parent, const char *name)
{
	struct qstr q;

	q.name = name;
	q.hash_len = hashlen_string(parent, name);
	return d_alloc(parent, &q);
}

EXPORT_SYMBOL(d_alloc_name);

#define DCACHE_OP_FLAGS \
	(DCACHE_OP_HASH | DCACHE_OP_COMPARE | DCACHE_OP_REVALIDATE | \
	 DCACHE_OP_WEAK_REVALIDATE | DCACHE_OP_DELETE | DCACHE_OP_PRUNE | \
	 DCACHE_OP_REAL)

/* d_op_flags() 把借用操作表的非 NULL 回调编码成快速测试位，NULL 返回 0。 */
/*
 * d_op_flags() 把 dentry_operations 中非 NULL 回调预编译成 DCACHE_OP_* 快速位。
 * @op 为借用且可 NULL；返回位图，无副作用、不睡眠。热路径先测 flags，避免
 * 反复读取操作表和间接分支；位图必须与 d_op 同时设置并保持一致。
 */
static unsigned int d_op_flags(const struct dentry_operations *op)
{
	unsigned int flags = 0;
	if (op) {
		if (op->d_hash)
			flags |= DCACHE_OP_HASH;
		if (op->d_compare)
			flags |= DCACHE_OP_COMPARE;
		if (op->d_revalidate)
			flags |= DCACHE_OP_REVALIDATE;
		if (op->d_weak_revalidate)
			flags |= DCACHE_OP_WEAK_REVALIDATE;
		if (op->d_delete)
			flags |= DCACHE_OP_DELETE;
		if (op->d_prune)
			flags |= DCACHE_OP_PRUNE;
		if (op->d_real)
			flags |= DCACHE_OP_REAL;
	}
	return flags;
}

/*
 * d_set_d_op() 只允许给尚无操作表的 dentry 安装一次 @op，并同步缓存回调位；
 * WARN 检测重复安装或旧 flags。调用者负责 dentry 尚未并发发布，函数不取锁。
 */
static void d_set_d_op(struct dentry *dentry, const struct dentry_operations *op)
{
	unsigned int flags = d_op_flags(op);
	WARN_ON_ONCE(dentry->d_op);
	WARN_ON_ONCE(dentry->d_flags & DCACHE_OP_FLAGS);
	dentry->d_op = op;
	if (flags)
		dentry->d_flags |= flags;
}

/*
 * set_default_d_op() 在 superblock 初始化期设置以后新 dentry 继承的默认操作表
 * 和 flags。@ops 生命周期至少覆盖 superblock；已分配 dentry 不被追溯修改。
 */
void set_default_d_op(struct super_block *s, const struct dentry_operations *ops)
{
	unsigned int flags = d_op_flags(ops);
	s->__s_d_op = ops;
	s->s_d_flags = (s->s_d_flags & ~DCACHE_OP_FLAGS) | flags;
}

EXPORT_SYMBOL(set_default_d_op);

/* d_flags_for_inode() 从借用 @inode 推导 dentry 类型/automount 位；NULL 表示负项。 */
/*
 * d_flags_for_inode() 从 @inode 的 mode、i_op 与 automount 状态生成 dentry 类型
 * 快速位；NULL 对应负项 MISS。它还把 lookup/get_link 是否缺失缓存到 inode
 * i_opflags，减少后续间接检查。@inode 为借用；返回位图，无引用转移、不睡眠。
 */
static unsigned d_flags_for_inode(struct inode *inode)
{
	unsigned add_flags = DCACHE_REGULAR_TYPE;

	if (!inode)
		return DCACHE_MISS_TYPE;

	if (S_ISDIR(inode->i_mode)) {
		add_flags = DCACHE_DIRECTORY_TYPE;
		if (unlikely(!(inode->i_opflags & IOP_LOOKUP))) {
			if (unlikely(!inode->i_op->lookup))
				add_flags = DCACHE_AUTODIR_TYPE;
			else
				inode->i_opflags |= IOP_LOOKUP;
		}
		goto type_determined;
	}

	if (unlikely(!(inode->i_opflags & IOP_NOFOLLOW))) {
		if (unlikely(inode->i_op->get_link)) {
			add_flags = DCACHE_SYMLINK_TYPE;
			goto type_determined;
		}
		inode->i_opflags |= IOP_NOFOLLOW;
	}

	if (unlikely(!S_ISREG(inode->i_mode)))
		add_flags = DCACHE_SPECIAL_TYPE;

type_determined:
	if (unlikely(IS_AUTOMOUNT(inode)))
		add_flags |= DCACHE_NEED_AUTOMOUNT;
	return add_flags;
}

/*
 * __d_instantiate() 要求 inode->i_lock 和 dentry->d_lock，把负 dentry 加入
 * inode->i_dentry alias 链，再在 d_seq 写区间内 release 发布 d_inode 与类型。
 * @inode 的现有引用转交给 dcache；若对象在公共 LRU，负项统计随之减少。函数
 * 不解锁、不失败，最后刷新 fsnotify 派生 flags。
 */
static void __d_instantiate(struct dentry *dentry, struct inode *inode)
{
	unsigned add_flags = d_flags_for_inode(inode);
	WARN_ON(d_in_lookup(dentry));

	/*
	 * The negative counter only tracks dentries on the LRU. Don't dec if
	 * d_lru is on another list.
	 */
	/* 只有公共 LRU 负项计入 negative；私有 shrink 链上的状态转换不触碰该统计。 */
	if ((dentry->d_flags &
	     (DCACHE_LRU_LIST|DCACHE_SHRINK_LIST)) == DCACHE_LRU_LIST)
		this_cpu_dec(nr_dentry_negative);
	hlist_add_head(&dentry->d_alias, &inode->i_dentry);
	raw_write_seqcount_begin(&dentry->d_seq);
	__d_set_inode_and_type(dentry, inode, add_flags);
	raw_write_seqcount_end(&dentry->d_seq);
	fsnotify_update_flags(dentry);
}

/**
 * d_instantiate - fill in inode information for a dentry
 * @entry: dentry to complete
 * @inode: inode to attach to this dentry
 *
 * Fill in inode information in the entry.
 *
 * This turns negative dentries into productive full members
 * of society.
 *
 * NOTE! This assumes that the inode count has been incremented
 * (or otherwise set) by the caller to indicate that it is now
 * in use by the dcache.
 */
 
/* 消耗可选 @inode 引用并把负 @entry 变正；不 rehash，无直接返回值。 */
/*
 * d_instantiate() 把已分配的负 @entry 与 @inode 绑定，但不负责 rehash。inode
 * 可 NULL（保持负项）；非 NULL 时先过 LSM，再按 i_lock -> d_lock 发布。调用者
 * 必须预先持有准备转移给 dcache 的 inode 引用。无返回，成功后 entry 为正项。
 */
void d_instantiate(struct dentry *entry, struct inode * inode)
{
	BUG_ON(d_really_is_positive(entry));
	if (inode) {
		security_d_instantiate(entry, inode);
		spin_lock(&inode->i_lock);
		spin_lock(&entry->d_lock);
		__d_instantiate(entry, inode);
		spin_unlock(&entry->d_lock);
		spin_unlock(&inode->i_lock);
	}
}

EXPORT_SYMBOL(d_instantiate);

/*
 * This should be equivalent to d_instantiate() + unlock_new_inode(),
 * with lockdep-related part of unlock_new_inode() done before
 * anything else.  Use that instead of open-coding d_instantiate()/
 * unlock_new_inode() combinations.
 */
/* 绑定 I_NEW @inode 后以屏障发布初始化完成并唤醒等待者；消耗 inode 引用。 */
/*
 * d_instantiate_new() 将 d_instantiate 与 unlock_new_inode 合成一个不可错序操作。
 * @inode 必须带 I_NEW/I_CREATING 且引用转交 dcache；先绑定 dentry，再用 smp_wmb
 * 与 igrab_from_hash() 配对，保证观察到 I_NEW 清除的线程已看到完整 inode，最后
 * 唤醒等待 __I_NEW 的线程。无返回，函数不负责 rehash entry。
 */
void d_instantiate_new(struct dentry *entry, struct inode *inode)
{
	BUG_ON(d_really_is_positive(entry));
	BUG_ON(!inode);
	lockdep_annotate_inode_mutex_key(inode);
	security_d_instantiate(entry, inode);
	spin_lock(&inode->i_lock);
	spin_lock(&entry->d_lock);
	__d_instantiate(entry, inode);
	spin_unlock(&entry->d_lock);
	WARN_ON(!(inode_state_read(inode) & I_NEW));
	/*
	 * Paired with igrab_from_hash()
	 */
	/*
	 * 写屏障先发布 inode/dentry 的全部初始化，再清 I_NEW；与 igrab_from_hash 的
	 * 读取顺序配对，防止等待者看到“已就绪”却读到半初始化字段。
	 */
	smp_wmb();
	inode_state_clear(inode, I_NEW | I_CREATING);
	inode_wake_up_bit(inode, __I_NEW);
	spin_unlock(&inode->i_lock);
}

EXPORT_SYMBOL(d_instantiate_new);

/* d_make_root() 消耗 @root_inode 引用，返回绑定它的匿名根引用或 NULL。 */
/*
 * d_make_root() 消耗 @root_inode 的一份引用：成功分配匿名根并绑定后返回 dentry
 * 持有引用；分配失败自动 iput inode 并返回 NULL；输入 NULL 也返回 NULL。调用者
 * 无论成功失败都不再拥有传入 inode 引用。
 */
struct dentry *d_make_root(struct inode *root_inode)
{
	struct dentry *res = NULL;

	if (root_inode) {
		res = d_alloc_anon(root_inode->i_sb);
		if (res)
			d_instantiate(res, root_inode);
		else
			iput(root_inode);
	}
	return res;
}

EXPORT_SYMBOL(d_make_root);

/*
 * __d_obtain_alias() 消耗 @inode 引用，返回已有/新建 alias 的持有引用或 ERR_PTR；
 * @disconnected 选择 DISCONNECTED 与登记 s_roots 两种匿名根语义。
 */
/*
 * __d_obtain_alias() 消耗 @inode 引用，为 open-by-handle/exportfs 获得可表示该
 * inode 的 dentry。NULL 转 -ESTALE，错误指针原样传播；已有 alias 直接增引并
 * 释放输入 inode 引用。否则分配匿名根，在 i_lock 下二次检查后发布；
 * @disconnected 决定置 DISCONNECTED 还是加入 sb->s_roots。成功返回持有 dentry
 * 引用且 inode 引用已转入该 dentry；失败返回 ERR_PTR 且输入引用已释放。
 */
static struct dentry *__d_obtain_alias(struct inode *inode, bool disconnected)
{
	struct super_block *sb;
	struct dentry *new, *res;

	if (!inode)
		return ERR_PTR(-ESTALE);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	sb = inode->i_sb;

	res = d_find_any_alias(inode); /* existing alias? */
	/* 先无条件复用已有 alias，维持目录 inode 单 alias 不变量。 */
	if (res)
		goto out;

	new = d_alloc_anon(sb);
	if (!new) {
		res = ERR_PTR(-ENOMEM);
		goto out;
	}

	security_d_instantiate(new, inode);
	spin_lock(&inode->i_lock);
	res = __d_find_any_alias(inode); /* recheck under lock */
	/* 分配期间可能有竞争者发布 alias，必须在 i_lock 下二次检查。 */
	if (likely(!res)) { /* still no alias, attach a disconnected dentry */
		/* 锁下二次确认仍无 alias 后，当前线程才取得把 new 绑定给 inode 的发布权。 */
		unsigned add_flags = d_flags_for_inode(inode);

		if (disconnected)
			add_flags |= DCACHE_DISCONNECTED;

		spin_lock(&new->d_lock);
		__d_set_inode_and_type(new, inode, add_flags);
		hlist_add_head(&new->d_alias, &inode->i_dentry);
		if (!disconnected) {
			spin_lock(&sb->s_roots_lock);
			hlist_add_head(&new->d_sib, &sb->s_roots);
			spin_unlock(&sb->s_roots_lock);
		}
		spin_unlock(&new->d_lock);
		spin_unlock(&inode->i_lock);
		inode = NULL; /* consumed by new->d_inode */
		/* 置 NULL 防止统一 out 路径再次 iput；该引用现在由 new->d_inode 持有。 */
		res = new;
	} else {
		spin_unlock(&inode->i_lock);
		dput(new);
	}

 out:
	iput(inode);
	return res;
}

/**
 * d_obtain_alias - find or allocate a DISCONNECTED dentry for a given inode
 * @inode: inode to allocate the dentry for
 *
 * Obtain a dentry for an inode resulting from NFS filehandle conversion or
 * similar open by handle operations.  The returned dentry may be anonymous,
 * or may have a full name (if the inode was already in the cache).
 *
 * When called on a directory inode, we must ensure that the inode only ever
 * has one dentry.  If a dentry is found, that is returned instead of
 * allocating a new one.
 *
 * On successful return, the reference to the inode has been transferred
 * to the dentry.  In case of an error the reference on the inode is released.
 * To make it easier to use in export operations a %NULL or IS_ERR inode may
 * be passed in and the error will be propagated to the return value,
 * with a %NULL @inode replaced by ERR_PTR(-ESTALE).
 */
/* 消耗 @inode 引用，返回 existing/new DISCONNECTED alias 引用或 ERR_PTR。 */
/* d_obtain_alias() 选择 DISCONNECTED，允许随后由 d_splice_alias() 接入命名树。 */
struct dentry *d_obtain_alias(struct inode *inode)
{
	return __d_obtain_alias(inode, true);
}

EXPORT_SYMBOL(d_obtain_alias);

/**
 * d_obtain_root - find or allocate a dentry for a given inode
 * @inode: inode to allocate the dentry for
 *
 * Obtain an IS_ROOT dentry for the root of a filesystem.
 *
 * We must ensure that directory inodes only ever have one dentry.  If a
 * dentry is found, that is returned instead of allocating a new one.
 *
 * On successful return, the reference to the inode has been transferred
 * to the dentry.  In case of an error the reference on the inode is
 * released.  A %NULL or IS_ERR inode may be passed in and will be the
 * error will be propagate to the return value, with a %NULL @inode
 * replaced by ERR_PTR(-ESTALE).
 */
/* 消耗 @inode 引用，返回 existing/new 次级根引用或 ERR_PTR。 */
/* d_obtain_root() 创建非 disconnected 次级根，并把新根登记到 sb->s_roots。 */
struct dentry *d_obtain_root(struct inode *inode)
{
	return __d_obtain_alias(inode, false);
}

EXPORT_SYMBOL(d_obtain_root);

/**
 * d_add_ci - lookup or allocate new dentry with case-exact name
 * @dentry: the negative dentry that was passed to the parent's lookup func
 * @inode:  the inode case-insensitive lookup has found
 * @name:   the case-exact name to be associated with the returned dentry
 *
 * This is to avoid filling the dcache with case-insensitive names to the
 * same inode, only the actual correct case is stored in the dcache for
 * case-insensitive filesystems.
 *
 * For a case-insensitive lookup match and if the case-exact dentry
 * already exists in the dcache, use it and return it.
 *
 * If no entry exists with the exact case name, allocate new dentry with
 * the exact case, and return the spliced entry.
 */
/* 为大小写不敏感 lookup 发布精确大小写名字；消耗 @inode 并返回结果引用/错误。 */
/*
 * d_add_ci() 用于大小写不敏感文件系统把实际大小写 @name 存入 dcache。输入
 * @dentry 是 lookup 的负项，@inode 引用被本函数消耗。先查精确名字；不存在时
 * 按原查询是否 parallel 分配精确项，再 d_splice_alias() 维持目录 alias 唯一性。
 * 返回持有 dentry、NULL 语义不使用；可能返回 ERR_PTR。若 splice 返回替代项，
 * 会结束并释放临时 found；成功返回 found 时其 lookup 完成责任按 splice 结果
 * 已处理。
 */
struct dentry *d_add_ci(struct dentry *dentry, struct inode *inode,
			struct qstr *name)
{
	struct dentry *found, *res;

	/*
	 * First check if a dentry matching the name already exists,
	 * if not go ahead and create it now.
	 */
	/* 先按精确大小写复查正式 dcache，避免为同一实际名字制造重复项。 */
	found = d_hash_and_lookup(dentry->d_parent, name);
	if (found) {
		iput(inode);
		return found;
	}
	if (d_in_lookup(dentry)) {
		found = d_alloc_parallel(dentry->d_parent, name);
		if (IS_ERR(found) || !d_in_lookup(found)) {
			iput(inode);
			return found;
		}
	} else {
		found = d_alloc(dentry->d_parent, name);
		if (!found) {
			iput(inode);
			return ERR_PTR(-ENOMEM);
		} 
	}
	res = d_splice_alias(inode, found);
	if (res) {
		d_lookup_done(found);
		dput(found);
		return res;
	}
	return found;
}

EXPORT_SYMBOL(d_add_ci);

/**
 * d_same_name - compare dentry name with case-exact name
 * @dentry: the negative dentry that was passed to the parent's lookup func
 * @parent: parent dentry
 * @name:   the case-exact name to be associated with the returned dentry
 *
 * Return: true if names are same, or false
 */
/* 按默认或父 d_compare 规则比较借用名字；相等 true，不改变引用。 */
/*
 * d_same_name() 在调用者稳定 @dentry 名字/父关系的前提下比较 @name。普通父项
 * 先比长度再走默认字节比较；DCACHE_OP_COMPARE 把大小写、编码等规则交给文件
 * 系统回调。三个指针均为借用，返回 bool，无引用变化。
 */
bool d_same_name(const struct dentry *dentry, const struct dentry *parent,
		 const struct qstr *name)
{
	if (likely(!(parent->d_flags & DCACHE_OP_COMPARE))) {
		if (dentry->d_name.len != name->len)
			return false;
		return dentry_cmp(dentry, name->name, name->len) == 0;
	}
	return parent->d_op->d_compare(dentry,
				       dentry->d_name.len, dentry->d_name.name,
				       name) == 0;
}

EXPORT_SYMBOL_GPL(d_same_name);

/*
 * This is __d_lookup_rcu() when the parent dentry has
 * DCACHE_OP_COMPARE, which makes things much nastier.
 */
/* 自定义 compare 的 RCU 查找慢路；返回裸指针并输出 d_seq，或 NULL。 */
/*
 * __d_lookup_rcu_op_compare() 是自定义 compare 的 RCU 慢分支。它逐候选用 d_seq
 * 取得一致的 (name,len,parent,hash) 快照，再调用文件系统 d_compare；命中时把
 * 序列写入 @seqp 并返回裸 RCU 指针，未命中 NULL。调用者必须持 RCU 并验证
 * 序列；该返回值不带引用，回调必须适合 RCU lookup 上下文且不能睡眠。
 */
static noinline struct dentry *__d_lookup_rcu_op_compare(
	const struct dentry *parent,
	const struct qstr *name,
	unsigned *seqp)
{
	u64 hashlen = name->hash_len;
	struct hlist_bl_head *b = d_hash(hashlen);
	struct hlist_bl_node *node;
	struct dentry *dentry;

	hlist_bl_for_each_entry_rcu(dentry, node, b, d_hash) {
		int tlen;
		const char *tname;
		unsigned seq;

seqretry:
		seq = raw_seqcount_begin(&dentry->d_seq);
		if (dentry->d_parent != parent)
			continue;
		if (d_unhashed(dentry))
			continue;
		if (dentry->d_name.hash != hashlen_hash(hashlen))
			continue;
		tlen = dentry->d_name.len;
		tname = dentry->d_name.name;
		/* we want a consistent (name,len) pair */
		/* 自定义 compare 必须接收同一 d_seq 版本的指针和长度，rename 时循环重取。 */
		if (read_seqcount_retry(&dentry->d_seq, seq)) {
			cpu_relax();
			goto seqretry;
		}
		if (parent->d_op->d_compare(dentry, tlen, tname, name) != 0)
			continue;
		*seqp = seq;
		return dentry;
	}
	return NULL;
}

/**
 * __d_lookup_rcu - search for a dentry (racy, store-free)
 * @parent: parent dentry
 * @name: qstr of name we wish to find
 * @seqp: returns d_seq value at the point where the dentry was found
 * Returns: dentry, or NULL
 *
 * __d_lookup_rcu is the dcache lookup function for rcu-walk name
 * resolution (store-free path walking) design described in
 * Documentation/filesystems/path-lookup.txt.
 *
 * This is not to be used outside core vfs.
 *
 * __d_lookup_rcu must only be used in rcu-walk mode, ie. with vfsmount lock
 * held, and rcu_read_lock held. The returned dentry must not be stored into
 * without taking d_lock and checking d_seq sequence count against @seq
 * returned here.
 *
 * Alternatively, __d_lookup_rcu may be called again to look up the child of
 * the returned dentry, so long as its parent's seqlock is checked after the
 * child is looked up. Thus, an interlocking stepping of sequence lock checks
 * is formed, giving integrity down the path walk.
 *
 * NOTE! The caller *has* to check the resulting dentry against the sequence
 * number we've returned before using any of the resulting dentry state!
 */
/* RCU-walk 无写查找；返回裸 dentry 与序列或 NULL，调用者必须验证 d_seq。 */
/*
 * __d_lookup_rcu() 是 path walk 的无写入快路径。入口要求 RCU-walk、mount 锁与
 * rcu_read_lock；@parent/@name 均借用，@seqp 为输出。它按 hash 桶遍历并用每项
 * d_seq 对抗 d_move；返回的是不增引用的裸指针，任何字段使用前都必须验证
 * *seqp，或按“查 child 后再验证 parent”的交错协议。并发 rename 可造成合法的
 * false negative；需要确定结果的调用者使用 d_lookup()。函数不睡眠。
 */
struct dentry *__d_lookup_rcu(const struct dentry *parent,
				const struct qstr *name,
				unsigned *seqp)
{
	u64 hashlen = name->hash_len;
	const unsigned char *str = name->name;
	struct hlist_bl_head *b = d_hash(hashlen);
	struct hlist_bl_node *node;
	struct dentry *dentry;

	/*
	 * Note: There is significant duplication with __d_lookup_rcu which is
	 * required to prevent single threaded performance regressions
	 * especially on architectures where smp_rmb (in seqcounts) are costly.
	 * Keep the two functions in sync.
	 */
	/*
	 * 此实现与带锁版本刻意重复，避免抽象 helper 给单线程热路径增加屏障和调用
	 * 成本，尤其 seqcount 的 smp_rmb 较贵的架构；修改任一版本必须同步另一份。
	 */

	if (unlikely(parent->d_flags & DCACHE_OP_COMPARE))
		return __d_lookup_rcu_op_compare(parent, name, seqp);

	/*
	 * The hash list is protected using RCU.
	 *
	 * Carefully use d_seq when comparing a candidate dentry, to avoid
	 * races with d_move().
	 *
	 * It is possible that concurrent renames can mess up our list
	 * walk here and result in missing our dentry, resulting in the
	 * false-negative result. d_lookup() protects against concurrent
	 * renames using rename_lock seqlock.
	 *
	 * See Documentation/filesystems/path-lookup.txt for more details.
	 */
	/*
	 * 哈希链由 RCU 保证节点存活，候选 parent/name 则由 d_seq 检测 d_move 竞态。
	 * rename 可能扰乱遍历并造成假未命中；需要确定语义的 d_lookup 另用全局
	 * rename_lock 序列重试。
	 */
	hlist_bl_for_each_entry_rcu(dentry, node, b, d_hash) {
		unsigned seq;

		/*
		 * The dentry sequence count protects us from concurrent
		 * renames, and thus protects parent and name fields.
		 *
		 * The caller must perform a seqcount check in order
		 * to do anything useful with the returned dentry.
		 *
		 * NOTE! We do a "raw" seqcount_begin here. That means that
		 * we don't wait for the sequence count to stabilize if it
		 * is in the middle of a sequence change. If we do the slow
		 * dentry compare, we will do seqretries until it is stable,
		 * and if we end up with a successful lookup, we actually
		 * want to exit RCU lookup anyway.
		 *
		 * Note that raw_seqcount_begin still *does* smp_rmb(), so
		 * we are still guaranteed NUL-termination of ->d_name.name.
		 */
		/*
		 * raw_seqcount_begin 不等待正在写的 rename，保持 miss 快速；慢 compare 会自行
		 * 等到稳定。它仍包含读取屏障，确保由 release store 发布的 name 与 NUL 已可见。
		 * 返回候选后调用者必须用该 seq 验证，不能把裸指针直接保存。
		 */
		seq = raw_seqcount_begin(&dentry->d_seq);
		/* raw begin 不等待 rename 完成；失败命中可便宜跳过，真命中由调用者验证。 */
		if (dentry->d_parent != parent)
			continue;
		if (dentry->d_name.hash_len != hashlen)
			continue;
		if (unlikely(dentry_cmp(dentry, str, hashlen_len(hashlen)) != 0))
			continue;
		/*
		 * Check for the dentry being unhashed.
		 *
		 * As tempting as it is, we *can't* skip it because of a race window
		 * between us finding the dentry before it gets unhashed and loading
		 * the sequence counter after unhashing is finished.
		 *
		 * We can at least predict on it.
		 */
		/*
		 * 不能省略 unhashed 检查：读者可能先取得节点、摘链者随后完成并更新序列、
		 * 读者再读到新序列；若只验证序列会把已不可查找项误当命中。
		 */
		if (unlikely(d_unhashed(dentry)))
			continue;
		*seqp = seq;
		return dentry;
	}
	return NULL;
}

/**
 * d_lookup - search for a dentry
 * @parent: parent dentry
 * @name: qstr of name we wish to find
 * Returns: dentry, or NULL
 *
 * d_lookup searches the children of the parent dentry for the name in
 * question. If the dentry is found its reference count is incremented and the
 * dentry is returned. The caller must use dput to free the entry when it has
 * finished using it. %NULL is returned if the dentry does not exist.
 */
/* 在 rename 序列重试下确定查找 @parent/@name；返回持有引用或 NULL。 */
/*
 * d_lookup() 在 rename_lock 序列保护下调用 __d_lookup()，未命中且有并发 rename
 * 就重试，因此消除其 false negative。命中返回一份必须 dput 的引用，未命中
 * NULL；@parent/@name 为借用，函数不睡眠。
 */
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name)
{
	struct dentry *dentry;
	unsigned seq;

	do {
		seq = read_seqbegin(&rename_lock);
		dentry = __d_lookup(parent, name);
		if (dentry)
			break;
	} while (read_seqretry(&rename_lock, seq));
	return dentry;
}

EXPORT_SYMBOL(d_lookup);

/**
 * __d_lookup - search for a dentry (racy)
 * @parent: parent dentry
 * @name: qstr of name we wish to find
 * Returns: dentry, or NULL
 *
 * __d_lookup is like d_lookup, however it may (rarely) return a
 * false-negative result due to unrelated rename activity.
 *
 * __d_lookup is slightly faster by avoiding rename_lock read seqlock,
 * however it must be used carefully, eg. with a following d_lookup in
 * the case of failure.
 *
 * __d_lookup callers must be commented.
 */
/* 允许并发 rename 假阴性的带引用查找；命中返回须 dput 的引用，否则 NULL。 */
/*
 * __d_lookup() 是带引用的、但允许 rename false negative 的查找。它在 RCU 下
 * 遍历桶，对 hash 候选取 d_lock 后核对 parent/unhashed/name 并直接增 lockref。
 * 命中返回持有引用，NULL 可能只是并发 rename 造成，调用者必须能容忍或随后
 * 用 d_lookup() 确认。与 __d_lookup_rcu 的代码刻意重复以保护单线程热路径。
 */
struct dentry *__d_lookup(const struct dentry *parent, const struct qstr *name)
{
	unsigned int hash = name->hash;
	struct hlist_bl_head *b = d_hash(hash);
	struct hlist_bl_node *node;
	struct dentry *found = NULL;
	struct dentry *dentry;

	/*
	 * Note: There is significant duplication with __d_lookup_rcu which is
	 * required to prevent single threaded performance regressions
	 * especially on architectures where smp_rmb (in seqcounts) are costly.
	 * Keep the two functions in sync.
	 */
	/* 带锁版与 RCU 版刻意重复以避免热路径抽象成本；修改比较条件时必须同步两者。 */

	/*
	 * The hash list is protected using RCU.
	 *
	 * Take d_lock when comparing a candidate dentry, to avoid races
	 * with d_move().
	 *
	 * It is possible that concurrent renames can mess up our list
	 * walk here and result in missing our dentry, resulting in the
	 * false-negative result. d_lookup() protects against concurrent
	 * renames using rename_lock seqlock.
	 *
	 * See Documentation/filesystems/path-lookup.txt for more details.
	 */
	/*
	 * RCU 保护桶节点内存，候选比较期间再取 d_lock 稳定 parent/name 与 unhashed。
	 * 未持 rename_lock 仍可能因链重排假未命中；需要确定结果时由 d_lookup 重试。
	 */
	rcu_read_lock();
	/* RCU 保证桶中被摘除的 dentry 内存仍在；候选字段一致性由 d_lock 保证。 */
	
	hlist_bl_for_each_entry_rcu(dentry, node, b, d_hash) {

		if (dentry->d_name.hash != hash)
			continue;

		spin_lock(&dentry->d_lock);
		if (dentry->d_parent != parent)
			goto next;
		if (d_unhashed(dentry))
			goto next;

		if (!d_same_name(dentry, parent, name))
			goto next;

		dentry->d_lockref.count++;
		/* d_lock 下从零增引用也安全，可把公共 LRU 对象重新变为活跃。 */
		found = dentry;
		spin_unlock(&dentry->d_lock);
		break;
next:
		spin_unlock(&dentry->d_lock);
 	}
 	rcu_read_unlock();

 	return found;
}

/**
 * d_hash_and_lookup - hash the qstr then search for a dentry
 * @dir: Directory to search in
 * @name: qstr of name we wish to find
 *
 * On lookup failure NULL is returned; on bad name - ERR_PTR(-error)
 */
/* 计算并可由文件系统改写 @name hash，再查 @dir；返回引用、NULL 或 ERR_PTR。 */
/*
 * d_hash_and_lookup() 先原地填写输入输出 @name->hash，再允许父目录 d_hash 回调
 * 规范化/拒绝名字，最后确定性 d_lookup。返回持有引用、NULL 或 ERR_PTR(回调
 * errno)；@dir 借用，@name 内容由调用者拥有但 hash 字段会改变。
 */
struct dentry *d_hash_and_lookup(struct dentry *dir, struct qstr *name)
{
	/*
	 * Check for a fs-specific hash function. Note that we must
	 * calculate the standard hash first, as the d_op->d_hash()
	 * routine may choose to leave the hash value unchanged.
	 */
	/*
	 * 先计算通用 hash，再允许文件系统 d_hash 就地规范化；回调可以选择保持原值，
	 * 也可因非法编码返回 errno，因此不能把通用计算放到回调之后。
	 */
	name->hash = full_name_hash(dir, name->name, name->len);
	if (dir->d_flags & DCACHE_OP_HASH) {
		int err = dir->d_op->d_hash(dir, name);
		if (unlikely(err < 0))
			return ERR_PTR(err);
	}
	return d_lookup(dir, name);
}

/*
 * When a file is deleted, we have two options:
 * - turn this dentry into a negative dentry
 * - unhash this dentry and free it.
 *
 * Usually, we want to just turn this into
 * a negative dentry, but if anybody else is
 * currently using the dentry or the inode
 * we can't do that and we fall back on removing
 * it from the hash queues and waiting for
 * it to be deleted later when it has no users
 */
/*
 * 删除命名关系有两种缓存结果：无人共享时把正项转成可命中的负项，缓存“不存在”；
 * 仍有 dentry/inode 使用者时不能改变其正绑定，只能先 unhash，待最后引用释放再
 * 销毁。这样既保留常见负缓存收益，又不破坏已持引用者看到的对象语义。
 */
 
/**
 * d_delete - delete a dentry
 * @dentry: The dentry to delete
 *
 * Turn the dentry into a negative dentry if possible, otherwise
 * remove it from the hash queues so it can be deleted later
 */
 
/* 删除正 @dentry：唯一用户时转负，否则只 unhash；输入引用仍归调用者。 */
/*
 * d_delete() 在 i_lock -> d_lock 下处理已经从文件系统删除的正 dentry。若只有
 * 调用者一份引用，可保留为负缓存（策略要求时先 unhash），清 CANT_MOUNT 并
 * 解除 inode；有其他使用者则只能 unhash，保持正绑定直到最后 dput。输入引用
 * 仍归调用者，函数无返回；dentry_unlink_inode() 会代为释放两把锁和 inode 引用。
 */
void d_delete(struct dentry * dentry)
{
	struct inode *inode = dentry->d_inode;

	spin_lock(&inode->i_lock);
	spin_lock(&dentry->d_lock);
	/*
	 * Are we the only user?
	 */
	/* count==1 代表只剩当前删除路径，允许原地解除 inode；否则仅取消未来 lookup。 */
	if (dentry->d_lockref.count == 1) {
		if (dentry_negative_policy)
			__d_drop(dentry);
		dentry->d_flags &= ~DCACHE_CANT_MOUNT;
		dentry_unlink_inode(dentry);
	} else {
		__d_drop(dentry);
		spin_unlock(&dentry->d_lock);
		spin_unlock(&inode->i_lock);
	}
}

EXPORT_SYMBOL(d_delete);

/* __d_rehash() 在桶锁下把调用者稳定的 @entry 以 RCU 节点发布到正式 hash。 */
/* __d_rehash() 在桶锁下以 RCU 方式发布 entry；调用者另行保证 d_lock/状态稳定。 */
static void __d_rehash(struct dentry *entry)
{
	struct hlist_bl_head *b = d_hash(entry->d_name.hash);

	hlist_bl_lock(b);
	hlist_bl_add_head_rcu(&entry->d_hash, b);
	hlist_bl_unlock(b);
}

/**
 * d_rehash - add an entry back to the hash
 * @entry: dentry to add to the hash
 *
 * Adds a dentry to the hash according to its name.
 */
 
/* 在 d_lock 下把当前 @entry 发布回正式哈希；不增引用、无返回。 */
/*
 * d_rehash() 取得 @entry->d_lock 后把当前 (parent,name,hash) 发布到正式哈希。
 * @entry 为借用且须处于 unhashed 合法状态；无引用转移、无返回、不睡眠。
 */
void d_rehash(struct dentry * entry)
{
	spin_lock(&entry->d_lock);
	__d_rehash(entry);
	spin_unlock(&entry->d_lock);
}

EXPORT_SYMBOL(d_rehash);

/* start_dir_add() 禁抢占并把借用目录 i_dir_seq 从偶数原子改奇数，返回旧代次。 */
/*
 * start_dir_add()/end_dir_add() 用 i_dir_seq 的奇偶位串行化同一目录中 parallel
 * lookup 的提交。前者禁用抢占并以 cmpxchg 把偶数改奇数，返回旧代次；后者用
 * release store 发布新偶数并恢复抢占。该序列与 d_alloc_parallel() 的 acquire
 * 读取配对，保证正式哈希与 in-lookup 表之间不存在两个执行者都认为缺失的窗。
 */
static inline unsigned start_dir_add(struct inode *dir)
{
	preempt_disable_nested();
	for (;;) {
		unsigned n = READ_ONCE(dir->i_dir_seq);
		if (!(n & 1) && try_cmpxchg(&dir->i_dir_seq, &n, n + 1))
			return n;
		cpu_relax();
	}
}

/*
 * end_dir_add() 用 release store 把 @dir->i_dir_seq 从写入中的奇数推进到下一偶数，
 * 发布本轮目录结果后恢复抢占；@n 必须是配对 start_dir_add() 返回的旧代次。
 */
static inline void end_dir_add(struct inode *dir, unsigned int n)
{
	smp_store_release(&dir->i_dir_seq, n + 2);
	preempt_enable_nested();
}

/* d_wait_lookup() 入口和出口均持 d_lock；必要时睡眠等待 PAR_LOOKUP 清除。 */
/*
 * d_wait_lookup() 入口持 d_lock；若仍为 PAR_LOOKUP，置 WAITERS 并使用
 * wait_var_event_spinlock 原子释放锁、睡眠、重取锁，直到执行者完成。返回仍持
 * d_lock。唤醒由 __d_wake_in_lookup_waiters() 在 i_dir_seq 写区间外完成。
 */
static void d_wait_lookup(struct dentry *dentry)
{
	if (likely(d_in_lookup(dentry))) {
		dentry->d_flags |= DCACHE_LOOKUP_WAITERS;
		wait_var_event_spinlock(&dentry->d_flags,
					!d_in_lookup(dentry),
					&dentry->d_lock);
	}
}

/*
 * d_alloc_parallel() 合并同一目录同名的并发 cache miss。@parent/@name 为借用；
 * 函数预分配 new，并通过 i_dir_seq + rename_lock + 正式 hash 排除刚完成的结果，
 * 再以 in_lookup 桶锁选出唯一执行者。返回三类：ERR_PTR(-ENOMEM)；已有/等待完成
 * 的持有 dentry（!d_in_lookup）；或调用者拥有且 d_in_lookup 的新 dentry，后者
 * 必须执行文件系统 lookup 并最终 d_add()/d_lookup_done 类接口完成和唤醒。
 * 等待竞争 lookup 时可睡眠；所有未返回的 new 都由本函数 dput 回收。
 */
struct dentry *d_alloc_parallel(struct dentry *parent,
				const struct qstr *name)
{
	unsigned int hash = name->hash;
	struct hlist_bl_head *b = in_lookup_hash(parent, hash);
	struct hlist_bl_node *node;
	struct dentry *new = __d_alloc(parent->d_sb, name);
	struct dentry *dentry;
	unsigned seq, r_seq, d_seq;

	if (unlikely(!new))
		return ERR_PTR(-ENOMEM);

	new->d_flags |= DCACHE_PAR_LOOKUP;
	/* new 是当前线程的候选执行者，先挂父树但尚未进正式或临时 hash。 */
	spin_lock(&parent->d_lock);
	new->d_parent = dget_dlock(parent);
	hlist_add_head(&new->d_sib, &parent->d_children);
	if (parent->d_flags & DCACHE_DISCONNECTED)
		new->d_flags |= DCACHE_DISCONNECTED;
	spin_unlock(&parent->d_lock);

retry:
	/*
	 * 第一阶段先对正式 dcache 作 RCU 查找；同时记录目录提交序列和 rename
	 * 序列，任一变化都说明“未命中”快照不可靠，必须重试。
	 */
	seq = smp_load_acquire(&parent->d_inode->i_dir_seq);
	r_seq = read_seqbegin(&rename_lock);
	rcu_read_lock();
	dentry = __d_lookup_rcu(parent, name, &d_seq);
	if (unlikely(dentry)) {
		if (!lockref_get_not_dead(&dentry->d_lockref)) {
			rcu_read_unlock();
			goto retry;
		}
		rcu_read_unlock();
		if (read_seqcount_retry(&dentry->d_seq, d_seq)) {
			dput(dentry);
			goto retry;
		}
		dput(new);
		return dentry;
	}
	rcu_read_unlock();
	if (unlikely(read_seqretry(&rename_lock, r_seq)))
		goto retry;

	if (unlikely(seq & 1))
		goto retry;

	hlist_bl_lock(b);
	if (unlikely(READ_ONCE(parent->d_inode->i_dir_seq) != seq)) {
		hlist_bl_unlock(b);
		goto retry;
	}
	/*
	 * No changes for the parent since the beginning of d_lookup().
	 * Since all removals from the chain happen with hlist_bl_lock(),
	 * any potential in-lookup matches are going to stay here until
	 * we unlock the chain.  All fields are stable in everything
	 * we encounter.
	 */
	/*
	 * i_dir_seq 未变说明正式查找以来目录没有提交变化；临时桶锁又阻止候选被摘除，
	 * 因而扫描到的 in-lookup 项字段稳定，可以唯一选择等待对象或发布 new。
	 */
	hlist_bl_for_each_entry(dentry, node, b, d_in_lookup_hash) {
		/* 第二阶段在临时桶锁下寻找同 parent/name 的现有 lookup 执行者。 */
		if (dentry->d_name.hash != hash)
			continue;
		if (dentry->d_parent != parent)
			continue;
		if (!d_same_name(dentry, parent, name))
			continue;
		rcu_read_lock();
		hlist_bl_unlock(b);
		spin_lock(&dentry->d_lock);
		rcu_read_unlock();
		/* now we can try to grab a reference */
		/* 从临时桶切换到 d_lock 后内存由 RCU 桥接，此时才可检查死亡标记并增引用。 */
		if (unlikely(dentry->d_lockref.count < 0)) {
			spin_unlock(&dentry->d_lock);
			goto retry;
		}
		/*
		 * somebody is likely to be still doing lookup for it;
		 * pin it and wait for them to finish
		 */
		/* 对现有执行者增引用后等待，保证等待期间对象不会因失败路径释放。 */
		dget_dlock(dentry);
		d_wait_lookup(dentry);
		/* 等待期间 rename/drop 可改变身份；持锁逐项复核，不能盲信旧匹配。 */
		/*
		 * it's not in-lookup anymore; in principle we should repeat
		 * everything from dcache lookup, but it's likely to be what
		 * d_lookup() would've found anyway.  If it is, just return it;
		 * otherwise we really have to repeat the whole thing.
		 */
		/*
		 * 唤醒只保证 PAR_LOOKUP 已结束，不保证它仍是原 parent/name 的 hashed 结果；
		 * 先按完整 d_lookup 条件复核，任一不符就释放引用并从头重试。
		 */
		if (unlikely(dentry->d_name.hash != hash))
			goto mismatch;
		if (unlikely(dentry->d_parent != parent))
			goto mismatch;
		if (unlikely(d_unhashed(dentry)))
			goto mismatch;
		if (unlikely(!d_same_name(dentry, parent, name)))
			goto mismatch;
		/* OK, it *is* a hashed match; return it */
		/* 等待后的身份复核全部通过，它等价于一次新的 d_lookup 命中，可直接返回引用。 */
		spin_unlock(&dentry->d_lock);
		dput(new);
		return dentry;
	}
	hlist_bl_add_head(&new->d_in_lookup_hash, b);
	/* 没有竞争者：发布 new 到 in-lookup 表，调用者成为唯一 ->lookup 执行者。 */
	hlist_bl_unlock(b);
	return new;
mismatch:
	spin_unlock(&dentry->d_lock);
	dput(dentry);
	goto retry;
}

EXPORT_SYMBOL(d_alloc_parallel);

/*
 * Move dentry from in-lookup state to busy-negative one.
 *
 * From now on d_in_lookup(dentry) will return false and dentry is gone from
 * in-lookup hash.
 *
 * Anyone who had been waiting on it in d_alloc_parallel() is free to
 * proceed after that.  Note that waking such waiters up is left to
 * the callers; PREEMPT_RT kernels can't have that wakeup done while
 * in write-side critical area for ->i_dir_seq, so it's done by calling
 * __d_wake_in_lookup_waiters() once it's safe to do so.
 *
 * Both __d_lookup_unhash() and __d_wake_in_lookup_waiters() should
 * be called within the same ->d_lock scope.  PAR_LOOKUP is cleared
 * here, while LOOKUP_WAITERS (set by somebody finding dentry in
 * the in-lookup hash and setting down to wait) is checked and cleared
 * in __d_wake_in_lookup_waiters().  Both are gone by the end of
 * ->d_lock scope.
 */
/* 持 d_lock 从 in-lookup 表摘除执行者并清 PAR_LOOKUP；唤醒由外层完成。 */
/*
 * __d_lookup_unhash() 要求 d_lock，把执行者从 in-lookup 桶摘除、清 PAR_LOOKUP，
 * 并把 union 字段 waiters 初始化为空。它只结束“正在查找”身份，不负责正式
 * rehash 或唤醒；与后者拆开是因为 PREEMPT_RT 不允许在 i_dir_seq 写侧区间唤醒。
 */
static void __d_lookup_unhash(struct dentry *dentry)
{
	struct hlist_bl_head *b;

	lockdep_assert_held(&dentry->d_lock);

	b = in_lookup_hash(dentry->d_parent, dentry->d_name.hash);
	hlist_bl_lock(b);
	dentry->d_flags &= ~DCACHE_PAR_LOOKUP;
	__hlist_bl_del(&dentry->d_in_lookup_hash);
	hlist_bl_unlock(b);
	dentry->waiters = NULL;
}

/*
 * __d_wake_in_lookup_waiters() 在同一 d_lock 作用域检查并清 WAITERS，唤醒睡在
 * d_flags 上的线程。清位后本轮 parallel lookup 的两个临时状态均已消失。
 */
static inline void __d_wake_in_lookup_waiters(struct dentry *dentry)
{
	if (dentry->d_flags & DCACHE_LOOKUP_WAITERS) {
		wake_up_var_locked(&dentry->d_flags, &dentry->d_lock);
		dentry->d_flags &= ~DCACHE_LOOKUP_WAITERS;
	}
}

/*
 * __d_lookup_unhash_wake() 用于不经 __d_add() 的失败/取消完成路径，完整执行摘
 * 临时 hash 与唤醒。@dentry 为借用且必须仍处于 in_lookup；无返回、不睡眠。
 */
void __d_lookup_unhash_wake(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	__d_lookup_unhash(dentry);
	__d_wake_in_lookup_waiters(dentry);
	spin_unlock(&dentry->d_lock);
}

EXPORT_SYMBOL(__d_lookup_unhash_wake);

/* inode->i_lock held if inode is non-NULL */

/* 提交 parallel/普通 lookup 结果：可绑定 @inode、正式 rehash，并结束等待协议。 */
/*
 * __d_add() 是 lookup 结果的原子提交核心。入口若 @inode 非 NULL，调用者已持
 * inode->i_lock 并转交其引用；@ops 可选地只安装一次。parallel 项先在目录
 * i_dir_seq 写区间内退出临时表，随后可绑定 inode、发布到正式 hash，最后结束
 * 序列并唤醒等待者。返回无；函数总是释放 d_lock，并在正项时代调用者释放
 * inode->i_lock。提交后 dentry 可被普通 lookup 发现。
 */
static inline void __d_add(struct dentry *dentry, struct inode *inode,
			   const struct dentry_operations *ops)
{
	struct inode *dir = NULL;
	unsigned n;
	spin_lock(&dentry->d_lock);
	if (unlikely(d_in_lookup(dentry))) {
		/* 奇数 i_dir_seq 覆盖“离开临时表到进入正式表”，阻止新执行者穿过空窗。 */
		dir = dentry->d_parent->d_inode;
		n = start_dir_add(dir);
		__d_lookup_unhash(dentry);
	}
	if (unlikely(ops))
		d_set_d_op(dentry, ops);
	if (inode) {
		unsigned add_flags = d_flags_for_inode(inode);
		hlist_add_head(&dentry->d_alias, &inode->i_dentry);
		raw_write_seqcount_begin(&dentry->d_seq);
		__d_set_inode_and_type(dentry, inode, add_flags);
		raw_write_seqcount_end(&dentry->d_seq);
		fsnotify_update_flags(dentry);
	}
	__d_rehash(dentry);
	/* 正/负结果均进入正式 hash；负项由 inode==NULL 表达并缓存 ENOENT 结果。 */
	if (dir) {
		end_dir_add(dir, n);
		__d_wake_in_lookup_waiters(dentry);
	}
	spin_unlock(&dentry->d_lock);
	if (inode)
		spin_unlock(&inode->i_lock);
}

/**
 * d_add - add dentry to hash queues
 * @entry: dentry to add
 * @inode: The inode to attach to this dentry
 *
 * This adds the entry to the hash queues and initializes @inode.
 * The entry was actually filled in earlier during d_alloc().
 */

/* 文件系统 lookup 完成接口；消耗可选 @inode，发布正/负 @entry 并唤醒等待者。 */
/*
 * d_add() 是文件系统 ->lookup 的常用完成接口。@entry 是本次负/parallel dentry；
 * @inode 可 NULL 表示负结果，非 NULL 的一份引用被消耗。先执行 LSM 实例化，再
 * 由 __d_add() 绑定并 rehash、结束 parallel lookup。无返回，成功后 entry 已
 * 正式可查找，等待相同名字的线程均可继续。
 */
void d_add(struct dentry *entry, struct inode *inode)
{
	if (inode) {
		security_d_instantiate(entry, inode);
		spin_lock(&inode->i_lock);
	}
	__d_add(entry, inode, NULL);
}

EXPORT_SYMBOL(d_add);

/*
 * d_make_persistent() 消耗 @inode 引用并为 @dentry 建立额外永久引用；返回同一
 * dentry，须以 d_make_discardable() 撤销持久状态。
 */
/*
 * d_make_persistent() 把负 dentry 绑定 @inode，并额外 dget 后置 PERSISTENT，必要
 * 时 rehash。它消耗 inode 引用并返回同一 dentry；额外引用必须最终由
 * d_make_discardable() 撤销，普通 dput 不会让它进入零引用回收。
 */
struct dentry *d_make_persistent(struct dentry *dentry, struct inode *inode)
{
	WARN_ON(d_really_is_positive(dentry));
	WARN_ON(!inode);
	security_d_instantiate(dentry, inode);
	spin_lock(&inode->i_lock);
	spin_lock(&dentry->d_lock);
	__d_instantiate(dentry, inode);
	dentry->d_flags |= DCACHE_PERSISTENT;
	dget_dlock(dentry);
	if (d_unhashed(dentry))
		__d_rehash(dentry);
	spin_unlock(&dentry->d_lock);
	spin_unlock(&inode->i_lock);
	return dentry;
}

EXPORT_SYMBOL(d_make_persistent);

/* swap_names() 在锁与 d_seq 写区间内交换两个 dentry 的内联/外置名及 hash_len。 */
/*
 * swap_names() 在两个 d_lock 与 d_seq 写区间内交换名字。外置/内联四种组合
 * 分别交换指针或复制固定缓冲，确保每个 __d_name.name 最终指向自己的内联
 * 存储或完整外置对象；最后交换 hash_len。名字对象引用总数不变，不会分配。
 */
static void swap_names(struct dentry *dentry, struct dentry *target)
{
	if (unlikely(dname_external(target))) {
		if (unlikely(dname_external(dentry))) {
			/*
			 * Both external: swap the pointers
			 */
			/* 两边都由引用计数外置对象承载，交换指针即可保持各自完整名字。 */
			swap(target->__d_name.name, dentry->__d_name.name);
		} else {
			/*
			 * dentry:internal, target:external.  Steal target's
			 * storage and make target internal.
			 */
			/* dentry 接管 target 外置存储，target 收下 dentry 的内联内容并修正自指针。 */
			dentry->__d_name.name = target->__d_name.name;
			target->d_shortname = dentry->d_shortname;
			target->__d_name.name = target->d_shortname.string;
		}
	} else {
		if (unlikely(dname_external(dentry))) {
			/*
			 * dentry:external, target:internal.  Give dentry's
			 * storage to target and make dentry internal
			 */
			/* target 接管外置对象，dentry 复制 target 内联内容并让 name 指回自身缓冲。 */
			target->__d_name.name = dentry->__d_name.name;
			dentry->d_shortname = target->d_shortname;
			dentry->__d_name.name = dentry->d_shortname.string;
		} else {
			/*
			 * Both are internal.
			 */
			/* 两边名字都在各自对象内，只能逐机器字交换内容，不能交换自引用指针。 */
			for (int i = 0; i < DNAME_INLINE_WORDS; i++)
				swap(dentry->d_shortname.words[i],
				     target->d_shortname.words[i]);
		}
	}
	swap(dentry->__d_name.hash_len, target->__d_name.hash_len);
}

/*
 * copy_name() 让 dentry 取得 target 名字而 target 保持不变。外置目标先增引用；
 * 被替换的旧外置名最后减引用并 kfree_rcu，防止并发名字快照 UAF。调用者持锁
 * 且处于 d_seq 写区间，函数不睡眠。
 */
static void copy_name(struct dentry *dentry, struct dentry *target)
{
	struct external_name *old_name = NULL;
	if (unlikely(dname_external(dentry)))
		old_name = external_name(dentry);
	if (unlikely(dname_external(target))) {
		atomic_inc(&external_name(target)->count);
		dentry->__d_name = target->__d_name;
	} else {
		dentry->d_shortname = target->d_shortname;
		dentry->__d_name.name = dentry->d_shortname.string;
		dentry->__d_name.hash_len = target->__d_name.hash_len;
	}
	if (old_name && likely(atomic_dec_and_test(&old_name->count)))
		kfree_rcu(old_name, head);
}

/*
 * __d_move - move a dentry
 * @dentry: entry to move
 * @target: new dentry
 * @exchange: exchange the two dentries
 *
 * Update the dcache to reflect the move of a file name. Negative dcache
 * entries should not be moved in this way. Caller must hold rename_lock, the
 * i_rwsem of the source and target directories (exclusively), and the sb->
 * s_vfs_rename_mutex if they differ. See lock_rename().
 */
/* 在 rename 锁协议下原子更新 hash、名字、父子链及序列；exchange 决定移动或交换。 */
/*
 * __d_move() 在外层写持 rename_lock、源/目标目录 i_rwsem（以及需要时 rename
 * mutex）的前提下更新整个 dcache 命名关系。@dentry 必须为正项，@target 是新
 * 名字占位；@exchange=false 消耗目标位置，true 交换两项。函数按祖先关系取得
 * parent/dentry 锁，封住 d_seq，更新哈希、父子链、名字、引用、fsnotify 与
 * fscrypt 派生状态，再唤醒被覆盖的 parallel lookup。无返回且不转移调用者
 * 对两个 dentry 的显式引用；锁内不睡眠。
 */
static void __d_move(struct dentry *dentry, struct dentry *target,
		     bool exchange)
{
	struct dentry *old_parent, *p;
	struct inode *dir = NULL;
	unsigned n;

	WARN_ON(!dentry->d_inode);
	if (WARN_ON(dentry == target))
		return;

	BUG_ON(d_ancestor(target, dentry));
	/* 把节点移入自身后代会制造目录环，调用契约禁止且在修改前立即终止。 */
	old_parent = dentry->d_parent;
	p = d_ancestor(old_parent, target);
	if (IS_ROOT(dentry)) {
		BUG_ON(p);
		spin_lock(&target->d_parent->d_lock);
	} else if (!p) {
		/* target is not a descendent of dentry->d_parent */
		/* 两父目录无祖先嵌套，先锁目标父，再以 nested 类别锁旧父。 */
		spin_lock(&target->d_parent->d_lock);
		spin_lock_nested(&old_parent->d_lock, DENTRY_D_LOCK_NESTED);
	} else {
		BUG_ON(p == dentry);
		spin_lock(&old_parent->d_lock);
		if (p != target)
			spin_lock_nested(&target->d_parent->d_lock,
					DENTRY_D_LOCK_NESTED);
	}
	/*
	 * 根据 old_parent 与 target 父的祖先关系选取从上到下的锁序；外层
	 * rename_lock 已让无亲缘的两棵分支串行，避免 ABBA。
	 */
	spin_lock_nested(&dentry->d_lock, 2);
	spin_lock_nested(&target->d_lock, 3);

	if (unlikely(d_in_lookup(target))) {
		dir = target->d_parent->d_inode;
		n = start_dir_add(dir);
		__d_lookup_unhash(target);
	}
	/* 覆盖一个 parallel lookup 占位时，先封住目录提交序列并取消其执行者身份。 */

	write_seqcount_begin(&dentry->d_seq);
	write_seqcount_begin_nested(&target->d_seq, DENTRY_D_LOCK_NESTED);

	/* unhash both */
	/* 修改名字/父关系前暂时摘除两项，防止 lookup 命中半更新的哈希键。 */
	if (!d_unhashed(dentry))
		___d_drop(dentry);
	if (!d_unhashed(target))
		___d_drop(target);

	/* ... and switch them in the tree */
	/*
	 * 两项先从正式 hash 暂时摘除，再同时更新 parent、name、d_sib 和 parent 引用；
	 * exchange 保留两项并互换位置，普通 move 让 target 成为 unhashed 占位。
	 */
	dentry->d_parent = target->d_parent;
	if (!exchange) {
		copy_name(dentry, target);
		target->d_hash.pprev = NULL;
		dentry->d_parent->d_lockref.count++;
		if (dentry != old_parent) /* wasn't IS_ROOT */
			/* 非根源项原先持有 old_parent 引用，搬离后必须精确交还。 */
			WARN_ON(!--old_parent->d_lockref.count);
	} else {
		target->d_parent = old_parent;
		swap_names(dentry, target);
		if (!hlist_unhashed(&target->d_sib))
			__hlist_del(&target->d_sib);
		hlist_add_head(&target->d_sib, &target->d_parent->d_children);
		__d_rehash(target);
		fsnotify_update_flags(target);
	}
	if (!hlist_unhashed(&dentry->d_sib))
		__hlist_del(&dentry->d_sib);
	hlist_add_head(&dentry->d_sib, &dentry->d_parent->d_children);
	__d_rehash(dentry);
	fsnotify_update_flags(dentry);
	fscrypt_handle_d_move(dentry);

	write_seqcount_end(&target->d_seq);
	write_seqcount_end(&dentry->d_seq);
	/* d_seq 变为稳定偶数后，RCU reader 才能接受新的名字/父/hash 组合。 */

	if (dir) {
		end_dir_add(dir, n);
		__d_wake_in_lookup_waiters(target);
	}
	if (dentry->d_parent != old_parent)
		spin_unlock(&dentry->d_parent->d_lock);
	if (dentry != old_parent)
		spin_unlock(&old_parent->d_lock);
	spin_unlock(&target->d_lock);
	spin_unlock(&dentry->d_lock);
}

/*
 * d_move - move a dentry
 * @dentry: entry to move
 * @target: new dentry
 *
 * Update the dcache to reflect the move of a file name. Negative
 * dcache entries should not be moved in this way. See the locking
 * requirements for __d_move.
 */
/* 以 rename_lock 写侧把正 @dentry 移到 @target 的名字位置；引用保持不变。 */
/* d_move() 以 rename_lock 写侧包装普通移动，使所有乐观树读者检测到版本变化。 */
void d_move(struct dentry *dentry, struct dentry *target)
{
	write_seqlock(&rename_lock);
	__d_move(dentry, target, false);
	write_sequnlock(&rename_lock);
}

EXPORT_SYMBOL(d_move);

/*
 * d_exchange - exchange two dentries
 * @dentry1: first dentry
 * @dentry2: second dentry
 */
/* 在 rename_lock 写侧交换两个非根正 dentry 的位置和名字；引用保持不变。 */
/*
 * d_exchange() 在 rename_lock 写侧交换两个非根正 dentry。WARN 诊断契约违例；
 * 返回后调用者引用仍分别指向原对象，但二者名字与父位置已交换。
 */
void d_exchange(struct dentry *dentry1, struct dentry *dentry2)
{
	write_seqlock(&rename_lock);

	WARN_ON(!dentry1->d_inode);
	WARN_ON(!dentry2->d_inode);
	WARN_ON(IS_ROOT(dentry1));
	WARN_ON(IS_ROOT(dentry2));

	__d_move(dentry1, dentry2, true);

	write_sequnlock(&rename_lock);
}

EXPORT_SYMBOL(d_exchange);

/**
 * d_ancestor - search for an ancestor
 * @p1: ancestor dentry
 * @p2: child dentry
 *
 * Returns the ancestor dentry of p2 which is a child of p1, if p1 is
 * an ancestor of p2, else NULL.
 */
/* 在调用者稳定树时查祖先关系；返回 @p1 下通往 @p2 的第一子项或 NULL。 */
/*
 * d_ancestor() 沿 @p2 的 parent 链查找 @p1；命中返回“p1 之下、通往 p2 的第一
 * 个孩子”，否则 NULL。它不取锁或引用，调用者必须用 rename_lock/RCU 稳定树。
 */
struct dentry *d_ancestor(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	for (p = p2; !IS_ROOT(p); p = p->d_parent) {
		if (p->d_parent == p1)
			return p;
	}
	return NULL;
}

/*
 * This helper attempts to cope with remotely renamed directories
 *
 * It assumes that the caller is already holding
 * dentry->d_parent->d_inode->i_rwsem, and rename_lock
 *
 * Note: If ever the locking in lock_rename() changes, then please
 * remember to update this too...
 */
/* 尝试取得远端 rename 附加锁并把目录 alias 移到 dentry；成功 0，否则 -ESTALE。 */
/*
 * __d_unalias() 把远端文件系统发现的目录 @alias 搬到 lookup 的 @dentry 位置。
 * 外层已有目标父 i_rwsem 与 rename_lock；父不同则 trylock superblock rename
 * mutex 和 alias 父 i_rwsem，避免阻塞时形成锁环。文件系统可用 d_unalias_*
 * 扩展锁协议。成功 0；不能无阻塞取得锁或回调拒绝返回 -ESTALE；已取得资源在
 * out_err 逆序释放。两个 dentry 引用仍归调用者。
 */
static int __d_unalias(struct dentry *dentry, struct dentry *alias)
{
	struct mutex *m1 = NULL;
	struct rw_semaphore *m2 = NULL;
	int ret = -ESTALE;

	/* If alias and dentry share a parent, then no extra locks required */
	/* 同父时调用者已有的父目录 i_rwsem 足以覆盖两名字，无需 superblock 附加锁。 */
	if (alias->d_parent == dentry->d_parent)
		goto out_unalias;

	/* See lock_rename() */
	/* 异父路径复制 lock_rename 的 mutex -> alias 父 i_rwsem 顺序，并用 trylock 防环。 */
	if (!mutex_trylock(&dentry->d_sb->s_vfs_rename_mutex))
		goto out_err;
	m1 = &dentry->d_sb->s_vfs_rename_mutex;
	if (!inode_trylock_shared(alias->d_parent->d_inode))
		goto out_err;
	m2 = &alias->d_parent->d_inode->i_rwsem;
out_unalias:
	if (alias->d_op && alias->d_op->d_unalias_trylock &&
	    !alias->d_op->d_unalias_trylock(alias))
		goto out_err;
	__d_move(alias, dentry, false);
	if (alias->d_op && alias->d_op->d_unalias_unlock)
		alias->d_op->d_unalias_unlock(alias);
	ret = 0;
out_err:
	if (m2)
		up_read(m2);
	if (m1)
		mutex_unlock(m1);
	return ret;
}

/*
 * d_splice_alias_ops() 是文件系统 lookup 完成时维护目录单 alias 不变量的核心。
 * @inode 为被消耗的引用（可 NULL/ERR_PTR），@dentry 是 unhashed 负占位，@ops
 * 可选安装。普通文件或无旧 alias 直接 __d_add 并返回 NULL；目录已有 alias 时
 * 在 rename_lock 下把它移动到新位置，检测目录环/远端 rename 锁冲突，并返回
 * 持有的替代 dentry 或 ERR_PTR。返回非 NULL 时输入 dentry 没有成为结果，调用
 * 者按 ->lookup 约定处理；所有路径恰好消费 inode 引用。
 */
struct dentry *d_splice_alias_ops(struct inode *inode, struct dentry *dentry,
				  const struct dentry_operations *ops)
{
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	BUG_ON(!d_unhashed(dentry));

	if (!inode)
		goto out;

	security_d_instantiate(dentry, inode);
	spin_lock(&inode->i_lock);
	if (S_ISDIR(inode->i_mode)) {
		/* 目录必须只有一个 alias；在 i_lock 下取得已有者引用以稳定后续 move。 */
		struct dentry *new = __d_find_dir_alias(inode);
		if (unlikely(new)) {
			/* The reference to new ensures it remains an alias */
			/* __d_find_dir_alias() 取得的引用跨越 i_lock 放锁，保证 new 不会被淘汰。 */
			spin_unlock(&inode->i_lock);
			write_seqlock(&rename_lock);
			if (unlikely(d_ancestor(new, dentry))) {
				/* 把祖先 splice 到后代位置会成环，拒绝并以 -ELOOP 报告损坏。 */
				write_sequnlock(&rename_lock);
				dput(new);
				new = ERR_PTR(-ELOOP);
				pr_warn_ratelimited(
					"VFS: Lookup of '%s' in %s %s"
					" would have caused loop\n",
					dentry->d_name.name,
					inode->i_sb->s_type->name,
					inode->i_sb->s_id);
			} else if (!IS_ROOT(new)) {
				struct dentry *old_parent = dget(new->d_parent);
				int err = __d_unalias(dentry, new);
				write_sequnlock(&rename_lock);
				if (err) {
					dput(new);
					new = ERR_PTR(err);
				}
				dput(old_parent);
			} else {
				if (unlikely(!hlist_unhashed(&new->d_sib))) {
					// secondary root getting spliced
					/* 匿名次级根即将接入普通命名树，必须先从 sb->s_roots 去重摘除。 */
					spin_lock(&new->d_lock);
					unlink_secondary_root(new);
					spin_unlock(&new->d_lock);
				}
				__d_move(new, dentry, false);
				write_sequnlock(&rename_lock);
			}
			iput(inode);
			return new;
		}
	}
out:
	__d_add(dentry, inode, ops);
	return NULL;
}

/**
 * d_splice_alias - splice a disconnected dentry into the tree if one exists
 * @inode:  the inode which may have a disconnected dentry
 * @dentry: a negative dentry which we want to point to the inode.
 *
 * If inode is a directory and has an IS_ROOT alias, then d_move that in
 * place of the given dentry and return it, else simply d_add the inode
 * to the dentry and return NULL.
 *
 * If a non-IS_ROOT directory is found, the filesystem is corrupt, and
 * we should error out: directories can't have multiple aliases.
 *
 * This is needed in the lookup routine of any filesystem that is exportable
 * (via knfsd) so that we can build dcache paths to directories effectively.
 *
 * If a dentry was found and moved, then it is returned.  Otherwise NULL
 * is returned.  This matches the expected return value of ->lookup.
 *
 * Cluster filesystems may call this function with a negative, hashed dentry.
 * In that case, we know that the inode will be a regular file, and also this
 * will only occur during atomic_open. So we need to check for the dentry
 * being already hashed only in the final case.
 */
/* 消耗 @inode，维持目录单 alias；返回搬入的 alias、NULL 或 ERR_PTR。 */
/* d_splice_alias() 是不安装额外 d_op 的公开包装，返回语义与 ->lookup 一致。 */
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry)
{
	return d_splice_alias_ops(inode, dentry, NULL);
}

EXPORT_SYMBOL(d_splice_alias);

/*
 * Test whether new_dentry is a subdirectory of old_dentry.
 *
 * Trivially implemented using the dcache structure
 */
/* 直接沿 dcache 的 d_parent 树判断祖先关系；并发 rename 的一致性由下方序列协议负责。 */

/**
 * is_subdir - is new dentry a subdirectory of old_dentry
 * @new_dentry: new dentry
 * @old_dentry: old dentry
 *
 * Returns true if new_dentry is a subdirectory of the parent (at any depth).
 * Returns false otherwise.
 * Caller must ensure that "new_dentry" is pinned before calling is_subdir()
 */
  
/* 在 RCU/rename 序列下判断 pinned @new_dentry 是否位于 @old_dentry 子树。 */
/*
 * is_subdir() 判断 pinned 的 @new_dentry 是否位于 @old_dentry 子树（相等也为真）。
 * 首次在 RCU + rename seq 下无锁沿父链；并发 rename 时升级为独占读锁保证深链
 * 最终进展。返回 bool，不取新引用，调用者必须保证 new_dentry 存活。
 */
bool is_subdir(struct dentry *new_dentry, struct dentry *old_dentry)
{
	bool subdir;
	unsigned seq;

	if (new_dentry == old_dentry)
		return true;

	/* Access d_parent under rcu as d_move() may change it. */
	/* d_move 可换父，RCU 保证沿旧父链时对象存储仍有效，序列负责验证拓扑。 */
	rcu_read_lock();
	seq = read_seqbegin(&rename_lock);
	subdir = d_ancestor(old_dentry, new_dentry);
	 /* Try lockless once... */
	 /* 先用 rename seq 乐观遍历一次，常见无 rename 情况无需全局锁。 */
	if (read_seqretry(&rename_lock, seq)) {
		/* ...else acquire lock for progress even on deep chains. */
		/* 冲突后取独占读锁，阻止写侧持续使深链遍历反复失败，保证最终完成。 */
		read_seqlock_excl(&rename_lock);
		subdir = d_ancestor(old_dentry, new_dentry);
		read_sequnlock_excl(&rename_lock);
	}
	rcu_read_unlock();
	return subdir;
}

EXPORT_SYMBOL(is_subdir);

/* d_mark_tmpfile() 给未实例化、unlinked 的 file dentry 写入内联“#ino”调试名。 */
/*
 * d_mark_tmpfile() 为尚未实例化、unlinked 且使用内联名的 O_TMPFILE dentry 生成
 * “#inode号”调试名字。它按 parent->d_lock -> child d_lock 更新长度与缓冲；
 * 名字不 rehash，因匿名文件仍不可路径查找。@file/@inode 均借用，无返回。
 */
void d_mark_tmpfile(struct file *file, struct inode *inode)
{
	struct dentry *dentry = file->f_path.dentry;

	BUG_ON(dname_external(dentry) ||
		d_really_is_positive(dentry) ||
		!d_unlinked(dentry));
	spin_lock(&dentry->d_parent->d_lock);
	spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
	dentry->__d_name.len = sprintf(dentry->d_shortname.string, "#%llu",
				(unsigned long long)inode->i_ino);
	spin_unlock(&dentry->d_lock);
	spin_unlock(&dentry->d_parent->d_lock);
}

EXPORT_SYMBOL(d_mark_tmpfile);

/* d_mark_tmpfile_name() 改写匿名 dentry 的短名字；成功 0，状态或长度非法返回 errno。 */
/*
 * d_mark_tmpfile_name() 用调用者给出的短 @name 替代上述调试名。对象状态不符
 * 返回 -EINVAL，超过内联容量返回 -ENAMETOOLONG；成功 0，并在父子锁下复制及
 * NUL 终止。它不分配外置名、不 rehash、不改变引用。
 */
int d_mark_tmpfile_name(struct file *file, const struct qstr *name)
{
	struct dentry *dentry = file->f_path.dentry;
	char *dname = dentry->d_shortname.string;

	if (unlikely(dname_external(dentry) ||
		     d_really_is_positive(dentry) ||
		     !d_unlinked(dentry)))
		return -EINVAL;
	if (unlikely(name->len > DNAME_INLINE_LEN - 1))
		return -ENAMETOOLONG;

	spin_lock(&dentry->d_parent->d_lock);
	spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
	dentry->__d_name.len = name->len;
	memcpy(dname, name->name, name->len);
	dname[name->len] = '\0';
	spin_unlock(&dentry->d_lock);
	spin_unlock(&dentry->d_parent->d_lock);
	return 0;
}

EXPORT_SYMBOL(d_mark_tmpfile_name);

/* d_tmpfile() 减 @inode 链接数、设置匿名名并把 inode 引用绑定到 file dentry。 */
/*
 * d_tmpfile() 完成匿名临时文件：先减 link count 表示不在目录中，再设置可诊断
 * 名字，最后把 @inode 引用转交 d_instantiate()。@file 持有 dentry；无返回，
 * 成功后 dentry 为正但仍 unlinked，只有 file 引用可达。
 */
void d_tmpfile(struct file *file, struct inode *inode)
{
	struct dentry *dentry = file->f_path.dentry;

	inode_dec_link_count(inode);
	d_mark_tmpfile(file, inode);
	d_instantiate(dentry, inode);
}

EXPORT_SYMBOL(d_tmpfile);

/*
 * Obtain inode number of the parent dentry.
 */
/* 返回借用 @dentry 当前父 inode 号；RCU 快照失败时以 d_lock 慢路读取。 */
/*
 * d_parent_ino() 返回 @dentry 当前父 inode 号。快路径在 scoped_guard(rcu) 下
 * 读取 d_seq、parent 和 d_inode_rcu，guard 离开块自动 rcu_read_unlock；快照
 * 不一致或父 inode 暂空时以 d_lock 慢路径重读。@dentry 借用，无引用转移。
 */
ino_t d_parent_ino(struct dentry *dentry)
{
	struct dentry *parent;
	struct inode *iparent;
	unsigned seq;
	ino_t ret;

	scoped_guard(rcu) {
		seq = raw_seqcount_begin(&dentry->d_seq);
		parent = READ_ONCE(dentry->d_parent);
		iparent = d_inode_rcu(parent);
		if (likely(iparent)) {
			ret = iparent->i_ino;
			if (!read_seqcount_retry(&dentry->d_seq, seq))
				return ret;
		}
	}

	spin_lock(&dentry->d_lock);
	ret = dentry->d_parent->d_inode->i_ino;
	spin_unlock(&dentry->d_lock);
	return ret;
}

EXPORT_SYMBOL(d_parent_ino);

/*
 * dhash_entries 保存 dhash_entries= 启动参数请求的正式哈希桶数量，0 表示由
 * alloc_large_system_hash() 按内存规模估算。它只在 init 阶段读写，初始化内存
 * 回收后不再存在，也不参与运行期哈希索引同步。
 */
static __initdata unsigned long dhash_entries;
/* dhash_entries 仅在 init 内存中存在，保存启动参数指定的目标桶数；0 表示自动估算。 */

/*
 * set_dhash_entries() 解析 dhash_entries= 启动参数。@str 为 init 期借用字符串；
 * 成功写全局值并返回 1，解析失败返回 0 让 __setup 报未处理。init 后代码释放。
 */
static int __init set_dhash_entries(char *str)
{
	return kstrtoul(str, 0, &dhash_entries) == 0;
}
__setup("dhash_entries=", set_dhash_entries);

/*
 * dcache_init_early() 在 vmalloc/完整 slab 尚不可用的早期分配正式 dentry hash。
 * 仅 hashdist==0 执行；NUMA 分布式 hash 延迟到 dcache_init。分配器返回的 shift
 * 语义转换成 runtime_const 所需的右移量，并特化 d_hash() 的指针/移位访问。
 * 入参无、无返回，HASH_EARLY|HASH_ZERO 保证早期内存与零初始化。
 */
static void __init dcache_init_early(void)
{
	/* If hashes are distributed across NUMA nodes, defer
	 * hash allocation until vmalloc space is available.
	 */
	/* NUMA 分布式哈希依赖稍后可用的 vmalloc/节点分配设施，早期阶段只直接返回。 */
	if (hashdist)
		return;

	dentry_hashtable =
		alloc_large_system_hash("Dentry cache",
					sizeof(struct hlist_bl_head),
					dhash_entries,
					13,
					HASH_EARLY | HASH_ZERO,
					&d_hash_shift,
					NULL,
					2,
					0);
	d_hash_shift = 32 - d_hash_shift;

	runtime_const_init(shift, d_hash_shift);
	runtime_const_init(ptr, dentry_hashtable);
}

/* dcache_init() 创建 dentry slab，并在 hashdist 配置下延迟分配正式哈希表。 */
/*
 * dcache_init() 创建可回收、计费且允许 d_shortname usercopy 的 dentry slab；
 * 若 hashdist!=0，再在完整内存管理可用后分配 NUMA 分布式 hash。runtime_const
 * 初始化后热路径可把 cache 指针、桶基址和 shift 当运行期常量使用。无返回。
 */
static void __init dcache_init(void)
{
	/*
	 * A constructor could be added for stable state like the lists,
	 * but it is probably not worth it because of the cache nature
	 * of the dcache.
 */
	/*
	 * dentry 是高周转缓存对象，逐对象 constructor 对链表等稳定字段的初始化
	 * 收益不足以抵消成本，所以 __d_alloc() 显式初始化每个字段。
	 */
	__dentry_cache = KMEM_CACHE_USERCOPY(dentry,
		SLAB_RECLAIM_ACCOUNT|SLAB_PANIC|SLAB_ACCOUNT,
		d_shortname.string);
	runtime_const_init(ptr, __dentry_cache);

	/* Hash may have been set up in dcache_init_early */
	/* 非分布配置已在 early 阶段完成，不能重复分配或重写 runtime constant。 */
	if (!hashdist)
		return;

	dentry_hashtable =
		alloc_large_system_hash("Dentry cache",
					sizeof(struct hlist_bl_head),
					dhash_entries,
					13,
					HASH_ZERO,
					&d_hash_shift,
					NULL,
					2,
					0);
	d_hash_shift = 32 - d_hash_shift;

	runtime_const_init(shift, d_hash_shift);
	runtime_const_init(ptr, dentry_hashtable);
}

/*
 * vfs_caches_init_early() 在早期启动逐桶初始化 parallel-lookup 临时表，再初始化
 * dcache 与 inode 的早期 hash。入参/返回均无；完成后只具备基础索引，slab、
 * mount tree 等仍须由 vfs_caches_init() 建立。
 */
void __init vfs_caches_init_early(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(in_lookup_hashtable); i++)
		INIT_HLIST_BL_HEAD(&in_lookup_hashtable[i]);

	dcache_init_early();
	inode_init_early();
}

/*
 * vfs_caches_init() —— VFS 层完整初始化，依次建立所有核心数据结构。
 *
 * 早期的 vfs_caches_init_early() 只初始化了 in_lookup_hashtable、
 * dcache/inode 哈希表的静态部分（hashdist=0 时），这里完成剩余工作。
 * 顺序有依赖：mnt_init 内部会调用 shmem_init / init_rootfs，
 * 后者需要 dcache/inode/file 缓存已就绪，所以前四步必须先于 mnt_init。
 */
/*
 * vfs_caches_init() 在核心内存管理可用后的启动阶段完成 VFS 全局缓存和根挂载树
 * 初始化。入参无、无直接返回值；各子初始化失败按其 __init/PANIC 契约处理。
 * 顺序是功能前置条件：名字、dentry、inode 和 file 缓存必须早于 mnt_init()，
 * 块/字符设备索引随后建立。函数只在单线程启动期调用，发布的对象持续整个系统
 * 生命周期，不需要运行期锁来保护本初始化序列。
 */
void __init vfs_caches_init(void)
{
	/*
	 * 创建路径名（struct filename）的 slab 缓存 "names_cache"。
	 * SLAB_HWCACHE_ALIGN：对齐到 cache line，减少伪共享。
	 * kmem_cache_create_usercopy：标记 iname 字段可安全复制到用户空间
	 * （CONFIG_HARDENED_USERCOPY 检查的基础），防止内核堆布局信息泄漏。
	 * 此后 getname() / putname() 可用，路径解析（namei）才能分配路径对象。
	 */
	filename_init();

	/*
	 * 创建 dentry 的 slab 缓存并（若 hashdist=1）分配大哈希表。
	 * dentry 是路径分量（目录项）的内核表示，是路径查找的核心缓存：
	 *   命中 dcache → 无需读磁盘，直接返回 inode；
	 *   未命中     → 调用具体文件系统的 lookup()。
	 * SLAB_RECLAIM_ACCOUNT：可在内存压力下被 shrinker 回收（dcache shrinker）。
	 * ARM64 上 hashdist=1（NUMA 感知），哈希表按 node 分布分配，
	 * 大小由 dhash_entries 命令行参数或默认公式（总内存 / 512 * 2）决定。
	 */
	/*
	 * 修正说明：hashdist 并非由 ARM64 架构固定为 1。当前实现的默认值来自
	 * HASHDIST_DEFAULT，可由 hashdist= 启动参数覆盖；单内存节点还会在
	 * fixup_hashdist() 中强制关闭。这里的真实分支条件只有 hashdist 布尔值。
	 */
	dcache_init();

	/*
	 * 创建 inode 的 slab 缓存并分配 inode 哈希表。
	 * inode 是文件元数据（权限、大小、数据块指针）的内核表示。
	 * 各文件系统在 inode_cachep 基础上嵌入自己的私有字段
	 * （如 ext4_inode_info），通过 container_of 互转。
	 * inode 哈希表以 (superblock, inode number) 为键加速查找，
	 * 避免重复分配同一文件的 inode。
	 */
	inode_init();

	/*
	 * 创建 struct file（filp）和 struct backing_file（bfilp）的 slab 缓存。
	 * struct file 是进程打开文件的描述符内核端，持有读写位置、flags、
	 * f_op 指针等；backing_file 是 overlay/fuse 的底层文件包装。
	 * SLAB_TYPESAFE_BY_RCU：允许在 RCU 读临界区持有裸指针，
	 * 支持 fget_light() 的无锁快速路径（ARM64 高并发场景关键）。
	 * freeptr_offset：slab 复用对象内部的 f_freeptr 字段存放空闲链指针，
	 * 减少额外指针开销，同时防止释放后使用时踩到外部数据。
	 * 同时初始化 nr_files percpu 计数器，用于 /proc/sys/fs/file-nr 统计。
	 */
	files_init();

	/*
	 * 根据当前可用内存动态计算 files_stat.max_files 上限。
	 * 公式：(可用页数 * PAGE_SIZE/1024) / 10，即约 10% 内存用于文件对象。
	 * 保证下限为 NR_FILE（通常 8192），防止小内存设备上限过低。
	 * ARM64 服务器内存大，此值通常在数百万级别。
	 */
	files_maxfiles_init();

	/*
	 * 挂载点子系统完整初始化，是本函数最重量级的一步：
	 *   1. mnt_cache slab       — struct mount（内核挂载实例）缓存；
	 *   2. mount_hashtable      — 以 (vfsmount, dentry) 为键的哈希表，
	 *                             加速 path_walk 中的挂载点穿越；
	 *   3. mountpoint_hashtable — 以 dentry 为键，找出该目录上的所有挂载；
	 *   4. kernfs_init          — kernfs 虚拟文件系统骨架（sysfs 的底层）；
	 *   5. sysfs_init           — 注册 sysfs，/sys 的基础；
	 *   6. shmem_init           — 基于 tmpfs 的匿名共享内存（mmap MAP_SHARED
	 *                             的底层，Android Ashmem / memfd 的前身）；
	 *   7. init_rootfs          — 注册 rootfs（内核内置的 tmpfs 变体）；
	 *   8. init_mount_tree      — 建立初始 VFS 挂载树（rootfs 挂到 "/"），
	 *                             设置 init_task 的 fs->root 和 fs->pwd，
	 *                             从此路径解析有了根节点。
	 */
	mnt_init();

	/*
	 * 初始化块设备的 inode 缓存（bdev_inode = block_device + inode 合体）
	 * 并挂载块设备伪文件系统（blockdev_superblock）。
	 * 块设备通过该伪文件系统持有 inode，writeback 子系统通过
	 * blockdev_superblock 找到脏页并回写，是块 I/O 路径的起点。
	 */
	bdev_cache_init();

	/*
	 * 初始化字符设备映射表（cdev_map），以主设备号为键的 kobj_map。
	 * open() 遇到字符设备节点时通过此表找到对应的 file_operations，
	 * /dev/null、/dev/zero、/dev/tty 等都依赖这张表。
	 */
	chrdev_init();
}
