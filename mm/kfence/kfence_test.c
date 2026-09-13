// SPDX-License-Identifier: GPL-2.0
/*
 * Test cases for KFENCE memory safety error detector. Since the interface with
 * which KFENCE's reports are obtained is via the console, this is the output we
 * should verify. For each test case checks the presence (or absence) of
 * generated reports. Relies on 'console' tracepoint to capture reports as they
 * appear in the kernel log.
 *
 * Copyright (C) 2020, Google LLC.
 * Author: Alexander Potapenko <glider@google.com>
 *         Marco Elver <elver@google.com>
 */

#include <kunit/test.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kfence.h>
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/string_choices.h>
/* tracepoint 头把报告观察从 printk 输出解耦；测试不依赖控制台实际设备。 */
#include <linux/tracepoint.h>
#include <trace/events/printk.h>

#include <asm/kfence.h>

#include "kfence.h"

/* 本文件不直接验证内存破坏是否阻止执行，而是通过 console tracepoint 捕获 KFENCE 报告并按测试契约匹配。 */

/* May be overridden by <asm/kfence.h>. */
/* 架构可重写报告地址的规范化方式，使测试匹配物理别名或标签后的稳定表示。 */
#ifndef arch_kfence_test_address
#define arch_kfence_test_address(addr) (addr)
#endif

#define KFENCE_TEST_REQUIRES(test, cond) do {			\
	/* 条件不满足时跳过而非失败：配置和架构能力不同。 */ \
	if (!(cond))						\
		kunit_skip((test), "Test requires: " #cond);	\
} while (0)

/* Report as observed from console. */
static struct {
	/* console probe 的共享快照；lock 保护字符串和 nlines，READ/WRITE_ONCE 允许先行无锁快速检查。 */
	spinlock_t lock;
	int nlines;
	char lines[2][256];
} observed = {
	.lock = __SPIN_LOCK_UNLOCKED(observed.lock),
};

/* Probe for console output: obtains observed lines of interest. */
static void probe_console(void *ignore, const char *buf, size_t len)
{
	/* 业务背景：tracepoint 回调只收集当前 test 的两行 KFENCE 诊断，供断言在正常测试上下文匹配。
	 * 入参：buf/len 是不保证 NUL 结尾的 console 片段；返回无直接值。
	 * 注意事项：IRQ-safe 自旋锁保护 observed；不能睡眠，复制长度必须受 len 和固定槽容量双重限制。
	 */
	unsigned long flags;
	int nlines;

	spin_lock_irqsave(&observed.lock, flags);
	/* 最终比较在锁内完成，probe 只能在解锁后发布下一次报告。 */
	/* 锁内先取已发布行数，两个相关日志片段只能按 0→1→2 的状态机追加。 */
	nlines = observed.nlines;

	if (strnstr(buf, "BUG: KFENCE: ", len) && strnstr(buf, "test_", len)) {
		/* 首行同时限定 KFENCE 标记和测试函数名，避免采集其它 CPU/子系统的异步报告。 */
		/*
		 * KFENCE report and related to the test.
		 *
		 * The provided @buf is not NUL-terminated; copy no more than
		 * @len bytes and let strscpy() add the missing NUL-terminator.
		 */
		strscpy(observed.lines[0], buf, min(len + 1, sizeof(observed.lines[0])));
		nlines = 1;
	} else if (nlines == 1 && (strnstr(buf, "at 0x", len) || strnstr(buf, "of 0x", len))) {
		/* 仅已得到标题时接受地址详情；顺序错乱的日志不能组成伪报告。 */
		strscpy(observed.lines[nlines++], buf, min(len + 1, sizeof(observed.lines[0])));
	}

	WRITE_ONCE(observed.nlines, nlines); /* Publish new nlines. */
	/* 发布在字符串复制后，读者看到 2 时两槽均已完成写入。 */
	spin_unlock_irqrestore(&observed.lock, flags);
}

/* Check if a report related to the test exists. */
static bool report_available(void)
{
	/* 无锁快速谓词只判断完整两行；真正读取内容的 report_matches 会在锁内二次确认。 */
	return READ_ONCE(observed.nlines) == ARRAY_SIZE(observed.lines);
}

/* Information we expect in a report. */
struct expect_report {
	/* 每个用例声明预期错误类型、触发函数、地址和读写方向；地址字符串由测试分配对象派生。 */
	enum kfence_error_type type; /* The type or error. */
	void *fn; /* Function pointer to expected function where access occurred. */
	char *addr; /* Address at which the bad access occurred. */
	bool is_write; /* Is access a write. */
};

static const char *get_access_type(const struct expect_report *r)
{
	/* 内核统一的 true/false 字符串确保期望文本与 KFENCE 报告的 read/write 拼写一致。 */
	return str_write_read(r->is_write);
}

