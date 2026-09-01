// SPDX-License-Identifier: GPL-2.0-only
/*
 * In memory quota format relies on quota infrastructure to store dquot
 * information for us. While conventional quota formats for file systems
 * with persistent storage can load quota information into dquot from the
 * storage on-demand and hence quota dquot shrinker can free any dquot
 * that is not currently being used, it must be avoided here. Otherwise we
 * can lose valuable information, user provided limits, because there is
 * no persistent storage to load the information from afterwards.
 *
 * One information that in-memory quota format needs to keep track of is
 * a sorted list of ids for each quota type. This is done by utilizing
 * an rb tree which root is stored in mem_dqinfo->dqi_priv for each quota
 * type.
 *
 * This format can be used to support quota on file system without persistent
 * storage such as tmpfs.
 *
 * Author:	Lukas Czerner <lczerner@redhat.com>
 *		Carlos Maiolino <cmaiolino@redhat.com>
 *
 * Copyright (C) 2023 Red Hat, Inc.
 */
/*
 * tmpfs 没有可在回收后重新读取的磁盘 quota 文件，因此这里借用通用 dquot
 * 框架管理用量，却把每个 uid/gid 的限制永久保存在超级块私有红黑树中。
 * dquot cache 只是一份可回收的工作副本；红黑树才是挂载生命周期内的真值。
 */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
/* slab 提供树节点分配，rbtree 提供按 id 有序枚举，shmem_fs 连接挂载私有限制。 */
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/shmem_fs.h>

#include <linux/quotaops.h>
#include <linux/quota.h>

/*
 * The following constants define the amount of time given a user
 * before the soft limits are treated as hard limits (usually resulting
 * in an allocation failure). The timer is started when the user crosses
 * their soft limit, it is reset when they go below their soft limit.
 */
/*
 * 下面两个常量给块数和 inode 数软限制各提供 7 天宽限期：首次越过软限制
 * 开始计时，退回限制内清零；超时后通用 quota 层把软限制当作硬限制拒绝分配。
 */
#define SHMEM_MAX_IQ_TIME 604800	/* (7*24*60*60) 1 week */
#define SHMEM_MAX_DQ_TIME 604800	/* (7*24*60*60) 1 week */

struct quota_id {
	/* node 以 id 为键嵌入每种 quota 类型各自的红黑树。 */
	struct rb_node	node;
	/* id 是 init_user_ns 中的原始 uid/gid 数值。 */
	qid_t		id;
	/* 四个限制以 quota 层 qsize_t 单位保存；当前用量仍由 dquot 维护。 */
	qsize_t		bhardlimit;
	qsize_t		bsoftlimit;
	qsize_t		ihardlimit;
	qsize_t		isoftlimit;
};

/*
 * 业务背景：通用 quota 启用流程先询问格式是否可用；tmpfs 格式没有文件头可校验，
 * 因而只需声明该内存格式始终匹配。
 * 入参：sb 是启用 quota 的借用超级块；type 是待检查的 quota 类型索引，均不转移所有权。
 * 出参/返回：恒返 1 表示格式有效；不修改 sb，也不取得引用。
 * 注意事项：调用者持有 quota 启停所需串行化；本函数不睡眠，不能把 1 理解为找到磁盘文件。
 */
static int shmem_check_quota_file(struct super_block *sb, int type)
{
	/* There is no real quota file, nothing to do */
	/* 实际不存在 quota 文件；1 只满足 quota 格式探测协议。 */
	return 1;
}

/*
 * There is no real quota file. Just allocate rb_root for quota ids and
 * set limits
 */
/*
 * tmpfs 不读取磁盘，而是为 type 创建一棵空的持久红黑树，并初始化该类型可接受的
 * 最大限制与宽限期；树在 quotaoff 时由 shmem_free_file_info() 销毁。
 */
