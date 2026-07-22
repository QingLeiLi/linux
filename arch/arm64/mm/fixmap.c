// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 fixmap 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。该标识只说明新增中文注释的来源，
 * 不属于上游作者或版权信息。
 *
 * 【背景：为什么需要 fixmap】
 * 内核有时在通用内存管理尚未可用、不能调用 ioremap/vmalloc，或者在 IRQ 等不能睡眠
 * 的上下文中，仍需临时访问一个已知物理页。例如 ARM64 启动早期必须先读取 bootloader
 * 传入的 DTB，才能发现系统内存；但建立普通映射所需的内存信息恰好就在 DTB 中。
 *
 * fixmap 用“编译期固定虚拟槽位 + 运行时可替换物理页”打破这个循环：
 *
 *   enum fixed_addresses 中的 idx
 *       -> __fix_to_virt(idx) = FIXADDR_TOP - idx * PAGE_SIZE
 *       -> 一个编译期可计算的虚拟页
 *       -> __set_fixmap(idx, phys, prot) 写入当前物理页及权限
 *
 * 槽位编号越大，虚拟地址越低；每个编号代表一页。调用者可以稳定持有“槽位地址”，
 * 但槽位当前映射哪个物理页取决于页表中的叶子 PTE。
 *
 * 【本文件职责与主路径】
 * 本文件不定义槽位布局；布局在 <asm/fixmap.h> 的 enum fixed_addresses 中。本文件负责：
 *
 *   setup_arch()
 *     -> early_fixmap_init()
 *        -> 把静态 bm_pud/bm_pmd/bm_pte 接入 init_mm.pgd
 *     -> fixmap_remap_fdt()
 *        -> 先映射 DTB 首页并校验 header
 *        -> 根据 totalsize 扩展映射
 *
 *   set_fixmap()/clear_fixmap()/GHES/KVM/entry trampoline
 *     -> __set_fixmap()
 *        -> 在预建页表中写入或清除单页 PTE
 *        -> 清除时刷新该页的内核 TLB
 *
 * 【核心对象】
 * bm_pte[] 是所有 fixmap 槽位最终落到的叶子页表；bm_pmd/bm_pud 是配置需要时的上级
 * 表。它们位于页对齐的 .bss..pgtbl，启动前已清零，生命周期覆盖整个内核运行期，
 * 不经过页分配器，也不释放。init_mm.pgd 是它们最终接入的内核顶级页表根。
 *
 * 【并发与不变量】
 * early_fixmap_init() 由启动 CPU 单线程执行，不需要锁。运行期 __set_fixmap() 甚至可能
 * 从 IRQ 调用，因此不能睡眠；本函数自身没有槽位锁，调用者必须保证不会并发重用同一
 * idx。不同用途通过不同固定槽位避免冲突。取消映射后必须做 TLB invalidation，否则
 * CPU 可能继续使用旧物理页；未来若 ARM64 TLB 广播需要 IPI，IRQ 调用约束会成为问题。
 *
 * 【方案权衡】
 * 收益是虚拟地址稳定、热路径只需修改一个 PTE，且启动前无需动态分配页表；代价是槽位
 * 数量和页表容量必须编译期预留，调用者需自行管理槽位并发，固定窗口也会永久占用一段
 * 内核虚拟地址空间。fixmap 适合小而特殊的映射，不是通用 ioremap/vmalloc 的替代品。
 */
/*
 * Fixmap manipulation code
 */
/*
 * 原注释概括了本文件边界：这里实现 ARM64 fixmap 页表的早期搭建与叶子映射修改；
 * 固定地址枚举、通用 set_fixmap 包装和索引换算分别由架构头文件及 asm-generic 提供。
 */

#include <linux/bug.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/sizes.h>

#include <asm/fixmap.h>
#include <asm/kernel-pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

/* ensure that the fixmap region does not grow down into the PCI I/O region */
/*
 * 编译期断言验证完整 fixmap 窗口的最低地址仍高于 PCI I/O 虚拟窗口末端。新增槽位会让
 * FIXADDR_TOT_START 向低地址增长；若越界，两个用途将共享虚拟地址，必须在构建时失败，
 * 不能等到启动后才发现页表互相覆盖。
 */
static_assert(FIXADDR_TOT_START > PCI_IO_END);

#define NR_BM_PTE_TABLES \
	SPAN_NR_ENTRIES(FIXADDR_TOT_START, FIXADDR_TOP, PMD_SHIFT)
#define NR_BM_PMD_TABLES \
	SPAN_NR_ENTRIES(FIXADDR_TOT_START, FIXADDR_TOP, PUD_SHIFT)

