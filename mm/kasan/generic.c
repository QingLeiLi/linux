// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains core generic KASAN code.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * Some code borrowed from https://github.com/xairy/kasan-prototype by
 *        Andrey Konovalov <andreyknvl@gmail.com>
 */
/*
 * 本文件实现 Generic KASAN 的 shadow 检查、编译器 ASan ABI 与 slab metadata；
 * 原版权、作者及代码来源保持不变。核心编码是每 8 字节应用内存对应 1 个 shadow 字节：
 * 0 表示全可访问，1..7 表示前若干字节可访问，负值毒码表示整粒度不可访问。
 */

#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/kfence.h>
#include <linux/kmemleak.h>
#include <linux/linkage.h>
/* memblock/memory/mm 接口覆盖启动期 shadow、地址分类与页表可见范围。 */
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
/* 调度、slab 与 stack depot 为对象生命周期 track 和 cache metadata 提供上下文。 */
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
/* 类型、vmalloc 与告警接口完成编译器 ABI 的范围校验及 shadow 更新。 */
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/bug.h>

#include "kasan.h"
#include "../slab.h"

/*
 * Initialize Generic KASAN and enable runtime checks.
 * This should be called from arch kasan_init() once shadow memory is ready.
 */
/*
 * Generic KASAN 必须等体系结构建好全部 shadow 映射后才能打开运行期检查；否则插桩访问
 * shadow 本身会缺页。该入口只发布 static key 并打印一次模式信息。
 */
/*
 * 业务背景：体系结构 KASAN 初始化末尾发布 Generic 模式可用状态。
 * 入参：无；出参/返回：无，副作用是启用 kasan_enabled() 并输出启动日志。
 * 注意事项：仅 __init 阶段调用且必须晚于 shadow 建表；过早发布会让插桩访问无效元数据。
 */
void __init kasan_init_generic(void)
{
	kasan_enable();

	pr_info("KernelAddressSanitizer initialized (generic)\n");
}

/*
 * All functions below always inlined so compiler could
 * perform better optimizations in each of __asan_loadX/__assn_storeX
 * depending on memory access size X.
 */
/*
 * 下列检查器强制内联，使编译器为固定访问宽度裁剪分支；最终由 __asan_load/storeX
 * 调用。原文中的 __assn_storeX 是拼写错误，当前源码实际符号为 __asan_storeX。
 */

/*
 * 业务背景：检查单个字节对应的 shadow 编码，是所有固定/变长检查的末端原语。
 * 入参：addr 为借用应用地址；出参/返回：被 poison 返回 true，否则 false。
 * 注意事项：调用者已确认地址有 metadata；直接读取 shadow，不提供并发生命周期锁。
 */
static __always_inline bool memory_is_poisoned_1(const void *addr)
{
	/* shadow_value 是整个 granule 状态；last_accessible_byte 是 addr 在其中的 0..7 偏移。 */
	s8 shadow_value = *(s8 *)kasan_mem_to_shadow(addr);

	/* 正数表示仅前 shadow_value 字节合法，负毒码经有符号比较会判所有偏移非法。 */
	if (unlikely(shadow_value)) {
		s8 last_accessible_byte = (unsigned long)addr & KASAN_GRANULE_MASK;
		return unlikely(last_accessible_byte >= shadow_value);
	}

	return false;
}

/*
 * 业务背景：优化 2/4/8 字节访问，只在跨 granule 时检查两个 shadow 字节。
 * 入参：addr 为借用起点；size 仅允许 2、4、8 字节。
 * 出参/返回：范围任一字节 poisoned 返回 true；无状态副作用。
 * 注意事项：末字节检查同时解释部分可访问编码，不能只测试首 shadow 是否非零。
 */
static __always_inline bool memory_is_poisoned_2_4_8(const void *addr,
						unsigned long size)
{
	u8 *shadow_addr = (u8 *)kasan_mem_to_shadow(addr);

	/*
	 * Access crosses 8(shadow size)-byte boundary. Such access maps
	 * into 2 shadow bytes, so we need to check them both.
	 */
	/* 若末偏移小于 size-1，访问已绕过 8 字节边界，首尾两侧都必须验证。 */
	if (unlikely((((unsigned long)addr + size - 1) & KASAN_GRANULE_MASK) < size - 1))
		return *shadow_addr || memory_is_poisoned_1(addr + size - 1);

	return memory_is_poisoned_1(addr + size - 1);
}

/*
 * 业务背景：优化编译器常见的 16 字节访问，并区分对齐与非对齐映射跨度。
 * 入参：addr 为借用起点；出参/返回：任一覆盖字节 poisoned 返回 true。
 * 注意事项：对齐时正好覆盖两个 shadow 字节；非对齐时覆盖三个，末字节需精确判断。
 */
