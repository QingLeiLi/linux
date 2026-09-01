// SPDX-License-Identifier: GPL-2.0
/*
 * KASAN quarantine.
 *
 * Author: Alexander Potapenko <glider@google.com>
 * Copyright (C) 2016 Google, Inc.
 *
 * Based on code by Dmitry Chernenkov.
 */
/*
 * Generic KASAN 先把已 poison 的 slab 对象延迟在隔离队列中，拉长 free 到
 * reuse 的时间窗；对象逐出后才交回 SLUB，从而更容易把悬空访问识别为 UAF。
 */

#define pr_fmt(fmt) "kasan: " fmt

#include <linux/gfp.h>
#include <linux/hash.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/printk.h>
/* shrinker/SRCU/CPU hotplug 分别连接容量回收、cache 销毁屏障和本地队列生命周期。 */
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/cpuhotplug.h>

#include "../slab.h"
#include "kasan.h"

/* Data structure and operations for quarantine queues. */
/* 以下是隔离单向队列的最小容器和拼接/拆分操作。 */

/*
 * Each queue is a single-linked list, which also stores the total size of
 * objects inside of it.
 */
/* 每条队列保存首尾指针和所含对象总字节；offline 仅用于 per-CPU 主队列。 */
struct qlist_head {
	/* head/tail 借用对象 free metadata 内嵌节点，队列本身不另行分配节点。 */
	struct qlist_node *head;
	struct qlist_node *tail;
	size_t bytes;
	/* CPU 下线后置位，阻止中断路径再次向已清空的本地队列追加。 */
	bool offline;
};

/* 栈上临时队列的空初始化；未显式给出的 offline 默认为 false。 */
#define QLIST_INIT { NULL, NULL, 0 }

/*
 * 业务背景：队列 helper 的空判断用于选择 O(1) 拼接路径。
 * 入参：q 为借用且由调用者同步保护的队列，不可空。
 * 出参/返回：head 为 NULL 返 true，否则 false；无副作用。
 * 注意事项：不加锁、不睡眠；只能在相应 per-CPU/自旋锁/SRCU 协议内调用。
 */
static bool qlist_empty(struct qlist_head *q)
{
	return !q->head;
}

/*
 * 业务背景：初始化或移空队列后重置链表与字节账本。
 * 入参：q 为调用者独占的输入输出队列。
 * 出参/返回：void；head/tail 清空且 bytes 归零，不释放原节点。
 * 注意事项：offline 刻意保留；调用者必须已转移或释放旧链，否则会丢失对象。
 */
static void qlist_init(struct qlist_head *q)
{
	q->head = q->tail = NULL;
	q->bytes = 0;
}

/*
 * 业务背景：把一个已 free 对象的 metadata 节点追加到 FIFO 尾部并累计容量。
 * 入参：q 为独占输入输出队列；qlink 为转入队列 ownership 的节点；size 为对象 cache->size 字节。
 * 出参/返回：void；节点成为新 tail，next 清 NULL，q->bytes 增加 size。
 * 注意事项：调用者负责同步；同一节点不能同时存在于两条队列。
 */
static void qlist_put(struct qlist_head *q, struct qlist_node *qlink,
		size_t size)
{
	/* 空队列同时建立 head；非空队列从旧 tail 链接新节点。 */
	if (unlikely(qlist_empty(q)))
		q->head = qlink;
	else
		q->tail->next = qlink;
	q->tail = qlink;
	qlink->next = NULL;
	q->bytes += size;
}

/*
 * 业务背景：在 per-CPU、全局 batch 与待释放队列之间 O(1) 转移整条 FIFO。
 * 入参：from 为被清空的输入输出队列，to 为接收队列；两者由调用者独占且不可相同。
 * 出参/返回：void；全部节点及 bytes 转给 to，from 变空。
 * 注意事项：空 to 快速路径会连同 offline 位复制整个头；当前调用点只在在线/临时队列间搬运。
 */
