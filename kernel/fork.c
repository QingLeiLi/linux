// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/kernel/fork.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also entry.S and others).
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/memory.c': 'copy_page_range()'
 */

/*
 * fork.c 是 Linux 内核进程创建的核心模块，负责实现所有进程/线程的创建逻辑。
 * 每次 fork()、vfork()、clone() 系统调用最终都会走到这个文件中的函数。
 *
 * 与 Android 的关系：
 *   Android 的进程模型以 Zygote 为核心。系统启动时，Zygote 进程预加载 ART
 *   虚拟机和常用类库，之后每当启动一个 App，ActivityManagerService 通过
 *   socket 通知 Zygote 调用 fork()，由此克隆出新的 App 进程。这个 fork()
 *   最终就是由本文件的函数实现的。fork 比重新创建进程快得多，因为 COW
 *   写时复制机制让父子进程初始共享内存页。
 *
 * 核心函数调用链：
 *   用户态 fork()/clone()/clone3() 系统调用
 *     → kernel_clone()       — 统一入口，解析 clone_flags
 *       → copy_process()     — 核心：完成进程描述符复制、资源复制、安全设置
 *         → dup_task_struct()     — 分配新 task_struct 和内核栈
 *         → copy_creds()          — 复制/继承凭证（uid/gid/capabilities）
 *         → security_task_alloc() — LSM 分配安全标签（SELinux domain）
 *         → copy_mm()             — 复制内存空间（COW 或共享）
 *         → copy_namespaces()     — 复制/共享命名空间（隔离边界）
 *         → copy_thread()         — 复制 CPU 寄存器状态
 *         → alloc_pid()           — 分配新 PID
 *
 * 安全边界说明：
 *   - copy_creds()：决定子进程继承哪些权限，是权限隔离的关键点
 *   - copy_namespaces()：决定子进程看到哪个 PID/网络/文件系统视图，
 *     容器（Docker/Android 沙箱）隔离的基础
 *   - security_task_alloc()：SELinux 在此为子进程打上安全标签，
 *     Android 中每个 App 有独立的 SELinux domain
 */

#include <linux/anon_inodes.h>
#include <linux/slab.h>
#include <linux/sched/autogroup.h>
#include <linux/sched/mm.h>
#include <linux/sched/user.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/stat.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/cputime.h>
#include <linux/sched/ext.h>
#include <linux/sched/exec_state.h>
#include <linux/seq_file.h>
#include <linux/rtmutex.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/mempolicy.h>
#include <linux/sem.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/iocontext.h>
#include <linux/key.h>
#include <linux/kmsan.h>
#include <linux/binfmts.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/memblock.h>
#include <linux/nsproxy.h>
#include <linux/ns/ns_common_types.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cgroup.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/seccomp.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/syscall_user_dispatch.h>
#include <linux/jiffies.h>
#include <linux/futex.h>
#include <linux/compat.h>
#include <linux/kthread.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/rcupdate.h>
#include <linux/ptrace.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/memcontrol.h>
#include <linux/ftrace.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/rmap.h>
#include <linux/ksm.h>
#include <linux/acct.h>
#include <linux/userfaultfd_k.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/freezer.h>
#include <linux/delayacct.h>
#include <linux/taskstats_kern.h>
#include <linux/tty.h>
#include <linux/fs_struct.h>
#include <linux/magic.h>
#include <linux/perf_event.h>
#include <linux/posix-timers.h>
#include <linux/user-return-notifier.h>
#include <linux/oom.h>
#include <linux/khugepaged.h>
#include <linux/signalfd.h>
#include <linux/uprobes.h>
#include <linux/aio.h>
#include <linux/compiler.h>
#include <linux/sysctl.h>
#include <linux/kcov.h>
#include <linux/livepatch.h>
#include <linux/thread_info.h>
#include <linux/kstack_erase.h>
#include <linux/kasan.h>
#include <linux/randomize_kstack.h>
#include <linux/scs.h>
#include <linux/io_uring.h>
#include <linux/io_uring_types.h>
#include <linux/bpf.h>
#include <linux/stackprotector.h>
#include <linux/user_events.h>
#include <linux/iommu.h>
#include <linux/rseq.h>
#include <uapi/linux/pidfd.h>
#include <linux/pidfs.h>
#include <linux/tick.h>
#include <linux/unwind_deferred.h>
#include <linux/pgalloc.h>
#include <linux/uaccess.h>

#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

/* For dup_mmap(). */
#include "../mm/internal.h"

#include <trace/events/sched.h>

#define CREATE_TRACE_POINTS
#include <trace/events/task.h>

#include <kunit/visibility.h>

/*
 * Minimum number of threads to boot the kernel
 */
#define MIN_THREADS 20

/*
 * Maximum number of threads
 */
#define MAX_THREADS FUTEX_TID_MASK

/*
 * Protected counters by write_lock_irq(&tasklist_lock)
 */
unsigned long total_forks;	/* Handle normal Linux uptimes. */
int nr_threads;			/* The idle threads do not count.. */

static int max_threads;		/* tunable limit on nr_threads */

#define NAMED_ARRAY_INDEX(x)	[x] = __stringify(x)

static const char * const resident_page_types[] = {
	NAMED_ARRAY_INDEX(MM_FILEPAGES),
	NAMED_ARRAY_INDEX(MM_ANONPAGES),
	NAMED_ARRAY_INDEX(MM_SWAPENTS),
	NAMED_ARRAY_INDEX(MM_SHMEMPAGES),
};

DEFINE_PER_CPU(unsigned long, process_counts) = 0;

__cacheline_aligned DEFINE_RWLOCK(tasklist_lock);  /* outer */

#ifdef CONFIG_PROVE_RCU
int lockdep_tasklist_lock_is_held(void)
{
	return lockdep_is_held(&tasklist_lock);
}
EXPORT_SYMBOL_GPL(lockdep_tasklist_lock_is_held);
#endif /* #ifdef CONFIG_PROVE_RCU */

/*
 * nr_processes - 返回系统中当前进程（线程组）的总数
 *
 * 背景：Linux 内核为每个 CPU 维护一个 per-CPU 计数器 process_counts，
 * 在 fork/exit 时分别递增/递减，避免全局锁争用。
 * 该函数通过遍历所有可能的 CPU 并累加各自的计数器来得到全局总数。
 *
 * 注意：返回值是一个近似值——在读取各 CPU 计数器的间隙中，
 * 其他 CPU 上可能正在发生 fork/exit，因此结果并非严格一致性快照。
 */
int nr_processes(void)
{
	int cpu;
	int total = 0;

	for_each_possible_cpu(cpu)
		total += per_cpu(process_counts, cpu); /* 累加每个 CPU 上的进程计数 */

	return total;
}

void __weak arch_release_task_struct(struct task_struct *tsk)
{
}

/* task_struct_cachep：专用 SLAB 缓存，用于快速分配/释放 task_struct 对象 */
static struct kmem_cache *task_struct_cachep;

/*
 * alloc_task_struct_node - 在指定 NUMA 节点上从 SLAB 缓存分配一个 task_struct
 *
 * @node: 期望分配内存所在的 NUMA 节点编号，传入 NUMA_NO_NODE 表示不限节点
 *
 * 使用专用 SLAB 缓存（task_struct_cachep）而非通用 kmalloc，
 * 可以减少内存碎片，并利用 SLAB 着色（coloring）提高 CPU 缓存利用率。
 */
static inline struct task_struct *alloc_task_struct_node(int node)
{
	return kmem_cache_alloc_node(task_struct_cachep, GFP_KERNEL, node); /* 从指定节点的缓存中分配 */
}

/*
 * free_task_struct - 将 task_struct 归还给 SLAB 缓存
 *
 * @tsk: 待释放的任务描述符指针
 *
 * 对应 alloc_task_struct_node()，将对象归还缓存而非直接释放页面，
 * 下次分配时可直接复用，避免重复初始化开销。
 */
static inline void free_task_struct(struct task_struct *tsk)
{
	kmem_cache_free(task_struct_cachep, tsk); /* 将 task_struct 归还给专用缓存 */
}

/*
 * CONFIG_VMAP_STACK：使用虚拟地址映射（vmalloc）分配内核栈
 *
 * 背景：传统内核栈通过 alloc_pages() 分配连续物理页面。
 * VMAP_STACK 改为使用 vmalloc 区域，带来以下优势：
 *   1. 可在虚拟地址空间两侧放置保护页（guard page），检测栈溢出；
 *   2. 不要求物理上连续，降低大内存系统的分配压力；
 *   3. 支持 per-CPU 缓存，减少频繁 vmalloc/vfree 的 TLB 刷新开销。
 * 代价是：vmalloc() 本身比 alloc_pages() 慢，且每次 vfree() 可能触发 TLB flush。
 * 因此内核在每个 CPU 上缓存最多 NR_CACHED_STACKS 个已释放的栈，供下次 fork 复用。
 */
#ifdef CONFIG_VMAP_STACK
/*
 * vmalloc() is a bit slow, and calling vfree() enough times will force a TLB
 * flush.  Try to minimize the number of calls by caching stacks.
 */
#define NR_CACHED_STACKS 2  /* 每个 CPU 缓存的内核栈数量上限 */
static DEFINE_PER_CPU(struct vm_struct *, cached_stacks[NR_CACHED_STACKS]); /* per-CPU 栈缓存数组 */
/*
 * Allocated stacks are cached and later reused by new threads, so memcg
 * accounting is performed by the code assigning/releasing stacks to tasks.
 * We need a zeroed memory without __GFP_ACCOUNT.
 */
/* GFP_VMAP_STACK：分配 vmap 栈的内存标志：普通内核分配 + 清零 + 跳过 KASAN 投毒 */
#define GFP_VMAP_STACK (GFP_KERNEL | __GFP_ZERO | __GFP_SKIP_KASAN)

/*
 * vm_stack：用于延迟释放 vmap 内核栈的辅助结构
 *
 * 内核栈在被释放时不能立刻调用 vfree()，因为可能仍有 RCU 读者持有引用。
 * 通过将释放操作嵌入 RCU 回调（rcu_head），保证在所有读者退出后才真正释放。
 */
struct vm_stack {
	struct rcu_head rcu;            /* RCU 回调头，用于延迟释放 */
	struct vm_struct *stack_vm_area; /* 指向对应的 vm_struct 元数据 */
};

/*
 * alloc_thread_stack_node_from_cache - 尝试从 per-CPU 缓存中取出一个可用的内核栈
 *
 * @tsk:  正在创建的新任务（当前未使用，预留扩展）
 * @node: 期望的 NUMA 节点；NUMA_NO_NODE 表示不限节点
 * 返回值：成功返回 vm_struct 指针，缓存为空或节点不匹配时返回 NULL
 *
 * 实现思路：
 *   - 在禁止抢占的保护域（scoped_guard(preempt)）内操作，保证当前 CPU 不变；
 *   - 若指定了 node 且当前 CPU 所在节点不匹配，直接放弃缓存（保证 NUMA 局部性）；
 *   - 用原子 xchg 将缓存槽置 NULL 并取出旧值，避免锁竞争。
 *
 * 注意：即使成功取出缓存栈，调用者仍可能被迁移到其他 NUMA 节点，
 * 所以该函数只能尽力保证局部性，而非绝对保证。
 */
static struct vm_struct *alloc_thread_stack_node_from_cache(struct task_struct *tsk, int node)
{
	struct vm_struct *vm_area;
	unsigned int i;

	/*
	 * If the node has memory, we are guaranteed the stacks are backed by local pages.
	 * Otherwise the pages are arbitrary.
	 *
	 * Note that depending on cpuset it is possible we will get migrated to a different
	 * node immediately after allocating here, so this does *not* guarantee locality for
	 * arbitrary callers.
	 */
	scoped_guard(preempt) { /* 禁止抢占，确保 this_cpu 操作期间 CPU 不变 */
		if (node != NUMA_NO_NODE && numa_node_id() != node)
			return NULL; /* 当前 CPU 不在目标 NUMA 节点，拒绝使用缓存 */

		for (i = 0; i < NR_CACHED_STACKS; i++) {
			vm_area = this_cpu_xchg(cached_stacks[i], NULL); /* 原子取出缓存槽 */
			if (vm_area)
				return vm_area; /* 命中缓存，直接返回 */
		}
	}

	return NULL; /* 缓存已空，需要重新分配 */
}

/*
 * try_release_thread_stack_to_cache - 尝试将内核栈归还给 per-CPU 缓存
 *
 * @vm_area: 待归还的栈对应的 vm_struct
 * 返回值：true 表示成功入缓存，false 表示缓存已满或 NUMA 不匹配（需调用者释放）
 *
 * 实现思路：
 *   1. NUMA 检查：若当前节点有本地内存（N_MEMORY），则要求栈的所有物理页都在本地节点；
 *      否则缓存跨节点的栈会在下次复用时造成远端内存访问，得不偿失。
 *      若当前节点没有本地内存，则不做 NUMA 过滤，仅避免反复调用 vmalloc。
 *   2. 用 cmpxchg 原子地将空缓存槽替换为 vm_area，若全部槽位都已被占用则返回 false。
 *
 * 注意：返回 false 时，调用者必须负责调用 vfree() 释放该栈内存。
 */
static bool try_release_thread_stack_to_cache(struct vm_struct *vm_area)
{
	unsigned int i;
	int nid;

	/*
	 * Don't cache stacks if any of the pages don't match the local domain, unless
	 * there is no local memory to begin with.
	 *
	 * Note that lack of local memory does not automatically mean it makes no difference
	 * performance-wise which other domain backs the stack. In this case we are merely
	 * trying to avoid constantly going to vmalloc.
	 */
	scoped_guard(preempt) { /* 禁止抢占，保证 NUMA 节点检测与缓存操作的原子性 */
		nid = numa_node_id(); /* 获取当前 CPU 所在 NUMA 节点 */
		if (node_state(nid, N_MEMORY)) { /* 仅当本地节点有内存时才做 NUMA 过滤 */
			for (i = 0; i < vm_area->nr_pages; i++) {
				struct page *page = vm_area->pages[i];
				if (page_to_nid(page) != nid)
					return false; /* 存在远端内存页，不缓存 */
			}
		}

		for (i = 0; i < NR_CACHED_STACKS; i++) {
			struct vm_struct *tmp = NULL;

			/* 仅当缓存槽为空时才写入，避免覆盖已有缓存 */
			if (this_cpu_try_cmpxchg(cached_stacks[i], &tmp, vm_area))
				return true; /* 成功入缓存 */
		}
	}
	return false; /* 所有缓存槽已满，无法入缓存 */
}

/*
 * thread_stack_free_rcu - RCU 宽限期结束后真正释放 vmap 内核栈
 *
 * @rh: 嵌入在 vm_stack 中的 RCU 回调头
 *
 * 该函数在所有 RCU 读者都退出之后由内核异步调用。
 * 先尝试将栈归还 per-CPU 缓存；若缓存已满则调用 vfree() 释放虚拟内存映射。
 * vfree() 会解除物理页映射并刷新 TLB，开销较大，因此优先复用缓存。
 */
static void thread_stack_free_rcu(struct rcu_head *rh)
{
	struct vm_stack *vm_stack = container_of(rh, struct vm_stack, rcu); /* 从 rcu_head 反推 vm_stack */
	struct vm_struct *vm_area = vm_stack->stack_vm_area;

	if (try_release_thread_stack_to_cache(vm_stack->stack_vm_area))
		return; /* 成功入缓存，延迟实际释放 */

	vfree(vm_area->addr); /* 缓存已满，立即释放 vmap 映射和物理页 */
}

/*
 * thread_stack_delayed_free - 将内核栈的释放推迟到 RCU 宽限期之后
 *
 * @tsk: 栈即将被释放的任务
 *
 * 由于任务退出后仍可能存在 RCU 读者持有对栈内数据的引用，
 * 不能立即调用 vfree()。通过 call_rcu() 将真正的释放操作（thread_stack_free_rcu）
 * 延迟到所有 RCU 读者都完成之后执行。
 *
 * 技巧：vm_stack 结构体复用了栈内存本身（tsk->stack 指向栈底，
 * 同时也是 vm_stack 的起始地址），节省了额外的内存分配。
 */
static void thread_stack_delayed_free(struct task_struct *tsk)
{
	struct vm_stack *vm_stack = tsk->stack; /* vm_stack 复用了栈内存本身的起始位置 */

	vm_stack->stack_vm_area = tsk->stack_vm_area; /* 保存 vm_struct 元数据指针，供 RCU 回调使用 */
	call_rcu(&vm_stack->rcu, thread_stack_free_rcu); /* 注册 RCU 回调，延迟释放 */
}

/*
 * free_vm_stack_cache - 清空指定 CPU 的 vmap 栈缓存
 *
 * @cpu: 目标 CPU 编号
 * 返回值：始终返回 0（符合 CPU hotplug 回调约定）
 *
 * 该函数注册为 CPU 下线（CPU_DEAD）时的回调，在 CPU 被热拔除前
 * 将其缓存的所有 vmap 内核栈释放，防止内存泄漏。
 * 直接调用 vfree() 而非延迟释放，因为此时该 CPU 已停止运行，
 * 不存在并发访问缓存的风险。
 */
static int free_vm_stack_cache(unsigned int cpu)
{
	struct vm_struct **cached_vm_stack_areas = per_cpu_ptr(cached_stacks, cpu); /* 获取目标 CPU 的缓存数组 */
	int i;

	for (i = 0; i < NR_CACHED_STACKS; i++) {
		struct vm_struct *vm_area = cached_vm_stack_areas[i];

		if (!vm_area)
			continue; /* 该槽位为空，跳过 */

		vfree(vm_area->addr);          /* 释放 vmap 映射及物理页 */
		cached_vm_stack_areas[i] = NULL; /* 清空缓存槽，防止悬空指针 */
	}

	return 0;
}

/*
 * memcg_charge_kernel_stack - 对 vmap 内核栈的所有物理页执行 memcg 记账
 *
 * @vm_area: 内核栈对应的 vm_struct，其 pages[] 数组包含所有物理页
 * 返回值：0 表示成功，负值表示 memcg 限额超出（-ENOMEM）
 *
 * 背景：使用 vmap 分配内核栈时，物理页通过 GFP_VMAP_STACK（含 __GFP_SKIP_KASAN 但
 * 不含 __GFP_ACCOUNT）分配，因此不会在分配时自动记账到 memcg。
 * 只有在将栈绑定到具体任务时才进行记账，释放时（exit_task_stack_account）才取消记账，
 * 这样可以正确反映"哪个 cgroup 实际在使用该栈"。
 *
 * 错误处理：若中途某页记账失败，回滚所有已记账的页面，保证原子性。
 */
static int memcg_charge_kernel_stack(struct vm_struct *vm_area)
{
	int i;
	int ret;
	int nr_charged = 0; /* 已成功记账的页数，用于出错时回滚 */

	BUG_ON(vm_area->nr_pages != THREAD_SIZE / PAGE_SIZE); /* 断言页面数与栈大小一致 */

	for (i = 0; i < THREAD_SIZE / PAGE_SIZE; i++) {
		ret = memcg_kmem_charge_page(vm_area->pages[i], GFP_KERNEL, 0); /* 逐页记账 */
		if (ret)
			goto err; /* 记账失败，回滚 */
		nr_charged++;
	}
	return 0;
err:
	/* 记账失败，回滚所有已成功记账的页面 */
	for (i = 0; i < nr_charged; i++)
		memcg_kmem_uncharge_page(vm_area->pages[i], 0);
	return ret;
}

/*
 * alloc_thread_stack_node（CONFIG_VMAP_STACK 版本）
 * - 为新任务在指定 NUMA 节点上分配 vmap 内核栈
 *
 * @tsk:  新建任务的 task_struct
 * @node: 期望的 NUMA 节点；NUMA_NO_NODE 表示不限节点
 * 返回值：0 表示成功，-ENOMEM 表示内存不足
 *
 * 分配策略（两阶段）：
 *   1. 优先从 per-CPU 缓存取一个已映射好的栈（快路径），
 *      避免重新调用 vmalloc() 和重建页表映射；
 *   2. 缓存为空时降级到 __vmalloc_node()（慢路径），
 *      并立即缓存 vm_struct 指针，因为 free_thread_stack()
 *      可能在中断上下文调用，而 find_vm_area() 不能在中断中使用。
 *
 * KASAN 处理：
 *   - 软件 KASAN：需要主动解毒（unpoison）复用栈的内存范围；
 *   - 硬件 KASAN（MTE）：由硬件处理，不需要软件解毒。
 *   kasan_reset_tag() 清除地址上的 KASAN 标签，确保后续访问不触发误报。
 */
static int alloc_thread_stack_node(struct task_struct *tsk, int node)
{
	struct vm_struct *vm_area;
	void *stack;

	vm_area = alloc_thread_stack_node_from_cache(tsk, node); /* 尝试快路径：从缓存取栈 */
	if (vm_area) {
		if (memcg_charge_kernel_stack(vm_area)) { /* 将栈页面记账到当前 memcg */
			vfree(vm_area->addr); /* 记账失败，释放缓存取出的栈 */
			return -ENOMEM;
		}

		/* Reset stack metadata. */
		if (!kasan_hw_tags_enabled())
			kasan_unpoison_range(vm_area->addr, THREAD_SIZE); /* 软件 KASAN：解毒复用栈 */

		stack = kasan_reset_tag(vm_area->addr); /* 清除 KASAN 地址标签 */

		/* Clear stale pointers from reused stack. */
		clear_pages(vm_area->addr, vm_area->nr_pages); /* 清零复用栈，防止信息泄漏 */

		tsk->stack_vm_area = vm_area; /* 缓存 vm_struct，供中断上下文释放时使用 */
		tsk->stack = stack;
		return 0;
	}

	/* 慢路径：缓存为空，重新通过 vmalloc 分配 */
	stack = __vmalloc_node(THREAD_SIZE, THREAD_ALIGN,
				     GFP_VMAP_STACK,
				     node, __builtin_return_address(0));
	if (!stack)
		return -ENOMEM;

	vm_area = find_vm_area(stack); /* 根据虚拟地址找到对应的 vm_struct */
	if (memcg_charge_kernel_stack(vm_area)) {
		vfree(stack); /* memcg 记账失败，释放刚分配的栈 */
		return -ENOMEM;
	}
	/*
	 * We can't call find_vm_area() in interrupt context, and
	 * free_thread_stack() can be called in interrupt context,
	 * so cache the vm_struct.
	 */
	/* 提前缓存 vm_struct：free_thread_stack() 可能在中断上下文调用，
	 * 而 find_vm_area() 不可在中断中使用，故此处提前保存。 */
	tsk->stack_vm_area = vm_area;
	stack = kasan_reset_tag(stack); /* 清除 KASAN 地址标签 */
	tsk->stack = stack;
	return 0;
}

/*
 * free_thread_stack（CONFIG_VMAP_STACK 版本）
 * - 释放 vmap 内核栈，优先归还缓存，否则延迟释放
 *
 * @tsk: 即将销毁的任务
 *
 * 两步清理：
 *   1. 尝试将 vm_struct 归还 per-CPU 缓存（try_release_thread_stack_to_cache）；
 *      若失败（缓存满或 NUMA 不匹配），通过 call_rcu() 延迟真正的 vfree()。
 *   2. 无论成功与否，立即将 tsk->stack 和 tsk->stack_vm_area 清 NULL，
 *      避免后续误用悬空指针。
 */
static void free_thread_stack(struct task_struct *tsk)
{
	if (!try_release_thread_stack_to_cache(tsk->stack_vm_area))
		thread_stack_delayed_free(tsk); /* 缓存失败，通过 RCU 延迟释放 */

	tsk->stack = NULL;          /* 清空栈指针，防止悬空引用 */
	tsk->stack_vm_area = NULL;  /* 清空 vm_struct 指针 */
}

/*
 * 非 VMAP_STACK 路径：使用传统方式分配内核栈
 *
 * 在未开启 CONFIG_VMAP_STACK 的情况下，内核栈通过普通页面分配器或 SLAB 缓存分配，
 * 不使用虚拟地址映射，因此：
 *   - 没有 vmap 保护页（guard page），栈溢出不会被立即检测；
 *   - 分配/释放性能更高（无 TLB flush 开销）；
 *   - 根据 THREAD_SIZE 与 PAGE_SIZE 的关系选择不同后端：
 *       THREAD_SIZE >= PAGE_SIZE：使用 alloc_pages（buddy 分配器）
 *       THREAD_SIZE <  PAGE_SIZE：使用 kmem_cache（SLAB 分配器）
 *
 * 释放同样需要延迟（call_rcu），原因与 VMAP_STACK 版本相同：
 * 可能存在 RCU 读者仍然引用栈上的数据。
 */
#else /* !CONFIG_VMAP_STACK */

/*
 * Allocate pages if THREAD_SIZE is >= PAGE_SIZE, otherwise use a
 * kmemcache based allocator.
 */
#if THREAD_SIZE >= PAGE_SIZE

/*
 * thread_stack_free_rcu（非 VMAP，大栈版本）
 * - RCU 宽限期结束后，通过 __free_pages() 释放内核栈物理页
 *
 * 技巧：rcu_head 直接复用了栈顶内存（tsk->stack 指针处），
 * 利用 virt_to_page() 将虚拟地址转为 struct page，再批量释放。
 */
static void thread_stack_free_rcu(struct rcu_head *rh)
{
	__free_pages(virt_to_page(rh), THREAD_SIZE_ORDER); /* 释放 2^THREAD_SIZE_ORDER 个物理页 */
}

/*
 * thread_stack_delayed_free（非 VMAP，大栈版本）
 * - 将内核栈释放推迟到 RCU 宽限期结束后执行
 *
 * rcu_head 复用了栈内存本身，无需额外分配。
 */
static void thread_stack_delayed_free(struct task_struct *tsk)
{
	struct rcu_head *rh = tsk->stack; /* rcu_head 嵌入在栈内存的起始处 */

	call_rcu(rh, thread_stack_free_rcu); /* 注册 RCU 回调，延迟释放栈页面 */
}

/*
 * alloc_thread_stack_node（非 VMAP，大栈版本）
 * - 从指定 NUMA 节点分配连续物理页作为内核栈
 *
 * @tsk:  新建任务的 task_struct
 * @node: 期望的 NUMA 节点
 * 返回值：0 成功，-ENOMEM 分配失败
 *
 * 使用 alloc_pages_node() + THREADINFO_GFP 标志，
 * THREAD_SIZE_ORDER 决定分配 2^n 个物理页（通常 2 或 4 页）。
 */
static int alloc_thread_stack_node(struct task_struct *tsk, int node)
{
	struct page *page = alloc_pages_node(node, THREADINFO_GFP,
					     THREAD_SIZE_ORDER); /* 分配物理连续页 */

	if (likely(page)) {
		tsk->stack = kasan_reset_tag(page_address(page)); /* 获取虚拟地址并清除 KASAN 标签 */
		return 0;
	}
	return -ENOMEM;
}

/*
 * free_thread_stack（非 VMAP，大栈版本）
 * - 触发 RCU 延迟释放并清空栈指针
 */
static void free_thread_stack(struct task_struct *tsk)
{
	thread_stack_delayed_free(tsk); /* 通过 RCU 延迟释放栈页面 */
	tsk->stack = NULL;              /* 立即清空指针，防止悬空引用 */
}

#else /* !(THREAD_SIZE >= PAGE_SIZE) */

/* SLAB 缓存，用于 THREAD_SIZE < PAGE_SIZE 场景下的内核栈分配 */
static struct kmem_cache *thread_stack_cache;

/*
 * thread_stack_free_rcu（非 VMAP，小栈版本）
 * - RCU 宽限期结束后将栈内存归还 SLAB 缓存
 */
static void thread_stack_free_rcu(struct rcu_head *rh)
{
	kmem_cache_free(thread_stack_cache, rh); /* 归还栈内存到 SLAB 缓存 */
}

/*
 * thread_stack_delayed_free（非 VMAP，小栈版本）
 * - 将内核栈释放推迟到 RCU 宽限期结束后执行
 *
 * 同样复用栈内存起始位置存放 rcu_head，无需额外分配。
 */
static void thread_stack_delayed_free(struct task_struct *tsk)
{
	struct rcu_head *rh = tsk->stack; /* rcu_head 复用栈内存起始处 */

	call_rcu(rh, thread_stack_free_rcu);
}

/*
 * alloc_thread_stack_node（非 VMAP，小栈版本）
 * - 从 SLAB 缓存在指定 NUMA 节点分配内核栈
 *
 * 适用于 THREAD_SIZE < PAGE_SIZE 的架构（如部分嵌入式平台），
 * 使用 SLAB 而非 buddy 分配器以减少内存浪费。
 */
static int alloc_thread_stack_node(struct task_struct *tsk, int node)
{
	unsigned long *stack;
	stack = kmem_cache_alloc_node(thread_stack_cache, THREADINFO_GFP, node); /* 从 SLAB 缓存分配 */
	stack = kasan_reset_tag(stack); /* 清除 KASAN 地址标签 */
	tsk->stack = stack;
	return stack ? 0 : -ENOMEM;
}

/*
 * free_thread_stack（非 VMAP，小栈版本）
 * - 触发 RCU 延迟释放并清空栈指针
 */
static void free_thread_stack(struct task_struct *tsk)
{
	thread_stack_delayed_free(tsk); /* 通过 RCU 延迟释放 */
	tsk->stack = NULL;
}

/*
 * thread_stack_cache_init - 初始化小栈场景下的 SLAB 缓存
 *
 * 在系统启动早期（start_kernel 调用链）调用，创建专用 SLAB 缓存。
 * 使用 kmem_cache_create_usercopy() 而非普通 create，是因为内核栈
 * 可能被 copy_to_user() 等函数访问，需要声明可安全复制的范围（整个栈）。
 * BUG_ON 保证早期启动失败时立即 panic，而不是带着空指针继续运行。
 */
void thread_stack_cache_init(void)
{
	thread_stack_cache = kmem_cache_create_usercopy("thread_stack",
					THREAD_SIZE, THREAD_SIZE, 0, 0,
					THREAD_SIZE, NULL); /* 整个栈范围均可安全复制给用户空间 */
	BUG_ON(thread_stack_cache == NULL); /* 启动时分配失败是不可恢复的错误 */
}

#endif /* THREAD_SIZE >= PAGE_SIZE */
#endif /* CONFIG_VMAP_STACK */

/* SLAB cache for signal_struct structures (tsk->signal) */
static struct kmem_cache *signal_cachep;

/* SLAB cache for sighand_struct structures (tsk->sighand) */
struct kmem_cache *sighand_cachep;

/* SLAB cache for files_struct structures (tsk->files) */
struct kmem_cache *files_cachep;

/* SLAB cache for fs_struct structures (tsk->fs) */
struct kmem_cache *fs_cachep;

/* SLAB cache for mm_struct structures (tsk->mm) */
static struct kmem_cache *mm_cachep;

/*
 * account_kernel_stack - 对内核栈占用的内存进行 LRU 向量（lruvec）统计记账
 *
 * @tsk:     目标任务
 * @account: +1 表示记账（栈分配），-1 表示取消记账（栈释放）
 *
 * 背景：内核需要跟踪每个 NUMA 节点/memcg 上消耗的内核栈内存，
 * 以便在内存压力下做出合理的回收决策（NR_KERNEL_STACK_KB 计数器）。
 *
 * 两种路径：
 *   - VMAP_STACK：栈的各物理页可能来自不同 NUMA 节点，
 *     必须逐页调用 mod_lruvec_page_state() 分别记账；
 *   - 非 VMAP_STACK：栈是连续物理页，属于同一节点，
 *     可以一次性调用 mod_lruvec_kmem_state() 记账整个栈。
 *
 * 单位说明：统计单位为 KB，因此乘以 PAGE_SIZE/1024 或 THREAD_SIZE/1024。
 */
static void account_kernel_stack(struct task_struct *tsk, int account)
{
	if (IS_ENABLED(CONFIG_VMAP_STACK)) {
		struct vm_struct *vm_area = task_stack_vm_area(tsk); /* 获取 vmap 栈的 vm_struct */
		int i;

		/* VMAP 栈各页可能分布在不同节点，逐页记账 */
		for (i = 0; i < THREAD_SIZE / PAGE_SIZE; i++)
			mod_lruvec_page_state(vm_area->pages[i], NR_KERNEL_STACK_KB,
					      account * (PAGE_SIZE / 1024)); /* 以 KB 为单位记账单页 */
	} else {
		void *stack = task_stack_page(tsk);

		/* All stack pages are in the same node. */
		/* 非 VMAP：连续物理页，属同一节点，一次记账整个栈 */
		mod_lruvec_kmem_state(stack, NR_KERNEL_STACK_KB,
				      account * (THREAD_SIZE / 1024)); /* 以 KB 为单位记账整个栈 */
	}
}

/*
 * exit_task_stack_account - 任务退出时撤销内核栈的所有内存记账
 *
 * @tsk: 正在退出的任务
 *
 * 该函数在任务即将销毁时调用，执行两步清理：
 *   1. 调用 account_kernel_stack(tsk, -1) 从 LRU 向量统计中减去该栈的占用；
 *   2. 仅在 VMAP_STACK 下：逐页调用 memcg_kmem_uncharge_page()，
 *      撤销 alloc_thread_stack_node() 中通过 memcg_charge_kernel_stack() 做的 memcg 记账。
 *      （非 VMAP 栈在分配时已含 __GFP_ACCOUNT，由 SLAB/buddy 自动处理取消记账）
 *
 * 注意：该函数必须在 free_thread_stack() 之前调用，因为之后栈内存可能已被释放或归还缓存。
 */
void exit_task_stack_account(struct task_struct *tsk)
{
	account_kernel_stack(tsk, -1); /* 从 LRU 统计中减去内核栈占用 */

	if (IS_ENABLED(CONFIG_VMAP_STACK)) {
		struct vm_struct *vm_area;
		int i;

		vm_area = task_stack_vm_area(tsk);
		/* VMAP 栈需要逐页取消 memcg 记账（对应 memcg_charge_kernel_stack 的逆操作） */
		for (i = 0; i < THREAD_SIZE / PAGE_SIZE; i++)
			memcg_kmem_uncharge_page(vm_area->pages[i], 0);
	}
}

/*
 * release_task_stack - 释放任务的内核栈（仅在任务完全死亡后调用）
 *
 * @tsk: 目标任务，必须处于 TASK_DEAD 状态
 *
 * 安全检查：使用 READ_ONCE() 读取 tsk->__state，避免编译器优化导致的
 * 竞态读取。若状态不是 TASK_DEAD，说明存在提前释放的 bug；
 * 此时宁可泄漏栈（WARN_ON + return）也不能提前释放——提前释放会导致
 * 仍在运行的任务栈被复用，造成内存损坏，远比内存泄漏更危险。
 */
static void release_task_stack(struct task_struct *tsk)
{
	if (WARN_ON(READ_ONCE(tsk->__state) != TASK_DEAD))
		return;  /* Better to leak the stack than to free prematurely */
	         /* 宁可内存泄漏，也不能在任务未死亡时释放栈 */

	free_thread_stack(tsk); /* 触发实际的栈释放（可能延迟到 RCU 宽限期后） */
}

/*
 * put_task_stack（仅 CONFIG_THREAD_INFO_IN_TASK 下编译）
 * - 递减内核栈的引用计数，计数归零时释放栈
 *
 * 背景：当 thread_info 嵌入在 task_struct 内部（CONFIG_THREAD_INFO_IN_TASK）时，
 * 内核栈与 task_struct 是独立的内存对象，需要单独的引用计数管理。
 * stack_refcount 的存在允许在 task_struct 生命周期内安全地持有对栈的引用。
 *
 * refcount_dec_and_test() 是原子操作：递减后若结果为 0，返回 true，
 * 此时由本调用者负责释放栈；否则还有其他持有者，不能释放。
 */
#ifdef CONFIG_THREAD_INFO_IN_TASK
void put_task_stack(struct task_struct *tsk)
{
	if (refcount_dec_and_test(&tsk->stack_refcount)) /* 原子递减，若归零则负责释放 */
		release_task_stack(tsk);
}
#endif

/*
 * free_task - 彻底销毁一个 task_struct 及其关联资源
 *
 * @tsk: 即将被销毁的任务描述符
 *
 * 该函数是任务生命周期的终点，在所有引用者都已放弃对 tsk 的引用后调用。
 * 销毁顺序的约定：
 *   1. seccomp 过滤器必须已被提前清理（WARN_ON_ONCE 检查）；
 *   2. 释放用户空间 CPU 掩码指针（release_user_cpus_ptr）；
 *   3. 释放影子调用栈（scs_release，仅 CONFIG_SHADOW_CALL_STACK）；
 *   4. 释放内核栈（行为因 CONFIG_THREAD_INFO_IN_TASK 不同而异）：
 *      - 未配置 THREAD_INFO_IN_TASK：thread_info 在栈底，两者一起释放；
 *      - 配置了 THREAD_INFO_IN_TASK：栈已通过 put_task_stack() 单独释放，
 *        此处仅做 WARN_ON_ONCE 完整性检查（引用计数必须为 0）；
 *   5. 清理调试、追踪、架构相关及 BPF 资源；
 *   6. 最后调用 free_task_struct() 将 task_struct 归还 SLAB 缓存。
 *
 * 注意：free_task() 通常由 RCU 回调或 delayed_free_task() 异步调用，
 * 不能在持有任何与 tsk 相关锁的情况下调用。
 */