static __always_inline bool memory_is_poisoned_16(const void *addr)
{
	u16 *shadow_addr = (u16 *)kasan_mem_to_shadow(addr);

	/* Unaligned 16-bytes access maps into 3 shadow bytes. */
	/* 非对齐 16 字节横跨三个 granule：先批量查前两项，再精确查最后一字节。 */
	if (unlikely(!IS_ALIGNED((unsigned long)addr, KASAN_GRANULE_SIZE)))
		return *shadow_addr || memory_is_poisoned_1(addr + 15);

	return *shadow_addr;
}

/*
 * 业务背景：作为变长扫描的字节级尾部/命中定位器，返回首个非零 shadow 地址。
 * 入参：start 为借用 shadow 指针；size 为要扫描的 shadow 字节数。
 * 出参/返回：命中返回 shadow 地址数值，全零返回 0；无状态副作用。
 * 注意事项：返回的是 shadow 地址而非应用地址，调用者只用它判断和定位毒值。
 */
static __always_inline unsigned long bytes_is_nonzero(const u8 *start,
					size_t size)
{
	/* 短循环在发现首个毒值时立即返回，避免继续读取无关 shadow。 */
	while (size) {
		if (unlikely(*start))
			return (unsigned long)start;
		start++;
		size--;
	}

	return 0;
}

/*
 * 业务背景：高效扫描一段 shadow，前后按字节、主体按 64 位字批量检查。
 * 入参：start/end 为借用 shadow 半开区间，end 不小于 start。
 * 出参/返回：首个非零 shadow 字节地址，全部为零返回 0。
 * 注意事项：仅做读取；先对齐到 8 字节避免未对齐 u64 访问。
 */
static __always_inline unsigned long memory_is_nonzero(const void *start,
						const void *end)
{
	/* prefix 是到下一个 8 字节边界的前缀长度；words 是主体 u64 数。 */
	unsigned int words;
	unsigned long ret;
	unsigned int prefix = (unsigned long)start % 8;

	/* 小范围直接字节扫，省去对齐和分段成本。 */
	if (end - start <= 16)
		return bytes_is_nonzero(start, end - start);

	if (prefix) {
		prefix = 8 - prefix;
		ret = bytes_is_nonzero(start, prefix);
		if (unlikely(ret))
			return ret;
		start += prefix;
	}

	/* 对齐后以机器字查零；命中再回落字节扫描以返回精确位置。 */
	words = (end - start) / 8;
	while (words) {
		if (unlikely(*(u64 *)start))
			return bytes_is_nonzero(start, 8);
		start += 8;
		words--;
	}

	return bytes_is_nonzero(start, (end - start) % 8);
}

/*
 * 业务背景：检查任意长度访问的所有 shadow，并正确解释末 granule 的部分可访问值。
 * 入参：addr 为借用应用地址；size 为非零字节数。
 * 出参/返回：范围存在不可访问字节返回 true，否则 false。
 * 注意事项：中间 shadow 任一非零都非法；仅最后一项可能是合法的正部分值。
 */
static __always_inline bool memory_is_poisoned_n(const void *addr, size_t size)
{
	/* ret 是扫描到的首个非零 shadow 地址；其是否恰为末项决定能否部分合法。 */
	unsigned long ret;

	ret = memory_is_nonzero(kasan_mem_to_shadow(addr),
			kasan_mem_to_shadow(addr + size - 1) + 1);

	/* 非零项在末 shadow 之前必为 poison；末项还需比较最后访问字节偏移。 */
	if (unlikely(ret)) {
		const void *last_byte = addr + size - 1;
		s8 *last_shadow = (s8 *)kasan_mem_to_shadow(last_byte);
		s8 last_accessible_byte = (unsigned long)last_byte & KASAN_GRANULE_MASK;

		if (unlikely(ret != (unsigned long)last_shadow ||
			     last_accessible_byte >= *last_shadow))
			return true;
	}
	return false;
}

/*
 * 业务背景：按编译器是否能常量折叠 size，在固定宽度快速路径和任意长度扫描间分派。
 * 入参：addr 为借用应用地址；size 是访问字节数。
 * 出参/返回：检测到 poison 返回 true，否则 false；无报告副作用。
 * 注意事项：常量宽度只允许 1/2/4/8/16，其他常量通过 BUILD_BUG 阻止生成错误 ABI。
 */
static __always_inline bool memory_is_poisoned(const void *addr, size_t size)
{
	/* 每个 case 对应编译器 ASan 固定访问 ABI；共享分支保持相同跨粒度算法。 */
	if (__builtin_constant_p(size)) {
		switch (size) {
		case 1:
			return memory_is_poisoned_1(addr);
		case 2:
		case 4:
		case 8:
			/* 三种宽度共享“最多跨两个 granule”的专用检查。 */
			return memory_is_poisoned_2_4_8(addr, size);
		case 16:
			return memory_is_poisoned_16(addr);
		default:
			BUILD_BUG();
		}
	}

	return memory_is_poisoned_n(addr, size);
}

