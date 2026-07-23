// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM 32 位 MMU 初始化学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：把启动汇编留下的临时页表，逐步整理成 ARM32 内核长期使用的
 * 地址空间；依据 CPU 架构、短描述符/LPAE 格式和启动参数生成内存属性，
 * 建立 lowmem、内核镜像、异常向量、静态设备、fixmap 与 highmem 辅助映射。
 * 本文件不负责普通进程 VMA 的创建、缺页处理或页表回收；运行期这些工作由
 * 通用 mm 与 ARM fault/pgtable 代码接手。
 *
 * 主调用链：
 *   setup_arch()
 *     -> early_mm_init()
 *        -> build_mem_type_table()：把抽象 MT_* 类型固化为本 CPU 的描述符位
 *        -> early_paging_init()：可选地修正高物理地址平台的 P:V 偏移
 *     -> adjust_lowmem_bounds()：确定线性映射上界和早期分配器安全边界
 *     -> paging_init()
 *        -> prepare_page_table()：撤销不再可信的启动映射
 *        -> map_lowmem()/map_kernel()：重建 RAM 与内核镜像映射
 *        -> early_fixmap_shutdown()/devicemaps_init()/kmap_init()
 *        -> bootmem_init()：把完成的地址空间交给后续内存初始化
 *
 * 核心对象与生命周期：
 * - mem_types[] 在编译期给出抽象模板，build_mem_type_table() 在单线程启动期
 *   原地补齐 cache、权限、domain、shareable、XN/AF 等位；__ro_after_init
 *   随后阻止运行期改写。
 * - map_desc 只是调用者提供的映射请求，不转移其所有权；创建函数将请求翻译
 *   成 swapper_pg_dir/init_mm 下的页表项。页表页早期来自 memblock，稍晚可
 *   来自页表分配器，发布后由 init_mm 持有。
 * - bm_pte 是页表分配器可用前的临时 fixmap PTE 表；切换到正式页表后，
 *   early_fixmap_shutdown() 迁移仍需保留的设备映射并停止使用它。
 *
 * 并发与硬件可见性：绝大多数函数带 __init，只在启动 CPU 串行执行，因此
 * 不依赖运行期页表锁；但写页表并不等于硬件立刻采用新翻译，代码会在发布
 * 边界调用 flush_pmd_entry()、cache/TLB flush。__set_fixmap() 特意只做本地
 * TLB 失效，调用者必须禁用抢占，避免任务迁到仍缓存旧翻译的 CPU。
 *
 * 方案权衡：优先使用 section/supersection 可减少页表内存和 TLB 压力，
 * 但要求地址、物理地址和长度严格对齐，且权限粒度较粗；不满足条件时退回
 * PTE 页映射。兼容多代 ARM 描述符格式增加了分支复杂度，却使同一映射模型
 * 能覆盖早期 CPU、XScale、ARMv6/v7 以及 LPAE。
 */
/*
 *  linux/arch/arm/mm/mmu.c
 *
 *  Copyright (C) 1995-2005 Russell King
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>

#include <asm/cp15.h>
#include <asm/cputype.h>
#include <asm/cachetype.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/tcm.h>
#include <asm/tlb.h>
#include <asm/highmem.h>
#include <asm/system_info.h>
#include <asm/traps.h>
#include <asm/procinfo.h>
#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/kasan_def.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/pci.h>
#include <asm/fixmap.h>

#include "fault.h"
#include "mm.h"

extern unsigned long __atags_pointer;

/*
 * The pmd table for the upper-most set of pages.
 */
/*
 * top_pmd 指向高向量地址 0xffff0000 所在的内核 PMD。paging_init() 在所有
 * 顶层映射稳定后发布它，异常向量相关代码只借用该指针，页表本身由 init_mm
 * 持有并贯穿内核运行期。
 */
pmd_t *top_pmd;

/*
 * 用户页表的一级描述符模板。build_mem_type_table() 可能根据短描述符 PXN
 * 能力追加 PMD_PXNTABLE；其值随后用于新建用户二级页表，限制特权态执行。
 */
pmdval_t user_pmd_table = _PAGE_USER_TABLE;

#define CPOLICY_UNCACHED	0
#define CPOLICY_BUFFERED	1
#define CPOLICY_WRITETHROUGH	2
#define CPOLICY_WRITEBACK	3
#define CPOLICY_WRITEALLOC	4

/*
 * cachepolicy 是 cache_policies[] 的 init 期索引，默认 write-back；启动参数、
 * 汇编初始描述符和 CPU 能力共同修正它，build_mem_type_table() 最终消费。
 * ecc_mask 是非 LPAE section/table protection 位模板，零表示关闭；两者只由
 * 启动 CPU 读写，无并发保护需求，并随 initdata 一同回收。
 */
static unsigned int cachepolicy __initdata = CPOLICY_WRITEBACK;
static unsigned int ecc_mask __initdata = 0;
/*
 * pgprot_user/kernel 是架构向通用 MM 发布的基础叶子属性。它们在
 * build_mem_type_table() 前为零，因此早期 fixmap 只能接受显式设备属性；
 * 初始化完成后只读使用，分别作为用户映射和内核映射的公共位集合。
 */
pgprot_t pgprot_user;
pgprot_t pgprot_kernel;

EXPORT_SYMBOL(pgprot_user);
EXPORT_SYMBOL(pgprot_kernel);

struct cachepolicy {
	/* 启动参数名称；仅在 __init 阶段用于匹配和日志。 */
	const char	policy[16];
	/* 选择此策略时需要从 CP15 控制寄存器清除的 cache/write-buffer 位。 */
	unsigned int	cr_mask;
	/* section 描述符与 PTE 描述符采用的缓存属性编码。 */
	pmdval_t	pmd;
	pteval_t	pte;
};

/*
 * cache_policies[] 把用户可见策略名绑定到控制寄存器动作和两种页表粒度的
 * 属性编码。表及其字符串在 init 内存释放前有效；cachepolicy 保存的是索引，
 * build_mem_type_table() 会把所选编码复制进长期存在的 mem_types[]。
 */
static struct cachepolicy cache_policies[] __initdata = {
	{
		.policy		= "uncached",
		.cr_mask	= CR_W|CR_C,
		.pmd		= PMD_SECT_UNCACHED,
		.pte		= L_PTE_MT_UNCACHED,
	}, {
		.policy		= "buffered",
		.cr_mask	= CR_C,
		.pmd		= PMD_SECT_BUFFERED,
		.pte		= L_PTE_MT_BUFFERABLE,
	}, {
		.policy		= "writethrough",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WT,
		.pte		= L_PTE_MT_WRITETHROUGH,
	}, {
		.policy		= "writeback",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WB,
		.pte		= L_PTE_MT_WRITEBACK,
	}, {
		.policy		= "writealloc",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WBWA,
		.pte		= L_PTE_MT_WRITEALLOC,
	}
};

#ifdef CONFIG_CPU_CP15
/*
 * 启动汇编所用 PMD 的完整快照，零是“尚未由汇编入口发布”的初值。初始化
 * 路径写一次并在构造共享/cache 属性时读取，启动期串行，随 initdata 回收。
 */
static unsigned long initial_pmd_value __initdata = 0;

/*
 * Initialise the cache_policy variable with the initial state specified
 * via the "pmd" value.  This is used to ensure that on ARMv6 and later,
 * the C code sets the page tables up with the same policy as the head
 * assembly code, which avoids an illegal state where the TLBs can get
 * confused.  See comments in early_cachepolicy() for more information.
 */
/*
 * init_default_cache_policy - 从启动汇编已生效的 section 描述符恢复缓存策略。
 *
 * pmd 是 head 汇编建立线性映射时使用的原始描述符值。函数保存其 shareable
 * 等位，并用 cache 属性匹配 cache_policies[]；没有可匹配项只记录错误，
 * 保留默认策略。该同步必须先于 build_mem_type_table()，否则同一物理内存
 * 可能被前后两套属性别名映射，导致体系结构定义的不可预测行为。
 *
 * 入参：
 *   @pmd：启动汇编已安装的 section 描述符值，纯输入；按位编码，不涉及
 *   指针 ownership 或可空性。
 * 前置条件：启动 CPU 串行执行、无需锁，普通内存分配尚不可依赖；不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：写 initial_pmd_value，匹配成功时更新 init 期 cachepolicy。
 * 失败结果：找不到策略时只记录错误，保留原 cachepolicy 并继续启动。
 */
void __init init_default_cache_policy(unsigned long pmd)
{
	int i;

	/*
	 * 阶段 1：先保存完整启动描述符，供后续恢复 S 等非 cache 属性；局部匹配
	 * 只保留 cache mask，避免权限/domain 位干扰策略查找。
	 */
	initial_pmd_value = pmd;

	pmd &= PMD_SECT_CACHE_MASK;

	/*
	 * 阶段 2：在静态策略表中寻找完全相同的硬件编码。命中才发布新索引；
	 * 未命中时旧 cachepolicy 仍有效，不能写入越界索引。
	 */
	for (i = 0; i < ARRAY_SIZE(cache_policies); i++)
		if (cache_policies[i].pmd == pmd) {
			cachepolicy = i;
			break;
		}

	if (i == ARRAY_SIZE(cache_policies))
		pr_err("ERROR: could not find cache policy\n");
}

/*
 * These are useful for identifying cache coherency problems by allowing
 * the cache or the cache and writebuffer to be turned off.  (Note: the
 * write buffer should not be on and the cache off).
 */
/*
 * early_cachepolicy - 解析 cachepolicy= 并在允许的旧 CPU 上切换全局策略。
 *
 * p 指向启动命令行中的策略字符串，不取得所有权。ARMv6+ 已由汇编创建映射，
 * 禁止更换属性，只给出告警；更早 CPU 可以先清理整个 cache，再修改 CP15
 * 控制位，避免脏数据在关闭 cache/write buffer 时丢失。返回 0 表示参数已
 * 消费，未知或被拒绝的策略也不会中止启动。
 *
 * 入参：
 *   @p：启动命令行中的借用字符串，纯输入、非 NULL；函数不保存或释放。
 * 前置条件：启动 CPU 串行解析参数，无锁；本函数不分配内存、不主动睡眠。
 * 出参：无。
 * 返回：始终为 0，未知值和被拒绝的切换都不通过错误码表达。
 * 副作用：旧 CPU 上可能更新 cachepolicy、清理全部 cache 并改写 CP15 CR。
 * 失败结果：ARMv6+ 的未知/冲突策略会告警并保持现状；当前旧 CPU 分支没有
 * 在 selected=-1 后提前返回，非法字符串会继续形成越界索引，因此调用者
 * 实际必须传入表中策略名。这是当前实现限制，不应误读为安全 fallback。
 */
static int __init early_cachepolicy(char *p)
{
	int i, selected = -1;

	/*
	 * 阶段 1：按完整策略名前缀匹配命令行值。selected=-1 是“尚未识别”的
	 * 哨兵；循环不改全局策略，因此无效输入尚未产生硬件副作用。
	 */
	for (i = 0; i < ARRAY_SIZE(cache_policies); i++) {
		int len = strlen(cache_policies[i].policy);

		if (memcmp(p, cache_policies[i].policy, len) == 0) {
			selected = i;
			break;
		}
	}

	if (selected == -1)
		pr_err("ERROR: unknown or unsupported cache policy\n");

	/*
	 * This restriction is partly to do with the way we boot; it is
	 * unpredictable to have memory mapped using two different sets of
	 * memory attributes (shared, type, and cache attribs).  We can not
	 * change these attributes once the initial assembly has setup the
	 * page tables.
	 */
	/*
	 * 阶段 2：ARMv6+ 在汇编建表后禁止属性切换。这里在任何 cache/CR 写入前
	 * 返回，也使该配置下的未知 selected=-1 不会用于数组索引。
	 */
	if (cpu_architecture() >= CPU_ARCH_ARMv6 && selected != cachepolicy) {
		pr_warn("Only cachepolicy=%s supported on ARMv6 and later\n",
			cache_policies[cachepolicy].policy);
		return 0;
	}

	/*
	 * 阶段 3：旧 CPU 且策略确有变化时提交。合法 selected 下先计算将清除
	 * 的 CR 位，再更新软件索引、写回 cache，最后改 CR。注意当前代码没有
	 * 排除 -1，故旧 CPU 的未知参数不具备安全失败保证。
	 */
	if (selected != cachepolicy) {
		unsigned long cr = __clear_cr(cache_policies[selected].cr_mask);
		cachepolicy = selected;
		flush_cache_all();
		set_cr(cr);
	}
	return 0;
}
early_param("cachepolicy", early_cachepolicy);

/*
 * early_nocache/early_nowrite 是旧命令行参数兼容层：分别固定转译为 buffered
 * 和 uncached，再复用 early_cachepolicy() 的架构检查与硬件切换语义。参数
 * 本身不读取，两个入口都只告警弃用且始终让启动继续。
 */
/*
 * early_nocache：
 * 入参 @__unused 是启动框架借用且忽略的字符串；无出参，返回恒为 0。
 * 启动期串行、无锁、不取得 ownership；副作用是告警，并按
 * early_cachepolicy("buffered") 的契约可能刷新 cache、更新 CR/策略。
 */
static int __init early_nocache(char *__unused)
{
	char *p = "buffered";
	pr_warn("nocache is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nocache", early_nocache);

/*
 * early_nowrite：
 * 入参 @__unused 是启动框架借用且忽略的字符串；无出参，返回恒为 0。
 * 启动期串行、无锁、不取得 ownership；副作用是告警，并按
 * early_cachepolicy("uncached") 的契约可能刷新 cache、更新 CR/策略。
 */
static int __init early_nowrite(char *__unused)
{
	char *p = "uncached";
	pr_warn("nowb is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nowb", early_nowrite);

#ifndef CONFIG_ARM_LPAE
/*
 * early_ecc - 解析短描述符页表的 ecc=on/off 兼容参数。
 *
 * 只更新 init 期模板 ecc_mask；真正写入向量和 RAM section 描述符发生在
 * build_mem_type_table()。LPAE 没有这套 PMD_PROTECTION 编码，故不编译。
 *
 * 入参：
 *   @p：ecc= 后的借用字符串，纯输入、非 NULL，不保存、不修改、不释放。
 * 前置条件：非 LPAE 启动参数解析期，串行、无锁、不睡眠。
 * 出参：无。
 * 返回：始终为 0；未知字符串表示不改变 ecc_mask。
 * 副作用：识别到 on/off 时更新 init 期 ecc_mask。
 * 失败结果：无错误返回，未知值保留旧策略。
 */
static int __init early_ecc(char *p)
{
	/*
	 * 单一解析阶段：只接受两个已知前缀并更新模板；其他输入不改变旧值。
	 * 此时尚未写页表，故无需 cache/TLB 同步。
	 */
	if (memcmp(p, "on", 2) == 0)
		ecc_mask = PMD_PROTECTION;
	else if (memcmp(p, "off", 3) == 0)
		ecc_mask = 0;
	return 0;
}
early_param("ecc", early_ecc);
#endif

#else /* ifdef CONFIG_CPU_CP15 */

/*
 * 无 CP15 构建无法控制 MMU/cache；两个解析器只消费参数并明确报告不支持。
 *
 * early_cachepolicy 的 @p 是借用的纯输入字符串，允许任意内容且不保存；
 * 无出参，返回恒为 0。启动期串行、无锁、不睡眠，唯一副作用是告警。
 */
static int __init early_cachepolicy(char *p)
{
	pr_warn("cachepolicy kernel parameter not supported without cp15\n");
	return 0;
}
early_param("cachepolicy", early_cachepolicy);

/*
 * noalign_setup 的 @__unused 是借用且忽略的字符串；无出参，返回 1 表示
 * __setup 参数已处理。启动期串行、无锁、不睡眠，副作用仅为告警，不改变
 * 无 CP15 构建无法提供的对齐控制状态。
 */
static int __init noalign_setup(char *__unused)
{
	pr_warn("noalign kernel parameter not supported without cp15\n");
	return 1;
}
__setup("noalign", noalign_setup);

#endif /* ifdef CONFIG_CPU_CP15 / else */

#define PROT_PTE_DEVICE		L_PTE_PRESENT|L_PTE_YOUNG|L_PTE_DIRTY|L_PTE_XN
#define PROT_PTE_S2_DEVICE	PROT_PTE_DEVICE
#define PROT_SECT_DEVICE	PMD_TYPE_SECT|PMD_SECT_AP_WRITE

/*
 * mem_types[] 是本文件的“抽象映射类型 -> ARM 描述符”中心表。
 *
 * prot_pte 用于 4 KiB 叶子映射，prot_l1 用于指向 PTE 表的上级描述符，
 * prot_sect 用于 1 MiB section（LPAE 下对应 block）映射，domain 只在采用
 * domain 的短描述符实现中有意义。零 prot_sect 表示该类型不能用 section；
 * 零 prot_l1 表示它不能退化为页粒度映射。build_mem_type_table() 会按实际
 * CPU 原地增补这些模板，之后 __ro_after_init 使表只读。
 *
 * 设备组 MT_DEVICE* 区分共享性、cache 与 write-combine；向量组给异常入口
 * 提供用户/内核不同权限；MT_MEMORY_RWX/RW/RO 描述普通 RAM 的启动阶段权限；
 * ROM、TCM、cache-clean 和 DMA-ready 类型服务于特定机器或维护路径。
 *
 * 类型地图：
 * - MT_DEVICE / NONSHARED / CACHED / WC：共享设备、非共享设备、可缓存 MMIO
 *   和写合并 MMIO；共同禁止执行，但一致性与吞吐策略不同。
 * - MT_UNCACHED：不能采用普通 cache 的 I/O/特殊范围；仍保留 section 能力。
 * - MT_CACHECLEAN / MT_MINICLEAN：旧 ARM cache 维护窗口，只允许 section，
 *   MINICLEAN 仅非 LPAE minicache 平台存在。
 * - MT_LOW_VECTORS / MT_HIGH_VECTORS：同一异常向量物理页的低/高地址权限，
 *   高向量按 CONFIG_KUSER_HELPERS 决定用户可见性。
 * - MT_MEMORY_RWX / RW / RO：启动期普通 RAM 的三种执行/写权限模板。
 * - MT_ROM：XIP 代码的只读可执行 section；无 PTE 回退能力。
 * - MT_MEMORY_RWX_NONCACHED：必须避免脏 cache 回写但仍需普通内存语义的区间。
 * - MT_MEMORY_RW_DTCM / MT_MEMORY_RWX_ITCM：数据/指令 TCM 的权限模板。
 * - MT_MEMORY_RW_SO：需要 strongly-ordered 访问语义的普通内存特殊路径。
 * - MT_MEMORY_DMA_READY：DMA 已完成 cache 准备的页粒度内核内存映射。
 */
static struct mem_type mem_types[] __ro_after_init = {
	[MT_DEVICE] = {		  /* Strongly ordered / ARMv6 shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_SHARED |
				  L_PTE_SHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_S,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_NONSHARED] = { /* ARMv6 non-shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_NONSHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_CACHED] = {	  /* ioremap_cache */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_CACHED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_WB,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_WC] = {	/* ioremap_wc */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_WC,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_UNCACHED] = {
		.prot_pte	= PROT_PTE_DEVICE,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PMD_TYPE_SECT | PMD_SECT_XN,
		.domain		= DOMAIN_IO,
	},
	[MT_CACHECLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
#ifndef CONFIG_ARM_LPAE
	[MT_MINICLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN | PMD_SECT_MINICACHE,
		.domain    = DOMAIN_KERNEL,
	},
#endif
	[MT_LOW_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_VECTORS,
	},
	[MT_HIGH_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_USER | L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_VECTORS,
	},
	[MT_MEMORY_RWX] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
			     L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RO] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
			     L_PTE_XN | L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
#ifdef CONFIG_ARM_LPAE
		.prot_sect = PMD_TYPE_SECT | L_PMD_SECT_RDONLY | PMD_SECT_AP2,
#else
		.prot_sect = PMD_TYPE_SECT,
#endif
		.domain    = DOMAIN_KERNEL,
	},
	[MT_ROM] = {
		.prot_sect = PMD_TYPE_SECT,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RWX_NONCACHED] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_BUFFERABLE,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW_DTCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RWX_ITCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_RW_SO] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_UNCACHED | L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_S |
				PMD_SECT_UNCACHED | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_DMA_READY] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
};

