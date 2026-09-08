// SPDX-License-Identifier: GPL-2.0-only
/* tnum: tracked (or tristate) numbers
 *
 * A tnum tracks knowledge about the bits of a value.  Each bit can be either
 * known (0 or 1), or unknown (x).  Arithmetic operations on tnums will
 * propagate the unknown bits such that the tnum result represents all the
 * possible results for possible values of the operands.
 */
/*
 * tnum（三态数）学习导读
 *
 * 一个 tnum 用 `(value, mask)` 表示一组 64 位整数：mask 某位为 0 时，value
 * 给出该位确定的 0/1；mask 某位为 1 时，该位可以是 0 或 1，value 对应位
 * 必须为 0，即始终保持 `value & mask == 0`。因此具体集合可写成：
 *
 *   { value | s : s 是 mask 的任意子掩码 }
 *
 * 验证器把它存入 `bpf_reg_state.var_off`，描述标量或指针可变偏移的逐位知识。
 * 算术运算传播进位造成的未知位，逻辑运算利用已知 0/1 消除不可能结果；返回
 * 集合必须覆盖所有具体输入组合，允许过近似但绝不能漏值，否则验证器可能把
 * 实际不安全的程序误判为安全。这里的值对象无引用、锁或分配，所有函数纯计算，
 * 可在验证器持锁与否的进程上下文调用，不修改输入，也不会睡眠。
 *
 * 原英文说明的完整含义是：tnum 即 tracked/tristate number，每一位是已知 0、
 * 已知 1 或未知 x；算术运算保守传播未知位，使结果代表操作数所有可能取值产生
 * 的结果。文件阅读主线为“构造 → 运算 → 集合关系 → 格式化/子寄存器 → step”。
 */
#include <linux/kernel.h>
#include <linux/tnum.h>
#include <linux/swab.h>

#define TNUM(_v, _m)	(struct tnum){.value = _v, .mask = _m}
/* A completely unknown value */
/* 完全未知值：64 个 mask 位全为 1，因不变量要求 value 位全为 0，具体集合覆盖全部 u64。 */
const struct tnum tnum_unknown = { .value = 0, .mask = -1 };

/*
 * tnum_const() - 把一个确定的 64 位常量提升为 tnum。
 * 业务背景：验证器遇到立即数或已证明常量时，以此建立 var_off 的最精确起点。
 * 入参：@value 是纯输入的完整 64 位数，无哨兵和 ownership。
 * 出参/返回：返回 `(value, 0)`；零 mask 表示集合只有该常量，无外部副作用。
 * 注意事项：纯计算、不睡眠且无需锁；调用者随后可继续用算术函数保守传播知识。
 */
struct tnum tnum_const(u64 value)
{
	return TNUM(value, 0);
}

/*
 * tnum_range() - 用一个 tnum 保守覆盖闭区间 [min, max]。
 * 业务背景：验证器把标量上下界转换成逐位知识；从 min/max 首个不同的高位向下均
 * 标成未知，因 tnum 只能表达“固定前缀+自由低位”，结果可能包含区间外的值。
 * 入参：@min/@max 是无符号闭区间端点，调用者应保证 min <= max，均为借用值。
 * 出参/返回：返回覆盖区间的 tnum；跨越最高位时返回 tnum_unknown，无副作用。
 * 注意事项：过近似会损失精度但保持安全；不分配、不睡眠、无并发状态。
 */
struct tnum tnum_range(u64 min, u64 max)
{
	/* chi 标出两端不同的位；bits 是最高差异位的 1 基编号，delta 将其及低位全置 1。 */
	u64 chi = min ^ max, delta;
	u8 bits = fls64(chi);

	/* special case, needed because 1ULL << 64 is undefined */
	/* 特例：最高差异落在 bit63 时 bits=64，C 中 `1ULL << 64` 未定义，只能返回全未知。 */
	if (bits > 63)
		return tnum_unknown;
	/* e.g. if chi = 4, bits = 3, delta = (1<<3) - 1 = 7.
	 * if chi = 0, bits = 0, delta = (1<<0) - 1 = 0, so we return
	 *  constant min (since min == max).
	 */
	/*
	 * 例如 chi=4 时最高差异覆盖 3 个低位，delta=(1<<3)-1=7；chi=0 时
	 * delta=0，min==max，结果退化为常量。固定高位取 min，低位由 mask 自由变化。
	 */
	delta = (1ULL << bits) - 1;
	return TNUM(min & ~delta, delta);
}

