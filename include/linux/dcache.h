/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_DCACHE_H
#define __LINUX_DCACHE_H

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/math.h>
#include <linux/rculist.h>
#include <linux/rculist_bl.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>
#include <linux/cache.h>
#include <linux/rcupdate.h>
#include <linux/lockref.h>
#include <linux/stringhash.h>
#include <linux/wait.h>

/*
 * dcache 接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5，2026-08-10）。
 *
 * 本文件定义 VFS 目录项缓存的核心对象、状态位、文件系统回调契约以及常用
 * 引用/类型查询接口。dentry 把“父目录中的一个名字”缓存为可复用的路径节点；
 * 它负责名字查找、父子拓扑和 inode 关联，却不拥有文件内容，也不替代 inode、
 * mount 或 path 对象各自的职责。
 *
 * 一条典型路径是：namei 路径遍历构造 qstr → d_lookup()/并行 lookup 在父目录
 * 哈希表查找 → 未命中时 d_alloc_parallel() 建立负 dentry → 文件系统 lookup
 * 用 d_add()/d_splice_alias() 关联 inode → dget()/dput() 管理主动引用；内存压力
 * 下未引用对象进入 LRU，最终从哈希和父子关系摘除，并在需要时等待 RCU 宽限期
 * 后释放。
 *
 * 并发不是由一把锁包办：d_lockref 把每个 dentry 的自旋锁与引用计数放在一起；
 * d_seq 让 RCU-walk 检测名字、父指针等是否在读取期间改变；rename_lock 为跨
 * dentry 的重命名拓扑提供全局序列；父 inode 的 i_lock 保护别名链；RCU 只保证
 * 合规读侧窗口内的存储期，不冻结字段内容，也不等同于持有引用。
 *
 * 这种设计让常见路径查找能复用短名字并走无引用的 RCU 快速路径，代价是字段
 * 布局、状态位和释放协议较复杂。调用者必须根据接口要求选择锁、引用或 seqcount
 * 重试，不能把返回的裸指针带出其保护范围。
 */

/*
 * 这些前置声明只建立类型关系，避免本公共头文件为 path、file 和 vfsmount 的
 * 完整定义引入额外依赖；使用相应字段的实现文件仍需包含定义它们的头文件。
 */
struct path;
struct file;
struct vfsmount;

/*
 * linux/include/linux/dcache.h
 *
 * Dirent cache data structures
 *
 * (C) Copyright 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */

/*
 * IS_ROOT() 判断 dentry 是否为其所在 dentry 树的根：根节点的 d_parent 指回
 * 自己。参数必须是有效、可访问的 dentry；并发重命名场景还需由调用者按路径
 * 遍历协议稳定 d_parent，宏本身不加锁也不取得引用。
 */
#define IS_ROOT(x) ((x) == (x)->d_parent)

/* The hash is always the low bits of hash_len */
/*
 * hash_len 把 32 位哈希和 32 位字节长度压进一个 u64。无论 CPU 字节序如何，
 * hash 都占该 64 位值的低有效位，便于一次装载/比较；字段声明顺序因此随端序
 * 调整。bytemask_from_count() 生成覆盖 cnt 个字节的机器字掩码，供字符串哈希
 * 的尾部处理使用，cnt 必须处于调用算法允许的机器字范围内。
 */
#ifdef __LITTLE_ENDIAN
 #define HASH_LEN_DECLARE u32 hash; u32 len
 #define bytemask_from_count(cnt)	(~(~0ul << (cnt)*8))
#else
 #define HASH_LEN_DECLARE u32 len; u32 hash
 #define bytemask_from_count(cnt)	(~(~0ul >> (cnt)*8))
#endif

/*
 * "quick string" -- eases parameter passing, but more importantly
 * saves "metadata" about the string (ie length and the hash).
 *
 * hash comes first so it snuggles against d_parent in the
 * dentry.
 */
/*
 * qstr 是路径分量的“名字视图”，不拥有 name 指向的字节。len 以字节计且不含
 * 结尾 NUL，hash 是按 VFS/文件系统规则计算的名字哈希；hash_len 允许并行查找
 * 热路径一次复制这两项元数据。hash 字段在布局上靠近 dentry->d_parent，减少
 * 查找热字段跨缓存线的机会。name 的存储期由调用者或承载它的 dentry 保证。
 */
struct qstr {
	/* 两种视图共享存储：按字段访问可读，按 u64 访问适合原子式快照/比较。 */
	union {
		struct {
			HASH_LEN_DECLARE;
		};
		u64 hash_len;
	};
	const unsigned char *name;
};

/*
 * QSTR_INIT() 构造只预置 len 和 name 的初始化器，hash 保持零；QSTR_LEN()
 * 把它转成可用于表达式的复合字面量，QSTR() 再从 NUL 结尾常量计算字节长度。
 * 三者都借用 n 指向的存储，不复制名字；需要真实哈希的查找路径仍须计算 hash。
 */
#define QSTR_INIT(n,l) { { { .len = l } }, .name = n }
#define QSTR_LEN(n,l) (struct qstr)QSTR_INIT(n,l)
#define QSTR(n) QSTR_LEN(n, strlen(n))

/*
 * empty_name、slash_name 和 dotdot_name 是全局只读的特殊路径分量，分别表示
 * 空名、"/" 和 ".."。调用者只借用这些静态对象及其名字存储，不负责释放。
 */
extern const struct qstr empty_name;
extern const struct qstr slash_name;
extern const struct qstr dotdot_name;

/*
 * Try to keep struct dentry aligned on 64 byte cachelines (this will
 * give reasonable cacheline footprint with larger lines without the
 * large memory footprint increase).
 */
/*
 * 内联短名字容量按位宽和 SMP 配置选择，使 struct dentry 尽量落在预期的
 * 128/192 字节档位。短名直接嵌入 dentry 可避免常见名字的额外分配和缓存缺失；
 * 代价是每个 dentry 都固定占用这部分空间。注释中的字节数描述整个 dentry 的
 * 目标尺寸，不是短名字数组自身大小。
 */
#ifdef CONFIG_64BIT
# define DNAME_INLINE_WORDS 5 /* 192 bytes */
#else
# ifdef CONFIG_SMP
#  define DNAME_INLINE_WORDS 9 /* 128 bytes */
# else
#  define DNAME_INLINE_WORDS 11 /* 128 bytes */
# endif
#endif

#define DNAME_INLINE_LEN (DNAME_INLINE_WORDS*sizeof(unsigned long))

/*
 * shortname_store 为同一块内联名字存储提供字节串和机器字两种视图；words 视图
 * 保证自然对齐并支持按字比较。该 union 随 dentry 生灭，名字超出容量时
 * d_name.name 会指向另行分配的存储。
 */
union shortname_store {
	unsigned char string[DNAME_INLINE_LEN];
	unsigned long words[DNAME_INLINE_WORDS];
};

/*
 * d_lock 和 d_iname 是布局别名：前者暴露 lockref 内嵌锁，后者暴露短名字字节
 * 数组。宏不执行同步或边界检查，调用点仍须遵循对应字段的锁与长度协议。
 */
#define d_lock	d_lockref.lock
#define d_iname d_shortname.string
/* completion_list 的布局由 dcache 实现私有；这里只让 dentry 保存可空等待者指针。 */
struct completion_list;

/*
 * struct dentry 表示“父目录中的一个名字”这一缓存节点。它通常由 d_alloc*()
 * 创建，在哈希表和父目录子链发布，经 d_add()/d_instantiate() 关联 inode；
 * 主动引用由 dget()/dput() 管理，零引用对象可留在 LRU，摘除后按普通或 RCU
 * 回收路径销毁。
 *
 * 字段按热路径分组：第一缓存线服务无锁/RCU 名字查找，第二组服务引用式查找，
 * 后部链表维护 LRU、父子和 inode 别名关系。d_lock 保护本对象可变状态；d_seq
 * 使 RCU 读者发现并重试并发 rename；目录拓扑还会与 rename_lock、父 inode
 * 的 i_lock 等外层协议配合。持有引用只保证对象存活，不保证这些字段静止。
 */
