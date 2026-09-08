/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */

/*
 * circular number（cnum）模板学习导读
 *
 * cnum 把 T 位整数空间视作模 2^T 的圆环，用 `{ base, size }` 表示从 base
 * 沿无符号递增方向走 size 的闭弧，故包含 size+1 个具体值。它能以同一对象
 * 表达普通区间、跨 U*_MAX/0 的无符号双段，以及跨 S*_MAX/S*_MIN 的有符号
 * 双段。验证器在 `bpf_reg_state.r32/r64` 中保存 cnum，与逐位 tnum 反复求交，
 * 既跟踪线性上下界也保留 ALU 环绕结果。
 *
 * 本文件故意没有 include guard：cnum.c 令 T=32 和 T=64 各包含一次。下面的
 * 类型/函数拼接宏分别展开成 cnum32/u32/s32/... 与 cnum64/u64/s64/...；每个
 * 源码函数因此生成两个实体。所有函数只操作按值对象，不分配、不睡眠、无锁和
 * 引用；`*_with` 变体只原地更新调用者提供的目标值。
 */

/* T 是模板位宽的强制参数；直接包含会报错，防止意外生成名称不完整的符号。 */
#ifndef T
#error "Define T (bit width: 32, 64) before including cnum_defs.h"
#endif

#include <linux/cnum.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/minmax.h>
/* compiler_types 提供类型/编译器标注；以上头文件共同提供 cnum 声明、极值与 min/max。 */
#include <linux/compiler_types.h>

/*
 * 下列临时宏建立“位宽 → 类型、极值、哨兵和函数名”映射，并在文件末尾全部撤销，
 * 避免第一次 T=32 展开污染第二次 T=64 展开或包含者的后续源码。
 */
#define cnum_t   __PASTE(cnum, T)
#define ut       __PASTE(u, T)
#define st       __PASTE(s, T)
#define UT_MAX   __PASTE(__PASTE(U, T), _MAX)
#define ST_MAX   __PASTE(__PASTE(S, T), _MAX)
#define ST_MIN   __PASTE(__PASTE(S, T), _MIN)
#define EMPTY    __PASTE(__PASTE(CNUM, T), _EMPTY)
#define FN(name) __PASTE(__PASTE(cnum, T), __PASTE(_, name))

/*
 * cnum{T}_from_urange() - 从不跨无符号断点的闭区间构造圆弧。
 * 业务背景：验证器把已知 umin/umax 写入 r32/r64；base=min，跨度为 max-min。
 * 入参：@min/@max 为纯输入无符号端点，调用者必须保证 min<=max。
 * 出参/返回：返回精确覆盖 [min,max] 的 cnum，无外部副作用。
 * 注意事项：错误的端点顺序会按模减法变成环绕弧；纯计算且不睡眠。
 */
struct cnum_t FN(from_urange)(ut min, ut max)
{
	return (struct cnum_t){ .base = min, .size = (ut)max - min };
}

/*
 * cnum{T}_from_srange() - 从不跨有符号顺序的闭区间构造圆弧。
 * 业务背景：把 smin/smax 转成相同位模式的圆环坐标，使负数到正数可跨无符号零点表示。
 * 入参：@min/@max 为纯输入有符号端点，调用者保证 min<=max。
 * 出参/返回：返回精确有符号区间；完整位宽范围规范化为 `{0,UT_MAX}`。
 * 注意事项：先转无符号再相减避免有符号溢出；无分配和并发状态。
 */
struct cnum_t FN(from_srange)(st min, st max)
{
	/* size 是按圆环方向的跨度；满圆统一从 0 开始，避免等价表示影响原始字段比较。 */
	ut size = (ut)max - (ut)min;
	ut base = size == UT_MAX ? 0 : (ut)min;

	return (struct cnum_t){ .base = base, .size = size };
}

/* True if this cnum represents two unsigned ranges. */
/*
 * cnum{T}_urange_overflow() - 判断圆弧是否跨过 UT_MAX/0 无符号断点。
 * 业务背景：线性 umin/umax 无法精确表达跨断点圆弧，查询函数据此退化为全范围。
 * 入参：@cnum 为纯输入且不得误用 EMPTY 哨兵。
 * 出参/返回：base+size 会发生模回绕时 true，否则 false；无副作用。
 * 注意事项：原英文说“表示两个无符号区间”；减法形式避免判定本身发生溢出。
 */
