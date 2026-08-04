// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 这个文件是 luna 注释的
 * 学习注释模型：Linux 内核源码学习注释方法论（当前版本）。
 *
 * 文件地图：本文件是 key 子系统的核心生命周期实现，负责按 UID 维护配额
 * 记录、为 key 分配稳定 serial、创建和发布 key、完成一次性构造与 keyring
 * 链接、查找/更新/撤销 key，以及注册和注销 key_type。它不实现具体 payload
 * 格式，也不负责 keyring 的关联数组细节；这些职责分别由 key_type 回调和
 * keyring.c 等文件承担。
 *
 * 主调用链：key_alloc() 分配未实例化 key → key_instantiate_and_link() 或
 * key_reject_and_link() 提交正向/负向状态并可链入 keyring → key_lookup()
 * 或 keyring 搜索取得带引用的对象 → key_update()/key_revoke()/
 * key_invalidate() 修改状态 → 最后一次 key_put() 将对象交给异步 GC。
 * key_create_or_update() 把“按类型和描述查找、更新或新建”这条常见入口串起。
 *
 * 核心对象和同步模型：key_user 以 UID 为索引并记录数量/字节配额；key 以
 * rb-tree 中的 serial_node 对外索引，并用 usage 延长存活；key->sem 保护
 * payload、状态相关的修改，key_construction_mutex 串行化一次性构造，
 * key_types_sem 保护类型注册表，key_serial_lock 和 key_user_lock 保护两棵
 * 红黑树。发布状态使用 release/acquire 配对，引用归零后由工作队列在进程
 * 上下文中回收。
 *
 * 当前方案以额外的索引、引用计数、配额记账和异步回收换取并发查找、类型可
 * 插拔以及构造等待者的明确生命周期；代价是必须严格维护锁顺序、引用和
 * cleanup 链。下面的注释按“状态/所有权/并发边界”解释代码，而不是翻译
 * 普通 C 语法。
 */
/* Basic authentication token and access key management
 *
 * Copyright (C) 2004-2008 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

/*
 * 这段原始标题说明本文件管理基本的认证 token 和访问 key；在当前实现中，
 * key 既可以承载密码学/认证 payload，也可以代表 keyring 等可被权限和搜索
 * 机制操作的内核对象。版权与作者信息保持原样，不把它们当作行为契约。
 */

#include <linux/export.h>
#include <linux/init.h>
#include <linux/poison.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/security.h>
#include <linux/workqueue.h>
#include <linux/random.h>
#include <linux/err.h>
#include "internal.h"

/*
 * 全局对象地图：key_jar 是 key 的 slab；key_serial_tree 是按 serial 索引的
 * 全局树；key_user_tree 是按 kuid 索引的配额记录树；四个 quota 变量给 root
 * 和普通 UID 提供 key 数量与字节上限。key_types_list 保存已注册类型，
 * key_types_sem 保护类型注册/查找，key_construction_mutex 串行化一次性构造。
 */
struct kmem_cache *key_jar;
struct rb_root		key_serial_tree; /* tree of keys indexed by serial */
/* key_serial_tree 是按 key->serial 排序的全局索引，访问由 key_serial_lock 保护。 */
DEFINE_SPINLOCK(key_serial_lock);

struct rb_root	key_user_tree; /* tree of quota records indexed by UID */
/* key_user_tree 按 UID 保存配额记录，lookup/摘除由 key_user_lock 串行化。 */
DEFINE_SPINLOCK(key_user_lock);

unsigned int key_quota_root_maxkeys = 1000000;	/* root's key count quota */
/* root 的数量上限用于限制 GLOBAL_ROOT_UID 的 key 数。 */
unsigned int key_quota_root_maxbytes = 25000000; /* root's key space quota */
/* root 的字节上限覆盖描述和 payload reservation 的总空间。 */
unsigned int key_quota_maxkeys = 200;		/* general key count quota */
/* 普通 UID 的 key 数量上限由 key_alloc() 的配额锁内检查读取。 */
unsigned int key_quota_maxbytes = 20000;	/* general key space quota */
/* 普通 UID 的总字节上限也约束后续 key_payload_reserve() 扩容。 */

/* 上述四个上游行分别定义 root 的 key 数量/空间上限，以及普通用户的数量/空间上限；
 * 它们由配额检查路径读取，修改时必须考虑并发创建和 payload 扩容。 */

static LIST_HEAD(key_types_list);
static DECLARE_RWSEM(key_types_sem);

/* We serialise key instantiation and link */
/* key_construction_mutex 把同一时刻的实例化提交串行化，防止重复 payload 或重复唤醒。 */
DEFINE_MUTEX(key_construction_mutex);

#ifdef KEY_DEBUGGING
/*
 * __key_check() - 在调试构建中验证 key 的魔数
 *
 * 调用位置：各个公开/内部 key 操作的入口。@key 是待检查的借用指针；函数
 * 只诊断，不取得引用，也不改变 key 生命周期。魔数不匹配意味着悬空、未
 * 初始化或错误类型的对象已经进入 key API，BUG() 让问题在错误传播前暴露。
 * 返回：无；检查失败不会返回。非 KEY_DEBUGGING 构建不生成此函数。
 */
void __key_check(const struct key *key)
{
	printk("__key_check: key %p {%08x} should be {%08x}\n",
	       key, key->magic, KEY_DEBUG_MAGIC);
	BUG();
}
#endif

/*
 * Get the key quota record for a user, allocating a new record if one doesn't
 * already exist.
 */
/*
 * key_user_lookup() - 查找或创建 UID 对应的配额记录
 *
 * @uid 是有效的所有者 kuid_t 值（标量参数不存在 NULL 指针语义）。成功返回
 * key_user 的一份 usage 引用；记录由全局红黑树持有并由 key_user_lock 保护，
 * 调用者完成使用后必须 key_user_put。
 * 找不到时函数会在锁外分配并可睡眠，再回锁重查；分配失败返回 NULL。创建
 * 成功会初始化数量、字节配额和构造锁，但不创建任何 key；同 UID 的并发调用
 * 最终共享同一记录，避免配额分裂。
 */
struct key_user *key_user_lookup(kuid_t uid)
{
	struct key_user *candidate = NULL, *user;
	struct rb_node *parent, **p;

	/*
	 * @uid 是 UID namespace 中的所有者标识。成功返回一个带 usage 引用的
	 * key_user；candidate 只在树中尚无记录时作为锁外预分配对象，user 表示
	 * 最终返回的记录；parent/p 是树遍历的父节点和当前链接位置，不转移引用。
	 * 该函数可睡眠，因为锁外的 kmalloc 可能调度。
	 */

try_again:
	parent = NULL;
	p = &key_user_tree.rb_node;
	spin_lock(&key_user_lock);

	/* search the tree for a user record with a matching UID */
	/* 当前持有 key_user_lock，遍历期间记录不会被并发摘除。 */
	while (*p) {
		parent = *p;
		user = rb_entry(parent, struct key_user, node);

		if (uid_lt(uid, user->uid))
			p = &(*p)->rb_left;
		else if (uid_gt(uid, user->uid))
			p = &(*p)->rb_right;
		else
			goto found;
	}

	/* if we get here, we failed to find a match in the tree */
	/*
	 * 未找到时不能在持有自旋锁的情况下分配：kmalloc 可能睡眠。因此先释放
	 * key_user_lock；如果另一个 CPU 在锁外分配期间插入了同 UID 记录，后续
	 * 第二次查找必须复用已有记录，不能生成两份彼此独立的配额账本。
	 */
	if (!candidate) {
		/* allocate a candidate user record if we don't already have
		 * one */
		/* 锁外只预分配一份候选记录；它尚未初始化，也尚未归属于树或任何 UID。 */
		spin_unlock(&key_user_lock);

		user = NULL;
		candidate = kmalloc_obj(struct key_user);
		if (unlikely(!candidate))
			goto out;

		/* the allocation may have scheduled, so we need to repeat the
		 * search lest someone else added the record whilst we were
		 * asleep */
		/*
		 * 这里重新执行完整查找，而不是直接插入 candidate；分配让出了 CPU，
		 * 树的事实可能已经改变。candidate 仍由当前调用者暂存，直到确认它
		 * 可以成为树中的唯一记录，或在 found 路径被释放。
		 */
		goto try_again;
	}

	/* if we get here, then the user record still hadn't appeared on the
	 * second pass - so we use the candidate record */
	/*
	 * 第二次查找仍为空，candidate 现在成为该 UID 的唯一记录。usage=1 是
	 * 返回给调用者的引用；nkeys/nikeys 是原子运行统计，qnkeys/qnbytes 是
	 * 由 user->lock 保护的配额账本，cons_lock 则供构造请求串行化使用。
	 */
	refcount_set(&candidate->usage, 1);
	atomic_set(&candidate->nkeys, 0);
	atomic_set(&candidate->nikeys, 0);
	candidate->uid = uid;
	candidate->qnkeys = 0;
	candidate->qnbytes = 0;
	spin_lock_init(&candidate->lock);
	mutex_init(&candidate->cons_lock);

	rb_link_node(&candidate->node, parent, p);
	rb_insert_color(&candidate->node, &key_user_tree);
	spin_unlock(&key_user_lock);
	user = candidate;
	goto out;

	/* okay - we found a user record for this UID */
found:
	/*
	 * 已有记录的引用增加必须发生在 key_user_lock 内：最后一个 put 不能在
	 * lookup 观察到它之后把它从树中删除。candidate 若存在只是本次未采用的
	 * 锁外分配，释放它不会影响树中记录。
	 */
	refcount_inc(&user->usage);
	spin_unlock(&key_user_lock);
	kfree(candidate);
out:
	/* NULL 只表示候选记录分配失败；非 NULL 返回值始终带有一份 usage 引用。 */
	return user;
}

/*
 * Dispose of a user structure
 */
/*
 * key_user_put() - 释放 key_user 的一份引用
 *
 * @user 必须是 key_user_lookup() 返回的非空持有引用。函数无返回值且不睡眠；仅
 * 当 usage 归零时在 key_user_lock 下从 UID 红黑树摘除并 kfree，仍有引用时
 * 只减少计数。调用者不能在 put 后继续读取该记录，也不能把 key_user_lock
 * 当作保护已释放对象内容的长期引用。
 */
void key_user_put(struct key_user *user)
{
	/*
	 * @user 必须是 lookup 路径取得的一份引用。refcount_dec_and_lock() 在
	 * 引用归零的同一原子路径中取得 key_user_lock，保证从树摘除和最终释放
	 * 之间不会有新的 lookup 看到它；仍有其他引用时只减少计数。
	 */
	if (refcount_dec_and_lock(&user->usage, &key_user_lock)) {
		rb_erase(&user->node, &key_user_tree);
		spin_unlock(&key_user_lock);

		kfree(user);
	}
}

