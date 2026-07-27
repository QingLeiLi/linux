// SPDX-License-Identifier: GPL-2.0
/*
 * early_ioremap 通用启动期临时映射学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 【为什么需要】
 * 内核刚进入 setup_arch() 时，固件表、设备寄存器、initrd 等对象只有物理
 * 地址，而 vmalloc/ioremap 基础设施和普通内存分配器尚未可用。本文件借用
 * 体系结构预留的 fixmap 虚拟地址区，直接改少量已准备好的页表项，为调用者
 * 提供容量有限、必须成对释放的临时映射。
 *
 * 【主调用链】
 *   体系结构 early_ioremap_init()
 *       -> early_ioremap_setup()：计算每个 slot 的固定虚拟基址
 *
 *   early_ioremap()/early_memremap[_ro|_prot]()
 *       -> __early_ioremap()
 *          -> 领取空闲 slot
 *          -> __early_set_fixmap()/__late_set_fixmap() 逐页写 PTE
 *          -> 返回 slot 基址 + 原物理地址的页内偏移
 *
 *   early_iounmap()/early_memunmap()
 *       -> 核对地址和原始长度
 *       -> 逐页清除 fixmap PTE/TLB
 *       -> 归还 slot
 *
 * paging_init() 末尾可由体系结构调用 early_ioremap_reset()，让后续请求改走
 * late fixmap hook；不支持该阶段继续映射的体系结构会以 BUG 明确拒绝。常规
 * ioremap() 可用后，新代码不应继续依赖这个启动期资源池。
 *
 * 【核心状态与生命周期】
 * FIX_BTMAPS_SLOTS 个并行 slot 各自拥有 NR_FIX_BTMAPS 个连续虚拟页。
 * slot_virt[] 保存永不动态分配的虚拟基址；prev_map[] 非 NULL 表示 slot 已
 * 发布给调用者；prev_size[] 保存调用者传入的原始字节长度，要求 unmap 精确
 * 配对。映射并不取得底层物理内存所有权，也不延长固件表/设备的生命周期。
 * 全部函数和状态带 __init/__initdata，free_initmem() 后不可再使用。
 *
 * 【并发、页表与权衡】
 * 代码不加锁：其契约是启动 CPU 串行调用，调用者可以嵌套占用不同 slot，
 * 但不能由多个 CPU 并发领取同一数组。架构 __set_fixmap 实现负责合法地写/
 * 清 PTE，并按本架构要求失效 TLB；本文件不把清 PTE、TLB invalidation、
 * cache maintenance 和释放物理页混为一件事。固定池无需动态分配，适合极早
 * 启动，代价是同时映射数和单次映射大小都有硬上限，泄漏最终会耗尽 slot。
 * CONFIG_MMU=n 时体系结构保证物理地址可直接当作 CPU 地址使用，API 退化为
 * 强制类型转换，既不建立页表，也不能真正实施只读/设备 cache 属性。
 */
/*
 * Provide common bits of early_ioremap() support for architectures needing
 * temporary mappings during boot before ioremap() is available.
 *
 * This is mostly a direct copy of the x86 early_ioremap implementation.
 *
 * (C) Copyright 1995 1996, 2014 Linus Torvalds
 *
 */
/*
 * 本文件提供需要“普通 ioremap 尚不可用时临时访问物理地址”的体系结构公共
 * 实现，算法源自 x86：预留一小段 fixmap 地址，以静态 slot 元数据管理映射。
 * 上游版权块仅说明来源；具体 PTE 编码、TLB 失效和 fixmap 布局仍由各架构实现。
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <asm/fixmap.h>
#include <asm/early_ioremap.h>
#include "internal.h"

#ifdef CONFIG_MMU
/*
 * 以下主实现仅用于有 MMU 的内核：虚拟地址与物理地址需要页表翻译。关闭 MMU
 * 时文件末尾提供直接地址版本，不能把这里的 slot/PTE 语义套到该配置。
 */

/*
 * early_ioremap_debug 命令行开关。
 *
 * 由 early_param 解析器在启动期写一次，所有映射/撤销路径只读；非零时每次
 * 操作打印参数并 dump_stack，便于从泄漏报告回溯未配对调用者。它不是并发
 * 调试锁，且位于 __initdata，初始化内存回收后消失。
 */