struct dentry {
	/* RCU lookup touched fields */
	/* RCU-walk 会读取本组字段；写者必须按 d_lock/d_seq 协议更新并发布。 */
	unsigned int d_flags;		/* protected by d_lock */
	/* d_flags 编码回调能力、生命周期状态和缓存的对象类型；写入受 d_lock 保护。 */
	seqcount_spinlock_t d_seq;	/* per dentry seqlock */
	/* d_seq 不延长生命周期，只让读者检测名字或父指针是否在读取期间发生变化。 */
	struct hlist_bl_node d_hash;	/* lookup hash list */
	/* d_hash 把节点挂入按 parent+name 索引的 dcache 哈希桶，桶锁保护链结构。 */
	struct dentry *d_parent;	/* parent directory */
	/* d_parent 是借用的父节点指针；非根节点的父引用/拓扑协议保证父不会先消失。 */
	union {
	struct qstr __d_name;		/* for use ONLY in fs/dcache.c */
	const struct qstr d_name;
	};
	/* __d_name 仅供 dcache 核心写入；其 const 视图阻止普通调用者绕过 rename 协议。 */
	struct inode *d_inode;		/* Where the name belongs to - NULL is
					 * negative */
	/* d_inode 为 NULL 表示负 dentry；非 NULL 时该名字已实例化为 inode 别名。 */
	union shortname_store d_shortname;
	/* 短名字在此随对象内联保存；长名字由 d_name.name 指向外部分配区。 */
	/* --- cacheline 1 boundary (64 bytes) was 32 bytes ago --- */
	/* 第一条 64 字节缓存线边界已在上方 32 字节处越过，这是布局核对标记。 */

	/* Ref lookup also touches following */
	/* 引用式路径查找在对象已稳定后还会访问本组文件系统扩展字段。 */
	const struct dentry_operations *d_op;
	/* d_op 在初始化阶段选定，随后为文件系统定制比较、重验证和回收等策略。 */
	struct super_block *d_sb;	/* The root of the dentry tree */
	/* d_sb 指向所属超级块；其生命周期由已存在的 dentry/mount 等外层关系约束。 */
	unsigned long d_time;		/* used by d_revalidate */
	/* d_time 是文件系统自定义的重验证时间/版本缓存，单位和解释由 d_op 决定。 */
	void *d_fsdata;			/* fs-specific data */
	/* d_fsdata 的类型、所有权和释放时机由文件系统与 d_release 等回调约定。 */
	/* --- cacheline 2 boundary (128 bytes) --- */
	/* 此处到达从对象起点计算的第二条 64 字节缓存线边界（128 字节）。 */
	struct lockref d_lockref;	/* per-dentry lock and refcount
					 * keep separate from RCU lookup area if
					 * possible!
					 */
	/*
	 * lockref 将 d_lock 与主动引用计数合并，允许架构支持时优化 get/put 热路径；
	 * 同时尽量让它与前面的 RCU lookup 热字段分处缓存线，减少写锁/计数造成干扰。
	 */

	struct list_head d_lru;		/* LRU list */
	/* d_lru 仅在相应状态位成立时链接到超级块 LRU 或临时 shrink 列表。 */
	struct hlist_node d_sib;	/* child of parent list */
	/* d_sib 把本节点挂入 d_parent->d_children，拓扑修改须遵守 dcache 锁序。 */
	struct hlist_head d_children;	/* our children */
	/* d_children 是直接子节点链表头；遍历者必须持有接口要求的锁或稳定条件。 */
	/*
	 * the following members can share memory - their uses are
	 * mutually exclusive.
	 */
	/*
	 * 这组 union 成员对应互斥生命周期阶段，因此可共享空间：正 dentry 链接
	 * inode 别名；并行 lookup 中的活负 dentry 链接在查找中哈希；被杀死对象
	 * 借 d_rcu 延迟释放；极少数 shrink 竞态中的负 dentry 保存等待者链。
	 */
	union {
		/* positives: inode alias list */
		/* d_alias 受 inode->i_lock 保护，把同一 inode 的多个名字串在一起。 */
		struct hlist_node d_alias;
		/* in-lookup ones (all negative, live): hash chain */
		/* d_in_lookup_hash 只在 DCACHE_PAR_LOOKUP 阶段有效，用于合并同名并行查找。 */
		struct hlist_bl_node d_in_lookup_hash;
		/* killed ones: (already negative) used to schedule freeing */
		/* d_rcu 在对象已从可发现结构摘除后排队，宽限期结束才允许释放其存储。 */
	 	struct rcu_head d_rcu;
		/*
		 * live non-in-lookup negatives: used if shrink_dcache_tree()
		 * races with eviction by another thread and needs to wait for
		 * this dentry to get killed .  Remains NULL for almost all
		 * negative dentries.
		 */
		/*
		 * waiters 仅在 shrink_dcache_tree() 与另一线程驱逐同一负 dentry 时建立；
		 * 等待者不取得该 union 的其他语义，完成通知后也不能继续使用裸指针。
		 */
		struct completion_list *waiters;
	};
};

/*
 * dentry->d_lock spinlock nesting subclasses:
 *
 * 0: normal
 * 1: nested
 */
/*
 * lockdep 子类描述同一路径上获取多把 d_lock 的合法嵌套关系。NORMAL 用于普通
 * 单锁；NESTED 用于调用者已证明层级顺序的子对象锁。它只帮助锁依赖检查，
 * 不改变运行时锁语义，也不能修复实际的反向锁序。
 */
enum dentry_d_lock_class
{
	DENTRY_D_LOCK_NORMAL, /* implicitly used by plain spin_lock() APIs. */
	DENTRY_D_LOCK_NESTED
};

/*
 * d_real_type 告诉叠加文件系统调用 d_real() 时要解析承载数据的下层对象，还是
 * 承载元数据的对象；普通文件系统两种请求都直接返回原 dentry。
 */
enum d_real_type {
	D_REAL_DATA,
	D_REAL_METADATA,
};

/*
 * dentry_operations 是文件系统为 dcache 提供的策略表。通常在分配/初始化 dentry
 * 时由 set_default_d_op() 或文件系统设置，随后只读；具体回调的锁和睡眠规则以
 * Documentation/filesystems/locking.rst 为准。
 *
 * revalidate/weak_revalidate 判断缓存名字是否仍可信；hash/compare 实现特殊名字
 * 规范；delete/prune/release/iput 参与回收；dname 生成显示路径；automount/manage
 * 处理受管路径；real 穿透叠加层；unalias_trylock/unlock 为别名调整提供文件系统
 * 锁协议。返回负 errno、布尔策略或对象指针的精确含义由各回调契约规定。
 */
