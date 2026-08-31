/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel Electric-Fence (KFENCE). For more info please see
 * Documentation/dev-tools/kfence.rst.
 *
 * Copyright (C) 2020, Google LLC.
 */
/*
 * KFENCE 以低采样率把 slab 对象放进“对象页 + guard page”池，用页保护捕获越界和
 * UAF；完整设计见 Documentation/dev-tools/kfence.rst。此私有头定义 core、fault
 * handler 与 report 共享的 metadata、状态机、canary 和锁契约。
 */

#ifndef MM_KFENCE_KFENCE_H
#define MM_KFENCE_KFENCE_H

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "../slab.h" /* for struct kmem_cache */
/* 上述私有 slab 头提供 kmem_cache/obj_ext 完整布局，供 metadata 保存最后分配来源。 */

/* 全局运行期开关；热路径读取，初始化/控制面写入，false 时不再采样新 KFENCE 对象。 */
extern bool kfence_enabled;

/*
 * Get the canary byte pattern for @addr. Use a pattern that varies based on the
 * lower 3 bits of the address, to detect memory corruptions with higher
 * probability, where similar constants are used.
 */
/*
 * 为地址生成 1 字节 canary：0xaa 与地址低 3 位异或，使相邻 8 字节内各位置模式
 * 不同；常量型越界写更难碰巧写回正确值。宏会求值 addr，实参不得带副作用。
 */
#define KFENCE_CANARY_PATTERN_U8(addr) ((u8)0xaa ^ (u8)((unsigned long)(addr) & 0x7))

/*
 * Define a continuous 8-byte canary starting from a multiple of 8. The canary
 * of each byte is only related to the lowest three bits of its address, so the
 * canary of every 8 bytes is the same. 64-bit memory can be filled and checked
 * at a time instead of byte by byte to improve performance.
 */
/*
 * 连续 8 字节 canary 以 8 字节对齐地址为周期；每个 byte 仍只依赖自身低三位，
 * 因此可用一次 64 位写/比较替代八次逐字节操作。le64_to_cpu 保证不同端序下内存
 * 中的逐字节图案仍与 KFENCE_CANARY_PATTERN_U8() 一致。
 */
#define KFENCE_CANARY_PATTERN_U64 ((u64)0xaaaaaaaaaaaaaaaa ^ (u64)(le64_to_cpu(0x0706050403020100)))

/* Maximum stack depth for reports. */
/* 每次分配/释放报告最多保存 64 个返回地址，超出部分截断而非动态扩容。 */
#define KFENCE_STACK_DEPTH 64

/* 保护 metadata freelist 链接与领取/归还，raw 锁可用于常规 spinlock 不安全的上下文。 */
extern raw_spinlock_t kfence_freelist_lock;

/* KFENCE object states. */
/* KFENCE 对象状态由 metadata_update_state()/分配释放路径在 meta->lock 下转换。 */
enum kfence_object_state {
	KFENCE_OBJECT_UNUSED,		/* Object is unused. */
	/* 初始化时尚未承载过对象；分配器第一次领取后进入 ALLOCATED，通常不会再回此态。 */
	KFENCE_OBJECT_ALLOCATED,	/* Object is currently allocated. */
	/* 正由调用者使用；对象页可访问，guard 页保持保护，free_track 尚无本次释放。 */
	KFENCE_OBJECT_RCU_FREEING,	/* Object was allocated, and then being freed by rcu. */
	/* kfree_rcu 后等待 grace period；禁止重用，直到回调确认旧 RCU reader 离开。 */
	KFENCE_OBJECT_FREED,		/* Object was allocated, and then freed. */
	/* 已记录释放且对象页重新保护；后续访问分类为 UAF，可在条件满足时重新采样。 */
};

/* Alloc/free tracking information. */
/*
 * 一次分配或释放事件的诊断快照：pid/cpu 标识执行上下文，ts_nsec 是单调时间戳，
 * num_stack_entries 给出 stack_entries[] 有效前缀。metadata 锁保护整组一致性；
 * 地址只用于符号化，不持有 task、模块或栈 depot 引用。
 */
struct kfence_track {
	pid_t pid;
	int cpu;
	u64 ts_nsec;
	int num_stack_entries;
	unsigned long stack_entries[KFENCE_STACK_DEPTH];
};

/* KFENCE metadata per guarded allocation. */
/* 每个受 guard 页保护的槽位恰有一份 metadata，数组内核全寿命存在。 */
struct kfence_metadata {
	struct list_head list __guarded_by(&kfence_freelist_lock);	/* Freelist node. */
	/* freelist 嵌入节点；只有持 kfence_freelist_lock 才能链接、摘除或检查归属。 */
	struct rcu_head rcu_head;	/* For delayed freeing. */
	/* RCU 延迟释放回调载体；不等于 metadata 自身被释放，而是延迟对象槽重新可用。 */

