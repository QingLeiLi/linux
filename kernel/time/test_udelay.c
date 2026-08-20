// SPDX-License-Identifier: GPL-2.0
/*
 * udelay() test kernel module
 *
 * Test is executed by writing and reading to /sys/kernel/debug/udelay_test
 * Tests are configured by writing: USECS ITERATIONS
 * Tests are executed by reading from the same file.
 * Specifying usecs of 0 or negative values will run multiples tests.
 *
 * Copyright (C) 2014 Google, Inc.
 */
/*
 * 本模块通过 /sys/kernel/debug/udelay_test 暴露一次可配置的忙等待测量：写入“微秒数 [迭代次数]”
 * 保存配置，读取同一文件时才真正执行测试；写入 0 会显示用法，负数或非正迭代次数不会运行测试。
 * 原说明中的“0 或负值运行多组测试”与当前实现不一致：当前代码没有多组扫描分支，负值读取为空。
 * 版权行只标识来源，不参与上述接口语义。
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#define DEFAULT_ITERATIONS 100

#define DEBUGFS_FILENAME "udelay_test"
/* 默认执行 100 次；debugfs 文件名同时用于创建、提示文本和卸载时按名查找，三处必须保持一致。 */

static DEFINE_MUTEX(udelay_test_lock);
static int udelay_test_usecs;
static int udelay_test_iterations = DEFAULT_ITERATIONS;
/*
 * udelay_test_lock 串行化配置的成对读写，使 usecs/iterations 始终来自同一次提交；读路径取得快照后
 * 立即解锁，漫长的忙等待不阻塞下一次配置。两个整数为模块静态状态：usecs 缺省 0 表示显示用法，
 * iterations 缺省 100；写接口不限制负值和过大值，show 分支负责决定是否执行，但不做上界防护。
 */

/*
 * udelay_test_single() - 测量固定微秒忙等待的最小、平均、最大耗时和过早返回次数。
 *
 * 【调用位置】udelay_test_show() 在配置 usecs>0 且 iters>0 时调用；本函数把一行统计写进 seq_file，
 * seq_read 随后复制给用户。本函数是真正执行 udelay() 的状态阶段，不修改全局配置。
 *
 * 【参数与 ownership】@s 是 single_open() 创建并由 seq_file 层持有的非 NULL 输出对象借用指针；
 * @usecs 是每轮请求的正微秒数；@iters 是正的无符号迭代数。函数不保存参数、不取得引用。调用者已
 * 释放 udelay_test_lock，所以测量期间无本模块锁；忙等待不主动睡眠，但会长时间占用当前 CPU。
 *
 * 【返回与副作用】固定返回 0，表示 show 记录可交付；通过 s 追加一行结果，若有过早样本再追加
 * FAIL 计数。无 errno 和回滚路径。seq_file 空间不足时会在外层扩容后重放 show；当前单行很短，
 * 正常不会重放，但该接口不承诺一次 read 只执行一次测量。
 *
 * 【变量地图】min/max 是纳秒样本边界，首次迭代负责初始化；fail_count 统计低于容差下界的样本；
 * sum/avg 用 64 位纳秒累计并由 do_div 原地求均值；i 是迭代序号；allowed_error_ns 是请求耗时的
 * 0.5%；kt1/kt2 是每轮单调时钟读值，time_passed 是二者的纳秒差。
 *
 * 【输入边界】show 只保证两个参数为正，不限制大小。过大的 usecs 可能触发架构 udelay 的溢出风险，
 * usecs*5、usecs*1000 和 int 型 time_passed 也可能溢出；因此该调试工具只适合短微秒延迟，不能把
 * 任意正整数都视为可靠输入。iters>0 保证 do_div() 的除数非零。
 */
