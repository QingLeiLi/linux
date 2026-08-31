// SPDX-License-Identifier: GPL-2.0
/*
 * Bootmem core functions.
 *
 * Copyright (c) 2020, Bytedance.
 *
 *     Author: Muchun Song <songmuchun@bytedance.com>
 *
 */
/*
 * 启动内存信息的核心实现。体系结构早期页表可能由 memblock 分配并标为
 * reserved；本文件把来源 section 与类型编码进 page->private，并用额外页引用
 * 延续其寿命，待内存热插拔拆除相关映射后才把最后的保留页归还 buddy。
 */
#include <linux/mm.h>
#include <linux/compiler.h>
#include <linux/memblock.h>
#include <linux/bootmem_info.h>
#include <linux/memory_hotplug.h>
#include <linux/kmemleak.h>

/*
 * get_page_bootmem() - 为启动期保留页登记来源信息并取得一份延迟释放引用。
 *
 * 业务背景：x86 等架构建立 vmemmap/页表时调用本函数，使后续热移除能从
 * page->private 找回所属 section，并由 put_page_bootmem() 成对释放。
 * 入参：info 是待编码的 section 等无符号标识，必须能装入高位；page 是调用者
 * 借用的已保留 struct page，非 NULL，调用前仍由启动内存路径管理；type 是
 * 1..15 的 bootmem_type 分类，写入 private 的低四位。
 * 出参/返回：无直接返回值；成功后 page->private=(info<<4)|type，页引用加一，
 * 该新增引用的释放责任交给未来的 put_page_bootmem()。
 * 注意事项：调用者须保证该页尚未进入普通 buddy 分配，且串行初始化 private；
 * 本函数不加锁、不睡眠，非法编码会 BUG，不能把它当作可恢复的输入校验接口。
 */
void get_page_bootmem(unsigned long info, struct page *page,
		enum bootmem_type type)
{
	/* 先证明低四位和高位不会互相覆盖，再一次性发布编码值。 */
	BUG_ON(type > 0xf);
	BUG_ON(info > (ULONG_MAX >> 4));
	set_page_private(page, info << 4 | type);
	/* private 完整可读后再增加引用，配对释放者不会看到未初始化的来源。 */
	page_ref_inc(page);
}

/*
 * put_page_bootmem() - 归还一份启动页引用，并在只剩保留基准引用时释放该页。
 *
 * 业务背景：热移除页表/元数据时与 get_page_bootmem() 配对；它把启动期
 * reserved page 重新交给 buddy，而不是普通 put_page() 的匿名释放路径。
 * 入参：page 是借用的非 NULL 启动保留页，private 低位须为热插拔允许的类型；
 * 调用者交出此前持有的一份 bootmem 引用，调用后不得依赖该引用继续访问。
 * 出参/返回：无直接返回值；引用未降到 1 时只减计数，降到 1 时清零 private，
 * free_reserved_page() 消耗保留状态并把页发布给 buddy。
 * 注意事项：不加锁且可能触发页释放后的分配可见性；页类型错误会 BUG。并发调用
 * 依赖 page refcount 原子化唯一选出最后释放者，但引用计数不冻结 private 内容。
 */
void put_page_bootmem(struct page *page)
{
	/* type 是释放协议的身份检查；读取仅在调用者仍持有引用期间有效。 */
	enum bootmem_type type = bootmem_type(page);

	BUG_ON(type < MEMORY_HOTPLUG_MIN_BOOTMEM_TYPE ||
	       type > MEMORY_HOTPLUG_MAX_BOOTMEM_TYPE);

	/* 计数 1 是页原有的 reserved 基准引用；到达者负责完成最终状态转换。 */
	if (page_ref_dec_return(page) == 1) {
		set_page_private(page, 0);
		free_reserved_page(page);
	}
}

/*
 * register_page_bootmem_info_section() - 登记一个 sparsemem section 的启动页元数据。
 *
 * 业务背景：register_page_bootmem_info_node() 按 section 调用；只有 vmemmap 尚未
 * 预初始化的 section 才需让架构扫描并标记其启动分配页。
 * 入参：start_pfn 是 section 内任意 PFN（页号），纯输入；函数向下对齐后使用，
 * 不要求调用者预先对齐。出参/返回：无直接返回值；必要时登记整段
 * PAGES_PER_SECTION 的 memmap，具体 page 引用副作用由架构 helper 完成。
 * 注意事项：仅在 __init 启动阶段调用，可使用即将释放的初始化代码；不加锁，
 * 依赖 sparsemem/节点拓扑尚未并发热插拔，preinited section 是无副作用快速路径。
 */
static void __init register_page_bootmem_info_section(unsigned long start_pfn)
{
	/* section_nr 标识登记单位，ms 是 sparsemem 全局表中的借用描述符。 */
	unsigned long section_nr;
	struct mem_section *ms;

	/* 规范化 PFN 后取得唯一 section 描述，防止同一段按不同起点重复登记。 */
	start_pfn = SECTION_ALIGN_DOWN(start_pfn);
	section_nr = pfn_to_section_nr(start_pfn);
	ms = __nr_to_section(section_nr);

	/* 预初始化 vmemmap 已具备自己的生命周期信息；其余情况交给架构逐页标记。 */
	if (!preinited_vmemmap_section(ms))
		register_page_bootmem_memmap(section_nr, pfn_to_page(start_pfn),
					     PAGES_PER_SECTION);
}

/*
 * register_page_bootmem_info_node() - 扫描节点覆盖的 section 并登记启动页信息。
 *
 * 业务背景：体系结构完成 NODE_DATA 与 vmemmap 建立后调用，为日后的内存热移除
 * 准备 page->private 来源和引用；它是节点级 wrapper，实际登记在上方 helper。
 * 入参：pgdat 是借用的已初始化节点描述符，非 NULL；node_start_pfn 到
 * pgdat_end_pfn() 给出扫描半开区间，函数不取得 pgdat 引用也不修改其所有权。
 * 出参/返回：无直接返回值；仅属于本节点且有效的 section 可能获得启动页登记。
 * 注意事项：仅 __init、可串行执行；循环以 PAGES_PER_SECTION 为单位。平台若把
 * 同一 PFN 报给多个节点，early_pfn_to_nid() 的归属检查保证只登记一次。
 */
void __init register_page_bootmem_info_node(struct pglist_data *pgdat)
{
	/* pfn/end_pfn 为页号半开区间，node 是本轮唯一允许登记的 NUMA 节点号。 */
	unsigned long pfn, end_pfn;
	int node = pgdat->node_id;

	pfn = pgdat->node_start_pfn;
	end_pfn = pgdat_end_pfn(pgdat);

	/* register section info */
	/* 按 section 登记元数据，而不是逐个普通页重复扫描。 */
	for (; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		/*
		 * Some platforms can assign the same pfn to multiple nodes - on
		 * node0 as well as nodeN.  To avoid registering a pfn against
		 * multiple nodes we check that this pfn does not already
		 * reside in some other nodes.
		 */
		/*
		 * 某些平台会把相同 PFN 同时报给 node0 与 nodeN；先验证 PFN 有效且
		 * early NUMA 归属等于当前节点，避免同一页被取得两份登记引用。
		 */
		if (pfn_valid(pfn) && (early_pfn_to_nid(pfn) == node))
			register_page_bootmem_info_section(pfn);
	}
}