/*
 * get_mem_type - 借出指定 MT_* 类型的只读描述符模板。
 *
 * type 越界返回 NULL；成功返回的指针没有新引用，调用者不得修改或释放。
 * build_mem_type_table() 完成后内容稳定到关机；早于该时点读取会看到未按 CPU
 * 修正的初始模板，因此启动调用顺序也是接口契约的一部分。
 *
 * 入参：
 *   @type：MT_* 枚举索引，纯输入、无单位；允许越界，不涉及 ownership。
 * 前置条件：需要最终 CPU 属性时应晚于 build_mem_type_table()；只读、无锁，
 * 可在原子上下文调用且不睡眠。
 * 出参：无。
 * 返回：有效索引返回表元素的借用只读指针，越界返回 NULL。
 * 副作用：无，不增加引用、不转移或释放对象。
 * 失败结果：NULL 精确表示 type 超出 mem_types[]。
 */
const struct mem_type *get_mem_type(unsigned int type)
{
	return type < ARRAY_SIZE(mem_types) ? &mem_types[type] : NULL;
}
EXPORT_SYMBOL(get_mem_type);

/*
 * fixmap PTE 查找策略在启动中途切换：最初忽略 dir、索引 bm_pte，正式页表
 * 就绪后改用 pte_offset_kernel()。启动期串行写入，无需锁；切换点必须位于
 * 旧 PMD 清除之前，避免后续 __set_fixmap() 再访问已脱离页表的临时数组。
 */
static pte_t *(*pte_offset_fixmap)(pmd_t *dir, unsigned long addr);

/* memblock/页表分配器可用前唯一的 early fixmap PTE 存储，按硬件表要求对齐。 */
static pte_t bm_pte[PTRS_PER_PTE + PTE_HWTABLE_PTRS]
	__aligned(PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE) __initdata;

/*
 * 三个小 helper 固化 fixmap 生命周期两端的寻址方式：
 * - pte_offset_early_fixmap() 在 bm_pte 中按虚拟页索引，dir 尚无实际作用；
 * - pte_offset_late_fixmap() 沿正式 PMD 找内核 PTE；
 * - fixmap_pmd() 从 init_mm 取得地址对应 PMD。
 * 返回值均为借用页表槽，不增加引用，调用者负责所处启动阶段正确。
 */
/*
 * pte_offset_early_fixmap：
 * @dir 是为统一签名保留且忽略的借用 PMD 指针，可为任意值；@addr 是 fixmap
 * 虚拟地址（字节），纯输入。无出参；返回 bm_pte 中的借用槽，无失败值和
 * 副作用。仅 early fixmap 生效期调用，无锁、不睡眠。
 */
static pte_t * __init pte_offset_early_fixmap(pmd_t *dir, unsigned long addr)
{
	return &bm_pte[pte_index(addr)];
}

/*
 * pte_offset_late_fixmap：
 * @dir 是正式页表中的借用 PMD，非 NULL 且已指向 PTE 表；@addr 是其覆盖的
 * 虚拟地址（字节）。无出参；返回借用 PTE 槽，无失败值和副作用。调用者
 * 稳定页表层级，函数不加锁、不睡眠。
 */
static pte_t *pte_offset_late_fixmap(pmd_t *dir, unsigned long addr)
{
	return pte_offset_kernel(dir, addr);
}

/*
 * fixmap_pmd：
 * @addr 是内核 fixmap 虚拟地址（字节），纯输入；无出参。返回 init_mm 中
 * 对应的借用 PMD 槽，无失败值和副作用；启动期串行、不睡眠。
 */
static inline pmd_t * __init fixmap_pmd(unsigned long addr)
{
	return pmd_off_k(addr);
}

/*
 * early_fixmap_init - 将 bm_pte 接到 fixmap 所在 PMD，并启用早期查找策略。
 *
 * 调用时仍不能动态分配页表。编译期断言保证 early_ioremap 整段只跨一个
 * PMD，否则单个 bm_pte 无法覆盖；pmd_populate_kernel() 发布表地址后，
 * __set_fixmap() 才能建立临时页映射。
 *
 * 入参：无。
 * 前置条件：启动页表已存在、动态页表分配尚不可用；启动 CPU 串行、无锁，
 * 本函数不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：把 bm_pte 发布到 init_mm 的 fixmap PMD，并切换查找函数指针。
 * 失败结果：覆盖范围不满足约束会在编译期失败，无运行期回滚。
 */
void __init early_fixmap_init(void)
{
	pmd_t *pmd;

	/*
	 * The early fixmap range spans multiple pmds, for which
	 * we are not prepared:
	 */
	BUILD_BUG_ON((__fix_to_virt(__end_of_early_ioremap_region) >> PMD_SHIFT)
		     != FIXADDR_TOP >> PMD_SHIFT);

	/*
	 * 构造阶段：取得 fixmap 顶端的 PMD，并把静态 bm_pte 作为其叶子表发布。
	 * 此时没有旧表需要回收，pmd_populate_kernel() 完成上级描述符写入。
	 */
	pmd = fixmap_pmd(FIXADDR_TOP);
	pmd_populate_kernel(&init_mm, pmd, bm_pte);

	/* 发布阶段：最后开放查找入口，避免调用者看见尚未连接的 bm_pte。 */
	pte_offset_fixmap = pte_offset_early_fixmap;
}

/*
 * To avoid TLB flush broadcasts, this uses local_flush_tlb_kernel_range().
 * As a result, this can only be called with preemption disabled, as under
 * stop_machine().
 */
/*
 * __set_fixmap - 建立或撤销一个固定虚拟页的映射。
 *
 * idx 必须落在固定地址枚举范围；phys 为页对应物理地址，prot 为零表示撤销。
 * pgprot_kernel 尚未生成时只允许 FIXMAP_PAGE_IO，防止用半初始化的普通内存
 * 属性访问设备。set_pte_at()/pte_clear() 改的是软件页表，随后本地 TLB
 * 失效才让当前 CPU 丢弃旧翻译。因为没有向其他 CPU 广播，调用者必须禁用
 * 抢占（典型为 stop_machine()），保证修改和随后访问发生在同一 CPU。
 *
 * 入参：
 *   @idx：fixed_addresses 索引，必须小于 __end_of_fixed_addresses。
 *   @phys：映射首页的物理字节地址，建立映射时应页对齐；撤销时不使用。
 *   @prot：纯输入页属性；值为零表示撤销，否则描述新 PTE。
 * 前置条件：调用期间禁用抢占；调用者保证目标槽无并发修改。本函数不分配
 * 内存、不睡眠，可在 stop_machine 等原子上下文执行。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：写或清一个 PTE，并使当前 CPU 对该页的内核 TLB 翻译失效。
 * 失败结果：非法 idx 触发 BUG；早期不支持的属性 WARN 后保持页表不变。
 */
void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot)
{
	unsigned long vaddr = __fix_to_virt(idx);
	pte_t *pte = pte_offset_fixmap(pmd_off_k(vaddr), vaddr);

	/* Make sure fixmap region does not exceed available allocation. */
	/*
	 * 校验阶段：编译期检查整体布局，运行期检查单个索引。BUG_ON 之前尚未
	 * 写 PTE，因此非法 idx 不会留下半发布映射。
	 */
	BUILD_BUG_ON(__fix_to_virt(__end_of_fixed_addresses) < FIXADDR_START);
	BUG_ON(idx >= __end_of_fixed_addresses);

	/* We support only device mappings before pgprot_kernel is set. */
	if (WARN_ON(pgprot_val(prot) != pgprot_val(FIXMAP_PAGE_IO) &&
		    pgprot_val(prot) && pgprot_val(pgprot_kernel) == 0))
		return;

	/*
	 * 提交阶段：非零 prot 发布新 PFN，零 prot 撤销槽位。两条分支汇合后都
	 * 必须失效本地 TLB，否则当前 CPU 仍可能命中修改前的翻译。
	 */
	if (pgprot_val(prot))
		set_pte_at(NULL, vaddr, pte,
			pfn_pte(phys >> PAGE_SHIFT, prot));
	else
		pte_clear(NULL, vaddr, pte);
	local_flush_tlb_kernel_range(vaddr, vaddr + PAGE_SIZE);
}

static pgprot_t protection_map[16] __ro_after_init = {
	[VM_NONE]					= __PAGE_NONE,
	[VM_READ]					= __PAGE_READONLY,
	[VM_WRITE]					= __PAGE_COPY,
	[VM_WRITE | VM_READ]				= __PAGE_COPY,
	[VM_EXEC]					= __PAGE_READONLY_EXEC,
	[VM_EXEC | VM_READ]				= __PAGE_READONLY_EXEC,
	[VM_EXEC | VM_WRITE]				= __PAGE_COPY_EXEC,
	[VM_EXEC | VM_WRITE | VM_READ]			= __PAGE_COPY_EXEC,
	[VM_SHARED]					= __PAGE_NONE,
	[VM_SHARED | VM_READ]				= __PAGE_READONLY,
	[VM_SHARED | VM_WRITE]				= __PAGE_SHARED,
	[VM_SHARED | VM_WRITE | VM_READ]		= __PAGE_SHARED,
	[VM_SHARED | VM_EXEC]				= __PAGE_READONLY_EXEC,
	[VM_SHARED | VM_EXEC | VM_READ]			= __PAGE_READONLY_EXEC,
	[VM_SHARED | VM_EXEC | VM_WRITE]		= __PAGE_SHARED_EXEC,
	[VM_SHARED | VM_EXEC | VM_WRITE | VM_READ]	= __PAGE_SHARED_EXEC
};
/*
 * protection_map 以 VM_{READ,WRITE,EXEC,SHARED} 四位作为索引，提供 mmap
 * 权限到 ARM PTE 属性的基础映射。build_mem_type_table() 再合入 CPU 相关的
 * cache/shareable/PXN 位；DECLARE_VM_GET_PAGE_PROT 据此生成通用 MM 查询入口。
 */
DECLARE_VM_GET_PAGE_PROT

/*
 * Adjust the PMD section entries according to the CPU in use.
 */
/*
 * build_mem_type_table - 把通用内存类型模板固化为当前 CPU 可用的页表属性。
 *
 * 调用关系：early_mm_init() 在任何正式映射创建前调用一次。输入来自 CP15
 * 架构/控制寄存器、SMP 状态、启动汇编的 initial_pmd_value 以及启动参数；
 * 无显式参数和错误返回，检测到不兼容时降级策略或打印告警继续启动。
 *
 * 主要阶段：
 * 1. 按 CPU 代际和内核配置收窄 cache 策略，SMP 强制 write-allocate/shared；
 * 2. 删除硬件不支持的 TEX/S/bit4，选择各代设备内存编码并补 XN；
 * 3. 把所选 cache 属性合入用户、内核、向量和每个 MT_* 模板；
 * 4. LPAE 下补 AF/PXN，短描述符下补 domain/ECC，最后发布 pgprot_*。
 *
 * 副作用是原地修改 mem_types[]、protection_map[]、user_pmd_table 和全局
 * pgprot_*。这些对象随后变为只读，因此本函数没有并发写者，也无需锁。
 * 最大风险不是资源泄漏，而是生成与启动汇编不一致的内存属性：同一物理页
 * 的属性别名可能使 cache/TLB 行为不可预测，所以 initial_pmd_value 必须参与。
 *
 * 入参：无。
 * 前置条件：early_mm_init() 的启动 CPU 串行上下文，尚无并发页表创建者；
 * 不持锁、不分配内存、不睡眠，CP15 状态已经可读。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：修改 cachepolicy、initial_pmd_value、user_pmd_table、mem_types[]、
 * protection_map[]、pgprot_user 和 pgprot_kernel，并输出最终策略日志。
 * 失败结果：不支持的策略被钳制到安全值；不存在错误返回或资源回滚。
 */
