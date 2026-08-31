// SPDX-License-Identifier: GPL-2.0
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>

/* 每组参数重复完整的“批量分配再全部释放”循环 100 次以获得可比较耗时。 */
#define NR_TESTS (100)

/*
 * 一次 dma_pool_alloc() 同时返回 CPU 可访问虚拟地址 v 和设备使用的 DMA 地址
 * dma；两者必须成对保存，并原样传回 dma_pool_free()。
 */
struct dma_pool_pair {
	/* 设备侧总线地址，不可用作 CPU 指针。 */
	dma_addr_t dma;
	/* CPU 侧一致性映射地址；成功分配后由当前测试循环临时持有。 */
	void *v;
};

/* 单个压测参数组，三个量均以 byte 为单位。 */
struct dmapool_parms {
	/* 每个小块的请求大小。 */
	size_t size;
	/* 块起始地址对齐，dma_pool 要求为 2 的幂。 */
	size_t align;
	/* 非零时单块不得跨越该 2 的幂边界；0 表示无额外跨界约束。 */
	size_t boundary;
};

/*
 * 覆盖从 16 B 到一页的自然对齐块，以及 68 B/32 B 对齐且不得跨 4 KiB
 * 边界的非整齐组合，用同一驱动模型设备比较 dma_pool 快慢路径。
 */
static const struct dmapool_parms pool_parms[] = {
	{ .size = 16, .align = 16, .boundary = 0 },
	{ .size = 64, .align = 64, .boundary = 0 },
	{ .size = 256, .align = 256, .boundary = 0 },
	{ .size = 1024, .align = 1024, .boundary = 0 },
	{ .size = 4096, .align = 4096, .boundary = 0 },
	{ .size = 68, .align = 32, .boundary = 4096 },
};

/* 当前参数组独占的 pool；模块初始化串行执行，测试之间不会并发替换它。 */
static struct dma_pool *pool;
/* 静态测试设备只为 DMA API 提供生命周期、sysfs 与 mask 上下文，不代表真实硬件。 */
static struct device test_dev;
/* test_dev.dma_mask 指向此稳定存储，dma_set_mask_and_coherent() 在其中提交能力。 */
static u64 dma_mask;

/*
 * nr_blocks() - 根据块大小选择一次循环的分配数量。
 * 业务背景：小块每页可容纳更多对象，测试按 PAGE_SIZE/size 放大工作量，同时用
 * 上下限避免极端大小导致样本过少或测试时间失控。
 * 入参：@size 为正的块大小，单位 byte；来自 pool_parms，当前范围 16..4096。
 * 出参/返回：返回 [1024, 8192] 内的块数，无状态或 ownership 变化。
 * 注意事项：纯算术 helper，不睡眠；若传 0 会除零，调用者必须保证正值。
 */
static inline int nr_blocks(int size)
{
	return clamp_t(int, (PAGE_SIZE / size) * 512, 1024, 8192);
}

/*
 * dmapool_test_alloc() - 完成一轮 blocks 个 DMA 块的分配与释放。
 *
 * 业务背景：dmapool_test_block() 重复调用它，既覆盖 pool 扩展分配也覆盖 free
 * 回收到池内的路径，并确保每轮结束时活动块数归零。
 * 入参：@p 指向至少 @blocks 项的输出/工作数组，由调用者独占；@blocks 为正的
 * 分配个数。全局 @pool 必须已创建且在调用期间稳定。
 * 出参/返回：全部分配并释放返回 0；任一分配失败返回 -ENOMEM，已成功前缀会
 * 逆序释放。返回时不持有 DMA 块，p 中旧地址只作失效历史值。
 * 注意事项：GFP_KERNEL 允许 dma_pool_alloc() 睡眠，不可在原子上下文；CPU/DMA
 * 地址必须严格配对释放，失败项自身没有可释放对象。
 */
static int dmapool_test_alloc(struct dma_pool_pair *p, int blocks)
{
	/* i 同时是当前索引和成功分配前缀长度，驱动正常与失败 cleanup。 */
	int i;

	/* 第一阶段填满数组；dma_pool_alloc() 通过输出参数写入匹配的 DMA 地址。 */
	for (i = 0; i < blocks; i++) {
		p[i].v = dma_pool_alloc(pool, GFP_KERNEL,
					&p[i].dma);
		if (!p[i].v)
			goto pool_fail;
	}

	/* 第二阶段释放完整批次，pool 仍保留底层 coherent 页供下轮复用。 */
	for (i = 0; i < blocks; i++)
		dma_pool_free(pool, p[i].v, p[i].dma);

	return 0;

pool_fail:
	/* p[i] 分配失败未取得 ownership，先 --i 再逆序释放成功的 [0, i) 前缀。 */
	for (--i; i >= 0; i--)
		dma_pool_free(pool, p[i].v, p[i].dma);
	return -ENOMEM;
}

/*
 * dmapool_test_block() - 对一组 size/align/boundary 参数执行计时压测。
 *
 * 业务背景：模块初始化按 pool_parms 调用；本函数创建专用 dma_pool，重复 100
 * 次批量分配/释放，打印微秒耗时后销毁全部临时资源。
 * 入参：@parms 是静态参数表中的只读借用指针，不可为 NULL且数值满足 dma_pool
 * 约束；函数不保留该指针。
 * 出参/返回：成功返回 0；pair 数组、pool 创建或块分配失败返回 -ENOMEM；无论
 * 成败都在返回前销毁已创建 pool 并释放数组。
 * 注意事项：可睡眠且运行时间较长；计时包含分配/释放与主动让出，不包含 pool
 * 创建销毁。全局 pool 仅在本函数期间有效。
 */