/* Check observed report matches information in @r. */
static bool report_matches(const struct expect_report *r)
{
	/* 业务背景：把测试期望序列化为两行报告子串，并在锁内对 console 快照做最终匹配。
	 * 入参：r 为只读期望；返回 true 仅表示完整报告与类型/函数/地址均相符。
	 * 注意事项：先无锁检查降低常见失败成本，锁内必须重检，因为 probe 可并发覆盖快照。
	 */
	unsigned long addr = (unsigned long)r->addr;
	bool ret = false;
	unsigned long flags;
	typeof(observed.lines) expect;
	const char *end;
	char *cur;

	/* Doubled-checked locking. */
	if (!report_available())
		/* 尚未收到两行不能读取 observed 字符串，避免把 probe 的中间状态当负面结论。 */
		return false;

	/* Generate expected report contents. */
	/* 阶段一先构造标题，阶段二再构造地址行；两者均在局部缓冲完成后才进入共享快照比较。 */

	/* Title */
	/* 标题必须包含错误类别和触发符号，偏移部分被特意忽略以适配不同编译布局。 */
	cur = expect[0];
	/* 标题缓冲从首地址开始，后续 scnprintf 返回值累计为下一写入位置。 */
	/* expect 是栈上两行固定缓冲，ARRAY_END 限制 scnprintf 的每次追加不会越界。 */
	end = ARRAY_END(expect[0]);
	switch (r->type) {
		/* 标题先编码错误类别，随后统一追加符号名，保证每个 case 的匹配格式可比较。 */
		/* 类型决定标题协议；没有 default，枚举新增必须显式扩展测试期望而不能静默接受。 */
		/* OOB/UAF 都需携带读写方向；其余类别的标题由错误本身完全描述。 */
	case KFENCE_ERROR_OOB:
		cur += scnprintf(cur, end - cur, "BUG: KFENCE: out-of-bounds %s",
				 get_access_type(r));
		break;
	case KFENCE_ERROR_UAF:
		cur += scnprintf(cur, end - cur, "BUG: KFENCE: use-after-free %s",
				 get_access_type(r));
		break;
		/* guard 篡改与非法释放没有访问方向，故报告文本不调用 get_access_type。 */
	case KFENCE_ERROR_CORRUPTION:
		cur += scnprintf(cur, end - cur, "BUG: KFENCE: memory corruption");
		break;
	case KFENCE_ERROR_INVALID:
		/* INVALID 仍描述一次 read/write，区别在于地址不对应已知对象边界。 */
		cur += scnprintf(cur, end - cur, "BUG: KFENCE: invalid %s",
				 get_access_type(r));
		break;
	case KFENCE_ERROR_INVALID_FREE:
		cur += scnprintf(cur, end - cur, "BUG: KFENCE: invalid free");
		break;
	}

	/* 所有标题分支汇合后统一追加符号，避免每种错误类别各自处理可变的函数格式。 */
	scnprintf(cur, end - cur, " in %pS", r->fn);
	/* The exact offset won't match, remove it; also strip module name. */
	/* 符号偏移与模块后缀随构建布局变化，故只保留函数基名作为跨配置稳定断言。 */
	cur = strchr(expect[0], '+');
	if (cur)
		*cur = '\0';

	/* Access information */
	/* 地址行按错误类别决定是否转换架构地址；测试只匹配稳定文本片段。 */
	cur = expect[1];
	/* 地址缓冲独立于标题，避免符号文本长度影响第二行匹配上限。 */
	/* 第二行只编码访问描述与地址；corruption/invalid-free 的地址规则由 KFENCE ABI 决定。 */
	end = ARRAY_END(expect[1]);

	switch (r->type) {
		/* 和标题同样穷尽所有枚举值，新增错误类型必须同步更新两行期望。 */
		/* 只有真实访问错误才经架构钩子重写地址，元数据错误保留报告给出的原始地址。 */
	case KFENCE_ERROR_OOB:
		cur += scnprintf(cur, end - cur, "Out-of-bounds %s at", get_access_type(r));
		addr = arch_kfence_test_address(addr);
		break;
	case KFENCE_ERROR_UAF:
		cur += scnprintf(cur, end - cur, "Use-after-free %s at", get_access_type(r));
		addr = arch_kfence_test_address(addr);
		break;
		/* 后三种类别不访问 guard page，地址行只描述被检出的对象或损坏位置。 */
	case KFENCE_ERROR_CORRUPTION:
		cur += scnprintf(cur, end - cur, "Corrupted memory at");
		break;
	case KFENCE_ERROR_INVALID:
		/* INVALID 的访问地址也需通过架构测试钩子，以适配带地址别名的平台。 */
		cur += scnprintf(cur, end - cur, "Invalid %s at", get_access_type(r));
		addr = arch_kfence_test_address(addr);
		break;
	case KFENCE_ERROR_INVALID_FREE:
		cur += scnprintf(cur, end - cur, "Invalid free of");
		break;
	}

	cur += scnprintf(cur, end - cur, " 0x%p", (void *)addr);
	/* 地址统一按报告格式拼接；函数偏移和模块名已在标题阶段剥离以避免 ASLR/链接差异。 */

	spin_lock_irqsave(&observed.lock, flags);
	/* 锁内重检防止 probe 在首次快速检查后写入下一份报告，匹配只接受同一完整快照。 */
	if (!report_available())
		/* 快照未完整时立即失败，调用者可由 KUnit 断言显示未生成报告。 */
		/* 新报告覆盖旧快照时放弃本次匹配，不能跨两份诊断组合标题和地址。 */
		goto out; /* A new report is being captured. */

	/* Finally match expected output to what we actually observed. */
	/* 此处只消费锁保护的已发布快照；strstr 匹配允许 console 行含有时间戳等无关前缀。 */
	/* 同时匹配两行避免仅有相同标题的无关报告误通过。 */
	ret = strstr(observed.lines[0], expect[0]) && strstr(observed.lines[1], expect[1]);
	/* 两个子串同时命中才证明类别、符号和地址行属于同一预期报告。 */
out:
	/* 所有出口恢复 IRQ 标志；ret 仅在两行子串都命中时发布为 true。 */
	spin_unlock_irqrestore(&observed.lock, flags);
	return ret;
}

/* 后续用例共享上述报告快照协议：任何触发动作都需在资源回收前完成两行匹配。 */

/* ===== Test cases ===== */
/* 以下每个 case 都经 test_init 清快照，并由 test_exit 回收独立 cache。 */

#define TEST_PRIV_WANT_MEMCACHE ((void *)1)

/* 非 NULL 哨兵只在 test_init 根据用例名称设置，避免为每个 KUnit case 新增参数化框架。 */

/* Cache used by tests; if NULL, allocate from kmalloc instead. */
/* 该选择也决定 test_free 的释放 API，不能在对象存活期间切换。 */
static struct kmem_cache *test_cache;

/* 全局 cache 只在单个 case 期间有效；suite 串行执行使它不需额外锁，但 teardown 必须归零。 */

static size_t setup_test_cache(struct kunit *test, size_t size, slab_flags_t flags,
			       void (*ctor)(void *))
{
	/* 业务背景：按用例私有标记建立独立 slab，避免合并缓存或其它测试对象改变 KFENCE 观察条件。
	 * 入参：size/flags/ctor 定义 cache；返回实际对象大小。注意事项：创建失败由 KUNIT_ASSERT 停止当前用例。
	 */
	if (test->priv != TEST_PRIV_WANT_MEMCACHE)
		/* 默认 kmalloc 路径不创建 cache，返回原 size 保持调用者后续对象边界计算一致。 */
		return size;

	kunit_info(test, "%s: size=%zu, ctor=%ps\n", __func__, size, ctor);

	/*
	 * Use SLAB_NO_MERGE to prevent merging with existing caches.
	 * Use SLAB_ACCOUNT to allocate via memcg, if enabled.
	 */
	flags |= SLAB_NO_MERGE | SLAB_ACCOUNT;
	/* NO_MERGE 稳定测试 cache 身份，ACCOUNT 同时覆盖 memcg 计费的 KFENCE 路径。 */
	test_cache = kmem_cache_create("test", size, 1, flags, ctor);
	KUNIT_ASSERT_TRUE_MSG(test, test_cache, "could not create cache");

	return size;
}