/*
 * tnum_lshift() - 对 tnum 执行固定次数逻辑左移。
 * 业务背景：验证器模拟 BPF_LSH 时同时移动确定值和未知位，保持具体结果集合覆盖。
 * 入参：@a 为纯输入 tnum；@shift 为调用者校验后的位数，必须小于 64。
 * 出参/返回：返回左移后的值/mask，高位按 C 无符号移位规则丢弃；无副作用。
 * 注意事项：不校验 shift，越界会触发 C 未定义行为；验证器调用点负责先限制操作数。
 */
struct tnum tnum_lshift(struct tnum a, u8 shift)
{
	return TNUM(a.value << shift, a.mask << shift);
}

/*
 * tnum_rshift() - 对 tnum 执行固定次数逻辑右移。
 * 业务背景：验证器模拟 BPF_RSH，值位和未知位同步右移，左侧补确定的 0。
 * 入参：@a 为纯输入；@shift 是已校验的小于 64 的位数。
 * 出参/返回：返回右移后的 tnum，无输出参数、分配或全局副作用。
 * 注意事项：本层不做范围检查；调用者必须阻止 shift>=64。
 */
struct tnum tnum_rshift(struct tnum a, u8 shift)
{
	return TNUM(a.value >> shift, a.mask >> shift);
}

/*
 * tnum_arshift() - 按 32/64 位有符号语义执行固定的最小算术右移。
 * 业务背景：验证器模拟 BPF_ARSH；把 value/mask 解释为相应有符号宽度，使符号位
 * 扩展到高位，保守表达负数和未知符号位的结果。
 * 入参：@a 为纯输入；@min_shift 是已校验移位数；@insn_bitness 只允许 32 或 64。
 * 出参/返回：返回相应宽度的算术右移结果；32 位分支清除高 32 位，无副作用。
 * 注意事项：不检查位宽/移位范围且不睡眠；错误前提会产生未定义或错误抽象结果。
 */
struct tnum tnum_arshift(struct tnum a, u8 min_shift, u8 insn_bitness)
{
	/* if a.value is negative, arithmetic shifting by minimum shift
	 * will have larger negative offset compared to more shifting.
	 * If a.value is nonnegative, arithmetic shifting by minimum shift
	 * will have larger positive offset compare to more shifting.
	 */
	/*
	 * 原文说明：value 为负时，采用最小移位量得到的负偏移绝对值更大；value 非负时，
	 * 最小移位量得到更大的正偏移。这里按指令位宽选择 s32/s64 符号扩展规则。
	 */
	if (insn_bitness == 32)
		return TNUM((u32)(((s32)a.value) >> min_shift),
			    (u32)(((s32)a.mask)  >> min_shift));
	else
		return TNUM((s64)a.value >> min_shift,
			    (s64)a.mask  >> min_shift);
}

/*
 * tnum_add() - 保守计算两个 tnum 的 64 位模加法。
 * 业务背景：BPF_ADD 会让低位未知性通过进位扩散；仅逐位合并 mask 会漏掉可能结果。
 * 入参：@a/@b 是两个纯输入集合，无指针、引用或 ownership 转移。
 * 出参/返回：返回覆盖所有 `x+y (mod 2^64)` 的 tnum，无外部副作用。
 * 注意事项：纯计算且不睡眠；结果可过近似，但继续满足 value 与 mask 不相交。
 */
struct tnum tnum_add(struct tnum a, struct tnum b)
{
	/* sm/sv 是 mask/value 两个极端和；两种和发生差异的位揭示进位可能影响的区域。 */
	u64 sm, sv, sigma, chi, mu;

	sm = a.mask + b.mask;
	sv = a.value + b.value;
	sigma = sm + sv;
	chi = sigma ^ sv;
	/* 原输入未知位与进位差异位共同成为结果 mask，再从确定 value 中清除它们。 */
	mu = chi | a.mask | b.mask;
	return TNUM(sv & ~mu, mu);
}

/*
 * tnum_sub() - 保守计算两个 tnum 的 64 位模减法。
 * 业务背景：BPF_SUB 中未知位可通过借位向高位扩散，本函数用上下方向的偏移界定差异位。
 * 入参：@a 是被减数集合，@b 是减数集合；二者均为纯输入值。
 * 出参/返回：返回覆盖所有 `x-y (mod 2^64)` 的 tnum，无输出参数和副作用。
 * 注意事项：不表达有符号溢出，按 u64 环绕；无需锁且不会睡眠。
 */
