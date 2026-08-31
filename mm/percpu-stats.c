// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2017		Facebook Inc.
 * Copyright (C) 2017		Dennis Zhou <dennis@kernel.org>
 *
 * Prints statistics about the percpu allocator and backing chunks.
 */
/*
 * 本文件通过只读 debugfs 快照输出 percpu 分配器及其 backing chunk 的统计。
 * 读取时持有 pcpu_lock 冻结 chunk 列表和位图，先在锁外分配足够的临时数组，再按
 * allocation/boundary bitmap 重建已分配区和空闲碎片；它只观测，不改变分配器策略。
 */
#include <linux/debugfs.h>
#include <linux/list.h>
#include <linux/percpu.h>
#include <linux/seq_file.h>
#include <linux/sort.h>
#include <linux/vmalloc.h>

#include "percpu-internal.h"

/* P 把给定名称和值统一打印为有符号 64 位十进制，仅在持有 seq_file 指针 m 的函数内用。 */
#define P(X, Y) \
	seq_printf(m, "  %-20s: %12lld\n", X, (long long int)Y)

/* pcpu_stats 由 pcpu_lock 保护的热路径钩子累计；stats_ai 是首块布局的定长快照。 */
struct percpu_stats pcpu_stats;
struct pcpu_alloc_info pcpu_stats_ai;

/*
 * cmpint() - 为 sort() 按有符号整数升序比较两个片段长度。
 * 业务背景：chunk_map_stats() 以负值编码空闲片段、正值编码分配，排序后两类自然分区。
 * 入参：a/b 是 sort 临时借用的有效 int 元素地址，不可为 NULL，ownership 不变。
 * 出参/返回：负/零/正分别表示 a 小于/等于/大于 b；无其他副作用。
 * 注意事项：仅比较受 pcpu_lock 稳定的临时数组，不睡眠；片段尺寸受 chunk 大小约束。
 */
static int cmpint(const void *a, const void *b)
{
	return *(int *)a - *(int *)b;
}

/*
 * Iterates over all chunks to find the max nr_alloc entries.
 */
/*
 * 遍历所有 chunk，寻找当前单个 chunk 的最大分配项数。
 *
 * 业务背景：percpu_stats_show() 据此在锁外预分配片段数组；一个分配最多把空闲空间分成
 * 两侧片段，所以容量 2*max_nr_alloc+1 足以重建任一 chunk。
 * 入参：无。
 * 出参/返回：返回所有槽链表中 chunk->nr_alloc 的最大值，无 chunk 时为 0。
 * 注意事项：调用者必须持有 pcpu_lock 且 IRQ 已禁用，以稳定列表和计数；函数不睡眠。
 */
static int find_max_nr_alloc(void)
{
	struct pcpu_chunk *chunk;
	int slot, max_nr_alloc;

	/* slot 覆盖正常、待回收和 sidelined 链表；chunk 是锁内借用指针。 */
	max_nr_alloc = 0;
	for (slot = 0; slot < pcpu_nr_slots; slot++)
		list_for_each_entry(chunk, &pcpu_chunk_lists[slot], list)
			max_nr_alloc = max(max_nr_alloc, chunk->nr_alloc);

	return max_nr_alloc;
}

/*
 * Prints out chunk state. Fragmentation is considered between
 * the beginning of the chunk to the last allocation.
 *
 * All statistics are in bytes unless stated otherwise.
 */
/*
 * 输出一个 chunk 的状态；碎片只统计 chunk 起点到最后一个有效分配之间的空洞，尾部
 * 尚未使用空间不计入 fragmentation。除明确说明外，所有统计值均以字节为单位。
 *
 * 业务背景：percpu_stats_show() 在 pcpu_lock 内逐 chunk 调用它，把两张 bitmap 还原成
 * 正数分配段和负数空闲段，再输出碎片与分配尺寸的极值/中位数。
 * 入参：m 是 seq_file 借用输出流；chunk 是锁内借用且位图稳定的对象；buffer 是调用者
 * 拥有、至少 2*chunk->nr_alloc+1 个 int 的可写暂存区。
 * 出参/返回：无直接返回值；向 m 追加文本并覆盖 buffer，有序统计不改变 chunk。
 * 注意事项：要求持有 pcpu_lock/IRQ disabled，不能睡眠；buffer 内容仅本次调用有效。
 */
