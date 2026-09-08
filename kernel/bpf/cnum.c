// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */

/*
 * cnum 跨位宽收紧学习导读
 *
 * cnum_defs.h 是同一套圆弧算法模板：本文件先以 T=32 生成 cnum32_*，再以
 * T=64 生成 cnum64_*。随后两个显式函数连接验证器的双视图：r64 描述整个
 * 寄存器，r32 描述每个 64 位候选值截断后的低 32 位。二者必须互相收紧，
 * 否则单独看任一视图都会保留可排除值。
 *
 * cnum64_cnum32_intersect() 的目标不是普通集合交，而是筛选
 * `{v in a | (u32)v in b}`。b 每隔 2^32 在 64 位轴上重复；函数只返回一个
 * 连续 64 位圆弧，所以裁掉首尾不合法段，中间周期性空洞仍可能造成安全的
 * 过近似。所有操作均为按值纯计算，无分配、锁、引用和睡眠。
 */
#include <linux/bits.h>

/* 第一次展开生成 cnum32_*；include 后立即撤销 T，防止影响第二次实例化。 */
#define T 32
#include "cnum_defs.h"
#undef T

/* 第二次用同一模板生成 cnum64_*；模板内部也已撤销所有临时拼接宏。 */
#define T 64
#include "cnum_defs.h"
#undef T

/*
 * cnum32_from_cnum64() - 把连续 64 位圆弧投影为低 32 位圆弧。
 * 业务背景：验证器从 r64 向 r32 推导；截断等价于每 2^32 重复一次的模投影。
 * 入参：@cnum 为纯输入 64 位圆弧，可为 EMPTY。
 * 出参/返回：空集返回 CNUM32_EMPTY；至少含 2^32 个连续值时返回全 32 位范围；
 *             否则截断 base 并保留 size，得到精确投影。无副作用。
 * 注意事项：阈值是 size>=U32_MAX，因为闭弧成员数为 size+1；纯计算、不睡眠。
 */
struct cnum32 cnum32_from_cnum64(struct cnum64 cnum)
{
	/* 先传播无可行状态；EMPTY 不能按普通满跨度圆弧参与截断。 */
	if (cnum64_is_empty(cnum))
		return CNUM32_EMPTY;

	/* 连续成员达到 2^32 个时，低 32 位已经走完一整圈，只能是 UNBOUNDED。 */
	if (cnum.size >= U32_MAX)
		return (struct cnum32){ .base = 0, .size = U32_MAX };
	else
		/* 短于一圈时，低位起点加相同跨度恰好描述全部截断结果，包括跨零环绕。 */
		return (struct cnum32){ .base = (u32)cnum.base, .size = cnum.size };
}

/*
 * Suppose 'a' and 'b' are laid out as follows:
 *
 *                                                          64-bit number axis --->
 *
 * N*2^32                   (N+1)*2^32                (N+2)*2^32                (N+3)*2^32
 * ||------|---|=====|-------||----------|=====|-------||----------|=====|----|--||
 *         |   |< b >|                   |< b >|                   |< b >|    |
 *         |   |                                                         |    |
 *         |<--+--------------------------- a ---------------------------+--->|
 *             |                                                         |
 *             |<-------------------------- t -------------------------->|
 *
 * In such a case it is possible to infer a more tight representation t
 * such that ∀ v ∈ a, (u32)v ∈ b: v ∈ t.
 */
/*
 * cnum64_cnum32_intersect() - 用低 32 位约束 b 收紧 64 位圆弧 a。
 * 业务背景：bounds 同步阶段已分别得到 r64=a 与 r32=b；本函数沿 64 位轴重复铺开 b，
 * 裁掉 a 首尾必不满足 `(u32)v in b` 的部分，补足旧 min/max 推导遗漏的环绕场景。
 * 入参：@a 是候选 64 位圆弧，@b 是允许的低 32 位圆弧；均为纯输入，可为空。
 * 出参/返回：任一为空或无交点返回 CNUM64_EMPTY；否则返回包含所有合法 v 的收紧圆弧 t，
 *             可能因无法表示内部周期空洞而过近似。无输出参数或外部副作用。
 * 注意事项：闭弧、32 位截断和 u32 溢出均是算法组成部分；不能改成 64 位饱和算术。
 * 原英文总图表示 b 在每个 2^32 周期重复，t 是从 a 两端裁剪后仍覆盖全部合法值的区间。
 */
