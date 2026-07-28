/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 学习注释：prandom.h 暴露的是“显式状态”的快速伪随机数接口。
 * 这里的“随机”来自调用者传入并持续更新的 struct rnd_state，
 * 而不是每次都从内核熵池取数；因此它的优势是开销低、可复现、
 * 适合热路径或测试路径，代价是
 * 不具备密码学安全性，也不会自动处理共享状态上的并发访问。
 *
 * 使用边界：
 * - 如果随机值会影响安全性、地址/密钥/令牌不可预测性，优先使用
 *   get_random_u32()、get_random_bytes() 等真正随机接口。
 * - 如果只是做调度扰动、故障注入、网络仿真、
 *   测试用可复现随机序列，prandom 这类“快但弱”的生成器更合适。
 * - 本头文件只定义状态形状、播种 helper 和函数契约；真正的 combined
 *   Tausworthe 状态推进逻辑在 lib/random32.c 中实现。
 */
/*
 * include/linux/prandom.h
 *
 * Include file for the fast pseudo-random 32-bit
 * generation.
 *
 * 中文翻译：这是快速 32 位伪随机数生成接口的头文件。
 *
 * 学习补充：这里强调 fast pseudo-random，意思是“快”和“伪随机”
 * 都属于接口契约的一部分。调用者不应把它当作安全随机源；
 * 相反，应把它看成由 rnd_state 驱动的轻量状态机，
 * 每次取数都会消耗并推进这个状态。
 */
#ifndef _LINUX_PRANDOM_H
#define _LINUX_PRANDOM_H

/* u32/u64/size_t 等基础类型来自这里，避免在头文件中依赖隐式包含。 */
#include <linux/types.h>
/* prandom_init_once() 通过 DO_ONCE 做懒初始化，定义在 once.h。 */
#include <linux/once.h>
/* prandom_seed_full_state() 的入参是 per-cpu rnd_state，需要 percpu 标注。 */
#include <linux/percpu.h>
/* per-cpu 状态的真实播种会从内核随机接口取得初始种子。 */
#include <linux/random.h>

/*
 * struct rnd_state - prandom 显式持有的可变生成器状态。
 * @s1: combined Tausworthe 生成器的第 1 个 32 位 LFSR 状态分量。
 * @s2: combined Tausworthe 生成器的第 2 个 32 位 LFSR 状态分量。
 * @s3: combined Tausworthe 生成器的第 3 个 32 位 LFSR 状态分量。
 * @s4: combined Tausworthe 生成器的第 4 个 32 位 LFSR 状态分量。
 *
 * 背景：lib/random32.c 中的 prandom_u32_state() 会分别推进 s1..s4，然后
 * 将四个分量异或成一个 u32 返回值。这种设计比直接访问全局熵池
 * 更便宜，也让调用者可以通过固定 seed 得到可复现序列。
 *
 * 不变量：四个分量不能落入算法禁止的小状态区间；因此播种路径
 * 会分别对它们施加 s1 >= 2、s2 >= 8、s3 >= 16、s4 >= 128
 * 的下界修正。若调用者手工填充
 * rnd_state 而绕过 __seed()/prandom_seed_state()，可能制造退化序列。
 *
 * 所有权/并发：rnd_state 由调用者拥有。prandom_u32_state() 和
 * prandom_bytes_state() 会原地修改该结构体，没有内部锁；同一个 state 被多个
 * CPU 或线程共享时，调用者必须自己串行化，或者改用 per-cpu state。
 */
struct rnd_state {
	__u32 s1, s2, s3, s4;
};

/*
 * prandom_u32_state() - 从显式状态中取一个 32 位伪随机数。
 * @state: 入参/出参，指向已播种的 rnd_state；函数会读取旧状态，
 * 推进 s1..s4，
 * 并把推进后的状态写回同一个对象。
 *
 * 返回值：生成出的 u32 伪随机值。它只保证对轻量扰动/测试足够快，
 * 不保证密码学不可预测性。
 *
 * 副作用/注意事项：每调用一次都会消耗一次状态推进；调用者如果
 * 需要可复现序列，应先用固定 seed 初始化 state，并避免并发乱序访问。
 * 同一个 state 上
 * 的竞争写会破坏序列语义，接口本身不加锁也不禁抢占。
 */
u32 prandom_u32_state(struct rnd_state *state);
/*
 * prandom_bytes_state() - 从显式状态中连续生成若干伪随机字节。
 * @state: 入参/出参，指向已播种的 rnd_state；函数会按需多次调用
 * prandom_u32_state()，因此会推进状态一次或多次。
 * @buf: 出参，目标缓冲区；函数会写入 nbytes 个字节，
 * 调用者负责保证缓冲区可写且大小足够。
 * @nbytes: 入参，请求写入的字节数；为 0 时语义上不需要产生输出。
 *
 * 返回值：无。成功路径通过 @buf 体现输出，通过 @state 体现新的
 * 生成器位置。
 *
 * 实现背景：lib/random32.c 中按 u32 批量写入，尾部不足 4 字节时再拆出剩余
 * 字节。这样比逐字节推进状态更便宜，但也意味着一次取 bytes 会消耗
 * ceil(nbytes / 4) 个 32 位随机输出。
 *
 * 注意事项：输出适合测试填充、扰动或非安全采样；若缓冲区内容
 * 需要安全随机性，应使用 get_random_bytes()。
 */
