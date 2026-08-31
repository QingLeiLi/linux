// SPDX-License-Identifier: GPL-2.0
/*
 * Lockless hierarchical page accounting & limiting
 *
 * Copyright (C) 2014 Red Hat, Inc., Johannes Weiner
 */

/*
 * page_counter 为 memcg、hugetlb cgroup 等提供无锁层级页数账本：每次 charge 从叶向根
 * 更新 usage，try_charge 在首个 max 超限祖先回滚；memory.min/low 则把子树实际使用量
 * 汇总到父节点，并在自顶向下 reclaim 遍历中换算为有效保护 emin/elow。
 */

#include <linux/page_counter.h>
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/bug.h>
#include <asm/page.h>

/*
 * 判断当前计数器树是否维护 min/low 保护派生统计。
 * 业务背景：只有启用保护的主 memory counter 需要承担额外原子更新，其他账本走轻路径。
 * 入参：@c 是借用、已初始化的计数器。出参/返回：返回 protection_support 快照；无副作用。
 * 注意事项：字段在初始化后保持稳定，函数无锁、不睡眠，不取得 counter 生命周期引用。
 */
static bool track_protection(struct page_counter *c)
{
	return c->protection_support;
}

/*
 * 根据 @c 当前 usage 刷新其已实际使用的 min/low 额度，并把差量汇总给父节点。
 * 业务背景：有效保护分配不能按声明值盲分，必须知道每个孩子真正使用了多少保护；charge、
 * uncharge 和保护设置变化都会调用本函数维护 parent->children_*_usage。
 * 入参：@c 是借用计数器；@usage 是刚完成原子账变后的页数快照，必须非负。无 ownership
 * 转移。出参/返回：无直接返回；可能更新 c->min_usage/low_usage 及父级两个 children 汇总。
 * 注意事项：无锁、不睡眠，允许并发调用；atomic xchg 取得本 CPU 提交前的真实旧值，只有
 * 实际差量加到父级，避免两个更新者重复累计。根节点无 parent，直接返回。
 */
static void propagate_protected_usage(struct page_counter *c,
				      unsigned long usage)
{
	/* protected/old_protected 分别是目标快照和原子交换前值；delta 可正可负，以页为单位。 */
	unsigned long protected, old_protected;
	long delta;

	/* 根没有上级 children 汇总消费者，不需要维护自己的 min_usage 派生值。 */
	if (!c->parent)
		return;

	/* 阶段 1：实际 min 使用量最多是 usage，也不能超过无锁读取的声明 min。 */
	protected = min(usage, READ_ONCE(c->min));
	old_protected = atomic_long_read(&c->min_usage);
	/* 快照看似变化时用 xchg 串行提交；delta 必须以交换返回值重新计算。 */
	if (protected != old_protected) {
		old_protected = atomic_long_xchg(&c->min_usage, protected);
		delta = protected - old_protected;
		if (delta)
			atomic_long_add(delta, &c->parent->children_min_usage);
	}

	/* 阶段 2：对可回收的 low 保护执行同一协议，维护独立父级汇总。 */
	protected = min(usage, READ_ONCE(c->low));
	old_protected = atomic_long_read(&c->low_usage);
	if (protected != old_protected) {
		old_protected = atomic_long_xchg(&c->low_usage, protected);
		delta = protected - old_protected;
		if (delta)
			atomic_long_add(delta, &c->parent->children_low_usage);
	}
}

/**
 * page_counter_cancel - take pages out of the local counter
 * @counter: counter
 * @nr_pages: number of pages to cancel
 */
/*
 * 仅从一个本地计数器撤销 @nr_pages，不自动遍历祖先。
 * 业务背景：try_charge 回滚已成功祖先时需精确撤销各层，层级 uncharge 也复用此原子原语。
 * 入参：@counter 是借用的目标计数器；@nr_pages 是要扣除的基页数，可为 0。
 * 出参/返回：无直接返回；usage 原子减少，启用保护时同步父级保护汇总。发现 underflow
 * 会 WARN 并把 usage 修正为 0，避免负值继续污染后续保护计算。
 * 注意事项：无锁、不睡眠；调用者必须保证逻辑 charge/uncharge 配对。underflow 修正只是
 * 容错诊断，不能恢复丢失的账目，也不修改任何祖先 usage。
 */
