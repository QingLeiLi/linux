// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/memblock.h>
#include <linux/stacktrace.h>
/* stacktrace 与 stackdepot 组合把分配调用链压缩为可长期保存的 handle。 */
#include <linux/page_owner.h>
#include <linux/jump_label.h>
#include <linux/migrate.h>
#include <linux/stackdepot.h>
#include <linux/seq_file.h>
#include <linux/memcontrol.h>
#include <linux/sched/clock.h>
/* 本文件在普通页分配热路径外保存来源，依赖不改变 allocator 的行为。 */

/* 依赖分别提供 page_ext、stack depot、memcg、迁移类型与 debugfs/seq_file 诊断接口。 */
#include "internal.h"

/*
 * TODO: teach PAGE_OWNER_STACK_DEPTH (__dump_page_owner and save_stack)
 * to use off stack temporal storage
 */
/* 补充说明：当前固定深度限制诊断栈的内存与可读性，尚未支持脱栈时间存储。 */
#define PAGE_OWNER_STACK_DEPTH (16)

/*
 * 每个 page_ext 携带的页面分配历史：allocation/free 的 stack depot 句柄、时间、任务身份、
 * GFP 与迁移原因。它随 page_ext 创建/销毁；更新在 page_ext/RCU 协议下进行，读者只取快照。
 */
struct page_owner {
	/* order 说明一次 allocation 覆盖 2^order 个基础页；-1 原因表示从未迁移。 */
	unsigned short order;
	short last_migrate_reason;
	/* gfp_mask 与 allocation stack handle 共同回答“以何种约束从哪里分配”。 */
	gfp_t gfp_mask;
	depot_stack_handle_t handle;
	/* free_handle 为最近一次释放栈；0 表示尚无可打印释放栈。 */
	depot_stack_handle_t free_handle;
	/* 时间戳采用 local_clock，适合本机时间排序而非跨 CPU 的全局时钟比较。 */
	u64 ts_nsec;
	u64 free_ts_nsec;
	/* comm/pid/tgid 是分配瞬间值；free_pid/free_tgid 是释放路径写入的任务快照。 */
	char comm[TASK_COMM_LEN];
	pid_t pid;
	pid_t tgid;
	pid_t free_pid;
	pid_t free_tgid;
};

/* order 以二为底表示覆盖的基础页数；handle/free_handle 是 stack depot 的值句柄。 */

/* stack_list 的节点把 stack depot record 串成 debugfs 可遍历集合；next 由 release/acquire 发布。 */
struct stack {
	/* stack_record 由 stack depot 长期拥有，本节点只借用；next 构成只增不删的发布链。 */
	struct stack_record *stack_record;
	struct stack *next;
};

/* dummy/failure 是静态首节点；动态节点从 stack_list 头插，stack_list_lock 只保护写侧。 */
static struct stack dummy_stack;
static struct stack failure_stack;
static struct stack *stack_list;
static DEFINE_SPINLOCK(stack_list_lock);

/* list lock 保护插入；读者以发布语义遍历初始化完成后永不删除的节点。 */

/* 输出符号化栈帧；show_stacks 与 show_stacks_handles 使用。 */
#define STACK_PRINT_FLAG_STACK		0x1
/* 输出当前基础页数并启用 threshold 过滤；show_stacks/show_handles 使用。 */
#define STACK_PRINT_FLAG_PAGES		0x2
/* 输出 depot 稳定 handle，便于与逐页记录关联。 */
#define STACK_PRINT_FLAG_HANDLE		0x4

struct stack_print_ctx {
	/* stack 保存下一次 seq 迭代位置；flags 决定输出 stack/pages/handle 的组合。 */
	struct stack *stack;
	u8 flags;
};

/* flags 由 debugfs inode 私有值传入，决定同一迭代器的输出列。 */

static bool page_owner_enabled __initdata;
/* static key 关闭时热路径近似无开销；init 完成全部 metadata 后才切换为 true。 */
DEFINE_STATIC_KEY_FALSE(page_owner_inited);

static depot_stack_handle_t dummy_handle;
static depot_stack_handle_t failure_handle;
static depot_stack_handle_t early_handle;

/* 三个特殊 handle 分别标识重入、depot 保存失败和常规记录前的启动期页面。 */

static void init_early_allocated_pages(void);

/*
 * page_owner 在保存栈时可能自身触发分配；置位 current 标志使递归调用改用 dummy stack。
 * 无入参/返回，副作用仅限当前任务，必须由 unset 配对，否则后续真实分配会永久失去栈记录。
 */
/*
 * 业务背景：save_stack() 可能由 stack depot 补池再次进入页分配，必须在递归发生前给当前执行流
 * 设置哨兵，使内层 page_owner 直接使用 dummy_handle 而不继续抓栈。
 * 入参：无；隐式操作 current，要求存在可写的当前任务上下文。
 * 出参/返回：无直接返回值；把 current->in_page_owner 置 1，不获取引用或锁。
 * 注意事项：不睡眠且不是跨 CPU 互斥；只允许由严格配对的 unset 清除。提前返回、异常嵌套或
 * 在置位期间切换到不配对路径都会让该任务后续记录退化为 dummy stack。
 */
static inline void set_current_in_page_owner(void)
{
	/*
	 * Avoid recursion.
	 *
	 * We might need to allocate more memory from page_owner code, so make
	 * sure to signal it in order to avoid recursion.
	 */
	/* 补充说明：此标志是每任务重入哨兵，不是跨 CPU 的互斥锁。 */
	current->in_page_owner = 1;
}

/* 清除本任务的重入哨兵；只在对应 set 成功后的退出路径调用，无返回值。 */
/*
 * 业务背景：结束可能递归的 stack depot/索引分配阶段，使当前任务后续分配恢复真实栈采集。
 * 入参：无；隐式操作 current。
 * 出参/返回：无直接返回值；把 current->in_page_owner 清零，无 ownership 副作用。
 * 注意事项：不睡眠；不得在没有对应 set 时调用，也不能早于最后一个可能递归的操作清除。
 */
static inline void unset_current_in_page_owner(void)
{
	current->in_page_owner = 0;
}

/*
 * early_param 解析 page_owner= 启动参数并在启用时请求 stack depot 的早期初始化。
 * buf 是内核借用字符串；成功返回 0，非法布尔值返回 errno。仅 __init 阶段可调用。
 */
/*
 * 业务背景：page_ext 大小和 stack depot 准备时机必须在早期启动确定，本参数处理器把用户
 * page_owner= 开关转换为两个子系统都能在普通分配器启用前消费的状态。
 * 入参：buf 是非空、NUL 结尾的借用启动参数值，只读且仅在 early_param 调用期间有效。
 * 出参/返回：合法布尔值返回 0 并更新 page_owner_enabled；非法文本返回 kstrtobool errno；
 * 启用时额外请求 stack depot early init，关闭时不请求资源。
 * 注意事项：仅 __init 单线程阶段调用且可执行启动初始化请求；返回错误时不得把功能视为启用。
 */
static int __init early_page_owner_param(char *buf)
{
	int ret = kstrtobool(buf, &page_owner_enabled);

	/* 必须在普通页分配可记录栈以前请求 depot，以覆盖启动期分配。 */
	if (page_owner_enabled)
		stack_depot_request_early_init();

	return ret;
}
early_param("page_owner", early_page_owner_param);

/* page_ext 框架询问是否应为每页分配 page_owner 私有区；返回启动参数快照。 */
/*
 * 业务背景：page_ext 在计算每页扩展布局时调用，避免 page_owner 关闭仍为所有页付出内存成本。
 * 入参：无。
 * 出参/返回：返回 early_param 保存的布尔开关；无输出参数和副作用。
 * 注意事项：仅 __init 布局阶段读取 __initdata，运行期不得再调用或保存该变量地址。
 */
static __init bool need_page_owner(void)
{
	return page_owner_enabled;
}

/*
 * 生成一个可识别的内部调用栈，供递归、失败和早期页使用。无入参；返回 depot 句柄，
 * 0 表示保存失败。可分配，调用者只能在 page_owner 初始化阶段使用其结果。
 */
/*
 * 业务背景：特殊状态也需要能在输出中区分的稳定 handle；通过三个 noinline 包装调用本 helper，
 * 可让相同采集代码得到三条不同顶层栈。
 * 入参：无。
 * 出参/返回：成功返回 stack depot 值句柄，失败返回 0；entries 只作本栈帧临时输入，depot 复制内容。
 * 注意事项：GFP_KERNEL 可能睡眠，只能在启动可睡眠上下文调用；返回句柄本身不带 GET 引用。
 */
static __always_inline depot_stack_handle_t create_dummy_stack(void)
{
	unsigned long entries[4];
	unsigned int nr_entries;

	nr_entries = stack_trace_save(entries, ARRAY_SIZE(entries), 0);
	return stack_depot_save(entries, nr_entries, GFP_KERNEL);
}

/* 注册递归保护用的 dummy 栈；无入参/返回，结果写入全局 dummy_handle。 */
/*
 * 业务背景：独立 noinline 帧让递归样本在 stack depot 中可识别为 dummy 类别。
 * 入参：无。出参/返回：无直接返回；写全局 dummy_handle，可能为 0。
 * 注意事项：仅 init、可睡眠且单次调用；必须早于 static key 发布和任何递归记录。
 */
static noinline void register_dummy_stack(void)
{
	dummy_handle = create_dummy_stack();
}

/* 注册 stack_depot 保存失败时的替代栈，避免 handle 为 0 失去诊断归因。 */
/*
 * 业务背景：真实 allocation stack 保存失败时仍需明确归因，而非让 0 被当成“未记录”。
 * 入参：无。出参/返回：无直接返回；写全局 failure_handle，底层失败时仍可能为 0。
 * 注意事项：仅启动可睡眠上下文单次调用；其 stack_record 稍后以静态节点发布。
 */
static noinline void register_failure_stack(void)
{
	failure_handle = create_dummy_stack();
}

/* 注册启动早期统一使用的栈；其引用计数尚未准备好，释放路径会专门跳过它。 */
/*
 * 业务背景：page_owner 正式启用前已经占用的页面没有真实分配栈，统一以 early_handle 标记。
 * 入参：无。出参/返回：无直接返回；写全局 early_handle，底层失败可为 0。
 * 注意事项：只在 init 调用；早期页不会增加 record count，reset 必须识别并跳过对应减计数。
 */
static noinline void register_early_stack(void)
{
	early_handle = create_dummy_stack();
}

/*
 * page_ext 初始化回调：仅在参数开启时注册三种特殊栈、标记早期页并发布 static key。
 * 无入参/返回；运行于启动阶段且可分配。static_branch_enable 是功能对外可见的发布点，
 * 之前 dummy/failure stack_list 必须已经完整构造。
 */
