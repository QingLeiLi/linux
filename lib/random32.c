// SPDX-License-Identifier: GPL-2.0
/*
 * This is a maximally equidistributed combined Tausworthe generator
 * based on code from GNU Scientific Library 1.5 (30 Jun 2004)
 *
 * lfsr113 version:
 *
 * x_n = (s1_n ^ s2_n ^ s3_n ^ s4_n)
 *
 * s1_{n+1} = (((s1_n & 4294967294) << 18) ^ (((s1_n <<  6) ^ s1_n) >> 13))
 * s2_{n+1} = (((s2_n & 4294967288) <<  2) ^ (((s2_n <<  2) ^ s2_n) >> 27))
 * s3_{n+1} = (((s3_n & 4294967280) <<  7) ^ (((s3_n << 13) ^ s3_n) >> 21))
 * s4_{n+1} = (((s4_n & 4294967168) << 13) ^ (((s4_n <<  3) ^ s4_n) >> 12))
 *
 * The period of this generator is about 2^113 (see erratum paper).
 *
 * From: P. L'Ecuyer, "Maximally Equidistributed Combined Tausworthe
 * Generators", Mathematics of Computation, 65, 213 (1996), 203--213:
 * http://www.iro.umontreal.ca/~lecuyer/myftp/papers/tausme.ps
 * ftp://ftp.iro.umontreal.ca/pub/simulation/lecuyer/papers/tausme.ps
 *
 * There is an erratum in the paper "Tables of Maximally Equidistributed
 * Combined LFSR Generators", Mathematics of Computation, 68, 225 (1999),
 * 261--269: http://www.iro.umontreal.ca/~lecuyer/myftp/papers/tausme2.ps
 *
 *      ... the k_j most significant bits of z_j must be non-zero,
 *      for each j. (Note: this restriction also applies to the
 *      computer code given in [4], but was mistakenly not mentioned
 *      in that paper.)
 *
 * This affects the seeding procedure by imposing the requirement
 * s1 > 1, s2 > 7, s3 > 15, s4 > 127.
 *
 * 中文翻译：本文件实现的是一个最大均匀分布的 combined Tausworthe
 * 伪随机数生成器，代码基础来自 GNU Scientific Library 1.5。
 * lfsr113 版本把四个状态分量 s1..s4 分别推进，然后用异或得到 x_n。
 * 论文勘误指出，每个分量的若干最高有效位必须非零；
 * 这会影响播种流程，
 * 因为 s1/s2/s3/s4 必须分别大于 1、7、15、127。
 *
 * 学习补充：这里的核心不是“从熵池拿随机数”，而是“给定一个
 * 可变状态，快速推进出下一项伪随机序列”。它适合调度扰动、
 * 故障注入、网络仿真、
 * 测试等不需要密码学强度的场景；安全随机数应使用 get_random_u32()
 * 或 get_random_bytes()。优势是状态小、开销低、可复现；代价是调用者
 * 必须理解并维护 state 的生命周期、播种质量和并发访问约束。
 */

/* 基础整数类型和 size_t/u8/u32 等类型依赖。 */
#include <linux/types.h>
/* per-cpu 播种接口需要遍历和定位每个 CPU 的 rnd_state。 */
#include <linux/percpu.h>
/* 本文件导出 prandom_* 符号给其他内核模块/子系统使用。 */
#include <linux/export.h>
/* 自测或历史播种路径可能需要 jiffies 相关基础设施，保留上游依赖。 */
#include <linux/jiffies.h>
/* struct rnd_state、__seed() 和对外声明位于 prandom.h。 */
#include <linux/prandom.h>
/* 自测循环中 cond_resched() 需要调度相关声明。 */
#include <linux/sched.h>
/* BITS_PER_BYTE 等位操作常量来自 bitops。 */
#include <linux/bitops.h>
/* 保留上游依赖：部分配置/历史路径可能通过本文件包含 slab 设施。 */
#include <linux/slab.h>
/* prandom_bytes_state() 允许对可能未对齐的缓冲区按 u32 写入。 */
#include <linux/unaligned.h>