static void __init build_mem_type_table(void)
{
	/*
	 * 变量地图：
	 *   cp          最终选中 cache 策略的借用表项。
	 *   cr          当前 CP15 控制寄存器快照，整个构造过程保持作为判定依据。
	 *   *_pgprot    用户、内核、向量三类叶子属性的逐阶段累加器。
	 *   cpu_arch/i  CPU 代际与表扫描索引，仅在本函数有效。
	 */
	struct cachepolicy *cp;
	unsigned int cr = get_cr();
	pteval_t user_pgprot, kern_pgprot, vecs_pgprot;
	int cpu_arch = cpu_architecture();
	int i;

	if (cpu_arch < CPU_ARCH_ARMv6) {
		/*
		 * 编译配置可能主动关闭 D-cache 或限制为 write-through；这里把
		 * 命令行选择钳制到内核实际支持的最强策略，不能留下不可实现编码。
		 */
#if defined(CONFIG_CPU_DCACHE_DISABLE)
		if (cachepolicy > CPOLICY_BUFFERED)
			cachepolicy = CPOLICY_BUFFERED;
#elif defined(CONFIG_CPU_DCACHE_WRITETHROUGH)
		if (cachepolicy > CPOLICY_WRITETHROUGH)
			cachepolicy = CPOLICY_WRITETHROUGH;
#endif
	}
	if (cpu_arch < CPU_ARCH_ARMv5) {
		/*
		 * ARMv4 及更早既不能表达 write-allocate，也不采用这里的 ECC
		 * protection 位；在进入通用表改写前统一降级，后续只处理有效编码。
		 */
		if (cachepolicy >= CPOLICY_WRITEALLOC)
			cachepolicy = CPOLICY_WRITEBACK;
		ecc_mask = 0;
	}

	if (is_smp()) {
		/*
		 * 多 CPU 对普通内存必须采用一致、可共享的属性。write-allocate
		 * 与 S 位还要和启动页表一致，否则不同 CPU/别名可能观察到不一致。
		 */
		if (cachepolicy != CPOLICY_WRITEALLOC) {
			pr_warn("Forcing write-allocate cache policy for SMP\n");
			cachepolicy = CPOLICY_WRITEALLOC;
		}
		if (!(initial_pmd_value & PMD_SECT_S)) {
			pr_warn("Forcing shared mappings for SMP\n");
			initial_pmd_value |= PMD_SECT_S;
		}
	}

	/*
	 * Strip out features not present on earlier architectures.
	 * Pre-ARMv5 CPUs don't have TEX bits.  Pre-ARMv6 CPUs or those
	 * without extended page tables don't have the 'Shared' bit.
	 */
	/*
	 * 阶段 2：从所有模板剥离硬件不存在的 TEX/S 位。这是能力交集计算，
	 * 只收窄属性，不会把某类型从不可映射错误地变成可映射。
	 */
	if (cpu_arch < CPU_ARCH_ARMv5)
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_TEX(7);
	if ((cpu_arch < CPU_ARCH_ARMv6 || !(cr & CR_XP)) && !cpu_is_xsc3())
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_S;

	/*
	 * ARMv5 and lower, bit 4 must be set for page tables (was: cache
	 * "update-able on write" bit on ARM610).  However, Xscale and
	 * Xscale3 require this bit to be cleared.
	 */
	/*
	 * 同一 bit4 在不同实现上的保留值要求相反：XScale 全部清除，其他旧 CPU
	 * 只对实际存在的一级/section 模板置位，零模板仍保持“不支持该粒度”。
	 */
	if (cpu_is_xscale_family()) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			mem_types[i].prot_sect &= ~PMD_BIT4;
			mem_types[i].prot_l1 &= ~PMD_BIT4;
		}
	} else if (cpu_arch < CPU_ARCH_ARMv6) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			if (mem_types[i].prot_l1)
				mem_types[i].prot_l1 |= PMD_BIT4;
			if (mem_types[i].prot_sect)
				mem_types[i].prot_sect |= PMD_BIT4;
		}
	}

	/*
	 * Mark the device areas according to the CPU/architecture.
	 */
	/*
	 * 阶段 3：选择设备内存编码。先区分 extended page table/XScale3，再在
	 * ARMv7 TEX remap、XScale3 和普通 ARMv6/v7 三套编码间分派；这些分支
	 * 最终都建立 shared/nonshared/WC 三类设备模板。
	 */
	if (cpu_is_xsc3() || (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP))) {
		if (!cpu_is_xsc3()) {
			/*
			 * Mark device regions on ARMv6+ as execute-never
			 * to prevent speculative instruction fetches.
			 */
			/*
			 * 普通 ARMv6+ 先给所有设备和只读/读写普通内存补 XN，阻止
			 * 取指器把数据或 MMIO 当指令流进行推测访问。
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_XN;

			/* Also setup NX memory mapping */
			mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_XN;
			mem_types[MT_MEMORY_RO].prot_sect |= PMD_SECT_XN;
		}
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/*
			 * For ARMv7 with TEX remapping,
			 * - shared device is SXCB=1100
			 * - nonshared device is SXCB=0100
			 * - write combine device mem is SXCB=0001
			 * (Uncached Normal memory)
			 */
			/* TRE 重映射由硬件重新解释 TEX/C/B，按该模式写入最小差异位。 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
		} else if (cpu_is_xsc3()) {
			/*
			 * For Xscale3,
			 * - shared device is TEXCB=00101
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Inner/Outer Uncacheable in xsc3 parlance)
			 */
			/* XScale3 使用私有 TEXCB 编码，不能复用 ARMv7 TRE 数值。 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1) | PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		} else {
			/*
			 * For ARMv6 and ARMv7 without TEX remapping,
			 * - shared device is TEXCB=00001
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Uncached Normal in ARMv6 parlance).
			 */
			/* 普通 extended 格式下分别组合 B/TEX，完成三类设备属性。 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		}
	} else {
		/*
		 * On others, write combining is "Uncached/Buffered"
		 */
		/* 旧格式没有上述设备编码，只能用 bufferable 近似 write-combine。 */
		mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
	}

	/*
	 * Now deal with the memory-type mappings
	 */
	cp = &cache_policies[cachepolicy];
	vecs_pgprot = kern_pgprot = user_pgprot = cp->pte;
	/*
	 * 三个局部模板起点相同，随后分别叠加用户 PXN、内核 AF/dirty、向量
	 * 特殊类型。分开维护可避免为共享物理向量页赋予过宽的普通 RAM 权限。
	 */

#ifndef CONFIG_ARM_LPAE
	/*
	 * We don't use domains on ARMv6 (since this causes problems with
	 * v6/v7 kernels), so we must use a separate memory type for user
	 * r/o, kernel r/w to map the vectors page.
	 */
	if (cpu_arch == CPU_ARCH_ARMv6)
		vecs_pgprot |= L_PTE_MT_VECTORS;

	/*
	 * Check is it with support for the PXN bit
	 * in the Short-descriptor translation table format descriptors.
	 */
	if (cpu_arch == CPU_ARCH_ARMv7 &&
		(read_cpuid_ext(CPUID_EXT_MMFR0) & 0xF) >= 4) {
		/*
		 * 支持短描述符 PXN table 时，把限制放在用户页表上级项；后续叶子
		 * 即使可执行，特权态也不能经该用户表执行。
		 */
		user_pmd_table |= PMD_PXNTABLE;
	}
#endif

	/*
	 * ARMv6 and above have extended page tables.
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP)) {
#ifndef CONFIG_ARM_LPAE
		/*
		 * Mark cache clean areas and XIP ROM read only
		 * from SVC mode and no access from userspace.
		 */
		mem_types[MT_ROM].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_MINICLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_MEMORY_RO].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
#endif

		/*
		 * If the initial page tables were created with the S bit
		 * set, then we need to do the same here for the same
		 * reasons given in early_cachepolicy().
		 */
		if (initial_pmd_value & PMD_SECT_S) {
			/*
			 * 阶段 4：启动描述符已 shared 时，所有可能别名映射的普通内存、
			 * 向量和 cacheable/WC 设备必须同步补 S，维持属性一致性。
			 */
			user_pgprot |= L_PTE_SHARED;
			kern_pgprot |= L_PTE_SHARED;
			vecs_pgprot |= L_PTE_SHARED;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_WC].prot_pte |= L_PTE_SHARED;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_CACHED].prot_pte |= L_PTE_SHARED;
			/*
			 * 以下普通 RAM 类型是机械重复的属性传播：section 与 PTE
			 * 两种粒度同时补 shared；DMA-ready 仅支持 PTE，故只改叶子。
			 */
			mem_types[MT_MEMORY_RWX].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RWX].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RW].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RW].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RO].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RO].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_pte |= L_PTE_SHARED;
		}
	}

	/*
	 * Non-cacheable Normal - intended for memory areas that must
	 * not cause dirty cache line writebacks when used
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6) {
		/*
		 * 阶段 5：为“不产生脏 cache 回写”的 Normal memory 选择当前格式的
		 * non-cacheable 编码；它仍是普通内存，不应误写成 Device 类型。
		 */
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/* Non-cacheable Normal is XCB = 001 */
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |=
				PMD_SECT_BUFFERED;
		} else {
			/* For both ARMv6 and non-TEX-remapping ARMv7 */
			mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |=
				PMD_SECT_TEX(1);
		}
	} else {
		mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= PMD_SECT_BUFFERABLE;
	}

#ifdef CONFIG_ARM_LPAE
	/*
	 * Do not generate access flag faults for the kernel mappings.
	 */
	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		/*
		 * LPAE 内核映射预置 AF，避免启动后首次访问产生 access-flag fault；
		 * 只对存在的 section 模板补 block AF，零值继续表示不支持。
		 */
		mem_types[i].prot_pte |= PTE_EXT_AF;
		if (mem_types[i].prot_sect)
			mem_types[i].prot_sect |= PMD_SECT_AF;
	}
	kern_pgprot |= PTE_EXT_AF;
	vecs_pgprot |= PTE_EXT_AF;

	/*
	 * Set PXN for user mappings
	 */
	user_pgprot |= PTE_EXT_PXN;
#endif

	/*
	 * 发布准备：把 CPU 相关用户基础位合入 16 种 VMA 权限组合。此后
	 * vm_get_page_prot() 返回值既含通用读写执行语义，也含本机 cache/PXN。
	 */
	for (i = 0; i < 16; i++) {
		pteval_t v = pgprot_val(protection_map[i]);
		protection_map[i] = __pgprot(v | user_pgprot);
	}

	mem_types[MT_LOW_VECTORS].prot_pte |= vecs_pgprot;
	mem_types[MT_HIGH_VECTORS].prot_pte |= vecs_pgprot;

	pgprot_user   = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | user_pgprot);
	pgprot_kernel = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG |
				 L_PTE_DIRTY | kern_pgprot);
	/*
	 * 这里正式发布通用用户/内核叶子模板；后续 early fixmap 和页表分配路径
	 * 才能安全使用普通内存属性。
	 */

	mem_types[MT_LOW_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_HIGH_VECTORS].prot_l1 |= ecc_mask;
	/*
	 * 普通 RAM 三种权限模板共享同一 cache/ECC section 基础，但分别保留
	 * 自己的 RWX/RW/RO 访问位；叶子统一合入 kern_pgprot。
	 */
	mem_types[MT_MEMORY_RWX].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY_RWX].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_RW].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY_RW].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_RO].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY_RO].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_DMA_READY].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_RWX_NONCACHED].prot_sect |= ecc_mask;
	mem_types[MT_ROM].prot_sect |= cp->pmd;

	switch (cp->pmd) {
	/*
	 * cache-clean 别名必须与普通 RAM 的 write-through/write-back 大类一致；
	 * write-allocate 在 clean 操作上按 write-back 编码处理。
	 */
	case PMD_SECT_WT:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WT;
		break;
	case PMD_SECT_WB:
	case PMD_SECT_WBWA:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WB;
		break;
	}
	pr_info("Memory policy: %sData cache %s\n",
		ecc_mask ? "ECC enabled, " : "", cp->policy);

	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		/*
		 * domain 属于一级/section 描述符而不是叶子软件权限。最后统一合入，
		 * 可保证前面针对 CPU 代际的位清理不会误伤 domain 编码。
		 */
		struct mem_type *t = &mem_types[i];
		if (t->prot_l1)
			t->prot_l1 |= PMD_DOMAIN(t->domain);
		if (t->prot_sect)
			t->prot_sect |= PMD_DOMAIN(t->domain);
	}
	/*
	 * 完成不变量：每个可用模板均包含最终 domain/cache/权限位，所有全局
	 * pgprot 已发布；__ro_after_init 生效后不再允许运行期改写。
	 */
}

#ifdef CONFIG_ARM_DMA_MEM_BUFFERABLE
/*
 * phys_mem_access_prot - 为用户态物理内存映射选择 cache 属性。
 *
 * file 只借用以读取 O_SYNC，pfn 是起始页帧，size 当前不参与决策，
 * vma_prot 是通用 MM 已生成的基础权限。无效 PFN 通常代表设备地址，必须
 * noncached；有效 RAM 且 O_SYNC 时选 write-combine，其他情况保留原属性。
 * 返回值只是 pgprot，不创建映射、不持有 file，也没有可回滚资源。
 *
 * 入参：
 *   @file：当前映射文件的借用指针，纯输入、非 NULL；只读 f_flags。
 *   @pfn：映射起始页帧号，纯输入，单位为 PAGE_SIZE。
 *   @size：请求长度，单位字节，纯输入；当前 ARM 决策不使用该值。
 *   @vma_prot：调用者给出的基础页属性，纯输入。
 * 前置条件：调用者稳定 file 生命周期；函数无锁、不可睡眠且不改页表。
 * 出参：无。
 * 返回：设备 PFN 返回 noncached 属性；RAM+O_SYNC 返回 writecombine；
 * 其余返回原 vma_prot，没有错误类别。
 * 副作用：无，不改变 file/VMA，也不取得引用。
 * 失败结果：无；PFN 不可验证被当作设备映射的保守分支。
 */
pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	/* 设备/空洞 PFN 不能使用普通 RAM cache 属性，优先返回 noncached。 */
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	/* O_SYNC 对有效 RAM 请求避免普通 write-back，改用 write-combine。 */
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	/* 普通有效 RAM 映射保留 VMA 已计算的基础属性。 */
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);
#endif

/*
 * vectors_base() 把当前 CPU 向量模式归一化成 create_mapping() 允许进入用户
 * 区的唯一例外虚拟地址：高向量为 0xffff0000，低向量为 0。
 */
#define vectors_base()	(vectors_high() ? 0xffff0000 : 0)

/*
 * early_alloc/late_alloc 是同一页表构造器的两个分配后端。
 *
 * early_alloc 在伙伴分配器工作前从 memblock 按请求大小自对齐分配，失败即
 * panic，返回的零化内存由启动页表永久接管。late_alloc 在页表基础设施可用
 * 后分配并执行 PTE 页构造；显式去掉 __GFP_HIGHMEM，确保内核可直接寻址。
 * 两者均返回已归调用方所有的内核虚拟地址，不提供可恢复失败结果。
 */
/*
 * early_alloc：
 * @sz 是分配字节数，纯输入且同时作为对齐值，必须为页表要求的二次幂大小。
 * 无出参；成功返回 memblock 分配区的非 NULL 拥有指针，ownership 转给页表
 * 构造路径。启动期无锁；memblock 分配不进入普通 reclaim。失败直接 panic。
 * 副作用是从 memblock 可分配集合永久占用该范围。
 */
static void __init *early_alloc(unsigned long sz)
{
	return memblock_alloc_or_panic(sz, sz);

}

/*
 * late_alloc：
 * @sz 是页表存储字节数，纯输入；无出参。允许睡眠的 init 上下文中通过
 * pagetable_alloc(GFP_PGTABLE_KERNEL) 分配，成功返回已完成 PTE ctor 的内核
 * 直映地址，ownership 转给目标 mm。分配或构造失败触发 BUG，无错误指针。
 * 副作用是分配页表页并建立其页表描述对象状态。
 */
static void *__init late_alloc(unsigned long sz)
{
	/*
	 * 分配阶段：去掉 __GFP_HIGHMEM，保证返回页表页可由内核虚拟地址直接
	 * 初始化；get_order() 把字节规模换算为伙伴分配阶数。
	 */
	void *ptdesc = pagetable_alloc(GFP_PGTABLE_KERNEL & ~__GFP_HIGHMEM,
			get_order(sz));

	/*
	 * 构造阶段：分配对象还不能作为 PTE 表使用，必须通过 ctor 建立页表
	 * 元数据。任一步失败均未发布到 PMD，直接 BUG，不留下外部可见半成品。
	 */
	if (!ptdesc || !pagetable_pte_ctor(NULL, ptdesc))
		BUG();
	return ptdesc_address(ptdesc);
}

/*
 * arm_pte_alloc - 确保 addr 所在 PMD 已连接 PTE 表并返回对应叶子槽。
 *
 * pmd 由调用者借入，prot 是表描述符属性，alloc 决定早/晚分配阶段。空 PMD
 * 时分配 ARM 软件 PTE 与紧随其后的硬件 PTE 所需整块空间，再通过
 * __pmd_populate() 发布物理地址；已有 PMD 则复用。坏 PMD 表明页表层级冲突，
 * 启动期无法安全回滚，直接 BUG。返回指针借用自页表，不转移页表所有权。
 *
 * 入参：
 *   @pmd：目标 PMD 槽的借用输入输出指针，非 NULL；空槽可能被本函数填充。
 *   @addr：目标虚拟字节地址，纯输入，用于选择 PTE 索引。
 *   @prot：新建表描述符属性位，纯输入。
 *   @alloc：分配后端函数指针，纯输入、非 NULL；返回存储 ownership。
 * 前置条件：启动期调用者独占目标页表槽；能否睡眠由 alloc 后端决定。
 * 出参：@pmd 可能从 none 变为指向新 PTE 表。
 * 返回：对应 @addr 的借用 PTE 指针；无 NULL/错误指针类别。
 * 副作用：必要时分配页表并向硬件页表层级发布。
 * 失败结果：分配后端或坏 PMD 触发 panic/BUG，不提供局部回滚。
 */
static pte_t * __init arm_pte_alloc(pmd_t *pmd, unsigned long addr,
				unsigned long prot,
				void *(*alloc)(unsigned long sz))
{
	/*
	 * 获取阶段：仅空 PMD 需要新表；alloc 返回的 storage 尚不可被 walker
	 * 发现，__pmd_populate() 是把其物理地址提交到上级描述符的发布点。
	 */
	if (pmd_none(*pmd)) {
		pte_t *pte = alloc(PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE);
		__pmd_populate(pmd, __pa(pte), prot);
	}
	/*
	 * 校验与返回阶段：非空但类型错误意味着 section/PTE 层级冲突。通过
	 * BUG 阻止把错误描述符解释成表地址；合法时只借出 addr 对应槽。
	 */
	BUG_ON(pmd_bad(*pmd));
	return pte_offset_kernel(pmd, addr);
}

/*
 * early_pte_alloc 固定选择 memblock 后端，供正式内存分配器启用前的映射使用。
 *
 * @pmd 是借用的输入输出 PMD，@addr 是虚拟字节地址，@prot 是表属性；参数
 * ownership 均不转移。无独立出参，@pmd 可能被填充；返回借用 PTE 槽，
 * 失败按 early_alloc/arm_pte_alloc 直接 panic/BUG。启动期无锁、不走 reclaim。
 */
static pte_t * __init early_pte_alloc(pmd_t *pmd, unsigned long addr,
				      unsigned long prot)
{
	return arm_pte_alloc(pmd, addr, prot, early_alloc);
}