void free_task(struct task_struct *tsk)
{
#ifdef CONFIG_SECCOMP
	WARN_ON_ONCE(tsk->seccomp.filter); /* seccomp 过滤器应已在进程退出时清理 */
#endif
	release_user_cpus_ptr(tsk); /* 释放用户空间 CPU 亲和性掩码的内核副本 */
	scs_release(tsk);           /* 释放影子调用栈（Shadow Call Stack），防止控制流劫持 */

#ifndef CONFIG_THREAD_INFO_IN_TASK
	/*
	 * The task is finally done with both the stack and thread_info,
	 * so free both.
	 */
	/* thread_info 位于栈底，随栈一同释放 */
	release_task_stack(tsk);
#else
	/*
	 * If the task had a separate stack allocation, it should be gone
	 * by now.
	 */
	/* thread_info 嵌入在 task_struct 中，栈已通过 put_task_stack() 单独释放 */
	WARN_ON_ONCE(refcount_read(&tsk->stack_refcount) != 0); /* 栈引用计数必须为 0 */
#endif
	rt_mutex_debug_task_free(tsk);    /* RT 互斥锁调试清理 */
	ftrace_graph_exit_task(tsk);      /* 清理函数图追踪（function graph tracer）状态 */
	arch_release_task_struct(tsk);    /* 架构相关的 task_struct 清理钩子 */
	if (tsk->flags & PF_KTHREAD)
		free_kthread_struct(tsk);  /* 释放内核线程专用的 kthread 结构 */
	bpf_task_storage_free(tsk);       /* 释放 BPF task-local 存储 */
	put_task_exec_state(rcu_access_pointer(tsk->exec_state)); /* 释放可执行状态引用 */
	free_task_struct(tsk);            /* 将 task_struct 归还 SLAB 缓存，这是最后一步 */
}
EXPORT_SYMBOL(free_task);

/*
 * 【dup_mm_exe_file】将父进程的可执行文件引用复制给子进程的 mm
 *
 * 每个 mm_struct 通过 exe_file 字段保存对该进程正在执行的二进制文件
 * 的引用（即 /proc/<pid>/exe 指向的文件）。fork 时需要将父进程的
 * exe_file 引用传递给子进程的新 mm。
 *
 * 关键设计：
 * - 使用 RCU_INIT_POINTER 而非普通赋值，因为 mm->exe_file 在运行时
 *   会通过 RCU 机制安全读取（如 /proc 文件系统访问），必须保证内存
 *   屏障语义。
 * - exe_file_deny_write_access()：防止在进程运行时有人以写模式打开
 *   该可执行文件（即 Linux 的 "text busy" 保护机制），确保运行中的
 *   代码段不会被篡改。父进程已经做过一次拒绝，子进程 mm 也需要各自
 *   持有一个拒绝写权限的引用计数。
 */
void dup_mm_exe_file(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct file *exe_file;

	exe_file = get_mm_exe_file(oldmm);
	RCU_INIT_POINTER(mm->exe_file, exe_file);
	/*
	 * We depend on the oldmm having properly denied write access to the
	 * exe_file already.
	 */
	if (exe_file && exe_file_deny_write_access(exe_file))
		pr_warn_once("exe_file_deny_write_access() failed in %s\n", __func__);
}

/*
 * 【mm_alloc_pgd / mm_free_pgd】分配/释放顶级页目录（PGD）
 *
 * PGD（Page Global Directory）是多级页表的最顶层，在不同架构上
 * 名称和层数有所不同（x86_64 为 PML4/PML5，ARM64 为 PGD/PUD/PMD/PTE）。
 * 每个 mm_struct 拥有一套独立的页表，因此每个进程都需要在创建 mm 时
 * 分配 PGD，在销毁 mm 时释放 PGD。
 *
 * CONFIG_MMU 未开启时（如 uClinux 无 MMU 平台），页表不存在，
 * 直接定义为空操作宏，避免不必要的开销。
 *
 * pgd_alloc() 是体系结构相关函数（arch/xxx/mm/pgalloc.c），
 * 通常从专用的 pgd_cache slab 缓存分配，并做必要的架构初始化
 * （如 x86_64 下拷贝内核地址空间的 PGD 条目）。
 */
#ifdef CONFIG_MMU
static inline int mm_alloc_pgd(struct mm_struct *mm)
{
	mm->pgd = pgd_alloc(mm);
	if (unlikely(!mm->pgd))
		return -ENOMEM;
	return 0;
}

static inline void mm_free_pgd(struct mm_struct *mm)
{
	pgd_free(mm, mm->pgd);
}
#else
#define mm_alloc_pgd(mm)	(0)
#define mm_free_pgd(mm)
#endif /* CONFIG_MMU */

/*
 * 【mm_alloc_id / mm_free_id】为 mm_struct 分配/释放唯一数字 ID
 *
 * CONFIG_MM_ID 启用时（通常由 IOMMU/SVA 等特性选中），每个 mm_struct
 * 需要一个唯一的整数 ID（mm_id），用于在硬件 PASID 表或 IOMMU 上下文中
 * 标识某个地址空间。
 *
 * 实现细节：
 * - 使用内核的 IDA（ID Allocator）机制，从 [MM_ID_MIN, MM_ID_MAX] 范围
 *   内分配一个空闲 ID，保证全局唯一性，且支持并发安全分配。
 * - mm_free_id 先将 mm->mm_id 重置为 MM_ID_DUMMY（哨兵值），
 *   再释放 IDA 条目，防止在释放过程中 mm_id 被重新读取导致 use-after-free。
 * - WARN_ON_ONCE 用于在异常路径下提前发现 ID 越界的 bug。
 *
 * 未开启 CONFIG_MM_ID 时退化为空操作。
 */
#ifdef CONFIG_MM_ID
static DEFINE_IDA(mm_ida);

static inline int mm_alloc_id(struct mm_struct *mm)
{
	int ret;

	ret = ida_alloc_range(&mm_ida, MM_ID_MIN, MM_ID_MAX, GFP_KERNEL);
	if (ret < 0)
		return ret;
	mm->mm_id = ret;
	return 0;
}

static inline void mm_free_id(struct mm_struct *mm)
{
	const mm_id_t id = mm->mm_id;

	mm->mm_id = MM_ID_DUMMY;
	if (id == MM_ID_DUMMY)
		return;
	if (WARN_ON_ONCE(id < MM_ID_MIN || id > MM_ID_MAX))
		return;
	ida_free(&mm_ida, id);
}
#else /* !CONFIG_MM_ID */
static inline int mm_alloc_id(struct mm_struct *mm) { return 0; }
static inline void mm_free_id(struct mm_struct *mm) {}
#endif /* CONFIG_MM_ID */

/*
 * 【check_mm】在释放 mm_struct 前进行完整性检查
 *
 * 当 mm_struct 的引用计数降到 0 即将被释放时，调用此函数做"死亡体检"，
 * 检测是否存在资源泄漏或统计数据错误，帮助开发者及早发现内存管理 bug。
 *
 * 检查项：
 * 1. RSS 计数器（rss_stat[]）：
 *    记录进程实际驻留物理内存的页数，分为匿名页、文件映射页、交换缓存页等。
 *    所有计数器都应归零，否则说明有页面在退出时未被正确释放/统计，
 *    这类 bug 可能导致全局内存统计偏差，打印 BUG 警告便于定位。
 *
 * 2. 页表内存用量（pgtables_bytes）：
 *    若进程退出后页表占用字节数非零，意味着有页表页没有被释放，
 *    属于内存泄漏，需要修复。
 *
 * 3. Transparent HugePage PMD（pmd_huge_pte）：
 *    在未开启 split PMD ptlocks 的配置下，检查是否遗留 THP 相关状态。
 *
 * BUILD_BUG_ON_MSG 是编译期断言，确保 resident_page_types 数组大小
 * 与 NR_MM_COUNTERS 保持同步，防止因新增计数器类型而忘记更新数组。
 */
static void check_mm(struct mm_struct *mm)
{
	int i;

	BUILD_BUG_ON_MSG(ARRAY_SIZE(resident_page_types) != NR_MM_COUNTERS,
			 "Please make sure 'struct resident_page_types[]' is updated as well");

	for (i = 0; i < NR_MM_COUNTERS; i++) {
		long x = percpu_counter_sum(&mm->rss_stat[i]);

		if (unlikely(x)) {
			pr_alert("BUG: Bad rss-counter state mm:%p type:%s val:%ld Comm:%s Pid:%d\n",
				 mm, resident_page_types[i], x,
				 current->comm,
				 task_pid_nr(current));
		}
	}

	if (mm_pgtables_bytes(mm))
		pr_alert("BUG: non-zero pgtables_bytes on freeing mm: %ld\n",
				mm_pgtables_bytes(mm));

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !defined(CONFIG_SPLIT_PMD_PTLOCKS)
	VM_BUG_ON_MM(mm->pmd_huge_pte, mm);
#endif
}

/*
 * 【allocate_mm / free_mm】从 slab 缓存分配/释放 mm_struct
 *
 * mm_struct 是描述进程地址空间的核心数据结构，生命周期频繁，
 * 使用专用的 mm_cachep slab 缓存（在 mm_init() 中通过
 * kmem_cache_create() 创建）可以：
 * - 避免频繁调用 kmalloc/kfree 产生内存碎片
 * - 利用 slab 着色（cache coloring）优化 CPU 缓存利用率
 * - 快速分配（从 per-CPU 本地缓存中直接获取，通常无需加锁）
 *
 * 这两个宏刻意设计成表达式（带括号包裹整个 kmem_cache_alloc/free 调用），
 * 方便在复杂表达式中使用。
 */
#define allocate_mm()	(kmem_cache_alloc(mm_cachep, GFP_KERNEL))
#define free_mm(mm)	(kmem_cache_free(mm_cachep, (mm)))

/*
 * 【do_check_lazy_tlb】调试用：检测某 CPU 是否仍以 mm 为 active_mm
 *
 * 通过 on_each_cpu() 在每个 CPU 上运行，确认在 mm 释放之后没有任何
 * CPU 的 active_mm 还指向它。若发现异常，WARN_ON_ONCE 打印调用栈。
 * 仅在 CONFIG_DEBUG_VM_SHOOT_LAZIES 启用时使用，属于调试辅助路径。
 */
static void do_check_lazy_tlb(void *arg)
{
	struct mm_struct *mm = arg;

	WARN_ON_ONCE(current->active_mm == mm);
}

/*
 * 【do_shoot_lazy_tlb】将仍在使用 mm 的 lazy TLB CPU 切换到 init_mm
 *
 * "Lazy TLB" 是指内核线程（current->mm == NULL）借用上一个用户进程的
 * mm 作为 active_mm，以避免不必要的 TLB 刷新（切换到内核线程时不需要
 * 切换地址空间）。但当该用户进程的 mm 引用计数降至 0 即将释放时，
 * 必须强制让所有仍在 lazy 使用该 mm 的 CPU 切换走，否则后续访问
 * 已释放的 mm 会导致 use-after-free。
 *
 * 此函数通过 IPI（处理器间中断）发送到 mm_cpumask(mm) 中标记的 CPU，
 * 在目标 CPU 的上下文中执行：
 * - WARN_ON_ONCE(current->mm)：检查确认当前 CPU 确实是内核线程
 *   （真正的用户线程不应出现在这里）
 * - 将 active_mm 切换为 init_mm，并调用 switch_mm() 完成实际的
 *   页表切换和 TLB 维护
 */
static void do_shoot_lazy_tlb(void *arg)
{
	struct mm_struct *mm = arg;

	if (current->active_mm == mm) {
		WARN_ON_ONCE(current->mm);
		current->active_mm = &init_mm;
		switch_mm(mm, &init_mm, current);
	}
}

/*
 * 【cleanup_lazy_tlbs】在 mm 即将销毁前，驱逐所有 lazy TLB 用户
 *
 * 背景：
 * Linux 支持两种 lazy TLB 策略：
 * a) 引用计数方案（未启用 CONFIG_MMU_LAZY_TLB_SHOOTDOWN）：
 *    内核线程 lazy 借用 mm 时同样持有 mm_count 引用，因此在所有
 *    CPU 主动放弃引用前 mm_count 不会降到 0，__mmdrop 自然不会
 *    过早被调用，无需额外 IPI。此分支直接 return。
 *
 * b) IPI shootdown 方案（启用 CONFIG_MMU_LAZY_TLB_SHOOTDOWN）：
 *    内核线程 lazy 借用 mm 时不增加 mm_count，换来更低的引用计数
 *    开销，但 mm_count 降到 0 时必须主动通过 IPI 通知仍在 lazy 使用
 *    该 mm 的 CPU 切换走，再安全释放 mm。
 *
 * IPI 优化思路（代码注释中讨论）：
 * - 可借助 exit_mm() 的最后一次 TLB flush IPI 捎带清理
 * - 可批量处理多个 mm 的 IPI
 * - 远程读取 active_mm 过滤假阳性 CPU
 * 这些优化尚未实现，当前实现简单正确，IPI 开销在实测中不显著。
 */
static void cleanup_lazy_tlbs(struct mm_struct *mm)
{
	if (!IS_ENABLED(CONFIG_MMU_LAZY_TLB_SHOOTDOWN)) {
		/*
		 * In this case, lazy tlb mms are refounted and would not reach
		 * __mmdrop until all CPUs have switched away and mmdrop()ed.
		 */
		return;
	}

	/*
	 * Lazy mm shootdown does not refcount "lazy tlb mm" usage, rather it
	 * requires lazy mm users to switch to another mm when the refcount
	 * drops to zero, before the mm is freed. This requires IPIs here to
	 * switch kernel threads to init_mm.
	 *
	 * archs that use IPIs to flush TLBs can piggy-back that lazy tlb mm
	 * switch with the final userspace teardown TLB flush which leaves the
	 * mm lazy on this CPU but no others, reducing the need for additional
	 * IPIs here. There are cases where a final IPI is still required here,
	 * such as the final mmdrop being performed on a different CPU than the
	 * one exiting, or kernel threads using the mm when userspace exits.
	 *
	 * IPI overheads have not found to be expensive, but they could be
	 * reduced in a number of possible ways, for example (roughly
	 * increasing order of complexity):
	 * - The last lazy reference created by exit_mm() could instead switch
	 *   to init_mm, however it's probable this will run on the same CPU
	 *   immediately afterwards, so this may not reduce IPIs much.
	 * - A batch of mms requiring IPIs could be gathered and freed at once.
	 * - CPUs store active_mm where it can be remotely checked without a
	 *   lock, to filter out false-positives in the cpumask.
	 * - After mm_users or mm_count reaches zero, switching away from the
	 *   mm could clear mm_cpumask to reduce some IPIs, perhaps together
	 *   with some batching or delaying of the final IPIs.
	 * - A delayed freeing and RCU-like quiescing sequence based on mm
	 *   switching to avoid IPIs completely.
	 */
	on_each_cpu_mask(mm_cpumask(mm), do_shoot_lazy_tlb, (void *)mm, 1);
	if (IS_ENABLED(CONFIG_DEBUG_VM_SHOOT_LAZIES))
		on_each_cpu(do_check_lazy_tlb, (void *)mm, 1);
}

/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 *
 * 【__mmdrop】mm_struct 的最终销毁函数
 *
 * 当 mm->mm_count（内核内部引用计数，由 mmgrab/mmdrop 管理）降至 0 时，
 * 由 mmdrop() 内联函数调用此函数完成彻底释放。
 *
 * 与 mmput()（减少 mm_users，用户空间引用）的关系：
 *   mm_users > 0：有用户态线程在使用该 mm
 *   mm_count > 0：有内核代码（含 lazy TLB、swap、core dump 等）持有引用
 *   mm_users 降到 0 时调用 __mmput() → mmput_async()，最终 mmdrop()
 *   mm_count 降到 0 时调用 __mmdrop()，进行真正的内存释放
 *
 * 销毁顺序（顺序很重要）：
 * 1. cleanup_lazy_tlbs：驱逐仍在 lazy 使用此 mm 的 CPU（见上文）
 * 2. mm_destroy_sched：清理调度器相关状态（如 CFS 带宽）
 * 3. mm_free_pgd：释放页表（必须在 destroy_context 之前）
 * 4. mm_free_id：释放 PASID/mm_id
 * 5. destroy_context：释放体系结构相关上下文（如 ARM64 ASID）
 * 6. mmu_notifier_subscriptions_destroy：通知 KVM/IOMMU 等订阅者
 * 7. check_mm：完整性检查（调试用）
 * 8. mm_pasid_drop / mm_destroy_cid：清理 PASID 和 CID 资源
 * 9. percpu_counter_destroy_many：销毁 rss_stat 计数器
 * 10. free_mm：将 mm_struct 归还 slab 缓存
 *
 * BUG_ON(mm == &init_mm)：init_mm 是内核 mm，永远不应被释放。
 * WARN_ON_ONCE(mm == current->mm/active_mm)：在当前 CPU 还在使用
 * 该 mm 时释放它是严重错误。
 */
void __mmdrop(struct mm_struct *mm)
{
	BUG_ON(mm == &init_mm);
	WARN_ON_ONCE(mm == current->mm);

	/* Ensure no CPUs are using this as their lazy tlb mm */
	cleanup_lazy_tlbs(mm);

	WARN_ON_ONCE(mm == current->active_mm);
	mm_destroy_sched(mm);
	mm_free_pgd(mm);
	mm_free_id(mm);
	destroy_context(mm);
	mmu_notifier_subscriptions_destroy(mm);
	check_mm(mm);
	mm_pasid_drop(mm);
	mm_destroy_cid(mm);
	percpu_counter_destroy_many(mm->rss_stat, NR_MM_COUNTERS);

	free_mm(mm);
}
EXPORT_SYMBOL_GPL(__mmdrop);

/*
 * 【mmdrop_async_fn / mmdrop_async】在工作队列上异步释放 mm_struct
 *
 * 问题背景：
 * __mmdrop() 在 x86 上会调用 pgd_free() → pgd_dtor()，而 pgd_dtor()
 * 可能需要睡眠（如操作 LDT）或执行不适合在软中断上下文中运行的操作。
 * 但 free_signal_struct() 可能在软中断（softirq）或 RCU 回调等原子
 * 上下文中被调用，直接调用 __mmdrop() 会触发"sleeping in atomic context"
 * 等内核 BUG。
 *
 * 解决方案：
 * mmdrop_async() 检查 mm_count 是否降至 0，若是则将真正的释放工作
 * 提交到系统工作队列（schedule_work），由内核工作线程在进程上下文中
 * 安全地执行 __mmdrop()。
 *
 * async_put_work 字段直接嵌入 mm_struct，避免额外分配，且在 mm 引用
 * 计数已经为 0 的情况下不存在并发使用冲突。
 */
static void mmdrop_async_fn(struct work_struct *work)
{
	struct mm_struct *mm;

	mm = container_of(work, struct mm_struct, async_put_work);
	__mmdrop(mm);
}

static void mmdrop_async(struct mm_struct *mm)
{
	if (unlikely(atomic_dec_and_test(&mm->mm_count))) {
		INIT_WORK(&mm->async_put_work, mmdrop_async_fn);
		schedule_work(&mm->async_put_work);
	}
}

/*
 * 【free_signal_struct】释放线程组共享的信号结构体
 *
 * signal_struct 由线程组中所有线程共享（通过 tsk->signal 指针），
 * 保存了信号处理状态、进程组 ID、会话 ID、资源限制、统计信息等。
 * 当最后一个引用（sigcnt）被释放时调用此函数。
 *
 * 释放步骤：
 * 1. taskstats_tgid_free：清理任务统计（/proc/PID/taskstats）
 * 2. sched_autogroup_exit：退出调度自动分组
 * 3. mmdrop_async(sig->oom_mm)：
 *    OOM Killer 在杀死进程时会记录受害进程的 mm（oom_mm），
 *    以便在 OOM 解除后统计内存释放情况。这里异步释放该 mm 引用，
 *    使用 async 版本是因为此处可能处于原子上下文（见 mmdrop_async 注释）。
 * 4. kmem_cache_free：将 signal_struct 归还 signal_cachep slab 缓存
 */
static inline void free_signal_struct(struct signal_struct *sig)
{
	taskstats_tgid_free(sig);
	sched_autogroup_exit(sig);
	/*
	 * __mmdrop is not safe to call from softirq context on x86 due to
	 * pgd_dtor so postpone it to the async context
	 */
	if (sig->oom_mm)
		mmdrop_async(sig->oom_mm);
	kmem_cache_free(signal_cachep, sig);
}

/*
 * 【put_signal_struct】减少 signal_struct 引用计数，必要时释放
 *
 * 采用标准的"减引用计数，若降至 0 则释放"模式。
 * sigcnt 使用 refcount_t（而非 atomic_t）提供溢出保护和更严格的语义。
 */
static inline void put_signal_struct(struct signal_struct *sig)
{
	if (refcount_dec_and_test(&sig->sigcnt))
		free_signal_struct(sig);
}

/*
 * 【__put_task_struct】task_struct 的最终销毁函数
 *
 * 当 tsk->usage 引用计数（通过 put_task_struct() 减少）降至 0 时调用。
 * 此时进程已经退出（exit_state 非零），所有其他引用都已释放，
 * 可以安全地销毁进程描述符本身。
 *
 * 前置条件检查：
 * - WARN_ON(!tsk->exit_state)：必须已经调用过 do_exit()
 * - WARN_ON(refcount_read(&tsk->usage))：usage 必须已降至 0
 * - WARN_ON(tsk == current)：不能释放当前正在运行的进程描述符
 *   （如果 current 正在 free 自己，下一条指令就是 use-after-free）
 *
 * 释放顺序：
 * 1. unwind_task_free：释放 ORC/CFI 等展开信息
 * 2. io_uring_free：释放 io_uring 相关资源
 * 3. cgroup_task_free：从 cgroup 子系统注销
 * 4. task_numa_free：释放 NUMA 亲和性统计数据
 * 5. security_task_free：通知 LSM（SELinux/AppArmor 等）释放安全标签
 * 6. exit_creds：释放进程凭据（uid/gid/capabilities 等）
 * 7. delayacct_tsk_free：释放延迟记账数据
 * 8. put_signal_struct：减少信号结构引用
 * 9. sched_core_free：释放调度核心共享组
 * 10. free_task：释放内核栈和 task_struct 本身
 */
void __put_task_struct(struct task_struct *tsk)
{
	WARN_ON(!tsk->exit_state);
	WARN_ON(refcount_read(&tsk->usage));
	WARN_ON(tsk == current);

	unwind_task_free(tsk);
	io_uring_free(tsk);
	cgroup_task_free(tsk);
	task_numa_free(tsk, true);
	security_task_free(tsk);
	exit_creds(tsk);
	delayacct_tsk_free(tsk);
	put_signal_struct(tsk->signal);
	sched_core_free(tsk);
	free_task(tsk);
}
EXPORT_SYMBOL_GPL(__put_task_struct);

/*
 * 【__put_task_struct_rcu_cb】RCU 回调：在 RCU 宽限期结束后释放 task_struct
 *
 * 某些场景下（如 CONFIG_PROVE_RCU 或特定的 ptrace/proc 访问路径），
 * 需要等待 RCU 宽限期结束才能安全释放 task_struct，因为读者可能通过
 * RCU 保护的指针访问进程描述符（如 tasklist_lock 的 RCU 读者）。
 *
 * 使用方式：
 *   call_rcu(&tsk->rcu, __put_task_struct_rcu_cb);
 * 当所有 RCU 读者退出临界区后，此回调被调用，再执行实际的释放。
 *
 * container_of 通过 rcu_head 成员的地址反推出整个 task_struct 的地址。
 */
void __put_task_struct_rcu_cb(struct rcu_head *rhp)
{
	struct task_struct *task = container_of(rhp, struct task_struct, rcu);

	__put_task_struct(task);
}
EXPORT_SYMBOL_GPL(__put_task_struct_rcu_cb);

/*
 * 【arch_task_cache_init】体系结构相关的任务缓存初始化（弱符号）
 *
 * __weak 弱符号：若某体系结构（如 x86、ARM64）不需要额外的任务缓存
 * 初始化，则使用此空实现；若需要（如某些嵌入式架构需要分配特殊的
 * per-CPU 任务状态区域），可在 arch/xxx/kernel/ 中提供同名强符号覆盖。
 *
 * 调用时机：fork_init() 中，在 task_struct_cachep 创建之后立即调用，
 * 确保架构可以基于已就绪的 slab 缓存做进一步初始化。
 */
void __init __weak arch_task_cache_init(void) { }

/*
 * set_max_threads
 *
 * 【set_max_threads】根据系统内存计算并设置最大线程数上限
 *
 * 安全原则：线程结构（内核栈 + task_struct 等）不应消耗超过总内存的 1/8。
 * 计算公式：threads = (总物理内存字节数) / (THREAD_SIZE * 8)
 *
 * 特殊情况处理：
 * - 若 fls64(nr_pages) + fls64(PAGE_SIZE) > 64，乘积会溢出 u64，
 *   此时直接使用 MAX_THREADS（系统硬上限，通常为 FUTEX_TID_MASK = 0x3fffffff）。
 * - 若计算结果超过调用者传入的建议值（max_threads_suggested），
 *   以建议值为准（不能超出硬上限）。
 * - 最终用 clamp_t 保证结果在 [MIN_THREADS, MAX_THREADS] 范围内，
 *   即使在内存极少的嵌入式系统上也至少允许若干线程。
 *
 * 此函数在 fork_init() 中以 MAX_THREADS 为参数调用，
 * 结果写入全局变量 max_threads，后续 copy_process() 会检查该限制。
 */
static void __init set_max_threads(unsigned int max_threads_suggested)
{
	u64 threads;
	unsigned long nr_pages = memblock_estimated_nr_free_pages();

	/*
	 * The number of threads shall be limited such that the thread
	 * structures may only consume a small part of the available memory.
	 */
	if (fls64(nr_pages) + fls64(PAGE_SIZE) > 64)
		threads = MAX_THREADS;
	else
		threads = div64_u64((u64) nr_pages * (u64) PAGE_SIZE,
				    (u64) THREAD_SIZE * 8UL);

	if (threads > max_threads_suggested)
		threads = max_threads_suggested;

	max_threads = clamp_t(u64, threads, MIN_THREADS, MAX_THREADS);
}

#ifdef CONFIG_ARCH_WANTS_DYNAMIC_TASK_STRUCT
/* Initialized by the architecture: */
int arch_task_struct_size __read_mostly;
#endif

/*
 * 【task_struct_whitelist】获取 task_struct 中允许被用户空间复制的内存范围
 *
 * 内核 slab usercopy 保护（CONFIG_HARDENED_USERCOPY）：
 * 当内核通过 copy_to_user() 向用户空间复制数据时，如果源地址位于 slab
 * 缓存分配的对象中，内核会检查被复制的范围是否在该对象的"usercopy 白名单"
 * 内，防止意外将内核内部状态（如密钥、指针）泄漏给用户空间。
 *
 * 对于 task_struct：
 * - 只有 thread_struct（体系结构相关的寄存器/FPU 状态）中的部分字段
 *   （由 arch_thread_struct_whitelist() 定义）允许被复制到用户空间，
 *   例如 ptrace 读取寄存器状态时需要访问这些字段。
 * - 其余字段（调度信息、凭据、文件描述符表指针等）不在白名单内，
 *   一旦被意外 copy_to_user 会触发内核 BUG。
 *
 * 特殊情况：若白名单大小为 0（架构不需要任何可复制字段），
 * 将 offset 设为 0 以表示整个对象都不允许复制（大小已经是 0）。
 * 否则将 offset 修正为 thread_struct 在 task_struct 中的偏移量。
 */
static void __init task_struct_whitelist(unsigned long *offset, unsigned long *size)
{
	/* Fetch thread_struct whitelist for the architecture. */
	arch_thread_struct_whitelist(offset, size);

	/*
	 * Handle zero-sized whitelist or empty thread_struct, otherwise
	 * adjust offset to position of thread_struct in task_struct.
	 */
	if (unlikely(*size == 0))
		*offset = 0;
	else
		*offset += offsetof(struct task_struct, thread);
}

/*
 * 【fork_init】系统启动时的进程子系统初始化
 *
 * 由 start_kernel() 在系统引导阶段调用，完成以下初始化工作：
 *
 * 1. 创建 task_struct 的 slab 缓存（task_struct_cachep）：
 *    task_struct 是进程描述符，每创建一个进程都需要从该缓存分配，
 *    slab 缓存比直接 kmalloc 更高效，且支持 usercopy 白名单以防止
 *    信息泄漏（只允许复制结构体中明确标记为安全的字段）。
 *
 * 2. 设置系统最大线程数（max_threads）：
 *    根据系统内存大小计算上限，默认为内存页数 / 8（每个线程需要内核栈等）。
 *    同时设置 init 进程的 RLIMIT_NPROC 为 max_threads/2。
 *
 * 3. 初始化每用户命名空间的资源计数上限（ucount_max）：
 *    控制每个用户能创建的进程数，防止 fork bomb。
 *    Android 中 AID_APP 用户受此限制约束。
 */
void __init fork_init(void)
{
	int i;
#ifndef ARCH_MIN_TASKALIGN
#define ARCH_MIN_TASKALIGN	0
#endif
	int align = max_t(int, L1_CACHE_BYTES, ARCH_MIN_TASKALIGN);
	unsigned long useroffset, usersize;

	/* create a slab on which task_structs can be allocated */
	task_struct_whitelist(&useroffset, &usersize);
	task_struct_cachep = kmem_cache_create_usercopy("task_struct",
			arch_task_struct_size, align,
			SLAB_PANIC|SLAB_ACCOUNT,
			useroffset, usersize, NULL);

	/* do the arch specific task caches init */
	arch_task_cache_init();

	set_max_threads(MAX_THREADS);

	init_task.signal->rlim[RLIMIT_NPROC].rlim_cur = max_threads/2;
	init_task.signal->rlim[RLIMIT_NPROC].rlim_max = max_threads/2;
	init_task.signal->rlim[RLIMIT_SIGPENDING] =
		init_task.signal->rlim[RLIMIT_NPROC];

	for (i = 0; i < UCOUNT_COUNTS; i++)
		init_user_ns.ucount_max[i] = max_threads/2;

	set_userns_rlimit_max(&init_user_ns, UCOUNT_RLIMIT_NPROC,      RLIM_INFINITY);
	set_userns_rlimit_max(&init_user_ns, UCOUNT_RLIMIT_MSGQUEUE,   RLIM_INFINITY);
	set_userns_rlimit_max(&init_user_ns, UCOUNT_RLIMIT_SIGPENDING, RLIM_INFINITY);
	set_userns_rlimit_max(&init_user_ns, UCOUNT_RLIMIT_MEMLOCK,    RLIM_INFINITY);

#ifdef CONFIG_VMAP_STACK
	cpuhp_setup_state(CPUHP_BP_PREPARE_DYN, "fork:vm_stack_cache",
			  NULL, free_vm_stack_cache);
#endif

	scs_init();

	lockdep_init_task(&init_task);
	uprobes_init();
}

/*
 * 【arch_dup_task_struct】将父进程的 task_struct 拷贝给子进程（弱符号）
 *
 * 默认实现：直接执行结构体的浅拷贝（*dst = *src），复制全部字段。
 * 体系结构可以提供强符号覆盖，以处理需要特殊操作的字段，例如：
 * - x86：需要重置 FPU 状态（avx/sse 寄存器不应直接继承父进程状态，
 *   而是在首次使用时懒加载）
 * - 某些架构可能需要处理对齐或特殊初始化
 *
 * 调用时机：dup_task_struct() 中，在分配新 task_struct 之后立即调用，
 * 此后 dup_task_struct 会覆盖部分字段（内核栈指针、引用计数等）。
 */
int __weak arch_dup_task_struct(struct task_struct *dst,
					       struct task_struct *src)
{
	*dst = *src;
	return 0;
}

/*
 * 【set_task_stack_end_magic】在内核栈底写入魔数以检测栈溢出
 *
 * 内核栈大小固定（通常 8KB 或 16KB，视 CONFIG_THREAD_SIZE 而定），
 * 若内核函数调用层次过深或局部变量过大，栈指针会向下越过栈底，
 * 覆盖相邻内存（如 thread_info 或其他内核数据），导致难以调试的崩溃。
 *
 * 防护机制：
 * 在栈底（end_of_stack(tsk) 指向的位置，即最低有效地址）写入
 * STACK_END_MAGIC（0x57AC6E9D），内核的调度器、中断处理和调试
 * 工具（如 /proc/<pid>/status 的 "StackGuard" 检查）会定期检查
 * 该值是否被覆盖，一旦发现栈溢出立即触发 kernel panic 或警告，
 * 而不是让系统在损坏状态下继续运行。
 *
 * 此函数在 dup_task_struct() 和 fork_init() 中被调用（包括对 init_task）。
 */
void set_task_stack_end_magic(struct task_struct *tsk)
{
	unsigned long *stackend;

	stackend = end_of_stack(tsk);
	*stackend = STACK_END_MAGIC;	/* for overflow detection */
}

/*
 * 【dup_task_struct】为新进程分配并初始化 task_struct 和内核栈
 *
 * 这是 copy_process() 调用的第一步，为子进程准备最基本的数据结构。
 *
 * 主要工作：
 * 1. 从 task_struct_cachep slab 缓存分配新的 task_struct（进程描述符），
 *    并调用 arch_dup_task_struct() 将父进程的 task_struct 内容拷贝过来。
 *
 * 2. 分配新的内核栈（alloc_thread_stack_node）：
 *    内核栈布局（以 ARM64 为例，栈向低地址增长）：
 *      高地址：栈顶（初始 sp 指向此处）
 *        ...   内核函数调用帧
 *      低地址：thread_info（嵌入在 task_struct 中，或位于栈底）
 *    set_task_stack_end_magic() 在栈底写入 STACK_END_MAGIC 魔数，
 *    用于运行时检测内核栈溢出（内核栈溢出是严重的安全漏洞）。
 *
 * 3. 设置 stack canary（tsk->stack_canary）：
 *    CONFIG_STACKPROTECTOR 启用时，每个进程的内核栈有独立的随机 canary 值，
 *    GCC/Clang 的栈保护插桩会在函数返回前校验，防止栈缓冲区溢出攻击。
 *
 * 4. 将 seccomp.filter 置 NULL：
 *    seccomp 过滤器在 copy_process() 后续步骤中正式继承，
 *    这里先清零防止错误引用父进程的过滤器链。
 */
static struct task_struct *dup_task_struct(struct task_struct *orig, int node)
{
	struct task_struct *tsk;
	int err;

	if (node == NUMA_NO_NODE)
		node = tsk_fork_get_node(orig);
	tsk = alloc_task_struct_node(node);
	if (!tsk)
		return NULL;

	err = arch_dup_task_struct(tsk, orig);
	if (err)
		goto free_tsk;

	err = alloc_thread_stack_node(tsk, node);
	if (err)
		goto free_tsk;

#ifdef CONFIG_THREAD_INFO_IN_TASK
	refcount_set(&tsk->stack_refcount, 1);
#endif
	account_kernel_stack(tsk, 1);

	err = scs_prepare(tsk, node);
	if (err)
		goto free_stack;

#ifdef CONFIG_SECCOMP
	/*
	 * We must handle setting up seccomp filters once we're under
	 * the sighand lock in case orig has changed between now and
	 * then. Until then, filter must be NULL to avoid messing up
	 * the usage counts on the error path calling free_task.
	 */
	tsk->seccomp.filter = NULL;
#endif

	RCU_INIT_POINTER(tsk->exec_state, NULL);

	setup_thread_stack(tsk, orig);
	clear_user_return_notifier(tsk);
	clear_tsk_need_resched(tsk);
	set_task_stack_end_magic(tsk);
	clear_syscall_work_syscall_user_dispatch(tsk);

#ifdef CONFIG_STACKPROTECTOR
	tsk->stack_canary = get_random_canary();
#endif
	if (orig->cpus_ptr == &orig->cpus_mask)
		tsk->cpus_ptr = &tsk->cpus_mask;
	dup_user_cpus_ptr(tsk, orig, node);

	/*
	 * One for the user space visible state that goes away when reaped.
	 * One for the scheduler.
	 */
	refcount_set(&tsk->rcu_users, 2);
	/* One for the rcu users */
	refcount_set(&tsk->usage, 1);
#ifdef CONFIG_BLK_DEV_IO_TRACE
	tsk->btrace_seq = 0;
#endif
	tsk->splice_pipe = NULL;
	tsk->task_frag.page = NULL;
	tsk->wake_q.next = NULL;
	tsk->worker_private = NULL;