static void qlist_move_all(struct qlist_head *from, struct qlist_head *to)
{
	if (unlikely(qlist_empty(from)))
		return;

	/* 接收方为空时复制整个队列头（含 offline），再清空来源但保留来源 offline。 */
	if (qlist_empty(to)) {
		*to = *from;
		qlist_init(from);
		return;
	}

	/* 两者非空时把来源接到尾部，保留 FIFO 顺序并合并字节计数。 */
	to->tail->next = from->head;
	to->tail = from->tail;
	to->bytes += from->bytes;

	qlist_init(from);
}

#define QUARANTINE_PERCPU_SIZE (1 << 20)
/* batch 数至少 1024，且至少每 CPU 四格，降低生产者追上 head 的概率。 */
#define QUARANTINE_BATCHES \
	(1024 > 4 * CONFIG_NR_CPUS ? 1024 : 4 * CONFIG_NR_CPUS)

/*
 * The object quarantine consists of per-cpu queues and a global queue,
 * guarded by quarantine_lock.
 */
/* 对象先进入当前 CPU 无锁队列，超过 1MiB 后整体转入 quarantine_lock 保护的全局环。 */
static DEFINE_PER_CPU(struct qlist_head, cpu_quarantine);

/* Round-robin FIFO array of batches. */
/* head 指向下一批逐出对象，tail 指向当前追加批；相等边界保留一格避免满/空歧义。 */
static struct qlist_head global_quarantine[QUARANTINE_BATCHES];
static int quarantine_head;
static int quarantine_tail;
/* Total size of all objects in global_quarantine across all batches. */
/* quarantine_size 只统计全局批次，不含每 CPU 队列；锁内写，快速路径 READ_ONCE 读。 */
static unsigned long quarantine_size;
static DEFINE_RAW_SPINLOCK(quarantine_lock);
DEFINE_STATIC_SRCU(remove_cache_srcu);

/* cache 定向移除时，各 CPU 用独立锁保护中转队列，供 IPI 回调与销毁者交接。 */
struct cpu_shrink_qlist {
	raw_spinlock_t lock;
	struct qlist_head qlist;
};

static DEFINE_PER_CPU(struct cpu_shrink_qlist, shrink_qlist) = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(shrink_qlist.lock),
};

/* Maximum size of the global queue. */
/* 随在线内存/CPU 数动态重算；超过它时 reduce 每次逐出一个最老 batch。 */
static unsigned long quarantine_max_size;

/*
 * Target size of a batch in global_quarantine.
 * Usually equal to QUARANTINE_PERCPU_SIZE unless we have too much RAM.
 */
/* 单个全局 batch 的目标字节数；至少 1MiB，并力求最多使用环数组一半槽位。 */
static unsigned long quarantine_batch_size;

/*
 * The fraction of physical memory the quarantine is allowed to occupy.
 * Quarantine doesn't support memory shrinker with SLAB allocator, so we keep
 * the ratio low to avoid OOM.
 */
/* 全部隔离容量约束为物理内存的 1/32；SLAB 下无 shrinker，低比例避免隔离导致 OOM。 */
#define QUARANTINE_FRACTION 32

/*
 * 业务背景：混合 cache 的全局队列逐出时，要从对象地址恢复其所属 slab cache。
 * 入参：qlink 为隔离对象内嵌 free metadata 节点，借用且不可空。
 * 出参/返回：返回所属 kmem_cache 的借用指针，不增加 cache 引用。
 * 注意事项：对象尚未交回 allocator，virt_to_slab 映射仍有效；不睡眠。
 */
static struct kmem_cache *qlink_to_cache(struct qlist_node *qlink)
{
	return virt_to_slab(qlink)->slab_cache;
}

/*
 * 业务背景：链节点位于 kasan_free_meta 内，而 allocator 需要原始对象首地址。
 * 入参：qlink 为借用内嵌节点；cache 为其所属 cache，提供 free_meta_offset。
 * 出参/返回：返回仍归 quarantine 持有的对象首地址，不改变 ownership。
 * 注意事项：container_of 先回到 free_meta，再减偏移；cache 与节点不匹配会计算错误地址。
 */