/**
 *	prandom_u32_state - seeded pseudo-random number generator.
 *	@state: pointer to state structure holding seeded state.
 *
 *	This is used for pseudo-randomness with no outside seeding.
 *	For more random results, use get_random_u32().
 *
 * 中文翻译：基于已经播种的状态生成一个 32 位伪随机数。
 * @state: 入参/出参，指向持有生成器状态的 rnd_state；函数会读取旧的
 * s1..s4，推进每个分量，并把新状态写回同一个结构体。
 *
 * 返回值：四个推进后分量异或得到的 u32 伪随机数。
 *
 * 学习补充：这个接口没有外部重新播种，也不会访问全局随机熵池。
 * 所以它很快、可复现，但不适合任何依赖不可预测性的安全场景。
 * 若调用者共享同一个 @state，必须自己加锁或保证单 CPU/单线程访问；
 * 本函数只负责状态推进，不提供并发保护。
 */
u32 prandom_u32_state(struct rnd_state *state)
{
	/*
	 * TAUSWORTHE() 是单个 LFSR 分量的状态推进公式：
	 * @s 是旧状态，@c 掩掉算法不允许参与左移的低位，@a/@b/@d 控制
	 * 反馈位混合与移位。宏只计算新分量，不访问外部状态。
	 */
#define TAUSWORTHE(s, a, b, c, d) ((s & c) << d) ^ (((s << a) ^ s) >> b)
	/*
	 * 四个分量使用不同参数推进，对应文件头中列出的 lfsr113 递推式。
	 * 这些写入是本函数最重要的副作用：调用者传入的 state
	 * 被原地消耗，
	 * 下一次调用会从这里写回的新状态继续。
	 */
	state->s1 = TAUSWORTHE(state->s1,  6U, 13U, 4294967294U, 18U);
	state->s2 = TAUSWORTHE(state->s2,  2U, 27U, 4294967288U,  2U);
	state->s3 = TAUSWORTHE(state->s3, 13U, 21U, 4294967280U,  7U);
	state->s4 = TAUSWORTHE(state->s4,  3U, 12U, 4294967168U, 13U);

	/*
	 * 输出不是额外维护的第五个状态，而是四个分量的异或。
	 * 因此返回值和状态推进紧密绑定：重复读取不会得到同一个值，
	 * 除非调用者保存并恢复整个 rnd_state。
	 */
	return (state->s1 ^ state->s2 ^ state->s3 ^ state->s4);
}
EXPORT_SYMBOL(prandom_u32_state);

/**
 *	prandom_bytes_state - get the requested number of pseudo-random bytes
 *
 *	@state: pointer to state structure holding seeded state.
 *	@buf: where to copy the pseudo-random bytes to
 *	@bytes: the requested number of bytes
 *
 *	This is used for pseudo-randomness with no outside seeding.
 *	For more random results, use get_random_bytes().
 *
 * 中文翻译：生成调用者请求数量的伪随机字节。
 * @state: 入参/出参，已播种的 rnd_state；函数会按需多次推进它。
 * @buf: 出参，接收伪随机字节的目标缓冲区；调用者保证缓冲区可写。
 * @bytes: 入参，请求写入的字节数；为 0 时不会写出随机数据。
 *
 * 返回值：无。生成结果写入 @buf，状态推进体现在 @state 中。
 *
 * 学习补充：这个函数是 prandom_u32_state() 的字节流包装层。
 * 它按 32 位为单位生成，尾部不足 4 字节时拆分最后一个 u32。
 * 这样比逐字节推进状态更便宜，但输出仍然只是快速伪随机字节。
 */
