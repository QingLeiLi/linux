// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 ARM Ltd.
 */

#include <linux/uaccess.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>

void memcpy_flushcache(void *dst, const void *src, size_t cnt)
{
	/*
	 * We assume this should not be called with @dst pointing to
	 * non-cacheable memory, such that we don't need an explicit
	 * barrier to order the cache maintenance against the memcpy.
	 */
	memcpy(dst, src, cnt);	/* 先让新数据进入普通 cacheable 目的区域。 */
	/* clean 到 PoP，保证持久化域/观察者能看到 [dst,dst+cnt) 的新数据。 */
	dcache_clean_pop((unsigned long)dst, (unsigned long)dst + cnt);
}
EXPORT_SYMBOL_GPL(memcpy_flushcache);

unsigned long __copy_user_flushcache(void *to, const void __user *from,
				     unsigned long n)
{
	unsigned long rc;

	/* raw_copy_from_user 返回未复制数，不是已复制数。 */
	rc = raw_copy_from_user(to, from, n);

	/* See above */
	/* fault 时仅 clean 实际成功写入的前缀 [to,to+n-rc)。 */
	dcache_clean_pop((unsigned long)to, (unsigned long)to + n - rc);
	return rc;
}