static void *qlink_to_object(struct qlist_node *qlink, struct kmem_cache *cache)
{
	struct kasan_free_meta *free_info =
		container_of(qlink, struct kasan_free_meta,
			     quarantine_link);

	/* free metadata 可能位于对象起点或 redzone，偏移由 cache 建立时固定。 */
	return ((void *)free_info) - cache->kasan_info.free_meta_offset;
}

/*
 * 业务背景：把单个隔离对象真正交回 slab freelist，同时保留可用于 UAF 报告的元数据契约。
 * 入参：qlink 为 quarantine 持有的节点；cache 为所属 cache 的借用指针。
 * 出参/返回：void；对象 ownership 转回 ___cache_free()/SLUB，随后不得再访问 qlink。
 * 注意事项：调用点在自旋锁外；init_on_free 特例须清掉 KASAN 后写入的对象内 metadata。
 */
static void qlink_free(struct qlist_node *qlink, struct kmem_cache *cache)
{
	void *object = qlink_to_object(qlink, cache);
	struct kasan_free_meta *free_meta = kasan_get_free_meta(cache, object);

	/*
	 * Note: Keep per-object metadata to allow KASAN print stack traces for
	 * use-after-free-before-realloc bugs.
	 */
	/* 保留每对象 metadata，使“释放后、重新分配前”的 UAF 报告仍可打印 free 栈。 */

	/*
	 * If init_on_free is enabled and KASAN's free metadata is stored in
	 * the object, zero the metadata. Otherwise, the object's memory will
	 * not be properly zeroed, as KASAN saves the metadata after the slab
	 * allocator zeroes the object.
	 */
	/*
	 * 若 free_meta 覆盖对象开头，allocator 先清零、KASAN 后写 metadata 会破坏
	 * init_on_free 保证；逐出前显式清零修复最终交回 freelist 的对象内容。
	 */
	if (slab_want_init_on_free(cache) &&
	    cache->kasan_info.free_meta_offset == 0)
		memzero_explicit(free_meta, sizeof(*free_meta));

	/* 真正释放是 ownership/可复用边界；此调用后 object 可能立刻被其他 CPU 分配。 */
	___cache_free(cache, object, _THIS_IP_);
}

/*
 * 业务背景：批量逐出一条队列，必要时逐节点恢复各自 cache，并最终重置账本。
 * 入参：q 为本函数独占并消费的队列；cache 非 NULL 表示所有节点均属该 cache，否则逐项查找。
 * 出参/返回：void；所有对象转回 slab，q 变空。
 * 注意事项：可能在释放路径执行 allocator 逻辑，必须在 quarantine 自旋锁外调用。
 */
static void qlist_free_all(struct qlist_head *q, struct kmem_cache *cache)
{
	struct qlist_node *qlink;

	if (unlikely(qlist_empty(q)))
		return;

	/* 先保存 next 再 free，因为 qlink 位于即将可复用的对象内存中。 */
	qlink = q->head;
	while (qlink) {
		struct kmem_cache *obj_cache =
			cache ? cache :	qlink_to_cache(qlink);
		struct qlist_node *next = qlink->next;

		/* cache 参数只用于已按 cache 筛出的队列；混合队列从 slab 反查。 */
		qlink_free(qlink, obj_cache);
		qlink = next;
	}
	qlist_init(q);
}

/*
 * 业务背景：KASAN free 路径尝试暂扣 poison 后的对象，成功时阻止 SLUB 立即复用它。
 * 入参：cache 为对象所属 cache；object 为待释放对象，成功时 ownership 转给 quarantine。
 * 出参/返回：入队成功返 true；无 metadata 或 CPU 已离线返 false，调用者继续普通 free。
 * 注意事项：关中断保护当前 CPU 队列，并跨越本地到全局转移；不能睡眠，锁内仅搬链。
 */
