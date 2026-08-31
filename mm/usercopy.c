// SPDX-License-Identifier: GPL-2.0-only
/*
 * This implements the various checks for CONFIG_HARDENED_USERCOPY*,
 * which are designed to protect kernel memory from needless exposure
 * and overwrite under many unintended conditions. This code is based
 * on PAX_USERCOPY, which is:
 *
 * Copyright (C) 2001-2016 PaX Team, Bradley Spengler, Open Source
 * Security Inc.
 */
/*
 * 上述 hardened usercopy 检查位于 copy_to_user()/copy_from_user() 真正复制前，
 * 目标是阻断意外的内核内存暴露或用户数据越界覆盖。设计源自 PaX USERCOPY；
 * 它按栈、slab/vmalloc/page allocation 和内核 text 分类验证范围，失败属于内核
 * 安全不变量破坏而不是普通 -EFAULT。
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/kstrtox.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
/* 后半依赖提供 uaccess 尺寸入口、vmalloc 分类、static key 与架构 text 边界。 */
#include <linux/ucopysize.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>
#include <linux/jump_label.h>
#include <asm/sections.h>
#include "slab.h"

/*
 * Checks if a given pointer and length is contained by the current
 * stack frame (if possible).
 *
 * Returns:
 *	NOT_STACK: not at all on the stack
 *	GOOD_FRAME: fully within a valid stack frame
 *	GOOD_STACK: within the current stack (when can't frame-check exactly)
 *	BAD_STACK: error condition (invalid stack position or bad stack frame)
 */
/*
 * 原文返回分类：完全不接触 current 内核栈为 NOT_STACK；架构能证明对象完整落在
 * 合法栈帧内为 GOOD_FRAME；无法精确验帧但落在当前活动栈范围为 GOOD_STACK；
 * 部分越界、跨非法帧或落在当前 SP 之外的失效区域为 BAD_STACK。
 *
 * 业务背景：__check_object_size() 先用本 helper 判断 uaccess 缓冲区是否是当前
 * syscall/内核路径仍然存活的栈对象，防止复制 saved state 或过期栈区。
 * 入参：obj 是借用的内核地址；len 是非零字节数，调用者已排除地址加法回绕。
 * 出参/返回：返回上述四类枚举之一；不修改栈、不取得 task/对象引用。
 * 注意事项：noinline 保留可供体系结构 frame walker 观察的独立调用帧；只检查
 * current 的 task stack。架构无精确 frame 支持时退化为 SP 深度/整栈范围检查。
 */
static noinline int check_stack_object(const void *obj, unsigned long len)
{
	const void * const stack = task_stack_page(current);
	const void * const stackend = stack + THREAD_SIZE;
	int ret;

	/* Object is not on the stack at all. */
	/* 半开区间完全位于栈下方或上方时交给 heap/text 分类继续判断。 */
	if (obj + len <= stack || stackend <= obj)
		return NOT_STACK;

	/*
	 * Reject: object partially overlaps the stack (passing the
	 * check above means at least one end is within the stack,
	 * so if this check fails, the other end is outside the stack).
	 */
	/*
	 * 上一检查已证明至少一端接触栈；若另一端在边界外，就是部分重叠，不能把
	 * 栈内片段作为整个 usercopy 对象放行。
	 */
	if (obj < stack || stackend < obj + len)
		return BAD_STACK;

	/* Check if object is safely within a valid frame. */
	/* 架构可走帧链时会排除返回地址等元数据；非零结果已经是最终 GOOD/BAD 判定。 */
	ret = arch_within_stack_frames(stack, stackend, obj, len);
	if (ret)
		return ret;

	/* Finally, check stack depth if possible. */
	/*
	 * 无法精确验帧时至少按栈增长方向要求对象位于当前 SP 的活动一侧，避免复制
	 * 尚未分配或已经退栈的空间。
	 */
#ifdef CONFIG_ARCH_HAS_CURRENT_STACK_POINTER
	if (IS_ENABLED(CONFIG_STACK_GROWSUP)) {
		if ((void *)current_stack_pointer < obj + len)
			return BAD_STACK;
	} else {
		if (obj < (void *)current_stack_pointer)
			return BAD_STACK;
	}
#endif

	/* 只能证明位于 current 活动栈，不能证明属于单一局部变量，因此返回 GOOD_STACK。 */
	return GOOD_STACK;
}