/*
 * alloc_init_pte - 用连续 PFN 填充 [addr, end) 的页粒度映射。
 *
 * addr/end 必须页对齐且非空，pfn 与 addr 同步递增；type 提供表和叶子属性，
 * ng 指定映射是否为 non-global。函数可能通过 alloc 新建并发布 PTE 表，
 * 随后逐项写入硬件描述符。分配失败策略由后端决定且均不可恢复，因此没有
 * 部分填充回滚路径；调用者只在启动期、无并发页表访问者时使用。
 *
 * 入参：
 *   @pmd：借用的目标 PMD 输入输出槽；必要时连接新 PTE 表。
 *   @addr：区间起始虚拟字节地址，页对齐。
 *   @end：区间结束虚拟字节地址，页对齐且大于 addr。
 *   @pfn：首个物理页帧号，随虚拟页一一递增。
 *   @type：借用的只读内存类型，非 NULL，调用期间稳定。
 *   @alloc：非 NULL 分配后端，ownership 规则由其契约决定。
 *   @ng：纯输入；true 为每项添加 non-global 属性。
 * 前置条件：启动期独占目标槽；能否睡眠取决于 alloc。
 * 出参：@pmd/PTE 表被填充，其他参数不变。
 * 返回：无直接返回值。
 * 副作用：可能分配页表并发布 [addr,end) 的连续硬件 PTE。
 * 失败结果：分配失败或坏 PMD 直接 BUG/panic，不返回部分成功。
 */
static void __init alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  const struct mem_type *type,
				  void *(*alloc)(unsigned long sz),
				  bool ng)
{
	pte_t *pte = arm_pte_alloc(pmd, addr, type->prot_l1, alloc);
	/*
	 * 填充阶段：表已连接但各叶子尚未发布。每轮把当前 PFN 与类型属性编码
	 * 为 PTE，并同步推进 PTE 指针、虚拟地址和 PFN，保持三者一一对应。
	 */
	do {
		set_pte_ext(pte, pfn_pte(pfn, __pgprot(type->prot_pte)),
			    ng ? PTE_EXT_NG : 0);
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

/*
 * __map_init_section - 直接写入连续 section/block 描述符。
 *
 * addr/end/phys 已由上层验证为 SECTION_SIZE 对齐，type->prot_sect 非零。
 * 非 LPAE 的一个 Linux PMD 包含两个 1 MiB 硬件项，所以奇数 section 需把
 * 指针推进一项。循环写完后 flush_pmd_entry(p) 清理/同步页表描述符 cache，
 * 使硬件 walker 能看到新值；这不是 TLB 失效，也不释放任何旧物理页。
 *
 * 入参：
 *   @pmd：借用的首个目标 PMD 输入输出槽，非 NULL。
 *   @addr/@end：section 对齐的虚拟半开区间，单位字节。
 *   @phys：与 addr 对应的 section 对齐物理字节地址。
 *   @type：借用且稳定的内存类型，prot_sect 必须非零。
 *   @ng：是否添加 non-global 位。
 * 前置条件：启动期独占范围、无需锁、不睡眠；范围已由上层验证可用大页。
 * 出参：目标 PMD 序列被覆盖为连续 section 描述符。
 * 返回：无直接返回值。
 * 副作用：写硬件页表并执行 PMD cache 同步，不做 TLB flush。
 * 失败结果：无可恢复失败；错误对齐会生成错误映射，故由调用者保证。
 */
static void __init __map_init_section(pmd_t *pmd, unsigned long addr,
			unsigned long end, phys_addr_t phys,
			const struct mem_type *type, bool ng)
{
	pmd_t *p = pmd;

#ifndef CONFIG_ARM_LPAE
	/*
	 * In classic MMU format, puds and pmds are folded in to
	 * the pgds. pmd_offset gives the PGD entry. PGDs refer to a
	 * group of L1 entries making up one logical pointer to
	 * an L2 table (2MB), where as PMDs refer to the individual
	 * L1 entries (1MB). Hence increment to get the correct
	 * offset for odd 1MB sections.
	 * (See arch/arm/include/asm/pgtable-2level.h)
	 */
	if (addr & SECTION_SIZE)
		pmd++;
#endif
	/*
	 * 发布阶段：连续写 section 描述符，phys 与 addr 同步按 1 MiB 推进。
	 * p 保存首槽，循环后只需从该入口执行描述符 cache 同步。
	 */
	do {
		*pmd = __pmd(phys | type->prot_sect | (ng ? PMD_SECT_nG : 0));
		phys += SECTION_SIZE;
	} while (pmd++, addr += SECTION_SIZE, addr != end);

	flush_pmd_entry(p);
	/* 完成后 walker 可见新 PMD；这里仍未处理可能存在的旧 TLB 翻译。 */
}

/*
 * alloc_init_pmd - 在一个 PUD 范围内选择 section 快路或 PTE 慢路。
 *
 * 同时满足类型支持 section 且虚拟起点、物理起点、子区间终点均对齐时，
 * 直接建大粒度映射；否则分配 PTE 表逐页映射。每轮用 pmd_addr_end() 截断
 * 到层级边界，并保持 phys 与 addr 的差值不变。代价是页映射多占内存和
 * TLB 项，但可表达非对齐范围或更细权限。
 *
 * 入参：
 *   @pud：借用的目标 PUD，非 NULL。
 *   @addr/@end：待映射虚拟半开区间，单位字节，页对齐。
 *   @phys：与 addr 对应的物理字节地址。
 *   @type：借用内存类型，决定 section 能力和叶子属性。
 *   @alloc：非 NULL 页表分配后端。
 *   @ng：是否创建 non-global 映射。
 * 前置条件：启动期独占目标页表；睡眠能力由 alloc 决定。
 * 出参：PUD 下相关 PMD/PTE 被创建并填充。
 * 返回：无直接返回值。
 * 副作用：发布页表映射，可能分配 PTE 表并同步 PMD cache。
 * 失败结果：分配或坏表失败为 BUG/panic，无事务回滚。
 */
static void __init alloc_init_pmd(pud_t *pud, unsigned long addr,
				      unsigned long end, phys_addr_t phys,
				      const struct mem_type *type,
				      void *(*alloc)(unsigned long sz), bool ng)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		/*
		 * With LPAE, we must loop over to map
		 * all the pmds for the given range.
		 */
		next = pmd_addr_end(addr, end);

		/*
		 * Try a section mapping - addr, next and phys must all be
		 * aligned to a section boundary.
		 */
		if (type->prot_sect &&
				((addr | next | phys) & ~SECTION_MASK) == 0) {
			/*
			 * 快路径：整个当前子区间满足大页对齐且类型支持 section，
			 * 无需额外 PTE 页，直接发布 block/section。
			 */
			__map_init_section(pmd, addr, next, phys, type, ng);
		} else {
			/*
			 * 回退路径：粒度或类型约束不满足时分配叶子表逐页映射，
			 * 以额外页表内存/TLB 项换取非对齐与细粒度表达能力。
			 */
			alloc_init_pte(pmd, addr, next,
				       __phys_to_pfn(phys), type, alloc, ng);
		}

		phys += next - addr;

		/* 本 PMD 子区间已完整，推进到下一边界并保持虚实偏移不变。 */
	} while (pmd++, addr = next, addr != end);
}

/*
 * alloc_init_pud/alloc_init_p4d 只负责跨越折叠或真实的上级页表边界。
 * 每层把当前子区间和对应物理起点下传，完成后按映射字节数推进 phys；
 * ARM32 上部分层级可能折叠，但统一写法让 LPAE 与非 LPAE 共用主算法。
 */
/*
 * alloc_init_pud：
 * @p4d 是借用目标上级表；@addr/@end 是页对齐虚拟半开区间；@phys 是对应
 * 物理字节起点；@type 为借用属性；@alloc 为分配后端；@ng 控制 non-global。
 * 启动期独占页表，睡眠取决于 alloc。无直接返回值；出参是 PUD 以下映射，
 * 副作用及失败语义完全继承 alloc_init_pmd()，参数 ownership 不转移。
 */
static void __init alloc_init_pud(p4d_t *p4d, unsigned long addr,
				  unsigned long end, phys_addr_t phys,
				  const struct mem_type *type,
				  void *(*alloc)(unsigned long sz), bool ng)
{
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		/* 当前 PUD 子区间交给 PMD 层完成，返回时该段映射已全部发布。 */
		alloc_init_pmd(pud, addr, next, phys, type, alloc, ng);
		phys += next - addr;
	} while (pud++, addr = next, addr != end);
}

/*
 * alloc_init_p4d：
 * @pgd 是借用目标顶层表；@addr/@end 是页对齐虚拟半开区间；@phys 是对应
 * 物理字节起点；@type 为借用属性；@alloc 为分配后端；@ng 控制 non-global。
 * 启动期独占页表，睡眠取决于 alloc。无直接返回值；出参是 P4D 以下映射，
 * 可能分配页表并发布硬件描述符，失败为 BUG/panic，ownership 不转移。
 */
static void __init alloc_init_p4d(pgd_t *pgd, unsigned long addr,
				  unsigned long end, phys_addr_t phys,
				  const struct mem_type *type,
				  void *(*alloc)(unsigned long sz), bool ng)
{
	p4d_t *p4d = p4d_offset(pgd, addr);
	unsigned long next;

	do {
		next = p4d_addr_end(addr, end);
		/* 当前 P4D 子区间完成后再推进物理起点，避免跨上级边界错配。 */
		alloc_init_pud(p4d, addr, next, phys, type, alloc, ng);
		phys += next - addr;
	} while (p4d++, addr = next, addr != end);
}

#ifndef CONFIG_ARM_LPAE
/*
 * create_36bit_mapping - 用 ARMv6 supersection 映射 4 GiB 以上物理地址。
 *
 * md 描述虚拟起点、PFN、长度和类型，mm 持有目标页表，ng 控制 global 位；
 * 均为借用对象。仅 ARMv6+/XScale3、domain 0 且虚拟/物理/长度 16 MiB 对齐
 * 才可编码。高物理地址 [35:32] 被移入描述符 [23:20]，每个 supersection
 * 连写 16 个 section 项。任何前置条件失败只打印错误且不建立映射；进入
 * 写循环后无分配失败和回滚路径，依赖启动期无并发 walker 修改同一区间。
 *
 * 入参：
 *   @mm：借用的目标地址空间，非 NULL；页表由其持有。
 *   @md：借用的纯输入映射描述符，非 NULL；函数不保存、不释放。
 *   @type：借用且稳定的内存类型，必须使用 domain 0。
 *   @ng：是否设置 non-global。
 * 前置条件：非 LPAE、CPU 支持 supersection，启动期独占目标范围、不睡眠。
 * 出参：@mm 页表中相应 PMD 序列可能被写入；md/type 不变。
 * 返回：无直接返回值。
 * 副作用：成功时发布连续 16 MiB supersection 描述符并打印诊断日志。
 * 失败结果：CPU、domain 或对齐不满足时只打印错误，目标范围保持未映射。
 */
static void __init create_36bit_mapping(struct mm_struct *mm,
					struct map_desc *md,
					const struct mem_type *type,
					bool ng)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	pgd_t *pgd;

	/*
	 * 准备阶段：把 map_desc 转成对齐后的虚拟/物理范围。length 向页上取整，
	 * 后续还会执行更严格的 16 MiB supersection 对齐校验。
	 */
	addr = md->virtual;
	phys = __pfn_to_phys(md->pfn);
	length = PAGE_ALIGN(md->length);

	/*
	 * 能力校验：旧于 ARMv6 且非 XScale3 的描述符没有 supersection 高地址
	 * 编码。失败时尚未定位或写入任何目标 PMD。
	 */
	if (!(cpu_architecture() >= CPU_ARCH_ARMv6 || cpu_is_xsc3())) {
		pr_err("MM: CPU does not support supersection mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/* N.B.	ARMv6 supersections are only defined to work with domain 0.
	 *	Since domain assignments can in fact be arbitrary, the
	 *	'domain == 0' check below is required to insure that ARMv6
	 *	supersections are only allocated for domain 0 regardless
	 *	of the actual domain assignments in use.
	 */
	/* domain 校验独立于 CPU 能力；非零 domain 同样在首次写表前拒绝。 */
	if (type->domain) {
		pr_err("MM: invalid domain in supersection mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/*
	 * 校验阶段：虚拟起点、物理起点和长度必须同时 16 MiB 对齐。所有拒绝
	 * 分支都位于首次 PMD 写入前，因此失败保证目标页表完全不变。
	 */
	if ((addr | length | __pfn_to_phys(md->pfn)) & ~SUPERSECTION_MASK) {
		pr_err("MM: cannot create mapping for 0x%08llx at 0x%08lx invalid alignment\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/*
	 * Shift bits [35:32] of address into bits [23:20] of PMD
	 * (See ARMv6 spec).
	 */
	phys |= (((md->pfn >> (32 - PAGE_SHIFT)) & 0xF) << 20);

	/*
	 * 发布阶段：每个 16 MiB supersection 复制为 16 个硬件 section 项；
	 * 外层循环再推进虚拟、物理和 PGD，使整个请求连续覆盖。
	 */
	pgd = pgd_offset(mm, addr);
	end = addr + length;
	do {
		/*
		 * 当前 supersection 沿折叠页表层级定位到首 PMD；局部指针均为
		 * 借用槽，只在本轮 16 项写入期间有效。
		 */
		p4d_t *p4d = p4d_offset(pgd, addr);
		pud_t *pud = pud_offset(p4d, addr);
		pmd_t *pmd = pmd_offset(pud, addr);
		int i;

		for (i = 0; i < 16; i++)
			*pmd++ = __pmd(phys | type->prot_sect | PMD_SECT_SUPER |
				       (ng ? PMD_SECT_nG : 0));

		/* 本块 16 项已发布，三个游标同步推进到下一 16 MiB 边界。 */
		addr += SUPERSECTION_SIZE;
		phys += SUPERSECTION_SIZE;
		pgd += SUPERSECTION_SIZE >> PGDIR_SHIFT;
	} while (addr != end);
}
#endif	/* !CONFIG_ARM_LPAE */

/*
 * __create_mapping - 把一个 map_desc 翻译为目标 mm 的多级 ARM 页表项。
 *
 * mm 与 md 均为借用；md->virtual 可不页对齐，md->pfn 给出物理页起点，
 * md->length 为原始字节数，md->type 必须是有效 MT_* 索引。alloc 选择页表页
 * 来源，ng 控制 non-global。函数先把范围向页边界扩展，非 LPAE 的高 PFN
 * 转交 supersection 路径，再按 PGD/P4D/PUD/PMD 层级切片。
 *
 * 若类型没有 prot_l1 且范围又不能完全用 section 表达，函数告警并保持区间
 * 不映射；分配后端失败则 panic/BUG，不返回半成功状态。成功后页表归 mm
 * 持有，map_desc 本身仍归调用者；这里只发布描述符，不做全局 TLB flush，
 * 上层必须在页表可能已有缓存翻译时选择适当的同步边界。
 *
 * 入参：
 *   @mm：借用的目标地址空间，非 NULL；其页表接收新映射。
 *   @md：借用输入描述符，非 NULL；virtual/length 单位字节，pfn 单位页帧。
 *   @alloc：非 NULL 分配后端；返回页表存储的 ownership 交给 mm。
 *   @ng：是否给叶子或 block 添加 non-global。
 * 前置条件：调用者独占目标范围；init 上下文，睡眠能力取决于 alloc。
 * 出参：@mm 页表可能新增映射；md 不修改。
 * 返回：无直接返回值。
 * 副作用：可能分配并发布多级页表、同步 PMD cache；不刷新整个 TLB。
 * 失败结果：类型不能表达所需粒度时告警并保持未映射；分配失败致命。
 */
static void __init __create_mapping(struct mm_struct *mm, struct map_desc *md,
				    void *(*alloc)(unsigned long sz),
				    bool ng)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	const struct mem_type *type;
	pgd_t *pgd;

	/*
	 * 准备阶段：md->type 的合法性由描述符生产者保证；取得的是全局只读
	 * 模板借用，不需要增加引用或在返回时释放。
	 */
	type = &mem_types[md->type];

#ifndef CONFIG_ARM_LPAE
	/*
	 * Catch 36-bit addresses
	 */
	if (md->pfn >= 0x100000) {
		/* 高于 4 GiB 的 PFN 只能转交短描述符 supersection 专用编码。 */
		create_36bit_mapping(mm, md, type, ng);
		return;
	}
#endif

	addr = md->virtual & PAGE_MASK;
	phys = __pfn_to_phys(md->pfn);
	length = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));
	/*
	 * virtual 向下取整后，长度必须补入原页内偏移再向上取整；这样原请求
	 * 的首尾字节都被覆盖，同时 phys 仍对应 md->pfn 所在页。
	 */

	/*
	 * 校验阶段：类型若没有 PTE 表属性，就必须由完整 section 表达。非对齐
	 * 请求在分配或写表前拒绝，因而失败不会留下部分映射。
	 */
	if (type->prot_l1 == 0 && ((addr | phys | length) & ~SECTION_MASK)) {
		pr_warn("BUG: map for 0x%08llx at 0x%08lx can not be mapped using pages, ignoring.\n",
			(long long)__pfn_to_phys(md->pfn), addr);
		return;
	}

	pgd = pgd_offset(mm, addr);
	end = addr + length;
	/*
	 * 构造阶段：按 PGD 边界批处理，子层负责在 section 与 PTE 之间选择。
	 * 每轮返回时 [旧 addr,next) 已发布，随后才推进 phys/addr。
	 */
	do {
		unsigned long next = pgd_addr_end(addr, end);

		/* 子层只处理到当前 PGD 边界，避免跨项时错误复用上级指针。 */
		alloc_init_p4d(pgd, addr, next, phys, type, alloc, ng);

		phys += next - addr;
		addr = next;
	} while (pgd++, addr != end);
}

/*
 * Create the page directory entries and any necessary
 * page tables for the mapping specified by `md'.  We
 * are able to cope here with varying sizes and address
 * offsets, and we take full advantage of sections and
 * supersections.
 */
/*
 * create_mapping - 在 init_mm 中建立早期静态映射并执行地址空间约束检查。
 *
 * md 只在调用期间借用。除低/高异常向量外，拒绝把内核静态映射放进用户
 * 地址区；设备映射若落在线性区等非 vmalloc/fixmap 区域则告警，因为可能
 * 造成属性别名，但为兼容旧平台仍继续创建。页表页来自 memblock，映射为
 * global。函数无错误返回，调用者通过启动日志发现被拒绝的请求。
 *
 * 入参：
 *   @md：借用纯输入映射描述符，非 NULL；字段单位同 __create_mapping()。
 * 前置条件：init_mm 启动期串行构造、无需页表锁；memblock 可用，不睡眠。
 * 出参：init_mm 页表可能增加映射；md 保持不变。
 * 返回：无直接返回值。
 * 副作用：可能从 memblock 分配页表并发布 global 映射、输出地址告警。
 * 失败结果：非法用户区请求被拒绝；设备位置异常只告警；分配失败 panic。
 */
static void __init create_mapping(struct map_desc *md)
{
	/*
	 * 校验阶段 1：普通内核静态映射不能侵入用户区；异常向量是体系结构
	 * 明确允许的例外。拒绝发生在任何页表分配前。
	 */
	if (md->virtual != vectors_base() && md->virtual < TASK_SIZE) {
		pr_warn("BUG: not creating mapping for 0x%08llx at 0x%08lx in user region\n",
			(long long)__pfn_to_phys((u64)md->pfn), md->virtual);
		return;
	}

	if (md->type == MT_DEVICE &&
	    md->virtual >= PAGE_OFFSET && md->virtual < FIXADDR_START &&
	    (md->virtual < VMALLOC_START || md->virtual >= VMALLOC_END)) {
		pr_warn("BUG: mapping for 0x%08llx at 0x%08lx out of vmalloc space\n",
			(long long)__pfn_to_phys((u64)md->pfn), md->virtual);
	}

	/*
	 * 提交阶段：设备位置告警不阻止兼容平台继续；统一使用 init_mm、
	 * memblock 后端和 global 属性进入公共构造器。
	 */
	__create_mapping(&init_mm, md, early_alloc, false);
}

/*
 * create_mapping_late - 在页表分配器可用后为指定 mm 建立同类静态映射。
 *
 * LPAE 的上级层不一定预建，先用通用 p4d/pud/pmd_alloc 补齐；任一分配失败
 * 只 WARN 并返回，已建上级表留在 mm 中供后续复用。叶子 PTE 表由 late_alloc
 * 分配并构造，ng 由调用者决定。非 LPAE 上层折叠，直接进入公共映射算法。
 *
 * 入参：
 *   @mm：借用目标地址空间，非 NULL；调用者保证 init 阶段稳定。
 *   @md：借用纯输入映射描述符，非 NULL，不保存。
 *   @ng：是否建立 non-global 映射。
 * 前置条件：页表分配器可用；init 上下文允许 GFP_PGTABLE_KERNEL 睡眠，
 * 调用者保证目标范围无并发修改。
 * 出参：@mm 可能新增上级表和最终映射。
 * 返回：无直接返回值。
 * 副作用：分配页表页、运行 PTE ctor 并发布描述符。
 * 失败结果：LPAE 上级分配失败 WARN 后返回，已分配层级留给 mm；叶子失败 BUG。
 */
void __init create_mapping_late(struct mm_struct *mm, struct map_desc *md,
				bool ng)
{
#ifdef CONFIG_ARM_LPAE
	p4d_t *p4d;
	pud_t *pud;

	/*
	 * LPAE 准备阶段：先补齐通用分配器管理的上级层。WARN 返回都发生在
	 * 叶子映射前；已经成功分配的层级继续归 mm，供以后调用复用。
	 */
	p4d = p4d_alloc(mm, pgd_offset(mm, md->virtual), md->virtual);
	if (WARN_ON(!p4d))
		return;
	pud = pud_alloc(mm, p4d, md->virtual);
	if (WARN_ON(!pud))
		return;
	pmd_alloc(mm, pud, 0);
#endif
	/* 叶子提交阶段：选择可睡眠的 late_alloc，并按调用者要求设置 nG。 */
	__create_mapping(mm, md, late_alloc, ng);
}

/*
 * Create the architecture specific mappings
 */
/*
 * iotable_init - 建立机器描述符提供的静态 I/O 映射并登记 vmalloc 占用。
 *
 * io_desc 指向 nr 个 map_desc，数组仍归平台代码；nr 为零无操作。函数一次
 * memblock 分配同样数量的 static_vm，逐项先写页表，再把对齐后的虚拟范围、
 * 物理起点、内存类型和调用者登记进 static_vmlist。登记使后续 ioremap/
 * vmalloc 不会重复占用该区间；static_vm 生命周期延续到运行期且不单独释放。
 * memblock 或映射前置条件失败没有事务式回滚，符合不可恢复的早期启动语义。
 *
 * 入参：
 *   @io_desc：借用的 map_desc 数组；nr>0 时非 NULL，数组不被修改。
 *   @nr：数组元素数，允许为 0，不允许为负值。
 * 前置条件：启动期串行，memblock 和 init_mm 可用；不持锁、不主动睡眠。
 * 出参：无输出参数；init_mm/static_vmlist 获得对应映射记录。
 * 返回：无直接返回值。
 * 副作用：分配 static_vm、建立静态 I/O 页表并向全局链表发布。
 * 失败结果：nr=0 无操作；分配失败 panic；平台必须保证每个描述符合法。
 */
void __init iotable_init(struct map_desc *io_desc, int nr)
{
	struct map_desc *md;
	struct vm_struct *vm;
	struct static_vm *svm;

	if (!nr)
		return;

	/*
	 * 获取阶段：一次分配全部登记对象，数组尚未发布。nr=0 已提前返回，
	 * 因而乘法结果对应至少一个 static_vm。
	 */
	svm = memblock_alloc_or_panic(sizeof(*svm) * nr, __alignof__(*svm));

	/*
	 * 逐项提交：先让页表映射生效，再完整初始化 vm_struct 并发布到
	 * static_vmlist，后续 ioremap/vmalloc 才能看到并避让该范围。
	 */
	for (md = io_desc; nr; md++, nr--) {
		create_mapping(md);

		vm = &svm->vm;
		vm->addr = (void *)(md->virtual & PAGE_MASK);
		vm->size = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));
		vm->phys_addr = __pfn_to_phys(md->pfn);
		vm->flags = VM_IOREMAP | VM_ARM_STATIC_MAPPING;
		vm->flags |= VM_ARM_MTYPE(md->type);
		vm->caller = iotable_init;
		add_static_vm_early(svm++);
	}
}

