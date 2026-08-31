// SPDX-License-Identifier: GPL-2.0
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/shrinker.h>
#include <linux/memcontrol.h>

#include "internal.h"

/* defined in vmscan.c */
/* 下列全局量定义于 vmscan.c；debugfs 控制面复用同一注册锁和 shrinker 链表。 */
extern struct mutex shrinker_mutex;
extern struct list_head shrinker_list;

/* 为每个 debugfs 目录分配进程期唯一后缀；detach 后由 remove 路径归还编号。 */
static DEFINE_IDA(shrinker_debugfs_ida);
/* late_initcall 发布的 /sys/kernel/debug/shrinker 根目录；此前注册只记入链表。 */
static struct dentry *shrinker_debugfs_root;

/*
 * shrinker_count_objects() - 汇总一个 shrinker 在指定 memcg/各 NUMA 节点的对象数。
 * @shrinker 为注册期内借用对象，@memcg 为借用组或 NULL，@count_per_node 是至少
 * nr_node_ids 项的调用者缓冲区。返回各节点计数之和，并填满数组；SHRINK_EMPTY
 * 规范化为 0。回调可能睡眠，调用者不得持自旋锁；本函数不取得 memcg ownership。
 */
static unsigned long shrinker_count_objects(struct shrinker *shrinker,
					    struct mem_cgroup *memcg,
					    unsigned long *count_per_node)
{
	/* nr 保存当前节点回调结果，total 是可能溢出 unsigned long 的诊断性汇总。 */
	unsigned long nr, total = 0;
	int nid;

	for_each_node(nid) {
		/* 非 NUMA-aware shrinker 只在 nid 0 调一次，其他列显式输出 0。 */
		if (nid == 0 || (shrinker->flags & SHRINKER_NUMA_AWARE)) {
			/* shrink_control 只在本次 count_objects 回调期间借用。 */
			struct shrink_control sc = {
				.gfp_mask = GFP_KERNEL,
				.nid = nid,
				.memcg = memcg,
			};

			nr = shrinker->count_objects(shrinker, &sc);
			/* SHRINK_EMPTY 是回收协议哨兵，不应作为巨大无符号数暴露给 debugfs。 */
			if (nr == SHRINK_EMPTY)
				nr = 0;
		} else {
			nr = 0;
		}

		count_per_node[nid] = nr;
		/* 输出数组保留逐节点视图，total 决定该 memcg 行是否值得打印。 */
		total += nr;
	}

	return total;
}

/*
 * shrinker_debugfs_count_show() - 生成 count 文件的“memcg-id 每节点计数”快照。
 * @m 的 private 指向该 dentry 绑定的借用 shrinker，@v 未使用。成功返回 0，
 * 分配失败返回 -ENOMEM，收到信号时归还迭代引用并返回 -EINTR。
 * MEMCG-aware 时遍历在线组；否则只用 NULL 组统计一次。输出是逐回调弱一致快照，
 * 期间可睡眠且不持 shrinker_mutex，不能据此推断后续 scan 一定释放相同数量。
 */
static int shrinker_debugfs_count_show(struct seq_file *m, void *v)
{
	/* 节点计数缓冲仅属于本次 seq_file show，所有退出都在函数尾释放。 */
	struct shrinker *shrinker = m->private;
	unsigned long *count_per_node;
	struct mem_cgroup *memcg;
	unsigned long total;
	bool memcg_aware;
	int ret = 0, nid;

	count_per_node = kcalloc(nr_node_ids, sizeof(unsigned long), GFP_KERNEL);
	if (!count_per_node)
		return -ENOMEM;

	memcg_aware = shrinker->flags & SHRINKER_MEMCG_AWARE;

	/* 迭代器把前一个 css 引用交接给下次调用；提前退出必须显式 break。 */
	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		/* 离线组可能仍可被迭代取得，但不再形成用户可操作的统计行。 */
		if (memcg && !mem_cgroup_online(memcg))
			continue;

		total = shrinker_count_objects(shrinker,
					       memcg_aware ? memcg : NULL,
					       count_per_node);
		if (total) {
			/* 全局/null 语境的 ID 为 0，随后固定输出 nr_node_ids 个计数列。 */
			seq_printf(m, "%llu", mem_cgroup_id(memcg));
			for_each_node(nid)
				seq_printf(m, " %lu", count_per_node[nid]);
			seq_putc(m, '\n');
		}

		if (!memcg_aware) {
			/* 非 memcg shrinker 只调用一次，避免为每个组重复同一全局计数。 */
			mem_cgroup_iter_break(NULL, memcg);
			break;
		}

		if (signal_pending(current)) {
			/* 大层级允许被信号中断，同时归还当前迭代引用。 */
			mem_cgroup_iter_break(NULL, memcg);
			ret = -EINTR;
			break;
		}
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);

	kfree(count_per_node);
	return ret;
}

/* 生成只读 count 文件所需的 open/read/llseek/release fops。 */
DEFINE_SHOW_ATTRIBUTE(shrinker_debugfs_count);

