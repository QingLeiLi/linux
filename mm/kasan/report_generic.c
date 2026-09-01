// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains generic KASAN specific error reporting code.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * Some code borrowed from https://github.com/xairy/kasan-prototype by
 *        Andrey Konovalov <andreyknvl@gmail.com>
 */
/* Generic KASAN 把 shadow 字节、slab 生命周期元数据和编译器栈描述解码为可读错误报告。 */

#include <linux/bitops.h>
#include <linux/ftrace.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
/* printk/sched 提供报告输出与 current 栈界；slab/stackdepot 提供对象和历史栈元数据。 */
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
/* 公共 kasan.h 暴露报告 ABI，私有 kasan.h/slab.h 暴露 shadow 与对象布局 helper。 */
#include <linux/types.h>
#include <linux/kasan.h>
#include <linux/module.h>

#include <asm/sections.h>

#include "kasan.h"
#include "../slab.h"

/*
 * 业务背景：通用报告先定位访问区间中第一个非完全可访问的 KASAN granule。
 * 入参：addr 为访问起点；size 为访问字节数，二者均仅借用且可能覆盖无 metadata 地址。
 * 出参/返回：无 shadow 映射时返 addr；否则返首个非零 shadow 对应地址，或区间末端。
 * 注意事项：报告路径只读 shadow、不取引用；按 KASAN_GRANULE_SIZE 步进，不能睡眠。
 */
const void *kasan_find_first_bad_addr(const void *addr, size_t size)
{
	/* p 是当前应用地址粒度起点；kasan_mem_to_shadow 把它映射为权限摘要字节。 */
	const void *p = addr;

	if (!addr_has_metadata(p))
		return p;

	/* shadow=0 表示整 granule 可访问；遇到部分/poison 值即停止。 */
	while (p < addr + size && !(*(u8 *)kasan_mem_to_shadow(p)))
		p += KASAN_GRANULE_SIZE;

	return p;
}

/*
 * 业务背景：报告 slab 越界时，从 shadow 推导本次分配的有效大小而非只显示 cache object_size。
 * 入参：object 为 slab 对象起点；cache 为所属 cache，均借用且 metadata 必须存在。
 * 出参/返回：返回 1..object_size 的有效字节；已释放/不可解码返回 0；全可访问返 object_size。
 * 注意事项：仅只读 shadow；对象生命周期由报告串行/allocator 状态保证，不睡眠。
 */
size_t kasan_get_alloc_size(void *object, struct kmem_cache *cache)
{
	size_t size = 0;
	u8 *shadow;

	/*
	 * Skip the addr_has_metadata check, as this function only operates on
	 * slab memory, which must have metadata.
	 */
	/* 调用者只对 slab 对象调用，slab KASAN shadow 是强制存在的，无需重复地址范围检查。 */

	/*
	 * The loop below returns 0 for freed objects, for which KASAN cannot
	 * calculate the allocation size based on the metadata.
	 */
	/* 已释放对象的 poison 值不编码原分配长度，因此循环会返回 0 触发 object_size 回退。 */
	shadow = (u8 *)kasan_mem_to_shadow(object);
	/* 每个零 shadow 贡献一个完整 granule，1..granule-1 编码尾粒度有效字节数。 */
	while (size < cache->object_size) {
		/* 分支按 shadow ABI 区分完整、部分与 poison granule，并在首次边界停止。 */
		if (*shadow == 0)
			size += KASAN_GRANULE_SIZE;
		else if (*shadow >= 1 && *shadow <= KASAN_GRANULE_SIZE - 1)
			return size + *shadow;
		else
			return size;
		shadow++;
	}

	return cache->object_size;
}

/*
 * 业务背景：依据首坏地址附近 shadow poison 魔数把通用访问错误分类。
 * 入参：info 为输入报告对象，first_bad_addr 已定位且有 metadata；本函数只借用。
 * 出参/返回：返回静态 bug type 字符串，未知值返回 "unknown-crash"。
 * 注意事项：shadow 可与无锁内核写竞争，分类是尽力诊断；不修改 info、不睡眠。
 */