void page_counter_cancel(struct page_counter *counter, unsigned long nr_pages)
{
	/* new 是原子扣减后的有符号页数，用于检测 unsigned 无法表达的欠账。 */
	long new;

	new = atomic_long_sub_return(nr_pages, &counter->usage);
	/* More uncharges than charges? */
	/* uncharge 多于 charge 表示调用者账本错误；只告警一次并钳制到零。 */
	if (WARN_ONCE(new < 0, "page_counter underflow: %ld nr_pages=%lu\n",
		      new, nr_pages)) {
		new = 0;
		atomic_long_set(&counter->usage, new);
	}
	/* usage 最终值确定后再传播，父级 children 汇总不得看到负保护量。 */
	if (track_protection(counter))
		propagate_protected_usage(counter, new);
}

/**
 * page_counter_charge - hierarchically charge pages
 * @counter: counter
 * @nr_pages: number of pages to charge
 *
 * NOTE: This does not consider any configured counter limits.
 */
/*
 * 从叶计数器到根无条件增加层级 usage。
 * 业务背景：调用者已在别处预留额度、迁移既有账目或根本不需要限额检查时使用；需要拒绝
 * 超限的新分配必须调用 page_counter_try_charge()，本函数不会查看 max。
 * 入参：@counter 是借用的叶计数器；@nr_pages 是增加的基页数。出参/返回：无直接返回；
 * 叶及全部祖先 usage 增加，保护汇总和两个峰值 watermark 尽力更新。
 * 注意事项：无锁、不睡眠；计数原子精确，watermark 故意允许竞态近似。调用者负责防止
 * 溢出并保证未来层级 uncharge；配置的 high/max 都不会阻止本次提交。
 */
void page_counter_charge(struct page_counter *counter, unsigned long nr_pages)
{
	/* c 是当前祖先游标；protection 由叶树初始化属性决定，整条链共用该策略。 */
	struct page_counter *c;
	bool protection = track_protection(counter);

	/* 阶段 1：从叶向根逐层提交，使每个祖先 usage 都包含后代页。 */
	for (c = counter; c; c = c->parent) {
		long new;

		/* add_return 给出本层提交后的精确值，供保护和峰值派生统计使用。 */
		new = atomic_long_add_return(nr_pages, &c->usage);
		if (protection)
			propagate_protected_usage(c, new);
		/*
		 * This is indeed racy, but we can live with some
		 * inaccuracy in the watermark.
		 *
		 * Notably, we have two watermarks to allow for both a globally
		 * visible peak and one that can be reset at a smaller scope.
		 *
		 * Since we reset both watermarks when the global reset occurs,
		 * we can guarantee that watermark >= local_watermark, so we
		 * don't need to do both comparisons every time.
		 *
		 * On systems with branch predictors, the inner condition should
		 * be almost free.
		 */
		/*
		 * 峰值更新存在并发覆盖，允许少量不精确，因为只用于统计。维护 global watermark
		 * 和可局部 reset 的 local_watermark 两份；全局 reset 同时重置二者，因此正常有
		 * watermark >= local_watermark。只在超过 local 时再比较 global，分支预测下常见
		 * 不增长路径成本很低。
		 */
		if (new > READ_ONCE(c->local_watermark)) {
			WRITE_ONCE(c->local_watermark, new);
			if (new > READ_ONCE(c->watermark))
				WRITE_ONCE(c->watermark, new);
		}
	}
}

/**
 * page_counter_try_charge - try to hierarchically charge pages
 * @counter: counter
 * @nr_pages: number of pages to charge
 * @fail: points first counter to hit its limit, if any
 *
 * Returns %true on success, or %false and @fail if the counter or one
 * of its ancestors has hit its configured limit.
 */