/*
 * shrinker_debugfs_scan_open() - 把 inode 绑定的 shrinker 交给一次写文件会话。
 * @inode/@file 均由 VFS 借用；成功返回 nonseekable_open() 结果。
 * 不取得 shrinker 引用，生命周期由 debugfs dentry 与注销顺序约束；文件不可 seek。
 */
static int shrinker_debugfs_scan_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return nonseekable_open(inode, file);
}

/*
 * shrinker_debugfs_scan_write() - 按用户给出的“memcg-id nid 数量”主动扫描缓存。
 * @file 绑定借用 shrinker，@buf 是用户缓冲，@size 为写入长度，@pos 未使用。
 * 成功即返回原始 @size（nr_to_scan=0 也是 no-op 成功）；复制/格式/节点错误分别
 * 返回 -EFAULT/-EINVAL，组不存在或已离线返回 -ENOENT。成功取得的 memcg 引用
 * 在 scan_objects 返回后释放；回调可睡眠，返回的释放数量只供回调内部处理。
 */
static ssize_t shrinker_debugfs_scan_write(struct file *file,
					   const char __user *buf,
					   size_t size, loff_t *pos)
{
	/* kbuf 有固定上界且手工补 NUL；超长输入只解析前 71 字节。 */
	struct shrinker *shrinker = file->private_data;
	unsigned long nr_to_scan = 0, read_len;
	u64 id;
	struct shrink_control sc = {
		.gfp_mask = GFP_KERNEL,
	};
	struct mem_cgroup *memcg = NULL;
	int nid;
	char kbuf[72];

	read_len = min(size, sizeof(kbuf) - 1);
	/* copy_from_user 失败时尚未取得 memcg 引用，可直接返回。 */
	if (copy_from_user(kbuf, buf, read_len))
		return -EFAULT;
	kbuf[read_len] = '\0';

	if (sscanf(kbuf, "%llu %d %lu", &id, &nid, &nr_to_scan) != 3)
		return -EINVAL;

	if (nid < 0 || nid >= nr_node_ids)
		return -EINVAL;

	if (nr_to_scan == 0)
		/* 零扫描量不验证 memcg ID，作为明确的无副作用成功请求。 */
		return size;

	if (shrinker->flags & SHRINKER_MEMCG_AWARE) {
		/* ID 查询成功会取得引用，使 memcg 至少稳定到 scan 回调结束。 */
		memcg = mem_cgroup_get_from_id(id);
		if (!memcg)
			return -ENOENT;

		if (!mem_cgroup_online(memcg)) {
			/* 已离线对象即使仍有引用也不能作为新的扫描目标。 */
			mem_cgroup_put(memcg);
			return -ENOENT;
		}
	} else if (id != 0) {
		/* 非 memcg-aware shrinker 只接受代表全局语境的 ID 0。 */
		return -EINVAL;
	}

	/* nr_scanned 以请求量为默认值，回调可改写成实际检查数量。 */
	sc.nid = nid;
	sc.memcg = memcg;
	sc.nr_to_scan = nr_to_scan;
	sc.nr_scanned = nr_to_scan;

	shrinker->scan_objects(shrinker, &sc);

	/* mem_cgroup_put(NULL) 是关闭 MEMCG/全局语境下的中性操作。 */
	mem_cgroup_put(memcg);

	return size;
}

/* scan 文件仅允许打开与写入，不提供 read/seek；module 字段固定实现代码生命周期。 */
static const struct file_operations shrinker_debugfs_scan_fops = {
	.owner	 = THIS_MODULE,
	.open	 = shrinker_debugfs_scan_open,
	.write	 = shrinker_debugfs_scan_write,
};

/*
 * shrinker_debugfs_add() - 为已注册 shrinker 创建带唯一 ID 的 debugfs 目录。
 * @shrinker 由注册路径借用，调用者必须持 shrinker_mutex。成功返回 0；根目录尚未
 * 初始化也返回 0 并留给 late init 补建；IDA 或目录创建失败返回负 errno。
 * 成功后 debugfs_id/entry 发布到 shrinker，子文件按回调能力创建；目录失败会归还
 * ID。debugfs 子文件创建失败不回滚目录，这是诊断接口的尽力语义。
 */
int shrinker_debugfs_add(struct shrinker *shrinker)
{
	struct dentry *entry;
	char buf[128];
	int id;

	lockdep_assert_held(&shrinker_mutex);

	/* debugfs isn't initialized yet, add debugfs entries later. */
	/* debugfs 尚未初始化；启动期已注册项稍后由 late_initcall 补建。 */
	if (!shrinker_debugfs_root)
		return 0;

	id = ida_alloc(&shrinker_debugfs_ida, GFP_KERNEL);
	if (id < 0)
		return id;
	shrinker->debugfs_id = id;

	/* 名称可重复，IDA 后缀保证同一挂载中的目录名唯一。 */
	snprintf(buf, sizeof(buf), "%s-%d", shrinker->name, id);

	/* create debugfs entry */
	/* 创建 shrinker 专属 debugfs 目录。 */
	entry = debugfs_create_dir(buf, shrinker_debugfs_root);
	if (IS_ERR(entry)) {
		ida_free(&shrinker_debugfs_ida, id);
		return PTR_ERR(entry);
	}
	shrinker->debugfs_entry = entry;

	/* 只暴露实现了相应回调的文件，权限分别是 root/group 读与 owner/group 写。 */
	if (shrinker->count_objects)
		debugfs_create_file("count", 0440, entry, shrinker,
				    &shrinker_debugfs_count_fops);
	if (shrinker->scan_objects)
		debugfs_create_file("scan", 0220, entry, shrinker,
				    &shrinker_debugfs_scan_fops);
	return 0;
}