bool kasan_quarantine_put(struct kmem_cache *cache, void *object)
{
	/* temp 是栈上中转队列，避免全局锁下操作 per-CPU 队列；meta 内含唯一链节点。 */
	unsigned long flags;
	struct qlist_head *q;
	struct qlist_head temp = QLIST_INIT;
	struct kasan_free_meta *meta = kasan_get_free_meta(cache, object);

	/*
	 * If there's no metadata for this object, don't put it into
	 * quarantine.
	 */
	/* 无 free metadata 就没有可嵌入节点，返回 false 让 slab 自己完成释放。 */
	if (!meta)
		return false;

	/*
	 * Note: irq must be disabled until after we move the batch to the
	 * global quarantine. Otherwise kasan_quarantine_remove_cache() can
	 * miss some objects belonging to the cache if they are in our local
	 * temp list. kasan_quarantine_remove_cache() executes on_each_cpu()
	 * at the beginning which ensures that it either sees the objects in
	 * per-cpu lists or in the global quarantine.
	 */
	/*
	 * 中断必须保持关闭直到对象到达全局队列：remove_cache() 的 on_each_cpu
	 * 屏障只能保证看见 per-CPU 或全局对象，看不见栈上 temp 会造成 cache 销毁后 UAF。
	 */
	local_irq_save(flags);

	/* 阶段 1：稳定当前 CPU；offline 表示下线清理已开始，不能再接收对象。 */
	q = this_cpu_ptr(&cpu_quarantine);
	if (q->offline) {
		local_irq_restore(flags);
		return false;
	}
	/* 小队列快速路径只追加 per-CPU FIFO，无全局锁竞争。 */
	qlist_put(q, &meta->quarantine_link, cache->size);
	if (unlikely(q->bytes > QUARANTINE_PERCPU_SIZE)) {
		qlist_move_all(q, &temp);

		/* 阶段 2：超过 1MiB 时整批转入全局环，并在同一锁域提交容量账本。 */
		raw_spin_lock(&quarantine_lock);
		WRITE_ONCE(quarantine_size, quarantine_size + temp.bytes);
		qlist_move_all(&temp, &global_quarantine[quarantine_tail]);
		/* 当前 batch 达目标后尝试推进 tail；若下一格是 head，则保持尾部避免覆盖待逐出批。 */
		if (global_quarantine[quarantine_tail].bytes >=
				READ_ONCE(quarantine_batch_size)) {
			int new_tail;

			/* 环尾按槽递增并回绕；仅在不会追上 head 时提交新位置。 */
			new_tail = quarantine_tail + 1;
			if (new_tail == QUARANTINE_BATCHES)
				new_tail = 0;
			if (new_tail != quarantine_head)
				quarantine_tail = new_tail;
		}
		raw_spin_unlock(&quarantine_lock);
	}

	/* 恢复中断后对象已处于 remove_cache 能观察的稳定位置。 */
	local_irq_restore(flags);

	return true;
}

/*
 * 业务背景：可阻塞分配前调用本函数，在全局 quarantine 超预算时逐出最老一个 batch。
 * 入参：无；使用全局容量、在线内存/CPU 状态。
 * 出参/返回：void；未超限无副作用，超限则更新预算/head 并把一批对象交回 slab。
 * 注意事项：可睡眠；锁内只摘链，锁外 free；SRCU 防止与 cache 定向移除漏看同一对象。
 */
