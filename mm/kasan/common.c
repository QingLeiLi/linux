// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains common KASAN code.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * Some code borrowed from https://github.com/xairy/kasan-prototype by
 *        Andrey Konovalov <andreyknvl@gmail.com>
 */
/*
 * 本文件汇集各种 KASAN 模式共享的分配/释放插桩逻辑；版权、作者和借用来源保持原样。
 * 它把 page allocator、slab、mempool、vmalloc 的生命周期事件转换为 shadow/tag 状态，
 * 并在非法释放或访问发生时把稳定的地址、栈和任务上下文交给报告层。
 */

#include <linux/export.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
/* 调度/时钟/栈接口为 alloc/free track 提供 PID、CPU、时间戳和调用链。 */
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
/* 字符串、类型、告警和 vmalloc 接口支撑跨分配器的范围检查与诊断。 */
#include <linux/string.h>
#include <linux/types.h>
#include <linux/bug.h>
#include <linux/vmalloc.h>

#include "kasan.h"
#include "../slab.h"

/* 主线：分配时选 tag 并 unpoison 可用区，释放时校验后 poison；隔离区延后真正复用。 */

#if defined(CONFIG_ARCH_DEFER_KASAN) || defined(CONFIG_KASAN_HW_TAGS)
/*
 * Definition of the unified static key declared in kasan-enabled.h.
 * This provides consistent runtime enable/disable across KASAN modes.
 */
/*
 * 这是 kasan-enabled.h 所声明统一 static key 的唯一定义，使所有 KASAN 模式共享运行期
 * 启停状态。初值 false 避免未完成初始化时进入检测，架构初始化发布后再打开快速分支。
 */
DEFINE_STATIC_KEY_FALSE(kasan_flag_enabled);
EXPORT_SYMBOL_GPL(kasan_flag_enabled);
#endif

/*
 * 业务背景：报告和释放校验需要从任意线性映射地址定位其所属 slab。
 * 入参：addr 为借用地址，可带任意对象内偏移；不取得 page/slab 引用。
 * 出参/返回：有效线性映射返回所属 slab，否则 NULL；无状态副作用。
 * 注意事项：只做地址类别转换，不延长 slab 生命周期，调用者需处在分配器保护窗口内。
 */
struct slab *kasan_addr_to_slab(const void *addr)
{
	if (virt_addr_valid(addr))
		return virt_to_slab(addr);
	return NULL;
}

/*
 * 业务背景：为分配/释放轨迹抓取当前调用栈并存入 stack depot，供之后报告去重和展开。
 * 入参：flags 控制 depot 内存分配上下文；depot_flags 控制保存策略。
 * 出参/返回：返回 depot handle，失败可为无效 handle；entries 只作栈上临时输入。
 * 注意事项：能否睡眠由 flags 决定；depot 保存的是副本，返回后局部数组失效。
 */
depot_stack_handle_t kasan_save_stack(gfp_t flags, depot_flags_t depot_flags)
{
	/* entries 保存最多 KASAN_STACK_DEPTH 个返回地址，nr_entries 是实际帧数。 */
	unsigned long entries[KASAN_STACK_DEPTH];
	unsigned int nr_entries;

	nr_entries = stack_trace_save(entries, ARRAY_SIZE(entries), 0);
	return stack_depot_save_flags(entries, nr_entries, flags, depot_flags);
}

/*
 * 业务背景：把已保存栈和当前执行上下文提交到对象 track，形成报告可读的事件快照。
 * 入参：track 为调用者拥有的输出结构；stack 是 stack depot 句柄。
 * 出参/返回：无直接返回值；写 pid/stack，EXTRA_INFO 下还写 CPU 与压缩时间戳。
 * 注意事项：raw CPU id 仅是采样值，不提供抢占稳定性；track 生命周期由对象 metadata 管理。
 */
void kasan_set_track(struct kasan_track *track, depot_stack_handle_t stack)
{
#ifdef CONFIG_KASAN_EXTRA_INFO
	/* cpu/ts_nsec 只在本次事件瞬间有效，时间戳右移 9 位以压缩 metadata。 */
	u32 cpu = raw_smp_processor_id();
	u64 ts_nsec = local_clock();

	track->cpu = cpu;
	track->timestamp = ts_nsec >> 9;
#endif /* CONFIG_KASAN_EXTRA_INFO */
	/* pid 与 stack 最后写入同一 track；调用者负责避免并发读到半更新快照。 */
	track->pid = current->pid;
	track->stack = stack;
}

/*
 * 业务背景：常用包装一次完成堆栈保存与 track 字段提交。
 * 入参：track 为输出 metadata；flags 描述当前分配上下文及可否分配 depot 空间。
 * 出参/返回：无；更新 track，stack depot 可能新增记录。
 * 注意事项：STACK_DEPOT_FLAG_CAN_ALLOC 仍受 flags 限制，失败 handle 也按契约写入。
 */
void kasan_save_track(struct kasan_track *track, gfp_t flags)
{
	depot_stack_handle_t stack;

	stack = kasan_save_stack(flags, STACK_DEPOT_FLAG_CAN_ALLOC);
	kasan_set_track(track, stack);
}

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
/*
 * 业务背景：与 disable 配对，结束当前任务的 KASAN 抑制区，使无符号计数最终回到 0。
 * 入参：无；出参/返回：无，递增 current->kasan_depth；回到 0 后报告重新启用。
 * 注意事项：仅修改当前任务字段、不可跨任务配对；加减不平衡会持续抑制或错误启用报告。
 */