/*
 * 业务背景：page_ext 分配完成后，本回调把特殊 handle、启动早期页面归因和 stack 聚合链表
 * 一次性准备完整，最后才打开热路径 static key，避免 allocator 观察半初始化状态。
 * 入参：无；读取 page_owner_enabled 和已建立的 page_ext/stack depot 基础设施。
 * 出参/返回：无直接返回；关闭时无副作用；启用时初始化三个特殊 handle、扫描早期页、建立
 * dummy→failure 静态链并发布 page_owner_inited。特殊 record 存在时 count 从饱和值改为 1。
 * 注意事项：仅 __init 可睡眠上下文调用一次；任何 handle 为 0 时对应 record 可为 NULL，链表
 * 节点仍安全但 show 会跳过。static key 必须最后启用，之后普通分配路径可并发读取全部状态。
 */
static __init void init_page_owner(void)
{
	/* 未请求功能时不分配额外 metadata，page_ext 不会调用记录路径。 */
	if (!page_owner_enabled)
		return;

	register_dummy_stack();
	register_failure_stack();
	register_early_stack();
	init_early_allocated_pages();
	/* Initialize dummy and failure stacks and link them to stack_list */
	/* 补充说明：把特殊栈放入同一列表，debugfs 汇总不会遗漏递归/失败样本。 */
	dummy_stack.stack_record = __stack_depot_get_stack_record(dummy_handle);
	failure_stack.stack_record = __stack_depot_get_stack_record(failure_handle);
	if (dummy_stack.stack_record)
		refcount_set(&dummy_stack.stack_record->count, 1);
	if (failure_stack.stack_record)
		refcount_set(&failure_stack.stack_record->count, 1);
	dummy_stack.next = &failure_stack;
	stack_list = &dummy_stack;
	static_branch_enable(&page_owner_inited);
}

/* page_ext 的操作表把本文件接入每页扩展空间创建、启用判断和启动初始化。 */
struct page_ext_operations page_owner_ops = {
	/* 每个 page_ext 为 page_owner 预留完整私有记录大小。 */
	.size = sizeof(struct page_owner),
	/* 启动参数关闭时不为本客户端扩展布局。 */
	.need = need_page_owner,
	/* page_ext backing 建立后再执行特殊 handle、早期扫描和 static key 发布。 */
	.init = init_page_owner,
	/* PAGE_EXT_OWNER/ALLOCATED 位存放在共享 page_ext->flags 中。 */
	.need_shared_flags = true,
};

/* 从稳定的 page_ext 偏移恢复本模块私有记录；借用返回值不得越过 page_ext_put。 */
/*
 * 业务背景：多个 page_ext 客户端共享同一 backing，本 helper 用初始化时分配的 offset 定位
 * page_owner 私有区域。
 * 入参：page_ext 是非空且已由 RCU、page_ext_get 或启动期不变量稳定的借用扩展对象。
 * 出参/返回：返回其内嵌 page_owner 的借用指针；不增加引用、不修改字段。
 * 注意事项：纯地址换算且不睡眠；返回值的有效期绝不强于输入，page_ext_put() 后不得继续解引用。
 */
static inline struct page_owner *get_page_owner(struct page_ext *page_ext)
{
	return page_ext_data(page_ext, &page_owner_ops);
}

/*
 * 保存当前分配调用链并返回 depot 句柄。flags 传给可能分配的 depot；成功为有效句柄，
 * 失败改用 failure_handle。重入时直接返回 dummy_handle，保证不会无限递归。
 * set/unset 包围所有可能触发分配的工作，故每个成功 set 必须在返回前清除标志。
 */
/*
 * 业务背景：每次分配需要把调用链压缩成可长期保存的 depot handle；本函数同时处理 page_owner
 * 自身递归和 depot 空间不足，使调用者始终得到可解释的特殊类别而非再次进入分配器。
 * 入参：flags 是原分配的 GFP 位图，纯输入并传给 stack_depot_save 约束其补池行为。
 * 出参/返回：递归返回 dummy_handle；正常保存成功返回 depot handle；保存失败返回
 * failure_handle。无 GET 引用转移，后续页面计数由 inc_stack_record_count 单独维护。
 * 注意事项：非递归路径在 current 上设置哨兵并必须清除；stack_trace_save 跳过两帧。是否可睡眠
 * 由 flags 决定，调用者必须传入与当前分配上下文相容的 GFP，handle 为 0 仍需被上层容错。
 */
static noinline depot_stack_handle_t save_stack(gfp_t flags)
{
	unsigned long entries[PAGE_OWNER_STACK_DEPTH];
	depot_stack_handle_t handle;
	unsigned int nr_entries;

	/* page_owner 自己分配时不能再次抓栈，否则会在 allocator 中递归。 */
	if (current->in_page_owner)
		return dummy_handle;

	set_current_in_page_owner();
	nr_entries = stack_trace_save(entries, ARRAY_SIZE(entries), 2);
	/* 跳过本 helper/调用点两帧，让输出从真正的分配者开始。 */
	handle = stack_depot_save(entries, nr_entries, flags);
	if (!handle)
		handle = failure_handle;
	unset_current_in_page_owner();

	return handle;
}

/*
 * 首次看到一个 depot record 时为它创建可遍历节点。stack_record 是 depot 借用记录，
 * gfp_mask 决定能否安全自旋/分配；无返回。不能自旋或 kmalloc 失败时仅放弃 debugfs 索引，
 * 不影响原有 record 计数。release store 与 stack_start 的 acquire load 形成发布配对。
 */
/*
 * 业务背景：stack depot 能按 handle 找栈，但 debugfs 聚合还需要遍历“page_owner 曾使用过的
 * record”；首次计数者在此为 record 建立只增不删的索引节点。
 * 入参：stack_record 是非空、由 depot 长期拥有的借用记录；gfp_mask 是当前分配上下文约束，
 * 用于判断能否自旋并派生诊断节点分配掩码。
 * 出参/返回：无直接返回；允许自旋且 kmalloc 成功时，新节点 ownership 转给永久 stack_list；
 * 不允许或失败时无链表副作用，record 的页面计数仍由调用者维护。
 * 注意事项：动态分配期间设置 current 重入哨兵并在所有出口清除；spin_lock_irqsave 只覆盖头插，
 * smp_store_release 与 stack_start 的 acquire 配对发布 stack_record/next。节点运行期不删除。
 */
static void add_stack_record_to_list(struct stack_record *stack_record,
				     gfp_t gfp_mask)
{
	unsigned long flags;
	struct stack *stack;
	/* flags 保存并恢复本 CPU 中断状态；stack 是成功后永久交给全局只增链表的新节点。 */

	/* 原子/不可睡眠上下文不能为了诊断节点引入更强的分配约束。 */
	if (!gfpflags_allow_spinning(gfp_mask))
		return;

	set_current_in_page_owner();
	/* kmalloc 继承受限 GFP；失败只丢失聚合视图并清除重入哨兵。 */
	stack = kmalloc_obj(*stack, gfp_nested_mask(gfp_mask));
	if (!stack) {
		unset_current_in_page_owner();
		return;
	}
	unset_current_in_page_owner();

	stack->stack_record = stack_record;
	stack->next = NULL;

	/* 锁保护链表插入；release store 还保证新节点字段先于读者可见。 */
	spin_lock_irqsave(&stack_list_lock, flags);
	stack->next = stack_list;
	/*
	 * This pairs with smp_load_acquire() from function
	 * stack_start(). This guarantees that stack_start()
	 * will see an updated stack_list before starting to
	 * traverse the list.
	 */
	/*
	 * 该 release store 与 stack_start() 的 acquire load 配对，保证读者在取得新表头后，
	 * 也能看到节点已经初始化完成的 stack_record 与 next，而不是半构造对象。
	 */
	smp_store_release(&stack_list, stack);
	spin_unlock_irqrestore(&stack_list_lock, flags);
}

/*
 * 按分配的基础页数增加栈记录引用计数，并把第一次可用 record 加入 stack_list。
 * handle 是值句柄，gfp_mask 用于索引分配，nr_base_pages 是 2^order；无返回。
 * saturated 是 stack depot 对未 GET record 的防误增哨兵，cmpxchg 成功者独占建链责任。
 */
/*
 * 业务背景：page_owner 不使用 STACK_DEPOT_FLAG_GET，却要按仍归因于某栈的基础页数做聚合；
 * 因而把 depot 的饱和哨兵转换为“1 个链表基准引用 + N 个页面计数”。
 * 入参：handle 是可为 0/无效的值句柄；gfp_mask 约束首次建链分配；nr_base_pages 是正的
 * 基础页数量，通常为 2^order。
 * 出参/返回：无直接返回；无 record 时无副作用。首次成功 cmpxchg 的并发者把 count 设 1
 * 并尝试建链，随后所有调用者各增加 nr_base_pages；不转移 handle ownership。
 * 注意事项：relaxed cmpxchg 只负责唯一初始化，链表字段发布由独立 release store 保证；即使
 * 节点分配失败，count 仍会增加且不会再次从 1 触发建链，所以聚合列表可能永久漏掉该 record。
 */
static void inc_stack_record_count(depot_stack_handle_t handle, gfp_t gfp_mask,
				   int nr_base_pages)
{
	struct stack_record *stack_record = __stack_depot_get_stack_record(handle);
	/* stack_record 由 depot 拥有；只有其 count 非零且未回收时当前内部接口的裸指针才可解释。 */

	if (!stack_record)
		return;

	/*
	 * New stack_record's that do not use STACK_DEPOT_FLAG_GET start
	 * with REFCOUNT_SATURATED to catch spurious increments of their
	 * refcount.
	 * Since we do not use STACK_DEPOT_FLAG_GET API, let us
	 * set a refcount of 1 ourselves.
	 */
	/* 只有一个并发分配者将饱和哨兵改为 1 并创建列表节点。 */
	if (refcount_read(&stack_record->count) == REFCOUNT_SATURATED) {
		int old = REFCOUNT_SATURATED;
		/* old 同时是 cmpxchg 期望值和失败时返回的当前值，仅用于竞争判定。 */

		if (atomic_try_cmpxchg_relaxed(&stack_record->count.refs, &old, 1))
			/* Add the new stack_record to our list */
			/* 当前线程赢得首次初始化竞争，尝试把新 stack_record 加入聚合链表。 */
			add_stack_record_to_list(stack_record, gfp_mask);
	}
	refcount_add(nr_base_pages, &stack_record->count);
}

/*
 * 释放页时撤销其基础页数对 allocation stack 的引用。handle 为值，nr_base_pages 为数量；
 * 无返回。归零是异常，因为历史 record 理应仍保留计数，故只告警而不试图释放 depot 对象。
 */