static int udelay_test_single(struct seq_file *s, int usecs, uint32_t iters)
{
	int min = 0, max = 0, fail_count = 0;
	uint64_t sum = 0;
	uint64_t avg;
	int i;
	/* Allow udelay to be up to 0.5% fast */
	/* 允许 udelay 最多快 0.5%；请求 usecs*1000 纳秒的容差因此是 usecs*5 纳秒。 */
	int allowed_error_ns = usecs * 5;

	/* 阶段 1：逐次用单调 ktime 包围忙等待；调度、IRQ 和虚拟化停顿只会把观测值拉长。 */
	for (i = 0; i < iters; ++i) {
		s64 kt1, kt2;
		int time_passed;

		kt1 = ktime_get_ns();
		udelay(usecs);
		kt2 = ktime_get_ns();
		time_passed = kt2 - kt1;

		/* 阶段 2：首样本建立 min/max，后续样本只在越过当前边界时更新。 */
		if (i == 0 || time_passed < min)
			min = time_passed;
		if (i == 0 || time_passed > max)
			max = time_passed;
		/* 先加纳秒容差再换成微秒；仍小于请求值才判定为真正的过早返回。 */
		if ((time_passed + allowed_error_ns) / 1000 < usecs)
			++fail_count;
		/* 单调时钟正常不后退；告警保留异常证据，但测试仍把该样本计入无符号 sum。 */
		WARN_ON(time_passed < 0);
		sum += time_passed;
	}

	/* 阶段 3：do_div() 以 iters 原地除 avg；调用链已保证 iters>0，不存在除零失败。 */
	avg = sum;
	do_div(avg, iters);
	/* 阶段 4：先输出统一统计；只有 fail_count 非零时才追加 FAIL 字段，最后以换行结束记录。 */
	seq_printf(s, "%d usecs x %d: exp=%d allowed=%d min=%d avg=%lld max=%d",
			usecs, iters, usecs * 1000,
			(usecs * 1000) - allowed_error_ns, min, avg, max);
	if (fail_count)
		seq_printf(s, " FAIL=%d", fail_count);
	seq_puts(s, "\n");

	return 0;
}

/*
 * udelay_test_show() - 为一次 debugfs 读取选择“执行测试、显示用法或空输出”路径。
 *
 * 【调用位置】single_open() 把它安装为单记录 seq_file 的 show 回调；seq_read() 可因首次生成、seek
 * 或缓冲扩容调用它。返回后 seq_file 决定如何缓存并复制本次记录。
 *
 * 【参数与 ownership】@s 是 seq_file 输出对象借用指针；@v 是 single iterator 传入的记录令牌，
 * 本回调不读取它。两者均不保存、不释放。函数在可睡眠的 debugfs 读上下文运行，入口无本模块锁。
 *
 * 【返回与副作用】有效正配置时透传 udelay_test_single() 的 0；usecs==0 时输出校准信息和用法；
 * usecs<0 或 iters<=0 且 usecs!=0 时输出空记录并返回 0。它只读取全局配置并写 s，不修改墙钟。
 *
 * 【变量地图】usecs/iters 是 mutex 下取得的同一配置快照，解锁后不受并发 write 影响；ret 当前恒为
 * 0，保留为非执行分支的统一返回值；ts 仅在用法分支保存单调时钟秒/纳秒快照。
 */
static int udelay_test_show(struct seq_file *s, void *v)
{
	int usecs;
	int iters;
	int ret = 0;

	/* 阶段 1：锁保护两个配置字段的一致快照；测量前解锁，避免长时间阻塞 writer。 */
	mutex_lock(&udelay_test_lock);
	usecs = udelay_test_usecs;
	iters = udelay_test_iterations;
	mutex_unlock(&udelay_test_lock);

	/* 阶段 2a：只有两个值都为正才运行忙等待，single 的返回语义直接交给 seq_file。 */
	if (usecs > 0 && iters > 0) {
		return udelay_test_single(s, usecs, iters);
	} else if (usecs == 0) {
		struct timespec64 ts;

		/* 阶段 2b：0 是用法模式；lpj 展示延迟循环校准值，kt 展示读取时的单调时间快照。 */
		ktime_get_ts64(&ts);
		seq_printf(s, "udelay() test (lpj=%ld kt=%lld.%09ld)\n",
				loops_per_jiffy, (s64)ts.tv_sec, ts.tv_nsec);
		seq_puts(s, "usage:\n");
		seq_puts(s, "echo USECS [ITERS] > " DEBUGFS_FILENAME "\n");
		seq_puts(s, "cat " DEBUGFS_FILENAME "\n");
	}

	/* 负 usecs 或不合法 iters 落到这里并产生空记录；当前实现没有把它们报告为 errno。 */
	return ret;
}

