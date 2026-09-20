// SPDX-License-Identifier: GPL-2.0-only
/* 通用 MM 实现依赖分配、VMA、调度和架构接口；本文件不拥有具体页或 VMA 的长期引用。 */
#include <linux/mm.h>
/* slab/string/compiler 负责对象分配、长度计算和编译期访问约束。 */
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
/* security 和 swap 接口连接映射准入与特殊 folio 的归属识别。 */
#include <linux/security.h>
#include <linux/swap.h>
#include <linux/swapops.h>
/* sysctl、mman、hugetlb 和 vmalloc 共同支撑承诺策略、地址布局与不同分配后端。 */
#include <linux/sysctl.h>
#include <linux/mman.h>
/* hugetlb/vmalloc 分别影响承诺统计和大对象的虚拟连续分配。 */
#include <linux/hugetlb.h>
#include <linux/vmalloc.h>
#include <linux/userfaultfd_k.h>
#include <linux/elf.h>
#include <linux/elf-randomize.h>
#include <linux/personality.h>
/* random、processor、sizes、compat 与 fsnotify 支撑地址随机化、布局选择和映射审计。 */
#include <linux/random.h>
#include <linux/processor.h>
#include <linux/sizes.h>
#include <linux/compat.h>
#include <linux/fsnotify.h>
#include <linux/page_idle.h>
/* 以下空行把内核通用头和显式用户访问接口分隔，避免普通代码误用 uaccess 原语。 */
/* uaccess 只在用户复制 helper 的受控窗口使用，调用者传入的用户地址不能直接解引用。 */

#include <linux/uaccess.h>

#include <kunit/visibility.h>

#include "internal.h"
#include "swap.h"

/* 本文件汇集独立的 MM 工具：复制/地址布局/记账/VMA 兼容层，调用者仍负责各自锁与引用。 */
/**
 * kfree_const - conditionally free memory
 * @x: pointer to the memory
 *
 * Function calls kfree only if @x is not in .rodata section.
 */
/*
 * kfree_const() - 释放可能来自只读常量区或动态分配器的字符串。
 * 业务背景：与 kstrdup_const() 的“常量借用/动态复制”双态返回配对。
 * 入参：x 可为 NULL、rodata 借用指针或可 kfree 对象。
 * 返回/副作用：无返回；只对非 rodata 指针调用 kfree。
 * 注意事项：不能用于其它静态区或要求专用释放器的对象。
 */
void kfree_const(const void *x)
{
	/* .rodata 指针不是分配器所有；仅动态对象才能归还给 kfree。 */
	if (!is_kernel_rodata((unsigned long)x))
		kfree(x);
}
EXPORT_SYMBOL(kfree_const);

/**
 * __kmemdup_nul - Create a NUL-terminated string from @s, which might be unterminated.
 * @s: The data to copy
 * @len: The size of the data, not including the NUL terminator
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 *
 * Return: newly allocated copy of @s with NUL-termination or %NULL in
 * case of error
 */
/*
 * __kmemdup_nul() - 复制精确 len 字节并追加内核控制的 NUL。
 * 业务背景：为多个内核字符串复制 API 统一分配、终止和失败语义。
 * 入参：s 至少 len 字节可读；gfp 决定分配上下文。
 * 返回/副作用：成功返回需 kfree 的新串，分配失败 NULL；不修改源数据。
 * 注意事项：调用者负责 len+1 不溢出，本函数不扫描源串。
 */
static __always_inline char *__kmemdup_nul(const char *s, size_t len, gfp_t gfp)
{
	/* 统一由此处分配 len+1，所有字符串包装器都获得独立且带终止符的所有权。 */
	char *buf;

	/* '+1' for the NUL terminator */
	buf = kmalloc_track_caller(len + 1, gfp);
	if (!buf)
		return NULL;

	/* 调用者承诺 s 至少有 len 字节可读；复制后显式写 NUL 不依赖源串格式。 */
	memcpy(buf, s, len);
	/* Ensure the buf is always NUL-terminated, regardless of @s. */
	buf[len] = '\0';
	return buf;
}

/**
 * kstrdup - allocate space for and copy an existing string
 * @s: the string to duplicate
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 *
 * Return: newly allocated copy of @s or %NULL in case of error
 */
/*
 * kstrdup() - 复制一个以 NUL 终止的内核字符串。
 * 业务背景：给需要独立可写生命周期的客户建立 kmalloc 副本。
 * 入参：s 可为 NULL，否则必须是可读终止串；gfp 控制分配。
 * 返回/副作用：NULL 输入/失败返回 NULL，成功返回需 kfree 的新串。
 * 注意事项：strlen 会读到终止符，不能用于只有长度边界的未终止缓冲。
 */
noinline
char *kstrdup(const char *s, gfp_t gfp)
{
	/* NULL 输入保持 NULL，非 NULL 的长度由 strlen 读取，返回对象由调用者 kfree。 */
	return s ? __kmemdup_nul(s, strlen(s), gfp) : NULL;
}
EXPORT_SYMBOL(kstrdup);

/**
 * kstrdup_const - conditionally duplicate an existing const string
 * @s: the string to duplicate
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 *
 * Note: Strings allocated by kstrdup_const should be freed by kfree_const and
 * must not be passed to krealloc().
 *
 * Return: source string if it is in .rodata section otherwise
 * fallback to kstrdup.
 */
/*
 * kstrdup_const() - 对 rodata 字符串借用原指针，否则创建动态副本。
 * 业务背景：避免复制永久常量，同时给调用者统一只读字符串接口。
 * 入参：s 为终止串借用指针；gfp 仅在需要复制时使用。
 * 返回/副作用：rodata 返回原指针，其它情况同 kstrdup；失败 NULL。
 * 注意事项：结果必须用 kfree_const()，不得传给 krealloc 或无条件 kfree。
 */
const char *kstrdup_const(const char *s, gfp_t gfp)
{
	/* 常量字符串借用原指针以避免分配；释放端必须配对 kfree_const。 */
	if (is_kernel_rodata((unsigned long)s))
		return s;

	return kstrdup(s, gfp);
}
EXPORT_SYMBOL(kstrdup_const);

/**
 * kstrndup - allocate space for and copy an existing string
 * @s: the string to duplicate
 * @max: read at most @max chars from @s
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 *
 * Note: Use kmemdup_nul() instead if the size is known exactly.
 *
 * Return: newly allocated copy of @s or %NULL in case of error
 */
/*
 * kstrndup() - 在最大读边界内复制内核字符串并保证终止。
 * 业务背景：源缓冲可能未在 max 内终止时，仍安全生成独立 C 字符串。
 * 入参：s 可为 NULL，否则至少在 max 范围内可读；gfp 控制分配。
 * 返回/副作用：成功返回需 kfree 的新串，NULL 输入/失败为 NULL。
 * 注意事项：已知精确字节长度时应使用 kmemdup_nul()，避免额外扫描。
 */
char *kstrndup(const char *s, size_t max, gfp_t gfp)
{
	/* strnlen 限制读边界，适用于来源可能无终止符但缓冲区长度已知的场景。 */
	return s ? __kmemdup_nul(s, strnlen(s, max), gfp) : NULL;
}
EXPORT_SYMBOL(kstrndup);

/**
 * kmemdup - duplicate region of memory
 *
 * @src: memory region to duplicate
 * @len: memory region length
 * @gfp: GFP mask to use
 *
 * Return: newly allocated copy of @src or %NULL in case of error,
 * result is physically contiguous. Use kfree() to free.
 */
/*
 * kmemdup_noprof() - 建立一段内核内存的物理连续副本。
 * 业务背景：把借用数据转换为调用者独占、可独立释放的 kmalloc 对象。
 * 入参：src 至少 len 字节可读；gfp 决定分配/睡眠策略。
 * 返回/副作用：成功返回需 kfree 的副本，失败 NULL；源区不变。
 * 注意事项：调用者必须保证长度与源边界合法，函数不处理重叠或用户地址。
 */
void *kmemdup_noprof(const void *src, size_t len, gfp_t gfp)
{
	/* kmalloc 返回物理连续对象；失败保持 NULL，不能对 NULL 进行 memcpy。 */
	void *p;

	p = kmalloc_node_track_caller_noprof(len, gfp, NUMA_NO_NODE, _RET_IP_);
	if (p)
		/* 所有权从 src 的借用数据变为 p 的私有副本。 */
		memcpy(p, src, len);
	return p;
}
EXPORT_SYMBOL(kmemdup_noprof);

/**
 * kmemdup_array - duplicate a given array.
 *
 * @src: array to duplicate.
 * @count: number of elements to duplicate from array.
 * @element_size: size of each element of array.
 * @gfp: GFP mask to use.
 *
 * Return: duplicated array of @src or %NULL in case of error,
 * result is physically contiguous. Use kfree() to free.
 */
/*
 * kmemdup_array() - 带元素数量乘法溢出检查地复制数组。
 * 业务背景：避免 count*element_size 回绕后小分配大复制。
 * 入参：src 覆盖乘积字节；count/element_size 描述数组；gfp 控制分配。
 * 返回/副作用：成功返回需 kfree 的物理连续副本，溢出/失败 NULL。
 * 注意事项：不验证元素布局；零乘积沿用 kmemdup/kmalloc 的零长度语义。
 */
void *kmemdup_array(const void *src, size_t count, size_t element_size, gfp_t gfp)
{
	/* size_mul 处理乘法溢出，避免 count*element_size 回绕成小分配。 */
	return kmemdup(src, size_mul(element_size, count), gfp);
}
EXPORT_SYMBOL(kmemdup_array);

/**
 * kvmemdup - duplicate region of memory
 *
 * @src: memory region to duplicate
 * @len: memory region length
 * @gfp: GFP mask to use
 *
 * Return: newly allocated copy of @src or %NULL in case of error,
 * result may be not physically contiguous. Use kvfree() to free.
 */
/*
 * kvmemdup() - 以 kvmalloc 后端复制可能较大的内核内存区。
 * 业务背景：kmalloc 无法满足大物理连续对象时允许回退 vmalloc。
 * 入参：src 至少 len 字节可读；gfp 控制可用分配策略。
 * 返回/副作用：成功返回虚拟连续副本，失败 NULL；结果必须 kvfree。
 * 注意事项：不得假定物理连续，也不能把结果交给只接受 kmalloc 对象的 API。
 */
void *kvmemdup(const void *src, size_t len, gfp_t gfp)
{
	/* kvmalloc 可回退 vmalloc，结果必须由 kvfree 而非仅 kfree 释放。 */
	void *p;

	p = kvmalloc(len, gfp);
	if (p)
		memcpy(p, src, len);
	return p;
}
EXPORT_SYMBOL(kvmemdup);

/**
 * kmemdup_nul - Create a NUL-terminated string from unterminated data
 * @s: The data to stringify
 * @len: The size of the data
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 *
 * Return: newly allocated copy of @s with NUL-termination or %NULL in
 * case of error
 */
/*
 * kmemdup_nul() - 复制精确内核字节区并追加 NUL。
 * 业务背景：把长度已知但未终止的数据安全转换为独立字符串。
 * 入参：s 可为 NULL，否则至少 len 字节可读；gfp 控制分配。
 * 返回/副作用：成功返回需 kfree 的新串，NULL 输入/失败为 NULL。
 * 注意事项：len 是精确数据长度而非上限；内部 len+1 溢出责任仍在调用者。
 */
char *kmemdup_nul(const char *s, size_t len, gfp_t gfp)
{
	/* 与 kstrndup 不同，len 是调用者确认的精确字节数，不扫描源数据。 */
	return s ? __kmemdup_nul(s, len, gfp) : NULL;
}
EXPORT_SYMBOL(kmemdup_nul);

/* 启动后只读的用户复制 bucket 集；成功 init 后由全局分配器持有至系统结束。 */
static kmem_buckets *user_buckets __ro_after_init;

/*
 * init_user_buckets() - 创建用户复制 helper 共用的专属 slab bucket 集。
 * 业务背景：把不可信用户输入副本隔离到可统计/加固的分配类别。
 * 入参：无；仅在 subsys_initcall 启动阶段执行。
 * 返回/副作用：始终 0，并把全寿命 bucket 指针发布到 user_buckets。
 * 注意事项：当前代码不传播创建失败；后续接口假定 bucket 可用。
 */
static int __init init_user_buckets(void)
{
	/* 启动期创建用户复制专用 bucket，使后续 allocation 具有独立统计归属。 */
	user_buckets = kmem_buckets_create("memdup_user", 0, 0, INT_MAX, NULL);

	return 0;
}
subsys_initcall(init_user_buckets);

/**
 * memdup_user - duplicate memory region from user space
 *
 * @src: source address in user space
 * @len: number of bytes to copy
 *
 * Return: an ERR_PTR() on failure.  Result is physically
 * contiguous, to be freed by kfree().
 */
/*
 * memdup_user() - 把用户字节区复制到物理连续内核缓冲。
 * 业务背景：在受控 uaccess 窗口稳定不可信输入，供内核后续使用。
 * 入参：src 是用户地址，len 是精确字节数；调用上下文须允许 GFP_USER/fault。
 * 返回/副作用：成功返回需 kfree 的副本；ENOMEM/EFAULT 以 ERR_PTR 返回并清理部分对象。
 * 注意事项：必须用 IS_ERR/PTR_ERR 判错，不能把返回约定当作 kmemdup 的 NULL 语义。
 */