/*
 * Allocate a serial number for a key.  These are assigned randomly to avoid
 * security issues through covert channel problems.
 */
/*
 * key_alloc_serial() - 为已初始化的 key 分配全局可查找 serial
 *
 * 调用位置：key_alloc() 已完成配额记账、key 对象初始化和 LSM 检查之后。
 * @key 由当前创建路径独占且不可为 NULL，serial_node 尚未插入树；函数不取得
 * 或转移 key 引用，也不睡眠。成功后 key->serial 是大于等于 3 的非负句柄，并已发布
 * 到 key_serial_tree；失败只会在理论上因随机数耗尽而持续寻找，不返回错误。
 * key_lookup() 必须与这里使用同一把 key_serial_lock，才能在取得引用前
 * 防止最后一个 key_put() 把 key 交给 GC。parent/p 是红黑树插入位置，xkey
 * 是遍历到的已有 key；它们只在 key_serial_lock 内有效，不携带额外引用。
 */
static inline void key_alloc_serial(struct key *key)
{
	struct rb_node *parent, **p;
	struct key *xkey;

	/* propose a random serial number and look for a hole for it in the
	 * serial number tree */
	/*
	 * serial 是对外的 key handle，不直接暴露地址；随机起点降低通过连续
	 * 句柄观察对象分配情况的 covert channel 风险。右移去掉符号位，并跳过
	 * 0、1、2 这些保留/无效值。
	 */
	do {
		get_random_bytes(&key->serial, sizeof(key->serial));

		key->serial >>= 1; /* negative numbers are not permitted */
		/* 右移后的候选值非负；循环下方还会跳过小于 3 的保留 serial。 */
	} while (key->serial < 3);

	spin_lock(&key_serial_lock);

attempt_insertion:
	/*
	 * 锁内从根开始查找唯一空洞。锁同时保护“检查 serial 是否占用”和“把
	 * serial_node 插入树”这两个不可拆分的步骤，避免两个并发创建者发布同一
	 * serial。
	 */
	parent = NULL;
	p = &key_serial_tree.rb_node;

	while (*p) {
		parent = *p;
		xkey = rb_entry(parent, struct key, serial_node);

		if (key->serial < xkey->serial)
			p = &(*p)->rb_left;
		else if (key->serial > xkey->serial)
			p = &(*p)->rb_right;
		else
			goto serial_exists;
	}

	/* we've found a suitable hole - arrange for this key to occupy it */
	/* 红黑树插入完成后，key_lookup() 才可能通过该 serial 找到 key。 */
	rb_link_node(&key->serial_node, parent, p);
	rb_insert_color(&key->serial_node, &key_serial_tree);

	spin_unlock(&key_serial_lock);
	return;

	/* we found a key with the proposed serial number - walk the tree from
	 * that point looking for the next unused serial number */
serial_exists:
	/*
	 * 碰撞时沿有序树寻找下一个空档，而不是立即重新生成随机数；越过 int
	 * 上界后回到树根并重新尝试。整个扫描仍在 key_serial_lock 内，候选值的
	 * 所有权和树快照不会被并发插入打乱。
	 */
	for (;;) {
		key->serial++;
		if (key->serial < 3) {
			key->serial = 3;
			goto attempt_insertion;
		}

		parent = rb_next(parent);
		if (!parent)
			goto attempt_insertion;

		xkey = rb_entry(parent, struct key, serial_node);
		if (key->serial < xkey->serial)
			goto attempt_insertion;
	}
}

/**
 * key_alloc - Allocate a key of the specified type.
 * @type: The type of key to allocate.
 * @desc: The key description to allow the key to be searched out.
 * @uid: The owner of the new key.
 * @gid: The group ID for the new key's group permissions.
 * @cred: The credentials specifying UID namespace.
 * @perm: The permissions mask of the new key.
 * @flags: Flags specifying quota properties.
 * @restrict_link: Optional link restriction for new keyrings.
 *
 * Allocate a key of the specified type with the attributes given.  The key is
 * returned in an uninstantiated state and the caller needs to instantiate the
 * key before returning.
 *
 * The restrict_link structure (if not NULL) will be freed when the
 * keyring is destroyed, so it must be dynamically allocated.
 *
 * The user's key count quota is updated to reflect the creation of the key and
 * the user's key data quota has the default for the key type reserved.  The
 * instantiation function should amend this as necessary.  If insufficient
 * quota is available, -EDQUOT will be returned.
 *
 * The LSM security modules can prevent a key being created, in which case
 * -EACCES will be returned.
 *
 * Returns a pointer to the new key if successful and an error code otherwise.
 *
 * Note that the caller needs to ensure the key type isn't uninstantiated.
 * Internally this can be done by locking key_types_sem.  Externally, this can
 * be done by either never unregistering the key type, or making sure
 * key_alloc() calls don't race with module unloading.
 */
/*
 * key_alloc() - 创建一个尚未实例化、但已经进入全局 serial 索引的 key
 *
 * 宏观位置：key_create_or_update() 或具体 key 类型的创建路径 → 本函数 →
 * key_instantiate_and_link()。@type 是不可为 NULL、且由调用者保证仍注册的
 * key_type 借用指针；@desc 是不可为 NULL 且不能为空的可搜索描述字符串；
 * @uid/@gid 是有效的所有者与权限匹配内核 ID；@cred 是不可为 NULL、用于
 * LSM 检查和 UID namespace 语义的借用凭据；@perm 是新 key 的权限位图；
 * @flags 控制配额、内建、UID keyring 和 KEEP 属性；@restrict_link 可为 NULL，
 * 非空时是动态限制对象，所有权转给新 keyring，并由 keyring 销毁路径释放。
 *
 * 入口通常可睡眠且不要求 key->sem；返回成功时 caller 持有 usage=1，key 已
 * 分配 serial 并可被 lookup，但状态仍是 KEY_IS_UNINSTANTIATED、payload 尚未
 * 提交。失败返回 ERR_PTR(-EINVAL/-ENOMEM/-EDQUOT 或 LSM errno)，并逆序撤销
 * 描述、slab 对象、用户配额和 key_user 引用。成功后的下一步必须是实例化；
 * 调用者还需通过类型注册锁/模块生命周期保证 @type 不会卸载。
 */
struct key *key_alloc(struct key_type *type, const char *desc,
		      kuid_t uid, kgid_t gid, const struct cred *cred,
		      key_perm_t perm, unsigned long flags,
		      struct key_restriction *restrict_link)
{
	struct key_user *user = NULL;
	struct key *key;
	size_t desclen, quotalen;
	int ret;
	unsigned long irqflags;

	/*
	 * 变量地图：user 是配额记录的持有引用；key 是本函数逐步构造的对象，
	 * 失败时暂存错误指针；desclen/quotalen 分别是描述长度和本 key 预留的
	 * 配额字节数；ret 保存 LSM/类型检查错误；irqflags 保存配额自旋锁的
	 * 中断状态，确保 irqsave/irqrestore 成对；maxkeys/maxbytes 是按 UID 类别选择
	 * 的上限，只在 user->lock 内用于检查，不能在锁外缓存后参与记账。
	 */

	key = ERR_PTR(-EINVAL);
	/* 先拒绝空描述，避免索引键无法形成稳定的匹配条件。 */
	if (!desc || !*desc)
		goto error;

	if (type->vet_description) {
		/*
		 * 类型可选的 vet_description() 只验证描述语义，不拥有 desc。失败
		 * 仍未分配 user/key，因此统一通过 error 返回其 errno。
		 */
		ret = type->vet_description(desc);
		if (ret < 0) {
			key = ERR_PTR(ret);
			goto error;
		}
	}

	desclen = strlen(desc);
	quotalen = desclen + 1 + type->def_datalen;
	/* 描述字符串的 NUL 和类型默认 payload 一并计入用户 key 空间配额。 */

	/* get hold of the key tracking for this user */
	/*
	 * key_user_lookup() 可能睡眠，成功后 user->usage 将一直持有到所有错误
	 * 出口或 key 成功转移到 key。没有 user 就无法正确记账，直接进入内存错误。
	 */
	user = key_user_lookup(uid);
	if (!user)
		goto no_memory_1;

	/* check that the user's quota permits allocation of another key and
	 * its description */
	/*
	 * 配额检查和增加必须在 user->lock 内完成；否则两个并发创建者都可能先
	 * 看到余量再同时超额。root 使用独立上限；NOT_IN_QUOTA 完全跳过账本，
	 * QUOTA_OVERRUN 则允许本次越过上限但仍增加已使用量。
	 */
	if (!(flags & KEY_ALLOC_NOT_IN_QUOTA)) {
		unsigned maxkeys = uid_eq(uid, GLOBAL_ROOT_UID) ?
			key_quota_root_maxkeys : key_quota_maxkeys;
		unsigned maxbytes = uid_eq(uid, GLOBAL_ROOT_UID) ?
			key_quota_root_maxbytes : key_quota_maxbytes;

		spin_lock_irqsave(&user->lock, irqflags);
		/*
		 * 加法回绕也视为超额：无符号 qnbytes 回绕会把超大 payload 伪装成
		 * 小值。增加 qnkeys/qnbytes 后，key->quotalen 会在初始化时记录这份
		 * 预留，最终 key_put() 负责归还。
		 */
		if (!(flags & KEY_ALLOC_QUOTA_OVERRUN)) {
			if (user->qnkeys + 1 > maxkeys ||
			    user->qnbytes + quotalen > maxbytes ||
			    user->qnbytes + quotalen < user->qnbytes)
				goto no_quota;
		}

		user->qnkeys++;
		user->qnbytes += quotalen;
		spin_unlock_irqrestore(&user->lock, irqflags);
	}

	/* allocate and initialise the key and its description */
	/*
	 * 配额已经先记账，所以下面任一分配失败都必须回到 no_memory_2/3 撤销。
	 * key_jar 提供固定布局的 struct key，description 是独立拷贝，避免调用者
	 * 在 key 生命周期内修改原始字符串。
	 */
	key = kmem_cache_zalloc(key_jar, GFP_KERNEL);
	if (!key)
		goto no_memory_2;

	key->index_key.desc_len = desclen;
	key->index_key.description = kmemdup(desc, desclen + 1, GFP_KERNEL);
	if (!key->index_key.description)
		goto no_memory_3;
	key->index_key.type = type;
	key_set_index_key(&key->index_key);