static const char *get_shadow_bug_type(struct kasan_report_info *info)
{
	const char *bug_type = "unknown-crash";
	u8 *shadow_addr;

	shadow_addr = (u8 *)kasan_mem_to_shadow(info->first_bad_addr);

	/*
	 * If shadow byte value is in [0, KASAN_GRANULE_SIZE) we can look
	 * at the next shadow byte to determine the type of the bad access.
	 */
	/* 部分可访问值描述当前 granule 尾界；越界真正原因通常由下一 shadow 字节给出。 */
	if (*shadow_addr > 0 && *shadow_addr <= KASAN_GRANULE_SIZE - 1)
		shadow_addr++;

	/* 每个编译器 ABI poison 值映射到用户可见错误类别；未知值保留兜底。 */
	switch (*shadow_addr) {
	case 0 ... KASAN_GRANULE_SIZE - 1:
		/*
		 * In theory it's still possible to see these shadow values
		 * due to a data race in the kernel code.
		 */
		/* 理论上正常/部分值仍可能因被检测代码数据竞争而被报告，此时归类普通越界。 */
		bug_type = "out-of-bounds";
		break;
	case KASAN_PAGE_REDZONE:
	case KASAN_SLAB_REDZONE:
		/* 页或 slab redzone：访问越过 slab 对象边界。 */
		bug_type = "slab-out-of-bounds";
		break;
	case KASAN_GLOBAL_REDZONE:
		/* 编译器为全局变量布置的 redzone 被访问。 */
		bug_type = "global-out-of-bounds";
		break;
	case KASAN_STACK_LEFT:
	case KASAN_STACK_MID:
	case KASAN_STACK_RIGHT:
	case KASAN_STACK_PARTIAL:
		/* 栈变量左/中/右/部分 redzone 均统一为栈越界。 */
		bug_type = "stack-out-of-bounds";
		break;
	case KASAN_PAGE_FREE:
		/* buddy 页释放 poison 表示页级 use-after-free。 */
		bug_type = "use-after-free";
		break;
	case KASAN_SLAB_FREE:
	case KASAN_SLAB_FREE_META:
		/* slab free poison 或 free metadata 标记表示对象级 use-after-free。 */
		bug_type = "slab-use-after-free";
		break;
	case KASAN_ALLOCA_LEFT:
	case KASAN_ALLOCA_RIGHT:
		/* 动态栈 alloca 两侧 redzone 被越界。 */
		bug_type = "alloca-out-of-bounds";
		break;
	case KASAN_VMALLOC_INVALID:
		/* vmalloc shadow 尚未有效映射或访问超出 vmap 范围。 */
		bug_type = "vmalloc-out-of-bounds";
		break;
	}

	return bug_type;
}

/*
 * 业务背景：没有 KASAN metadata 的坏地址只能按虚拟地址区间做粗粒度 wild access 分类。
 * 入参：info 为借用报告，access_addr 是原始故障地址。
 * 出参/返回：低页返 null-ptr-deref，用户范围返 user-memory-access，其余返 wild-memory-access。
 * 注意事项：分类不证明根因，只提供地址空间线索；无副作用、不睡眠。
 */
static const char *get_wild_bug_type(struct kasan_report_info *info)
{
	const char *bug_type = "unknown-crash";

	/* 第一页通常来自 NULL+小偏移；TASK_SIZE 以下是意外直接访问用户地址。 */
	if ((unsigned long)info->access_addr < PAGE_SIZE)
		bug_type = "null-ptr-deref";
	else if ((unsigned long)info->access_addr < TASK_SIZE)
		bug_type = "user-memory-access";
	else
		bug_type = "wild-memory-access";

	return bug_type;
}

/*
 * 业务背景：统一选择算术溢出、shadow 分类或无 metadata 地址分类路径。
 * 入参：info 为借用报告，必须含 access_addr/access_size/first_bad_addr。
 * 出参/返回：返回静态错误类型字符串，不修改报告。
 * 注意事项：先检测 addr+size 回绕，避免把负 ssize_t 转 size_t 后误走 shadow 地址计算。
 */