struct tnum tnum_sub(struct tnum a, struct tnum b)
{
	/* dv 是基准差；加被减数 mask、减减数 mask 得到借位可能到达的两个边界。 */
	u64 dv, alpha, beta, chi, mu;

	dv = a.value - b.value;
	alpha = dv + a.mask;
	beta = dv - b.mask;
	chi = alpha ^ beta;
	/* 原未知位或两个边界不一致的位置均不可继续宣称为已知。 */
	mu = chi | a.mask | b.mask;
	return TNUM(dv & ~mu, mu);
}

/*
 * tnum_neg() - 计算 tnum 的二进制补码相反数。
 * 业务背景：验证器模拟 BPF_NEG 时复用已经证明保守的减法，避免复制借位传播逻辑。
 * 入参：@a 为纯输入 tnum。
 * 出参/返回：返回 `0-a (mod 2^64)` 的保守集合，无副作用。
 * 注意事项：继承 tnum_sub() 的环绕语义；不分配、不睡眠。
 */
struct tnum tnum_neg(struct tnum a)
{
	return tnum_sub(TNUM(0, 0), a);
}

/*
 * tnum_and() - 保守计算两个 tnum 的逐位与。
 * 业务背景：已知 0 可直接把结果位钉为 0；只有双方都可能为 1 且不能确定时才未知。
 * 入参：@a/@b 为纯输入 tnum 集合。
 * 出参/返回：返回覆盖所有 `x & y` 的最精确逐位 tnum，无副作用。
 * 注意事项：各位独立，无算术进位；纯计算、无需同步。
 */
struct tnum tnum_and(struct tnum a, struct tnum b)
{
	/* alpha/beta 表示每个输入“可能为 1”的位，v 表示双方“确定为 1”的位。 */
	u64 alpha, beta, v;

	alpha = a.value | a.mask;
	beta = b.value | b.mask;
	v = a.value & b.value;
	return TNUM(v, alpha & beta & ~v);
}

/*
 * tnum_or() - 保守计算两个 tnum 的逐位或。
 * 业务背景：任一输入确定为 1 即可把结果钉为 1，其余输入未知位才传播到结果。
 * 入参：@a/@b 为纯输入集合。
 * 出参/返回：返回覆盖所有 `x | y` 的逐位 tnum，无副作用。
 * 注意事项：用 `~v` 清掉已确定为 1 的 mask 位，以维持 value&mask==0。
 */
struct tnum tnum_or(struct tnum a, struct tnum b)
{
	/* v 收集确定 1；mu 先合并未知性，再排除已经由 v 决定的位。 */
	u64 v, mu;

	v = a.value | b.value;
	mu = a.mask | b.mask;
	return TNUM(v, mu & ~v);
}

/*
 * tnum_xor() - 保守计算两个 tnum 的逐位异或。
 * 业务背景：只有双方对应位都已知时 XOR 才已知；任一未知就必须保留两种可能。
 * 入参：@a/@b 为纯输入集合。
 * 出参/返回：返回覆盖所有 `x ^ y` 的逐位 tnum，无外部副作用。
 * 注意事项：结果 value 必须清掉 mu 位；函数纯计算且不睡眠。
 */
struct tnum tnum_xor(struct tnum a, struct tnum b)
{
	/* v 是假定双方均已知时的 XOR；mu 标记实际无法作此假定的位置。 */
	u64 v, mu;

	v = a.value ^ b.value;
	mu = a.mask | b.mask;
	return TNUM(v & ~mu, mu);
}

/* Perform long multiplication, iterating through the bits in a using rshift:
 * - if LSB(a) is a known 0, keep current accumulator
 * - if LSB(a) is a known 1, add b to current accumulator
 * - if LSB(a) is unknown, take a union of the above cases.
 *
 * For example:
 *
 *               acc_0:        acc_1:
 *
 *     11 *  ->      11 *  ->      11 *  -> union(0011, 1001) == x0x1
 *     x1            01            11
 * ------        ------        ------
 *     11            11            11
 *    xx            00            11
 * ------        ------        ------
 *   ????          0011          1001
 */