	refcount_set(&key->usage, 1);
	init_rwsem(&key->sem);
	lockdep_set_class(&key->sem, &type->lock_class);
	key->user = user;
	key->quotalen = quotalen;
	key->datalen = type->def_datalen;
	key->uid = uid;
	key->gid = gid;
	key->perm = perm;
	key->expiry = TIME64_MAX;
	key->restrict_link = restrict_link;
	key->last_used_at = ktime_get_real_seconds();
	/*
	 * 至此对象已有引用、权限、所属 user、配额长度和默认过期时间，但还未
	 * 通过 LSM，也尚未取得 serial；state 仍由零初始化表示未实例化。
	 */

	key->flags |= 1 << KEY_FLAG_USER_ALIVE;
	if (!(flags & KEY_ALLOC_NOT_IN_QUOTA))
		key->flags |= 1 << KEY_FLAG_IN_QUOTA;
	if (flags & KEY_ALLOC_BUILT_IN)
		key->flags |= 1 << KEY_FLAG_BUILTIN;
	if (flags & KEY_ALLOC_UID_KEYRING)
		key->flags |= 1 << KEY_FLAG_UID_KEYRING;
	if (flags & KEY_ALLOC_SET_KEEP)
		key->flags |= 1 << KEY_FLAG_KEEP;

#ifdef KEY_DEBUGGING
	/* 调试字段只用于验证 slab 对象没有被错误复用，不参与正常 key 协议。 */
	key->magic = KEY_DEBUG_MAGIC;
#endif

	/* let the security module know about the key */
	/*
	 * LSM 在 key 获得全局可见 serial 前检查创建请求；拒绝时 key 尚未发布，
	 * 因而 security_error 可以完整释放对象并退回配额，不需要从 serial 树摘除。
	 */
	ret = security_key_alloc(key, cred, flags);
	if (ret < 0)
		goto security_error;

	/* publish the key by giving it a serial number */
	/*
	 * serial 分配是本文件中的发布边界之一：完成后 key_lookup() 可以获得它，
	 * 所以此前所有字段和 LSM 状态必须已完成。domain_tag 的引用和 user->nkeys
	 * 统计在同一阶段建立；“可查找”不等于“已实例化”，读取者仍须检查 state。
	 */
	refcount_inc(&key->domain_tag->usage);
	atomic_inc(&user->nkeys);
	key_alloc_serial(key);

error:
	return key;

security_error:
	/*
	 * LSM 拒绝发生在 serial 发布前，按 description → slab → quota → user
	 * 引用的逆序清理。ret 保留 LSM 的精确 errno，向调用者编码成错误指针。
	 */
	kfree(key->description);
	kmem_cache_free(key_jar, key);
	if (!(flags & KEY_ALLOC_NOT_IN_QUOTA)) {
		spin_lock_irqsave(&user->lock, irqflags);
		user->qnkeys--;
		user->qnbytes -= quotalen;
		spin_unlock_irqrestore(&user->lock, irqflags);
	}
	key_user_put(user);
	key = ERR_PTR(ret);
	goto error;

no_memory_3:
	/* 描述拷贝失败：slab 对象尚未对外可见，只需释放对象并回滚 quota/user。 */
	kmem_cache_free(key_jar, key);
no_memory_2:
	/* key slab 分配失败或描述失败的汇合点，配额必须与前面的增加严格配对。 */
	if (!(flags & KEY_ALLOC_NOT_IN_QUOTA)) {
		spin_lock_irqsave(&user->lock, irqflags);
		user->qnkeys--;
		user->qnbytes -= quotalen;
		spin_unlock_irqrestore(&user->lock, irqflags);
	}
	key_user_put(user);
no_memory_1:
	/* user 记录分配失败时尚未修改配额；返回统一的 -ENOMEM 错误指针。 */
	key = ERR_PTR(-ENOMEM);
	goto error;

no_quota:
	/*
	 * 配额锁只覆盖检查/记账，离开后才释放 user 引用；-EDQUOT 表示调用者
	 * 可重试但本次没有创建任何 key，也没有遗留资源。
	 */
	spin_unlock_irqrestore(&user->lock, irqflags);
	key_user_put(user);
	key = ERR_PTR(-EDQUOT);
	goto error;
}
EXPORT_SYMBOL(key_alloc);

/**
 * key_payload_reserve - Adjust data quota reservation for the key's payload
 * @key: The key to make the reservation for.
 * @datalen: The amount of data payload the caller now wants.
 *
 * Adjust the amount of the owning user's key data quota that a key reserves.
 * If the amount is increased, then -EDQUOT may be returned if there isn't
 * enough free quota available.
 *
 * If successful, 0 is returned.
 */
/*
 * key_payload_reserve() - 调整 key payload 占用的用户字节配额
 *
 * @key 是不可为 NULL 且由调用者持有的 key 指针，@datalen 是新的 payload 字节数（不是旧值
 * 的增量）。函数通常在 key->sem 已由上层取得或对象仍处于构造阶段时调用；
 * 本函数只用 key->user->lock 保护配额账本，不负责保护 payload 内容。成功返
 * 回 0，并把 key->datalen 更新为新值；失败返回 -EDQUOT，账本和 datalen
 * 保持原值。扩容会增加未来 key_put() 要归还的 quotalen，缩容则释放差额。
 * delta 是相对旧 datalen 的有符号差额；实现把 datalen 转为 int 后计算 delta，
 * 因而调用者必须提供当前 key 类型允许且能安全表示的长度；ret 是配额检查结果，
 * flags 是 irqsave 保存的中断状态。
 */
int key_payload_reserve(struct key *key, size_t datalen)
{
	int delta = (int)datalen - key->datalen;
	int ret = 0;

	/* delta 是相对当前记账值的变化；它可能为负，缩容不需要配额上限检查。 */

	key_check(key);

	/* contemplate the quota adjustment */
	/*
	 * 未计入配额的 key（例如内建或特殊路径）只更新自身 datalen。计入配额
	 * 的 key 必须把“检查新总量”和“提交差额”放在 user->lock 内，且用回绕
	 * 检查拒绝无法由 size_t→int 安全表达的异常大增长。
	 */
	if (delta != 0 && test_bit(KEY_FLAG_IN_QUOTA, &key->flags)) {
		unsigned maxbytes = uid_eq(key->user->uid, GLOBAL_ROOT_UID) ?
			key_quota_root_maxbytes : key_quota_maxbytes;
		unsigned long flags;

		spin_lock_irqsave(&key->user->lock, flags);

		if (delta > 0 &&
		    (key->user->qnbytes + delta > maxbytes ||
		     key->user->qnbytes + delta < key->user->qnbytes)) {
			ret = -EDQUOT;
		}
		else {
			/* quotalen 与用户总字节账本同步移动，保持 key_put() 可完整归还。 */
			key->user->qnbytes += delta;
			key->quotalen += delta;
		}
		spin_unlock_irqrestore(&key->user->lock, flags);
	}

	/* change the recorded data length if that didn't generate an error */
	/*
	 * 只有配额提交成功后才改变 datalen；失败时保留旧值，使调用者可以按
	 * 原有 reservation 继续 cleanup，而不会出现账本与对象记录不一致。
	 */
	if (ret == 0)
		key->datalen = datalen;

	return ret;
}
EXPORT_SYMBOL(key_payload_reserve);

/*
 * Change the key state to being instantiated.
 */
/*
 * mark_key_instantiated() - 发布 key 的正向或负向实例化状态
 *
 * @key 的 payload/错误码在调用前已经由具体 key_type 写入；@reject_error 为
 * 负 errno 时表示 negative key，否则表示正向实例化。此函数不取得锁、不改变
 * 引用，也无直接返回值。release store 是发布边界：key_read_state() 的
 * acquire 读取看到 KEY_IS_POSITIVE 或负错误时，也必须能看到此前提交的
 * payload/状态字段；普通赋值会允许读者观察到“已实例化但 payload 尚未可见”。
 */
static void mark_key_instantiated(struct key *key, int reject_error)
{
	/* Commit the payload before setting the state; barrier versus
	 * key_read_state().
	 */
	/* 原注释说明必须先提交 payload 再改 state；release/acquire 配对建立发布顺序。 */
	smp_store_release(&key->state,
			  (reject_error < 0) ? reject_error : KEY_IS_POSITIVE);
}

/*
 * Instantiate a key and link it into the target keyring atomically.  Must be
 * called with the target keyring's semaphore writelocked.  The target key's
 * semaphore need not be locked as instantiation is serialised by
 * key_construction_mutex.
 */
/*
 * __key_instantiate_and_link() - 在构造锁下提交 key 并完成可选 keyring 链接
 *
 * 调用者：key_instantiate_and_link()、key_reject_and_link() 以及创建/更新共
 * 用路径。@key 不可为 NULL，是未实例化且由调用者持有的 key；@prep 不可为
 * NULL，是已完成 preparse 的输入/输出结构，具体 instantiate 回调可从中接管
 * payload；@keyring 可为 NULL，非空时调用者已通过 __key_link_begin() 准备好
 * 目标 keyring 的写侧 edit；@authkey 可为 NULL，是成功后会被 invalidate 的
 * 授权 key；@_edit 不可为 NULL，是关联数组编辑计划的输入输出指针，链接成功
 * 时由 __key_link() 消耗。入口要求目标
 * keyring 的写锁已持有，函数本身不再获取它；可睡眠的 key_type->instantiate()
 * 在 key_construction_mutex 下运行。
 *
 * 返回 0 表示 key 只被成功实例化（若给了 keyring 还已链接），并唤醒等待者；
 * -EBUSY 表示已经被另一个调用实例化，类型回调的其他负 errno 原样返回。
 * 失败时 payload/链接由调用者按 prep 和 edit 的约定清理，authkey 不会被
 * 无故失效；成功后 state 已发布、nikeys 增加、通知已发送，authkey 的使用
 * 权限被撤销，expiry 可能被设置。
 */
static int __key_instantiate_and_link(struct key *key,
				      struct key_preparsed_payload *prep,
				      struct key *keyring,
				      struct key *authkey,
				      struct assoc_array_edit **_edit)
{
	int ret, awaken;

	/* ret 贯穿类型实例化和最终返回；awaken 记录 USER_CONSTRUCT 位是否从 1 清零，
	 * 只有它为真时才在解锁后唤醒等待者。 */

	key_check(key);
	key_check(keyring);

	awaken = 0;
	ret = -EBUSY;

	/*
	 * 构造锁把“检查未实例化”和“调用类型回调/发布状态”合并成一次提交。
	 * key->sem 不必再锁：同一 key 的实例化协议由这把全局 mutex 串行化，而
	 * keyring 写锁由调用者持有，形成先 keyring、后 construction 的固定顺序。
	 */
	mutex_lock(&key_construction_mutex);