void *memdup_user(const void __user *src, size_t len)
{
	/* 用户地址在 copy_from_user 期间可能 fault；错误路径必须释放刚取得的内核缓冲。 */
	void *p;

	p = kmem_buckets_alloc_track_caller(user_buckets, len, GFP_USER | __GFP_NOWARN);
	if (!p)
		/* 返回 ERR_PTR 使调用者能够区分内存不足与合法空长度对象。 */
		return ERR_PTR(-ENOMEM);

	if (copy_from_user(p, src, len)) {
		/* 残余字节表示用户访问失败，绝不能把部分初始化数据交给调用者。 */
		kfree(p);
		return ERR_PTR(-EFAULT);
	}

	return p;
}
EXPORT_SYMBOL(memdup_user);

/**
 * vmemdup_user - duplicate memory region from user space
 *
 * @src: source address in user space
 * @len: number of bytes to copy
 *
 * Return: an ERR_PTR() on failure.  Result may be not
 * physically contiguous.  Use kvfree() to free.
 */
/*
 * vmemdup_user() - 把用户字节区复制到可由 vmalloc 后端承载的内核缓冲。
 * 业务背景：允许较大用户输入不依赖物理连续分配成功。
 * 入参：src 是用户地址，len 是精确字节数；调用可睡眠并允许 fault。
 * 返回/副作用：成功返回需 kvfree 的虚拟连续副本；失败 ERR_PTR(-ENOMEM/-EFAULT)。
 * 注意事项：结果不保证物理连续，复制失败会释放部分初始化对象。
 */
void *vmemdup_user(const void __user *src, size_t len)
{
	/* 大用户输入可用非连续虚拟分配，释放协议随之改为 kvfree。 */
	void *p;

	p = kmem_buckets_valloc(user_buckets, len, GFP_USER);
	if (!p)
		return ERR_PTR(-ENOMEM);

	if (copy_from_user(p, src, len)) {
		/* 复制失败与物理连续版本语义一致，只是归还函数不同。 */
		kvfree(p);
		return ERR_PTR(-EFAULT);
	}

	return p;
}
EXPORT_SYMBOL(vmemdup_user);

/**
 * strndup_user - duplicate an existing string from user space
 * @s: The string to duplicate
 * @n: Maximum number of bytes to copy, including the trailing NUL.
 *
 * Return: newly allocated copy of @s or an ERR_PTR() in case of error
 */
/*
 * strndup_user() - 在上限内稳定复制用户 NUL 字符串。
 * 业务背景：先量长再复制，并抵抗用户在两步之间修改终止符。
 * 入参：s 为用户字符串地址；n 是包含尾 NUL 的最大可读字节数。
 * 返回/副作用：成功返回需 kfree 的内核串；访问失败/无终止/分配失败返回 ERR_PTR。
 * 注意事项：返回串保证 NUL，但用户可并发变化，所以内容只代表复制窗口快照。
 */
char *strndup_user(const char __user *s, long n)
{
	/* strnlen_user 返回值包含终止符，先验证用户可读和长度边界再进行复制。 */
	char *p;
	long length;

	length = strnlen_user(s, n);

	if (!length)
		/* 0 代表访问故障而不是空字符串。 */
		return ERR_PTR(-EFAULT);

	if (length > n)
		/* 没有在 n 内找到 NUL，拒绝把未界定的用户数据当字符串。 */
		return ERR_PTR(-EINVAL);

	p = memdup_user(s, length);

	if (IS_ERR(p))
		return p;

	p[length - 1] = '\0';
	/* 再次写终止符抵抗用户并发修改，返回串在内核端已稳定。 */

	return p;
}
EXPORT_SYMBOL(strndup_user);

/**
 * memdup_user_nul - duplicate memory region from user space and NUL-terminate
 *
 * @src: source address in user space
 * @len: number of bytes to copy
 *
 * Return: an ERR_PTR() on failure.
 */
/*
 * memdup_user_nul() - 复制用户字节块并在末尾追加内核 NUL。
 * 业务背景：让长度已知的用户 blob 可安全交给需要 C 字符串的解析器。
 * 入参：src 为用户地址，len 为不含新增 NUL 的精确长度。
 * 返回/副作用：成功返回需 kfree 的缓冲；ENOMEM/EFAULT 返回 ERR_PTR并清理。
 * 注意事项：调用者负责 len+1 不溢出；源数据内部可包含 NUL。
 */