/*
 * vm_reserve_area_early - 只在 static_vmlist 预留虚拟范围，不建立页表映射。
 *
 * addr/size 由调用者保证对齐且不重叠，caller 仅作为诊断标识保存。新
 * static_vm 标记 VM_ARM_EMPTY_MAPPING，阻止 vmalloc/ioremap 分配该地址；
 * 对象来自 memblock 并由全局静态映射链表持有到运行期。
 *
 * 入参：
 *   @addr：预留虚拟起始字节地址。
 *   @size：预留字节数，必须非零且范围有效。
 *   @caller：诊断用借用标识，可为代码地址；不解引用、不取得引用。
 * 前置条件：启动期串行，static_vmlist 无并发读写；不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：从 memblock 分配并发布一个空 static_vm 占位区。
 * 失败结果：分配失败 panic；重叠或非法范围不在本函数检测。
 */
void __init vm_reserve_area_early(unsigned long addr, unsigned long size,
				  void *caller)
{
	struct vm_struct *vm;
	struct static_vm *svm;

	/* 获取阶段：新对象尚未进入全局链表，失败会直接终止启动。 */
	svm = memblock_alloc_or_panic(sizeof(*svm), __alignof__(*svm));

	/*
	 * 初始化与发布阶段：EMPTY 表示只有地址占位、没有页表映射；所有字段
	 * 写完后 add_static_vm_early() 才使对象全局可发现。
	 */
	vm = &svm->vm;
	vm->addr = (void *)addr;
	vm->size = size;
	vm->flags = VM_IOREMAP | VM_ARM_EMPTY_MAPPING;
	vm->caller = caller;
	add_static_vm_early(svm);
}

#ifndef CONFIG_ARM_LPAE

/*
 * The Linux PMD is made of two consecutive section entries covering 2MB
 * (see definition in include/asm/pgtable-2level.h).  However a call to
 * create_mapping() may optimize static mappings by using individual
 * 1MB section mappings.  This leaves the actual PMD potentially half
 * initialized if the top or bottom section entry isn't used, leaving it
 * open to problems if a subsequent ioremap() or vmalloc() tries to use
 * the virtual space left free by that unused section entry.
 *
 * Let's avoid the issue by inserting dummy vm entries covering the unused
 * PMD halves once the static mappings are in place.
 */

/*
 * pmd_empty_section_gap - 用一个空 static_vm 占住未使用的半 PMD section。
 *
 * addr 是 1 MiB 对齐的虚拟起点；函数不写页表，仅借 vmalloc 登记协议阻止
 * 后续分配者把该半区改造成与相邻 section 不兼容的 PTE 表。
 *
 * 入参：
 *   @addr：待占位 section 的虚拟字节地址，必须 SECTION_SIZE 对齐。
 * 前置条件：非 LPAE 启动期、static_vmlist 串行构造；不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：发布 1 MiB 空映射记录，不改变页表。
 * 失败结果：分配失败由 vm_reserve_area_early() panic。
 */
static void __init pmd_empty_section_gap(unsigned long addr)
{
	vm_reserve_area_early(addr, SECTION_SIZE, pmd_empty_section_gap);
}

/*
 * fill_pmd_gaps - 为非 LPAE 两级页表中“半个 PMD 空闲”的 1 MiB 缝隙占位。
 *
 * static_vmlist 已按虚拟地址描述静态映射。由于一个 Linux PMD 对应两个硬件
 * section 项，若只使用其中一半，后续 ioremap 若把另一半改成 PTE 表会破坏
 * 已有 section。函数扫描每个映射的首尾，仅在对应硬件项确实为空时插入
 * VM_ARM_EMPTY_MAPPING。next 跳过同一 PMD 内后续条目，避免重复预留。
 *
 * 入参：无。
 * 前置条件：平台静态映射已按虚拟地址登记，启动期串行、无锁、不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：可能向 static_vmlist 插入半 PMD 占位记录；不改现有映射。
 * 失败结果：占位分配失败 panic；已使用的 PMD 半区不会被覆盖。
 */
static void __init fill_pmd_gaps(void)
{
	struct static_vm *svm;
	struct vm_struct *vm;
	unsigned long addr, next = 0;
	pmd_t *pmd;

	/*
	 * 扫描阶段：static_vmlist 按地址遍历。next 表示上一个已处理映射之后的
	 * PMD 边界；落在其前的条目属于同一已审计 PMD，无需重复检查。
	 */
	list_for_each_entry(svm, &static_vmlist, list) {
		vm = &svm->vm;
		addr = (unsigned long)vm->addr;
		if (addr < next)
			continue;

		/*
		 * Check if this vm starts on an odd section boundary.
		 * If so and the first section entry for this PMD is free
		 * then we block the corresponding virtual address.
		 */
		/*
		 * 起点检查：映射从 PMD 的第二个 section 开始时，若第一个硬件项
		 * 为空，就为前半段登记占位，阻止未来把整个 PMD 改造成 PTE 表。
		 */
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr);
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr & PMD_MASK);
		}

		/*
		 * Then check if this vm ends on an odd section boundary.
		 * If so and the second section entry for this PMD is empty
		 * then we block the corresponding virtual address.
		 */
		/*
		 * 终点检查：映射恰好结束于 PMD 中点时，检查第二个硬件项；为空
		 * 才占住后半段，已有映射绝不会被占位记录覆盖。
		 */
		addr += vm->size;
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr) + 1;
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr);
		}

		/* no need to look at any vm entry until we hit the next PMD */
		next = (addr + PMD_SIZE - 1) & PMD_MASK;
	}
}

#else
/* LPAE 不存在“一个 Linux PMD 含两个 section”的半区问题，故为空操作。 */
#define fill_pmd_gaps() do { } while (0)
#endif

#if defined(CONFIG_PCI) && !defined(CONFIG_NEED_MACH_IO_H)
/*
 * pci_reserve_io - 在平台尚未登记时预留固定 PCI I/O 虚拟窗口。
 *
 * 已有同起点 static_vm 表示平台自行处理，直接返回；否则只保留 2 MiB
 * 地址空间，不创建物理映射，待 PCI 子系统稍后接管。
 *
 * 入参：无。
 * 前置条件：平台 map_io 已完成，启动期串行、static_vmlist 稳定；不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：缺少既有记录时发布固定 2 MiB 空占位；已有记录时无变化。
 * 失败结果：占位分配失败 panic。
 */
static void __init pci_reserve_io(void)
{
	struct static_vm *svm;

	/* 查找阶段返回借用记录；命中即说明平台已拥有该固定窗口。 */
	svm = find_static_vm_vaddr((void *)PCI_IO_VIRT_BASE);
	if (svm)
		return;

	/* 回退阶段只占地址，不猜测 PCI 控制器的实际物理 I/O 基址。 */
	vm_reserve_area_early(PCI_IO_VIRT_BASE, SZ_2M, pci_reserve_io);
}
#else
/* 无通用固定 PCI I/O 窗口的配置不需要预留虚拟地址。 */
#define pci_reserve_io() do { } while (0)
#endif

#ifdef CONFIG_DEBUG_LL
/*
 * debug_ll_io_init - 把早期低级调试 UART 的单页设备地址转成正式静态映射。
 *
 * debug_ll_addr() 以输出参数给出物理地址和链接期虚拟地址，任一为零表示平台
 * 未配置。函数按页对齐并借助 iotable_init() 同时建页表、登记占用；无可用
 * 地址时静默返回，调用者在设备映射重建期间不能依赖该 UART。
 *
 * 入参：无。
 * 前置条件：CONFIG_DEBUG_LL 启用，启动期串行；不持锁、不主动睡眠。
 * 出参：无直接输出参数；局部 map 接收 debug_ll_addr() 的两个输出。
 * 返回：无直接返回值。
 * 副作用：配置有效时建立并登记一页 MT_DEVICE UART 映射。
 * 失败结果：物理或虚拟地址为零时无操作；分配失败由 iotable_init() panic。
 */
void __init debug_ll_io_init(void)
{
	struct map_desc map;

	/*
	 * 探测阶段：回调写 map.pfn/map.virtual；此时 pfn 暂时承载物理字节地址，
	 * 两个零值出口都发生在构造描述符前。
	 */
	debug_ll_addr(&map.pfn, &map.virtual);
	if (!map.pfn || !map.virtual)
		return;
	map.pfn = __phys_to_pfn(map.pfn);
	map.virtual &= PAGE_MASK;
	/* 提交阶段：规范化单位/对齐后，登记一页设备映射并交给 static_vmlist。 */
	map.length = PAGE_SIZE;
	map.type = MT_DEVICE;
	iotable_init(&map, 1);
}
#endif

/*
 * 请求保留的 vmalloc 窗口字节数，默认 240 MiB；early_vmalloc() 可写一次，
 * adjust_lowmem_bounds() 读取后决定 lowmem 上界，随后随 initdata 回收。
 */
static unsigned long __initdata vmalloc_size = 240 * SZ_1M;

/*
 * vmalloc=size forces the vmalloc area to be exactly 'size'
 * bytes. This can be used to increase (or decrease) the vmalloc
 * area - the default is 240MiB.
 */
/*
 * early_vmalloc - 解析 vmalloc= 并钳制 ARM 内核虚拟地址预留规模。
 *
 * arg 由启动参数框架借入，memparse() 返回字节数。下限 16 MiB 保留基本
 * vmalloc 能力；上限还必须在 PAGE_OFFSET 之上留下 32 MiB 安全余量和
 * VMALLOC_OFFSET。函数只更新 init 期 vmalloc_size，实际 lowmem/vmalloc
 * 分界由 adjust_lowmem_bounds() 随后计算；越界值会告警但参数仍视为成功。
 *
 * 入参：
 *   @arg：vmalloc= 值的借用字符串，纯输入、非 NULL；不保存或释放。
 * 前置条件：early_param 启动解析期，串行、无锁，memparse() 不睡眠。
 * 出参：无。
 * 返回：始终为 0，表示参数已处理。
 * 副作用：把 vmalloc_size 更新为钳制后的字节数，越界时打印告警。
 * 失败结果：无错误返回；过小或过大输入分别钳制到安全下限/上限。
 */