/*
 * 尝试从叶到根层级计费，并在首个 max 超限处完整回滚已提交层级。
 * 业务背景：memcg/hugetlb 新资源分配用它检查整条祖先约束；成功后账目即已提交，失败者
 * 通过 @fail 知道应在哪个 cgroup 回收、OOM 或报告事件。
 * 入参：@counter 是借用叶计数器；@nr_pages 是请求基页数；@fail 是非空输出指针，失败时
 * 写入首个超限 counter 的借用指针，成功时内容不保证改变。
 * 出参/返回：全层成功返回 true，并更新 usage/保护/峰值；失败返回 false，超限层的试探
 * 增量及更低已提交层全部撤销，最终层级 usage 回到调用前，failcnt 可近似增加。
 * 注意事项：无锁、不睡眠；使用先加后查与 set_max 的屏障协议。瞬时试探值可能让并发小额
 * charge 提前失败，但误差受本次大额请求大小限制；调用者不能在 false 后再 uncharge。
 */
bool page_counter_try_charge(struct page_counter *counter,
			     unsigned long nr_pages,
			     struct page_counter **fail)
{
	/* c 遍历祖先；两项策略由叶计数器固定，避免每层重复判断配置。 */
	struct page_counter *c;
	bool protection = track_protection(counter);
	bool track_failcnt = counter->track_failcnt;

	/* 阶段 1：逐层试探提交；一旦某层超限，转入账目回滚。 */
	for (c = counter; c; c = c->parent) {
		long new;
		/*
		 * Charge speculatively to avoid an expensive CAS.  If
		 * a bigger charge fails, it might falsely lock out a
		 * racing smaller charge and send it into reclaim
		 * early, but the error is limited to the difference
		 * between the two sizes, which is less than 2M/4M in
		 * case of a THP locking out a regular page charge.
		 *
		 * The atomic_long_add_return() implies a full memory
		 * barrier between incrementing the count and reading
		 * the limit.  When racing with page_counter_set_max(),
		 * we either see the new limit or the setter sees the
		 * counter has changed and retries.
		 */
		/*
		 * 先原子加再读 max，省去昂贵 CAS 循环。大请求最终失败前的瞬时占用可能
		 * 暂时挡住并发小请求并使其提前回收；误差至多两者大小差，例如 THP 与
		 * 普通页竞争时小于约 2M/4M。
		 *
		 * atomic_long_add_return 含完整屏障：与 set_max 的 read-xchg-read 配对。
		 * 要么本路径看到新 limit，要么 setter 看到已改变 usage 并重试，避免二者
		 * 同时认为“旧 usage 配新 limit”安全。
		 */
		new = atomic_long_add_return(nr_pages, &c->usage);
		if (new > c->max) {
			/* 超过当前层硬上限：先撤销本层试探增量，本层保护/峰值尚未更新。 */
			atomic_long_sub(nr_pages, &c->usage);
			/*
			 * This is racy, but we can live with some
			 * inaccuracy in the failcnt which is only used
			 * to report stats.
			 */
			/* failcnt 只作统计，允许并发非原子增量丢失；data_race 明确这一选择。 */
			if (track_failcnt)
				data_race(c->failcnt++);
			/* 发布首个失败祖先，供回滚终点和外层回收决策共同使用。 */
			*fail = c;
			goto failed;
		}
		if (protection)
			propagate_protected_usage(c, new);

		/* see comment on page_counter_charge */
		/* 峰值同无条件 charge：只提供近似统计，不参与 max 正确性。 */
		if (new > READ_ONCE(c->local_watermark)) {
			WRITE_ONCE(c->local_watermark, new);
			if (new > READ_ONCE(c->watermark))
				WRITE_ONCE(c->watermark, new);
		}
	}
	/* 根也已成功提交，调用者现在拥有完整层级额度。 */
	return true;

failed:
	/* 失败层已就地撤销；从叶走到但不包含 *fail，逆转此前成功的各层及保护统计。 */
	for (c = counter; c != *fail; c = c->parent)
		page_counter_cancel(c, nr_pages);