	kcov_task_init(tsk);
	kmsan_task_create(tsk);
	kmap_local_fork(tsk);

#ifdef CONFIG_FAULT_INJECTION
	tsk->fail_nth = 0;
#endif

#ifdef CONFIG_BLK_CGROUP
	tsk->throttle_disk = NULL;
	tsk->use_memdelay = 0;
#endif

#ifdef CONFIG_ARCH_HAS_CPU_PASID
	tsk->pasid_activated = 0;
#endif

#ifdef CONFIG_MEMCG
	tsk->active_memcg = NULL;
#endif

#ifdef CONFIG_X86_BUS_LOCK_DETECT
	tsk->reported_split_lock = 0;
#endif

#ifdef CONFIG_SCHED_MM_CID
	tsk->mm_cid.cid = MM_CID_UNSET;
	tsk->mm_cid.active = 0;
	INIT_HLIST_NODE(&tsk->mm_cid.node);
#endif

#ifdef CONFIG_BPF_SYSCALL
	RCU_INIT_POINTER(tsk->bpf_storage, NULL);
	tsk->bpf_ctx = NULL;
#endif
	return tsk;

free_stack:
	exit_task_stack_account(tsk);
	free_thread_stack(tsk);
free_tsk:
	free_task_struct(tsk);
	return NULL;
}

__cacheline_aligned_in_smp DEFINE_SPINLOCK(mmlist_lock);

static unsigned long coredump_filter = MMF_DUMP_FILTER_DEFAULT;

/*
 * coredump_filter_setup - 解析内核命令行参数 "coredump_filter="
 * @s: 命令行传入的字符串值
 *
 * 通过内核启动参数设置全局 coredump 过滤器默认值。
 * 该值会被写入新创建 mm_struct 的 flags 字段，决定 core dump
 * 时哪些 VMA 类型（匿名页、共享内存、文件映射等）需要被转储。
 * 返回 1 表示解析成功，0 表示失败（__setup 约定）。
 */
static int __init coredump_filter_setup(char *s)
{
	if (kstrtoul(s, 0, &coredump_filter)) /* 将字符串转为无符号长整型，失败则返回 0 */
		return 0;
	coredump_filter <<= MMF_DUMP_FILTER_SHIFT; /* 左移到 mm_flags 中过滤器位域的正确位置 */
	coredump_filter &= MMF_DUMP_FILTER_MASK;   /* 与掩码做 AND，确保只设置合法的过滤位 */
	return 1;
}

__setup("coredump_filter=", coredump_filter_setup);

#include <linux/init_task.h>

/*
 * mm_init_aio - 初始化 mm_struct 中的 AIO（异步 I/O）相关字段
 * @mm: 待初始化的内存描述符
 *
 * AIO 子系统使用 ioctx_lock 保护 ioctx_table（io_context 哈希表）。
 * 只有在内核配置了 CONFIG_AIO 时才需要这些字段。
 */
static void mm_init_aio(struct mm_struct *mm)
{
#ifdef CONFIG_AIO
	spin_lock_init(&mm->ioctx_lock); /* 初始化保护 AIO context 表的自旋锁 */
	mm->ioctx_table = NULL;          /* AIO context 指针表初始为空，按需分配 */
#endif
}

/*
 * mm_clear_owner - 清除 mm_struct 的 MEMCG owner 字段
 * @mm: 内存描述符
 * @p:  要清除的任务（只有当 mm->owner == p 时才清除）
 *
 * mm->owner 用于 Memory Cgroup 记账：当一个进程退出时，如果它
 * 是该 mm 的 owner，需要将 owner 置 NULL，否则 memcg 会对已死进程
 * 继续记账。使用 WRITE_ONCE 保证并发读者看到原子更新。
 * 仅在 CONFIG_MEMCG 启用时有意义。
 */
static __always_inline void mm_clear_owner(struct mm_struct *mm,
					   struct task_struct *p)
{
#ifdef CONFIG_MEMCG
	if (mm->owner == p)          /* 只有当前任务确实是 owner 时才清除 */
		WRITE_ONCE(mm->owner, NULL); /* 原子写 NULL，避免并发读者看到脏指针 */
#endif
}

/*
 * mm_init_owner - 设置 mm_struct 的初始 MEMCG owner
 * @mm: 内存描述符
 * @p:  成为 owner 的任务
 *
 * 在 mm_struct 刚分配、初始化时调用，将 p 设为该 mm 的内存记账责任人。
 * MEMCG 通过 owner 确定向哪个 cgroup 收费内存使用量。
 */
static void mm_init_owner(struct mm_struct *mm, struct task_struct *p)
{
#ifdef CONFIG_MEMCG
	mm->owner = p; /* 将创建者任务设为该 mm 的 memcg owner */
#endif
}

/*
 * mm_init_uprobes_state - 初始化 mm_struct 中 uprobes 相关状态
 * @mm: 内存描述符
 *
 * uprobes（用户空间探针）需要在 mm 中维护一块特殊映射区域（XOL area，
 * eXecute Out of Line），用于在用户态执行被替换的原始指令。
 * 此函数将该区域指针清零，并调用体系结构相关的初始化钩子。
 */
static void mm_init_uprobes_state(struct mm_struct *mm)
{
#ifdef CONFIG_UPROBES
	mm->uprobes_state.xol_area = NULL; /* XOL 执行区域尚未分配，置 NULL */
	arch_uprobe_init_state(mm);        /* 调用体系结构相关的 uprobe 状态初始化 */
#endif
}

/*
 * mmap_init_lock - 初始化 mm_struct 中与 mmap 相关的所有锁
 * @mm: 内存描述符
 *
 * mmap_lock 是保护进程虚拟地址空间的读写信号量：
 *   - 读锁：遍历 VMA、缺页处理等只读操作
 *   - 写锁：mmap/munmap/mprotect 等修改地址空间的操作
 * mm_lock_seqcount 配合 per-VMA 锁机制使用，提供更细粒度的并发控制。
 * CONFIG_PER_VMA_LOCK 开启时，vma_writer_wait 用于等待所有 VMA 读者退出。
 */
static void mmap_init_lock(struct mm_struct *mm)
{
	init_rwsem(&mm->mmap_lock);        /* 初始化 mmap 读写信号量，保护整个地址空间 */
	mm_lock_seqcount_init(mm);         /* 初始化序列计数器，用于 per-VMA 锁的写者检测 */
#ifdef CONFIG_PER_VMA_LOCK
	rcuwait_init(&mm->vma_writer_wait); /* 初始化 VMA 写者等待队列，供细粒度锁使用 */
#endif
}

/*
 * mm_init - 对已分配的 mm_struct 进行完整初始化
 * @mm: 已由 allocate_mm() 分配的内存描述符（调用前应已 memset 为 0）
 * @p:  关联的任务，用于设置 memcg owner 等
 *
 * 本函数完成 mm_struct 所有字段的初始化，包括：
 *   - Maple Tree（mm_mt）：替代红黑树管理 VMA 的数据结构
 *   - 引用计数：mm_users（使用者数）和 mm_count（存活引用数）均初始化为 1
 *   - 锁：mmap_lock、page_table_lock、arg_lock
 *   - 统计：RSS、page table 字节数、页表锁、pinned/locked 页计数
 *   - 子系统：AIO、MEMCG owner、PASID（IO 虚拟化）、MMU notifier、TLB flush
 *   - Flags：从父进程继承 coredump 过滤器及 legacy flags（如有父 mm）
 *   - 页表：分配顶级页目录（PGD）
 *   - 上下文：体系结构相关的新 mm 上下文初始化
 *   - CID、调度域、per-CPU RSS 计数器、LRU gen 等
 *
 * 任何步骤失败都会跳到对应的 fail 标签逐层回滚，最终返回 NULL。
 * 成功时返回已完全初始化的 mm。
 */
static struct mm_struct *mm_init(struct mm_struct *mm, struct task_struct *p)
{
	mt_init_flags(&mm->mm_mt, MM_MT_FLAGS);          /* 初始化 Maple Tree，设置 MM 相关标志 */
	mt_set_external_lock(&mm->mm_mt, &mm->mmap_lock); /* 告知 Maple Tree 使用 mmap_lock 作为外部锁 */
	atomic_set(&mm->mm_users, 1);   /* mm_users=1：当前有一个用户（创建者进程）持有此 mm */
	atomic_set(&mm->mm_count, 1);   /* mm_count=1：mm_struct 自身的存活引用计数为 1 */
	seqcount_init(&mm->write_protect_seq); /* 初始化写保护序列计数，用于 copy-on-write 检测 */
	mmap_init_lock(mm);              /* 初始化 mmap_lock 及相关锁（见 mmap_init_lock 注释） */
	INIT_LIST_HEAD(&mm->mmlist);     /* 初始化链表节点，mm 加入全局 mmlist 时使用 */
	mm_pgtables_bytes_init(mm);      /* 初始化页表字节数统计计数器为 0 */
	mm->map_count = 0;               /* VMA 数量清零 */
	mm->locked_vm = 0;               /* 被 mlock 锁定的页面数清零 */
	atomic64_set(&mm->pinned_vm, 0); /* 被 pin（如 RDMA/DMA）的页面数清零 */
	memset(&mm->rss_stat, 0, sizeof(mm->rss_stat)); /* 常驻内存集统计（匿名/文件/shmem）清零 */
	spin_lock_init(&mm->page_table_lock); /* 保护页表的自旋锁，与 mmap_lock 配合使用 */
	spin_lock_init(&mm->arg_lock);        /* 保护 arg_start/arg_end/env_start/env_end 的锁 */
	mm_init_cpumask(mm);   /* 初始化 CPU 掩码（记录哪些 CPU 载入了此 mm 的 TLB 条目） */
	mm_init_aio(mm);       /* 初始化 AIO context 锁和表（仅 CONFIG_AIO） */
	mm_init_owner(mm, p);  /* 设置 memcg owner 为创建任务 p（仅 CONFIG_MEMCG） */
	mm_pasid_init(mm);     /* 初始化 PASID（用于 SVA/IOMMU 地址空间共享，仅相关架构） */
	RCU_INIT_POINTER(mm->exe_file, NULL); /* 可执行文件引用初始为 NULL，execve 时设置 */
	mmu_notifier_subscriptions_init(mm);  /* 初始化 MMU notifier 订阅链表（KVM/RDMA 等使用） */
	init_tlb_flush_pending(mm);           /* 初始化 TLB flush 待处理位图 */
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !defined(CONFIG_SPLIT_PMD_PTLOCKS)
	mm->pmd_huge_pte = NULL; /* 透明大页 PMD 特殊页表指针，仅在不拆分 PMD 锁时需要 */
#endif
	mm_init_uprobes_state(mm); /* 初始化 uprobes XOL 区域及体系结构相关状态 */
	hugetlb_count_init(mm);    /* 初始化 hugetlb 使用计数 */
	futex_mm_init(mm);         /* 初始化 futex 哈希桶（私有 futex 需要 mm 级别的哈希表） */

	mm_flags_clear_all(mm);    /* 清除所有 mm flags，之后按需重新设置 */
	if (current->mm) {
		/* 有父 mm：从父进程继承 coredump 过滤器的 legacy flags */
		unsigned long flags = __mm_flags_get_word(current->mm);

		__mm_flags_overwrite_word(mm, mmf_init_legacy_flags(flags)); /* 继承 dump 相关标志 */
		mm->def_flags = current->mm->def_flags & VM_INIT_DEF_MASK;  /* 继承允许的 VMA 默认标志 */
	} else {
		/* 无父 mm（内核线程或 init 进程）：使用全局 coredump_filter 默认值 */
		__mm_flags_overwrite_word(mm, coredump_filter);
		mm->def_flags = 0; /* 无继承，默认标志清零 */
	}

	if (mm_alloc_pgd(mm))         /* 分配顶级页目录（PGD），失败则回滚 */
		goto fail_mm_init;

	if (mm_alloc_id(mm))          /* 分配 mm ID（用于追踪，如 perf 事件） */
		goto fail_noid;

	if (init_new_context(p, mm))  /* 体系结构相关的新地址空间上下文初始化（如 x86 LDT） */
		goto fail_nocontext;

	if (mm_alloc_cid(mm, p))      /* 分配 concurrency ID，用于 NUMA 感知调度 */
		goto fail_cid;

	if (mm_alloc_sched(mm))       /* 初始化调度相关的 mm 数据（如 NUMA 扫描状态） */
		goto fail_sched;

	if (percpu_counter_init_many(mm->rss_stat, 0, GFP_KERNEL_ACCOUNT,
				     NR_MM_COUNTERS)) /* 初始化 per-CPU RSS 计数器数组，内存记账用 */
		goto fail_pcpu;

	lru_gen_init_mm(mm); /* 初始化 MGLRU（多代 LRU）的 mm 级别状态 */
	return mm;           /* 所有步骤成功，返回已初始化的 mm */

	/* 以下为错误回滚路径，按初始化逆序逐层释放资源 */
fail_pcpu:
	mm_destroy_sched(mm);   /* 释放调度相关数据 */
fail_sched:
	mm_destroy_cid(mm);     /* 释放 concurrency ID */
fail_cid:
	destroy_context(mm);    /* 释放体系结构相关上下文（如 LDT） */
fail_nocontext:
	mm_free_id(mm);         /* 释放 mm ID */
fail_noid:
	mm_free_pgd(mm);        /* 释放 PGD 页目录 */
fail_mm_init:
	free_mm(mm);            /* 释放 mm_struct 本身（kmem_cache_free） */
	return NULL;
}

/*
 * mm_alloc - 分配并初始化一个全新的 mm_struct
 *
 * 先从 mm_cachep slab 分配一块内存，清零后调用 mm_init() 完成完整初始化。
 * 与 dup_mm() 的区别：mm_alloc() 创建空白 mm，dup_mm() 复制现有 mm 的地址空间。
 * 主要用于内核线程借用用户 mm（kthread_use_mm）或测试（EXPORT_SYMBOL_IF_KUNIT）。
 */
struct mm_struct *mm_alloc(void)
{
	struct mm_struct *mm;

	mm = allocate_mm();    /* 从 mm_cachep slab 缓存中分配一个 mm_struct 对象 */
	if (!mm)
		return NULL;

	memset(mm, 0, sizeof(*mm)); /* 清零所有字段，mm_init() 会按需初始化各字段 */
	return mm_init(mm, current); /* 完成 mm 的完整初始化，关联到 current 任务 */
}
EXPORT_SYMBOL_IF_KUNIT(mm_alloc); /* 仅在 kunit 测试时导出符号，普通模块不可用 */

/*
 * __mmput - mm_users 降为 0 后执行最终资源释放
 * @mm: mm_users 已为 0 的内存描述符
 *
 * 当最后一个用户（进程）退出对 mm 的引用后调用。
 * 执行顺序有严格要求：
 *   1. uprobes 清理必须在 exit_mmap 之前（XOL 区域还存在）
 *   2. khugepaged 必须在 exit_mmap 之前退出（避免并发分裂/合并大页）
 *   3. exit_mmap 之后才能释放 exe_file 引用
 *   4. 最后调用 mmdrop() 将 mm_count 减 1，真正释放 mm_struct 内存
 * 注意：此函数调用后 mm 可能仍被 mmdrop 的其他路径持有（如 lazy TLB），
 * 直到 mm_count 降为 0 才真正释放。
 */
static inline void __mmput(struct mm_struct *mm)
{
	VM_BUG_ON(atomic_read(&mm->mm_users)); /* 断言：此时 mm_users 必须为 0 */

	uprobe_clear_state(mm);   /* 清理 uprobes 状态（XOL area 等），须在 exit_mmap 前 */
	exit_aio(mm);             /* 等待并释放所有挂起的 AIO 请求 */
	ksm_exit(mm);             /* 注销 KSM（内核同页合并）对此 mm 的扫描 */
	khugepaged_exit(mm); /* must run before exit_mmap */ /* 停止 khugepaged 对此 mm 的大页合并 */
	exit_mmap(mm);            /* 释放所有 VMA，解除所有地址映射，归还物理页 */
	mm_put_huge_zero_folio(mm); /* 释放对 huge zero page 的引用（透明大页优化） */
	set_mm_exe_file(mm, NULL);  /* 清除 exe_file 引用，允许写访问并减少文件引用计数 */
	if (!list_empty(&mm->mmlist)) {
		/* 如果此 mm 在全局 mmlist 中（进程存活期间被加入），则从链表移除 */
		spin_lock(&mmlist_lock);
		list_del(&mm->mmlist);
		spin_unlock(&mmlist_lock);
	}
	if (mm->binfmt)
		module_put(mm->binfmt->module); /* 释放对 binfmt（如 ELF 加载器）内核模块的引用 */
	lru_gen_del_mm(mm);   /* 从 MGLRU 的 mm 链表中移除此 mm */
	futex_hash_free(mm);  /* 释放 futex 私有哈希表（CONFIG_FUTEX_PRIVATE_HASH） */
	mmdrop(mm);           /* 减少 mm_count，若降为 0 则真正释放 mm_struct */
}

/*
 * mmput - 释放对 mm_struct 的一个用户级引用
 * @mm: 要释放引用的内存描述符
 *
 * mm_users 计数追踪"正在使用这个地址空间的线程数"。
 * 每个用户空间线程持有一个 mm_users 引用。
 * 当 mm_users 降为 0 时（所有线程都退出），调用 __mmput() 清理资源。
 * might_sleep() 提示此函数可能睡眠（exit_mmap 等会睡眠），不可在原子上下文调用。
 */
void mmput(struct mm_struct *mm)
{
	might_sleep(); /* 提示可能睡眠，禁止在中断/持锁等原子上下文中调用 */

	if (atomic_dec_and_test(&mm->mm_users)) /* 原子减 1，若结果为 0 则触发最终释放 */
		__mmput(mm);
}
EXPORT_SYMBOL_GPL(mmput); /* 导出给内核模块（GPL 许可）使用 */

#if defined(CONFIG_MMU) || defined(CONFIG_FUTEX_PRIVATE_HASH)
/*
 * mmput_async_fn - 工作队列回调：在进程上下文中执行 __mmput()
 * @work: 内嵌在 mm_struct->async_put_work 中的工作项
 *
 * mmput_async() 将此函数提交到工作队列，以便在可睡眠的进程上下文中
 * 安全地执行 exit_mmap() 等可能睡眠的操作。
 * 通过 container_of() 从 work 指针反推出 mm_struct 指针。
 */
static void mmput_async_fn(struct work_struct *work)
{
	struct mm_struct *mm = container_of(work, struct mm_struct,
					    async_put_work); /* 从工作项反推 mm 指针 */

	__mmput(mm); /* 在工作队列（进程上下文）中执行真正的 mm 资源释放 */
}

/*
 * mmput_async - 异步释放 mm_struct 用户引用
 * @mm: 要释放引用的内存描述符
 *
 * 与 mmput() 的区别：此函数可在原子上下文（如中断处理）或不可睡眠的
 * 代码路径中调用。若 mm_users 降为 0，将清理工作推迟到工作队列中执行，
 * 避免在原子上下文中调用可睡眠的 exit_mmap() 等函数。
 * 典型使用场景：mmu_notifier 回调、page fault 快速路径等。
 */
void mmput_async(struct mm_struct *mm)
{
	if (atomic_dec_and_test(&mm->mm_users)) {
		/* mm_users 降为 0，初始化工作项并提交到系统工作队列 */
		INIT_WORK(&mm->async_put_work, mmput_async_fn);
		schedule_work(&mm->async_put_work); /* 异步调度，不在此处睡眠 */
	}
}
EXPORT_SYMBOL_GPL(mmput_async); /* 导出给需要异步释放 mm 引用的内核模块 */
#endif

/**
 * set_mm_exe_file - change a reference to the mm's executable file
 * @mm: The mm to change.
 * @new_exe_file: The new file to use.
 *
 * This changes mm's executable file (shown as symlink /proc/[pid]/exe).
 *
 * Main users are mmput() and sys_execve(). Callers prevent concurrent
 * invocations: in mmput() nobody alive left, in execve it happens before
 * the new mm is made visible to anyone.
 *
 * Can only fail if new_exe_file != NULL.
 */
int set_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file)
{
	struct file *old_exe_file;

	/*
	 * It is safe to dereference the exe_file without RCU as
	 * this function is only called if nobody else can access
	 * this mm -- see comment above for justification.
	 * 此处无需 RCU 读锁，因为调用者（mmput 或 execve 早期）保证
	 * 没有其他人并发访问此 mm，可以直接 raw 解引用。
	 */
	old_exe_file = rcu_dereference_raw(mm->exe_file); /* 绕过 RCU 检查直接读取旧的 exe_file */

	if (new_exe_file) {
		/*
		 * We expect the caller (i.e., sys_execve) to already denied
		 * write access, so this is unlikely to fail.
		 * execve 在设置 exe_file 前已调用 deny_write_access，正常不会失败。
		 * 拒绝写访问是为了防止有人在进程运行时修改可执行文件（text busy 保护）。
		 */
		if (unlikely(exe_file_deny_write_access(new_exe_file))) /* 增加文件的"写拒绝"计数 */
			return -EACCES;
		get_file(new_exe_file); /* 增加新 exe_file 的引用计数，mm 持有一个引用 */
	}
	rcu_assign_pointer(mm->exe_file, new_exe_file); /* RCU 赋值，保证并发读者看到一致值 */
	if (old_exe_file) {
		exe_file_allow_write_access(old_exe_file); /* 减少旧文件的写拒绝计数，恢复可写 */
		fput(old_exe_file); /* 释放 mm 对旧 exe_file 的引用 */
	}
	return 0;
}

/**
 * replace_mm_exe_file - replace a reference to the mm's executable file
 * @mm: The mm to change.
 * @new_exe_file: The new file to use.
 *
 * This changes mm's executable file (shown as symlink /proc/[pid]/exe).
 *
 * Main user is sys_prctl(PR_SET_MM_MAP/EXE_FILE).
 */
int replace_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file)
{
	struct vm_area_struct *vma;
	struct file *old_exe_file;
	int ret = 0;

	/* Forbid mm->exe_file change if old file still mapped.
	 * 安全检查：如果旧的可执行文件仍被映射到地址空间，则拒绝替换。
	 * 这防止了通过 prctl(PR_SET_MM_EXE_FILE) 伪造 /proc/pid/exe 的攻击向量。
	 */
	old_exe_file = get_mm_exe_file(mm); /* 获取当前 exe_file 的引用（RCU 安全） */
	if (old_exe_file) {
		VMA_ITERATOR(vmi, mm, 0); /* 初始化 VMA 迭代器，从地址 0 开始遍历 */
		mmap_read_lock(mm);       /* 读锁保护：并发遍历 VMA 列表 */
		for_each_vma(vmi, vma) {
			if (!vma->vm_file) /* 跳过匿名映射（没有关联文件的 VMA） */
				continue;
			if (path_equal(&vma->vm_file->f_path,
				       &old_exe_file->f_path)) {
				/* 发现旧 exe_file 仍被映射，拒绝替换 */
				ret = -EBUSY;
				break;
			}
		}
		mmap_read_unlock(mm);
		fput(old_exe_file); /* 释放 get_mm_exe_file() 增加的引用 */
		if (ret)
			return ret; /* 旧文件仍被映射，返回 -EBUSY */
	}

	ret = exe_file_deny_write_access(new_exe_file); /* 拒绝对新 exe 文件的写访问 */
	if (ret)
		return -EACCES; /* 文件无法设为只读（不可执行），拒绝 */
	get_file(new_exe_file); /* 增加新文件引用计数，mm 将持有一个引用 */

	/* set the new file */
	mmap_write_lock(mm); /* 写锁：修改 mm->exe_file，阻塞并发读者 */
	old_exe_file = rcu_dereference_raw(mm->exe_file); /* 持锁后再次读取旧值 */
	rcu_assign_pointer(mm->exe_file, new_exe_file);   /* RCU 赋值，原子更新指针 */
	mmap_write_unlock(mm);

	if (old_exe_file) {
		exe_file_allow_write_access(old_exe_file); /* 恢复旧文件的可写状态 */
		fput(old_exe_file); /* 释放 mm 对旧 exe_file 持有的引用 */
	}
	return 0;
}

/**
 * get_mm_exe_file - acquire a reference to the mm's executable file
 * @mm: The mm of interest.
 *
 * Returns %NULL if mm has no associated executable file.
 * User must release file via fput().
 */
struct file *get_mm_exe_file(struct mm_struct *mm)
{
	struct file *exe_file;

	rcu_read_lock(); /* 开启 RCU 读临界区，保护 mm->exe_file 指针免被并发释放 */
	exe_file = get_file_rcu(&mm->exe_file); /* RCU 安全地读取并增加文件引用计数 */
	rcu_read_unlock(); /* 退出 RCU 读临界区 */
	return exe_file;   /* 调用者负责用 fput() 释放此引用 */
}

/**
 * get_task_exe_file - acquire a reference to the task's executable file
 * @task: The task.
 *
 * Returns %NULL if task's mm (if any) has no associated executable file or
 * this is a kernel thread with borrowed mm (see the comment above get_task_mm).
 * User must release file via fput().
 */
struct file *get_task_exe_file(struct task_struct *task)
{
	struct file *exe_file = NULL;
	struct mm_struct *mm;

	/*
	 * 内核线程（PF_KTHREAD）可能借用用户进程的 mm（通过 kthread_use_mm），
	 * 但其 task->mm 并非真正属于它，不代表"可执行文件"，直接返回 NULL。
	 */
	if (task->flags & PF_KTHREAD)
		return NULL;

	task_lock(task); /* 获取 task 锁，防止 task->mm 在读取期间被并发修改（如 execve） */
	mm = task->mm;
	if (mm)
		exe_file = get_mm_exe_file(mm); /* 在 task 锁保护下安全地获取 exe_file 引用 */
	task_unlock(task);
	return exe_file; /* 调用者须用 fput() 释放引用；若无 mm 或 exe_file 则返回 NULL */
}

/**
 * get_task_mm - acquire a reference to the task's mm
 * @task: The task.
 *
 * Returns %NULL if the task has no mm.  Checks PF_KTHREAD (meaning
 * this kernel workthread has transiently adopted a user mm with kthread_use_mm,
 * to do its AIO) is not set and if so returns a reference to it, after
 * bumping up the use count.  User must release the mm via mmput()
 * after use.  Typically used by /proc and ptrace.
 */
struct mm_struct *get_task_mm(struct task_struct *task)
{
	struct mm_struct *mm;

	/*
	 * 内核线程（PF_KTHREAD）可能临时借用用户进程的 mm（kthread_use_mm），
	 * 此时 task->mm != NULL，但不代表真正的用户地址空间。
	 * 为避免误用，PF_KTHREAD 时直接返回 NULL。
	 */
	if (task->flags & PF_KTHREAD)
		return NULL;

	task_lock(task); /* 防止 task->mm 被并发的 execve/exit 修改 */
	mm = task->mm;
	if (mm)
		mmget(mm); /* 增加 mm_users 引用计数，确保 mm 在调用者使用期间不被释放 */
	task_unlock(task);
	return mm; /* 调用者必须用 mmput() 释放此引用 */
}
EXPORT_SYMBOL_GPL(get_task_mm); /* 导出给 /proc、ptrace、perf 等内核子系统使用 */

/*
 * may_access_mm - 检查当前进程是否有权限访问目标任务的 mm
 * @mm:   目标内存描述符
 * @task: 目标任务
 * @mode: 访问模式（PTRACE_MODE_READ / PTRACE_MODE_ATTACH 等）
 *
 * 三种情况允许访问：
 *   1. 访问自身 mm（mm == current->mm）：无条件允许
 *   2. 有 ptrace 权限（LSM/DAC 检查，考虑 dumpable、capabilities 等）
 *   3. 只读访问且具有 perfmon 能力（允许性能监控工具读取任意进程内存）
 */
static bool may_access_mm(struct mm_struct *mm, struct task_struct *task, unsigned int mode)
{
	if (mm == current->mm)              /* 访问自己的地址空间，始终允许 */
		return true;
	if (ptrace_may_access(task, mode))  /* 通过 ptrace 权限检查（含 LSM 钩子） */
		return true;
	if ((mode & PTRACE_MODE_READ) && perfmon_capable()) /* 性能监控工具的只读特权访问 */
		return true;
	return false;
}

/*
 * mm_access - 带权限检查地获取目标任务 mm 的引用
 * @task: 目标任务
 * @mode: 访问模式（PTRACE_MODE_READ 等）
 *
 * 在 exec_update_lock 读锁保护下获取 mm 并做权限检查：
 *   - exec_update_lock 防止在检查期间发生 execve，避免 TOCTTOU 竞争
 *     （即：检查时有权限，execve 后权限变了但 mm 已被拿走）
 *   - 成功时返回已增加 mm_users 引用的 mm，调用者须用 mmput() 释放
 *   - 失败时返回 ERR_PTR（-ESRCH 表示无 mm，-EACCES 表示权限不足）
 *
 * 主要用于 ptrace、/proc/pid/mem 等需要跨进程访问内存的场景。
 */
struct mm_struct *mm_access(struct task_struct *task, unsigned int mode)
{
	struct mm_struct *mm;
	int err;

	/*
	 * exec_update_lock 是读写信号量：execve 持写锁，读者在此持读锁。
	 * 这保证了在获取 mm 和检查权限的过程中，目标进程不会发生 execve。
	 * down_read_killable：可被致命信号中断，避免长时间阻塞。
	 */
	err =  down_read_killable(&task->signal->exec_update_lock);
	if (err)
		return ERR_PTR(err); /* 被信号中断，返回错误 */

	mm = get_task_mm(task); /* 获取 mm 并增加 mm_users 引用 */
	if (!mm) {
		mm = ERR_PTR(-ESRCH); /* 目标进程已退出或是内核线程，无 mm */
	} else if (!may_access_mm(mm, task, mode)) {
		mmput(mm);              /* 权限不足，释放刚获取的引用 */
		mm = ERR_PTR(-EACCES); /* 返回权限拒绝错误 */
	}
	up_read(&task->signal->exec_update_lock); /* 释放 exec_update 读锁 */

	return mm;
}

/*
 * complete_vfork_done - 子进程通知父进程 vfork 完成
 * @tsk: 调用 vfork 后的子进程（或执行 exec 的进程）
 *
 * vfork() 语义：父进程阻塞，直到子进程调用 exec() 或 exit()。
 * 子进程在 exec/exit 路径中调用此函数，通过 completion 机制唤醒父进程。
 * 使用 task_lock 保护对 vfork_done 的读-清零-complete 序列，
 * 防止与父进程被信号杀死时的并发清零（wait_for_vfork_done）产生竞争。
 */
static void complete_vfork_done(struct task_struct *tsk)
{
	struct completion *vfork;

	task_lock(tsk); /* 与 wait_for_vfork_done 中的 task_lock 互斥 */
	vfork = tsk->vfork_done;
	if (likely(vfork)) {
		tsk->vfork_done = NULL; /* 清零，防止重复 complete */
		complete(vfork);        /* 唤醒在 vfork_done 上等待的父进程 */
	}
	task_unlock(tsk);
}

/*
 * wait_for_vfork_done - 父进程等待 vfork 子进程完成 exec 或 exit
 * @child: vfork 创建的子进程
 * @vfork: 等待的 completion 对象（在父进程栈上分配）
 *
 * 父进程在 vfork() 调用后进入此函数等待，直到：
 *   - 子进程调用 exec()：complete_vfork_done() 唤醒父进程
 *   - 子进程调用 exit()：同上
 *   - 父进程被致命信号杀死（TASK_KILLABLE）：提前退出等待
 * TASK_FREEZABLE 允许在等待期间进入 cgroup freezer 冻结状态。
 * 返回 0 表示正常完成，非 0 表示被信号杀死。
 *
 * 重要：无论是否等到，最后都会 put_task_struct(child)，
 * 释放 fork 时 get_task_struct 增加的对子进程的引用。
 */
static int wait_for_vfork_done(struct task_struct *child,
				struct completion *vfork)
{
	unsigned int state = TASK_KILLABLE|TASK_FREEZABLE; /* 可被信号唤醒，也可被 freezer 冻结 */
	int killed;

	cgroup_enter_frozen();                             /* 通知 cgroup：父进程进入冻结态等待 */
	killed = wait_for_completion_state(vfork, state);  /* 阻塞等待子进程完成 vfork */
	cgroup_leave_frozen(false);                        /* 离开冻结等待状态 */

	if (killed) {
		/*
		 * 父进程被致命信号杀死，子进程可能还没来得及调用 complete。
		 * 此时父进程需要清零子进程的 vfork_done 指针，
		 * 防止子进程之后访问已失效的栈上 completion 对象（use-after-free）。
		 */
		task_lock(child);
		child->vfork_done = NULL; /* 防止子进程后续访问父进程已销毁的 completion */
		task_unlock(child);
	}

	put_task_struct(child); /* 释放 fork 时增加的对子进程的引用 */
	return killed;          /* 0=正常完成，非0=被信号中断 */
}

/* Please note the differences between mmput and mm_release.
 * mmput is called whenever we stop holding onto a mm_struct,
 * error success whatever.
 *
 * mm_release is called after a mm_struct has been removed
 * from the current process.
 *
 * This difference is important for error handling, when we
 * only half set up a mm_struct for a new process and need to restore
 * the old one.  Because we mmput the new mm_struct before
 * restoring the old one. . .
 * Eric Biederman 10 January 1998
 */
/*
 * mm_release - 从当前进程移除 mm_struct 时的清理工作
 * @tsk: 正在释放 mm 的任务
 * @mm:  被释放的内存描述符
 *
 * 注意：与 mmput() 的区别（见上方英文注释）：
 *   - mm_release：从进程移除 mm 时调用（进程不再使用该 mm）
 *   - mmput：停止持有 mm 引用时调用（可能仍有其他进程使用同一 mm）
 *
 * 主要工作：
 *   1. 释放 uprobes utask 资源
 *   2. 通知体系结构停用此 mm（清除缓存的寄存器状态/TLS 等）
 *   3. 处理 clear_child_tid（CLONE_CHILD_CLEARTID 语义，glibc 线程库使用）
 *   4. 完成 vfork 通知（如果是 vfork 子进程）
 */
static void mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	uprobe_free_utask(tsk); /* 释放此任务相关的 uprobes utask 结构 */

	/* Get rid of any cached register state
	 * 通知体系结构停用此 mm：清除 TLS、浮点状态等缓存的用户态寄存器 */
	deactivate_mm(tsk, mm);

	/*
	 * Signal userspace if we're not exiting with a core dump
	 * because we want to leave the value intact for debugging
	 * purposes.
	 *
	 * clear_child_tid 实现 CLONE_CHILD_CLEARTID 语义：
	 * 线程退出时向用户态指定地址写 0，并唤醒等待该地址的 futex。
	 * glibc 的 pthread_join() 依赖此机制。
	 * 仅当 mm_users > 1（还有其他线程）时才触发，coredump 路径跳过（避免干扰调试）。
	 */
	if (tsk->clear_child_tid) {
		if (atomic_read(&mm->mm_users) > 1) {
			/*
			 * We don't check the error code - if userspace has
			 * not set up a proper pointer then tough luck.
			 * 向用户空间地址写 0，即使用户空间指针无效也不检查错误。
			 */
			put_user(0, tsk->clear_child_tid); /* 向线程 ID 地址写 0 */
			do_futex(tsk->clear_child_tid, FUTEX_WAKE,
					1, NULL, NULL, 0, 0); /* 唤醒等待该地址的线程（如 pthread_join） */
		}
		tsk->clear_child_tid = NULL; /* 清除指针，防止重复处理 */
	}

	/*
	 * All done, finally we can wake up parent and return this mm to him.
	 * Also kthread_stop() uses this completion for synchronization.
	 * vfork 子进程在 exec/exit 时通知父进程，kthread_stop() 也用此机制同步。
	 */
	if (tsk->vfork_done)
		complete_vfork_done(tsk); /* 唤醒正在 wait_for_vfork_done() 的父进程 */
}

/*
 * exit_mm_release - 进程退出时释放 mm 前的清理工作
 * @tsk: 正在退出的任务
 * @mm:  被释放的内存描述符
 *
 * 在进程 exit() 路径中调用。先处理 futex 退出逻辑（释放所有 robust futex），
 * 再执行通用的 mm_release 清理（deactivate_mm、clear_child_tid、vfork 通知等）。
 */
void exit_mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	futex_exit_release(tsk); /* 处理 robust futex 列表：通知所有等待者此进程已死（pi_state 处理） */
	mm_release(tsk, mm);     /* 通用 mm 释放清理 */
}

/*
 * exec_mm_release - execve 时替换旧 mm 前的清理工作
 * @tsk: 正在执行 exec 的任务
 * @mm:  旧的（被替换的）内存描述符
 *
 * 在 execve() 路径中调用，处理旧 mm 的清理。
 * 与 exit_mm_release 的区别：exec 场景下 futex 的清理方式不同
 * （exec 不需要做 robust futex 通知，只需清理 futex 内部状态）。
 */
void exec_mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	futex_exec_release(tsk); /* exec 场景的 futex 清理（非 robust，仅清内部状态） */
	mm_release(tsk, mm);     /* 通用 mm 释放清理（deactivate_mm、vfork 通知等） */
}

/**
 * dup_mm() - duplicates an existing mm structure
 * @tsk: the task_struct with which the new mm will be associated.
 * @oldmm: the mm to duplicate.
 *
 * Allocates a new mm structure and duplicates the provided @oldmm structure
 * content into it.
 *
 * Return: the duplicated mm or NULL on failure.
 */