static int __init early_vmalloc(char *arg)
{
	unsigned long vmalloc_reserve = memparse(arg, NULL);
	unsigned long vmalloc_max;

	/*
	 * 下限校验：过小窗口无法承载基本 vmalloc/ioremap 用户，先提高到
	 * 16 MiB；告警报告的是实际采用值而非原始输入。
	 */
	if (vmalloc_reserve < SZ_16M) {
		vmalloc_reserve = SZ_16M;
		pr_warn("vmalloc area is too small, limiting to %luMiB\n",
			vmalloc_reserve >> 20);
	}

	vmalloc_max = VMALLOC_END - (PAGE_OFFSET + SZ_32M + VMALLOC_OFFSET);
	/*
	 * 上限校验：为线性映射至少保留 32 MiB 并计入保护间隔，避免 vmalloc
	 * 起点下压越过 PAGE_OFFSET 附近的最小内核布局。
	 */
	if (vmalloc_reserve > vmalloc_max) {
		vmalloc_reserve = vmalloc_max;
		pr_warn("vmalloc area is too big, limiting to %luMiB\n",
			vmalloc_reserve >> 20);
	}

	/* 发布阶段：只有经过双向钳制的值才写入全局，供后续边界计算使用。 */
	vmalloc_size = vmalloc_reserve;
	return 0;
}
early_param("vmalloc", early_vmalloc);

/*
 * 线性映射能够覆盖的物理地址上界（开区间）。adjust_lowmem_bounds() 写入，
 * map_lowmem()/prepare_page_table()/paging_init() 读取；仅启动期有效。
 */
phys_addr_t arm_lowmem_limit __initdata = 0;

/*
 * adjust_lowmem_bounds - 协调 lowmem 线性映射、vmalloc 窗口与 memblock 分配。
 *
 * 调用时 memblock 已描述全部 RAM，但正式页表尚未建立。函数以 64 位算术从
 * 虚拟布局反推可线性映射的最大物理地址，扫描 memory ranges 得到实际
 * arm_lowmem_limit，并把 high_memory 发布为对应虚拟上界。
 *
 * memblock_limit 是更保守的早期分配上界：首个非 PMD 对齐 bank 边界之后，
 * 新页表可能需要从尚未映射的内存分配，故必须向下截断。无 HIGHMEM 或
 * VIPT aliasing cache 无法安全支持 highmem 时，超出部分直接从 memblock
 * 删除。副作用包括 NOMAP 标记、可能删除 RAM、更新 high_memory 及当前
 * memblock limit；这些变更在启动期不可回滚。
 *
 * 入参：无。
 * 前置条件：memblock RAM 拓扑已完成、正式线性映射尚未重建；启动 CPU
 * 串行、无锁，memblock 操作不进入普通 reclaim，本函数不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：可能标记 NOMAP/删除高端 RAM，发布 arm_lowmem_limit/high_memory，
 * 并收紧 memblock 当前分配上限。
 * 失败结果：无错误返回；不支持 highmem 的范围被明确移除而非部分保留。
 */
void __init adjust_lowmem_bounds(void)
{
	/*
	 * 变量地图：
	 *   block_start/end 当前 memblock RAM 区间的物理字节边界。
	 *   vmalloc_limit   虚拟布局允许线性映射到的物理开区间上界。
	 *   lowmem_limit    实际 RAM 与 vmalloc_limit 交集的最高端。
	 *   memblock_limit  页表重建完成前，early allocator 可安全使用的上界。
	 *   i               memblock 迭代游标，不表示 PFN 或字节数。
	 */
	phys_addr_t block_start, block_end, memblock_limit = 0;
	u64 vmalloc_limit, i;
	phys_addr_t lowmem_limit = 0;

	/*
	 * Let's use our own (unoptimized) equivalent of __pa() that is
	 * not affected by wrap-arounds when sizeof(phys_addr_t) == 4.
	 * The result is used as the upper bound on physical memory address
	 * and may itself be outside the valid range for which phys_addr_t
	 * and therefore __pa() is defined.
	 */
	vmalloc_limit = (u64)VMALLOC_END - vmalloc_size - VMALLOC_OFFSET -
			PAGE_OFFSET + PHYS_OFFSET;
	/*
	 * 使用 u64 是因为 32 位 phys_addr_t 下，合法的数学上界也可能正好位于
	 * 4 GiB 之外；若先截为 phys_addr_t，会回绕并错误丢弃低端 RAM。
	 */

	/*
	 * The first usable region must be PMD aligned. Mark its start
	 * as MEMBLOCK_NOMAP if it isn't
	 */
	/*
	 * 阶段 1：只检查第一个可用 RAM range 的起点。首段映射建立前还没有内存
	 * 可供创建补齐 PTE，因此非 PMD 对齐前缀必须从可映射集合排除。
	 */
	for_each_mem_range(i, &block_start, &block_end) {
		if (!IS_ALIGNED(block_start, PMD_SIZE)) {
			phys_addr_t len;

			len = round_up(block_start, PMD_SIZE) - block_start;
			/*
			 * 第一 bank 起始处尚无可用已映射内存来分配 PTE 页，不能用
			 * 页粒度补齐；标 NOMAP 让线性映射从下一个完整 PMD 开始。
			 */
			memblock_mark_nomap(block_start, len);
		}
		break;
	}

	/*
	 * 阶段 2：完整扫描 RAM，累计 lowmem 的最高可达端点，同时记录首次导致
	 * 页粒度补表需求的 bank 边界，作为更保守的 early allocation 上限。
	 */
	for_each_mem_range(i, &block_start, &block_end) {
		if (block_start < vmalloc_limit) {
			/*
			 * 当前 bank 至少有一部分落在线性映射容量内；只扩大上界，
			 * bank 完全位于 vmalloc_limit 之上时不参与 lowmem。
			 */
			if (block_end > lowmem_limit)
				/*
				 * Compare as u64 to ensure vmalloc_limit does
				 * not get truncated. block_end should always
				 * fit in phys_addr_t so there should be no
				 * issue with assignment.
				 */
				lowmem_limit = min_t(u64,
							 vmalloc_limit,
							 block_end);

			/*
			 * Find the first non-pmd-aligned page, and point
			 * memblock_limit at it. This relies on rounding the
			 * limit down to be pmd-aligned, which happens at the
			 * end of this function.
			 *
			 * With this algorithm, the start or end of almost any
			 * bank can be non-pmd-aligned. The only exception is
			 * that the start of the bank 0 must be section-
			 * aligned, since otherwise memory would need to be
			 * allocated when mapping the start of bank 0, which
			 * occurs before any free memory is mapped.
			 */
			if (!memblock_limit) {
				/*
				 * 只记录第一个非 PMD 对齐边界：从这里开始可能需要从尚未
				 * 映射内存分配页表，后续再大的边界不能放宽该安全限制。
				 */
				if (!IS_ALIGNED(block_start, PMD_SIZE))
					memblock_limit = block_start;
				else if (!IS_ALIGNED(block_end, PMD_SIZE))
					memblock_limit = lowmem_limit;
			}

		}
	}

	/*
	 * 阶段 3：发布线性映射的物理/虚拟开区间上界。此后 map_lowmem() 与
	 * 通用 MM 对 lowmem 边界采用同一值。
	 */
	arm_lowmem_limit = lowmem_limit;

	high_memory = __va(arm_lowmem_limit - 1) + 1;
	/* 先转换最后一个有效字节再加一，避免直接转换开区间上界发生回绕。 */

	if (!memblock_limit)
		memblock_limit = arm_lowmem_limit;

	/*
	 * Round the memblock limit down to a pmd size.  This
	 * helps to ensure that we will allocate memory from the
	 * last full pmd, which should be mapped.
	 */
	memblock_limit = round_down(memblock_limit, PMD_SIZE);

	/*
	 * 阶段 4：决定上界之外 RAM 是否还能作为 HIGHMEM 管理。未启用 HIGHMEM
	 * 或 VIPT aliasing cache 无法安全 kmap 时，必须从 memblock 物理拓扑摘除。
	 */
	if (!IS_ENABLED(CONFIG_HIGHMEM) || cache_is_vipt_aliasing()) {
		if (memblock_end_of_DRAM() > arm_lowmem_limit) {
			phys_addr_t end = memblock_end_of_DRAM();

			pr_notice("Ignoring RAM at %pa-%pa\n",
				  &memblock_limit, &end);
			pr_notice("Consider using a HIGHMEM enabled kernel.\n");

			memblock_remove(memblock_limit, end - memblock_limit);
			/*
			 * 这是从可管理物理内存集合摘除，而非清页表或释放页面；后续
			 * buddy allocator 将完全看不到该范围。
			 */
		}
	}

	/*
	 * 最终提交：把 PMD 对齐的保守上界交给 memblock allocator；paging_init()
	 * 完成全部 lowmem 映射后才会把限制放宽到 arm_lowmem_limit。
	 */
	memblock_set_current_limit(memblock_limit);
}

/*
 * prepare_page_table - 清除启动汇编遗留且不应进入正式地址空间的 PMD 映射。
 *
 * 入口时 swapper_pg_dir 正在被 MMU 使用，第一段 lowmem 与 early fixmap 必须
 * 保留以维持当前执行和临时 I/O；其余用户区、模块区空洞、非首 bank 线性区
 * 到 vmalloc 起点的描述符被清零，随后由 map_* 重建。KASAN shadow 和 XIP
 * 镜像是不能清除的配置例外。
 *
 * pmd_clear() 这里只改变页表内存，没有逐项 TLB flush；启动代码尚未依赖
 * 被撤销区域，并在后续发布边界统一同步。函数不释放被取消映射的物理 RAM，
 * 也不改变 memblock 所有权。
 *
 * 入参：无。
 * 前置条件：启动 CPU 正运行于需保留的首 bank/内核映射，init_mm 无并发
 * 修改者；无锁、不分配、不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：清除多个 PMD 范围的软件页表项；不做 TLB/cache flush、不释放页。
 * 失败结果：无错误返回；KASAN/XIP 配置通过保留区间避免清除正在使用的映射。
 */
static __init void prepare_page_table(void)
{
	unsigned long addr;
	phys_addr_t end;

	/*
	 * Clear out all the mappings below the kernel image.
	 */
	/*
	 * 阶段 1：清用户/模块低地址区域。每次按 PMD 粒度撤销启动描述符；
	 * KASAN 配置必须把预建 shadow 当作正在使用的保留洞跳过。
	 */
#ifdef CONFIG_KASAN
	/*
	 * KASan's shadow memory inserts itself between the TASK_SIZE
	 * and MODULES_VADDR. Do not clear the KASan shadow memory mappings.
	 */
	for (addr = 0; addr < KASAN_SHADOW_START; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));
	/*
	 * Skip over the KASan shadow area. KASAN_SHADOW_END is sometimes
	 * equal to MODULES_VADDR and then we exit the pmd clearing. If we
	 * are using a thumb-compiled kernel, there there will be 8MB more
	 * to clear as KASan always offset to 16 MB below MODULES_VADDR.
	 */
	for (addr = KASAN_SHADOW_END; addr < MODULES_VADDR; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));
#else
	for (addr = 0; addr < MODULES_VADDR; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));
#endif

#ifdef CONFIG_XIP_KERNEL
	/* The XIP kernel is mapped in the module area -- skip over it */
	/*
	 * XIP 镜像位于 module area，addr 必须从镜像结束后的下一个 PMD 继续；
	 * 否则下面的通用清理会撤销当前正在执行的 ROM 映射。
	 */
	addr = ((unsigned long)_exiprom + PMD_SIZE - 1) & PMD_MASK;
#endif
	/* 阶段 2：撤销其余 module gap 到 PAGE_OFFSET 的启动临时映射。 */
	for ( ; addr < PAGE_OFFSET; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Find the end of the first block of lowmem.
	 */
	end = memblock.memory.regions[0].base + memblock.memory.regions[0].size;
	if (end >= arm_lowmem_limit)
		end = arm_lowmem_limit;
	/*
	 * 第一 bank 是当前执行和 early allocation 的立足点，保留到其实际 lowmem
	 * 端点；后续 bank 的旧线性映射不能假设仍与 memblock 拓扑一致。
	 */

	/*
	 * Clear out all the kernel space mappings, except for the first
	 * memory bank, up to the vmalloc region.
	 */
	for (addr = __phys_to_virt(end);
	     addr < VMALLOC_START; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));
	/*
	 * 完成状态：首 bank、KASAN/XIP 例外和 early fixmap 仍可用，其余正式
	 * 映射区域已清空，等待 map_lowmem()/map_kernel() 重建。
	 */
}

#ifdef CONFIG_ARM_LPAE
/* the first page is reserved for pgd */
/*
 * swapper_pg_dir 占用量因格式不同：LPAE 的顶层 PGD 单独占第一页，后面还有
 * 启动时预置的 PMD 表；短描述符仅需 PGD 数组。该范围必须整体留给页表。
 */
#define SWAPPER_PG_DIR_SIZE	(PAGE_SIZE + \
				 PTRS_PER_PGD * PTRS_PER_PMD * sizeof(pmd_t))
#else
#define SWAPPER_PG_DIR_SIZE	(PTRS_PER_PGD * sizeof(pgd_t))
#endif

/*
 * Reserve the special regions of memory
 */
/*
 * arm_mm_memblock_reserve - 在通用内存分配前保护正在使用的启动页表等区域。
 *
 * swapper_pg_dir 已被 MMU 硬件遍历，必须先 memblock_reserve()，否则后续
 * early_alloc 可能覆盖它。SA1111 配置还因 DMA 硬件缺陷保留低端可 DMA RAM。
 * reserve 只改变 memblock 分配状态，不建立映射；失败属于早期启动致命条件。
 *
 * 入参：无。
 * 前置条件：swapper_pg_dir 物理位置已确定，memblock 可修改；启动期串行、
 * 无锁、不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：把页表及可选 SA1111 低端范围标为 memblock reserved。
 * 失败结果：无错误返回；保留失败属于无法安全继续的启动错误。
 */
void __init arm_mm_memblock_reserve(void)
{
	/*
	 * Reserve the page tables.  These are already in use,
	 * and can only be in node 0.
	 */
	/*
	 * 阶段 1：先保留硬件正在遍历的 swapper 页表。该调用只更新 memblock
	 * reserved 集合，不改变当前页表或 TLB。
	 */
	memblock_reserve(__pa(swapper_pg_dir), SWAPPER_PG_DIR_SIZE);

#ifdef CONFIG_SA1111
	/*
	 * Because of the SA1111 DMA bug, we want to preserve our
	 * precious DMA-able memory...
	 */
	/*
	 * SA1111 回退：从 PHYS_OFFSET 到页表之间的低端区间不交给普通分配器，
	 * 为受地址限制且有硬件缺陷的 DMA 保留；其他配置不承担这项空间代价。
	 */
	memblock_reserve(PHYS_OFFSET, __pa(swapper_pg_dir) - PHYS_OFFSET);
#endif
}

/*
 * Set up the device mappings.  Since we clear out the page tables for all
 * mappings above VMALLOC_START, except early fixmap, we might remove debug
 * device mappings.  This means earlycon can be used to debug this function
 * Any other function or debugging method which may touch any device _will_
 * crash the kernel.
 */
/*
 * devicemaps_init - 重建异常向量、静态设备和启动调试所需的内核高地址映射。
 *
 * mdesc 借用自机器描述符且贯穿启动期有效。函数先分配两页向量存储并让
 * early_trap_init() 填充内容，然后清除 VMALLOC_START 以上除 early fixmap
 * PMD 外的旧映射；在新设备映射发布前，任何访问被清除设备的调试方式都可能
 * fault，只有未受影响的 earlycon 可用于观察此阶段。
 *
 * 随后按配置映射 FDT、cache-clean 区、高/低向量和机器 I/O，登记 PMD 半区
 * 空洞及 PCI 窗口。最后先使本地 TLB 旧翻译失效，再清理 cache/write buffer，
 * 确保向量页写回且设备属性对硬件可见，之后才启用异步 abort。所有分配来自
 * memblock，映射和 static_vm 由 init_mm/全局链表长期持有，无局部回滚。
 *
 * 入参：
 *   @mdesc：借用的只读机器描述符，非 NULL；map_io 回调可为空，不保存引用。
 * 前置条件：lowmem/内核映射已建立，early fixmap 仍可用；启动 CPU 串行、
 * 无锁，memblock 分配不走普通 reclaim，本函数不主动睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：分配向量页，重写高地址页表，登记静态 VM，刷新本地 TLB/cache，
 * 最后启用异步 abort。
 * 失败结果：早期分配失败 panic；无平台 map_io 时只恢复 debug UART 映射。
 */