	/*
	 * Lock protecting below data; to ensure consistency of the below data,
	 * since the following may execute concurrently: __kfence_alloc(),
	 * __kfence_free(), kfence_handle_page_fault(). However, note that we
	 * cannot grab the same metadata off the freelist twice, and multiple
	 * __kfence_alloc() cannot run concurrently on the same metadata.
	 */
	/*
	 * raw lock 保护以下同一对象生命周期快照。分配、释放与页故障可并发，必须让
	 * state/address/track/cache/unprotected_page 相互对应；freelist 锁保证同一
	 * metadata 不会被两个 allocator 同时领取，所以两个 alloc 无需在此竞争。
	 */
	raw_spinlock_t lock;

	/* The current state of the object; see above. */
	/* 当前状态机值；普通无锁诊断只能 READ_ONCE，转换必须遵守 metadata lock。 */
	enum kfence_object_state state;

	/*
	 * Allocated object address; cannot be calculated from size, because of
	 * alignment requirements.
	 *
	 * Invariant: ALIGN_DOWN(addr, PAGE_SIZE) is constant.
	 */
	/*
	 * 最近一次分配返回的实际对象地址；对齐要求会使它不等于对象页首，不能由 size
	 * 反推。整个槽生命周期内 ALIGN_DOWN(addr, PAGE_SIZE) 固定为其对象页。
	 */
	unsigned long addr;

	/*
	 * The size of the original allocation.
	 */
	/* 最近一次请求的用户可用字节数，用于区分对象内访问与左右越界。 */
	size_t size;

	/*
	 * The kmem_cache cache of the last allocation; NULL if never allocated
	 * or the cache has already been destroyed.
	 */
	/*
	 * 最近分配所属 cache 的借用指针；从未分配或 cache 销毁后为 NULL。持 meta->lock
	 * 保证字段快照一致，但 cache 销毁路径仍须先清除此指针再释放 cache。
	 */
	struct kmem_cache *cache;

	/*
	 * In case of an invalid access, the page that was unprotected; we
	 * optimistically only store one address.
	 */
	/*
	 * fault handler 为继续执行临时解除保护的一个页地址，0 表示无；释放路径会清零内容、
	 * 重新保护该页并把字段归零。只保存一个是诊断/恢复折中，多页异常不保证全部记录。
	 */
	unsigned long unprotected_page __guarded_by(&lock);

	/* Allocation and free stack information. */
	/* 同一生命周期的分配/释放快照，读写都要求 meta->lock。 */
	struct kfence_track alloc_track __guarded_by(&lock);
	struct kfence_track free_track __guarded_by(&lock);
	/* For updating alloc_covered on frees. */
	/* 分配栈哈希用于 counting Bloom filter；free 时用同值抵消覆盖计数。 */
	u32 alloc_stack_hash __guarded_by(&lock);
#ifdef CONFIG_MEMCG
	/* 可选 memcg/slab 对象扩展，随槽位状态一起迁移和清理。 */
	struct slabobj_ext obj_exts;
#endif
};

/* metadata 数组按配置槽位数计算并页对齐，便于启动期池布局和页级映射。 */
#define KFENCE_METADATA_SIZE PAGE_ALIGN(sizeof(struct kfence_metadata) * \
					CONFIG_KFENCE_NUM_OBJECTS)

/* 指向 CONFIG_KFENCE_NUM_OBJECTS 个内核全寿命 metadata；初始化完成后只替换字段不换数组。 */
extern struct kfence_metadata *kfence_metadata;

/*
 * addr_to_metadata() - 把 KFENCE 池内对象/guard 地址换算为槽位 metadata。
 * 业务背景：free、对象查询和 page-fault 慢路径先验证地址属于专用池，再按两页一槽布局定位。
 * 入参：addr 是内核虚拟地址数值，纯输入，可为池边缘或普通地址，不建立对象引用。
 * 出参/返回：有效槽返回借用 metadata 指针，否则 NULL；不锁定 metadata 或改变状态。
 * 注意事项：仅慢路径调用；返回后若要读取一致生命周期字段，调用者必须取得 meta->lock。
 */
