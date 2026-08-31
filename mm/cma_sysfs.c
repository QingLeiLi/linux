// SPDX-License-Identifier: GPL-2.0
/*
 * CMA SysFS Interface
 *
 * Copyright (c) 2021 Minchan Kim <minchan@kernel.org>
 */
/*
 * 本文件在 /sys/kernel/mm/cma/ 下为每个已激活 CMA area 发布只读容量与累计
 * 事件计数；分配热路径只做原子累加，sysfs 对象的创建和失败回滚集中在启动期。
 */

#include <linux/cma.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "cma.h"

/*
 * 宏把 _name 展开为静态只读 kobj_attribute，并按约定绑定 _name_show()；生成的
 * 五个属性随后由 cma_attrs 组成默认属性组，不存在 store 写入口。
 */
#define CMA_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

/*
 * cma_sysfs_account_success_pages() - 累计一次成功 CMA 分配的页数。
 * 业务背景：__cma_alloc_frozen() 在页已成功取得后调用，供 sysfs 展示历史吞吐。
 * 入参：@cma 是已激活 area 的借用指针，不可为 NULL；@nr_pages 是本次成功页数。
 * 出参/返回：无直接返回值；原子增加 nr_pages_succeeded，不改变 bitmap 或页 ownership。
 * 注意事项：可被并发分配路径调用且不睡眠；必须每个外部请求只计一次。
 */
void cma_sysfs_account_success_pages(struct cma *cma, unsigned long nr_pages)
{
	atomic64_add(nr_pages, &cma->nr_pages_succeeded);
}

/*
 * cma_sysfs_account_fail_pages() - 累计一次失败 CMA 请求涉及的页数。
 * 业务背景：__cma_alloc_frozen() 穷尽范围而未返回 page 时调用，观测 CMA 压力。
 * 入参：@cma 为已激活 area 借用指针；@nr_pages 为失败请求规模，单位 page。
 * 出参/返回：无直接返回值；只原子增加 nr_pages_failed，原失败语义保持不变。
 * 注意事项：内部候选重试不应重复调用；统计不代表当前不可用页数。
 */
void cma_sysfs_account_fail_pages(struct cma *cma, unsigned long nr_pages)
{
	atomic64_add(nr_pages, &cma->nr_pages_failed);
}

/*
 * cma_sysfs_account_release_pages() - 累计已真实归还的 CMA 页数。
 * 业务背景：__cma_release_frozen() 在 free_contig_frozen_range() 与 bitmap 清除后调用。
 * 入参：@cma 为 area 借用指针；@nr_pages 为本次成功释放页数，单位 page。
 * 出参/返回：无直接返回值；原子增加 nr_pages_released，不负责实际释放页面。
 * 注意事项：不得在释放校验或 bitmap 清理前调用，否则 sysfs 会报告虚假成功。
 */
void cma_sysfs_account_release_pages(struct cma *cma, unsigned long nr_pages)
{
	atomic64_add(nr_pages, &cma->nr_pages_released);
}

/*
 * cma_from_kobj() - 从嵌入 kobject 反向取得对应的 CMA area。
 * 业务背景：show/release 回调只收到 sysfs core 传入的 kobject，需要恢复包装关系。
 * 入参：@kobj 必须是 struct cma_kobject 内嵌成员的借用指针，不可为 NULL。
 * 出参/返回：返回全局 cma_areas 中的借用指针，不增加 kobject 或 CMA 引用。
 * 注意事项：container_of 依赖准确对象类型；传入其他 ktype 的 kobject 会产生越界解释。
 */
static inline struct cma *cma_from_kobj(struct kobject *kobj)
{
	return container_of(kobj, struct cma_kobject, kobj)->cma;
}

/*
 * alloc_pages_success_show() - 输出该 area 累计成功分配页数。
 * 业务背景：读取 alloc_pages_success 文件时由 kobj_sysfs_ops 调用。
 * 入参：@kobj 指向有效 cma_kobject；@attr 是本只读属性的借用描述符但无需读取；
 * @buf 是 sysfs 提供的一页输出缓冲区。
 * 出参/返回：返回写入字节数，文本为十进制页数加换行；不转移任何 ownership。
 * 注意事项：atomic64_read 与并发计账无数据竞争，但它只是读取时刻的累计快照。
 */