	return false;
}

/**
 * page_counter_uncharge - hierarchically uncharge pages
 * @counter: counter
 * @nr_pages: number of pages to uncharge
 */
/*
 * 从叶到根层级撤销 @nr_pages 账目。
 * 业务背景：资源最终释放时必须对成功 try_charge/charge 的整条祖先链对称扣减。
 * 入参：@counter 是借用叶计数器；@nr_pages 是释放的基页数。出参/返回：无直接返回；
 * 每层 usage 与保护派生统计同步减少。注意事项：无锁、不睡眠；必须与先前层级 charge
 * 精确配对，任一层欠账会由 page_counter_cancel WARN 并钳制，但无法恢复账本一致性。
 */
void page_counter_uncharge(struct page_counter *counter, unsigned long nr_pages)
{
	/* c 从叶走到根，逐层复用只改本地计数的 cancel 原语。 */
	struct page_counter *c;

	for (c = counter; c; c = c->parent)
		page_counter_cancel(c, nr_pages);
}

/**
 * page_counter_set_max - set the maximum number of pages allowed
 * @counter: counter
 * @nr_pages: limit to set
 *
 * Returns 0 on success, -EBUSY if the current number of pages on the
 * counter already exceeds the specified limit.
 *
 * The caller must serialize invocations on the same counter.
 */
/*
 * 并发 charge 期间安全地设置硬上限 max，且绝不把上限降到当前 usage 以下。
 * 业务背景：cgroup 文件写 max 与无锁 try_charge 竞争；read-xchg-read 协议使 setter 与
 * 先加后查的 charger 至少一方观察到对方更新，避免已超限却双方都成功。
 * 入参：@counter 是借用目标计数器；@nr_pages 是新硬上限页数。
 * 出参/返回：提交成功返回 0；读取时 usage 已高于请求值返回 -EBUSY。成功只修改 max，
 * 不回收现有页；重试时会先恢复 old max，无部分提交残留。
 * 注意事项：调用者必须串行同一 counter 的多个 setter；函数可 cond_resched 因而可睡眠。
 * 与 charge 的正确性依赖 xchg 完整屏障，不可改成无序普通写。
 */
int page_counter_set_max(struct page_counter *counter, unsigned long nr_pages)
{
	/* 阶段 1：并发 usage 增长可能迫使循环恢复旧值并让出 CPU 后重试。 */
	for (;;) {
		/* old 是交换前上限，usage 是交换前观察到的页数快照。 */
		unsigned long old;
		long usage;

		/*
		 * Update the limit while making sure that it's not
		 * below the concurrently-changing counter value.
		 *
		 * The xchg implies two full memory barriers before
		 * and after, so the read-swap-read is ordered and
		 * ensures coherency with page_counter_try_charge():
		 * that function modifies the count before checking
		 * the limit, so if it sees the old limit, we see the
		 * modified counter and retry.
		 */
		/*
		 * 先读 usage，确认请求上限当前可容纳；xchg 在前后提供完整屏障，再读 usage。
		 * try_charge 先改计数再查上限：若它看到旧上限，本 setter 必看到增长并重试；
		 * 若 setter 未见增长而提交，新 charge 必看到新上限。
		 */
		usage = page_counter_read(counter);

		/* 已有占用高于请求值时不强制降限或回收，直接要求调用者稍后重试。 */
		if (usage > nr_pages)
			return -EBUSY;

		old = xchg(&counter->max, nr_pages);

		/* usage 未增长，或本次是放宽上限时，无需担心并发 charge 越过更低边界。 */
		if (page_counter_read(counter) <= usage || nr_pages >= old)
			return 0;

		/* 收紧期间 usage 增长：恢复旧上限，保留 charger 的合法结果，再调度后重试。 */
		counter->max = old;
		cond_resched();
	}
}