static inline bool FN(urange_overflow)(struct cnum_t cnum)
{
	/* Same as cnum.base + cnum.size > UT_MAX but avoids overflow */
	/* 等价于判断 base+size>UT_MAX，但改写为 size>UT_MAX-base，避免计算和先溢出。 */
	return cnum.size > UT_MAX - (ut)cnum.base;
}

/*
 * cnum{T}_umin / cnum{T}_umax query an unsigned range represented by this cnum.
 * If cnum represents a range crossing the UT_MAX/0 boundary, the unbound range
 * [0..UT_MAX] is returned.
 */
/*
 * cnum{T}_umin() - 查询圆弧可安全对外声明的无符号下界。
 * 业务背景：验证器旧式 umin 接口需要单个线性区间；跨断点时只能保守返回 0。
 * 入参：@cnum 为纯输入非 EMPTY 圆弧。
 * 出参/返回：普通弧返回 base，环绕弧返回 0；无副作用。
 * 注意事项：与 umax() 配对；原文说明环绕弧会共同退化为 [0,UT_MAX]。
 */
ut FN(umin)(struct cnum_t cnum)
{
	return FN(urange_overflow)(cnum) ? 0 : cnum.base;
}
EXPORT_SYMBOL_GPL(FN(umin));

/*
 * cnum{T}_umax() - 查询圆弧可安全对外声明的无符号上界。
 * 业务背景：与 umin() 组成验证器线性无符号 bounds；跨零点时放弃精度但不漏值。
 * 入参：@cnum 为纯输入非 EMPTY 圆弧。
 * 出参/返回：普通弧返回 base+size，环绕弧返回 UT_MAX；无副作用。
 * 注意事项：导出符号供验证器/测试消费者使用，函数纯计算且不睡眠。
 */
ut FN(umax)(struct cnum_t cnum)
{
	return FN(urange_overflow)(cnum) ? UT_MAX : cnum.base + cnum.size;
}
EXPORT_SYMBOL_GPL(FN(umax));

/* True if this cnum represents two signed ranges. */
/*
 * cnum{T}_srange_overflow() - 判断圆弧是否跨过有符号 ST_MAX/ST_MIN 断点。
 * 业务背景：同一位模式按有符号排序时，跨符号翻转点会拆成两段，线性 bounds 需退化。
 * 入参：@cnum 为纯输入圆弧。
 * 出参/返回：圆弧同时包含 ST_MAX 与 ST_MIN 时 true，否则 false。
 * 注意事项：复用 contains() 的模环语义；不改变对象且无需同步。
 */
static inline bool FN(srange_overflow)(struct cnum_t cnum)
{
	return FN(contains)(cnum, (ut)ST_MAX) && FN(contains)(cnum, (ut)ST_MIN);
}

/*
 * cnum{T}_smin / cnum{T}_smax query a signed range represented by this cnum.
 * If cnum represents a range crossing the ST_MAX/ST_MIN boundary, the unbound range
 * [ST_MIN..ST_MAX] is returned.
 */
/*
 * cnum{T}_smin() - 查询圆弧的保守有符号下界。
 * 业务背景：验证器需要 smin；若圆弧跨符号断点，单一区间只能返回 ST_MIN。
 * 入参：@cnum 为纯输入非 EMPTY 圆弧。
 * 出参/返回：不跨断点时返回两端按有符号解释后的较小值，否则 ST_MIN。
 * 注意事项：与 smax() 配对恢复原文所述 [ST_MIN,ST_MAX] 退化范围。
 */
st FN(smin)(struct cnum_t cnum)
{
	return FN(srange_overflow)(cnum)
	       ? ST_MIN
	       : min((st)cnum.base, (st)(cnum.base + cnum.size));
}

/*
 * cnum{T}_smax() - 查询圆弧的保守有符号上界。
 * 业务背景：验证器需要 smax；跨符号断点时必须放宽到 ST_MAX。
 * 入参：@cnum 为纯输入非 EMPTY 圆弧。
 * 出参/返回：不跨断点时返回两端有符号值的较大者，否则 ST_MAX。
 * 注意事项：只读按值对象、无副作用；不能单独用线性结果重建原环绕圆弧。
 */