static struct mm_struct *dup_mm(struct task_struct *tsk,
				struct mm_struct *oldmm)
{
	struct mm_struct *mm;
	int err;

	mm = allocate_mm(); /* 从 mm_cachep slab 分配新的 mm_struct */
	if (!mm)
		goto fail_nomem;

	/*
	 * 先浅拷贝父 mm 的所有字段，再由 mm_init() 重新初始化各种锁、
	 * 引用计数、per-CPU 计数器等不能直接继承的字段。
	 * 这种"先拷贝后重初始化"的方式可以继承父进程的配置（如 def_flags）。
	 */
	memcpy(mm, oldmm, sizeof(*mm)); /* 浅拷贝父 mm，包含 flags、def_flags 等配置 */

	if (!mm_init(mm, tsk)) /* 重新初始化锁、引用计数、页表等，失败则 mm_init 内部已 free */
		goto fail_nomem;

	uprobe_start_dup_mmap(); /* 通知 uprobes 即将开始复制地址映射（用于断点管理） */
	err = dup_mmap(mm, oldmm); /* 复制父进程的所有 VMA 和页表到新 mm */
	if (err)
		goto free_pt;
	uprobe_end_dup_mmap(); /* 通知 uprobes 地址映射复制完成 */

	/* 用复制后的实际 RSS 更新高水位标记（用于 /proc/pid/status 中的 VmPeak 等） */
	mm->hiwater_rss = get_mm_rss(mm);  /* 初始化 RSS 高水位线为当前实际 RSS */
	mm->hiwater_vm = mm->total_vm;     /* 初始化虚拟内存高水位线为当前总虚拟内存 */

	if (mm->binfmt && !try_module_get(mm->binfmt->module)) /* 增加 binfmt 模块引用计数 */
		goto free_pt; /* 模块即将卸载，不能使用 */

	return mm; /* 成功返回复制好的新 mm */

free_pt:
	/* don't put binfmt in mmput, we haven't got module yet
	 * 注意：binfmt 模块引用尚未获取（try_module_get 失败或还未调用），
	 * 必须在调用 mmput 前清零，否则 mmput/__mmput 会尝试 module_put 导致错误。 */
	mm->binfmt = NULL;
	mm_init_owner(mm, NULL); /* 清除 memcg owner，避免对错误的 cgroup 记账 */
	mmput(mm);               /* 释放新 mm（mm_users 降为 0，触发 __mmput 清理） */
	if (err)
		uprobe_end_dup_mmap(); /* dup_mmap 失败时也需要通知 uprobes 结束 */

fail_nomem:
	return NULL;
}

/*
 * copy_mm - fork/clone 时为新任务设置内存描述符
 * @clone_flags: clone 标志，决定如何处理 mm
 * @tsk: 新创建的任务
 *
 * 处理三种情况：
 *   1. 内核线程（current->mm == NULL）：无用户地址空间，直接返回 0
 *   2. CLONE_VM（线程）：共享父进程的 mm，只增加 mm_users 引用计数
 *   3. 普通 fork：调用 dup_mm() 深拷贝父进程的整个地址空间
 *
 * 注意：active_mm 与 mm 的区别：
 *   - mm：进程"拥有"的地址空间（用户进程才有）
 *   - active_mm：当前正在使用的地址空间（内核线程借用某进程的 mm 时，
 *     mm==NULL 但 active_mm 非空）
 */
static int copy_mm(u64 clone_flags, struct task_struct *tsk)
{
	struct mm_struct *mm, *oldmm;

	tsk->min_flt = tsk->maj_flt = 0; /* 清零缺页统计（minor/major fault 计数） */
	tsk->nvcsw = tsk->nivcsw = 0;    /* 清零上下文切换统计（自愿/非自愿） */
#ifdef CONFIG_DETECT_HUNG_TASK
	tsk->last_switch_count = tsk->nvcsw + tsk->nivcsw; /* 初始化 hung task 检测的基准值 */
	tsk->last_switch_time = 0;
#endif

	tsk->mm = NULL;        /* 先置 NULL，后面按需赋值 */
	tsk->active_mm = NULL;

	/*
	 * Are we cloning a kernel thread?
	 *
	 * We need to steal a active VM for that..
	 * current->mm == NULL 说明 current 是内核线程，新任务也是内核线程，无需 mm。
	 */
	oldmm = current->mm;
	if (!oldmm)
		return 0; /* 内核线程不需要用户地址空间，直接返回 */

	if (clone_flags & CLONE_VM) {
		/* CLONE_VM：共享父进程地址空间（线程创建），只增加引用计数 */
		mmget(oldmm); /* 增加 mm_users，新线程持有一个引用 */
		mm = oldmm;
	} else {
		/* 普通 fork：复制整个地址空间（COW 写时复制） */
		mm = dup_mm(tsk, current->mm);
		if (!mm)
			return -ENOMEM; /* 内存不足，dup_mm 失败 */
	}

	tsk->mm = mm;        /* 设置新任务的用户地址空间 */
	tsk->active_mm = mm; /* 同时设为活跃 mm（用户进程 mm == active_mm） */
	return 0;
}

/*
 * copy_exec_state - fork/clone 时为新任务处理 exec_state
 * @clone_flags: clone 标志
 * @tsk: 新创建的任务
 *
 * exec_state 保存与 execve 相关的状态（如 no_new_privs 等安全属性）。
 * 处理两种情况：
 *   1. CLONE_VM（线程）：共享父进程的 exec_state，通过引用计数共享，
 *      避免内存浪费，且线程组内所有线程的 exec 状态应保持一致。
 *   2. 普通 fork：创建一个父进程 exec_state 的独立副本，
 *      子进程可以独立修改（如 execve 后继承新状态）。
 */
static int copy_exec_state(u64 clone_flags, struct task_struct *tsk)
{
	struct task_exec_state *exec_state;

	/* CLONE_VM siblings refcount-share the parent's exec_state.
	 * 线程（CLONE_VM）直接共享父进程的 exec_state，增加引用计数即可。 */
	if (clone_flags & CLONE_VM) {
		exec_state = rcu_dereference_protected(current->exec_state, true); /* 获取父进程的 exec_state */
		refcount_inc(&exec_state->count);                  /* 增加共享引用计数 */
		rcu_assign_pointer(tsk->exec_state, exec_state);  /* RCU 赋值给新任务 */
		return 0;
	}

	/* Everyone else inherits a fresh copy.
	 * 普通 fork：创建独立的 exec_state 副本，子进程可独立修改。 */
	return task_exec_state_copy(tsk); /* 分配并复制父进程的 exec_state */
}

/*
 * copy_fs - fork/clone 时为新任务处理文件系统命名空间（fs_struct）
 * @clone_flags: clone 标志
 * @tsk: 新创建的任务
 *
 * fs_struct 保存进程的根目录（root）、当前目录（pwd）及 umask。
 * 处理两种情况：
 *   1. CLONE_FS（共享 fs）：与父进程共享 fs_struct，增加 users 计数。
 *      但若父进程正在执行 exec（in_exec），则拒绝（返回 -EAGAIN），
 *      因为 exec 可能改变 root/pwd，与共享语义冲突（check_unsafe_exec 用到）。
 *   2. 普通 fork：深拷贝父进程的 fs_struct，子进程独立管理 cwd/root。
 *
 * 注意：copy_fs_struct() 会复制 root/pwd 并增加对应目录的引用计数。
 */
static int copy_fs(u64 clone_flags, struct task_struct *tsk)
{
	struct fs_struct *fs = current->fs;
	if (clone_flags & CLONE_FS) {
		/* tsk->fs is already what we want
		 * task_struct 创建时已从父进程复制了 fs 指针，这里只需增加计数 */
		read_seqlock_excl(&fs->seq); /* 独占读序列锁，保护 users 和 in_exec 的一致性读 */
		/* "users" and "in_exec" locked for check_unsafe_exec()
		 * 在持锁期间检查 in_exec，与 check_unsafe_exec() 中的检查互斥 */
		if (fs->in_exec) {
			/* 父进程正在 exec，此时共享 fs 不安全，返回 -EAGAIN 让调用者重试 */
			read_sequnlock_excl(&fs->seq);
			return -EAGAIN;
		}
		fs->users++; /* 增加 fs_struct 的用户计数，新任务也持有一个引用 */
		read_sequnlock_excl(&fs->seq);
		return 0;
	}
	tsk->fs = copy_fs_struct(fs); /* 深拷贝 fs_struct（复制 root/pwd 并增加 dentry 引用） */
	if (!tsk->fs)
		return -ENOMEM;
	return 0;
}

/*
 * copy_files - fork/clone 时为新任务处理打开文件描述符表
 * @clone_flags: clone 标志
 * @tsk: 新创建的任务
 * @no_files: 非零则创建无文件描述符的任务（如某些内核工作线程）
 *
 * 处理三种情况：
 *   1. 父进程无文件（守护进程关闭所有 fd 后）：直接返回 0
 *   2. no_files：新任务不需要文件描述符（如 idle 线程），置 NULL
 *   3. CLONE_FILES（线程）：共享父进程的 files_struct，增加引用计数
 *      注：共享 files_struct 意味着一个线程关闭 fd 会影响其他线程
 *   4. 普通 fork：通过 dup_fd() 深拷贝文件描述符表（COW 不适用于 fd 表）
 *
 * tsk->files 在 task_struct 创建时已从父进程拷贝（浅拷贝），
 * 这里负责按照 clone_flags 进行正确的引用处理。
 */
static int copy_files(u64 clone_flags, struct task_struct *tsk,
		      int no_files)
{
	struct files_struct *oldf, *newf;

	/*
	 * A background process may not have any files ...
	 * 后台进程（如某些守护进程）可能已关闭所有文件，files 为 NULL
	 */
	oldf = current->files;
	if (!oldf)
		return 0; /* 父进程无文件描述符表，子进程也无需处理 */

	if (no_files) {
		tsk->files = NULL; /* 明确要求新任务无文件描述符 */
		return 0;
	}

	if (clone_flags & CLONE_FILES) {
		/* 线程：共享父进程的文件描述符表，原子增加引用计数 */
		atomic_inc(&oldf->count);
		return 0; /* tsk->files 已在 task_struct 创建时指向 oldf */
	}

	/* 普通 fork：复制文件描述符表（复制 fd 数组，继承 O_CLOEXEC 等标志） */
	newf = dup_fd(oldf, NULL); /* NULL 表示不限制 fd 数量上限 */
	if (IS_ERR(newf))
		return PTR_ERR(newf); /* 内存不足或其他错误 */

	tsk->files = newf; /* 新任务使用独立的文件描述符表 */
	return 0;
}

/*
 * copy_sighand - fork/clone 时为新任务处理信号处理器表
 * @clone_flags: clone 标志
 * @tsk: 新创建的任务
 *
 * sighand_struct 保存进程的信号处理函数表（每个信号的 sa_handler、sa_flags 等）。
 * 处理两种情况：
 *   1. CLONE_SIGHAND（线程）：共享信号处理器，增加引用计数。
 *      注：共享 sighand 意味着一个线程 signal() 修改处理器会影响整个线程组。
 *      约束：CLONE_SIGHAND 必须同时设置 CLONE_VM（内核强制要求）。
 *   2. 普通 fork：分配新的 sighand_struct，在持 siglock 的情况下复制信号处理表，
 *      保证复制期间不会有并发的信号处理函数修改。
 *
 * CLONE_CLEAR_SIGHAND（execve 路径使用）：复制后将非 SIG_IGN 的处理器重置为 SIG_DFL，
 * 实现 exec 语义（exec 后信号处理器恢复默认，但 SIG_IGN 保留）。
 */
static int copy_sighand(u64 clone_flags, struct task_struct *tsk)
{
	struct sighand_struct *sig;

	if (clone_flags & CLONE_SIGHAND) {
		/* 线程：共享父进程的信号处理器表，只增加引用计数 */
		refcount_inc(&current->sighand->count);
		return 0; /* tsk->sighand 已在 task_struct 创建时指向父进程的 sighand */
	}
	sig = kmem_cache_alloc(sighand_cachep, GFP_KERNEL); /* 从 slab 分配新的 sighand_struct */
	RCU_INIT_POINTER(tsk->sighand, sig); /* 先设置指针（即使 sig 为 NULL），保证 RCU 安全 */
	if (!sig)
		return -ENOMEM;

	refcount_set(&sig->count, 1); /* 新的 sighand，初始引用计数为 1 */
	spin_lock_irq(&current->sighand->siglock); /* 持 siglock 防止并发修改信号处理函数 */
	memcpy(sig->action, current->sighand->action, sizeof(sig->action)); /* 复制所有信号的处理函数配置 */
	spin_unlock_irq(&current->sighand->siglock);

	/* Reset all signal handler not set to SIG_IGN to SIG_DFL.
	 * CLONE_CLEAR_SIGHAND 在 execve 路径中设置，实现 exec 后信号处理器复位语义。
	 * SIG_IGN 的处理器不复位（POSIX 要求 exec 后保留 SIG_IGN）。 */
	if (clone_flags & CLONE_CLEAR_SIGHAND)
		flush_signal_handlers(tsk, 0); /* 将非 SIG_IGN 的处理器重置为 SIG_DFL */

	return 0;
}

/*
 * __cleanup_sighand - 释放对 sighand_struct 的一个引用
 * @sighand: 要释放的信号处理器表
 *
 * 原子减少引用计数，若降为 0 则真正释放：
 *   1. 先通知 signalfd 清理（关闭所有监听此 sighand 的 signalfd 文件）
 *   2. 再释放 slab 内存
 *
 * 重要：sighand_cachep 使用 SLAB_TYPESAFE_BY_RCU 标志，这意味着：
 *   - slab 对象可以在 RCU 宽限期结束前被重新分配
 *   - 但对象所在内存页在 RCU 宽限期内保持有效
 *   - 因此可以在不等待 RCU 宽限期的情况下直接 free
 *   - __lock_task_sighand() 利用此特性安全地 RCU 读取 sighand 指针
 */
void __cleanup_sighand(struct sighand_struct *sighand)
{
	if (refcount_dec_and_test(&sighand->count)) {
		/* 引用计数降为 0，执行最终清理 */
		signalfd_cleanup(sighand); /* 通知所有监听此 sighand 的 signalfd 实例关闭 */
		/*
		 * sighand_cachep is SLAB_TYPESAFE_BY_RCU so we can free it
		 * without an RCU grace period, see __lock_task_sighand().
		 * SLAB_TYPESAFE_BY_RCU：无需等待 RCU 宽限期即可直接释放内存到 slab。
		 */
		kmem_cache_free(sighand_cachep, sighand); /* 将 sighand_struct 归还 slab 缓存 */
	}
}

/*
 * posix_cpu_timers_init_group - 初始化线程组的 POSIX CPU 时间计时器
 * @sig: 线程组的 signal_struct
 *
 * POSIX CPU 时间计时器（CLOCK_PROCESS_CPUTIME_ID 等）以整个线程组为单位计时。
 * 此函数在新进程创建时（copy_signal 中）初始化 posix_cputimers 结构，
 * 并将 RLIMIT_CPU 软限制作为初始到期阈值写入（超过此限制发送 SIGXCPU）。
 * READ_ONCE 防止编译器对 rlim_cur 的读取进行优化（避免 TOCTOU 问题）。
 */
static void posix_cpu_timers_init_group(struct signal_struct *sig)
{
	struct posix_cputimers *pct = &sig->posix_cputimers;
	unsigned long cpu_limit;

	cpu_limit = READ_ONCE(sig->rlim[RLIMIT_CPU].rlim_cur); /* 读取 CPU 时间软限制（秒） */
	posix_cputimers_group_init(pct, cpu_limit); /* 用 CPU 限制初始化线程组级 POSIX 计时器 */
}

/*
 * copy_signal - fork 时为新进程创建独立的 signal_struct
 * @clone_flags: clone 标志
 * @tsk: 新创建的任务
 *
 * signal_struct 是线程组级别的结构，保存整个进程（线程组）共享的信号状态：
 *   - 线程计数（nr_threads、live）
 *   - 共享待处理信号队列（shared_pending）
 *   - POSIX 定时器（real_timer、posix_timers）
 *   - 资源限制（rlim）
 *   - OOM 调整值、TTY、cgroup 锁等
 *
 * CLONE_THREAD（线程创建）：线程共享父进程的 signal_struct，无需创建新的，
 * 直接返回 0（tsk->signal 已在 task_struct 创建时指向父进程的 signal）。
 *
 * 普通 fork：分配新的 signal_struct，完整初始化后子进程拥有独立的信号状态。
 */