/**
 * page_counter_set_min - set the amount of protected memory
 * @counter: counter
 * @nr_pages: value to set
 *
 * The caller must serialize invocations on the same counter.
 */
/*
 * 更新 memory.min 声明并沿祖先链刷新实际保护汇总。
 * 业务背景：min 是硬回收保护，修改叶值会改变本节点 min_usage 以及父节点看到的 children
 * 保护总量，最终供自顶向下 effective_protection 计算。
 * 入参：@counter 是借用目标；@nr_pages 是新 min 基页数。出参/返回：无直接返回；min
 * 发布后，从该节点到根按各自 usage 重新传播。注意事项：调用者串行 setter；无锁、不睡眠，
 * charge/uncharge 可并发，派生值通过原子差量最终收敛。
 */
void page_counter_set_min(struct page_counter *counter, unsigned long nr_pages)
{
	/* c 遍历受本次保护声明影响的祖先链。 */
	struct page_counter *c;

	/* 先发布新声明，后续传播的 READ_ONCE 才能按新阈值计算。 */
	WRITE_ONCE(counter->min, nr_pages);

	for (c = counter; c; c = c->parent)
		propagate_protected_usage(c, atomic_long_read(&c->usage));
}

/**
 * page_counter_set_low - set the amount of protected memory
 * @counter: counter
 * @nr_pages: value to set
 *
 * The caller must serialize invocations on the same counter.
 */
/*
 * 更新 memory.low 声明并沿祖先链刷新实际保护汇总。
 * 业务背景：low 是可在极端压力下突破的回收保护，但层级分摊账法与 min 独立同构。
 * 入参：@counter 是借用目标；@nr_pages 是新 low 基页数。出参/返回：无直接返回；low
 * 与 children_low_usage 派生值更新。注意事项：调用者串行 setter；无锁、不睡眠，并发
 * usage 变化由原子交换差量使统计收敛。
 */
void page_counter_set_low(struct page_counter *counter, unsigned long nr_pages)
{
	/* c 是从目标到根的传播游标。 */
	struct page_counter *c;

	/* 发布声明后再重算，避免传播继续使用旧 low。 */
	WRITE_ONCE(counter->low, nr_pages);

	for (c = counter; c; c = c->parent)
		propagate_protected_usage(c, atomic_long_read(&c->usage));
}

/**
 * page_counter_memparse - memparse() for page counter limits
 * @buf: string to parse
 * @max: string meaning maximum possible value
 * @nr_pages: returns the result in number of pages
 *
 * Returns -EINVAL, or 0 and @nr_pages on success.  @nr_pages will be
 * limited to %PAGE_COUNTER_MAX.
 */
/*
 * 把 cgroup 文本限额解析并换算为 page_counter 使用的基页数。
 * 业务背景：memory/hugetlb 控制文件允许“max”或带 K/M/G 后缀的字节数，本函数统一处理
 * 哨兵、完整字符串校验、页粒度向下取整与体系结构安全上限。
 * 入参：@buf 是借用且 NUL 结尾的输入字符串；@max 是表示无限值的精确关键字（v2 常为
 * "max"，v1 可为 "-1"）；@nr_pages 是非空输出指针，失败时内容不保证更新。
 * 出参/返回：成功写入页数并返回 0；尾随非法字符返回 -EINVAL。数值超过可表示范围时
 * 饱和到 PAGE_COUNTER_MAX，不返回溢出错误；不足一页的字节数向下取整为 0。
 * 注意事项：不睡眠、不持引用；memparse 接受标准二进制单位后缀，只有 *end=='\0' 才接受。
 */
