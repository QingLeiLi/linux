// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Module-based API test facility for ww_mutexes
 *
 * 基于模块的 ww_mutex API 测试设施。
 *
 * 本文件不只检查单把锁是否互斥，还会主动构造 ABBA 和多节点等待环，验证调用者收到
 * -EDEADLK 后“释放已持有锁、慢速取得争用锁、再重试其余锁”的完整恢复协议。
 */

#include <linux/kernel.h>

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/prandom.h>
#include <linux/slab.h>
#include <linux/ww_mutex.h>

/* Wait-Die 测试类：较新的事务在冲突时主动退出，避免形成等待环。 */
static DEFINE_WD_CLASS(wd_class);
/* Wound-Wait 测试类：较老事务可 wound 较新持有者，促使后者退出。 */
static DEFINE_WW_CLASS(ww_class);
/* 专用无绑定工作队列承载并发测试参与者，由模块初始化创建、退出时销毁。 */
struct workqueue_struct *wq;

#ifdef CONFIG_DEBUG_WW_MUTEX_SLOWPATH
/* 初始化获取上下文，但关闭调试配置下的人工死锁注入，保证定向测试只观察真实依赖。 */
#define ww_acquire_init_noinject(a, b) do { \
		ww_acquire_init((a), (b)); \
		(a)->deadlock_inject_countdown = ~0U; \
	} while (0)
#else
/* 非注入构建无需改写倒计时，直接使用标准初始化。 */
#define ww_acquire_init_noinject(a, b) ww_acquire_init((a), (b))
#endif

/* 一次基础互斥测试的共享状态；work 是竞争者，三个 completion 编排其开始和结束。 */
struct test_mutex {
	struct work_struct work;
	struct ww_mutex mutex;
	struct completion ready, go, done;
	unsigned int flags;
};

/* 主线程持锁期间主动轮询，验证工作线程不能提前完成。 */
#define TEST_MTX_SPIN BIT(0)
/* 工作线程使用 trylock 自旋而不是阻塞式 lock。 */
#define TEST_MTX_TRY BIT(1)
/* 主线程的锁操作携带 ww_acquire_ctx。 */
#define TEST_MTX_CTX BIT(2)
/* 三个独立标志的组合上界，因此测试覆盖 [0, 7] 共八种情形。 */
#define __TEST_MTX_LAST BIT(3)

/*
 * 基础互斥测试的工作队列竞争者。
 * 输入 work 必须嵌入仍然存活的 test_mutex；调用者先等待 ready，再持锁并触发 go。
 * 本函数在 go 后以 trylock 循环或普通 lock 取得 mutex，发布 done 后才解锁；因此主线程若在
 * 自己仍持锁时看到 done，就证明互斥性失效。函数无返回值，完成通知和最终解锁是其后置条件。
 */
static void test_mutex_work(struct work_struct *work)
{
	/* 从 work 成员恢复本轮测试对象；其栈生命周期由 flush_work 保证覆盖本函数。 */
	struct test_mutex *mtx = container_of(work, typeof(*mtx), work);

	/* 阶段一：声明竞争者已经启动，等待主线程布置好“持锁后竞争”的场景。 */
	complete(&mtx->ready);
	wait_for_completion(&mtx->go);

	/* 阶段二：按标志选择忙等 trylock 或睡眠式 lock，两路都必须真正拥有锁才可继续。 */
	if (mtx->flags & TEST_MTX_TRY) {
		while (!ww_mutex_trylock(&mtx->mutex, NULL))
			cond_resched();
	} else {
		ww_mutex_lock(&mtx->mutex, NULL);
	}
	/* 阶段三：在持锁区内发布完成，再释放 ownership 供主线程收尾。 */
	complete(&mtx->done);
	ww_mutex_unlock(&mtx->mutex);
}

/*
 * 执行一组 flags 指定的单锁互斥性测试。
 * class 选择 Wait-Die 或 Wound-Wait；flags 可组合轮询、trylock 和 acquire_ctx 三种维度。
 * 返回 0 表示竞争者在主线程解锁前始终未进入临界区；返回 -EINVAL 表示过早完成或超时语义异常。
 * 无论结果如何都会 flush/destroy 栈上 work，并在启用 ctx 时成对 fini。
 */