/*
 * NR_BM_PTE_TABLES 是 [FIXADDR_TOT_START, FIXADDR_TOP) 跨越的 PMD 区间数：每个 PMD
 * 区间需要一页 PTE 表。NR_BM_PMD_TABLES 同理统计跨越的 PUD 区间数。SPAN_NR_ENTRIES
 * 同时计入首尾不对齐区间，因此不会少分配边界页表；两者均是编译期常量。
 */

static_assert(NR_BM_PMD_TABLES == 1);

/*
 * 当前布局必须完整落在一个 PUD 覆盖范围内，所以只需一页 bm_pmd。若枚举扩张使窗口
 * 跨越两个 PUD，本实现的单 bm_pmd 假设不再成立，编译期断言要求先重构页表存储。
 */

#define __BM_TABLE_IDX(addr, shift) \
	(((addr) >> (shift)) - (FIXADDR_TOT_START >> (shift)))

#define BM_PTE_TABLE_IDX(addr)	__BM_TABLE_IDX(addr, PMD_SHIFT)

/*
 * __BM_TABLE_IDX 把绝对虚拟地址所在的页表区间，平移成从 FIXADDR_TOT_START 开始的
 * 零基数组下标；@shift 决定按哪一级覆盖范围分组。BM_PTE_TABLE_IDX 固定使用 PMD_SHIFT，
 * 选出 addr 应落入 bm_pte[] 的哪一页 PTE 表。传入地址必须位于完整 fixmap 窗口。
 */

static pte_t bm_pte[NR_BM_PTE_TABLES][PTRS_PER_PTE] __bss_pgtbl;
static pmd_t bm_pmd[PTRS_PER_PMD] __bss_pgtbl __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __bss_pgtbl __maybe_unused;

/*
 * 静态页表地图：
 *   bm_pte  叶子表二维数组；第一维选择 PMD 区间，第二维是该区间内的 PTE 索引。
 *   bm_pmd  唯一 PUD 区间下的 PMD 表，把各 PMD entry 指向对应 bm_pte 页。
 *   bm_pud  四级页表配置需要的 PUD 表；页表层级折叠或复用既有上级表时可能未引用，
 *           因此标记 __maybe_unused。
 * __bss_pgtbl 把对象放入页对齐的页表 BSS 段；零初始化正好表示所有 entry 初始 none。
 * 三者是 init_mm 页表的一部分，不取得 page 引用，也没有释放路径。
 */

/*
 * fixmap_pte - 定位一个 fixmap 虚拟页对应的静态叶子 PTE
 *
 * 入参：@addr 是 __fix_to_virt() 产生、位于完整 fixmap 窗口内的内核虚拟地址，单位字节；
 * 可带页内偏移，但当前调用者传入页对齐槽位地址。
 *
 * 返回：指向 bm_pte 静态数组中对应 entry 的借用指针，生命周期永久；函数不加锁、不
 * 修改页表。第一层下标选择跨越的 PMD 区间，pte_index() 选择其中的页号。
 */
static inline pte_t *fixmap_pte(unsigned long addr)
{
	return &bm_pte[BM_PTE_TABLE_IDX(addr)][pte_index(addr)];
}

/*
 * early_fixmap_init_pte - 确保一个 PMD entry 指向预留的 bm_pte 表
 *
 * 入参：@pmdp 是当前 fixmap PMD entry 的借用指针；@addr 是该 entry 覆盖范围内的
 * 虚拟地址，用来选择 bm_pte[] 页。调用者保证地址合法且启动期无并发写者。
 *
 * 副作用：仅当 entry 为 none 时写入 table descriptor；已有映射保持不变。无返回值、
 * 不分配内存。READ_ONCE 防止编译器把页表读取拆分/重复，但同步依赖启动期单线程。
 */
static void __init early_fixmap_init_pte(pmd_t *pmdp, unsigned long addr)
{
	/* pmd 是 entry 的值快照；ptep 仅在需要建表时指向静态 bm_pte 子表。 */
	pmd_t pmd = READ_ONCE(*pmdp);
	pte_t *ptep;

	if (pmd_none(pmd)) {
		ptep = bm_pte[BM_PTE_TABLE_IDX(addr)];
		/*
		 * __pa_symbol() 对内核静态符号执行 KASLR-safe 的虚实转换；底层 populate 接收
		 * 物理地址并写 table descriptor，AF 位供支持硬件页表 access flag 的实现使用。
		 */
		__pmd_populate(pmdp, __pa_symbol(ptep),
			       PMD_TYPE_TABLE | PMD_TABLE_AF);
	}
}