/*
 * If these functions are reached, then CONFIG_HARDENED_USERCOPY has found
 * an unexpected state during a copy_from_user() or copy_to_user() call.
 * There are several checks being performed on the buffer by the
 * __check_object_size() function. Normal stack buffer usage should never
 * trip the checks, and kernel text addressing will always trip the check.
 * For cache objects, it is checking that only the whitelisted range of
 * bytes for a given cache is being accessed (via the cache's usersize and
 * useroffset fields). To adjust a cache whitelist, use the usercopy-aware
 * kmem_cache_create_usercopy() function to create the cache (and
 * carefully audit the whitelist range).
 */
/*
 * 原文说明到达这里代表 HARDENED_USERCOPY 在 copy_from_user()/copy_to_user()
 * 缓冲区中发现不可能的状态：正常栈对象不应失败、kernel text 必然失败；slab
 * 对象只允许访问 cache 的 useroffset/usersize 白名单。需要扩大白名单时必须用
 * kmem_cache_create_usercopy() 建 cache，并审计暴露范围，不能绕过检查。
 *
 * 业务背景：所有分类检查的统一致命出口，打印方向、对象类型、细节和越界范围。
 * 入参：name/detail 是借用的静态或调用期字符串，可为 NULL；to_user=true 表示
 * 内核向用户暴露，false 表示用户覆盖内核；offset/len 是诊断字节偏移和长度。
 * 出参/返回：__noreturn；打印 emergency 日志后 BUG/Oops，绝不把普通错误返回
 * uaccess 调用者，也不接管字符串 ownership。
 * 注意事项：这是内核内存安全断言，可能导致当前任务或系统终止；调用者不得
 * 持有只能由正常返回释放的资源并期待恢复。
 */
void __noreturn usercopy_abort(const char *name, const char *detail,
			       bool to_user, unsigned long offset,
			       unsigned long len)
{
	pr_emerg("Kernel memory %s attempt detected %s %s%s%s%s (offset %lu, size %lu)!\n",
		 to_user ? "exposure" : "overwrite",
		 to_user ? "from" : "to",
		 name ? : "unknown?!",
		 detail ? " '" : "", detail ? : "", detail ? "'" : "",
		 offset, len);

	/*
	 * For greater effect, it would be nice to do do_group_exit(),
	 * but BUG() actually hooks all the lock-breaking and per-arch
	 * Oops code, so that is used here instead.
	 */
	/*
	 * 原文设想 do_group_exit()，但 BUG() 会进入各架构 Oops 及破锁处理，能在锁
	 * 上下文中按内核故障路径收束，因此这里选择 BUG 而非普通进程退出。
	 */
	BUG();
}

/* Returns true if any portion of [ptr,ptr+n) over laps with [low,high). */
/*
 * 该英文注释定义两个半开区间：任意部分相交返回 true。
 * 业务背景：kernel text 主映射和 linear alias 检查复用统一的范围相交判定。
 * 入参：ptr/n 定义待复制字节区间；low/high 定义被保护区间，均为纯输入地址值。
 * 出参/返回：有交集返回 true，完全位于上方或下方返回 false；无副作用。
 * 注意事项：调用前必须由 check_bogus_address() 排除 ptr+n 的无符号回绕；
 * high 是排他边界，端点相接不算重叠。
 */
static bool overlaps(const unsigned long ptr, unsigned long n,
		     unsigned long low, unsigned long high)
{
	const unsigned long check_low = ptr;
	unsigned long check_high = check_low + n;

	/* Does not overlap if entirely above or entirely below. */
	/* [ptr,ptr+n) 从 high 开始，或结束于 low 及其下方，均没有共同字节。 */
	if (check_low >= high || check_high <= low)
		return false;

	return true;
}