void kasan_enable_current(void)
{
	current->kasan_depth++;
}
EXPORT_SYMBOL(kasan_enable_current);

/*
 * 业务背景：进入会递归触发插桩的内部路径前，暂时抑制当前任务的 KASAN 报告。
 * 入参：无；出参/返回：无，递减无符号 current->kasan_depth；从 0 下溢后变为非零。
 * 注意事项：report.c 把任何非零值视为禁用；嵌套调用继续递减，必须逐层 enable 加回 0。
 */
void kasan_disable_current(void)
{
	current->kasan_depth--;
}
EXPORT_SYMBOL(kasan_disable_current);

#endif /* CONFIG_KASAN_GENERIC || CONFIG_KASAN_SW_TAGS */

/*
 * 业务背景：让一段新获得内存对 KASAN 可访问，同时把 KFENCE 管理的地址留给 KFENCE。
 * 入参：address 为借用起点，size 为字节数；不转移底层内存所有权。
 * 出参/返回：无；非 KFENCE 范围清除对应毒值/tag，KFENCE 地址保持不变。
 * 注意事项：调用者保证范围有效并与分配生命周期同步；false 表示非初始化语义。
 */
void __kasan_unpoison_range(const void *address, size_t size)
{
	if (is_kfence_address(address))
		return;

	kasan_unpoison(address, size, false);
}

#ifdef CONFIG_KASAN_STACK
/* Unpoison the entire stack for a task. */
/*
 * 为任务的整段 THREAD_SIZE 栈清除 poison，通常用于新任务或复用栈准备；task 是借用且
 * 必须仍持有有效栈。无直接返回值，副作用是整栈可访问，函数本身不取得 task 引用。
 */
/*
 * 业务背景：任务栈投入执行前必须让插桩访问合法。
 * 入参：task 为借用且持有有效内核栈；出参/返回：无，整栈 unpoison。
 * 注意事项：调用者稳定任务和栈生命周期；本函数不分配、不取得引用。
 */
void kasan_unpoison_task_stack(struct task_struct *task)
{
	void *base = task_stack_page(task);

	kasan_unpoison(base, THREAD_SIZE, false);
}

/* Unpoison the stack for the current task beyond a watermark sp value. */
/*
 * 业务背景：早期 resume 尚不能使用 current 时，按栈指针水位恢复当前栈已用部分。
 * 入参：watermark 为借用栈内地址，代表已用区间上界。
 * 出参/返回：无；从 THREAD_SIZE 对齐的栈基址到 watermark 清除 poison。
 * 注意事项：不得读取 current，因为此时 percpu %gs 可能未建立；错误水位会算出错误栈基址。
 */
asmlinkage void kasan_unpoison_task_stack_below(const void *watermark)
{
	/*
	 * Calculate the task stack base address.  Avoid using 'current'
	 * because this function is called by early resume code which hasn't
	 * yet set up the percpu register (%gs).
	 */
	/*
	 * 通过 watermark 向下按 THREAD_SIZE 对齐计算栈基址，刻意避开尚未设置 percpu
	 * 寄存器的 early resume 环境；随后只恢复 base 到当前水位，不开放未使用栈段。
	 */
	void *base = (void *)((unsigned long)watermark & ~(THREAD_SIZE - 1));

	kasan_unpoison(base, watermark - base, false);
}
#endif /* CONFIG_KASAN_STACK */

/*
 * 业务背景：伙伴分配器交付页块时为其选择 tag、解除 poison，并把 tag 写到每个 page。
 * 入参：page 为订单块首页借用指针；order 决定 2^order 页；init 表示初始化阶段语义。
 * 出参/返回：完成 KASAN 跟踪返回 true；highmem 或抽样跳过返回 false。
 * 注意事项：调用者仍拥有页块；page_address 必须可用，逐页 tag 与地址 tag 必须一致。
 */
bool __kasan_unpoison_pages(struct page *page, unsigned int order, bool init)
{
	/* tag 是本次页块统一标签；i 遍历所有 base page 写入页级元数据。 */
	u8 tag;
	unsigned long i;

	/* highmem 没有稳定线性地址；抽样模式也可能有意不跟踪本次分配。 */
	if (unlikely(PageHighMem(page)))
		return false;

	if (!kasan_sample_page_alloc(order))
		return false;

	/* 先开放整块 shadow，再发布每页 tag，使后续释放能识别是否被抽样。 */
	tag = kasan_random_tag();
	kasan_unpoison(set_tag(page_address(page), tag),
		       PAGE_SIZE << order, init);
	for (i = 0; i < (1 << order); i++)
		page_kasan_tag_set(page + i, tag);

	return true;
}

/*
 * 业务背景：页块归还伙伴系统前将其标为 PAGE_FREE，捕获释放后访问。
 * 入参：page/order 描述借用页块；init 传给底层 poison 表示初始化语义。
 * 出参/返回：无；低端页 shadow 被 poison，highmem 因无直接映射而跳过。
 * 注意事项：不释放页面，只更新 KASAN 元数据；真正回收仍由 page allocator 完成。
 */
void __kasan_poison_pages(struct page *page, unsigned int order, bool init)
{
	if (likely(!PageHighMem(page)))
		kasan_poison(page_address(page), PAGE_SIZE << order,
			     KASAN_PAGE_FREE, init);
}