st FN(smax)(struct cnum_t cnum)
{
	return FN(srange_overflow)(cnum)
	       ? ST_MAX
	       : max((st)cnum.base, (st)(cnum.base + cnum.size));
}

/*
 * Returns a possibly empty intersection of cnums 'a' and 'b'.
 * If 'a' and 'b' intersect in two sub-arcs, the function over-approximates
 * and returns either 'a' or 'b', whichever is smaller.
 */
/*
 * cnum{T}_intersect() - 求两个圆弧交集的单 cnum 保守表示。
 * 业务背景：验证器把独立来源的 bounds 收紧；多数交集是一段，双弧交集因表示能力限制
 * 选择较小输入作为过近似，仍覆盖真实交集。
 * 入参：@a/@b 为纯输入圆弧，可为空、普通或环绕；参数交换不改变集合语义。
 * 出参/返回：返回 EMPTY、精确单弧交集，或双子弧场景下较小输入；无副作用。
 * 注意事项：原文明确双子弧结果会过近似；这只损失拒绝能力，不能遗漏真实值。
 */
struct cnum_t FN(intersect)(struct cnum_t a, struct cnum_t b)
{
	/* b1 是旋转坐标中的 b，dbase 是两个起点沿无符号方向的距离。 */
	struct cnum_t b1;
	ut dbase;

	if (FN(is_empty)(a) || FN(is_empty)(b))
		return EMPTY;

	/* 先按数值起点排序，再把 a.base 旋转到 0，可用普通大小比较分析圆弧布局。 */
	if (a.base > b.base)
		swap(a, b);

	/*
	 * Rotate frame of reference such that a.base is 0.
	 * 'b1' is 'b' in this frame of reference.
	 */
	/* 把参考系旋转到 a.base=0；b1 保存 b 在新坐标中的起点和原跨度。 */
	dbase = b.base - a.base;
	b1 = (struct cnum_t){ dbase, b.size };
	if (FN(urange_overflow)(b1)) {
		if (b1.base <= a.size) {
			/*
			 * Rotated frame (a.base at origin):
			 *
			 * 0                                       UT_MAX
			 * |--------------------------------------------|
			 * [=== a ==========================]           |
			 * [= b1 tail =]  [========= b1 main ==========>]
			 *                 ^-- b1.base <= a.size
			 *
			 * 'a' and 'b' intersect in two disjoint arcs,
			 * can't represent as single cnum, over-approximate
			 * the result.
			 */
			/* b1 跨零且尾段也碰到 a，交集分成两段；单弧无法精确表达，返回较小超集。 */
			return a.size <= b.size ? a : b;
		} else {
			/*
			 * Rotated frame (a.base at origin):
			 *
			 * 0                                       UT_MAX
			 * |--------------------------------------------|
			 * [=== a =============]  |                     |
			 * [= b1 tail =]          [======= b1 main ====>]
			 *                         ^-- b1.base > a.size
			 *
			 * Only 'b' tail intersects 'a'.
			 */
			/* b1 主段位于 a 右侧，只有回绕到零点的尾段相交；从 a.base 起截到较短末端。 */
			return (struct cnum_t) {
				.base = a.base,
				.size = min(a.size, (ut)(b1.base + b1.size)),
			};
		}
	} else if (a.size >= b1.base) {
		/*
		 * Rotated frame (a.base at origin):
		 *
		 * 0                                             UT_MAX
		 * |--------------------------------------------------|
		 * [=== a ==================================]         |
		 *                   [== b1 =====================]
		 *
		 * 0                                             UT_MAX
		 * |--------------------------------------------------|
		 * [=== a ==================================]         |
		 *                   [== b1 ====]
		 *                   ^-- b1.base <= a.size
		 *                   |<-- a.size - dbase -->|
		 *
		 * 'a' and 'b' intersect as one cnum.
		 */
		/* b1 不环绕且起点落入 a，交集从原 b.base 开始，跨度取双方剩余部分较小值。 */
		return (struct cnum_t) {
			.base = b.base,
			.size = min((ut)(a.size - dbase), b.size),
		};
	} else {
		/* b1 起点已越过 a 末端且没有回绕尾段，两集合不相交。 */
		return EMPTY;
	}
}