/*
 * tnum_mul() - 以逐位长乘法保守计算两个 tnum 的模乘积。
 * 业务背景：乘法同时包含移位、加法和未知乘数位的分支；验证器需要有限 tnum 而不能
 * 枚举指数级具体值，所以未知位用 tnum_union() 合并“乘/不乘”两条路径。
 * 入参：@a 是逐轮消费的乘数副本，@b 是逐轮左移的被乘数副本，均为纯输入。
 * 出参/返回：返回覆盖所有 `x*y (mod 2^64)` 的 tnum；输入实参和外界状态不变。
 * 注意事项：最多迭代到 a 的最高可能 1 位；纯计算、不睡眠，但可能因合并损失精度。
 * 原英文给出的 11*x1 示例展示：未知最低位分别取 0/1 后得到 0011/1001，合并为 x0x1。
 */
struct tnum tnum_mul(struct tnum a, struct tnum b)
{
	/* acc 从确定的 0 开始，每轮处理 a 当前最低位对应的一列部分积。 */
	struct tnum acc = TNUM(0, 0);

	while (a.value || a.mask) {
		/* LSB of tnum a is a certain 1 */
		/* a 的最低位确定为 1：该列必然贡献 b，用保守加法累计。 */
		if (a.value & 1)
			acc = tnum_add(acc, b);
		/* LSB of tnum a is uncertain */
		/* a 的最低位未知：具体执行可能保持 acc，也可能加 b，必须合并两种集合。 */
		else if (a.mask & 1) {
			/* acc = tnum_union(acc_0, acc_1), where acc_0 and
			 * acc_1 are partial accumulators for cases
			 * LSB(a) = certain 0 and LSB(a) = certain 1.
			 * acc_0 = acc + 0 * b = acc.
			 * acc_1 = acc + 1 * b = tnum_add(acc, b).
			 */
			/* acc_0 是原累加器，acc_1 是加上本列 b；union 取覆盖两者的最优 tnum。 */

			acc = tnum_union(acc, tnum_add(acc, b));
		}
		/* Note: no case for LSB is certain 0 */
		/* 最低位确定为 0 时不更新 acc；随后 a 右移、b 左移，进入下一列。 */
		a = tnum_rshift(a, 1);
		b = tnum_lshift(b, 1);
	}
	return acc;
}

/*
 * tnum_overlap() - 判断两个 tnum 的具体集合是否可能相交。
 * 业务背景：验证器比较两个寄存器的相等/不等分支时，若共同已知位已冲突即可排除路径。
 * 入参：@a/@b 为纯输入集合。
 * 出参/返回：共同确定的所有位一致返回 true，否则 false；不返回交集本身。
 * 注意事项：true 只表示存在兼容取值，不表示两集合相等；纯计算、无同步要求。
 */
bool tnum_overlap(struct tnum a, struct tnum b)
{
	/* mu 只保留双方都已知的位置；任一方未知的位置不会造成集合不相交。 */
	u64 mu;

	mu = ~a.mask & ~b.mask;
	return (a.value & mu) == (b.value & mu);
}

/* Note that if a and b disagree - i.e. one has a 'known 1' where the other has
 * a 'known 0' - this will return a 'known 1' for that bit.
 */
/*
 * tnum_intersect() - 在输入约束兼容时收紧为同时满足两者的 tnum。
 * 业务背景：验证器把 var_off 与数值范围或另一寄存器相等约束相交，以获得更多已知位。
 * 入参：@a/@b 为纯输入 tnum；调用者应确保其已知位没有互相矛盾。
 * 出参/返回：返回合并双方已知信息的 tnum，无副作用。
 * 注意事项：原文警告，若一方已知 1、另一方已知 0，函数仍把该位返回为已知 1；
 * 因而它不是能表达空集的一般集合交算法，冲突检查须由 tnum_overlap() 或调用上下文保证。
 */
struct tnum tnum_intersect(struct tnum a, struct tnum b)
{
	/* value 用 OR 接纳任一方的已知 1；只有双方都未知的位才继续留在交集 mask。 */
	u64 v, mu;

	v = a.value | b.value;
	mu = a.mask & b.mask;
	return TNUM(v & ~mu, mu);
}

/* Returns a tnum with the uncertainty from both a and b, and in addition, new
 * uncertainty at any position that a and b disagree. This represents a
 * superset of the union of the concrete sets of both a and b. Despite the
 * overapproximation, it is optimal.
 */