/*
 * 业务背景：slab 新建时重置组成页的 tag，并把尚未分配的整块标为 slab redzone。
 * 入参：slab 为分配器稳定持有的借用对象。
 * 出参/返回：无；更新所有组成 page tag 与整 slab shadow，不改变 slab ownership。
 * 注意事项：调用者保证可通过线性映射访问；初始化路径不记录单对象释放栈。
 */
void __kasan_poison_slab(struct slab *slab)
{
	/* page 是 slab 首页；i 覆盖 compound_nr() 个组成页，确保旧页 tag 不泄漏。 */
	struct page *page = slab_page(slab);
	unsigned long i;

	for (i = 0; i < compound_nr(page); i++)
		page_kasan_tag_reset(page + i);
	kasan_poison(page_address(page), page_size(page),
		     KASAN_SLAB_REDZONE, false);
}

/*
 * 业务背景：构造函数或 slab 初始化需要临时访问一个新对象的有效数据区。
 * 入参：cache 借用且给出 object_size；object 为借用对象起点。
 * 出参/返回：无；仅开放对象有效字节，不转移对象所有权。
 * 注意事项：redzone 是否精确收紧由后续 kmalloc 路径完成。
 */
void __kasan_unpoison_new_object(struct kmem_cache *cache, void *object)
{
	kasan_unpoison(object, cache->object_size, false);
}

/*
 * 业务背景：构造阶段结束后重新封闭对象区域，直到它真正分配给调用者。
 * 入参：cache/object 均为 slab 路径借用；大小向 KASAN granule 上取整。
 * 出参/返回：无；写 KASAN_SLAB_REDZONE 毒值，不释放对象。
 * 注意事项：与 unpoison_new_object 配对，调用者必须保证对象仍属于该 cache。
 */
void __kasan_poison_new_object(struct kmem_cache *cache, void *object)
{
	kasan_poison(object, round_up(cache->object_size, KASAN_GRANULE_SIZE),
			KASAN_SLAB_REDZONE, false);
}

/*
 * This function assigns a tag to an object considering the following:
 * 1. A cache might have a constructor, which might save a pointer to a slab
 *    object somewhere (e.g. in the object itself). We preassign a tag for
 *    each object in caches with constructors during slab creation and reuse
 *    the same tag each time a particular object is allocated.
 * 2. A cache might be SLAB_TYPESAFE_BY_RCU, which means objects can be
 *    accessed after being freed. We preassign tags for objects in these
 *    caches as well.
 */
/*
 * tag 分配必须兼顾两类会跨越普通 alloc/free 边界持有裸指针的 cache：构造函数可能把
 * 对象自身指针永久写入对象，SLAB_TYPESAFE_BY_RCU 又允许释放后读者继续观察内存；因此
 * 两者在 slab 创建时预分配并复用 tag。普通 cache 则每次真实分配生成新 tag，以提高
 * use-after-free 被地址/tag 不匹配捕获的概率。
 */
/*
 * 业务背景：为 slab 对象选择本次可见 tag，同时保持 ctor/RCU cache 的裸指针稳定性。
 * 入参：cache/object 为借用对象关系；init=true 表示 slab 创建，false 表示用户分配。
 * 出参/返回：generic 模式恒 0xff；tag 模式返回内核默认、随机或对象已有 tag；无分配。
 * 注意事项：返回值尚未写入指针，调用者必须用 set_tag 发布；RCU 类型安全不等于内容稳定。
 */
static inline u8 assign_tag(struct kmem_cache *cache,
					const void *object, bool init)
{
	/* generic shadow 不比较指针 tag，用固定值避免无意义随机开销。 */
	if (IS_ENABLED(CONFIG_KASAN_GENERIC))
		return 0xff;

	/*
	 * If the cache neither has a constructor nor has SLAB_TYPESAFE_BY_RCU
	 * set, assign a tag when the object is being allocated (init == false).
	 */
	/* 普通对象仅在真正 alloc 时随机化；创建期使用内核默认 tag。 */
	if (!cache->ctor && !(cache->flags & SLAB_TYPESAFE_BY_RCU))
		return init ? KASAN_TAG_KERNEL : kasan_random_tag();

	/*
	 * For caches that either have a constructor or SLAB_TYPESAFE_BY_RCU,
	 * assign a random tag during slab creation, otherwise reuse
	 * the already assigned tag.
	 */
	/* ctor/RCU cache 创建期写随机 tag，之后从对象指针取回并保持不变。 */
	return init ? kasan_random_tag() : get_tag(object);
}

/*
 * 业务背景：slab 创建对象时初始化可选 metadata，并预置后续分配要使用的 tag。
 * 入参：cache 是借用 cache；object 是新对象的借用未标记地址。
 * 出参/返回：返回可能带 tag 的同一对象指针，调用者必须使用返回值；ownership 不变。
 * 注意事项：__must_check 防止丢失 tag；metadata 生命周期与对象槽位一致。
 */
void * __must_check __kasan_init_slab_obj(struct kmem_cache *cache,
						const void *object)
{
	/* Initialize per-object metadata if it is present. */
	/* cache 模式需要 alloc/free track 时先建立对象 metadata，再生成预分配 tag。 */
	if (kasan_requires_meta())
		kasan_init_object_meta(cache, object);

	/* Tag is ignored in set_tag() without CONFIG_KASAN_SW/HW_TAGS */
	/* 非 tag 模式 set_tag() 退化为原地址；tag 模式返回值才是后续应保存的指针。 */
	object = set_tag(object, assign_tag(cache, object, true));

	return (void *)object;
}