static inline struct kfence_metadata *addr_to_metadata(unsigned long addr)
{
	/* index 是候选槽号，允许先得到负数/越界值，再统一边界检查。 */
	long index;

	/* The checks do not affect performance; only called from slow-paths. */
	/* 完整检查位于慢路径，不牺牲普通 slab 分配性能；范围外立即返回 NULL。 */

	if (!is_kfence_address((void *)addr))
		return NULL;

	/*
	 * May be an invalid index if called with an address at the edge of
	 * __kfence_pool, in which case we would report an "invalid access"
	 * error.
	 */
	/*
	 * 池边缘 guard 地址通过范围测试后仍可能算出 -1 或末端越界槽；这类访问应报告
	 * invalid access，而不是越界索引 metadata 数组。
	 */
	index = (addr - (unsigned long)__kfence_pool) / (PAGE_SIZE * 2) - 1;
	if (index < 0 || index >= CONFIG_KFENCE_NUM_OBJECTS)
		return NULL;

	return &kfence_metadata[index];
}

/* KFENCE error types for report generation. */
/* 以下值描述检测到的内存错误原因，由 fault/free/canary 路径设置、report 格式化。 */
enum kfence_error_type {
	KFENCE_ERROR_OOB,		/* Detected a out-of-bounds access. */
	/* guard fault 可归因于已分配对象边界之外的读写。 */
	KFENCE_ERROR_UAF,		/* Detected a use-after-free access. */
	/* 对象已处于 FREED/RCU_FREEING 后仍被访问。 */
	KFENCE_ERROR_CORRUPTION,	/* Detected a memory corruption on free. */
	/* free 时 canary 已变化，说明未触发 guard fault 的邻接越界写。 */
	KFENCE_ERROR_INVALID,		/* Invalid access of unknown type. */
	/* 地址属于/靠近池，但没有足够 metadata 将其可靠分类。 */
	KFENCE_ERROR_INVALID_FREE,	/* Invalid free. */
	/* free 地址、状态或 cache 契约不匹配，不能按正常对象释放。 */
};

/* 报告生成与最终处置分离：返回值由运行策略选择继续、oops 或 panic。 */
enum kfence_fault {
	/* 无需额外动作，常用于已抑制/仅打印完成的情况。 */
	KFENCE_FAULT_NONE,
	/* 报告但允许调用路径继续，由 handler 完成非致命处置。 */
	KFENCE_FAULT_REPORT,
	/* 触发 oops 语义，是否继续由全局 panic_on_oops 等策略决定。 */
	KFENCE_FAULT_OOPS,
	/* 错误不可恢复，handler 直接 panic。 */
	KFENCE_FAULT_PANIC,
};

/*
 * kfence_report_error() - 格式化一次 KFENCE 错误并返回后续处置策略。
 * 业务背景：page-fault、invalid-free 与 canary 检查在持有一致 metadata 快照时调用，
 * 输出诊断后把全局 kfence.fault 策略交给 kfence_handle_fault() 执行。
 * 入参：address/is_write 描述故障；regs 可空；meta 可空且非空时调用者须持其 lock；
 * type 是上方错误分类。返回 NONE/REPORT/OOPS/PANIC 策略，不取得 meta/regs 引用。
 * 注意事项：除 INVALID 外 meta 不可空；报告会 printk、trace、taint 并短暂关闭 lockdep，
 * 不是无副作用查询，严重内存破坏下输出可能不完整。
 */
enum kfence_fault
kfence_report_error(unsigned long address, bool is_write, struct pt_regs *regs,
		    const struct kfence_metadata *meta, enum kfence_error_type type);

/*
 * kfence_handle_fault() - 执行 report 返回的 NONE/REPORT/OOPS/PANIC 策略。
 * 业务背景：报告完成后由 fault/free 调用者统一决定继续、oops 或 panic。
 * 入参：fault 为完整枚举值；无返回，可能继续、触发 BUG/oops 或不返回地 panic。
 * 不接管 metadata；应在完成必要锁释放/页保护处理后调用，避免终止路径持锁。
 */
void kfence_handle_fault(enum kfence_fault fault);

/*
 * kfence_print_object() - 向 seq_file 或错误日志输出一个 metadata 一致快照。
 * 业务背景：debugfs 对象遍历和错误报告共用同一格式，避免两套生命周期解释漂移。
 * 入参：seq 可为 NULL（使用 printk）；meta 为借用非 NULL，调用者必须持 meta->lock。
 * 返回无，不改变状态/ownership；持 raw lock 时格式化不得进入可睡眠路径。
 */
void kfence_print_object(struct seq_file *seq, const struct kfence_metadata *meta) __must_hold(&meta->lock);

/* 结束 KFENCE core/report 私有契约的防重复包含范围。 */
#endif /* MM_KFENCE_KFENCE_H */