/*
 * 业务背景：高阶分配释放后要从对应栈的“仍占用基础页数”中扣回 2^order，但保留建链时的
 * 一个基准引用，使 stack_record/stack_list 节点可供历史聚合持续读取。
 * 入参：handle 是 allocation 时保存的值句柄；nr_base_pages 是正的待扣基础页数。
 * 出参/返回：无直接返回；有效 record 的 count 原子减少，无效 handle 无副作用；意外减到零
 * 仅打印警告，不在此释放或摘链。
 * 注意事项：必须与此前 inc 精确配对；early_handle 从未 inc，调用者须在外层跳过。下溢或归零
 * 表示账本错误，警告不能修复丢失的引用关系。
 */
static void dec_stack_record_count(depot_stack_handle_t handle,
				   int nr_base_pages)
{
	struct stack_record *stack_record = __stack_depot_get_stack_record(handle);

	if (!stack_record)
		return;

	if (refcount_sub_and_test(nr_base_pages, &stack_record->count))
		pr_warn("%s: refcount went to 0 for %u handle\n", __func__,
			handle);
}

/*
 * 将一次 allocation/migration 的来源写入范围内每个 page_ext。page 借用且 order 指定
 * 连续基础页数；handle、GFP、原因、时间和任务快照成为输出字段，无返回。
 * RCU 保护 page_ext 迭代窗口；设置 OWNER/ALLOCATED 位是读者看到“当前已分配”的发布状态。
 */
/*
 * 业务背景：单次高阶 allocation 的所有基础页都应能独立从 PFN 查到同一来源，本 helper
 * 将调用者准备好的不可睡眠快照复制到每个 page_ext，并在字段写完后设置有效/已分配标志。
 * 入参：page 是非空借用首页；order 为 [0, MAX_PAGE_ORDER] 的二进制阶；handle 是 depot
 * 值句柄；gfp_mask、last_migrate_reason、ts_nsec、pid、tgid、comm 是纯输入快照，ts_nsec
 * 单位纳秒，comm 非空且最多复制 TASK_COMM_LEN。
 * 出参/返回：无直接返回；对存在的 2^order 个 page_ext 写相同 owner 字段并置 OWNER 与
 * OWNER_ALLOCATED；不改变 stack record count，也不取得 page/handle/task ownership。
 * 注意事项：显式 RCU 读锁要求函数内不睡眠；字段没有整体 seqlock，诊断读者只能得到尽力快照。
 * 标志写在字段之后是逻辑发布顺序，但普通 bitop 不提供跨字段事务一致性。
 */
static inline void __update_page_owner_handle(struct page *page,
					      depot_stack_handle_t handle,
					      unsigned short order,
					      gfp_t gfp_mask,
					      short last_migrate_reason, u64 ts_nsec,
					      pid_t pid, pid_t tgid, char *comm)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page_owner *page_owner;
	/* iter/page_ext 遍历每个基础页扩展；page_owner 是当前迭代项的临时借用私有记录。 */

	/* 整个高阶分配的每个基础页都保存同一归因，tail 页也能在内部查询。 */
	/* RCU 读侧覆盖全部高阶页的扩展记录，避免热插拔回收期间使用失效 metadata。 */
	rcu_read_lock();
	for_each_page_ext(page, 1 << order, page_ext, iter) {
		/* 先完整填充归因字段，再置两个可见性标志，读者不会把未初始化记录当有效。 */
		page_owner = get_page_owner(page_ext);
		/* handle、order、GFP 与调用者身份描述同一次 allocation 的快照。 */
		page_owner->handle = handle;
		page_owner->order = order;
		/* GFP/迁移原因与任务字段为 dump 和碎片统计提供分配时上下文。 */
		page_owner->gfp_mask = gfp_mask;
		page_owner->last_migrate_reason = last_migrate_reason;
		page_owner->pid = pid;
		/* pid/tgid/comm 均为分配瞬间复制值，任务退出后仍可用于离线诊断。 */
		page_owner->tgid = tgid;
		page_owner->ts_nsec = ts_nsec;
		strscpy(page_owner->comm, comm,
			sizeof(page_owner->comm));
		__set_bit(PAGE_EXT_OWNER, &page_ext->flags);
		__set_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags);
	}
	rcu_read_unlock();
}

/*
 * 在 free/reset 路径记录释放栈、时间和任务，并可清除 ALLOCATED 位。page/order 描述范围；
 * handle 为 0 时仅复制历史 free 信息而不改变分配状态。无返回，RCU 保护 page_ext 遍历。
 * OWNER 位保留历史归因，只有 __reset_page_owner 才允许把当前分配状态摘除。
 */
/*
 * 业务背景：释放路径需要把当前 allocation 转成历史记录；migration 复制路径则只想复制
 * free 时间信息而不能清除 new folio 的已分配状态，因此以 handle 是否为 0 区分两种模式。
 * 入参：page 是非空借用首页；handle 是释放栈句柄，非零表示真实 reset、0 表示只复制历史；
 * order 为覆盖范围阶；pid/tgid 是调用者提供的任务快照；free_ts_nsec 是纳秒时间快照。
 * 出参/返回：无直接返回；遍历 2^order 个 page_ext，非零 handle 时清 ALLOCATED 并写
 * free_handle；始终写 free 时间和任务字段，不改变 OWNER 或 allocation handle。
 * 注意事项：当前实现没有使用 pid/tgid 形参，而是对每项写 current->pid/current->tgid；
 * 因此 migration 调用即使传入 old 的 free_pid/free_tgid，也会记录执行复制的当前任务。
 * RCU 临界区内不可睡眠；调用者必须把这一当前版本限制当作诊断精度边界。
 */
static inline void __update_page_owner_free_handle(struct page *page,
						   depot_stack_handle_t handle,
						   unsigned short order,
						   pid_t pid, pid_t tgid,
						   u64 free_ts_nsec)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page_owner *page_owner;
	/* iter/page_ext/page_owner 是范围遍历状态；pid/tgid 目前仅保留在接口中，函数体未读取。 */

	/* free 快照只写释放字段；allocation 归因由此前 set 路径保留。 */
	rcu_read_lock();
	for_each_page_ext(page, 1 << order, page_ext, iter) {
		page_owner = get_page_owner(page_ext);
		/* Only __reset_page_owner() wants to clear the bit */
		/* 补充说明：迁移复制 free 历史时传入 0，不能把目标 folio 误标为已释放。 */
		if (handle) {
			__clear_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags);
			page_owner->free_handle = handle;
		}
		page_owner->free_ts_nsec = free_ts_nsec;
		page_owner->free_pid = current->pid;
		page_owner->free_tgid = current->tgid;
	}
	rcu_read_unlock();
}

/*
 * 分配器释放高阶页时记录 free stack 并撤销 allocation stack 引用。page 是借用首页，
 * order 是分配阶；无返回。若没有 page_ext 则静默跳过。early_handle 没有对应增计数，
 * 必须跳过减计数；其余路径以 __GFP_NOWARN 保存栈避免诊断递归失败噪声。
 */
/*
 * 业务背景：伙伴分配器释放一笔 allocation 时调用，先保留最近释放现场，再把该 allocation
 * 对 stack record 的基础页计数撤销，使 debugfs 聚合反映当前占用而非累计历史。
 * 入参：page 是非空、即将释放的借用首页；order 为原 allocation 阶并决定 2^order 基础页。
 * 出参/返回：无直接返回；无 page_ext 时无副作用；否则记录 free stack/时间/current 身份、
 * 清除整个范围的 ALLOCATED 位，并对非 early allocation 扣减对应 stack count。
 * 注意事项：page_ext_get 交付 RCU 锁，复制 alloc_handle 后必须先 put 才能调用可能分配的
 * save_stack；两阶段之间允许诊断并发观察。early_handle 未增 count，绝不可执行 dec。
 */
void __reset_page_owner(struct page *page, unsigned short order)
{
	struct page_ext *page_ext;
	depot_stack_handle_t handle;
	depot_stack_handle_t alloc_handle;
	struct page_owner *page_owner;
	u64 free_ts_nsec = local_clock();
	/* handle 是本次 free 栈；alloc_handle 是原 allocation 栈；free_ts_nsec 是本地纳秒快照。 */

	/* 先取 allocation handle 快照，随后释放 ext 引用，较慢的 stack 保存不持有该引用。 */
	page_ext = page_ext_get(page);
	if (unlikely(!page_ext))
		return;

	/* 首页的记录足以代表整个 order 范围，获取后立即把需要的 handle 复制出来。 */
	page_owner = get_page_owner(page_ext);
	alloc_handle = page_owner->handle;
	page_ext_put(page_ext);

	/*
	 * Do not specify GFP_NOWAIT to make gfpflags_allow_spinning() == false
	 * to prevent issues in stack_depot_save().
	 * This is similar to alloc_pages_nolock() gfp flags, but only used
	 * to signal stack_depot to avoid spin_locks.
	 */
	/* 补充说明：禁止 NOWAIT 让 stack depot 可按允许自旋的规则工作，但不把 warning 扩散给释放路径。 */
	/* 保存 free 调用链后才清 ALLOCATED 位，使读者不会看到无释放栈的“已释放”状态。 */
	handle = save_stack(__GFP_NOWARN);
	__update_page_owner_free_handle(page, handle, order, current->pid,
					current->tgid, free_ts_nsec);

	if (alloc_handle != early_handle)
		/*
		 * early_handle is being set as a handle for all those
		 * early allocated pages. See init_pages_in_zone().
		 * Since their refcount is not being incremented because
		 * the machinery is not ready yet, we cannot decrement
		 * their refcount either.
		 */
		/*
		 * early_handle 统一标记常规计数机制就绪前的页面；这些页从未增加 record count，
		 * 所以释放时也不能扣减，否则会把特殊记录的基准计数减到零。
		 */
		dec_stack_record_count(alloc_handle, 1 << order);
}

/*
 * 分配成功后记录 allocation stack、GFP、任务和时间，并增加该栈的基础页引用数。
 * page/order/gfp_mask 均为输入借用值；无返回。调用者已拥有新分配页，更新后 page_ext
 * 的 OWNER/ALLOCATED 位向 dump/debugfs 宣告该归因；save_stack 失败会得到明确替代 handle。
 */
/*
 * 业务背景：伙伴分配成功后的 page_owner 热路径，把当前调用栈和任务上下文发布到每个基础页，
 * 并让 stack 聚合计数增加相同页数。
 * 入参：page 是非空且由分配器拥有的新分配首页；order 为 allocation 阶；gfp_mask 是本次
 * 分配约束，同时用于 stack depot 和首次索引节点分配。
 * 出参/返回：无直接返回；范围 owner 记录被标为有效且 allocated，对 handle 的基础页计数
 * 增加 2^order；不改变 page 引用或分配成功语义。
 * 注意事项：save_stack 可能按 GFP 约束分配；元数据失败退化为 failure/dummy handle 而不让
 * 主分配失败。当前调用者必须确保 page/order 匹配，计数增加要由 reset 或迁移协议最终配平。
 */