/* Returns true when freeing the object is not safe. */
/*
 * 业务背景：slab 真正释放前验证指针位于对象起点且仍可访问，区分非法释放与双重释放。
 * 入参：cache/object 为借用分配关系，object 可带 tag；ip 是释放调用点地址。
 * 出参/返回：不安全且已报告返回 true，合法返回 false；不改变 ownership。
 * 注意事项：reset_tag 只供地址算术；报告必须使用原 tagged_object 才能诊断 tag mismatch。
 */
static bool check_slab_allocation(struct kmem_cache *cache, void *object,
				  unsigned long ip)
{
	/* tagged_object 保留调用者看到的 tag，object 则转为分配器可比较的裸地址。 */
	void *tagged_object = object;

	object = kasan_reset_tag(object);

	/* 非对象首地址不能交给 slab，否则 freelist 会被破坏。 */
	if (unlikely(nearest_obj(cache, virt_to_slab(object), object) != object)) {
		kasan_report_invalid_free(tagged_object, ip, KASAN_REPORT_INVALID_FREE);
		return true;
	}

	/* 首字节已 poison 或 tag 不匹配表示对象已经释放。 */
	if (!kasan_byte_accessible(tagged_object)) {
		kasan_report_invalid_free(tagged_object, ip, KASAN_REPORT_DOUBLE_FREE);
		return true;
	}

	return false;
}

/*
 * 业务背景：合法对象离开已分配状态时封闭整个槽位，并可保存释放栈供 UAF 报告。
 * 入参：cache/object 为借用对象；init 传递初始化语义。
 * 出参/返回：无；写 SLAB_FREE poison 和可选 free track，真正 ownership 尚未交回 freelist。
 * 注意事项：底层 shadow 使用去 tag 地址，track 则保留 tagged_object 关联。
 */
static inline void poison_slab_object(struct kmem_cache *cache, void *object,
				      bool init)
{
	void *tagged_object = object;

	object = kasan_reset_tag(object);

	/* 先 poison 再记释放栈，之后任何插桩访问都应被判为 UAF。 */
	kasan_poison(object, round_up(cache->object_size, KASAN_GRANULE_SIZE),
			KASAN_SLAB_FREE, init);

	if (kasan_stack_collection_enabled())
		kasan_save_free_info(cache, tagged_object);
}

/*
 * 业务背景：slab 释放的只校验入口，供需要把检查和实际 poison 分开的调用者使用。
 * 入参：cache/object/ip 含义同 check_slab_allocation，均为借用信息。
 * 出参/返回：KFENCE 对象返回 false 交给 KFENCE；其余返回 true 表示释放不安全。
 * 注意事项：本函数不 poison、不入 quarantine；合法 false 后调用者才可继续释放协议。
 */
bool __kasan_slab_pre_free(struct kmem_cache *cache, void *object,
				unsigned long ip)
{
	if (is_kfence_address(object))
		return false;
	return check_slab_allocation(cache, object, ip);
}

/*
 * 业务背景：slab 释放提交点；根据 RCU 可访问性和 quarantine 策略决定是否暂缓回 freelist。
 * 入参：cache/object 为借用待释放对象；init 为初始化语义；still_accessible 表示 RCU 读者
 * 仍可访问；no_quarantine 强制跳过隔离。
 * 出参/返回：true 表示 KASAN 接管对象进 quarantine、slab 暂不能复用；false 表示 slab 可继续。
 * 注意事项：调用前应已完成合法性校验；函数会 poison/保存 metadata，但不自行释放槽位。
 */
bool __kasan_slab_free(struct kmem_cache *cache, void *object, bool init,
		       bool still_accessible, bool no_quarantine)
{
	/* KFENCE 与 KASAN 各自维护元数据，避免双重接管。 */
	if (is_kfence_address(object))
		return false;

	/*
	 * If this point is reached with an object that must still be
	 * accessible under RCU, we can't poison it; in that case, also skip the
	 * quarantine. This should mostly only happen when CONFIG_SLUB_RCU_DEBUG
	 * has been disabled manually.
	 *
	 * Putting the object on the quarantine wouldn't help catch UAFs (since
	 * we can't poison it here), and it would mask bugs caused by
	 * SLAB_TYPESAFE_BY_RCU users not being careful enough about object
	 * reuse; so overall, putting the object into the quarantine here would
	 * be counterproductive.
	 */
	/* 仍受 RCU 裸读保护时不能 poison，也不能用 quarantine 掩盖错误复用协议。 */
	if (still_accessible)
		return false;

	/* 从这里起对象对 KASAN 不可访问，但物理槽位尚未决定由谁暂存。 */
	poison_slab_object(cache, object, init);

	if (no_quarantine)
		return false;

	/*
	 * If the object is put into quarantine, do not let slab put the object
	 * onto the freelist for now. The object's metadata is kept until the
	 * object gets evicted from quarantine.
	 */
	/* quarantine 成功即转移暂存责任，返回 true 阻止 slab 发布到 freelist。 */
	if (kasan_quarantine_put(cache, object))
		return true;

	/*
	 * Note: Keep per-object metadata to allow KASAN print stack traces for
	 * use-after-free-before-realloc bugs.
	 */
	/* 即便不隔离也保留对象 metadata，使再次分配前发生的 UAF 能输出释放栈。 */

	/* Let slab put the object onto the freelist. */
	/* false 明确把槽位回收责任交还 slab；KASAN 不再持有对象。 */
	return false;
}