void kasan_quarantine_reduce(void)
{
	/* total_size 是 1/32 RAM；percpu_quarantines 是所有在线 CPU 的保留上限。 */
	size_t total_size, new_quarantine_size, percpu_quarantines;
	unsigned long flags;
	int srcu_idx;
	struct qlist_head to_free = QLIST_INIT;

	/* 无锁快速检查允许读到旧值；最坏只是延后一次收缩，锁内会用当前账本复核。 */
	if (likely(READ_ONCE(quarantine_size) <=
		   READ_ONCE(quarantine_max_size)))
		return;

	/*
	 * srcu critical section ensures that kasan_quarantine_remove_cache()
	 * will not miss objects belonging to the cache while they are in our
	 * local to_free list. srcu is chosen because (1) it gives us private
	 * grace period domain that does not interfere with anything else,
	 * and (2) it allows synchronize_srcu() to return without waiting
	 * if there are no pending read critical sections (which is the
	 * expected case).
	 */
	/*
	 * SRCU 读侧覆盖“从全局摘下到真正 free”的不可见窗口。remove_cache()
	 * synchronize_srcu() 等所有这类私有待释放队列耗尽，且独立 domain 不拖累普通 RCU。
	 */
	srcu_idx = srcu_read_lock(&remove_cache_srcu);
	/* 阶段 1：锁住全局环，串行预算重算、head 摘除和 quarantine_size 更新。 */
	raw_spin_lock_irqsave(&quarantine_lock, flags);

	/*
	 * Update quarantine size in case of hotplug. Allocate a fraction of
	 * the installed memory to quarantine minus per-cpu queue limits.
	 */
	/* CPU 热插拔会改变本地队列总预留，故每次真正收缩前依据当前拓扑重算。 */
	total_size = (totalram_pages() << PAGE_SHIFT) /
		QUARANTINE_FRACTION;
	percpu_quarantines = QUARANTINE_PERCPU_SIZE * num_online_cpus();
	new_quarantine_size = (total_size < percpu_quarantines) ?
		0 : total_size - percpu_quarantines;
	WRITE_ONCE(quarantine_max_size, new_quarantine_size);
	/* Aim at consuming at most 1/2 of slots in quarantine. */
	/* 每批目标至少 1MiB，并让总预算约分布到半数槽，给生产者留出环形余量。 */
	WRITE_ONCE(quarantine_batch_size, max((size_t)QUARANTINE_PERCPU_SIZE,
		2 * total_size / QUARANTINE_BATCHES));

	/* 阶段 2：仍超限才摘最老 head；一次调用最多释放一个 batch，限制分配延迟。 */
	if (likely(quarantine_size > quarantine_max_size)) {
		qlist_move_all(&global_quarantine[quarantine_head], &to_free);
		WRITE_ONCE(quarantine_size, quarantine_size - to_free.bytes);
		quarantine_head++;
		if (quarantine_head == QUARANTINE_BATCHES)
			quarantine_head = 0;
	}

	/* 阶段 3：出锁后执行可能较重的 slab free，再退出 SRCU 可见性窗口。 */
	raw_spin_unlock_irqrestore(&quarantine_lock, flags);

	qlist_free_all(&to_free, NULL);
	srcu_read_unlock(&remove_cache_srcu, srcu_idx);
}

/*
 * 业务背景：cache shrink/destroy 必须从混合隔离队列中筛出属于目标 cache 的对象。
 * 入参：from 为待原地过滤队列；to 接收匹配节点；cache 为目标且均由调用者借用。
 * 出参/返回：void；匹配节点 ownership 转给 to，其余按原次序重建到 from。
 * 注意事项：调用者独占两队列；不释放对象、不睡眠，逐节点从 slab 元数据反查 cache。
 */
static void qlist_move_cache(struct qlist_head *from,
				   struct qlist_head *to,
				   struct kmem_cache *cache)
{
	struct qlist_node *curr;

	if (unlikely(qlist_empty(from)))
		return;

	/* 先把 from 账本清空，再逐节点归类，避免维护删除前驱指针和 bytes 差值。 */
	curr = from->head;
	qlist_init(from);
	while (curr) {
		struct qlist_node *next = curr->next;
		struct kmem_cache *obj_cache = qlink_to_cache(curr);

		/* next 在重新链接 curr 前保存；匹配与否分别追加到目标或重建队列。 */
		if (obj_cache == cache)
			qlist_put(to, curr, obj_cache->size);
		else
			qlist_put(from, curr, obj_cache->size);

		curr = next;
	}
}

