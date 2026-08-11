// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 HiSilicon Limited.
 */

/* 为所有 pr_*() 诊断统一加模块名前缀，便于并发 benchmark 失败时定位日志来源。 */
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <uapi/linux/map_benchmark.h>

/*
 * 一次 debugfs benchmark 会话的共享状态。
 *
 * @bparam: ioctl 从用户态复制进来的配置，并在结束时承载均值和标准差结果。
 * @dev: 被临时用于 DMA map/unmap 的绑定设备；运行期间由 do_map_benchmark() 额外持有引用。
 * @debugfs: 根目录测试文件，设备解绑时由 devm action 删除。
 * @dir: ioctl 校验后得到的内核 DMA 方向；当前数据路径仍从 bparam.dma_dir 填入线程私有参数。
 * @sum_*: 所有线程以 100 ns 为单位累加的 map/unmap 延时及其平方和。
 * @loops: 已完整完成 map、模拟传输、unmap 的样本总数，是统计量的共同分母。
 *
 * 对象由 probe 的 devm_kzalloc() 创建并随设备释放。ioctl 串行使用这一共享对象是接口前提；工作
 * 线程仅并发更新 atomic64_t 统计量并只读配置，主线程在全部 kthread_stop_put() 后读取最终快照。
 */
struct map_benchmark_data {
	struct map_benchmark bparam;
	struct device *dev;
	struct dentry  *debugfs;
	enum dma_data_direction dir;
	atomic64_t sum_map_100ns;
	atomic64_t sum_unmap_100ns;
	atomic64_t sum_sq_map;
	atomic64_t sum_sq_unmap;
	atomic64_t loops;
};

/*
 * 把 benchmark 主循环与具体缓冲组织方式解耦的操作表。
 *
 * prepare/unprepare 管理每个线程的私有资源；initialize_data 在计时前制造真实缓存状态；do_map 与
 * do_unmap 是唯一落入计时区间的配对操作。操作表为模块期只读全局对象，函数指针不在运行时更换；
 * prepare 失败时主循环不会调用其余回调，map 成功后则保证恰好一次 unmap。
 */
struct map_benchmark_ops {
	void *(*prepare)(struct map_benchmark_data *map);
	void (*unprepare)(void *mparam);
	void (*initialize_data)(void *mparam);
	int (*do_map)(void *mparam);
	void (*do_unmap)(void *mparam);
};

/*
 * single 模式单条工作线程的私有参数。
 *
 * @dev: 借用共享会话中已持引用的设备。
 * @addr: 最近一次 dma_map_single() 成功返回的 DMA 地址，仅在 map/unmap 配对区间有效。
 * @xbuf: alloc_pages_exact() 取得的物理连续 CPU 缓冲区。
 * @npages: 缓冲区页数，single 模式下等于用户 granule。
 * @dma_dir: DMA 方向；必须在 map 与 unmap 时保持一致。
 *
 * 每条 kthread 独占一个对象和缓冲区，不需要线程间锁；prepare 成功后由 unprepare 统一释放。
 */
struct dma_single_map_param {
	struct device *dev;
	dma_addr_t addr;
	void *xbuf;
	u32 npages;
	u32 dma_dir;
};

/*
 * 为 single 模式线程分配参数对象和连续测试缓冲区。
 *
 * @map: 共享会话，提供设备、granule 和方向。
 * 返回值: 成功返回线程独占参数；任一 GFP_KERNEL 分配失败返回 NULL。
 *
 * params 带 __free(kfree) 自动清理，只有 return_ptr() 在成功路径转移其 ownership；缓冲区失败时
 * 参数自动释放，成功后由 unprepare 同时释放两者。函数可能睡眠，只在新建 kthread 的启动阶段调用。
 */
static void *dma_single_map_benchmark_prepare(struct map_benchmark_data *map)
{
	struct dma_single_map_param *params __free(kfree) = kzalloc(sizeof(*params),
								    GFP_KERNEL);
	if (!params)
		return NULL;

	params->npages = map->bparam.granule;
	params->dma_dir = map->bparam.dma_dir;
	params->dev = map->dev;
	params->xbuf = alloc_pages_exact(params->npages * PAGE_SIZE, GFP_KERNEL);
	if (!params->xbuf)
		return NULL;

	return_ptr(params);
}