static const char *get_bug_type(struct kasan_report_info *info)
{
	/*
	 * If access_size is a negative number, then it has reason to be
	 * defined as out-of-bounds bug type.
	 *
	 * Casting negative numbers to size_t would indeed turn up as
	 * a large size_t and its value will be larger than ULONG_MAX/2,
	 * so that this can qualify as out-of-bounds.
	 */
	/*
	 * 编译器可把负访问长度作为 ssize_t 传入再存为 size_t；其巨大无符号值使
	 * addr+size 回绕，因此稳定归类 out-of-bounds 而不遍历荒谬范围。
	 */
	if (info->access_addr + info->access_size < info->access_addr)
		return "out-of-bounds";

	/* 有 shadow 时读取精确 poison；否则只能按虚拟地址范围作 wild 分类。 */
	if (addr_has_metadata(info->access_addr))
		return get_shadow_bug_type(info);
	return get_wild_bug_type(info);
}

/*
 * 业务背景：通用报告框架定位对象后，由 Generic KASAN 补齐 bug type 与 alloc/free 栈快照。
 * 入参：info 为输入输出报告；cache/object 可为空，已有 bug_type 时保留调用者分类。
 * 出参/返回：void；可能写 bug_type、alloc_track、free_track，不取得 metadata 所有权。
 * 注意事项：只在 shadow 明确为 FREE_META 时读取 free_meta；并发下是尽力诊断、不睡眠。
 */
void kasan_complete_mode_report_info(struct kasan_report_info *info)
{
	struct kasan_alloc_meta *alloc_meta;
	struct kasan_free_meta *free_meta;

	/* 阶段 1：仅在上层未指定 invalid-free 等类型时，从访问/shadow 推断。 */
	if (!info->bug_type)
		info->bug_type = get_bug_type(info);

	/* 非 slab 地址没有生命周期 metadata，保留类型后即可返回。 */
	if (!info->cache || !info->object)
		return;

	/* 阶段 2：复制而非借用 track，使后续打印不依赖 metadata 指针本身。 */
	alloc_meta = kasan_get_alloc_meta(info->cache, info->object);
	if (alloc_meta)
		memcpy(&info->alloc_track, &alloc_meta->alloc_track,
		       sizeof(info->alloc_track));

	if (*(u8 *)kasan_mem_to_shadow(info->object) == KASAN_SLAB_FREE_META) {
		/* Free meta must be present with KASAN_SLAB_FREE_META. */
		/* 该 poison 是 free_meta 存在的不变量标记，因此无需 NULL 分支即可复制释放栈。 */
		free_meta = kasan_get_free_meta(info->cache, info->object);
		memcpy(&info->free_track, &free_meta->free_track,
		       sizeof(info->free_track));
	}
}

/*
 * 业务背景：通用 hexdump 不能直接拿 shadow 地址，否则其自身访问可能再次触发 KASAN。
 * 入参：buffer 为至少 META_BYTES_PER_ROW 的输出区；row 为对应应用内存行起点。
 * 出参/返回：void；复制一行 shadow 字节到调用者缓冲，无 ownership 变化。
 * 注意事项：源地址由 kasan_mem_to_shadow 转换；仅用于有 metadata 的报告路径。
 */
void kasan_metadata_fetch_row(char *buffer, void *row)
{
	memcpy(buffer, kasan_mem_to_shadow(row), META_BYTES_PER_ROW);
}

/*
 * 业务背景：对象与异步 work 相关时，额外打印最近两次潜在 work 创建栈辅助关联 UAF 根因。
 * 入参：cache/object 为借用 slab 对象定位；对象可能没有 alloc metadata。
 * 出参/返回：void；只输出非零 stack depot handle，不修改对象。
 * 注意事项：报告上下文调用；handle 是 stack depot 引用标识，打印次序从最近到次近。
 */