void prandom_bytes_state(struct rnd_state *state, void *buf, size_t nbytes);
/*
 * prandom_seed_full_state() - 为一组 per-cpu rnd_state 完整播种并预热。
 * @pcpu_state: 出参，指向 per-cpu rnd_state 对象；函数会遍历 possible CPU，
 * 为每个 CPU 的本地 state 生成独立初始状态并写回。
 *
 * 返回值：无。初始化结果体现在每个 CPU 对应的 rnd_state 中。
 *
 * 实现背景：函数在 lib/random32.c 中从 get_random_bytes() 取四个种子分量，
 * 再用 __seed() 修正算法下界，最后调用 warmup 预热状态，
 * 避免刚播种后的前几次输出直接暴露初始结构。
 *
 * 使用边界：这是给 per-cpu 快速随机状态准备初始条件的接口，
 * 不是每次取随机数前都应调用的重播种接口。频繁重播种会破坏
 * 调用者对序列推进和性能的预期。
 */
void prandom_seed_full_state(struct rnd_state __percpu *pcpu_state);

/*
 * prandom_init_once() - 对 per-cpu prandom 状态做一次性懒初始化。
 * @pcpu_state: 出参，通常是 DEFINE_PER_CPU(struct rnd_state, ...) 定义出来的
 * per-cpu 状态地址；宏会把它传给 prandom_seed_full_state()。
 *
 * 返回值：宏展开为 DO_ONCE(...) 的返回语义；调用者通常只关心
 * 初始化动作最多发生一次，而不是把它当作随机值接口。
 *
 * 设计意图：很多调用点位于热路径，提前初始化所有可能用到的
 * 随机状态不一定划算；DO_ONCE 允许第一个到达的调用者完成播种，
 * 后续调用直接复用已初始化的 per-cpu 状态，
 * 避免重复播种覆盖正在使用的序列。
 *
 * 注意事项：once 只保护“初始化函数执行一次”这件事，不替代后续
 * 对同一 rnd_state 的并发约束。每个 CPU 最好只操作自己的 state；
 * 跨 CPU 共享仍需
 * 调用者自行约束。
 */
#define prandom_init_once(pcpu_state)			\
	DO_ONCE(prandom_seed_full_state, (pcpu_state))

/*
 * Handle minimum values for seeds
 *
 * 中文翻译：处理种子的最小取值要求。
 *
 * 学习补充：combined Tausworthe 的每个状态分量都有最低有效状态
 * 要求。若种子太小，某些高位约束不满足，生成器可能进入退化周期。
 * 这里不追求“增加随机性”，只是把候选值从非法/弱区间
 * 推回算法允许的状态空间。
 */
static inline u32 __seed(u32 x, u32 m)
{
	/*
	 * @x: 入参，候选种子分量。
	 * @m: 入参，该分量的最小阈值；调用点分别传入 2、8、16、128。
	 *
	 * 返回值：若 x 小于阈值，则返回 x + m；否则原样返回 x。
	 * 函数没有额外副作用，也不访问全局随机源。
	 *
	 * 注意：这里使用加法而不是简单替换成 m，可以保留小种子
	 * 之间的差异；
	 * 但它仍只是边界修正，不会把低质量 seed 变成安全随机种子。
	 */
	return (x < m) ? x + m : x;
}

/**
 * prandom_seed_state - set seed for prandom_u32_state().
 * @state: pointer to state structure to receive the seed.
 * @seed: arbitrary 64-bit value to use as a seed.
 *
 * 中文翻译：为 prandom_u32_state() 设置种子。
 * @state: 出参，指向要接收新种子的状态结构；函数会覆盖其中的 s1..s4。
 * @seed: 入参，任意 64 位种子值，用来派生四个 32 位状态分量。
 *
 * 返回值：无。播种结果写入 @state。
 *
 * 学习补充：这个 helper 适合需要确定性序列的局部 rnd_state，
 * 例如测试或希望用固定 seed 复现实验结果的路径。
 * 它不会像 prandom_seed_full_state() 那样为
 * 每个 CPU 取独立熵、也不会执行 warmup；调用者要理解“下一次输出”
 * 就是从这里写入的状态继续推进。
 *
 * 并发/所有权：函数直接覆盖 @state，没有锁和引用计数语义。
 * 若别的执行流正在使用同一个 state，重播种会让对方看到序列突变；
 * 共享状态必须由调用者保护。
 */
static inline void prandom_seed_state(struct rnd_state *state, u64 seed)
{
	/*
	 * 将 64 位 seed 的高 32 位、低 32 位以及左移后的低位信息混合到一个
	 * 32 位候选值中。这里的目标是为四个分量准备一个确定性起点，
	 * 而不是进行密码学强度的扩散。
	 */
	u32 i = ((seed >> 32) ^ (seed << 10) ^ seed) & 0xffffffffUL;

	/*
	 * 四个状态分量使用同一个混合值，但采用不同的最小阈值修正，
	 * 匹配 Tausworthe 算法对 s1/s2/s3/s4 的边界要求。
	 * 输出只体现在 @state 中。
	 */
	state->s1 = __seed(i,   2U);
	state->s2 = __seed(i,   8U);
	state->s3 = __seed(i,  16U);
	state->s4 = __seed(i, 128U);
}

#endif