/*
 * udelay_test_open() - 把一次 debugfs open 转换成单记录 seq_file 会话。
 *
 * 【调用位置】debugfs 的 full proxy 在取得活动访问保护和 THIS_MODULE 引用后调用本函数；成功后
 * file_operations.read/llseek/release 分别由 seq_read/seq_lseek/single_release 接管。
 *
 * 【参数与 ownership】@inode 是 debugfs inode 借用指针，i_private 来自 create 时的 data（本模块
 * 传 NULL）；@file 是 VFS 正在构造的打开文件借用指针。single_open() 成功会把新分配的 seq_file
 * 和动态 seq_operations 所有权绑定到 file，最终必须由 single_release() 释放。
 *
 * 【上下文与返回】运行于可睡眠的 open 路径，不持 udelay_test_lock。返回 0 表示会话建立完成；
 * 分配或 seq_open 失败时返回 -ENOMEM/相应 errno，single_open 已回滚临时 ops，file 没有可用会话。
 */
static int udelay_test_open(struct inode *inode, struct file *file)
{
	return single_open(file, udelay_test_show, inode->i_private);
}

/*
 * udelay_test_write() - 解析一次用户写入并原子提交下一次读取使用的测试配置。
 *
 * 【调用位置】debugfs full proxy 在确认文件尚未移除后调用；成功返回后，后续 show 在 mutex 下复制
 * 新配置。当前打开文件的 seq_file 读缓存不一定因此失效，可靠用法是写完再重新读取/打开文件。
 *
 * 【参数与 ownership】@file 是本次打开文件借用指针，当前实现不使用；@buf 是用户地址空间的纯输入
 * 指针，仅在 count 指定范围内借用；@count 是待复制字节数，必须小于 32；@pos 是 VFS 文件位置的
 * 输入输出指针，但本实现不读取也不推进它。所有指针均不保存、不释放。
 *
 * 【上下文与返回】可因用户拷贝和 mutex 睡眠，入口不持本模块锁。成功完整消费 count 字节并返回
 * count；过长或没有整数返回 -EINVAL，用户页不可读返回 -EFAULT。失败发生在提交锁之前，全局配置
 * 保持原值；成功时两个字段在同一临界区发布，不会出现新 usecs 搭配旧 iters 的撕裂快照。
 *
 * 【变量地图】lbuf 是带额外 NUL 结尾的 32 字节内核栈缓冲；ret 是 sscanf 成功转换的字段数；
 * usecs/iters 是尚未发布的候选值。只给一个整数时 iters 补为 DEFAULT_ITERATIONS；负值、零、额外
 * 尾随文本和过大正值都未在这里拒绝，读取分支或底层 delay 决定其后果。
 */
static ssize_t udelay_test_write(struct file *file, const char __user *buf,
		size_t count, loff_t *pos)
{
	char lbuf[32];
	int ret;
	int usecs;
	int iters;

	/* 阶段 1：必须为末尾 NUL 留一字节；count==32 也拒绝，任何失败都未触碰全局配置。 */
	if (count >= sizeof(lbuf))
		return -EINVAL;

	/* 用户拷贝可能 fault；成功后显式终止字符串，sscanf 才不会越过本次输入。 */
	if (copy_from_user(lbuf, buf, count))
		return -EFAULT;
	lbuf[count] = '\0';

	/* 阶段 2：至少需要 usecs；第二个整数可省略并恢复默认迭代数。多余内容不会参与配置。 */
	ret = sscanf(lbuf, "%d %d", &usecs, &iters);
	if (ret < 1)
		return -EINVAL;
	else if (ret < 2)
		iters = DEFAULT_ITERATIONS;

	/* 阶段 3（提交点）：锁内同时替换两个字段，与 show 的成对快照读取形成互斥协议。 */
	mutex_lock(&udelay_test_lock);
	udelay_test_usecs = usecs;
	udelay_test_iterations = iters;
	mutex_unlock(&udelay_test_lock);

	return count;
}