/*
 * 销毁 single 模式线程的全部私有资源。
 *
 * @mparam: prepare 成功返回的 struct dma_single_map_param。
 *
 * 调用前必须已撤销最后一次 DMA 映射；先按完全相同的页数归还 exact-pages 缓冲区，再释放描述对象。
 * 它在工作线程退出路径执行且可进入页分配器同步，返回后 mparam、xbuf 和 addr 均不可再使用。
 */
static void dma_single_map_benchmark_unprepare(void *mparam)
{
	struct dma_single_map_param *params = mparam;

	free_pages_exact(params->xbuf, params->npages * PAGE_SIZE);
	kfree(params);
}

/*
 * 在 single map 计时前预热 CPU 缓存中的源数据。
 *
 * @mparam: 当前线程独占的 single 参数。
 *
 * FROM_DEVICE 不需要 CPU 向设备提供初始内容；其余方向用 0x66 覆盖整块缓冲，使非一致性设备的
 * map 路径必须处理真实脏缓存线。写入不计入 map 延时。线程独占 xbuf，函数不加锁、不睡眠。
 */
static void dma_single_map_benchmark_initialize_data(void *mparam)
{
	struct dma_single_map_param *params = mparam;

	/*
	 * for a non-coherent device, if we don't stain them in the
	 * cache, this will give an underestimate of the real-world
	 * overhead of BIDIRECTIONAL or TO_DEVICE mappings;
	 * 66 means everything goes well! 66 is lucky.
	 */
	/*
	 * 非一致性设备若没有先把缓冲区写脏，BIDIRECTIONAL/TO_DEVICE 的真实缓存维护成本会被低估；
	 * 0x66 只是用于制造脏缓存状态的幸运填充值，不参与结果判断。
	 */
	if (params->dma_dir != DMA_FROM_DEVICE)
		memset(params->xbuf, 0x66, params->npages * PAGE_SIZE);
}

/*
 * 对 single 缓冲区执行一次被计时的 DMA 映射。
 *
 * @mparam: 当前线程私有参数；成功时更新 addr。
 * 返回值: 成功为 0；dma_mapping_error() 时记录设备名并返回 -ENOMEM。
 *
 * 成功把缓冲区 ownership 按 dma_dir 交给设备侧，后续必须先 do_unmap() 才能再次初始化或释放；
 * DMA map API 可在非睡眠上下文使用，本函数不持额外锁，xbuf/addr 仅由当前线程访问。
 */
static int dma_single_map_benchmark_do_map(void *mparam)
{
	struct dma_single_map_param *params = mparam;

	params->addr = dma_map_single(params->dev, params->xbuf,
				      params->npages * PAGE_SIZE, params->dma_dir);
	if (unlikely(dma_mapping_error(params->dev, params->addr))) {
		pr_err("dma_map_single failed on %s\n", dev_name(params->dev));
		return -ENOMEM;
	}

	return 0;
}

/*
 * 撤销最近一次成功的 single DMA 映射。
 *
 * @mparam: 含有效 addr、原长度和方向的线程私有参数。
 *
 * dma_unmap_single() 完成必要的设备到 CPU 同步并结束 DMA 地址生命周期；无返回值且只允许对成功
 * 映射调用一次。函数不等待真实硬件，本 benchmark 只以 ndelay() 模拟传输阶段。
 */
static void dma_single_map_benchmark_do_unmap(void *mparam)
{
	struct dma_single_map_param *params = mparam;

	dma_unmap_single(params->dev, params->addr,
			 params->npages * PAGE_SIZE, params->dma_dir);
}

/* single 模式操作表在模块整个加载期存活，所有线程只读并共享其函数指针。 */
static struct map_benchmark_ops dma_single_map_benchmark_ops = {
	.prepare = dma_single_map_benchmark_prepare,
	.unprepare = dma_single_map_benchmark_unprepare,
	.initialize_data = dma_single_map_benchmark_initialize_data,
	.do_map = dma_single_map_benchmark_do_map,
	.do_unmap = dma_single_map_benchmark_do_unmap,
};