static int __test_mutex(struct ww_class *class, unsigned int flags)
{
/* 给竞争者一个短但可调度的观察窗口；定义局限在本函数并在末尾撤销。 */
#define TIMEOUT (HZ / 16)
	/* 栈上对象同时保存锁、工作项与三阶段同步点。 */
	struct test_mutex mtx;
	/* 仅 TEST_MTX_CTX 路径初始化和传入，生命周期覆盖本轮持锁区。 */
	struct ww_acquire_ctx ctx;
	/* 轮询路径保存错误标志，等待路径接收剩余 jiffies；非零最终都表示测试失败。 */
	int ret;

	/* 阶段一：初始化锁、可选上下文、栈上工作项和所有 completion。 */
	ww_mutex_init(&mtx.mutex, class);
	if (flags & TEST_MTX_CTX)
		ww_acquire_init(&ctx, class);

	INIT_WORK_ONSTACK(&mtx.work, test_mutex_work);
	init_completion(&mtx.ready);
	init_completion(&mtx.go);
	init_completion(&mtx.done);
	mtx.flags = flags;

	/* 阶段二：启动竞争者并等其就绪，消除“尚未开始执行”造成的假通过。 */
	queue_work(wq, &mtx.work);

	wait_for_completion(&mtx.ready);
	/* 阶段三：主线程先取得锁，再放行竞争者；此后 done 在解锁前必须保持未完成。 */
	ww_mutex_lock(&mtx.mutex, (flags & TEST_MTX_CTX) ? &ctx : NULL);
	complete(&mtx.go);
	if (flags & TEST_MTX_SPIN) {
		unsigned long timeout = jiffies + TIMEOUT;

		ret = 0;
		do {
			if (completion_done(&mtx.done)) {
				ret = -EINVAL;
				break;
			}
			cond_resched();
		} while (time_before(jiffies, timeout));
	} else {
		ret = wait_for_completion_timeout(&mtx.done, TIMEOUT);
	}
	/* 阶段四：观察窗口结束后释放锁和上下文，让竞争者取得锁并正常退出。 */
	ww_mutex_unlock(&mtx.mutex);
	if (flags & TEST_MTX_CTX)
		ww_acquire_fini(&ctx);

	if (ret) {
		pr_err("%s(flags=%x): mutual exclusion failure\n",
		       __func__, flags);
		ret = -EINVAL;
	}

	/* 阶段五：等待工作项真正结束，随后才销毁其栈上调试状态并返回统一结果。 */
	flush_work(&mtx.work);
	destroy_work_on_stack(&mtx.work);
	return ret;
#undef TIMEOUT
}

/*
 * 穷举基础互斥测试的八种标志组合。
 * class 决定 ww 死锁策略；每种组合依次执行，首次失败立即返回，否则返回 0。
 * 串行执行使每轮栈上 work 和 acquire_ctx 都在进入下一轮前完成回收。
 */