struct dentry_operations {
	/*
	 * d_revalidate(parent_inode, name, dentry, flags)：路径查找时验证缓存项；
	 * 返回正值为有效、0 为失效、负值为 errno。前三个对象均借用，flags 来自
	 * lookup 语境，回调可更新文件系统私有验证状态。
	 * d_weak_revalidate(dentry, flags)：在较弱语境验证已有引用，返回类别同上。
	 */
	int (*d_revalidate)(struct inode *, const struct qstr *,
			    struct dentry *, unsigned int);
	int (*d_weak_revalidate)(struct dentry *, unsigned int);
	/*
	 * d_hash(parent, name)：按文件系统规则规范化/重算可写 qstr 的 hash，返回 0
	 * 或负 errno；parent 借用。d_compare(parent, len, str, name) 比较候选字节串
	 * 与缓存 qstr，返回 0 表示同名、非零表示不同，所有指针均只在回调期借用。
	 */
	int (*d_hash)(const struct dentry *, struct qstr *);
	int (*d_compare)(const struct dentry *,
			unsigned int, const char *, const struct qstr *);
	/*
	 * d_delete(dentry) 在最后引用释放附近决定是否立即删除缓存；d_init(dentry)
	 * 初始化新对象的文件系统私有状态并返回 0/负 errno。参数均为借用，失败时
	 * dcache 负责撤销尚未发布的新对象。
	 */
	int (*d_delete)(const struct dentry *);
	int (*d_init)(struct dentry *);
	/*
	 * d_release(dentry) 在对象最终回收前释放 d_fsdata；d_prune(dentry) 在从 LRU
	 * 摘除时通知文件系统；d_iput(dentry, inode) 取代默认 iput 策略。它们无直接
	 * 返回值，输入为正在退出相应生命周期阶段的借用对象，所有权依具体契约释放。
	 */
	void (*d_release)(struct dentry *);
	void (*d_prune)(struct dentry *);
	void (*d_iput)(struct dentry *, struct inode *);
	/*
	 * d_dname(dentry, buf, len) 为特殊对象生成显示名，返回 buf 内指针或错误指针；
	 * d_automount(path) 在受管路径触发挂载，返回带引用的 vfsmount 或错误指针；
	 * d_manage(path, rcu_walk) 控制 transit，返回 0、负 errno 或要求退回慢路的值。
	 */
	char *(*d_dname)(struct dentry *, char *, int);
	struct vfsmount *(*d_automount)(struct path *);
	int (*d_manage)(const struct path *, bool);
	/*
	 * d_real(dentry, type) 返回叠加层实际承载数据/元数据的借用 dentry；
	 * d_unalias_trylock(dentry) 尝试取得文件系统别名调整所需锁并返回是否成功，
	 * d_unalias_unlock(dentry) 成对释放。三者不改变调用者的 dentry 引用所有权。
	 */
	struct dentry *(*d_real)(struct dentry *, enum d_real_type type);
	bool (*d_unalias_trylock)(const struct dentry *);
	void (*d_unalias_unlock)(const struct dentry *);
} ____cacheline_aligned;

/*
 * Locking rules for dentry_operations callbacks are to be found in
 * Documentation/filesystems/locking.rst. Keep it updated!
 *
 * FUrther descriptions are found in Documentation/filesystems/vfs.rst.
 * Keep it updated too!
 */
/*
 * 上述文档是回调锁规则和 VFS 语义的权威契约；新增回调或改变调用上下文时必须
 * 同步更新。这里的操作表声明本身不能表达“调用时持有哪些锁、能否睡眠”。
 */

/* d_flags entries */
/*
 * dentry_flags 将三类信息压入 d_flags：已安装的 d_op 能力位、生命周期/路径状态
 * 位，以及 19..21 位的互斥对象类型。能力位避免热路径反复检查 NULL 回调；状态
 * 修改一般需 d_lock，并在 RCU 可见字段变化时配合 d_seq。读取者必须按所在路径
 * 使用 READ_ONCE、锁或 seqcount 重试，不能假定普通加载构成稳定快照。
 */
enum dentry_flags {
	/* DCACHE_OP_* 位缓存相应 d_op 回调是否存在，避免路径热流逐个解引用操作表。 */
	DCACHE_OP_HASH			= BIT(0),
	DCACHE_OP_COMPARE		= BIT(1),
	DCACHE_OP_REVALIDATE		= BIT(2),
	DCACHE_OP_DELETE		= BIT(3),
	DCACHE_OP_PRUNE			= BIT(4),
	/*
	 * This dentry is possibly not currently connected to the dcache tree,
	 * in which case its parent will either be itself, or will have this
	 * flag as well.  nfsd will not use a dentry with this bit set, but will
	 * first endeavour to clear the bit either by discovering that it is
	 * connected, or by performing lookup operations.  Any filesystem which
	 * supports nfsd_operations MUST have a lookup function which, if it
	 * finds a directory inode with a DCACHE_DISCONNECTED dentry, will
	 * d_move that dentry into place and return that dentry rather than the
	 * passed one, typically using d_splice_alias.
	 */
	/*
	 * DCACHE_DISCONNECTED 标记尚未接回命名树的别名，常见于按文件句柄找到的 NFS
	 * 对象。支持 nfsd 的文件系统 lookup 必须优先把已有断连目录 d_move() 到正确
	 * 位置并返回它，避免同一目录 inode 出现两个彼此独立的目录 dentry。
	 */
	DCACHE_DISCONNECTED		= BIT(5),
	/* REFERENCED 提供 LRU 二次机会；DONTCACHE 要求最后 dput 时不保留缓存。 */
	DCACHE_REFERENCED		= BIT(6),	/* Recently used, don't discard. */
	DCACHE_DONTCACHE		= BIT(7),	/* Purge from memory on final dput() */
	/* CANT_MOUNT 禁止成为挂载点；LOOKUP_WAITERS/PAR_LOOKUP 配对描述并行查找与等待。 */
	DCACHE_CANT_MOUNT		= BIT(8),
	DCACHE_LOOKUP_WAITERS		= BIT(9),	/* A thread is waiting for
							 * PAR_LOOKUP to clear
							 */
	/* SHRINK_LIST 表示对象已被某个回收批次领取，避免重复加入临时释放链。 */
	DCACHE_SHRINK_LIST		= BIT(10),
	DCACHE_OP_WEAK_REVALIDATE	= BIT(11),
	/*
	 * this dentry has been "silly renamed" and has to be deleted on the
	 * last dput()
	 */
	/* DCACHE_NFSFS_RENAMED 记录 NFS silly-rename，最后一个引用释放时才删除临时名。 */
	DCACHE_NFSFS_RENAMED		= BIT(12),
	/* FSNOTIFY_PARENT_WATCHED 缓存“父 inode 被某个 fsnotify 监听者监视”。 */
	DCACHE_FSNOTIFY_PARENT_WATCHED	= BIT(13),	/* Parent inode is watched by some fsnotify listener */
	/* KILLED 表示已进入销毁；MOUNTED/NEED_AUTOMOUNT/MANAGE_TRANSIT 驱动受管路径慢路。 */
	DCACHE_DENTRY_KILLED		= BIT(14),
	DCACHE_MOUNTED			= BIT(15),	/* is a mountpoint */
	DCACHE_NEED_AUTOMOUNT		= BIT(16),	/* handle automount on this dir */
	DCACHE_MANAGE_TRANSIT		= BIT(17),	/* manage transit from this dirent */
	DCACHE_LRU_LIST			= BIT(18),
	/* LRU_LIST 表示 d_lru 正在缓存回收链；ENTRY_TYPE 掩码的七种编码如下。 */
	DCACHE_ENTRY_TYPE		= (7 << 19),	/* bits 19..21 are for storing type: */
	/* MISS 为负项；WHITEOUT 阻止下层穿透；DIRECTORY/AUTODIR 为两种目录语义。 */
	DCACHE_MISS_TYPE		= (0 << 19),	/* Negative dentry */
	DCACHE_WHITEOUT_TYPE		= (1 << 19),	/* Whiteout dentry (stop pathwalk) */
	DCACHE_DIRECTORY_TYPE		= (2 << 19),	/* Normal directory */
	DCACHE_AUTODIR_TYPE		= (3 << 19),	/* Lookupless directory (presumed automount) */
	/* REGULAR、SPECIAL、SYMLINK 分别编码普通文件、其他特殊文件和符号链接。 */
	DCACHE_REGULAR_TYPE		= (4 << 19),	/* Regular file type */
	DCACHE_SPECIAL_TYPE		= (5 << 19),	/* Other file type */
	DCACHE_SYMLINK_TYPE		= (6 << 19),	/* Symlink */
	/* NOKEY_NAME 表示加密名因缺密钥而编码；OP_REAL 缓存 d_real 回调存在。 */
	DCACHE_NOKEY_NAME		= BIT(22),	/* Encrypted name encoded without key */
	DCACHE_OP_REAL			= BIT(23),
	/* PAR_LOOKUP 表示父目录以共享锁并行执行文件系统 lookup，竞争者可能等待完成。 */
	DCACHE_PAR_LOOKUP		= BIT(24),	/* being looked up (with parent locked shared) */
	DCACHE_DENTRY_CURSOR		= BIT(25),
	/* CURSOR 是 readdir 游标；NORCU 允许立即释放；PERSISTENT 用额外引用钉住对象。 */
	DCACHE_NORCU			= BIT(26),	/* No RCU delay for freeing */
	DCACHE_PERSISTENT		= BIT(27)
};