/* Is this address range in the kernel text area? */
/*
 * 该英文问题对应硬拒绝规则：任何触及 kernel text 的 usercopy 都不合法。
 * 业务背景：在栈/heap 分类之后阻止向用户泄露指令字节，或由用户覆盖可执行
 * 内核映像；同时覆盖部分架构对同一物理 text 建立的 linear-map alias。
 * 入参：ptr/n 是已通过非零、非回绕检查的内核半开地址范围；to_user 表示方向。
 * 出参/返回：void；未重叠静默返回，命中主映射或 alias 时 usercopy_abort 不返回。
 * 注意事项：_stext/_etext 生命周期永久稳定；lm_alias 只做地址换算。函数不验证
 * 普通 module text（相关体系结构/上层机制另行负责），也不取得映射引用。
 */
static inline void check_kernel_text_object(const unsigned long ptr,
					    unsigned long n, bool to_user)
{
	unsigned long textlow = (unsigned long)_stext;
	unsigned long texthigh = (unsigned long)_etext;
	unsigned long textlow_linear, texthigh_linear;

	/* 先查链接器定义的规范 kernel text 虚拟区间。 */
	if (overlaps(ptr, n, textlow, texthigh))
		usercopy_abort("kernel text", NULL, to_user, ptr - textlow, n);

	/*
	 * Some architectures have virtual memory mappings with a secondary
	 * mapping of the kernel text, i.e. there is more than one virtual
	 * kernel address that points to the kernel image. It is usually
	 * when there is a separate linear physical memory mapping, in that
	 * __pa() is not just the reverse of __va(). This can be detected
	 * and checked:
	 */
	/*
	 * 原文说明某些架构同时存在 kernel image 映射和线性物理映射，且 __pa()/__va()
	 * 不再简单互逆；因此同一 text 物理页可能从第二虚址被绕过主区间检查。
	 */
	textlow_linear = (unsigned long)lm_alias(textlow);
	/* No different mapping: we're done. */
	/* alias 与规范地址相同即没有第二入口，无需重复范围判断。 */
	if (textlow_linear == textlow)
		return;

	/* Check the secondary mapping... */
	/* 对 text 终点做相同 alias 换算，拒绝触及整个 secondary 半开区间。 */
	texthigh_linear = (unsigned long)lm_alias(texthigh);
	if (overlaps(ptr, n, textlow_linear, texthigh_linear))
		usercopy_abort("linear kernel text", NULL, to_user,
			       ptr - textlow_linear, n);
}

static inline void check_bogus_address(const unsigned long ptr, unsigned long n,
				       bool to_user)
{
	/*
	 * 业务背景：在任何指针区间算术和分类前拒绝回绕、NULL 与 ZERO_SIZE_PTR，
	 * 避免后续边界检查被无符号溢出绕过。
	 * 入参：ptr 是待复制内核地址值；n 是由顶层保证非零的字节数；to_user 为方向。
	 * 出参/返回：void；合法时静默返回，非法时 usercopy_abort 不返回。
	 * 注意事项：n==0 会令 n-1 下溢，所以本 helper 只能由已执行零长度快返的
	 * __check_object_size() 调用；不解引用 ptr，也不取得对象引用。
	 */
	/* Reject if object wraps past end of memory. */
	/* 最后一个有效字节 ptr+n-1 若落回 ptr 下方，说明半开区间越过 ULONG_MAX。 */
	if (ptr + (n - 1) < ptr)
		usercopy_abort("wrapped address", NULL, to_user, 0, ptr + n);

	/* Reject if NULL or ZERO-allocation. */
	/* kmalloc(0) 的 ZERO_SIZE_PTR 也不可作为非零 usercopy 缓冲区。 */
	if (ZERO_OR_NULL_PTR(ptr))
		usercopy_abort("null address", NULL, to_user, ptr, n);
}

/*
 * 业务背景：把非栈 usercopy 地址按 kmap、vmalloc、slab 或 compound page 分类，
 * 并验证请求没有越过实际分配/白名单边界。
 * 入参：ptr 是借用的内核缓冲区；n 是已验证非零且不回绕的字节数；to_user
 * 指示暴露或覆盖方向，仅用于失败诊断。
 * 出参/返回：void；可证明合法或当前类别无法进一步验证时返回，越界则 BUG。
 * 注意事项：函数只做边界验证、不 pin page；分类结果依赖调用时映射仍有效。
 * vmalloc 查找在 pagefault_disabled() 时跳过，以免不安全上下文进入 vmap 查找。
 */