noinline void __set_page_owner(struct page *page, unsigned short order,
					gfp_t gfp_mask)
{
	u64 ts_nsec = local_clock();
	depot_stack_handle_t handle;
	/* ts_nsec/handle 分别是本次 allocation 的本地时钟快照与最终可用栈句柄。 */

	/* handle 可能是 dummy/failure，但仍必须进入统一的 count 与 owner 更新协议。 */
	handle = save_stack(gfp_mask);
	__update_page_owner_handle(page, handle, order, gfp_mask, -1,
				   ts_nsec, current->pid, current->tgid,
				   current->comm);
	inc_stack_record_count(handle, gfp_mask, 1 << order);
}

/*
 * 迁移完成时更新 folio 首页的最近迁移原因。folio 是借用对象，reason 是 migrate 枚举；
 * 无返回。page_ext_get 失败时不影响迁移本身；成功后 put 配对，字段供诊断输出读取。
 */
/*
 * 业务背景：迁移完成后诊断需要解释为何移动页面，本函数只更新目标 folio 的最近原因，
 * 不参与真正的页内容、LRU 或 owner handle 转移。
 * 入参：folio 是非空借用目标 folio；reason 是 migrate_reason 枚举整数，调用者保证可用于
 * migrate_reason_names 索引，且只作诊断值。
 * 出参/返回：无直接返回；存在 page_ext 时写首页 last_migrate_reason，否则静默跳过；
 * 不持有 folio/page_ext 引用到返回后。
 * 注意事项：page_ext_get 成功后实际处于 RCU 读侧，put 前不可睡眠；字段更新无锁，读者只可
 * 视为尽力快照。无扩展绝不能影响迁移主流程成功与否。
 */
void __folio_set_owner_migrate_reason(struct folio *folio, int reason)
{
	struct page_ext *page_ext = page_ext_get(&folio->page);
	struct page_owner *page_owner;
	/* page_ext 是带 RCU 保护的首页扩展；page_owner 只在对应 put 之前有效。 */

	if (unlikely(!page_ext))
		return;

	/* 只写元数据，不持有 folio 引用，也不改变迁移路径的完成条件。 */
	page_owner = get_page_owner(page_ext);
	page_owner->last_migrate_reason = reason;
	page_ext_put(page_ext);
}

/*
 * 高阶页拆分后，把原范围每个 page_ext 的记录阶从 old_order 改为 new_order。
 * page 是借用首页；无返回。RCU 覆盖迭代，归因栈不变；调用者已完成实际分裂的状态转换。
 */
/*
 * 业务背景：高阶 folio/页拆成更小块后，若仍保留旧 order，debugfs 会错误跳过新首页或把多个
 * 小分配当成一个大分配；本函数只修正 owner 元数据粒度。
 * 入参：page 是非空借用原高阶首页；old_order 决定遍历 2^old_order 个旧范围基础页；
 * new_order 是拆分后每个子块的阶，要求 0 <= new_order <= old_order。
 * 出参/返回：无直接返回；把旧范围每个现有 page_owner.order 改为 new_order，handle/count
 * 与 OWNER 标志均不变。
 * 注意事项：调用者已完成实际拆分并稳定页面；显式 RCU 读锁期间不可睡眠。该函数不重新分配
 * stack count，因为覆盖的基础页总数不变。
 */
void __split_page_owner(struct page *page, int old_order, int new_order)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page_owner *page_owner;
	/* iter/page_ext 遍历旧范围，page_owner 是每项仅在 RCU 临界区有效的借用记录。 */

	/* old_order 决定需要修正的 page_ext 数量，新 order 是拆分后每份的归因粒度。 */
	rcu_read_lock();
	for_each_page_ext(page, 1 << old_order, page_ext, iter) {
		page_owner = get_page_owner(page_ext);
		page_owner->order = new_order;
	}
	rcu_read_unlock();
}

/*
 * folio migration 把 old 的 allocation 归因复制给 new，并保存 new 原有 handle 到 old 的
 * page_ext，保持 stack 引用计数平衡。两个 folio 均为借用对象；无返回。page_ext 引用
 * 只用于读取/定位，复制循环在 RCU 下运行；不提前清 OWNER 位以保留迁移后释放诊断。
 */
/*
 * 业务背景：迁移为目标 folio 预先分配过页面，old/new 各自已对一个 stack record 计数；
 * 迁移成功后诊断归因应随逻辑内容去 new，但未来释放两边仍必须各扣回原有的一份基础页计数。
 * 入参：newfolio 是迁移目标、old 是迁移源，均为非空借用 folio；调用者保证页面与迁移状态
 * 在本函数期间稳定，且两者已有 page_owner 元数据。
 * 出参/返回：无直接返回；new 获得 old 的 allocation 字段与 free 历史时间，old 范围的 handle
 * 改为 new 原 handle，形成“归因随内容移动、释放计数仍平衡”的交换；不直接增减 record count。
 * 任一首页 page_ext_get 失败时提前返回，可能不产生任何或只完成前序读取，不影响迁移主结果。
 * 注意事项：当前实现从 old/new page_ext 取得 page_owner 指针后立即 page_ext_put()，随后仍读取
 * 这些指针；它依赖迁移/内存热拔上层协议在整个调用期间稳定对应 page_ext backing，不能把这种
 * 用法推广到普通调用者。free pid/tgid 形参又被下层忽略，复制后写入的是当前迁移任务身份。
 */
void __folio_copy_owner(struct folio *newfolio, struct folio *old)
{
	struct page_ext *page_ext;
	struct page_ext_iter iter;
	struct page_owner *old_page_owner;
	struct page_owner *new_page_owner;
	depot_stack_handle_t migrate_handle;
	/*
	 * page_ext/iter 用于两次首页取得和 old 范围回填；old/new_page_owner 指向各自私有记录；
	 * migrate_handle 暂存目标页原 allocation 句柄，供源页未来释放时扣回其计数。
	 */

	/* old 与 new 分别短暂取 ext，避免迁移路径长期阻塞 page_ext 生命周期。 */
	page_ext = page_ext_get(&old->page);
	if (unlikely(!page_ext))
		return;

	old_page_owner = get_page_owner(page_ext);
	page_ext_put(page_ext);

	page_ext = page_ext_get(&newfolio->page);
	if (unlikely(!page_ext))
		return;

	new_page_owner = get_page_owner(page_ext);
	/*
	 * 当前代码在 put 后仍通过 new_page_owner 读取多个字段，并非真正的值快照；其有效性依赖
	 * 迁移路径排除对应 page_ext backing 被热拔回收这一外层前提。
	 */
	page_ext_put(page_ext);

	/* new 原有 handle 需转给 old，否则后续 free 会对旧栈错误减计数。 */
	migrate_handle = new_page_owner->handle;
	__update_page_owner_handle(&newfolio->page, old_page_owner->handle,
				   old_page_owner->order, old_page_owner->gfp_mask,
				   old_page_owner->last_migrate_reason,
				   old_page_owner->ts_nsec, old_page_owner->pid,
				   old_page_owner->tgid, old_page_owner->comm);
	/*
	 * Do not proactively clear PAGE_EXT_OWNER{_ALLOCATED} bits as the folio
	 * will be freed after migration. Keep them until then as they may be
	 * useful.
	 */
	/* 补充说明：页面真正释放前保留标志可让并发诊断仍区分 allocation 与历史 free。 */
	__update_page_owner_free_handle(&newfolio->page, 0, old_page_owner->order,
					old_page_owner->free_pid,
					old_page_owner->free_tgid,
					old_page_owner->free_ts_nsec);
	/*
	 * We linked the original stack to the new folio, we need to do the same
	 * for the new one and the old folio otherwise there will be an imbalance
	 * when subtracting those pages from the stack.
	 */
	/* 补充说明：old/new 两边都会经历释放计数，故必须把迁移前 new 的 handle 回填给 old。 */
	rcu_read_lock();
	/* old 范围的回填确保随后释放两边 folio 时各自减回正确的栈。 */
	for_each_page_ext(&old->page, 1 << new_page_owner->order, page_ext, iter) {
		old_page_owner = get_page_owner(page_ext);
		old_page_owner->handle = migrate_handle;
	}
	rcu_read_unlock();
}

/*
 * pagetypeinfo 诊断接口按 zone 统计 pageblock migratetype 与页实际 GFP migratetype 不同的块。
 * m 是输出流，pgdat/zone 是借用拓扑对象；无返回。扫描不持 zone lock，允许保守漏报以避免
 * 诊断造成锁竞争；遇到 buddy、高阶页、reserved、离线 PFN 均按各自规则跳过。
 */
/*
 * 业务背景：/proc/pagetypeinfo 需要指出一个 pageblock 是否混入与其策略不同的当前分配，
 * 这能解释外部碎片和 CMA/MOVABLE 隔离效果；本函数为每种 pageblock 类型累计混合块数。
 * 入参：m 是非空 seq_file 输出对象；pgdat 是 zone 所属节点的借用描述符；zone 是非空借用
 * 区域，三者在 proc show 回调期间稳定且 pgdat/zone 必须相互匹配。
 * 出参/返回：无直接返回；向 m 追加一行 node/zone 及 MIGRATE_TYPES 顺序的块计数；不修改
 * page、zone、page_ext 或 stack record。
 * 注意事项：不持 zone->lock，PageBuddy/order 与 owner 字段都是并发近似快照，允许漏报但不得
 * 影响分配器。page_ext_get 成功后到 put 前不可睡眠；每个混合 pageblock 只计一次并跳至块尾。
 */