void prandom_bytes_state(struct rnd_state *state, void *buf, size_t bytes)
{
	/* @ptr 是当前写入位置；它只在 @buf 范围内前进，不拥有缓冲区。 */
	u8 *ptr = buf;

	/*
	 * 主循环一次消耗一个 u32 伪随机输出，并用 put_unaligned() 写入。
	 * 使用 unaligned helper 的原因是调用者传入的 @buf
	 * 不一定按 4 字节对齐；
	 * 直接做普通 u32 存储在某些架构上可能触发未对齐访问问题。
	 */
	while (bytes >= sizeof(u32)) {
		put_unaligned(prandom_u32_state(state), (u32 *) ptr);
		ptr += sizeof(u32);
		bytes -= sizeof(u32);
	}

	/*
	 * 尾部不足 4 字节时仍然推进一次 state，取出一个 u32 后
	 * 低字节优先写入。注意这意味着请求 1、2、3 个尾字节也会
	 * 消耗完整的一个 32 位输出；
	 * 调用者不能假设“少取几个字节”会少推进部分状态。
	 */
	if (bytes > 0) {
		u32 rem = prandom_u32_state(state);
		do {
			*ptr++ = (u8) rem;
			bytes--;
			rem >>= BITS_PER_BYTE;
		} while (bytes > 0);
	}
}
EXPORT_SYMBOL(prandom_bytes_state);

/*
 * prandom_warmup() - 播种后预热单个 rnd_state。
 * @state: 入参/出参，刚完成下界修正的 rnd_state；函数会连续推进它。
 *
 * 返回值：无。预热后的状态写回 @state。
 *
 * 背景：Tausworthe 递推对初始状态有约束，刚播种后的前几个输出
 * 可能更直接暴露种子结构。预热通过丢弃若干次输出，
 * 让状态进入更稳定的递推阶段。
 */
static void prandom_warmup(struct rnd_state *state)
{
	/* Calling RNG ten times to satisfy recurrence condition */
	/*
	 * 中文翻译：调用随机数生成器十次，以满足递推条件。
	 *
	 * 学习补充：这里的十次返回值全部被丢弃，目的不是提供输出，
	 * 而是推进状态。函数调用者只关心 @state 被更新后的结果。
	 */
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
}

/*
 * prandom_seed_full_state() - 为 per-cpu prandom 状态完整播种。
 * @pcpu_state: 出参，per-cpu rnd_state 存储；函数会为每个 possible CPU
 * 写入一个独立状态。
 *
 * 返回值：无。成功结果体现在每个 CPU 的 rnd_state 中。
 *
 * 生命周期：典型调用者用 prandom_init_once() 包装本函数，让热路径第一次
 * 使用时完成播种，之后复用 per-cpu 状态。这里遍历 possible CPU，而不是
 * online CPU，是为了给之后可能上线的 CPU 也准备好状态。
 *
 * 并发/所有权：本函数初始化调用者提供的 per-cpu 存储，
 * 不接管其生命周期。
 * 一次性初始化由调用者或 DO_ONCE 保证；若运行中重复调用，会覆盖各 CPU
 * 正在推进的序列。
 */
void prandom_seed_full_state(struct rnd_state __percpu *pcpu_state)
{
	/* @i 是 possible CPU 编号；每轮只初始化该 CPU 对应的本地 state。 */
	int i;

	for_each_possible_cpu(i) {
		/*
		 * per_cpu_ptr() 把 per-cpu 基址转换成 CPU @i 的具体 rnd_state。
		 * 返回指针只表示该 CPU 槽位的地址，
		 * 不意味着当前正在 CPU @i 上执行。
		 */
		struct rnd_state *state = per_cpu_ptr(pcpu_state, i);
		/*
		 * 四个独立随机种子分别喂给 s1..s4。
		 * 这里用数组只是临时承载熵，
		 * 写入 state 并预热后不再需要。
		 */
		u32 seeds[4];

		/*
		 * 真实熵来自 get_random_bytes()；随后仍要经过 __seed()，
		 * 因为“随机”不等于一定满足 Tausworthe 的分量下界要求。
		 */
		get_random_bytes(&seeds, sizeof(seeds));
		state->s1 = __seed(seeds[0],   2U);
		state->s2 = __seed(seeds[1],   8U);
		state->s3 = __seed(seeds[2],  16U);
		state->s4 = __seed(seeds[3], 128U);

		/*
		 * 播种完成后立即预热，确保后续真正的调用者拿到的是
		 * 预热后的
		 * 序列位置，而不是刚由外部 seed 直接构造出的初始状态。
		 */
		prandom_warmup(state);
	}
}
EXPORT_SYMBOL(prandom_seed_full_state);