/*
 * 业务背景：把某 CPU 主队列中的目标 cache 对象搬到其带锁 shrink 中转队列。
 * 入参：q 为该 CPU 的主隔离队列；arg 编码借用 kmem_cache 指针。
 * 出参/返回：void；匹配对象移入当前 CPU shrink_qlist，不释放。
 * 注意事项：调用者保证主 q 不受本 CPU 中断并发；sq->lock 与后续收集者同步。
 */
static void __per_cpu_remove_cache(struct qlist_head *q, void *arg)
{
	struct kmem_cache *cache = arg;
	unsigned long flags;
	struct cpu_shrink_qlist *sq;

	/* shrink 队列可能正被 remove_cache 主线程收集，必须在每 CPU 原始自旋锁下交接。 */
	sq = this_cpu_ptr(&shrink_qlist);
	raw_spin_lock_irqsave(&sq->lock, flags);
	qlist_move_cache(q, &sq->qlist, cache);
	raw_spin_unlock_irqrestore(&sq->lock, flags);
}

/*
 * 业务背景：on_each_cpu() 的 IPI 回调在每个在线 CPU 上冻结本地追加并筛出目标 cache。
 * 入参：arg 为借用目标 cache；当前 CPU 由回调执行环境隐式给出。
 * 出参/返回：void；在线队列中的匹配对象转到 shrink 中转队列。
 * 注意事项：CPU offline 队列已由下线回调清空；回调上下文不能睡眠。
 */
static void per_cpu_remove_cache(void *arg)
{
	struct qlist_head *q;

	q = this_cpu_ptr(&cpu_quarantine);
	/*
	 * Ensure the ordering between the writing to q->offline and
	 * per_cpu_remove_cache.  Prevent cpu_quarantine from being corrupted
	 * by interrupt.
	 */
	/* READ_ONCE 与 offline 写及 barrier 配合；离线 CPU 不再触碰其已清空主队列。 */
	if (READ_ONCE(q->offline))
		return;
	__per_cpu_remove_cache(q, arg);
}

/* Free all quarantined objects belonging to cache. */
/* 在 cache shrink/销毁前，从 per-CPU、全局及并发逐出窗口中排空其全部隔离对象。 */
/*
 * 业务背景：SLUB 释放 cache 元数据前必须确保 quarantine 不再保存指向该 cache 的对象。
 * 入参：cache 为待收缩/销毁的借用 cache；调用期间其 slab 元数据仍有效。
 * 出参/返回：void；所有已观察到的该 cache 对象真正 free，并等待并发 reduce 越过 SRCU。
 * 注意事项：跨 CPU IPI、cond_resched 和 synchronize_srcu 均可等待，只能在可睡眠上下文调用。
 */