/*
 * cnum{T}_intersect_with() - 把目标圆弧原地收紧为与 @src 的交集。
 * 业务背景：验证器更新寄存器 bounds 时用 in-place 包装减少重复赋值。
 * 入参：@dst 是非 NULL 输入输出指针，内存仍归调用者；@src 是纯输入值。
 * 出参/返回：无直接返回值；`*dst` 被替换为 intersect 结果，src 不变。
 * 注意事项：调用者负责 dst 的并发稳定；函数本身不加锁、不睡眠。
 */
void FN(intersect_with)(struct cnum_t *dst, struct cnum_t src)
{
	*dst = FN(intersect)(*dst, src);
}

/*
 * cnum{T}_intersect_with_urange() - 用无符号闭区间原地收紧目标。
 * 业务背景：条件跳转推导出 umin/umax 后，先构造 cnum 再复用统一交集算法。
 * 入参：@dst 为调用者持有的非 NULL 输入输出对象；@min/@max 要求 min<=max。
 * 出参/返回：无直接返回；dst 变为与 [min,max] 的交集，可能 EMPTY。
 * 注意事项：不接管 dst，调用者提供同步；纯计算、不睡眠。
 */
void FN(intersect_with_urange)(struct cnum_t *dst, ut min, ut max)
{
	FN(intersect_with)(dst, FN(from_urange)(min, max));
}

/*
 * cnum{T}_intersect_with_srange() - 用有符号闭区间原地收紧目标。
 * 业务背景：有符号比较分支产生 smin/smax 约束，由 from_srange() 转成圆弧后求交。
 * 入参：@dst 为非 NULL 输入输出对象；@min/@max 是满足 min<=max 的有符号端点。
 * 出参/返回：无直接返回；dst 被收紧或置 EMPTY，无 ownership 变化。
 * 注意事项：调用者串行保护 dst；函数不分配、不睡眠。
 */
void FN(intersect_with_srange)(struct cnum_t *dst, st min, st max)
{
	FN(intersect_with)(dst, FN(from_srange)(min, max));
}

/*
 * cnum{T}_normalize() - 统一满圆的非哨兵表示。
 * 业务背景：算术可能生成 size=UT_MAX 且 base 任意的等价满圆；统一 base 便于比较和判空。
 * 入参：@cnum 为纯输入局部副本。
 * 出参/返回：通常原样返回；满圆除 base=0 或 base=ST_MAX 的形态外改为 base=0。
 * 注意事项：EMPTY 实际编码为 base=UT_MAX/size=UT_MAX，算术入口会先单独过滤；
 * 本 helper 明确保留的特殊形态是 base=ST_MAX，不能把它误解为 EMPTY。
 */
static inline struct cnum_t FN(normalize)(struct cnum_t cnum)
{
	if (cnum.size == UT_MAX && cnum.base != 0 && cnum.base != (ut)ST_MAX)
		cnum.base = 0;
	return cnum;
}

/*
 * cnum{T}_add() - 保守计算两个圆弧中任意成员的模加法集合。
 * 业务背景：验证器模拟 BPF_ADD/指针偏移；两段连续弧相加的起点相加、跨度相加。
 * 入参：@a/@b 为纯输入，可为空或环绕，无 ownership。
 * 出参/返回：任一为空返回 EMPTY；跨度和过大返回 UNBOUNDED；否则返回规范圆弧。
 * 注意事项：按 T 位无符号模运算；先检查 size 加法防溢出，纯计算且不睡眠。
 */
struct cnum_t FN(add)(struct cnum_t a, struct cnum_t b)
{
	/* 空集合与任何集合做点对点加法仍为空；跨度过大时所有位模式都可能出现。 */
	if (FN(is_empty)(a) || FN(is_empty)(b))
		return EMPTY;
	if (a.size > UT_MAX - b.size)
		return (struct cnum_t){ 0, (ut)UT_MAX };
	else
		/* 起点和按模 T 位环绕；normalize 合并等价满圆表示。 */
		return FN(normalize)((struct cnum_t){ a.base + b.base, a.size + b.size });
}

/*
 * cnum{T}_negate() - 对圆弧内所有成员取二进制补码相反数。
 * 业务背景：验证器以 `a + negate(b)` 复用加法实现 BPF_SUB 的连续范围传播。
 * 入参：@a 为纯输入圆弧，可为空。
 * 出参/返回：EMPTY 保持为空；否则反转圆弧方向后返回相同跨度的新起点。
 * 注意事项：新起点是旧末端的模负值，不能只对 base 取负；纯计算、无副作用。
 */
