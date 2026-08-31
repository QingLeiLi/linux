// SPDX-License-Identifier: GPL-2.0
/*
 * Common Code for DAMON Sysfs Interface
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#include <linux/slab.h>

#include "sysfs-common.h"

/*
 * DAMON sysfs 控制面的共享互斥量。目录增删、配置提交和回调快照等跨对象操作由
 * 上层显式获取它；本文件的简单 min/max show/store 本身不隐式加锁。
 */
DEFINE_MUTEX(damon_sysfs_lock);

/*
 * unsigned long range directory
 */
/* 以下实现一个含 min/max 两个属性文件、端点类型为 unsigned long 的共享目录。 */

/* 分配尚未发布的范围容器；完整 ownership 和失败契约见同名头文件声明。 */
struct damon_sysfs_ul_range *damon_sysfs_ul_range_alloc(
		unsigned long min,
		unsigned long max)
{
	/* kmalloc_obj 分配容器；嵌入的 kobject 还没有初始化或加入 sysfs。 */
	struct damon_sysfs_ul_range *range = kmalloc_obj(*range);

	if (!range)
		return NULL;
	/* 清零是 kobject_init_and_add() 的前置状态，不等于持有一个 kobject 引用。 */
	range->kobj = (struct kobject){};
	range->min = min;
	range->max = max;

	return range;
}

/* min_show() 读取当前下界并以十进制加换行的 sysfs 文本形式返回。 */
static ssize_t min_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	/* sysfs core 传入嵌入成员，container_of 恢复其所属范围对象。 */
	struct damon_sysfs_ul_range *range = container_of(kobj,
			struct damon_sysfs_ul_range, kobj);

	/* sysfs_emit 遵守 PAGE_SIZE 输出边界；返回实际写入的字节数。 */
	return sysfs_emit(buf, "%lu\n", range->min);
}

/*
 * min_store() - 更新范围目录的下界属性。
 *
 * buf/count 由 sysfs core 借用且 buf 含用户文本；成功解析一个基数自动识别的
 * unsigned long 后直接替换 min，并返回 count 表示整次写入已消费。解析失败返回
 * 原 errno 且保持旧值。这里不检查 min <= max：端点组合的业务约束由使用该范围
 * 的提交路径处理，也不在内部获取 damon_sysfs_lock。
 */
static ssize_t min_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	struct damon_sysfs_ul_range *range = container_of(kobj,
			struct damon_sysfs_ul_range, kobj);
	unsigned long min;
	int err;

	/* 先解析到局部量，保证错误输入不会留下部分更新。 */
	err = kstrtoul(buf, 0, &min);
	if (err)
		return err;

	range->min = min;
	return count;
}

/* max_show() 读取当前上界；对象和输出缓冲均只在本次 sysfs 回调期间借用。 */
static ssize_t max_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	/* max 与 min 共用同一容器生命周期，只读取另一个端点。 */
	struct damon_sysfs_ul_range *range = container_of(kobj,
			struct damon_sysfs_ul_range, kobj);

	return sysfs_emit(buf, "%lu\n", range->max);
}

/*
 * max_store() - 更新范围目录的上界属性。
 *
 * 返回/错误、原子式解析提交及锁边界与 min_store() 对称；成功返回 count，解析
 * 失败保留原 max 并返回负 errno，不校验 max 是否大于等于 min。
 */
static ssize_t max_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	/* 与 min_store 对称：解析成功才提交，端点顺序留给更高层验证。 */
	struct damon_sysfs_ul_range *range = container_of(kobj,
			struct damon_sysfs_ul_range, kobj);
	unsigned long max;
	int err;

	/* 使用局部 max 承接转换结果，只有完整转换成功后才写回共享对象。 */
	err = kstrtoul(buf, 0, &max);
	if (err)
		return err;

	range->max = max;
	return count;
}