void kasan_print_aux_stacks(struct kmem_cache *cache, const void *object)
{
	struct kasan_alloc_meta *alloc_meta;

	alloc_meta = kasan_get_alloc_meta(cache, object);
	if (!alloc_meta)
		return;

	/* 两个槽独立有效；缺一项不会阻止打印另一项。 */
	if (alloc_meta->aux_stack[0]) {
		/* 槽 0 是最近一次潜在关联 work 创建事件。 */
		pr_err("Last potentially related work creation:\n");
		stack_depot_print(alloc_meta->aux_stack[0]);
		pr_err("\n");
	}
	/* 槽 1 保存再前一次事件，帮助识别 work 对象重复排队/复用历史。 */
	if (alloc_meta->aux_stack[1]) {
		pr_err("Second to last potentially related work creation:\n");
		stack_depot_print(alloc_meta->aux_stack[1]);
		pr_err("\n");
	}
}

#ifdef CONFIG_KASAN_STACK
/*
 * 业务背景：编译器把栈帧描述编码为空格分隔串，本 helper 逐 token 解析并可转十进制数。
 * 入参：frame_descr 为输入输出游标；token 可空输出缓冲；max_tok_len 是容量；value 可空数值输出。
 * 出参/返回：成功返 true 并推进游标；token 过长或数值非法返 false，已推进部分不回滚。
 * 注意事项：所有指针借用；value 非空时调用者也必须提供 token，报告错误可打印其文本。
 */
static bool __must_check tokenize_frame_descr(const char **frame_descr,
					      char *token, size_t max_tok_len,
					      unsigned long *value)
{
	const char *sep = strchr(*frame_descr, ' ');

	/* 找不到空格时把字符串末尾当分隔点，允许解析最后一个 token。 */
	if (sep == NULL)
		sep = *frame_descr + strlen(*frame_descr);

	/* 可选复制阶段保证 NUL 空间；过长描述停止解码，避免报告路径栈缓冲溢出。 */
	if (token != NULL) {
		const size_t tok_len = sep - *frame_descr;

		if (tok_len + 1 > max_tok_len) {
			pr_err("internal error: frame description too long: %s\n",
			       *frame_descr);
			return false;
		}

		/* Copy token (+ 1 byte for '\0'). */
		/* strscpy 长度包含终止 NUL，精确复制当前 token 而不带分隔空格。 */
		strscpy(token, *frame_descr, tok_len + 1);
	}

	/* Advance frame_descr past separator. */
	/* 输入输出游标提交到下一 token；调用者随后按字段协议继续解析。 */
	*frame_descr = sep + 1;

	/* 需要数值时使用十进制严格转换；失败保留诊断并中止本帧。 */
	if (value != NULL && kstrtoul(token, 10, value)) {
		pr_err("internal error: not a valid number: %s\n", token);
		return false;
	}

	return true;
}

/*
 * 业务背景：把编译器栈帧字符串解码为每个局部对象的 [offset,end) 与名称。
 * 入参：frame_descr 为借用 NUL 字符串，格式是对象数后跟每对象四个 token。
 * 出参/返回：void；逐项打印，格式错误时停止且不影响主 KASAN 报告。
 * 注意事项：只接受固定 64 字节 token；name:line 会去掉行号，因为描述中没有文件名。
 */
static void print_decoded_frame_descr(const char *frame_descr)
{
	/*
	 * We need to parse the following string:
	 *    "n alloc_1 alloc_2 ... alloc_n"
	 * where alloc_i looks like
	 *    "offset size len name"
	 * or "offset size len name:line".
	 */
	/* 每个对象编码 offset、size、name length、name 或 name:line，首 token 是对象数。 */

	char token[64];
	unsigned long num_objects;

	/* 阶段 1：解析对象数；失败只放弃附加栈对象信息。 */
	if (!tokenize_frame_descr(&frame_descr, token, sizeof(token),
				  &num_objects))
		return;

	pr_err("\n");
	pr_err("This frame has %lu %s:\n", num_objects,
	       num_objects == 1 ? "object" : "objects");

	/* 阶段 2：按编译器 ABI 固定字段顺序逐对象消费描述。 */
	while (num_objects--) {
		unsigned long offset;
		unsigned long size;

		/* access offset */
		/* offset/size 共同确定对象在 frame 基址内的半开区间。 */
		if (!tokenize_frame_descr(&frame_descr, token, sizeof(token),
					  &offset))
			return;
		/* access size */
		if (!tokenize_frame_descr(&frame_descr, token, sizeof(token),
					  &size))
			return;
		/* name length (unused) */
		/* 当前实现不需要 name length，但必须消费 token 才能保持游标对齐。 */
		if (!tokenize_frame_descr(&frame_descr, NULL, 0, NULL))
			return;
		/* object name */
		if (!tokenize_frame_descr(&frame_descr, token, sizeof(token),
					  NULL))
			return;

		/* Strip line number; without filename it's not very helpful. */
		/* 只保留变量名；孤立行号缺少文件名，反而会误导定位。 */
		strreplace(token, ':', '\0');

		/* Finally, print object information. */
		/* 此处只是报告输出，不验证区间是否覆盖坏地址。 */
		pr_err(" [%lu, %lu) '%s'", offset, offset + size, token);
	}
}