struct cnum_t FN(negate)(struct cnum_t a)
{
	/* 原弧 [base ... base+size] 取负后顺序反转，新顺时针起点为 -(base+size)。 */
	if (FN(is_empty)(a))
		return EMPTY;
	return FN(normalize)((struct cnum_t){ -((ut)a.base + a.size), a.size });
}

/*
 * cnum{T}_is_empty() - 识别专用空集合哨兵。
 * 业务背景：普通 base/size 组合几乎覆盖全部位模式，接口约定用 EMPTY 表示无可行值。
 * 入参：@cnum 为纯输入值。
 * 出参/返回：两个字段都等于 EMPTY 编码时 true，否则 false；无副作用。
 * 注意事项：size=0 是单常量而非空；所有算术/交集入口必须传播空集合。
 */
bool FN(is_empty)(struct cnum_t cnum)
{
	return cnum.base == EMPTY.base && cnum.size == EMPTY.size;
}

/*
 * cnum{T}_contains() - 判断一个具体位模式是否落在圆弧中。
 * 业务背景：符号断点检查和状态收紧需要针对单值查询集合成员关系。
 * 入参：@cnum 为纯输入圆弧，@v 为待查 T 位无符号位模式。
 * 出参/返回：空集合 false；普通/环绕弧按一段/两段闭区间返回 bool。
 * 注意事项：比较按无符号圆环坐标，不等同于 C 有符号大小关系；纯计算。
 */
bool FN(contains)(struct cnum_t cnum, ut v)
{
	/* 环绕弧由 [base,UT_MAX] 与 [0,base+size] 两段组成，成员满足任一条件。 */
	if (FN(is_empty)(cnum))
		return false;
	if (FN(urange_overflow)(cnum))
		return v >= cnum.base || v <= (ut)cnum.base + cnum.size;
	else
		return v >= cnum.base && v <= (ut)cnum.base + cnum.size;
}

/*
 * cnum{T}_is_const() - 判断圆弧是否只含一个具体值。
 * 业务背景：验证器可把 size=0 的范围提升为精确常量，与 tnum/value 同步。
 * 入参：@cnum 为纯输入，调用者通常已排除 EMPTY。
 * 出参/返回：size==0 返回 true，否则 false；无副作用。
 * 注意事项：EMPTY 的 size=UT_MAX，故自然返回 false；常量值存于 base。
 */
bool FN(is_const)(struct cnum_t cnum)
{
	return cnum.size == 0;
}

/*
 * cnum{T}_is_subset() - 判断 @smaller 是否完全包含在 @bigger 中。
 * 业务背景：验证器状态剪枝要求旧状态 bigger 覆盖当前状态 smaller，才能跳过重复探索。
 * 入参：@bigger 是候选超集，@smaller 是候选子集，均为纯输入；顺序不可颠倒。
 * 出参/返回：集合包含成立返回 true；空子集恒真，非空子集不可能属于空超集。
 * 注意事项：线性 min/max 会把环绕弧中间空洞误算为覆盖，故必须旋转圆环坐标。
 */
bool FN(is_subset)(struct cnum_t bigger, struct cnum_t smaller)
{
	/* 先处理空集规则，再以 bigger.base 为新原点，使 bigger 自身不再环绕。 */
	if (FN(is_empty(smaller)))
		return true;
	if (FN(is_empty(bigger)))
		return false;
	/* rotate both arcs such that 'bigger' starts at origin, hence does not overflow */
	/* 同时旋转两弧，让 bigger 从 0 开始且不环绕；集合包含关系在旋转下保持不变。 */
	smaller.base -= bigger.base;
	bigger.base = 0;
	if (FN(urange_overflow)(smaller) && bigger.size < UT_MAX)
		return false;
	return smaller.base + smaller.size <= bigger.size;
}

/* 模板展开完成后先撤销哨兵、结构体和整数类型别名，避免泄漏到包含者。 */
#undef EMPTY
#undef cnum_t
#undef ut
#undef st
/* 再撤销极值和函数拼接宏；T 由 cnum.c 在每次 include 后单独撤销。 */
#undef UT_MAX
#undef ST_MAX
#undef ST_MIN
#undef FN