static void test_cache_destroy(void)
{
	/* 每个用例退出时销毁独立 cache；NULL 表示本例走 kmalloc，不能错误调用 destroy。 */
	if (!test_cache)
		/* NULL 是幂等 teardown；创建失败 case 已断言停止，不应有半初始化 cache。 */
		return;

	kmem_cache_destroy(test_cache);
	/* destroy 后立即清全局，禁止后续测试把已释放 cache 当作分配后端。 */
	test_cache = NULL;
}

static inline size_t kmalloc_cache_alignment(size_t size)
{
	/* 查询实际 kmalloc cache 对齐以把访问精确推到 guard page；不分配也不持有 cache 引用。 */
	/* just to get ->align so no need to pass in the real caller */
	enum kmalloc_cache_type type = kmalloc_type(GFP_KERNEL, __kmalloc_token(0));
	return kmalloc_caches[type][__kmalloc_index(size, false)]->align;
}

/* Must always inline to match stack trace against caller. */
/* 栈归属是 report_matches 断言的一部分，包装层不得污染预期 fn。 */
static __always_inline void test_free(void *ptr)
{
	/* 与 test_alloc 的后端选择严格配对；always_inline 让报告栈显示测试触发点而非包装函数。 */
	if (test_cache)
		/* 使用自建 cache 时 free 必须匹配其对象布局，不能混入 kfree。 */
		kmem_cache_free(test_cache, ptr);
	else
		kfree(ptr);
}

/*
 * If this should be a KFENCE allocation, and on which side the allocation and
 * the closest guard page should be.
 */
enum allocation_policy {
	/* policy 既筛选 KFENCE 分配，也规定对象相对 guard page 的侧别；NONE 用于验证普通分配退化。 */
	ALLOCATE_ANY, /* KFENCE, any side. */
	ALLOCATE_LEFT, /* KFENCE, left side of page. */
	ALLOCATE_RIGHT, /* KFENCE, right side of page. */
	ALLOCATE_NONE, /* No KFENCE allocation. */
};

/*
 * Try to get a guarded allocation from KFENCE. Uses either kmalloc() or the
 * current test_cache if set up.
 */
static void *test_alloc(struct kunit *test, size_t size, gfp_t gfp, enum allocation_policy policy)
{
	/* 业务背景：反复采样分配直到得到符合 guard-side 策略的 KFENCE 对象。
	 * 返回：成功为借给用例的对象；超时由 KUnit 断言失败。注意事项：失败候选立即按原后端释放，循环会让出 CPU 给采样 timer。
	 */
	void *alloc;
	unsigned long timeout, resched_after;
	const char *policy_name;

	switch (policy) {
		/* policy_name 仅用于诊断；四个枚举分支都必须赋值，否则日志会掩盖无效输入。 */
		/* 这里的字符串仅服务失败日志，实际筛选语义在随后 is_kfence_address 分支执行。 */
	case ALLOCATE_ANY:
		policy_name = "any";
		break;
	case ALLOCATE_LEFT:
		policy_name = "left";
		break;
	case ALLOCATE_RIGHT:
		/* RIGHT 名称对应对象起点邻接右侧 guard 的布局判定，实际由页对齐测试确认。 */
		policy_name = "right";
		break;
	case ALLOCATE_NONE:
		policy_name = "none";
		break;
	}

	kunit_info(test, "%s: size=%zu, gfp=%pGg, policy=%s, cache=%i\n", __func__, size, &gfp,
		   policy_name, !!test_cache);

	/*
	 * 100x the sample interval should be more than enough to ensure we get
	 * a KFENCE allocation eventually.
	 */
	timeout = jiffies + msecs_to_jiffies(100 * kfence_sample_interval);
	/* 上限是采样周期的 100 倍，防止配置/调度异常时 KUnit 无限占用 CPU。 */
	/*
	 * Especially for non-preemption kernels, ensure the allocation-gate
	 * timer can catch up: after @resched_after, every failed allocation
	 * attempt yields, to ensure the allocation-gate timer is scheduled.
	 */
	resched_after = jiffies + msecs_to_jiffies(kfence_sample_interval);
	do {
		/* 每轮只取得一个候选；无论是否 KFENCE 都必须在继续重试前归还。 */
		if (test_cache)
			alloc = kmem_cache_alloc(test_cache, gfp);
		else
			alloc = kmalloc(size, gfp);

		if (is_kfence_address(alloc)) {
			/* 命中后先验证 SLUB/memcg 依赖的索引辅助函数，再按 policy 接受或拒绝位置。 */
			/* slab 与 cache 必须由本次 alloc 推导，才能检查 sampled 对象也符合通用 slab 辅助接口。 */
			struct slab *slab = virt_to_slab(alloc);
			enum kmalloc_cache_type type = kmalloc_type(GFP_KERNEL, __kmalloc_token(size));
			struct kmem_cache *s = test_cache ?:
					kmalloc_caches[type][__kmalloc_index(size, false)];

			/*
			 * Verify that various helpers return the right values
			 * even for KFENCE objects; these are required so that
			 * memcg accounting works correctly.
			 */
			KUNIT_EXPECT_EQ(test, obj_to_index(s, slab, alloc), 0U);
			KUNIT_EXPECT_EQ(test, objs_per_slab(s, slab), 1);
			/* 位置策略只在确认对象来自 KFENCE 后判断；普通对象不能拿页对齐推断 guard 侧别。 */

			if (policy == ALLOCATE_ANY)
				return alloc;
			if (policy == ALLOCATE_LEFT && PAGE_ALIGNED(alloc))
				return alloc;
			if (policy == ALLOCATE_RIGHT && !PAGE_ALIGNED(alloc))
				return alloc;
		} else if (policy == ALLOCATE_NONE)
			/* NONE 明确要求非 KFENCE 对象，用来验证普通路径不会被错误采样。 */
			return alloc;

		test_free(alloc);
		/* 拒绝的候选不保留，避免测试自身耗尽 sampled pool 或污染后续报告。 */

		if (time_after(jiffies, resched_after))
		/* 条件让出只在等待采样 timer 后开始，兼顾快速命中与非抢占内核的定时器推进。 */
			cond_resched();
	} while (time_before(jiffies, timeout));

	KUNIT_ASSERT_TRUE_MSG(test, false, "failed to allocate from KFENCE");
	/* 断言失败会终止 case；NULL 仅满足编译器的不可达返回要求。 */
	return NULL; /* Unreachable. */
}