/*
 * 业务背景：dquot_load_quota_sb() 在启用某类 quota 时调用本函数建立格式私有状态。
 * 入参：sb 为借用超级块；type 是 sb_dqopt()->info[] 的有效类型索引。
 * 出参/返回：成功返回 0 并让 dqi_priv 持有新 rb_root；内存不足返回 -ENOMEM，未发布树。
 * 注意事项：GFP_NOFS 避免配额初始化递归进入文件系统；可睡眠，调用者负责启停串行化。
 */
static int shmem_read_file_info(struct super_block *sb, int type)
{
	/* dqopt/info 都借用自 sb，有效期受超级块和 quota 启用阶段约束。 */
	struct quota_info *dqopt = sb_dqopt(sb);
	struct mem_dqinfo *info = &dqopt->info[type];

	/* 阶段 1：先分配零化根；NULL 根节点天然表示尚无任何 uid/gid 条目。 */
	info->dqi_priv = kzalloc_obj(struct rb_root, GFP_NOFS);
	if (!info->dqi_priv)
		return -ENOMEM;

	/* 阶段 2：发布格式能力与默认计时策略，供通用 setquota/check 路径使用。 */
	info->dqi_max_spc_limit = SHMEM_QUOTA_MAX_SPC_LIMIT;
	info->dqi_max_ino_limit = SHMEM_QUOTA_MAX_INO_LIMIT;

	info->dqi_bgrace = SHMEM_MAX_DQ_TIME;
	info->dqi_igrace = SHMEM_MAX_IQ_TIME;
	info->dqi_flags = 0;

	return 0;
}

/*
 * 业务背景：通用 quota 层要求格式提供“写回格式信息”回调；tmpfs 的格式信息已在内存中。
 * 入参：sb 为借用超级块，type 为 quota 类型；二者均仅用于满足统一接口。
 * 出参/返回：恒返 0，无输出参数、无持久化副作用。
 * 注意事项：不睡眠；成功仅表示无需写回，不代表数据已落盘。
 */
static int shmem_write_file_info(struct super_block *sb, int type)
{
	/* There is no real quota file, nothing to do */
	/* 没有真实 quota 文件，因此当前内存状态已经是最终状态。 */
	return 0;
}

/*
 * Free all the quota_id entries in the rb tree and rb_root.
 */
/* 关闭该 quota 类型时，释放红黑树中的全部 id/限制记录以及根对象。 */
/*
 * 业务背景：quotaoff 的最后阶段调用本函数撤销 read_file_info() 建立的私有状态。
 * 入参：sb 为借用超级块；type 选择待销毁的 info 槽位。
 * 出参/返回：恒返 0；dqi_priv 先变为 NULL，所有 quota_id 与 rb_root 随后释放。
 * 注意事项：调用者已阻止新的该类型访问；函数可释放内存，旧 root/entry 指针返回后均失效。
 */
static int shmem_free_file_info(struct super_block *sb, int type)
{
	/* root 是本函数接管并最终释放的格式私有树；node/entry 是遍历游标。 */
	struct mem_dqinfo *info = &sb_dqopt(sb)->info[type];
	struct rb_root *root = info->dqi_priv;
	struct quota_id *entry;
	struct rb_node *node;

	/* 先摘除公开入口，避免清理期间再通过 info 找到半销毁树。 */
	info->dqi_priv = NULL;
	node = rb_first(root);
	while (node) {
		entry = rb_entry(node, struct quota_id, node);
		node = rb_next(&entry->node);

		/* rb_next 必须在 erase/free 前取得，否则会解引用已释放节点。 */
		rb_erase(&entry->node, root);
		kfree(entry);
	}

	kfree(root);
	return 0;
}

/*
 * 业务背景：quotactl 枚举配额记录时，需要从当前 qid 找到树中第一个 id >= 它的条目。
 * 入参：sb 为借用超级块；qid 是输入输出参数，输入含类型和起始 id，成功时被同类型结果覆盖。
 * 出参/返回：成功返 0；未启用返 -ESRCH；树中无候选返 -ENOENT；qid 失败时保持原值。
 * 注意事项：dqio_sem 读锁稳定整棵树，可与查询并发但排斥 acquire/release；本函数会睡眠。
 */