#ifdef CONFIG_RANDOM32_SELFTEST
/*
 * CONFIG_RANDOM32_SELFTEST 下的表驱动自测只在启用该配置时编译。
 * 它们不是运行期随机服务的一部分，而是用固定种子和固定期望值
 * 保护算法、
 * 播种边界和递推参数不被无意改坏。
 */
static struct prandom_test1 {
	/* @seed: 输入种子；@result: 预热后第一次输出的期望值。 */
	u32 seed;
	u32 result;
} test1[] = {
	/*
	 * 小种子用例覆盖 __seed() 的边界修正。
	 * 若 s1/s2/s3/s4 的下界规则被改坏，
	 * 这些用例会比长序列用例更早暴露问题。
	 */
	{ 1U, 3484351685U },
	{ 2U, 2623130059U },
	{ 3U, 3125133893U },
	{ 4U,  984847254U },
};

static struct prandom_test2 {
	/* @seed: 输入种子；@iteration: 推进次数；@result: 该次输出的期望值。 */
	u32 seed;
	u32 iteration;
	u32 result;
} test2[] = {
	/* Test cases against taus113 from GSL library. */
	/*
	 * 中文翻译：这些测试用例对照 GSL 库中的 taus113 实现。
	 *
	 * 学习补充：长表不是为了覆盖“随机性”，而是锁定确定性序列。
	 * 只要递推公式、移位参数、播种下界或 warmup 次数发生变化，
	 * 固定 seed 在固定 iteration 上的结果就会偏离表中期望值。
	 */
	{  931557656U, 959U, 2975593782U },
	{ 1339693295U, 876U, 3887776532U },
	{ 1545556285U, 961U, 1615538833U },
	{  601730776U, 723U, 1776162651U },
	{ 1027516047U, 687U,  511983079U },
	{  416526298U, 700U,  916156552U },
	{ 1395522032U, 652U, 2222063676U },
	{  366221443U, 617U, 2992857763U },
	{ 1539836965U, 714U, 3783265725U },
	{  556206671U, 994U,  799626459U },
	{  684907218U, 799U,  367789491U },
	{ 2121230701U, 931U, 2115467001U },
	{ 1668516451U, 644U, 3620590685U },
	{  768046066U, 883U, 2034077390U },
	{ 1989159136U, 833U, 1195767305U },
	{  536585145U, 996U, 3577259204U },
	{ 1008129373U, 642U, 1478080776U },
	{ 1740775604U, 939U, 1264980372U },
	{ 1967883163U, 508U,   10734624U },
	{ 1923019697U, 730U, 3821419629U },
	{  442079932U, 560U, 3440032343U },
	{ 1961302714U, 845U,  841962572U },
	{ 2030205964U, 962U, 1325144227U },
	{ 1160407529U, 507U,  240940858U },
	{  635482502U, 779U, 4200489746U },
	{ 1252788931U, 699U,  867195434U },
	{ 1961817131U, 719U,  668237657U },
	{ 1071468216U, 983U,  917876630U },
	{ 1281848367U, 932U, 1003100039U },
	{  582537119U, 780U, 1127273778U },
	{ 1973672777U, 853U, 1071368872U },
	{ 1896756996U, 762U, 1127851055U },
	{  847917054U, 500U, 1717499075U },
	{ 1240520510U, 951U, 2849576657U },
	{ 1685071682U, 567U, 1961810396U },
	{ 1516232129U, 557U,    3173877U },
	{ 1208118903U, 612U, 1613145022U },
	{ 1817269927U, 693U, 4279122573U },
	{ 1510091701U, 717U,  638191229U },
	{  365916850U, 807U,  600424314U },
	{  399324359U, 702U, 1803598116U },
	{ 1318480274U, 779U, 2074237022U },
	{  697758115U, 840U, 1483639402U },
	{ 1696507773U, 840U,  577415447U },
	{ 2081979121U, 981U, 3041486449U },
	{  955646687U, 742U, 3846494357U },
	{ 1250683506U, 749U,  836419859U },
	{  595003102U, 534U,  366794109U },
	{   47485338U, 558U, 3521120834U },
	{  619433479U, 610U, 3991783875U },
	{  704096520U, 518U, 4139493852U },
	{ 1712224984U, 606U, 2393312003U },
	{ 1318233152U, 922U, 3880361134U },
	{  855572992U, 761U, 1472974787U },
	{   64721421U, 703U,  683860550U },
	{  678931758U, 840U,  380616043U },
	{  692711973U, 778U, 1382361947U },
	{  677703619U, 530U, 2826914161U },
	{   92393223U, 586U, 1522128471U },
	{ 1222592920U, 743U, 3466726667U },
	{  358288986U, 695U, 1091956998U },
	{ 1935056945U, 958U,  514864477U },
	{  735675993U, 990U, 1294239989U },
	{ 1560089402U, 897U, 2238551287U },
	{   70616361U, 829U,   22483098U },
	{  368234700U, 731U, 2913875084U },
	{   20221190U, 879U, 1564152970U },
	{  539444654U, 682U, 1835141259U },
	{ 1314987297U, 840U, 1801114136U },
	{ 2019295544U, 645U, 3286438930U },
	{  469023838U, 716U, 1637918202U },
	{ 1843754496U, 653U, 2562092152U },
	{  400672036U, 809U, 4264212785U },
	{  404722249U, 965U, 2704116999U },
	{  600702209U, 758U,  584979986U },
	{  519953954U, 667U, 2574436237U },
	{ 1658071126U, 694U, 2214569490U },
	{  420480037U, 749U, 3430010866U },
	{  690103647U, 969U, 3700758083U },
	{ 1029424799U, 937U, 3787746841U },
	{ 2012608669U, 506U, 3362628973U },
	{ 1535432887U, 998U,   42610943U },
	{ 1330635533U, 857U, 3040806504U },
	{ 1223800550U, 539U, 3954229517U },
	{ 1322411537U, 680U, 3223250324U },
	{ 1877847898U, 945U, 2915147143U },
	{ 1646356099U, 874U,  965988280U },
	{  805687536U, 744U, 4032277920U },
	{ 1948093210U, 633U, 1346597684U },
	{  392609744U, 783U, 1636083295U },
	{  690241304U, 770U, 1201031298U },
	{ 1360302965U, 696U, 1665394461U },
	{ 1220090946U, 780U, 1316922812U },
	{  447092251U, 500U, 3438743375U },
	{ 1613868791U, 592U,  828546883U },
	{  523430951U, 548U, 2552392304U },
	{  726692899U, 810U, 1656872867U },
	{ 1364340021U, 836U, 3710513486U },
	{ 1986257729U, 931U,  935013962U },
	{  407983964U, 921U,  728767059U },
};