/* Returns true only if the address is on the current task's stack. */
/* 仅当地址可从当前任务栈 shadow 回溯到合法编译器帧标记时，返回帧偏移、描述和 PC。 */
/*
 * 业务背景：栈越界报告需从 shadow 左 redzone 向下找到编译器埋入的帧头。
 * 入参：addr 为当前任务栈内坏地址；offset/frame_descr/frame_pc 均为不可空输出指针。
 * 出参/返回：找到且 magic 正确返 true 并填全部输出；越过栈底或 marker 错误返 false。
 * 注意事项：只支持向下增长栈；输出描述/PC 为借用编译器元数据，不取得引用。
 */
static bool __must_check get_address_stack_frame_info(const void *addr,
						      unsigned long *offset,
						      const char **frame_descr,
						      const void **frame_pc)
{
	unsigned long aligned_addr;
	unsigned long mem_ptr;
	const u8 *shadow_bottom;
	const u8 *shadow_ptr;
	const unsigned long *frame;

	/* 算法按地址递减回溯 shadow，向上增长栈在编译期明确拒绝。 */
	BUILD_BUG_ON(IS_ENABLED(CONFIG_STACK_GROWSUP));

	/* 阶段 1：对齐坏地址并建立应用地址、shadow 地址和当前任务栈底三套游标。 */
	aligned_addr = round_down((unsigned long)addr, sizeof(long));
	mem_ptr = round_down(aligned_addr, KASAN_GRANULE_SIZE);
	shadow_ptr = kasan_mem_to_shadow((void *)aligned_addr);
	shadow_bottom = kasan_mem_to_shadow(end_of_stack(current));

	/* 阶段 2：先找左 redzone，再跨过连续左 redzone 到达其前方帧头。 */
	while (shadow_ptr >= shadow_bottom && *shadow_ptr != KASAN_STACK_LEFT) {
		shadow_ptr--;
		mem_ptr -= KASAN_GRANULE_SIZE;
	}

	while (shadow_ptr >= shadow_bottom && *shadow_ptr == KASAN_STACK_LEFT) {
		shadow_ptr--;
		mem_ptr -= KASAN_GRANULE_SIZE;
	}

	/* 未在当前任务栈范围找到完整边界时，不产生猜测输出。 */
	if (shadow_ptr < shadow_bottom)
		return false;

	/* 阶段 3：左 redzone 后一个 granule 是编译器 ABI 帧头，先验证 magic 再解引用字段。 */
	frame = (const unsigned long *)(mem_ptr + KASAN_GRANULE_SIZE);
	if (frame[0] != KASAN_CURRENT_STACK_FRAME_MAGIC) {
		pr_err("internal error: frame has invalid marker: %lu\n",
		       frame[0]);
		return false;
	}

	/* 成功出口一次填充偏移、描述指针和创建该帧的 PC。 */
	*offset = (unsigned long)addr - (unsigned long)frame;
	*frame_descr = (const char *)frame[1];
	*frame_pc = (void *)frame[2];

	return true;
}

/*
 * 业务背景：为栈上坏地址打印当前任务、所属函数帧、帧内偏移和局部对象布局。
 * 入参：addr 为借用坏地址，必须位于 current 的内核栈。
 * 出参/返回：void；尽力输出，地址/帧元数据无效时提前返回。
 * 注意事项：仅 CONFIG_KASAN_STACK 编译；不持有 task 引用，报告路径的 current 保证栈生命周期。
 */