/*
 * SG 模式单条工作线程的私有资源集合。
 *
 * @sgt: 含 npages 个输入项的 scatterlist table；map 后 DMA 字段由 DMA API 临时填写。
 * @dev: 借用共享会话持有引用的设备。
 * @npages: SG 输入项数，每项固定映射一页。
 * @dma_dir: map/unmap 必须一致的数据方向。
 * @buf: 柔性数组，逐项保存 __get_free_page() 返回的 CPU 页地址，长度由 __counted_by 约束。
 *
 * 每线程独占表、页和 DMA 字段；prepare 建全对象，unprepare 在所有映射撤销后逆向释放。
 */
struct dma_sg_map_param {
	struct sg_table sgt;
	struct device *dev;
	u32 npages;
	u32 dma_dir;
	void *buf[] __counted_by(npages);
};

/*
 * 为 SG 模式线程建立参数、scatterlist 和逐页缓冲区。
 *
 * @map: 共享 benchmark 配置。
 * 返回值: 全部 npages 项构造成功时返回线程私有对象；失败为 NULL。
 *
 * granule 在 SG 模式表示“表项数”，每项恰好一页。失败回滚按已完成的 i 项释放页，再释放 SG table
 * 和柔性对象；因此未初始化的 buf 槽不会被访问。所有分配使用 GFP_KERNEL，函数可睡眠且不并发发布
 * 半初始化对象。
 */
static void *dma_sg_map_benchmark_prepare(struct map_benchmark_data *map)
{
	struct dma_sg_map_param *params;
	struct scatterlist *sg;
	u32 npages;
	int i;

	/*
	 * Set the number of scatterlist entries based on the granule.
	 * In SG mode, 'granule' represents the number of scatterlist entries.
	 * Each scatterlist entry corresponds to a single page.
	 */
	/* SG 模式的 granule 是 scatterlist 表项数；每个表项绑定一个单独分配的 PAGE_SIZE 页。 */
	npages = map->bparam.granule;

	params = kzalloc_flex(*params, buf, npages);
	if (!params)
		return NULL;

	params->npages = npages;
	params->dma_dir = map->bparam.dma_dir;
	params->dev = map->dev;

	if (sg_alloc_table(&params->sgt, params->npages, GFP_KERNEL))
		goto free_params;

	for_each_sgtable_sg(&params->sgt, sg, i) {
		params->buf[i] = (void *)__get_free_page(GFP_KERNEL);
		if (!params->buf[i])
			goto free_page;

		sg_set_buf(sg, params->buf[i], PAGE_SIZE);
	}

	return params;

free_page:
	while (i-- > 0)
		free_page((unsigned long)params->buf[i]);

	sg_free_table(&params->sgt);
free_params:
	kfree(params);
	return NULL;
}

/*
 * 释放 SG 模式线程的所有私有页、表和参数对象。
 *
 * @mparam: prepare 完整成功的 struct dma_sg_map_param。
 *
 * 调用前所有 SG DMA 映射必须已经撤销；逐项归还页后销毁 table，最后释放含柔性数组的对象。
 * 该释放路径在工作线程退出时串行执行，返回后任何 sg/buf 指针均失效。
 */
static void dma_sg_map_benchmark_unprepare(void *mparam)
{
	struct dma_sg_map_param *params = mparam;
	int i;

	for (i = 0; i < params->npages; i++)
		free_page((unsigned long)params->buf[i]);

	sg_free_table(&params->sgt);

	kfree(params);
}

/*
 * 在 SG map 计时前按页预热 CPU 缓存数据。
 *
 * @mparam: 当前线程独占的 SG 参数。
 *
 * FROM_DEVICE 跳过写入；BIDIRECTIONAL/TO_DEVICE 对每个输入页填充 0x66，使后续映射包含真实的
 * 非一致性缓存清理成本。循环不睡眠、不加锁，且不修改 SG 拓扑。
 */
static void dma_sg_map_benchmark_initialize_data(void *mparam)
{
	struct dma_sg_map_param *params = mparam;
	struct scatterlist *sg;
	int i = 0;

	if (params->dma_dir == DMA_FROM_DEVICE)
		return;

	for_each_sgtable_sg(&params->sgt, sg, i)
		memset(params->buf[i], 0x66, PAGE_SIZE);
}