int page_counter_memparse(const char *buf, const char *max,
			  unsigned long *nr_pages)
{
	/* end 指向 memparse 未消费处；bytes 用 u64 避免常见大字节值在换页前截断。 */
	char *end;
	u64 bytes;

	/* 阶段 1：无限关键字不走数值解析，直接使用架构定义的最大安全 usage。 */
	if (!strcmp(buf, max)) {
		*nr_pages = PAGE_COUNTER_MAX;
		return 0;
	}

	/* 阶段 2：解析数值/单位，并拒绝空格或其他任何尾随字符。 */
	bytes = memparse(buf, &end);
	if (*end != '\0')
		return -EINVAL;

	/* 字节向下换算基页，再饱和，保证后续 atomic_long_t 计数和乘回字节不越界。 */
	*nr_pages = min(bytes / PAGE_SIZE, (u64)PAGE_COUNTER_MAX);

	return 0;
}


#if IS_ENABLED(CONFIG_MEMCG) || IS_ENABLED(CONFIG_CGROUP_DMEM)
/*
 * This function calculates an individual page counter's effective
 * protection which is derived from its own memory.min/low, its
 * parent's and siblings' settings, as well as the actual memory
 * distribution in the tree.
 *
 * The following rules apply to the effective protection values:
 *
 * 1. At the first level of reclaim, effective protection is equal to
 *    the declared protection in memory.min and memory.low.
 *
 * 2. To enable safe delegation of the protection configuration, at
 *    subsequent levels the effective protection is capped to the
 *    parent's effective protection.
 *
 * 3. To make complex and dynamic subtrees easier to configure, the
 *    user is allowed to overcommit the declared protection at a given
 *    level. If that is the case, the parent's effective protection is
 *    distributed to the children in proportion to how much protection
 *    they have declared and how much of it they are utilizing.
 *
 *    This makes distribution proportional, but also work-conserving:
 *    if one counter claims much more protection than it uses memory,
 *    the unused remainder is available to its siblings.
 *
 * 4. Conversely, when the declared protection is undercommitted at a
 *    given level, the distribution of the larger parental protection
 *    budget is NOT proportional. A counter's protection from a sibling
 *    is capped to its own memory.min/low setting.
 *
 * 5. However, to allow protecting recursive subtrees from each other
 *    without having to declare each individual counter's fixed share
 *    of the ancestor's claim to protection, any unutilized -
 *    "floating" - protection from up the tree is distributed in
 *    proportion to each counter's *usage*. This makes the protection
 *    neutral wrt sibling cgroups and lets them compete freely over
 *    the shared parental protection budget, but it protects the
 *    subtree as a whole from neighboring subtrees.
 *
 * Note that 4. and 5. are not in conflict: 4. is about protecting
 * against immediate siblings whereas 5. is about protecting against
 * neighboring subtrees.
 */
/*
 * 该算法计算单个 page_counter 的有效保护：结果不仅取决于自身 memory.min/low，还取决于
 * 父级可分配预算、兄弟声明的实际使用量，以及整棵树当前内存分布。
 *
 * 有效保护遵守五条规则：
 * 1. 回收根的第一层直接采用各自声明的 memory.min/low；
 * 2. 更深层为支持安全委派，不能超过父节点已经算出的有效保护；
 * 3. 同层实际使用的声明保护总和超卖时，父预算按各子节点“已使用保护”比例分配；使用
 *    实际量而非静态声明，使某个孩子闲置的额度能让给兄弟，保持 work-conserving；
 * 4. 同层声明保护不足时，默认不会按比例瓜分父级更大预算，节点对直接兄弟的保护仍以
 *    自身 min/low 为上限；
 * 5. 开启递归保护时，祖先剩余的“浮动”预算按各节点未保护 usage 比例分配。这不改变
 *    直接兄弟间的显式优先级，却能让整个子树共同抵御邻近子树的回收。
 *
 * 第 4 条处理直接兄弟，第 5 条处理相邻子树，因此二者并不冲突。
 */
