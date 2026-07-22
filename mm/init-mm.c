// SPDX-License-Identifier: GPL-2.0
/*
 * init_mm 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。该标识只说明新增中文注释的来源，
 * 不属于上游作者或版权信息。
 *
 * 【文件职责】
 * 本文件静态构造全系统唯一的 init_mm，并在体系结构完成早期地址空间布局后记录
 * 内核映像的代码、数据和 brk 边界。普通用户进程通过 fork/exec 动态分配 mm_struct；
 * init_mm 不代表某个用户进程，而是内核地址空间的公共描述符和永久锚点：
 *
 *   init_task.mm        = NULL        内核线程没有用户地址空间
 *   init_task.active_mm = &init_mm    CPU 仍需一个可装载的内核页表上下文
 *   init_mm.pgd         = swapper_pg_dir
 *                                    指向体系结构建立的内核顶级页表
 *
 * 后续内核线程也可能在 lazy-TLB 协议中临时借用用户 mm；需要强制退出该借用时，
 * 会切回 init_mm。因此 init_mm 既是启动期对象，也是运行期永不释放的安全落点。
 *
 * 【主调用链】
 *   start_kernel()
 *     -> setup_arch()
 *        -> setup_initial_init_mm(_text, _etext, _edata, _end/_brk_end)
 *           -> 填充 init_mm 的内核映像边界
 *
 *   内核页表建立/修改路径
 *     -> 以 &init_mm 传给 set_pte_at()/pmd_populate()/page-table walker
 *     -> 使用 init_mm.pgd、page_table_lock 和体系结构 context 表达内核地址空间
 *
 * 【生命周期与并发】
 * init_mm 具有静态存储期，在任何普通 mm 分配之前存在，且 __mmdrop() 明确禁止释放它。
 * 字段不是全部只读：内核页表和部分记账仍会在运行期变化，因此 mmap_lock、
 * page_table_lock、arg_lock、seqcount 等同步对象必须和动态 mm 一样正确初始化。
 *
 * 【方案权衡】
 * 复用 mm_struct 让页表、TLB、vmalloc 和 page-walk 代码无需为“内核地址空间”维护
 * 第二套接口；代价是 init_mm 的引用计数和 VMA 语义具有特殊性，不能机械套用普通
 * 用户 mm 的创建、退出和释放模型。
 */
#include <linux/mm_types.h>
#include <linux/maple_tree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/mman.h>
#include <linux/pgtable.h>

#include <linux/atomic.h>
#include <linux/user_namespace.h>
#include <linux/iommu.h>
#include <asm/mmu.h>

#ifndef INIT_MM_CONTEXT
#define INIT_MM_CONTEXT(name)
#endif

/*
 * INIT_MM_CONTEXT 是体系结构扩展点：需要 ASID、上下文锁或架构私有状态的架构在
 * <asm/mmu.h> 中把它展开为指定初始化器；没有特殊状态的架构使用这里的空定义。
 * @name 是正在构造的 mm_struct 名称，使宏可生成自引用锁初始化器。条件回退让通用
 * init_mm 初始化表不必散布各架构的 #ifdef。
 */

const struct vm_operations_struct vma_dummy_vm_ops;

/*
 * vma_dummy_vm_ops 是全零、静态存储期的“禁止继续调用”操作表。VMA 建立失败、文件
 * mmap 回调失败或 close 回调执行后，MM 代码把 vm_ops 指向它，从而让对象仍持有一个
 * 稳定非 NULL 指针，却不会再次进入已失效的文件/驱动回调。它不拥有资源、没有状态，
 * 也无需释放；其地址身份本身就是哨兵。
 */

/*
 * For dynamically allocated mm_structs, there is a dynamically sized cpumask
 * at the end of the structure, the size of which depends on the maximum CPU
 * number the system can see. That way we allocate only as much memory for
 * mm_cpumask() as needed for the hundreds, or thousands of processes that
 * a system typically runs.
 *
 * Since there is only one init_mm in the entire system, keep it simple
 * and size this cpu_bitmask to NR_CPUS.
 */
/*
 * 原注释说明了 flexible_array 的特殊分配策略：动态 mm_struct 会在对象尾部按
 * nr_cpu_ids 追加恰好所需的 CPU 位图等数据，避免每个进程都为 NR_CPUS 上限付费；
 * init_mm 只有一个且由静态对象承载，因此直接预留完整 cpumask_t 和静态 MM-CID
 * 空间，换取初始化简单且启动早期无需分配器。
 *
 * init_mm 字段地图：
 *   mm_mt              内核地址空间的 VMA Maple Tree 根；使用外部 mmap_lock。
 *   pgd                内核顶级页表 swapper_pg_dir，体系结构启动代码提前建立。
 *   mm_users/mm_count  两层 mm 生命周期计数的永久启动基线；init_mm 不走归零释放路径。
 *   write_protect_seq  页表批量写保护/COW 协议的序列号，初始无写者。
 *   mmap_lock          VMA 布局的睡眠型读写锁，也是 mm_mt 声明的外部锁。
 *   page_table_lock    页表和相关计数的短临界区自旋锁。
 *   arg_lock           保护代码/数据、参数和环境地址边界字段。
 *   mmlist             全局可换出 mm 链表的哨兵表头，init_mm 自身充当链表根。
 *   flexible_array     mm_cpumask/MM-CID 所需的固定尾部存储。
 *   context            由 INIT_MM_CONTEXT 提供的体系结构私有 MMU 上下文。
 */