/*
 * 映射当前线程的完整 SG 输入表。
 *
 * @mparam: 含 npages 个单页条目的线程私有参数。
 * 返回值: dma_map_sg() 返回非零映射段数时为 0；返回 0 时记录错误并转成 -ENOMEM。
 *
 * DMA API 可能合并相邻输入项，返回段数只用于判断成功；unmap 仍必须传原输入 npages。成功后 SG
 * DMA 字段和设备 ownership 一直有效到 do_unmap()，本函数不保存合并段数，也不与其他线程共享表。
 */
static int dma_sg_map_benchmark_do_map(void *mparam)
{
	struct dma_sg_map_param *params = mparam;
	int ret = 0;

	int sg_mapped = dma_map_sg(params->dev, params->sgt.sgl,
				   params->npages, params->dma_dir);
	if (!sg_mapped) {
		pr_err("dma_map_sg failed on %s\n", dev_name(params->dev));
		ret = -ENOMEM;
	}

	return ret;
}

/*
 * 撤销当前线程最近一次成功的 SG 映射。
 *
 * @mparam: SG 表、原输入项数和方向均与 do_map() 相同的私有参数。
 *
 * 按 DMA API 契约传入原始 npages 而非合并后的段数；unmap 完成缓存/ownership 回收后，各 CPU 页
 * 才能重新初始化或释放。函数不等待硬件，本测试用固定延时模拟设备活动窗口。
 */
static void dma_sg_map_benchmark_do_unmap(void *mparam)
{
	struct dma_sg_map_param *params = mparam;

	dma_unmap_sg(params->dev, params->sgt.sgl, params->npages,
		     params->dma_dir);
}

/* SG 模式操作表在模块期固定，prepare/unprepare 与 map/unmap 必须成对使用。 */
static struct map_benchmark_ops dma_sg_map_benchmark_ops = {
	.prepare = dma_sg_map_benchmark_prepare,
	.unprepare = dma_sg_map_benchmark_unprepare,
	.initialize_data = dma_sg_map_benchmark_initialize_data,
	.do_map = dma_sg_map_benchmark_do_map,
	.do_unmap = dma_sg_map_benchmark_do_unmap,
};

/* 用户 ABI 的 map_mode 直接索引该表；ioctl 在创建线程前验证索引小于 MODE_MAX。 */
static struct map_benchmark_ops *dma_map_benchmark_ops[DMA_MAP_BENCH_MODE_MAX] = {
	[DMA_MAP_BENCH_SINGLE_MODE] = &dma_single_map_benchmark_ops,
	[DMA_MAP_BENCH_SG_MODE] = &dma_sg_map_benchmark_ops,
};

/*
 * 单条 benchmark kthread 的完整工作循环。
 *
 * @data: 指向 probe 期 map_benchmark_data；设备引用由主控函数覆盖全部线程寿命。
 * 返回值: 正常被 stop 时为 0；私有资源准备失败为 -ENOMEM，map 失败返回相应错误。
 *
 * 每线程先按模式建立独占缓冲区，随后反复执行“预热 → 计时 map → 模拟 DMA → 计时 unmap → 原子
 * 聚合”。只有成功 unmap 的完整样本才增加 loops。kthread_should_stop() 与 cond_resched() 共同保证
 * 停止请求可达；退出总会 unprepare 私有资源。配置只读、统计用 atomic64，不需要共享互斥锁。
 */