static int shmem_get_next_id(struct super_block *sb, struct kqid *qid)
{
	/* id 是 init_user_ns 数值键；entry 保留最后比较节点以求后继。 */
	struct mem_dqinfo *info = sb_dqinfo(sb, qid->type);
	struct rb_node *node;
	qid_t id = from_kqid(&init_user_ns, *qid);
	struct quota_info *dqopt = sb_dqopt(sb);
	struct quota_id *entry = NULL;
	int ret = 0;

	/* quotaoff 后 dqi_priv 不再有效，必须在取锁和解引用前拒绝。 */
	if (!sb_has_quota_active(sb, qid->type))
		return -ESRCH;

	/* 阶段 1：在读锁内按 id 二分查找完全相等的节点或搜索终止位置。 */
	down_read(&dqopt->dqio_sem);
	node = ((struct rb_root *)info->dqi_priv)->rb_node;
	while (node) {
		entry = rb_entry(node, struct quota_id, node);

		/* 比较只改变借用游标；命中时 entry 就是 >= 输入 id 的最小候选。 */
		if (id < entry->id)
			node = node->rb_left;
		else if (id > entry->id)
			node = node->rb_right;
		else
			goto got_next_id;
	}

	/* 空树没有最后比较节点，自然也没有可枚举 id。 */
	if (!entry) {
		ret = -ENOENT;
		goto out_unlock;
	}

	/* 搜索落在最后节点右侧时，候选只能是该节点的中序后继。 */
	if (id > entry->id) {
		node = rb_next(&entry->node);
		if (!node) {
			ret = -ENOENT;
			goto out_unlock;
		}
		entry = rb_entry(node, struct quota_id, node);
	}

got_next_id:
	/* 阶段 2：只在找到候选后提交输出；类型保持不变并重新编码为 kqid。 */
	*qid = make_kqid(&init_user_ns, qid->type, entry->id);
out_unlock:
	/* 所有成功/未找到路径在同一出口释放树的读锁。 */
	up_read(&dqopt->dqio_sem);
	return ret;
}

/*
 * Load dquot with limits from existing entry, or create the new entry if
 * it does not exist.
 */
/* 找到持久条目时恢复限制；首次见到 id 时创建条目并继承 tmpfs 挂载默认硬限制。 */
/*
 * 业务背景：dqget() 得到未激活 dquot 后调用这里，把缓存工作副本接到内存格式真值上。
 * 入参：dquot 是通用 quota 层持有的输入输出对象；本函数借用它，不接管其引用。
 * 出参/返回：成功返 0、填充 dq_dqb 并最后置 DQ_ACTIVE_B；分配失败返 -ENOMEM 且保持未激活。
 * 注意事项：dq_lock 串行同一 dquot，dqio_sem 写锁保护树，dq_dqb_lock 保护限制字段；可睡眠。
 */