/*
 * 静态 file_operations 描述 debugfs 文件的完整生命周期：owner 使已打开文件持有模块引用；open 建立
 * single seq_file；read/llseek 使用通用 seq_file 状态机；write 提交配置；release 释放 open 分配的
 * seq_file 和动态 ops。表在模块存续期只读，由 debugfs inode 借用，不需要单独销毁。
 */
static const struct file_operations udelay_test_debugfs_ops = {
	.owner = THIS_MODULE,
	.open = udelay_test_open,
	.read = seq_read,
	.write = udelay_test_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/*
 * udelay_test_init() - 在 debugfs 根目录发布 udelay_test 文件并允许模块继续加载。
 *
 * 【调用位置】module_init 在模块加载（或内建 initcall）阶段调用；成功返回后用户可通过 fops 打开、
 * 读取并在权限允许时写入该文件，卸载阶段由 udelay_test_exit() 按名称摘除。
 *
 * 【参数与上下文】入参：无。运行在可睡眠的初始化上下文，入口不持锁；函数取得配置 mutex，使文件
 * 一旦对并发 open 可见，初始配置已稳定。debugfs_create_file() 可能分配并睡眠。
 *
 * 【返回与副作用】固定返回 0。create 以根目录、S_IRUSR、NULL 私有数据和本文件 fops 发布 dentry；
 * 它返回 dentry 或错误指针，但本函数按 debugfs 的可选诊断接口惯例忽略失败，所以 DEBUG_FS 关闭、
 * 分配失败或重名时模块仍报告加载成功，只是接口可能不存在。返回 dentry 未保存，退出时改为按名查找。
 *
 * 【权限边界】模式只设置 owner-read，没有 owner-write；普通 DAC 检查会拒绝写入，具备权限绕过能力
 * 的管理者仍可写。该现状与文件头“通过写入配置”的说明存在权限门槛，本函数不修正它。
 */
static int __init udelay_test_init(void)
{
	/* 发布期间持锁，使发布后立即到达的 show/write 只能观察完整的初始状态。 */
	mutex_lock(&udelay_test_lock);
	debugfs_create_file(DEBUGFS_FILENAME, S_IRUSR, NULL, NULL,
			    &udelay_test_debugfs_ops);
	mutex_unlock(&udelay_test_lock);

	return 0;
}

/* 注册 init 入口；__init 代码在成功初始化后可由内核回收，静态 fops 与配置仍常驻。 */
module_init(udelay_test_init);

/*
 * udelay_test_exit() - 摘除 debugfs 入口，结束模块对新用户操作的可见性。
 *
 * 【调用位置】module_exit 在可卸载模块退出时调用；模块 owner 引用阻止仍有打开文件时进入正常卸载，
 * debugfs 移除层本身也会等待已开始的代理操作退出，因此返回后可安全释放模块静态代码和数据。
 *
 * 【参数与上下文】入参：无。运行在可睡眠的模块退出上下文，入口不持锁；取得同一配置 mutex，与
 * show/write 的快照或提交串行。正常模块引用规则下不存在持锁等待活动 open 的循环依赖。
 *
 * 【返回与副作用】无直接返回值。按 DEBUGFS_FILENAME 在根目录查找、递归摘除并释放查找引用；文件
 * 从未创建或已经不存在时无操作。函数不持有保存的 dentry，也没有失败资源需要调用者回滚。
 */
static void __exit udelay_test_exit(void)
{
	/* 摘除点与配置操作互斥；helper 内部处理 lookup 得到的 dentry 引用并等待活动代理访问结束。 */
	mutex_lock(&udelay_test_lock);
	debugfs_lookup_and_remove(DEBUGFS_FILENAME, NULL);
	mutex_unlock(&udelay_test_lock);
}

/* 注册仅供可卸载模块使用的退出入口；内建场景不会调用它。 */
module_exit(udelay_test_exit);

/* 模块元数据描述用途、作者和 GPL 许可证，不改变 debugfs 或测量控制流。 */
MODULE_DESCRIPTION("udelay test module");
MODULE_AUTHOR("David Riley <davidriley@chromium.org>");
MODULE_LICENSE("GPL");