/*
 * early_fixmap_init_pmd - 为一个 PUD 覆盖范围接入 PMD 表并逐段接入 PTE 表
 *
 * 入参：@pudp 是上级 PUD entry；[@addr, @end) 是要覆盖的 fixmap 字节区间，必须按
 * 页表边界单调前进且不跨出 bm_pmd 所代表的 PUD 范围。指针均为启动期借用。
 *
 * 变量：pud 是上级 entry 快照；pmdp 是当前 PMD entry；next 是当前 PMD 覆盖段末端。
 * 副作用：按需把 pudp 指向 bm_pmd，并保证区间触及的每个 PMD 都指向一页 bm_pte。
 */
static void __init early_fixmap_init_pmd(pud_t *pudp, unsigned long addr,
					 unsigned long end)
{
	unsigned long next;
	pud_t pud = READ_ONCE(*pudp);
	pmd_t *pmdp;

	if (pud_none(pud))
		__pud_populate(pudp, __pa_symbol(bm_pmd),
			       PUD_TYPE_TABLE | PUD_TABLE_AF);

	pmdp = pmd_offset_kimg(pudp, addr);
	do {
		/* pmd_addr_end 把本轮限制在一个 PMD 覆盖范围，避免一次选择错误的 PTE 表。 */
		next = pmd_addr_end(addr, end);
		early_fixmap_init_pte(pmdp, addr);
	} while (pmdp++, addr = next, addr != end);
}


/*
 * early_fixmap_init_pud - 处理 fixmap 在 P4D/PUD 层的接入与特殊共享布局
 *
 * 入参：@p4dp 是 fixmap 起始地址对应的 P4D entry；[@addr, @end) 是完整待建区间。
 * p4d 保存 entry 快照，pudp 是最终用于向下遍历的 PUD 指针，均无所有权转移。
 *
 * 在普通空 entry 情况下把 p4dp 指向静态 bm_pud；16 KiB/四级页表可能让内核映像与
 * fixmap 共享顶级 PGD entry，此时上级表已存在，必须沿既有表向下走，不能覆盖它。
 */