/* 受管 dentry 的三个状态合并为一次热路径测试：挂载点、自动挂载和 transit 管理。 */
#define DCACHE_MANAGED_DENTRY \
	(DCACHE_MOUNTED|DCACHE_NEED_AUTOMOUNT|DCACHE_MANAGE_TRANSIT)

/*
 * rename_lock 是全局重命名序列锁：写侧跨 dentry 改变父子拓扑时推进序列，RCU
 * 路径读取者据此发现整条路径可能失效并重试。它不是 dentry 生命周期引用，也
 * 不替代单个 d_lock 对字段写入的保护。
 */
extern seqlock_t rename_lock;

/*
 * These are the low-level FS interfaces to the dcache..
 */
/*
 * 以下是文件系统与 dcache 核心之间的低层接口；它们直接改变 inode 关联、哈希
 * 可见性或对象生命周期，调用者必须遵守各接口的锁和引用契约。
 */
/*
 * d_instantiate() 把已分配的负 dentry 与可空 inode 关联并加入 inode 别名链。
 * dentry 为借用；非 NULL inode 必须带一份预先取得、成功后转交 dcache 的引用。
 * 不返回错误、不负责 rehash，调用者随后按需要 d_rehash()。
 */
extern void d_instantiate(struct dentry *, struct inode *);
/*
 * d_instantiate_new() 用于 I_NEW/I_CREATING inode 的首次实例化：消费 inode 引用，
 * 绑定借用 dentry，以写屏障发布初始化后清状态并唤醒等待者。无直接返回值、不
 * 负责 rehash，失败必须在调用前处理。
 */
extern void d_instantiate_new(struct dentry *, struct inode *);
/*
 * __d_drop() 在调用者已满足底层锁条件时把 dentry 从查找哈希摘除；它不释放
 * dentry、不减少调用者引用，也不自行取得 d_lock，供 dcache 内部组合操作使用。
 */
extern void __d_drop(struct dentry *dentry);
/*
 * d_drop() 是带 d_lock 的安全包装，将非 NULL、由调用者借用/持有的 dentry 从
 * 哈希摘除，使后续名字查找不再命中；对象仍由现有引用维持存活。
 */
extern void d_drop(struct dentry *dentry);
/*
 * d_delete() 处理 unlink 后的缓存状态：根据引用情况把 dentry 变负或直接摘除。
 * 参数是活 dentry 的借用指针；无返回值，但会改变哈希、inode 关联及类型状态。
 */
extern void d_delete(struct dentry *);

/* allocate/de-allocate */
/* 以下接口分配或取得 dentry；返回指针通常携带一个需由 dput() 配对的引用。 */
/*
 * d_alloc() 在 parent 下为 name 分配尚未哈希的负 dentry。parent/name 均非 NULL
 * 且仅借用，名字会复制到新对象；成功返回持有引用，内存不足返回 NULL。
 */
extern struct dentry * d_alloc(struct dentry *, const struct qstr *);
/*
 * d_alloc_anon() 为给定超级块创建不连接到普通父目录名字树的匿名 dentry；
 * super_block 为借用指针，成功返回持有引用，失败返回 NULL。
 */
extern struct dentry * d_alloc_anon(struct super_block *);
/*
 * d_alloc_parallel() 合并同一 parent/name 的并行 lookup：返回新建的
 * DCACHE_PAR_LOOKUP dentry 或等待并取得已有结果。调用可能睡眠；成功引用需 dput，
 * 分配失败返回 ERR_PTR(-ENOMEM)，名字与 parent 仅在调用期借用。返回的新执行者
 * 必须由调用者用 d_add()/d_lookup_done() 一类接口完成并唤醒同名等待者。
 */
extern struct dentry * d_alloc_parallel(struct dentry *, const struct qstr *);
/*
 * d_splice_alias() 把 lookup 得到的 inode 接到给定负 dentry，目录 inode 若已有
 * 断连别名则移动并返回那个别名。inode 可为 NULL，其引用无论结果都由函数消费；
 * 成功可能返回 NULL（使用传入 dentry）或持有引用的替代 dentry，失败返回
 * ERR_PTR，传入 dentry 引用仍由调用者按 ->lookup 契约处理。
 */
extern struct dentry * d_splice_alias(struct inode *, struct dentry *);
/* weird procfs mess; *NOT* exported */
/*
 * d_splice_alias_ops() 是 procfs 所需、未导出的特殊版本，额外把 ops 安装到最终
 * dentry；参数与返回类别同 d_splice_alias()，ops 为静态策略表的借用指针。
 */
extern struct dentry * d_splice_alias_ops(struct inode *, struct dentry *,
					  const struct dentry_operations *);
/*
 * d_add_ci() 为大小写不敏感查找选择实际拼写并实例化 inode。dentry/name 借用，
 * inode 引用被本函数消费；返回最终使用且带引用的 dentry 或错误指针，可能不等于
 * 输入，调用者不论成功失败都不得再单独 iput 该 inode 引用。
 */
extern struct dentry * d_add_ci(struct dentry *, struct inode *, struct qstr *);
/*
 * d_same_name() 在 parent 的命名规则下比较 dentry 与 name；三者均为只读借用，
 * 返回 true 表示长度与文件系统比较规则均匹配，不取得引用也不改变缓存。
 */
extern bool d_same_name(const struct dentry *dentry, const struct dentry *parent,
			const struct qstr *name);
/*
 * d_find_any_alias() 在 inode 的别名链查找任意正 dentry；inode 为借用输入，成功
 * 返回带引用的别名，未找到返回 NULL。内部按 i_lock 协议稳定别名链。
 */
extern struct dentry *d_find_any_alias(struct inode *inode);
/*
 * d_obtain_alias() 为按句柄取得的 inode 查找或创建断连别名，并接管传入 inode
 * 引用；成功返回带引用的 dentry。NULL 输入变为 ERR_PTR(-ESTALE)，错误指针原样
 * 传播，分配失败返回 ERR_PTR(-ENOMEM)；所有出口都消费输入引用，调用者不得 iput。
 */
extern struct dentry * d_obtain_alias(struct inode *);
/*
 * d_obtain_root() 与 d_obtain_alias() 类似，但保证得到适合作为树根的 dentry；
 * 输入 inode 引用被消费，返回持有的 dentry；NULL、错误指针和 -ENOMEM 的处理
 * 类别与 d_obtain_alias() 相同，新建根会登记到超级块次级根集合。
 */
extern struct dentry * d_obtain_root(struct inode *);
/*
 * shrink_dcache_sb() 扫描并回收给定超级块上可释放的 dentry。sb 为借用输入；
 * 可能睡眠并触发批量摘除，无直接返回值，也不保证所有有引用对象都被清空。
 */
extern void shrink_dcache_sb(struct super_block *);
/*
 * shrink_dcache_parent() 回收 parent 子树中未使用的后代；parent 在调用期须存活，
 * 函数可能睡眠，无直接返回值，仍被引用或不可回收的节点会保留。
 */
extern void shrink_dcache_parent(struct dentry *);
/*
 * d_invalidate() 使 dentry 及可回收子树不再作为有效缓存命中；参数为活对象借用，
 * 函数可能执行收缩和摘除，无直接返回值，现有外部引用仍各自负责 dput()。
 */
extern void d_invalidate(struct dentry *);