/*
 * prandom_state_selftest_seed() - 自测专用播种器。
 * @state: 出参，接收自测状态。
 * @seed: 入参，表中给出的 32 位测试种子。
 *
 * 返回值：无。四个状态分量写入 @state。
 *
 * 注意：这个 helper 故意不复用 prandom_seed_state() 的 64 位混合方式，
 * 而是用测试期望匹配的 LCG 链式派生方式；它服务于历史测试向量，
 * 不是对外推荐的新播种接口。
 */
static void prandom_state_selftest_seed(struct rnd_state *state, u32 seed)
{
	/*
	 * LCG 是自测中用来从一个 seed 派生多个状态分量的
	 * 简单线性同余生成器。注释里的 “super-duper” 是上游原文玩笑；
	 * 学习时重点看它的确定性，
	 * 而不是把它当作高质量随机扩散函数。
	 */
#define LCG(x)	 ((x) * 69069U)	/* super-duper LCG */
	/*
	 * 每个分量以前一个分量为输入继续派生，并仍然调用 __seed()
	 * 做下界修正。
	 * 这样自测同时覆盖 LCG 派生链和 Tausworthe 的种子边界规则。
	 */
	state->s1 = __seed(LCG(seed),        2U);
	state->s2 = __seed(LCG(state->s1),   8U);
	state->s3 = __seed(LCG(state->s2),  16U);
	state->s4 = __seed(LCG(state->s3), 128U);
}