static void test_out_of_bounds_read(struct kunit *test)
{
	/* 左右两侧各越界读一次；READ_ONCE 强制真实访问，避免编译器删除应由 KFENCE 检出的 load。 */
	size_t size = 32;
	struct expect_report expect = {
		.type = KFENCE_ERROR_OOB,
		.fn = test_out_of_bounds_read,
		.is_write = false,
	};
	char *buf;

	setup_test_cache(test, size, 0, NULL);
	/* 测试开始前 cache 选择已固定，后续左右分配必须共享该后端以便比较 guard 放置。 */

	/*
	 * If we don't have our own cache, adjust based on alignment, so that we
	 * actually access guard pages on either side.
	 */
	if (!test_cache)
		/* kmalloc 对齐可在对象与 guard 之间留下空隙，先修正 size 才能让左右测试触及 guard。 */
		size = kmalloc_cache_alignment(size);

	/* Test both sides. */
	/* 两次访问各自等待报告匹配后释放，避免第一个对象状态影响另一侧 guard。 */

	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_LEFT);
	/* LEFT 布局把对象末端贴近右 guard，写入 size 偏移由 free 时的红区校验归类为 corruption。 */
	/* LEFT 使对象靠右放置，buf-1 即为左 guard 的首个非法字节。 */
	expect.addr = buf - 1;
	READ_ONCE(*expect.addr);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
	/* 匹配完成后才释放对象，确保报告地址仍可归因于本次受保护分配。 */
	test_free(buf);

	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_RIGHT);
	/* RIGHT 使对象靠左放置，buf+size 即为右 guard 的首个非法字节。 */
	expect.addr = buf + size;
	READ_ONCE(*expect.addr);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
	test_free(buf);
}

static void test_out_of_bounds_write(struct kunit *test)
{
	/* 业务背景：验证写越界的报告类型与读取路径不同。
	 * 入参：test 提供 KUnit 断言上下文。
	 * 注意事项：guard 访问后仍释放原始对象指针。
	 */
	/* 向 left guard 写入应立即报告 OOB；随后仍按原对象指针释放，避免测试制造额外 invalid-free。 */
	size_t size = 32;
	struct expect_report expect = {
		.type = KFENCE_ERROR_OOB,
		.fn = test_out_of_bounds_write,
		.is_write = true,
	};
	char *buf;

	setup_test_cache(test, size, 0, NULL);
	/* 单侧写测试无需调整 alignment，因为 LEFT 策略保证要访问的 guard 紧邻对象。 */
	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_LEFT);
	expect.addr = buf - 1;
	WRITE_ONCE(*expect.addr, 42);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
	test_free(buf);
}

static void test_use_after_free_read(struct kunit *test)
{
	/* 业务背景：验证 free 后 CPU load 的 UAF 分类。
	 * 出参：报告必须含本函数和释放对象地址。
	 * 注意事项：对象在首次 free 后已无调用者 ownership。
	 */
	/* 先释放再 READ_ONCE，验证 KFENCE 将失效对象访问分类为 UAF 而非一般 OOB。 */
	const size_t size = 32;
	struct expect_report expect = {
		.type = KFENCE_ERROR_UAF,
		.fn = test_use_after_free_read,
		.is_write = false,
	};

	setup_test_cache(test, size, 0, NULL);
	expect.addr = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);
	/* 记录地址后立即 free，后续 load 的唯一合法结果是 KFENCE UAF 诊断。 */
	test_free(expect.addr);
	READ_ONCE(*expect.addr);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

static void test_use_after_free_read_nofault(struct kunit *test)
{
	/* 业务背景：验证 nofault 探测不会制造诊断递归。
	 * 返回检查：copy helper 应返回 -EFAULT。
	 * 注意事项：预期没有 console 报告。
	 */
	/* nofault copy 的契约是返回 -EFAULT 且不触发 KFENCE 报告，验证异常探测路径不会递归诊断。 */
	const size_t size = 32;
	char *addr;
	char dst;
	int ret;

	setup_test_cache(test, size, 0, NULL);
	/* nofault 路径同样先取得 sampled 对象，确保 -EFAULT 来自 KFENCE protection 而非普通地址错误。 */
	addr = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);
	test_free(addr);
	/* Use after free with *_nofault() */
	ret = copy_from_kernel_nofault(&dst, addr, 1);
	KUNIT_EXPECT_EQ(test, ret, -EFAULT);
	KUNIT_EXPECT_FALSE(test, report_available());
	/* 普通路径断言无报告，确保本测试只把预期异常归因于随后指定的触发访问。 */
}

static void test_double_free(struct kunit *test)
{
	/* 业务背景：验证 KFENCE 记录释放状态并拒绝第二次归还。
	 * 出参：预期 INVALID_FREE 报告。
	 * 注意事项：第一次 free 才是真正的 ownership 转移。
	 */
	/* 第二次 free 保留原地址作为报告证据；无需第三次清理，因为第一次已完成对象 ownership 归还。 */
	const size_t size = 32;
	struct expect_report expect = {
		.type = KFENCE_ERROR_INVALID_FREE,
		.fn = test_double_free,
	};

	setup_test_cache(test, size, 0, NULL);
	/* 双重释放只要求 sampled 对象；报告匹配函数随后检查它精确指向本 test 函数。 */
	expect.addr = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);
	test_free(expect.addr);
	test_free(expect.addr); /* Double-free. */
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