static void chunk_map_stats(struct seq_file *m, struct pcpu_chunk *chunk,
			    int *buffer)
{
	/* 变量地图：chunk_md 是整块 hint；alloc_sizes 用正/负号编码段类型，p 用于排序后扫描。 */
	struct pcpu_block_md *chunk_md = &chunk->chunk_md;
	int i, last_alloc, as_len, start, end;
	int *alloc_sizes, *p;
	/* statistics */
	/* 以下聚合量都是当前 chunk 的即时统计，初始化为 0 以覆盖无分配场景。 */
	int sum_frag = 0, max_frag = 0;
	int cur_min_alloc = 0, cur_med_alloc = 0, cur_max_alloc = 0;

	alloc_sizes = buffer;

	/*
	 * find_last_bit returns the start value if nothing found.
	 * Therefore, we must determine if it is a failure of find_last_bit
	 * and set the appropriate value.
	 */
	/*
	 * find_last_bit() 未命中也返回传入起点，因此必须再 test_bit() 区分“位 0 被分配”
	 * 与“完全没有分配”；成功时加 1，把 last_alloc 变为扫描半开上界。
	 */
	last_alloc = find_last_bit(chunk->alloc_map,
				   pcpu_chunk_map_bits(chunk) -
				   chunk->end_offset / PCPU_MIN_ALLOC_SIZE - 1);
	last_alloc = test_bit(last_alloc, chunk->alloc_map) ?
		     last_alloc + 1 : 0;

	/* 跳过为页对齐而与前一区域重叠的 start_offset，不把它算作当前 chunk 空间。 */
	as_len = 0;
	start = chunk->start_offset / PCPU_MIN_ALLOC_SIZE;

	/*
	 * If a bit is set in the allocation map, the bound_map identifies
	 * where the allocation ends.  If the allocation is not set, the
	 * bound_map does not identify free areas as it is only kept accurate
	 * on allocation, not free.
	 *
	 * Positive values are allocations and negative values are free
	 * fragments.
	 */
	/*
	 * alloc_map 的置位表示分配起点，此时 bound_map 能找到该分配结尾；空闲区则只能
	 * 搜索下一个 alloc_map 置位，因为 free 时不会维护其 bound_map。正值代表分配，
	 * 负值代表空闲碎片，绝对值都是字节数。
	 */
	while (start < last_alloc) {
		/* 根据当前 bit 的类型选择可信的下一边界，并先记录符号。 */
		if (test_bit(start, chunk->alloc_map)) {
			end = find_next_bit(chunk->bound_map, last_alloc,
					    start + 1);
			alloc_sizes[as_len] = 1;
		} else {
			end = find_next_bit(chunk->alloc_map, last_alloc,
					    start + 1);
			alloc_sizes[as_len] = -1;
		}

		/* 位数乘最小分配粒度得到字节长度，再把游标推进到下一段。 */
		alloc_sizes[as_len++] *= (end - start) * PCPU_MIN_ALLOC_SIZE;

		start = end;
	}

	/*
	 * The negative values are free fragments and thus sorting gives the
	 * free fragments at the beginning in largest first order.
	 */
	/*
	 * 负数空闲片段排序后位于数组开头，且绝对值最大的片段最先出现；随后是正数
	 * 分配段。last_alloc 来自真实分配起点，所以 as_len>0 时至少存在一个正数元素。
	 */
	if (as_len > 0) {
		sort(alloc_sizes, as_len, sizeof(int), cmpint, NULL);

		/* iterate through the unallocated fragments */
		/* 扫描负数前缀，转回正数累加总碎片并保存最大单个空洞。 */
		for (i = 0, p = alloc_sizes; *p < 0 && i < as_len; i++, p++) {
			sum_frag -= *p;
			max_frag = max(max_frag, -1 * (*p));
		}

		/* i 指向第一个分配段；在正数后缀上取最小、中位和最大分配尺寸。 */
		cur_min_alloc = alloc_sizes[i];
		cur_med_alloc = alloc_sizes[(i + as_len - 1) / 2];
		cur_max_alloc = alloc_sizes[as_len - 1];
	}

	/* 依次输出计数、整块 hint、碎片聚合和当前分配尺寸分布。 */
	P("nr_alloc", chunk->nr_alloc);
	P("max_alloc_size", chunk->max_alloc_size);
	P("empty_pop_pages", chunk->nr_empty_pop_pages);
	P("first_bit", chunk_md->first_free);
	P("free_bytes", chunk->free_bytes);
	P("contig_bytes", chunk_md->contig_hint * PCPU_MIN_ALLOC_SIZE);
	/* 后半组把重建出的碎片和分配尺寸分布写入同一 chunk 小节。 */
	P("sum_frag", sum_frag);
	P("max_frag", max_frag);
	P("cur_min_alloc", cur_min_alloc);
	P("cur_med_alloc", cur_med_alloc);
	P("cur_max_alloc", cur_max_alloc);
	seq_putc(m, '\n');
}