static int test_mutex(struct ww_class *class)
{
	/* ret 传递首个失败，i 是 TEST_MTX 三位组合编号。 */
	int ret;
	int i;

	/* [0, __TEST_MTX_LAST) 正好覆盖三个位的笛卡尔组合。 */
	for (i = 0; i < __TEST_MTX_LAST; i++) {
		ret = __test_mutex(class, i);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * 验证同一 acquire_ctx 对同一 ww_mutex 的递归获取会被可靠拒绝。
 * trylock 选择首次获取方式；随后分别用无 ctx trylock、有 ctx trylock 和阻塞式 lock 重入。
 * 前两次 trylock 必须失败，阻塞式重入必须返回 -EALREADY；任一偏差转换为 -EINVAL。
 * 退出前总会释放初始 ownership 并 fini ctx，避免测试自身泄漏状态。
 */
static int test_aa(struct ww_class *class, bool trylock)
{
	/* mutex 是被重复获取的唯一对象，ctx 标识同一事务。 */
	struct ww_mutex mutex;
	struct ww_acquire_ctx ctx;
	/* ret 既承接锁 API 结果也承接最终测试结论，from 仅用于诊断首次获取方式。 */
	int ret;
	const char *from = trylock ? "trylock" : "lock";

	/* 阶段一：用指定方式建立初始 ownership。 */
	ww_mutex_init(&mutex, class);
	ww_acquire_init(&ctx, class);

	if (!trylock) {
		ret = ww_mutex_lock(&mutex, &ctx);
		if (ret) {
			pr_err("%s: initial lock failed!\n", __func__);
			goto out;
		}
	} else {
		ret = !ww_mutex_trylock(&mutex, &ctx);
		if (ret) {
			pr_err("%s: initial trylock failed!\n", __func__);
			goto out;
		}
	}

	/* 阶段二：无 ctx 的 trylock 也不得绕过已占用状态。 */
	if (ww_mutex_trylock(&mutex, NULL))  {
		pr_err("%s: trylocked itself without context from %s!\n", __func__, from);
		ww_mutex_unlock(&mutex);
		ret = -EINVAL;
		goto out;
	}

	/* 阶段三：携带相同 ctx 的 trylock 必须同样失败，不能伪造第二次 acquired。 */
	if (ww_mutex_trylock(&mutex, &ctx))  {
		pr_err("%s: trylocked itself with context from %s!\n", __func__, from);
		ww_mutex_unlock(&mutex);
		ret = -EINVAL;
		goto out;
	}

	/* 阶段四：阻塞式 API 应识别同一上下文递归并返回明确的 -EALREADY。 */
	ret = ww_mutex_lock(&mutex, &ctx);
	if (ret != -EALREADY) {
		pr_err("%s: missed deadlock for recursing, ret=%d from %s\n",
		       __func__, ret, from);
		if (!ret)
			ww_mutex_unlock(&mutex);
		ret = -EINVAL;
		goto out;
	}

	/* 成功路径释放最初那次获取；out 统一结束 ctx 生命周期。 */
	ww_mutex_unlock(&mutex);
	ret = 0;
out:
	ww_acquire_fini(&ctx);
	return ret;
}

/* ABBA 两参与者共享状态：两把锁交叉获取，completion 保证双方先各持一把再请求另一把。 */
struct test_abba {
	struct work_struct work;
	struct ww_class *class;
	struct ww_mutex a_mutex;
	struct ww_mutex b_mutex;
	struct completion a_ready;
	struct completion b_ready;
	bool resolve, trylock;
	int result;
};

/*
 * ABBA 场景的工作队列参与者 B：先持有 b，再等待 A 持有 a，最后请求 a。
 * abba 必须在 flush_work 前有效；resolve 决定收到 -EDEADLK 后是否执行标准退避恢复。
 * 最终释放自己实际持有的锁、fini ctx，并把锁 API 结果写入 abba->result 供主线程判定。
 */
static void test_abba_work(struct work_struct *work)
{
	/* abba 是共享编排对象，ctx 是 B 事务独占且只在本 worker 生命周期内有效。 */
	struct test_abba *abba = container_of(work, typeof(*abba), work);
	struct ww_acquire_ctx ctx;
	/* err 保存请求 a 的结果；恢复成功后归零。 */
	int err;

	/* 阶段一：关闭人工注入，按测试参数用 lock 或 trylock 取得 b。 */
	ww_acquire_init_noinject(&ctx, abba->class);
	if (!abba->trylock)
		ww_mutex_lock(&abba->b_mutex, &ctx);
	else
		WARN_ON(!ww_mutex_trylock(&abba->b_mutex, &ctx));

	/* ctx 发布检查确保 b 的 ownership 确实属于本事务，而非仅底层锁成功。 */
	WARN_ON(READ_ONCE(abba->b_mutex.ctx) != &ctx);

	/* 阶段二：通知 A“B 已持 b”，并等待 A 对称地持有 a，至此 ABBA 前提成立。 */
	complete(&abba->b_ready);
	wait_for_completion(&abba->a_ready);

	/* 阶段三：请求 A 持有的 a；算法必须让至少一个参与者收到 -EDEADLK。 */
	err = ww_mutex_lock(&abba->a_mutex, &ctx);
	if (abba->resolve && err == -EDEADLK) {
		/* 规范恢复：先释放已持 b，慢速无死锁检查地取得争用 a，再按原序重新取得 b。 */
		ww_mutex_unlock(&abba->b_mutex);
		ww_mutex_lock_slow(&abba->a_mutex, &ctx);
		err = ww_mutex_lock(&abba->b_mutex, &ctx);
	}

	/* 只释放成功取得的 a；b 在正常路径或恢复路径结束时均由本事务持有。 */
	if (!err)
		ww_mutex_unlock(&abba->a_mutex);
	ww_mutex_unlock(&abba->b_mutex);
	ww_acquire_fini(&ctx);

	abba->result = err;
}

/*
 * 在调用线程扮演参与者 A，并与 worker B 构造、可选地解除 ABBA 死锁。
 * trylock 只改变双方第一把锁的获取方式；resolve=false 要求至少一方返回 -EDEADLK，
 * resolve=true 则要求双方依照回退协议最终都成功。返回 0 表示预期成立，否则返回 -EINVAL。
 * 栈上 work 在返回前必经 flush/destroy，两个 ctx 也各自只覆盖所属参与者的持锁期。
 */
static int test_abba(struct ww_class *class, bool trylock, bool resolve)
{
	/* abba 汇集两把锁、同步点和 B 的结果；ctx/err 属于 A，ret 是测试结论。 */
	struct test_abba abba;
	struct ww_acquire_ctx ctx;
	int err, ret;

	ww_mutex_init(&abba.a_mutex, class);
	ww_mutex_init(&abba.b_mutex, class);
	INIT_WORK_ONSTACK(&abba.work, test_abba_work);
	init_completion(&abba.a_ready);
	init_completion(&abba.b_ready);
	abba.class = class;
	abba.trylock = trylock;
	abba.resolve = resolve;

	/* 阶段一：启动 B；随后 A 取得 a，并核验 a.ctx 已发布为自己的上下文。 */
	queue_work(wq, &abba.work);

	ww_acquire_init_noinject(&ctx, class);
	if (!trylock)
		ww_mutex_lock(&abba.a_mutex, &ctx);
	else
		WARN_ON(!ww_mutex_trylock(&abba.a_mutex, &ctx));

	WARN_ON(READ_ONCE(abba.a_mutex.ctx) != &ctx);

	/* 阶段二：放行 B 请求 a，同时等待 B 已持有 b，再由 A 请求 b，闭合 ABBA。 */
	complete(&abba.a_ready);
	wait_for_completion(&abba.b_ready);

	err = ww_mutex_lock(&abba.b_mutex, &ctx);
	if (resolve && err == -EDEADLK) {
		/* A 与 B 使用完全对称的释放、slow-lock 争用锁、重试原锁流程。 */
		ww_mutex_unlock(&abba.a_mutex);
		ww_mutex_lock_slow(&abba.b_mutex, &ctx);
		err = ww_mutex_lock(&abba.a_mutex, &ctx);
	}

	/* 阶段三：按实际获取结果释放锁并结束 A 的事务，再等待 B 完成后读取其 result。 */
	if (!err)
		ww_mutex_unlock(&abba.b_mutex);
	ww_mutex_unlock(&abba.a_mutex);
	ww_acquire_fini(&ctx);

	flush_work(&abba.work);
	destroy_work_on_stack(&abba.work);

	/* 阶段四：恢复模式要求两方均为 0；检测模式只要求至少一方准确报告 -EDEADLK。 */
	ret = 0;
	if (resolve) {
		if (err || abba.result) {
			pr_err("%s: failed to resolve ABBA deadlock, A err=%d, B err=%d\n",
			       __func__, err, abba.result);
			ret = -EINVAL;
		}
	} else {
		if (err != -EDEADLK && abba.result != -EDEADLK) {
			pr_err("%s: missed ABBA deadlock, A err=%d, B err=%d\n",
			       __func__, err, abba.result);
			ret = -EINVAL;
		}
	}
	return ret;
}

/* 环中一个参与者：先持 a_mutex，再请求下一节点的 b_mutex；信号边把所有节点同步成闭环。 */
struct test_cycle {
	struct work_struct work;
	struct ww_class *class;
	struct ww_mutex a_mutex;
	struct ww_mutex *b_mutex;
	struct completion *a_signal;
	struct completion b_signal;
	int result;
};

/*
 * 多节点环形死锁中的单个工作项。
 * cycle->a_mutex 属于本节点，b_mutex 指向下一节点；a_signal 指向前一节点的 b_signal。
 * 所有节点先持 a 并沿环发送信号，收到自身 b_signal 后才请求下一把锁，从而确定性制造环。
 * 若被选为退避者，则释放 a、慢速取得 b、再重试 a；最终 result 为两次请求中的首个错误。
 */
static void test_cycle_work(struct work_struct *work)
{
	/* cycle 是预先连成环的节点；ctx 标识本节点事务，err/erra 分别记录 b/a 请求。 */
	struct test_cycle *cycle = container_of(work, typeof(*cycle), work);
	struct ww_acquire_ctx ctx;
	int err, erra = 0;

	/* 阶段一：先持本节点 a，再唤醒环上的后继同步关系。 */
	ww_acquire_init_noinject(&ctx, cycle->class);
	ww_mutex_lock(&cycle->a_mutex, &ctx);

	complete(cycle->a_signal);
	/* 等待本节点的 b_signal，意味着下一节点也已持有其 a，即本节点的 b。 */
	wait_for_completion(&cycle->b_signal);

	/* 阶段二：请求下一节点之锁，某个事务必须收到 -EDEADLK 才能打破全环。 */
	err = ww_mutex_lock(cycle->b_mutex, &ctx);
	if (err == -EDEADLK) {
		/* 被选中的退避者按协议释放 a，保证 slow-lock(b) 可随链式解锁向前推进。 */
		err = 0;
		ww_mutex_unlock(&cycle->a_mutex);
		ww_mutex_lock_slow(cycle->b_mutex, &ctx);
		erra = ww_mutex_lock(&cycle->a_mutex, &ctx);
	}

	/* 阶段三：仅释放成功拥有的锁，结束上下文并发布本节点最终结果。 */
	if (!err)
		ww_mutex_unlock(cycle->b_mutex);
	if (!erra)
		ww_mutex_unlock(&cycle->a_mutex);
	ww_acquire_fini(&ctx);

	cycle->result = err ?: erra;
}

/*
 * 构造 nthreads 个节点的锁等待环并验证 ww_mutex 能将其完全解除。
 * nthreads 至少为 2；每节点的 b 指向下一节点 a，最后节点回指第 0 节点，completion 信号亦闭环。
 * 返回 0 表示所有 worker 经一次可能的 -EDEADLK 恢复后成功，-ENOMEM 表示分配失败，
 * -EINVAL 表示至少一个节点遗留错误。返回前销毁全部锁并释放节点数组。
 */
static int __test_cycle(struct ww_class *class, unsigned int nthreads)
{
	/* cycles 保存环节点；last 用于闭合首尾；ret 汇总首个非零节点结果。 */
	struct test_cycle *cycles;
	unsigned int n, last = nthreads - 1;
	int ret;

	cycles = kmalloc_objs(*cycles, nthreads);
	if (!cycles)
		return -ENOMEM;

	/* 阶段一：初始化每把 a，并把 b_mutex 与 completion 信号都连接成首尾相接的环。 */
	for (n = 0; n < nthreads; n++) {
		struct test_cycle *cycle = &cycles[n];

		cycle->class = class;
		ww_mutex_init(&cycle->a_mutex, class);
		if (n == last)
			cycle->b_mutex = &cycles[0].a_mutex;
		else
			cycle->b_mutex = &cycles[n + 1].a_mutex;

		if (n == 0)
			cycle->a_signal = &cycles[last].b_signal;
		else
			cycle->a_signal = &cycles[n - 1].b_signal;
		init_completion(&cycle->b_signal);

		INIT_WORK(&cycle->work, test_cycle_work);
		cycle->result = 0;
	}

	/* 阶段二：并发启动所有节点；flush 保证可以安全检查结果与回收数组。 */
	for (n = 0; n < nthreads; n++)
		queue_work(wq, &cycles[n].work);

	flush_workqueue(wq);

	/* 阶段三：任何残留错误都说明死锁回退协议未能让整个环收敛。 */
	ret = 0;
	for (n = 0; n < nthreads; n++) {
		struct test_cycle *cycle = &cycles[n];

		if (!cycle->result)
			continue;

		pr_err("cyclic deadlock not resolved, ret[%d/%d] = %d\n",
		       n, nthreads, cycle->result);
		ret = -EINVAL;
		break;
	}

	/* 阶段四：worker 已全部退出，逐锁销毁调试状态并释放环节点。 */
	for (n = 0; n < nthreads; n++)
		ww_mutex_destroy(&cycles[n].a_mutex);
	kfree(cycles);
	return ret;
}

/*
 * 从二节点环递增测试到“在线 CPU 数加一”节点环。
 * ncpus 决定覆盖规模；每一规模串行运行，首个错误立即返回，全部解除则返回 0。
 * 加一使最大参与者数超过在线 CPU 数，可覆盖 worker 调度而非同时运行的情况。
 */
static int test_cycle(struct ww_class *class, unsigned int ncpus)
{
	/* n 是当前环长度，ret 传递构造/协议失败。 */
	unsigned int n;
	int ret;

	/* 最小 ABBA 等价环为 2，最大规模为 ncpus + 1。 */
	for (n = 2; n <= ncpus + 1; n++) {
		ret = __test_cycle(class, n);
		if (ret)
			return ret;
	}

	return 0;
}

/* 一项压力 worker 的只读配置与工作项；锁数组由 stress() 持有到整个工作队列 flush 完成。 */
struct stress {
	struct work_struct work;
	struct ww_mutex *locks;
	struct ww_class *class;
	unsigned long timeout;
	int nlocks;
};

/* 可复现伪随机状态只由 rng_lock 保护下的本地 helper 访问。 */
struct rnd_state rng;
/* 多个 unbound worker 共享 rng，普通自旋锁防止状态更新相互覆盖。 */
DEFINE_SPINLOCK(rng_lock);

/*
 * 从模块私有 PRNG 取得 [0, ceil) 的伪随机数。
 * ceil 必须非零；rng_lock 串行化 prandom_u32_state() 对全局 rng 的读改写。
 * 返回值用于生成锁顺序，不承担密码学随机性；退出前总会释放自旋锁。
 */
static inline u32 prandom_u32_below(u32 ceil)
{
	/* ret 在锁内生成，在解锁后以普通值返回。 */
	u32 ret;

	/* 取模足以满足压力测试扰动需求，轻微分布偏差不影响正确性判定。 */
	spin_lock(&rng_lock);
	ret = prandom_u32_state(&rng) % ceil;
	spin_unlock(&rng_lock);
	return ret;
}

/*
 * 分配并生成 [0, count) 的随机排列，供单个压力 worker 固定使用。
 * count 应为正数；返回 kmalloc 数组，分配失败返回 NULL，调用者负责 kfree。
 * Fisher-Yates 逆序交换保证每把锁恰出现一次，使竞争来自顺序差异而非重复索引。
 */
static int *get_random_order(int count)
{
	/* order 是返回数组，n/r 分别是当前尾位置与随机交换位置。 */
	int *order;
	int n, r;

	order = kmalloc_objs(*order, count);
	if (!order)
		return order;

	for (n = 0; n < count; n++)
		order[n] = n;

	/* 从尾部逐项固定元素；r 落在仍未固定的 [0, n] 区间。 */
	for (n = count - 1; n > 1; n--) {
		r = prandom_u32_below(n + 1);
		if (r != n)
			swap(order[n], order[r]);
	}

	return order;
}

/*
 * 模拟持有整组锁期间的短临界区负载。
 * stress 当前未被读取，但保留参数以表达 worker 场景并便于未来扩展。
 * 睡眠 1～2ms 主动扩大其他 worker 发生锁竞争的时间窗口，无返回状态。
 */
static void dummy_load(struct stress *stress)
{
	usleep_range(1000, 2000);
}

/*
 * 按每个 worker 固定的随机顺序反复获取全部 ww_mutex，并用标准 slow-lock 流程恢复死锁。
 * work 必须嵌入有效 stress；锁数组和 class 在 worker 退出前不可销毁。
 * 收到 -EDEADLK 时释放本轮已持锁，慢速取得争用锁，并跳过它重试其余顺序；超时后停止重试。
 * 分配的 order 总会释放，正常、超时或异常路径均不遗留锁和 acquire_ctx。
 */
static void stress_inorder_work(struct work_struct *work)
{
	/* stress 提供共享锁集与截止时间；nlocks/locks 是只读别名，order 是本 worker 私有排列。 */
	struct stress *stress = container_of(work, typeof(*stress), work);
	const int nlocks = stress->nlocks;
	struct ww_mutex *locks = stress->locks;
	struct ww_acquire_ctx ctx;
	int *order;

	order = get_random_order(nlocks);
	if (!order)
		return;

	/* 每轮使用新的 ctx，但复用同一随机顺序，以持续制造跨 worker 的次序冲突。 */
	do {
		/* contended 是已由 slow path 预先取得的顺序下标，-1 表示尚无；n/err 跟踪本次扫描。 */
		int contended = -1;
		int n, err;

		ww_acquire_init(&ctx, stress->class);
retry:
		/* 从头请求全部锁，跳过上次 -EDEADLK 后已通过 lock_slow 持有的那一把。 */
		err = 0;
		for (n = 0; n < nlocks; n++) {
			if (n == contended)
				continue;

			err = ww_mutex_lock(&locks[order[n]], &ctx);
			if (err < 0)
				break;
		}
		if (!err)
			dummy_load(stress);

		/* 释放旧 contended 之后已经取得的锁，再记录本次停止位置并逆序释放此前锁。 */
		if (contended > n)
			ww_mutex_unlock(&locks[order[contended]]);
		contended = n;
		while (n--)
			ww_mutex_unlock(&locks[order[n]]);

		/* 仅在截止时间前执行标准恢复；lock_slow 成功后回到 retry，ctx 保持不变。 */
		if (err == -EDEADLK) {
			if (!time_after(jiffies, stress->timeout)) {
				ww_mutex_lock_slow(&locks[order[contended]], &ctx);
				goto retry;
			}
		}

		/* 本轮已无锁，结束 ctx；非预期错误终止 worker，正常则在截止前开启下一轮。 */
		ww_acquire_fini(&ctx);
		if (err) {
			pr_err_once("stress (%s) failed with %d\n",
				    __func__, err);
			break;
		}
	} while (!time_after(jiffies, stress->timeout));

	/* order 只归本 worker 所有，退出循环后统一回收。 */
	kfree(order);
}

/* 重排压力测试的链表节点；每个节点只借用 stress() 创建的一把 ww_mutex。 */
struct reorder_lock {
	struct list_head link;
	struct ww_mutex *lock;
};

/*
 * 以链表保存随机锁序，并把每次引发 -EDEADLK 的争用锁移动到表头后重新遍历。
 * 与固定顺序 worker 不同，本 worker 学习出“先拿争用锁”的动态顺序，持续验证锁序重排协议。
 * 非 -EDEADLK 错误终止当前压力循环；所有成功获取的锁在每轮末释放，链表节点在 out 统一回收。
 */
static void stress_reorder_work(struct work_struct *work)
{
	/* locks 是本 worker 私有顺序链表；ll/ln 用于遍历和安全释放，order 仅用于初始构造。 */
	struct stress *stress = container_of(work, typeof(*stress), work);
	LIST_HEAD(locks);
	struct ww_acquire_ctx ctx;
	struct reorder_lock *ll, *ln;
	int *order;
	int n, err;

	order = get_random_order(stress->nlocks);
	if (!order)
		return;

	/* 阶段一：为每个随机索引建立节点；任一分配失败都转到 out 回收已建部分。 */
	for (n = 0; n < stress->nlocks; n++) {
		ll = kmalloc_obj(*ll);
		if (!ll)
			goto out;

		ll->lock = &stress->locks[order[n]];
		list_add(&ll->link, &locks);
	}
	kfree(order);
	order = NULL;

	/* 阶段二：每轮建立 ctx，并沿当前链表顺序获取整组锁。 */
	do {
		ww_acquire_init(&ctx, stress->class);

		list_for_each_entry(ll, &locks, link) {
			err = ww_mutex_lock(ll->lock, &ctx);
			if (!err)
				continue;

			ln = ll;
			/* 失败时逆向释放当前节点之前已经成功获取的全部锁。 */
			list_for_each_entry_continue_reverse(ln, &locks, link)
				ww_mutex_unlock(ln->lock);

			if (err != -EDEADLK) {
				pr_err_once("stress (%s) failed with %d\n",
					    __func__, err);
				break;
			}

			/* 慢速取得争用锁后将其前置，使下一次迭代从该锁之后重新获取其余锁。 */
			ww_mutex_lock_slow(ll->lock, &ctx);
			list_move(&ll->link, &locks); /* restarts iteration */
			/* 上句会重新开始链表迭代。 */
		}

		/* 全部锁到手后模拟临界区，再按当前链表顺序释放并结束事务。 */
		dummy_load(stress);
		list_for_each_entry(ll, &locks, link)
			ww_mutex_unlock(ll->lock);

		ww_acquire_fini(&ctx);
	} while (!time_after(jiffies, stress->timeout));

out:
	/* worker 已不持锁；安全删除所有私有节点，order 可能在早期失败时仍非 NULL。 */
	list_for_each_entry_safe(ll, ln, &locks, link)
		kfree(ll);
	kfree(order);
}

/*
 * 在不使用 acquire_ctx 的情况下反复竞争随机选定的一把普通 ww_mutex。
 * 该 worker 覆盖 ww_mutex 退化为普通 mutex 的 API 路径；每次成功获取后模拟短负载并立即解锁。
 * 到达共享截止时间或遇到非零错误后退出，不保留锁，也不创建需要 fini 的上下文。
 */
static void stress_one_work(struct work_struct *work)
{
	/* 每个 worker 启动时固定随机选一把锁；err 只应为 0，否则记录一次诊断并停止。 */
	struct stress *stress = container_of(work, typeof(*stress), work);
	const int nlocks = stress->nlocks;
	struct ww_mutex *lock = stress->locks + get_random_u32_below(nlocks);
	int err;

	/* 在截止时间内反复扩大同锁竞争窗口，同时验证 NULL ctx 获取/释放闭环。 */
	do {
		err = ww_mutex_lock(lock, NULL);
		if (!err) {
			dummy_load(stress);
			ww_mutex_unlock(lock);
		} else {
			pr_err_once("stress (%s) failed with %d\n",
				    __func__, err);
			break;
		}
	} while (!time_after(jiffies, stress->timeout));
}

/* 创建使用固定随机顺序并执行 ww 死锁恢复的 worker。 */
#define STRESS_INORDER BIT(0)
/* 创建会把争用锁移到表头、动态调整获取次序的 worker。 */
#define STRESS_REORDER BIT(1)
/* 创建不带 acquire_ctx、只竞争单把锁的 worker。 */
#define STRESS_ONE BIT(2)
/* 同时启用三类压力模型。 */
#define STRESS_ALL (STRESS_INORDER | STRESS_REORDER | STRESS_ONE)

/*
 * 创建 nlocks 把共享锁和 nthreads 个压力 worker，并按 flags 轮转选择 worker 类型。
 * class 决定 ww 策略；至少应启用一种 flags，否则分配 worker 的循环无法消耗 nthreads。
 * 返回 -ENOMEM 表示数组分配失败；成功排队后 flush 到全部 worker 超时退出，销毁每把锁并释放数组。
 * worker 只借用 locks/stress_array，因此这两个数组的生命周期由 flush_workqueue() 建立边界。
 */
static int stress(struct ww_class *class, int nlocks, int nthreads, unsigned int flags)
{
	/* locks 是共享锁池，stress_array 保存实际创建的 worker，n/count 分别负责轮转和紧凑下标。 */
	struct ww_mutex *locks;
	struct stress *stress_array;
	int n, count;

	locks = kmalloc_objs(*locks, nlocks);
	if (!locks)
		return -ENOMEM;

	stress_array = kmalloc_objs(*stress_array, nthreads);
	if (!stress_array) {
		kfree(locks);
		return -ENOMEM;
	}

	/* 阶段一：全部锁属于同一 class，使跨 worker 的 acquire_ctx 可参与同一死锁策略。 */
	for (n = 0; n < nlocks; n++)
		ww_mutex_init(&locks[n], class);

	count = 0;
	/* 阶段二：n 持续轮转三类槽位，仅启用的槽位才创建 worker 并消耗一个 nthreads。 */
	for (n = 0; nthreads; n++) {
		/* stress 指向下一个紧凑槽位；fn 为本轮由 flags 选出的工作函数。 */
		struct stress *stress;
		void (*fn)(struct work_struct *work);

		fn = NULL;
		switch (n & 3) {
		case 0:
			if (flags & STRESS_INORDER)
				fn = stress_inorder_work;
			break;
		case 1:
			if (flags & STRESS_REORDER)
				fn = stress_reorder_work;
			break;
		case 2:
			if (flags & STRESS_ONE)
				fn = stress_one_work;
			break;
		}

		if (!fn)
			continue;

		/* 所有 worker 使用同一锁池和约两秒绝对截止时间，再提交到专用队列。 */
		stress = &stress_array[count++];

		INIT_WORK(&stress->work, fn);
		stress->class = class;
		stress->locks = locks;
		stress->nlocks = nlocks;
		stress->timeout = jiffies + 2*HZ;

		queue_work(wq, &stress->work);
		nthreads--;
	}

	/* 阶段三：等待所有借用者退出后，才能销毁锁调试状态和两个拥有数组。 */
	flush_workqueue(wq);

	for (n = 0; n < nlocks; n++)
		ww_mutex_destroy(&locks[n]);
	kfree(stress_array);
	kfree(locks);

	return 0;
}

/*
 * 对一个 ww_class 依次运行全部定向与压力测试，首次失败立即停止。
 * class 分别由上层传入 Wound-Wait 或 Wait-Die；在线 CPU 数控制环长度和并发 worker 数。
 * 返回 0 表示基础互斥、递归、四种 ABBA、所有环规模和三组压力配置全部通过，
 * 否则原样传递首个测试错误，便于模块初始化失败或日志定位。
 */
static int run_tests(struct ww_class *class)
{
	/* ncpus 是本次拓扑规模快照；ret 传递失败，i 枚举 trylock/resolve 两位组合。 */
	int ncpus = num_online_cpus();
	int ret, i;

	/* 阶段一：先验证单锁基本语义和 lock/trylock 两种首次获取下的同 ctx 递归检测。 */
	ret = test_mutex(class);
	if (ret)
		return ret;

	ret = test_aa(class, false);
	if (ret)
		return ret;

	ret = test_aa(class, true);
	if (ret)
		return ret;

	/* 阶段二：四种组合覆盖首次 trylock 与是否执行死锁恢复。 */
	for (i = 0; i < 4; i++) {
		ret = test_abba(class, i & 1, i & 2);
		if (ret)
			return ret;
	}

	/* 阶段三：把确定性环从 2 扩到 ncpus+1，验证超过并行度时仍可解除。 */
	ret = test_cycle(class, ncpus);
	if (ret)
		return ret;

	/* 阶段四：分别压测固定顺序、动态重排，最后用大锁池混合全部三类 worker。 */
	ret = stress(class, 16, 2 * ncpus, STRESS_INORDER);
	if (ret)
		return ret;

	ret = stress(class, 16, 2 * ncpus, STRESS_REORDER);
	if (ret)
		return ret;

	ret = stress(class, 2046, hweight32(STRESS_ALL) * ncpus, STRESS_ALL);
	if (ret)
		return ret;

	return 0;
}

/*
 * 先后对 Wound-Wait 与 Wait-Die 两个 class 运行同一套测试。
 * 每类开始前打印进度；任一类失败立即返回其错误，只有两类均通过才打印总成功并返回 0。
 * 本函数不自行串行化调用，当前两个入口都必须在调用期间持有 run_lock。
 */
static int run_test_classes(void)
{
	/* ret 传递当前 class 的首个失败。 */
	int ret;

	/* Wound-Wait 先运行，验证较老事务 wound 较新事务的策略实例。 */
	pr_info("Beginning ww (wound) mutex selftests\n");

	ret = run_tests(&ww_class);
	if (ret)
		return ret;

	/* Wait-Die 后运行相同场景，验证较新事务主动退避的策略实例。 */
	pr_info("Beginning ww (die) mutex selftests\n");
	ret = run_tests(&wd_class);
	if (ret)
		return ret;

	pr_info("All ww mutex selftests passed\n");
	return 0;
}

/* 模块初始化自动测试与 sysfs 手工触发共享此锁，trylock 让重复触发快速返回。 */
static DEFINE_MUTEX(run_lock);

/*
 * sysfs 的 run_tests 写入口；写入内容本身不解析，任意写操作都尝试启动完整测试。
 * kobj、attr、buf 由 sysfs 框架传入且仅 count 影响返回；若已有测试运行，记录错误但仍消费写入。
 * 成功取得 run_lock 后同步运行两类测试并解锁，返回原始 count；测试错误仅通过内核日志报告。
 */
static ssize_t run_tests_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	/* trylock 避免第二个 sysfs 写者无限等待一轮较长的压力测试。 */
	if (!mutex_trylock(&run_lock)) {
		pr_err("Test already running\n");
		return count;
	}

	/* 返回值刻意不传播 selftest 结果，sysfs 写语义只表示请求已被接收。 */
	run_test_classes();
	mutex_unlock(&run_lock);

	return count;
}

/* 暴露只写回调 run_tests_store；读回调为 NULL，权限沿用现有测试接口定义。 */
static struct kobj_attribute run_tests_attribute =
	__ATTR(run_tests, 0664, NULL, run_tests_store);

/* sysfs 属性数组必须以 NULL 哨兵结尾，供属性组遍历确定边界。 */
static struct attribute *attrs[] = {
	&run_tests_attribute.attr,
	NULL,   /* need to NULL terminate the list of attributes */
	/* 属性列表需要以 NULL 结尾。 */
};

/* 将 run_tests 单属性集合交给 sysfs_create_group() 一次性注册。 */
static struct attribute_group attr_group = {
	.attrs = attrs,
};

/* /sys/kernel/test_ww_mutex 对应 kobject，由 init 创建并由 exit 或失败回滚 put。 */
static struct kobject *test_ww_mutex_kobj;

/*
 * 初始化 ww_mutex 自测模块：播种 PRNG，创建专用 workqueue 和 sysfs 节点，并自动运行全套测试。
 * 成功返回 0；资源创建失败返回 -ENOMEM/sysfs 错误并回滚已建资源；自测失败则直接传递测试错误。
 * run_lock 覆盖自动测试，防止 sysfs 节点刚发布后并发触发第二轮；当前源码未在自测失败路径撤销
 * 已创建的 kobject/workqueue，正常成功初始化后的资源则留给 exit 回收。
 */
static int __init test_ww_mutex_init(void)
{
	/* ret 先承接 sysfs 注册结果，随后承接两类自测结果。 */
	int ret;

	/* 阶段一：为模块私有随机序列设置启动种子，再创建不绑定特定 CPU 的并发工作队列。 */
	prandom_seed_state(&rng, get_random_u64());

	wq = alloc_workqueue("test-ww_mutex", WQ_UNBOUND, 0);
	if (!wq)
		return -ENOMEM;

	/* 阶段二：在 kernel_kobj 下建立测试目录；失败时工作队列尚无任务，可直接销毁。 */
	test_ww_mutex_kobj = kobject_create_and_add("test_ww_mutex", kernel_kobj);
	if (!test_ww_mutex_kobj) {
		destroy_workqueue(wq);
		return -ENOMEM;
	}

	/* Create the files associated with this kobject */
	/* 创建与该 kobject 关联的文件。失败时依创建逆序撤销 kobject 和工作队列。 */
	ret = sysfs_create_group(test_ww_mutex_kobj, &attr_group);
	if (ret) {
		kobject_put(test_ww_mutex_kobj);
		destroy_workqueue(wq);
		return ret;
	}

	/* 阶段三：sysfs 已可见，持 run_lock 完成自动测试，阻止写入口并发重入。 */
	mutex_lock(&run_lock);
	ret = run_test_classes();
	mutex_unlock(&run_lock);

	return ret;
}

/*
 * 卸载模块时撤销 sysfs kobject 并销毁专用工作队列。
 * 模块退出框架保证 init 已成功且不存在新的模块调用者；测试本身同步 flush 所有已排队 work。
 * kobject_put() 先移除用户触发入口，destroy_workqueue() 再回收执行资源，无返回值。
 */
static void __exit test_ww_mutex_exit(void)
{
	/* 先阻止新的 sysfs 请求，再销毁只可能已空闲的专用队列。 */
	kobject_put(test_ww_mutex_kobj);
	destroy_workqueue(wq);
}

/* 注册上述初始化与退出函数为模块生命周期入口。 */
module_init(test_ww_mutex_init);
module_exit(test_ww_mutex_exit);

/* 模块元数据：GPL 许可、原作者和用途描述。 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("API test facility for ww_mutexes");