void *memdup_user_nul(const void __user *src, size_t len)
{
	/* 为任意用户 byte blob 多保留一个字节，便于后续按 C 字符串消费。 */
	char *p;

	p = kmem_buckets_alloc_track_caller(user_buckets, len + 1, GFP_USER | __GFP_NOWARN);
	if (!p)
		return ERR_PTR(-ENOMEM);

	if (copy_from_user(p, src, len)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	p[len] = '\0';
	/* 该 NUL 永远来自内核，不受用户页在 copy 后变化影响。 */

	return p;
}
EXPORT_SYMBOL(memdup_user_nul);

/* Check if the vma is being used as a stack by this task */
/*
 * vma_is_stack_for_current() - 判断 VMA 是否覆盖 current 的瞬时用户栈指针。
 * 业务背景：调用者据此识别当前任务实际使用的栈映射，而非仅依赖 VMA 标志。
 * 入参：vma 为调用期间稳定的借用对象；current 隐式提供用户 SP。
 * 返回/副作用：覆盖则返回非零，否则 0；只读，无引用或状态变化。
 * 注意事项：栈指针可随执行变化，结果不是跨调度点稳定属性；边界按当前实现包含 vm_end。
 */
int vma_is_stack_for_current(const struct vm_area_struct *vma)
{
	/* KSTK_ESP 读取 current 的用户栈指针；比较仅判断该 VMA 是否覆盖此瞬时位置。 */
	struct task_struct * __maybe_unused t = current;

	return (vma->vm_start <= KSTK_ESP(t) && vma->vm_end >= KSTK_ESP(t));
}

/*
 * Change backing file, only valid to use during initial VMA setup.
 */
/*
 * vma_set_file() - 在 VMA 初始构造期替换 backing file 引用。
 * 业务背景：保证交换过程中 VMA 始终由一个有效 file 引用支撑。
 * 入参：vma 为未发布/受锁保护对象；file 非 NULL且为借用引用。
 * 返回/副作用：无返回；先 get 新 file、交换，再 fput 旧 file，VMA 接管新引用。
 * 注意事项：匿名 VMA 或已发布并发可见 VMA 禁止使用；传入同一 file 仍须配对引用。
 */
void vma_set_file(struct vm_area_struct *vma, struct file *file)
{
	/* 先增新 file 引用，再交换并 fput 旧对象，保证 VMA 始终持有一个有效 backing file。 */
	/* Changing an anonymous vma with this is illegal */
	get_file(file);
	swap(vma->vm_file, file);
	fput(file);
}
EXPORT_SYMBOL(vma_set_file);

#ifndef STACK_RND_MASK
/* 默认栈 ASLR 页数掩码：按 PAGE_SHIFT 缩放后对应约 8MB 虚拟地址窗口。 */
#define STACK_RND_MASK (0x7ff >> (PAGE_SHIFT - 12))     /* 8MB of VA */
#endif

/*
 * randomize_stack_top() - 按进程 ASLR 状态计算页对齐的栈顶候选。
 * 业务背景：exec 布局在架构栈窗口内扰动初始栈位置，降低地址可预测性。
 * 入参：stack_top 是未随机化上界；current flags 与栈增长方向决定策略。
 * 返回/副作用：返回页对齐并加/减随机偏移的地址；不建立 VMA、不修改 mm。
 * 注意事项：随机窗口由 STACK_RND_MASK 限制，真正映射仍需后续地址合法性检查。
 */
unsigned long randomize_stack_top(unsigned long stack_top)
{
	/* PF_RANDOMIZE 决定是否启用 ASLR；随机量按页对齐以保持后续栈映射边界合法。 */
	unsigned long random_variable = 0;

	if (current->flags & PF_RANDOMIZE) {
		/* 掩码限制扰动窗口，移位后把随机页数转换为虚拟地址偏移。 */
		/* get_random_long 的原始随机位先被限制到架构定义的 stack 窗口。 */
		random_variable = get_random_long();
		random_variable &= STACK_RND_MASK;
		/* 掩码后值仍是页数尺度，下一步左移前不会参与地址算术。 */
		random_variable <<= PAGE_SHIFT;
	}
#ifdef CONFIG_STACK_GROWSUP
	return PAGE_ALIGN(stack_top) + random_variable;
#else
	return PAGE_ALIGN(stack_top) - random_variable;
#endif
}

/**
 * randomize_page - Generate a random, page aligned address
 * @start:	The smallest acceptable address the caller will take.
 * @range:	The size of the area, starting at @start, within which the
 *		random address must fall.
 *
 * If @start + @range would overflow, @range is capped.
 *
 * NOTE: Historical use of randomize_range, which this replaces, presumed that
 * @start was already page aligned.  We now align it regardless.
 *
 * Return: A page aligned address within [start, start + range).  On error,
 * @start is returned.
 */
/*
 * randomize_page() - 在给定半开字节范围内选取页对齐随机地址。
 * 业务背景：为 brk/mmap 等布局提供统一的溢出与起点对齐处理。
 * 入参：start 是最小地址，range 是从原 start 起可用字节数。
 * 返回/副作用：有完整页时返回范围内随机页，否则返回对齐后的 start；纯计算。
 * 注意事项：起点上调会扣减 range；调用者仍须验证体系结构地址上限和实际空洞。
 */
unsigned long randomize_page(unsigned long start, unsigned long range)
{
	/* 先校正起点并扣除对齐消耗，保证返回值不会落到原请求范围外。 */
	if (!PAGE_ALIGNED(start)) {
		range -= PAGE_ALIGN(start) - start;
		start = PAGE_ALIGN(start);
	}

	if (start > ULONG_MAX - range)
		/* 饱和截断防止 start+range 无符号回绕到低地址。 */
		range = ULONG_MAX - start;

	range >>= PAGE_SHIFT;

	if (range == 0)
		/* 没有完整页可选时，唯一安全结果是对齐后的起点。 */
		return start;

	return start + (get_random_long() % range << PAGE_SHIFT);
}

/* 地址随机 helper 只计算候选基址；真正的 VMA 插入仍由 mmap 路径在锁内完成。 */

#ifdef CONFIG_ARCH_WANT_DEFAULT_TOPDOWN_MMAP_LAYOUT
/*
 * arch_randomize_brk() - 默认按任务位宽为 brk 选择随机地址。
 * 业务背景：给未提供架构覆盖的 topdown 布局实现独立 heap ASLR 窗口。
 * 入参：mm 为正在建立布局的借用地址空间，brk 已初始化。
 * 返回/副作用：返回 32MB或1GB窗口内候选，不修改 mm；weak 实现可被架构替换。
 * 注意事项：compat 任务始终使用较小窗口，最终可用性由 brk 路径验证。
 */
unsigned long __weak arch_randomize_brk(struct mm_struct *mm)
{
	/* 默认实现给 brk 独立随机窗口；体系结构可覆盖以匹配自身地址空间约束。 */
	/* Is the current task 32bit ? */
	if (!IS_ENABLED(CONFIG_64BIT) || is_compat_task())
		return randomize_page(mm->brk, SZ_32M);

	return randomize_page(mm->brk, SZ_1G);
}

/*
 * arch_mmap_rnd() - 生成默认 mmap 布局的页对齐随机偏移。
 * 业务背景：topdown/bottom-up 基址都复用架构配置的随机位宽。
 * 入参：无显式参数；current 的 compat 身份选择 native 或 compat bits。
 * 返回/副作用：返回以字节表示的随机页偏移，纯计算、不修改 mm。
 * 注意事项：配置位数必须小于机器字宽；安全性取决于后续布局保留足够空洞。
 */
unsigned long arch_mmap_rnd(void)
{
	/* 32 位 compat 与原生进程可有不同随机位数，最终统一转换为页单位。 */
	unsigned long rnd;

#ifdef CONFIG_HAVE_ARCH_MMAP_RND_COMPAT_BITS
	if (is_compat_task())
		rnd = get_random_long() & ((1UL << mmap_rnd_compat_bits) - 1);
	else
#endif /* CONFIG_HAVE_ARCH_MMAP_RND_COMPAT_BITS */
		rnd = get_random_long() & ((1UL << mmap_rnd_bits) - 1);

	return rnd << PAGE_SHIFT;
}

/* legacy 判断优先考虑 personality 与无限栈等 ABI 约束，sysctl 只是最终回退。 */

/*
 * mmap_is_legacy() - 判断 current 是否必须采用传统 bottom-up mmap 布局。
 * 业务背景：兼容 personality、无限向下栈和管理员 sysctl 可能要求旧 ABI 地址假设。
 * 入参：rlim_stack 为 exec 时稳定的栈限制借用值。
 * 返回/副作用：需要 legacy 返回非零，否则 0；只读 current/personality/sysctl。
 * 注意事项：向上增长栈不会因无限 rlimit 自动触发 legacy。
 */
static int mmap_is_legacy(const struct rlimit *rlim_stack)
{
	/* 返回值只选择搜索方向，不直接保留或释放任何虚拟地址范围。 */
	/* personality 明确请求兼容布局时优先，避免随机 topdown 改变旧程序的地址假设。 */
	if (current->personality & ADDR_COMPAT_LAYOUT)
		return 1;

	/* On parisc the stack always grows up - so a unlimited stack should
	 * not be an indicator to use the legacy memory layout. */
	if (rlim_stack->rlim_cur == RLIM_INFINITY &&
		!IS_ENABLED(CONFIG_STACK_GROWSUP))
		return 1;

	return sysctl_legacy_va_layout;
}

/*
 * Leave enough space between the mmap area and the stack to honour ulimit in
 * the face of randomisation.
 */
/* 最小/最大 gap 防止栈与 topdown mmap 过近，也防止栈保留吞掉几乎全部地址空间。 */
#define MIN_GAP		(SZ_128M)
#define MAX_GAP		(STACK_TOP / 6 * 5)

/*
 * mmap_base() - 计算默认 topdown 布局的搜索基址。
 * 业务背景：在栈、guard gap、ASLR 与 mmap 区之间保留互不碰撞的地址空间。
 * 入参：rnd 是页对齐随机偏移；rlim_stack 是 exec 时栈限制。
 * 返回/副作用：返回页对齐基址，纯计算；按栈增长方向使用不同公式。
 * 注意事项：gap 被夹在 MIN_GAP/MAX_GAP，溢出的 rlimit 加法不会采用 pad。
 */
static unsigned long mmap_base(const unsigned long rnd, const struct rlimit *rlim_stack)
{
	/* mmap_base 同时给最大栈、guard gap 和 ASLR 留空间，决定 topdown 搜索的起点。 */
#ifdef CONFIG_STACK_GROWSUP
	/*
	 * For an upwards growing stack the calculation is much simpler.
	 * Memory for the maximum stack size is reserved at the top of the
	 * task. mmap_base starts directly below the stack and grows
	 * downwards.
	 */
	return PAGE_ALIGN_DOWN(mmap_upper_limit(rlim_stack) - rnd);
#else
	unsigned long gap = rlim_stack->rlim_cur;
	unsigned long pad = stack_guard_gap;

	/* Account for stack randomization if necessary */
	if (current->flags & PF_RANDOMIZE)
		/* stack 随机化扩大保留区，防止 mmap 与随机后的栈碰撞。 */
		pad += (STACK_RND_MASK << PAGE_SHIFT);

	/* Values close to RLIM_INFINITY can overflow. */
	if (gap + pad > gap)
		/* 仅在加法未溢出时纳入 guard/random padding。 */
		gap += pad;

	if (gap < MIN_GAP && MIN_GAP < MAX_GAP)
		/* 小栈也须保留最小隔离窗口，避免 mmap 紧贴向下增长栈。 */
		gap = MIN_GAP;
	else if (gap > MAX_GAP)
		gap = MAX_GAP;

	return PAGE_ALIGN(STACK_TOP - gap - rnd);
#endif
}

/* layout 写入仅发生在 exec 建立地址空间时；运行中变更 rlimit 不会重排既有 VMA。 */

/* 不同架构分支最终都写入 mmap_base 与 MMF_TOPDOWN，供 unmapped-area 搜索读取。 */

/*
 * arch_pick_mmap_layout() - 为新 mm 发布默认 legacy 或 topdown mmap 布局。
 * 业务背景：exec 建立地址空间时统一选择搜索基址、随机量和 MMF_TOPDOWN 状态。
 * 入参：mm 为尚在构造/独占的地址空间；rlim_stack 是稳定栈限制。
 * 返回/副作用：无返回；写 mm->mmap_base 并同步设置或清除 TOPDOWN。
 * 注意事项：运行中 rlimit 变化不会重排已发布 VMA；仅默认架构分支编译。
 */
void arch_pick_mmap_layout(struct mm_struct *mm, const struct rlimit *rlim_stack)
{
	/* 写入 mm 的布局字段发生在地址空间建立期，之后 unmapped-area 搜索按此策略运行。 */
	unsigned long random_factor = 0UL;

	if (current->flags & PF_RANDOMIZE)
		random_factor = arch_mmap_rnd();

	if (mmap_is_legacy(rlim_stack)) {
		/* bottom-up 布局从 TASK_UNMAPPED_BASE 起找洞，并清除 topdown 状态位。 */
		mm->mmap_base = TASK_UNMAPPED_BASE + random_factor;
		mm_flags_clear(MMF_TOPDOWN, mm);
	} else {
		/* modern 布局从栈下方向下找洞，MMF_TOPDOWN 与 mmap_base 必须同步更新。 */
		mm->mmap_base = mmap_base(random_factor, rlim_stack);
		mm_flags_set(MMF_TOPDOWN, mm);
	}
}
#elif defined(CONFIG_MMU) && !defined(HAVE_ARCH_PICK_MMAP_LAYOUT)
/*
 * arch_pick_mmap_layout() - 无架构布局实现时发布固定 legacy 基址。
 * 业务背景：给 MMU 构建提供统一符号，同时明确不支持默认 topdown 随机布局。
 * 入参：mm 为构造期独占地址空间；rlim_stack 在此配置下不读取。
 * 返回/副作用：无返回；设置 TASK_UNMAPPED_BASE 并清 MMF_TOPDOWN。
 * 注意事项：这是条件编译替代实现，不应与架构自定义函数同时存在。
 */
void arch_pick_mmap_layout(struct mm_struct *mm, const struct rlimit *rlim_stack)
{
	/* 无默认 topdown 支持时固定传统基址，调用者无需判断配置分支。 */
	mm->mmap_base = TASK_UNMAPPED_BASE;
	mm_flags_clear(MMF_TOPDOWN, mm);
}
#endif
#ifdef CONFIG_MMU
EXPORT_SYMBOL_IF_KUNIT(arch_pick_mmap_layout);
#endif

/**
 * __account_locked_vm - account locked pages to an mm's locked_vm
 * @mm:          mm to account against
 * @pages:       number of pages to account
 * @inc:         %true if @pages should be considered positive, %false if not
 * @task:        task used to check RLIMIT_MEMLOCK
 * @bypass_rlim: %true if checking RLIMIT_MEMLOCK should be skipped
 *
 * Assumes @task and @mm are valid (i.e. at least one reference on each), and
 * that mmap_lock is held as writer.
 *
 * Return:
 * * 0       on success
 * * -ENOMEM if RLIMIT_MEMLOCK would be exceeded.
 */
/*
 * __account_locked_vm() - 在已持 mmap 写锁时原子更新 mm 的 locked_vm 记账。
 * 业务背景：锁页操作必须先受 RLIMIT_MEMLOCK 约束，失败不能留下部分计费。
 * 入参：mm/task 均持有效引用；pages 为页数；inc 选择增减；bypass_rlim 跳过限额检查。
 * 返回/副作用：成功 0 并更新计数；增账超限 -ENOMEM且不修改；减账下溢 WARN。
 * 注意事项：调用者必须持 mm mmap 写锁；bypass 只跳限制，不跳实际记账。
 */
int __account_locked_vm(struct mm_struct *mm, unsigned long pages, bool inc,
			const struct task_struct *task, bool bypass_rlim)
{
	/* mmap 写锁是 locked_vm 与 RLIMIT 判断的序列化前提，调用者必须已经持有。 */
	unsigned long locked_vm, limit;
	int ret = 0;

	mmap_assert_write_locked(mm);

	locked_vm = mm->locked_vm;
	if (inc) {
		/* 增加时先检查限制，失败不修改 mm->locked_vm 以保留原子记账语义。 */
		if (!bypass_rlim) {
			limit = task_rlimit(task, RLIMIT_MEMLOCK) >> PAGE_SHIFT;
			if (locked_vm + pages > limit)
				ret = -ENOMEM;
		}
		if (!ret)
			mm->locked_vm = locked_vm + pages;
	} else {
		/* 减账仍做下溢诊断；WARN 后按调用约定完成反向更新。 */
		WARN_ON_ONCE(pages > locked_vm);
		mm->locked_vm = locked_vm - pages;
	}

	pr_debug("%s: [%d] caller %ps %c%lu %lu/%lu%s\n", __func__, task->pid,
		 (void *)_RET_IP_, (inc) ? '+' : '-', pages << PAGE_SHIFT,
		 locked_vm << PAGE_SHIFT, task_rlimit(task, RLIMIT_MEMLOCK),
		 ret ? " - exceeded" : "");

	return ret;
}
EXPORT_SYMBOL_GPL(__account_locked_vm);

/* locked_vm 是地址空间级计数，公共包装器为无锁调用者补足 mmap 写锁。 */

/**
 * account_locked_vm - account locked pages to an mm's locked_vm
 * @mm:          mm to account against, may be NULL
 * @pages:       number of pages to account
 * @inc:         %true if @pages should be considered positive, %false if not
 *
 * Assumes a non-NULL @mm is valid (i.e. at least one reference on it).
 *
 * Return:
 * * 0       on success, or if mm is NULL
 * * -ENOMEM if RLIMIT_MEMLOCK would be exceeded.
 */
/*
 * account_locked_vm() - 为无锁调用者封装 locked_vm 记账和权限绕过判断。
 * 业务背景：公共客户无需自行管理 mmap 写锁即可按 current RLIMIT 更新锁页额度。
 * 入参：mm 可 NULL但非空时持引用；pages 为页数；inc 选择增减。
 * 返回/副作用：空/零页或成功为 0，超限 -ENOMEM；内部加写锁并更新 mm。
 * 注意事项：CAP_IPC_LOCK 只允许越过 rlimit，仍会改变 locked_vm。
 */
int account_locked_vm(struct mm_struct *mm, unsigned long pages, bool inc)
{
	/* 公共包装器处理空 mm/零页快路径，并自行取得写锁。 */
	int ret;

	if (pages == 0 || !mm)
		return 0;

	mmap_write_lock(mm);
	/* CAP_IPC_LOCK 调用者可绕过 rlimit，但记账本身仍必须发生。 */
	ret = __account_locked_vm(mm, pages, inc, current,
				  capable(CAP_IPC_LOCK));
	mmap_write_unlock(mm);

	return ret;
}
EXPORT_SYMBOL_GPL(account_locked_vm);

/* vm_mmap 系列把内核调用规范化为用户 mmap 等价事务，返回值是地址或负 errno。 */

/*
 * vm_mmap_pgoff() - 以内核调用模拟 current 的页偏移 mmap 事务。
 * 业务背景：统一执行 LSM、fsnotify、VMA 写锁、userfaultfd 收尾和可选预填充。
 * 入参：file 可空；addr/len/prot/flag/pgoff 与 mmap 语义一致，pgoff 以页计。
 * 返回/副作用：成功返回映射地址，失败返回编码负 errno；可能修改 current->mm。
 * 注意事项：可睡眠；锁外完成 userfaultfd 通知/人口填充，调用者须按地址返回约定判错。
 */
unsigned long vm_mmap_pgoff(struct file *file, unsigned long addr,
	unsigned long len, unsigned long prot,
	unsigned long flag, unsigned long pgoff)
{
	/* 此包装器模拟当前进程 mmap 系统调用：LSM/FS 权限、mmap 锁、userfaultfd 收尾均不可跳过。 */
	loff_t off = (loff_t)pgoff << PAGE_SHIFT;
	unsigned long ret;
	struct mm_struct *mm = current->mm;
	unsigned long populate;
	LIST_HEAD(uf);
	/* uf 收集 unmap 事件，必须在 do_mmap 后、返回用户前通知 userfaultfd。 */

	ret = security_mmap_file(file, prot, flag);
	/* 先经 LSM，再让 fsnotify 审核文件映射权限；任一拒绝都不进入 VMA 修改。 */
	if (!ret)
		ret = fsnotify_mmap_perm(file, prot, off, len);
	if (!ret) {
		/* do_mmap 修改 VMA 树需要写锁；可中断锁失败时保持地址空间未变。 */
		if (mmap_write_lock_killable(mm))
			return -EINTR;
		ret = do_mmap(file, addr, len, prot, flag, 0, pgoff, &populate,
			      &uf);
		mmap_write_unlock(mm);
		/* 解锁后完成 userfaultfd unmap 通知和可选预填充，避免在 mmap_lock 内做耗时工作。 */
		userfaultfd_unmap_complete(mm, &uf);
		if (populate)
			mm_populate(ret, populate);
	}
	return ret;
}

/*
 * Perform a userland memory mapping into the current process address space. See
 * the comment for do_mmap() for more details on this operation in general.
 *
 * This differs from do_mmap() in that:
 *
 * a. An offset parameter is provided rather than pgoff, which is both checked
 *    for overflow and page alignment.
 * b. mmap locking is performed on the caller's behalf.
 * c. Userfaultfd unmap events and memory population are handled.
 *
 * This means that this function performs essentially the same work as if
 * userland were invoking mmap (2).
 *
 * Returns either an error, or the address at which the requested mapping has
 * been performed.
 */
/*
 * vm_mmap() - 以字节 offset 发起 current 的完整内核 mmap 事务。
 * 业务背景：在 vm_mmap_pgoff() 前补齐 syscall 风格的 offset 对齐和溢出校验。
 * 入参：file/addr/len/prot/flag 同 mmap；offset 为字节且必须页对齐。
 * 返回/副作用：非法 offset 返回 -EINVAL，否则透传地址或 errno并可能修改 current->mm。
 * 注意事项：可睡眠；offset+PAGE_ALIGN(len) 不能回绕。
 */
unsigned long vm_mmap(struct file *file, unsigned long addr,
	unsigned long len, unsigned long prot,
	unsigned long flag, unsigned long offset)
{
	/* byte offset 转页偏移前验证加法与页对齐，拒绝无法由 do_mmap 正确表达的输入。 */
	if (unlikely(offset + PAGE_ALIGN(len) < offset))
		return -EINVAL;
	if (unlikely(offset_in_page(offset)))
		return -EINVAL;

	return vm_mmap_pgoff(file, addr, len, prot, flag, offset >> PAGE_SHIFT);
}
EXPORT_SYMBOL(vm_mmap);

/* shadow-stack 版本仅在支持的架构编译，普通架构不会暴露此映射策略。 */

#ifdef CONFIG_ARCH_HAS_USER_SHADOW_STACK
/*
 * Perform a userland memory mapping for a shadow stack into the current
 * process address space. This is intended to be used by architectures that
 * support user shadow stacks.
 */
/*
 * vm_mmap_shadow_stack() - 为 current 建立受架构保护的用户 shadow-stack VMA。
 * 业务背景：统一强制匿名私有、读写、不可替换和禁 THP 的 shadow-stack 映射策略。
 * 入参：addr 可为 0；len 为字节；flags 是调用者附加 mmap flags。
 * 返回/副作用：返回映射地址或 errno，并在 mmap 写锁下修改 current->mm。
 * 注意事项：仅支持配置下存在；给定 addr 时自动 MAP_FIXED_NOREPLACE。
 */
unsigned long vm_mmap_shadow_stack(unsigned long addr, unsigned long len,
		unsigned long flags)
{
	/* shadow stack 固定为私有匿名且读写映射；给定 addr 时禁止覆盖已有 VMA。 */
	struct mm_struct *mm = current->mm;
	unsigned long ret, unused;
	vm_flags_t vm_flags = VM_SHADOW_STACK;

	flags |= MAP_ANONYMOUS | MAP_PRIVATE;
	if (addr)
		flags |= MAP_FIXED_NOREPLACE;

	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE))
		/* shadow stack 的访问/保护语义不适合 THP，显式关闭该优化。 */
		vm_flags |= VM_NOHUGEPAGE;

	mmap_write_lock(mm);
	/* 直接调用 do_mmap 但仍用写锁包住 VMA 树更新。 */
	ret = do_mmap(NULL, addr, len, PROT_READ | PROT_WRITE, flags,
		      vm_flags, 0, &unused, NULL);
	mmap_write_unlock(mm);

	return ret;
}
#endif /* CONFIG_ARCH_HAS_USER_SHADOW_STACK */

