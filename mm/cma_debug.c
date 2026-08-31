// SPDX-License-Identifier: GPL-2.0
/*
 * CMA DebugFS Interface
 *
 * Copyright (c) 2015 Sasha Levin <sasha.levin@oracle.com>
 */
/*
 * 本文件把已激活 CMA area 暴露给 debugfs：只读节点观察容量、占用和最大
 * 连续空闲块，alloc/free 节点则主动制造并撤销 CMA 分配，主要用于调试与
 * 压力实验。它不是稳定用户 ABI，写接口持有自己分配出的页，不能释放其他
 * CMA 客户端的对象。
 */


#include <linux/debugfs.h>
#include <linux/cma.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm_types.h>

#include "cma.h"

/*
 * 一次由 debugfs alloc 创建、尚未被 free 完全归还的测试分配记录。p 指向
 * 连续页首，n 以 base page 为单位；节点由 cma->mem_head_lock 保护，摘链后
 * 临时 ownership 归释放者，整项释放后 kfree，部分释放后以更新后的 p/n
 * 重新挂回。实际页的 ownership 则由 CMA bitmap 和该记录共同表达。
 */
struct cma_mem {
	struct hlist_node node;
	struct page *p;
	unsigned long n;
};

/*
 * 业务背景：为 count/order_per_bit/base_pfn 等 unsigned long 字段复用
 * DEFINE_DEBUGFS_ATTRIBUTE 的只读 getter。
 * 入参：data 是 debugfs inode 借出的 unsigned long 指针且必须长期有效；val
 * 是调用方提供的非 NULL 输出槽。
 * 出参/返回：把字段当前值扩展到 u64 写入 *val，返回 0；无引用或 ownership 转移。
 * 注意事项：该通用 getter不加锁，适用于初始化后不变字段或允许近似观察的值；
 * 可变的 available_count 由专用 getter 在 cma->lock 下读取。
 */
static int cma_debugfs_get(void *data, u64 *val)
{
	unsigned long *p = data;

	*val = *p;

	return 0;
}

/* 生成只读 file_operations：open/read/release 由 debugfs attribute 框架承接。 */
DEFINE_DEBUGFS_ATTRIBUTE(cma_debugfs_fops, cma_debugfs_get, NULL, "%llu\n");

/*
 * 业务背景：debugfs 的 used 节点展示 CMA 已认领页数，即总容量减可用容量。
 * 入参：data 是借用的已激活 cma；val 是非 NULL u64 输出槽。
 * 出参/返回：在 *val 写入当前已用 base page 数并返回 0；不保留 cma 引用。
 * 注意事项：cma->lock 与分配/释放路径配对，保证 count 与 available_count 的
 * 同一时刻关系；spin_lock_irq 避免本 CPU 中断侧形成锁反转，持锁区不可睡眠。
 */
static int cma_used_get(void *data, u64 *val)
{
	struct cma *cma = data;

	spin_lock_irq(&cma->lock);
	*val = cma->count - cma->available_count;
	spin_unlock_irq(&cma->lock);

	return 0;
}

/* used 只读节点通过专用 getter 获得锁内一致快照。 */
DEFINE_DEBUGFS_ATTRIBUTE(cma_used_fops, cma_used_get, NULL, "%llu\n");

/*
 * 业务背景：maxchunk 节点估计当前任一 CMA range 中可立即认领的最大连续块，
 * 帮助区分“总空闲足够”与“bitmap 已碎片化”。
 * 入参：data 是借用的已激活 cma；val 是非 NULL 输出槽。
 * 出参/返回：写入最大连续清零 bitmap 区间折算出的 base page 数，返回 0。
 * 注意事项：持 cma->lock 扫描所有 range，与 bitmap 认领/释放互斥；扫描只反映
 * CMA bitmap，不保证随后 alloc_contig_range 不受迁移失败影响。锁内不可睡眠。
 */