/*
 * 业务背景：所有 load/store 插桩的共同判定点，先验证范围形状与 metadata 再读取 shadow。
 * 入参：addr/size 描述借用访问范围；write 区分读写；ret_ip 是插桩调用点。
 * 出参/返回：合法或报告选择继续时返回 true；报告要求终止语义时返回 false。
 * 注意事项：零长度合法；显式捕获地址溢出和无 shadow 地址，kasan_report 负责递归抑制。
 */
static __always_inline bool check_region_inline(const void *addr,
						size_t size, bool write,
						unsigned long ret_ip)
{
	/* static key 未打开时完全绕过，避免启动早期 shadow 尚未就绪。 */
	if (!kasan_enabled())
		return true;

	if (unlikely(size == 0))
		return true;

	/* 指针加法回绕代表范围不可表示；无 metadata 也不能安全读取 shadow。 */
	if (unlikely(addr + size < addr))
		return !kasan_report(addr, size, write, ret_ip);

	if (unlikely(!addr_has_metadata(addr)))
		return !kasan_report(addr, size, write, ret_ip);

	/* 热路径在 shadow 全合法时直接返回，只有异常才进入昂贵报告组装。 */
	if (likely(!memory_is_poisoned(addr, size)))
		return true;

	return !kasan_report(addr, size, write, ret_ip);
}

/*
 * 业务背景：向内核其他代码暴露 Generic KASAN 的范围检查入口。
 * 入参：addr/size/write/ret_ip 完整传递访问语义，地址均为借用。
 * 出参/返回：透传 check_region_inline 的可继续结果；异常可能打印报告。
 * 注意事项：不取得目标内存引用，调用者仍须解决与并发释放的生命周期竞态。
 */
bool kasan_check_range(const void *addr, size_t size, bool write,
					unsigned long ret_ip)
{
	return check_region_inline(addr, size, write, ret_ip);
}

/*
 * 业务背景：释放校验只需判断一个地址当前 shadow 是否允许至少一个字节访问。
 * 入参：addr 为借用应用地址；出参/返回：KASAN 关闭或可访问返回 true，否则 false。
 * 注意事项：READ_ONCE 只保证单次 shadow 读取，不冻结对象；正值小于 granule 表示部分开放。
 */
bool kasan_byte_accessible(const void *addr)
{
	s8 shadow_byte;

	if (!kasan_enabled())
		return true;

	shadow_byte = READ_ONCE(*(s8 *)kasan_mem_to_shadow(addr));

	return shadow_byte >= 0 && shadow_byte < KASAN_GRANULE_SIZE;
}

/*
 * 业务背景：cache shrink 前排空属于该 cache 的 quarantine，避免被隔离对象阻止回收。
 * 入参：cache 为分配器稳定持有的借用对象；出参/返回：无，隔离对象回归 slab。
 * 注意事项：remove_cache 内部完成跨 CPU 同步，调用者遵守 slab shrink 锁序。
 */
void kasan_cache_shrink(struct kmem_cache *cache)
{
	kasan_quarantine_remove_cache(cache);
}

/*
 * 业务背景：cache shutdown 时只有非空 cache 才可能仍有需按 cache 过滤的隔离对象。
 * 入参：cache 为待销毁借用对象；出参/返回：无，必要时排空 quarantine。
 * 注意事项：检查与移除由 slab 销毁协议串行；函数不自行释放 cache 描述符。
 */
void kasan_cache_shutdown(struct kmem_cache *cache)
{
	if (!__kmem_cache_empty(cache))
		kasan_quarantine_remove_cache(cache);
}

/*
 * 业务背景：编译器登记全局对象时开放真实数据，并把其编译期预留尾部设为 global redzone。
 * 入参：global 为借用描述符，beg/size/size_with_redzone 由编译器生成。
 * 出参/返回：无；更新对应 shadow，不取得全局对象 ownership。
 * 注意事项：size 向 granule 上取整后才开始 poison，保留末粒度的合法数据编码。
 */
static void register_global(struct kasan_global *global)
{
	size_t aligned_size = round_up(global->size, KASAN_GRANULE_SIZE);

	kasan_unpoison(global->beg, global->size, false);

	kasan_poison(global->beg + aligned_size,
		     global->size_with_redzone - aligned_size,
		     KASAN_GLOBAL_REDZONE, false);
}

/*
 * 业务背景：模块/内核装载时接收编译器生成的全局对象描述符数组并逐个建立 shadow。
 * 入参：ptr 为借用 kasan_global 数组；size 是元素个数而非字节数。
 * 出参/返回：无；所有描述对象被开放且 redzone 被 poison。
 * 注意事项：调用时 shadow 必须已就绪；数组生命周期由编译器和模块装载器保证。
 */
void __asan_register_globals(void *ptr, ssize_t size)
{
	/* globals 是数组借用别名，i 逐项提交，不保留描述符指针。 */
	int i;
	struct kasan_global *globals = ptr;

	for (i = 0; i < size; i++)
		register_global(&globals[i]);
}
EXPORT_SYMBOL(__asan_register_globals);