/**
 * __vmalloc_array - allocate memory for a virtually contiguous array.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 */
/*
 * __vmalloc_array_noprof() - 溢出安全地分配虚拟连续数组。
 * 业务背景：防止元素乘积回绕导致小分配后越界使用。
 * 入参：n/size 描述数组，flags 传给底层 vmalloc。
 * 返回/副作用：成功返回需 vfree 的对象，溢出/失败 NULL。
 * 注意事项：不自动清零，除非 flags 自带 __GFP_ZERO；结果不保证物理连续。
 */
void *__vmalloc_array_noprof(size_t n, size_t size, gfp_t flags)
{
	/* vmalloc 数组先做溢出检查，字节数合法后才进入虚拟连续分配器。 */
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		/* 溢出返回 NULL，绝不能把截断长度传给分配器。 */
		return NULL;
	return __vmalloc_noprof(bytes, flags);
}
EXPORT_SYMBOL(__vmalloc_array_noprof);

/**
 * vmalloc_array - allocate memory for a virtually contiguous array.
 * @n: number of elements.
 * @size: element size.
 */
/*
 * vmalloc_array_noprof() - 以 GFP_KERNEL 分配虚拟连续数组。
 * 业务背景：为普通可睡眠客户提供固定策略的溢出安全包装。
 * 入参：n/size 描述数组。
 * 返回/副作用：成功返回需 vfree 的对象，失败 NULL；不清零。
 * 注意事项：只能在允许 GFP_KERNEL 睡眠的上下文调用。
 */
void *vmalloc_array_noprof(size_t n, size_t size)
{
	/* 常用包装器固定 GFP_KERNEL，适合可睡眠的普通内核调用路径。 */
	return __vmalloc_array_noprof(n, size, GFP_KERNEL);
}
EXPORT_SYMBOL(vmalloc_array_noprof);

/**
 * __vcalloc - allocate and zero memory for a virtually contiguous array.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 */
/*
 * __vcalloc_noprof() - 按调用者 flags 分配并清零虚拟连续数组。
 * 业务背景：复用数组溢出检查，同时以 __GFP_ZERO 建立零初始化契约。
 * 入参：n/size 描述数组，flags 控制分配上下文。
 * 返回/副作用：成功返回需 vfree 的全零对象，失败 NULL。
 * 注意事项：底层可能睡眠且物理不连续；调用者 flags 不能破坏 vmalloc 约束。
 */
void *__vcalloc_noprof(size_t n, size_t size, gfp_t flags)
{
	/* __GFP_ZERO 使所有字节初始为零，仍复用同一数组溢出检查。 */
	return __vmalloc_array_noprof(n, size, flags | __GFP_ZERO);
}
EXPORT_SYMBOL(__vcalloc_noprof);

/**
 * vcalloc - allocate and zero memory for a virtually contiguous array.
 * @n: number of elements.
 * @size: element size.
 */
/*
 * vcalloc_noprof() - 以 GFP_KERNEL 分配并清零虚拟连续数组。
 * 业务背景：为普通可睡眠调用者提供 calloc 风格的溢出安全接口。
 * 入参：n/size 描述数组。
 * 返回/副作用：成功返回需 vfree 的全零对象，失败 NULL。
 * 注意事项：结果只保证虚拟连续；零元素沿用 vmalloc 零长度语义。
 */
void *vcalloc_noprof(size_t n, size_t size)
{
	/* vcalloc 是可睡眠、虚拟连续且清零的便利接口。 */
	return __vmalloc_array_noprof(n, size, GFP_KERNEL | __GFP_ZERO);
}
EXPORT_SYMBOL(vcalloc_noprof);

/* 以下 folio helper 只转换/复制页面元数据或内容，不改变 LRU、refcount 或 mapping 所有权。 */

/*
 * folio_anon_vma() - 从 folio->mapping 的低位编码中提取匿名 VMA 根。
 * 业务背景：匿名 folio 用标志位复用 mapping 字段，普通 address_space 不能直接解释。
 * 入参：folio 为调用期间稳定的借用对象。
 * 返回/副作用：匿名映射返回 anon_vma 借用指针，其它类型 NULL；纯读取。
 * 注意事项：不取得 anon_vma 引用，调用者须用自身锁/RCU 保证返回对象生命周期。
 */
struct anon_vma *folio_anon_vma(const struct folio *folio)
{
	/* folio->mapping 低位编码类型；只有匿名标记才可还原为 anon_vma 指针。 */
	unsigned long mapping = (unsigned long)folio->mapping;

	if ((mapping & FOLIO_MAPPING_FLAGS) != FOLIO_MAPPING_ANON)
		return NULL;
	return (void *)(mapping - FOLIO_MAPPING_ANON);
}

/**
 * folio_mapping - Find the mapping where this folio is stored.
 * @folio: The folio.
 *
 * For folios which are in the page cache, return the mapping that this
 * page belongs to.  Folios in the swap cache return the swap mapping
 * this page is stored in (which is different from the mapping for the
 * swap file or swap device where the data is stored).
 *
 * You can call this for folios which aren't in the swap cache or page
 * cache and it will return NULL.
 */
/*
 * folio_mapping() - 解析 folio 当前所属的 page/swap cache address_space。
 * 业务背景：统一处理 swapcache 特例并屏蔽匿名、slab 和其它低位编码。
 * 入参：folio 为调用期间稳定的借用对象。
 * 返回/副作用：普通/swap cache 返回 mapping 借用指针，其它返回 NULL；无引用变化。
 * 注意事项：调用者须自行稳定 folio 状态和 mapping 生命周期，返回后类型可能并发变化。
 */
struct address_space *folio_mapping(const struct folio *folio)
{
	/* slab folio 没有 page-cache mapping；先排除避免把复用字段解释为地址空间。 */
	struct address_space *mapping;

	/* This happens if someone calls flush_dcache_page on slab page */
	if (unlikely(folio_test_slab(folio)))
		return NULL;

	if (unlikely(folio_test_swapcache(folio)))
		/* swap cache 使用独立 swap address_space，而非底层 swapfile 的 mapping。 */
		return swap_address_space(folio->swap);

	mapping = folio->mapping;
	/* 其余低位标记表示匿名/特殊映射，不能作为普通 address_space 解引用。 */
	if ((unsigned long)mapping & FOLIO_MAPPING_FLAGS)
		return NULL;

	return mapping;
}
EXPORT_SYMBOL(folio_mapping);

/**
 * folio_copy - Copy the contents of one folio to another.
 * @dst: Folio to copy to.
 * @src: Folio to copy from.
 *
 * The bytes in the folio represented by @src are copied to @dst.
 * Assumes the caller has validated that @dst is at least as large as @src.
 * Can be called in atomic context for order-0 folios, but if the folio is
 * larger, it may sleep.
 */
/*
 * folio_copy() - 把源 folio 的全部基础页内容复制到目标 folio。
 * 业务背景：迁移等路径需要跨高端页逐页复制并避免长时间独占 CPU。
 * 入参：dst/src 为稳定 folio；调用者保证 dst 至少与 src 一样大且不重叠。
 * 返回/副作用：无返回；覆盖目标对应字节，不改变 refcount/mapping/LRU。
 * 注意事项：order-0 可原子调用，高阶循环含 cond_resched 因而可能睡眠。
 */
void folio_copy(struct folio *dst, struct folio *src)
{
	/* 调用者保证目标容量；高阶 folio 分页复制并在迭代间让出 CPU。 */
	long i = 0;
	long nr = folio_nr_pages(src);

	for (;;) {
		/* 每次循环处理一个基础页，cond_resched 只出现在尚有后续页时。 */
		copy_highpage(folio_page(dst, i), folio_page(src, i));
		if (++i == nr)
			break;
		cond_resched();
	}
}
EXPORT_SYMBOL(folio_copy);

/*
 * folio_mc_copy() - 以 machine-check 感知原语复制 folio 内容。
 * 业务背景：内存故障恢复路径需在读到硬件坏页时停止并上报，而非传播损坏数据。
 * 入参：dst/src 为稳定 folio；目标容量不小于源。
 * 返回/副作用：全部复制返回 0；坏页返回 -EHWPOISON，目标此前页可能已部分写入。
 * 注意事项：高阶循环可调度；失败后调用者不得把 dst 当作完整副本发布。
 */
int folio_mc_copy(struct folio *dst, struct folio *src)
{
	/* 成功路径保证遍历 src 的全部基础页，调用者据返回值决定是否使用 dst。 */
	/* machine-check 感知副本在读坏页时返回 -EHWPOISON，调用者据此隔离故障数据。 */
	long nr = folio_nr_pages(src);
	long i = 0;

	for (;;) {
		if (copy_mc_highpage(folio_page(dst, i), folio_page(src, i)))
			/* 发生不可纠正读错误时立即停止，目标 folio 只保证此前页已写入。 */
			return -EHWPOISON;
		if (++i == nr)
			break;
		cond_resched();
	}

	return 0;
}

/* overcommit 计数只记录承诺量，实际匿名页的分配、回收与 swap out 由其它子系统完成。 */
EXPORT_SYMBOL(folio_mc_copy);

/* overcommit sysctl 在 read-mostly 存储中发布，频繁分配路径只读取这些策略值。 */

/* 当前虚拟承诺策略，默认启发式；分配热路径高频读取、sysctl 低频写入。 */
int sysctl_overcommit_memory __read_mostly = OVERCOMMIT_GUESS;
/* 比例模式下可承诺的非 hugetlb RAM 百分比。 */
static int sysctl_overcommit_ratio __read_mostly = 50;
/* 非零时覆盖 ratio 的绝对承诺上限，单位 kbytes。 */
static unsigned long sysctl_overcommit_kbytes __read_mostly;
/* 单个 mm 允许的 VMA 数上限，供映射路径独立读取。 */
int sysctl_max_map_count __read_mostly = DEFAULT_MAX_MAP_COUNT;
/* 严格模式为普通用户保留的恢复预算上限，单位 kbytes。 */
unsigned long sysctl_user_reserve_kbytes __read_mostly = 1UL << 17; /* 128MB */
/* 严格模式为管理员保留的全局恢复预算，单位 kbytes。 */
unsigned long sysctl_admin_reserve_kbytes __read_mostly = 1UL << 13; /* 8MB */

#ifdef CONFIG_SYSCTL