/*
 * 业务背景：大块页分配释放前验证指针必须是 compound head 的线性映射起点且仍可访问。
 * 入参：ptr 为待释放借用地址，可带错误偏移；ip 是调用点。
 * 出参/返回：已报告非法/双重释放返回 true，合法返回 false。
 * 注意事项：不 poison 或释放页；virt_to_head_page 要求调用者传入有效线性映射地址。
 */
static inline bool check_page_allocation(void *ptr, unsigned long ip)
{
	/* 中间地址若进入 buddy 会破坏页块边界，因此先按 head 起点严格比较。 */
	if (ptr != page_address(virt_to_head_page(ptr))) {
		kasan_report_invalid_free(ptr, ip, KASAN_REPORT_INVALID_FREE);
		return true;
	}

	if (!kasan_byte_accessible(ptr)) {
		kasan_report_invalid_free(ptr, ip, KASAN_REPORT_DOUBLE_FREE);
		return true;
	}

	return false;
}

/*
 * 业务背景：kfree() 处理大块页对象时执行释放合法性诊断，poison 留给统一页释放钩子。
 * 入参：ptr 为待释放借用指针；ip 为释放调用点。
 * 出参/返回：无；非法情形产生报告，页状态和 ownership 不在此函数改变。
 * 注意事项：即使检查失败也无返回反馈，调用者的后续释放策略由上层契约决定。
 */
void __kasan_kfree_large(void *ptr, unsigned long ip)
{
	check_page_allocation(ptr, ip);

	/* The object will be poisoned by kasan_poison_pages(). */
	/* 此处仅校验；伙伴释放路径稍后调用 kasan_poison_pages() 统一封闭整页块。 */
}

/*
 * 业务背景：slab 分配成功后开放整个对象，并为非 kmalloc cache 保存分配轨迹。
 * 入参：cache/object 为借用对象；flags 控制栈存储分配；init 表示初始化阶段。
 * 出参/返回：无；更新 shadow 与可选 alloc metadata，ownership 仍由 slab 分配路径持有。
 * 注意事项：kmalloc 的精确请求边界稍后由 poison_kmalloc_redzone() 收紧。
 */
static inline void unpoison_slab_object(struct kmem_cache *cache, void *object,
					gfp_t flags, bool init)
{
	/*
	 * Unpoison the whole object. For kmalloc() allocations,
	 * poison_kmalloc_redzone() will do precise poisoning.
	 */
	/* 先开放完整 object_size，保证构造/返回前的分配器内部初始化合法。 */
	kasan_unpoison(object, cache->object_size, init);

	/* Save alloc info (if possible) for non-kmalloc() allocations. */
	/* kmalloc 要等知道请求 size 后记录；普通 cache 此处即可固定 alloc 现场。 */
	if (kasan_stack_collection_enabled() && !is_kmalloc_cache(cache))
		kasan_save_alloc_info(cache, object, flags);
}

/*
 * 业务背景：slab allocator 交付对象前减少 quarantine、选择 tag、开放对象并记录分配。
 * 入参：cache/object 为借用分配结果；flags 为 GFP 上下文；init 表示 cache 初始化分配。
 * 出参/返回：NULL 原样返回；KFENCE 原样返回；否则返回带正确 tag 的对象，ownership 仍归调用者。
 * 注意事项：可阻塞 flags 下 quarantine_reduce 可能做回收；__must_check 要求使用 tagged 返回值。
 */
void * __must_check __kasan_slab_alloc(struct kmem_cache *cache,
					void *object, gfp_t flags, bool init)
{
	/* tag 是新/复用标签；tagged_object 是最终向分配者发布的指针形式。 */
	u8 tag;
	void *tagged_object;

	/* 只有允许阻塞的上下文才主动缩减隔离区，避免原子路径睡眠。 */
	if (gfpflags_allow_blocking(flags))
		kasan_quarantine_reduce();

	if (unlikely(object == NULL))
		return NULL;

	if (is_kfence_address(object))
		return (void *)object;

	/*
	 * Generate and assign random tag for tag-based modes.
	 * Tag is ignored in set_tag() for the generic mode.
	 */
	/* tag 模式在此形成地址/tag 组合；generic 模式保持普通内核地址。 */
	tag = assign_tag(cache, object, false);
	tagged_object = set_tag(object, tag);

	/* Unpoison the object and save alloc info for non-kmalloc() allocations. */
	/* unpoison 完成可访问性发布，返回后上层才可把对象交给用户代码。 */
	unpoison_slab_object(cache, tagged_object, flags, init);

	return tagged_object;
}

/*
 * 业务背景：kmalloc 槽位通常大于请求 size，本函数把多余尾部重新 poison 成 redzone。
 * 入参：cache/object 为借用分配对象；size 为请求字节数；flags 用于保存 alloc 栈。
 * 出参/返回：无；更新尾粒度、对齐 redzone 和 kmalloc metadata，ownership 不变。
 * 注意事项：generic 支持末粒度字节精度；调用前对象已整体开放。
 */