/* only used at mount-time */
/*
 * d_make_root() 只在挂载建根阶段把 inode 包装成根 dentry；它消费 inode 引用，
 * 成功返回持有引用的自父指根节点，分配失败返回 NULL 并释放输入 inode。
 */
extern struct dentry * d_make_root(struct inode *);

/*
 * d_mark_tmpfile() 为尚未实例化、unlinked 且使用内联名的 file dentry 写入
 * “#inode号”调试名字；它不 rehash，file/inode 均借用，无直接返回值。
 */
extern void d_mark_tmpfile(struct file *, struct inode *);
/*
 * d_mark_tmpfile_name() 以指定短 @name 替换 tmpfile 调试名；两参数均借用，成功
 * 返回 0，状态不符返回 -EINVAL，超过内联容量返回 -ENAMETOOLONG；不分配、不
 * rehash，也不改变 file/dentry 引用。
 */
int d_mark_tmpfile_name(struct file *file, const struct qstr *name);
/*
 * d_tmpfile() 完成匿名临时文件：减少 @inode 链接数、设置调试名并把该 inode
 * 引用转交 d_instantiate()。file 借用、inode 引用被消费；无返回值，成功后其
 * dentry 为正但仍未哈希，主要由 file 引用可达。
 */
extern void d_tmpfile(struct file *, struct inode *);

/*
 * d_find_alias() 优先为 inode 查找合适的已连接别名；成功返回带引用的 dentry，
 * 无别名返回 NULL。inode 只在调用期借用，别名链由 i_lock 协议保护。
 */
extern struct dentry *d_find_alias(struct inode *);
/*
 * d_prune_aliases() 摘除 inode 上可回收的别名缓存；inode 为借用输入，可能释放
 * dentry，无返回值，仍被引用的别名保持有效。
 */
extern void d_prune_aliases(struct inode *);
/*
 * __move_to_shrink_list() 在底层回收锁协议下尝试把 dentry 移入调用者提供的临时
 * list；二者均借用，返回 true 表示已领取待回收对象，false 表示状态不允许。
 */
extern bool __move_to_shrink_list(struct dentry *, struct list_head *);
/*
 * shrink_dentry_list() 消费临时 shrink 列表中的待回收节点，逐个摘除并释放；
 * list 由调用者提供且调用后被清空，无直接返回值，过程可能等待并发驱逐完成。
 */
extern void shrink_dentry_list(struct list_head *);

/*
 * d_find_alias_rcu() 在已持有 RCU 读锁且遵守 inode 别名协议时返回裸别名指针；
 * 未找到返回 NULL。它不增加引用，退出保护窗口后不得继续解引用结果。
 */
extern struct dentry *d_find_alias_rcu(struct inode *);

/* test whether we have any submounts in a subdir tree */
/*
 * path_has_submounts() 检查 path 所在子树是否包含子挂载；path 是只读借用输入，
 * 返回非零表示存在，0 表示未发现。检查按 mount/dcache 遍历协议运行并可能睡眠。
 */
extern int path_has_submounts(const struct path *);

/*
 * This adds the entry to the hash queues.
 */
/*
 * d_rehash() 把已完成初始化的 dentry 发布到名字查找哈希，使并发路径遍历开始可
 * 发现它；参数为活对象借用，无返回值，不额外转移调用者持有的引用。
 */
extern void d_rehash(struct dentry *);
 
/*
 * d_add() 是常规 lookup 完成入口：把借用的负 dentry 与可空 inode 关联并哈希
 * 发布。非 NULL inode 的引用转交 dcache；无直接返回值，NULL inode 发布可缓存
 * 的负查找结果，调用者仍负责自己持有的 dentry 引用。
 */
extern void d_add(struct dentry *, struct inode *);

/* used for rename() and baskets */
/*
 * d_move() 为 rename 和临时“篮子”操作把 source 移到 target 的名字/父位置；
 * 两者均须是活 dentry，函数会改变拓扑和哈希可见性但不消费调用者引用。
 */
extern void d_move(struct dentry *, struct dentry *);
/*
 * d_exchange() 原子式交换两个活 dentry 的父位置和名字，服务 RENAME_EXCHANGE；
 * 无返回值，调用者须先满足 rename 锁序，现有引用仍指向各自对象而非路径位置。
 */
extern void d_exchange(struct dentry *, struct dentry *);
/*
 * d_ancestor() 判断第一个 dentry 是否是第二个的祖先；参数均为借用指针，成功
 * 返回关系链上的直接后继裸指针，否则返回 NULL，调用者须在拓扑稳定窗口内使用。
 */
extern struct dentry *d_ancestor(struct dentry *, struct dentry *);

/*
 * d_lookup() 按 parent+name 在哈希中做引用式查找；参数为只读借用，命中返回带
 * 引用的 dentry，未命中返回 NULL。它处理并发 rename/哈希变化，调用者最终 dput。
 */
extern struct dentry *d_lookup(const struct dentry *, const struct qstr *);

/*
 * d_count() 读取当前 lockref 计数用于诊断或在外层锁保证下判断；dentry 是只读
 * 借用，返回瞬时引用数。该普通加载不提供并发稳定快照，不能据此尝试“复活”对象。
 */
static inline unsigned d_count(const struct dentry *dentry)
{
	return dentry->d_lockref.count;
}

/*
 * d_parent_ino() 在适当的 dentry 同步下取得父目录 inode 号；dentry 为借用输入，
 * 返回 ino_t 值而非 inode 引用，不改变父节点或引用计数。
 */
ino_t d_parent_ino(struct dentry *dentry);

/*
 * helper function for dentry_operations.d_dname() members
 */
/*
 * dynamic_dname() 帮助 d_dname 回调把 printf 风格文本从缓冲区尾部生成路径名。
 * 缓冲区/长度为可写输出范围，格式串及变参为输入；成功返回缓冲区内指针，空间
 * 不足返回错误指针。__printf(3,4) 让编译器校验格式串与变参。
 */
extern __printf(3, 4)
char *dynamic_dname(char *, int, const char *, ...);

/*
 * 以下路径渲染函数把借用的 path/dentry 写入调用者缓冲区（长度以字节计），
 * 成功返回缓冲区内部的起始指针，失败返回 ERR_PTR；它们不为结果另行分配内存。
 * __d_path() 受 root 边界约束，d_absolute_path()/d_path() 处理绝对/用户可见路径，
 * dentry_path_raw()/dentry_path() 只沿 dentry 父链，后者还提供面向显示的处理。
 */
/*
 * __d_path() 以第二个借用 path 作为 root 边界渲染第一个 path；buf/len 是可写
 * 输出区及字节容量。成功返回 buf 内起点，路径不在 root 下可返回 NULL，空间或
 * 遍历失败返回错误指针；不取得或转移 path 引用。
 */
extern char *__d_path(const struct path *, const struct path *, char *, int);
/*
 * d_absolute_path() 将 path 渲染为不受进程 root 截断的绝对路径；path 只读借用，
 * buf/len 是输出区及字节容量，返回区内起点或 ERR_PTR，不取得 path 引用。
 */
extern char *d_absolute_path(const struct path *, char *, int);
/*
 * d_path() 生成面向用户的路径，考虑进程根、挂载以及 d_dname 回调；path 借用，
 * buf/len 为输出区，返回区内指针或错误指针，调用可能获取路径相关锁并重试。
 */
extern char *d_path(const struct path *, char *, int);
/*
 * dentry_path_raw() 仅沿 dentry 父链生成原始名字，不进行面向用户的删除标记修饰；
 * dentry 借用，buf/len 为输出区，返回区内起点或 ERR_PTR。
 */
extern char *dentry_path_raw(const struct dentry *, char *, int);
/*
 * dentry_path() 生成适合显示的 dentry 路径并处理已删除状态；输入对象借用，
 * 输出落在调用者缓冲区，成功返回其中的起始地址，失败返回错误指针。
 */
extern char *dentry_path(const struct dentry *, char *, int);