/*
 * overcommit_ratio_handler() - 处理 ratio sysctl 并维持 ratio/kbytes 互斥。
 * 业务背景：承诺上限只能由百分比或绝对值一种来源主导，避免配置含糊。
 * 入参：table/buffer/lenp/ppos 遵循 sysctl 协议；write 区分读写。
 * 返回/副作用：透传整数 handler 结果；成功写 ratio 时清 kbytes。
 * 注意事项：解析失败不改变互斥字段；并发发布依赖 sysctl 框架串行。
 */
static int overcommit_ratio_handler(const struct ctl_table *table, int write,
				void *buffer, size_t *lenp, loff_t *ppos)
{
	/* ratio 与 kbytes 是互斥输入；成功写 ratio 会清除显式 kbytes 配置。 */
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret == 0 && write)
		sysctl_overcommit_kbytes = 0;
	return ret;
}

/*
 * sync_overcommit_as() - 在目标 CPU 上把 committed_as 本地批量收敛到全局计数。
 * 业务背景：切入严格 NEVER 策略前必须降低松散 per-CPU 估算偏差。
 * 入参：dummy 为 schedule_on_each_cpu 工作参数且不读取。
 * 返回/副作用：无返回；同步当前 CPU 的 percpu counter 状态。
 * 注意事项：由可睡眠控制路径跨 CPU 调度，不在分配热路径直接调用。
 */
static void sync_overcommit_as(struct work_struct *dummy)
{
	/* 严格策略切换前汇总各 CPU 的近似计数，消除 per-cpu batch 偏差。 */
	percpu_counter_sync(&vm_committed_as);
}

/*
 * overcommit_policy_handler() - 按安全顺序读取或切换 overcommit 策略。
 * 业务背景：进入 NEVER 前先缩小 batch 并同步各 CPU，避免严格策略观察旧松散计数。
 * 入参：标准 sysctl table/write/buffer/lenp/ppos，值限于合法策略枚举。
 * 返回/副作用：成功 0并最终发布 policy，解析/同步前错误返回 errno；读操作只导出当前值。
 * 注意事项：写路径可跨 CPU 调度并睡眠；policy 必须最后写入。
 */
static int overcommit_policy_handler(const struct ctl_table *table, int write,
				void *buffer, size_t *lenp, loff_t *ppos)
{
	/* 先在临时表解析用户值，避免无效写入直接污染运行中的全局 policy。 */
	struct ctl_table t;
	int new_policy = -1;
	int ret;

	/*
	 * The deviation of sync_overcommit_as could be big with loose policy
	 * like OVERCOMMIT_ALWAYS/OVERCOMMIT_GUESS. When changing policy to
	 * strict OVERCOMMIT_NEVER, we need to reduce the deviation to comply
	 * with the strict "NEVER", and to avoid possible race condition (even
	 * though user usually won't too frequently do the switching to policy
	 * OVERCOMMIT_NEVER), the switch is done in the following order:
	 *	1. changing the batch
	 *	2. sync percpu count on each CPU
	 *	3. switch the policy
	 */
	if (write) {
		/* 先调整 batch、必要时同步，再发布 NEVER，保证新策略不看到旧的松散计数。 */
		t = *table;
		t.data = &new_policy;
		ret = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
		if (ret || new_policy == -1)
			return ret;

		mm_compute_batch(new_policy);
		/* NEVER 对精确上限敏感，跨 CPU 同步可能睡眠，不能在原子上下文调用。 */
		/* 策略变量最后写入，使读者不会先观察严格模式再观察旧 batch。 */
		if (new_policy == OVERCOMMIT_NEVER)
			/* 对所有 CPU 执行同步，确保 per-cpu 余量不会让 strict 上限短暂失真。 */
			schedule_on_each_cpu(sync_overcommit_as);
		sysctl_overcommit_memory = new_policy;
	} else {
		ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	}

	return ret;
}

/*
 * overcommit_kbytes_handler() - 处理绝对承诺上限并清除 ratio 模式。
 * 业务背景：与 ratio handler 共同保证 vm_commit_limit 只选择一种配置来源。
 * 入参：标准 sysctl 参数；值以 kbytes 表示。
 * 返回/副作用：透传 unsigned long handler；成功写时把 ratio 清零。
 * 注意事项：解析失败不改 ratio；零 kbytes 会使计算回退 ratio 当前值。
 */
static int overcommit_kbytes_handler(const struct ctl_table *table, int write,
				void *buffer, size_t *lenp, loff_t *ppos)
{
	/* 解析由通用 handler 完成，本层只维护 ratio/kbytes 的互斥状态机。 */
	/* 写入绝对 kbytes 后清 ratio，vm_commit_limit 只会选用其中一种来源。 */
	int ret;

	ret = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
	if (ret == 0 && write)
		sysctl_overcommit_ratio = 0;
	return ret;
}

/* sysctl 表字段只保存指针和 handler；读写权限由 procfs/sysctl 框架执行。 */