static inline void check_heap_object(const void *ptr, unsigned long n,
				     bool to_user)
{
	unsigned long addr = (unsigned long)ptr;
	unsigned long offset;
	struct page *page;
	struct slab *slab;

	/* 临时 kmap 只允许访问当前映射页内剩余字节，不能跨到下一槽位。 */
	if (is_kmap_addr(ptr)) {
		offset = offset_in_page(ptr);
		if (n > PAGE_SIZE - offset)
			usercopy_abort("kmap", NULL, to_user, offset, n);
		return;
	}

	/* 可睡眠/可缺页上下文中，用 vmap_area 的排他终点验证整个 vmalloc 区间。 */
	if (is_vmalloc_addr(ptr) && !pagefault_disabled()) {
		struct vmap_area *area = find_vmap_area(addr);

		if (!area)
			/* 地址声称属于 vmalloc 空间却查不到 area，说明对象已失效或分类异常。 */
			usercopy_abort("vmalloc", "no area", to_user, 0, n);

		if (n > area->va_end - addr) {
			offset = addr - area->va_start;
			usercopy_abort("vmalloc", NULL, to_user, offset, n);
		}
		return;
	}

	/* 非线性映射地址无法安全 virt_to_page；留给最后的 kernel text 检查。 */
	if (!virt_addr_valid(ptr))
		return;

	page = virt_to_page(ptr);
	slab = page_slab(page);
	if (slab) {
		/* Check slab allocator for flags and size. */
		/* SLUB 进一步核对对象边界、redzone 以及 cache useroffset/usersize 白名单。 */
		__check_heap_object(ptr, n, slab, to_user);
	} else if (PageCompound(page)) {
		/* compound allocation 可由 head 与 order 恢复总大小，故能验证跨 base page。 */
		page = compound_head(page);
		offset = ptr - page_address(page);
		if (n > page_size(page) - offset)
			usercopy_abort("page alloc", NULL, to_user, offset, n);
	}

	/*
	 * We cannot check non-compound pages.  They might be part of
	 * a large allocation, in which case crossing a page boundary
	 * is fine.
	 */
	/*
	 * 原文说明普通非 compound page 可能只是高阶分配中尚无 compound 标记的一页；
	 * 因而无法知道真实 allocation 边界，跨页可能合法，只能保守放行而非误报。
	 */
}

/*
 * validate_usercopy_range 是 uaccess 热路径的只读 static key：编译期默认值来自
 * HARDENED_USERCOPY_DEFAULT_ON，late init 再按启动参数 patch 分支。导出符号让
 * 内联 check_object_size() 的模块调用点共享同一开关；关闭只跳过运行期范围
 * 检查，不改变编译期 object-size 诊断。
 */
DEFINE_STATIC_KEY_MAYBE_RO(CONFIG_HARDENED_USERCOPY_DEFAULT_ON,
			   validate_usercopy_range);
/* 模块只读取/跳转该 static key，不拥有或直接修改它。 */
EXPORT_SYMBOL(validate_usercopy_range);

/*
 * Validates that the given object is:
 * - not bogus address
 * - fully contained by stack (or stack frame, when available)
 * - fully within SLAB object (or object whitelist area, when available)
 * - not in kernel text
 */
/*
 * 原文列出完整流水线：地址不得伪造/回绕；若在栈上必须完整落入合法 frame 或
 * 活动栈；若可归类为 SLAB 必须落在对象或 usercopy 白名单；最终不得触及 text。
 *
 * 业务背景：check_object_size() 的 static-key 慢路径，在真正 uaccess 前对动态
 * 长度缓冲区执行运行期 hardening。
 * 入参：ptr 是同步借用的内核源/目标地址；n 为字节数；to_user=true 表示 ptr
 * 是 copy_to_user 的内核源，false 表示 copy_from_user 的内核目标。
 * 出参/返回：void；零长度或所有检查通过时返回，任何违规调用 BUG 不返回；
 * 不改变缓冲区、不取得引用。
 * 注意事项：调用者在复制期间必须保证对象生命周期；本检查不是 access_ok()，
 * 不验证用户地址，也不能消除“检查后对象被并发释放”的上层竞态。
 */