void kasan_quarantine_remove_cache(struct kmem_cache *cache)
{
	/* to_free 汇聚目标节点；sq/cpu 遍历每 CPU IPI 交接队列；i 扫描全部全局 batch。 */
	unsigned long flags, i;
	struct qlist_head to_free = QLIST_INIT;
	int cpu;
	struct cpu_shrink_qlist *sq;

	/*
	 * Must be careful to not miss any objects that are being moved from
	 * per-cpu list to the global quarantine in kasan_quarantine_put(),
	 * nor objects being freed in kasan_quarantine_reduce(). on_each_cpu()
	 * achieves the first goal, while synchronize_srcu() achieves the
	 * second.
	 */
	/*
	 * on_each_cpu 关闭“per-CPU→栈 temp→全局”的生产者窗口；随后扫描全局环。
	 * synchronize_srcu 则等待 reduce 已摘出的私有 to_free，三者共同保证不漏对象。
	 */
	on_each_cpu(per_cpu_remove_cache, cache, 1);

	/* 阶段 1：收集各 CPU IPI 回调留下的中转节点；锁外集中 free。 */
	for_each_online_cpu(cpu) {
		sq = per_cpu_ptr(&shrink_qlist, cpu);
		raw_spin_lock_irqsave(&sq->lock, flags);
		qlist_move_cache(&sq->qlist, &to_free, cache);
		raw_spin_unlock_irqrestore(&sq->lock, flags);
	}
	qlist_free_all(&to_free, cache);

	/* 阶段 2：逐批过滤全局环；每批后暂时解锁并让出 CPU，避免长时间关抢占。 */
	raw_spin_lock_irqsave(&quarantine_lock, flags);
	for (i = 0; i < QUARANTINE_BATCHES; i++) {
		if (qlist_empty(&global_quarantine[i]))
			continue;
		qlist_move_cache(&global_quarantine[i], &to_free, cache);
		/* Scanning whole quarantine can take a while. */
		/* 扫完整个隔离环可能很慢；筛出的节点已在私有 to_free，不受解锁影响。 */
		raw_spin_unlock_irqrestore(&quarantine_lock, flags);
		cond_resched();
		raw_spin_lock_irqsave(&quarantine_lock, flags);
	}
	raw_spin_unlock_irqrestore(&quarantine_lock, flags);

	/* 全局扫描结束后才真正释放目标对象，避免在 raw spinlock 内进入 allocator。 */
	qlist_free_all(&to_free, cache);

	/* 阶段 3：等待并发 reduce 的私有待释放链清空，之后 cache 才可安全销毁。 */
	synchronize_srcu(&remove_cache_srcu);
}

/*
 * 业务背景：CPU 上线时重新允许其 per-CPU quarantine 接收对象。
 * 入参：cpu 是热插拔核心传入的上线 CPU 编号；本回调实际在目标 CPU 上执行。
 * 出参/返回：恒返 0；清 q->offline，不分配资源。
 * 注意事项：热插拔状态机串行上下线；队列已在静态存储中并应为空。
 */
static int kasan_cpu_online(unsigned int cpu)
{
	this_cpu_ptr(&cpu_quarantine)->offline = false;
	return 0;
}

/*
 * 业务背景：CPU 下线前阻止新的本地隔离入队，并把遗留对象直接交回 slab。
 * 入参：cpu 是目标 CPU 编号；回调在其热插拔停机序列中运行。
 * 出参/返回：恒返 0；置 offline 并清空当前 CPU 主队列。
 * 注意事项：WRITE_ONCE+编译器 barrier 约束标志先于清队列，避免中断追加破坏链表。
 */
static int kasan_cpu_offline(unsigned int cpu)
{
	struct qlist_head *q;

	q = this_cpu_ptr(&cpu_quarantine);
	/* Ensure the ordering between the writing to q->offline and
	 * qlist_free_all. Otherwise, cpu_quarantine may be corrupted
	 * by interrupt.
	 */
	/* 先发布 offline，再清队列；本 CPU 中断中的 put 观察标志后会退回普通 free。 */
	WRITE_ONCE(q->offline, true);
	barrier();
	qlist_free_all(q, NULL);
	return 0;
}

/*
 * 业务背景：late init 时向 CPU hotplug 注册 quarantine 上下线回调，闭合 per-CPU 生命周期。
 * 入参：无。
 * 出参/返回：成功返回动态 state 编号（非负）；失败返回负 errno 并打印错误。
 * 注意事项：late_initcall 可睡眠；注册成功后热插拔核心负责按 CPU 调用 online/offline。
 */
static int __init kasan_cpu_quarantine_init(void)
{
	int ret = 0;

	/* 动态 online state 同时覆盖当前在线 CPU，并为后续上下线安装对称回调。 */
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "mm/kasan:online",
				kasan_cpu_online, kasan_cpu_offline);
	if (ret < 0)
		pr_err("cpu quarantine register failed [%d]\n", ret);
	return ret;
}
late_initcall(kasan_cpu_quarantine_init);