static int shmem_acquire_dquot(struct dquot *dquot)
{
	/* parent/n 描述插入位置；new_node 仅在缺失 id 时指向新对象内嵌节点。 */
	struct mem_dqinfo *info = sb_dqinfo(dquot->dq_sb, dquot->dq_id.type);
	struct rb_node **n;
	struct shmem_sb_info *sbinfo = dquot->dq_sb->s_fs_info;
	struct rb_node *parent = NULL, *new_node = NULL;
	struct quota_id *new_entry, *entry;
	qid_t id = from_kqid(&init_user_ns, dquot->dq_id);
	struct quota_info *dqopt = sb_dqopt(dquot->dq_sb);
	int ret = 0;

	/* 阶段 1：先稳定 dquot 状态，再独占格式树；锁序与 release 路径一致。 */
	mutex_lock(&dquot->dq_lock);

	down_write(&dqopt->dqio_sem);
	n = &((struct rb_root *)info->dqi_priv)->rb_node;

	/* 按 init_user_ns id 搜索；退出时 n 正是 rb_link_node() 所需的空链接地址。 */
	while (*n) {
		parent = *n;
		entry = rb_entry(parent, struct quota_id, node);

		if (id < entry->id)
			n = &(*n)->rb_left;
		else if (id > entry->id)
			n = &(*n)->rb_right;
		else
			goto found;
	}

	/* We don't have entry for this id yet, create it */
	/* 树中尚无此 id：分配永久条目，失败时两把睡眠锁由统一出口释放。 */
	new_entry = kzalloc_obj(struct quota_id, GFP_NOFS);
	if (!new_entry) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	/* 新条目先继承挂载参数中的用户/组默认硬限制，软限制保持零。 */
	new_entry->id = id;
	if (dquot->dq_id.type == USRQUOTA) {
		new_entry->bhardlimit = sbinfo->qlimits.usrquota_bhardlimit;
		new_entry->ihardlimit = sbinfo->qlimits.usrquota_ihardlimit;
	} else if (dquot->dq_id.type == GRPQUOTA) {
		new_entry->bhardlimit = sbinfo->qlimits.grpquota_bhardlimit;
		new_entry->ihardlimit = sbinfo->qlimits.grpquota_ihardlimit;
	}

	/* 链接与再平衡都发生在 dqio_sem 写锁下，之后读侧即可按有序键发现它。 */
	new_node = &new_entry->node;
	rb_link_node(new_node, parent, n);
	rb_insert_color(new_node, (struct rb_root *)info->dqi_priv);
	entry = new_entry;

found:
	/* Load the stored limits from the tree */
	/* 阶段 2：在 dqb 自旋锁内把持久限制复制进活跃 dquot 工作副本。 */
	spin_lock(&dquot->dq_dqb_lock);
	dquot->dq_dqb.dqb_bhardlimit = entry->bhardlimit;
	dquot->dq_dqb.dqb_bsoftlimit = entry->bsoftlimit;
	dquot->dq_dqb.dqb_ihardlimit = entry->ihardlimit;
	dquot->dq_dqb.dqb_isoftlimit = entry->isoftlimit;

	/* 四种限制全为零时用 DQ_FAKE 标记“只计用量、没有限制”的快速路径。 */
	if (!dquot->dq_dqb.dqb_bhardlimit &&
	    !dquot->dq_dqb.dqb_bsoftlimit &&
	    !dquot->dq_dqb.dqb_ihardlimit &&
	    !dquot->dq_dqb.dqb_isoftlimit)
		set_bit(DQ_FAKE_B, &dquot->dq_flags);
	spin_unlock(&dquot->dq_dqb_lock);

	/* Make sure flags update is visible after dquot has been filled */
	/*
	 * 发布屏障保证限制字段先于 DQ_ACTIVE_B 对 dqget() 的读取者可见；它与
	 * fs/quota/dquot.c 在观察 active 后执行的 smp_rmb() 配对。
	 */
	smp_mb__before_atomic();
	set_bit(DQ_ACTIVE_B, &dquot->dq_flags);
out_unlock:
	/* 失败尚未置 active；成功则树和 dquot 已同时成为一致、可查询状态。 */
	up_write(&dqopt->dqio_sem);
	mutex_unlock(&dquot->dq_lock);
	return ret;
}

/*
 * 业务背景：release 用本 helper 判断树条目是否可删除；判据是 fake，或零用量且硬限制等于挂载默认值。
 * 入参：dquot 为已锁定且借用的活跃对象；其类型应为 USRQUOTA 或 GRPQUOTA。
 * 出参/返回：满足 fake，或用量为零且硬限制等于挂载默认值时返 true，否则 false；不检查软限制字段。
 * 注意事项：调用者持有 dq_lock/dqio_sem 写锁；字段快照受外层释放协议稳定，不取得引用。
 */