static const struct ctl_table util_sysctl_table[] = {
	/* policy 取值 0..2；handler 负责切换顺序而非普通整数写入。 */
	{
		.procname	= "overcommit_memory",
		.data		= &sysctl_overcommit_memory,
		.maxlen		= sizeof(sysctl_overcommit_memory),
		.mode		= 0644,
		.proc_handler	= overcommit_policy_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
	/* ratio 是 RAM 去除 hugetlb 后的百分比承诺限额。 */
	{
		.procname	= "overcommit_ratio",
		.data		= &sysctl_overcommit_ratio,
		.maxlen		= sizeof(sysctl_overcommit_ratio),
		.mode		= 0644,
		.proc_handler	= overcommit_ratio_handler,
	},
	/* kbytes 非零时覆盖 ratio，便于管理员指定固定承诺预算。 */
	{
		.procname	= "overcommit_kbytes",
		.data		= &sysctl_overcommit_kbytes,
		.maxlen		= sizeof(sysctl_overcommit_kbytes),
		.mode		= 0644,
		.proc_handler	= overcommit_kbytes_handler,
	},
	/* user reserve 限制单进程吞掉全部可承诺空间，保留其他用户的恢复余地。 */
	{
		.procname	= "user_reserve_kbytes",
		.data		= &sysctl_user_reserve_kbytes,
		.maxlen		= sizeof(sysctl_user_reserve_kbytes),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	/* admin reserve 只对非 CAP_SYS_ADMIN 扣减，保证管理会话可在紧张时运行。 */
	{
		.procname	= "admin_reserve_kbytes",
		.data		= &sysctl_admin_reserve_kbytes,
		.maxlen		= sizeof(sysctl_admin_reserve_kbytes),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
};

/*
 * init_vm_util_sysctls() - 在 vm sysctl 树注册 util.c 的承诺与保留策略。
 * 业务背景：向管理员发布 overcommit policy/limit 和用户、管理员恢复保留量。
 * 入参：无；subsys_initcall 阶段调用。
 * 返回/副作用：始终 0并注册静态表；表和后端变量随后全寿命有效。
 * 注意事项：当前代码不传播 register_sysctl_init 失败。
 */
static int __init init_vm_util_sysctls(void)
{
	/* subsys 初始化时挂入 vm sysctl 树；表和全局变量随后长期有效。 */
	register_sysctl_init("vm", util_sysctl_table);
	return 0;
}
subsys_initcall(init_vm_util_sysctls);
#endif /* CONFIG_SYSCTL */

/* sysctl 未配置时使用编译期初值；承诺计算和计数接口仍始终可用。 */

/*
 * Committed memory limit enforced when OVERCOMMIT_NEVER policy is used
 */
/*
 * vm_commit_limit() - 计算严格 overcommit 策略可承诺的总页数。
 * 业务背景：把管理员的绝对/比例 RAM 预算与 swap 合并为统一票据上限。
 * 入参：无；读取 read-mostly sysctl、RAM、hugetlb 和 swap 快照。
 * 返回/副作用：返回页单位上限，纯计算；kbytes 非零时优先于 ratio。
 * 注意事项：扣除 hugetlb 是因其不可供普通匿名承诺使用；结果可随热插拔/swap 变化。
 */
unsigned long vm_commit_limit(void)
{
	/* strict overcommit 的总票据数 = 配置 RAM 份额或固定 kbytes，再加全部 swap。 */
	unsigned long allowed;

	if (sysctl_overcommit_kbytes)
		/* kbytes 转成页单位，和 totalram_pages/total_swap_pages 保持同一量纲。 */
		allowed = sysctl_overcommit_kbytes >> (PAGE_SHIFT - 10);
	else
		/* hugetlb 页从普通承诺池扣除，因为它们不能按普通匿名页方式超售。 */
		allowed = ((totalram_pages() - hugetlb_total_pages())
			   * sysctl_overcommit_ratio / 100);
	allowed += total_swap_pages;

	return allowed;
}

/*
 * Make sure vm_committed_as in one cacheline and not cacheline shared with
 * other variables. It can be updated by several CPUs frequently.
 */
/* 高频承诺增减使用 per-cpu 批量计数降低全局锁竞争；严格模式切换会主动收敛它。 */
struct percpu_counter vm_committed_as ____cacheline_aligned_in_smp;

/*
 * The global memory commitment made in the system can be a metric
 * that can be used to drive ballooning decisions when Linux is hosted
 * as a guest. On Hyper-V, the host implements a policy engine for dynamically
 * balancing memory across competing virtual machines that are hosted.
 * Several metrics drive this policy engine including the guest reported
 * memory commitment.
 *
 * The time cost of this is very low for small platforms, and for big
 * platform like a 2S/36C/72T Skylake server, in worst case where
 * vm_committed_as's spinlock is under severe contention, the time cost
 * could be about 30~40 microseconds.
 */
/*
 * vm_memory_committed() - 汇总系统当前正的虚拟内存承诺页数。
 * 业务背景：虚拟机 balloon 等策略需要观察 guest 承诺压力而非仅实际驻留页。
 * 入参：无；读取高频 per-CPU counter。
 * 返回/副作用：返回非负近似总页数，不修改计数。
 * 注意事项：大机强竞争时汇总有微秒级成本，且结果不是事务一致快照。
 */
unsigned long vm_memory_committed(void)
{
	/* sum_positive 汇总 per-cpu 计数并屏蔽暂态负数，结果用于策略和监控而非锁保护。 */
	return percpu_counter_sum_positive(&vm_committed_as);
}
EXPORT_SYMBOL_GPL(vm_memory_committed);

/* 从此处开始的检查决定虚拟映射是否可承诺，不等同于立即分配物理页。 */

/*
 * Check that a process has enough memory to allocate a new virtual
 * mapping. 0 means there is enough memory for the allocation to
 * succeed and -ENOMEM implies there is not.
 *
 * We currently support three overcommit policies, which are set via the
 * vm.overcommit_memory sysctl.  See Documentation/mm/overcommit-accounting.rst
 *
 * Strict overcommit modes added 2002 Feb 26 by Alan Cox.
 * Additional code 2002 Jul 20 by Robert Love.
 *
 * cap_sys_admin is 1 if the process has admin privileges, 0 otherwise.
 *
 * Note this is a helper function intended to be used by LSMs which
 * wish to use this logic.
 */
/*
 * __vm_enough_memory() - 按当前 overcommit 策略批准或拒绝新增虚拟承诺。
 * 业务背景：LSM/映射路径先记账，再依据 ALWAYS/GUESS/NEVER 与恢复保留决定是否可继续。
 * 入参：mm 可 NULL；pages 为新增页数；cap_sys_admin 指示是否免扣管理员 reserve。
 * 返回/副作用：批准 0并保留记账；拒绝 -ENOMEM、限速日志并回滚同一 pages。
 * 注意事项：NEVER 的 committed 读取是受 batch 约束的近似值；本函数不实际分配物理页。
 */
int __vm_enough_memory(const struct mm_struct *mm, long pages, int cap_sys_admin)
{
	/* 先乐观记账；任何拒绝出口必须 vm_unacct_memory 回滚这次 pages。 */
	long allowed;
	unsigned long bytes_failed;

	vm_acct_memory(pages);

	/*
	 * Sometimes we want to use more memory than we have
	 */
	if (sysctl_overcommit_memory == OVERCOMMIT_ALWAYS)
		/* ALWAYS 不设上限，但承诺计数仍保留给统计和后续策略切换。 */
		return 0;

	if (sysctl_overcommit_memory == OVERCOMMIT_GUESS) {
		/* GUESS 仅拒绝单次请求已超过 RAM+swap 的明显不可能情况。 */
		if (pages > totalram_pages() + total_swap_pages)
			goto error;
		return 0;
	}

	allowed = vm_commit_limit();
	/* NEVER 从严格总额开始逐项扣除管理员和用户恢复保留。 */
	/*
	 * Reserve some for root
	 */
	if (!cap_sys_admin)
		/* 非管理员不得消耗留给紧急恢复工具的承诺空间。 */
		allowed -= sysctl_admin_reserve_kbytes >> (PAGE_SHIFT - 10);

	/*
	 * Don't let a single process grow so big a user can't recover
	 */
	if (mm) {
		/* 每个地址空间最多扣自身 total_vm 的 1/32，且不超过 sysctl 上限。 */
		long reserve = sysctl_user_reserve_kbytes >> (PAGE_SHIFT - 10);

		allowed -= min_t(long, mm->total_vm / 32, reserve);
	}

	if (percpu_counter_read_positive(&vm_committed_as) < allowed)
		/* 读是近似的但 strict 切换已同步；小偏差由 per-cpu batch 机制约束。 */
		return 0;
error:
	/* 失败日志限速，随后回滚初始记账，使下一次尝试看到一致的 committed_as。 */
	bytes_failed = pages << PAGE_SHIFT;
	pr_warn_ratelimited("%s: pid: %d, comm: %s, bytes: %lu not enough memory for the allocation\n",
			    __func__, current->pid, current->comm, bytes_failed);
	vm_unacct_memory(pages);

	return -ENOMEM;
}

/* 进程信息和调试 helper 读的是瞬时状态，调用者不能把输出当成跨调度点的稳定快照。 */

/**
 * get_cmdline() - copy the cmdline value to a buffer.
 * @task:     the task whose cmdline value to copy.
 * @buffer:   the buffer to copy to.
 * @buflen:   the length of the buffer. Larger cmdline values are truncated
 *            to this length.
 *
 * Return: the size of the cmdline field copied. Note that the copy does
 * not guarantee an ending NULL byte.
 */
/*
 * get_cmdline() - 从目标 task 的用户地址空间复制当前命令行快照。
 * 业务背景：/proc 等观察者需要在 exec/exit 并发下安全读取 argv，并兼容 setproctitle 覆盖尾 NUL。
 * 入参：task 为稳定借用；buffer 可写 buflen 字节；buflen 应非负。
 * 返回/副作用：返回实际复制/截断字节数，可能无尾 NUL；内部 get/mmput，不修改目标。
 * 注意事项：跨进程读取可部分成功；arg_lock 只稳定边界，不冻结用户页内容。
 */
int get_cmdline(struct task_struct *task, char *buffer, int buflen)
{
	/* 返回长度可小于请求长度，因为目标用户页、参数区或缓冲上限都可能截断复制。 */
	/* get_task_mm 固定地址空间；没有 mm 或 exec 尚未完成时返回空结果而非窥探半初始化参数。 */
	int res = 0;
	unsigned int len;
	struct mm_struct *mm = get_task_mm(task);
	unsigned long arg_start, arg_end, env_start, env_end;
	/* task 可以在读取间 exit，但 get_task_mm 保证 mm 对象存活到 mmput。 */
	if (!mm)
		goto out;
	if (!mm->arg_end)
		goto out_mm;	/* Shh! No looking before we're done */

	spin_lock(&mm->arg_lock);
	/* arg/env 四个边界必须作为同一快照读取，避免 setproctitle/exec 并发得到交叉范围。 */
	arg_start = mm->arg_start;
	arg_end = mm->arg_end;
	env_start = mm->env_start;
	env_end = mm->env_end;
	spin_unlock(&mm->arg_lock);

	len = arg_end - arg_start;
	/* 截断到调用者缓冲区，函数契约不保证写入结尾 NUL。 */

	if (len > buflen)
		len = buflen;

	res = access_process_vm(task, arg_start, buffer, len, FOLL_FORCE);
	/* 跨进程读取可能部分成功；res 是实际拷贝数量，后续只能在该范围内检查。 */

	/*
	 * If the nul at the end of args has been overwritten, then
	 * assume application is using setproctitle(3).
	 */
	if (res > 0 && buffer[res-1] != '\0' && len < buflen) {
		/* setproctitle 可能覆盖 argv 终止符，尝试把紧随的环境区拼入剩余空间。 */
		/* 首先检查已复制部分是否内含 NUL，避免无谓读取环境区。 */
		len = strnlen(buffer, res);
		if (len < res) {
			/* argv 区自身已有 NUL，结果应截到第一个 NUL 而非拼接环境。 */
			res = len;
		} else {
			len = env_end - env_start;
			/* 环境区长度也受调用者剩余缓冲限制，避免 res+len 溢出。 */
			if (len > buflen - res)
				len = buflen - res;
			res += access_process_vm(task, env_start,
						 buffer+res, len,
						 FOLL_FORCE);
			res = strnlen(buffer, res);
		}
	}
out_mm:
	/* 与 get_task_mm 配对，释放 mm 引用后不再访问其 argv/env 字段。 */
	mmput(mm);
out:
	return res;
}

/* 下面的 weak 比较实现建立临时 CPU 本地映射，不应在持有长期页表锁时做耗时调用。 */

/*
 * memcmp_pages() - 比较两个基础页的完整内容。
 * 业务背景：提供可被架构覆盖的高端页安全默认实现。
 * 入参：page1/page2 在调用期间稳定且可映射。
 * 返回/副作用：返回 memcmp 风格负/零/正值；只建立并拆除本地临时映射。
 * 注意事项：映射按 LIFO 释放，地址不得逸出；weak 实现可能被架构替换。
 */
int __weak memcmp_pages(struct page *page1, struct page *page2)
{
	/* 默认实现临时映射两页；架构可覆盖以利用专用比较或规避 cache/加密限制。 */
	char *addr1, *addr2;
	int ret;

	addr1 = kmap_local_page(page1);
	addr2 = kmap_local_page(page2);
	ret = memcmp(addr1, addr2, PAGE_SIZE);
	/* kmap_local 映射按 LIFO 拆除，禁止把 addr 指针带出本函数。 */
	kunmap_local(addr2);
	kunmap_local(addr1);
	return ret;
}

#ifdef CONFIG_PRINTK
/**
 * mem_dump_obj - Print available provenance information
 * @object: object for which to find provenance information.
 *
 * This function uses pr_cont(), so that the caller is expected to have
 * printed out whatever preamble is appropriate.  The provenance information
 * depends on the type of object and on how much debugging is enabled.
 * For example, for a slab-cache object, the slab name is printed, and,
 * if available, the return address and stack trace from the allocation
 * and last free path of that object.
 */
/*
 * mem_dump_obj() - 续写给定地址可获得的分配来源诊断。
 * 业务背景：WARN/debug 路径先让 slab/vmalloc 输出精确信息，再给未知地址做保守分类。
 * 入参：object 可为任意内核地址、NULL 或 ZERO_SIZE_PTR；不要求对象引用。
 * 返回/副作用：无返回，只用 pr_cont 输出；不会取得或释放对象。
 * 注意事项：调用者必须先打印前缀；分类是瞬时诊断，不能证明地址仍可安全解引用。
 */
void mem_dump_obj(void *object)
{
	/* pr_cont 要求调用者已持有合适的打印上下文，函数不单独添加日志前缀。 */
	/* 先让 slab/vmalloc 子系统输出更精确 provenance；只有都未知时才做通用分类。 */
	const char *type;

	if (kmem_dump_obj(object))
		return;

	if (vmalloc_dump_obj(object))
		return;

	/* 通用分类按地址性质排列，先识别 vmalloc 以避免落入普通虚拟地址分支。 */
	if (is_vmalloc_addr(object))
		/* 分类仅用于日志，不能据此取得对象引用或访问其内容。 */
		type = "vmalloc memory";
	else if (virt_addr_valid(object))
		/* 地址有效但非 slab/vmalloc 时只报告宽泛类别，避免推断分配来源。 */
		type = "non-slab/vmalloc memory";
	else if (object == NULL)
		type = "NULL pointer";
	else if (object == ZERO_SIZE_PTR)
		type = "zero-size pointer";
	else
		type = "non-paged memory";

	pr_cont(" %s\n", type);
}

/* 离线读写锁没有页面引用语义；使用者仍须自行确保 PFN/page 生命周期有效。 */
EXPORT_SYMBOL_GPL(mem_dump_obj);
#endif

/* PageOffline 协议与 D-cache helper 将访问者和热插拔/架构 cache 维护分开串行化。 */

/*
 * A driver might set a page logically offline -- PageOffline() -- and
 * turn the page inaccessible in the hypervisor; after that, access to page
 * content can be fatal.
 *
 * Some special PFN walkers -- i.e., /proc/kcore -- read content of random
 * pages after checking PageOffline(); however, these PFN walkers can race
 * with drivers that set PageOffline().
 *
 * page_offline_freeze()/page_offline_thaw() allows for a subsystem to
 * synchronize with such drivers, achieving that a page cannot be set
 * PageOffline() while frozen.
 *
 * page_offline_begin()/page_offline_end() is used by drivers that care about
 * such races when setting a page PageOffline().
 */
/* 读者冻结 PFN walker 与离线写者互斥；读锁可并行，PageOffline 状态转换需独占。 */
static DECLARE_RWSEM(page_offline_rwsem);

/*
 * page_offline_freeze() - 取得 PageOffline 协议的读侧冻结锁。
 * 业务背景：随机 PFN walker 在读页面内容期间阻止驱动把该页变为不可访问。
 * 入参：无；调用者将开始依赖页面内容可访问。
 * 返回/副作用：无返回；获取可睡眠 rwsem 读锁。
 * 注意事项：必须由同一控制流调用 page_offline_thaw()，且不能在原子上下文使用。
 */
void page_offline_freeze(void)
{
	/* walker 在访问页面内容前取得读锁，直到 thaw 前离线驱动无法开始转换。 */
	down_read(&page_offline_rwsem);
}

/*
 * page_offline_thaw() - 结束 PageOffline 读侧冻结区间。
 * 业务背景：允许等待中的离线写者在 walker 完成页面访问后继续状态转换。
 * 入参：无；当前控制流必须已成功 freeze。
 * 返回/副作用：无返回；释放 rwsem 读锁并可能唤醒写者。
 * 注意事项：未配对或重复调用会破坏 rwsem 状态。
 */
void page_offline_thaw(void)
{
	/* 必须与 freeze 成对，遗漏会永久阻塞热插拔/驱动离线流程。 */
	up_read(&page_offline_rwsem);
}

/*
 * page_offline_begin() - 开始驱动 PageOffline 状态转换的独占区间。
 * 业务背景：等待所有 PFN readers 离开，并阻止新 reader 在页面变不可访问时进入。
 * 入参：无；驱动尚未发布不可访问状态。
 * 返回/副作用：无返回；获取可睡眠 rwsem 写锁。
 * 注意事项：必须覆盖设置 PageOffline 和 hypervisor/硬件失效全过程，并以 end 配对。
 */
void page_offline_begin(void)
{
	/* 驱动写锁覆盖设置 PageOffline 与使页不可访问的整个临界区。 */
	down_write(&page_offline_rwsem);
}
EXPORT_SYMBOL(page_offline_begin);

/*
 * page_offline_end() - 发布离线转换完成并释放独占锁。
 * 业务背景：让随后 PFN walker 在重新检查 PageOffline 后决定是否读取页面。
 * 入参：无；调用者必须持 begin 取得的写锁且状态更新已完成。
 * 返回/副作用：无返回；释放写锁并唤醒等待者。
 * 注意事项：锁只串行协议，不为 page/PFN 对象增加引用或验证状态位。
 */
void page_offline_end(void)
{
	/* 发布离线状态完成后释放写锁，等待中的 PFN walker 才能重新检查页面。 */
	up_write(&page_offline_rwsem);
}
EXPORT_SYMBOL(page_offline_end);

#ifndef flush_dcache_folio
/*
 * flush_dcache_folio() - 默认逐基础页刷新整个 folio 的 D-cache。
 * 业务背景：未提供架构级 folio hook 时，保证高阶 folio 每个子页都完成 cache 维护。
 * 入参：folio 在调用期间稳定；页数可大于一。
 * 返回/副作用：无返回；逐页调用架构 flush_dcache_page，不改变 folio ownership。
 * 注意事项：仅在架构未定义同名接口时编译，具体同步效果由架构 hook 决定。
 */
void flush_dcache_folio(struct folio *folio)
{
	/* 若架构自行定义该符号，本备用实现不会编译，避免重复 cache 维护。 */
	/* 大 folio 按基础页刷 D-cache，确保所有子页的别名/指令可见性都被覆盖。 */
	long i, nr = folio_nr_pages(folio);

	for (i = 0; i < nr; i++)
		/* folio_page 将子页索引转换为基础页，架构 hook 决定实际 cache maintenance。 */
		flush_dcache_page(folio_page(folio, i));
}

/* 描述符转换不额外获取 file/mm 引用，兼容调用者必须保持原 VMA 和 file 有效。 */
EXPORT_SYMBOL(flush_dcache_folio);
#endif

/* VMA descriptor 兼容层把旧驱动回调桥接到 prepare/complete 的显式两阶段接口。 */

/**
 * compat_set_desc_from_vma() - assigns VMA descriptor @desc fields from a VMA.
 * @desc: A VMA descriptor whose fields need to be set.
 * @file: The file object describing the file being mmap()'d.
 * @vma: The VMA whose fields we wish to assign to @desc.
 *
 * This is a compatibility function to allow an mmap() hook to call
 * mmap_prepare() hooks when drivers nest these. This function specifically
 * allows the construction of a vm_area_desc value, @desc, from a VMA @vma for
 * the purposes of doing this.
 *
 * Once the conversion of drivers is complete this function will no longer be
 * required and will be removed.
 */
/*
 * compat_set_desc_from_vma() - 从既有 VMA 构造 mmap_prepare 工作描述符。
 * 业务背景：旧嵌套 .mmap 驱动借此调用新两阶段接口而不直接修改原 VMA。
 * 入参：desc 为可写输出；file/vma 为调用期间稳定借用对象。
 * 返回/副作用：无返回；清零并复制范围、file、flags、prot、ops，action 默认 NOTHING。
 * 注意事项：不获取 file/mm 引用；描述符只能在原对象受锁且有效期间使用。
 */
void compat_set_desc_from_vma(struct vm_area_desc *desc,
			      const struct file *file,
			      const struct vm_area_struct *vma)
{
	/* 描述符是可由 mmap_prepare 修改的工作副本；先清零防止旧 action/private 字段泄漏。 */
	memset(desc, 0, sizeof(*desc));

	desc->mm = vma->vm_mm;
	/* 基础范围、file、pgoff、保护与 ops 从现有 VMA 复制，尚未改变原 VMA。 */
	desc->file = (struct file *)file;
	desc->start = vma->vm_start;
	desc->end = vma->vm_end;

	desc->pgoff = vma->vm_pgoff;
	desc->vm_file = vma->vm_file;
	desc->vma_flags = vma->flags;
	desc->page_prot = vma->vm_page_prot;
	desc->vm_ops = vma->vm_ops;

	/* Default. */
	/* 默认无 action，底层 prepare 只有显式请求时才会执行 remap 等副作用。 */
	desc->action.type = MMAP_NOTHING;
}
EXPORT_SYMBOL(compat_set_desc_from_vma);

/**
 * __compat_vma_mmap() - Similar to compat_vma_mmap(), only it allows
 * flexibility as to how the mmap_prepare callback is invoked, which is useful
 * for drivers which invoke nested mmap_prepare callbacks in an mmap() hook.
 * @desc: A VMA descriptor upon which an mmap_prepare() hook has already been
 * executed.
 * @vma: The VMA to which @desc should be applied.
 *
 * The function assumes that you have obtained a VMA descriptor @desc from
 * compat_set_desc_from_vma(), and already executed the mmap_prepare() hook upon
 * it.
 *
 * It then performs any specified mmap actions, and invokes the vm_ops->mapped()
 * hook if one is present.
 *
 * See the description of compat_vma_mmap() for more details.
 *
 * Once the conversion of drivers is complete this function will no longer be
 * required and will be removed.
 *
 * Returns: 0 on success or error.
 */
/*
 * __compat_vma_mmap() - 把已运行 mmap_prepare 的描述符提交到既有 VMA。
 * 业务背景：兼容层按 prepare action、写回描述符、complete 的顺序桥接新旧驱动协议。
 * 入参：desc 是已 prepare 的工作副本；vma 是受 mmap 锁保护的目标。
 * 返回/副作用：成功 0，失败 errno；可能修改 VMA并执行 action/mapped 回调。
 * 注意事项：compat=true 时 complete 失败后的外层 hook 负责清理，本函数不自行 unmap。
 */
int __compat_vma_mmap(struct vm_area_desc *desc,
		      struct vm_area_struct *vma)
{
	/* 兼容层遵守 prepare -> 写回 VMA -> complete 的两阶段协议。 */
	int err;

	/* Perform any preparatory tasks for mmap action. */
	err = mmap_action_prepare(desc);
	if (err)
		return err;
	/* Update the VMA from the descriptor. */
	compat_set_vma_from_desc(vma, desc);
	/* prepare 的描述符修改在此一次性提交给既有 VMA，随后 complete 可依赖其状态。 */
	/* Complete any specified mmap actions. */
	return mmap_action_complete(vma, &desc->action, /*is_compat=*/true);
}
EXPORT_SYMBOL(__compat_vma_mmap);

/**
 * compat_vma_mmap() - Apply the file's .mmap_prepare() hook to an
 * existing VMA and execute any requested actions.
 * @file: The file which possesss an f_op->mmap_prepare() hook.
 * @vma: The VMA to apply the .mmap_prepare() hook to.
 *
 * Ordinarily, .mmap_prepare() is invoked directly upon mmap(). However, certain
 * stacked drivers invoke a nested mmap hook of an underlying file.
 *
 * Until all drivers are converted to use .mmap_prepare(), we must be
 * conservative and continue to invoke these stacked drivers using the
 * deprecated .mmap() hook.
 *
 * However we have a problem if the underlying file system possesses an
 * .mmap_prepare() hook, as we are in a different context when we invoke the
 * .mmap() hook, already having a VMA to deal with.
 *
 * compat_vma_mmap() is a compatibility function that takes VMA state,
 * establishes a struct vm_area_desc descriptor, passes to the underlying
 * .mmap_prepare() hook and applies any changes performed by it.
 *
 * Once the conversion of drivers is complete this function will no longer be
 * required and will be removed.
 *
 * Returns: 0 on success or error.
 */
/*
 * compat_vma_mmap() - 对旧 .mmap 上下文中的既有 VMA 应用底层 .mmap_prepare。
 * 业务背景：堆叠驱动尚未全部迁移时，将现有 VMA 转为 descriptor 并执行新 action 协议。
 * 入参：file 提供 prepare hook；vma 是锁内稳定、待更新的既有映射。
 * 返回/副作用：成功 0，失败 errno；可能更新 VMA/私有数据并执行映射 action。
 * 注意事项：兼容 VMA 已在 rmap 可见，故强制关闭 hide_from_rmap_until_complete。
 */
int compat_vma_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* 旧 .mmap 嵌套调用借此适配新 .mmap_prepare 接口，避免驱动间重复实现转换。 */
	struct vm_area_desc desc;
	struct mmap_action *action;
	int err;

	compat_set_desc_from_vma(&desc, file, vma);
	err = vfs_mmap_prepare(file, &desc);
	if (err)
		return err;
	action = &desc.action;
	/* 已有 VMA 的兼容调用不需暂时从 rmap 隐藏；该约束只适用于新建映射。 */

	/* being invoked from .mmmap means we don't have to enforce this. */
	action->hide_from_rmap_until_complete = false;

	return __compat_vma_mmap(&desc, vma);
}
EXPORT_SYMBOL(compat_vma_mmap);

/* page snapshot 故意容忍并发 compound 变化：可用性优先于把调试路径变成全局锁点。 */

/*
 * set_ps_flags() - 为 page snapshot 补充 buddy 与 idle 派生标志。
 * 业务背景：原始 struct page 副本不足以识别高阶 buddy 内页和 folio idle 状态。
 * 入参：ps 为可写快照；folio/page 是同一目标的瞬时借用视图。
 * 返回/副作用：无返回；只 OR 快照 flags，不修改真实页。
 * 注意事项：无锁读取允许并发变化，标志只代表观测时刻且不稳定对象生命周期。
 */
static void set_ps_flags(struct page_snapshot *ps, const struct folio *folio,
			 const struct page *page)
{
	/* snapshot flag 补充复制的 struct page 位，供调试者区分 buddy/free 与 idle 状态。 */
	/*
	 * Only the first page of a high-order buddy page has PageBuddy() set.
	 * So we have to check manually whether this page is part of a high-
	 * order buddy page.
	 */
	if (PageBuddy(page))
		ps->flags |= PAGE_SNAPSHOT_PG_BUDDY;
	else if (page_count(page) == 0 && is_free_buddy_page(page))
		ps->flags |= PAGE_SNAPSHOT_PG_BUDDY;

	if (folio_test_idle(folio))
		/* idle 是 folio 属性，快照只记录观测时刻而不尝试稳定该状态。 */
		ps->flags |= PAGE_SNAPSHOT_PG_IDLE;
}

/**
 * snapshot_page() - Create a snapshot of a struct page
 * @ps: Pointer to a struct page_snapshot to store the page snapshot
 * @page: The page to snapshot
 *
 * Create a snapshot of the page and store both its struct page and struct
 * folio representations in @ps.
 *
 * A snapshot is marked as "faithful" if the compound state of @page was
 * stable and allowed safe reconstruction of the folio representation. In
 * rare cases where this is not possible (e.g. due to folio splitting),
 * snapshot_page() falls back to treating @page as a single page and the
 * snapshot is marked as "unfaithful". The snapshot_page_is_faithful()
 * helper can be used to check for this condition.
 */
/*
 * snapshot_page() - 无锁构造 page 与 folio 元数据的调试快照。
 * 业务背景：在 compound split/merge 并发下尽量重建一致 folio，失败则安全降级为单页。
 * 入参：ps 为可写输出；page 在复制期间地址有效，但 compound 状态可变化。
 * 返回/副作用：无返回；写 pfn/index/flags 和结构副本，不修改真实页或取得引用。
 * 注意事项：最多重试五次；FAITHFUL 清除表示 folio 解释不可靠，消费者必须检查。
 */
void snapshot_page(struct page_snapshot *ps, const struct page *page)
{
	/* 无锁诊断快照允许 compound 状态并发变化；有限重试后宁可标记不可信也不无限循环。 */
	unsigned long info, nr_pages = 1;
	struct folio *foliop;
	int loops = 5;

	ps->pfn = page_to_pfn(page);
	/* faithful 初值只有在无法一致重建 folio 时才清除。 */
	ps->flags = PAGE_SNAPSHOT_FAITHFUL;

again:
	/* 每轮都重新复制原 page，避免把前一轮的部分 compound 信息混入本轮。 */
	memset(&ps->folio_snapshot, 0, sizeof(struct folio));
	memcpy(&ps->page_snapshot, page, sizeof(*page));
	info = ps->page_snapshot.compound_info;
	if (!(info & 1)) {
		/* 非 tail 标记时先把本地快照解释为 folio；小 folio 可立即完成。 */
		ps->idx = 0;
		foliop = (struct folio *)&ps->page_snapshot;
		if (!folio_test_large(foliop)) {
			/* order-0 情况无需读取真实 folio 的第二个 struct page。 */
			set_ps_flags(ps, page_folio(page), page);
			memcpy(&ps->folio_snapshot, foliop,
			       sizeof(struct page));
			return;
		}
		foliop = (struct folio *)page;
	} else {
		/* tail page 的 compound_info 编码 head 地址，需按当前架构的掩码格式还原。 */
		/* See compound_head() */
		if (compound_info_has_mask()) {
			unsigned long p = (unsigned long)page;

			foliop = (struct folio *)(p & info);
		} else {
			foliop = (struct folio *)(info - 1);
		}

		ps->idx = folio_page_idx(foliop, page);
	}

	if (ps->idx < MAX_FOLIO_NR_PAGES) {
		/* 仅在 snapshot 表示范围内复制 head/第二页，防止异常元数据导致越界读取。 */
		memcpy(&ps->folio_snapshot, foliop, 2 * sizeof(struct page));
		nr_pages = folio_nr_pages(&ps->folio_snapshot);
		if (nr_pages > 1)
			memcpy(&ps->folio_snapshot.__page_2, &foliop->__page_2,
			       sizeof(struct page));
		set_ps_flags(ps, foliop, page);
	}

	if (ps->idx > nr_pages) {
		/* 分裂/合并竞争可能让 idx 超出刚读到的页数，重试获得一致视图。 */
		if (loops-- > 0)
			goto again;
		clear_compound_head(&ps->page_snapshot);
		/* 重试耗尽时降级为单页非 faithful 快照，保证调试接口仍返回安全对象。 */
		foliop = (struct folio *)&ps->page_snapshot;
		memcpy(&ps->folio_snapshot, foliop, sizeof(struct page));
		ps->flags = 0;
		ps->idx = 0;
	}
}

/*
 * call_vma_mapped() - 在 VMA 建立后调用可选 vm_ops->mapped 并提交私有数据。
 * 业务背景：文件/驱动只有在映射成功存在后才能完成后置初始化。
 * 入参：vma 为受锁保护且已建立的借用对象。
 * 返回/副作用：无 hook/成功返回 0，失败 errno；仅成功时发布回调返回的 private_data。
 * 注意事项：回调可修改临时指针；失败保持原 vm_private_data 供统一清理。
 */
static int call_vma_mapped(struct vm_area_struct *vma)
{
	/* mapped 回调在 VMA 已建立后通知文件/驱动，并可替换 vm_private_data。 */
	const struct vm_operations_struct *vm_ops = vma->vm_ops;
	void *vm_private_data = vma->vm_private_data;
	int err;

	if (!vm_ops || !vm_ops->mapped)
		return 0;

	err = vm_ops->mapped(vma->vm_start, vma->vm_end, vma->vm_pgoff,
			     vma->vm_file, &vm_private_data);
	if (err)
		return err;

	if (vm_private_data != vma->vm_private_data)
		/* 回调成功后才发布新的私有数据，失败时保留 VMA 原状态供清理。 */
		vma->vm_private_data = vm_private_data;
	return 0;
}

/*
 * mmap_action_finish() - 统一收尾 action、mapped 回调、rmap 锁与失败回滚。
 * 业务背景：complete 的各种 action 必须共享同一锁释放和新 VMA 撤销顺序。
 * 入参：vma/action 为当前映射事务对象；err 是前序结果；is_compat 决定清理归属。
 * 返回/副作用：返回最终 errno/0；可能解 rmap 锁、调用 mapped，非 compat 失败时 unmap 新 VMA。
 * 注意事项：只清本次未合并创建的范围；compat 失败由外层 post hook 清理。
 */
static int mmap_action_finish(struct vm_area_struct *vma,
			      struct mmap_action *action, int err,
			      bool is_compat)
{
	/* complete 后统一调用 mapped、释放可能持有的 rmap 锁，并决定是否回滚新 VMA。 */
	size_t len;

	if (!err)
		err = call_vma_mapped(vma);

	/* do_munmap() might take rmap lock, so release if held. */
	maybe_rmap_unlock_action(vma, action);
	/* do_munmap 可再次取得 rmap 锁，因此必须先解除 action 所持锁以避免锁递归。 */
	/*
	 * If this is invoked from the compatibility layer, post-mmap() hook
	 * logic will handle cleanup for us.
	 */
	if (!err || is_compat)
		/* 兼容层的 post hook 自行清理失败，普通新映射则由此处撤销。 */
		return err;

	/*
	 * If an error occurs, unmap the VMA altogether and return an error. We
	 * only clear the newly allocated VMA, since this function is only
	 * invoked if we do NOT merge, so we only clean up the VMA we created.
	 */
	len = vma_pages(vma) << PAGE_SHIFT;
	/* 只 unmap 本次未合并创建的范围，error_override 可把底层错误转换为驱动指定 errno。 */
	do_munmap(current->mm, vma->vm_start, len, NULL);

	return action->error_override ?: err;
}

#ifdef CONFIG_MMU

/*
 * check_mmap_action() - 校验 mmap action 的错误覆盖值编码。
 * 业务背景：防止正值被后续 Elvis 运算误当成合法 errno 覆盖。
 * 入参：action 为 prepare 前可读借用对象。
 * 返回/副作用：合法返回 0；非法 override WARN并返回 -EINVAL，不修改 action。
 * 注意事项：这里只验 error_override，其余 type/PFN 参数由专用 prepare 校验。
 */
static int check_mmap_action(struct mmap_action *action)
{
	/* override 若存在必须编码负 errno，拒绝正值避免调用者误把成功码当失败。 */
	const unsigned long override = action->error_override;

	if (WARN_ON_ONCE(override && !IS_ERR_VALUE(override)))
		return -EINVAL;

	return 0;
}

/**
 * mmap_action_prepare - Perform preparatory setup for an VMA descriptor
 * action which need to be performed.
 * @desc: The VMA descriptor to prepare for its @desc->action.
 *
 * Returns: %0 on success, otherwise error.
 */
/*
 * mmap_action_prepare() - 在 VMA 发布前校验并准备描述符指定的 MMU action。
 * 业务背景：把可能失败的 PFN/I/O/kernel-page 资源准备与实际映射提交分离。
 * 入参：desc 为可写工作描述符，action 尚未 complete。
 * 返回/副作用：NOTHING 为 0，其它透传专用 prepare errno；未知 type WARN并返回 -EINVAL。
 * 注意事项：成功只表示资源已准备，调用者仍必须 complete 或按专用协议回滚。
 */
int mmap_action_prepare(struct vm_area_desc *desc)
{
	/* prepare 只建立 action 所需资源，不应在 VMA 还未提交前暴露映射。 */
	struct mmap_action *action = &desc->action;
	int err;

	err = check_mmap_action(action);
	if (err)
		return err;

	switch (action->type) {
	/* 每种 action 由专门 prepare 校验 PFN、I/O 或内核页参数。 */
	/* NOTHING 是有效无操作，调用者可统一走 prepare 而不用预先分支。 */
	case MMAP_NOTHING:
		return 0;
	case MMAP_REMAP_PFN:
		/* remap prepare 可以拒绝不合法 PFN/保护组合，失败时尚未改变 VMA。 */
		return remap_pfn_range_prepare(desc);
	case MMAP_IO_REMAP_PFN:
		return io_remap_pfn_range_prepare(desc);
	case MMAP_SIMPLE_IO_REMAP:
		return simple_ioremap_prepare(desc);
	case MMAP_MAP_KERNEL_PAGES:
		return map_kernel_pages_prepare(desc);
	}

	WARN_ON_ONCE(1);
	/* 未知枚举属于内核调用者 bug，返回 -EINVAL 阻止继续 complete。 */
	return -EINVAL;
}
EXPORT_SYMBOL(mmap_action_prepare);

/**
 * mmap_action_complete - Execute VMA descriptor action.
 * @vma: The VMA to perform the action upon.
 * @action: The action to perform.
 * @is_compat: Is this being invoked from the compatibility layer?
 *
 * Similar to mmap_action_prepare().
 *
 * Return: 0 on success, or error, at which point the VMA will be unmapped if
 * !@is_compat.
 */
/*
 * mmap_action_complete() - 对已存在 VMA 执行已准备的 MMU action并统一收尾。
 * 业务背景：VMA 发布后提交 PFN/kernel pages，随后调用 mapped 并处理失败 unmap。
 * 入参：vma/action 为同一事务；is_compat 指定失败清理由本层还是兼容外层负责。
 * 返回/副作用：成功 0，失败 errno；可能改变映射、私有数据、锁状态或撤销新 VMA。
 * 注意事项：I/O 两种 action 应走委托路径，抵达此处会 WARN并失败。
 */
int mmap_action_complete(struct vm_area_struct *vma,
			 struct mmap_action *action, bool is_compat)
{
	/* action type 是 prepare 阶段已验证的契约；这里仍防御性拒绝不应抵达的类型。 */
	/* complete 在 VMA 已存在时执行实际映射，最后无论成败进入统一 finish。 */
	int err = 0;

	switch (action->type) {
	case MMAP_NOTHING:
		break;
	case MMAP_REMAP_PFN:
		/* PFN remap 与 kernel-pages 映射各有独立 complete，实现不在此层混用。 */
		err = remap_pfn_range_complete(vma, action);
		break;
	case MMAP_MAP_KERNEL_PAGES:
		err = map_kernel_pages_complete(vma, action);
		break;
	case MMAP_IO_REMAP_PFN:
	case MMAP_SIMPLE_IO_REMAP:
		/* Should have been delegated. */
		WARN_ON_ONCE(1);
		/* I/O action 应已由专用路径委托，落到这里说明 prepare/调用路径违规。 */
		err = -EINVAL;
		break;
	}

	return mmap_action_finish(vma, action, err, is_compat);
}
EXPORT_SYMBOL(mmap_action_complete);
#else
/*
 * mmap_action_prepare() - NOMMU 构建的 action 兼容桩。
 * 业务背景：共享驱动代码保留两阶段接口，但无页表可执行 PFN/kernel-page remap。
 * 入参：desc 为借用描述符。
 * 返回/副作用：始终 0；非 NOTHING 类型仅 WARN，不准备资源。
 * 注意事项：调用者不能把 0 解释为 NOMMU 已支持该映射 action。
 */
int mmap_action_prepare(struct vm_area_desc *desc)
{
	/* NOMMU 没有可执行的 PFN/VMA remap；保留接口以让共享调用者可编译。 */
	/* 即使收到非法 action 也只 WARN，因为 NOMMU 路径不能创建可回滚的页表映射。 */
	switch (desc->action.type) {
	case MMAP_NOTHING:
		break;
	case MMAP_REMAP_PFN:
	case MMAP_IO_REMAP_PFN:
	case MMAP_SIMPLE_IO_REMAP:
	case MMAP_MAP_KERNEL_PAGES:
		/* NOMMU 不支持的类型统一告警，保持后续 case 共享同一失败出口。 */
		WARN_ON_ONCE(1); /* nommu cannot handle these. */
		break;
	}

	return 0;
}
EXPORT_SYMBOL(mmap_action_prepare);

/*
 * mmap_action_complete() - NOMMU 构建的 action 收尾实现。
 * 业务背景：保持 mapped/rmap/compat 清理流程统一，同时拒绝不可执行的映射类型。
 * 入参：vma/action 为事务对象；is_compat 决定失败清理由谁承担。
 * 返回/副作用：NOTHING 走 finish；其它类型 WARN并以 -EINVAL 进入 finish。
 * 注意事项：不会创建页表映射，但 finish 仍可能调用 mapped 或执行兼容清理逻辑。
 */
int mmap_action_complete(struct vm_area_struct *vma,
			 struct mmap_action *action,
			 bool is_compat)
{
	/* NOMMU 对非空 action 返回 -EINVAL，但仍走 finish 保持清理/兼容语义一致。 */
	/* err 初始为零对应 MMAP_NOTHING；非法类型才转换成失败。 */
	int err = 0;

	switch (action->type) {
	case MMAP_NOTHING:
		break;
	case MMAP_REMAP_PFN:
	case MMAP_IO_REMAP_PFN:
	case MMAP_SIMPLE_IO_REMAP:
	case MMAP_MAP_KERNEL_PAGES:
		/* 此处的 -EINVAL 会传入 finish，兼容路径可依据 is_compat 决定额外清理。 */
		WARN_ON_ONCE(1); /* nommu cannot handle this. */

		err = -EINVAL;
		break;
	}

	return mmap_action_finish(vma, action, err, is_compat);
}
EXPORT_SYMBOL(mmap_action_complete);
#endif

/* PTE batch 调用者负责限制 VMA/页表边界，函数本身不取得页表锁或 folio 引用。 */

/* 末尾 helper 分别服务大 folio 的 PTE 批处理和 sparsemem 的页面连续性检查。 */

#ifdef CONFIG_MMU
/**
 * folio_pte_batch - detect a PTE batch for a large folio
 * @folio: The large folio to detect a PTE batch for.
 * @ptep: Page table pointer for the first entry.
 * @pte: Page table entry for the first page.
 * @max_nr: The maximum number of table entries to consider.
 *
 * This is a simplified variant of folio_pte_batch_flags().
 *
 * Detect a PTE batch: consecutive (present) PTEs that map consecutive
 * pages of the same large folio in a single VMA and a single page table.
 *
 * All PTEs inside a PTE batch have the same PTE bits set, excluding the PFN,
 * the accessed bit, writable bit, dirt-bit and soft-dirty bit.
 *
 * ptep must map any page of the folio. max_nr must be at least one and
 * must be limited by the caller so scanning cannot exceed a single VMA and
 * a single page table.
 *
 * Return: the number of table entries in the batch.
 */
/*
 * folio_pte_batch() - 识别同一大 folio 的连续等价 PTE 批次。
 * 业务背景：上层可一次处理多个连续映射，减少逐 PTE 检查开销。
 * 入参：folio/ptep/pte 描述首项；max_nr>=1且由调用者限制在单 VMA、单页表。
 * 返回/副作用：返回批次项数，至少 1；只读 PTE，不取 folio 引用或页表锁。
 * 注意事项：调用者必须已持所需页表同步；包装固定 flags=0。
 */
unsigned int folio_pte_batch(struct folio *folio, pte_t *ptep, pte_t pte,
		unsigned int max_nr)
{
	/* 返回批次数供上层一次处理连续映射，PFN 与访问位差异由下层 helper 忽略。 */
	/* 包装器不额外扫描，只固定 flags=0 并把首个 PTE 以指针传给通用批处理器。 */
	return folio_pte_batch_flags(folio, NULL, ptep, &pte, max_nr, 0);
}
#endif /* CONFIG_MMU */

#if defined(CONFIG_SPARSEMEM) && !defined(CONFIG_SPARSEMEM_VMEMMAP)
/**
 * page_range_contiguous - test whether the page range is contiguous
 * @page: the start of the page range.
 * @nr_pages: the number of pages in the range.
 *
 * Test whether the page range is contiguous, such that they can be iterated
 * naively, corresponding to iterating a contiguous PFN range.
 *
 * This function should primarily only be used for debug checks, or when
 * working with page ranges that are not naturally contiguous (e.g., pages
 * within a folio are).
 *
 * Returns true if contiguous, otherwise false.
 */
/*
 * page_range_contiguous() - 验证逻辑连续 PFN 是否也可按 struct page 指针线性迭代。
 * 业务背景：sparsemem 非 vmemmap 的不同 section memmap 可能在虚拟地址上不相邻。
 * 入参：page 是首页借用指针；nr_pages 描述范围且调用者负责 PFN 加法不溢出。
 * 返回/副作用：跨越的每个 section 边界均匹配则 true，否则 false；纯读取。
 * 注意事项：只在相关 sparsemem 配置编译；主要供调试或非天然连续 page range 使用。
 */
bool page_range_contiguous(const struct page *page, unsigned long nr_pages)
{
	/* 仅在 sparsemem 非 vmemmap 配置可见；其它布局天然保证线性 memmap。 */
	/* sparsemem 非 vmemmap 下 struct page 地址未必随 PFN 连续，跨 section 必须显式验证。 */
	const unsigned long start_pfn = page_to_pfn(page);
	const unsigned long end_pfn = start_pfn + nr_pages;
	unsigned long pfn;
	/* 起止 PFN 描述逻辑范围；循环只检查跨越的 section 边界，无需逐页扫描。 */

	/*
	 * The memmap is allocated per memory section, so no need to check
	 * within the first section. However, we need to check each other
	 * spanned memory section once, making sure the first page in a
	 * section could similarly be reached by just iterating pages.
	 */
	for (pfn = ALIGN(start_pfn, PAGES_PER_SECTION);
	     pfn < end_pfn; pfn += PAGES_PER_SECTION)
		if (unlikely(page + (pfn - start_pfn) != pfn_to_page(pfn)))
			/* 任一 section 首页无法由指针步进到达，就不能对整个范围做线性迭代。 */
			return false;
	return true;
}
EXPORT_SYMBOL(page_range_contiguous);
#endif