static int dmapool_test_block(const struct dmapool_parms *parms)
{
	/* blocks 是每轮工作规模；时间戳包围全部 NR_TESTS 轮。 */
	int blocks = nr_blocks(parms->size);
	ktime_t start_time, end_time;
	struct dma_pool_pair *p;
	int i, ret;

	/* pair 数组仅保存本轮成对地址，清零使未写槽位保持明确的空状态。 */
	p = kzalloc_objs(*p, blocks);
	if (!p)
		return -ENOMEM;

	/* pool 从 test_dev 的 coherent DMA 能力派生，成功后由本函数独占并最终销毁。 */
	pool = dma_pool_create("test pool", &test_dev, parms->size,
			       parms->align, parms->boundary);
	if (!pool) {
		ret = -ENOMEM;
		goto free_pairs;
	}

	/* 核心计时循环：每轮内部无活动块遗留，调度请求出现时主动让出 CPU。 */
	start_time = ktime_get();
	for (i = 0; i < NR_TESTS; i++) {
		ret = dmapool_test_alloc(p, blocks);
		if (ret)
			goto free_pool;
		if (need_resched())
			cond_resched();
	}
	end_time = ktime_get();

	/* 输出参数、每轮块数以及 100 轮总耗时，供不同实现或版本横向比较。 */
	printk("dmapool test: size:%-4zu align:%-4zu blocks:%-4d time:%llu\n",
		parms->size, parms->align, blocks,
		ktime_us_delta(end_time, start_time));

free_pool:
	/* 到达时 dmapool_test_alloc() 已释放其成功前缀，pool 内无活动测试块。 */
	dma_pool_destroy(pool);
free_pairs:
	/* pair 数组最后释放；ret 保留触发 cleanup 的成功值或 -ENOMEM。 */
	kfree(p);
	return ret;
}

/*
 * dmapool_test_release() - 完成静态测试 device 的引用计数释放回调契约。
 * 业务背景：put_device() 的最后引用需要 release 回调；承载对象是静态变量，不能 kfree。
 * 入参：@dev 指向 test_dev 的借用指针，本函数不读取也不保留它。
 * 出参/返回：无直接返回值、无副作用。
 * 注意事项：只用于满足 device core 生命周期；真实测试资源已在此前显式销毁。
 */
static void dmapool_test_release(struct device *dev)
{
}

/*
 * dmapool_checks() - 注册伪 DMA 设备并依次运行全部 dma_pool 参数组。
 *
 * 业务背景：作为 module_init 入口，构造 DMA API 所需 device 上下文；测试结束即
 * 删除并 put 设备，因此模块驻留期间不保留 pool 或注册设备。
 * 入参：无。
 * 出参/返回：全部参数组通过返回 0；命名、注册、DMA mask 或压测失败返回相应
 * 错误码。所有已取得的 device/pool/pair 资源均沿标签回滚。
 * 注意事项：进程上下文执行并可睡眠；test_dev/dma_mask 为模块全局且不可并发复用。
 */
static int dmapool_checks(void)
{
	/* i 遍历静态参数表；ret 在每阶段保存下一出口要返回的状态。 */
	int i, ret;

	/* 设置 kobject 名称；成功后该名称随 device 最后 put 一并清理。 */
	ret = dev_set_name(&test_dev, "dmapool-test");
	if (ret)
		return ret;

	/* device_register() 初始化引用并尝试发布；即使失败也必须 put_device()。 */
	ret = device_register(&test_dev);
	if (ret) {
		printk("%s: register failed:%d\n", __func__, ret);
		goto put_device;
	}

	/*
	 * 成功注册后补齐静态对象 release，选择架构默认 DMA ops，并让 dma_mask
	 * 指针引用模块全寿命存储；随后验证 64 位 streaming/coherent mask。
	 */
	test_dev.release = dmapool_test_release;
	set_dma_ops(&test_dev, NULL);
	test_dev.dma_mask = &dma_mask;
	ret = dma_set_mask_and_coherent(&test_dev, DMA_BIT_MASK(64));
	if (ret) {
		printk("%s: mask failed:%d\n", __func__, ret);
		goto del_device;
	}

	/* 参数组串行运行，共用全局 pool 句柄；首个失败终止后续测试。 */
	for (i = 0; i < ARRAY_SIZE(pool_parms); i++) {
		ret = dmapool_test_block(&pool_parms[i]);
		if (ret)
			break;
	}

del_device:
	/* 注册成功后的统一撤销：先从 driver model/sysfs 摘除，再释放初始引用。 */
	device_del(&test_dev);
put_device:
	/* 注册失败也已由 device_register() 初始化引用，必须通过 put 而非直接清理。 */
	put_device(&test_dev);
	return ret;
}

/*
 * dmapool_exit() - 模块卸载空操作。
 * 业务背景：module_init 已同步完成测试并清理设备、pool 和数组，卸载时无资源遗留。
 * 入参：无。
 * 出参/返回：无直接返回值、无副作用。
 * 注意事项：若未来让测试资源跨越模块驻留期，必须同步扩展此 teardown。
 */
static void dmapool_exit(void)
{
}

/* 加载模块即运行同步压测；卸载入口当前只表达对称生命周期。 */
module_init(dmapool_checks);
module_exit(dmapool_exit);
/* 模块元数据说明其用途是 dma_pool 耗时测试，许可证允许调用 GPL-only device API。 */
MODULE_DESCRIPTION("dma_pool timing test");
MODULE_LICENSE("GPL");