static int copy_signal(u64 clone_flags, struct task_struct *tsk)
{
	struct signal_struct *sig;

	if (clone_flags & CLONE_THREAD)
		return 0; /* 线程共享 signal_struct，无需新建 */

	sig = kmem_cache_zalloc(signal_cachep, GFP_KERNEL); /* 分配并清零新的 signal_struct */
	tsk->signal = sig;
	if (!sig)
		return -ENOMEM;

	sig->nr_threads = 1;   /* 新进程初始只有一个线程 */
	sig->quick_threads = 1; /* 快速线程计数（用于快速判断是否多线程） */
	atomic_set(&sig->live, 1);      /* 存活线程数为 1（live 降为 0 触发 group exit） */
	refcount_set(&sig->sigcnt, 1);  /* signal_struct 引用计数初始为 1 */

	/*
	 * list_add(thread_node, thread_head) without INIT_LIST_HEAD()
	 * 巧妙地将 tsk->thread_node 加入 sig->thread_head 链表，
	 * 同时初始化两个链表头，使 thread_head 和 thread_node 互相指向对方。
	 * 这比先 INIT_LIST_HEAD 再 list_add 少一次写操作。
	 */
	sig->thread_head = (struct list_head)LIST_HEAD_INIT(tsk->thread_node);
	tsk->thread_node = (struct list_head)LIST_HEAD_INIT(sig->thread_head);

	init_waitqueue_head(&sig->wait_chldexit); /* 初始化 wait4()/waitpid() 的等待队列 */
	sig->curr_target = tsk;    /* 信号投递目标（轮转选择线程）初始化为新进程自身 */
	init_sigpending(&sig->shared_pending); /* 初始化线程组共享的待处理信号队列 */
	INIT_HLIST_HEAD(&sig->multiprocess);   /* 初始化多进程 futex 哈希链表 */
	seqlock_init(&sig->stats_lock);        /* 初始化统计信息（CPU 时间等）的序列锁 */
	prev_cputime_init(&sig->prev_cputime); /* 初始化上一次 CPU 时间快照（用于 /proc 统计） */

#ifdef CONFIG_POSIX_TIMERS
	INIT_HLIST_HEAD(&sig->posix_timers);         /* 初始化 POSIX 定时器链表（timer_create 创建的） */
	INIT_HLIST_HEAD(&sig->ignored_posix_timers); /* 初始化被忽略的 POSIX 定时器链表 */
	/* 初始化 ITIMER_REAL（setitimer）对应的高精度定时器，到期触发 SIGALRM */
	hrtimer_setup(&sig->real_timer, it_real_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#endif

	/* 从父进程的线程组 leader 处复制资源限制（rlim 数组），需持 task_lock 保护 */
	task_lock(current->group_leader);
	memcpy(sig->rlim, current->signal->rlim, sizeof sig->rlim); /* 继承所有 RLIMIT_* 限制 */
	task_unlock(current->group_leader);

	posix_cpu_timers_init_group(sig); /* 初始化线程组级 POSIX CPU 计时器 */

	tty_audit_fork(sig);       /* 复制 TTY audit 状态（用于安全审计） */
	sched_autogroup_fork(sig); /* 初始化调度器自动分组（CONFIG_SCHED_AUTOGROUP） */

#ifdef CONFIG_CGROUPS
	/* 初始化 cgroup 线程组读写信号量，用于 cgroup 迁移和 fork/exec 的同步 */
	init_rwsem(&sig->cgroup_threadgroup_rwsem);
#endif

	/* 继承父进程的 OOM 评分调整值（/proc/pid/oom_score_adj），影响 OOM killer 选择 */
	sig->oom_score_adj = current->signal->oom_score_adj;
	sig->oom_score_adj_min = current->signal->oom_score_adj_min;

	mutex_init(&sig->cred_guard_mutex); /* 初始化凭证保护互斥锁（保护 exec 和 ptrace 的并发） */
	init_rwsem(&sig->exec_update_lock); /* 初始化 exec 更新读写信号量（mm_access 等需要此锁） */

	return 0;
}

/*
 * 【copy_seccomp】将父进程的 seccomp 过滤器继承给新进程
 *
 * seccomp（Secure Computing Mode）是 Linux 的系统调用过滤机制，允许进程（或容器）
 * 通过 BPF 程序限制自身可以使用的系统调用集合，是容器安全隔离的核心机制之一。
 *
 * 为何必须在 sighand->siglock 持有期间调用：
 *   sighand->siglock 是整个线程组共享的锁（因为 CLONE_SIGHAND 使所有线程共享
 *   sighand），在此锁保护下操作 seccomp 状态，可以保证：
 *   1. 同线程组的所有线程看到一致的 seccomp 策略（seccomp 过滤器是线程组级别的）
 *   2. 防止 exec() 与 fork() 并发时出现竞态——exec 路径会先获取 cred_guard_mutex，
 *      而 fork 路径在复制 task_struct 后、持 sighand 锁之间存在一个竞态窗口：
 *      父进程可能在此窗口内通过 prctl(PR_SET_SECCOMP) 修改了 seccomp 状态，
 *      所以需要在 sighand 锁保护下重新同步 seccomp 和 no_new_privs 两个字段。
 *
 * no_new_privs（nnp）与 seccomp 的关系：
 *   prctl(PR_SET_NO_NEW_PRIVS, 1) 设置后，进程不能通过 execve 获得新的特权（如
 *   setuid 位），这是 seccomp FILTER 模式（SECCOMP_MODE_FILTER）的前提条件之一。
 *   二者必须保持一致，否则可能出现有 filter 但没有 nnp 的异常状态。
 */
static void copy_seccomp(struct task_struct *p)
{
#ifdef CONFIG_SECCOMP
	/*
	 * Must be called with sighand->lock held, which is common to
	 * all threads in the group. Holding cred_guard_mutex is not
	 * needed because this new task is not yet running and cannot
	 * be racing exec.
	 */
	assert_spin_locked(&current->sighand->siglock); /* 断言调用者已持有 siglock */

	/* Ref-count the new filter user, and assign it. */
	get_seccomp_filter(current); /* 增加父进程 seccomp filter 的引用计数（filter 是引用计数共享的） */
	p->seccomp = current->seccomp; /* 将父进程的 seccomp 结构体浅拷贝给子进程（含 mode 和 filter 指针） */

	/*
	 * Explicitly enable no_new_privs here in case it got set
	 * between the task_struct being duplicated and holding the
	 * sighand lock. The seccomp state and nnp must be in sync.
	 */
	/*
	 * 竞态窗口修复：dup_task_struct() 之后、获取 sighand 锁之前，父进程可能
	 * 调用了 prctl(PR_SET_NO_NEW_PRIVS)，所以此处重新检查并同步 nnp 标志。
	 * seccomp filter 模式要求 nnp=1，二者必须保持一致。
	 */
	if (task_no_new_privs(current))
		task_set_no_new_privs(p); /* 确保子进程的 no_new_privs 与父进程一致 */

	/*
	 * If the parent gained a seccomp mode after copying thread
	 * flags and between before we held the sighand lock, we have
	 * to manually enable the seccomp thread flag here.
	 */
	/*
	 * 类似地，若父进程在竞态窗口内切换到了某个 seccomp 模式，
	 * 子进程 task_struct 中的 syscall_work 标志可能没有 SECCOMP 位，
	 * 此处手动补设，确保子进程进入内核时会经过 seccomp 过滤路径。
	 */
	if (p->seccomp.mode != SECCOMP_MODE_DISABLED)
		set_task_syscall_work(p, SECCOMP); /* 在 syscall_work 中打开 SECCOMP 位，使调度路径触发过滤 */
#endif
}

/*
 * 【set_tid_address 系统调用】设置线程退出时的 TID 清零地址
 *
 * 这是 POSIX 线程库（如 glibc 的 NPTL）用于实现线程退出通知的机制：
 *   - 调用 set_tid_address(tidptr) 后，当前线程退出时内核会：
 *     1. 将 *tidptr 清零（写入 0）
 *     2. 对 tidptr 指向的地址执行 futex_wake，唤醒等待在该地址的线程
 *   - pthread_join() 底层就是对 clear_child_tid 地址做 futex_wait，
 *     等待被等待线程退出时内核触发 futex_wake
 *
 * 与 set_child_tid 的区别：
 *   - set_child_tid（通过 clone CLONE_CHILD_SETTID 标志）：子线程启动时写入自己的 TID
 *   - clear_child_tid（本系统调用）：线程退出时清零并唤醒等待者
 *
 * 返回值：当前线程在当前 PID 命名空间中的虚拟 TID（vnr = virtual number）。
 */
SYSCALL_DEFINE1(set_tid_address, int __user *, tidptr)
{
	current->clear_child_tid = tidptr; /* 记录退出时需要清零的用户态地址，do_exit() 中会用到 */

	return task_pid_vnr(current); /* 返回当前线程在本 PID 命名空间的 TID（容器内看到的 TID） */
}

/*
 * 【rt_mutex_init_task】初始化新进程的优先级继承（PI）互斥锁相关字段
 *
 * Priority Inheritance（优先级继承）是解决优先级反转问题的经典机制：
 *   - 场景：低优先级任务 L 持有锁，高优先级任务 H 等待该锁，
 *     中优先级任务 M 抢占 L 导致 H 被间接阻塞——这是"优先级反转"。
 *   - 解决：当 H 等待 L 持有的锁时，内核临时将 L 的优先级提升到 H 的级别，
 *     使 L 能够尽快运行并释放锁，这称为"优先级继承"。
 *   - Linux 的 rt_mutex（实时互斥锁）实现了 PI 机制，pi_waiters 红黑树记录
 *     所有因等待本任务持有的锁而被阻塞的任务，按优先级排序。
 *
 * 字段说明：
 *   - pi_lock：保护 PI 相关字段的原始自旋锁（raw，不可抢占，RT 内核下也直接自旋）
 *   - pi_waiters：等待本任务释放锁的任务集合（红黑树，按优先级有序），
 *     用于快速找到最高优先级等待者
 *   - pi_top_task：pi_waiters 中优先级最高的任务（快捷缓存，避免每次遍历树）
 *   - pi_blocked_on：本任务当前阻塞在哪个 rt_mutex_waiter 上（NULL 表示未阻塞）
 */
static void rt_mutex_init_task(struct task_struct *p)
{
	raw_spin_lock_init(&p->pi_lock); /* 初始化 PI 保护锁（raw_spinlock，不受 RT 抢占影响） */
#ifdef CONFIG_RT_MUTEXES
	p->pi_waiters = RB_ROOT_CACHED;  /* 初始化等待者红黑树为空（RB_ROOT_CACHED 含最左缓存，O(1)取最高优先级） */
	p->pi_top_task = NULL;           /* 当前无最高优先级等待者 */
	p->pi_blocked_on = NULL;         /* 新进程未阻塞在任何 rt_mutex 上 */
#endif
}

/*
 * 【init_task_pid_links】初始化新进程的 PID 哈希链表节点
 *
 * Linux 内核维护多个 PID 相关的哈希表，每种 PID 类型（PIDTYPE）对应一条链：
 *   - PIDTYPE_PID  (0)：进程/线程自身的 PID，每个 task 独有
 *   - PIDTYPE_TGID (1)：线程组 ID（Thread Group ID），即用户态看到的 PID，
 *                       同一线程组的所有线程共享同一个 TGID
 *   - PIDTYPE_PGID (2)：进程组 ID（Process Group ID），用于 kill(-pgid) 等操作
 *   - PIDTYPE_SID  (3)：会话 ID（Session ID），用于终端控制
 *
 * 每个 task_struct 通过 pid_links[PIDTYPE_MAX] 数组中的 hlist_node 挂入
 * 对应 struct pid 的 tasks[] 链表。新进程创建时，这些节点必须先初始化为
 * 独立的空节点（INIT_HLIST_NODE），否则后续 hlist_add/del 操作会访问
 * 父进程遗留的非法指针，导致链表损坏。
 */
static inline void init_task_pid_links(struct task_struct *task)
{
	enum pid_type type;

	/* 遍历所有 PID 类型，将每个 hlist_node 初始化为孤立节点（pprev=NULL, next=NULL） */
	for (type = PIDTYPE_PID; type < PIDTYPE_MAX; ++type)
		INIT_HLIST_NODE(&task->pid_links[type]);
}

/*
 * 【init_task_pid】将 struct pid 关联到新进程的指定 PID 类型槽位
 *
 * task_struct 中存储 PID 有两处，分别针对不同的语义：
 *
 *   1. task->thread_pid（仅 PIDTYPE_PID 使用）：
 *      指向该线程自身的 struct pid，每个线程都有唯一的 TID（Thread ID）。
 *      这是 gettid() 系统调用返回的值，在线程级别唯一。
 *
 *   2. task->signal->pids[type]（PIDTYPE_TGID / PIDTYPE_PGID / PIDTYPE_SID）：
 *      存放在 signal_struct 中，因为同一线程组的所有线程共享 signal_struct，
 *      所以这些 PID 自然也被所有线程共享。例如：
 *        - signal->pids[PIDTYPE_TGID]：线程组 PID（用户态的 getpid() 返回值）
 *        - signal->pids[PIDTYPE_PGID]：进程组 PID（getpgrp() 返回值）
 *        - signal->pids[PIDTYPE_SID] ：会话 PID（getsid() 返回值）
 *
 * 这种设计使得"线程私有 PID"与"线程组共享 PID"在存储上自然分离，
 * task_pid(task)、task_tgid(task) 等内联函数封装了这种区别。
 */
static inline void
init_task_pid(struct task_struct *task, enum pid_type type, struct pid *pid)
{
	if (type == PIDTYPE_PID)
		task->thread_pid = pid;         /* 线程私有 TID：直接存在 task_struct 中 */
	else
		task->signal->pids[type] = pid; /* 线程组共享 PID（TGID/PGID/SID）：存在共享的 signal_struct 中 */
}

/*
 * 【rcu_copy_process】为新进程重置/初始化所有 RCU 相关状态
 *
 * RCU（Read-Copy-Update）是 Linux 内核的核心同步机制，允许读者无锁并发访问，
 * 写者通过"宽限期"（grace period）延迟释放旧数据。内核有三种 RCU 变体：
 *
 * 1. PREEMPT_RCU（可抢占 RCU，CONFIG_PREEMPT_RCU）：
 *    用于可抢占内核（如 RT 内核）。读侧临界区（rcu_read_lock/unlock）可被抢占，
 *    因此内核需要跟踪每个任务的 RCU 读锁嵌套深度和特殊解锁标志。
 *    - rcu_read_lock_nesting：当前 RCU 读锁嵌套层数，>0 表示在临界区内
 *    - rcu_read_unlock_special：解锁时需要特殊处理的标志（如延迟回调、需要退出临界区等）
 *    - rcu_blocked_node：任务在 RCU 宽限期树（rcu_node）上的阻塞节点
 *    - rcu_node_entry：将任务链入 rcu_node 的链表节点
 *    新进程从未进入任何 RCU 读临界区，所以全部清零。
 *
 * 2. TASKS_RCU（任务 RCU，CONFIG_TASKS_RCU）：
 *    以"任务调度"为宽限期边界，宽限期等待所有任务都经历一次调度切换。
 *    主要用于需要等待所有正在执行特定代码路径的任务退出（如 trampoline、ftrace）。
 *    - rcu_tasks_holdout：宽限期检查时该任务是否"拖后腿"（未完成调度切换）
 *    - rcu_tasks_holdout_list：拖后腿任务链表节点
 *    - rcu_tasks_idle_cpu：任务在哪个 CPU 上处于 idle 状态（-1 表示非 idle）
 *    - rcu_tasks_exit_list：任务退出时的清理链表节点
 *
 * 3. TASKS_TRACE_RCU（跟踪任务 RCU，CONFIG_TASKS_TRACE_RCU）：
 *    专为 BPF/tracing 基础设施设计的变体，允许在任务运行用户态代码或 idle 时
 *    安全地完成宽限期，无需等待所有任务发生调度切换。
 *    - trc_reader_nesting：当前任务在 TRACE_RCU 读临界区的嵌套深度
 *
 * fork 时必须重置这些字段，因为新进程是全新的，不继承父进程的 RCU 状态——
 * 父进程可能正处于某个 RCU 临界区，但子进程从 ret_from_fork 起点开始执行，
 * 不在任何 RCU 临界区中。
 */
static inline void rcu_copy_process(struct task_struct *p)
{
#ifdef CONFIG_PREEMPT_RCU
	p->rcu_read_lock_nesting = 0;        /* 新进程不在任何 RCU 读临界区，嵌套深度为 0 */
	p->rcu_read_unlock_special.s = 0;    /* 清除所有特殊解锁标志（无需延迟处理） */
	p->rcu_blocked_node = NULL;          /* 新进程未阻塞在任何 RCU 宽限期树节点上 */
	INIT_LIST_HEAD(&p->rcu_node_entry);  /* 初始化链表节点，防止野指针 */
#endif /* #ifdef CONFIG_PREEMPT_RCU */
#ifdef CONFIG_TASKS_RCU
	p->rcu_tasks_holdout = false;              /* 新进程不是宽限期的"拖后腿"任务 */
	INIT_LIST_HEAD(&p->rcu_tasks_holdout_list); /* 初始化拖后腿链表节点 */
	p->rcu_tasks_idle_cpu = -1;                /* -1 表示不在 idle 状态 */
	INIT_LIST_HEAD(&p->rcu_tasks_exit_list);   /* 初始化退出清理链表节点 */
#endif /* #ifdef CONFIG_TASKS_RCU */
#ifdef CONFIG_TASKS_TRACE_RCU
	p->trc_reader_nesting = 0; /* 新进程未进入任何 TRACE_RCU 读临界区 */
#endif /* #ifdef CONFIG_TASKS_TRACE_RCU */
}

/**
 * pidfd_prepare - allocate a new pidfd_file and reserve a pidfd
 * @pid:   the struct pid for which to create a pidfd
 * @flags: flags of the new @pidfd
 * @ret_file: return the new pidfs file
 *
 * Allocate a new file that stashes @pid and reserve a new pidfd number in the
 * caller's file descriptor table. The pidfd is reserved but not installed yet.
 *
 * The helper verifies that @pid is still in use, without PIDFD_THREAD the
 * task identified by @pid must be a thread-group leader.
 *
 * If this function returns successfully the caller is responsible to either
 * call fd_install() passing the returned pidfd and pidfd file as arguments in
 * order to install the pidfd into its file descriptor table or they must use
 * put_unused_fd() and fput() on the returned pidfd and pidfd file
 * respectively.
 *
 * This function is useful when a pidfd must already be reserved but there
 * might still be points of failure afterwards and the caller wants to ensure
 * that no pidfd is leaked into its file descriptor table.
 *
 * Return: On success, a reserved pidfd is returned from the function and a new
 *         pidfd file is returned in the last argument to the function. On
 *         error, a negative error code is returned from the function and the
 *         last argument remains unchanged.
 */
/*
 * pidfd_prepare 的完整文档见上方的 kernel-doc 注释块。此处补充实现层面的解读：
 *
 * 为何采用"预留但不安装"（reserve-but-not-install）的两步操作：
 *
 *   pidfd 的创建（如 clone3(CLONE_PIDFD)）需要在进程创建尚未完成时就分配好 fd，
 *   但如果 copy_process() 后续步骤失败（如内存不足、权限检查失败），
 *   则必须能够安全地回滚，不能将一个指向未完全初始化进程的 pidfd 泄露给用户态。
 *
 *   两步分离的做法：
 *     1. pidfd_prepare()：分配 fd number（占位，尚未对用户可见）+ 分配 pidfs file 对象
 *     2. 成功后调用 fd_install()：将 fd 写入文件描述符表，用户态才能看到
 *        或失败时调用 put_unused_fd() + fput()：回收 fd number 和 file 对象
 *
 *   这样保证了原子性语义：用户态要么看到一个完全就绪的 pidfd，要么什么都看不到。
 *
 * pidfs 是内核内部的伪文件系统（类似 anon_inode），专门存放 pidfd 文件对象，
 * 每个 pidfd 对应一个 struct pid 的引用，即使进程退出后 pidfd 仍可保留退出信息。
 */
int pidfd_prepare(struct pid *pid, unsigned int flags, struct file **ret_file)
{
	struct file *pidfs_file;

	/*
	 * PIDFD_STALE is only allowed to be passed if the caller knows
	 * that @pid is already registered in pidfs and thus
	 * PIDFD_INFO_EXIT information is guaranteed to be available.
	 */
	/*
	 * PIDFD_STALE 标志：允许为已退出（但尚未被完全回收）的进程创建 pidfd，
	 * 此时 pid 已在 pidfs 注册，可获取退出信息（EXIT code 等）。
	 * 非 STALE 情况下，需要验证进程仍然存活。
	 */
	if (!(flags & PIDFD_STALE)) {
		/*
		 * While holding the pidfd waitqueue lock removing the
		 * task linkage for the thread-group leader pid
		 * (PIDTYPE_TGID) isn't possible. Thus, if there's still
		 * task linkage for PIDTYPE_PID not having thread-group
		 * leader linkage for the pid means it wasn't a
		 * thread-group leader in the first place.
		 */
		/*
		 * 持有 wait_pidfd.lock（pidfd 等待队列锁）期间，
		 * 线程组领导者 pid 的 PIDTYPE_TGID 链接无法被移除（do_exit 路径
		 * 也需要这把锁），从而保证检查结果的一致性。
		 */
		guard(spinlock_irq)(&pid->wait_pidfd.lock); /* 持锁保护 pid->tasks[] 链表的一致性检查 */

		/* Task has already been reaped. */
		if (!pid_has_task(pid, PIDTYPE_PID))   /* pid 的 PIDTYPE_PID 链为空：进程已被回收 */
			return -ESRCH;
		/*
		 * If this struct pid isn't used as a thread-group
		 * leader but the caller requested to create a
		 * thread-group leader pidfd then report ENOENT.
		 */
		/*
		 * 非 PIDFD_THREAD 模式要求目标必须是线程组领导者（即用户态意义上的进程）。
		 * 若 pid 没有 PIDTYPE_TGID 链接，说明它只是一个普通线程，不符合要求。
		 */
		if (!(flags & PIDFD_THREAD) && !pid_has_task(pid, PIDTYPE_TGID))
			return -ENOENT; /* 目标不是线程组领导者，且调用者要求线程组级 pidfd */
	}

	CLASS(get_unused_fd, pidfd)(O_CLOEXEC); /* 在调用者的 fd 表中预留一个 fd number，设置 O_CLOEXEC */
	if (pidfd < 0)
		return pidfd; /* 预留失败（如 fd 表已满），直接返回错误 */

	pidfs_file = pidfs_alloc_file(pid, flags | O_RDWR); /* 在 pidfs 伪文件系统中分配 file 对象，持有 pid 引用 */
	if (IS_ERR(pidfs_file))
		return PTR_ERR(pidfs_file); /* CLASS 析构器会自动调用 put_unused_fd 归还 fd number */

	*ret_file = pidfs_file; /* 将 file 对象返回给调用者，由其决定何时 fd_install 或 fput */
	return take_fd(pidfd);  /* 将预留的 fd number 所有权转移给调用者（CLASS 不再析构） */
}

/*
 * 【__delayed_free_task】RCU 回调函数：在宽限期结束后真正释放 task_struct
 *
 * 当 delayed_free_task() 通过 call_rcu() 注册本函数后，RCU 子系统会在所有
 * CPU 都经历一次"宽限期"（grace period，即所有 CPU 都离开了 RCU 读临界区）
 * 之后，才调用此回调，此时可以安全地释放内存。
 */
static void __delayed_free_task(struct rcu_head *rhp)
{
	/* container_of 从 rcu_head 指针反推出 task_struct 指针 */
	struct task_struct *tsk = container_of(rhp, struct task_struct, rcu);

	free_task(tsk); /* 释放 task_struct 及其内核栈等附属资源 */
}

/*
 * 【delayed_free_task】有条件地延迟释放 task_struct
 *
 * 为何需要延迟释放（仅在 CONFIG_MEMCG 下）：
 *   Memory Control Group（memcg）允许对进程的内存使用量记账。task_struct 本身
 *   通过 kmem_cache_alloc 分配，其 slab 对象可能被 memcg 跟踪。
 *   当 task_struct 被释放时（free_task -> kmem_cache_free），memcg 的记账逻辑
 *   可能需要访问该 task 所属的 css（cgroup subsystem state）。
 *
 *   问题在于：task 退出后，其 css 引用可能已经被释放（通过 RCU 宽限期），
 *   如果立即调用 free_task()，可能在 RCU 读临界区结束之前就释放了 task，
 *   导致其他 CPU 上的 RCU 读者访问到已释放的内存（use-after-free）。
 *
 *   解决方案：通过 call_rcu() 将释放操作延迟到 RCU 宽限期结束后，
 *   保证所有可能持有该 task 指针的 RCU 读者都已退出临界区。
 *
 *   非 MEMCG 配置下无此顾虑，可以立即释放，避免 RCU 延迟开销。
 */
static __always_inline void delayed_free_task(struct task_struct *tsk)
{
	if (IS_ENABLED(CONFIG_MEMCG))
		call_rcu(&tsk->rcu, __delayed_free_task); /* 注册 RCU 回调，宽限期后才真正释放 */
	else
		free_task(tsk); /* 无 memcg 时直接释放，无需等待宽限期 */
}

/*
 * 【copy_oom_score_adj】为新进程同步 OOM（Out-Of-Memory）评分调整值
 *
 * OOM killer 是内存耗尽时内核杀死进程以释放内存的机制。每个进程有一个
 * oom_score_adj（范围 -1000~1000），用户可通过 /proc/pid/oom_score_adj 调整，
 * 值越大越容易被 OOM killer 选中杀死（-1000 = 永不被杀，1000 = 最优先被杀）。
 *
 * 为何需要此函数（而不直接依赖 copy_signal 的复制）：
 *   copy_signal() 在进程创建早期复制了 oom_score_adj，但此后直到本函数被调用
 *   之间存在一个窗口期，父进程可能通过 /proc/self/oom_score_adj 修改了该值。
 *   为避免父子进程的 oom_score_adj 出现不一致，本函数在持 oom_adj_mutex 的
 *   情况下重新同步该值，确保读取到最新状态。
 *
 * MMF_MULTIPROCESS 标志的作用：
 *   当同一个 mm_struct（地址空间）被多个进程共享（通过 CLONE_VM 创建的新进程），
 *   设置 MMF_MULTIPROCESS 标志，OOM killer 在打印调试信息和统计时可以感知
 *   该地址空间被多个进程使用，避免重复计算内存占用。
 *
 * 触发条件分析：
 *   - 必须有用户地址空间（!tsk->mm 表示内核线程，跳过）
 *   - 必须是 CLONE_VM（共享地址空间）但非 CLONE_THREAD（线程）、非 CLONE_VFORK：
 *     即用 clone(CLONE_VM) 创建的共享内存的新"进程"（非线程）。
 *     线程跳过是因为线程组共享 signal_struct，oom_score_adj 天然一致；
 *     vfork 跳过是因为 vfork 子进程立即 exec，不需要建立独立的 OOM 记账。
 */
static void copy_oom_score_adj(u64 clone_flags, struct task_struct *tsk)
{
	/* Skip if kernel thread */
	if (!tsk->mm) /* 内核线程没有用户地址空间，OOM killer 不杀内核线程 */
		return;

	/* Skip if spawning a thread or using vfork */
	/*
	 * 仅处理 CLONE_VM 且不带 CLONE_THREAD 和 CLONE_VFORK 的情况：
	 * 这类进程共享父进程 mm 但有独立的 signal_struct，需要独立同步 oom_score_adj。
	 */
	if ((clone_flags & (CLONE_VM | CLONE_THREAD | CLONE_VFORK)) != CLONE_VM)
		return;

	/* We need to synchronize with __set_oom_adj */
	/*
	 * 持 oom_adj_mutex 锁，与 /proc/.../oom_score_adj 写路径（__set_oom_adj）同步，
	 * 防止并发修改导致父子进程 oom_score_adj 不一致的竞态。
	 */
	mutex_lock(&oom_adj_mutex);
	mm_flags_set(MMF_MULTIPROCESS, tsk->mm); /* 标记该 mm 被多个进程共享，OOM killer 需要感知 */
	/* Update the values in case they were changed after copy_signal */
	tsk->signal->oom_score_adj = current->signal->oom_score_adj;         /* 同步最新的 OOM 调整值 */
	tsk->signal->oom_score_adj_min = current->signal->oom_score_adj_min; /* 同步 OOM 调整值的下限（防止非特权进程降低值） */
	mutex_unlock(&oom_adj_mutex);
}

/*
 * 【rv_task_fork】为新进程初始化运行时验证（Runtime Verification）状态
 *
 * Runtime Verification（RV）是 Linux 内核的一种轻量级形式化验证框架，
 * 通过在内核关键路径上插入"监视器"（monitor），在运行时检查内核行为是否
 * 符合预期的状态机规约（如锁的获取/释放顺序、调度状态转换等）。
 *
 * RV 框架（CONFIG_RV）在 kernel/trace/rv/ 目录实现，每个监视器对应一个
 * 有限状态机（DFA），task_struct 中的 rv 字段保存该任务在各监视器中的
 * 当前状态。
 *
 * fork 时需要清零：新进程是一个全新的执行实体，其 RV 监视器状态应从初始
 * 状态开始，不继承父进程可能已处于中间状态的监视器状态，否则会触发误报。
 */
#ifdef CONFIG_RV
static void rv_task_fork(struct task_struct *p)
{
	memset(&p->rv, 0, sizeof(p->rv)); /* 将所有 RV 监视器状态清零，新进程从初始状态开始 */
}
#else
#define rv_task_fork(p) do {} while (0) /* 未启用 RV 时，编译为空操作 */
#endif

/*
 * 【need_futex_hash_allocate_default】判断是否需要为新进程预分配私有 futex 哈希表
 *
 * futex（Fast Userspace Mutex）是 Linux 用户态锁的底层机制。内核为每个进程维护
 * 一个 futex 哈希表，用于加速 futex_wait/futex_wake 操作中的等待队列查找。
 *
 * 哈希表分配策略：
 *   - 每个独立的地址空间（独立 mm）的进程，在第一次调用 futex 时按需分配哈希表。
 *   - 但对于通过 CLONE_VM 共享父进程 mm 的子进程（"共享内存进程"，非线程），
 *     如果不预先分配独立的哈希表，子进程会与父进程竞争同一个哈希表，
 *     可能导致性能问题（高并发下锁争用）。
 *   - 因此，对于 CLONE_VM 且非 CLONE_VFORK 的子进程，在 fork 时预先分配
 *     一个"默认大小"的私有哈希表。
 *
 * 条件解析：(clone_flags & (CLONE_VM | CLONE_VFORK)) == CLONE_VM
 *   - CLONE_VM 置位：子进程与父进程共享地址空间（非 fork/exec 的独立进程）
 *   - CLONE_VFORK 未置位：不是 vfork（vfork 子进程会立即 exec，马上替换地址空间，
 *     无需为短暂的共享窗口分配哈希表）
 *   - 注意：CLONE_THREAD（线程）会同时设置 CLONE_VM，但线程共享整个进程的
 *     futex 基础设施，不需要独立哈希表，因此线程情况由 copy_process 上层逻辑处理。
 *
 * 典型场景：clone(CLONE_VM | CLONE_FILES) 创建的"共享内存进程"（类似 vfork 但不立即 exec），
 * 常见于某些高性能进程池实现中。
 */
static bool need_futex_hash_allocate_default(u64 clone_flags)
{
	/*
	 * Allocate a default futex hash for any sibling that will
	 * share the parent's mm, except vfork.
	 */
	/*
	 * 返回 true 的条件：设置了 CLONE_VM（共享 mm）但未设置 CLONE_VFORK。
	 * 位运算技巧：同时检查两个标志——将二者一起 AND，再与仅含 CLONE_VM 的期望值比较。
	 * 若 CLONE_VFORK 已置位，则 AND 结果会含 CLONE_VFORK 位，不等于 CLONE_VM，返回 false。
	 */
	return (clone_flags & (CLONE_VM | CLONE_VFORK)) == CLONE_VM;
}

/*
 * This creates a new process as a copy of the old one,
 * but does not actually start it yet.
 *
 * It copies the registers, and all the appropriate
 * parts of the process environment (as per the clone
 * flags). The actual kick-off is left to the caller.
 */

/*
 * 【copy_process】进程创建的核心函数 —— 所有创建工作都在这里完成
 *
 * 无论是 fork()、vfork()、clone() 还是 clone3()，最终都通过 kernel_clone()
 * 调用本函数。函数接收 clone_flags 标志位，决定哪些资源被复制、哪些被共享。
 *
 * 整体流程分为以下几个阶段：
 *
 * 【阶段一：标志位合法性检查】
 *   检查 clone_flags 的标志组合是否合法，例如：
 *   - CLONE_NEWNS 与 CLONE_FS 不能同时设置（新挂载命名空间时不能共享根目录）
 *   - CLONE_THREAD 必须同时设置 CLONE_SIGHAND（线程必须共享信号处理器）
 *
 * 【阶段二：分配进程描述符】
 *   dup_task_struct() 分配 task_struct 和内核栈，这是进程存在的物理基础。
 *
 * 【阶段三：凭证与安全检查】
 *   copy_creds() 复制父进程凭证，security_task_alloc() 分配 LSM 安全标签。
 *
 * 【阶段四：资源复制】
 *   依次复制文件描述符表、文件系统信息、信号处理器、内存空间、命名空间等。
 *   根据 clone_flags，各资源可能是独立复制或与父进程共享。
 *
 * 【阶段五：进程标识与调度】
 *   copy_thread() 复制 CPU 寄存器状态，alloc_pid() 分配新 PID，
 *   sched_fork() 设置调度参数。
 *
 * 【阶段六：加入进程树】
 *   将新进程挂入父进程的子进程链表，加入全局进程哈希表，完成创建。
 *
 * 安全关注点（Android 场景）：
 *   - Zygote fork App 进程时，clone_flags 不含 CLONE_VM（地址空间独立），
 *     不含 CLONE_NEWPID（共享 PID 命名空间），也不含 CLONE_NEWNET（共享网络）。
 *   - App 进程的隔离主要依赖 SELinux domain 转换（execve 后由 init 触发），
 *     而非 fork 时的命名空间隔离。
 */
__latent_entropy struct task_struct *copy_process(
					struct pid *pid,
					int trace,
					int node,
					struct kernel_clone_args *args)
{
	/*
	 * 【变量声明】
	 * pidfd    : 若调用者请求 CLONE_PIDFD，这里记录分配的文件描述符号，
	 *            初始为 -1 表示"尚未分配"，错误路径据此决定是否需要释放。
	 * retval   : 统一的错误码暂存，大量 goto 标签通过它向调用者返回错误原因。
	 * p        : 指向新进程的 task_struct；copy_process 的核心产出。
	 * delayed  : multiprocess_signals 结构，用于"延迟信号"机制：
	 *            在 fork 进行时，其他线程可能向父进程组发送信号（如 SIGTERM），
	 *            这些信号需要在 fork 完成后才能被分发，否则父子进程都会收到，
	 *            导致行为混乱。delayed.node 挂入父进程的 multiprocess 链表。
	 * pidfile  : CLONE_PIDFD 时，pidfd 对应的 struct file 指针。
	 * clone_flags : 从 args->flags 提取，const 修饰确保本函数内不会意外修改。
	 * nsp      : 当前进程的命名空间代理，用于后续 PID 命名空间检查。
	 */
	int pidfd = -1, retval;
	struct task_struct *p;
	struct multiprocess_signals delayed;
	struct file *pidfile = NULL;
	const u64 clone_flags = args->flags;
	struct nsproxy *nsp = current->nsproxy;

	/*
	 * 【阶段一：clone_flags 合法性检查】
	 * 以下一系列检查确保标志位的组合在语义上自洽，
	 * 每个 check 都对应一条"不变量"：某两个标志在语义上相互矛盾，
	 * 同时设置会导致内核数据结构状态不一致或安全漏洞。
	 * 这些检查在资源分配之前做，成本极低，出错直接返回 ERR_PTR(-EINVAL)。
	 */

	/*
	 * Don't allow sharing the root directory with processes in a different
	 * namespace
	 */
	/*
	 * CLONE_NEWNS 创建新的挂载命名空间（进程有独立的挂载树），
	 * CLONE_FS 共享父进程的文件系统信息（root、cwd）。
	 * 两者互斥：新命名空间需要独立的根目录视图，
	 * 若同时共享 fs_struct，则根目录信息无法独立，语义矛盾。
	 */
	if ((clone_flags & (CLONE_NEWNS|CLONE_FS)) == (CLONE_NEWNS|CLONE_FS))
		return ERR_PTR(-EINVAL);

	/*
	 * CLONE_NEWUSER 创建新的用户命名空间，uid/gid 映射独立。
	 * CLONE_FS 共享文件系统上下文（含根目录）。
	 * 两者互斥：新用户命名空间中的进程不能与父命名空间共享根目录，
	 * 否则权限边界会被绕过（新 user ns 中的"root"可访问父 ns 的文件）。
	 */
	if ((clone_flags & (CLONE_NEWUSER|CLONE_FS)) == (CLONE_NEWUSER|CLONE_FS))
		return ERR_PTR(-EINVAL);

	/*
	 * Thread groups must share signals as well, and detached threads
	 * can only be started up within the thread group.
	 */
	/*
	 * CLONE_THREAD 要求新进程成为同一线程组的成员（与父进程共享 TGID），
	 * 线程组的语义强制要求所有成员共享信号处理器（CLONE_SIGHAND）。
	 * 原因：线程组中的任何线程都可以处理发送给整个进程的信号，
	 * 若信号处理器不共享，不同线程看到的 sa_handler 会不一致，
	 * 造成信号处理行为混乱。
	 */
	if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
		return ERR_PTR(-EINVAL);

	/*
	 * Shared signal handlers imply shared VM. By way of the above,
	 * thread groups also imply shared VM. Blocking this case allows
	 * for various simplifications in other code.
	 */
	/*
	 * CLONE_SIGHAND 共享信号处理器，CLONE_VM 共享虚拟地址空间。
	 * 共享信号处理器隐含共享 VM 的原因：
	 * sigaction 结构中的 sa_handler 是函数指针，指向进程地址空间中的代码。
	 * 若两个进程地址空间不同，同一个指针值指向的代码也不同，
	 * 共享信号处理器就没有意义。此约束简化了内核其他路径的逻辑。
	 */
	if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
		return ERR_PTR(-EINVAL);

	/*
	 * Siblings of global init remain as zombies on exit since they are
	 * not reaped by their parent (swapper). To solve this and to avoid
	 * multi-rooted process trees, prevent global and container-inits
	 * from creating siblings.
	 */
	/*
	 * SIGNAL_UNKILLABLE 标志由 PID 命名空间的 init 进程（PID=1）持有，
	 * 该进程不能被杀死（不响应 SIGKILL，除非来自父命名空间）。
	 * CLONE_PARENT 让新进程成为调用者的兄弟（共享同一父进程）。
	 * 若 init 进程创建兄弟，其父进程是 swapper（PID=0，内核线程），
	 * swapper 不执行 wait()，兄弟进程退出后会永久僵死（zombie），
	 * 还可能破坏进程树的唯一根结构，故禁止。
	 */
	if ((clone_flags & CLONE_PARENT) &&
				current->signal->flags & SIGNAL_UNKILLABLE)
		return ERR_PTR(-EINVAL);

	/*
	 * If the new process will be in a different pid or user namespace
	 * do not allow it to share a thread group with the forking task.
	 */
	/*
	 * PID 命名空间或 user 命名空间不同时，不能共享线程组（CLONE_THREAD）。
	 * 原因：线程组内所有成员必须在同一 PID 命名空间中拥有相同的 TGID，
	 * 跨 PID 命名空间时 TGID 的语义无法保持一致。
	 * 同理，user 命名空间不同时，uid/gid 映射不同，线程共享的凭证会矛盾。
	 * task_active_pid_ns 检查当前 PID 命名空间是否与 children 的目标命名空间一致。
	 */
	if (clone_flags & CLONE_THREAD) {
		if ((clone_flags & (CLONE_NEWUSER | CLONE_NEWPID)) ||
		    (task_active_pid_ns(current) != nsp->pid_ns_for_children))
			return ERR_PTR(-EINVAL);
	}

	if (clone_flags & CLONE_PIDFD) {
		/*
		 * - CLONE_DETACHED is blocked so that we can potentially
		 *   reuse it later for CLONE_PIDFD.
		 */
		/*
		 * CLONE_DETACHED 是历史遗留标志（已废弃），其数值位被预留给
		 * 未来扩展（目前正被 CLONE_PIDFD 机制复用），
		 * 为避免冲突，禁止与 CLONE_PIDFD 同时使用。
		 */
		if (clone_flags & CLONE_DETACHED)
			return ERR_PTR(-EINVAL);
	}

	/*
	 * CLONE_AUTOREAP：子进程退出时自动被内核回收，不产生 zombie，
	 * 也不向父进程发送 SIGCHLD。
	 * 与 CLONE_THREAD 互斥：线程退出由线程组统一管理，不走独立回收路径。
	 * 与 CLONE_PARENT 互斥：自动回收意味着无需父进程 wait()，
	 *   但 CLONE_PARENT 将新进程的父进程改为调用者的父进程，
	 *   两者在"谁负责回收"这一问题上语义冲突。
	 * 若 exit_signal != 0，表示退出时要通知父进程，与自动回收矛盾。
	 */
	if (clone_flags & CLONE_AUTOREAP) {
		if (clone_flags & CLONE_THREAD)
			return ERR_PTR(-EINVAL);
		if (clone_flags & CLONE_PARENT)
			return ERR_PTR(-EINVAL);
		if (args->exit_signal)
			return ERR_PTR(-EINVAL);
	}

	/*
	 * 若父进程自身也设置了 autoreap，则其子进程不能使用 CLONE_PARENT
	 * 将新孙进程"托付"给同一父进程（父进程自动回收子进程后，
	 * 新的 sibling 将没有合适的 reaper）。
	 */
	if ((clone_flags & CLONE_PARENT) && current->signal->autoreap)
		return ERR_PTR(-EINVAL);

	/*
	 * CLONE_NNP（no_new_privs）：子进程不能获得比父进程更高的权限，
	 * 即使执行 setuid 程序也不能提权（安全沙箱关键标志）。
	 * 与 CLONE_THREAD 互斥：线程共享 task_struct 中的 no_new_privs 位，
	 * 不能在创建线程时单独为某个线程设置该标志（需要对整个线程组生效）。
	 */
	if (clone_flags & CLONE_NNP) {
		if (clone_flags & CLONE_THREAD)
			return ERR_PTR(-EINVAL);
	}

	/*
	 * CLONE_PIDFD_AUTOKILL：当持有 pidfd 的进程（创建者）退出时，
	 * 内核自动向子进程发送 SIGKILL，实现"生命周期绑定"语义。
	 * 前置条件：必须同时设置 CLONE_PIDFD（需要 pidfd 句柄）
	 *           和 CLONE_AUTOREAP（子进程退出后自动回收，无 zombie）。
	 * 与 CLONE_THREAD 互斥：线程没有独立的 pidfd。
	 * 权限要求：若未设置 CLONE_NNP（子进程可提权），则要求 CAP_SYS_ADMIN，
	 *           防止无特权进程将高权限子进程的生命周期绑定到自身。
	 */
	if (clone_flags & CLONE_PIDFD_AUTOKILL) {
		if (!(clone_flags & CLONE_PIDFD))
			return ERR_PTR(-EINVAL);
		if (!(clone_flags & CLONE_AUTOREAP))
			return ERR_PTR(-EINVAL);
		if (clone_flags & CLONE_THREAD)
			return ERR_PTR(-EINVAL);
		/*
		 * Without CLONE_NNP the child could escalate privileges
		 * after being spawned, so require CAP_SYS_ADMIN.
		 * With CLONE_NNP the child can't gain new privileges,
		 * so allow unprivileged usage.
		 */
		if (!(clone_flags & CLONE_NNP) &&
		    !ns_capable(current_user_ns(), CAP_SYS_ADMIN))
			return ERR_PTR(-EPERM);
	}

	/*
	 * 【阶段二：延迟信号（multiprocess signal）处理】
	 *
	 * Force any signals received before this point to be delivered
	 * before the fork happens.  Collect up signals sent to multiple
	 * processes that happen during the fork and delay them so that
	 * they appear to happen after the fork.
	 *
	 * 背景：当一个多线程进程执行 fork 时，其他进程可能同时向该进程组
	 * 发送信号（如 kill(-pgid, SIGTERM)）。若信号在 fork 过程中到达，
	 * 它可能既被父进程处理，又传播到新子进程，造成"一个信号，两次响应"。
	 * 解决方案：在 fork 期间将进程组信号"拦截"到 delayed.signal，
	 * fork 完成后再将其分配给父进程（或转给子进程的 shared_pending）。
	 */
	sigemptyset(&delayed.signal); /* 清空延迟信号集合，等待后续收集 */
	INIT_HLIST_NODE(&delayed.node); /* 初始化哈希链表节点，准备挂入 multiprocess 链表 */

	spin_lock_irq(&current->sighand->siglock); /* 持有 siglock，保护信号状态的原子修改 */
	/*
	 * 仅对非线程创建（fork/clone 创建新进程）挂入 multiprocess 链表；
	 * 线程共享 signal_struct，不存在"向进程组发送信号后 fork" 的歧义问题。
	 */
	if (!(clone_flags & CLONE_THREAD))
		hlist_add_head(&delayed.node, &current->signal->multiprocess);
	recalc_sigpending(); /* 重新计算 TIF_SIGPENDING，使之前到达的信号在下面的检查中可见 */
	spin_unlock_irq(&current->sighand->siglock);
	retval = -ERESTARTNOINTR;
	/*
	 * 若当前进程在 fork 之前已有待处理信号，则中止本次 fork，
	 * 让调用者（系统调用返回路径）先处理信号，再重试 fork。
	 * ERESTARTNOINTR 告诉内核：系统调用被信号中断，但不应自动重启
	 * （由 glibc 或用户代码判断是否重试）。
	 */
	if (task_sigpending(current))
		goto fork_out;

	/*
	 * 【阶段三：分配进程描述符（dup_task_struct）】
	 * dup_task_struct() 做以下工作：
	 *   1. kmem_cache_alloc 分配新 task_struct（从 task_struct slab 缓存）
	 *   2. alloc_thread_stack_node() 分配内核栈（通常 16KB，CONFIG_THREAD_INFO_IN_TASK 时
	 *      thread_info 嵌入 task_struct 首部，无需单独分配）
	 *   3. arch_dup_task_struct() 逐字节复制父进程的 task_struct（memcpy 语义）
	 *   4. 初始化新进程的内核栈指针和 stack_canary
	 * 返回新 task_struct 指针，失败返回 NULL（内存不足）。
	 * node 参数指定 NUMA 节点，确保子进程的内存在合适的节点上分配（NUMA-aware fork）。
	 */
	retval = -ENOMEM;
	p = dup_task_struct(current, node);
	if (!p)
		goto fork_out;
	/*
	 * copy_exec_state() 复制可执行文件相关的状态标志（exec_id 等），
	 * 使子进程有独立的 exec 身份标识，用于 SIGCHLD 的 parent_exec_id 判断。
	 */
	retval = copy_exec_state(clone_flags, p);
	if (retval)
		goto bad_fork_free;
	p->flags &= ~PF_KTHREAD; /* 默认清除内核线程标志（继承自父进程的副本） */
	if (args->kthread)
		p->flags |= PF_KTHREAD; /* 内核线程：没有用户地址空间，直接执行 fn() */
	if (args->user_worker) {
		/*
		 * Mark us a user worker, and block any signal that isn't
		 * fatal or STOP
		 */
		/*
		 * 用户工作者线程（如 io_uring 工作线程）：
		 * 设置 PF_USER_WORKER 标志，屏蔽除 SIGKILL/SIGSTOP 之外的所有信号。
		 * siginitsetinv() 对信号掩码取反：blocked = ~(SIGKILL|SIGSTOP)，
		 * 即屏蔽绝大多数信号，只保留不可屏蔽的致命信号。
		 */
		p->flags |= PF_USER_WORKER;
		siginitsetinv(&p->blocked, sigmask(SIGKILL)|sigmask(SIGSTOP));
	}
	if (args->io_thread)
		p->flags |= PF_IO_WORKER; /* IO 工作线程（内核 IO 路径专用线程）标志 */

	if (args->name)
		strscpy_pad(p->comm, args->name, sizeof(p->comm)); /* 设置进程名（内核线程时由调用者指定） */

	/*
	 * CLONE_CHILD_SETTID：fork 成功后，内核将子进程 TID 写入用户空间的 child_tid 地址。
	 * 这是 POSIX 线程库（glibc pthread_create）用来通知库"线程已创建"的机制。
	 * 实际写入发生在子进程首次被调度执行时（see copy_thread / ret_from_fork）。
	 */
	p->set_child_tid = (clone_flags & CLONE_CHILD_SETTID) ? args->child_tid : NULL;
	/*
	 * TID is cleared in mm_release() when the task exits
	 */
	/*
	 * CLONE_CHILD_CLEARTID：子进程退出时，内核将 child_tid 处的值清零，
	 * 并执行 futex_wake()，唤醒在该地址上等待的线程（如 pthread_join 的等待者）。
	 * 清零发生在 mm_release()，即地址空间释放时。
	 */
	p->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? args->child_tid : NULL;

	ftrace_graph_init_task(p); /* 初始化函数调用图追踪（CONFIG_FUNCTION_GRAPH_TRACER） */

	rt_mutex_init_task(p); /* 初始化 RT mutex 相关字段（优先级继承链等） */
	raw_spin_lock_init(&p->blocked_lock); /* 初始化信号阻塞集合的保护锁 */

	lockdep_assert_irqs_enabled(); /* 断言：此处中断必须开启，否则锁依赖分析会出错 */
#ifdef CONFIG_PROVE_LOCKING
	DEBUG_LOCKS_WARN_ON(!p->softirqs_enabled); /* 验证软中断也处于使能状态 */
#endif
	/*
	 * 【安全边界：复制进程凭证】
	 * copy_creds() 复制父进程的 struct cred（包含 uid/gid/euid/egid/
	 * fsuid/fsgid 以及 capabilities 能力集）到子进程。
	 *
	 * fork 语义：子进程完全继承父进程的凭证，不做任何降权处理。
	 * 这意味着 Zygote（以 root uid=0 或特殊 uid 运行）fork 出的子进程
	 * 初始时也拥有同等权限，App 进程的降权（setuid 到 AID_APP_xxx）
	 * 发生在 fork 之后、execve 之前（由 zygote 的 Java 层调用
	 * Os.setuid/setgid 完成）。
	 *
	 * 安全隐患：如果 fork 后降权步骤被绕过，子进程将以高权限运行。
	 * CLONE_NEWUSER 标志会触发新用户命名空间的创建，此时 uid 映射
	 * 需要显式配置（/proc/<pid>/uid_map），是容器逃逸研究的重点。
	 */
	retval = copy_creds(p, clone_flags);
	if (retval < 0)
		goto bad_fork_free;

	/*
	 * 【资源限制检查：RLIMIT_NPROC】
	 * 检查用户的进程数是否超过 RLIMIT_NPROC 软限制。
	 * is_rlimit_overlimit() 对用户计数器（task_ucounts 基于 uid 聚合）进行比较。
	 * 豁免条件：
	 *   - INIT_USER（uid=0）不受此限制（允许 root 无限制创建进程）
	 *   - 持有 CAP_SYS_RESOURCE 或 CAP_SYS_ADMIN 能力的进程可豁免
	 * 若超限且不满足豁免，返回 -EAGAIN（调用方可重试，但通常意味着失败）。
	 */
	retval = -EAGAIN;
	if (is_rlimit_overlimit(task_ucounts(p), UCOUNT_RLIMIT_NPROC, rlimit(RLIMIT_NPROC))) {
		if (p->real_cred->user != INIT_USER &&
		    !capable(CAP_SYS_RESOURCE) && !capable(CAP_SYS_ADMIN))
			goto bad_fork_cleanup_count;
	}
	current->flags &= ~PF_NPROC_EXCEEDED; /* 清除"超限"标志：本次 fork 成功通过限制检查 */

	/*
	 * If multiple threads are within copy_process(), then this check
	 * triggers too late. This doesn't hurt, the check is only there
	 * to stop root fork bombs.
	 */
	/*
	 * 全局线程数上限检查（max_threads 由 /proc/sys/kernel/threads-max 控制，
	 * 默认根据系统内存计算）。
	 * data_race() 包装：允许在没有锁的情况下读 nr_threads，接受数据竞争，
	 * 因为这里只是一个"尽力而为"的软性检查，不是精确的原子操作。
	 * 注释说明：多个线程同时 fork 时此检查可能失效（检查后值已变），
	 * 但这只是防止 root fork bomb 的粗粒度保护，不影响正确性。
	 */
	retval = -EAGAIN;
	if (data_race(nr_threads >= max_threads))
		goto bad_fork_cleanup_count;

	delayacct_tsk_init(p);	/* Must remain after dup_task_struct() */
	/* 延迟记账初始化：统计进程在 block I/O、内存回收、swap 等操作中的等待时间，
	 * 数据通过 /proc/<pid>/sched 或 taskstats netlink 导出，用于性能分析。
	 * 必须在 dup_task_struct() 之后调用，确保 task_struct 已完全初始化。 */

	/*
	 * 清除从父进程继承的敏感 flags，防止子进程意外获得特权或特殊状态：
	 * - PF_SUPERPRIV：父进程曾用过超级用户权限（子进程不继承此历史记录）
	 * - PF_WQ_WORKER ：父进程是工作队列 worker（子进程不是）
	 * - PF_IDLE      ：父进程是 idle 线程（子进程不是）
	 * - PF_NO_SETAFFINITY：父进程禁止设置 CPU 亲和性（子进程重新允许）
	 */
	p->flags &= ~(PF_SUPERPRIV | PF_WQ_WORKER | PF_IDLE | PF_NO_SETAFFINITY);
	p->flags |= PF_FORKNOEXEC; /* 标记"刚 fork 尚未 exec"，exec 后由 exec 路径清除 */
	INIT_LIST_HEAD(&p->children); /* 子进程链表头：子进程会挂入此链表 */
	INIT_LIST_HEAD(&p->sibling);  /* 兄弟链表节点：稍后挂入父进程的 children 链表 */
	rcu_copy_process(p); /* 复制 RCU 相关字段（rcu_head、tasks_rcu 等），用于 RCU 保护的遍历 */
	p->vfork_done = NULL; /* vfork 完成通知指针，vfork 时由内核分配，普通 fork 为 NULL */
	spin_lock_init(&p->alloc_lock); /* 通用分配锁，保护 p->thread_pid、p->files 等字段的修改 */

	init_sigpending(&p->pending); /* 初始化子进程的私有信号 pending 队列（空），
	                                * 区别于 signal->shared_pending（线程组共享） */

	/*
	 * CPU 时间计数器初始化：子进程从 0 开始计时，不继承父进程的 CPU 时间。
	 * utime：用户态 CPU 时间；stime：内核态 CPU 时间；gtime：guest 模式 CPU 时间。
	 */
	p->utime = p->stime = p->gtime = 0;
#ifdef CONFIG_ARCH_HAS_SCALED_CPUTIME
	p->utimescaled = p->stimescaled = 0; /* 体系结构缩放的 CPU 时间（如 ARM 的频率感知记账） */
#endif
	prev_cputime_init(&p->prev_cputime); /* 初始化"上次 cputime 快照"，用于增量计算 */

#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	/* 虚拟 CPU 时间记账（通用实现，比 tick-based 记账更精确）：
	 * seqcount 保护读写原子性；starttime 记录当前 vtime 阶段开始时间；
	 * VTIME_INACTIVE 表示进程当前未在 CPU 上运行。 */
	seqcount_init(&p->vtime.seqcount);
	p->vtime.starttime = 0;
	p->vtime.state = VTIME_INACTIVE;
#endif

#ifdef CONFIG_IO_URING
	p->io_uring = NULL;
	/*
	 * io_uring_fork()：继承父进程的 io_uring 上下文（如果有）。
	 * io_uring 允许进程注册固定的文件/缓冲区，fork 时需要复制引用计数。
	 */
	retval = io_uring_fork(p);
	if (unlikely(retval))
		goto bad_fork_cleanup_delayacct;
	retval = -EAGAIN;
#endif

	/* 继承父进程的定时器精度松弛值（timer slack）：
	 * timer_slack_ns 允许内核在 epoll_wait/poll/select 等操作中对超时
	 * 进行合并，减少 wakeup 次数，节省功耗。可通过 prctl(PR_SET_TIMERSLACK) 修改。 */
	p->default_timer_slack_ns = current->timer_slack_ns;

#ifdef CONFIG_PSI
	p->psi_flags = 0; /* 压力停滞信息（PSI）标志清零，子进程从无压力状态开始 */
#endif

	task_io_accounting_init(&p->ioac); /* 初始化 I/O 记账结构（read_bytes/write_bytes 等） */
	acct_clear_integrals(p); /* 清除 BSD 进程记账的累积量（cpu/mem integrals） */

	posix_cputimers_init(&p->posix_cputimers); /* 初始化 POSIX CPU 定时器链表（ITIMER_PROF 等） */
	tick_dep_init_task(p); /* 初始化 tick 依赖状态，用于 nohz 模式下的 tick 唤醒管理 */

	p->io_context = NULL; /* I/O 调度上下文（用于 CFQ/BFQ），fork 时不继承，需要时延迟分配 */
	audit_set_context(p, NULL); /* 审计上下文清空，子进程的审计从新系统调用开始记录 */
	cgroup_fork(p); /* cgroup fork 预初始化：将新进程暂时关联到父进程的 css_set */
	if (args->kthread) {
		/* 内核线程需要 kthread_struct 来支持 kthread_stop() 等接口 */
		if (!set_kthread_struct(p))
			goto bad_fork_cleanup_delayacct;
	}
#ifdef CONFIG_NUMA
	/* 复制父进程的 NUMA 内存策略（mempolicy）：
	 * mempolicy 控制进程的内存分配在哪些 NUMA 节点上进行
	 * （如 MPOL_BIND 绑定到指定节点，MPOL_INTERLEAVE 交叉分配）。
	 * mpol_dup() 增加引用计数或深拷贝，失败时置 NULL 并回退。 */
	p->mempolicy = mpol_dup(p->mempolicy);
	if (IS_ERR(p->mempolicy)) {
		retval = PTR_ERR(p->mempolicy);
		p->mempolicy = NULL;
		goto bad_fork_cleanup_delayacct;
	}
#endif
#ifdef CONFIG_CPUSETS
	/* cpuset 内存扩展：spread_rotor 控制内存页分散到多个 NUMA 节点的轮转起始位置 */
	p->cpuset_mem_spread_rotor = NUMA_NO_NODE;
	/* mems_allowed_seq：cpuset 允许的内存节点掩码的顺序锁，保护并发读取 */
	seqcount_spinlock_init(&p->mems_allowed_seq, &p->alloc_lock);
#endif
#ifdef CONFIG_TRACE_IRQFLAGS
	/* irqtrace：中断追踪记录，用于 lockdep 检测中断上下文中的锁使用问题。
	 * 清零后设置初始 IP（指令指针），表示"从这里开始追踪"。
	 * softirqs_enabled=1 表示新进程默认允许软中断；softirq_context=0 表示不在软中断上下文。 */
	memset(&p->irqtrace, 0, sizeof(p->irqtrace));
	p->irqtrace.hardirq_disable_ip	= _THIS_IP_;
	p->irqtrace.softirq_enable_ip	= _THIS_IP_;
	p->softirqs_enabled		= 1;
	p->softirq_context		= 0;
#endif

	p->pagefault_disabled = 0; /* 允许缺页异常：pagefault_disabled > 0 时禁止，
	                             * 原子上下文（spinlock 持有期间）会临时增加此计数 */

	lockdep_init_task(p); /* 初始化 lockdep 任务状态（持锁链表等），用于运行时死锁检测 */

	p->blocked_on = NULL; /* not blocked yet */
	/* blocked_on：RT mutex 死锁检测用，记录进程当前等待哪把锁 */
	p->blocked_donor = NULL; /* nobody is boosting p yet */
	/* blocked_donor：优先级继承（PI）链，记录谁在为 p 提升优先级 */

#ifdef CONFIG_BCACHE
	/* bcache（块设备缓存）的顺序 I/O 检测计数器，用于识别顺序访问模式并优化预读 */
	p->sequential_io	= 0;
	p->sequential_io_avg	= 0;
#endif

	unwind_task_init(p); /* 初始化内核栈展开（unwinding）相关数据，用于 oops/panic 时的栈回溯 */

	/*
	 * 【阶段四：调度器初始化（sched_fork）】
	 * Perform scheduler related setup. Assign this task to a CPU.
	 *
	 * sched_fork() 做以下工作：
	 *   1. 初始化 task_struct 中的调度相关字段（se、rt、dl 等调度实体）
	 *   2. 继承父进程的调度策略和优先级（但 vruntime 重置以避免子进程获得不公平的时间片）
	 *   3. __set_task_cpu() 将进程分配到某个 CPU 的 runqueue
	 *   4. 注意：此时进程状态为 TASK_NEW，尚未加入 runqueue，
	 *      直到 wake_up_new_task() 被调用后才真正开始参与调度
	 */
	retval = sched_fork(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_policy;

	/*
	 * 继承父进程的性能监控事件（perf_event）设置：
	 * perf_event_init_task() 复制父进程已打开的 perf event（如硬件计数器、
	 * software 事件等），并根据 clone_flags 决定是否继承（CLONE_THREAD 等）。
	 * 错误时需要撤销 sched_fork()，因此跳转到 bad_fork_sched_cancel_fork。
	 */
	retval = perf_event_init_task(p, clone_flags);
	if (retval)
		goto bad_fork_sched_cancel_fork;
	/* 为新进程分配审计（audit）上下文，用于记录系统调用和安全事件 */
	retval = audit_alloc(p);
	if (retval)
		goto bad_fork_cleanup_perf;
	/* copy all the process information */
	shm_init_task(p); /* 初始化 System V 共享内存相关字段（shmem_list 等） */
	/*
	 * 【安全标签分配：LSM/SELinux】
	 * security_task_alloc() 调用所有已注册的 LSM（Linux Security Module）
	 * 钩子为新进程分配安全上下文（security blob）。
	 *
	 * 对于 SELinux：新进程初始继承父进程的 SELinux domain（类型）。
	 * 真正的 domain transition（域转换）发生在 execve() 时，由 SELinux
	 * 策略中的 type_transition 规则触发。
	 *
	 * Android 中每个 App 有独立的 SELinux domain（如 untrusted_app、
	 * isolated_app），这是 Android 沙箱的核心机制之一，限制 App
	 * 能访问的文件、设备、系统调用等。
	 */
	retval = security_task_alloc(p, clone_flags);
	if (retval)
		goto bad_fork_cleanup_audit;
	/*
	 * 复制 POSIX 信号量撤销记录（semundo）：
	 * 当进程持有 System V 信号量的操作时（semop 带 SEM_UNDO 标志），
	 * 内核记录"撤销值"，进程退出时自动回滚以防止死锁。
	 * copy_semundo() 根据 CLONE_SYSVSEM 标志决定共享或复制这些记录。
	 */
	retval = copy_semundo(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_security;
	/*
	 * 【资源复制：文件描述符表】
	 * copy_files() 处理进程的文件描述符表（files_struct）：
	 * - 若设置 CLONE_FILES（线程创建时）：共享同一个 files_struct（引用计数+1）
	 * - 否则（普通 fork）：深拷贝一份独立的文件描述符表
	 *
	 * 安全意义：fork 后子进程拥有父进程所有已打开文件的副本。
	 * Zygote fork App 进程后，App 进程会继承 Zygote 打开的文件描述符，
	 * 如果 Zygote 持有敏感 fd（如 /dev/ashmem、socket），子进程也能访问。
	 * 这是 Android 安全审计中需要关注的 fd 泄漏场景。
	 */
	retval = copy_files(clone_flags, p, args->no_files);
	if (retval)
		goto bad_fork_cleanup_semundo;
	/*
	 * 【资源复制：文件系统信息】
	 * copy_fs() 处理进程的文件系统上下文（fs_struct），包含：
	 * - 进程的根目录（root）：chroot 隔离的基础
	 * - 进程的当前工作目录（pwd）
	 * - umask（文件创建掩码）
	 *
	 * - CLONE_FS：共享同一个 fs_struct（线程模型）
	 * - 否则：独立复制一份
	 *
	 * CLONE_NEWNS（挂载命名空间）与此独立，由 copy_namespaces() 处理。
	 */
	retval = copy_fs(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_files;
	/*
	 * 【资源复制：信号处理器】
	 * copy_sighand() 处理进程的信号处理器表（sighand_struct）：
	 * - CLONE_SIGHAND：共享信号处理器（必须同时设置 CLONE_VM，即线程）
	 * - 否则：深拷贝，子进程有独立的信号处理器
	 *
	 * 注意：信号的 pending 队列由 copy_signal() 单独处理。
	 */
	retval = copy_sighand(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_fs;
	retval = copy_signal(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_sighand;
	/*
	 * 【资源复制：内存地址空间】
	 * copy_mm() 是 fork 性能的关键路径，处理进程的虚拟内存空间（mm_struct）：
	 *
	 * - CLONE_VM（线程）：共享同一个 mm_struct，线程间共享地址空间，
	 *   引用计数加一，无需复制页表。
	 *
	 * - 普通 fork（无 CLONE_VM）：使用 COW（写时复制）机制：
	 *   1. 复制父进程的页表结构（dup_mmap）
	 *   2. 将父子进程所有可写页面的 PTE 都标记为只读
	 *   3. 任何一方尝试写入时，触发缺页异常（page fault），
	 *      内核此时才真正分配新物理页并复制内容
	 *   COW 使 fork 极快，Zygote 的效率来源于此。
	 *
	 * 安全意义：COW 期间父进程的物理内存被子进程"可见"（只读），
	 * 这是 Dirty COW（CVE-2016-5195）漏洞的基础场景。
	 */
	retval = copy_mm(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_signal;
	/*
	 * 【资源复制：命名空间 —— 隔离边界的核心】
	 * copy_namespaces() 处理进程的命名空间（nsproxy），这是 Linux
	 * 容器技术和 Android 沙箱隔离的基础机制。
	 *
	 * 支持的命名空间类型（通过 CLONE_NEW* 标志创建新命名空间）：
	 * - CLONE_NEWNS   (mnt)：挂载命名空间，子进程看到独立的文件系统挂载树
	 * - CLONE_NEWUTS  (uts)：UTS 命名空间，独立的 hostname/domainname
	 * - CLONE_NEWIPC  (ipc)：IPC 命名空间，独立的 System V IPC、POSIX 消息队列
	 * - CLONE_NEWPID  (pid)：PID 命名空间，子进程在新空间中 PID=1（容器 init）
	 * - CLONE_NEWNET  (net)：网络命名空间，独立的网络接口、路由表、iptables
	 * - CLONE_NEWUSER (user)：用户命名空间，独立的 uid/gid 映射
	 * - CLONE_NEWTIME (time)：时间命名空间，独立的系统时钟偏移
	 *
	 * 若不设置任何 CLONE_NEW* 标志，子进程与父进程共享同一套命名空间
	 * （nsproxy 引用计数加一）。Zygote fork App 进程时不隔离网络和PID
	 * 命名空间，Android 的隔离主要依赖 uid 隔离 + SELinux，而非命名空间。
	 */
	retval = copy_namespaces(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_mm;
	/*
	 * 【资源复制：I/O 上下文】
	 * copy_io() 处理进程的 I/O 调度上下文（io_context），包含：
	 * - CFQ/BFQ 调度器的 I/O 优先级（ioprio）
	 * - 历史 I/O 统计信息（用于调度决策）
	 * - CLONE_IO：共享父进程的 io_context（引用计数+1），
	 *   适用于内核线程（io_worker）与创建者共享 I/O 优先级的场景。
	 * - 否则：子进程继承父进程的 ioprio 值，但拥有独立的 io_context 结构。
	 */
	retval = copy_io(clone_flags, p);
	if (retval)
		goto bad_fork_cleanup_namespaces;
	/*
	 * 【CPU 状态复制：子进程从哪里开始执行】
	 * copy_thread() 是体系结构相关的函数（见 arch/arm64/kernel/process.c
	 * 或 arch/x86/kernel/process.c），负责复制父进程的 CPU 寄存器状态
	 * 到子进程的内核栈帧（struct pt_regs），使子进程"看起来"像是刚从
	 * 系统调用返回。
	 *
	 * fork 返回值为 0 的原理：
	 *   copy_thread() 将子进程的返回值寄存器（x86: eax/rax，ARM64: x0）
	 *   设置为 0，而父进程的 copy_process() 返回子进程的 PID。
	 *   因此同一套代码 fork() 后，父进程得到子 PID，子进程得到 0。
	 *
	 * 内核线程（kernel_thread）：
	 *   copy_thread() 同样处理内核线程的创建，此时 pt_regs 为空，
	 *   子进程从指定的函数指针开始执行。
	 */
	retval = copy_thread(p, args);
	if (retval)
		goto bad_fork_cleanup_io;

	/*
	 * STACKLEAK 安全特性初始化：在每次系统调用返回时，内核会将用过的
	 * 栈空间填充为 STACKLEAK_POISON（0xFEBEFEBEFEBEFEBE），
	 * 防止攻击者通过 uninitialized stack 泄漏上一次系统调用的内核数据。
	 * stackleak_task_init() 记录当前栈的最低水位线（lowest_stack）。
	 */
	stackleak_task_init(p);

	if (pid != &init_struct_pid) {
		/*
		 * 【PID 分配】
		 * alloc_pid() 在目标 PID 命名空间中分配一个新的 PID 号。
		 * 若进程位于嵌套的 PID 命名空间，它在每层命名空间都有一个
		 * 不同的 PID 号（struct pid 中保存所有层级的 PID）。
		 * 最内层命名空间中的 PID 即进程自身看到的 PID（getpid() 返回值）。
		 */
		pid = alloc_pid(p->nsproxy->pid_ns_for_children, args->set_tid,
				args->set_tid_size);
		if (IS_ERR(pid)) {
			retval = PTR_ERR(pid);
			goto bad_fork_cleanup_thread;
		}
	}

	/*
	 * 【CLONE_PIDFD：创建进程文件描述符】
	 * This has to happen after we've potentially unshared the file
	 * descriptor table (so that the pidfd doesn't leak into the child
	 * if the fd table isn't shared).
	 *
	 * pidfd 是 Linux 5.2 引入的机制，用文件描述符表示一个进程，
	 * 解决了传统 PID 的"PID 重用竞态"问题（waitpid/kill 的 TOCTOU 漏洞）。
	 * 通过 pidfd，可以用 pidfd_send_signal()、waitid(P_PIDFD) 等接口
	 * 以无竞态的方式操作进程。
	 */
	if (clone_flags & CLONE_PIDFD) {
		unsigned flags = PIDFD_STALE; /* PIDFD_STALE：此时 pid 尚无进程附着，
		                                * 文件对象先创建，关联在 attach_pid 时完成 */

		if (clone_flags & CLONE_THREAD)
			flags |= PIDFD_THREAD; /* 线程级 pidfd（引用线程而非进程） */
		if (clone_flags & CLONE_PIDFD_AUTOKILL)
			flags |= PIDFD_AUTOKILL; /* 持有者退出时自动 SIGKILL 子进程 */

		/*
		 * Note that no task has been attached to @pid yet indicate
		 * that via CLONE_PIDFD.
		 */
		/*
		 * pidfd_prepare() 分配 struct file 和文件描述符号（fd），
		 * 并将文件对象绑定到 struct pid。成功返回 fd 号。
		 * 此时尚未安装到文件表（fd_install 在成功路径最后做），
		 * 确保失败时可以干净回滚。
		 */
		retval = pidfd_prepare(pid, flags, &pidfile);
		if (retval < 0)
			goto bad_fork_free_pid;
		pidfd = retval; /* 保存 fd 号，用于后续 put_user 和 fd_install */

		/* 将分配到的 fd 号写回用户空间的 args->pidfd 指针，
		 * 调用方（clone3 syscall）通过这个地址接收 pidfd */
		retval = put_user(pidfd, args->pidfd);
		if (retval)
			goto bad_fork_put_pidfd;
	}

#ifdef CONFIG_BLOCK
	p->plug = NULL; /* block layer 请求队列 plug 指针，新进程无待提交的 I/O 请求 */
	p->flags &= ~PF_BLOCK_TS; /* 清除 block 层时间戳标志 */
#endif
	futex_init_task(p); /* 初始化 futex 哈希桶指针和相关字段，用于用户态锁 */

	/*
	 * sigaltstack should be cleared when sharing the same VM
	 */
	/*
	 * 若是 CLONE_VM（共享地址空间）但不是 vfork，清除信号备用栈（sigaltstack）。
	 * 原因：备用栈地址来自共享的虚拟地址空间，父子进程同时在同一地址上使用备用栈
	 * 会导致栈破坏。vfork 不清除，因为 vfork 子进程会立即 exec，且
	 * vfork 子进程执行期间父进程被暂停，不存在并发问题。
	 */
	if ((clone_flags & (CLONE_VM|CLONE_VFORK)) == CLONE_VM)
		sas_ss_reset(p);

	/*
	 * Syscall tracing and stepping should be turned off in the
	 * child regardless of CLONE_PTRACE.
	 */
	/*
	 * 子进程不继承父进程的单步执行（single step）和系统调用追踪状态，
	 * 即使父进程被 ptrace，子进程默认不被追踪（需要显式 CLONE_PTRACE）。
	 * 这防止调试器意外控制 fork 出的子进程。
	 */
	user_disable_single_step(p); /* 清除体系结构相关的单步标志（如 x86 TF 标志位） */
	clear_task_syscall_work(p, SYSCALL_TRACE); /* 清除系统调用追踪标志 */
#if defined(CONFIG_GENERIC_ENTRY) || defined(TIF_SYSCALL_EMU)
	clear_task_syscall_work(p, SYSCALL_EMU); /* 清除系统调用仿真标志（ptrace SYSEMU） */
#endif
	clear_tsk_latency_tracing(p); /* 清除延迟追踪标志 */

	/* ok, now we should be set up.. */
	/*
	 * 【进程标识设置】
	 * pid_nr() 返回进程在初始 PID 命名空间（pid_ns=0）中的 PID 号，
	 * 这是内核内部和 /proc 文件系统使用的"全局 PID"。
	 */
	p->pid = pid_nr(pid);
	if (clone_flags & CLONE_THREAD) {
		/* 线程加入父进程的线程组：共享 group_leader 和 TGID */
		p->group_leader = current->group_leader;
		p->tgid = current->tgid;
	} else {
		/* 新进程自己是线程组组长（group_leader 指向自身）；
		 * TGID 等于自己的 PID（getpid() 与 gettid() 返回相同值）。 */
		p->group_leader = p;
		p->tgid = p->pid;
	}

	p->nr_dirtied = 0; /* 脏页计数器清零（写操作会递增此计数） */
	p->nr_dirtied_pause = 128 >> (PAGE_SHIFT - 10); /* 触发写回节流的脏页阈值（约 128KB） */
	p->dirty_paused_when = 0; /* 上次写回节流开始的时间戳 */

	p->pdeath_signal = 0; /* 父进程死亡信号清零；可由子进程调用 prctl(PR_SET_PDEATHSIG) 设置 */
	p->task_works = NULL; /* task_work 链表清空（task_work 在进程返回用户空间时执行） */
	clear_posix_cputimers_work(p); /* 清除 POSIX CPU 定时器的 task_work 回调链表 */

#ifdef CONFIG_KRETPROBES
	p->kretprobe_instances.first = NULL; /* kretprobe 实例链表清空（函数返回探针） */
#endif
#ifdef CONFIG_RETHOOK
	p->rethooks.first = NULL; /* rethook 链表清空（通用函数返回钩子，替代 kretprobe） */
#endif

	/*
	 * 【cgroup 策略检查：cgroup_can_fork】
	 * Ensure that the cgroup subsystem policies allow the new process to be
	 * forked. It should be noted that the new process's css_set can be changed
	 * between here and cgroup_post_fork() if an organisation operation is in
	 * progress.
	 *
	 * cgroup_can_fork() 调用各 cgroup 子系统的 can_fork 钩子：
	 * - pids controller：检查当前 cgroup 的进程数是否超过 pids.max 限制
	 *   （容器中常用此机制防止 fork bomb）
	 * - 其他子系统：内存、CPU 等子系统也可以在此拒绝 fork
	 * 失败跳转到 bad_fork_put_pidfd（PID 和 pidfd 需要释放）。
	 */
	retval = cgroup_can_fork(p, args);
	if (retval)
		goto bad_fork_put_pidfd;

	/*
	 * 【调度器 cgroup fork：sched_cgroup_fork】
	 * Now that the cgroups are pinned, re-clone the parent cgroup and put
	 * the new task on the correct runqueue. All this *before* the task
	 * becomes visible.
	 *
	 * This isn't part of ->can_fork() because while the re-cloning is
	 * cgroup specific, it unconditionally needs to place the task on a
	 * runqueue.
	 *
	 * sched_cgroup_fork() 将新进程关联到正确的调度 cgroup（cpu cgroup），
	 * 并根据 sched_ext（BPF 调度器）或 CFS/RT 调度类将 p 放入 runqueue。
	 * 此时进程仍处于 TASK_NEW 状态，不会被调度，但 runqueue 关联已完成。
	 */
	retval = sched_cgroup_fork(p, args);
	if (retval)
		goto bad_fork_cancel_cgroup;

	/*
	 * 多线程进程（CLONE_VM，非 vfork）需要共享 futex hash 表。
	 * need_futex_hash_allocate_default() 检查是否需要为该进程组分配默认 futex hash：
	 * 若当前进程是该 mm 的第一个线程（刚从单线程变为多线程），则分配。
	 * 分配后，即使后续失败也不释放（假设会有其他线程使用它，
	 * 最终由主线程退出时释放）。
	 */
	if (need_futex_hash_allocate_default(clone_flags)) {
		retval = futex_hash_allocate_default();
		if (retval)
			goto bad_fork_cancel_cgroup;
		/*
		 * If we fail beyond this point we don't free the allocated
		 * futex hash map. We assume that another thread will be created
		 * and makes use of it. The hash map will be freed once the main
		 * thread terminates.
		 */
	}
	/*
	 * 【时间戳记录：start_time / start_boottime】
	 * From this point on we must avoid any synchronous user-space
	 * communication until we take the tasklist-lock. In particular, we do
	 * not want user-space to be able to predict the process start-time by
	 * stalling fork(2) after we recorded the start_time but before it is
	 * visible to the system.
	 *
	 * 安全说明：若允许用户空间在记录时间后、进程可见前阻塞 fork，
	 * 攻击者可通过观察 /proc/<pid>/stat 中的 starttime 来精确推断
	 * 系统内部状态（侧信道攻击）。因此此后不能有任何用户空间通信。
	 */

	/*
	 * start_time：单调时钟（不受 NTP 和 settimeofday 影响），
	 *             对应 /proc/<pid>/stat 中的第22个字段（以 clock tick 为单位）。
	 * start_boottime：从系统启动开始计时（含系统睡眠），
	 *                 对应 /proc/<pid>/status 中的 VmRSS 等字段的时间基准。
	 */
	p->start_time = ktime_get_ns();
	p->start_boottime = ktime_get_boottime_ns();

	/*
	 * 【阶段五：获取 tasklist_lock，将新进程加入系统】
	 * Make it visible to the rest of the system, but dont wake it up yet.
	 * Need tasklist lock for parent etc handling!
	 *
	 * tasklist_lock 是全局读写锁，保护所有进程的 task_struct 链表、
	 * 进程树（parent/children/sibling 链表）以及 pid 相关结构。
	 * 写锁确保此刻没有其他进程在遍历进程列表（如 /proc 读取、ps 命令等）。
	 * write_lock_irq 同时禁用本地中断，防止中断处理程序访问进程树。
	 */
	write_lock_irq(&tasklist_lock);

	/* CLONE_PARENT re-uses the old parent */
	/*
	 * 设置新进程的父进程（real_parent）和退出信号（exit_signal）：
	 * - CLONE_PARENT：子进程与当前进程共享同一父进程（用于 daemon 进程，
	 *   避免子进程变成孤儿进程）
	 * - CLONE_THREAD：子进程是线程，exit_signal = -1 表示线程退出不向父发信号；
	 *   线程组的退出信号（通常 SIGCHLD）只由最后一个线程（group_leader）退出时发送
	 * - 普通 fork：父进程是当前进程（current），exit_signal 由调用者指定
	 * parent_exec_id 用于检测"父进程在子进程退出前是否执行了 exec"，
	 * 以决定是否发送 SIGCHLD（POSIX 要求父 exec 后不发 SIGCHLD）。
	 */
	if (clone_flags & (CLONE_PARENT|CLONE_THREAD)) {
		p->real_parent = current->real_parent;
		p->parent_exec_id = current->parent_exec_id;
		if (clone_flags & CLONE_THREAD)
			p->exit_signal = -1; /* 线程退出不发信号给父进程 */
		else
			p->exit_signal = current->group_leader->exit_signal;
	} else {
		p->real_parent = current; /* 普通 fork：父进程是当前进程 */
		p->parent_exec_id = current->self_exec_id;
		p->exit_signal = args->exit_signal; /* 通常为 SIGCHLD */
	}

	klp_copy_process(p); /* 内核热补丁（livepatch）状态复制，确保子进程使用最新补丁后的函数 */

	sched_core_fork(p); /* 调度器 core scheduling 状态复制：
	                      * core scheduling 用于 SMT（超线程）安全隔离，
	                      * 确保同一物理核上的超线程进程属于同一安全域，
	                      * 防止侧信道攻击（如 MDS/L1TF）。 */

	spin_lock(&current->sighand->siglock); /* 持有 siglock 保护后续信号相关操作 */

	rv_task_fork(p); /* Runtime Verification（运行时验证）框架通知：新进程 fork 事件 */

	rseq_fork(p, clone_flags); /* 重启序列（restartable sequences）初始化：
	                              * rseq 允许用户空间的关键区段在被抢占时自动"重启"，
	                              * 是实现无锁高性能用户空间 TLS 的基础。 */

	/*
	 * If zap_pid_ns_processes() was called after alloc_pid(), the new
	 * child missed SIGKILL.  If current is not in the same namespace,
	 * we can't rely on fatal_signal_pending() below.
	 */
	/*
	 * 竞态窗口检查：若在 alloc_pid() 之后、此处之前，
	 * zap_pid_ns_processes()（容器 shutdown）被调用，
	 * 它会向所有进程发送 SIGKILL，但新进程尚未进入系统而错过了该信号。
	 * PIDNS_ADDING 标志表示命名空间仍在接受新进程；若已清除，
	 * 说明命名空间正在关闭，新进程不应继续创建。
	 */
	if (unlikely(!(ns_of_pid(pid)->pid_allocated & PIDNS_ADDING))) {
		retval = -ENOMEM;
		goto bad_fork_core_free;
	}

	/* Let kill terminate clone/fork in the middle */
	/*
	 * 检查父进程是否收到了致命信号（如 SIGKILL）。
	 * 若 fork 过程中父进程被 kill，则中止 fork 并返回 -EINTR。
	 * 此检查在持有 siglock 时做，确保不会错过信号。
	 */
	if (fatal_signal_pending(current)) {
		retval = -EINTR;
		goto bad_fork_core_free;
	}

	/* No more failure paths after this point. */
	/* 从此处开始，不再有错误路径（除上面两个检查之外）。
	 * 所有资源已成功分配，以下代码完成最终的状态设置和进程树插入。 */

	/*
	 * Copy seccomp details explicitly here, in case they were changed
	 * before holding sighand lock.
	 */
	/*
	 * 复制 seccomp 过滤器配置：
	 * 必须在持有 sighand->siglock 之后才能复制，确保父进程的 seccomp
	 * 过滤器在复制过程中不被并发修改（另一个线程调用 prctl(SECCOMP_SET_MODE_FILTER)）。
	 * seccomp 是系统调用过滤机制（Linux 沙箱核心），Android 大量使用。
	 */
	copy_seccomp(p);

	if (clone_flags & CLONE_NNP)
		task_set_no_new_privs(p); /* 设置 no_new_privs 标志位，禁止子进程通过 exec 获得新权限 */

	/*
	 * 【阶段六：将新进程插入进程树（在 tasklist_lock 保护下）】
	 * init_task_pid_links() 初始化 p->pid_links[] 哈希节点，
	 * 使各 pid 类型的 attach_pid() 调用可以正确将进程关联到 pid 哈希表。
	 */
	init_task_pid_links(p);
	if (likely(p->pid)) {
		/*
		 * ptrace_init_task() 处理 ptrace 继承：
		 * - 若父进程正在被 ptrace（trace != 0），子进程也会被同一 tracer 追踪
		 * - CLONE_PTRACE 允许调用者显式要求子进程继承 ptrace 状态
		 * - 普通 fork 时子进程不被追踪（即使父进程被 ptrace）
		 */
		ptrace_init_task(p, (clone_flags & CLONE_PTRACE) || trace);

		init_task_pid(p, PIDTYPE_PID, pid); /* 关联 PID（进程/线程自身唯一标识） */
		if (thread_group_leader(p)) {
			/* 新进程是线程组组长（普通 fork 或 clone 创建新进程）：
			 * 需要关联 TGID、PGID、SID 四种 pid 类型。 */
			init_task_pid(p, PIDTYPE_TGID, pid); /* TGID = PID（自己是组长） */
			init_task_pid(p, PIDTYPE_PGID, task_pgrp(current)); /* 继承父进程的进程组 */
			init_task_pid(p, PIDTYPE_SID, task_session(current)); /* 继承父进程的会话 */

			if (is_child_reaper(pid)) {
				/* 若新进程是 PID 命名空间的第一个进程（PID=1），
				 * 设置其为该命名空间的 child_reaper（孤儿进程收割者）。
				 * 容器中的 init 进程即是如此。
				 * SIGNAL_UNKILLABLE：PID=1 的进程不响应同命名空间内的 SIGKILL，
				 * 保证容器 init 不会被意外杀死。 */
				struct pid_namespace *ns = ns_of_pid(pid);

				ASSERT_EXCLUSIVE_WRITER(ns->child_reaper);
				WRITE_ONCE(ns->child_reaper, p);
				p->signal->flags |= SIGNAL_UNKILLABLE;
			}
			/*
			 * 将之前在 multiprocess 链表中积累的延迟信号转移到子进程的
			 * shared_pending（线程组共享的 pending 信号队列）。
			 * 这些信号是 fork 期间发给父进程组的信号，现在子进程继承它们。
			 */
			p->signal->shared_pending.signal = delayed.signal;
			p->signal->tty = tty_kref_get(current->signal->tty); /* 继承控制终端 */
			/*
			 * Inherit has_child_subreaper flag under the same
			 * tasklist_lock with adding child to the process tree
			 * for propagate_has_child_subreaper optimization.
			 */
			/*
			 * 继承 subreaper 标志：若父进程或其祖先是 subreaper
			 * （通过 prctl(PR_SET_CHILD_SUBREAPER) 设置），
			 * 子进程的孤儿子进程也会被该 subreaper 收割，而非全局 init。
			 * 必须在 tasklist_lock 下设置，与 propagate_has_child_subreaper 优化同步。
			 */
			p->signal->has_child_subreaper = p->real_parent->signal->has_child_subreaper ||
							 p->real_parent->signal->is_child_subreaper;
			if (clone_flags & CLONE_AUTOREAP)
				p->signal->autoreap = 1; /* 子进程设置自动回收标志 */
			list_add_tail(&p->sibling, &p->real_parent->children); /* 挂入父进程子链表 */
			list_add_tail_rcu(&p->tasks, &init_task.tasks); /* 挂入全局任务链表（RCU 保护） */
			attach_pid(p, PIDTYPE_TGID); /* 将进程关联到 TGID 的 pid 哈希桶 */
			attach_pid(p, PIDTYPE_PGID); /* 将进程关联到 PGID 的 pid 哈希桶 */
			attach_pid(p, PIDTYPE_SID);  /* 将进程关联到 SID 的 pid 哈希桶 */
			__this_cpu_inc(process_counts); /* 本 CPU 进程计数器递增（用于负载统计） */
		} else {
			/* 新进程是线程（非组长）：加入现有线程组 */
			current->signal->nr_threads++;   /* 线程组线程总数 */
			current->signal->quick_threads++; /* 活跃线程快速计数（避免 atomic 开销） */
			atomic_inc(&current->signal->live); /* 原子计数：存活线程数 */
			refcount_inc(&current->signal->sigcnt); /* signal_struct 引用计数 */
			/*
			 * task_join_group_stop()：若线程组当前正处于 SIGSTOP/SIGTSTP 暂停状态，
			 * 新线程也应立即进入暂停（TASK_STOPPED），保持整个进程组状态一致。
			 */
			task_join_group_stop(p);
			list_add_tail_rcu(&p->thread_node,
					  &p->signal->thread_head); /* 加入线程组的线程链表 */
		}
		attach_pid(p, PIDTYPE_PID); /* 无论进程还是线程，都关联到 PID 哈希桶 */
		nr_threads++; /* 全局线程数递增（包括进程和线程） */
	}
	total_forks++; /* 全局 fork 统计计数（/proc/stat 的 processes 字段） */
	hlist_del_init(&delayed.node); /* 从 multiprocess 链表摘除（延迟信号已处理完毕） */
	spin_unlock(&current->sighand->siglock); /* 释放 siglock */
	syscall_tracepoint_update(p); /* 更新新进程的系统调用追踪点状态（ftrace/ebpf） */
	write_unlock_irq(&tasklist_lock); /* 释放进程树写锁，新进程现在对系统完全可见 */

	/*
	 * 在锁外安装 pidfd 文件描述符：
	 * 必须在 tasklist_lock 释放后才做，因为 fd_install 会访问进程文件表，
	 * 而持有 tasklist_lock 时不能获取其他锁（避免锁序问题）。
	 */
	if (pidfile)
		fd_install(pidfd, pidfile); /* 将 pidfd 安装到当前进程的文件描述符表 */

	proc_fork_connector(p); /* 发送 proc connector netlink 事件（用户空间进程监控工具使用） */
	/*
	 * sched_ext needs @p to be associated with its cgroup in its post_fork
	 * hook. cgroup_post_fork() should come before sched_post_fork().
	 */
	cgroup_post_fork(p, args); /* cgroup fork 后处理：通知各 cgroup 子系统新进程已加入 */
	sched_post_fork(p); /* 调度器 fork 后处理：sched_ext BPF 调度器的 ops.task_new 钩子 */
	perf_event_fork(p); /* 通知 perf 子系统：新进程已创建，可以开始统计其事件 */

	trace_task_newtask(p, clone_flags); /* ftrace 追踪点：新任务创建事件（perf/bpftrace 可见） */
	uprobe_copy_process(p, clone_flags); /* 复制 uprobe（用户探针）状态到子进程 */
	user_events_fork(p, clone_flags); /* 用户空间事件追踪（user_events）fork 通知 */

	/* 继承父进程的 OOM killer 分数调整值（/proc/<pid>/oom_score_adj），
	 * 用于内存不足时的进程选择决策。 */
	copy_oom_score_adj(clone_flags, p);

	return p; /* 成功：返回新进程的 task_struct 指针给 kernel_clone() */

/*
 * 【错误恢复路径（bad_fork_* 标签链）】
 * 以下标签形成一条"逆序撤销链"：资源按申请的逆序释放。
 * 每个标签处理一个分配阶段的清理，然后直接 fall-through（不含 return），
 * 依次执行所有更早阶段的清理，最终到达 fork_out 返回错误。
 *
 * 设计原则：越晚分配的资源，其 bad_fork 标签越靠前（越早执行清理），
 * 确保释放顺序与分配顺序严格相反，不会遗漏也不会重复释放。
 */

bad_fork_core_free:
	sched_core_free(p);  /* 撤销 sched_core_fork()：释放 core scheduling 状态 */
	spin_unlock(&current->sighand->siglock); /* 释放 siglock（在 tasklist_lock 内获取的） */
	write_unlock_irq(&tasklist_lock); /* 释放进程树写锁 */
bad_fork_cancel_cgroup:
	cgroup_cancel_fork(p, args); /* 撤销 cgroup_can_fork()：通知各 cgroup 子系统取消 fork */
bad_fork_put_pidfd:
	if (clone_flags & CLONE_PIDFD) {
		fput(pidfile);          /* 释放 pidfd 对应的 struct file */
		put_unused_fd(pidfd);   /* 归还文件描述符号（将其标记为未使用） */
	}
bad_fork_free_pid:
	if (pid != &init_struct_pid)
		free_pid(pid); /* 释放 alloc_pid() 分配的 struct pid（idle 进程的静态 pid 不需要释放） */
bad_fork_cleanup_thread:
	exit_thread(p); /* 撤销 copy_thread()：释放体系结构相关的线程状态 */
bad_fork_cleanup_io:
	if (p->io_context)
		exit_io_context(p); /* 撤销 copy_io()：释放 I/O 上下文 */
bad_fork_cleanup_namespaces:
	exit_nsproxy_namespaces(p); /* 撤销 copy_namespaces()：递减命名空间引用计数 */
bad_fork_cleanup_mm:
	if (p->mm) {
		mm_clear_owner(p->mm, p); /* 清除 mm_struct 的 owner 指针（避免悬挂指针） */
		mmput(p->mm);             /* 撤销 copy_mm()：递减 mm_struct 引用计数（可能触发释放） */
	}
bad_fork_cleanup_signal:
	if (!(clone_flags & CLONE_THREAD))
		free_signal_struct(p->signal); /* 撤销 copy_signal()：仅非线程需要释放（线程共享 signal_struct） */
bad_fork_cleanup_sighand:
	__cleanup_sighand(p->sighand); /* 撤销 copy_sighand()：递减 sighand_struct 引用计数 */
bad_fork_cleanup_fs:
	exit_fs(p); /* blocking */    /* 撤销 copy_fs()：递减 fs_struct 引用计数（可能阻塞） */
bad_fork_cleanup_files:
	exit_files(p); /* blocking */ /* 撤销 copy_files()：关闭所有文件描述符（可能阻塞） */
bad_fork_cleanup_semundo:
	exit_sem(p); /* 撤销 copy_semundo()：释放 POSIX 信号量撤销记录 */
bad_fork_cleanup_security:
	security_task_free(p); /* 撤销 security_task_alloc()：调用 LSM 钩子释放安全上下文 */
bad_fork_cleanup_audit:
	audit_free(p); /* 撤销 audit_alloc()：释放审计上下文 */
bad_fork_cleanup_perf:
	perf_event_free_task(p); /* 撤销 perf_event_init_task()：释放 perf event 资源 */
bad_fork_sched_cancel_fork:
	sched_cancel_fork(p); /* 撤销 sched_fork()：取消调度器 fork（从 runqueue 移除等） */
bad_fork_cleanup_policy:
	lockdep_free_task(p); /* 释放 lockdep 为该任务分配的记录（锁依赖追踪） */
#ifdef CONFIG_NUMA
	mpol_put(p->mempolicy); /* 撤销 mpol_dup()：递减 NUMA 内存策略引用计数 */
#endif
bad_fork_cleanup_delayacct:
	io_uring_free(p);        /* 撤销 io_uring_fork()：释放 io_uring 上下文 */
	delayacct_tsk_free(p);   /* 撤销 delayacct_tsk_init()：释放延迟记账结构 */
bad_fork_cleanup_count:
	dec_rlimit_ucounts(task_ucounts(p), UCOUNT_RLIMIT_NPROC, 1); /* 递减用户进程计数，撤销 copy_creds 的增量 */
	exit_cred_namespaces(p); /* 释放凭证相关命名空间引用 */
	exit_creds(p);           /* 撤销 copy_creds()：释放 struct cred */
bad_fork_free:
	/*
	 * 将进程状态标记为 TASK_DEAD，防止调度器或其他代码访问一个正在释放的 task_struct。
	 * WRITE_ONCE 确保编译器不会对此写操作进行重排序。
	 */
	WRITE_ONCE(p->__state, TASK_DEAD);
	exit_task_stack_account(p); /* 撤销内核栈的计账（accounting） */
	put_task_stack(p);          /* 释放内核栈（归还给 vmalloc 或 slab） */
	delayed_free_task(p);       /* 通过 RCU 延迟释放 task_struct，
	                              * 确保所有 RCU 读者完成后才真正 kfree */
fork_out:
	/*
	 * 无论在哪个阶段失败，都需要从 multiprocess 链表中移除 delayed.node，
	 * 否则 delayed 结构（栈上分配）被释放后链表将包含悬挂指针。
	 * 注意：此处不需要重新设置 retval，它在整个函数中始终保存正确的错误码。
	 */
	spin_lock_irq(&current->sighand->siglock);
	hlist_del_init(&delayed.node); /* 安全地从链表删除（即使 node 未挂入也不会崩溃） */
	spin_unlock_irq(&current->sighand->siglock);
	return ERR_PTR(retval); /* 返回错误指针，调用方通过 IS_ERR()/PTR_ERR() 检测 */
}

/*
 * 【init_idle_pids】为 idle 进程初始化 PID 相关数据结构
 *
 * idle 进程（swapper/N）是每个 CPU 的特殊进程，PID 为 0，不通过普通的
 * alloc_pid() 分配 PID，而是直接使用全局静态结构 init_struct_pid。
 * 该函数将所有 PID 类型（PIDTYPE_PID/PGID/SID/TGID）都指向 init_struct_pid。
 */
static inline void init_idle_pids(struct task_struct *idle)
{
	enum pid_type type;

	for (type = PIDTYPE_PID; type < PIDTYPE_MAX; ++type) {
		INIT_HLIST_NODE(&idle->pid_links[type]); /* not really needed */
		/* 将 idle 任务的所有 PID 类型都绑定到 init_struct_pid（PID 0） */
		init_task_pid(idle, type, &init_struct_pid);
	}
}

/*
 * 【idle_dummy】idle 进程的占位函数入口，实际上永远不会被执行
 *
 * fork_idle() 在构建 kernel_clone_args 时需要提供一个 fn 字段，
 * 但 idle 进程（swapper/N）创建后会由 init_idle() 将其直接设置为
 * CPU 的 idle 调度实体，永远不会通过 kthread/fn 机制启动，
 * 因此这个函数只是为了满足接口要求而存在的占位符。
 */
static int idle_dummy(void *dummy)
{
	/* This function is never called */
	return 0;
}

/*
 * 【fork_idle】为指定 CPU 创建 idle 进程（swapper/N）
 *
 * 系统启动时，每个 CPU 都需要一个 idle 进程，当该 CPU 上没有其他可运行任务时
 * 就执行 idle 进程（通常执行 cpu_idle_loop() 并发出 HLT/WFI 等低功耗指令）。
 *
 * 关键标志说明：
 *   CLONE_VM    — idle 进程共享 init 进程的地址空间（内核空间），避免额外分配 mm
 *   .kthread=1  — 标记为内核线程，不进入用户态
 *   .idle=1     — 告知 copy_process() 这是 idle 进程，做特殊处理
 *
 * 注意：该函数使用 __init 标记，只在启动阶段调用，之后内存可以被释放。
 * init_struct_pid 作为第一个参数传给 copy_process()，表示不分配新 PID，
 * 直接使用 PID 0（全局 init_struct_pid）。
 */
struct task_struct * __init fork_idle(int cpu)
{
	struct task_struct *task;
	struct kernel_clone_args args = {
		.flags		= CLONE_VM,      /* 共享 init_task 的内核地址空间 */
		.fn		= &idle_dummy,   /* 占位函数，实际不会被调用 */
		.fn_arg		= NULL,
		.kthread	= 1,             /* 标记为内核线程 */
		.idle		= 1,             /* 标记为 idle 进程，特殊调度处理 */
	};

	/* 直接传入 init_struct_pid，跳过 PID 分配；在对应 NUMA 节点上创建任务 */
	task = copy_process(&init_struct_pid, 0, cpu_to_node(cpu), &args);
	if (!IS_ERR(task)) {
		init_idle_pids(task);  /* 将所有 PID 类型绑定到 init_struct_pid（PID 0） */
		init_idle(task, cpu);  /* 将任务初始化为指定 CPU 的 idle 调度实体 */
	}

	return task;
}

/*
 * This is like kernel_clone(), but shaved down and tailored to just
 * creating io_uring workers. It returns a created task, or an error pointer.
 * The returned task is inactive, and the caller must fire it up through
 * wake_up_new_task(p). All signals are blocked in the created task.
 */
/*
 * 【create_io_thread】创建 io_uring 内核工作线程
 *
 * io_uring 需要在后台运行异步 I/O 请求，为此创建工作线程。
 * 返回的任务处于非活跃状态，调用者必须显式调用 wake_up_new_task() 来启动它。
 * 创建的任务中所有信号都被阻塞（通过 copy_process 中的信号初始化逻辑）。
 *
 * 各 CLONE 标志含义：
 *   CLONE_FS      — 共享文件系统上下文（根目录/cwd/umask），工作线程使用提交任务的 fs
 *   CLONE_FILES   — 共享文件描述符表，工作线程可以直接操作提交者打开的文件
 *   CLONE_SIGHAND — 共享信号处理器表（CLONE_THREAD 要求此标志）
 *   CLONE_THREAD  — 加入父进程线程组，io_uring worker 作为提交者进程的线程运行
 *   CLONE_IO      — 共享 I/O 上下文（blkcg、io 调度器状态），保持 I/O 亲和性
 *   CLONE_VM      — 共享地址空间，工作线程可访问提交者的内存（如 iovec 缓冲区）
 *   CLONE_UNTRACED — 禁止 ptrace 追踪，避免调试器干扰内部工作线程
 *
 * .io_thread=1   — 标记为 io_uring 线程，在 copy_process 中做特殊信号屏蔽处理
 * .user_worker=1 — 标记为用户工作线程，影响调度策略和 OOM 处理
 */
struct task_struct *create_io_thread(int (*fn)(void *), void *arg, int node)
{
	unsigned long flags = CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|
			      CLONE_IO|CLONE_VM|CLONE_UNTRACED;
	struct kernel_clone_args args = {
		.flags		= flags,
		.fn		= fn,
		.fn_arg		= arg,
		.io_thread	= 1,    /* 屏蔽所有信号，工作线程不响应信号 */
		.user_worker	= 1,    /* 标记为用户工作线程，影响调度和 OOM 处理 */
	};

	/* 直接调用 copy_process，不经过 kernel_clone 的唤醒流程，由调用者控制启动时机 */
	return copy_process(NULL, 0, node, &args);
}

/*
 *  Ok, this is the main fork-routine.
 *
 * It copies the process, and if successful kick-starts
 * it and waits for it to finish using the VM if required.
 */

/*
 * 【kernel_clone】fork/vfork/clone/clone3 系统调用的统一内核入口
 *
 * 所有创建进程/线程的系统调用最终都汇聚到这里：
 *   sys_fork()   → kernel_clone(flags=SIGCHLD)
 *   sys_vfork()  → kernel_clone(flags=CLONE_VFORK|CLONE_VM|SIGCHLD)
 *   sys_clone()  → kernel_clone(用户指定 flags)
 *   sys_clone3() → kernel_clone(通过 struct clone_args 传入)
 *
 * clone_flags 关键标志位说明：
 *
 * 进程/线程行为类：
 *   CLONE_VM        — 共享父进程的虚拟内存空间（线程的核心标志）
 *   CLONE_FS        — 共享文件系统信息（根目录、当前目录、umask）
 *   CLONE_FILES     — 共享文件描述符表
 *   CLONE_SIGHAND   — 共享信号处理器，必须与 CLONE_VM 同时使用
 *   CLONE_THREAD    — 加入父进程的线程组（共享 TGID），POSIX 线程语义
 *   CLONE_VFORK     — vfork 语义：父进程挂起，直到子进程 execve 或退出
 *
 * 命名空间隔离类（容器/沙箱基础）：
 *   CLONE_NEWNS     — 创建新的挂载命名空间（mnt ns）
 *   CLONE_NEWUTS    — 创建新的 UTS 命名空间（hostname 隔离）
 *   CLONE_NEWIPC    — 创建新的 IPC 命名空间
 *   CLONE_NEWPID    — 创建新的 PID 命名空间（子进程成为新空间的 PID 1）
 *   CLONE_NEWNET    — 创建新的网络命名空间（独立网络栈）
 *   CLONE_NEWUSER   — 创建新的用户命名空间（uid/gid 重映射）
 *   CLONE_NEWTIME   — 创建新的时间命名空间
 *
 * 其他：
 *   CLONE_PIDFD     — 在 parent_tid 指向的地址返回 pidfd（进程文件描述符）
 *   CLONE_PARENT_SETTID — fork 后将子进程 PID 写入父进程的指定地址
 *   CLONE_CHILD_CLEARTID — 子进程退出时清零并 futex 唤醒指定地址（POSIX 线程退出通知）
 *
 * 函数流程：
 *   1. 参数合法性检查（CLONE_PIDFD 与 CLONE_PARENT_SETTID 互斥等）
 *   2. 确定 ptrace 事件类型（PTRACE_EVENT_FORK/CLONE/VFORK）
 *   3. 调用 copy_process() 完成实际的进程复制
 *   4. 唤醒新进程进入调度队列（wake_up_new_task）
 *   5. 若 CLONE_VFORK，等待子进程释放 vfork_done 完成量
 */
pid_t kernel_clone(struct kernel_clone_args *args)
{
	u64 clone_flags = args->flags;
	struct completion vfork;   /* vfork 完成量：父进程通过它等待子进程完成 execve/exit */
	struct pid *pid;
	struct task_struct *p;
	int trace = 0;             /* ptrace 事件类型，0 表示不上报 */
	pid_t nr;                  /* 子进程在调用者 PID 命名空间中的 PID（虚拟 PID） */

	/*
	 * Creating an empty mount namespace implies creating a new mount
	 * namespace.  Set this before copy_process() so that the
	 * CLONE_NEWNS|CLONE_FS mutual exclusion check works correctly.
	 */
	/*
	 * CLONE_EMPTY_MNTNS 表示创建一个空的挂载命名空间（不继承任何挂载点）。
	 * 这必须隐含 CLONE_NEWNS（创建新的挂载命名空间），否则无法实现"空"的语义。
	 * 必须在 copy_process() 调用前设置，因为 copy_process 会检查
	 * CLONE_NEWNS 与 CLONE_FS 的互斥关系（同时设置两者会返回 -EINVAL）。
	 */
	if (clone_flags & CLONE_EMPTY_MNTNS) {
		clone_flags |= CLONE_NEWNS;  /* 空挂载命名空间必须是一个新的挂载命名空间 */
		args->flags = clone_flags;   /* 同步回 args，确保 copy_process 看到更新后的标志 */
	}

	/*
	 * For legacy clone() calls, CLONE_PIDFD uses the parent_tid argument
	 * to return the pidfd. Hence, CLONE_PIDFD and CLONE_PARENT_SETTID are
	 * mutually exclusive. With clone3() CLONE_PIDFD has grown a separate
	 * field in struct clone_args and it still doesn't make sense to have
	 * them both point at the same memory location. Performing this check
	 * here has the advantage that we don't need to have a separate helper
	 * to check for legacy clone().
	 */
	/*
	 * CLONE_PIDFD 与 CLONE_PARENT_SETTID 互斥检查：
	 * - 旧的 clone() 调用：两者都使用 parent_tidptr 参数，指向同一内存地址，
	 *   同时设置会导致该地址含义冲突（一个写 pidfd，一个写 pid）。
	 * - 新的 clone3() 调用：虽然 pidfd 有独立字段，但同时设置语义上仍无意义。
	 * 在这里统一检查，避免为 legacy clone() 单独写检查逻辑。
	 */
	if ((clone_flags & CLONE_PIDFD) &&
	    (clone_flags & CLONE_PARENT_SETTID) &&
	    (args->pidfd == args->parent_tid))   /* 两个指针指向同一地址，冲突 */
		return -EINVAL;

	/* 退出信号必须是合法的信号编号（0 或 1~_NSIG-1），否则拒绝 */
	if (!valid_signal(args->exit_signal))
		return -EINVAL;

	/*
	 * Determine whether and which event to report to ptracer.  When
	 * called from kernel_thread or CLONE_UNTRACED is explicitly
	 * requested, no event is reported; otherwise, report if the event
	 * for the type of forking is enabled.
	 */
	/*
	 * ptrace 事件类型选择：
	 * - 内核线程（kernel_thread 强制设置 CLONE_UNTRACED）或显式请求不追踪时，
	 *   trace=0，不上报任何 ptrace 事件。
	 * - 否则根据创建类型选择：
	 *   CLONE_VFORK        → PTRACE_EVENT_VFORK  （vfork 创建）
	 *   exit_signal≠SIGCHLD → PTRACE_EVENT_CLONE  （clone 创建，非标准子进程）
	 *   exit_signal==SIGCHLD → PTRACE_EVENT_FORK   （标准 fork）
	 * - 最终检查父进程 ptrace 是否真的启用了该事件，若未启用则 trace=0 跳过。
	 */
	if (!(clone_flags & CLONE_UNTRACED)) {
		if (clone_flags & CLONE_VFORK)
			trace = PTRACE_EVENT_VFORK;
		else if (args->exit_signal != SIGCHLD)
			trace = PTRACE_EVENT_CLONE;
		else
			trace = PTRACE_EVENT_FORK;

		/* 若 ptracer 未开启此类事件监听，则不上报（最常见路径） */
		if (likely(!ptrace_event_enabled(current, trace)))
			trace = 0;
	}

	/* 核心调用：完成实际的进程/线程复制（地址空间、文件、命名空间等） */
	p = copy_process(NULL, trace, NUMA_NO_NODE, args);

	/*
	 * 向内核熵池贡献一些延迟熵（latent entropy）。
	 * fork 操作本身的时序具有不可预测性，可作为随机性来源。
	 * 这是一种低成本的安全加固措施，特别是在 /dev/random 熵不足时有意义。
	 */
	add_latent_entropy();

	if (IS_ERR(p))
		return PTR_ERR(p);

	/*
	 * Do this prior waking up the new thread - the thread pointer
	 * might get invalid after that point, if the thread exits quickly.
	 */
	/*
	 * 必须在 wake_up_new_task() 之前完成所有对 task_struct 的访问。
	 * 子进程一旦被唤醒并获得 CPU，可能很快就退出，届时 task_struct 可能被释放。
	 * trace_sched_process_fork 向 tracepoint/perf 上报 fork 事件，需要 p 有效。
	 */
	trace_sched_process_fork(current, p);

	pid = get_task_pid(p, PIDTYPE_PID);  /* 获取子进程 pid 结构的引用（增加引用计数） */
	nr = pid_vnr(pid);                   /* 转换为调用者所在 PID 命名空间中的虚拟 PID */

	/* CLONE_PARENT_SETTID：将子进程 PID 写入父进程用户空间的指定地址 */
	if (clone_flags & CLONE_PARENT_SETTID)
		put_user(nr, args->parent_tid);  /* 写入用户态内存，忽略错误（非致命） */

	if (clone_flags & CLONE_VFORK) {
		/*
		 * vfork 语义：父进程必须等待子进程完成 execve 或 exit。
		 * 在栈上分配 completion 结构，将其地址存入子进程的 vfork_done 指针。
		 * 子进程在 execve 或 exit 时会调用 complete(vfork_done) 来唤醒父进程。
		 * get_task_struct 增加子进程引用计数，防止在 wait_for_vfork_done 之前被回收。
		 */
		p->vfork_done = &vfork;        /* 让子进程知道要通知哪个 completion */
		init_completion(&vfork);       /* 初始化完成量（初始计数=0，等待者会阻塞） */
		get_task_struct(p);            /* 增加引用计数，等待期间保持 p 有效 */
	}

	if (IS_ENABLED(CONFIG_LRU_GEN_WALKS_MMU) && !(clone_flags & CLONE_VM)) {
		/*
		 * LRU gen（mglru）页面老化功能需要追踪每个独立地址空间（mm）。
		 * 只有当子进程拥有独立的 mm（未设置 CLONE_VM）时才需要注册。
		 * 加 task_lock 是为了与 memcg 迁移操作同步，防止 mm 在注册过程中被迁移。
		 */
		task_lock(p);            /* 保护 p->mm，与 memcg 迁移同步 */
		lru_gen_add_mm(p->mm);   /* 将新的 mm 加入 LRU gen 的全局追踪链表 */
		task_unlock(p);
	}

	/* 将子进程加入调度队列，使其可以被调度运行；调用后 p 可能随时被调度执行或退出 */
	wake_up_new_task(p);

	/* forking complete and child started to run, tell ptracer */
	/*
	 * 子进程已经开始运行，现在通知 ptracer。
	 * 注意顺序：必须在 wake_up_new_task 之后，确保 ptracer 看到的是已运行的子进程。
	 */
	if (unlikely(trace))
		ptrace_event_pid(trace, pid);  /* 通过 pid（而非 p）引用，避免 use-after-free */

	if (clone_flags & CLONE_VFORK) {
		/*
		 * 等待子进程完成 execve 或 exit。
		 * wait_for_vfork_done 会在子进程调用 complete(vfork_done) 后返回。
		 * 若返回 0（成功等待，无信号中断），则上报 PTRACE_EVENT_VFORK_DONE 事件，
		 * 告知 ptracer vfork 等待已结束，父进程即将恢复运行。
		 * 注意：此处 p 引用是有效的，因为之前已经 get_task_struct(p)，
		 * wait_for_vfork_done 内部会释放这个额外的引用。
		 */
		if (!wait_for_vfork_done(p, &vfork))
			ptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
	}

	put_pid(pid);  /* 释放 get_task_pid 获得的 pid 引用 */
	return nr;     /* 返回子进程在调用者 PID 命名空间中的 PID */
}

/*
 * Create a kernel thread.
 */
/*
 * 【kernel_thread】创建内核线程
 *
 * 内核线程是纯内核态执行体，不进入用户空间。
 * 与 user_mode_thread 的区别：.kthread=1 且线程有 name（用于调试/ps 显示）。
 *
 * 强制设置的标志：
 *   CLONE_VM      — 共享内核地址空间（内核线程不需要独立的用户态地址空间）
 *   CLONE_UNTRACED — 禁止 ptrace 追踪内核线程，内核线程是内部实现，不应被调试器干扰
 *   & ~CSIGNAL    — 剥离低位的信号字段（CSIGNAL = 0xff），将其单独放入 exit_signal。
 *                   clone() 的 flags 低8位是"子进程退出时向父进程发送的信号"，
 *                   这与其他标志位混在一起，必须分离处理。
 *                   例如 flags=SIGCHLD 时，exit_signal=SIGCHLD，其余位为 0。
 */
pid_t kernel_thread(int (*fn)(void *), void *arg, const char *name,
		    unsigned long flags)
{
	struct kernel_clone_args args = {
		/* 强制加 CLONE_VM|CLONE_UNTRACED，并剥离低8位的信号字段 */
		.flags		= ((flags | CLONE_VM | CLONE_UNTRACED) & ~CSIGNAL),
		.exit_signal	= (flags & CSIGNAL),  /* 子进程退出时向父进程发送的信号 */
		.fn		= fn,
		.fn_arg		= arg,
		.name		= name,   /* 内核线程名，显示在 /proc/PID/comm 和 ps 输出中 */
		.kthread	= 1,      /* 标记为内核线程，copy_process 中会做相应处理 */
	};

	return kernel_clone(&args);
}

/*
 * Create a user mode thread.
 */
/*
 * 【user_mode_thread】创建可以进入用户态的内核启动线程
 *
 * 与 kernel_thread 的区别：
 *   - 没有 .kthread=1，因此不会被标记为内核线程
 *   - 没有 .name，不是长期后台守护线程
 *   - 设计用于内核代码创建后续会 execve 进入用户态的线程
 *     （典型用途：init 进程的创建，即 kernel_init 线程最终 exec /sbin/init）
 *
 * 同样强制设置 CLONE_VM|CLONE_UNTRACED，原因与 kernel_thread 相同：
 *   - 创建初期在内核态运行，共享地址空间即可
 *   - 不希望被 ptrace 追踪内核启动阶段的行为
 *   - 一旦 execve，这些标志的效果会被新的地址空间替代
 */
pid_t user_mode_thread(int (*fn)(void *), void *arg, unsigned long flags)
{
	struct kernel_clone_args args = {
		/* 与 kernel_thread 相同的标志处理：强制 VM+UNTRACED，分离信号字段 */
		.flags		= ((flags | CLONE_VM | CLONE_UNTRACED) & ~CSIGNAL),
		.exit_signal	= (flags & CSIGNAL),  /* 退出时向父进程发送的信号 */
		.fn		= fn,
		.fn_arg		= arg,
		/* 注意：无 .kthread=1，无 .name，与 kernel_thread 的核心区别 */
	};

	return kernel_clone(&args);
}

/*
 * 【sys_fork】传统 fork() 系统调用
 *
 * fork() 创建子进程：完整复制父进程地址空间（写时复制 COW），
 * 子进程获得独立的 mm_struct，父子进程相互独立。
 *
 * 仅设置 exit_signal=SIGCHLD：
 *   - 子进程退出时向父进程发送 SIGCHLD，触发 wait() 系列调用返回
 *   - 没有其他 CLONE_* 标志，意味着不共享任何资源（完整的进程复制语义）
 *
 * __ARCH_WANT_SYS_FORK 由具体架构选择定义，部分架构（如 ARM64）使用
 * clone() 实现 fork，不需要单独的 sys_fork 入口。
 *
 * nommu 模式（无 MMU 的嵌入式系统）无法实现 COW，fork() 没有意义，
 * 返回 -EINVAL。这类系统通常使用 vfork()+exec 模式。
 */
#ifdef __ARCH_WANT_SYS_FORK
SYSCALL_DEFINE0(fork)
{
#ifdef CONFIG_MMU
	struct kernel_clone_args args = {
		.exit_signal = SIGCHLD,  /* 子进程退出时通知父进程的信号 */
	};

	return kernel_clone(&args);
#else
	/* can not support in nommu mode */
	return -EINVAL;
#endif
}
#endif

/*
 * 【sys_vfork】vfork() 系统调用
 *
 * vfork() 是一种优化的 fork：
 *   CLONE_VM   — 子进程与父进程共享地址空间（不复制页表），效率极高
 *   CLONE_VFORK — 父进程挂起（阻塞在 wait_for_vfork_done），直到子进程
 *                 调用 execve()（此时子进程获得新的地址空间）或 _exit()
 *
 * 使用场景：在 execve 前不需要操作地址空间时，vfork 比 fork 快得多。
 * POSIX 规范要求 vfork 子进程只能调用 execve 或 _exit，
 * 任何其他操作（包括修改变量、调用库函数）都可能破坏父进程的栈帧，
 * 行为未定义（UB）。
 *
 * exit_signal=SIGCHLD：子进程退出时通知父进程（与 fork 一致）。
 */
#ifdef __ARCH_WANT_SYS_VFORK
SYSCALL_DEFINE0(vfork)
{
	struct kernel_clone_args args = {
		.flags		= CLONE_VFORK | CLONE_VM,  /* 共享地址空间 + 父进程等待 */
		.exit_signal	= SIGCHLD,                 /* 退出时通知父进程 */
	};

	return kernel_clone(&args);
}
#endif

/*
 * 【sys_clone】clone() 系统调用——多架构参数顺序兼容
 *
 * clone() 是 fork/vfork 的超集，允许精细控制哪些资源在父子进程间共享。
 * Linux 各架构对 clone() 的参数顺序有历史差异，通过条件编译处理：
 *
 * 标准顺序（x86-64、ARM64 等）：
 *   clone(clone_flags, newsp, parent_tidptr, child_tidptr, tls)
 *
 * CONFIG_CLONE_BACKWARDS（旧版 ARM、MIPS 等）：
 *   clone(clone_flags, newsp, parent_tidptr, tls, child_tidptr)
 *   — tls 和 child_tidptr 位置对调，历史遗留 ABI
 *
 * CONFIG_CLONE_BACKWARDS2（旧版 s390/sparc 等）：
 *   clone(newsp, clone_flags, ...)
 *   — newsp 和 clone_flags 位置对调
 *
 * CONFIG_CLONE_BACKWARDS3（旧版 microblaze 等）：
 *   clone(clone_flags, newsp, stack_size, ...)
 *   — 多一个 stack_size 参数（但内核实际不使用）
 *
 * 参数处理关键点：
 *   lower_32_bits(clone_flags) — 只取低 32 位：CLONE_* 标志只占用 32 位，
 *     高 32 位在旧 ABI 中可能是脏数据，必须截断。
 *   & ~CSIGNAL   — 剥离低 8 位信号字段，放入 exit_signal（含义分离）
 *   & CSIGNAL    — 提取低 8 位作为退出信号（通常是 SIGCHLD 或 0）
 *
 *   .pidfd = parent_tidptr   — 旧版 clone()：CLONE_PIDFD 时 pidfd 写入 parent_tid 地址
 *   .parent_tid = parent_tidptr — CLONE_PARENT_SETTID 时写子 PID 的目标地址
 *   （两者复用同一参数，这正是 kernel_clone 中 CLONE_PIDFD|CLONE_PARENT_SETTID 互斥检查的原因）
 */
#ifdef __ARCH_WANT_SYS_CLONE
#ifdef CONFIG_CLONE_BACKWARDS
/* ARM 等旧架构：tls 在 child_tidptr 之前 */
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 unsigned long, tls,
		 int __user *, child_tidptr)
#elif defined(CONFIG_CLONE_BACKWARDS2)
/* s390 等架构：newsp 与 clone_flags 顺序颠倒 */
SYSCALL_DEFINE5(clone, unsigned long, newsp, unsigned long, clone_flags,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr,
		 unsigned long, tls)
#elif defined(CONFIG_CLONE_BACKWARDS3)
/* microblaze 等架构：额外有 stack_size 参数（内核忽略） */
SYSCALL_DEFINE6(clone, unsigned long, clone_flags, unsigned long, newsp,
		int, stack_size,
		int __user *, parent_tidptr,
		int __user *, child_tidptr,
		unsigned long, tls)
#else
/* 标准参数顺序：x86-64、ARM64、RISC-V 等现代架构 */
SYSCALL_DEFINE5(clone, unsigned long, clone_flags, unsigned long, newsp,
		 int __user *, parent_tidptr,
		 int __user *, child_tidptr,
		 unsigned long, tls)
#endif
{
	struct kernel_clone_args args = {
		/* lower_32_bits：截断可能的脏高位；& ~CSIGNAL：分离信号字段 */
		.flags		= (lower_32_bits(clone_flags) & ~CSIGNAL),
		.pidfd		= parent_tidptr,  /* CLONE_PIDFD：pidfd 写入此地址 */
		.child_tid	= child_tidptr,   /* CLONE_CHILD_SETTID/CLEARTID：子进程地址 */
		.parent_tid	= parent_tidptr,  /* CLONE_PARENT_SETTID：子 PID 写入此地址 */
		.exit_signal	= (lower_32_bits(clone_flags) & CSIGNAL),  /* 退出通知信号 */
		.stack		= newsp,          /* 子进程栈顶指针（0 表示继承父进程栈） */
		.tls		= tls,            /* CLONE_SETTLS：子进程 TLS 基地址 */
	};

	return kernel_clone(&args);
}
#endif

/*
 * 【copy_clone_args_from_user】从用户空间安全拷贝 clone3() 的参数结构体
 *
 * clone3() 使用版本化的结构体（struct clone_args）传递参数，
 * 支持向前兼容：旧内核拒绝过大的结构体，新内核能接受小结构体（忽略新字段）。
 *
 * 版本化设计：
 *   VER0：基础字段，截止到 tls（clone3 最初版本）
 *   VER1：新增 set_tid 和 set_tid_size（指定子进程 PID）
 *   VER2：新增 cgroup（CLONE_INTO_CGROUP，创建时加入指定 cgroup）
 *
 * noinline：防止编译器将这个大函数内联，减小 sys_clone3 的栈帧。
 *
 * set_tid 的特殊处理：
 *   set_tid 是用户空间的数组指针，不能直接存入 kernel_clone_args（内核不保存用户指针）。
 *   必须将其内容拷贝到内核栈上预分配的 kargs->set_tid 数组中（由 sys_clone3 分配）。
 *   这里先保存 kset_tid 指针，在结构体整体赋值后再填充数组内容，
 *   最后恢复 kargs->set_tid = kset_tid（因为整体赋值会清零此字段）。
 */
static noinline int copy_clone_args_from_user(struct kernel_clone_args *kargs,
					      struct clone_args __user *uargs,
					      size_t usize)
{
	int err;
	struct clone_args args;
	pid_t *kset_tid = kargs->set_tid;  /* 提前保存内核栈上的 set_tid 数组指针 */

	/*
	 * 编译期断言：确保版本常量与结构体实际布局一致。
	 * 若结构体定义被意外修改（字段增减/重排），这里会产生编译错误，防止 ABI 悄然破坏。
	 * BUILD_BUG_ON 在条件为真时触发编译错误（与 assert 相反）。
	 */
	BUILD_BUG_ON(offsetofend(struct clone_args, tls) !=
		     CLONE_ARGS_SIZE_VER0);            /* VER0 大小必须精确匹配 tls 字段末尾 */
	BUILD_BUG_ON(offsetofend(struct clone_args, set_tid_size) !=
		     CLONE_ARGS_SIZE_VER1);            /* VER1 大小必须精确匹配 set_tid_size 末尾 */
	BUILD_BUG_ON(offsetofend(struct clone_args, cgroup) !=
		     CLONE_ARGS_SIZE_VER2);            /* VER2 大小必须精确匹配 cgroup 末尾 */
	BUILD_BUG_ON(sizeof(struct clone_args) != CLONE_ARGS_SIZE_VER2); /* 当前版本就是 VER2 */

	/* 防止恶意用户传入巨大结构体耗尽内核栈，1 页（4KB）已足够存放所有版本 */
	if (unlikely(usize > PAGE_SIZE))
		return -E2BIG;
	/* 结构体必须至少包含 VER0 的基础字段，否则无法使用 clone3 */
	if (unlikely(usize < CLONE_ARGS_SIZE_VER0))
		return -EINVAL;

	/*
	 * copy_struct_from_user：版本化安全拷贝。
	 * - 若用户结构体比内核结构体小（旧用户空间）：新字段清零，向后兼容
	 * - 若用户结构体比内核结构体大（新用户空间+旧内核）：
	 *   检查超出部分全为零，否则返回 -E2BIG，确保用户不依赖内核忽略的字段
	 */
	err = copy_struct_from_user(&args, sizeof(args), uargs, usize);
	if (err)
		return err;

	/* set_tid_size 最大为 PID 命名空间嵌套深度，每层指定一个 PID */
	if (unlikely(args.set_tid_size > MAX_PID_NS_LEVEL))
		return -EINVAL;

	/* set_tid 与 set_tid_size 必须同时为零或同时非零，不允许部分指定 */
	if (unlikely(!args.set_tid && args.set_tid_size > 0))
		return -EINVAL;

	if (unlikely(args.set_tid && args.set_tid_size == 0))
		return -EINVAL;

	/*
	 * Verify that higher 32bits of exit_signal are unset
	 */
	/*
	 * exit_signal 在 clone3 中是 64 位字段，但语义上只使用低 8 位（CSIGNAL 掩码）。
	 * 高 32 位必须为零，防止未来扩展时产生歧义，也防止用户误传非法值。
	 */
	if (unlikely(args.exit_signal & ~((u64)CSIGNAL)))
		return -EINVAL;

	/* CLONE_INTO_CGROUP 需要 VER2 结构体支持，且 cgroup fd 必须是有效的 int 范围 */
	if ((args.flags & CLONE_INTO_CGROUP) &&
	    (args.cgroup > INT_MAX || usize < CLONE_ARGS_SIZE_VER2))
		return -EINVAL;

	/* 整体赋值：将用户结构体字段映射到内核参数结构体 */
	*kargs = (struct kernel_clone_args){
		.flags		= args.flags,
		.pidfd		= u64_to_user_ptr(args.pidfd),      /* u64 → 用户空间指针 */
		.child_tid	= u64_to_user_ptr(args.child_tid),
		.parent_tid	= u64_to_user_ptr(args.parent_tid),
		.exit_signal	= args.exit_signal,
		.stack		= args.stack,
		.stack_size	= args.stack_size,
		.tls		= args.tls,
		.set_tid_size	= args.set_tid_size,
		.cgroup		= args.cgroup,
		/* 注意：set_tid 字段此处为 NULL，后面单独恢复 */
	};

	/*
	 * 将 set_tid 数组从用户空间拷贝到内核栈预分配的缓冲区。
	 * set_tid_size 已经过上面的检查（<= MAX_PID_NS_LEVEL），不会溢出。
	 * args.set_tid 是用户空间地址（u64），通过 u64_to_user_ptr 转为内核可用指针。
	 */
	if (args.set_tid &&
		copy_from_user(kset_tid, u64_to_user_ptr(args.set_tid),
			(kargs->set_tid_size * sizeof(pid_t))))
		return -EFAULT;

	/* 恢复 set_tid 指针（整体赋值将其清零，现在指向内核栈上的数组） */
	kargs->set_tid = kset_tid;

	return 0;
}

/**
 * clone3_stack_valid - check and prepare stack
 * @kargs: kernel clone args
 *
 * Verify that the stack arguments userspace gave us are sane.
 * In addition, set the stack direction for userspace since it's easy for us to
 * determine.
 */
/*
 * 【clone3_stack_valid】验证并调整 clone3() 传入的栈参数
 *
 * clone3() 要求用户明确指定栈的起始地址和大小，内核负责根据架构的栈增长方向
 * 将"起始地址"调整为正确的"栈顶指针"（sp）。
 *
 * 栈方向处理：
 *   大多数架构（x86、ARM、RISC-V 等）：栈向低地址增长（GROWSDOWN）
 *     用户提供：stack = 栈底（低地址）, stack_size = 大小
 *     内核调整：sp = stack + stack_size（栈顶 = 高地址端）
 *     → kargs->stack += kargs->stack_size
 *
 *   少数架构（如 HP PA-RISC）：栈向高地址增长（GROWSUP，CONFIG_STACK_GROWSUP）
 *     用户提供：stack = 栈底（低地址）, stack_size = 大小
 *     内核不调整：sp = stack（栈顶就是低地址端）
 *
 * 这样 clone3 的接口对用户空间是统一的：始终传入"低地址+大小"，
 * 由内核负责转换，避免用户程序关心架构差异。
 */
static inline bool clone3_stack_valid(struct kernel_clone_args *kargs)
{
	if (kargs->stack == 0) {
		/* stack=0 表示不指定栈（继承父进程），此时 stack_size 也必须为 0 */
		if (kargs->stack_size > 0)
			return false;
	} else {
		/* 指定了栈地址，则 stack_size 也必须非零，否则无意义 */
		if (kargs->stack_size == 0)
			return false;

		/* 检查用户提供的栈地址范围是否可访问（防止内核后续操作出错） */
		if (!access_ok((void __user *)kargs->stack, kargs->stack_size))
			return false;

#if !defined(CONFIG_STACK_GROWSUP)
		/*
		 * 向低地址增长的架构（绝大多数）：
		 * 将栈底地址调整为栈顶地址（sp 指向高地址端）。
		 * 此后 kargs->stack 就是直接传给子进程 sp 寄存器的值。
		 */
		kargs->stack += kargs->stack_size;
#endif
	}

	return true;
}

/*
 * 【clone3_args_valid】验证 clone3() 参数组合的合法性
 *
 * 采用"白名单"设计：只允许已知的合法标志，拒绝任何未知标志位。
 * 这样做的好处是：当内核新增标志时，旧版内核会拒绝，避免静默忽略导致行为不一致。
 * 未来可安全扩展，用户空间通过 errno=EINVAL 知道需要降级到旧接口。
 */
static bool clone3_args_valid(struct kernel_clone_args *kargs)
{
	/* Verify that no unknown flags are passed along. */
	/*
	 * 白名单检查：flags 中不能有任何未被认可的位。
	 * 允许的标志包括：
	 *   CLONE_LEGACY_FLAGS  — 传统 clone() 支持的所有标志
	 *   CLONE_CLEAR_SIGHAND — 清除继承的信号处理器（clone3 新增）
	 *   CLONE_INTO_CGROUP   — 创建时加入指定 cgroup（clone3 VER2 新增）
	 *   CLONE_AUTOREAP      — 子进程退出时自动回收，无需 wait()
	 *   CLONE_NNP           — 禁止获取新权限（no_new_privs 的 clone 版本）
	 *   CLONE_PIDFD_AUTOKILL — pidfd 关闭时自动杀死子进程
	 *   CLONE_EMPTY_MNTNS   — 创建空的挂载命名空间
	 */
	if (kargs->flags &
	    ~(CLONE_LEGACY_FLAGS | CLONE_CLEAR_SIGHAND |
	      CLONE_INTO_CGROUP | CLONE_AUTOREAP | CLONE_NNP |
	      CLONE_PIDFD_AUTOKILL | CLONE_EMPTY_MNTNS))
		return false;

	/*
	 * - make the CLONE_DETACHED bit reusable for clone3
	 * - make the CSIGNAL bits reusable for clone3
	 */
	/*
	 * clone3 复用了旧 clone() 中已废弃的位：
	 *   CLONE_DETACHED：旧 clone() 中是无效标志，clone3 将此位用于新用途，
	 *     因此必须拒绝用户传入 CLONE_DETACHED（防止旧程序误用）。
	 *   CSIGNAL（低8位）：旧 clone() 用低8位传递退出信号，clone3 有独立的
	 *     exit_signal 字段，所以 flags 的低8位可以被复用；
	 *     但 CLONE_NEWTIME（也在低8位范围内）是合法的，需要排除。
	 *     公式：(CSIGNAL & ~CLONE_NEWTIME) = 需要拒绝的低8位组合
	 */
	if (kargs->flags & (CLONE_DETACHED | (CSIGNAL & (~CLONE_NEWTIME))))
		return false;

	/*
	 * CLONE_SIGHAND 和 CLONE_CLEAR_SIGHAND 语义互斥：
	 *   CLONE_SIGHAND      — 子进程继承并共享父进程的信号处理器表
	 *   CLONE_CLEAR_SIGHAND — 子进程重置所有信号处理器为默认（SIG_DFL）
	 * 同时设置没有意义（共享了又要清除），内核拒绝此组合。
	 */
	if ((kargs->flags & (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND)) ==
	    (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND))
		return false;

	/*
	 * CLONE_THREAD 或 CLONE_PARENT 时，exit_signal 必须为 0：
	 *   CLONE_THREAD — 新线程加入同一线程组，退出时不向父进程发信号
	 *   CLONE_PARENT — 新进程的父进程与调用者相同，退出信号由父进程决定
	 * 这两种情况下设置 exit_signal 会产生歧义，内核拒绝。
	 */
	if ((kargs->flags & (CLONE_THREAD | CLONE_PARENT)) &&
	    kargs->exit_signal)
		return false;

	/* 验证并调整栈参数（含架构相关的栈方向转换） */
	if (!clone3_stack_valid(kargs))
		return false;

	return true;
}

/**
 * sys_clone3 - create a new process with specific properties
 * @uargs: argument structure
 * @size:  size of @uargs
 *
 * clone3() is the extensible successor to clone()/clone2().
 * It takes a struct as argument that is versioned by its size.
 *
 * Return: On success, a positive PID for the child process.
 *         On error, a negative errno number.
 */
/*
 * 【sys_clone3】clone3() 系统调用——clone 的可扩展继任者
 *
 * clone3 相对于 clone 的改进：
 *   1. 通过结构体传参，避免了 clone 的多架构参数顺序差异问题
 *   2. 版本化设计（按结构体大小判断版本），未来可安全添加新字段
 *   3. pidfd、set_tid 等新功能有专用字段，不需要复用旧字段
 *
 * set_tid 栈上分配策略：
 *   set_tid 数组最多 MAX_PID_NS_LEVEL 个元素（PID 命名空间最大嵌套深度，通常 32）。
 *   在栈上分配（而非 kmalloc）避免了内存分配失败和内存碎片，
 *   并且在函数返回时自动释放，无需手动 free。
 *   copy_clone_args_from_user 会将用户空间数组拷贝到此缓冲区。
 *
 * __ARCH_BROKEN_SYS_CLONE3：某些架构的 clone3 系统调用入口尚未实现，
 *   通过编译时 warning 提醒维护者，运行时返回 -ENOSYS。
 */
SYSCALL_DEFINE2(clone3, struct clone_args __user *, uargs, size_t, size)
{
	int err;

	struct kernel_clone_args kargs;
	pid_t set_tid[MAX_PID_NS_LEVEL];  /* 栈上分配 set_tid 数组，避免动态内存分配 */

#ifdef __ARCH_BROKEN_SYS_CLONE3
#warning clone3() entry point is missing, please fix
	return -ENOSYS;
#endif

	/* 提前将 set_tid 指向栈上缓冲区，copy_clone_args_from_user 会填充内容 */
	kargs.set_tid = set_tid;

	/* 从用户空间安全拷贝参数，处理版本兼容和各字段合法性 */
	err = copy_clone_args_from_user(&kargs, uargs, size);
	if (err)
		return err;

	/* 检查参数组合合法性（白名单标志检查、互斥检查、栈参数检查） */
	if (!clone3_args_valid(&kargs))
		return -EINVAL;

	return kernel_clone(&kargs);
}

/*
 * 【walk_process_tree】对进程树进行深度优先遍历
 *
 * 从 top 进程开始，深度优先遍历其整个子进程树，对每个子进程调用 visitor 回调。
 * 持有 tasklist_lock 读锁期间完成遍历，保证进程树结构稳定。
 *
 * visitor 返回值语义：
 *   0  — 继续遍历当前层级的下一个兄弟节点
 *   >0 — 进入该子进程的子树（深度优先下探）
 *   <0 — 立即终止整个遍历（goto out）
 *
 * 实现技巧：使用 goto 而非递归实现深度优先遍历（DFS）。
 * 递归实现在内核中有栈溢出风险（进程树可能很深），goto 方案完全避免了栈增长。
 *
 * 遍历逻辑（非递归 DFS）：
 *   down 标签：遍历 leader（线程组）的所有线程（parent），
 *     对每个线程的直接子进程列表调用 visitor。
 *     若 visitor 返回 >0，将 leader 切换为该子进程，goto down 下探。
 *
 *   up 标签：当前层级遍历完毕，通过 real_parent 和 group_leader
 *     回溯到父进程线程组，继续遍历父进程的下一个子进程（goto up）。
 *     直到回溯到原始的 top 进程为止。
 */
void walk_process_tree(struct task_struct *top, proc_visitor visitor, void *data)
{
	struct task_struct *leader, *parent, *child;
	int res;

	read_lock(&tasklist_lock);            /* 持锁保证遍历期间进程树不变 */
	leader = top = top->group_leader;     /* 从线程组 leader 开始（而非任意线程） */
down:
	/* 遍历 leader 线程组的所有线程，每个线程都可能有自己的子进程 */
	for_each_thread(leader, parent) {
		/* 遍历该线程的直接子进程列表 */
		list_for_each_entry(child, &parent->children, sibling) {
			res = visitor(child, data);   /* 调用访问者，处理当前子进程 */
			if (res) {
				if (res < 0)
					goto out;         /* visitor 要求终止，立即退出 */
				leader = child;           /* 下探：以 child 为新起点 */
				goto down;                /* 进入 child 的子树 */
			}
up:
			;   /* 空语句：作为 up 标签的合法位置（标签不能直接跟 } ） */
		}
	}

	if (leader != top) {
		/*
		 * 当前层级遍历完毕，回溯到父进程层级继续遍历。
		 * child/leader 就是我们刚才下探进去的那个子进程，
		 * 其 real_parent 是发起下探的父线程，
		 * parent->group_leader 是父线程组的 leader，
		 * goto up 继续处理父层级中 child 之后的兄弟节点。
		 */
		child = leader;
		parent = child->real_parent;
		leader = parent->group_leader;
		goto up;                          /* 回溯到父层级，继续兄弟节点遍历 */
	}
out:
	read_unlock(&tasklist_lock);
}

#ifndef ARCH_MIN_MMSTRUCT_ALIGN
#define ARCH_MIN_MMSTRUCT_ALIGN 0
#endif

/*
 * 【sighand_ctor】sighand_struct 的 SLAB 构造函数
 *
 * SLAB 分配器支持构造函数（ctor），在每次从 slab 缓存分配对象时自动调用。
 * 构造函数用于初始化那些"生命周期与缓存槽相同"的成员：
 * 即每次分配时都需要处于固定初始状态的成员，而不是每次 alloc 后手动初始化。
 *
 * 这里初始化：
 *   siglock         — 保护信号处理器表的自旋锁，必须在使用前初始化
 *   signalfd_wqh    — signalfd 的等待队列头，用于 signalfd_read() 的阻塞等待
 *
 * 注意：SLAB_TYPESAFE_BY_RCU 标志（在 proc_caches_init 中设置）允许通过 RCU
 * 访问 sighand_struct，但需要在访问时重新验证内容，构造函数保证锁的初始状态正确。
 */
static void sighand_ctor(void *data)
{
	struct sighand_struct *sighand = data;

	spin_lock_init(&sighand->siglock);       /* 初始化保护信号处理表的自旋锁 */
	init_waitqueue_head(&sighand->signalfd_wqh); /* 初始化 signalfd 等待队列 */
}

/*
 * 【mm_cache_init】创建 mm_struct 的 SLAB 缓存
 *
 * mm_struct 的大小不是编译期常量，包含两个动态部分：
 *   1. cpumask：mm_cpumask() 位图，大小取决于系统最大 CPU 数（nr_cpu_ids）
 *      包括 CPU 热插拔（hotplug）的最大值，在系统启动时确定
 *   2. mm_cid：per-CPU 的并发 ID 数组（用于 MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ
 *      和 per-mm per-CPU ID 分配），大小同样依赖 nr_cpu_ids
 *
 * 因此必须在运行时动态计算 mm_size，并据此创建专用 SLAB 缓存，
 * 而不能使用编译期固定大小的 kmem_cache_create。
 *
 * kmem_cache_create_usercopy：与普通 create 的区别是指定了可被用户空间访问的区域
 * （saved_auxv），这是为了满足 CONFIG_HARDENED_USERCOPY 的安全检查要求。
 * SLAB 标志说明：
 *   SLAB_HWCACHE_ALIGN — 按硬件 cache line 对齐，减少 false sharing
 *   SLAB_PANIC         — 分配失败时直接 panic（mm_cachep 是系统关键资源）
 *   SLAB_ACCOUNT       — 将此缓存的使用量计入 kmemcg（内存 cgroup 统计）
 */
void __init mm_cache_init(void)
{
	unsigned int mm_size;

	/*
	 * The mm_cpumask is located at the end of mm_struct, and is
	 * dynamically sized based on the maximum CPU number this system
	 * can have, taking hotplug into account (nr_cpu_ids).
	 */
	/* 动态计算 mm_struct 总大小：基础结构 + cpumask 位图 + mm_cid 数组 */
	mm_size = sizeof(struct mm_struct) + cpumask_size() + mm_cid_size();

	mm_cachep = kmem_cache_create_usercopy("mm_struct",
			mm_size, ARCH_MIN_MMSTRUCT_ALIGN,   /* 架构最小对齐要求（0 表示默认） */
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			offsetof(struct mm_struct, saved_auxv),       /* usercopy 区域起始偏移 */
			sizeof_field(struct mm_struct, saved_auxv),   /* usercopy 区域大小 */
			NULL);   /* 无构造函数（mm_struct 由 allocate_mm/mm_init 初始化） */
}

/*
 * 【proc_caches_init】为进程/线程相关数据结构创建 SLAB 缓存
 *
 * 在系统启动早期（__init）初始化所有进程创建路径中频繁分配的结构体缓存。
 * 使用专用 SLAB 缓存（而非通用 kmalloc）的优势：
 *   1. 减少碎片：对象大小固定，缓存利用率高
 *   2. 性能：per-CPU 缓存减少锁竞争，对象可以保持热缓存状态
 *   3. 调试：可以为每种对象启用独立的 KASAN/KMEMLEAK 跟踪
 */
void __init proc_caches_init(void)
{
	/*
	 * sighand_cache：信号处理器表缓存
	 * SLAB_TYPESAFE_BY_RCU — 允许通过 RCU 访问已释放的对象（访问者需重新验证）。
	 *   这是性能关键路径的优化：信号处理器查找可以在不持锁的情况下进行，
	 *   但必须用 rcu_read_lock + 重新验证的模式保证安全。
	 * sighand_ctor — 构造函数初始化 siglock 和 signalfd_wqh
	 */
	sighand_cachep = kmem_cache_create("sighand_cache",
			sizeof(struct sighand_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_TYPESAFE_BY_RCU|
			SLAB_ACCOUNT, sighand_ctor);

	/*
	 * signal_cache：信号队列和线程组信息缓存
	 * 每个线程组共享一个 signal_struct，存储：
	 *   - 挂起信号队列（sigpending）
	 *   - 线程组统计信息（utime/stime 等）
	 *   - 作业控制状态
	 */
	signal_cachep = kmem_cache_create("signal_cache",
			sizeof(struct signal_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);

	/* 初始化 exec 状态（binfmt、exec 相关数据结构） */
	exec_state_init();

	/*
	 * files_cache：文件描述符表缓存
	 * files_struct 包含：
	 *   - fdtable（文件描述符数组和 close-on-exec 位图）
	 *   - 引用计数（多线程共享时）
	 */
	files_cachep = kmem_cache_create("files_cache",
			sizeof(struct files_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);

	/*
	 * fs_cache：文件系统上下文缓存
	 * fs_struct 包含：
	 *   - 根目录 dentry/vfsmount（/proc/PID/root）
	 *   - 当前工作目录（/proc/PID/cwd）
	 *   - umask
	 *   - users 计数（多线程通过 CLONE_FS 共享时）
	 */
	fs_cachep = kmem_cache_create("fs_cache",
			sizeof(struct fs_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_ACCOUNT,
			NULL);

	mmap_init();            /* 初始化 vm_area_struct 等内存映射相关缓存 */
	nsproxy_cache_init();   /* 初始化命名空间代理（nsproxy）缓存 */
}

/*
 * Check constraints on flags passed to the unshare system call.
 */
/*
 * 【check_unshare_flags】检查 unshare() 系统调用的标志合法性
 *
 * unshare() 允许进程将之前通过 clone() 共享的资源"独立化"（解除共享）。
 * 本函数检查两类约束：
 *
 * 1. 白名单检查：只允许已知的可 unshare 标志，拒绝其他位
 *    （与 clone3_args_valid 的设计思路相同，防止用户传入无意义的位）
 *
 * 2. 资源依赖约束（"Not implemented, but pretend it works"注释说明了背景）：
 *    某些资源的 unshare 在技术上尚未完全实现或有内在约束：
 *
 *    a) CLONE_THREAD/CLONE_SIGHAND/CLONE_VM → 要求当前进程是单线程：
 *       这些资源的 unshare 在多线程进程中没有意义（或实现上不安全）。
 *       "pretend it works if there is nothing to unshare"：
 *       若线程组已经是单线程，则即使设置了这些标志，实际上什么都不做也没问题。
 *
 *    b) CLONE_SIGHAND/CLONE_VM → 要求 sighand 引用计数为 1（未被共享）：
 *       若 sighand 被多个进程共享（refcount > 1），unshare SIGHAND 或 VM
 *       需要先确保没有其他进程在使用同一个 sighand。
 *
 *    c) CLONE_VM → 要求当前进程是单线程（current_is_single_threaded）：
 *       地址空间 unshare 比 thread_group_empty 要求更严格，
 *       还需检查没有其他进程通过 ptrace 等方式引用同一 mm。
 */
static int check_unshare_flags(unsigned long unshare_flags)
{
	/* 白名单：只允许这些标志，拒绝任何未知位 */
	if (unshare_flags & ~(CLONE_THREAD|CLONE_FS|CLONE_SIGHAND|
				CLONE_VM|CLONE_FILES|CLONE_SYSVSEM|
				CLONE_NS_ALL | UNSHARE_EMPTY_MNTNS))
		return -EINVAL;
	/*
	 * Not implemented, but pretend it works if there is nothing
	 * to unshare.  Note that unsharing the address space or the
	 * signal handlers also need to unshare the signal queues (aka
	 * CLONE_THREAD).
	 */
	/*
	 * 解除线程/信号处理/地址空间共享时，必须是单线程进程：
	 * 多线程进程的这些 unshare 操作未被实现（或语义不明确）。
	 */
	if (unshare_flags & (CLONE_THREAD | CLONE_SIGHAND | CLONE_VM)) {
		if (!thread_group_empty(current))  /* 有其他线程则拒绝 */
			return -EINVAL;
	}
	/*
	 * 解除信号处理器/地址空间共享时，sighand 必须没有被其他进程引用：
	 * refcount > 1 说明有其他进程（通过 CLONE_SIGHAND）共享此 sighand，
	 * 此时无法安全 unshare。
	 */
	if (unshare_flags & (CLONE_SIGHAND | CLONE_VM)) {
		if (refcount_read(&current->sighand->count) > 1)
			return -EINVAL;
	}
	/*
	 * 解除地址空间共享的更严格检查：
	 * current_is_single_threaded() 不仅检查线程组，还检查 mm->mm_users，
	 * 确保没有任何其他实体（包括 ptrace 附加者）共享此地址空间。
	 */
	if (unshare_flags & CLONE_VM) {
		if (!current_is_single_threaded())
			return -EINVAL;
	}

	return 0;
}

/*
 * Unshare the filesystem structure if it is being shared
 */
/*
 * 【unshare_fs】创建文件系统上下文的独立副本（若需要）
 *
 * fs_struct 包含进程的根目录、当前工作目录和 umask，多线程或通过 CLONE_FS
 * fork 的进程会共享同一个 fs_struct（fs->users > 1）。
 *
 * 快路径优化（fs->users == 1）：
 *   若当前进程是该 fs_struct 的唯一使用者，则无需复制，可以"原地 unshare"。
 *   不需要加锁是因为：即使在读取 users 之后有新线程 fork 并共享此 fs，
 *   最坏情况也只是做了一次"不必要的复制"（useless copy），不会产生错误。
 *   这是一种乐观锁的思路：允许偶尔的冗余工作以避免加锁开销。
 *
 *   例外：若同时要创建新的挂载命名空间（CLONE_NEWNS），即使 users==1，
 *   也必须复制 fs，因为新命名空间需要一个独立的文件系统上下文。
 */
static int unshare_fs(unsigned long unshare_flags, struct fs_struct **new_fsp)
{
	struct fs_struct *fs = current->fs;

	/* 未请求 CLONE_FS 或没有 fs_struct（极少见），直接返回 */
	if (!(unshare_flags & CLONE_FS) || !fs)
		return 0;

	/* don't need lock here; in the worst case we'll do useless copy */
	/*
	 * 快路径：当前进程是 fs 的唯一用户，且不需要新挂载命名空间，
	 * 则 fs 本质上已经是"独立"的，无需复制。
	 */
	if (!(unshare_flags & CLONE_NEWNS) && fs->users == 1)
		return 0;

	/* 创建 fs_struct 的深拷贝（包含根目录和 cwd 的 dentry/vfsmount 引用） */
	*new_fsp = copy_fs_struct(fs);
	if (!*new_fsp)
		return -ENOMEM;

	return 0;
}

/*
 * Unshare file descriptor table if it is being shared
 */
/*
 * 【unshare_fd】创建文件描述符表的独立副本（若需要）
 *
 * files_struct 包含进程的文件描述符表（fd 数组 + close-on-exec 位图）。
 * 多线程进程通过 CLONE_FILES 共享同一个 files_struct（fd->count > 1）。
 *
 * 引用计数检查（fd->count > 1）：
 *   若引用计数为 1，则此进程是 files_struct 的唯一所有者，
 *   无需复制，直接返回 0（与 unshare_fs 的 users==1 优化类似）。
 *
 *   注意：这里使用 atomic_read 读取引用计数，不加锁。
 *   与 unshare_fs 类似，这是乐观读取：偶尔可能做不必要的 dup_fd，但不会出错。
 *
 * dup_fd(fd, NULL)：复制文件描述符表，NULL 表示不进行 close-on-exec 过滤。
 */
static int unshare_fd(unsigned long unshare_flags, struct files_struct **new_fdp)
{
	struct files_struct *fd = current->files;

	if ((unshare_flags & CLONE_FILES) &&
	    (fd && atomic_read(&fd->count) > 1)) {   /* 仅当 fd 被共享时才需要复制 */
		fd = dup_fd(fd, NULL);                    /* 深拷贝文件描述符表 */
		if (IS_ERR(fd))
			return PTR_ERR(fd);
		*new_fdp = fd;   /* 返回新的独立 files_struct，由调用者原子替换 */
	}

	return 0;
}

/*
 * unshare allows a process to 'unshare' part of the process
 * context which was originally shared using clone.  copy_*
 * functions used by kernel_clone() cannot be used here directly
 * because they modify an inactive task_struct that is being
 * constructed. Here we are modifying the current, active,
 * task_struct.
 */
/*
 * 【ksys_unshare】unshare() 系统调用的核心实现
 *
 * unshare() 允许正在运行的进程将通过 clone() 共享的资源独立化，
 * 而无需 fork() 新进程。这是容器技术的基础之一（如 unshare(1) 命令）。
 *
 * 与 kernel_clone() 中的 copy_* 函数的关键区别：
 *   copy_* 函数操作的是正在构建的非活跃 task_struct（子进程）；
 *   而这里操作的是当前正在运行的 current，需要原子替换各资源指针。
 *
 * 资源依赖关系（传递闭包）：
 *   CLONE_NEWUSER → CLONE_THREAD + CLONE_FS（用户命名空间要求独立线程组和 fs）
 *   CLONE_VM      → CLONE_SIGHAND          （地址空间独立需要独立信号处理器）
 *   CLONE_SIGHAND → CLONE_THREAD           （信号处理器独立需要独立线程组）
 *   CLONE_NEWNS   → CLONE_FS              （新挂载命名空间需要独立 fs 上下文）
 *   UNSHARE_EMPTY_MNTNS → CLONE_NEWNS    （空挂载命名空间隐含新挂载命名空间）
 *
 * 原子替换策略（两阶段）：
 *   阶段1（准备）：在 task_lock 之外创建所有新资源（耗时操作）
 *   阶段2（替换）：在 task_lock 保护下原子替换 current 的资源指针
 *   这样最小化了持锁时间，避免长时间阻塞其他等待 task_lock 的操作。
 *
 * 错误标签链（goto 链）：
 *   每个资源分配步骤都有对应的清理标签，确保部分失败时正确释放已分配的资源。
 *   标签顺序与分配顺序相反（LIFO 清理），遵循内核错误处理惯例。
 */
int ksys_unshare(unsigned long unshare_flags)
{
	struct fs_struct *fs, *new_fs = NULL;       /* fs：旧的 fs_struct（用于减引用计数） */
	struct files_struct *new_fd = NULL;
	struct cred *new_cred = NULL;
	struct nsproxy *new_nsproxy = NULL;
	int do_sysvsem = 0;   /* 是否需要处理 SysV 信号量撤销列表 */
	int err;

	/*
	 * If unsharing a user namespace must also unshare the thread group
	 * and unshare the filesystem root and working directories.
	 */
	/*
	 * 资源依赖关系展开：将用户请求的标志扩展为完整的依赖集合。
	 * CLONE_NEWUSER 需要独立线程组（不能与其他进程共享 user ns）
	 * 且需要独立 fs（user ns 改变后，根目录可能需要更新）。
	 */
	if (unshare_flags & CLONE_NEWUSER)
		unshare_flags |= CLONE_THREAD | CLONE_FS;
	/*
	 * If unsharing vm, must also unshare signal handlers.
	 */
	/* 地址空间独立化必须同时独立信号处理器（sighand 内部引用 mm） */
	if (unshare_flags & CLONE_VM)
		unshare_flags |= CLONE_SIGHAND;
	/*
	 * If unsharing a signal handlers, must also unshare the signal queues.
	 */
	/* 信号处理器独立化需要独立线程组（signal queues 与 thread group 绑定） */
	if (unshare_flags & CLONE_SIGHAND)
		unshare_flags |= CLONE_THREAD;
	/*
	 * If unsharing namespace, must also unshare filesystem information.
	 */
	/* 空挂载命名空间需要先创建新的挂载命名空间 */
	if (unshare_flags & UNSHARE_EMPTY_MNTNS)
		unshare_flags |= CLONE_NEWNS;
	/* 新挂载命名空间需要独立的 fs_struct（不同的根目录/cwd 视图） */
	if (unshare_flags & CLONE_NEWNS)
		unshare_flags |= CLONE_FS;

	/* 检查标志合法性和资源状态约束 */
	err = check_unshare_flags(unshare_flags);
	if (err)
		goto bad_unshare_out;
	/*
	 * CLONE_NEWIPC must also detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.
	 */
	/*
	 * 切换 IPC 命名空间或 SYSVSEM 时，必须清理 SysV 信号量撤销记录：
	 * 进入新 IPC 命名空间后，旧命名空间的信号量数组不可访问，
	 * 撤销记录（semundo）若不清理，进程退出时会尝试访问已失效的信号量。
	 */
	if (unshare_flags & (CLONE_NEWIPC|CLONE_SYSVSEM))
		do_sysvsem = 1;

	/*
	 * 阶段1：在持锁之外准备所有新资源（可能耗时，如分配内存、创建命名空间）。
	 * 任何一步失败，通过 goto 链跳转清理已分配的资源。
	 */
	err = unshare_fs(unshare_flags, &new_fs);
	if (err)
		goto bad_unshare_out;
	err = unshare_fd(unshare_flags, &new_fd);
	if (err)
		goto bad_unshare_cleanup_fs;         /* 失败：释放 new_fs */
	err = unshare_userns(unshare_flags, &new_cred);
	if (err)
		goto bad_unshare_cleanup_fd;         /* 失败：释放 new_fd */
	err = unshare_nsproxy_namespaces(unshare_flags, &new_nsproxy,
					 new_cred, new_fs);
	if (err)
		goto bad_unshare_cleanup_cred;       /* 失败：释放 new_cred */
	if (new_cred) {
		/* 更新新 cred 的 rlimit ucounts（用于 RLIMIT_NPROC 统计） */
		err = set_cred_ucounts(new_cred);
		if (err)
			goto bad_unshare_cleanup_nsproxy;  /* 失败：释放 new_nsproxy */
	}

	/* 所有新资源准备就绪，仅在至少有一个资源需要替换时才进入替换阶段 */
	if (new_fs || new_fd || do_sysvsem || new_cred || new_nsproxy) {
		if (do_sysvsem) {
			/*
			 * CLONE_SYSVSEM is equivalent to sys_exit().
			 */
			/* 清理 SysV 信号量撤销记录，语义等同于进程退出时的清理 */
			exit_sem(current);
		}
		if (unshare_flags & CLONE_NEWIPC) {
			/* Orphan segments in old ns (see sem above). */
			/*
			 * 从旧 IPC 命名空间中分离共享内存段：
			 * 进入新命名空间后，旧的 shm 段在新命名空间不可见，
			 * 必须显式"孤立"这些段（减少其附加计数）。
			 */
			exit_shm(current);         /* 从旧命名空间的 shm 分离 */
			shm_init_task(current);    /* 清空 task 的 shm 链表 */
		}

		if (new_nsproxy) {
			/*
			 * 原子替换命名空间代理：switch_task_namespaces 内部会
			 * 将 old_nsproxy 的引用放到 current 的 nsproxy 中，
			 * 并递减旧 nsproxy 的引用计数。
			 * 替换成功后清零 new_nsproxy，防止错误标签链再次 put。
			 */
			switch_task_namespaces(current, new_nsproxy);
			new_nsproxy = NULL;  /* 已转移所有权，不需要在清理标签中释放 */
		}

		/*
		 * 阶段2：在 task_lock 保护下原子替换 fs 和 files。
		 * task_lock 防止 /proc 读取者在替换过程中看到不一致状态。
		 */
		task_lock(current);

		if (new_fs) {
			fs = current->fs;
			/*
			 * read_seqlock_excl：以排他模式获取 seqlock，防止并发读者
			 * 在替换 current->fs 时看到中间状态（如 cwd 路径查找）。
			 */
			read_seqlock_excl(&fs->seq);
			current->fs = new_fs;          /* 原子替换为新的 fs_struct */
			if (--fs->users)
				/*
				 * 旧 fs 仍被其他进程/线程使用（users > 0），
				 * 将 new_fs 设为 NULL，不需要在函数末尾释放旧 fs。
				 */
				new_fs = NULL;
			else
				/*
				 * 旧 fs 引用计数降为 0，不再被任何人使用，
				 * 将 new_fs 指向旧 fs，在函数末尾通过 free_fs_struct 释放。
				 */
				new_fs = fs;
			read_sequnlock_excl(&fs->seq);
		}

		if (new_fd)
			swap(current->files, new_fd);  /* 原子交换 files 指针，new_fd 变为旧的 */

		task_unlock(current);

		if (new_cred) {
			/* Install the new user namespace */
			/*
			 * commit_creds：原子提交新凭据（含新用户命名空间），
			 * 内部处理凭据的内存屏障和通知。
			 * commit_creds 之后 new_cred 的所有权转移，清零避免重复释放。
			 */
			commit_creds(new_cred);
			new_cred = NULL;
		}
	}

	/* 通知 perf 事件子系统命名空间已切换（用于 perf 的命名空间感知采样） */
	perf_event_namespaces(current);

	/*
	 * 错误清理标签链（LIFO 顺序，与分配顺序相反）：
	 * 每个标签只释放对应层级的资源，并顺序执行后续所有标签的清理。
	 * 成功路径：new_* 变量在使用后已被清零，条件判断为假，标签是空操作。
	 */
bad_unshare_cleanup_nsproxy:
	if (new_nsproxy)
		put_nsproxy(new_nsproxy);    /* 释放命名空间代理的引用计数 */
bad_unshare_cleanup_cred:
	if (new_cred)
		put_cred(new_cred);          /* 释放新凭据 */
bad_unshare_cleanup_fd:
	if (new_fd)
		put_files_struct(new_fd);    /* 释放文件描述符表（减引用，可能触发关闭所有 fd） */
bad_unshare_cleanup_fs:
	if (new_fs)
		free_fs_struct(new_fs);      /* 释放 fs_struct（可能是新 fs 或已不再使用的旧 fs） */

bad_unshare_out:
	return err;
}

/*
 * 【sys_unshare】unshare() 系统调用入口
 *
 * 系统调用与实现分离的原因（sys_unshare → ksys_unshare）：
 *   ksys_unshare 可以被内核内部代码直接调用（例如 do_setns 路径），
 *   而无需经过系统调用的参数传递/返回值处理机制。
 *   这是内核代码的通用模式：ksys_xxx 是实现，sys_xxx 是系统调用薄包装。
 *   参见 ksys_read/write/open 等类似模式。
 */
SYSCALL_DEFINE1(unshare, unsigned long, unshare_flags)
{
	return ksys_unshare(unshare_flags);
}

/*
 *	Helper to unshare the files of the current task.
 *	We don't want to expose copy_files internals to
 *	the exec layer of the kernel.
 */
/*
 * 【unshare_files】为当前任务创建独立的文件描述符表
 *
 * 主要服务于 exec 路径（do_execve），在执行新程序前需要确保 files_struct
 * 不被其他线程共享，否则 close-on-exec 处理可能与其他线程竞争。
 *
 * 为什么不直接调用 ksys_unshare(CLONE_FILES)？
 *   exec 路径不应暴露 copy_files/dup_fd 的内部细节，
 *   同时 exec 路径也不需要 ksys_unshare 的完整依赖检查（exec 有自己的约束）。
 *   这里是一个更简单、专用的包装：仅处理 CLONE_FILES 的 unshare。
 *
 * 注意：这里直接操作 task->files（无需检查 thread_group_empty），
 * 因为 exec 路径在调用本函数前已确保了必要的同步。
 */
int unshare_files(void)
{
	struct task_struct *task = current;
	struct files_struct *old, *copy = NULL;
	int error;

	/* 若 files 未被共享（count==1），unshare_fd 不分配 copy，直接返回 0 */
	error = unshare_fd(CLONE_FILES, &copy);
	if (error || !copy)
		return error;

	/* 在 task_lock 保护下原子替换 files 指针 */
	old = task->files;
	task_lock(task);
	task->files = copy;   /* 安装新的独立文件描述符表 */
	task_unlock(task);
	put_files_struct(old);  /* 释放旧的共享 files（减引用计数） */
	return 0;
}

/*
 * 【sysctl_max_threads】/proc/sys/kernel/threads-max 的读写处理函数
 *
 * threads-max 控制系统允许的最大线程数（pid_max 的补充约束）。
 * 直接使用 proc_dointvec_minmax 即可读写，但 sysctl_table 中的 data 指针
 * 指向 NULL（fork_sysctl_table 中 .data = NULL），因此不能直接使用通用处理器。
 *
 * 局部 ctl_table 副本技巧：
 *   原始 sysctl_table 是 const 的，不能修改 .data 字段。
 *   通过栈上复制一份（t = *table），然后设置 t.data 指向局部变量 threads，
 *   同时设置 t.extra1/extra2 为合法范围（[1, MAX_THREADS]）。
 *   这样可以复用 proc_dointvec_minmax 的所有逻辑（边界检查、格式化输出等），
 *   只是把 I/O 对象换成了局部变量，成功写入后再更新全局变量 max_threads。
 *
 * 为什么不直接在 sysctl_table 中设置 .data = &max_threads？
 *   因为需要额外的验证逻辑（min/max 边界），而且 extra1/extra2
 *   需要指向变量（而非常量），在全局静态表中这很难处理。
 *   另外，const sysctl_table 无法被修改，栈上副本是标准解法。
 */
static int sysctl_max_threads(const struct ctl_table *table, int write,
		       void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;             /* 局部副本，用于设置可变字段 */
	int ret;
	int threads = max_threads;      /* 读取当前值到局部变量 */
	int min = 1;                    /* 最小值：至少要允许 1 个线程 */
	int max = MAX_THREADS;          /* 最大值：由内存大小和 PID 命名空间决定 */

	t = *table;                     /* 复制原始 sysctl_table 的所有字段 */
	t.data = &threads;              /* 重定向数据指针到局部变量 */
	t.extra1 = &min;                /* 设置合法范围下界 */
	t.extra2 = &max;                /* 设置合法范围上界 */

	/* 使用通用整数 sysctl 处理函数（读：格式化输出；写：解析并验证范围） */
	ret = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;               /* 读操作或错误：直接返回 */

	/* 写操作成功，将验证后的值写回全局变量 */
	max_threads = threads;

	return 0;
}

static const struct ctl_table fork_sysctl_table[] = {
	{
		.procname	= "threads-max",
		.data		= NULL,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sysctl_max_threads,
	},
};

/*
 * 【init_fork_sysctl】注册 fork 相关 sysctl 条目
 *
 * 使用 subsys_initcall 而非 core_initcall 或 device_initcall：
 *   subsys_initcall 在内核初始化的"子系统"阶段执行，晚于 core_initcall
 *   但早于设备初始化。此时 /proc/sys 已经挂载，sysctl 框架已初始化，
 *   可以安全注册新的 sysctl 条目。
 *
 *   为什么不在 proc_caches_init() 中直接注册？
 *   proc_caches_init 在 mm_init() 中被调用（早于 subsys_initcall），
 *   那时 sysctl 框架还未完全初始化，强行注册可能失败或引发内核 panic。
 *   将注册推迟到 subsys_initcall 阶段是安全的做法。
 *
 * register_sysctl_init：在 /proc/sys/kernel/ 目录下注册 fork_sysctl_table 中的条目，
 *   注册失败时内核会 panic（SLAB_PANIC 语义），因为 threads-max 是关键内核参数。
 */
static int __init init_fork_sysctl(void)
{
	register_sysctl_init("kernel", fork_sysctl_table);  /* 注册到 /proc/sys/kernel/ */
	return 0;
}

/* subsys_initcall：在子系统初始化阶段执行，早于设备初始化，晚于核心初始化 */
subsys_initcall(init_fork_sysctl);
