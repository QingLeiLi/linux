// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/mmdebug.h>
#include <linux/highmem.h>
#include <linux/poison.h>
#include <linux/ratelimit.h>
#include <linux/kasan.h>

/* 启动参数解析阶段记录用户意图；init_mem_debugging() 之后由 static key 接管热路径。 */
bool _page_poisoning_enabled_early;
EXPORT_SYMBOL(_page_poisoning_enabled_early);
/* 默认关闭的 jump label，启用后 free/alloc 热路径才调用实际 poison 检查。 */
DEFINE_STATIC_KEY_FALSE(_page_poisoning_enabled);
EXPORT_SYMBOL(_page_poisoning_enabled);

/*
 * early_page_poison_param() - 解析 page_poison= 启动布尔值。
 * 业务背景：早期命令行先收集请求，mm_init 后续再与 DEBUG_PAGEALLOC、init_on_*
 * 选项协调并启用 static key。
 * 入参：@buf 是 early_param 借用的 NUL 结尾字符串，非 NULL，不保存。
 * 出参/返回：合法布尔值返回 0 并更新 early 全局量，非法文本返回 kstrtobool errno。
 * 注意事项：__init、单线程启动上下文，可写全局量但不直接发布热路径开关。
 */
static int __init early_page_poison_param(char *buf)
{
	return kstrtobool(buf, &_page_poisoning_enabled_early);
}
early_param("page_poison", early_page_poison_param);

/*
 * poison_page() - 用 PAGE_POISON 覆盖一页刚释放的物理内存。
 * 业务背景：伙伴释放路径在页尚未重新分配前调用，后续 unpoison_page() 通过
 * 比较图样发现 use-after-free 写入。
 * 入参：@page 是调用者借用且当前独占的单页描述符，不增加引用。
 * 出参/返回：无直接返回值；整页内容被改写，页 ownership 仍归释放路径。
 * 注意事项：kmap_local_page 的映射仅在当前线程/CPU 局部有效；函数临时关闭
 * 当前 KASAN 检查以避免调试器把主动写 poison 误报，结束前恢复并解除映射。
 */
static void poison_page(struct page *page)
{
	/* addr 是 PAGE_SIZE 字节局部映射，只在 kunmap_local() 前有效。 */
	void *addr = kmap_local_page(page);

	/* KASAN still think the page is in-use, so skip it. */
	/* KASAN 此刻仍把页视为在用，因此跳过本次人工覆盖；这不改变页的 KASAN 元数据。 */
	kasan_disable_current();
	/* reset_tag 去除 HW/SW tag 后填满稳定字节 0xaa，供重新分配时核对。 */
	memset(kasan_reset_tag(addr), PAGE_POISON, PAGE_SIZE);
	kasan_enable_current();
	kunmap_local(addr);
}

/*
 * __kernel_poison_pages() - 逐页 poison 一个连续 page 数组。
 * 业务背景：kernel_poison_pages() 的 static-key 包装在高阶页归还 buddy 前调用。
 * 入参：@page 为首个借用 page；@n 为连续页数，必须非负且范围有效。
 * 出参/返回：无直接返回值；每页内容被覆盖，不改变引用或 buddy ownership。
 * 注意事项：成本与 @n 成正比，逐页 local map；调用者保证页不被并发访问。
 */
void __kernel_poison_pages(struct page *page, int n)
{
	/* i 是 [0,n) 的页偏移，不是 PFN。 */
	int i;

	/* 每次只映射一页，避免高端内存无法构造永久连续虚拟映射。 */
	for (i = 0; i < n; i++)
		poison_page(page + i);
}

/*
 * single_bit_flip() - 判断两个字节是否恰好相差一个 bit。
 * 业务背景：poison 损坏报告用它区分可能的单比特硬件错误与一般内存覆盖。
 * 入参：@a/@b 是纯输入字节。
 * 出参/返回：异或结果只有一个置位 bit 时返回 true；相同或多 bit 不同返回 false。
 * 注意事项：无锁、无副作用、不睡眠；x & (x - 1) 清除最低置位 bit。
 */
static bool single_bit_flip(unsigned char a, unsigned char b)
{
	/* error 的每个置位 bit 表示 @a 与 @b 在该位不同。 */
	unsigned char error = a ^ b;

	return error && !(error & (error - 1));
}

/*
 * check_poison_mem() - 检查范围是否仍全为 poison，并输出最小损坏区间。
 *
 * 业务背景：页重新分配前调用；先找首个异常字节，再反向找最后一个异常字节，
 * 以便限速打印有用范围、栈和 page 元数据而不倾倒整页。
 * 入参：@page 是用于诊断的借用 page；@mem 是该页去 tag 后的借用映射；
 * @bytes 是检查字节数，必须大于 0 且不超过映射长度。均不转移 ownership。
 * 出参/返回：无直接返回值；完好时静默返回；损坏时在 ratelimit 允许下记录
 * 单 bit 或一般 corruption、hex dump、调用栈和 page 状态。不会修复内容。
 * 注意事项：静态 ratelimit 在并发 CPU 间协调日志额度；即使被限速，调用者仍
 * 继续分配该页，因此这是尽力诊断而非隔离机制。
 */