void pagetypeinfo_showmixedcount_print(struct seq_file *m,
				       pg_data_t *pgdat, struct zone *zone)
{
	struct page *page;
	struct page_ext *page_ext;
	struct page_owner *page_owner;
	unsigned long pfn, block_end_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count[MIGRATE_TYPES] = { 0, };
	/* count 的索引来自 pageblock 类型而非单页 GFP 类型，符合 pagetypeinfo ABI。 */
	int pageblock_mt, page_mt;
	int i;
	/*
	 * page/page_ext/page_owner 是当前 PFN 的借用对象；pfn/block_end/end 定义扫描窗口；
	 * pageblock_mt/page_mt 是策略与实际 GFP 类型；i 为固定输出列游标。
	 */

	/* 该函数不分配内存；所有指针均借用 zone/seq 框架对象。 */
	/* 局部变量分别跟踪当前 PFN、块终点、策略类型和每种类型的累计结果。 */
	/* Scan block by block. First and last block may be incomplete */
	/* 补充说明：pageblock 是迁移类型的最小策略单位，边界不完整不影响混合块计数含义。 */
	pfn = zone->zone_start_pfn;

	/*
	 * Walk the zone in pageblock_nr_pages steps. If a page block spans
	 * a zone boundary, it will be double counted between zones. This does
	 * not matter as the mixed block count will still be correct
	 */
	/*
	 * 按 pageblock_nr_pages 遍历；跨 zone 的边界块可能在两边重复扫描，但本函数输出的是
	 * 各 zone 内“是否混合”的块数，重复边界检查不会改变单个 zone 的分类结论。
	 */
	/* 外层按 pageblock 前进，内层按页或高阶 buddy/owner 的跨度跳跃。 */
	for (; pfn < end_pfn; ) {
		page = pfn_to_online_page(pfn);
		if (!page) {
			pfn = ALIGN(pfn + 1, MAX_ORDER_NR_PAGES);
			continue;
		}

		block_end_pfn = pageblock_end_pfn(pfn);
		/* block_end 被 zone 末尾裁剪，内层不会跨 zone 访问 page_ext。 */
		block_end_pfn = min(block_end_pfn, end_pfn);

		pageblock_mt = get_pageblock_migratetype(page);

		for (; pfn < block_end_pfn; pfn++) {
			/* The pageblock is online, no need to recheck. */
			/* 外层已确认 pageblock 起点在线，块内按 PFN 直接取 struct page，无需重复在线检查。 */
			page = pfn_to_page(pfn);

			if (page_zone(page) != zone)
				continue;

			/* 空闲 buddy 块没有“已分配”的 owner，整块跳过可避免重复扫描 tail 页。 */
			if (PageBuddy(page)) {
				unsigned long freepage_order;

				freepage_order = buddy_order_unsafe(page);
				if (freepage_order <= MAX_PAGE_ORDER)
					pfn += (1UL << freepage_order) - 1;
				continue;
			}

			/* reserved 页不是普通可迁移分配，不能被 mixed 统计误判。 */
			if (PageReserved(page))
				continue;

			page_ext = page_ext_get(page);
			if (unlikely(!page_ext))
				continue;

			/* 仅当前已分配页影响当前内存使用；历史 owner 记录不参与。 */
			if (!test_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags))
				goto ext_put_continue;

			/* 仅在已分配标志稳定为真后读取 GFP 和 order。 */
			page_owner = get_page_owner(page_ext);
			page_mt = gfp_migratetype(page_owner->gfp_mask);
			/* 块策略和分配 GFP 不同即为 mixed；CMA 特例归入 MOVABLE 展示列。 */
			if (pageblock_mt != page_mt) {
				if (is_migrate_cma(pageblock_mt))
					count[MIGRATE_MOVABLE]++;
				else
					count[pageblock_mt]++;

				pfn = block_end_pfn;
				page_ext_put(page_ext);
				break;
			}
			/* 同一高阶 allocation 的尾页共享 owner，跳过防止重复计数。 */
			pfn += (1UL << page_owner->order) - 1;
ext_put_continue:
			page_ext_put(page_ext);
		}
	}

	/* Print counts */
	/* 补充说明：输出遵循 MIGRATE_TYPES 顺序，供 pagetypeinfo 用户态解析。 */
	seq_printf(m, "Node %d, zone %8s ", pgdat->node_id, zone->name);
	for (i = 0; i < MIGRATE_TYPES; i++)
		seq_printf(m, "%12lu ", count[i]);
	seq_putc(m, '\n');
}

/*
 * Looking for memcg information and print it out
 */
/*
 * 补充说明：该 helper 在 page owner 文本末尾附加 memcg 归属；CONFIG_MEMCG=n 时保持
 * 原 ret 不变，调用者无需为配置差异分支。
 */
/*
 * 业务背景：单页 owner 输出若能关联 cgroup，可把异常分配定位到租户；配置关闭或页无 memcg
 * 时必须保持通用输出可用。
 * 入参：kbuf 是非空、容量为 count 字节的内核输出缓冲；ret 是已使用字节数且范围应为
 * [0,count]；page 是非空借用页，仅在调用期间读取。
 * 出参/返回：CONFIG_MEMCG=y 且取得归属时在 kbuf+ret 追加 slab/online/name 文本并返回新长度；
 * 其它情况返回原 ret；不转移 page/memcg/kbuf ownership。
 * 注意事项：RCU 读侧稳定 memcg css/name 查找，期间不可睡眠；page->memcg_data 与 online 状态
 * 仍只是并发快照。调用者负责确保剩余空间，scnprintf 负责截断而不越界。
 */
static inline int print_page_owner_memcg(char *kbuf, size_t count, int ret,
					 struct page *page)
{
#ifdef CONFIG_MEMCG
	unsigned long memcg_data;
	struct mem_cgroup *memcg;
	bool online;
	char name[80];
	/* memcg_data 是带标志的原始归属快照；memcg 为 RCU 借用对象；online/name 为输出副本。 */

	/* memcg css 可能并发 offline，RCU 只保护本次名字/online 状态快照。 */
	rcu_read_lock();
	memcg_data = READ_ONCE(page->memcg_data);
	if (!memcg_data || PageTail(page))
		goto out_unlock;

	if (memcg_data & MEMCG_DATA_OBJEXTS)
		/* objexts 表示 slab 对象扩展归属，输出与普通页 memcg 区分。 */
		ret += scnprintf(kbuf + ret, count - ret,
				"Slab cache page\n");

	memcg = page_memcg_check(page);
	/* 检查失败时不解引用可能已变更的 memcg 指针，直接在 RCU 出口返回。 */
	if (!memcg)
		goto out_unlock;

	/* css online 状态和名称在同一个 RCU 窗口取样，离线组仍可被明确标记。 */
	online = css_is_online(&memcg->css);
	cgroup_name(memcg->css.cgroup, name, sizeof(name));
	ret += scnprintf(kbuf + ret, count - ret,
			"Charged %sto %smemcg %s\n",
			PageMemcgKmem(page) ? "(via objcg) " : "",
			online ? "" : "offline ",
			name);
out_unlock:
	/* 所有 CONFIG_MEMCG 出口统一结束 RCU 窗口后返回当前文本长度。 */
	rcu_read_unlock();
#endif /* CONFIG_MEMCG */
	/* CONFIG_MEMCG 关闭时整段被编译掉，函数原样返回调用者传入的 ret。 */

	return ret;
}

/*
 * 将一个已快照的 page_owner 格式化并复制到用户 debugfs 读缓冲。buf 是用户输出，count
 * 为容量，pfn/page/page_owner/handle 为稳定输入；成功返回字节数，内存不足 -ENOMEM，
 * copy_to_user 失败 -EFAULT。临时 kbuf 由本函数取得并在所有出口释放，不能持 page_ext 引用。
 */
/*
 * 业务背景：read_page_owner 已在不可睡眠的 page_ext 窗口内复制记录，本函数离开该窗口后执行
 * 可能睡眠的格式化、memcg 查询和用户复制，形成一条完整 debugfs 记录。
 * 入参：buf 是用户输出指针；count 是用户容量，内部最多使用 PAGE_SIZE；pfn 是页帧号；page
 * 是非空借用页；page_owner 指向调用者值快照；handle 是已验证非零的 allocation depot 句柄。
 * 出参/返回：成功复制后返回实际字节数；kmalloc 或格式化超出单页返回 -ENOMEM，copy_to_user
 * 失败返回 -EFAULT；所有出口释放本函数拥有的 kbuf，不改变 file position。
 * 注意事项：GFP_KERNEL 和 copy_to_user 均可睡眠，调用者不得持 page_ext RCU 窗口或自旋锁；
 * page/handle 仍需由上层页面与 stack depot 生命周期保证。输出字段来自非原子快照，供诊断而非同步。
 */
static ssize_t
print_page_owner(char __user *buf, size_t count, unsigned long pfn,
		struct page *page, struct page_owner *page_owner,
		depot_stack_handle_t handle)
{
	int ret, pageblock_mt, page_mt;
	char *kbuf;
	/* ret 是累计输出长度/最终 errno；两种 mt 用于碎片解释；kbuf 是本函数拥有的临时页内缓冲。 */

	/* 单次输出最多一页，避免 debugfs read 用用户给出的巨大长度分配内核内存。 */
	count = min_t(size_t, count, PAGE_SIZE);
	kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	ret = scnprintf(kbuf, count,
			"Page allocated via order %u, mask %#x(%pGg), pid %d, tgid %d (%s), ts %llu ns\n",
			page_owner->order, page_owner->gfp_mask,
			&page_owner->gfp_mask, page_owner->pid,
			page_owner->tgid, page_owner->comm,
			page_owner->ts_nsec);
	/* 首行固定包含可将文本关联回 allocation 事件的阶、GFP、任务和本地时间。 */

	/* Print information relevant to grouping pages by mobility */
	/* 补充说明：比较实际 GFP 与 pageblock 策略有助解释可迁移性碎片。 */
	pageblock_mt = get_pageblock_migratetype(page);
	page_mt  = gfp_migratetype(page_owner->gfp_mask);
	ret += scnprintf(kbuf + ret, count - ret,
			"PFN 0x%lx type %s Block %lu type %s Flags %pGp\n",
			pfn,
			migratetype_names[page_mt],
			pfn >> pageblock_order,
			migratetype_names[pageblock_mt],
			&page->flags.f);

	/* stack_depot 通过稳定 handle 解码调用栈；溢出统一走 err 释放 kbuf。 */
	ret += stack_depot_snprint(handle, kbuf + ret, count - ret, 0);
	if (ret >= count)
		goto err;

	if (page_owner->last_migrate_reason != -1) {
		/* -1 是未迁移哨兵；其它值可索引迁移原因名称表。 */
		ret += scnprintf(kbuf + ret, count - ret,
			"Page has been migrated, last migrate reason: %s\n",
			migrate_reason_names[page_owner->last_migrate_reason]);
	}

	/* memcg 追加在栈后，配置关闭或无归属时原样返回已写长度。 */
	ret = print_page_owner_memcg(kbuf, count, ret, page);

	ret += snprintf(kbuf + ret, count - ret, "\n");
	if (ret >= count)
		goto err;

	/* 用户拷贝是最后一步；失败不留下 kbuf 所有权。 */
	if (copy_to_user(buf, kbuf, ret))
		ret = -EFAULT;

	kfree(kbuf);
	return ret;

err:
	kfree(kbuf);
	return -ENOMEM;
}