/*
 * prandom_state_selftest() - 启动期验证 prandom32 固定测试向量。
 *
 * 返回值：始终返回 0，让 core_initcall 流程继续；失败通过 pr_warn()
 * 报告，而不是阻止内核启动。
 *
 * 执行阶段：core_initcall 阶段运行，早于大多数驱动初始化。自测可能遍历
 * 较长表，因此循环中调用 cond_resched()，避免在可调度上下文里
 * 长时间独占 CPU。
 */
static int __init prandom_state_selftest(void)
{
	/*
	 * @errors 统计长序列测试失败次数；@runs 统计执行的长序列用例数。
	 * @error 单独记录边界测试是否失败，便于打印更明确的告警。
	 */
	int i, j, errors = 0, runs = 0;
	bool error = false;

	/*
	 * 第一组测试聚焦种子边界。每个用例先按自测播种器初始化，
	 * 再预热，
	 * 最后比较第一项输出是否等于固定期望。
	 */
	for (i = 0; i < ARRAY_SIZE(test1); i++) {
		struct rnd_state state;

		prandom_state_selftest_seed(&state, test1[i].seed);
		prandom_warmup(&state);

		/* 只要任一边界用例失败，就记录整体边界自测失败。 */
		if (test1[i].result != prandom_u32_state(&state))
			error = true;
	}

	/* 边界测试只打印一条汇总日志，避免启动日志被逐项结果淹没。 */
	if (error)
		pr_warn("prandom: seed boundary self test failed\n");
	else
		pr_info("prandom: seed boundary self test passed\n");

	/*
	 * 第二组测试锁定较长递推序列。每个用例从相同播种和 warmup
	 * 规则开始，
	 * 丢弃 iteration - 1 个输出后，检查目标位置的输出值。
	 */
	for (i = 0; i < ARRAY_SIZE(test2); i++) {
		struct rnd_state state;

		prandom_state_selftest_seed(&state, test2[i].seed);
		prandom_warmup(&state);

		/* 前 iteration - 1 次只是把状态推进到待检查位置。 */
		for (j = 0; j < test2[i].iteration - 1; j++)
			prandom_u32_state(&state);

		/*
		 * 失败只累计，不立即返回；
		 * 这样一次启动能暴露所有坏掉的向量。
		 */
		if (test2[i].result != prandom_u32_state(&state))
			errors++;

		runs++;
		/*
		 * 自测在 initcall 上下文运行，允许主动让出 CPU。
		 * 这不改变随机状态，只改善大表测试对启动调度延迟的影响。
		 */
		cond_resched();
	}

	/* 长序列测试同样只输出汇总，便于启动日志阅读。 */
	if (errors)
		pr_warn("prandom: %d/%d self tests failed\n", errors, runs);
	else
		pr_info("prandom: %d self tests passed\n", runs);
	/* 自测失败不阻断启动；返回 0 表示 initcall 框架继续执行。 */
	return 0;
}

/* core_initcall 把自测挂到核心初始化阶段，仅在 RANDOM32_SELFTEST 下存在。 */
core_initcall(prandom_state_selftest);
#endif