	/* can't instantiate twice */
	/* 同一 key 的 state 只能从未实例化提交一次；后续调用保留 -EBUSY，避免覆盖 payload。 */
	if (key->state == KEY_IS_UNINSTANTIATED) {
		/* instantiate the key */
		/* 具体类型把 preparsed 输入转换为自己的 payload；回调失败时 state 不变，
		 * 调用者仍拥有未实例化 key 和 prep 中尚未转移的资源。 */
		ret = key->type->instantiate(key, prep);

		if (ret == 0) {
			/* mark the key as being instantiated */
			/*
			 * 先递增已实例化统计，再用 release store 发布 state；通知观察者
			 * 发生了实例化，顺序保证观察者不会先看到“已完成”再看到旧 payload。
			 */
			atomic_inc(&key->user->nikeys);
			mark_key_instantiated(key, 0);
			notify_key(key, NOTIFY_KEY_INSTANTIATED, 0);

			if (test_and_clear_bit(KEY_FLAG_USER_CONSTRUCT, &key->flags))
				/* 清除构造中位并记录是否真的存在等待者；只有从 1→0 时才需唤醒。 */
				awaken = 1;

			/* and link it into the destination keyring */
			/*
			 * state 已先发布，随后才把 key 放进目标 ring；KEEP 属性从目标 ring
			 * 继承到新 key。__key_link() 使用调用者准备的 edit，不在这里重新
			 * 分配/获取 keyring 锁。
			 */
			if (keyring) {
				if (test_bit(KEY_FLAG_KEEP, &keyring->flags))
					set_bit(KEY_FLAG_KEEP, &key->flags);

				__key_link(keyring, key, _edit);
			}

			/* disable the authorisation key */
			/* 授权 token 只允许这一次构造；成功提交后立即失效，避免重复使用。 */
			if (authkey)
				key_invalidate(authkey);

			if (prep->expiry != TIME64_MAX)
				/* preparse 可指定绝对过期时刻；默认 TIME64_MAX 表示不覆盖现值。 */
				key_set_expiry(key, prep->expiry);
		}
	}

	mutex_unlock(&key_construction_mutex);

	/* wake up anyone waiting for a key to be constructed */
	/*
	 * 不能在 construction mutex 内唤醒并让等待者立即重入相关路径；先释放
	 * 构造锁，再以 flags 上的 wait-bit 协议唤醒。awaken=0 表示没有观察到
	 * USER_CONSTRUCT 位，不需要无条件广播。
	 */
	if (awaken)
		wake_up_bit(&key->flags, KEY_FLAG_USER_CONSTRUCT);

	return ret;
}

/**
 * key_instantiate_and_link - Instantiate a key and link it into the keyring.
 * @key: The key to instantiate.
 * @data: The data to use to instantiate the keyring.
 * @datalen: The length of @data.
 * @keyring: Keyring to create a link in on success (or NULL).
 * @authkey: The authorisation token permitting instantiation.
 *
 * Instantiate a key that's in the uninstantiated state using the provided data
 * and, if successful, link it in to the destination keyring if one is
 * supplied.
 *
 * If successful, 0 is returned, the authorisation token is revoked and anyone
 * waiting for the key is woken up.  If the key was already instantiated,
 * -EBUSY will be returned.
 */
/*
 * key_instantiate_and_link() - 解析输入、实例化 key，并原子准备可选链接
 *
 * @key 不可为 NULL，是未实例化 key 的持有引用；@data 可为 NULL 但仅在
 * @datalen 为 0 时有意义，@datalen 是待交给 key_type->preparse 的输入字节数，
 * 调用者在函数返回前保持 data 有效；@keyring 可为 NULL，非空是成功后要建立
 * 链接的目标 keyring 借用指针；@authkey 可为 NULL，是成功后必须
 * 失效的授权 token。函数可睡眠，内部先建立 prep，再为 keyring 获取写侧
 * 链接锁/编辑计划，并通过 restriction->check 做策略校验。
 *
 * 成功返回 0：key 状态已发布、可选链接已提交、授权 key 已失效、等待者被唤醒；
 * -EBUSY 表示 key 已被实例化，其他负 errno 来自 preparse、链接或 restriction
 * 检查。所有失败出口释放 prep 中仍由本函数拥有的资源，并结束 edit；key 本身
 * 的引用和生命周期仍由调用者管理。该包装器是“输入预解析/目标 ring 事务”
 * 层，真正的状态转换发生在 __key_instantiate_and_link()。
 */
int key_instantiate_and_link(struct key *key,
			     const void *data,
			     size_t datalen,
			     struct key *keyring,
			     struct key *authkey)
{
	struct key_preparsed_payload prep;
	struct assoc_array_edit *edit = NULL;
	int ret;

	/* prep 是 preparse 的临时 ownership 容器，edit 是可选的 keyring 编辑计划；
	 * ret 保存每个阶段的 errno，并决定进入 error 或 error_link_end。 */

	/* prep 是本函数的临时 ownership 容器：preparse 可填充 description/payload，
	 * free_preparse() 在所有出口收回未被 key_type instantiate 接管的部分。 */

	memset(&prep, 0, sizeof(prep));
	prep.orig_description = key->description;
	prep.data = data;
	prep.datalen = datalen;
	prep.quotalen = key->type->def_datalen;
	prep.expiry = TIME64_MAX;
	if (key->type->preparse) {
		/* 先把外部数据转换为类型约定的 preparsed payload，失败时尚未锁 ring。 */
		ret = key->type->preparse(&prep);
		if (ret < 0)
			goto error;
	}

	if (keyring) {
		/*
		 * 目标 ring 的锁和 edit 先准备好，restriction 检查也在链接提交前完成；
		 * 任何一步失败都跳到 error_link_end，确保半成品编辑不会遗留。
		 */
		ret = __key_link_lock(keyring, &key->index_key);
		if (ret < 0)
			goto error;

		ret = __key_link_begin(keyring, &key->index_key, &edit);
		if (ret < 0)
			goto error_link_end;

		if (keyring->restrict_link && keyring->restrict_link->check) {
			struct key_restriction *keyres = keyring->restrict_link;

			/* 限制回调只观察待链接类型和 payload，可拒绝但不取得 key 所有权。 */
			ret = keyres->check(keyring, key->type, &prep.payload,
					    keyres->key);
			if (ret < 0)
				goto error_link_end;
		}
	}

	/* 进入核心提交点：成功后 prep 中被 instantiate 接管的 payload 不再由 free_preparse 释放。 */
	ret = __key_instantiate_and_link(key, &prep, keyring, authkey, &edit);

error_link_end:
	/* edit 可能为空；helper 负责结束/提交或丢弃关联数组编辑并释放其锁状态。 */
	if (keyring)
		__key_link_end(keyring, &key->index_key, edit);

error:
	/* 无论失败发生在哪个阶段，preparse 的临时 ownership 都在这里闭合。 */
	if (key->type->preparse)
		key->type->free_preparse(&prep);
	return ret;
}

EXPORT_SYMBOL(key_instantiate_and_link);

/**
 * key_reject_and_link - Negatively instantiate a key and link it into the keyring.
 * @key: The key to instantiate.
 * @timeout: The timeout on the negative key.
 * @error: The error to return when the key is hit.
 * @keyring: Keyring to create a link in on success (or NULL).
 * @authkey: The authorisation token permitting instantiation.
 *
 * Negatively instantiate a key that's in the uninstantiated state and, if
 * successful, set its timeout and stored error and link it in to the
 * destination keyring if one is supplied.  The key and any links to the key
 * will be automatically garbage collected after the timeout expires.
 *
 * Negative keys are used to rate limit repeated request_key() calls by causing
 * them to return the stored error code (typically ENOKEY) until the negative
 * key expires.
 *
 * If successful, 0 is returned, the authorisation token is revoked and anyone
 * waiting for the key is woken up.  If the key was already instantiated,
 * -EBUSY will be returned.
 */
/*
 * key_reject_and_link() - 以缓存错误的 negative key 完成构造并可选地链接
 *
 * @key 不可为 NULL，是未实例化 key 的持有引用；@timeout 是 negative key 的
 * 相对秒数；@error 是命中该 key 时向 request_key() 返回的非零正 errno，函数内部
 * 以负值发布；@keyring 可为 NULL，是成功时建立链接的目标；@authkey 可为 NULL，
 * 成功提交后失效。
 * 该函数可睡眠。它不需要 payload/preparse，而是先准备链接事务，再在
 * key_construction_mutex 下把 state 设为负错误并设置 expiry。
 *
 * 返回 0 表示 negative key 已发布且（若链接事务成功）已链入；若链接失败，
 * 返回 link_ret 而 key 本身仍已负向实例化；未实例化竞争返回 -EBUSY，受限
 * keyring 直接返回 -EPERM。成功会增加 nikeys、通知/唤醒等待者、失效授权
 * token；negative key 及其链接由超时后的 GC 清理。这个快路径用缓存错误
 * 抑制重复 request_key()，以额外的短生命周期对象换取失败限流。
 */
int key_reject_and_link(struct key *key,
			unsigned timeout,
			unsigned error,
			struct key *keyring,
			struct key *authkey)
{
	struct assoc_array_edit *edit = NULL;
	int ret, awaken, link_ret = 0;

	/* ret 是实例化结果，link_ret 是目标 ring 事务结果；awaken 控制 wait-bit 唤醒，
	 * edit 在链接锁成功后保存待提交的关联数组编辑。 */

	/* link_ret 与 ret 分离：key 的负向实例化和“是否成功建立链接”是两个结果；
	 * edit 可为 NULL，awaken 是等待位是否被本次提交清除的布尔哨兵。 */

	key_check(key);
	key_check(keyring);

	awaken = 0;
	ret = -EBUSY;

	if (keyring) {
		/* restricted ring 不允许用 negative key 旁路其策略；这里不启动 edit。 */
		if (keyring->restrict_link)
			return -EPERM;

		link_ret = __key_link_lock(keyring, &key->index_key);
		/* 只有链接锁成功才准备 edit；失败会保留错误，后面仍可完成 negative state。 */
		if (link_ret == 0) {
			link_ret = __key_link_begin(keyring, &key->index_key, &edit);
			if (link_ret < 0)
				__key_link_end(keyring, &key->index_key, edit);
		}
	}

	mutex_lock(&key_construction_mutex);