/*
 * 在警告/崩溃路径向内核日志打印单页的 allocation/free stack、任务、GFP 与迁移原因。
 * page 是借用页；无返回。没有 page_ext 或从未记录时打印原因后退出；page_ext_get/put
 * 覆盖所有读取，READ_ONCE 取得可能与更新并发的 stack handle 快照。
 */
/*
 * 业务背景：内核警告路径无法经用户 debugfs 迭代，需直接打印指定 page 最近 allocation/free
 * 归因和迁移原因，帮助分析坏页、泄漏或引用错误。
 * 入参：page 是非空 const 借用页；函数不会取得 page 引用或修改页面。
 * 出参/返回：无直接返回；无 page_ext 或无 OWNER 时打印缺失原因，否则向内核日志输出状态、
 * 任务/时间、allocation/free stack 及可选迁移原因；所有成功 get 路径精确 page_ext_put。
 * 注意事项：page_ext_get 到 put 之间不可睡眠；日志和 stack 打印必须适用于当前诊断上下文。
 * 字段缺少统一锁，READ_ONCE 仅稳定单个 handle，整条报告可能混合并发更新时刻。
 */
void __dump_page_owner(const struct page *page)
{
	struct page_ext *page_ext = page_ext_get((void *)page);
	struct page_owner *page_owner;
	depot_stack_handle_t handle;
	gfp_t gfp_mask;
	int mt;
	/* page_owner 是 page_ext 内借用记录；handle/gfp_mask/mt 是为本次日志复制的局部快照。 */

	/* page_ext 引用是打印期间唯一的生命周期保证，所有提前返回都必须 put。 */
	if (unlikely(!page_ext)) {
		pr_alert("There is not page extension available.\n");
		return;
	}

	/* page_ext_get 成功只保证扩展对象生命期，不保证 owner 字段已被分配路径初始化。 */
	/* 先读取 GFP 用于日志；若 OWNER 未置则随后拒绝把零值当真实归因。 */
	page_owner = get_page_owner(page_ext);
	gfp_mask = page_owner->gfp_mask;
	mt = gfp_migratetype(gfp_mask);

	/* OWNER 位未置意味着 page owner 从未记录，后续字段没有可解释的归因。 */
	if (!test_bit(PAGE_EXT_OWNER, &page_ext->flags)) {
		pr_alert("page_owner info is not present (never set?)\n");
		page_ext_put(page_ext);
		return;
	}

	/* ALLOCATED 位区分当前占用与仅保留历史 free 记录。 */
	if (test_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags))
		pr_alert("page_owner tracks the page as allocated\n");
	else
		pr_alert("page_owner tracks the page as freed\n");

	pr_alert("page last allocated via order %u, migratetype %s, gfp_mask %#x(%pGg), pid %d, tgid %d (%s), ts %llu, free_ts %llu\n",
		 page_owner->order, migratetype_names[mt], gfp_mask, &gfp_mask,
		 page_owner->pid, page_owner->tgid, page_owner->comm,
		 page_owner->ts_nsec, page_owner->free_ts_nsec);

	/* 写者可能更新 handle；单次 READ_ONCE 防止读取被撕裂，但不是多字段一致快照。 */
	handle = READ_ONCE(page_owner->handle);
	if (!handle)
		pr_alert("page_owner allocation stack trace missing\n");
	else
		stack_depot_print(handle);

	/* allocation/free 句柄独立读取，诊断允许输出两个不同时间点的快照。 */
	handle = READ_ONCE(page_owner->free_handle);
	if (!handle) {
		pr_alert("page_owner free stack trace missing\n");
	} else {
		pr_alert("page last free pid %d tgid %d stack trace:\n",
			  page_owner->free_pid, page_owner->free_tgid);
		stack_depot_print(handle);
	}

	/* 有迁移记录才索引名称表，避免用 -1 访问无效数组元素。 */
	if (page_owner->last_migrate_reason != -1)
		pr_alert("page has been migrated, last migrate reason: %s\n",
			migrate_reason_names[page_owner->last_migrate_reason]);
	page_ext_put(page_ext);
}

/*
 * debugfs page_owner 的 read 迭代 PFN，寻找一个当前 allocated 的首页并输出其归因。
 * file 未使用，buf/count 为用户输出，ppos 既是下一 PFN 输入也是成功后的输出；返回 0 表示
 * 扫描结束，或 print_page_owner 的结果。无 zone lock，故并发 alloc/free 可造成保守漏页。
 */
/*
 * 业务背景：debugfs page_owner 以文件位置直接表示 PFN，每次 read 从该位置向上扫描并只返回
 * 一个当前 allocation 首页，便于用户态流式枚举而不在内核积累全量快照。
 * 入参：file 是未读取内容的借用打开文件；buf 是用户输出指针；count 是容量；ppos 是非空
 * 输入输出 PFN 游标，0 从 min_low_pfn 开始，非零从该 PFN 继续。
 * 出参/返回：功能未启用返回 -EINVAL；扫描至 max_pfn 无记录返回 0；找到记录时先把 *ppos
 * 更新为下一 PFN，再返回 print_page_owner 的正字节数或 -ENOMEM/-EFAULT。
 * 注意事项：不持 zone lock，洞、buddy 块、历史 free 和高阶 tail 被跳过，并发 alloc/free
 * 允许漏过/读取近似记录。page_ext_get 到 put 期间不可睡眠，故先复制 page_owner 到栈上再格式化。
 */
static ssize_t
read_page_owner(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	unsigned long pfn;
	struct page *page;
	struct page_ext *page_ext;
	struct page_owner *page_owner;
	depot_stack_handle_t handle;
	/* pfn/page 是扫描位置；page_ext/page_owner 只在 get/put 窗口有效；handle 是非零栈句柄快照。 */

	/* static key 是 init_page_owner 的发布标志，关闭时不允许访问未分配的 page_ext。 */
	if (!static_branch_unlikely(&page_owner_inited))
		return -EINVAL;

	page = NULL;
	if (*ppos == 0)
		pfn = min_low_pfn;
	else
		pfn = *ppos;
	/* Find a valid PFN or the start of a MAX_ORDER_NR_PAGES area */
	/* 补充说明：洞内跳到高阶边界，避免逐 PFN 扫描没有 memmap 的区域。 */
	while (!pfn_valid(pfn) && (pfn & (MAX_ORDER_NR_PAGES - 1)) != 0)
		pfn++;

	/* Find an allocated page */
	/* 补充说明：每次 read 最多返回一个记录，ppos 让下一次从其后继续。 */
	for (; pfn < max_pfn; pfn++) {
		/*
		 * This temporary page_owner is required so
		 * that we can avoid the context switches while holding
		 * the rcu lock and copying the page owner information to
		 * user through copy_to_user() or GFP_KERNEL allocations.
		 */
		/* 补充说明：离开 page_ext 临界区后使用副本，避免用户复制期间阻塞更新者。 */
		struct page_owner page_owner_tmp;
		/* page_owner_tmp 是离开 RCU 后用于睡眠格式化的完整值副本，不含任何新增 ownership。 */

		/*
		 * If the new page is in a new MAX_ORDER_NR_PAGES area,
		 * validate the area as existing, skip it if not
		 */
		/*
		 * 每进入一个新的 MAX_ORDER_NR_PAGES 区域都重新验证起始 PFN；若该区域不存在，
		 * 一次跳过整个区域，既避免无效 pfn_to_page，也避免逐 PFN 探测洞。
		 */
		if ((pfn & (MAX_ORDER_NR_PAGES - 1)) == 0 && !pfn_valid(pfn)) {
			pfn += MAX_ORDER_NR_PAGES - 1;
			continue;
		}

		page = pfn_to_page(pfn);
		/* buddy 高阶块没有有效的当前 owner，整块跳过。 */
		if (PageBuddy(page)) {
			unsigned long freepage_order = buddy_order_unsafe(page);
			/* freepage_order 是无 zone 锁近似值，仅用于安全上限内的扫描加速。 */

			if (freepage_order <= MAX_PAGE_ORDER)
				pfn += (1UL << freepage_order) - 1;
			continue;
		}

		page_ext = page_ext_get(page);
		if (unlikely(!page_ext))
			continue;

		/*
		 * Some pages could be missed by concurrent allocation or free,
		 * because we don't hold the zone lock.
		 */
		/* 并发释放/分配会改变该位；本次扫描宁可跳过也不打印不可信记录。 */
		if (!test_bit(PAGE_EXT_OWNER, &page_ext->flags))
			goto ext_put_continue;

		/*
		 * Although we do have the info about past allocation of free
		 * pages, it's not relevant for current memory usage.
		 */
		/* 历史 free 页虽保留 owner 信息，但本接口统计当前内存占用，故只接受 ALLOCATED。 */
		if (!test_bit(PAGE_EXT_OWNER_ALLOCATED, &page_ext->flags))
			goto ext_put_continue;

		page_owner = get_page_owner(page_ext);

		/*
		 * Don't print "tail" pages of high-order allocations as that
		 * would inflate the stats.
		 */
		/* 只输出高阶分配首页，tail 页共用记录而不能重复计入用户态统计。 */
		if (!IS_ALIGNED(pfn, 1 << page_owner->order))
			goto ext_put_continue;

		/*
		 * Access to page_ext->handle isn't synchronous so we should
		 * be careful to access it.
		 */
		/* handle 可与更新路径并发变化，只做一次 READ_ONCE 快照并拒绝零句柄。 */
		handle = READ_ONCE(page_owner->handle);
		if (!handle)
			goto ext_put_continue;

		/* Record the next PFN to read in the file offset */
		/* 补充说明：先推进位置再复制，用户下次 read 即使失败也不会重复同一页。 */
		*ppos = pfn + 1;

		page_owner_tmp = *page_owner;
		page_ext_put(page_ext);
		return print_page_owner(buf, count, pfn, page,
				&page_owner_tmp, handle);
ext_put_continue:
		page_ext_put(page_ext);
	}

	return 0;
}

/*
 * 实现 page_owner debugfs 的 SEEK_SET/SEEK_CUR，以 PFN 形式调整 file->f_pos。
 * offset/orig 是输入；成功返回新位置，未知 whence 返回 -EINVAL。未校验 PFN，read 路径负责跳过洞。
 */
/*
 * 业务背景：该诊断文件的 f_pos 不是字节，而是下一次扫描 PFN；自定义 llseek 只实现绝对和相对
 * PFN 定位，避免通用字节语义误导调用者。
 * 入参：file 是非空借用打开文件；offset 是有符号 PFN 偏移/目标；orig 仅接受 SEEK_SET/SEEK_CUR。
 * 出参/返回：支持时更新并返回 file->f_pos；其它 whence 返回 -EINVAL 且不修改位置。
 * 注意事项：不验证负值、溢出、max_pfn 或 PFN 有效性，后续 read 负责扫描/结束；无锁且不睡眠，
 * 同一 file 的并发位置更新由 VFS/调用者串行语义负责。
 */