static void test_invalid_addr_free(struct kunit *test)
{
	/* 业务背景：验证内部偏移地址不能冒充对象起点释放。
	 * 出参：偏移 free 报 INVALID_FREE。
	 * 注意事项：buf 本体必须仍由本用例正常释放。
	 */
	/* 偏移地址的 free 应报 invalid-free，但原始 buf 仍归测试所有，必须随后正常释放。 */
	const size_t size = 32;
	struct expect_report expect = {
		.type = KFENCE_ERROR_INVALID_FREE,
		.fn = test_invalid_addr_free,
	};
	char *buf;

	setup_test_cache(test, size, 0, NULL);
	/* 偏移 free 的地址仍落在对象附近，使分类测试专注于“非对象起点”而非任意坏地址。 */
	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);
	expect.addr = buf + 1; /* Free on invalid address. */
	test_free(expect.addr); /* Invalid address free. */
	test_free(buf); /* No error. */
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

static void test_corruption(struct kunit *test)
{
	/* 业务背景：验证 guard 字节的篡改延迟到 free 时检出。
	 * 出参：预期 CORRUPTION 而非即时 OOB。
	 * 注意事项：左右 guard 覆盖不同对象放置方向。
	 */
	/* guard 内容篡改在释放时校验，故预期为 corruption 而非访问时 OOB；两侧各覆盖一次。 */
	size_t size = 32;
	struct expect_report expect = {
		.type = KFENCE_ERROR_CORRUPTION,
		.fn = test_corruption,
	};
	char *buf;

	setup_test_cache(test, size, 0, NULL);

	/* Test both sides. */
	/* corruption 用释放触发校验，因此每次写后均先 free 再匹配报告。 */

	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_LEFT);
	expect.addr = buf + size;
	WRITE_ONCE(*expect.addr, 42);
	test_free(buf);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));

	/* RIGHT 布局则把对象起始贴近左 guard，验证两侧元数据均在释放路径被检查。 */
	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_RIGHT);
	expect.addr = buf - 1;
	WRITE_ONCE(*expect.addr, 42);
	test_free(buf);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * KFENCE is unable to detect an OOB if the allocation's alignment requirements
 * leave a gap between the object and the guard page. Specifically, an
 * allocation of e.g. 73 bytes is aligned on 8 and 128 bytes for SLUB or SLAB
 * respectively. Therefore it is impossible for the allocated object to
 * contiguously line up with the right guard page.
 *
 * However, we test that an access to memory beyond the gap results in KFENCE
 * detecting an OOB access.
 */
static void test_kmalloc_aligned_oob_read(struct kunit *test)
{
	/* 业务背景：区分 allocator alignment 空隙和真正 guard-page 越界。
	 * 出参：仅跨过 align 的读应报告 OOB。
	 * 注意事项：前两次访问刻意断言无报告。
	 */
	/* 对齐空隙内的访问不必命中 guard；跨越完整对齐粒度后才断言真实 OOB，避免错误测试 KFENCE 边界。 */
	const size_t size = 73;
	const size_t align = kmalloc_cache_alignment(size);
	/* size 与 align 同时保留：前者是请求边界，后者是 cache 实际布局边界。 */
	struct expect_report expect = {
		.type = KFENCE_ERROR_OOB,
		.fn = test_kmalloc_aligned_oob_read,
		.is_write = false,
	};
	char *buf;

	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_RIGHT);
	/* RIGHT 策略特意让对象左边不临 guard，先验证相邻负偏移不产生假阳性。 */

	/*
	 * The object is offset to the right, so there won't be an OOB to the
	 * left of it.
	 */
	READ_ONCE(*(buf - 1));
	KUNIT_EXPECT_FALSE(test, report_available());

	/*
	 * @buf must be aligned on @align, therefore buf + size belongs to the
	 * same page -> no OOB.
	 */
	READ_ONCE(*(buf + size));
	KUNIT_EXPECT_FALSE(test, report_available());

	/* 只有跨完整对齐空洞后才落入右 guard；将目标写入 expect 使报告地址可精确比对。 */
	/* Overflowing by @align bytes will result in an OOB. */
	expect.addr = buf + size + align;
	READ_ONCE(*expect.addr);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));

	test_free(buf);
}

static void test_kmalloc_aligned_oob_write(struct kunit *test)
{
	/* 业务背景：验证对齐空隙写入在释放校验阶段转化为 corruption。
	 * 出参：free 后才要求报告匹配。
	 * 注意事项：即时无报告是本用例的关键断言。
	 */
	/* 写入对齐空隙不会立即 fault，但 free 的 guard 校验必须把它识别为 corruption。 */
	const size_t size = 73;
	/* 本例故意不计算 align：对象右侧的首个空洞字节已足够验证延迟 corruption 检查。 */
	struct expect_report expect = {
		.type = KFENCE_ERROR_CORRUPTION,
		.fn = test_kmalloc_aligned_oob_write,
	};
	char *buf;

	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_RIGHT);
	/* 先读再写保留空洞的原值；随后的 false 断言证明写入没有在访问时直接 fault。 */
	/*
	 * The object is offset to the right, so we won't get a page
	 * fault immediately after it.
	 */
	expect.addr = buf + size;
	WRITE_ONCE(*expect.addr, READ_ONCE(*expect.addr) + 1);
	KUNIT_EXPECT_FALSE(test, report_available());
	test_free(buf);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Test cache shrinking and destroying with KFENCE. */
static void test_shrink_memcache(struct kunit *test)
{
	/* 业务背景：验证 shrink 不会破坏仍分配的 KFENCE 对象。
	 * 出参：全程不应生成报告。
	 * 注意事项：cache 由 case teardown 统一销毁。
	 */
	/* 缩 shrink 与 destroy 回归验证 KFENCE 对独立 cache 的对象/元数据不会留下悬挂状态。 */
	const size_t size = 32;
	/* shrink 在对象仍活着时执行；用例随后释放该对象以覆盖 cache 回收与对象回收的顺序。 */
	void *buf;

	setup_test_cache(test, size, 0, NULL);
	KUNIT_EXPECT_TRUE(test, test_cache);
	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);
	/* 此对象跨 shrink 存活，最终无报告同时证明 shrink 没有破坏其 KFENCE 元数据。 */
	kmem_cache_shrink(test_cache);
	test_free(buf);

	KUNIT_EXPECT_FALSE(test, report_available());
}

static void ctor_set_x(void *obj)
{
	/* cache ctor 写固定前缀，供后续用例确认 KFENCE 对象仍遵循 SLAB 构造器契约。 */
	/* Every object has at least 8 bytes. */
	memset(obj, 'x', 8);
}