	/* can't instantiate twice */
	/* negative key 也遵循一次性状态转换，已经正向/负向实例化就不重复提交。 */
	if (key->state == KEY_IS_UNINSTANTIATED) {
		/* mark the key as being negatively instantiated */
		/*
		 * -error 被写入 state，key_read_state() 看到负值即可复用该错误；expiry
		 * 从当前实时时钟加 timeout，保证 GC 最迟在这段窗口后回收该缓存结果。
		 */
		atomic_inc(&key->user->nikeys);
		mark_key_instantiated(key, -error);
		notify_key(key, NOTIFY_KEY_INSTANTIATED, -error);
		key_set_expiry(key, ktime_get_real_seconds() + timeout);

		if (test_and_clear_bit(KEY_FLAG_USER_CONSTRUCT, &key->flags))
			awaken = 1;

		ret = 0;

		/* and link it into the destination keyring */
		/* 链接只有在目标事务准备成功时执行；即使 link_ret 失败，negative state 仍有效。 */
		if (keyring && link_ret == 0)
			__key_link(keyring, key, &edit);

		/* disable the authorisation key */
		/* 一次性授权在 negative commit 后同样失效，防止调用者用它再次构造。 */
		if (authkey)
			key_invalidate(authkey);
	}

	mutex_unlock(&key_construction_mutex);

	if (keyring && link_ret == 0)
		/* 先离开 construction mutex，再完成 keyring 编辑事务，避免唤醒/回调在全局构造锁内运行。 */
		__key_link_end(keyring, &key->index_key, edit);

	/* wake up anyone waiting for a key to be constructed */
	/* state 已发布后才唤醒 waiter；等待者会读取到负 errno，而不是旧的未实例化状态。 */
	if (awaken)
		wake_up_bit(&key->flags, KEY_FLAG_USER_CONSTRUCT);

	return ret == 0 ? link_ret : ret;
}
EXPORT_SYMBOL(key_reject_and_link);

/**
 * key_put - Discard a reference to a key.
 * @key: The key to discard a reference from.
 *
 * Discard a reference to a key, and when all the references are gone, we
 * schedule the cleanup task to come and pull it out of the tree in process
 * context at some later time.
 */
/*
 * key_put() - 释放一份 key 引用，并在最后一次 put 后安排异步回收
 *
 * @key 可空，是调用者持有的 usage 引用；函数无返回值、可在不能睡眠的上下文
 * 调用，因为引用归零后的工作只是记账、清位和 schedule_work()。非最后一次
 * put 不改变对象；最后一次会归还 user 配额、以 clear_bit_unlock() 清除
 * KEY_FLAG_USER_ALIVE，再把回收交给 key_gc_work。serial 树中的摘除、payload
 * 释放和最终 slab free 不在这里同步完成，因此调用者不能把“最后一次 put”
 * 当作对象可立即复用的边界。
 */
void key_put(struct key *key)
{
	if (key) {
		key_check(key);

		if (refcount_dec_and_test(&key->usage)) {
			unsigned long flags;

			/*
			 * 引用归零意味着不会再有新的合法 key_lookup() 引用；先退回配额，
			 * 再用 release 语义清 USER_ALIVE。GC 以此判断 user 记账已完成，
			 * 并在进程上下文安全地处理可能睡眠的类型回收。
			 */

			/* deal with the user's key tracking and quota */
			/* 最后一个引用归零时退回该用户的 key 数量和字节账本，和 key_alloc 的增加配对。 */
			if (test_bit(KEY_FLAG_IN_QUOTA, &key->flags)) {
				spin_lock_irqsave(&key->user->lock, flags);
				key->user->qnkeys--;
				key->user->qnbytes -= key->quotalen;
				spin_unlock_irqrestore(&key->user->lock, flags);
			}
			/* Mark key as safe for GC after key->user done. */
			/* 原注释的约束是：必须在 user 配额更新完成后清位，GC 才能安全处理 key。 */
			clear_bit_unlock(KEY_FLAG_USER_ALIVE, &key->flags);
			schedule_work(&key_gc_work);
		}
	}
}
EXPORT_SYMBOL(key_put);

/*
 * Find a key by its serial number.
 */
/*
 * key_lookup() - 按全局 serial 查找并取得一份稳定 key 引用
 *
 * @id 是用户可见的、允许为负但通常来自用户态合法句柄的 key_serial_t；函数在
 * key_serial_lock 下查找红黑树并把
 * usage 从非零原子增加；成功返回持有引用的裸 key 指针，失败返回 ERR_PTR
 * (-ENOKEY)。不能返回 NULL。引用增加与树锁配对很关键：若 key 正在等待 GC，
 * 不能让 lookup 在最后一次 put 后重新“复活”一个对象。调用者完成使用后必须
 * key_put()；本函数不睡眠，也不保证 key 的 payload 状态不再变化。
 * n 是当前红黑树节点，key 是从 serial_node 反解出的借用指针；只有
 * refcount_inc_not_zero() 成功后，返回的 key 才带有可释放的 usage 引用。
 */
struct key *key_lookup(key_serial_t id)
{
	struct rb_node *n;
	struct key *key;

	spin_lock(&key_serial_lock);

	/* search the tree for the specified key */
	/*
	 * serial_tree 的查找和引用获取必须共用 key_serial_lock：GC 可能正在准备
	 * 摘除树节点，普通的“先读指针、再加引用”会产生 use-after-free 或复活。
	 */
	n = key_serial_tree.rb_node;
	while (n) {
		key = rb_entry(n, struct key, serial_node);

		if (id < key->serial)
			n = n->rb_left;
		else if (id > key->serial)
			n = n->rb_right;
		else
			goto found;
	}

not_found:
	key = ERR_PTR(-ENOKEY);
	goto error;

found:
	/* A key is allowed to be looked up only if someone still owns a
	 * reference to it - otherwise it's awaiting the gc.
	 */
	/*
	 * refcount_inc_not_zero() 把“仍有 owner”和“增加本次引用”合成一个原子
	 * 判定；返回 false 表示最后一个 owner 已经 put，树中节点虽可能尚未被
	 * 工作队列摘除，也不能再向调用者暴露。
	 */
	if (!refcount_inc_not_zero(&key->usage))
		goto not_found;

error:
	spin_unlock(&key_serial_lock);
	return key;
}
EXPORT_SYMBOL(key_lookup);

/*
 * Find and lock the specified key type against removal.
 *
 * We return with the sem read-locked if successful.  If the type wasn't
 * available -ENOKEY is returned instead.
 */
/*
 * key_type_lookup() - 查找并读锁住一个已注册的 key_type
 *
 * @type 是不可为 NULL 的 NUL 结尾类型名借用字符串，函数可睡眠。成功返回带
 * key_types_sem 读锁的 key_type 借用指针，调用者必须以 key_type_put() 配对；
 * 失败返回 ERR_PTR(-ENOKEY)，且不持锁。读锁阻止 unregister_key_type() 在本
 * 调用者完成使用前删除/回收类型，多个并发 lookup 仍可并行读取注册链表。
 * ktype 是链表遍历中的借用指针；它的稳定性来自 key_types_sem 读锁，不是
 * 引用计数。
 */
struct key_type *key_type_lookup(const char *type)
{
	struct key_type *ktype;

	down_read(&key_types_sem);

	/* look up the key type to see if it's one of the registered kernel
	 * types */
	/* 读锁覆盖整个 list_for_each_entry 和返回后的类型使用，直到 key_type_put。 */
	list_for_each_entry(ktype, &key_types_list, link) {
		if (strcmp(ktype->name, type) == 0)
			goto found_kernel_type;
	}

	up_read(&key_types_sem);
	ktype = ERR_PTR(-ENOKEY);

found_kernel_type:
	return ktype;
}

/*
 * key_set_timeout() - 为 key 设置相对秒数对应的绝对过期时间
 *
 * @key 是调用者持有的借用指针；@timeout 为 0 时恢复 TIME64_MAX，否则以
 * 当前 real time 加秒数计算 expiry。函数可睡眠、无返回值，key->sem 写锁
 * 串行化与 revoke/其他属性更新；key_set_expiry() 会安排 GC，但不立即释放
 * key 或撤销调用者引用。成功后的可观察变化是 expiry 和后续 GC 调度，失败
 * 只能表现为底层锁/调度路径无法继续，函数本身没有错误返回。
 */
void key_set_timeout(struct key *key, unsigned timeout)
{
	time64_t expiry = TIME64_MAX;

	/*
 * @timeout 是相对当前实时时钟的秒数，0 表示清除有限过期时间并恢复
 * TIME64_MAX；@key 不可为 NULL，是借用指针；成功后 expiry 的修改由 key->sem 保护，
	 * 函数无返回值且可睡眠。key_set_expiry() 还会安排后续 GC，但不会立即
 * 释放 key；调用者仍负责自己的 usage 引用。
 * expiry 是传给 key_set_expiry() 的绝对时刻，初值 TIME64_MAX 表示不设置有限期限。
	 */

	/* make the changes with the locks held to prevent races */
	/* key->sem 串行化过期更新与 revoke/update/读取路径，避免覆盖并发状态变化。 */
	down_write(&key->sem);

	if (timeout > 0)
		expiry = ktime_get_real_seconds() + timeout;
	key_set_expiry(key, expiry);

	up_write(&key->sem);
}
EXPORT_SYMBOL_GPL(key_set_timeout);

/*
 * Unlock a key type locked by key_type_lookup().
 */
/*
 * key_type_put() - 释放 key_type_lookup() 返回的读侧注册表锁
 *
 * @ktype 不可为 NULL，仅用于表达配对关系，当前实现不从指针读取字段；调用者必须传入
 * 对应 lookup 的结果。函数无返回值，up_read() 后 unregister_key_type() 可以
 * 继续执行，因此 lookup 返回的裸指针不能再使用。
 */
void key_type_put(struct key_type *ktype)
{
	up_read(&key_types_sem);
}

/*
 * Attempt to update an existing key.
 *
 * The key is given to us with an incremented refcount that we need to discard
 * if we get an error.
 */
/*
 * __key_update() - 在已有 key 上执行类型更新回调
 *
 * @key_ref 是有效且带 possession 位、已由查找路径增加 usage 的 key 引用；
 * @prep 不可为 NULL，是已完成 preparse 的输入/输出 payload。函数先检查
 * KEY_NEED_WRITE，再在 key->sem 写侧调用
 * type->update；更新失败时必须 key_put() 释放传入的额外引用。成功返回原
 * key_ref（包括 possession 位），并把 negative key 正向实例化、发送更新通知；
 * 返回错误指针时 key 引用已在本函数内消费。该 helper 可睡眠，真正的 payload
 * 语义、配额调整和释放由具体 key_type->update() 负责。
 * key 是从 key_ref 解码出的借用指针，ret 保存权限/类型回调 errno；成功时
 * key_ref 原样返回，失败时 key 的传入引用由本函数消费。
 */
static inline key_ref_t __key_update(key_ref_t key_ref,
				     struct key_preparsed_payload *prep)
{
	struct key *key = key_ref_to_ptr(key_ref);
	int ret;

	/* key 是由 key_ref 解码出的借用指针，prep 是可由 preparse 填充的临时资源，
	 * ret 依次保存权限、preparse 和类型 update 的结果。 */