static bool shmem_is_empty_dquot(struct dquot *dquot)
{
	/* bhardlimit/ihardlimit 是当前 quota 类型对应的挂载默认值。 */
	struct shmem_sb_info *sbinfo = dquot->dq_sb->s_fs_info;
	qsize_t bhardlimit;
	qsize_t ihardlimit;

	/* 阶段 1：选择用户或组默认值，作为“没有个性化配置”的判据。 */
	if (dquot->dq_id.type == USRQUOTA) {
		bhardlimit = sbinfo->qlimits.usrquota_bhardlimit;
		ihardlimit = sbinfo->qlimits.usrquota_ihardlimit;
	} else if (dquot->dq_id.type == GRPQUOTA) {
		bhardlimit = sbinfo->qlimits.grpquota_bhardlimit;
		ihardlimit = sbinfo->qlimits.grpquota_ihardlimit;
	}

	/* 阶段 2：fake 可直接丢弃；否则还必须没有当前用量且只保留默认硬限制。 */
	if (test_bit(DQ_FAKE_B, &dquot->dq_flags) ||
		(dquot->dq_dqb.dqb_curspace == 0 &&
		 dquot->dq_dqb.dqb_curinodes == 0 &&
		 dquot->dq_dqb.dqb_bhardlimit == bhardlimit &&
		 dquot->dq_dqb.dqb_ihardlimit == ihardlimit))
		return true;

	return false;
}
/*
 * Store limits from dquot in the tree unless it's fake. If it is fake
 * remove the id from the tree since there is no useful information in
 * there.
 */
/*
 * 修正说明：上述“fake 才删除”比当前实现更窄。shmem_is_empty_dquot() 还会在用量为零、
 * 硬限制等于挂载默认值时删除条目，而且不检查软限制；其余情况才把四种限制写回树。
 */
/*
 * 业务背景：最后一个 dquot 引用释放后，通用回收线程调用这里把工作副本折叠回格式真值。
 * 入参：dquot 为通用层持有且尝试释放的输入输出对象，本函数只借用、不销毁它。
 * 出参/返回：正常或发现新使用者时返 0；违反“激活 dquot 必有树条目”不变量时返 -ENOENT。
 * 注意事项：dq_lock 防止 acquire/release 交错；dqio_sem 写锁保护树；busy 时保留 active 状态。
 */
static int shmem_release_dquot(struct dquot *dquot)
{
	/* id 是搜索键；entry 只在 dqio_sem 写锁持有期间有效。 */
	struct mem_dqinfo *info = sb_dqinfo(dquot->dq_sb, dquot->dq_id.type);
	struct rb_node *node;
	qid_t id = from_kqid(&init_user_ns, dquot->dq_id);
	struct quota_info *dqopt = sb_dqopt(dquot->dq_sb);
	struct quota_id *entry = NULL;

	/* 阶段 1：锁住对象后重查引用，解决最后引用判断与并发 dqget() 的竞态。 */
	mutex_lock(&dquot->dq_lock);
	/* Check whether we are not racing with some other dqget() */
	/* 若另一个 dqget 已重新持有它，就取消释放，让活跃缓存继续服务。 */
	if (dquot_is_busy(dquot))
		goto out_dqlock;

	/* 阶段 2：独占树并按 id 找回 acquire 时创建或复用的永久条目。 */
	down_write(&dqopt->dqio_sem);
	node = ((struct rb_root *)info->dqi_priv)->rb_node;
	while (node) {
		entry = rb_entry(node, struct quota_id, node);

		/* release 必须精确命中 acquire 发布的同一 id，不能选择相邻节点。 */
		if (id < entry->id)
			node = node->rb_left;
		else if (id > entry->id)
			node = node->rb_right;
		else
			goto found;
	}

	/* We should always find the entry in the rb tree */
	/* 缺失说明树与 DQ_ACTIVE 状态已失配；告警并保持错误可见，不能凭空重建限制。 */
	WARN_ONCE(1, "quota id %u from dquot %p, not in rb tree!\n", id, dquot);
	up_write(&dqopt->dqio_sem);
	mutex_unlock(&dquot->dq_lock);
	return -ENOENT;

found:
	/* 阶段 3：根据是否仍有信息决定摘除条目或提交最新四种限制。 */
	if (shmem_is_empty_dquot(dquot)) {
		/* Remove entry from the tree */
		/* 无用量、无个性化策略，删除后未来 dqget 会按挂载默认值重新创建。 */
		rb_erase(&entry->node, info->dqi_priv);
		kfree(entry);
	} else {
		/* Store the limits in the tree */
		/* dqb 锁给出一致限制快照；用量由通用 dquot 账本处理，不存入本树。 */
		spin_lock(&dquot->dq_dqb_lock);
		entry->bhardlimit = dquot->dq_dqb.dqb_bhardlimit;
		entry->bsoftlimit = dquot->dq_dqb.dqb_bsoftlimit;
		entry->ihardlimit = dquot->dq_dqb.dqb_ihardlimit;
		entry->isoftlimit = dquot->dq_dqb.dqb_isoftlimit;
		spin_unlock(&dquot->dq_dqb_lock);
	}

	/* 最后撤销 active 发布；此后通用层才能把 dquot 移入可回收状态。 */
	clear_bit(DQ_ACTIVE_B, &dquot->dq_flags);
	up_write(&dqopt->dqio_sem);

out_dqlock:
	/* busy 快速路径未取得 dqio_sem，也不会清 active；这里只释放共同的对象锁。 */
	mutex_unlock(&dquot->dq_lock);
	return 0;
}