/* 范围对象的最终 kobject release；详细生命周期契约见头文件声明。 */
void damon_sysfs_ul_range_release(struct kobject *kobj)
{
	/* release 仅在最后一个 kobject 引用归还后调用，随后整个容器失效。 */
	kfree(container_of(kobj, struct damon_sysfs_ul_range, kobj));
}

/* 0600 让 root 通过通用 kobj_sysfs_ops 读写两个端点。 */
static struct kobj_attribute damon_sysfs_ul_range_min_attr =
		__ATTR_RW_MODE(min, 0600);

static struct kobj_attribute damon_sysfs_ul_range_max_attr =
		__ATTR_RW_MODE(max, 0600);

static struct attribute *damon_sysfs_ul_range_attrs[] = {
	/* NULL 终止的属性表由 ATTRIBUTE_GROUPS 转换为默认 group 数组。 */
	&damon_sysfs_ul_range_min_attr.attr,
	&damon_sysfs_ul_range_max_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(damon_sysfs_ul_range);

const struct kobj_type damon_sysfs_ul_range_ktype = {
	/* ktype 将属性访问、默认文件创建和容器终结绑定到每个 range kobject。 */
	.release = damon_sysfs_ul_range_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = damon_sysfs_ul_range_groups,
};


static bool damon_sysfs_memcg_path_eq(struct mem_cgroup *memcg,
		char *memcg_path_buf, char *path)
{
#ifdef CONFIG_MEMCG
	/*
	 * 把候选 cgroup 的当前层级路径写入调用者提供的 PATH_MAX 缓冲区；
	 * sysfs_streq 接受 sysfs 输入常见的尾随换行，而普通 strcmp 不接受。
	 */
	cgroup_path(memcg->css.cgroup, memcg_path_buf, PATH_MAX);
	if (sysfs_streq(memcg_path_buf, path))
		return true;
#endif /* CONFIG_MEMCG */
	/* MEMCG 未编译或路径不相等都归一为未命中。 */
	return false;
}

/*
 * damon_sysfs_memcg_path_to_id() - 将 memcg 层级路径解析为在线 memcg 的数值 ID。
 *
 * memcg_path 是只读借用的 NUL 结尾输入；id 是仅在成功时写入的输出。函数分配一个
 * PATH_MAX 临时缓冲，遍历根层级并跳过离线节点；命中时在提前退出前用
 * mem_cgroup_iter_break() 归还迭代器持有的引用。成功返回 0，空输入或未命中返回
 * -EINVAL，临时分配失败返回 -ENOMEM；函数不保留 memcg 引用，ID 供 DAMOS 过滤器
 * 后续匹配使用。CONFIG_MEMCG=n 时比较恒假，非空路径最终返回 -EINVAL。
 */
int damon_sysfs_memcg_path_to_id(char *memcg_path, u64 *id)
{
	struct mem_cgroup *memcg;
	char *path;
	bool found = false;

	if (!memcg_path)
		return -EINVAL;

	/* 可睡眠上下文中申请一次可复用缓冲，避免在每个迭代节点重复分配。 */
	path = kmalloc_array(PATH_MAX, sizeof(*path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	/* prev 参数让迭代器在前进时交接 css 引用，正常耗尽会自行释放最后引用。 */
	for (memcg = mem_cgroup_iter(NULL, NULL, NULL); memcg;
			memcg = mem_cgroup_iter(NULL, memcg, NULL)) {
		/* skip offlined memcg */
		/* 离线节点不再是可提交给新 DAMOS 配置的有效目标。 */
		if (!mem_cgroup_online(memcg))
			continue;
		if (damon_sysfs_memcg_path_eq(memcg, path, memcg_path)) {
			/* 在引用仍有效时取得稳定 ID，然后显式中止迭代并归还该引用。 */
			*id = mem_cgroup_id(memcg);
			found = true;
			mem_cgroup_iter_break(NULL, memcg);
			break;
		}
	}

	/* 所有出口在此释放临时路径；未命中时绝不改写调用者的 *id。 */
	kfree(path);
	return found ? 0 : -EINVAL;
}