/*
 * percpu_stats_show() - 生成一次全局和逐 chunk 的 percpu 分配器快照。
 *
 * 业务背景：DEFINE_SHOW_ATTRIBUTE 生成的 debugfs open/read 路径调用本函数。它先短暂
 * 加锁估算缓冲区，锁外 vmalloc，再重新加锁复核，解决估算与分配之间新 allocation 的竞态。
 * 入参：m 是 seq_file 借用输出流；v 是 single_open 传入但本实现不使用的私有指针。
 * 出参/返回：成功输出完整快照并返回 0；临时数组分配失败返回 -ENOMEM，无 ownership 遗留。
 * 注意事项：打印 chunk 期间持有 pcpu_lock 且 IRQ disabled，保证列表/bitmap 一致；禁止
 * 在该区段加入可睡眠操作。buffer 在锁外分配并在解锁后释放。
 */
static int percpu_stats_show(struct seq_file *m, void *v)
{
	struct pcpu_chunk *chunk;
	int slot, max_nr_alloc;
	int *buffer;

alloc_buffer:
	/* 阶段 1：锁内取得容量上界，立即解锁以允许 vmalloc 睡眠。 */
	spin_lock_irq(&pcpu_lock);
	max_nr_alloc = find_max_nr_alloc();
	spin_unlock_irq(&pcpu_lock);

	/* there can be at most this many free and allocated fragments */
	/* 每 N 个分配最多形成 N+1 个空闲片段，故 2N+1 覆盖两类段总数。 */
	buffer = vmalloc_array(2 * max_nr_alloc + 1, sizeof(int));
	if (!buffer)
		return -ENOMEM;

	/* 阶段 2：重新冻结状态；若估算窗口内增长，释放旧 buffer 后重试。 */
	spin_lock_irq(&pcpu_lock);

	/* if the buffer allocated earlier is too small */
	/* 早先分配的缓冲区若已不足，不能在锁内扩容，只能完整重走锁外分配。 */
	if (max_nr_alloc < find_max_nr_alloc()) {
		spin_unlock_irq(&pcpu_lock);
		vfree(buffer);
		goto alloc_buffer;
	}

	/* PL 从只读启动布局快照中按字段名和值输出六个字节单位参数。 */
#define PL(X)								\
	seq_printf(m, "  %-20s: %12lld\n", #X, (long long int)pcpu_stats_ai.X)

	seq_printf(m,
			"Percpu Memory Statistics\n"
			"Allocation Info:\n"
			"----------------------------------------\n");
	PL(unit_size);
	PL(static_size);
	PL(reserved_size);
	/* dyn/atom/alloc_size 分别描述动态区、最小 backing 粒度和整组分配量。 */
	PL(dyn_size);
	PL(atom_size);
	PL(alloc_size);
	seq_putc(m, '\n');

#undef PL

	/* PU 输出 pcpu_lock 保护的无符号累计/峰值字段；P 处理 int/size_t 型即时值。 */
#define PU(X) \
	seq_printf(m, "  %-20s: %12llu\n", #X, (unsigned long long)pcpu_stats.X)

	seq_printf(m,
			"Global Stats:\n"
			"----------------------------------------\n");
	PU(nr_alloc);
	PU(nr_dealloc);
	PU(nr_cur_alloc);
	PU(nr_max_alloc);
	/* 后半组输出当前/峰值 chunk 数及历史分配尺寸边界。 */
	PU(nr_chunks);
	PU(nr_max_chunks);
	PU(min_alloc_size);
	PU(max_alloc_size);
	P("empty_pop_pages", pcpu_nr_empty_pop_pages);
	seq_putc(m, '\n');

#undef PU

	/* 下一个小节不再使用全局宏字段，只逐个解析锁内稳定的 chunk。 */
	seq_printf(m,
			"Per Chunk Stats:\n"
			"----------------------------------------\n");

	/* reserved chunk 不一定挂入普通 slot，存在时需单独输出且不转移引用。 */
	if (pcpu_reserved_chunk) {
		seq_puts(m, "Chunk: <- Reserved Chunk\n");
		chunk_map_stats(m, pcpu_reserved_chunk, buffer);
	}

	/* 阶段 3：按槽和链表顺序输出，额外标识首块、待 depopulate 与 sidelined 状态。 */
	for (slot = 0; slot < pcpu_nr_slots; slot++) {
		list_for_each_entry(chunk, &pcpu_chunk_lists[slot], list) {
			/* 标题揭示特殊身份或当前回收槽，随后统一输出位图统计。 */
			if (chunk == pcpu_first_chunk)
				seq_puts(m, "Chunk: <- First Chunk\n");
			else if (slot == pcpu_to_depopulate_slot)
				seq_puts(m, "Chunk (to_depopulate)\n");
			else if (slot == pcpu_sidelined_slot)
				seq_puts(m, "Chunk (sidelined):\n");
			else
				seq_puts(m, "Chunk:\n");
			/* 同一 buffer 串行复用，helper 会完全按本 chunk 的 as_len 覆盖有效前缀。 */
			chunk_map_stats(m, chunk, buffer);
		}
	}

	/* 快照结束后先恢复 IRQ/锁，再执行可睡眠的 vfree。 */
	spin_unlock_irq(&pcpu_lock);

	vfree(buffer);

	return 0;
}

/* 生成 percpu_stats_open()/read()/llseek()/release() 及只读 file_operations。 */
DEFINE_SHOW_ATTRIBUTE(percpu_stats);

/*
 * init_percpu_stats_debugfs() - 注册 /sys/kernel/debug/percpu_stats 只读入口。
 * 业务背景：late_initcall 在 debugfs 和 percpu 分配器均就绪后暴露统计文件。
 * 入参：无。
 * 出参/返回：始终返回 0；成功时 debugfs 持有 dentry 和 fops，失败时 debugfs helper
 * 返回错误指针/NULL 但本调试功能选择静默缺失，不影响启动。
 * 注意事项：仅 __init 调用一次，可睡眠；文件权限 0444，无写路径和用户私有数据。
 */
static int __init init_percpu_stats_debugfs(void)
{
	debugfs_create_file("percpu_stats", 0444, NULL, NULL,
			&percpu_stats_fops);

	return 0;
}

/* 在核心初始化完成后注册；失败不阻断内核启动。 */
late_initcall(init_percpu_stats_debugfs);