static inline void poison_kmalloc_redzone(struct kmem_cache *cache,
				const void *object, size_t size, gfp_t flags)
{
	/* redzone_start/end 描述请求尾后到 cache 槽尾的对齐 shadow 区间。 */
	unsigned long redzone_start;
	unsigned long redzone_end;

	/*
	 * The redzone has byte-level precision for the generic mode.
	 * Partially poison the last object granule to cover the unaligned
	 * part of the redzone.
	 */
	/* generic 先编码最后一个不完整 granule，避免遗漏请求尾部字节。 */
	if (IS_ENABLED(CONFIG_KASAN_GENERIC))
		kasan_poison_last_granule((void *)object, size);

	/* Poison the aligned part of the redzone. */
	/* 再封闭完整 granule；零长度范围由底层 poison helper 安全处理。 */
	redzone_start = round_up((unsigned long)(object + size),
				KASAN_GRANULE_SIZE);
	redzone_end = round_up((unsigned long)(object + cache->object_size),
				KASAN_GRANULE_SIZE);
	kasan_poison((void *)redzone_start, redzone_end - redzone_start,
			   KASAN_SLAB_REDZONE, false);

	/*
	 * Save alloc info (if possible) for kmalloc() allocations.
	 * This also rewrites the alloc info when called from kasan_krealloc().
	 */
	/* krealloc 也复用本函数，因此以最新调用现场覆盖旧 alloc track。 */
	if (kasan_stack_collection_enabled() && is_kmalloc_cache(cache))
		kasan_save_alloc_info(cache, (void *)object, flags);

}

/*
 * 业务背景：kmalloc slab 路径在通用 slab alloc 后按真实请求尺寸收紧可访问区。
 * 入参：cache/object 为借用结果；size 为请求字节数；flags 为分配上下文。
 * 出参/返回：NULL/KFENCE 原样返回，否则返回已有 tag 的同一对象；ownership 不变。
 * 注意事项：对象已由 kasan_slab_alloc() 开放，必须保留其 tag。
 */
void * __must_check __kasan_kmalloc(struct kmem_cache *cache, const void *object,
					size_t size, gfp_t flags)
{
	/* 可睡眠分配顺便推动 quarantine，给长期隔离对象提供有界回收机会。 */
	if (gfpflags_allow_blocking(flags))
		kasan_quarantine_reduce();

	if (unlikely(object == NULL))
		return NULL;

	if (is_kfence_address(object))
		return (void *)object;

	/* The object has already been unpoisoned by kasan_slab_alloc(). */
	/* 这里只重建真实 size 之后的 redzone，不重复开放或更换对象 tag。 */
	poison_kmalloc_redzone(cache, object, size, flags);

	/* Keep the tag that was set by kasan_slab_alloc(). */
	/* 返回原 tagged 指针是 tag-based KASAN 正确性的组成部分。 */
	return (void *)object;
}
EXPORT_SYMBOL(__kasan_kmalloc);

/*
 * 业务背景：大于 slab cache 的 kmalloc 使用整页块，把请求尾到页块末端设为 redzone。
 * 入参：ptr 为借用页块起点；size 为请求字节数；flags 保持钩子接口一致。
 * 出参/返回：无；更新末粒度和页块尾 shadow，不改变页块 ownership/tag。
 * 注意事项：ptr 必须是 large kmalloc head；容量从 compound page 元数据取得。
 */
static inline void poison_kmalloc_large_redzone(const void *ptr, size_t size,
						gfp_t flags)
{
	/* redzone_start/end 覆盖请求对齐尾到 compound allocation 容量末端。 */
	unsigned long redzone_start;
	unsigned long redzone_end;

	/*
	 * The redzone has byte-level precision for the generic mode.
	 * Partially poison the last object granule to cover the unaligned
	 * part of the redzone.
	 */
	/* generic 先精确处理非对齐请求尾，随后统一 poison 完整 granule。 */
	if (IS_ENABLED(CONFIG_KASAN_GENERIC))
		kasan_poison_last_granule(ptr, size);

	/* Poison the aligned part of the redzone. */
	/* 从对齐后的请求尾一直封闭到 compound 页块末端，形成页级 redzone。 */
	redzone_start = round_up((unsigned long)(ptr + size), KASAN_GRANULE_SIZE);
	redzone_end = (unsigned long)ptr + page_size(virt_to_page(ptr));
	kasan_poison((void *)redzone_start, redzone_end - redzone_start,
		     KASAN_PAGE_REDZONE, false);
}

/*
 * 业务背景：large kmalloc 完成后收紧页块可访问范围并保留页分配阶段选择的 tag。
 * 入参：ptr 为借用结果；size 为请求字节数；flags 为 GFP 上下文。
 * 出参/返回：NULL 原样返回，否则返回同一指针；副作用是建立页尾 redzone。
 * 注意事项：可阻塞上下文先缩 quarantine；调用前整页已经开放。
 */
void * __must_check __kasan_kmalloc_large(const void *ptr, size_t size,
						gfp_t flags)
{
	if (gfpflags_allow_blocking(flags))
		kasan_quarantine_reduce();

	if (unlikely(ptr == NULL))
		return NULL;

	/* The object has already been unpoisoned by kasan_unpoison_pages(). */
	/* 不重新设置 tag，只按请求 size 封闭页块容量中的未使用尾部。 */
	poison_kmalloc_large_redzone(ptr, size, flags);

	/* Keep the tag that was set by alloc_pages(). */
	/* 返回页分配器已设置 tag 的原指针，维持地址与 page metadata 一致。 */
	return (void *)ptr;
}

/*
 * 业务背景：krealloc 原地调整逻辑大小时重建可访问区、redzone 和 alloc 轨迹。
 * 入参：object 为借用有效对象；size 为新字节数；flags 为当前分配上下文。
 * 出参/返回：ZERO_SIZE_PTR/KFENCE 原样返回，其余返回同一对象；ownership/tag 不变。
 * 注意事项：先开放新 data 范围，再按 slab/large 类型封闭尾部；容量由上层保证。
 */