static int map_benchmark_thread(void *data)
{
	struct map_benchmark_data *map = data;
	__u8 map_mode = map->bparam.map_mode;
	int ret = 0;

	struct map_benchmark_ops *mb_ops = dma_map_benchmark_ops[map_mode];
	void *mparam = mb_ops->prepare(map);

	if (!mparam)
		return -ENOMEM;

	while (!kthread_should_stop())  {
		u64 map_100ns, unmap_100ns, map_sq, unmap_sq;
		ktime_t map_stime, map_etime, unmap_stime, unmap_etime;
		ktime_t map_delta, unmap_delta;

		mb_ops->initialize_data(mparam);
		map_stime = ktime_get();
		ret = mb_ops->do_map(mparam);
		if (ret)
			goto out;

		map_etime = ktime_get();
		map_delta = ktime_sub(map_etime, map_stime);

		/* Pretend DMA is transmitting */
		/* benchmark 没有真实硬件事务，以忙等纳秒延时模拟 DMA 地址处于设备 ownership 的时间。 */
		ndelay(map->bparam.dma_trans_ns);

		unmap_stime = ktime_get();
		mb_ops->do_unmap(mparam);

		unmap_etime = ktime_get();
		unmap_delta = ktime_sub(unmap_etime, unmap_stime);

		/* calculate sum and sum of squares */
		/* 先把纳秒截断为 100 ns 单位，再累加一阶矩和平方矩，供主线程计算均值与标准差。 */

		map_100ns = div64_ul(map_delta,  100);
		unmap_100ns = div64_ul(unmap_delta, 100);
		map_sq = map_100ns * map_100ns;
		unmap_sq = unmap_100ns * unmap_100ns;

		atomic64_add(map_100ns, &map->sum_map_100ns);
		atomic64_add(unmap_100ns, &map->sum_unmap_100ns);
		atomic64_add(map_sq, &map->sum_sq_map);
		atomic64_add(unmap_sq, &map->sum_sq_unmap);
		atomic64_inc(&map->loops);

		/*
		 * We may test for a long time so periodically check whether
		 * we need to schedule to avoid starving the others. Otherwise
		 * we may hangup the kernel in a non-preemptible kernel when
		 * the test kthreads number >= CPU number, the test kthreads
		 * will run endless on every CPU since the thread resposible
		 * for notifying the kthread stop (in do_map_benchmark())
		 * could not be scheduled.
		 *
		 * Note this may degrade the test concurrency since the test
		 * threads may need to share the CPU time with other load
		 * in the system. So it's recommended to run this benchmark
		 * on an idle system.
		 */
		/*
		 * 长时间测试必须周期性主动让出 CPU；否则在非抢占内核且线程数不少于 CPU 数时，每个 CPU
		 * 都可能被测试线程占满，负责在 do_map_benchmark() 中发停止通知的线程反而永远得不到调度。
		 * 让出会引入其他负载干扰并降低并发度，所以应在空闲系统上运行以减少统计噪声。
		 */
		cond_resched();
	}

out:
	mb_ops->unprepare(mparam);
	return ret;
}

/*
 * 创建、运行并回收一次多线程 DMA map benchmark。
 *
 * @map: 已通过 ioctl 校验的共享会话；函数回填统计结果。
 * 返回值: 成功为 0；数组/线程/工作线程失败时返回对应负错误码。
 *
 * 先分配 task 指针数组并持有设备引用，再创建所有“尚未唤醒”的 kthread；创建中途失败会 stop 已创建
 * 线程并统一清理。全部创建后清零统计，为每个 task 增加一份引用再唤醒。限时结束后
 * kthread_stop_put() 同时等待退出并消费该引用，形成明确 join 边界；之后才读取原子统计并释放设备。
 * 函数使用 GFP_KERNEL、线程创建和 msleep，可睡眠。单个 map 错误最终传播，但仍会停止其余线程。
 */