/*
 * 在父预算内计算一个孩子本轮可见的 min 或 low 有效保护页数。
 * 业务背景：page_counter_calculate_protection() 对 min/low 各调用一次；父节点必须先完成
 * 自身 effective 值，当前节点才能执行层级分摊。
 * 入参：@usage 是当前节点页数；@parent_usage 是父节点页数；@setting 是本节点 min/low；
 * @parent_effective 是父节点对应有效预算；@siblings_protected 是父级汇总的所有孩子实际
 * 已使用声明保护；@recursive_protection 决定是否分配祖先浮动预算，单位均为基页。
 * 出参/返回：返回当前节点的有效保护页数，无状态写入和 ownership 变化。
 * 注意事项：无锁、不睡眠，参数是非原子快照，分母条件必须同时复核以避免并发变化导致
 * 无意义除法；调用者只能在自顶向下树遍历中使用结果。
 */
static unsigned long effective_protection(unsigned long usage,
					  unsigned long parent_usage,
					  unsigned long setting,
					  unsigned long parent_effective,
					  unsigned long siblings_protected,
					  bool recursive_protection)
{
	/* protected 是本节点已实际使用的声明额度；ep 是逐阶段累积的有效保护。 */
	unsigned long protected;
	unsigned long ep;

	/* 阶段 1：声明但未使用的页不参与兄弟分配，先把本地保护钳到实际 usage。 */
	protected = min(usage, setting);
	/*
	 * If all cgroups at this level combined claim and use more
	 * protection than what the parent affords them, distribute
	 * shares in proportion to utilization.
	 *
	 * We are using actual utilization rather than the statically
	 * claimed protection in order to be work-conserving: claimed
	 * but unused protection is available to siblings that would
	 * otherwise get a smaller chunk than what they claimed.
	 */
	/*
	 * 同层孩子合计使用的保护超过父预算时，按各自 protected 比例缩减。以实际
	 * 利用率而非静态声明作权重，可把声明但闲置的份额留给真正使用内存的兄弟。
	 */
	if (siblings_protected > parent_effective)
		return protected * parent_effective / siblings_protected;

	/*
	 * Ok, utilized protection of all children is within what the
	 * parent affords them, so we know whatever this child claims
	 * and utilizes is effectively protected.
	 *
	 * If there is unprotected usage beyond this value, reclaim
	 * will apply pressure in proportion to that amount.
	 *
	 * If there is unutilized protection, the cgroup will be fully
	 * shielded from reclaim, but we do return a smaller value for
	 * protection than what the group could enjoy in theory. This
	 * is okay. With the overcommit distribution above, effective
	 * protection is always dependent on how memory is actually
	 * consumed among the siblings anyway.
	 */
	/*
	 * 未超卖时，本节点已使用的声明保护全部生效。超出 protected 的 usage 会按量承受
	 * 回收；声明额度未用满时返回值虽小于理论上限，但节点当前内存仍已全部覆盖。
	 */
	ep = protected;

	/*
	 * If the children aren't claiming (all of) the protection
	 * afforded to them by the parent, distribute the remainder in
	 * proportion to the (unprotected) memory of each cgroup. That
	 * way, cgroups that aren't explicitly prioritized wrt each
	 * other compete freely over the allowance, but they are
	 * collectively protected from neighboring trees.
	 *
	 * We're using unprotected memory for the weight so that if
	 * some cgroups DO claim explicit protection, we don't protect
	 * the same bytes twice.
	 *
	 * Check both usage and parent_usage against the respective
	 * protected values. One should imply the other, but they
	 * aren't read atomically - make sure the division is sane.
	 */
	/*
	 * recursiveprot 关闭时，第 4 条到此结束。开启时，若父有效预算仍有剩余、父与本节点
	 * 都确有未保护 usage，才按本节点 (usage-protected) 在父级未保护 usage 中的比例
	 * 领取浮动预算。三重检查还防止非原子快照造成减法下溢或除零。
	 */
	if (!recursive_protection)
		return ep;

	if (parent_effective > siblings_protected &&
	    parent_usage > siblings_protected &&
	    usage > protected) {
		unsigned long unclaimed;

		/* unclaimed 先是父剩余页数，随后乘权重并除以父级未保护总量。 */
		unclaimed = parent_effective - siblings_protected;
		unclaimed *= usage - protected;
		unclaimed /= parent_usage - siblings_protected;

		ep += unclaimed;
	}

	/* 返回本轮快照下的有效页数；调用者负责 WRITE_ONCE 发布到 emin/elow。 */
	return ep;
}