void * __must_check __kasan_krealloc(const void *object, size_t size, gfp_t flags)
{
	/* slab 用于区分页级 large kmalloc 与 cache 对象，并选择 redzone 边界。 */
	struct slab *slab;

	if (gfpflags_allow_blocking(flags))
		kasan_quarantine_reduce();

	if (unlikely(object == ZERO_SIZE_PTR))
		return (void *)object;

	if (is_kfence_address(object))
		return (void *)object;

	/*
	 * Unpoison the object's data.
	 * Part of it might already have been unpoisoned, but it's unknown
	 * how big that part is.
	 */
	/* 旧请求尺寸未知，先开放新 size；后续 helper 再精确恢复尾部 poison。 */
	kasan_unpoison(object, size, false);

	slab = virt_to_slab(object);

	/* Piggy-back on kmalloc() instrumentation to poison the redzone. */
	/* 无 slab 走 compound 页容量，有 slab 走 cache->object_size。 */
	if (unlikely(!slab))
		poison_kmalloc_large_redzone(object, size, flags);
	else
		poison_kmalloc_redzone(slab->slab_cache, object, size, flags);

	return (void *)object;
}

/*
 * 业务背景：mempool 归还页块时 poison 它，但允许 highmem 或未抽样页块绕过 KASAN 跟踪。
 * 入参：page 为订单块首页借用指针；order 表示 2^order 页；ip 为释放调用点。
 * 出参/返回：true 表示可继续 mempool 协议（包括跳过）；非法释放已报告则返回 false。
 * 注意事项：本函数不释放页；tag-based 模式用 page tag 判断分配是否被抽样排除。
 */
bool __kasan_mempool_poison_pages(struct page *page, unsigned int order,
				  unsigned long ip)
{
	/* ptr 是线性映射 head 地址；只对非 highmem、受跟踪的页块赋值。 */
	unsigned long *ptr;

	if (unlikely(PageHighMem(page)))
		return true;

	/* Bail out if allocation was excluded due to sampling. */
	/* 默认内核 tag 表示本次页分配未被抽样，不能当作 KASAN tracked 对象处理。 */
	if (!IS_ENABLED(CONFIG_KASAN_GENERIC) &&
	    page_kasan_tag(page) == KASAN_TAG_KERNEL)
		return true;

	ptr = page_address(page);

	/* 非 head 或已 poison 会先产生报告，并阻止再次写 poison。 */
	if (check_page_allocation(ptr, ip))
		return false;

	kasan_poison(ptr, PAGE_SIZE << order, KASAN_PAGE_FREE, false);

	return true;
}

/*
 * 业务背景：mempool 重新交付页块时复用普通页分配的 unpoison/tag 协议。
 * 入参：page 为首页借用指针；order 表示页块阶数；ip 仅保持钩子 ABI。
 * 出参/返回：无；可能开放页块，也可能因 highmem/抽样而跳过。
 * 注意事项：忽略 bool 是既定契约，页块 ownership 已由 mempool 上层决定。
 */
void __kasan_mempool_unpoison_pages(struct page *page, unsigned int order,
				    unsigned long ip)
{
	__kasan_unpoison_pages(page, order, false);
}

/*
 * 业务背景：mempool 归还对象时按 large-kmalloc 或 slab 类型校验并 poison。
 * 入参：ptr 为待归还借用地址；ip 为释放调用点。
 * 出参/返回：可处理或无需处理返回 true；非法或双重释放已报告返回 false。
 * 注意事项：不真正释放对象；KFENCE 地址由其自身检测器管理。
 */
bool __kasan_mempool_poison_object(void *ptr, unsigned long ip)
{
	/* page/slab 只在分配器保护窗口内借用，用来选择对应检查规则。 */
	struct page *page = virt_to_page(ptr);
	struct slab *slab;

	/* large-kmalloc 必须从 compound head 释放，随后封闭整个页块。 */
	if (unlikely(PageLargeKmalloc(page))) {
		if (check_page_allocation(ptr, ip))
			return false;
		kasan_poison(ptr, page_size(page), KASAN_PAGE_FREE, false);
		return true;
	}

	if (is_kfence_address(ptr))
		return true;

	/* 普通对象转到所属 slab cache，复用对象边界与可访问性校验。 */
	slab = page_slab(page);

	if (check_slab_allocation(slab->slab_cache, ptr, ip))
		return false;

	poison_slab_object(slab->slab_cache, ptr, false);
	return true;
}

/*
 * 业务背景：mempool 重新交付对象时恢复 large-kmalloc 或 slab 对象的可访问范围。
 * 入参：ptr 为借用对象；size 为字节数；ip 仅保持接口一致。
 * 出参/返回：无；更新 shadow/tag，底层对象 ownership 仍归 mempool 调用者。
 * 注意事项：调用者保证 ptr 对应活跃池元素及足够容量；KFENCE 由独立路径处理。
 */