static void __init early_fixmap_init_pud(p4d_t *p4dp, unsigned long addr,
					 unsigned long end)
{
	p4d_t p4d = READ_ONCE(*p4dp);
	pud_t *pudp;

	if (CONFIG_PGTABLE_LEVELS > 3 && !p4d_none(p4d) &&
	    p4d_page_paddr(p4d) != __pa_symbol(bm_pud)) {
		/*
		 * We only end up here if the kernel mapping and the fixmap
		 * share the top level pgd entry, which should only happen on
		 * 16k/4 levels configurations.
		 */
		/*
		 * 原注释说明了非空且不指向 bm_pud 的唯一合法来源。BUG_ON 验证该共享只发生在
		 * 16 KiB 页/四级配置；其他配置出现这种状态意味着页表布局或早期映射被破坏。
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
	}

	if (p4d_none(p4d))
		__p4d_populate(p4dp, __pa_symbol(bm_pud),
			       P4D_TYPE_TABLE | P4D_TABLE_AF);

	pudp = pud_offset_kimg(p4dp, addr);
	early_fixmap_init_pmd(pudp, addr, end);
}

/*
 * The p*d_populate functions call virt_to_phys implicitly so they can't be used
 * directly on kernel symbols (bm_p*d). This function is called too early to use
 * lm_alias so __p*d_populate functions must be used to populate with the
 * physical address from __pa_symbol.
 */
/*
 * 原注释解释了为何不能使用普通 p*d_populate()：那些包装内部走 virt_to_phys()，要求
 * 地址已处于可用的 linear-map alias；此时线性映射别名尚不能可靠使用，而 bm_p*d 是
 * 受 KASLR 影响的内核符号。这里显式 __pa_symbol() 后调用底层 __p*d_populate()，把
 * 正确物理地址写入 table descriptor。
 */
/*
 * early_fixmap_init - 将预留静态页表接入内核页表，覆盖完整 fixmap 窗口
 *
 * 调用关系：ARM64 setup_arch() 在普通 fixmap/FDT 使用前调用；内部从 init_mm.pgd
 * 找到起始层级，再由 early_fixmap_init_pud/pmd/pte 完成所有下级表链接。
 *
 * 入参/返回：无参数、无返回值。addr/end 分别是完整窗口的低地址和高地址边界；pgdp、
 * p4dp 是 init_mm 页表中的借用 entry 指针。__init 表示启动完成后函数代码可回收，
 * 但 bm_* 页表本身继续作为内核页表永久存在。
 *
 * 前置条件：启动 CPU 单线程执行，swapper_pg_dir 已建立，静态页表仍为零或包含允许的
 * 共享上级映射；函数不分配内存、没有失败返回，布局冲突通过编译断言或 BUG 暴露。
 */
void __init early_fixmap_init(void)
{
	/* addr/end 是半开区间；pgdp/p4dp 分别定位该区间起点对应的顶层和下一层 entry。 */
	unsigned long addr = FIXADDR_TOT_START;
	unsigned long end = FIXADDR_TOP;

	pgd_t *pgdp = pgd_offset_k(addr);
	p4d_t *p4dp = p4d_offset_kimg(pgdp, addr);

	early_fixmap_init_pud(p4dp, addr, end);
}

/*
 * Unusually, this is also called in IRQ context (ghes_iounmap_irq) so if we
 * ever need to use IPIs for TLB broadcasting, then we're in trouble here.
 */
/*
 * 原注释给出一个重要上下文约束：GHES 错误处理的 ghes_iounmap_irq() 会在 IRQ 中清除
 * fixmap，因此本函数必须保持原子、不可睡眠。当前 ARM64 内核范围 TLB invalidation
 * 可以在该上下文完成；若未来必须用可能等待的跨 CPU IPI 广播，这个调用契约就需要
 * 重新设计，而不能简单把睡眠操作塞入这里。
 */
/*
 * __set_fixmap - 把一个固定虚拟槽位映射到物理页，或清除该槽位
 *
 * 调用关系：asm-generic 的 set_fixmap()/clear_fixmap()、early/late 包装，以及 GHES、
 * KVM VNCR、entry trampoline 等架构路径最终进入这里。early_fixmap_init() 必须已经
 * 为所有合法槽位接好 bm_pte 页表。
 *
 * 入参：
 *   @idx   enum fixed_addresses 槽位编号；必须严格位于 FIX_HOLE 与
 *          __end_of_fixed_addresses 之间。编号决定唯一虚拟页，不是数组裸下标。
 *   @phys  目标物理地址，单位字节；映射路径按 PAGE_SHIFT 取 PFN，因此调用者应传入
 *          页对齐基址。清除路径通常传 0，但是否清除实际由 @flags 的值决定。
 *   @flags ARM64 PTE 页属性；非零表示建立/替换映射，零值 FIXMAP_PAGE_CLEAR 表示清除。
 *
 * 变量：addr 是 idx 对应的页对齐内核虚拟地址；ptep 是 bm_pte 中永久借用的叶子 entry。
 *
 * 并发/返回：无返回值、不可睡眠且可能在 IRQ 中调用。本函数不提供同槽位互斥，调用者
 * 必须按用途独占或自行串行化 idx。非法 idx 直接 BUG，因为继续计算会写坏任意页表。
 */
/*
 * 修正说明：上文“调用者应传入页对齐基址”只适用于直接把槽位当作页基址使用的调用。
 * asm-generic 的 set_fixmap_offset() 可以传入非页对齐 @phys：本函数通过右移自然选择
 * 该地址所在 PFN，包装层再把 phys 的低 PAGE_SHIFT 位加到 fixmap 虚拟基址后返回。
 * 因而本函数消费 @phys 的页号，页内偏移是否保留由上层 API 契约决定。
 */
void __set_fixmap(enum fixed_addresses idx,
			       phys_addr_t phys, pgprot_t flags)
{
	unsigned long addr = __fix_to_virt(idx);
	pte_t *ptep;

	BUG_ON(idx <= FIX_HOLE || idx >= __end_of_fixed_addresses);

	ptep = fixmap_pte(addr);

	if (pgprot_val(flags)) {
		/*
		 * phys >> PAGE_SHIFT 把字节物理地址变成页帧号；pfn_pte 合并 PFN 与访问属性，
		 * __set_pte 使用架构要求的页表写入序列发布新叶子。该分支预期从 invalid 建立
		 * 映射或由调用协议安全替换，不在这里执行额外 TLB flush。
		 */
		__set_pte(ptep, pfn_pte(phys >> PAGE_SHIFT, flags));
	} else {
		/*
		 * 清除 entry 后必须失效 [addr, addr + PAGE_SIZE) 的内核 TLB；否则 CPU 仍可能用
		 * 缓存的旧 PFN 访问已经撤销/重用的物理页。init_mm 传给 helper 表明这是内核页表。
		 */
		__pte_clear(&init_mm, addr, ptep);
		flush_tlb_kernel_range(addr, addr+PAGE_SIZE);
	}
}

/*
 * fixmap_remap_fdt - 在预留 fixmap 窗口映射并验证启动 FDT
 *
 * 调用关系：ARM64 setup_machine_fdt() 在 memblock/通用 ioremap 可用前调用。第一次以
 * PAGE_KERNEL 映射并扫描/修正 DTB，完成后可再次以 PAGE_KERNEL_RO 建立只读映射。
 *
 * 入参：
 *   @dt_phys bootloader 传入的 FDT 起始物理地址，单位字节；必须非零并满足
 *            MIN_FDT_ALIGN，对象仍由固件/启动内存拥有，本函数只建立映射。
 *   @size    调用者拥有的 int 输出地址，成功通过 magic 检查后写入 FDT totalsize 字节数；
 *            必须非 NULL。早于 magic 检查的失败不会写它。
 *   @prot    页表访问属性，常见为 PAGE_KERNEL 或 PAGE_KERNEL_RO，按值传入。
 *
 * 返回：成功时返回指向 FDT 精确首字节的 fixmap 虚拟指针；失败返回 NULL。返回地址不是
 * linear-map alias，不能用 __pa() 反推物理地址，也不能释放。映射使用静态预建页表，
 * 本函数无动态资源回滚；启动调用者把 NULL 视为无效 DTB。
 *
 * 核心机制：FDT 总长度存放在 header 中，映射前未知，所以先映射包含 header 的一页，
 * 校验 magic/长度后再覆盖完整 [页对齐物理基址, offset + totalsize) 区间。fixmap.h 为
 * FDT 预留 MAX_FDT_SIZE 再加一页，保证最大合法且跨页起始的 DTB 仍装得下。
 */
void *__init fixmap_remap_fdt(phys_addr_t dt_phys, int *size, pgprot_t prot)
{
	/*
	 * 变量地图：
	 *   dt_virt_base FIX_FDT 槽位对应的页对齐固定虚拟窗口基址，启动期保持稳定。
	 *   dt_phys_base 将 @dt_phys 向下对齐后的物理页基址。
	 *   offset       FDT 首字节在该物理页内的字节偏移，范围 [0, PAGE_SIZE)。
	 *   dt_virt      dt_virt_base 加 offset 后、指向真实 FDT header 的返回候选指针。
	 */
	const u64 dt_virt_base = __fix_to_virt(FIX_FDT);
	phys_addr_t dt_phys_base;
	int offset;
	void *dt_virt;

	/*
	 * Check whether the physical FDT address is set and meets the minimum
	 * alignment requirement. Since we are relying on MIN_FDT_ALIGN to be
	 * at least 8 bytes so that we can always access the magic and size
	 * fields of the FDT header after mapping the first chunk, double check
	 * here if that is indeed the case.
	 */
	/*
	 * 原注释说明两层校验：BUILD_BUG_ON 保证架构常量至少为 8，使一个满足对齐的 FDT
	 * header 前 8 字节（magic 与 totalsize）不会横跨页尾；运行期再拒绝零地址和未按
	 * MIN_FDT_ALIGN 对齐的固件输入。两种失败都发生在建映射前。
	 */
	BUILD_BUG_ON(MIN_FDT_ALIGN < 8);
	if (!dt_phys || dt_phys % MIN_FDT_ALIGN)
		return NULL;

	dt_phys_base = round_down(dt_phys, PAGE_SIZE);
	offset = dt_phys % PAGE_SIZE;
	dt_virt = (void *)dt_virt_base + offset;

	/* map the first chunk so we can read the size from the header */
	/*
	 * 先映射从 dt_phys_base 开始的一页。create_mapping_noalloc 只能修改已存在的页表层级，
	 * 不会申请新表；early_fixmap_init() 已建立这一前置条件，因此此处适合极早启动阶段。
	 */
	create_mapping_noalloc(dt_phys_base, dt_virt_base, PAGE_SIZE, prot);

	/* magic 不匹配说明该地址不是 FDT；保留临时槽位映射，但不把内容交给调用者。 */
	if (fdt_magic(dt_virt) != FDT_MAGIC)
		return NULL;

	/* totalsize 是 FDT header 声明的完整 blob 字节数；超过窗口上限会被拒绝。 */
	*size = fdt_totalsize(dt_virt);
	if (*size > MAX_FDT_SIZE)
		return NULL;

	if (offset + *size > PAGE_SIZE) {
		/*
		 * 当 DTB 从页中部开始或本身超过一页时，以同一起点把映射扩展到 offset+size。
		 * helper 会按页覆盖范围，调用者最终仍使用带 offset 的 dt_virt，而非页基址。
		 */
		create_mapping_noalloc(dt_phys_base, dt_virt_base,
				       offset + *size, prot);
	}

	return dt_virt;
}