static void check_poison_mem(struct page *page, unsigned char *mem, size_t bytes)
{
	/* 五秒最多十条报告，避免广泛破坏造成日志风暴；start/end 界定异常闭区间。 */
	static DEFINE_RATELIMIT_STATE(ratelimit, 5 * HZ, 10);
	unsigned char *start;
	unsigned char *end;

	/* memchr_inv 返回首个不等于 0xaa 的字节；NULL 表示整段保持完好。 */
	start = memchr_inv(mem, PAGE_POISON, bytes);
	if (!start)
		return;

	/* 从末尾回退到最后一个异常字节，缩小后续 dump 的范围。 */
	for (end = mem + bytes - 1; end > start; end--) {
		if (*end != PAGE_POISON)
			break;
	}

	/* 日志额度耗尽只抑制报告，不改变页分配结果。 */
	if (!__ratelimit(&ratelimit))
		return;
	/* 单个异常字节且仅一 bit 变化时单独标为 single bit error。 */
	else if (start == end && single_bit_flip(*start, PAGE_POISON))
		pr_err("pagealloc: single bit error\n");
	else
		pr_err("pagealloc: memory corruption\n");

	/* 最后给出异常字节、当前调用链及对应 page flags/refcount 以便定位写坏者。 */
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 16, 1, start,
			end - start + 1, 1);
	dump_stack();
	dump_page(page, "pagealloc: corrupted page details");
}

/*
 * unpoison_page() - 在单页重新分配前验证释放期 poison 图样。
 * 业务背景：伙伴分配准备路径调用；它建立局部映射并交给 check_poison_mem()
 * 诊断释放后写入，然后恢复 KASAN 状态和映射上下文。
 * 入参：@page 是即将归新分配者的借用单页，调用者保证独占。
 * 出参/返回：无直接返回值；可能输出诊断，但不改写 poison、不阻止分配。
 * 注意事项：临时关闭当前 KASAN 以读取其仍认为在用/特殊状态的页；必须严格
 * 与 enable 和 kunmap 配对，且 local map 指针不得逸出。
 */
static void unpoison_page(struct page *page)
{
	/* addr 是仅供本函数检查的 PAGE_SIZE 局部映射。 */
	void *addr;

	addr = kmap_local_page(page);
	kasan_disable_current();
	/*
	 * Page poisoning when enabled poisons each and every page
	 * that is freed to buddy. Thus no extra check is done to
	 * see if a page was poisoned.
	 */
	/*
	 * 启用 page poisoning 后，每一页归还 buddy 时都会写 poison，因此这里不再
	 * 另查“该页是否曾 poison”的状态位，而是无条件验证整页；遗漏检查会让 UAF
	 * 写入失去诊断机会。
	 */
	check_poison_mem(page, kasan_reset_tag(addr), PAGE_SIZE);
	kasan_enable_current();
	kunmap_local(addr);
}

/*
 * __kernel_unpoison_pages() - 逐页验证一个即将分配的连续页块。
 * 业务背景：kernel_unpoison_pages() 的 static-key 包装在伙伴页交给分配者前调用。
 * 入参：@page 为首 page；@n 为连续页数，二者均由分配路径借用。
 * 出参/返回：无直接返回值；逐页可能报告损坏，不取得引用、不转移 ownership。
 * 注意事项：调用者保证页块独占；诊断不构成失败返回，循环会继续检查其余页。
 */
void __kernel_unpoison_pages(struct page *page, int n)
{
	/* i 是高阶块内的 page 偏移。 */
	int i;

	for (i = 0; i < n; i++)
		unpoison_page(page + i);
}

#ifndef CONFIG_ARCH_SUPPORTS_DEBUG_PAGEALLOC
/*
 * __kernel_map_pages() - 为无原生 DEBUG_PAGEALLOC 的架构提供兼容空桩。
 * 业务背景：这些架构用 page poisoning 检测释放后访问，不能真正撤销 direct-map
 * PTE；通用 debug pagealloc 调用仍可保持相同接口。
 * 入参：@page 为借用首 page；@numpages 为页数；@enable 表示映射或解映射请求，
 * 三者均故意不使用。
 * 出参/返回：无直接返回值，无映射或 ownership 副作用。
 * 注意事项：仅在 !CONFIG_ARCH_SUPPORTS_DEBUG_PAGEALLOC 编译；检测能力来自 poison。
 */
void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	/* This function does nothing, all work is done via poison pages */
	/* 本函数不执行任何操作，所有调试工作都由页 poison 写入与重新分配检查完成。 */
}
#endif