static ssize_t alloc_pages_success_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	struct cma *cma = cma_from_kobj(kobj);

	return sysfs_emit(buf, "%llu\n",
			  atomic64_read(&cma->nr_pages_succeeded));
}

/* 生成只读 alloc_pages_success 属性并绑定上方 show 回调。 */
CMA_ATTR_RO(alloc_pages_success);

/*
 * alloc_pages_fail_show() - 输出该 area 累计失败请求页数。
 * 业务背景：为用户区分分配吞吐与失败压力，而非报告失败次数。
 * 入参：@kobj 是有效包装的借用指针；@attr 未使用；@buf 为 sysfs 输出缓冲区。
 * 出参/返回：返回十进制计数与换行的字节数，无状态和 ownership 变化。
 * 注意事项：原子读取只保证单个计数一致，不与 success/release 组成事务快照。
 */
static ssize_t alloc_pages_fail_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	struct cma *cma = cma_from_kobj(kobj);

	return sysfs_emit(buf, "%llu\n", atomic64_read(&cma->nr_pages_failed));
}

/* 生成只读 alloc_pages_fail 属性。 */
CMA_ATTR_RO(alloc_pages_fail);

/*
 * release_pages_success_show() - 输出该 area 累计成功释放页数。
 * 业务背景：与成功分配计数对照可观察历史周转，但二者差值不保证等于实时占用。
 * 入参：@kobj 是有效包装借用指针；@attr 未使用；@buf 是 sysfs 输出缓冲区。
 * 出参/返回：返回写入的十进制文本字节数，无副作用或引用变化。
 * 注意事项：读取可与释放并发，返回一个合法但可能随即过时的原子快照。
 */
static ssize_t release_pages_success_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	struct cma *cma = cma_from_kobj(kobj);

	return sysfs_emit(buf, "%llu\n", atomic64_read(&cma->nr_pages_released));
}

/* 生成只读 release_pages_success 属性。 */
CMA_ATTR_RO(release_pages_success);

/*
 * total_pages_show() - 输出 CMA area 启动激活后的总页数。
 * 业务背景：为容量基线提供 count，便于解释 available 与事件累计值。
 * 入参：@kobj 是有效包装借用指针；@attr 未使用；@buf 为 sysfs 输出缓冲区。
 * 出参/返回：返回 count 十进制文本及换行的字节数，无副作用。
 * 注意事项：count 在 area 发布后保持稳定，单位是 page 而非 byte。
 */
static ssize_t total_pages_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	struct cma *cma = cma_from_kobj(kobj);

	return sysfs_emit(buf, "%lu\n", cma->count);
}

/* 生成只读 total_pages 属性。 */
CMA_ATTR_RO(total_pages);

/*
 * available_pages_show() - 输出 CMA bitmap 当前记录的可用页数近似快照。
 * 业务背景：分配/释放在 cma->lock 下维护 available_count，sysfs 用它展示即时容量。
 * 入参：@kobj 是有效包装借用指针；@attr 未使用；@buf 为 sysfs 输出缓冲区。
 * 出参/返回：返回 available_count 十进制文本及换行的字节数，不修改 area。
 * 注意事项：这里不取 cma->lock，读取可能与计数更新并发，仅适合监控而非分配决策。
 */
static ssize_t available_pages_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	struct cma *cma = cma_from_kobj(kobj);

	return sysfs_emit(buf, "%lu\n", cma->available_count);
}

/* 生成只读 available_pages 属性。 */
CMA_ATTR_RO(available_pages);

/*
 * cma_kobj_release() - 在 kobject 最后一个引用消失时释放动态包装。
 * 业务背景：kobject 要求 release 回调收尾承载内存；CMA 描述符本身位于全局数组，
 * 不随 sysfs 节点释放。
 * 入参：@kobj 是引用计数已归零的内嵌对象，函数消费其 cma_kobject 包装。
 * 出参/返回：无直接返回值；释放包装并清空 cma->cma_kobj，CMA area 继续存在。
 * 注意事项：由 sysfs/kobject core 调用；返回后 @kobj 及包装均不可再访问。
 */