void __kasan_mempool_unpoison_object(void *ptr, size_t size, unsigned long ip)
{
	/* slab 决定对象来自 cache 还是 page_alloc；flags=0 因调用点可能持锁，禁止分配。 */
	struct slab *slab;
	gfp_t flags = 0; /* Might be executing under a lock. */
	/* 这里可能在持锁状态执行，所以栈记录不得申请内存或睡眠。 */

	slab = virt_to_slab(ptr);

	/*
	 * This function can be called for large kmalloc allocation that get
	 * their memory from page_alloc.
	 */
	/* large kmalloc 没有 slab，直接开放请求区并按 compound 页容量重建 redzone。 */
	if (unlikely(!slab)) {
		kasan_unpoison(ptr, size, false);
		poison_kmalloc_large_redzone(ptr, size, flags);
		return;
	}

	/* KFENCE 的 guard 与 metadata 不能由 KASAN shadow 覆盖。 */
	if (is_kfence_address(ptr))
		return;

	/* Unpoison the object and save alloc info for non-kmalloc() allocations. */
	/* slab 路径先开放整个对象；非 kmalloc cache 同时尽力记录 alloc 栈。 */
	unpoison_slab_object(slab->slab_cache, ptr, flags, false);

	/* Poison the redzone and save alloc info for kmalloc() allocations. */
	/* kmalloc cache 再按 size 收紧尾部，并以当前调用现场刷新 alloc 轨迹。 */
	if (is_kmalloc_cache(slab->slab_cache))
		poison_kmalloc_redzone(slab->slab_cache, ptr, size, flags);
}

/*
 * 业务背景：为显式探测入口检查单字节可访问性，并在失败时触发报告。
 * 入参：address 为借用目标地址；ip 是发起检查的指令地址。
 * 出参/返回：可访问返回 true；否则报告并返回 false，不建立任何 ownership。
 * 注意事项：只证明一个字节在检查瞬间有效，不冻结并发释放或更大范围。
 */
bool __kasan_check_byte(const void *address, unsigned long ip)
{
	if (!kasan_byte_accessible(address)) {
		kasan_report(address, 1, false, ip);
		return false;
	}
	return true;
}

#ifdef CONFIG_KASAN_VMALLOC
/*
 * 业务背景：由多个 vm_struct 组成的一次 vmap 分配必须共享同一 tag，供调用者作为连续对象使用。
 * 入参：vms 是借用的输入输出数组；nr_vms 为元素数；flags 控制 vmalloc shadow 操作。
 * 出参/返回：无；逐项把 vms[i]->addr 更新为带统一 tag 的地址并开放各自范围。
 * 注意事项：nr_vms 必须大于零；入口禁止 KEEP_TAG，因为首区必须产生新 tag，后续才复用。
 */
void __kasan_unpoison_vmap_areas(struct vm_struct **vms, int nr_vms,
				 kasan_vmalloc_flags_t flags)
{
	/* size/addr 描述当前 area；tag 来自第一个 area；area 是后续数组索引。 */
	unsigned long size;
	void *addr;
	int area;
	u8 tag;

	/*
	 * If KASAN_VMALLOC_KEEP_TAG was set at this point, all vms[] pointers
	 * would be unpoisoned with the KASAN_TAG_KERNEL which would disable
	 * KASAN checks down the line.
	 */
	/* 若首区也 KEEP_TAG，所有地址会保留内核默认 tag，后续访问检查等同被禁用。 */
	if (WARN_ON_ONCE(flags & KASAN_VMALLOC_KEEP_TAG))
		return;

	/* 第一个 area 负责生成并发布统一 tag，其返回地址回写给所有者。 */
	size = vms[0]->size;
	addr = vms[0]->addr;
	vms[0]->addr = __kasan_unpoison_vmalloc(addr, size, flags);
	tag = get_tag(vms[0]->addr);

	/* 后续 area 先套用首区 tag，再以 KEEP_TAG 开放，形成单一逻辑对象。 */
	for (area = 1 ; area < nr_vms ; area++) {
		size = vms[area]->size;
		addr = set_tag(vms[area]->addr, tag);
		vms[area]->addr =
			__kasan_unpoison_vmalloc(addr, size, flags | KASAN_VMALLOC_KEEP_TAG);
	}
}

/*
 * 业务背景：vrealloc 原地改变 vmalloc 对象逻辑大小时同步调整尾部 shadow 可访问性。
 * 入参：addr 为借用对象起点；old_size/new_size 为调整前后字节数。
 * 出参/返回：无；缩小时 poison 删除尾部，扩展时 unpoison 新增尾部；ownership/tag 不变。
 * 注意事项：调用者已保证底层映射容量和生命周期；granule 边界按增缩方向保守取整。
 */
void __kasan_vrealloc(const void *addr, unsigned long old_size,
		unsigned long new_size)
{
	/* 缩小先精确编码新末粒度，再封闭其后完整 granule，避免尾部仍可访问。 */
	if (new_size < old_size) {
		kasan_poison_last_granule(addr, new_size);

		new_size = round_up(new_size, KASAN_GRANULE_SIZE);
		old_size = round_up(old_size, KASAN_GRANULE_SIZE);
		if (new_size < old_size)
			__kasan_poison_vmalloc(addr + new_size,
					old_size - new_size);
	} else if (new_size > old_size) {
		/* 扩展从旧末端向下对齐处开放，KEEP_TAG 保留 addr 的既有逻辑对象身份。 */
		old_size = round_down(old_size, KASAN_GRANULE_SIZE);
		__kasan_unpoison_vmalloc(addr + old_size,
					new_size - old_size,
					KASAN_VMALLOC_PROT_NORMAL |
					KASAN_VMALLOC_VM_ALLOC |
					KASAN_VMALLOC_KEEP_TAG);
	}
}
#endif