static loff_t lseek_page_owner(struct file *file, loff_t offset, int orig)
{
	/* f_pos 保存 PFN 而不是字节偏移，这是此 debugfs 文件的专用约定。 */
	switch (orig) {
	case SEEK_SET:
		/* SEEK_SET 把 offset 直接解释为绝对 PFN，下一次 read 再验证。 */
		file->f_pos = offset;
		break;
	case SEEK_CUR:
		/* 当前 PFN 加偏移后仍由 read 验证，无效目标不会访问不存在的 memmap。 */
		file->f_pos += offset;
		break;
	default:
		/* SEEK_END 与未知值没有可定义的 PFN 语义，明确拒绝。 */
		return -EINVAL;
	}
	return file->f_pos;
}

/* seek 不调整至有效 PFN；read 的洞跳过逻辑保持同一语义。 */
/* early 扫描只能在 page_owner 初始化阶段调用，此时 hstate/page_ext 布局已经固定。 */
/*
 * 启动时扫描一个 zone，给早于 page_owner 常规记录机制的已分配页写入 early_handle。
 * zone 是借用对象；无返回。为避免抢 zone->lock，buddy order 只作不安全读取且允许遗漏；
 * 每轮 pageblock 后 cond_resched，避免启动扫描长期占用 CPU。
 */
/*
 * 业务背景：page_owner 正式发布前已有启动分配无法回溯真实栈；本函数扫描一个 zone，把非 buddy、
 * 非 reserved 且尚无 OWNER 的页面统一标为 early_handle，使后续 dump 能区分“早于追踪”与“未记录”。
 * 入参：zone 是非空、启动期稳定的借用 zone；其 PFN 范围可能含洞或重叠边界。
 * 出参/返回：无直接返回；为找到的基础页写 order 0、early_handle、当前 init 任务/时间并置 owner
 * 标志，最后打印数量；不增加 early stack record count。
 * 注意事项：函数可在 pageblock 间 cond_resched，不能持 zone 锁；buddy order 是无锁近似，允许
 * 为降低争用而漏标但不能越过 MAX_PAGE_ORDER。page_ext_get/put 窗口内不可睡眠且已有 OWNER 不覆盖。
 */
static void init_pages_in_zone(struct zone *zone)
{
	unsigned long pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count = 0;
	/* pfn/end 定义当前 zone 扫描范围；count 是成功新标记的 order-0 基础页数。 */

	/* pfn/end_pfn 是本 zone 的扫描边界；count 仅作最终信息性统计。 */
	/*
	 * Walk the zone in pageblock_nr_pages steps. If a page block spans
	 * a zone boundary, it will be double counted between zones. This does
	 * not matter as the mixed block count will still be correct
	 */
	/*
	 * 按 pageblock_nr_pages 步长遍历；跨 zone 边界的块可能在相邻 zone 各扫描一次，
	 * 但每个 zone 的 mixed block 判断仍正确，因此这里接受边界重复扫描。
	 */
	/* 与 pagetypeinfo 相同按 pageblock 扫描，洞区直接跳到下一个高阶边界。 */
	for (; pfn < end_pfn; ) {
		unsigned long block_end_pfn;
		/* block_end_pfn 是当前 pageblock 与 zone 末端裁剪后的排他上界。 */

		if (!pfn_valid(pfn)) {
			/* 无有效 struct page 的洞不能查询 page_ext，直接跳过完整高阶区域。 */
			pfn = ALIGN(pfn + 1, MAX_ORDER_NR_PAGES);
			continue;
		}

		block_end_pfn = pageblock_end_pfn(pfn);
		/* 终点取 min，确保边界 pageblock 不会越过 zone_end_pfn。 */
		block_end_pfn = min(block_end_pfn, end_pfn);

		for (; pfn < block_end_pfn; pfn++) {
			struct page *page = pfn_to_page(pfn);
			struct page_ext *page_ext;
			/* page 为当前 PFN 借用 memmap；page_ext 成功时携带必须配对 put 的 RCU 临界区。 */

			if (page_zone(page) != zone)
				continue;

			/*
			 * To avoid having to grab zone->lock, be a little
			 * careful when reading buddy page order. The only
			 * danger is that we skip too much and potentially miss
			 * some early allocated pages, which is better than
			 * heavy lock contention.
			 */
			/* 补充说明：漏标少量早期页只降低诊断覆盖，不得为它阻塞正常启动分配。 */
			if (PageBuddy(page)) {
				unsigned long order = buddy_order_unsafe(page);
				/* order 是无锁读取的空闲块阶，仅在合法范围内用于跳过其 tail。 */

				if (order > 0 && order <= MAX_PAGE_ORDER)
					pfn += (1UL << order) - 1;
				continue;
			}

			if (PageReserved(page))
				/* 保留页从不作为普通早期分配归因。 */
				continue;

			page_ext = page_ext_get(page);
			/* page_ext 缺失说明此页没有可保存的扩展记录，扫描继续。 */
			if (unlikely(!page_ext))
				continue;

			/* Maybe overlapping zone */
			/* 补充说明：已有 OWNER 表示常规或此前扫描已归因，不能覆盖其真实 handle。 */
			if (test_bit(PAGE_EXT_OWNER, &page_ext->flags))
				goto ext_put_continue;

			/* Found early allocated page */
			/* 补充说明：early_handle 不增加 depot count，因此 reset 路径会专门跳过 dec。 */
			__update_page_owner_handle(page, early_handle, 0, 0,
					   /* early 分配没有原始 GFP/栈，只用统一特殊句柄标识。 */
						   -1, local_clock(), current->pid,
						   current->tgid, current->comm);
			count++;
ext_put_continue:
			page_ext_put(page_ext);
		}
		cond_resched();
		/* 每个 pageblock 后允许调度，避免大 zone 的启动扫描饿死其它任务。 */
	}

	pr_info("Node %d, zone %8s: page owner found early allocated %lu pages\n",
		zone->zone_pgdat->node_id, zone->name, count);
}

/* 遍历所有 populated zone 调用早期归因扫描；无入参/返回，仅在 page_owner 启动初始化时运行。 */
/*
 * 业务背景：init_page_owner 需覆盖所有已有内存节点/zone，本包装用统一宏选择 populated zone。
 * 入参：无。
 * 出参/返回：无直接返回；逐 zone 完成 early_handle 标记与信息日志。
 * 注意事项：仅启动阶段可睡眠调用；zone 游标为宏借用对象，不在循环外保存，热插拔并发由初始化时序排除。
 */
static void init_early_allocated_pages(void)
{
	struct zone *zone;
	/* zone 是 for_each_populated_zone 的启动期借用游标。 */

	for_each_populated_zone(zone)
		init_pages_in_zone(zone);
}

/* page_owner PFN 扫描文件仅支持 read/llseek，避免用户写入诊断状态。 */
static const struct file_operations page_owner_fops = {
	/* 每次 read 输出至多一个当前 allocation 首页记录。 */
	.read		= read_page_owner,
	/* 文件位置按 PFN 解释，只允许自定义 SET/CUR。 */
	.llseek		= lseek_page_owner,
};

/*
 * seq_file start 从 stack_list 取得首节点或恢复上轮 ctx->stack。m/ppos 是 seq 框架输入，
 * 返回节点或 NULL；ppos=-1 表示结束。首次 acquire load 与插入端 release store 配对，
 * 保证观察到已初始化的 next/stack_record 字段，而非提供节点生命周期引用。
 */
/*
 * 业务背景：seq_file 可能分多次调用 start/next/show，ctx->stack 保存断点；首次迭代必须从
 * 写侧发布的 stack_list 头开始，并建立节点字段可见性。
 * 入参：m 是非空 seq_file，m->private 指向本次打开专属 stack_print_ctx；ppos 是非空输入
 * 输出位置，0 表示首次，-1UL 表示已经到尾部，其它值从 ctx->stack 恢复。
 * 出参/返回：返回当前借用 stack 节点供 show，尾部返回 NULL；首次同时把节点写入 ctx->stack。
 * 注意事项：acquire 只保证初始化可见性，节点生命周期安全依赖链表运行期不删除；函数不加锁、
 * 不睡眠。ppos 使用 -1UL 哨兵虽然类型为 loff_t，比较必须保持当前约定。
 */
static void *stack_start(struct seq_file *m, loff_t *ppos)
{
	struct stack *stack;
	struct stack_print_ctx *ctx = m->private;
	/* stack 是本次返回节点；ctx 是 seq_open_private 分配并由 release_private 销毁的打开实例状态。 */

	if (*ppos == -1UL)
		return NULL;

	/* 只有首轮需要发布同步，后续节点由前一轮保存在私有 ctx。 */
	if (!*ppos) {
		/*
		 * This pairs with smp_store_release() from function
		 * add_stack_record_to_list(), so we get a consistent
		 * value of stack_list.
		 */
		/* acquire 与头插 release 配对，先看到新表头，再看到该节点完整的 record/next。 */
		stack = smp_load_acquire(&stack_list);
		ctx->stack = stack;
	} else {
		stack = ctx->stack;
	}

	return stack;
}

/*
 * seq_file next 沿单向 stack_list 前进并更新位置/私有游标。v 是当前借用节点，返回下一节点
 * 或 NULL；无分配/释放。末尾把 ppos 置 -1，令下次 start 明确结束。
 */
/*
 * 业务背景：这是 stack 聚合 seq 迭代器的推进步骤，把本次 show 的节点替换为其永久 next。
 * 入参：m 是非空 seq_file；v 是当前非空借用 stack 节点；ppos 是非空位置游标。
 * 出参/返回：有 next 时返回它、位置加一并保存 ctx；无 next 时返回 NULL、把位置置 -1UL
 * 且 ctx->stack 置 NULL；不修改链表或 record。
 * 注意事项：无锁且不睡眠，依赖节点只增不删；m->private 必须是本 open 的 stack_print_ctx。
 */
static void *stack_next(struct seq_file *m, void *v, loff_t *ppos)
{
	struct stack *stack = v;
	struct stack_print_ctx *ctx = m->private;
	/* stack 是当前借用节点并被推进到 next；ctx 保存跨 seq 调用的下一节点。 */

	stack = stack->next;
	*ppos = stack ? *ppos + 1 : -1UL;
	ctx->stack = stack;

	return stack;
}

/* debugfs 按页数过滤栈记录的阈值；READ_ONCE/WRITE_ONCE 避免并发读写撕裂。 */
static unsigned long page_owner_pages_threshold;

/*
 * seq show 根据 inode 私有 flags 输出一个 stack record 的栈帧、handle 和/或基础页数。
 * m 是输出流，v 是借用 stack 节点；返回 0。record 为空或低于 threshold 时静默跳过，
 * 计数减一排除首次建链所占的哨兵引用。
 */