static void __init devicemaps_init(const struct machine_desc *mdesc)
{
	/*
	 * 变量地图：
	 *   map     每个静态映射的栈上请求，create_mapping() 同步消费。
	 *   addr    清理高地址 PMD 的虚拟字节游标。
	 *   vectors memblock 分配的两页向量存储，ownership 交给长期向量映射。
	 */
	struct map_desc map;
	unsigned long addr;
	void *vectors;

	/*
	 * Allocate the vector page early.
	 */
	vectors = early_alloc(PAGE_SIZE * 2);

	/*
	 * 第一页承载用户可见/异常向量，第二页承载仅内核可读的辅助代码或数据；
	 * 填充发生在映射权限收紧前，因此可直接写入。
	 */
	early_trap_init(vectors);

	/*
	 * Clear page table except top pmd used by early fixmaps
	 */
	/*
	 * 阶段 2：撤销旧 vmalloc/device 映射，但停在 fixmap 顶部 PMD 之前，
	 * 保证迁移期间 early fixmap 仍可访问。此窗口内普通设备地址不可触碰。
	 */
	for (addr = VMALLOC_START; addr < (FIXADDR_TOP & PMD_MASK); addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	if (__atags_pointer) {
		/* create a read-only mapping of the device tree */
		/*
		 * 指针可能位于 section 内任意偏移；固定窗口映射整个 FDT 区域并
		 * 使用只读普通内存属性，使解析代码可在旧启动映射清除后继续读取。
		 */
		map.pfn = __phys_to_pfn(__atags_pointer & SECTION_MASK);
		map.virtual = FDT_FIXED_BASE;
		map.length = FDT_FIXED_SIZE;
		map.type = MT_MEMORY_RO;
		create_mapping(&map);
	}

	/*
	 * Map the cache flushing regions.
	 */
	/*
	 * 阶段 4：按平台常量恢复 cache 维护别名。它们使用专用内存类型，不能
	 * 退化为普通 cached RAM，否则维护访问本身会再次被 cache 吸收。
	 */
#ifdef FLUSH_BASE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS);
	map.virtual = FLUSH_BASE;
	map.length = SZ_1M;
	map.type = MT_CACHECLEAN;
	create_mapping(&map);
#endif
#ifdef FLUSH_BASE_MINICACHE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS + SZ_1M);
	map.virtual = FLUSH_BASE_MINICACHE;
	map.length = SZ_1M;
	map.type = MT_MINICLEAN;
	create_mapping(&map);
#endif

	/*
	 * Create a mapping for the machine vectors at the high-vectors
	 * location (0xffff0000).  If we aren't using high-vectors, also
	 * create a mapping at the low-vectors virtual address.
	 */
	/*
	 * 阶段 5：先把向量第一页固定映射到高向量地址；KUSER_HELPERS 决定用户
	 * 是否可读。若 CPU 选低向量，再为同一物理存储建立低地址别名。
	 */
	map.pfn = __phys_to_pfn(virt_to_phys(vectors));
	map.virtual = 0xffff0000;
	map.length = PAGE_SIZE;
#ifdef CONFIG_KUSER_HELPERS
	map.type = MT_HIGH_VECTORS;
#else
	map.type = MT_LOW_VECTORS;
#endif
	create_mapping(&map);

	if (!vectors_high()) {
		/*
		 * 低向量需要两页连续覆盖，第二页仍按 LOW_VECTORS 权限；稍后高地址
		 * 第二页会单独收紧为 kernel read-only。
		 */
		map.virtual = 0;
		map.length = PAGE_SIZE * 2;
		map.type = MT_LOW_VECTORS;
		create_mapping(&map);
	}

	/* Now create a kernel read-only mapping */
	/*
	 * 向量辅助页不应暴露给用户：推进到第二个 PFN，在高向量后一页建立仅
	 * 内核可读映射，完成两页向量对象的权限分离。
	 */
	map.pfn += 1;
	map.virtual = 0xffff0000 + PAGE_SIZE;
	map.length = PAGE_SIZE;
	map.type = MT_LOW_VECTORS;
	create_mapping(&map);

	/*
	 * Ask the machine support to map in the statically mapped devices.
	 */
	if (mdesc->map_io)
		mdesc->map_io();
	else
		debug_ll_io_init();
	/*
	 * map_io() 通常通过 iotable_init() 发布平台 static_vm。必须等全部静态
	 * 映射登记完再填半 PMD 空洞，扫描结果才完整。
	 */
	fill_pmd_gaps();

	/* Reserve fixed i/o space in VMALLOC region */
	pci_reserve_io();

	/*
	 * Finally flush the caches and tlb to ensure that we're in a
	 * consistent state wrt the writebuffer.  This also ensures that
	 * any write-allocated cache lines in the vector page are written
	 * back.  After this point, we can start to touch devices again.
	 */
	/*
	 * 硬件提交阶段：先丢弃当前 CPU 的旧地址翻译，再把向量页和页表相关
	 * 脏 cache/write-buffer 内容写回；完成后新设备属性才可被可靠使用。
	 */
	local_flush_tlb_all();
	flush_cache_all();

	/* Enable asynchronous aborts */
	/* 最后开放异步 abort；此前设备映射不完整，过早开放会进入不可用向量。 */
	early_abt_enable();
}

/*
 * kmap_init - 预建 permanent kmap 与 fixmap 区域所需的 PTE 表。
 *
 * highmem 启用时发布 pkmap_page_table，供永久高端页映射代码复用；fixmap
 * PTE 表无条件建立。early_pte_alloc() 可能从 memblock 分配，返回表由
 * init_mm 持有。此处只保证页表层级存在，不映射具体物理页。
 *
 * 入参：无。
 * 前置条件：init_mm 与 memblock 可用、启动期串行、无页表并发者；不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：可能发布 pkmap_page_table，并保证 PKMAP/FIXADDR 的 PTE 表存在。
 * 失败结果：页表分配失败 panic/BUG，不返回部分状态。
 */
static void __init kmap_init(void)
{
#ifdef CONFIG_HIGHMEM
	/*
	 * HIGHMEM 阶段：确保 PKMAP_BASE 有叶子表并发布其借用指针；这里只创建
	 * 容器，具体高端页由 kmap 路径稍后填入。
	 */
	pkmap_page_table = early_pte_alloc(pmd_off_k(PKMAP_BASE),
		PKMAP_BASE, _PAGE_KERNEL_TABLE);
#endif

	/* fixmap 阶段同样只预建叶子表，具体固定槽由 __set_fixmap() 管理。 */
	early_pte_alloc(pmd_off_k(FIXADDR_START), FIXADDR_START,
			_PAGE_KERNEL_TABLE);
}

/*
 * map_lowmem - 为所有可线性寻址 RAM 建立 PAGE_OFFSET 线性映射。
 *
 * 遍历 memblock memory ranges，并把每段截断到 arm_lowmem_limit。通常使用
 * MT_MEMORY_RW；若内核镜像被单独映射在 vmalloc/module 区域，则从 lowmem
 * 范围中凿除 [kernel_sec_start, kernel_sec_end)，避免同一物理内核页同时
 * 出现属性或权限不同的别名。map_desc 是栈上请求，create_mapping() 同步
 * 消费，不保存其地址。
 *
 * 六种相交关系保证被凿除区间前后仍连续映射；此处的 break 表示当前已截断
 * bank 之后也不会再有 lowmem，而不是错误回滚。成功后 RAM 物理页仍归
 * memblock，新增的只是 init_mm 可见的虚拟翻译。
 *
 * 入参：无。
 * 前置条件：arm_lowmem_limit 已发布，memblock RAM ranges 稳定，启动期
 * 串行、无页表锁；early_alloc 不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：为低端 RAM（排除独立内核镜像物理区）向 init_mm 发布线性映射。
 * 失败结果：映射分配失败 panic；超过 lowmem 上界的 ranges 正常跳过。
 */
static void __init map_lowmem(void)
{
	/*
	 * 变量地图：
	 *   start/end 当前 memblock RAM bank 的物理字节半开区间，可能被截断或
	 *             因内核镜像相交而向内收缩。
	 *   i         memblock 迭代游标。
	 *   map       当前待提交的线性映射描述符，栈上短期有效。
	 */
	phys_addr_t start, end;
	u64 i;

	/* Map all the lowmem memory banks. */
	/*
	 * 扫描阶段：memblock ranges 按物理地址递增。每个 bank 先截到
	 * arm_lowmem_limit；一旦 start>=end，后续更高 bank 也不属于 lowmem。
	 */
	for_each_mem_range(i, &start, &end) {
		struct map_desc map;

		pr_debug("map lowmem start: 0x%08llx, end: 0x%08llx\n",
			 (long long)start, (long long)end);
		if (end > arm_lowmem_limit)
			end = arm_lowmem_limit;
		if (start >= end)
			break;

		/*
		 * If our kernel image is in the VMALLOC area we need to remove
		 * the kernel physical memory from lowmem since the kernel will
		 * be mapped separately.
		 *
		 * The kernel will typically be at the very start of lowmem,
		 * but any placement relative to memory ranges is possible.
		 *
		 * If the memblock contains the kernel, we have to chisel out
		 * the kernel memory from it and map each part separately. We
		 * get 6 different theoretical cases:
		 *
		 *                            +--------+ +--------+
		 *  +-- start --+  +--------+ | Kernel | | Kernel |
		 *  |           |  | Kernel | | case 2 | | case 5 |
		 *  |           |  | case 1 | +--------+ |        | +--------+
		 *  |  Memory   |  +--------+            |        | | Kernel |
		 *  |  range    |  +--------+            |        | | case 6 |
		 *  |           |  | Kernel | +--------+ |        | +--------+
		 *  |           |  | case 3 | | Kernel | |        |
		 *  +-- end ----+  +--------+ | case 4 | |        |
		 *                            +--------+ +--------+
		 */
		/*
		 * 分支目标：只映射“当前 bank 减去独立 kernel section”的剩余集合。
		 * 六种情况不是错误路径，而是两个半开区间的完整相交分类。
		 */

		/* Case 5: kernel covers range, don't map anything, should be rare */
		/* case 5 无剩余区间，不能发布长度为零或负值的 map_desc。 */
		if ((start > kernel_sec_start) && (end < kernel_sec_end))
			break;

		/* Cases where the kernel is starting inside the range */
		if ((kernel_sec_start >= start) && (kernel_sec_start <= end)) {
			/* Case 6: kernel is embedded in the range, we need two mappings */
			if ((start < kernel_sec_start) && (end > kernel_sec_end)) {
				/* Map memory below the kernel */
				/*
				 * case 6 下半段：先提交 [start,kernel_sec_start)，调用返回
				 * 后这段线性映射已可由 init_mm 遍历。
				 */
				map.pfn = __phys_to_pfn(start);
				map.virtual = __phys_to_virt(start);
				map.length = kernel_sec_start - start;
				map.type = MT_MEMORY_RW;
				create_mapping(&map);
				/* Map memory above the kernel */
				/*
				 * 再提交 [kernel_sec_end,end)。两段之间恰好留下内核镜像
				 * 空洞，完成后本 bank 已处理完，可退出整个 lowmem 扫描。
				 */
				map.pfn = __phys_to_pfn(kernel_sec_end);
				map.virtual = __phys_to_virt(kernel_sec_end);
				map.length = end - kernel_sec_end;
				map.type = MT_MEMORY_RW;
				create_mapping(&map);
				break;
			}
			/* Case 1: kernel and range start at the same address, should be common */
			/* case 1 删除 bank 前缀，剩余区间从 kernel_sec_end 开始。 */
			if (kernel_sec_start == start)
				start = kernel_sec_end;
			/* Case 3: kernel and range end at the same address, should be rare */
			/* case 3 删除 bank 后缀，剩余区间在 kernel_sec_start 结束。 */
			if (kernel_sec_end == end)
				end = kernel_sec_start;
		} else if ((kernel_sec_start < start) && (kernel_sec_end > start) && (kernel_sec_end < end)) {
			/* Case 2: kernel ends inside range, starts below it */
			/* 只保留内核结束后的 bank 后半段。 */
			start = kernel_sec_end;
		} else if ((kernel_sec_start > start) && (kernel_sec_start < end) && (kernel_sec_end > end)) {
			/* Case 4: kernel starts inside range, ends above it */
			/* 只保留内核开始前的 bank 前半段。 */
			end = kernel_sec_start;
		}
		/*
		 * 汇合提交：无相交或 case 1～4 都归一化为一个非空剩余区间，统一
		 * 构造 RW 线性映射；物理页 ownership 始终留在 memblock。
		 */
		map.pfn = __phys_to_pfn(start);
		map.virtual = __phys_to_virt(start);
		map.length = end - start;
		map.type = MT_MEMORY_RW;
		create_mapping(&map);
	}
}

/*
 * map_kernel - 以启动阶段可表达的最小权限映射内核镜像。
 *
 * kernel_sec_start/end 是按 section 对齐的物理边界。普通内核把开头到
 * __init_end 所在 section 映射为 RWX，其余映射 RW+XN；XIP 内核的代码在
 * ROM/module 区映射为 MT_ROM，RAM 部分仅 RW。由于此阶段采用 section
 * 粒度，边界 section 可能暂时赋予多余执行权限，后续细粒度内存权限初始化
 * 会重映射修正，这是减少早期页表依赖所付出的短暂安全粒度代价。
 *
 * 入参：无。
 * 前置条件：kernel_sec_start/end 已按 section 固化，init_mm 启动期独占；
 * memblock 可分配页表，不睡眠。
 * 出参：无。
 * 返回：无直接返回值；普通内核没有独立 NX 尾段时提前返回仍属成功。
 * 副作用：发布 XIP ROM 或普通 RWX 代码映射，以及需要时的 RW+XN RAM 映射。
 * 失败结果：页表分配失败 panic，描述符非法由 create_mapping() 告警/拒绝。
 */
static void __init map_kernel(void)
{
	/*
	 * We use the well known kernel section start and end and split the area in the
	 * middle like this:
	 *  .                .
	 *  | RW memory      |
	 *  +----------------+ kernel_x_start
	 *  | Executable     |
	 *  | kernel memory  |
	 *  +----------------+ kernel_x_end / kernel_nx_start
	 *  | Non-executable |
	 *  | kernel memory  |
	 *  +----------------+ kernel_nx_end
	 *  | RW memory      |
	 *  .                .
	 *
	 * Notice that we are dealing with section sized mappings here so all of this
	 * will be bumped to the closest section boundary. This means that some of the
	 * non-executable part of the kernel memory is actually mapped as executable.
	 * This will only persist until we turn on proper memory management later on
	 * and we remap the whole kernel with page granularity.
	 */
	/*
	 * 边界准备：所有局部边界均为物理字节地址。普通内核把 __init_end 向上
	 * 对齐为可执行段末端；XIP 的执行代码在 ROM，RAM 从 kernel_sec_start
	 * 开始全部按 NX 候选处理。
	 */
#ifdef CONFIG_XIP_KERNEL
	phys_addr_t kernel_nx_start = kernel_sec_start;
#else
	phys_addr_t kernel_x_start = kernel_sec_start;
	phys_addr_t kernel_x_end = round_up(__pa(__init_end), SECTION_SIZE);
	phys_addr_t kernel_nx_start = kernel_x_end;
#endif
	phys_addr_t kernel_nx_end = kernel_sec_end;
	struct map_desc map;

	/*
	 * Map the kernel if it is XIP.
	 * It is always first in the modulearea.
	 */
#ifdef CONFIG_XIP_KERNEL
	/*
	 * XIP 分支：ROM 物理起点按 section 向下对齐，虚拟起点固定在 module
	 * area；长度覆盖到 _exiprom，并按 section 粒度向上扩展后发布为 MT_ROM。
	 */
	map.pfn = __phys_to_pfn(CONFIG_XIP_PHYS_ADDR & SECTION_MASK);
	map.virtual = MODULES_VADDR;
	map.length = ((unsigned long)_exiprom - map.virtual + ~SECTION_MASK) & SECTION_MASK;
	map.type = MT_ROM;
	create_mapping(&map);
#else
	/*
	 * 普通分支阶段 1：先发布包含启动代码的 RWX section 区。早期写权限供
	 * 重定位/初始化使用，细粒度只读与 NX 权限由后续内存权限初始化收紧。
	 */
	map.pfn = __phys_to_pfn(kernel_x_start);
	map.virtual = __phys_to_virt(kernel_x_start);
	map.length = kernel_x_end - kernel_x_start;
	map.type = MT_MEMORY_RWX;
	create_mapping(&map);

	/* If the nx part is small it may end up covered by the tail of the RWX section */
	/*
	 * 若 section 对齐使可执行区已经覆盖到 kernel_sec_end，就没有独立 NX
	 * 尾段；提前返回避免提交零长度 map_desc。
	 */
	if (kernel_x_end == kernel_nx_end)
		return;
#endif
	/*
	 * 最终阶段：XIP 的全部 RAM 或普通内核剩余尾段以 RW+XN 发布。返回后
	 * 内核物理区不再依赖 map_lowmem() 的普通线性别名。
	 */
	map.pfn = __phys_to_pfn(kernel_nx_start);
	map.virtual = __phys_to_virt(kernel_nx_start);
	map.length = kernel_nx_end - kernel_nx_start;
	map.type = MT_MEMORY_RW;
	create_mapping(&map);
}

#ifdef CONFIG_ARM_PV_FIXUP
/*
 * pgtables_remap 描述 identity-map 汇编入口的调用 ABI：@offset 是物理地址
 * 差值（字节），@pgd 是旧语义下的页目录物理地址。lpae_pgtables_remap_asm
 * 是链接进来的代码符号，C 侧只借用其地址，不拥有或修改该对象。
 */
typedef void pgtables_remap(long long offset, unsigned long pgd);
pgtables_remap lpae_pgtables_remap_asm;

/*
 * early_paging_init() recreates boot time page table setup, allowing machines
 * to switch over to a high (>4G) address space on LPAE systems
 */
/*
 * early_paging_init - 在 LPAE PV 平台切换到修正后的高物理地址空间。
 *
 * mdesc->pv_fixup() 返回物理地址增量，零表示无需处理。函数先修正内核物理
 * section 边界，并在改变全局 P:V 常量前取得 identity map 中汇编重映射
 * 例程和页表物理地址；barrier() 是编译器屏障，防止这些地址计算被移到
 * __pv_offset 修改之后，它本身不是 CPU cache/TLB 屏障。
 *
 * 切换期间关闭 I/D cache 和页表遍历 cache 属性，flush_cache_all() 把旧
 * 物理地址语义下的脏数据写回；汇编例程位于 identity map，可在关闭 MMU
 * 时重写页表物理地址。完成后恢复 TTBCR/CR。该过程跨越不可回滚点，任一
 * 错误都会使当前执行地址失效，因此没有错误返回或恢复路径，且只能启动 CPU
 * 串行执行。
 *
 * 入参：
 *   @mdesc：借用只读机器描述符，非 NULL；pv_fixup 回调允许为空。
 * 前置条件：ARM LPAE+PV fixup 启动 CPU、identity map 和临时页表仍有效；
 * 无并发 CPU 使用待修改常量，不持锁、不睡眠。
 * 出参：无。
 * 返回：无直接返回值；无回调或 offset=0 为成功无操作。
 * 副作用：可能修改 kernel_sec_*、P:V 全局偏移和 patch table，临时关闭
 * cache/MMU 相关能力、刷新 cache、由汇编重映射页表后恢复控制寄存器。
 * 失败结果：进入非零 offset 切换后无可恢复出口，硬件错误会导致启动失败。
 */
static void __init early_paging_init(const struct machine_desc *mdesc)
{
	/*
	 * 变量地图：
	 *   offset   平台报告的新旧物理地址空间差值，单位字节。
	 *   pa_pgd   改写 P:V 常量前计算的 swapper_pg_dir 旧物理地址。
	 *   cr/ttbcr 原控制寄存器快照，切换完成后必须原样恢复。
	 *   tmp      临时关闭 table-walk cache 属性的 TTBCR。
	 */
	pgtables_remap *lpae_pgtables_remap;
	unsigned long pa_pgd;
	u32 cr, ttbcr, tmp;
	long long offset;

	/* 探测阶段：无平台回调或回调返回零都表示当前物理空间无需切换。 */
	if (!mdesc->pv_fixup)
		return;

	offset = mdesc->pv_fixup();
	if (offset == 0)
		return;

	/*
	 * Offset the kernel section physical offsets so that the kernel
	 * mapping will work out later on.
	 */
	kernel_sec_start += offset;
	kernel_sec_end += offset;
	/*
	 * 从这里开始软件记录已采用新物理边界，但页表/转换常量仍是旧值；
	 * 后续必须完成全部切换，不能以普通错误返回中止。
	 */

	/*
	 * Get the address of the remap function in the 1:1 identity
	 * mapping setup by the early page table assembly code.  We
	 * must get this prior to the pv update.  The following barrier
	 * ensures that this is complete before we fixup any P:V offsets.
	 */
	lpae_pgtables_remap = (pgtables_remap *)(unsigned long)__pa(lpae_pgtables_remap_asm);
	pa_pgd = __pa(swapper_pg_dir);
	barrier();
	/*
	 * barrier() 之后两个旧地址快照固定；接下来修改 __pv_offset 后再调用
	 * __pa() 会得到新语义，不能用于定位 identity-map 汇编入口。
	 */

	pr_info("Switching physical address space to 0x%08llx\n",
		(u64)PHYS_OFFSET + offset);

	/* Re-set the phys pfn offset, and the pv offset */
	__pv_offset += offset;
	__pv_phys_pfn_offset += PFN_DOWN(offset);

	/* Run the patch stub to update the constants */
	/*
	 * 提交软件常量：fixup_pv_table() 修补所有登记的 P:V 立即数。完成后 C
	 * 代码的地址换算已指向新空间，但硬件页表尚待汇编例程重写。
	 */
	fixup_pv_table(&__pv_table_begin,
		(&__pv_table_end - &__pv_table_begin) << 2);

	/*
	 * We changing not only the virtual to physical mapping, but also
	 * the physical addresses used to access memory.  We need to flush
	 * all levels of cache in the system with caching disabled to
	 * ensure that all data is written back, and nothing is prefetched
	 * into the caches.  We also need to prevent the TLB walkers
	 * allocating into the caches too.  Note that this is ARMv7 LPAE
	 * specific.
	 */
	cr = get_cr();
	/*
	 * 硬件切换准备：保存控制寄存器后关闭 I/D cache，并禁止 table walk
	 * 分配 cache line；随后 flush 把旧地址空间下的脏数据全部写回。
	 */
	set_cr(cr & ~(CR_I | CR_C));
	ttbcr = cpu_get_ttbcr();
	/* Disable all kind of caching of the translation table */
	tmp = ttbcr & ~(TTBCR_ORGN0_MASK | TTBCR_IRGN0_MASK);
	cpu_set_ttbcr(tmp);
	flush_cache_all();

	/*
	 * Fixup the page tables - this must be in the idmap region as
	 * we need to disable the MMU to do this safely, and hence it
	 * needs to be assembly.  It's fairly simple, as we're using the
	 * temporary tables setup by the initial assembly code.
	 */
	lpae_pgtables_remap(offset, pa_pgd);
	/*
	 * 这是不可回滚提交点：汇编在 identity map 中关闭 MMU并修正页表物理
	 * 地址，返回时 CPU 已能用新物理空间解释内核虚拟地址。
	 */

	/* Re-enable the caches and cacheable TLB walks */
	/* 恢复阶段按相反顺序恢复 table-walk 属性和 CR，重新开放正常 cache。 */
	cpu_set_ttbcr(ttbcr);
	set_cr(cr);
}

#else

/*
 * 非 LPAE 版本只检测错误配置：pv_fixup 请求非零偏移时，短描述符无法表达
 * Keystone2 所需高物理地址，故记录严重错误并 taint 内核，而非伪装成成功
 * 切换。返回后继续启动仅用于尽可能给出诊断，机器可能随后崩溃。
 *
 * 入参：
 *   @mdesc：借用只读机器描述符，非 NULL；pv_fixup 可为空。
 * 前置条件：非 LPAE 的启动 CPU 串行上下文，无锁、不睡眠。
 * 出参：无。
 * 返回：无直接返回值；无回调或 offset=0 时无操作。
 * 副作用：不支持的非零 offset 会打印严重日志并设置 CPU_OUT_OF_SPEC taint。
 * 失败结果：无法执行地址空间切换，只保留诊断后继续，机器可能随后崩溃。
 */
static void __init early_paging_init(const struct machine_desc *mdesc)
{
	long long offset;

	/* 探测阶段与 LPAE 版本相同：无回调或零偏移都安全返回且无副作用。 */
	if (!mdesc->pv_fixup)
		return;

	offset = mdesc->pv_fixup();
	if (offset == 0)
		return;

	/*
	 * 失败诊断阶段：非零偏移无法由本配置实现，明确记录两条配置要求并
	 * taint；没有页表可回滚，因为本函数从未尝试修改地址空间。
	 */
	pr_crit("Physical address space modification is only to support Keystone2.\n");
	pr_crit("Please enable ARM_LPAE and ARM_PATCH_PHYS_VIRT support to use this\n");
	pr_crit("feature. Your kernel may crash now, have a good day.\n");
	add_taint(TAINT_CPU_OUT_OF_SPEC, LOCKDEP_STILL_OK);
}

#endif

/*
 * early_fixmap_shutdown - 退役 bm_pte，并把永久设备 fixmap 迁入正式页表。
 *
 * 先把查找函数切到 pte_offset_kernel()，再清除连接 bm_pte 的 PMD 并对其
 * 虚拟页做本地 TLB 失效；从此旧数组只作为迁移快照读取。循环检查永久
 * fixmap 槽，只迁移当前支持的 shared-device PTE，转换为 map_desc 后通过
 * create_mapping() 重建。空槽或其他属性跳过。
 *
 * 顺序至关重要：若先清 PMD 后仍让 __set_fixmap() 使用旧策略，会继续修改
 * 已不被硬件遍历的 bm_pte；若不失效 TLB，CPU 可能继续访问已退役表产生的
 * 翻译。迁移完成后 bm_pte 随 initdata 回收。
 *
 * 入参：无。
 * 前置条件：正式页表分配可用，early fixmap 无并发修改者；启动 CPU 串行、
 * 无锁、不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：切换 fixmap 查找函数，清旧 PMD、失效本地 TLB，并迁移设备 PTE。
 * 失败结果：不支持的早期 PTE 属性被跳过；新页表分配失败 panic。
 */
static void __init early_fixmap_shutdown(void)
{
	int i;
	unsigned long va = fix_to_virt(__end_of_permanent_fixed_addresses - 1);

	/*
	 * 退役阶段：先让未来查找走正式页表，再断开 bm_pte 所在 PMD 并使旧
	 * 本地翻译失效。该顺序保证不会再向已脱离硬件层级的临时表写新映射。
	 */
	pte_offset_fixmap = pte_offset_late_fixmap;
	pmd_clear(fixmap_pmd(va));
	local_flush_tlb_kernel_page(va);

	/*
	 * 迁移阶段：bm_pte 虽已断开但存储仍在 initdata 中，可作为只读快照
	 * 扫描。每个永久槽独立转换成正式 map_desc。
	 */
	for (i = 0; i < __end_of_permanent_fixed_addresses; i++) {
		pte_t *pte;
		struct map_desc map;

		map.virtual = fix_to_virt(i);
		pte = pte_offset_early_fixmap(pmd_off_k(map.virtual), map.virtual);

		/* Only i/o device mappings are supported ATM */
		if (pte_none(*pte) ||
		    (pte_val(*pte) & L_PTE_MT_MASK) != L_PTE_MT_DEV_SHARED)
			continue;

		/*
		 * 仅 shared-device 属性具有当前迁移协议。提取 PFN 后以 MT_DEVICE
		 * 重建一页 global 映射；旧 bm_pte 不清除，稍后随 initdata 回收。
		 */
		map.pfn = pte_pfn(*pte);
		map.type = MT_DEVICE;
		map.length = PAGE_SIZE;

		create_mapping(&map);
	}
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps, and sets up the zero page, bad page and bad page tables.
 */
/*
 * paging_init - 建立 ARM32 正式内核地址空间并启动物理内存管理。
 *
 * mdesc 为只读机器描述符。入口条件是 early_mm_init() 已生成内存类型，
 * adjust_lowmem_bounds() 已确定 lowmem，启动页表和 memblock 仍可使用。
 * 函数按依赖顺序清旧映射、建立 lowmem、放宽 memblock 分配上限、映射内核
 * 与 DMA、迁移 fixmap、重建设备、预建 kmap/fixmap 表并初始化 TCM。
 *
 * top_pmd 只在上述映射稳定后发布，bootmem_init() 随后建立 zones、页元数据
 * 等通用内存管理状态。此函数没有可恢复失败返回；任一早期分配失败会 panic。
 *
 * 入参：
 *   @mdesc：借用只读机器描述符，非 NULL，调用期间与启动期保持有效。
 * 前置条件：early_mm_init()/adjust_lowmem_bounds() 已完成；启动 CPU 串行，
 * 无并发页表用户。子阶段可能执行内存初始化，但不返回可恢复错误。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：替换启动映射、扩大 memblock 分配范围、发布 top_pmd，并启动
 * DMA、设备、kmap、TCM 与 bootmem/zone 初始化。
 * 失败结果：任一不可恢复的页表/内存初始化错误直接 panic/BUG。
 */
void __init paging_init(const struct machine_desc *mdesc)
{
#ifdef CONFIG_XIP_KERNEL
	/* Store the kernel RW RAM region start/end in these variables */
	/* XIP 准备：执行代码在 ROM，先重新界定需要单独映射的 RW RAM section。 */
	kernel_sec_start = CONFIG_PHYS_OFFSET & SECTION_MASK;
	kernel_sec_end = round_up(__pa(_end), SECTION_SIZE);
#endif
	pr_debug("physical kernel sections: 0x%08llx-0x%08llx\n",
		 kernel_sec_start, kernel_sec_end);

	/*
	 * 阶段 1：以首 bank/early fixmap 为立足点清旧表并建立全部 lowmem。
	 * map_lowmem() 返回后，arm_lowmem_limit 以下 RAM 已可由线性地址访问。
	 */
	prepare_page_table();
	map_lowmem();
	/*
	 * lowmem 已全部可寻址后，早期分配器可以使用其完整范围；在此之前保持
	 * adjust_lowmem_bounds() 设置的保守 PMD 对齐上限，避免从未映射 RAM 分配。
	 */
	memblock_set_current_limit(arm_lowmem_limit);
	pr_debug("lowmem limit is %08llx\n", (long long)arm_lowmem_limit);
	/*
	 * After this point early_alloc(), i.e. the memblock allocator, can
	 * be used
	 */
	/*
	 * 阶段 2：现在 early_alloc 可使用完整 lowmem，依次发布内核权限映射、
	 * DMA 连续区，并把 early fixmap 迁往正式分配的页表。
	 */
	map_kernel();
	dma_contiguous_remap();
	early_fixmap_shutdown();
	/*
	 * 阶段 3：重建高地址设备/向量，预建 kmap/fixmap 容器并初始化 TCM。
	 * devicemaps_init() 内部完成必要 TLB/cache 同步后才重新允许设备访问。
	 */
	devicemaps_init(mdesc);
	kmap_init();
	tcm_init();

	top_pmd = pmd_off_k(0xffff0000);
	/* 页表结构已稳定，发布高向量 PMD 借用指针供异常/向量路径使用。 */

	/*
	 * 最终交接：bootmem_init() 基于已稳定的线性映射建立 zones 与页元数据；
	 * 返回后通用页分配器接管，早期页表构造阶段结束。
	 */
	bootmem_init();
}

/*
 * early_mm_init - 在正式页表构造前完成 ARM MMU 的 CPU/平台相关准备。
 *
 * build_mem_type_table() 必须先于任何依赖 pgprot_kernel 的普通内存 fixmap；
 * early_paging_init() 随后可依据机器描述符改变 P:V 偏移和启动页表。函数只在
 * 启动 CPU 调用一次，无返回值；成功保证后续映射构造看到最终属性与物理空间。
 *
 * 入参：
 *   @mdesc：借用只读机器描述符，非 NULL；不取得或释放引用。
 * 前置条件：setup_arch() 早期串行上下文，CP15/启动页表可用；无锁、不睡眠。
 * 出参：无。
 * 返回：无直接返回值。
 * 副作用：固化全局内存属性，并可能切换平台物理地址空间。
 * 失败结果：无错误返回；致命硬件/映射问题在子函数中终止启动。
 */
void __init early_mm_init(const struct machine_desc *mdesc)
{
	/* 阶段 1 先生成属性模板，避免后续页表使用未按 CPU 修正的 prot。 */
	build_mem_type_table();
	/* 阶段 2 再执行可选物理空间切换，返回后 P:V 语义与页表保持一致。 */
	early_paging_init(mdesc);
}

/*
 * set_ptes - 向连续 PTE 槽写入连续 PFN 映射，并补 ARM 特有一致性属性。
 *
 * mm 是目标地址空间，addr 是首个虚拟地址，ptep 指向首槽，pteval 是首个
 * 叶子值，nr 必须大于零；调用者负责持有通用 MM 规定的页表锁并保证槽范围
 * 有效。用户区的有效普通页在发布前调用 __sync_icache_dcache()，处理可能
 * 执行的用户映射所需 I-cache/D-cache 一致性；special PTE 不代表常规
 * struct page，不能走该同步路径。用户映射还加 nG，使 ASID/地址空间切换
 * 不会把它当作全局内核翻译保留。
 *
 * set_pte_ext() 逐项执行架构描述符写入，pte_next_pfn() 保留权限并推进 PFN。
 * 函数不分配页表、不取得页面引用，也不做范围 TLB flush；旧映射失效和引用
 * 生命周期由更上层的 set_pte_range/mmap/fault 协议负责。
 *
 * 入参：
 *   @mm：借用的目标地址空间；内核映射场景允许按架构调用约定传 NULL。
 *   @addr：首个虚拟字节地址，必须页对齐并与 ptep 对应。
 *   @ptep：借用的首个 PTE 输入输出槽，非 NULL，至少连续 nr 项。
 *   @pteval：首个叶子描述符值，PFN 随循环递增，权限保持不变。
 *   @nr：待写 PTE 数量，必须大于 0。
 * 前置条件：调用者持有通用 MM 要求的页表锁并稳定映射页面；函数不睡眠。
 * 出参：@ptep 开始的 nr 个槽被覆盖，其他参数本身不修改。
 * 返回：无直接返回值。
 * 副作用：可能同步映射页的 I/D cache，并向硬件页表发布连续 PTE；不刷新 TLB。
 * 失败结果：无错误返回；nr=0 会因无符号递减破坏范围，必须由调用者排除。
 */
void set_ptes(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, pte_t pteval, unsigned int nr)
{
	unsigned long ext = 0;

	/*
	 * 准备阶段：只对用户地址且有效的用户 PTE 做 cache 同步/nG 修正。
	 * special PTE 没有普通页面语义，不能传给 __sync_icache_dcache()。
	 */
	if (addr < TASK_SIZE && pte_valid_user(pteval)) {
		if (!pte_special(pteval))
			__sync_icache_dcache(pteval);
		ext |= PTE_EXT_NG;
	}

	/*
	 * 发布阶段：每轮写一个硬件 PTE；nr 在写后递减，保证请求的首项一定
	 * 发布。后续迭代同步推进槽指针和 PFN，权限/ext 保持不变。
	 */
	for (;;) {
		set_pte_ext(ptep, pteval, ext);
		if (--nr == 0)
			break;
		ptep++;
		pteval = pte_next_pfn(pteval);
	}
	/*
	 * 返回不变量：nr 个连续槽均已更新，但任何旧 TLB 翻译仍由上层失效；
	 * 本函数也没有替调用者取得或释放映射页引用。
	 */
}