void __check_object_size(const void *ptr, unsigned long n, bool to_user)
{
	/* Skip all tests if size is zero. */
	/* 空区间不读取 ptr，NULL 与 ZERO_SIZE_PTR 在 n==0 时均合法。 */
	if (!n)
		return;

	/* Check for invalid addresses. */
	/* 先建立非回绕前提，后面的 stack/text/heap 半开区间算术才可信。 */
	check_bogus_address((const unsigned long)ptr, n, to_user);

	/* Check for bad stack object. */
	/* 栈对象一旦被证明安全即可返回；它不应再被当作 direct-map heap 分类。 */
	switch (check_stack_object(ptr, n)) {
	case NOT_STACK:
		/* Object is not touching the current process stack. */
		/* 完全不接触 current 栈，继续尝试 heap 与 kernel text 分类。 */
		break;
	case GOOD_FRAME:
	case GOOD_STACK:
		/*
		 * Object is either in the correct frame (when it
		 * is possible to check) or just generally on the
		 * process stack (when frame checking not available).
		 */
		/*
		 * 原文区分精确 frame 证明与退化整栈证明；两者都足以允许本次同步复制，
		 * 因而无需继续执行 heap/text 检查。
		 */
		return;
	default:
		/* BAD_STACK 计算相对当前 SP 的诊断偏移；栈增长方向决定减法次序。 */
		usercopy_abort("process stack", NULL, to_user,
#ifdef CONFIG_ARCH_HAS_CURRENT_STACK_POINTER
			IS_ENABLED(CONFIG_STACK_GROWSUP) ?
				ptr - (void *)current_stack_pointer :
				(void *)current_stack_pointer - ptr,
#else
			0,
#endif
			n);
	}

	/* Check for bad heap object. */
	/* 非栈对象按可识别 allocator 边界验证；无法分类时该 helper 会保守返回。 */
	check_heap_object(ptr, n, to_user);

	/* Check for object in kernel to avoid text exposure. */
	/* 最后独立拒绝 text；即使 heap 分类无法识别地址，也不能绕过可执行映像保护。 */
	check_kernel_text_object((const unsigned long)ptr, n, to_user);
}

/* 导出给 uaccess/uio 的内联慢路径；函数不构成用户态 ABI。 */
EXPORT_SYMBOL(__check_object_size);

/* 启动参数解析期间的临时策略，late init 应用后其 __initdata 内存可回收。 */
static bool enable_checks __initdata =
		IS_ENABLED(CONFIG_HARDENED_USERCOPY_DEFAULT_ON);

/*
 * 业务背景：解析 early kernel command line 的 hardened_usercopy=on/off，允许在
 * 不重编内核的情况下覆盖 Kconfig 默认值。
 * 入参：str 是 setup 框架借出的 NUL 结尾参数值，不得为长期保存用途。
 * 出参/返回：总返回 1 表示参数已被识别/消费；合法布尔值更新 enable_checks，
 * 非法值只告警并保持此前默认值。
 * 注意事项：仅启动单线程阶段调用，不能在运行期切换 static key；无 ownership 转移。
 */
static int __init parse_hardened_usercopy(char *str)
{
	if (kstrtobool(str, &enable_checks))
		pr_warn("Invalid option string for hardened_usercopy: '%s'\n",
			str);
	return 1;
}

/* 注册精确的 hardened_usercopy= 启动参数前缀及上述消费回调。 */
__setup("hardened_usercopy=", parse_hardened_usercopy);

/*
 * 业务背景：late init 时把最终启动策略提交到 validate_usercopy_range static key，
 * 之后 uaccess 热路径只付出已 patch 分支的成本。
 * 入参：无；读取初始化期 enable_checks。
 * 出参/返回：启用或禁用 static branch 后返回 1；无输出参数或对象 ownership。
 * 注意事项：只执行一次，static_branch_enable/disable 可能修改内核 text，必须在
 * 初始化上下文调用；提交后 enable_checks 所在 __initdata 可回收。
 */
static int __init set_hardened_usercopy(void)
{
	if (enable_checks)
		static_branch_enable(&validate_usercopy_range);
	else
		static_branch_disable(&validate_usercopy_range);
	return 1;
}

/* 晚于命令行解析应用策略，保证所有 hardened_usercopy= 覆盖已确定。 */
late_initcall(set_hardened_usercopy);