static int cma_maxchunk_get(void *data, u64 *val)
{
	struct cma *cma = data;
	struct cma_memrange *cmr;
	unsigned long maxchunk = 0;
	unsigned long start, end;
	unsigned long bitmap_maxno;
	int r;

	spin_lock_irq(&cma->lock);
	/* 每个 range 有独立 bitmap；跨 range 的空闲位不能拼成一个连续候选。 */
	for (r = 0; r < cma->nranges; r++) {
		cmr = &cma->ranges[r];
		bitmap_maxno = cma_bitmap_maxno(cma, cmr);
		for_each_clear_bitrange(start, end, cmr->bitmap, bitmap_maxno)
			maxchunk = max(end - start, maxchunk);
	}
	spin_unlock_irq(&cma->lock);
	/* bitmap 一位代表 2^order_per_bit 个 base page，出锁后只转换局部快照。 */
	*val = (u64)maxchunk << cma->order_per_bit;

	return 0;
}

/* maxchunk 是锁内计算的只读 attribute。 */
DEFINE_DEBUGFS_ATTRIBUTE(cma_maxchunk_fops, cma_maxchunk_get, NULL, "%llu\n");

/*
 * 业务背景：把新测试分配发布到该 area 的 LIFO 记录链，供后续 free 写操作认领。
 * 入参：cma 为借用且已初始化；mem 为调用者独占、未挂链的有效记录。
 * 出参/返回：void；成功后 mem 链接 ownership 转给 cma->mem_head，页仍由记录代表。
 * 注意事项：mem_head_lock 只保护测试记录链，不保护 CMA bitmap；函数不可在已持
 * 同一锁时调用，spinlock 临界区内不睡眠。
 */
static void cma_add_to_cma_mem_list(struct cma *cma, struct cma_mem *mem)
{
	spin_lock(&cma->mem_head_lock);
	hlist_add_head(&mem->node, &cma->mem_head);
	spin_unlock(&cma->mem_head_lock);
}

/*
 * 业务背景：free 路径从测试记录链取出一个对象，在锁外执行可能较重的 CMA 释放。
 * 入参：cma 是借用的已初始化 area。
 * 出参/返回：链非空时返回已摘除记录并把释放责任转给调用者；空链返回 NULL。
 * 注意事项：mem_head_lock 保证每项只被一个并发 free writer 领取；返回后对象不再
 * 受链锁保护，调用者必须 kfree 或更新后重新挂链。
 */
static struct cma_mem *cma_get_entry_from_list(struct cma *cma)
{
	struct cma_mem *mem = NULL;

	spin_lock(&cma->mem_head_lock);
	/* 摘链发生在锁内，后续页释放故意移到锁外以缩短自旋锁临界区。 */
	if (!hlist_empty(&cma->mem_head)) {
		mem = hlist_entry(cma->mem_head.first, struct cma_mem, node);
		hlist_del_init(&mem->node);
	}
	spin_unlock(&cma->mem_head_lock);

	return mem;
}

/*
 * 业务背景：响应 debugfs free=N，按 LIFO 顺序最多归还 N 个由本接口分配的页。
 * 入参：cma 为借用 area；count 是期望释放的 base page 数，应为正且能由写入值
 * 无损转换为 int。
 * 出参/返回：始终返回 0；链提前为空或粒度不允许部分释放时也是部分成功。
 * 注意事项：整记录路径调用 cma_release() 后销毁记录；order_per_bit==0 才能从
 * 记录头部按任意页数部分释放。函数不报告实际释放量，且忽略 cma_release()
 * 布尔返回，依赖记录始终来自同一 cma_alloc() 的内部不变量。
 */