static int do_map_benchmark(struct map_benchmark_data *map)
{
	struct task_struct **tsk;
	int threads = map->bparam.threads;
	int node = map->bparam.node;
	u64 loops;
	int ret = 0;
	int i;

	tsk = kmalloc_objs(*tsk, threads);
	if (!tsk)
		return -ENOMEM;

	get_device(map->dev);

	for (i = 0; i < threads; i++) {
		tsk[i] = kthread_create_on_node(map_benchmark_thread, map,
				map->bparam.node, "dma-map-benchmark/%d", i);
		if (IS_ERR(tsk[i])) {
			pr_err("create dma_map thread failed\n");
			ret = PTR_ERR(tsk[i]);
			while (--i >= 0)
				kthread_stop(tsk[i]);
			goto out;
		}

		if (node != NUMA_NO_NODE)
			kthread_bind_mask(tsk[i], cpumask_of_node(node));
	}

	/* clear the old value in the previous benchmark */
	/* 所有 worker 仍处于未唤醒状态，此处可无竞争地清除上一轮聚合统计。 */
	atomic64_set(&map->sum_map_100ns, 0);
	atomic64_set(&map->sum_unmap_100ns, 0);
	atomic64_set(&map->sum_sq_map, 0);
	atomic64_set(&map->sum_sq_unmap, 0);
	atomic64_set(&map->loops, 0);

	for (i = 0; i < threads; i++) {
		get_task_struct(tsk[i]);
		wake_up_process(tsk[i]);
	}

	msleep_interruptible(map->bparam.seconds * 1000);

	/* wait for the completion of all started benchmark threads */
	/* 逐条发送停止请求、等待退出并释放主控方额外 task 引用，完成全部 worker 的 join。 */
	for (i = 0; i < threads; i++) {
		int kthread_ret = kthread_stop_put(tsk[i]);

		if (kthread_ret)
			ret = kthread_ret;
	}

	if (ret)
		goto out;

	loops = atomic64_read(&map->loops);
	if (likely(loops > 0)) {
		u64 map_variance, unmap_variance;
		u64 sum_map = atomic64_read(&map->sum_map_100ns);
		u64 sum_unmap = atomic64_read(&map->sum_unmap_100ns);
		u64 sum_sq_map = atomic64_read(&map->sum_sq_map);
		u64 sum_sq_unmap = atomic64_read(&map->sum_sq_unmap);

		/* average latency */
		/* 以完整样本数为共同分母计算 map/unmap 的 100 ns 整数均值。 */
		map->bparam.avg_map_100ns = div64_u64(sum_map, loops);
		map->bparam.avg_unmap_100ns = div64_u64(sum_unmap, loops);

		/* standard deviation of latency */
		/*
		 * 使用 sqrt(E[x^2] - E[x]^2) 计算总体标准差，结果同样以 100 ns 为单位；该整数公式
		 * 依赖累加值及平方和未发生 u64 回绕，当前实现不另做溢出检测。
		 */
		map_variance = div64_u64(sum_sq_map, loops) -
				map->bparam.avg_map_100ns *
				map->bparam.avg_map_100ns;
		unmap_variance = div64_u64(sum_sq_unmap, loops) -
				map->bparam.avg_unmap_100ns *
				map->bparam.avg_unmap_100ns;
		map->bparam.map_stddev = int_sqrt64(map_variance);
		map->bparam.unmap_stddev = int_sqrt64(unmap_variance);
	}

out:
	put_device(map->dev);
	kfree(tsk);
	return ret;
}

/*
 * 处理 debugfs 的 DMA_MAP_BENCHMARK ioctl。
 *
 * @file: simple_open() 打开的 debugfs 文件，private_data 指向设备共享 map 对象。
 * @cmd: 仅接受 DMA_MAP_BENCHMARK。
 * @arg: 用户态 struct map_benchmark 指针，同时承载输入配置与输出统计。
 * 返回值: 成功为 0；复制失败为 -EFAULT，参数/命令/mask 不合法为 -EINVAL，benchmark 错误原样返回。
 *
 * 函数先复制并逐项验证模式、线程、时长、延时、NUMA、granule 和方向，再临时改写设备 DMA mask。
 * 无论 benchmark 成败都会恢复旧 mask，之后才把结果复制回用户。整个过程可睡眠且持续数秒；代码
 * 没有会话 mutex，因此同一 debugfs 对象的并发 ioctl、或原驱动同时修改 DMA mask，必须由使用方
 * 避免。当前实现也未单独限制 dma_bits，调用者需遵守 DMA_BIT_MASK() 的有效位宽契约。
 */