/* Ensure that SL*B does not modify KFENCE objects on bulk free. */
static void test_free_bulk(struct kunit *test)
{
	/* 混合 KFENCE/普通对象批量释放后不应改写受保护对象或触发误报告；每轮 cache 独立销毁。 */
	int iter;

	for (iter = 0; iter < 5; iter++) {
		/* 每轮随机对象大小和 ctor 组合，防止只在单一 SLAB 布局下通过。 */
		const size_t size = setup_test_cache(test, get_random_u32_inclusive(8, 307),
						     0, (iter & 1) ? ctor_set_x : NULL);
		void *objects[] = {
			test_alloc(test, size, GFP_KERNEL, ALLOCATE_RIGHT),
			test_alloc(test, size, GFP_KERNEL, ALLOCATE_NONE),
			test_alloc(test, size, GFP_KERNEL, ALLOCATE_LEFT),
			test_alloc(test, size, GFP_KERNEL, ALLOCATE_NONE),
			test_alloc(test, size, GFP_KERNEL, ALLOCATE_NONE),
		};

		kmem_cache_free_bulk(test_cache, ARRAY_SIZE(objects), objects);
		/* bulk free 接收混合对象数组；无报告证明 KFENCE 对象未被批量路径误写。 */
		KUNIT_ASSERT_FALSE(test, report_available());
		test_cache_destroy();
	}
}

/* Test init-on-free works. */
static void test_init_on_free(struct kunit *test)
{
	/* INIT_ON_FREE 将释放对象清零；首次 UAF 仍应报告，之后仅验证清零而不依赖重复访问结果。 */
	const size_t size = 32;
	/* size 固定为小对象，逐字节模式可在一次报告后检查 allocator 的全对象清零承诺。 */
	struct expect_report expect = {
		.type = KFENCE_ERROR_UAF,
		.fn = test_init_on_free,
		.is_write = false,
	};
	int i;

	KFENCE_TEST_REQUIRES(test, IS_ENABLED(CONFIG_INIT_ON_FREE_DEFAULT_ON));
	/* Assume it hasn't been disabled on command line. */
	/* Kconfig 默认开启仍可由启动参数关闭，因此 skip 条件同时约束编译配置和实际运行状态。 */

	setup_test_cache(test, size, 0, NULL);
	expect.addr = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);
	for (i = 0; i < size; i++)
		/* 先写非零模式，释放后的逐字节检查才能区分真正 init-on-free 与旧内容残留。 */
		expect.addr[i] = i + 1;
	test_free(expect.addr);

	for (i = 0; i < size; i++) {
		/* 循环体在首次触发 UAF 后继续读取相同对象；报告快照只需匹配一次。 */
		/* 首次访问产生 UAF 诊断，后续访问只验证清零，避免页面再保护时使测试不稳定。 */
		/*
		 * This may fail if the page was recycled by KFENCE and then
		 * written to again -- this however, is near impossible with a
		 * default config.
		 */
		KUNIT_EXPECT_EQ(test, expect.addr[i], (char)0);

		if (!i) /* Only check first access to not fail test if page is ever re-protected. */
			KUNIT_EXPECT_TRUE(test, report_matches(&expect));
	}
}

/* Ensure that constructors work properly. */
/* 该测试只检验 ctor 写入约定的字节范围，不扩大对 allocator 初始化细节的假设。 */
static void test_memcache_ctor(struct kunit *test)
{
	/* 验证 KFENCE 采样对象也执行 cache ctor，free 后无非法报告。 */
	const size_t size = 32;
	char *buf;
	int i;

	setup_test_cache(test, size, 0, ctor_set_x);
	buf = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);

	for (i = 0; i < 8; i++)
		/* ctor 只约定前八字节，测试不把未初始化尾部误当成构造器 ABI。 */
		KUNIT_EXPECT_EQ(test, buf[i], (char)'x');

	test_free(buf);

	KUNIT_EXPECT_FALSE(test, report_available());
}

/* Test that memory is zeroed if requested. */
/* 只有重新获得同一地址才比较内容，避免把不同对象的未初始化状态当作失败。 */
static void test_gfpzero(struct kunit *test)
{
	/* 反复取得同一 KFENCE 地址后验证 __GFP_ZERO 覆盖旧数据；达到对象数上限时警告并安全退出。 */
	const size_t size = PAGE_SIZE; /* PAGE_SIZE so we can use ALLOCATE_ANY. */
	char *buf1, *buf2;
	int i;

	/* Skip if we think it'd take too long. */
	/* 采样周期过大时地址复用的概率循环会拖慢 suite，跳过比把环境时序误判为功能失败更可靠。 */
	KFENCE_TEST_REQUIRES(test, kfence_sample_interval <= 100);

	setup_test_cache(test, size, 0, NULL);
	buf1 = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);
	/* 先污染整个页；只有后续重获同地址时，零化断言才有意义。 */
	for (i = 0; i < size; i++)
		buf1[i] = i + 1;
	test_free(buf1);

	/* Try to get same address again -- this can take a while. */
	for (i = 0;; i++) {
		/* 地址复用是概率事件，循环受对象池大小和 kthread stop 双重限制。 */
		/* 每次未复用都立即归还，确保尝试本身不会降低下次选回同一对象的机会。 */
		buf2 = test_alloc(test, size, GFP_KERNEL | __GFP_ZERO, ALLOCATE_ANY);
		if (buf1 == buf2)
			break;
		/* 非同一地址的对象不读取其内容，只归还并继续等待目标对象被重新采样。 */
		test_free(buf2);

		if (kthread_should_stop() || (i == CONFIG_KFENCE_NUM_OBJECTS)) {
			kunit_warn(test, "giving up ... cannot get same object back\n");
			return;
		}
		cond_resched();
	}

	/* 复用成功后才读取新对象内容；此前 buf2 的任何内容均不属于零化断言范围。 */
	for (i = 0; i < size; i++)
		KUNIT_EXPECT_EQ(test, buf2[i], (char)0);

	test_free(buf2);

	KUNIT_EXPECT_FALSE(test, report_available());
}