	/* key_ref 的低位是 possession 属性，必须通过 key_ref_to_ptr() 解码后才能访问 key。 */

	/* need write permission on the key to update it */
	/* 权限检查先于加写锁，失败时不触碰 payload，只释放本函数接管的引用。 */
	ret = key_permission(key_ref, KEY_NEED_WRITE);
	if (ret < 0)
		goto error;

	ret = -EEXIST;
	/* 没有 update 回调的类型不能通过此路径更新；-EEXIST 保留给创建/更新语义。 */
	if (!key->type->update)
		goto error;

	down_write(&key->sem);

	/* 写锁把类型回调与 revoke/并发 update 串行化；回调返回后才可发布新状态。 */
	ret = key->type->update(key, prep);
	if (ret == 0) {
		/* Updating a negative key positively instantiates it */
		/* 负 key 被成功更新后变成正 key；release store 确保新 payload 先于状态可见。 */
		mark_key_instantiated(key, 0);
		notify_key(key, NOTIFY_KEY_UPDATED, 0);
	}

	up_write(&key->sem);

	if (ret < 0)
		/* 错误出口统一丢弃传入的 usage，避免 find_key_to_update() 的 pin 泄漏。 */
		goto error;
out:
	return key_ref;

error:
	key_put(key);
	key_ref = ERR_PTR(ret);
	goto out;
}

/*
 * Create or potentially update a key. The combined logic behind
 * key_create_or_update() and key_create()
 */
static key_ref_t __key_create_or_update(key_ref_t keyring_ref,
					const char *type,
					const char *description,
					const void *payload,
					size_t plen,
					key_perm_t perm,
					unsigned long flags,
					bool allow_update)
{
	struct keyring_index_key index_key = {
		.description	= description,
	};
	struct key_preparsed_payload prep;
	struct assoc_array_edit *edit = NULL;
	const struct cred *cred = current_cred();
	struct key *keyring, *key = NULL;
	key_ref_t key_ref;
	int ret;
	struct key_restriction *restrict_link = NULL;

	/*
	 * 变量地图：
	 *   keyring_ref 是有效且非 NULL 的目标 keyring 引用，keyring 是其借用指针；
	 *   index_key 由 type+description 构成查找/链接索引；
	 *   prep 承载 preparse 后的 payload、description、配额和 expiry；
	 *   edit 是 assoc_array 的暂存编辑，必须以 __key_link_end() 收尾；
	 *   cred 是 current_cred() 返回的非 NULL 借用快照；
	 *   key_ref 是带 possession 位的返回引用；key 只在新建分支有效；
	 *   restrict_link 是目标 ring 的策略借用指针；allow_update 区分“命中则更新”
	 *   与“命中即 -EEXIST”的两个公开 API。
	 *
	 * 入口：调用者已经持有 keyring_ref 所代表的引用，但不预先持有 ring 写锁；
	 * @type 不可为 NULL 且必须是 NUL 结尾类型名，@description 可为 NULL 但仅在
	 * 类型 preparse 能生成 description 时允许，@payload 可为 NULL 但仅在 plen=0
	 * 或具体类型允许空输入时有意义，@plen 是 payload 字节数，@perm/flags 是新
	 * key 的权限和创建策略，@allow_update 决定命中后的更新或 -EEXIST 语义。
	 * 成功返回持有 key 的 key_ref；所有失败出口释放 type 读锁、preparse 资源、
	 * edit 和新 key 引用。新建与更新共用前半段事务，只有 found_matching_key
	 * 在释放 ring 编辑锁后进入已有 key 的更新路径。
	 * ret 保存中间错误；restrict_link 是目标 ring 限制的借用快照，不能在本函数
	 * 结束后继续使用；key 为 NULL 只表示尚未进入新建分支。
	 */

	/* look up the key type to see if it's one of the registered kernel
	 * types */
	/*
	 * key_type_lookup() 成功带着 key_types_sem 读锁返回；因此后续访问 type 的
	 * 回调和 def_datalen 都受模块卸载保护，所有出口必须 key_type_put()。
	 */
	index_key.type = key_type_lookup(type);
	if (IS_ERR(index_key.type)) {
		key_ref = ERR_PTR(-ENODEV);
		goto error;
	}

	key_ref = ERR_PTR(-EINVAL);
	/* 类型必须提供 instantiate；没有 description 时必须能由 preparse 产生。 */
	if (!index_key.type->instantiate ||
	    (!index_key.description && !index_key.type->preparse))
		goto error_put_type;

	keyring = key_ref_to_ptr(keyring_ref);
	/* keyring_ref 的 possession 位留在 key_ref 中，裸 keyring 只用于对象访问。 */

	key_check(keyring);

	if (!(flags & KEY_ALLOC_BYPASS_RESTRICTION))
		/* restrict_link 是借用指针；只在本次 ring 事务持有必要的读侧稳定性。 */
		restrict_link = keyring->restrict_link;

	key_ref = ERR_PTR(-ENOTDIR);
	if (keyring->type != &key_type_keyring)
		/* 目标对象必须真的是 keyring，否则不能执行关联数组链接。 */
		goto error_put_type;

	memset(&prep, 0, sizeof(prep));
	prep.orig_description = description;
	prep.data = payload;
	prep.datalen = plen;
	prep.quotalen = index_key.type->def_datalen;
	prep.expiry = TIME64_MAX;
	/* prep 初始为空；preparse 可以接管外部 payload 或补出 canonical description。 */
	if (index_key.type->preparse) {
		ret = index_key.type->preparse(&prep);
		if (ret < 0) {
			key_ref = ERR_PTR(ret);
			goto error_free_prep;
		}
		if (!index_key.description)
			/* 没有原始描述时，preparse 必须提供可搜索描述，否则无法形成 index_key。 */
			index_key.description = prep.description;
		key_ref = ERR_PTR(-EINVAL);
		if (!index_key.description)
			goto error_free_prep;
	}
	index_key.desc_len = strlen(index_key.description);
	/* key_set_index_key() 计算哈希并固定短描述字段，后续链接锁以此键定位冲突。 */
	key_set_index_key(&index_key);

	ret = __key_link_lock(keyring, &index_key);
	/*
	 * 取得 ring 写侧后，直到 __key_link_end() 前，目标索引不会被并发链接/删除
	 * 改写；失败时仍要走 error_free_prep，而不是泄漏 preparsed ownership。
	 */
	if (ret < 0) {
		key_ref = ERR_PTR(ret);
		goto error_free_prep;
	}

	ret = __key_link_begin(keyring, &index_key, &edit);
	/* edit 记录关联数组的待提交变化；begin 失败时没有可提交的编辑计划。 */
	if (ret < 0) {
		key_ref = ERR_PTR(ret);
		goto error_link_end;
	}

	if (restrict_link && restrict_link->check) {
		/* 策略检查发生在分配新 key 和修改 ring 之前，拒绝不会产生半发布对象。 */
		ret = restrict_link->check(keyring, index_key.type,
					   &prep.payload, restrict_link->key);
		if (ret < 0) {
			key_ref = ERR_PTR(ret);
			goto error_link_end;
		}
	}

	/* if we're going to allocate a new key, we're going to have
	 * to modify the keyring */
	/*
	 * 即使最终走 update 分支，也要先确认调用者有修改目标 ring 的权限；创建
	 * 分支还需要把新 key 链入其中。这里的 keyring_ref 保留 possession 属性，
	 * key_permission() 同时检查当前凭据与 keyring 语义。
	 */
	ret = key_permission(keyring_ref, KEY_NEED_WRITE);
	if (ret < 0) {
		key_ref = ERR_PTR(ret);
		goto error_link_end;
	}

	/* if it's requested and possible to update this type of key, search
	 * for an existing key of the same type and description in the
	 * destination keyring and update that instead if possible
	 */
	/*
	 * allow_update=true 时只有类型支持 update 才查找并复用已有 key；false 时
	 * 仍执行查找，但命中即返回 -EEXIST。find_key_to_update() 返回的 key_ref
	 * 已 pin 住对象，允许稍后释放 ring 锁再更新，避免持锁运行类型回调。
	 */
	if (allow_update) {
		if (index_key.type->update) {
			key_ref = find_key_to_update(keyring_ref, &index_key);
			if (key_ref)
				goto found_matching_key;
		}
	} else {
		/* key_create() 的语义是严格新建；命中对象的引用先由 key_ref_put() 消费。 */
		key_ref = find_key_to_update(keyring_ref, &index_key);
		if (key_ref) {
			key_ref_put(key_ref);
			key_ref = ERR_PTR(-EEXIST);
			goto error_link_end;
		}
	}

	/* if the client doesn't provide, decide on the permissions we want */
	/*
	 * KEY_PERM_UNDEF 只在新建时解析为默认策略：possessor 可查看/搜索/链接/
	 * 设属性，若类型能读则允许读，keyring 或可更新类型再允许写。显式 perm
	 * 完全由调用者提供，不在这里合并默认位。
	 */
	if (perm == KEY_PERM_UNDEF) {
		perm = KEY_POS_VIEW | KEY_POS_SEARCH | KEY_POS_LINK | KEY_POS_SETATTR;
		perm |= KEY_USR_VIEW;

		if (index_key.type->read)
			perm |= KEY_POS_READ;

		if (index_key.type == &key_type_keyring ||
		    index_key.type->update)
			perm |= KEY_POS_WRITE;
	}

	/* allocate a new key */
	/* key_alloc() 此处还未实例化；它会记账、复制 description、执行 LSM 并发布 serial。 */
	key = key_alloc(index_key.type, index_key.description,
			cred->fsuid, cred->fsgid, cred, perm, flags, NULL);
	if (IS_ERR(key)) {
		/* 新 key 尚未拥有链接或 prep；只需把错误转成 key_ref 并结束 ring 事务。 */
		key_ref = ERR_CAST(key);
		goto error_link_end;
	}

	/* instantiate it and link it into the target keyring */
	/*
	 * 这是新建路径的提交边界：类型实例化成功且链接事务完成后，key 才同时
	 * 具备 payload 和目标 ring 可见性。失败时 key_alloc() 返回的 usage 由 key_put
	 * 消费，edit/prep 则由下面的标签处理。
	 */
	ret = __key_instantiate_and_link(key, &prep, keyring, NULL, &edit);
	if (ret < 0) {
		key_put(key);
		key_ref = ERR_PTR(ret);
		goto error_link_end;
	}

	security_key_post_create_or_update(keyring, key, payload, plen, flags,
					   true);
	/* LSM post hook 观察已经成功发布的新 key；不参与失败回滚。 */

	key_ref = make_key_ref(key, is_key_possessed(keyring_ref));

error_link_end:
	/*
	 * 新建分支或策略/权限失败都从这里收束：结束 edit、释放 preparsed
	 * 临时所有权、解锁 key type。标签按资源取得的逆序排列，允许部分成功路径安全落入。
	 */
	__key_link_end(keyring, &index_key, edit);
error_free_prep:
	/* free_preparse() 只释放仍由 prep 持有的部分；已被 instantiate 转移的指针已清空。 */
	if (index_key.type->preparse)
		index_key.type->free_preparse(&prep);
error_put_type:
	/* 读锁计数归还后，模块才可从 key_types_list 移除该类型。 */
	key_type_put(index_key.type);
error:
	return key_ref;

 found_matching_key:
	/* we found a matching key, so we're going to try to update it
	 * - we can drop the locks first as we have the key pinned
	 */
	/*
	 * 命中的 key 已有 usage 引用，因此可以先结束目标 ring 编辑并释放其写锁，
	 * 再等待构造完成和执行 type->update。这个顺序避免在 ring 锁下睡眠/回调，
	 * 也避免与另一条构造路径反向获取 key->sem。
	 */
	__key_link_end(keyring, &index_key, edit);

	key = key_ref_to_ptr(key_ref);
	if (test_bit(KEY_FLAG_USER_CONSTRUCT, &key->flags)) {
		/* 用户态构造中的 key 必须先等到最终 state；true 表示等待后重新检查。 */
		ret = wait_for_key_construction(key, true);
		if (ret < 0) {
			/* 等待失败时释放 find_key_to_update() 提供的 pin，继续共享 prep/type 清理。 */
			key_ref_put(key_ref);
			key_ref = ERR_PTR(ret);
			goto error_free_prep;
		}
	}

	key_ref = __key_update(key_ref, &prep);
	/* __key_update() 成功保留 key_ref，失败已在内部 key_put()，故不能重复 put。 */

	if (!IS_ERR(key_ref))
		/* post hook 只在 update 真正成功后调用；它看见的是已持久化的新内容。 */
		security_key_post_create_or_update(keyring, key, payload, plen,
						   flags, false);

	goto error_free_prep;
}