/*
 * 业务背景：满足编译器 ASan 注销 ABI；内核全局 shadow 生命周期不在卸载时逐项恢复。
 * 入参：ptr/size 为未使用的描述符数组与元素数。
 * 出参/返回：无，当前实现不改变 shadow 或 ownership。
 * 注意事项：空实现是 ABI 桩，不能推断全局 redzone 已被 unpoison。
 */
void __asan_unregister_globals(void *ptr, ssize_t size)
{
}
EXPORT_SYMBOL(__asan_unregister_globals);

/*
 * 编译器固定宽度 ABI 生成器：每个 size 产生 load/store 报告入口及 noabort 别名；
 * addr 为借用访问起点，无直接返回值，检查结果只决定是否报告，访问本身由原指令执行。
 * _RET_IP_ 绑定真实插桩调用点；别名共享同一实现，避免两套语义漂移。
 */
#define DEFINE_ASAN_LOAD_STORE(size)					\
	void __asan_load##size(void *addr)				\
	{								\
		check_region_inline(addr, size, false, _RET_IP_);	\
	}								\
	/* load/noabort 共享实现，导出后继续生成同宽度 store。 */	\
	EXPORT_SYMBOL(__asan_load##size);				\
	__alias(__asan_load##size)					\
	void __asan_load##size##_noabort(void *);			\
	EXPORT_SYMBOL(__asan_load##size##_noabort);			\
	void __asan_store##size(void *addr)				\
	{								\
		check_region_inline(addr, size, true, _RET_IP_);	\
	}								\
	/* store/noabort 同样以别名维持完全一致的报告语义。 */	\
	EXPORT_SYMBOL(__asan_store##size);				\
	__alias(__asan_store##size)					\
	void __asan_store##size##_noabort(void *);			\
	EXPORT_SYMBOL(__asan_store##size##_noabort)

DEFINE_ASAN_LOAD_STORE(1);
DEFINE_ASAN_LOAD_STORE(2);
DEFINE_ASAN_LOAD_STORE(4);
DEFINE_ASAN_LOAD_STORE(8);
DEFINE_ASAN_LOAD_STORE(16);

/*
 * 业务背景：编译器对非常量长度读访问调用此 ABI，转入通用范围检查。
 * 入参：addr 为借用起点；size 为读字节数。
 * 出参/返回：无；非法范围产生报告，不改变目标 ownership。
 * 注意事项：noabort 符号是本实现别名；调用点通过 _RET_IP_ 记录。
 */
void __asan_loadN(void *addr, ssize_t size)
{
	kasan_check_range(addr, size, false, _RET_IP_);
}
EXPORT_SYMBOL(__asan_loadN);

__alias(__asan_loadN)
void __asan_loadN_noabort(void *, ssize_t);
EXPORT_SYMBOL(__asan_loadN_noabort);

/*
 * 业务背景：非常量长度写访问的编译器 ABI，与 loadN 共享检查器但标记 write=true。
 * 入参：addr 为借用起点；size 为写字节数。
 * 出参/返回：无；非法写产生报告；noabort 别名复用相同实现。
 * 注意事项：只检查即将发生的访问，不负责实际写入或目标生命周期。
 */
void __asan_storeN(void *addr, ssize_t size)
{
	kasan_check_range(addr, size, true, _RET_IP_);
}
EXPORT_SYMBOL(__asan_storeN);

__alias(__asan_storeN)
void __asan_storeN_noabort(void *, ssize_t);
EXPORT_SYMBOL(__asan_storeN_noabort);

/* to shut up compiler complaints */
/* 该空 ABI 仅满足编译器对 noreturn 路径的符号需求，避免链接/告警；无入参、返回或副作用。 */
/*
 * 业务背景：编译器在不返回控制流处可能发出该回调。
 * 入参：无；出参/返回：无；注意事项：当前内核无需额外清理 alloca shadow。
 */
void __asan_handle_no_return(void) {}
EXPORT_SYMBOL(__asan_handle_no_return);

/* Emitted by compiler to poison alloca()ed objects. */
/* 编译器为 alloca 对象发出此调用，建立左右 redzone 并仅开放对象实际尾字节。 */
/*
 * 业务背景：栈上动态对象创建后必须按编译器布局同步 Generic KASAN shadow。
 * 入参：addr 为借用且按 ALLOCA_REDZONE_SIZE 对齐的对象起点；size 为字节数。
 * 出参/返回：无；开放数据尾粒度并 poison 左右 redzone。
 * 注意事项：对齐错误 WARN；栈内存 ownership 不变，展开时由 allocas_unpoison 撤销。
 */
void __asan_alloca_poison(void *addr, ssize_t size)
{
	/* 三种尺寸分别界定数据取整、右 redzone padding 与末粒度实际字节。 */
	size_t rounded_up_size = round_up(size, KASAN_GRANULE_SIZE);
	size_t padding_size = round_up(size, KASAN_ALLOCA_REDZONE_SIZE) -
			rounded_up_size;
	size_t rounded_down_size = round_down(size, KASAN_GRANULE_SIZE);

	const void *left_redzone = (const void *)(addr -
			KASAN_ALLOCA_REDZONE_SIZE);
	const void *right_redzone = (const void *)(addr + rounded_up_size);

	/* 编译器应保证对齐；告警后仍按既定 ABI 布局更新 shadow。 */
	WARN_ON(!IS_ALIGNED((unsigned long)addr, KASAN_ALLOCA_REDZONE_SIZE));

	kasan_unpoison((const void *)(addr + rounded_down_size),
			size - rounded_down_size, false);
	/* 数据尾开放完成后，左右毒码分别标记越界方向，便于报告分类。 */
	kasan_poison(left_redzone, KASAN_ALLOCA_REDZONE_SIZE,
		     KASAN_ALLOCA_LEFT, false);
	kasan_poison(right_redzone, padding_size + KASAN_ALLOCA_REDZONE_SIZE,
		     KASAN_ALLOCA_RIGHT, false);
}
EXPORT_SYMBOL(__asan_alloca_poison);

/* Emitted by compiler to unpoison alloca()ed areas when the stack unwinds. */
/* 栈展开时编译器用该回调开放被丢弃的 alloca 区域，避免旧 redzone 污染复用栈。 */
/*
 * 业务背景：函数退出或异常展开后清除动态栈对象留下的 poison。
 * 入参：stack_top 为借用区间起点；stack_bottom 是末地址的整数表示。
 * 出参/返回：无；合法半开区间被 unpoison，空或反向区间保持不变。
 * 注意事项：不访问 current，不改变栈 ownership；边界由编译器 ABI 保证。
 */
void __asan_allocas_unpoison(void *stack_top, ssize_t stack_bottom)
{
	if (unlikely(!stack_top || stack_top > (void *)stack_bottom))
		return;

	kasan_unpoison(stack_top, (void *)stack_bottom - stack_top, false);
}
EXPORT_SYMBOL(__asan_allocas_unpoison);

/* Emitted by the compiler to [un]poison local variables. */
/* 编译器用这些 ABI 以指定毒码批量写局部变量 shadow；00 开放，其余码区分 redzone。 */
/*
 * 宏生成函数的 addr 为借用 shadow 地址、size 为 shadow 字节数；无返回值，直接写 metadata。
 * 调用者必须传 shadow 而非应用地址，生成符号必须与编译器约定完全一致。
 */
#define DEFINE_ASAN_SET_SHADOW(byte) \
	void __asan_set_shadow_##byte(const void *addr, ssize_t size)	\
	{								\
		/* byte 是编译器选择的毒码，size 已是 shadow 字节数。 */ \
		__memset((void *)addr, 0x##byte, size);			\
	}								\
	EXPORT_SYMBOL(__asan_set_shadow_##byte)

DEFINE_ASAN_SET_SHADOW(00);
DEFINE_ASAN_SET_SHADOW(f1);
DEFINE_ASAN_SET_SHADOW(f2);
DEFINE_ASAN_SET_SHADOW(f3);
DEFINE_ASAN_SET_SHADOW(f5);
DEFINE_ASAN_SET_SHADOW(f8);

/*
 * Adaptive redzone policy taken from the userspace AddressSanitizer runtime.
 * For larger allocations larger redzones are used.
 */
/*
 * 该策略取自用户态 ASan：对象越大，尾部 redzone 越大，以检测更远越界；分段阈值同时
 * 把对象加 redzone 控制在相邻尺寸级附近，平衡检测范围与 slab 内存开销。
 */
/*
 * 业务背景：为 slab 对象选择 16..2048 字节的目标 redzone。
 * 入参：object_size 为对象字节数；出参/返回：目标 redzone 字节数，无副作用。
 * 注意事项：只参与 cache 尺寸规划，最终总大小仍受 KMALLOC_MAX_SIZE 限制。
 */
static inline unsigned int optimal_redzone(unsigned int object_size)
{
	/* 阶梯阈值保证小对象低开销、大对象获得更宽的越界探测带。 */
	return
		object_size <= 64        - 16   ? 16 :
		object_size <= 128       - 32   ? 32 :
		object_size <= 512       - 64   ? 64 :
		object_size <= 4096      - 128  ? 128 :
		/* 超过页级对象后逐级扩大到 256/512/1024，最终封顶 2048。 */
		object_size <= (1 << 14) - 256  ? 256 :
		object_size <= (1 << 15) - 512  ? 512 :
		object_size <= (1 << 16) - 1024 ? 1024 : 2048;
}

/*
 * 业务背景：slab cache 建立时在对象或 redzone 中布局 alloc/free metadata，并扩充目标尺寸。
 * 入参：cache 为正在构造的输入输出 cache；size 为输入输出槽位字节数；flags 为输入输出标志。
 * 出参/返回：无；可能设置 SLAB_KASAN/NO_MERGE、offset 与更大 size，不取得 cache ownership。
 * 注意事项：布局必须避开 ctor、TYPESAFE_BY_RCU、orig_size 和 slub_debug 仍会访问的对象字节；
 * 超过 KMALLOC_MAX_SIZE 时逐级放弃 metadata，offset 哨兵决定后续功能是否可用。
 */
void kasan_cache_create(struct kmem_cache *cache, unsigned int *size,
			  slab_flags_t *flags)
{
	/* ok_size 保存每阶段可回滚尺寸；orig offset 用于 free-meta 失败时恢复 alloc-meta 布局。 */
	unsigned int ok_size;
	unsigned int optimal_size;
	unsigned int rem_free_meta_size;
	unsigned int orig_alloc_meta_offset;

	/* 不收集栈/隔离的配置无需增加对象开销。 */
	if (!kasan_requires_meta())
		return;

	/*
	 * SLAB_KASAN is used to mark caches that are sanitized by KASAN and
	 * that thus have per-object metadata. Currently, this flag is used in
	 * slab_ksize() to account for per-object metadata when calculating the
	 * size of the accessible memory within the object. Additionally, we use
	 * SLAB_NO_MERGE to prevent merging of caches with per-object metadata.
	 */
	/* metadata 改变 cache ABI，禁止与无 metadata 或不同 offset 的 cache 合并。 */
	*flags |= SLAB_KASAN | SLAB_NO_MERGE;

	ok_size = *size;

	/* Add alloc meta into the redzone. */
	/* alloc meta 首选紧随对象，成功后 size 成为下一布局阶段起点。 */
	cache->kasan_info.alloc_meta_offset = *size;
	*size += sizeof(struct kasan_alloc_meta);

	/* If alloc meta doesn't fit, don't add it. */
	/* 0 offset 同时表示 alloc metadata 不存在；仍继续尝试更关键的 free metadata。 */
	if (*size > KMALLOC_MAX_SIZE) {
		cache->kasan_info.alloc_meta_offset = 0;
		*size = ok_size;
		/* Continue, since free meta might still fit. */
		/* alloc meta 超限不代表 free meta 也放不下，因此继续下一阶段而非退出。 */
	}

	ok_size = *size;
	orig_alloc_meta_offset = cache->kasan_info.alloc_meta_offset;

	/*
	 * Store free meta in the redzone when it's not possible to store
	 * it in the object. This is the case when:
	 * 1. Object is SLAB_TYPESAFE_BY_RCU, which means that it can
	 *    be touched after it was freed, or
	 * 2. Object has a constructor, which means it's expected to
	 *    retain its content until the next allocation, or
	 * 3. It is from a kmalloc cache which enables the debug option
	 *    to store original size.
	 */
	/* RCU/ctor/orig-size 都不能覆盖对象内容，free meta 必须完全移到 redzone。 */
	if ((cache->flags & SLAB_TYPESAFE_BY_RCU) || cache->ctor ||
	     slub_debug_orig_size(cache)) {
		cache->kasan_info.free_meta_offset = *size;
		*size += sizeof(struct kasan_free_meta);
		goto free_meta_added;
	}

	/*
	 * Otherwise, if the object is large enough to contain free meta,
	 * store it within the object.
	 */
	/* 普通大对象释放后内容已无语义，可直接把 free meta 嵌入对象起始处。 */
	if (sizeof(struct kasan_free_meta) <= cache->object_size) {
		/* cache->kasan_info.free_meta_offset = 0 is implied. */
		/* offset 默认零即表示 free meta 从对象起点开始，无需显式赋值。 */
		goto free_meta_added;
	}

	/*
	 * For smaller objects, store the beginning of free meta within the
	 * object and the end in the redzone. And thus shift the location of
	 * alloc meta to free up space for free meta.
	 * This is only possible when slub_debug is disabled, as otherwise
	 * the end of free meta will overlap with slub_debug metadata.
	 */
	/* 小对象可把 free meta 前半放对象内、尾部放 redzone，并顺延 alloc meta。 */
	if (!__slub_debug_enabled()) {
		rem_free_meta_size = sizeof(struct kasan_free_meta) -
							cache->object_size;
		*size += rem_free_meta_size;
		if (cache->kasan_info.alloc_meta_offset != 0)
			cache->kasan_info.alloc_meta_offset += rem_free_meta_size;
		goto free_meta_added;
	}

	/*
	 * If the object is small and slub_debug is enabled, store free meta
	 * in the redzone after alloc meta.
	 */
	/* slub_debug 已占用边界空间时采用最保守布局：free meta 放在 alloc meta 之后。 */
	cache->kasan_info.free_meta_offset = *size;
	*size += sizeof(struct kasan_free_meta);

free_meta_added:
	/* If free meta doesn't fit, don't add it. */
	/* 失败以 KASAN_NO_FREE_META 明示，恢复到添加 free meta 前的 size 和 alloc offset。 */
	if (*size > KMALLOC_MAX_SIZE) {
		cache->kasan_info.free_meta_offset = KASAN_NO_FREE_META;
		cache->kasan_info.alloc_meta_offset = orig_alloc_meta_offset;
		*size = ok_size;
	}

	/* Calculate size with optimal redzone. */
	/* metadata 放得下后仍按对象尺度补足 redzone，最终不越过 kmalloc 上限。 */
	optimal_size = cache->object_size + optimal_redzone(cache->object_size);
	/* Limit it with KMALLOC_MAX_SIZE. */
	/* 上限保证扩充后的 cache 仍可由 kmalloc/slab 尺寸体系表示。 */
	if (optimal_size > KMALLOC_MAX_SIZE)
		optimal_size = KMALLOC_MAX_SIZE;
	/* Use optimal size if the size with added metas is not large enough. */
	/* metadata 占用不足以形成目标 redzone 时，再把槽位补大到 optimal_size。 */
	if (*size < optimal_size)
		*size = optimal_size;
}

/*
 * 业务背景：按 cache 创建时记录的 offset 定位对象 alloc track。
 * 入参：cache/object 为借用关系；出参/返回：存在则返回借用 metadata 指针，否则 NULL。
 * 注意事项：不验证对象生命周期、不取得引用；offset=0 是无 alloc meta 哨兵。
 */
struct kasan_alloc_meta *kasan_get_alloc_meta(struct kmem_cache *cache,
					      const void *object)
{
	if (!cache->kasan_info.alloc_meta_offset)
		return NULL;
	return (void *)object + cache->kasan_info.alloc_meta_offset;
}

/*
 * 业务背景：定位 quarantine 链和 free track 使用的 free metadata。
 * 入参：cache/object 为借用关系；出参/返回：可用则返回借用指针，否则 NULL。
 * 注意事项：编译期限制结构不超过 32 字节；offset=0 可表示嵌入对象起点，不能当作缺失。
 */
struct kasan_free_meta *kasan_get_free_meta(struct kmem_cache *cache,
					    const void *object)
{
	BUILD_BUG_ON(sizeof(struct kasan_free_meta) > 32);
	if (cache->kasan_info.free_meta_offset == KASAN_NO_FREE_META)
		return NULL;
	return (void *)object + cache->kasan_info.free_meta_offset;
}

/*
 * 业务背景：新建 slab 对象时把可选 alloc metadata 标为无效，防止继承旧页内容的栈句柄。
 * 入参：cache/object 为借用新对象；出参/返回：无，存在 alloc meta 时清零。
 * 注意事项：free meta 有效性由首 shadow 毒码表示，无需写结构本身；对象仍归 slab。
 */
void kasan_init_object_meta(struct kmem_cache *cache, const void *object)
{
	struct kasan_alloc_meta *alloc_meta;

	alloc_meta = kasan_get_alloc_meta(cache, object);
	if (alloc_meta) {
		/* Zero out alloc meta to mark it as invalid. */
		/* 全零同时清除旧 stack-depot handle、PID 及扩展信息。 */
		__memset(alloc_meta, 0, sizeof(*alloc_meta));
	}

	/*
	 * Explicitly marking free meta as invalid is not required: the shadow
	 * value for the first 8 bytes of a newly allocated object is not
	 * KASAN_SLAB_FREE_META.
	 */
	/* 新对象首 shadow 不会是 FREE_META，因此无需额外清 free-meta 内容。 */
}

/*
 * 业务背景：覆盖 alloc track 前先失效旧记录，避免 krealloc/mempool 遗留现场。
 * 入参：meta 为借用输出结构；出参/返回：无，完整清零。
 * 注意事项：stack depot 句柄由 depot 自身管理，这里只删除对象对它的引用。
 */
static void release_alloc_meta(struct kasan_alloc_meta *meta)
{
	/* Zero out alloc meta to mark it as invalid. */
	/* 全零是报告层识别“没有有效分配现场”的稳定哨兵。 */
	__memset(meta, 0, sizeof(*meta));
}

/*
 * 业务背景：重写 free track 前，仅在 shadow 明示 FREE_META 有效时撤销该状态。
 * 入参：object 为借用对象起点；meta 为借用 free metadata，当前仅表达关联。
 * 出参/返回：无；有效时把首 shadow 从 FREE_META 改为普通 SLAB_FREE。
 * 注意事项：不清结构内存，shadow 毒码才是有效性判据；无效时保持原状。
 */
static void release_free_meta(const void *object, struct kasan_free_meta *meta)
{
	/* Check if free meta is valid. */
	/* 只有 quarantine/free 路径发布过 FREE_META 才需要转换，避免覆盖其他毒码。 */
	if (*(u8 *)kasan_mem_to_shadow(object) != KASAN_SLAB_FREE_META)
		return;

	/* Mark free meta as invalid. */
	/* SLAB_FREE 仍保持对象不可访问，只撤销“metadata 可解释”这一附加状态。 */
	*(u8 *)kasan_mem_to_shadow(object) = KASAN_SLAB_FREE;
}

/*
 * 业务背景：slab 计算用户可访问大小时区分对象内与 redzone 中的 KASAN metadata 开销。
 * 入参：cache 为借用 cache；in_object 选择统计对象内还是对象外部分。
 * 出参/返回：返回对应 metadata 字节数；无需 metadata 时为 0，无状态副作用。
 * 注意事项：free offset 0 表示嵌入对象，alloc offset 0 却表示不存在，判据不同。
 */
size_t kasan_metadata_size(struct kmem_cache *cache, bool in_object)
{
	/* info 只借用 cache 内嵌布局结果，不延长 cache 生命周期。 */
	struct kasan_cache *info = &cache->kasan_info;

	if (!kasan_requires_meta())
		return 0;

	/* 对象内只可能容纳 free meta；对象外分别按两个 offset/哨兵累计。 */
	if (in_object)
		return (info->free_meta_offset ?
			0 : sizeof(struct kasan_free_meta));
	else
		return (info->alloc_meta_offset ?
			sizeof(struct kasan_alloc_meta) : 0) +
			((info->free_meta_offset &&
			info->free_meta_offset != KASAN_NO_FREE_META) ?
			sizeof(struct kasan_free_meta) : 0);
}

/*
 * This function avoids dynamic memory allocations and thus can be called from
 * contexts that do not allow allocating memory.
 */
/* 本函数避免动态分配，因此可在禁止分配的上下文记录最近两条辅助调用栈。 */
/*
 * 业务背景：为异步对象事件追加 aux stack，帮助报告还原非 alloc/free 的来源。
 * 入参：addr 为借用对象内地址；出参/返回：无，存在 alloc meta 时滚动两槽并保存栈。
 * 注意事项：KFENCE/非 slab/无 metadata 静默跳过；并发写同一对象需由调用者串行。
 */
void kasan_record_aux_stack(void *addr)
{
	/* slab/cache/object/alloc_meta 逐级把任意对象内地址归一到 metadata。 */
	struct slab *slab = kasan_addr_to_slab(addr);
	struct kmem_cache *cache;
	struct kasan_alloc_meta *alloc_meta;
	void *object;

	if (is_kfence_address(addr) || !slab)
		return;

	cache = slab->slab_cache;
	object = nearest_obj(cache, slab, addr);
	/* aux stack 属于对象 alloc metadata；没有该区域的 cache 不支持此诊断。 */
	alloc_meta = kasan_get_alloc_meta(cache, object);
	if (!alloc_meta)
		return;

	/* 旧最新记录移到槽 1，新记录写槽 0；flags=0 保证这里不动态分配。 */
	alloc_meta->aux_stack[1] = alloc_meta->aux_stack[0];
	alloc_meta->aux_stack[0] = kasan_save_stack(0, 0);
}

/*
 * 业务背景：分配、krealloc 或 mempool 复用时刷新对象唯一 alloc track。
 * 入参：cache/object 为借用对象；flags 控制 stack depot 的分配上下文。
 * 出参/返回：无；存在 metadata 时失效旧记录并保存当前现场。
 * 注意事项：不改变 shadow；调用者保证 metadata 与释放路径不并发改写。
 */
void kasan_save_alloc_info(struct kmem_cache *cache, void *object, gfp_t flags)
{
	struct kasan_alloc_meta *alloc_meta;

	alloc_meta = kasan_get_alloc_meta(cache, object);
	if (!alloc_meta)
		return;

	/* Invalidate previous stack traces (might exist for krealloc or mempool). */
	/* 先清旧 handle，再发布本轮分配现场，避免两次生命周期混合。 */
	release_alloc_meta(alloc_meta);

	kasan_save_track(&alloc_meta->alloc_track, flags);
}

/*
 * 业务背景：对象 poison 时保存释放现场，并发布 free metadata 可解释状态。
 * 入参：cache/object 为借用待释放对象；出参/返回：无，可能更新 free track 与 shadow。
 * 注意事项：无 free meta 静默跳过；先撤销旧 mempool 记录，再保存当前释放栈。
 */
void kasan_save_free_info(struct kmem_cache *cache, void *object)
{
	struct kasan_free_meta *free_meta;

	free_meta = kasan_get_free_meta(cache, object);
	if (!free_meta)
		return;

	/* Invalidate previous stack trace (might exist for mempool). */
	/* 旧 free track 可能属于上一次池内归还，必须先撤销其有效毒码。 */
	release_free_meta(object, free_meta);

	kasan_save_track(&free_meta->free_track, 0);

	/* Mark free meta as valid. */
	/* 最后发布 FREE_META 毒码，报告层看到它时前面的 track 已完整写入。 */
	*(u8 *)kasan_mem_to_shadow(object) = KASAN_SLAB_FREE_META;
}