static void test_invalid_access(struct kunit *test)
{
	/* 直接访问 KFENCE pool 的非对象位置必须分类为 INVALID，地址常量避免依赖分配器采样。 */
	const struct expect_report expect = {
		.type = KFENCE_ERROR_INVALID,
		.fn = test_invalid_access,
		.addr = &__kfence_pool[10],
		.is_write = false,
	};

	READ_ONCE(__kfence_pool[10]);
	/* 直接 pool 访问绕过 allocator 元数据，因此预期 INVALID 而非 UAF/OOB。 */
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Test SLAB_TYPESAFE_BY_RCU works. */
/* RCU 用例把逻辑 free 与物理回收拆开，正是 KFENCE 不应过早报告的并发边界。 */
static void test_memcache_typesafe_by_rcu(struct kunit *test)
{
	/* TYPESAFE_BY_RCU 下 read-side 临界区内对象仍可读且不应报告；rcu_barrier 后才验证 UAF。 */
	const size_t size = 32;
	struct expect_report expect = {
		.type = KFENCE_ERROR_UAF,
		.fn = test_memcache_typesafe_by_rcu,
		.is_write = false,
	};

	setup_test_cache(test, size, SLAB_TYPESAFE_BY_RCU, NULL);
	/* 该 flag 改变释放后的可访问窗口，必须使用自建 cache 才能避免 kmalloc 默认语义干扰。 */
	KUNIT_EXPECT_TRUE(test, test_cache); /* Want memcache. */

	expect.addr = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY);
	*expect.addr = 42;

	rcu_read_lock();
	/* read-side 锁延后实际回收；此处访问验证 TYPESAFE_BY_RCU 的允许窗口。 */
	test_free(expect.addr);
	KUNIT_EXPECT_EQ(test, *expect.addr, (char)42);
	/*
	 * Up to this point, memory should not have been freed yet, and
	 * therefore there should be no KFENCE report from the above access.
	 */
	rcu_read_unlock();
	/* 解锁仅结束当前读侧临界区，尚不足以证明所有延迟回调已经完成。 */

	/* Above access to @expect.addr should not have generated a report! */
	/* 读侧锁内的访问是允许窗口；这里的无报告断言必须在 rcu_barrier 前完成。 */
	KUNIT_EXPECT_FALSE(test, report_available());

	/* Only after rcu_barrier() is the memory guaranteed to be freed. */
	/* rcu_barrier 等待已排队回调，建立“允许旧对象读取”到“必须报 UAF”的明确转换点。 */
	rcu_barrier();
	/* barrier 等待所有 RCU callback，之后同一地址必然不再是可读旧对象。 */

	/* Expect use-after-free. */
	KUNIT_EXPECT_EQ(test, *expect.addr, (char)42);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Test krealloc(). */
/* krealloc 改变对象 ownership；旧指针仅用于最后的 UAF 验证，不能再次释放。 */
static void test_krealloc(struct kunit *test)
{
	/* grow/shrink/zero-size 三阶段保持前缀内容；最终访问最初对象证明 krealloc 已释放其 KFENCE backing。 */
	const size_t size = 32;
	const struct expect_report expect = {
		.type = KFENCE_ERROR_UAF,
		.fn = test_krealloc,
		.addr = test_alloc(test, size, GFP_KERNEL, ALLOCATE_ANY),
		.is_write = false,
	};
	char *buf = expect.addr;
	int i;

	/* expect 保存第一次分配的地址；后续 buf 会随 krealloc 转移，二者不能混为同一 ownership。 */

	KUNIT_EXPECT_FALSE(test, test_cache);
	KUNIT_EXPECT_EQ(test, ksize(buf), size); /* Precise size match after KFENCE alloc. */
	for (i = 0; i < size; i++)
		buf[i] = i + 1;

	/* grow 后填充扩展区域，shrink 才能同时检验旧前缀和新的保留边界。 */

	/* Check that we successfully change the size. */
	/* 扩容后的 ksize 只保证不小于请求值，测试随后检验旧数据复制而非依赖精确 cache 尺寸。 */
	buf = krealloc(buf, size * 3, GFP_KERNEL); /* Grow. */
	/* 扩容可迁移出 KFENCE，但必须保留原前缀数据；返回指针成为新的 ownership。 */
	/* Note: Might no longer be a KFENCE alloc. */
	/* 因此后续数据检查使用 krealloc 返回的新对象，不能再把它当作仍受 KFENCE guard 的对象。 */
	KUNIT_EXPECT_GE(test, ksize(buf), size * 3);
	for (i = 0; i < size; i++)
		KUNIT_EXPECT_EQ(test, buf[i], (char)(i + 1));
	for (; i < size * 3; i++) /* Fill to extra bytes. */
		buf[i] = i + 1;

	buf = krealloc(buf, size * 2, GFP_KERNEL); /* Shrink. */
	KUNIT_EXPECT_GE(test, ksize(buf), size * 2);
	for (i = 0; i < size * 2; i++)
		KUNIT_EXPECT_EQ(test, buf[i], (char)(i + 1));

	buf = krealloc(buf, 0, GFP_KERNEL); /* Free. */
	/* size 0 的 krealloc 完成最终释放并返回 ZERO_SIZE_PTR；随后访问旧地址应报告 UAF。 */
	KUNIT_EXPECT_EQ(test, (unsigned long)buf, (unsigned long)ZERO_SIZE_PTR);
	KUNIT_ASSERT_FALSE(test, report_available()); /* No reports yet! */

	READ_ONCE(*expect.addr); /* Ensure krealloc() actually freed earlier KFENCE object. */
	KUNIT_ASSERT_TRUE(test, report_matches(&expect));
}

/* Test that some objects from a bulk allocation belong to KFENCE pool. */
/* 批量 API 的观察目标是至少一个 sampled 对象，而非要求整个批次均来自 KFENCE。 */
static void test_memcache_alloc_bulk(struct kunit *test)
{
	/* 批量分配必须最终包含 sampled KFENCE 对象；循环 cond_resched 让 static-key/timer 有机会推进。 */
	const size_t size = 32;
	bool pass = false;
	unsigned long timeout;

	setup_test_cache(test, size, 0, NULL);
	KUNIT_EXPECT_TRUE(test, test_cache); /* Want memcache. */
	/*
	 * 100x the sample interval should be more than enough to ensure we get
	 * a KFENCE allocation eventually.
	 */
	timeout = jiffies + msecs_to_jiffies(100 * kfence_sample_interval);
	do {
		/* 每批原子分配后扫描 100 个对象；无命中时整批归还，不能泄漏普通对象。 */
		/* objects 是栈上批次容器；alloc_bulk 失败时没有对象可释放，直接重试即可。 */
		void *objects[100];
		int i;

		if (!kmem_cache_alloc_bulk(test_cache, GFP_ATOMIC,
				ARRAY_SIZE(objects), objects))
			continue;
		/* 成功批次逐项查找即可，pass 一旦置位仍须走统一的整批释放路径。 */
		for (i = 0; i < ARRAY_SIZE(objects); i++) {
			if (is_kfence_address(objects[i])) {
				pass = true;
				break;
			}
		}
		kmem_cache_free_bulk(test_cache, ARRAY_SIZE(objects), objects);
		/* 即使已命中也归还整批，保证 test_exit 不依赖 allocator 隐式清理。 */
		/*
		 * kmem_cache_alloc_bulk() disables interrupts, and calling it
		 * in a tight loop may not give KFENCE a chance to switch the
		 * static branch. Call cond_resched() to let KFENCE chime in.
		 */
		cond_resched();
	} while (!pass && time_before(jiffies, timeout));

	KUNIT_EXPECT_TRUE(test, pass);
	KUNIT_EXPECT_FALSE(test, report_available());
}

/*
 * KUnit does not provide a way to provide arguments to tests, and we encode
 * additional info in the name. Set up 2 tests per test case, one using the
 * default allocator, and another using a custom memcache (suffix '-memcache').
 */
#define KFENCE_KUNIT_CASE(test_name)						\
	/* 名称后缀编码 allocator 模式，同一函数运行两次。 */ \
	{ .run_case = test_name, .name = #test_name },				\
	{ .run_case = test_name, .name = #test_name "-memcache" }

static struct kunit_case kfence_test_cases[] = {
	/* 前半由宏展开为 kmalloc/memcache 成对路径；后半仅适用于其显式 allocator 前提。 */
	KFENCE_KUNIT_CASE(test_out_of_bounds_read),
	KFENCE_KUNIT_CASE(test_out_of_bounds_write),
	KFENCE_KUNIT_CASE(test_use_after_free_read),
	KFENCE_KUNIT_CASE(test_use_after_free_read_nofault),
	KFENCE_KUNIT_CASE(test_double_free),
	KFENCE_KUNIT_CASE(test_invalid_addr_free),
	KFENCE_KUNIT_CASE(test_corruption),
	KFENCE_KUNIT_CASE(test_free_bulk),
	KFENCE_KUNIT_CASE(test_init_on_free),
	/* 下列 case 依赖固定布局、RCU 或专用分配标志，故不通过双路径宏扩展。 */
	KUNIT_CASE(test_kmalloc_aligned_oob_read),
	KUNIT_CASE(test_kmalloc_aligned_oob_write),
	KUNIT_CASE(test_shrink_memcache),
	KUNIT_CASE(test_memcache_ctor),
	KUNIT_CASE(test_invalid_access),
	KUNIT_CASE(test_gfpzero),
	/* 生命周期与 realloc 用例需各自断言延迟回收或 ownership 转移，仍共用统一 test_init/test_exit。 */
	KUNIT_CASE(test_memcache_typesafe_by_rcu),
	KUNIT_CASE(test_krealloc),
	KUNIT_CASE(test_memcache_alloc_bulk),
	{},
};

/* ===== End test cases ===== */

static int test_init(struct kunit *test)
{
	/* 每个 case 前清 console 两行快照并根据名称设置 allocator 私有模式；无 pool 表示 KFENCE 未可用。 */
	unsigned long flags;
	int i;

	if (!__kfence_pool)
		return -EINVAL;

	spin_lock_irqsave(&observed.lock, flags);
	/* case 初始化在锁内同时清字符串与计数，读者不会把旧标题和新地址拼成报告。 */
	/* 先清槽内容再置 nlines 为零，保持 report_available 与实际可读文本的发布顺序。 */
	for (i = 0; i < ARRAY_SIZE(observed.lines); i++)
		observed.lines[i][0] = '\0';
	observed.nlines = 0;
	spin_unlock_irqrestore(&observed.lock, flags);

	/* Any test with 'memcache' in its name will want a memcache. */
	/* 名称编码使同一 run_case 的 setup 分支可复用，test_exit 据此销毁仅在本例创建的 cache。 */
	if (strstr(test->name, "memcache"))
		test->priv = TEST_PRIV_WANT_MEMCACHE;
	else
		test->priv = NULL;

	return 0;
}

static void test_exit(struct kunit *test)
{
	/* case teardown 无条件销毁可能创建的 cache，隔离下一用例的 slab 状态与 memcg 计费。 */
	test_cache_destroy();
}

static int kfence_suite_init(struct kunit_suite *suite)
{
	/* suite 生命周期注册 console trace probe；失败会阻止所有依赖报告文本的断言启动。 */
	register_trace_console(probe_console, NULL);
	/* 注册后整个 suite 才能消费 console 报告；probe 无私有数据，生命周期由 suite exit 配对。 */
	return 0;
}

/* suite 注册完成后，所有 case 才能把异步 console 报告转换为同步 KUnit 断言。 */

static void kfence_suite_exit(struct kunit_suite *suite)
{
	/* 先注销 probe，再等待所有已在执行的 trace 回调退出，防止卸载后访问本文件静态 observed。 */
	/* 顺序不可颠倒：先禁止新回调，再同步旧回调，最后才让 suite 静态状态结束生命周期。 */
	unregister_trace_console(probe_console, NULL);
	/* synchronize 等待已注册回调退场，随后静态测试数据才可随模块卸载失效。 */
	tracepoint_synchronize_unregister();
}

	/* suite 将 case 初始化、每例回收和 tracepoint 生命周期显式配对，KUnit 据此调度整个测试集。 */
static struct kunit_suite kfence_test_suite = {
	.name = "kfence",
	.test_cases = kfence_test_cases,
	/* init/exit 是每个 case 的状态边界，suite_init/exit 则包住整套 tracepoint 注册。 */
	.init = test_init,
	.exit = test_exit,
	.suite_init = kfence_suite_init,
	.suite_exit = kfence_suite_exit,
};

kunit_test_suites(&kfence_test_suite);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Alexander Potapenko <glider@google.com>, Marco Elver <elver@google.com>");
MODULE_DESCRIPTION("kfence unit test suite");