/**
 * key_create_or_update - Update or create and instantiate a key.
 * @keyring_ref: A pointer to the destination keyring with possession flag.
 * @type: The type of key.
 * @description: The searchable description for the key.
 * @payload: The data to use to instantiate or update the key.
 * @plen: The length of @payload.
 * @perm: The permissions mask for a new key.
 * @flags: The quota flags for a new key.
 *
 * Search the destination keyring for a key of the same description and if one
 * is found, update it, otherwise create and instantiate a new one and create a
 * link to it from that keyring.
 *
 * If perm is KEY_PERM_UNDEF then an appropriate key permissions mask will be
 * concocted.
 *
 * Returns a pointer to the new key if successful, -ENODEV if the key type
 * wasn't available, -ENOTDIR if the keyring wasn't a keyring, -EACCES if the
 * caller isn't permitted to modify the keyring or the LSM did not permit
 * creation of the key.
 *
 * On success, the possession flag from the keyring ref will be tacked on to
 * the key ref before it is returned.
 */
/*
 * key_create_or_update() - 按类型/描述更新已有 key，否则创建并实例化新 key
 *
 * @keyring_ref 是有效且带 possession 位的目标 keyring 引用；@type 是不可为
 * NULL 的类型名；@description 可为 NULL 但仅在类型 preparse 能生成描述时允许；
 * @payload 可为 NULL 但仅在 plen=0 或类型允许空输入时有意义，@plen 是输入字节数；@perm
 * 是新 key 权限，KEY_PERM_UNDEF 表示使用类型相关默认值；@flags 控制配额等
 * 创建属性。函数可睡眠，返回带同一 possession 属性的 key_ref，或带 errno
 * 的错误指针；成功时新建和更新都已完成 post-create/update 通知。真正事务
 * 由 __key_create_or_update() 完成，本包装器只选择 allow_update=true。
 */
key_ref_t key_create_or_update(key_ref_t keyring_ref,
			       const char *type,
			       const char *description,
			       const void *payload,
			       size_t plen,
			       key_perm_t perm,
			       unsigned long flags)
{
	return __key_create_or_update(keyring_ref, type, description, payload,
				      plen, perm, flags, true);
}
EXPORT_SYMBOL(key_create_or_update);

/**
 * key_create - Create and instantiate a key.
 * @keyring_ref: A pointer to the destination keyring with possession flag.
 * @type: The type of key.
 * @description: The searchable description for the key.
 * @payload: The data to use to instantiate or update the key.
 * @plen: The length of @payload.
 * @perm: The permissions mask for a new key.
 * @flags: The quota flags for a new key.
 *
 * Create and instantiate a new key and link to it from the destination keyring.
 *
 * If perm is KEY_PERM_UNDEF then an appropriate key permissions mask will be
 * concocted.
 *
 * Returns a pointer to the new key if successful, -EEXIST if a key with the
 * same description already exists, -ENODEV if the key type wasn't available,
 * -ENOTDIR if the keyring wasn't a keyring, -EACCES if the caller isn't
 * permitted to modify the keyring or the LSM did not permit creation of the
 * key.
 *
 * On success, the possession flag from the keyring ref will be tacked on to
 * the key ref before it is returned.
 */
/*
 * key_create() - 强制创建并实例化一个新 key
 *
 * 参数与 key_create_or_update() 相同：keyring_ref 是有效且带 possession 的目标引用，
 * type 不可为 NULL，description/payload 的可空条件与类型 preparse 和 plen 相同，
 * plen 描述输入字节数，perm/flags 控制权限和配额。该函数
 * 可睡眠，成功返回新 key 的带 possession key_ref；如果同类型同描述 key 已存
 * 在返回 -EEXIST，不会更新旧对象。其他失败值包括 -ENODEV、-ENOTDIR、权限或
 * LSM 错误以及类型/preparse 错误；所有部分资源由内部 cleanup 释放。它是
 * __key_create_or_update(..., allow_update=false) 的语义薄包装。
 */
key_ref_t key_create(key_ref_t keyring_ref,
		     const char *type,
		     const char *description,
		     const void *payload,
		     size_t plen,
		     key_perm_t perm,
		     unsigned long flags)
{
	return __key_create_or_update(keyring_ref, type, description, payload,
				      plen, perm, flags, false);
}
EXPORT_SYMBOL(key_create);

/**
 * key_update - Update a key's contents.
 * @key_ref: The pointer (plus possession flag) to the key.
 * @payload: The data to be used to update the key.
 * @plen: The length of @payload.
 *
 * Attempt to update the contents of a key with the given payload data.  The
 * caller must be granted Write permission on the key.  Negative keys can be
 * instantiated by this method.
 *
 * Returns 0 on success, -EACCES if not permitted and -EOPNOTSUPP if the key
 * type does not support updating.  The key type may return other errors.
 */
/*
 * key_update() - 在调用者有写权限的已有 key 上替换/更新 payload
 *
 * @key_ref 是有效且带 possession 位的 key 引用，调用者负责保持其 usage；@payload
 * 可为 NULL 但仅在 plen=0 或类型允许空输入时有意义，@plen 是类型 preparse/update
 * 使用的输入字节数，调用期间由调用者保持有效。函数可睡眠：先做写权限检查
 * 和可更新能力检查，再 preparse，最后持有 key->sem 写锁调用类型回调。成功
 * 返回 0，并可能把 negative key 正向
 * 实例化、发出更新通知；权限失败返回 -EACCES 等，类型不支持返回
 * -EOPNOTSUPP，preparse 或类型回调的其他 errno 原样返回。key_ref 本身不由
 * 本函数消费，调用者仍需 key_ref_put()；prep 的临时 ownership 在所有出口释放。
 */
int key_update(key_ref_t key_ref, const void *payload, size_t plen)
{
	struct key_preparsed_payload prep;
	struct key *key = key_ref_to_ptr(key_ref);
	int ret;

	/* key 是 key_ref 解码出的借用指针，prep 保存 preparse 产生的临时 ownership，
	 * ret 依次记录权限、preparse 和类型 update 的返回值。 */

	/* key_ref 是带属性的伪指针；这里只借用其 key，不改变引用计数。 */

	key_check(key);

	/* the key must be writable */
	/* 权限拒绝发生在任何 preparse 和锁操作之前，避免未授权输入触碰 key。 */
	ret = key_permission(key_ref, KEY_NEED_WRITE);
	if (ret < 0)
		return ret;

	/* attempt to update it if supported */
	/* 没有类型回调时不能猜测 payload 格式，直接报告不支持。 */
	if (!key->type->update)
		return -EOPNOTSUPP;

	memset(&prep, 0, sizeof(prep));
	prep.data = payload;
	prep.datalen = plen;
	prep.quotalen = key->type->def_datalen;
	prep.expiry = TIME64_MAX;
	if (key->type->preparse) {
		/* preparse 可能产生新 description/payload；失败时进入统一 free_preparse。 */
		ret = key->type->preparse(&prep);
		if (ret < 0)
			goto error;
	}

	down_write(&key->sem);

	/*
	 * key->sem 保护 update 与 revoke/其他 update 的互斥；类型回调可调整 quota
	 * 并改变 payload，但不能在这里绕过 key 的写侧协议。
	 */
	ret = key->type->update(key, &prep);
	if (ret == 0) {
		/* Updating a negative key positively instantiates it */
		/* 成功更新负 key 后发布正状态；通知在状态发布后发送。 */
		mark_key_instantiated(key, 0);
		notify_key(key, NOTIFY_KEY_UPDATED, 0);
	}

	up_write(&key->sem);

error:
	/* 释放尚未由 type->update() 接管的 preparsed 资源；没有资源时是空操作。 */
	if (key->type->preparse)
		key->type->free_preparse(&prep);
	return ret;
}
EXPORT_SYMBOL(key_update);

/**
 * key_revoke - Revoke a key.
 * @key: The key to be revoked.
 *
 * Mark a key as being revoked and ask the type to free up its resources.  The
 * revocation timeout is set and the key and all its links will be
 * automatically garbage collected after key_gc_delay amount of time if they
 * are not manually dealt with first.
 */
/*
 * key_revoke() - 把 key 标记为 revoked，并让类型释放其可撤销资源
 *
 * @key 不可为 NULL，是调用者持有的借用指针，函数可睡眠但没有返回值。key->sem 写锁把
 * revoke 与 update/读取 payload 的协议串行化；nested=1 告知 lockdep，当前
 * 路径可能正在持有刚实例化 key 的 sem，同时撤销 authkey。第一次设置
 * KEY_FLAG_REVOKED 时发送通知、调用可选 type->revoke，并记录 revoked_at，
 * 再安排不早于 key_gc_delay 的 GC；重复 revoke 是幂等的。撤销不立即删除
 * serial 或 keyring 链接，调用者引用仍有效，后续搜索/使用会根据 flag 拒绝。
 */