static long map_benchmark_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct map_benchmark_data *map = file->private_data;
	void __user *argp = (void __user *)arg;
	u64 old_dma_mask;
	int ret;

	if (copy_from_user(&map->bparam, argp, sizeof(map->bparam)))
		return -EFAULT;

	switch (cmd) {
	case DMA_MAP_BENCHMARK:
		if (map->bparam.map_mode < 0 ||
		    map->bparam.map_mode >= DMA_MAP_BENCH_MODE_MAX) {
			pr_err("invalid map mode\n");
			return -EINVAL;
		}

		if (map->bparam.threads == 0 ||
		    map->bparam.threads > DMA_MAP_MAX_THREADS) {
			pr_err("invalid thread number\n");
			return -EINVAL;
		}

		if (map->bparam.seconds == 0 ||
		    map->bparam.seconds > DMA_MAP_MAX_SECONDS) {
			pr_err("invalid duration seconds\n");
			return -EINVAL;
		}

		if (map->bparam.dma_trans_ns > DMA_MAP_MAX_TRANS_DELAY) {
			pr_err("invalid transmission delay\n");
			return -EINVAL;
		}

		if (map->bparam.node != NUMA_NO_NODE &&
		    (map->bparam.node < 0 || map->bparam.node >= MAX_NUMNODES ||
		     !node_possible(map->bparam.node))) {
			pr_err("invalid numa node\n");
			return -EINVAL;
		}

		if (map->bparam.granule < 1 || map->bparam.granule > 1024) {
			pr_err("invalid granule size\n");
			return -EINVAL;
		}

		switch (map->bparam.dma_dir) {
		case DMA_MAP_BIDIRECTIONAL:
			map->dir = DMA_BIDIRECTIONAL;
			break;
		case DMA_MAP_FROM_DEVICE:
			map->dir = DMA_FROM_DEVICE;
			break;
		case DMA_MAP_TO_DEVICE:
			map->dir = DMA_TO_DEVICE;
			break;
		default:
			pr_err("invalid DMA direction\n");
			return -EINVAL;
		}

		old_dma_mask = dma_get_mask(map->dev);

		ret = dma_set_mask(map->dev,
				   DMA_BIT_MASK(map->bparam.dma_bits));
		if (ret) {
			pr_err("failed to set dma_mask on device %s\n",
				dev_name(map->dev));
			return -EINVAL;
		}

		ret = do_map_benchmark(map);

		/*
		 * restore the original dma_mask as many devices' dma_mask are
		 * set by architectures, acpi, busses. When we bind them back
		 * to their original drivers, those drivers shouldn't see
		 * dma_mask changed by benchmark
		 */
		/*
		 * 设备原 DMA mask 可能由体系结构、ACPI 或总线设置；测试结束必须恢复，避免设备重新绑定原驱动
		 * 后观察到 benchmark 遗留的寻址能力。恢复调用的返回值沿用原实现未另行覆盖测试结果。
		 */
		dma_set_mask(map->dev, old_dma_mask);

		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user(argp, &map->bparam, sizeof(map->bparam)))
		return -EFAULT;

	return ret;
}

/*
 * debugfs 文件操作表只提供 simple_open 与 ioctl；open 会把 inode 私有 map 指针转存到 file，
 * 没有 read/write/mmap 数据面。全局常量表随模块存在，打开文件由 debugfs/module 生命周期约束。
 */
static const struct file_operations map_benchmark_fops = {
	.open			= simple_open,
	.unlocked_ioctl		= map_benchmark_ioctl,
};

/*
 * 设备 devres 回收时删除其 benchmark debugfs 文件。
 *
 * @data: probe 期注册的 struct map_benchmark_data。
 *
 * debugfs_remove() 会使新打开失效，并按 debugfs 语义处理现有引用；回调无返回值，可安全接收尚未
 * 成功创建文件时的空 dentry。它由 devm action 串行触发，不释放 devm 管理的 map 本身。
 */
static void map_benchmark_remove_debugfs(void *data)
{
	struct map_benchmark_data *map = (struct map_benchmark_data *)data;

	debugfs_remove(map->debugfs);
}

/*
 * 为一个 platform/PCI 设备建立共享 benchmark 状态和 debugfs 控制入口。
 *
 * @dev: 当前绑定设备，devm 资源及后续 DMA 操作均以它为 owner。
 * 返回值: 成功为 0；状态分配、devm action 或 debugfs 创建失败返回对应错误。
 *
 * 先用 devm 分配 map 并注册 debugfs 删除 action，再在 debugfs 根目录创建 0600 的固定文件名。
 * 固定名称有意限制全系统同一时间只成功绑定一个设备；第二次 probe 会因名称冲突失败。probe 在
 * 可睡眠的设备绑定上下文运行，发布 debugfs 前对象已完整初始化。
 */