/* Allocation counts.. */
/* 以下辅助函数围绕 d_lockref 增减或观察主动引用计数。 */

/**
 * dget_dlock -	get a reference to a dentry
 * @dentry: dentry to get a reference to
 *
 * Given a live dentry, increment the reference count and return the dentry.
 * Caller must hold @dentry->d_lock.  Making sure that dentry is alive is
 * caller's resonsibility.  There are many conditions sufficient to guarantee
 * that; e.g. anything with non-negative refcount is alive, so's anything
 * hashed, anything positive, anyone's parent, etc.
 */
/*
 * dget_dlock() 在调用者已经持有 dentry->d_lock 时取得一个引用。@dentry 必须
 * 非 NULL，且已由哈希、正关联、父子关系或非负引用等条件证明仍存活；受锁且
 * 尚未进入死亡协议的零引用缓存项也可在这里重新变活。函数直接递增计数并返回
 * 同一指针，新增引用最终必须 dput()；它不睡眠、没有失败返回，但不能用于引用
 * 已为负数或仅由不受保护裸指针指向的垂死对象。
 */
static inline struct dentry *dget_dlock(struct dentry *dentry)
{
	dentry->d_lockref.count++;
	return dentry;
}


/**
 * dget - get a reference to a dentry
 * @dentry: dentry to get a reference to
 *
 * Given a dentry or %NULL pointer increment the reference count
 * if appropriate and return the dentry.  A dentry will not be
 * destroyed when it has references.  Conversely, a dentry with
 * no references can disappear for any number of reasons, starting
 * with memory pressure.  In other words, that primitive is
 * used to clone an existing reference; using it on something with
 * zero refcount is a bug.
 *
 * NOTE: it will spin if @dentry->d_lock is held.  From the deadlock
 * avoidance point of view it is equivalent to spin_lock()/increment
 * refcount/spin_unlock(), so calling it under @dentry->d_lock is
 * always a bug; so's calling it under ->d_lock on any of its descendents.
 *
 */
/*
 * dget() 复制调用者已经持有的 dentry 引用；@dentry 可为 NULL，非 NULL 时必须
 * 已有正当存活保证，返回同一指针并新增一个需 dput() 的引用，NULL 原样返回。
 * lockref_get() 内部等价于取得 d_lock、递增再解锁，因此可能自旋但不会睡眠；
 * 在本 dentry 或任一后代的 d_lock 下调用会形成锁递归/反向锁序，是死锁错误。
 * 零引用对象可能因内存压力等原因随时消失，所以本接口只能“复制引用”，不能
 * 从未经保护的裸指针获取首个引用。
 */
static inline struct dentry *dget(struct dentry *dentry)
{
	if (dentry)
		lockref_get(&dentry->d_lockref);
	return dentry;
}

/* dentry->d_inode->i_lock must be held by caller */
/*
 * dget_alias_ilocked() 在调用者持有 dentry->d_inode->i_lock、因而稳定 inode 别名
 * 链时尝试取得引用。普通可 RCU 延迟释放的 dentry 可直接 lockref_get()；NORCU
 * 对象若引用已归零就可能立即回收，必须再持 d_lock 检查 count，只允许正计数时
 * 调用 dget_dlock()。@dentry 非 NULL且为借用输入；成功返回 true 并交付一个需
 * dput() 的引用，false 表示不能安全领取。函数只自旋、不睡眠。
 */
static inline bool dget_alias_ilocked(struct dentry *dentry)
{
	/* READ_ONCE 只防止编译器合并/撕裂本次标志读取，不提供整个函数的字段快照。 */
	if (likely(!(READ_ONCE(dentry->d_flags) & DCACHE_NORCU))) {
		lockref_get(&dentry->d_lockref);
		return true;
	}
	// NORCU dentries with zero refcount MUST NOT be grabbed
	/* NORCU dentry 不经 RCU 延迟释放；计数为零时绝不能重新领取，否则会发生 UAF。 */
	spin_lock(&dentry->d_lock);
	if (dentry->d_lockref.count > 0) {
		dget_dlock(dentry);
		spin_unlock(&dentry->d_lock);
		return true;
	}
	spin_unlock(&dentry->d_lock);
	return false;
}

/*
 * dget_parent() 在并发 rename 下稳定读取 parent 并取得引用；@dentry 为活对象借用，
 * 成功总是返回一个需 dput() 的父 dentry（根节点可能返回自身），过程只使用锁。
 */
extern struct dentry *dget_parent(struct dentry *dentry);

/**
 * d_unhashed - is dentry hashed
 * @dentry: entry to check
 *
 * Returns true if the dentry passed is not currently hashed.
 */
/*
 * d_unhashed() 检查 @dentry 当前是否未链接到名字查找哈希。参数是只读借用，非零
 * 表示当前未哈希；这不等于对象已删除或为负，也不延长生命周期。并发修改时调用
 * 者仍需 d_lock、RCU/seqcount 或其他上层稳定条件来解释这个瞬时结果。
 */
static inline int d_unhashed(const struct dentry *dentry)
{
	return hlist_bl_unhashed(&dentry->d_hash);
}

/*
 * d_unlinked() 识别“未哈希且不是树根”的已断链节点；@dentry 为只读借用，返回
 * 非零表示普通名字查找不再能发现它。结果不取得引用，也不表示内存已经释放。
 */
static inline int d_unlinked(const struct dentry *dentry)
{
	return d_unhashed(dentry) && !IS_ROOT(dentry);
}

/*
 * cant_mount() 读取禁止挂载状态；@dentry 借用，返回 DCACHE_CANT_MOUNT 位值。
 * 调用者只需布尔判断时可直接使用；需要与并发写形成稳定判断时必须遵循外层锁。
 */
static inline int cant_mount(const struct dentry *dentry)
{
	return (dentry->d_flags & DCACHE_CANT_MOUNT);
}

/*
 * dont_mount() 在 @dentry 的自旋锁下原子地设置 DCACHE_CANT_MOUNT，使后续挂载检查
 * 拒绝把它作为挂载点。参数为活对象借用；无返回值、不改变引用，且不能睡眠。
 */
static inline void dont_mount(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	dentry->d_flags |= DCACHE_CANT_MOUNT;
	spin_unlock(&dentry->d_lock);
}

/*
 * __d_lookup_unhash_wake() 完成并行 lookup 的慢路径：从 in-lookup 哈希摘除 dentry、
 * 清理状态并唤醒同名等待者。参数为活对象借用；调用条件由 d_lookup_done() 保证。
 */
extern void __d_lookup_unhash_wake(struct dentry *dentry);

/*
 * d_in_lookup() 测试 @dentry 是否仍处于 DCACHE_PAR_LOOKUP 构造/文件系统查找阶段；
 * 返回位值，不取引用。并发精确性依赖调用者已有的 lookup 或锁协议。
 */
static inline int d_in_lookup(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_PAR_LOOKUP;
}

/*
 * d_lookup_done() 在文件系统完成并行 lookup 后结束发布协议。普通 dentry 走快速
 * 空操作；PAR_LOOKUP 对象进入 __d_lookup_unhash_wake() 清状态并唤醒竞争查找者。
 * @dentry 为活对象借用；无直接返回值，慢路径可能涉及等待队列同步但不转移引用。
 */
static inline void d_lookup_done(struct dentry *dentry)
{
	if (unlikely(d_in_lookup(dentry)))
		__d_lookup_unhash_wake(dentry);
}

/*
 * dput() 释放调用者持有的一个 dentry 引用。计数仍非零时通常快速返回；最后一个
 * 引用可能触发 LRU 缓存、d_delete 回调、摘除、父引用级联释放及 RCU 延迟回收，
 * 因而可能执行复杂慢路径。返回后调用者不得再使用该引用；无直接返回值。
 */
extern void dput(struct dentry *);

/*
 * d_managed() 汇总测试挂载点、自动挂载和 transit 管理三类路径陷阱；@dentry
 * 只读借用，true 表示 pathwalk 必须进入 managed-dentry 慢路径，不取得引用。
 */