/*
 * shrinker_debugfs_rename() - 原子地更换 shrinker 名称并同步 debugfs 目录名。
 * @shrinker 为调用者持有的已分配对象，@fmt/... 生成由 shrinker 接管的新常量字符串。
 * 成功返回 0 并释放旧名；分配失败返回 -ENOMEM，重命名失败恢复旧名并释放新名。
 * 函数内部持 shrinker_mutex，可睡眠；调用者不得已持该锁，也不得继续借用旧 name。
 */
int shrinker_debugfs_rename(struct shrinker *shrinker, const char *fmt, ...)
{
	const char *new, *old;
	va_list ap;
	int ret = 0;

	/* 在取全局锁前完成可能睡眠的格式化分配，失败不触碰当前名称或 dentry。 */
	va_start(ap, fmt);
	new = kvasprintf_const(GFP_KERNEL, fmt, ap);
	va_end(ap);

	if (!new)
		return -ENOMEM;

	mutex_lock(&shrinker_mutex);

	/* 先临时发布新名供 debugfs_change_name 格式化，失败路径再回滚指针。 */
	old = shrinker->name;
	shrinker->name = new;

	ret = debugfs_change_name(shrinker->debugfs_entry, "%s-%d",
			shrinker->name, shrinker->debugfs_id);

	if (ret) {
		/* dentry 未改名：恢复旧 ownership，并销毁尚未发布的新字符串。 */
		shrinker->name = old;
		kfree_const(new);
	} else {
		/* dentry 与 shrinker 均已引用新名，旧字符串可在锁内释放。 */
		kfree_const(old);
	}
	mutex_unlock(&shrinker_mutex);

	return ret;
}
EXPORT_SYMBOL(shrinker_debugfs_rename);

/*
 * shrinker_debugfs_detach() - 在注销锁内解除 shrinker 对 debugfs dentry 的发布。
 * @shrinker 为借用对象，@debugfs_id 为必需出参；调用者必须持 shrinker_mutex。
 * 返回旧 dentry 借用指针供锁外递归删除；无 entry 时写 -1 并返回 NULL。
 * 此函数不删除 dentry、不归还 ID，避免在全局注册锁内执行较重 debugfs 清理。
 */
struct dentry *shrinker_debugfs_detach(struct shrinker *shrinker,
				       int *debugfs_id)
{
	struct dentry *entry = shrinker->debugfs_entry;

	lockdep_assert_held(&shrinker_mutex);

	*debugfs_id = entry ? shrinker->debugfs_id : -1;
	/* NULL 是对并发注册/重命名路径的逻辑撤销发布点。 */
	shrinker->debugfs_entry = NULL;

	return entry;
}

/*
 * shrinker_debugfs_remove() - 在 shrinker_mutex 外删除目录并归还唯一编号。
 * @debugfs_entry/@debugfs_id 必须来自成功的 detach；无返回值，可睡眠。
 * 删除递归完成后才释放 ID，避免新目录在旧名字仍可见时复用同一后缀。
 */
void shrinker_debugfs_remove(struct dentry *debugfs_entry, int debugfs_id)
{
	debugfs_remove_recursive(debugfs_entry);
	ida_free(&shrinker_debugfs_ida, debugfs_id);
}

/*
 * shrinker_debugfs_init() - late init 创建根目录并补建启动期 shrinker 条目。
 * 无参数；成功返回 0，根目录或首个子项失败返回负 errno。根目录先发布，再持
 * shrinker_mutex 遍历注册链；已存在 entry 的项跳过。部分失败不回滚此前目录，
 * debugfs 是尽力诊断面，且本函数只在启动 late_initcall 阶段执行一次。
 */
static int __init shrinker_debugfs_init(void)
{
	struct shrinker *shrinker;
	struct dentry *dentry;
	int ret = 0;

	dentry = debugfs_create_dir("shrinker", NULL);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	shrinker_debugfs_root = dentry;

	/* Create debugfs entries for shrinkers registered at boot */
	/* 为 debugfs 就绪前已经注册到全局链表的 shrinker 补建目录。 */
	mutex_lock(&shrinker_mutex);
	list_for_each_entry(shrinker, &shrinker_list, list)
		if (!shrinker->debugfs_entry) {
			ret = shrinker_debugfs_add(shrinker);
			if (ret)
				break;
		}
	mutex_unlock(&shrinker_mutex);

	return ret;
}

/* 在大多数 shrinker 已注册后建立 debugfs 视图，同时仍允许后续动态 add。 */
late_initcall(shrinker_debugfs_init);
