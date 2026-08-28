// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2019-2020 Arm Ltd.

#include <linux/compiler.h>
#include <linux/kasan-checks.h>
#include <linux/kernel.h>

#include <net/checksum.h>

/*
 * 学习背景：Internet checksum 使用“一补数加法”。普通无符号加法溢出的
 * 第 65 位不能丢掉，而要折回最低位（end-around carry）。借助 __uint128_t，
 * 编译器可用 adds/adc 高效地产生这一步，而无需 C 代码显式判断溢出。
 */
static u64 accumulate(u64 sum, u64 data)
{
	/* tmp 保留完整 65 位结果；高 64 位在这里实际只可能是 0 或 1。 */
	__uint128_t tmp = (__uint128_t)sum + data;
	/* 低 64 位加进位；若再次进位，后续 accumulate/fold 会继续折叠。 */
	return tmp + (tmp >> 64);
}

/*
 * We over-read the buffer and this makes KASAN unhappy. Instead, disable
 * instrumentation and call kasan explicitly.
 */
unsigned int __no_sanitize_address do_csum(const unsigned char *buff, int len)
{
	unsigned int offset, shift, sum;
	const u64 *ptr;
	u64 data, sum64 = 0;

	if (unlikely(len <= 0))
		return 0;

	/* offset 是首地址在 8 字节字中的位置；ptr 随后向下对齐。 */
	offset = (unsigned long)buff & 7;
	/*
	 * This is to all intents and purposes safe, since rounding down cannot
	 * result in a different page or cache line being accessed, and @buff
	 * should absolutely not be pointing to anything read-sensitive. We do,
	 * however, have to be careful not to piss off KASAN, which means using
	 * unchecked reads to accommodate the head and tail, for which we'll
	 * compensate with an explicit check up-front.
	 */
	kasan_check_read(buff, len);
	ptr = (u64 *)(buff - offset);
	/* 首次固定消耗一个 8 字节字，所以把循环余量预减 8。 */
	len = len + offset - 8;

	/*
	 * Head: zero out any excess leading bytes. Shifting back by the same
	 * amount should be at least as fast as any other way of handling the
	 * odd/even alignment, and means we can ignore it until the very end.
	 */
	shift = offset * 8;	/* 字节偏移换算成位偏移。 */
	data = *ptr++;
#ifdef __LITTLE_ENDIAN
	data = (data >> shift) << shift;
#else
	data = (data << shift) >> shift;
#endif

	/*
	 * Body: straightforward aligned loads from here on (the paired loads
	 * underlying the quadword type still only need dword alignment). The
	 * main loop strictly excludes the tail, so the second loop will always
	 * run at least once.
	 */
	/* 每轮 64 字节、四条 128 位加载；多累加链可提高指令级并行度。 */
	while (unlikely(len > 64)) {
		__uint128_t tmp1, tmp2, tmp3, tmp4;

		tmp1 = *(__uint128_t *)ptr;
		tmp2 = *(__uint128_t *)(ptr + 2);
		tmp3 = *(__uint128_t *)(ptr + 4);
		tmp4 = *(__uint128_t *)(ptr + 6);

		len -= 64;
		ptr += 8;

		/* This is the "don't dump the carry flag into a GPR" idiom */
		tmp1 += (tmp1 >> 64) | (tmp1 << 64);
		tmp2 += (tmp2 >> 64) | (tmp2 << 64);
		tmp3 += (tmp3 >> 64) | (tmp3 << 64);
		tmp4 += (tmp4 >> 64) | (tmp4 << 64);
		tmp1 = ((tmp1 >> 64) << 64) | (tmp2 >> 64);
		tmp1 += (tmp1 >> 64) | (tmp1 << 64);
		tmp3 = ((tmp3 >> 64) << 64) | (tmp4 >> 64);
		tmp3 += (tmp3 >> 64) | (tmp3 << 64);
		tmp1 = ((tmp1 >> 64) << 64) | (tmp3 >> 64);
		tmp1 += (tmp1 >> 64) | (tmp1 << 64);
		tmp1 = ((tmp1 >> 64) << 64) | sum64;
		tmp1 += (tmp1 >> 64) | (tmp1 << 64);
		sum64 = tmp1 >> 64;
	}
	/* 处理剩余完整数据，同时把最后一个可能越界读取的字留给尾部掩码。 */
	while (len > 8) {
		__uint128_t tmp;

		sum64 = accumulate(sum64, data);
		tmp = *(__uint128_t *)ptr;

		len -= 16;
		ptr += 2;

#ifdef __LITTLE_ENDIAN
		data = tmp >> 64;
		sum64 = accumulate(sum64, tmp);
#else
		data = tmp;
		sum64 = accumulate(sum64, tmp >> 64);
#endif
	}
	if (len > 0) {
		sum64 = accumulate(sum64, data);
		data = *ptr;
		len -= 8;
	}
	/*
	 * Tail: zero any over-read bytes similarly to the head, again
	 * preserving odd/even alignment.
	 */
	/* 此时 len 在 -7..0；-len 是末字中真正有效的字节数。 */
	shift = len * -8;
#ifdef __LITTLE_ENDIAN
	data = (data << shift) >> shift;
#else
	data = (data >> shift) << shift;
#endif
	sum64 = accumulate(sum64, data);

	/* 把 64 位和折成 32 位，再折成最终 16 位一补数和。 */
	sum64 += (sum64 >> 32) | (sum64 << 32);
	sum = sum64 >> 32;
	sum += (sum >> 16) | (sum << 16);
	/* 奇地址会交换校验和的字节相位，最后统一纠正。 */
	if (offset & 1)
		return (u16)swab32(sum);

	return sum >> 16;
}

__sum16 csum_ipv6_magic(const struct in6_addr *saddr,
			const struct in6_addr *daddr,
			__u32 len, __u8 proto, __wsum csum)
{
	/* IPv6 伪首部 = 128 位源/目的地址 + 32 位长度 + 8 位 next-header。 */
	__uint128_t src, dst;
	u64 sum = (__force u64)csum;

	src = *(const __uint128_t *)saddr->s6_addr;
	dst = *(const __uint128_t *)daddr->s6_addr;

	sum += (__force u32)htonl(len);
#ifdef __LITTLE_ENDIAN
	sum += (u32)proto << 24;
#else
	sum += proto;
#endif
	/* 每个 128 位地址的两个 64 位半部相加，并保留端回进位。 */
	src += (src >> 64) | (src << 64);
	dst += (dst >> 64) | (dst << 64);

	sum = accumulate(sum, src >> 64);
	sum = accumulate(sum, dst >> 64);

	sum += ((sum >> 32) | (sum << 32));
	return csum_fold((__force __wsum)(sum >> 32));
}
EXPORT_SYMBOL(csum_ipv6_magic);