void key_revoke(struct key *key)
{
	time64_t time;

	/* time 只在撤销成功的第一次状态转换中记录当前 real time，作为 GC 截止依据。 */

	key_check(key);

	/* make sure no one's trying to change or use the key when we mark it
	 * - we tell lockdep that we might nest because we might be revoking an
	 *   authorisation key whilst holding the sem on a key we've just
	 *   instantiated
	 */
	/* 原注释说明这里既要排斥并发使用/修改，又允许嵌套授权 key 的 sem；写锁
	 * 不负责延长 key 生命周期，调用者仍须持有引用。 */
	down_write_nested(&key->sem, 1);
	if (!test_and_set_bit(KEY_FLAG_REVOKED, &key->flags)) {
		/* test_and_set_bit() 只让第一个撤销者执行通知、类型回调和计时。 */
		notify_key(key, NOTIFY_KEY_REVOKED, 0);
		if (key->type->revoke)
			key->type->revoke(key);

		/* set the death time to no more than the expiry time */
		/* revoked_at 保留最早的撤销时刻，避免重复 revoke 把 GC 推迟；schedule_gc
		 * 只安排后续回收，不在这里同步释放 serial、payload 或链接。 */
		time = ktime_get_real_seconds();
		if (key->revoked_at == 0 || key->revoked_at > time) {
			key->revoked_at = time;
			key_schedule_gc(key->revoked_at + key_gc_delay);
		}
	}

	up_write(&key->sem);
}
EXPORT_SYMBOL(key_revoke);

/**
 * key_invalidate - Invalidate a key.
 * @key: The key to be invalidated.
 *
 * Mark a key as being invalidated and have it cleaned up immediately.  The key
 * is ignored by all searches and other operations from this point.
 */
/*
 * key_invalidate() - 立即让 key 对搜索和后续操作不可见
 *
 * @key 不可为 NULL，是借用的 key 指针，函数无返回值且可睡眠。INVALIDATED 是比普通 revoke
 * 更强的摘除标记：第一次设置后发送通知并安排 keyring links GC；重复调用幂等。
 * 外层 test_bit() 只是避免重复写锁，真正的 test_and_set_bit() 仍在 key->sem
 * 写侧完成。此函数不释放 caller 的引用，也不保证 slab 已立即回收。
 */
void key_invalidate(struct key *key)
{
	kenter("%d", key_serial(key));

	key_check(key);

	if (!test_bit(KEY_FLAG_INVALIDATED, &key->flags)) {
		/* 快速检查不能替代锁内原子转换，两个并发调用仍须在锁内竞争。 */
		down_write_nested(&key->sem, 1);
		if (!test_and_set_bit(KEY_FLAG_INVALIDATED, &key->flags)) {
			/* 设置标志后搜索路径忽略该 key；通知和 links GC 在同一状态转换内发布。 */
			notify_key(key, NOTIFY_KEY_INVALIDATED, 0);
			key_schedule_gc_links();
		}
		up_write(&key->sem);
	}
}
EXPORT_SYMBOL(key_invalidate);

/**
 * generic_key_instantiate - Simple instantiation of a key from preparsed data
 * @key: The key to be instantiated
 * @prep: The preparsed data to load.
 *
 * Instantiate a key from preparsed data.  We assume we can just copy the data
 * in directly and clear the old pointers.
 *
 * This can be pointed to directly by the key type instantiate op pointer.
 */
/*
 * generic_key_instantiate() - 把 preparsed 的四个 payload 槽直接转给 key
 *
 * @key 不可为 NULL，是持有引用且尚未实例化的 key；@prep 不可为 NULL，是类型 preparse 填充的临时输入，
 * 其中 payload.data[0..3] 的 ownership 在成功时全部转移给 key，函数可睡眠。
 * 返回 0 表示配额已调整且指针已转移；返回 -EDQUOT 等错误表示 prep 仍拥有
 * 原 payload，调用者可由 free_preparse() 清理。该 helper 不设置 key->state，
 * 由 __key_instantiate_and_link() 在本函数返回后以 release store 发布。
 */
int generic_key_instantiate(struct key *key, struct key_preparsed_payload *prep)
{
	int ret;

	/* ret 是 quota reservation 的结果；成功时 prep 的四个 payload 指针转移给 key，
	 * 失败时 prep 仍保留原 ownership。 */

	pr_devel("==>%s()\n", __func__);

	/* 先为 preparsed 长度建立 quota reservation；失败时不能接管任何 payload。 */
	ret = key_payload_reserve(key, prep->quotalen);
	if (ret == 0) {
		/*
		 * 第一个槽通过 RCU 发布：读侧可能用 rcu_dereference 取得它，因此先把
		 * 新指针写入，再清空 prep 中的所有权。其余槽仍在构造协议内复制，
		 * 最终 state 发布前不会被合法读取者当作已完成 key。
		 */
		rcu_assign_keypointer(key, prep->payload.data[0]);
		key->payload.data[1] = prep->payload.data[1];
		key->payload.data[2] = prep->payload.data[2];
		key->payload.data[3] = prep->payload.data[3];
		prep->payload.data[0] = NULL;
		prep->payload.data[1] = NULL;
		prep->payload.data[2] = NULL;
		prep->payload.data[3] = NULL;
		/* 清空 prep 表示四个指针已转移；free_preparse() 不会二次释放它们。 */
	}
	pr_devel("<==%s() = %d\n", __func__, ret);
	return ret;
}
EXPORT_SYMBOL(generic_key_instantiate);

/**
 * register_key_type - Register a type of key.
 * @ktype: The new key type.
 *
 * Register a new key type.
 *
 * Returns 0 on success or -EEXIST if a type of this name already exists.
 */
/*
 * register_key_type() - 将一个 key_type 加入全局类型注册表
 *
 * @ktype 不可为 NULL，是调用者/模块拥有的静态或长期对象；name、回调和 lock_class 必须
 * 在注册期间有效。函数可睡眠，写锁独占 key_types_list，成功返回 0 并发布
 * 类型；同名类型返回 -EEXIST，原对象不插入。清空 lock_class 让该类型的
 * key->sem 建立自己的 lockdep 类；注册成功后 key_type_lookup() 可读锁访问
 * 它。模块卸载必须以 unregister_key_type() 配对并等待读者退出。
 */
int register_key_type(struct key_type *ktype)
{
	struct key_type *p;
	int ret;

	/* p 是注册链表中的借用类型，ret 先设为 -EEXIST，只有完成唯一性检查并插入后改为 0。 */

	/* 每次注册重新初始化 lockdep 类，避免复用模块对象时继承旧分类。 */
	memset(&ktype->lock_class, 0, sizeof(ktype->lock_class));

	ret = -EEXIST;
	down_write(&key_types_sem);

	/* disallow key types with the same name */
	/* 同名检查和插入必须在同一写锁内，否则并发注册可产生不可区分的类型。 */
	list_for_each_entry(p, &key_types_list, link) {
		if (strcmp(p->name, ktype->name) == 0)
			goto out;
	}

	/* store the type */
	/* 链表插入后，读锁持有者才能通过 name 找到完整初始化的 ktype。 */
	list_add(&ktype->link, &key_types_list);

	pr_notice("Key type %s registered\n", ktype->name);
	ret = 0;

out:
	up_write(&key_types_sem);
	return ret;
}
EXPORT_SYMBOL(register_key_type);

/**
 * unregister_key_type - Unregister a type of key.
 * @ktype: The key type.
 *
 * Unregister a key type and mark all the extant keys of this type as dead.
 * Those keys of this type are then destroyed to get rid of their payloads and
 * they and their links will be garbage collected as soon as possible.
 */
/*
 * unregister_key_type() - 从注册表摘除类型并杀死其现存 key
 *
 * @ktype 不可为 NULL，是已经注册且由调用者保证身份的类型对象。函数可睡眠且无返回值：
 * 先用写锁从 list 摘除，随后降级为读锁，阻止新的 lookup 获得该类型，同时
 * 允许 key_gc_keytype() 扫描并把既有 key 标记为 dead、释放 payload/链接；
 * 读锁释放后模块才可真正离开。降级建立了“停止新使用、完成旧对象清理”的
 * 窗口，避免模块回调在仍有 key 使用时被卸载。
 */
void unregister_key_type(struct key_type *ktype)
{
	/* 写侧摘除先阻止未来 lookup；读侧清理期间仍阻止最后的模块卸载。 */
	down_write(&key_types_sem);
	list_del_init(&ktype->link);
	downgrade_write(&key_types_sem);
	key_gc_keytype(ktype);
	/* 既有 key 的死亡标记/回收由 GC 协议处理，当前函数等待该阶段完成。 */
	pr_notice("Key type %s unregistered\n", ktype->name);
	up_read(&key_types_sem);
}
EXPORT_SYMBOL(unregister_key_type);

/*
 * Initialise the key management state.
 */
/*
 * key_init() - 初始化 key 子系统的 slab、内建类型和 root 配额记录
 *
 * 初始化入口在启动阶段调用，运行于可睡眠上下文；无参数、无直接返回值，
 * 失败由 SLAB_PANIC 直接终止启动。key_jar 之后供 key_alloc() 使用；四个
 * 内建类型先加入注册表，root_key_user 最后插入 UID 配额树，确保早期 key
 * 创建路径已经拥有配额账本。此时树为空且没有并发使用者，因此初始化不需
 * 额外锁；完成后正常路径依靠对应的锁/引用协议。
 */
void __init key_init(void)
{
	/* allocate a slab in which we can store keys */
	/* 固定大小 slab 便于 key 的快速分配；HWCACHE_ALIGN 减少共享 cache line，
	 * NO_MERGE 保持对象边界，PANIC 表示 key 子系统不能无 slab 运行。 */
	key_jar = kmem_cache_create("key_jar", sizeof(struct key),
			0, SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_NO_MERGE, NULL);

	/* add the special key types */
	/* 这些类型是内核启动即需要的基础实现，不经模块注册流程也必须可查找。 */
	list_add_tail(&key_type_keyring.link, &key_types_list);
	list_add_tail(&key_type_dead.link, &key_types_list);
	list_add_tail(&key_type_user.link, &key_types_list);
	list_add_tail(&key_type_logon.link, &key_types_list);

	/* record the root user tracking */
	/* root_key_user 是静态生命周期的 GLOBAL_ROOT_UID 配额账本初始节点。 */
	rb_link_node(&root_key_user.node,
		     NULL,
		     &key_user_tree.rb_node);

	rb_insert_color(&root_key_user.node,
			&key_user_tree);
}