struct cnum64 cnum64_cnum32_intersect(struct cnum64 a, struct cnum32 b)
{
	/*
	 * To simplify reasoning, rotate the circles so that [virtual] a1 starts
	 * at u32 boundary, b1 represents b in this new frame of reference.
	 */
	/*
	 * 为简化推理，旋转低 32 位圆，使虚拟 a1 从一个 u32 周期边界开始；b1 是 b
	 * 在新参考系中的位置。t 保存待裁剪的 a，d 是尾部裁剪长度，b1_max 是 b1 末端。
	 */
	struct cnum32 b1 = { b.base - (u32)a.base, b.size };
	struct cnum64 t = a;
	u64 d, b1_max;

	/* 空输入没有任何同时满足两视图的值，必须在把哨兵当普通圆弧运算前直接返回。 */
	if (cnum64_is_empty(a) || cnum32_is_empty(b))
		return CNUM64_EMPTY;

	/* 第一大类：旋转后的 b1 跨 U32_MAX/0，由尾段和主段共同描述允许低位。 */
	if (cnum32_urange_overflow(b1)) {
		b1_max = (u32)b1.base + (u32)b1.size; /* overflow here is fine and necessary */
		/* 此处 u32 回绕是必要语义：b1_max 正是跨零后尾段的右端点。 */
		if ((u32)a.size > b1_max && (u32)a.size < b1.base) {
			/*
			 * N*2^32                   (N+1)*2^32
			 * ||=====|------------|=====||=====|---------|---|=====||
			 *  |b1 ->|            |<- b1||b1 ->|         |   |<- b1|
			 *  |<----------------- a1 ------------------>|
			 *  |<-------------- t ------------>|<-- d -->| (after adjustment)
			 *                                  ^
			 *                                b1_max
			 */
			/* a1 的末端落在 b1 两段之间的禁区，d 是必须从 t 尾部裁掉的距离。 */
			d = (u32)a.size - b1_max;
			t.size -= d;
		} else {
			/*
			 * No adjustments possible in the following cases:
			 *
			 * ||=====|------------|=====||===|=|-------------|=|===||
			 *  |b1 ->|            |<- b1||b1 +>|             |<+ b1|
			 *  |<----------------- a1 ------>|                 |
			 *  |<----------------- (or) a1 ------------------->|
			 */
			/*
			 * 上图两种情况无需调整：a1 末端已在 b1 允许段内，或 a 长到跨越更多周期，
			 * 单一圆弧无法安全删除中间空洞。保留 t=a 是有意的保守结果。
			 */
		}
	} else {
		/* 第二大类：b1 是单个普通低位区间，先裁 a 前缀到本周期第一个允许值。 */
		if (t.size < b1.base)
			/*
			 * N*2^32                   (N+1)*2^32
			 * ||----------|--|=======|--||------>
			 *  |<-- a1 -->|  |<- b ->|
			 */
			/* a 短于到达 b1 起点的距离，首个周期内完全无交点。 */
			return CNUM64_EMPTY;
		/*
		 * N*2^32                   (N+1)*2^32
		 * ||-------------|========|-||-----| -------|========|-||
		 *  |             |<- b1 ->|        |        |<- b1 ->|
		 *  |<------------+ a1 ------------>|
		 *                |<------ t ------>| (after adjustment)
		 */
		/* 丢弃 b1.base 长的非法前缀，使 t.base 指向第一个低 32 位落入 b 的值。 */
		t.base += b1.base;
		t.size -= b1.base;
		b1_max = b1.base + b1.size;
		/* 再检查 a 原末端在最后一个 32 位周期中的位置，计算尾部非法长度 d。 */
		d = 0;
		if ((u32)a.size < b1.base)
			/*
			 * N*2^32                   (N+1)*2^32
			 * ||-------------|========|-||------|-------|========|-||
			 *  |             |<- b1 ->|         |       |<- b1 ->|
			 *  |<------------+-- a1 --+-------->|
			 *                |<- t  ->|<-- d -->| (after adjustment)
			 */
			/* 末端位于下一周期 b1 之前：跨周期尾隙由当前余量加回绕到 b1_max 的距离组成。 */
			d = (u32)a.size + (BIT_ULL(32) - b1_max);
		else if ((u32)a.size >= b1_max)
			/*
			 * N*2^32                   (N+1)*2^32
			 * ||--|========|------------||--|========|-------|-----||
			 *  |  |<- b1 ->|                |<- b1 ->|       |
			 *  |<-+------------------ a1 ------------+------>|
			 *     |<-------------- t --------------->|<- d ->| (after adjustment)
			 */
			/* 末端已越过本周期 b1 末端，裁掉 `(u32)a.size-b1_max` 的后缀。 */
			d = (u32)a.size - b1_max;
		/* 若待裁尾长超过前缀调整后的 t，说明两端收缩相交，实际集合为空。 */
		if (t.size < d)
			return CNUM64_EMPTY;
		t.size -= d;
	}
	/* t 仍以单圆弧覆盖全部合法 64 位值；返回值按值交给验证器写回 r64。 */
	return t;
}