/**
 * page_counter_calculate_protection - check if memory consumption is in the normal range
 * @root: the top ancestor of the sub-tree being checked
 * @counter: the page_counter the counter to update
 * @recursive_protection: Whether to use memory_recursiveprot behavior.
 *
 * Calculates elow/emin thresholds for given page_counter.
 *
 * WARNING: This function is not stateless! It can only be used as part
 *          of a top-down tree iteration, not for isolated queries.
 */
/*
 * 在回收子树的自顶向下遍历中计算 @counter 的 emin/elow。
 * 业务背景：mem_cgroup_calculate_protection() 先算父再算子，把静态 min/low 和当前兄弟
 * usage 转成 reclaim 可直接比较的有效阈值；脱离遍历单独调用会读取陈旧父结果。
 * 入参：@root 是本轮回收域顶点；@counter 是要更新的借用节点；@recursive_protection
 * 表示是否启用 memory_recursiveprot 的浮动祖先保护。root/counter 必须在同一祖先链。
 * 出参/返回：无直接返回；普通节点可能写 counter->emin/elow。root、自身 usage=0 时不写，
 * root 的直接孩子直接采用自身 min/low，更深节点按父预算和兄弟利用量计算。
 * 注意事项：无锁、不睡眠，读取的是近似并发快照；只能按 top-down 顺序调用。回收目标
 * 节点的 effective 值有意忽略且可陈旧；原 TODO 希望未来增强算法以去除此特判。
 */
void page_counter_calculate_protection(struct page_counter *root,
				       struct page_counter *counter,
				       bool recursive_protection)
{
	/* usage/parent_usage 是计算快照；parent 必须已在本轮遍历中先完成有效值。 */
	unsigned long usage, parent_usage;
	struct page_counter *parent = counter->parent;

	/*
	 * Effective values of the reclaim targets are ignored so they
	 * can be stale. Have a look at mem_cgroup_protection for more
	 * details.
	 * TODO: calculation should be more robust so that we do not need
	 * that special casing.
	 */
	/*
	 * root 是回收目标，其自身 effective 值不会用于保护自己，允许保留旧值；当前代码
	 * 因此显式跳过。该特殊处理正是原注释 TODO 希望通过更健壮计算消除的部分。
	 */
	if (root == counter)
		return;

	/* 无实际使用量时没有字节需要保护，避免覆盖并计算无消费者的派生值。 */
	usage = page_counter_read(counter);
	if (!usage)
		return;

	/* 第一层不受更高祖先预算裁剪，直接发布声明值，对应规则 1。 */
	if (parent == root) {
		counter->emin = READ_ONCE(counter->min);
		counter->elow = READ_ONCE(counter->low);
		return;
	}

	/* 更深层读取父 usage 与已发布的 emin/elow，分别进行 min/low 独立分摊。 */
	parent_usage = page_counter_read(parent);

	/* 阶段 1：硬保护使用 min 和 children_min_usage 账本，结果发布为 emin。 */
	WRITE_ONCE(counter->emin, effective_protection(usage, parent_usage,
			READ_ONCE(counter->min),
			READ_ONCE(parent->emin),
			atomic_long_read(&parent->children_min_usage),
			recursive_protection));

	/* 阶段 2：low 使用独立账本发布 elow，避免两种回收语义互相污染。 */
	WRITE_ONCE(counter->elow, effective_protection(usage, parent_usage,
			READ_ONCE(counter->low),
			READ_ONCE(parent->elow),
			atomic_long_read(&parent->children_low_usage),
			recursive_protection));
}
#endif /* CONFIG_MEMCG || CONFIG_CGROUP_DMEM */
/* 两个控制器都关闭时不编译保护计算，公开头提供空桩，基础层级计数接口仍可使用。 */