/*
 * tnum_union() - 求能同时覆盖两个 tnum 集合的最精确 tnum 上界。
 * 业务背景：验证器合并控制流状态或未知乘数分支时，必须保留任一路径可能产生的值。
 * 入参：@a/@b 为纯输入集合，无先后和包含关系要求。
 * 出参/返回：返回覆盖两者具体集合并集的最优 tnum 表示；可能因表达能力产生过近似。
 * 注意事项：原文强调新增未知位既来自双方 mask，也来自双方已知值不一致的位置；纯计算。
 */
struct tnum tnum_union(struct tnum a, struct tnum b)
{
	/* 只有双方都确定为 1 的位可先留在 v；已知值分歧或任一未知的位全部进入 mu。 */
	u64 v = a.value & b.value;
	u64 mu = (a.value ^ b.value) | a.mask | b.mask;

	/* 清除未知位置上的 value 位，恢复规范形式；该规范形式在所有可表示上界中最精确。 */
	return TNUM(v & ~mu, mu);
}

/*
 * tnum_cast() - 只保留 tnum 的低 @size 字节。
 * 业务背景：验证器模拟窄宽度 load/cast 时丢弃高位知识，再由调用点决定是否符号扩展。
 * 入参：@a 为输入输出式局部副本；@size 是 1～7 的字节数，当前验证器传 1、2 或 4。
 * 出参/返回：返回高于 `size*8` 的 value/mask 位均清零的 tnum，无外部副作用。
 * 注意事项：表达式在 size=8 时会执行 `1ULL<<64`，故调用者不可传 8；纯计算且不睡眠。
 */
struct tnum tnum_cast(struct tnum a, u8 size)
{
	/* 同一低位掩码分别裁剪确定值和未知位，保持规范不变量。 */
	a.value &= (1ULL << (size * 8)) - 1;
	a.mask &= (1ULL << (size * 8)) - 1;
	return a;
}

/*
 * tnum_is_aligned() - 判断集合中每个值是否都按 @size 对齐。
 * 业务背景：验证器在允许内存访问前证明“固定偏移+可变偏移”的所有可能值满足访问宽度。
 * 入参：@a 为纯输入偏移集合；@size 为字节对齐，调用契约要求 0 或 2 的幂。
 * 出参/返回：size=0 直接 true；否则低 `log2(size)` 位无确定 1 且无未知位才 true。
 * 注意事项：函数不验证 2 的幂前提；false 只表示无法证明对齐，不必然表示每个具体值都错位。
 */
bool tnum_is_aligned(struct tnum a, u64 size)
{
	if (!size)
		return true;
	return !((a.value | a.mask) & (size - 1));
}

/*
 * tnum_in() - 判断 @b 表示的具体集合是否为 @a 的子集。
 * 业务背景：状态剪枝用旧状态 a 覆盖更精确的当前状态 b；成立时无需重复探索后续指令。
 * 入参：@a 是候选超集，@b 是候选子集，均为纯输入；参数顺序不可颠倒。
 * 出参/返回：b 没有超出 a 的未知位且双方在 a 已知位置相等时返回 true，否则 false。
 * 注意事项：若 a 来自 tnum_range()，判断针对其过近似集合，例如 range(0,2) 也包含 3；
 * 因此 true 不证明 b 落在原始数值端点内。纯计算、无副作用。
 */
bool tnum_in(struct tnum a, struct tnum b)
{
	/* b 在 a 已知位置仍未知，就可能取到 a 不允许的另一位值，不能是子集。 */
	if (b.mask & ~a.mask)
		return false;
	/* a 未知的位置对包含关系没有约束；清掉 b 对应 value 后比较其余确定模式。 */
	b.value &= ~a.mask;
	return a.value == b.value;
}

/*
 * tnum_sbin() - 把 tnum 格式化为固定 64 位的三态二进制字符串。
 * 业务背景：验证器诊断需要用 0/1/x 直接展示逐位知识；接口采用 snprintf 式返回长度。
 * 入参：@str 是调用者提供的可写输出缓冲区；@size 是含终止 NUL 的容量，必须至少为 1；
 *       @a 是纯输入 tnum。函数不接管缓冲区 ownership。
 * 出参/返回：最多写 `size-1` 个字符并终止，恒返回理想内容长度 64；截断不算错误。
 * 注意事项：size=0 会在终止位置计算中下溢，调用者必须保证非零；无分配、锁或睡眠。
 */