static int cma_free_mem(struct cma *cma, int count)
{
	struct cma_mem *mem = NULL;

	while (count) {
		/* 每轮独占领取一项；链空意味着已尽最大努力完成请求。 */
		mem = cma_get_entry_from_list(cma);
		if (mem == NULL)
			return 0;

		if (mem->n <= count) {
			/* 整项释放后记录不再代表任何页，可立即销毁。 */
			cma_release(cma, mem->p, mem->n);
			count -= mem->n;
			kfree(mem);
		} else if (cma->order_per_bit == 0) {
			/* 一位一页时可释放前缀，并把剩余区间的新首址/长度重新发布。 */
			cma_release(cma, mem->p, count);
			mem->p += count;
			mem->n -= count;
			count = 0;
			cma_add_to_cma_mem_list(cma, mem);
		} else {
			/* 多页 bitmap 粒度不能表达任意前缀，保持整项记录以免丢失 ownership。 */
			pr_debug("cma: cannot release partial block when order_per_bit != 0\n");
			cma_add_to_cma_mem_list(cma, mem);
			break;
		}
	}

	return 0;

}

/*
 * 业务背景：把 debugfs attribute 的 u64 写值适配为 cma_free_mem() 页数请求。
 * 入参：data 是借用 cma；val 是用户写入的无符号页数。
 * 出参/返回：返回 cma_free_mem() 的 0；可能只完成部分释放。
 * 注意事项：当前实现先窄化为 int 且不做范围校验，调用者应只写 1..INT_MAX；
 * 超范围值的截断不构成稳定 ABI。函数不取得 cma ownership。
 */
static int cma_free_write(void *data, u64 val)
{
	int pages = val;
	struct cma *cma = data;

	return cma_free_mem(cma, pages);
}

/* free 是只写 attribute；框架负责解析十进制 u64 后调用 setter。 */
DEFINE_DEBUGFS_ATTRIBUTE(cma_free_fops, NULL, cma_free_write, "%llu\n");

/*
 * 业务背景：为 debugfs alloc=N 主动从指定 CMA area 取得 N 个连续 base page，
 * 并创建记录使这些页只能由本调试接口后续归还。
 * 入参：cma 是借用的已激活 area；count 是正的 base page 数。
 * 出参/返回：成功返回 0并把页与记录发布到 mem_head；记录分配或 CMA 分配失败
 * 返回 -ENOMEM，失败时不留下页/记录。
 * 注意事项：align=0 只要求 CMA 最小对齐，no_warn=false 允许正常告警；cma_alloc()
 * 可睡眠且在链锁外执行。记录先分配，从而避免取得连续页后无法登记 ownership。
 */
static int cma_alloc_mem(struct cma *cma, int count)
{
	struct cma_mem *mem;
	struct page *p;

	mem = kzalloc_obj(*mem);
	if (!mem)
		return -ENOMEM;

	/* 尚未发布 mem；若 CMA 失败，当前函数仍独占它并立即回滚。 */
	p = cma_alloc(cma, count, 0, false);
	if (!p) {
		kfree(mem);
		return -ENOMEM;
	}

	mem->p = p;
	mem->n = count;

	/* 此处是记录发布点；返回后并发 free writer 可以摘取并释放这批页。 */
	cma_add_to_cma_mem_list(cma, mem);

	return 0;
}

/*
 * 业务背景：把 alloc 文件的 u64 写入适配为连续页分配请求。
 * 入参：data 是借用 cma；val 是用户写入的页数。
 * 出参/返回：透传 0 或 -ENOMEM；成功后页由调试记录链持有。
 * 注意事项：u64 到 int 的窄化未校验，使用者应限制为 1..INT_MAX；超范围值
 * 可能改变底层 unsigned long count，不能视为受支持输入。
 */
static int cma_alloc_write(void *data, u64 val)
{
	int pages = val;
	struct cma *cma = data;

	return cma_alloc_mem(cma, pages);
}

/* alloc 是只写 attribute；读取回调为空。 */
DEFINE_DEBUGFS_ATTRIBUTE(cma_alloc_fops, NULL, cma_alloc_write, "%llu\n");