static void cma_kobj_release(struct kobject *kobj)
{
	/* cma 是全局稳定借用指针，cma_kobj 则是本回调唯一负责释放的动态对象。 */
	struct cma *cma = cma_from_kobj(kobj);
	struct cma_kobject *cma_kobj = cma->cma_kobj;

	kfree(cma_kobj);
	cma->cma_kobj = NULL;
}

/* 五个属性按 NULL 结尾组成默认组；数组与属性描述符均贯穿内核生命周期。 */
static struct attribute *cma_attrs[] = {
	&alloc_pages_success_attr.attr,
	&alloc_pages_fail_attr.attr,
	&release_pages_success_attr.attr,
	&total_pages_attr.attr,
	&available_pages_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cma);

/* ktype 把通用 sysfs 操作、默认属性组与包装最终释放协议绑定起来。 */
static const struct kobj_type cma_ktype = {
	.release = cma_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = cma_groups,
};

/*
 * cma_sysfs_init() - 为所有已激活 CMA area 创建 sysfs 目录树。
 *
 * 业务背景：subsys_initcall 在 CMA area 完成早期登记/激活后运行，先创建公共
 * /sys/kernel/mm/cma 根，再按 area 名称发布子 kobject 与默认只读属性。
 * 入参：无。
 * 出参/返回：全部发布成功返回 0；根或任一包装分配失败返回 -ENOMEM，kobject
 * 注册错误原样返回。失败时撤销本函数此前建立的全部节点。
 * 注意事项：仅启动期单线程调用、可睡眠；cma_areas 是全局稳定数组。成功后保留
 * 创建所得初始引用作为系统全生命周期引用；子包装仍可由 cma->cma_kobj 定位。
 */
static int __init cma_sysfs_init(void)
{
	/* root 是本轮目录树所有权根；i 指向正在处理的 area，err 保存首个失败码。 */
	struct kobject *cma_kobj_root;
	struct cma_kobject *cma_kobj;
	struct cma *cma;
	int i, err;

	/* 第一阶段在 mm_kobj 下发布根目录；失败前尚无子对象需要回滚。 */
	cma_kobj_root = kobject_create_and_add("cma", mm_kobj);
	if (!cma_kobj_root)
		return -ENOMEM;

	/* 第二阶段只为已激活、bitmap 与容量字段均可用的 area 创建节点。 */
	for (i = 0; i < cma_area_count; i++) {
		cma = &cma_areas[i];
		if (!test_bit(CMA_ACTIVATED, &cma->flags))
			continue;

		/* 包装必须动态分配，最终由 cma_kobj_release() 随 kobject 引用释放。 */
		cma_kobj = kzalloc_obj(*cma_kobj);
		if (!cma_kobj) {
			err = -ENOMEM;
			goto out;
		}

		/*
		 * 先建立双向关系，再初始化并发布 kobject；即使 add 失败，put 也能
		 * 通过 release 找回包装、释放它并清空 cma->cma_kobj。
		 */
		cma->cma_kobj = cma_kobj;
		cma_kobj->cma = cma;
		err = kobject_init_and_add(&cma_kobj->kobj, &cma_ktype,
					   cma_kobj_root, "%s", cma->name);
		if (err) {
			kobject_put(&cma_kobj->kobj);
			goto out;
		}
	}

	/* 所有节点已发布并保留初始引用，启动函数不再执行正常路径 teardown。 */
	return 0;
out:
	/*
	 * 到达时当前 i 的失败包装已由 kobject_put() 处理或尚未发布；逆序 put
	 * 之前成功的子节点。未激活 area 的 cma_kobj 为 NULL，可安全跳过。
	 */
	while (--i >= 0) {
		cma = &cma_areas[i];
		if (cma->cma_kobj)
			kobject_put(&cma->cma_kobj->kobj);
	}
	/* 子节点引用全部撤销后再释放父根，保持 kobject 父子生命周期顺序。 */
	kobject_put(cma_kobj_root);

	return err;
}

/* 在设备等更晚子系统之前建立只读 CMA 观测接口。 */
subsys_initcall(cma_sysfs_init);