int tnum_sbin(char *str, size_t size, struct tnum a)
{
	/* n 从 64 递减，而 a 从最低位向右消费；写入 n-1 后最终得到高位在左的常规显示。 */
	size_t n;

	for (n = 64; n; n--) {
		if (n < size) {
			/* mask 优先：未知位显示 x；否则 value 区分确定 1 与确定 0。 */
			if (a.mask & 1)
				str[n - 1] = 'x';
			else if (a.value & 1)
				str[n - 1] = '1';
			else
				str[n - 1] = '0';
		}
		a.mask >>= 1;
		a.value >>= 1;
	}
	/* size<=64 时 NUL 位于 size-1；容量更大时位于完整 64 字符之后。 */
	str[min(size - 1, (size_t)64)] = 0;
	return 64;
}

/*
 * tnum_subreg() - 提取 64 位抽象值的低 32 位子寄存器。
 * 业务背景：BPF ALU32/JMP32 只观察低半部，验证器用该视图独立维护 32 位知识。
 * 入参：@a 为纯输入 64 位 tnum。
 * 出参/返回：返回低 4 字节，高 32 位确定为 0；无副作用。
 * 注意事项：这是 tnum_cast(a, 4) 的窄包装，纯计算且不改变原 a。
 */
struct tnum tnum_subreg(struct tnum a)
{
	return tnum_cast(a, 4);
}

/*
 * tnum_clear_subreg() - 清除低 32 位，同时保留高 32 位的确定/未知信息。
 * 业务背景：验证器准备覆盖一次 32 位写结果时，先从旧 64 位寄存器提取不受影响的高半部。
 * 入参：@a 为纯输入寄存器抽象值。
 * 出参/返回：低半 value/mask 均为 0，高半不变；无副作用。
 * 注意事项：右移再左移同时作用于 value/mask，因而不会把旧低位未知性残留到结果。
 */
struct tnum tnum_clear_subreg(struct tnum a)
{
	return tnum_lshift(tnum_rshift(a, 32), 32);
}

/*
 * tnum_with_subreg() - 用另一个 tnum 的低 32 位替换寄存器低半部。
 * 业务背景：JMP32 约束或子寄存器运算只收紧低半部时，高 32 位知识必须原样保留。
 * 入参：@reg 提供高 32 位；@subreg 提供低 32 位，均为纯输入且不转移 ownership。
 * 出参/返回：返回拼接后的规范 tnum；subreg 高半部被忽略，无副作用。
 * 注意事项：两部分位域不重叠，复用 tnum_or() 不会引入额外未知性。
 */
struct tnum tnum_with_subreg(struct tnum reg, struct tnum subreg)
{
	return tnum_or(tnum_clear_subreg(reg), tnum_subreg(subreg));
}

/*
 * tnum_const_subreg() - 把寄存器低 32 位替换成确定常量。
 * 业务背景：验证器处理 ALU32 立即数结果时保留 reg 高半信息，同时精确记录低半。
 * 入参：@a 提供原高半；@value 是新的确定 u32 值，二者均为纯输入。
 * 出参/返回：返回高半来自 a、低半等于 value 的 tnum，无副作用。
 * 注意事项：组合 tnum_const() 与 tnum_with_subreg()，不会影响调用者的 a 副本。
 */
struct tnum tnum_const_subreg(struct tnum a, u32 value)
{
	return tnum_with_subreg(a, tnum_const(value));
}

/*
 * tnum_bswap16() - 在低 16 位内同步交换 value/mask 的两个字节。
 * 业务背景：验证器模拟 16 位 BPF endian 转换，未知性必须随其对应数据位一起换位。
 * 入参：@a 为纯输入；只有低 16 位属于操作数。
 * 出参/返回：返回交换后的 16 位 tnum，高位清零，无副作用。
 * 注意事项：先掩码再 swab16，确保窄操作不会泄漏旧高位知识。
 */
struct tnum tnum_bswap16(struct tnum a)
{
	return TNUM(swab16(a.value & 0xFFFF), swab16(a.mask & 0xFFFF));
}

/*
 * tnum_bswap32() - 在低 32 位内同步反转 value/mask 的字节序。
 * 业务背景：验证器模拟 32 位 endian 指令；确定性属性必须跟随每个字节的新位置。
 * 入参：@a 为纯输入，低 32 位有效。
 * 出参/返回：返回交换后的 32 位 tnum，高位清零，无副作用。
 * 注意事项：该函数只重排位，不增加或消除未知性，纯计算且不睡眠。
 */