/*
 * 业务背景：为一个已激活 CMA area 构造完整 debugfs 子树和每个 range 的视图。
 * 入参：cma 是全局数组中长期存活的借用对象；root_dentry 是 /sys/kernel/debug/cma。
 * 出参/返回：void；创建 area/ranges 目录、属性、bitmap 数组和兼容符号链接。
 * 注意事项：仅 late init 调用，无并发发布者；debugfs helper 允许返回错误指针或
 * NULL 且本调试接口不传播失败。存入 dfs_bitmap 的数组借用 range bitmap，二者
 * 生命周期都随静态 CMA area 持续到系统结束。
 */
static void cma_debugfs_add_one(struct cma *cma, struct dentry *root_dentry)
{
	struct dentry *tmp, *dir, *rangedir;
	int r;
	char rdirname[12];
	struct cma_memrange *cmr;

	tmp = debugfs_create_dir(cma->name, root_dentry);

	/* area 根节点提供主动测试入口和聚合容量/碎片观测。 */
	debugfs_create_file("alloc", 0200, tmp, cma, &cma_alloc_fops);
	debugfs_create_file("free", 0200, tmp, cma, &cma_free_fops);
	debugfs_create_file("count", 0444, tmp, &cma->count, &cma_debugfs_fops);
	debugfs_create_file("order_per_bit", 0444, tmp,
			    &cma->order_per_bit, &cma_debugfs_fops);
	debugfs_create_file("used", 0444, tmp, cma, &cma_used_fops);
	debugfs_create_file("maxchunk", 0444, tmp, cma, &cma_maxchunk_fops);

	rangedir = debugfs_create_dir("ranges", tmp);
	/* 每个物理 range 分目录暴露基址及 bitmap 原始 u32 视图。 */
	for (r = 0; r < cma->nranges; r++) {
		cmr = &cma->ranges[r];
		snprintf(rdirname, sizeof(rdirname), "%d", r);
		dir = debugfs_create_dir(rdirname, rangedir);
		debugfs_create_file("base_pfn", 0444, dir,
			    &cmr->base_pfn, &cma_debugfs_fops);
		cmr->dfs_bitmap.array = (u32 *)cmr->bitmap;
		/* n_elements 向上取整，末个 u32 的高位可能超出有效 bitmap 位数。 */
		cmr->dfs_bitmap.n_elements =
			DIV_ROUND_UP(cma_bitmap_maxno(cma, cmr),
					BITS_PER_BYTE * sizeof(u32));
		debugfs_create_u32_array("bitmap", 0444, dir,
				&cmr->dfs_bitmap);
	}

	/*
	 * Backward compatible symlinks to range 0 for base_pfn and bitmap.
	 */
	/*
	 * 为兼容旧的单 range 布局，area 根下两个旧名称指向 ranges/0；新工具应遍历
	 * ranges，因为多 range area 的后续区间不会经旧链接呈现。
	 */
	debugfs_create_symlink("base_pfn", tmp, "ranges/0/base_pfn");
	debugfs_create_symlink("bitmap", tmp, "ranges/0/bitmap");
}

/*
 * 业务背景：所有 CMA area 完成早期激活后，在 late_initcall 阶段一次性发布
 * debugfs 控制面，确保 range bitmap 与锁均已就绪。
 * 入参：无。
 * 出参/返回：返回 0，即使个别 debugfs 节点创建失败也不阻止启动。
 * 注意事项：只为 CMA_ACTIVATED area 建目录；根 dentry 由 debugfs 持有，本文件
 * 无退出清理，因为它是内建 MM 初始化代码而非可卸载模块。
 */
static int __init cma_debugfs_init(void)
{
	struct dentry *cma_debugfs_root;
	int i;

	cma_debugfs_root = debugfs_create_dir("cma", NULL);

	/* cma_area_count 和激活位在此阶段稳定，遍历不需要 cma->lock。 */
	for (i = 0; i < cma_area_count; i++)
		if (test_bit(CMA_ACTIVATED, &cma_areas[i].flags))
			cma_debugfs_add_one(&cma_areas[i], cma_debugfs_root);

	return 0;
}

/* 晚于 CMA 激活执行，避免把未完成初始化的 area 暴露给 debugfs。 */
late_initcall(cma_debugfs_init);