/*
 * 业务背景：通用层在用量/限制变化时要求标脏；tmpfs dquot 没有异步磁盘写回队列。
 * 入参：dquot 为发生变化的借用对象，本回调不读取或持有它。
 * 出参/返回：恒返 0，无直接副作用；最终限制由 release_dquot 同步保存到树。
 * 注意事项：不睡眠；空实现依赖 DQUOT_NOLIST_DIRTY 和内存格式生命周期，不能用于磁盘格式。
 */
static int shmem_mark_dquot_dirty(struct dquot *dquot)
{
	return 0;
}

/*
 * 业务背景：quota 核心可能请求写回全局 quota 信息；tmpfs 的 info 本来就在超级块内存中。
 * 入参：sb 为借用超级块，type 为 quota 类型索引，仅满足统一接口。
 * 出参/返回：恒返 0，无输出、无所有权变化，也不产生 I/O。
 * 注意事项：不睡眠；返回成功表示“不需要写”，不提供崩溃持久性。
 */
static int shmem_dquot_write_info(struct super_block *sb, int type)
{
	return 0;
}

/*
 * 格式操作表由 register_quota_format() 发布：启停流程通过它创建/销毁红黑树，
 * 两个 write/check 桩把“无磁盘 quota 文件”适配成通用 quota 格式协议。
 */
static const struct quota_format_ops shmem_format_ops = {
	.check_quota_file	= shmem_check_quota_file,
	.read_file_info		= shmem_read_file_info,
	.write_file_info	= shmem_write_file_info,
	.free_file_info		= shmem_free_file_info,
};

/* QFMT_SHMEM 将格式 id、操作表和模块生命周期绑定，供 shmem_enable_quotas() 查找。 */
struct quota_format_type shmem_quota_format = {
	.qf_fmt_id = QFMT_SHMEM,
	.qf_ops = &shmem_format_ops,
	.qf_owner = THIS_MODULE
};

/*
 * tmpfs 超级块把 dq_op 指向此表：通用层仍负责 dquot 分配/销毁与用量核算，
 * 本文件只接管激活、释放、枚举和无需磁盘写回的适配边界。
 */
const struct dquot_operations shmem_quota_operations = {
	.acquire_dquot		= shmem_acquire_dquot,
	.release_dquot		= shmem_release_dquot,
	.alloc_dquot		= dquot_alloc,
	.destroy_dquot		= dquot_destroy,
	.write_info		= shmem_dquot_write_info,
	.mark_dirty		= shmem_mark_dquot_dirty,
	.get_next_id		= shmem_get_next_id,
};