struct mm_struct init_mm = {
	/*
	 * Maple Tree 保存按虚拟地址索引的 VMA；MM_MT_FLAGS 启用区间分配、RCU 读语义并声明
	 * 锁由外部 mmap_lock 管理。MTREE_INIT_EXT 还在 LOCKDEP 配置下关联该外部锁的 dep_map。
	 */
	.mm_mt		= MTREE_INIT_EXT(mm_mt, MM_MT_FLAGS, init_mm.mmap_lock),
	/* swapper_pg_dir 是链接/架构早期启动阶段建立的内核顶级页表，不由 init_mm 分配。 */
	.pgd		= swapper_pg_dir,
	/*
	 * mm_users 表示用户级使用者，mm_count 表示包含 mm_users 这一个聚合引用在内的结构
	 * 引用。这里的 2/1 是 init_mm 的永久启动基线，不应拿普通进程“最后一个用户退出”
	 * 的规则推导其销毁；__mmdrop() 会 BUG_ON(mm == &init_mm)。
	 */
	.mm_users	= ATOMIC_INIT(2),
	.mm_count	= ATOMIC_INIT(1),
	/* 零序列表示当前没有 fork 等路径正在批量写保护本地址空间的页表。 */
	.write_protect_seq = SEQCNT_ZERO(init_mm.write_protect_seq),
	/* 静态初始化可睡眠的 mmap_lock；宏末尾自带字段分隔逗号。 */
	MMAP_LOCK_INITIALIZER(init_mm)
	/* 下列两把自旋锁分别保护页表状态和可执行映像/参数边界，初始均为 unlocked。 */
	.page_table_lock =  __SPIN_LOCK_UNLOCKED(init_mm.page_table_lock),
	.arg_lock	=  __SPIN_LOCK_UNLOCKED(init_mm.arg_lock),
	/* 自指向空链表既代表“当前没有可换出 mm”，又作为所有 mmlist 节点的全局表头。 */
	.mmlist		= LIST_HEAD_INIT(init_mm.mmlist),
#ifdef CONFIG_PER_VMA_LOCK
	/*
	 * vma_writer_wait 供排他 VMA 写者等待无锁读者退出；mm_lock_seq 从偶数 0 开始，
	 * 表示 mmap_lock 当前没有写持有者，并为每个 VMA 的锁代际提供共同基准。
	 */
	.vma_writer_wait = __RCUWAIT_INITIALIZER(init_mm.vma_writer_wait),
	.mm_lock_seq	= SEQCNT_ZERO(init_mm.mm_lock_seq),
#endif
#ifdef CONFIG_SCHED_MM_CID
	/* 调度器 MM-CID 子对象有自己的 raw spinlock；静态初始化避免启动期锁未就绪。 */
	.mm_cid.lock = __RAW_SPIN_LOCK_UNLOCKED(init_mm.mm_cid.lock),
#endif
	/* 为完整 NR_CPUS 位图及配置相关的静态 MM-CID 数据提供清零后的对象尾部空间。 */
	.flexible_array	= MM_STRUCT_FLEXIBLE_ARRAY_INIT,
	/* 宏可能展开一个或多个指定初始化字段，也可能在本架构为空；它必须位于初始化表内。 */
	INIT_MM_CONTEXT(init_mm)
};

/*
 * setup_initial_init_mm - 记录内核映像在 init_mm 中的虚拟地址边界
 *
 * 调用关系：各体系结构的 setup_arch() 在识别最终内核映像布局后调用；/proc、调试、
 * core-dump/内存统计等通用代码随后可按普通 mm_struct 字段读取这些边界。
 *
 * 入参：
 *   @start_code 内核可执行代码起始虚拟地址，通常来自 _text/_stext 链接符号；可按架构
 *               约定取 PAGE_OFFSET，调用期间仅借用，不解引用。
 *   @end_code   代码末端虚拟地址，通常为 _etext；与 start_code 形成半开代码区间。
 *   @end_data   已初始化数据末端虚拟地址，通常为 _edata。
 *   @brk        内核静态映像/早期 brk 末端，通常为 _end 或架构的 _brk_end；个别 NOMMU
 *               架构可传 NULL，转换后以 0 表示没有该边界。
 *
 * 前置条件：启动 CPU 正在单线程执行架构初始化，地址布局和 KASLR 重定位已经确定；
 * 此时无需取得 arg_lock。四个指针是地址数值载体，不转移所有权，也不会被解引用。
 *
 * 出参/副作用：无返回值；覆盖全局 init_mm 的 start_code、end_code、end_data 和 brk。
 * 函数不建立 VMA、不修改页表，也不推导 start_data/start_brk；调用者必须传入本架构
 * 已经确认的链接/启动边界。
 */
void setup_initial_init_mm(void *start_code, void *end_code,
			   void *end_data, void *brk)
{
	/*
	 * unsigned long 是 mm_struct 地址边界字段的规范表示。这里转换的是内核虚拟地址本身，
	 * 不是读取指针所指内容；写入顺序没有发布协议，因为并发读者尚未启动。
	 */
	init_mm.start_code = (unsigned long)start_code;
	init_mm.end_code = (unsigned long)end_code;
	init_mm.end_data = (unsigned long)end_data;
	init_mm.brk = (unsigned long)brk;
}