void kasan_print_address_stack_frame(const void *addr)
{
	unsigned long offset;
	const char *frame_descr;
	const void *frame_pc;

	/* 阶段 1：守住 current 栈前置条件，避免用其他地址扫描任意 shadow。 */
	if (WARN_ON(!object_is_on_stack(addr)))
		return;

	pr_err("The buggy address belongs to stack of task %s/%d\n",
	       current->comm, task_pid_nr(current));

	/* 阶段 2：解出编译器帧头；失败仍保留上面已打印的任务身份。 */
	if (!get_address_stack_frame_info(addr, &offset, &frame_descr,
					  &frame_pc))
		return;

	pr_err(" and is located at offset %lu in frame:\n", offset);
	pr_err(" %pS\n", frame_pc);

	/* 描述可空：PC/offset 仍有效，但没有局部对象表可继续解码。 */
	if (!frame_descr)
		return;

	print_decoded_frame_descr(frame_descr);
}
#endif /* CONFIG_KASAN_STACK */

/*
 * 编译器会调用固定大小的 __asan_report_loadN_noabort 符号；宏为每个 N 生成
 * void(addr) 包装并把 size=N、write=false、调用点 _RET_IP_ 交给 kasan_report。
 * 每个生成函数无直接返回值，addr 为借用故障地址，EXPORT_SYMBOL 供模块插桩引用。
 */
#define DEFINE_ASAN_REPORT_LOAD(size)                     \
void __asan_report_load##size##_noabort(void *addr) \
{                                                         \
	kasan_report(addr, size, false, _RET_IP_);	  \
}                                                         \
EXPORT_SYMBOL(__asan_report_load##size##_noabort)

/*
 * store 版本与 load ABI 对称，唯一语义差异是 write=true，使报告明确这是写越界；
 * 宏续行不可插入独立代码，展开后同样生成并导出 1/2/4/8/16 字节入口。
 */
#define DEFINE_ASAN_REPORT_STORE(size)                     \
void __asan_report_store##size##_noabort(void *addr) \
{                                                          \
	kasan_report(addr, size, true, _RET_IP_);	   \
}                                                          \
EXPORT_SYMBOL(__asan_report_store##size##_noabort)

/* 展开编译器 Generic ASan ABI 要求的五种固定宽度 load/store 报告入口。 */
DEFINE_ASAN_REPORT_LOAD(1);
DEFINE_ASAN_REPORT_LOAD(2);
DEFINE_ASAN_REPORT_LOAD(4);
DEFINE_ASAN_REPORT_LOAD(8);
DEFINE_ASAN_REPORT_LOAD(16);
DEFINE_ASAN_REPORT_STORE(1);
DEFINE_ASAN_REPORT_STORE(2);
DEFINE_ASAN_REPORT_STORE(4);
DEFINE_ASAN_REPORT_STORE(8);
DEFINE_ASAN_REPORT_STORE(16);

/*
 * 业务背景：编译器对非常量/非标准宽度读访问调用通用长度的 noabort 报告 ABI。
 * 入参：addr 为借用故障起点；size 为有符号访问字节数，异常负值由分类逻辑识别。
 * 出参/返回：void；调用 kasan_report 输出读错误并按 noabort 语义返回调用者。
 * 注意事项：_RET_IP_ 捕获插桩调用点；函数导出给可加载模块。
 */
void __asan_report_load_n_noabort(void *addr, ssize_t size)
{
	kasan_report(addr, size, false, _RET_IP_);
}
EXPORT_SYMBOL(__asan_report_load_n_noabort);

/*
 * 业务背景：编译器对非常量/非标准宽度写访问调用通用长度的 noabort 报告 ABI。
 * 入参：addr 为借用故障起点；size 为有符号写入字节数。
 * 出参/返回：void；以 write=true 调用 kasan_report，报告后返回。
 * 注意事项：不取得地址 ownership；_RET_IP_ 必须在此 wrapper 捕获以指向原插桩调用链。
 */
void __asan_report_store_n_noabort(void *addr, ssize_t size)
{
	kasan_report(addr, size, true, _RET_IP_);
}
EXPORT_SYMBOL(__asan_report_store_n_noabort);