/*
 * 业务背景：三个 debugfs 视图共用 show，通过 flags 选择打印符号栈、稳定 handle 和当前基础页数，
 * PAGES 视图还用全局 threshold 过滤低占用记录。
 * 入参：m 是非空 seq 输出，private 为打开实例 ctx；v 是可为有效节点的借用 stack 指针。
 * 出参/返回：恒返回 0；record 缺失或低于阈值时无输出，否则按 flags 顺序追加所选字段和空行；
 * 不修改 refcount、链表或 ctx。
 * 注意事项：record count 的 1 是永久链表基准，不代表页面，故输出先减一；并发 alloc/free 和
 * threshold 写入使结果是近似快照。entries/size 由非零 record 生命周期稳定，show 不取得新引用。
 */
static int stack_print(struct seq_file *m, void *v)
{
	int i, nr_base_pages;
	struct stack *stack = v;
	unsigned long *entries;
	unsigned long nr_entries;
	struct stack_record *stack_record = stack->stack_record;
	struct stack_print_ctx *ctx = m->private;
	/* i/entries/nr_entries 遍历栈帧；nr_base_pages 是扣除基准 1 后的当前归因页数。 */

	/* record 引用计数减一后才是实际被页面占用的基础页数。 */
	if (!stack->stack_record)
		return 0;

	nr_base_pages = refcount_read(&stack_record->count) - 1;

	/* 仅请求 pages 的视图才应用阈值，show_stacks_handles 不应被过滤。 */
	if (ctx->flags & STACK_PRINT_FLAG_PAGES &&
	    (nr_base_pages < 1 || nr_base_pages < page_owner_pages_threshold))
		return 0;

	/* flags 可以组合，按固定顺序输出栈、句柄和页数。 */
	if (ctx->flags & STACK_PRINT_FLAG_STACK) {
		/* 栈帧保存为地址数组，%pS 在输出时解析符号。 */
		nr_entries = stack_record->size;
		entries = stack_record->entries;
		for (i = 0; i < nr_entries; i++)
			seq_printf(m, " %pS\n", (void *)entries[i]);
	}
	if (ctx->flags & STACK_PRINT_FLAG_HANDLE)
		/* handle 允许用户把聚合输出与单页 owner 文本关联。 */
		seq_printf(m, "handle: %d\n", stack_record->handle.handle);
	if (ctx->flags & STACK_PRINT_FLAG_PAGES)
		seq_printf(m, "nr_base_pages: %d\n", nr_base_pages);
	seq_putc(m, '\n');

	return 0;
}

/* stack_list 节点由启动后持续保留，seq 遍历结束无需解锁或释放；无入参语义/返回。 */
/*
 * 业务背景：seq 框架要求 stop 回调处理迭代结束/中断；本协议没有 start 锁或临时节点引用可释放。
 * 入参：m 是借用 seq_file，v 可为当前节点或 NULL，二者均故意不读取。
 * 出参/返回：无直接返回值、无副作用。
 * 注意事项：空实现依赖 stack_list 节点永久不删除；若未来引入删除/锁，必须同步扩展 start/stop。
 */
static void stack_stop(struct seq_file *m, void *v)
{
}

/* seq 操作表把发布同步的 start/next 与格式化 show 注册为一个只读迭代协议。 */
static const struct seq_operations page_owner_stack_op = {
	/* 首次 acquire 表头或从每-open ctx 恢复节点。 */
	.start	= stack_start,
	/* 沿永久 next 前进并维护 -1UL 结束哨兵。 */
	.next	= stack_next,
	/* 当前无迭代锁/引用需要释放。 */
	.stop	= stack_stop,
	/* 按打开文件的 flags 格式化 record。 */
	.show	= stack_print
};

/*
 * 打开 stack debugfs 文件并分配每打开实例独有的 stack_print_ctx。inode->i_private 是创建
 * 文件时编码的 flags，file 接收 seq 私有数据；成功 0，seq_open_private 的错误原样返回。
 * ctx 所有权交给 seq_release_private，open 不得在失败时访问 file->private_data。
 */
/*
 * 业务背景：不同 debugfs 文件复用同一 seq_operations，但各自 inode->i_private 编码不同输出列；
 * open 为每个文件描述符创建独立 ctx，避免并发读者共享迭代位置或 flags。
 * 入参：inode/file 是 VFS 在 open 期间稳定的非空借用对象；inode->i_private 被按 uintptr_t
 * 解释为 STACK_PRINT_FLAG_* 位图，file 用于接收 seq_file 私有状态。
 * 出参/返回：成功返回 0，file->private_data 拥有 seq_file 且其 ctx.flags 已初始化；失败透传
 * seq_open_private errno，未取得可由本函数释放的 ctx ownership。
 * 注意事项：分配可睡眠；只有 ret==0 才能解引用 file->private_data。释放必须走
 * seq_release_private，flags 只允许预定义组合且不应来自用户可控任意指针。
 */
static int page_owner_stack_open(struct inode *inode, struct file *file)
{
	int ret = seq_open_private(file, &page_owner_stack_op,
				   sizeof(struct stack_print_ctx));
	/* ret 是 seq 私有对象分配状态；成功后 m/ctx 仅属于本次打开实例。 */

	if (!ret) {
		/* flags 是创建文件时的常量，复制到每次打开专属 ctx 后可并发读取。 */
		struct seq_file *m = file->private_data;
		struct stack_print_ctx *ctx = m->private;

		ctx->flags = (uintptr_t) inode->i_private;
	}

	return ret;
}

/* stack debugfs 文件的标准 seq 生命周期；release 同时销毁打开时分配的 ctx。 */
static const struct file_operations page_owner_stack_fops = {
	/* 分配 ctx 并从 inode private 复制视图 flags。 */
	.open		= page_owner_stack_open,
	/* seq 框架驱动 start/next/show/stop。 */
	.read		= seq_read,
	/* seq_lseek 保持迭代位置与 ctx 恢复协议。 */
	.llseek		= seq_lseek,
	/* 同时释放 seq_file 和 open 时分配的 stack_print_ctx。 */
	.release	= seq_release_private,
};

/* 读取过滤阈值；data 未使用，val 是调用者输出指针，返回 0。 */
/*
 * 业务背景：DEFINE_SIMPLE_ATTRIBUTE 通过此 getter 向用户态暴露 stack 页数过滤阈值。
 * 入参：data 是未使用且可为 NULL 的私有指针；val 是非空输出指针，调用前内容无要求。
 * 出参/返回：把 page_owner_pages_threshold 的一次 READ_ONCE 快照写入 *val 并返回 0。
 * 注意事项：无锁且不睡眠；READ_ONCE 防止编译器合并/撕裂单值访问，不与并发 setter 建事务。
 */
static int page_owner_threshold_get(void *data, u64 *val)
{
	*val = READ_ONCE(page_owner_pages_threshold);
	return 0;
}

/* 写入过滤阈值；data 未使用，val 为新值，WRITE_ONCE 与并发 stack_print 读取配对，返回 0。 */
/*
 * 业务背景：用户通过 count_threshold 调整仅 PAGES 视图的最小当前基础页数。
 * 入参：data 未使用且可为 NULL；val 是新的无符号基础页阈值，0 表示只保留内置的至少一页条件。
 * 出参/返回：WRITE_ONCE 发布截断为 unsigned long 的阈值并返回 0；无错误类别或 ownership 变化。
 * 注意事项：32 位平台 u64 到 unsigned long 可能截断，这是当前接口行为；无锁更新允许正在进行的
 * stack_print 使用旧值或新值，但单次读取不撕裂。
 */
static int page_owner_threshold_set(void *data, u64 val)
{
	WRITE_ONCE(page_owner_pages_threshold, val);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(page_owner_threshold_fops, &page_owner_threshold_get,
			&page_owner_threshold_set, "%llu");


/*
 * late_initcall 创建 page_owner 及 stack 汇总 debugfs 文件。无入参；返回 0，即使功能关闭
 * 或 debugfs 创建失败也只保留诊断可用性而不影响启动。static key 已是早期 init 的发布条件，
 * 各文件 i_private 编码输出 flags，读回调据此选择栈/句柄/页数视图。
 */
/*
 * 业务背景：page_owner 元数据在更早阶段发布，late_initcall 只建立用户可见 debugfs 入口，
 * 将逐 PFN 视图、三种聚合视图和过滤阈值接到已就绪的只读/读写回调。
 * 入参：无。
 * 出参/返回：始终返回 0；功能关闭时仅打印提示；启用时请求创建根文件、目录和四个子文件，
 * dentry ownership 全部交给 debugfs，创建失败不回滚其它已创建项也不阻止启动。
 * 注意事项：仅 late init 可睡眠上下文调用一次；debugfs_create_* 的错误指针/NULL 在当前代码中
 * 不检查，体现“诊断接口尽力可用”边界。文件权限决定 page_owner 只读、threshold 可读写。
 */
static int __init pageowner_init(void)
{
	struct dentry *dir;
	/* dir 是 debugfs_create_dir 返回的借用/错误 dentry，仅传给随后创建调用，不由本函数释放。 */

	/* 未启用时不创建空接口，避免用户误以为数据完整。 */
	if (!static_branch_unlikely(&page_owner_inited)) {
		pr_info("page_owner is disabled\n");
		return 0;
	}

	/* 先创建 PFN 逐条接口，再在目录下创建按 stack 聚合的三个只读视图和可写阈值。 */
	debugfs_create_file("page_owner", 0400, NULL, NULL, &page_owner_fops);
	/* 目录归属 debugfs；创建失败只让接口缺失，不能影响内核内存管理。 */
	dir = debugfs_create_dir("page_owner_stacks", NULL);
	debugfs_create_file("show_stacks", 0400, dir,
		/* 该视图同时请求栈帧和页数阈值过滤。 */
			    (void *)(STACK_PRINT_FLAG_STACK |
				     STACK_PRINT_FLAG_PAGES),
			     &page_owner_stack_fops);
	debugfs_create_file("show_handles", 0400, dir,
			    (void *)(STACK_PRINT_FLAG_HANDLE |
				     STACK_PRINT_FLAG_PAGES),
			    &page_owner_stack_fops);
	debugfs_create_file("show_stacks_handles", 0400, dir,
		/* 该视图故意不含 PAGES 标志，因此显示所有 handle 而不应用阈值。 */
			    (void *)(STACK_PRINT_FLAG_STACK |
				     STACK_PRINT_FLAG_HANDLE),
			    &page_owner_stack_fops);
	debugfs_create_file("count_threshold", 0600, dir, NULL,
			    &page_owner_threshold_fops);
	return 0;
}
late_initcall(pageowner_init)