static int early_ioremap_debug __initdata;

/*
 * 处理 early_ioremap_debug 启动参数。
 *
 * @str：命令行参数值的借用字符串，可为空；此开关只关心参数是否出现，故不
 *       解析内容、不保存指针。返回 0 表示参数已成功消费。
 *
 * early parameter 解析发生在单 CPU 启动上下文，不持锁、不睡眠。唯一副作用
 * 是发布 early_ioremap_debug=1，影响随后映射/撤销的诊断输出。
 */
static int __init early_ioremap_debug_setup(char *str)
{
	early_ioremap_debug = 1;

	return 0;
}
/*
 * early_param 在早期命令行扫描表中登记上述回调；它不会生成普通 module_param，
 * 因而系统运行后不能再通过 sysfs 修改该开关。
 */
early_param("early_ioremap_debug", early_ioremap_debug_setup);

/*
 * 调试包装宏：开关关闭时只剩一次 unlikely() 分支；开启时打印本次操作并输出
 * 当前栈。do/while 形式使宏在 if/else 等语句环境中仍表现为单条语句。
 * args 前的 ## 允许调用者没有可变参数。宏只诊断，不改变映射状态。
 */
#define early_ioremap_dbg(fmt, args...)			\
	do {						\
		if (unlikely(early_ioremap_debug)) {	\
			pr_warn(fmt, ##args);		\
			dump_stack();			\
		}					\
	} while (0)

/*
 * 标记体系结构是否已经宣告完成早期 paging 初始化。
 *
 * 0 使用 __early_set_fixmap；early_ioremap_reset() 写 1 后改用 late hooks。
 * 这是启动顺序状态而非并发同步变量，单 CPU 协议下无需 READ_ONCE/锁。
 */
static int after_paging_init __initdata;

/*
 * 允许体系结构调整“普通内存”早期映射的页保护属性。
 *
 * @phys_addr：待映射物理起始字节地址。
 * @size：调用者请求的原始字节数，可跨页；本 hook 可能先看到 0，随后核心
 *        映射才会拒绝零长度。
 * @prot：通用层选择的初始属性，纯输入值。
 *
 * weak 默认实现原样返回 @prot，无副作用、不睡眠。x86 的强实现会结合内存
 * 加密状态和物理区间，把 EFI/setup data 等映射调整为 encrypted/decrypted。
 * 该 hook 只选择 PTE 属性，不建立映射、不拥有物理内存，也不验证区间大小。
 */
pgprot_t __init __weak early_memremap_pgprot_adjust(resource_size_t phys_addr,
						    unsigned long size,
						    pgprot_t prot)
{
	return prot;
}

/*
 * 通知通用层 paging_init 的体系结构转换点已经到达。
 *
 * 无入参、无直接返回值。调用者通常是 setup_arch()/paging_init() 的尾部；
 * 函数仅把 after_paging_init 置 1。之后尚存的 early map 在 unmap 时也必须走
 * late clear hook，新的 map 走 late set hook，以匹配已经切换完成的页表布局。
 *
 * 支持 late hooks 的架构可继续临时映射；没有提供 hooks 的架构会在下一次
 * map/unmap 时进入下面的 BUG stub。因此调用者应在 reset 前释放不允许跨越
 * 页表转换的映射。单 CPU 启动上下文，无锁、不睡眠。
 */
void __init early_ioremap_reset(void)
{
	after_paging_init = 1;
}

/*
 * Generally, ioremap() is available after paging_init() has been called.
 * Architectures wanting to allow early_ioremap after paging_init() can
 * define __late_set_fixmap and __late_clear_fixmap to do the right thing.
 */
/*
 * 通常 paging_init() 后应改用完整 ioremap()。若架构仍允许使用本 API，必须
 * 定义 late set/clear，使其操作当前正式页表并完成相应 TLB 协议；否则这里
 * 提供的默认实现以 BUG 阻止静默写入已经失效的早期页表。
 */
#ifndef __late_set_fixmap
/*
 * 不支持 late map 的体系结构兜底。
 *
 * @idx、@phys、@prot 分别是 fixmap 槽号、页对齐物理字节地址和 PTE 属性；
 * 函数不返回，任何调用都说明启动顺序契约被破坏并触发 BUG。参数不会被消费。
 */
static inline void __init __late_set_fixmap(enum fixed_addresses idx,
					    phys_addr_t phys, pgprot_t prot)
{
	BUG();
}
#endif

#ifndef __late_clear_fixmap
/*
 * 不支持 late unmap 的体系结构兜底。
 *
 * @idx 是待清除 fixmap 槽号。进入即 BUG，避免调用者误以为映射已撤销、实际
 * TLB/PTE 却仍指向旧物理页。
 */
static inline void __init __late_clear_fixmap(enum fixed_addresses idx)
{
	BUG();
}
#endif

/*
 * slot 元数据（均只在启动期有效）：
 * - prev_map[i]：slot i 当前发布给调用者的带页内偏移虚拟地址；NULL 表示空闲。
 * - prev_size[i]：该映射 API 接收的原始字节长度；unmap 必须原值匹配。
 * - slot_virt[i]：slot i 第一个虚拟页的固定基址，由 setup 一次性计算。
 *
 * 三个数组共同形成映射账本，但不持有物理页引用。只有 prev_map 是占用提交
 * 标志；核心映射在全部 PTE 建好后才写它，撤销全部 PTE 后才清零。启动串行
 * 约束替代锁，任何并发调用都会在空闲扫描和发布之间产生重复领取竞态。
 */
static void __iomem *prev_map[FIX_BTMAPS_SLOTS] __initdata;
static unsigned long prev_size[FIX_BTMAPS_SLOTS] __initdata;
static unsigned long slot_virt[FIX_BTMAPS_SLOTS] __initdata;

/*
 * 初始化每个 boot-time fixmap slot 的虚拟基址。
 *
 * 由体系结构 early_ioremap_init() 或等价早期入口在 fixmap 布局确定后调用；
 * 某些架构会在本函数返回后才把预留 PTE 表接入上级页表，因此“slot 元数据
 * 就绪”不等于此刻一定已经能够映射。无入参和直接返回值；循环为每个 slot
 * 计算：
 *   __fix_to_virt(FIX_BTMAP_BEGIN - NR_FIX_BTMAPS * i)
 * 相邻 slot 因而相隔 NR_FIX_BTMAPS 页，互不覆盖。
 *
 * prev_map[] 依赖静态零初始化。WARN_ON_ONCE 检测重复 setup 时仍有映射，但
 * 不清除或接管它；函数仍重算基址。无分配、不睡眠、无锁，成功后 map API
 * 才拥有完整的 slot 地址地图。
 */
void __init early_ioremap_setup(void)
{
	/* i 是 slot 下标，范围 [0, FIX_BTMAPS_SLOTS)，离开循环即失效。 */
	int i;

	for (i = 0; i < FIX_BTMAPS_SLOTS; i++) {
		WARN_ON_ONCE(prev_map[i]);
		/*
		 * fixmap 的索引增大时 VA 下降；每次减去一个 slot 的页数，可得到
		 * 下一段更高的连续 VA 起点，供核心映射按 idx-- 向高地址铺 PTE。
		 */
		slot_virt[i] = __fix_to_virt(FIX_BTMAP_BEGIN - NR_FIX_BTMAPS*i);
	}
}

/*
 * 在 late initcall 阶段检查是否遗留未撤销的 early mapping。
 *
 * 无入参。count 汇总 prev_map[] 中仍占用的 slot，i 是遍历下标。返回 0 表示
 * 无泄漏；存在泄漏时 WARN 打印数量并返回 1，使 initcall 调试结果标出失败，
 * 但不会自动清 PTE：此时无法知道泄漏调用者是否仍在使用地址，强行回收可能
 * 制造故障。建议通过 early_ioremap_debug 的逐次栈回溯定位。
 *
 * late_initcall 仍早于 init memory 回收，因此 __initdata 数组有效。启动串行
 * 阶段无需锁；检查本身不睡眠、不改变 slot 所有权。
 */
static int __init check_early_ioremap_leak(void)
{
	/* count 是占用 slot 数，最大 FIX_BTMAPS_SLOTS；i 是数组游标。 */
	int count = 0;
	int i;

	for (i = 0; i < FIX_BTMAPS_SLOTS; i++)
		if (prev_map[i])
			count++;

	if (WARN(count, KERN_WARNING
		 "Debug warning: early ioremap leak of %d areas detected.\n"
		 "please boot with early_ioremap_debug and report the dmesg.\n",
		 count))
		return 1;
	return 0;
}
/* 把泄漏审计安排在内核 late initcall 阶段，而不是每次 unmap 都全表扫描。 */
late_initcall(check_early_ioremap_leak);

/*
 * 在一个 boot-time fixmap slot 中建立临时物理映射。
 *
 * @phys_addr：待访问物理区间起始字节地址，可不页对齐，不转移底层内存
 *             所有权。
 * @size：请求字节数，必须非零且区间不能回绕；调用者必须把同一原值传给
 *        early_iounmap()/early_memunmap()。
 * @prot：纯输入 PTE 属性，由 IO/normal/RO/prot wrapper 选择。
 *
 * 返回带 __iomem 语义的临时 VA，地址已包含原物理页内偏移；失败返回 NULL。
 * 可恢复失败只有无空 slot、零长度/地址回绕、或超过单 slot 页数。架构
 * set_fixmap hook 为 void，通用层无法观察其内部 WARN/拒绝并转换成 NULL；
 * 调用者必须满足架构页表和属性前置条件，否则返回地址不具备可靠映射保证，
 * 严重违约还可能 BUG。函数不分配、不睡眠、不加锁，只允许启动串行上下文。
 *
 * 成功后 prev_map/prev_size 账本把该 slot 的临时使用权交给调用者；底层物理
 * 对象仍归原所有者。失败发生在发布 prev_map 前，slot 仍可再次领取。
 */
static void __init __iomem *
__early_ioremap(resource_size_t phys_addr, unsigned long size, pgprot_t prot)
{
	/*
	 * 变量地图：
	 *   offset    原物理地址在首页中的字节偏移，最终加回返回 VA。
	 *   last_addr 请求区间最后一个物理字节，用于检测加法回绕和计算闭区间。
	 *   nrpages   对齐后需要写入的 PTE 数，必须不超过单 slot 容量。
	 *   idx       当前待写的 fixmap enum 索引；idx-- 对应连续上升的虚拟页。
	 *   i/slot    空闲扫描游标与最终领取的 slot；-1 表示尚未找到。
	 */
	unsigned long offset;
	resource_size_t last_addr;
	unsigned int nrpages;
	enum fixed_addresses idx;
	int i, slot;

	/*
	 * SYSTEM_RUNNING 表示正常并发运行期已经开始，本 API 的无锁 __initdata
	 * 账本不再安全。WARN 保留故障现场但不直接返回，便于特殊启动路径继续；
	 * 正常调用者不能把该告警当成运行期并发支持。
	 */
	WARN_ON(system_state >= SYSTEM_RUNNING);

	/* 阶段 1：以 prev_map==NULL 为唯一空闲判据，领取第一个可用 slot。 */
	slot = -1;
	for (i = 0; i < FIX_BTMAPS_SLOTS; i++) {
		if (!prev_map[i]) {
			slot = i;
			break;
		}
	}

	if (WARN(slot < 0, "%s(%pa, %08lx) not found slot\n",
		 __func__, &phys_addr, size))
		return NULL;

	/* Don't allow wraparound or zero size */
	/*
	 * 先形成闭区间 [phys_addr, last_addr]。size==0 会使减一语义无效；
	 * phys_addr+size-1 小于起点则说明 resource_size_t 加法回绕。两者都在
	 * 写 PTE 前拒绝，因此 prev_map 仍为空，slot 可被下次请求重新领取。
	 */
	last_addr = phys_addr + size - 1;
	if (WARN_ON(!size || last_addr < phys_addr))
		return NULL;

	/*
	 * 账本保存“API 原始长度”而不是稍后页对齐的长度，确保 unmap 能发现调用
	 * 者用错配对参数。此时 prev_map 尚未发布；后续容量校验失败只留下一个
	 * 不参与空闲判定的旧 prev_size 值，不会占住 slot。
	 */
	prev_size[slot] = size;
	/*
	 * Mappings have to be page-aligned
	 */
	/*
	 * PTE 只能映射整页：offset 记录首页前缀，phys_addr 向下对齐，size 扩为
	 * 从对齐首页到原闭区间末尾之后的整页字节数。返回时加回 offset，使调用
	 * 者看到的第一个字节仍精确对应原始 phys_addr。
	 */
	offset = offset_in_page(phys_addr);
	phys_addr &= PAGE_MASK;
	size = PAGE_ALIGN(last_addr + 1) - phys_addr;

	/*
	 * Mappings have to fit in the FIX_BTMAP area.
	 */
	/*
	 * 每个 slot 只有 NR_FIX_BTMAPS 个 PTE，不能把一个请求拆到其他 slot，
	 * 因为 prev_map/prev_size 只记录单一所有者。size 已页对齐，右移得到精确
	 * 页数；超限时尚未写任何 PTE，直接失败。
	 */
	nrpages = size >> PAGE_SHIFT;
	if (WARN_ON(nrpages > NR_FIX_BTMAPS))
		return NULL;

	early_ioremap_dbg("%s(%pa, %08lx) [%d] => %08lx + %08lx\n",
			  __func__, &phys_addr, size, slot, slot_virt[slot], offset);

	/*
	 * Ok, go for it..
	 */
	/*
	 * 阶段 2：slot 的第一个 fixmap 索引由 BEGIN 减去前面所有 slot 的容量。
	 * __fix_to_virt() 与索引方向相反，所以每映射下一个物理页都执行 idx--，
	 * 得到连续递增的虚拟页。after_paging_init 只选择架构页表 hook，不改变
	 * slot 账本协议；hook 自身负责 PTE 写入及本架构要求的 TLB 处理。
	 */
	idx = FIX_BTMAP_BEGIN - NR_FIX_BTMAPS*slot;
	while (nrpages > 0) {
		if (after_paging_init)
			__late_set_fixmap(idx, phys_addr, prot);
		else
			__early_set_fixmap(idx, phys_addr, prot);
		phys_addr += PAGE_SIZE;
		--idx;
		--nrpages;
	}

	/*
	 * 阶段 3（发布）：全部 PTE 已建立后才写 prev_map。返回地址是 slot 首页
	 * VA 加 offset；调用者借用该临时地址，必须在底层对象仍有效时用相同 size
	 * 归还。启动串行协议下无需 release barrier；若引入并发，仅此赋值不足以
	 * 防止两个 CPU 同时领取 slot。
	 */
	prev_map[slot] = (void __iomem *)(offset + slot_virt[slot]);
	return prev_map[slot];
}

/*
 * 撤销一段 early_ioremap/early_memremap 临时映射并归还 slot。
 *
 * @addr：map API 原样返回的借用 VA，不能为空且必须精确匹配某个 prev_map；
 *        函数不接受区间内部地址。
 * @size：map API 当时传入的原始字节数，必须精确匹配 prev_size。
 *
 * 成功时按 addr 页内偏移重新计算页数，逐页调用 early/late clear hook 清 PTE
 * 并由架构完成所需 TLB invalidation，最后把 prev_map[slot] 清为 NULL，使
 * slot 可复用。它不释放、回写或改变底层物理内存。
 *
 * 地址未找到、长度不一致或 VA 越界时只 WARN 并保留原映射/占用状态：在无法
 * 确定正确范围时，泄漏比清错邻接 slot 的 PTE 更安全。函数无直接返回值，
 * 不睡眠、不加锁，只适用于启动串行上下文。
 */
void __init early_iounmap(void __iomem *addr, unsigned long size)
{
	/*
	 * 变量地图：
	 *   virt_addr addr 的整数形式，用于页内偏移/边界计算，不改变 ownership。
	 *   offset    返回 VA 在 slot 首页中的字节偏移。
	 *   nrpages   覆盖 offset+原始 size 所需清除的 PTE 数。
	 *   idx       当前待清除的 fixmap 索引。
	 *   i/slot    账本反查游标和匹配 slot，-1 表示未找到。
	 */
	unsigned long virt_addr;
	unsigned long offset;
	unsigned int nrpages;
	enum fixed_addresses idx;
	int i, slot;

	/* 阶段 1：只有 map 原样返回的地址才拥有撤销该 slot 的资格。 */
	slot = -1;
	for (i = 0; i < FIX_BTMAPS_SLOTS; i++) {
		if (prev_map[i] == addr) {
			slot = i;
			break;
		}
	}

	if (WARN(slot < 0, "%s(%p, %08lx) not found slot\n",
		  __func__, addr, size))
		return;

	/*
	 * 长度是映射账本的一部分。若不相等，按较小值清会遗留 PTE，按较大值清会
	 * 越过 slot，因此拒绝两种猜测并让 late leak checker 报告占用。
	 */
	if (WARN(prev_size[slot] != size,
		 "%s(%p, %08lx) [%d] size not consistent %08lx\n",
		  __func__, addr, size, slot, prev_size[slot]))
		return;

	early_ioremap_dbg("%s(%p, %08lx) [%d]\n", __func__, addr, size, slot);

	/*
	 * slot 反查已经证明 addr 来自本模块；下界检查再防御账本损坏或异常架构
	 * fixmap 布局。fix_to_virt 同时带编译期合法索引检查。
	 */
	virt_addr = (unsigned long)addr;
	if (WARN_ON(virt_addr < fix_to_virt(FIX_BTMAP_BEGIN)))
		return;

	/* 重建与 map 端相同的“首页前缀 + 原始长度”覆盖页数。 */
	offset = offset_in_page(virt_addr);
	nrpages = PAGE_ALIGN(offset + size) >> PAGE_SHIFT;

	/*
	 * 阶段 2（撤销）：索引计算必须与 map 完全对称。paging 转换前没有独立
	 * early clear 接口，故以 phys=0、FIXMAP_PAGE_CLEAR 调用 set hook；转换后
	 * 使用架构 late clear。清页表/TLB 不等于释放底层物理页。
	 */
	idx = FIX_BTMAP_BEGIN - NR_FIX_BTMAPS*slot;
	while (nrpages > 0) {
		if (after_paging_init)
			__late_clear_fixmap(idx);
		else
			__early_set_fixmap(idx, 0, FIXMAP_PAGE_CLEAR);
		--idx;
		--nrpages;
	}
	/*
	 * 阶段 3（归还）：只有全部 PTE 清完后才清占用标志。prev_size 无需清零，
	 * 因为空闲判据只看 prev_map，下一次成功领取会覆盖它。
	 */
	prev_map[slot] = NULL;
}

/* Remap an IO device */
/*
 * 为设备 MMIO 建立临时映射。
 *
 * @phys_addr/@size 是设备物理字节区间，语义与 __early_ioremap() 相同；返回
 * __iomem 借用指针或 NULL。FIXMAP_PAGE_IO 选择架构的 device/uncached 等属性，
 * 调用者必须使用 readl()/writel() 等 MMIO accessor，并以 early_iounmap()
 * 原地址、原长度配对。函数仅固定策略，不新增资源或失败类别。
 */
void __init __iomem *
early_ioremap(resource_size_t phys_addr, unsigned long size)
{
	return __early_ioremap(phys_addr, size, FIXMAP_PAGE_IO);
}

/* Remap memory */
/*
 * 为 RAM/固件表类物理内存建立可读写临时映射。
 *
 * @phys_addr/@size 是物理字节区间；返回普通 void 借用指针或 NULL，调用者用
 * early_memunmap() 精确配对。先让体系结构依据地址范围调整 FIXMAP_PAGE_NORMAL
 * （例如 x86 内存加密位），再进入统一 slot/PTE 状态机。
 *
 * __force 明确跨越 sparse 的 __iomem 类型域：核心函数复用统一返回类型，但
 * 此 wrapper 的页属性和契约确认目标是 memory，可由普通 memcpy/解引用访问。
 */
void __init *
early_memremap(resource_size_t phys_addr, unsigned long size)
{
	/* prot 是按本物理区间调整后的 PTE 属性值，只在本次调用中有效。 */
	pgprot_t prot = early_memremap_pgprot_adjust(phys_addr, size,
						     FIXMAP_PAGE_NORMAL);

	return (__force void *)__early_ioremap(phys_addr, size, prot);
}
#ifdef FIXMAP_PAGE_RO
/*
 * 只在体系结构能表达 FIXMAP_PAGE_RO 时编译；这避免用“普通可写映射”伪装只读
 * 契约。MMU 路径由 PTE 强制只读，无 MMU 路径则无法提供同等硬件保护。
 */
/*
 * 为固件表等只读物理内存建立临时映射。
 *
 * 参数、NULL 失败和配对规则同 early_memremap()；区别是初始属性为
 * FIXMAP_PAGE_RO，体系结构仍可追加加密等区间属性。返回的 C 类型不是 const，
 * 但 MMU 页表会阻止普通写入；调用者仍应把内容视为借用的只读快照。
 */
void __init *
early_memremap_ro(resource_size_t phys_addr, unsigned long size)
{
	/* prot 同时包含架构只读基线和按物理区间选择的附加属性。 */
	pgprot_t prot = early_memremap_pgprot_adjust(phys_addr, size,
						     FIXMAP_PAGE_RO);

	return (__force void *)__early_ioremap(phys_addr, size, prot);
}
#endif

#ifdef CONFIG_ARCH_USE_MEMREMAP_PROT
/*
 * 仅向明确选择 CONFIG_ARCH_USE_MEMREMAP_PROT 的体系结构开放任意 prot_val，
 * 因为调用者必须理解本架构的原始 PTE 位编码；通用代码不能验证其 cache、
 * 加密或只读组合是否对目标物理区间合法。
 */
/*
 * 以调用者指定的原始页保护值建立临时 memory 映射。
 *
 * @phys_addr/@size 遵循统一区间和配对契约。
 * @prot_val 是体系结构页表属性的无类型整数表示，纯输入；__pgprot() 只包装
 * 类型，不做合法性修正。返回普通借用指针或 NULL，由 early_memunmap() 释放。
 *
 * 与 early_memremap() 不同，本函数不调用 early_memremap_pgprot_adjust()；
 * 加密/解密、cache 和写保护选择完全由调用者负责。
 */
void __init *
early_memremap_prot(resource_size_t phys_addr, unsigned long size,
		    unsigned long prot_val)
{
	return (__force void *)__early_ioremap(phys_addr, size,
					       __pgprot(prot_val));
}
#endif

/*
 * 单个 slot 可映射的最大整页字节数。它是 copy_from_early_mem() 的分块上限，
 * 不是所有 slot 总容量；左移 PAGE_SHIFT 将页数转换为字节。
 */
#define MAX_MAP_CHUNK	(NR_FIX_BTMAPS << PAGE_SHIFT)

/*
 * If no empty slot, handle that and return -ENOMEM.
 */
/*
 * 从尚无永久映射的物理内存分块复制到已映射内核缓冲区。
 *
 * @dest：输出缓冲区起点，调用者拥有且必须至少容纳 @size 字节；不能为空（除非
 *        size 为 0），函数不接管其所有权。
 * @src：源物理字节地址，可不页对齐；只借用底层内容。
 * @size：要复制的字节总数；0 是合法空操作。
 *
 * 每轮最多占用一个 slot：根据 src 页内前缀缩小有效 payload，把页对齐源范围
 * early_memremap()，memcpy 后立即 unmap，再推进三个游标。这样可复制远大于
 * 单 slot 的对象，同时最多占用一个额外 slot。
 *
 * 返回 0 表示全部字节已复制；任一分块无法取得 slot 时返回 -ENOMEM。此前分块
 * 已写入 dest，不会回滚，调用者必须把失败视为“输出前缀有效但整体未完成”。
 * 每轮映射都在返回前撤销，因此失败时本函数不泄漏 slot。无锁、不睡眠，只适用
 * 启动串行上下文。
 */
int __init copy_from_early_mem(void *dest, phys_addr_t src, unsigned long size)
{
	/*
	 * 变量地图：
	 *   slop src 在物理首页中的字节偏移。
	 *   clen 本轮真正复制的 payload 字节数。
	 *   p    对齐物理首页的临时普通内存映射，成功后本轮必须 unmap。
	 */
	unsigned long slop, clen;
	char *p;

	while (size) {
		/*
		 * MAX_MAP_CHUNK 是映射总跨度，首页前缀 slop 也占 slot 空间，所以
		 * payload 最多为 MAX_MAP_CHUNK-slop，保证 clen+slop 不越过容量。
		 */
		slop = offset_in_page(src);
		clen = size;
		if (clen > MAX_MAP_CHUNK - slop)
			clen = MAX_MAP_CHUNK - slop;
		/*
		 * 映射从页边界开始，p 因而无页内偏移；复制源为 p+slop。失败发生
		 * 在本轮 memcpy 前，之前轮次的映射均已撤销。
		 */
		p = early_memremap(src & PAGE_MASK, clen + slop);
		if (!p)
			return -ENOMEM;
		memcpy(dest, p + slop, clen);
		early_memunmap(p, clen + slop);
		/*
		 * 三个游标以同一 clen 前进，保持不变量：
		 *   已复制字节 + size == 初始 size。
		 * GNU C 允许 void * 的加法按字节推进；src 本身就是字节物理地址。
		 */
		dest += clen;
		src += clen;
		size -= clen;
	}
	return 0;
}

#else /* CONFIG_MMU */
/*
 * CONFIG_MMU=n 的替代实现：CPU 地址空间不依赖本文件动态写页表，故物理地址
 * 可按体系结构约定直接作为访问地址。这里没有 slot 容量、PTE/TLB 或泄漏账本；
 * size 只为保持 API 一致而存在。调用者仍应保持 map/unmap 配对，便于跨配置代码
 * 具有统一生命周期。
 */

/*
 * 无 MMU 设备映射。
 *
 * @phys_addr：设备物理字节地址，必须能由本配置的指针宽度直接表示。
 * @size：请求字节数，本实现不检查也不消费。
 * 返回把地址值标为 __iomem 的借用指针；没有校验、分配或页表副作用，物理
 * 地址 0 也会自然得到 NULL 数值。cache/device ordering 必须由体系结构静态
 * 地址属性和 MMIO accessor 保证，本函数不提供额外保证。
 */
void __init __iomem *
early_ioremap(resource_size_t phys_addr, unsigned long size)
{
	/* __force 表明这是有意把物理整数解释为 sparse 的 MMIO 地址域。 */
	return (__force void __iomem *)phys_addr;
}

/* Remap memory */
/*
 * 无 MMU 普通内存映射。
 *
 * @phys_addr/@size 与 early_ioremap() 相同；返回可普通解引用的借用指针。
 * 本实现不建立映射或主动报告失败，地址 0 会得到 NULL 数值；函数不获得
 * 底层内存所有权。
 */
void __init *
early_memremap(resource_size_t phys_addr, unsigned long size)
{
	return (void *)phys_addr;
}

/*
 * 无 MMU 只读内存映射兼容入口。
 *
 * 参数和返回同 early_memremap()。没有页表就无法在这里施加只读 PTE，因此
 * “ro”仅表达调用者不得写的接口契约，不提供本文件层面的硬件写保护。
 */
void __init *
early_memremap_ro(resource_size_t phys_addr, unsigned long size)
{
	return (void *)phys_addr;
}

/*
 * 无 MMU 撤销兼容入口。
 *
 * @addr 是先前返回的借用 MMIO 地址，@size 是原始字节数；两者均不消费。
 * 无页表和 slot 可撤销，故函数为空。无返回值、无副作用、不睡眠。
 */
void __init early_iounmap(void __iomem *addr, unsigned long size)
{
}

#endif /* CONFIG_MMU */
/*
 * 至此两种配置重新汇合：MMU 配置真正撤销 fixmap，非 MMU 配置为空操作；
 * 上层 early_memunmap() 无需重复条件编译。
 */


/*
 * 普通内存映射的统一撤销包装。
 *
 * @addr：early_memremap[_ro|_prot]() 原样返回的借用地址。
 * @size：建立映射时的原始字节数，MMU 配置必须精确匹配。
 * 无直接返回值。MMU 配置转入 early_iounmap() 清 PTE/TLB 并归还 slot；
 * 非 MMU 配置最终为空操作。__force 只消除 sparse 地址域差异，不改变数值。
 *
 * 调用者在此后不得继续解引用 addr；函数不负责释放底层物理内存。
 */
void __init early_memunmap(void *addr, unsigned long size)
{
	early_iounmap((__force void __iomem *)addr, size);
}