static int __map_benchmark_probe(struct device *dev)
{
	struct dentry *entry;
	struct map_benchmark_data *map;
	int ret;

	map = devm_kzalloc(dev, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;
	map->dev = dev;

	ret = devm_add_action(dev, map_benchmark_remove_debugfs, map);
	if (ret) {
		pr_err("Can't add debugfs remove action\n");
		return ret;
	}

	/*
	 * we only permit a device bound with this driver, 2nd probe
	 * will fail
	 */
	/* 固定根目录文件名只允许一个设备绑定本驱动；第二个设备无法创建同名入口，因此 probe 失败。 */
	entry = debugfs_create_file("dma_map_benchmark", 0600, NULL, map,
			&map_benchmark_fops);
	if (IS_ERR(entry))
		return PTR_ERR(entry);
	map->debugfs = entry;

	return 0;
}

/*
 * platform bus 的薄 probe 适配器。
 *
 * @pdev: 正在绑定的 platform_device。
 * 返回值: 通用 probe 的结果。
 *
 * 仅把内嵌 device 交给共享初始化，不取得额外引用；设备核心串行管理 probe/remove 生命周期。
 */
static int map_benchmark_platform_probe(struct platform_device *pdev)
{
	return __map_benchmark_probe(&pdev->dev);
}

/* platform 驱动只提供名称匹配和 probe；资源清理由 devres 完成，无需显式 remove。 */
static struct platform_driver map_benchmark_platform_driver = {
	.driver		= {
		.name	= "dma_map_benchmark",
	},
	.probe = map_benchmark_platform_probe,
};

/*
 * PCI bus 的薄 probe 适配器。
 *
 * @pdev: 正在绑定的 PCI 设备。
 * @id: 匹配项；共享初始化不需要其 driver_data。
 * 返回值: 通用 probe 的结果。
 *
 * 函数在 PCI 设备绑定上下文可睡眠执行；设备引用与解绑清理由 PCI core/devres 负责。
 */
static int
map_benchmark_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	return __map_benchmark_probe(&pdev->dev);
}

/* PCI 驱动保存模块名和 probe，可通过驱动绑定机制选择设备；没有私有 remove，依赖 devres action。 */
static struct pci_driver map_benchmark_pci_driver = {
	.name	= "dma_map_benchmark",
	.probe	= map_benchmark_pci_probe,
};

/*
 * 注册模块的 PCI 与 platform 两类驱动。
 *
 * 返回值: 两者都注册成功为 0；PCI 注册失败直接返回，platform 失败则先注销 PCI 再返回错误。
 *
 * 注册顺序与失败回滚、模块退出时的逆序注销配对。函数在模块初始化进程上下文执行，可能睡眠；
 * 返回成功后总线核心拥有驱动注册关系，实际设备 probe 可随即发生。
 */
static int __init map_benchmark_init(void)
{
	int ret;

	ret = pci_register_driver(&map_benchmark_pci_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&map_benchmark_platform_driver);
	if (ret) {
		pci_unregister_driver(&map_benchmark_pci_driver);
		return ret;
	}

	return 0;
}

/*
 * 注销 benchmark 的两类总线驱动并触发所有绑定设备的 devres 清理。
 *
 * 先注销后注册的 platform 驱动，再注销 PCI 驱动；总线核心等待相关解绑完成，debugfs action 会在
 * map 对象销毁前删除入口。函数仅在模块退出进程上下文调用，返回后不得再有新 ioctl 进入本模块。
 */
static void __exit map_benchmark_cleanup(void)
{
	platform_driver_unregister(&map_benchmark_platform_driver);
	pci_unregister_driver(&map_benchmark_pci_driver);
}

/* 模块装载/卸载宏把上述注册与逆序注销函数接入模块生命周期。 */
module_init(map_benchmark_init);
module_exit(map_benchmark_cleanup);

/* 作者和用途元数据仅用于模块信息展示，不参与设备匹配或资源管理。 */
MODULE_AUTHOR("Barry Song <song.bao.hua@hisilicon.com>");
MODULE_DESCRIPTION("dma_map benchmark driver");