static inline bool d_managed(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_MANAGED_DENTRY;
}

/*
 * d_mountpoint() 测试 @dentry 的 DCACHE_MOUNTED 缓存位；返回 true 只说明需要查询
 * mount 表，不单独证明某个具体挂载仍存在。参数借用，稳定性由路径遍历协议保证。
 */
static inline bool d_mountpoint(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_MOUNTED;
}

/*
 * Directory cache entry type accessor functions.
 */
/*
 * 下列访问器只解释 d_flags 的互斥类型位，不读取 inode 模式，也不取得引用。
 * 调用者必须先保证 dentry 存活；若类型可能并发变化，还需 d_lock 或 RCU-walk
 * 的 seqcount 验证。它们作为纯内联判断，不睡眠且没有错误返回。
 */
/* __d_entry_type() 返回原始 19..21 位编码，供后续谓词统一比较。 */
static inline unsigned __d_entry_type(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_ENTRY_TYPE;
}

/* d_is_miss() 判断类型位是否为普通负查找结果。 */
static inline bool d_is_miss(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_MISS_TYPE;
}

/* d_is_whiteout() 判断它是否为阻断下层查找的 overlay whiteout。 */
static inline bool d_is_whiteout(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_WHITEOUT_TYPE;
}

/* d_can_lookup() 仅对可执行子名字查找的普通目录类型返回 true。 */
static inline bool d_can_lookup(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_DIRECTORY_TYPE;
}

/* d_is_autodir() 识别无需常规 lookup、通常触发自动挂载的目录类型。 */
static inline bool d_is_autodir(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_AUTODIR_TYPE;
}

/* d_is_dir() 合并普通目录与 autodir；它回答路径语义，不等同于读取 inode mode。 */
static inline bool d_is_dir(const struct dentry *dentry)
{
	return d_can_lookup(dentry) || d_is_autodir(dentry);
}

/* d_is_symlink() 判断缓存类型是否为符号链接。 */
static inline bool d_is_symlink(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_SYMLINK_TYPE;
}

/* d_is_reg() 判断缓存类型是否为普通文件。 */
static inline bool d_is_reg(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_REGULAR_TYPE;
}

/* d_is_special() 判断缓存类型是否为普通目录/文件/链接以外的特殊 inode。 */
static inline bool d_is_special(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_SPECIAL_TYPE;
}

/* d_is_file() 合并普通文件与特殊文件，用于“非目录、非链接”类路径判断。 */
static inline bool d_is_file(const struct dentry *dentry)
{
	return d_is_reg(dentry) || d_is_special(dentry);
}

/*
 * d_is_negative() 按 VFS 类型位判断普通 miss。@dentry 借用，true 不覆盖所有
 * d_inode==NULL 的叠加层标记；紧邻 TODO 提醒 whiteout 语义仍需单独考虑。
 */
static inline bool d_is_negative(const struct dentry *dentry)
{
	// TODO: check d_is_whiteout(dentry) also.
	/* 待核对：是否还应把 whiteout 计入此“negative”谓词；当前实现只认 miss。 */
	return d_is_miss(dentry);
}

/*
 * d_flags_negative() 对已经取得的 flags 快照判断是否编码为 miss；输入是值而非
 * dentry，因此不涉及所有权。快照如何同步取得由调用者负责。
 */
static inline bool d_flags_negative(unsigned flags)
{
	return (flags & DCACHE_ENTRY_TYPE) == DCACHE_MISS_TYPE;
}

/*
 * d_is_positive() 是 d_is_negative() 的逻辑反面，沿用其“按类型位而非 d_inode”
 * 语义；whiteout/叠加层调用者不能据此推断一定存在 inode。
 */
static inline bool d_is_positive(const struct dentry *dentry)
{
	return !d_is_negative(dentry);
}

/**
 * d_really_is_negative - Determine if a dentry is really negative (ignoring fallthroughs)
 * @dentry: The dentry in question
 *
 * Returns true if the dentry represents either an absent name or a name that
 * doesn't map to an inode (ie. ->d_inode is NULL).  The dentry could represent
 * a true miss, a whiteout that isn't represented by a 0,0 chardev or a
 * fallthrough marker in an opaque directory.
 *
 * Note!  (1) This should be used *only* by a filesystem to examine its own
 * dentries.  It should not be used to look at some other filesystem's
 * dentries.  (2) It should also be used in combination with d_inode() to get
 * the inode.  (3) The dentry may have something attached to ->d_lower and the
 * type field of the flags may be set to something other than miss or whiteout.
 */
/*
 * d_really_is_negative() 忽略叠加层 fallthrough 和 d_flags 类型编码，直接以
 * @dentry->d_inode==NULL 判断该文件系统自己的 dentry 是否没有 inode。它可覆盖
 * 真正 miss、非 0:0 字符设备表示的 whiteout 以及 opaque 目录中的 fallthrough。
 * 只能由文件系统检查自己的对象，并应与 d_inode() 配对取得 inode；其他层可能
 * 在 d_lower 等私有字段附着对象，且 flags 类型未必是 miss/whiteout。参数只读
 * 借用，返回布尔值，不提供并发稳定性或生命周期保证。
 */
static inline bool d_really_is_negative(const struct dentry *dentry)
{
	return dentry->d_inode == NULL;
}

/**
 * d_really_is_positive - Determine if a dentry is really positive (ignoring fallthroughs)
 * @dentry: The dentry in question
 *
 * Returns true if the dentry represents a name that maps to an inode
 * (ie. ->d_inode is not NULL).  The dentry might still represent a whiteout if
 * that is represented on medium as a 0,0 chardev.
 *
 * Note!  (1) This should be used *only* by a filesystem to examine its own
 * dentries.  It should not be used to look at some other filesystem's
 * dentries.  (2) It should also be used in combination with d_inode() to get
 * the inode.
 */
/*
 * d_really_is_positive() 同样绕过叠加层类型位，直接以 @dentry->d_inode!=NULL
 * 判断本文件系统自己的名字是否关联 inode；介质上用 0:0 字符设备实现的
 * whiteout 仍可能返回 true。它只适用于文件系统检查自己的 dentry，并应通过
 * d_inode() 取得对应 inode；参数借用，返回瞬时布尔结果，不取得引用。
 */
static inline bool d_really_is_positive(const struct dentry *dentry)
{
	return dentry->d_inode != NULL;
}

/*
 * simple_positive() 判断 @dentry 同时具有 inode 且仍在名字哈希中，适合 simple
 * 文件系统的“可见正项”测试。参数只读借用；返回瞬时布尔值，不稳定拓扑也不取引用。
 */
static inline int simple_positive(const struct dentry *dentry)
{
	return d_really_is_positive(dentry) && !d_unhashed(dentry);
}

/*
 * vfs_pressure_ratio() 按当前 vfs_cache_pressure/denom sysctl 比例缩放 @val。
 * 输入和返回保持同一无符号计数单位；函数不持锁、不睡眠、不涉及对象所有权，
 * 使用 mult_frac() 降低中间乘法溢出的风险。
 */
unsigned long vfs_pressure_ratio(unsigned long val);

/**
 * d_inode - Get the actual inode of this dentry
 * @dentry: The dentry to query
 *
 * This is the helper normal filesystems should use to get at their own inodes
 * in their own dentries and ignore the layering superimposed upon them.
 */
/*
 * d_inode() 是普通文件系统读取自己 dentry 所关联 inode 的直接访问器，刻意忽略
 * overlay/union 叠加关系。@dentry 为只读借用，返回裸 inode 指针或 NULL；它不
 * 增加 inode 引用、不执行 READ_ONCE，调用者必须已有能稳定关联和生命周期的锁/
 * 引用条件。叠加文件系统应根据目的使用 d_real()/d_backing_inode()。
 */
static inline struct inode *d_inode(const struct dentry *dentry)
{
	return dentry->d_inode;
}