struct tnum tnum_bswap32(struct tnum a)
{
	return TNUM(swab32(a.value & 0xFFFFFFFF), swab32(a.mask & 0xFFFFFFFF));
}

/*
 * tnum_bswap64() - 同步反转完整 64 位 value/mask 的字节序。
 * 业务背景：验证器模拟 64 位 endian 指令，保持每个未知位与其数据位成对迁移。
 * 入参：@a 为纯输入完整 64 位 tnum。
 * 出参/返回：返回八字节反序后的 tnum，无副作用。
 * 注意事项：swab64 是固定置换，结果仍满足 value&mask==0；无需锁且不睡眠。
 */
struct tnum tnum_bswap64(struct tnum a)
{
	return TNUM(swab64(a.value), swab64(a.mask));
}

/* Given tnum t, and a number z such that tmin <= z < tmax, where tmin
 * is the smallest member of the t (= t.value) and tmax is the largest
 * member of t (= t.value | t.mask), returns the smallest member of t
 * larger than z.
 *
 * For example,
 * t      = x11100x0
 * z      = 11110001 (241)
 * result = 11110010 (242)
 *
 * Note: if this function is called with z >= tmax, it just returns
 * early with tmax; if this function is called with z < tmin, the
 * algorithm already returns tmin.
 */
/*
 * tnum_step() - 求 tnum 中严格大于 @z 的最小成员，并在边界处饱和。
 * 业务背景：验证器把连续数值上下界与离散 tnum 集合互相收紧时，需要找到区间内下一
 * 个实际可表示值，而非简单做 z+1。调用链位于寄存器 bounds 同步阶段。
 * 入参：@t 是纯输入、满足规范不变量的集合；@z 是搜索起点。常规契约为
 *       t.value <= z < t.value|t.mask，但函数也显式处理两侧越界。
 * 出参/返回：区间内返回最小成员 r>z；z<tmin 返回 tmin；z>=tmax 返回 tmax，无副作用。
 * 注意事项：边界返回不一定严格大于 z，调用者必须结合前置比较解释；纯计算、不睡眠。
 * 原英文示例 x11100x0 在 z=241 后的下一成员是 242，并说明两类越界饱和行为。
 */
u64 tnum_step(struct tnum t, u64 z)
{
	/* tmax 令所有未知位取 1；d/filled/inc 用于只在 mask 允许的位置传播一次进位。 */
	u64 tmax, d, carry_mask, filled, inc;

	tmax = t.value | t.mask;

	/* if z >= largest member of t, return largest member of t */
	/* z 已到达/越过最大成员时没有更大成员，按接口约定饱和返回 tmax。 */
	if (z >= tmax)
		return tmax;

	/* if z < smallest member of t, return smallest member of t */
	/* t.value 是所有未知位取 0 的最小成员；它自然是 z 以下越界时的下一成员。 */
	if (z < t.value)
		return t.value;

	/*
	 * Let r be the result tnum member, z = t.value + d.
	 * Every tnum member is t.value | s for some submask s of t.mask,
	 * and since t.value & t.mask == 0, t.value | s == t.value + s.
	 * So r > z becomes s > d where d = z - t.value.
	 *
	 * Find the smallest submask s of t.mask greater than d by
	 * "incrementing d within the mask": fill every non-mask
	 * position with 1 (`filled`) so +1 ripples through the gaps,
	 * then keep only mask bits. `carry_mask` additionally fills
	 * positions below the highest non-mask 1 in d, preventing
	 * it from trapping the carry.
	 */
	/*
	 * 令 z=t.value+d。任何成员都是 t.value|s，s 为 t.mask 子掩码；因 value 与
	 * mask 不相交，问题化为寻找最小的 s>d。把非 mask 位填 1 后加一，可让进位
	 * 穿越“不允许选择”的空洞；carry_mask 还填充 d 中最高非 mask 置位以下的位置，
	 * 防止进位提前被这些空洞截住。最后与 mask 相与，只保留合法可选位。
	 */
	d = z - t.value;
	carry_mask = (1ULL << fls64(d & ~t.mask)) - 1;
	filled = d | carry_mask | ~t.mask;
	inc = (filled + 1) & t.mask;
	return t.value | inc;
}