/**
 * d_inode_rcu - Get the actual inode of this dentry with READ_ONCE()
 * @dentry: The dentry to query
 *
 * This is the helper normal filesystems should use to get at their own inodes
 * in their own dentries and ignore the layering superimposed upon them.
 */
/*
 * d_inode_rcu() 为 RCU 路径读取本文件系统自己的 d_inode，并用 READ_ONCE 保证
 * 单次加载不被编译器合并或撕裂。@dentry 借用，返回裸 inode 或 NULL，不取得
 * 引用；调用者必须位于合规 RCU/seqcount 验证窗口，READ_ONCE 本身既不冻结关联
 * 也不保证退出 RCU 后 inode 仍存活。
 */
static inline struct inode *d_inode_rcu(const struct dentry *dentry)
{
	return READ_ONCE(dentry->d_inode);
}

/**
 * d_backing_inode - Get upper or lower inode we should be using
 * @upper: The upper layer
 *
 * This is the helper that should be used to get at the inode that will be used
 * if this dentry were to be opened as a file.  The inode may be on the upper
 * dentry or it may be on a lower dentry pinned by the upper.
 *
 * Normal filesystems should not use this to access their own inodes.
 */
/*
 * d_backing_inode() 是叠加文件系统语义入口：意图返回打开 @upper 时实际承载文件
 * 的 upper 或被其钉住的 lower inode；普通文件系统检查自身 inode 不应使用它。
 * 当前实现仅返回 upper->d_inode，参数与结果都是借用裸指针，可能为 NULL，不增
 * 引用也不提供同步。该薄封装保留了调用点的“backing inode”语义边界。
 */
/*
 * 修正说明：原英文注释描述了可能返回 lower inode 的接口意图，但当前版本代码
 * 没有层间分派，只读取 upper->d_inode；需要真实下层数据 inode 时应走
 * d_real_inode()。这个结论以紧随其后的当前实现为准。
 */
static inline struct inode *d_backing_inode(const struct dentry *upper)
{
	/* 局部 inode 只保存借用结果，作用域内没有引用或所有权变化。 */
	struct inode *inode = upper->d_inode;

	return inode;
}

/**
 * d_real - Return the real dentry
 * @dentry: the dentry to query
 * @type: the type of real dentry (data or metadata)
 *
 * If dentry is on a union/overlay, then return the underlying, real dentry.
 * Otherwise return the dentry itself.
 *
 * See also: Documentation/filesystems/vfs.rst
 */
/*
 * d_real() 根据 @type（数据或元数据）解析 @dentry 的真实承载对象。若缓存的
 * DCACHE_OP_REAL 位表明文件系统安装了回调，就调用 d_op->d_real；否则返回输入
 * 本身。参数是活 dentry 的借用指针，成功结果也是裸借用指针，接口不增加引用；
 * 调用者须保持 upper 对下层对象的钉住关系。普通文件系统走无回调快速路径。
 */
static inline struct dentry *d_real(struct dentry *dentry, enum d_real_type type)
{
	if (unlikely(dentry->d_flags & DCACHE_OP_REAL))
		return dentry->d_op->d_real(dentry, type);
	else
		return dentry;
}

/**
 * d_real_inode - Return the real inode hosting the data
 * @dentry: The dentry to query
 *
 * If dentry is on a union/overlay, then return the underlying, real inode.
 * Otherwise return d_inode().
 */
/*
 * d_real_inode() 以 D_REAL_DATA 调用 d_real()，再取得真正承载文件数据的 inode；
 * 非叠加对象等价于 d_inode(@dentry)。输入和返回均为借用，结果可为 NULL，不增加
 * dentry/inode 引用；强制去 const 仅为调用 d_real()，接口约定不修改输入对象。
 */
static inline struct inode *d_real_inode(const struct dentry *dentry)
{
	/* This usage of d_real() results in const dentry */
	/* 此处 d_real() 的用途只产生 const 语义的 dentry，强制转换不授权写入。 */
	return d_inode(d_real((struct dentry *) dentry, D_REAL_DATA));
}

/*
 * name_snapshot 保存可跨越并发 rename 使用的稳定 dentry 名字。短名复制到
 * inline_name，长名让 name.name 指向带独立引用的外置名字；对象由调用者分配，
 * take 初始化其所有权，release 负责归还，不能复制后各自释放。
 */
struct name_snapshot {
	/* name 保存最终 hash/长度/字节指针；指针指向下方内联区或有引用的外置名。 */
	struct qstr name;
	/* inline_name 是短名字快照存储，也用于判别是否需要释放外置名字引用。 */
	union shortname_store inline_name;
};
/*
 * take_dentry_name_snapshot() 在 RCU+d_seq 重试协议下从活 @dentry 填充调用者提供
 * 的输出 @name。短名复制、长名取得非零引用；函数可能自旋但不睡眠，无返回值，
 * 成功后快照资源必须由 release_dentry_name_snapshot() 配对释放。
 */
void take_dentry_name_snapshot(struct name_snapshot *, struct dentry *);
/*
 * release_dentry_name_snapshot() 消费 @name 持有的外置名字引用；短名无需释放。
 * 无返回值，调用后 name.name 指针失效，不能重复释放或继续作为稳定快照使用。
 */
void release_dentry_name_snapshot(struct name_snapshot *);

/*
 * d_first_child() 在调用者已经稳定 @dentry->d_children 的条件下返回第一个孩子；
 * 无孩子返回 NULL。结果是由侵入式 d_sib 节点恢复出的裸借用指针，不增加引用。
 */
static inline struct dentry *d_first_child(const struct dentry *dentry)
{
	return hlist_entry_safe(dentry->d_children.first, struct dentry, d_sib);
}

/*
 * d_next_sibling() 在同一受保护的父子链遍历中返回 @dentry 的下一个兄弟；链尾
 * 返回 NULL。结果为裸借用指针，调用者不能越过锁/RCU 稳定窗口使用。
 */
static inline struct dentry *d_next_sibling(const struct dentry *dentry)
{
	return hlist_entry_safe(dentry->d_sib.next, struct dentry, d_sib);
}

/*
 * set_default_d_op() 在 super_block 初始化、尚未并发创建相关 dentry 时设置默认
 * ops 并缓存能力位。s/ops 均为借用，ops 生命周期须覆盖超级块；无返回值，不
 * 追溯修改已经分配的 dentry。
 */
void set_default_d_op(struct super_block *, const struct dentry_operations *);
/*
 * d_make_persistent() 把负 @dentry 绑定到非 NULL @inode，必要时哈希，并额外取得
 * 一份 PERSISTENT 引用。它消费 inode 引用、返回同一 dentry；调用者原 dentry
 * 引用不变，持久引用最终必须用 d_make_discardable() 撤销。函数无错误返回。
 */
struct dentry *d_make_persistent(struct dentry *, struct inode *);
/*
 * d_make_discardable() 清除 @dentry 的 PERSISTENT 位并释放对应钉住引用；若没有
 * 其他引用可在函数内同步销毁对象。参数为活持久 dentry，函数可能睡眠，无返回，
 * 调用后不交付新引用且调用者不得依赖该钉住引用继续访问。
 */
void d_make_discardable(struct dentry *dentry);

/* inode->i_lock must be held over that */
/*
 * for_each_alias() 在调用者持有 inode->i_lock 时沿 inode->i_dentry 遍历所有正
 * dentry 别名，并把每个 d_alias 侵入式节点还原到 @dentry。循环变量只是受锁
 * 保护的裸指针，不增加引用；解锁后若要保留对象必须另行 dget。宏会多次求值
 * inode，参数应是无副作用表达式。
 */
#define for_each_alias(dentry, inode) \
	hlist_for_each_entry(dentry, &(inode)->i_dentry, d_alias)

/* 结束 __LINUX_DCACHE_H include guard，避免同一编译单元重复声明。 */
#endif	/* __LINUX_DCACHE_H */
