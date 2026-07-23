// SPDX-License-Identifier: GPL-2.0
/*
 * Flattened Device Tree（FDT）解析与展开学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 职责边界：本文件负责校验固件提供的扁平 DTB、在 memblock 建立前提取
 * /chosen 与 /memory 信息、保留 DT 描述的物理区，并把紧凑 FDT 两遍展开为
 * 可由通用 OF API 遍历的 struct device_node/property 树。它不负责驱动匹配、
 * platform device 创建，也不实现 libfdt 的二进制格式解析。
 *
 * 主调用链：
 *   arch early setup
 *     -> early_init_dt_scan()
 *        -> verify/header + 根 #address-cells/#size-cells
 *        -> /chosen（initrd、crash、随机种子、命令行）
 *        -> /memory -> memblock
 *     -> early_init_fdt_reserve_self()/scan_reserved_mem()
 *     -> unflatten[_and_copy]_device_tree()
 *        -> __unflatten_device_tree()
 *           -> dry-run 计量 -> 单块分配 -> 实际构造 -> 反转 sibling 顺序
 *        -> of_root/of_aliases 发布
 *   overlay/unittest -> of_fdt_unflatten_tree() -> detached 子树
 *
 * 核心对象与生命周期：
 * - initial_boot_params 是早期 DTB 借用地址，校验成功后发布为 __ro_after_init；
 *   property 的 name/value 多数仍直接指向该 blob，因此 blob 必须被保留，或在
 *   unflatten 前由 copy_device_tree() 复制到长期 memblock。
 * - unflatten 分配一个连续块保存 device_node、property 和合成 name；节点的
 *   父子/兄弟关系只在构造期修改，发布后由 OF core 的锁、引用与 overlay
 *   协议管理。detached 树在挂接前对全局查找不可见。
 * - dt_root_*_cells、chosen_node_offset 和 of_fdt_crc32 是启动阶段派生状态；
 *   前两者决定 big-endian cell 的地址/长度解码，CRC 防止向 sysfs 暴露已被
 *   启动期擦除敏感属性后又意外篡改的原始 blob。
 *
 * 并发模型：early scan、memblock 登记和全局树展开由 boot CPU 串行执行；
 * 通用 of_fdt_unflatten_tree() 可在运行期使用，因此用 mutex 串行化共享的
 * unflatten/OF 初始化过程。bump allocator 本身无锁，只能操作调用者私有块。
 *
 * 方案权衡：直接让 property 借用 FDT 字节可避免逐属性复制和额外分配，但
 * 要求 DTB 生命周期覆盖节点树；两遍扫描能精确一次分配、减少碎片，代价是
 * 重走整棵树。早期只解析启动必需信息可在 slab/完整 OF 树建立前工作，但需
 * 使用节点 offset、big-endian cell 和 memblock 这套受限接口。
 */
/*
 * Functions for working with the Flattened Device Tree data format
 *
 * Copyright 2009 Benjamin Herrenschmidt, IBM Corp
 * benh@kernel.crashing.org
 */

#define pr_fmt(fmt)	"OF: fdt: " fmt

#include <linux/crash_dump.h>
#include <linux/crc32.h>
#include <linux/kernel.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/libfdt.h>
#include <linux/debugfs.h>
#include <linux/serial_core.h>
#include <linux/sysfs.h>
#include <linux/random.h>
#include <linux/kexec_handover.h>

#include <asm/setup.h>  /* for COMMAND_LINE_SIZE */
#include <asm/page.h>

#include "of_private.h"

/*
 * __dtb_empty_root_begin[] and __dtb_empty_root_end[] magically created by
 * cmd_wrap_S_dtb in scripts/Makefile.dtbs
 */
/*
 * 补充说明：两个链接器符号界定由 dtb 汇编包装规则生成的内建空根 DTB。
 * bootloader 未提供 DT 时，unflatten_device_tree() 借用该只读区并先校验
 * totalsize 未越过 end；随后复制到 memblock，使合成 property 的借用指针
 * 具有启动期所需生命周期。本文件不分配或释放这两个链接器区间。
 */
extern uint8_t __dtb_empty_root_begin[];
extern uint8_t __dtb_empty_root_end[];

/*
 * of_fdt_limit_memory - limit the number of regions in the /memory node
 * @limit: maximum entries
 *
 * Adjust the flattened device tree to have at most 'limit' number of
 * memory entries in the /memory node. This function may be called
 * any time after initial_boot_param is set.
 */
/*
 * 补充说明：of_fdt_limit_memory - 截短根 /memory 节点的 reg 条目数。
 *
 * 调用关系：特定体系结构在 initial_boot_params 发布、内存扫描前调用。
 * @limit 是最多保留的 {address,size} 条目数，应为非负；每项 cell 数取自已
 * 初始化的根 cell 配置。函数直接原地缩短可写 FDT 的 reg property，不复制
 * value、不更新返回状态；找不到 /memory/reg 或原长度未超限时无副作用。
 * fdt_setprop() 的失败未向上传播，因此调用者仍须把固件树视为可信输入。
 */
void __init of_fdt_limit_memory(int limit)
{
	/*
	 * 变量地图：memory 是 libfdt 节点 offset；val 借用 blob 内 reg 数据；
	 * len 是属性字节数；cell_size 是一个地址/长度元组的字节跨度。
	 */
	int memory;
	int len;
	const void *val;
	int cell_size = sizeof(uint32_t)*(dt_root_addr_cells + dt_root_size_cells);

	memory = fdt_path_offset(initial_boot_params, "/memory");
	if (memory > 0) {
		/* 只有原属性确实超过 limit 个完整 tuple 时才原地缩短。 */
		val = fdt_getprop(initial_boot_params, memory, "reg", &len);
		if (len > limit*cell_size) {
			len = limit*cell_size;
			pr_debug("Limiting number of entries to %d\n", limit);
			fdt_setprop(initial_boot_params, memory, "reg", val,
					len);
		}
	}
}

/*
 * of_fdt_device_is_available - 按 status property 判断扁平节点是否可用。
 *
 * @blob 是调用期间稳定的 FDT 借用指针；@node 是其中有效节点 offset。
 * 缺少 status 按 DT 规范默认为 available；只有首字符串严格为 "ok"/"okay"
 * 返回 true，其他值返回 false。函数只读 blob，不分配、不睡眠、无引用变化。
 */
bool of_fdt_device_is_available(const void *blob, unsigned long node)
{
	/* status 指针直接借用字符串块，只在 blob 生命周期内有效。 */
	const char *status = fdt_stringlist_get(blob, node, "status", 0, NULL);

	if (!status)
		return true;

	if (!strcmp(status, "ok") || !strcmp(status, "okay"))
		return true;

	return false;
}

/*
 * unflatten_dt_alloc - 从调用者私有连续块中按对齐要求切出一段空间。
 *
 * @mem 是输入输出 bump 指针：入口指向尚未使用区域，成功后推进到分配末尾；
 * @size 为字节数，@align 为 2 的幂对齐。返回对齐后的起点，不清零、无失败
 * 表达和边界检查；安全性依赖 __unflatten_device_tree() 的 dry-run 已精确
 * 计算总大小。dry-run 时 NULL 指针只作为概念偏移累计，绝不能解引用。
 */
static void *unflatten_dt_alloc(void **mem, unsigned long size,
				       unsigned long align)
{
	void *res;

	*mem = PTR_ALIGN(*mem, align);
	res = *mem;
	*mem += size;

	return res;
}

/*
 * populate_properties - 为一个 device_node 构造 property 链及兼容 name。
 *
 * @blob/@nodename 是 FDT 借用数据；@offset 标识当前节点；@mem 是两遍扫描
 * 共用的 bump 指针；@np 是正在构造的节点；@dryrun=true 时只累计空间，
 * 不写 np 或 property。实际构造时 property 结构位于连续块中，但 name/value
 * 通常仍借用 FDT 字符串/结构块，故 blob 不能早于节点树失效。
 * 函数遍历全部属性，提取多种 phandle 兼容形式，尾插保持属性原顺序；旧版
 * FDT 缺少 name 时从 unit name 合成内联 value。坏属性被告警并跳过，无返回值。
 */
static void populate_properties(const void *blob,
				int offset,
				void **mem,
				struct device_node *np,
				const char *nodename,
				bool dryrun)
{
	/*
	 * 变量地图：pprev 始终指向“下一条链指针”的存放位置，避免区分首节点；
	 * cur 是 libfdt property offset；has_name 决定是否需要额外合成属性。
	 */
	struct property *pp, **pprev = NULL;
	int cur;
	bool has_name = false;

	pprev = &np->properties;
	/* 每轮先验证 value 和名字，再计入 property 结构空间。 */
	for (cur = fdt_first_property_offset(blob, offset);
	     cur >= 0;
	     cur = fdt_next_property_offset(blob, cur)) {
		const __be32 *val;
		const char *pname;
		u32 sz;

		val = fdt_getprop_by_offset(blob, cur, &pname, &sz);
		if (!val) {
			pr_warn("Cannot locate property at 0x%x\n", cur);
			continue;
		}

		/*
		 * property value 可定位但名字不可定位时也不能构造 property：运行期
		 * 查找依赖 name，保留匿名 value 只会产生不可访问的半对象。
		 */
		if (!pname) {
			pr_warn("Cannot find property name at 0x%x\n", cur);
			continue;
		}

		if (!strcmp(pname, "name"))
			has_name = true;

		pp = unflatten_dt_alloc(mem, sizeof(struct property),
					__alignof__(struct property));
		/* dry-run 到这里已完成尺寸记账，禁止触碰概念地址 pp/np。 */
		if (dryrun)
			continue;

		/* We accept flattened tree phandles either in
		 * ePAPR-style "phandle" properties, or the
		 * legacy "linux,phandle" properties.  If both
		 * appear and have different values, things
		 * will get weird. Don't do that.
		 */
		/*
		 * 原注释含义：接受 ePAPR "phandle" 与旧 "linux,phandle"；两者同时
		 * 且数值不同没有可靠冲突解决方案。只在 np->phandle 尚未设置时取值，
		 * 因而属性顺序会决定冲突结果，固件必须保证一致。
		 */
		if (!strcmp(pname, "phandle") ||
		    !strcmp(pname, "linux,phandle")) {
			if (!np->phandle)
				np->phandle = be32_to_cpup(val);
		}

		/* And we process the "ibm,phandle" property
		 * used in pSeries dynamic device tree
		 * stuff
		 */
		/* 原注释含义：pSeries 动态设备树另用 ibm,phandle，并允许其覆盖前值。 */
		if (IS_ENABLED(CONFIG_PPC_PSERIES) && !strcmp(pname, "ibm,phandle"))
			np->phandle = be32_to_cpup(val);

		pp->name   = (char *)pname;
		pp->length = sz;
		pp->value  = (__be32 *)val;
		/* 尾插发布当前 property，并把 pprev 推进到它的 next 字段。 */
		*pprev     = pp;
		pprev      = &pp->next;
	}

	/* With version 0x10 we may not have the name property,
	 * recreate it here from the unit name if absent
	 */
	/*
	 * 原注释含义：FDT v0x10 可能没有 name；从最后一个 '/' 后到 '@' 前提取
	 * base name。合成 property 与字符串一次分配，value 指向 pp 后的内联区，
	 * 因而不再依赖 blob 中不存在的数据。
	 */
	if (!has_name) {
		const char *p = nodename, *ps = p, *pa = NULL;
		int len;

		/* 单遍记录最后一个路径分隔符和最后一个 unit-address 分隔符。 */
		while (*p) {
			if ((*p) == '@')
				pa = p;
			else if ((*p) == '/')
				ps = p + 1;
			p++;
		}

		if (pa < ps)
			pa = p;
		/* 长度包含 NUL；没有 @ 时复制整个最后路径分量。 */
		len = (pa - ps) + 1;
		pp = unflatten_dt_alloc(mem, sizeof(struct property) + len,
					__alignof__(struct property));
		/* dry-run 只计入结构和字符串长度；实际遍才写链、复制并补 NUL。 */
		if (!dryrun) {
			pp->name   = "name";
			pp->length = len;
			pp->value  = pp + 1;
			*pprev     = pp;
			memcpy(pp->value, ps, len - 1);
			((char *)pp->value)[len - 1] = 0;
			pr_debug("fixed up name for %s -> %s\n",
				 nodename, (char *)pp->value);
		}
	}
}

/*
 * populate_node - 构造一个 device_node 并接入尚未发布的父节点。
 *
 * @blob/@offset 标识 FDT 节点；@mem 是 bump 指针；@dad 是可空父节点；
 * @pnp 是非 NULL 输出槽，成功写新节点、名称读取失败写 NULL；@dryrun 只计量。
 * 节点结构和 full_name 字符串连续分配，property 由 populate_properties()
 * 继续切分同一块。实际构造时新节点头插 dad->child，稍后 reverse_nodes()
 * 恢复 DTS 顺序。返回 0 或 libfdt 的负错误；失败前无已发布全局对象。
 */
static int populate_node(const void *blob,
			  int offset,
			  void **mem,
			  struct device_node *dad,
			  struct device_node **pnp,
			  bool dryrun)
{
	struct device_node *np;
	const char *pathp;
	int len;

	pathp = fdt_get_name(blob, offset, &len);
	/* libfdt 通过 len 返回负错误；输出清空防止调用者使用半构造节点。 */
	if (!pathp) {
		*pnp = NULL;
		return len;
	}

	len++;

	np = unflatten_dt_alloc(mem, sizeof(struct device_node) + len,
				__alignof__(struct device_node));
	if (!dryrun) {
		/* of_node_init 建立引用/标志初态，full_name 紧随结构体存放。 */
		char *fn;
		of_node_init(np);
		np->full_name = fn = ((char *)np) + sizeof(*np);

		memcpy(fn, pathp, len);

		if (dad != NULL) {
			/* 构造期头插无需锁；整棵树尚未对 OF 读者发布。 */
			np->parent = dad;
			np->sibling = dad->child;
			dad->child = np;
		}
	}

	populate_properties(blob, offset, mem, np, pathp, dryrun);
	if (!dryrun) {
		/* name 指向 property value；缺失仅用静态哨兵保证后续读者非 NULL。 */
		np->name = of_get_property(np, "name", NULL);
		if (!np->name)
			np->name = "<NULL>";
	}

	*pnp = np;
	return 0;
}

/*
 * reverse_nodes - 递归恢复每个父节点下的 DTS 原始 sibling 顺序。
 *
 * @parent 是构造期私有子树根，非 NULL；函数不分配、不失败。populate_node()
 * 为 O(1) 头插而反转了兄弟顺序，本函数先深度处理子树，再原地反转每条 child
 * 链。调用时树尚未发布，无需 devtree 锁；递归深度已受 FDT_MAX_DEPTH 限制。
 */
static void reverse_nodes(struct device_node *parent)
{
	struct device_node *child, *next;

	/* In-depth first */
	/* 原注释含义：先深入处理每个 child 的子链，确保整棵树逐层恢复。 */
	child = parent->child;
	while (child) {
		reverse_nodes(child);

		child = child->sibling;
	}

	/* Reverse the nodes in the child list */
	/* 原注释含义：逐个摘下旧链首并头插新链，最终只改变 sibling 指向。 */
	child = parent->child;
	parent->child = NULL;
	while (child) {
		next = child->sibling;

		child->sibling = parent->child;
		parent->child = child;
		child = next;
	}
}

/**
 * unflatten_dt_nodes - Alloc and populate a device_node from the flat tree
 * @blob: The parent device tree blob
 * @mem: Memory chunk to use for allocating device nodes and properties
 * @dad: Parent struct device_node
 * @nodepp: The device_node tree created by the call
 *
 * Return: The size of unflattened device tree or error code
 */
/*
 * 补充说明：unflatten_dt_nodes - 单遍计量或构造 FDT 节点与属性连续块。
 *
 * @blob 是已校验 FDT 借用指针；@mem 为 NULL 表示 dry-run，否则指向足够大的
 * 私有块；@dad 可空，非空表示把多个一级节点接成其子树；@nodepp 可空，实际
 * 构造时输出第一个创建节点。函数用固定 64 层栈数组跟踪当前父节点。
 * 返回消耗字节数；libfdt 遍历/节点错误返回负值。实际构造成功后 sibling
 * 顺序已恢复；失败对象尚未全局发布，连续块由上层决定回收。
 */
static int unflatten_dt_nodes(const void *blob,
			      void *mem,
			      struct device_node *dad,
			      struct device_node **nodepp)
{
	/*
	 * 变量地图：offset/depth 驱动 libfdt DFS；nps[d] 是深度 d 的父节点；
	 * base 固定用于计算消耗量；dryrun 控制“只移动指针”还是写对象。
	 */
	struct device_node *root;
	int offset = 0, depth = 0, initial_depth = 0;
#define FDT_MAX_DEPTH	64
	struct device_node *nps[FDT_MAX_DEPTH];
	void *base = mem;
	bool dryrun = !base;
	int ret;

	if (nodepp)
		*nodepp = NULL;

	/*
	 * We're unflattening device sub-tree if @dad is valid. There are
	 * possibly multiple nodes in the first level of depth. We need
	 * set @depth to 1 to make fdt_next_node() happy as it bails
	 * immediately when negative @depth is found. Otherwise, the device
	 * nodes except the first one won't be unflattened successfully.
	 */
	/*
	 * 原注释含义：展开子树时遍历起点并非 FDT 根，第一层可能有多个 sibling。
	 * 把初始 depth 设为 1 可避免 fdt_next_node() 见到负 depth 提前停止。
	 */
	if (dad)
		depth = initial_depth = 1;

	root = dad;
	nps[depth] = dad;

	for (offset = 0;
	     offset >= 0 && depth >= initial_depth;
	     offset = fdt_next_node(blob, offset, &depth)) {
		if (WARN_ON_ONCE(depth >= FDT_MAX_DEPTH - 1))
			/* 超深节点无法安全索引 nps[depth+1]，跳过而非越界写栈。 */
			continue;

		/*
		 * 未配置 OF_KOBJ 时，disabled 节点不会进入运行期树；需要 kobject
		 * 表示时保留它们，便于 sysfs/动态 OF 管理完整固件层次。
		 */
		if (!IS_ENABLED(CONFIG_OF_KOBJ) &&
		    !of_fdt_device_is_available(blob, offset))
			continue;

		ret = populate_node(blob, offset, &mem, nps[depth],
				   &nps[depth+1], dryrun);
		if (ret < 0)
			return ret;

		if (!dryrun && nodepp && !*nodepp)
			/* 只发布本次遍历创建的第一节点，供子树调用者取得根。 */
			*nodepp = nps[depth+1];
		if (!dryrun && !root)
			root = nps[depth+1];
	}

	if (offset < 0 && offset != -FDT_ERR_NOTFOUND) {
		/* NOTFOUND 是正常 DFS 结束，其他负值表示 blob 结构损坏。 */
		pr_err("Error %d processing FDT\n", offset);
		return -EINVAL;
	}

	/*
	 * Reverse the child list. Some drivers assumes node order matches .dts
	 * node order
	 */
	/*
	 * 原注释含义：部分驱动依赖运行期节点顺序与 .dts 一致；实际构造才有链
	 * 可反转，dry-run 只计算大小。root 是 dad 或首个新建节点。
	 */
	if (!dryrun)
		reverse_nodes(root);

	return mem - base;
}

/**
 * __unflatten_device_tree - create tree of device_nodes from flat blob
 * @blob: The blob to expand
 * @dad: Parent device node
 * @mynodes: The device_node tree created by the call
 * @dt_alloc: An allocator that provides a virtual address to memory
 * for the resulting tree
 * @detached: if true set OF_DETACHED on @mynodes
 *
 * unflattens a device-tree, creating the tree of struct device_node. It also
 * fills the "name" and "type" pointers of the nodes so the normal device-tree
 * walking functions can be used.
 *
 * Return: NULL on failure or the memory chunk containing the unflattened
 * device tree on success.
 */
/*
 * 补充说明：__unflatten_device_tree - 两遍展开 FDT 并返回承载整树的内存块。
 *
 * @blob 为已存活的 FDT 借用指针；@dad/@mynodes 可空，分别指定父节点和输出
 * 子树根；@dt_alloc 是必须返回至少 size 字节、满足 align 的分配回调；
 * @detached 决定是否给输出根设置 OF_DETACHED。
 * 第一遍 dry-run 精确计量，随后一次分配并在尾部放 canary，第二遍构造。
 * 返回连续块起点；NULL 表示参数/header/遍历/分配失败。若第二遍在分配后
 * 失败，当前接口不会把 mem 返回给调用者，依赖“已校验 blob 的两遍结果一致”
 * 避免该罕见泄漏。成功后 node/property 归该块所有，但多数 property
 * name/value 仍借用 blob；释放责任交给调用者。
 */
void *__unflatten_device_tree(const void *blob,
			      struct device_node *dad,
			      struct device_node **mynodes,
			      void *(*dt_alloc)(u64 size, u64 align),
			      bool detached)
{
	int size;
	void *mem;
	int ret;

	if (mynodes)
		/* 所有失败出口保持输出为 NULL，避免观察到半构造树。 */
		*mynodes = NULL;

	/* 入口诊断和 header 校验阶段尚未调用 allocator，失败没有内存需回收。 */
	pr_debug(" -> unflatten_device_tree()\n");

	if (!blob) {
		pr_debug("No device tree pointer\n");
		return NULL;
	}

	/* 非 NULL 仍是未信任外部字节；只打印固定 header 字段，随后立即完整校验。 */
	pr_debug("Unflattening device tree:\n");
	pr_debug("magic: %08x\n", fdt_magic(blob));
	pr_debug("size: %08x\n", fdt_totalsize(blob));
	pr_debug("version: %08x\n", fdt_version(blob));

	if (fdt_check_header(blob)) {
		pr_err("Invalid device tree blob header\n");
		return NULL;
	}

	/* First pass, scan for size */
	/*
	 * 原注释含义：第一遍 mem=NULL，仅执行与真实构造相同的分配推进，得到
	 * 精确字节数；任何结构错误都在占用资源前失败。
	 */
	size = unflatten_dt_nodes(blob, NULL, dad, NULL);
	if (size <= 0)
		return NULL;

	size = ALIGN(size, 4);
	pr_debug("  size is %d, allocating...\n", size);

	/* Allocate memory for the expanded device tree */
	/*
	 * 原注释含义：按 device_node 对齐一次分配，并额外留 4 字节 canary。
	 * 分配成功后整棵树共享此块，避免每个 node/property 单独分配。
	 */
	mem = dt_alloc(size + 4, __alignof__(struct device_node));
	if (!mem)
		return NULL;

	*(__be32 *)(mem + size) = cpu_to_be32(0xdeadbeef);

	pr_debug("  unflattening %p...\n", mem);

	/* Second pass, do actual unflattening */
	/* 原注释含义：第二遍以真实基址重复同一遍历，写入节点、属性和链接。 */
	ret = unflatten_dt_nodes(blob, mem, dad, mynodes);

	/* canary 改变说明计量与构造不一致并发生越界，但仍记录后续 ret 供诊断。 */
	if (be32_to_cpup(mem + size) != 0xdeadbeef)
		pr_warn("End of tree marker overwritten: %08x\n",
			be32_to_cpup(mem + size));

	if (ret <= 0)
		return NULL;

	if (detached && mynodes && *mynodes) {
		/* detached 子树尚未进入全局 OF 索引，需由 overlay/调用者后续挂接。 */
		of_node_set_flag(*mynodes, OF_DETACHED);
		pr_debug("unflattened tree is detached\n");
	}

	pr_debug(" <- unflatten_device_tree()\n");
	return mem;
}

/*
 * kernel_tree_alloc - 为运行期 detached FDT 展开分配清零连续块。
 *
 * @size 为字节数，@align 由通用展开器给出但 kzalloc 的天然对齐已满足
 * device_node 要求，故无需单独使用。返回持有内存或 NULL；GFP_KERNEL 表示
 * 可睡眠。调用者最终负责释放整个块。
 */
static void *kernel_tree_alloc(u64 size, u64 align)
{
	return kzalloc(size, GFP_KERNEL);
}

/*
 * 运行期 unflatten 串行锁：保护多个 overlay/unittest 调用不会同时进入共享
 * OF 节点初始化协议。锁不保护已发布树的遍历，后者遵循 OF core 自身同步。
 */
static DEFINE_MUTEX(of_fdt_unflatten_mutex);

/**
 * of_fdt_unflatten_tree - create tree of device_nodes from flat blob
 * @blob: Flat device tree blob
 * @dad: Parent device node
 * @mynodes: The device tree created by the call
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 *
 * Return: NULL on failure or the memory chunk containing the unflattened
 * device tree on success.
 */
/*
 * 补充说明：of_fdt_unflatten_tree - 把任意 FDT 展开为尚未挂接的运行期子树。
 *
 * @blob 是必须覆盖返回树生命周期的借用 blob；@dad 可空父节点；@mynodes
 * 可空输出根。进程上下文获取 mutex，使用 GFP_KERNEL 分配，因而可以睡眠。
 * 返回连续内存块或 NULL；成功输出根带 OF_DETACHED，调用者负责后续挂接及
 * 最终释放 blob/展开块。mutex 在全部失败和成功路径成对释放。
 */
void *of_fdt_unflatten_tree(const unsigned long *blob,
			    struct device_node *dad,
			    struct device_node **mynodes)
{
	void *mem;

	/* 不可并发执行节点初始化和 detached 标志发布，故覆盖完整展开调用。 */
	mutex_lock(&of_fdt_unflatten_mutex);
	mem = __unflatten_device_tree(blob, dad, mynodes, &kernel_tree_alloc,
				      true);
	mutex_unlock(&of_fdt_unflatten_mutex);

	return mem;
}
EXPORT_SYMBOL_GPL(of_fdt_unflatten_tree);

/* Everything below here references initial_boot_params directly. */
/*
 * 原注释含义：以下早期 helper 不再接收独立 blob，而是统一借用全局
 * initial_boot_params；调用前必须由 early_init_dt_verify() 成功发布。
 *
 * dt_root_*_cells 是根节点地址/长度 cell 数，仅在 __init 期读写；两个
 * initial_boot_params 变量分别保存 DTB 虚拟借用地址和物理地址，验证后写入，
 * __ro_after_init 防止运行期被改指向其他未校验 blob。
 */
int __initdata dt_root_addr_cells;
int __initdata dt_root_size_cells;

void *initial_boot_params __ro_after_init;
phys_addr_t initial_boot_params_pa __ro_after_init;

#ifdef CONFIG_OF_EARLY_FLATTREE

/*
 * 已验证 FDT 的启动期 CRC 快照。early_init_dt_verify() 初次计算；若后续主动
 * 擦除 rng-seed 则同步重算。late sysfs 发布前再次校验，防止把意外变化或
 * 敏感旧内容暴露给用户态。仅启动期写，of_fdt_raw_init() 读取。
 */
static u32 of_fdt_crc32;

/*
 * fdt_reserve_elfcorehdr() - reserves memory for elf core header
 *
 * This function reserves the memory occupied by an elf core header
 * described in the device tree. This region contains all the
 * information about primary kernel's core image and is used by a dump
 * capture kernel to access the system memory on primary kernel.
 */
/*
 * 补充说明：fdt_reserve_elfcorehdr - 为捕获内核保留主内核 vmcore 元数据。
 *
 * 无入参；读取 /chosen 解析后发布的 elfcorehdr_addr/size。仅在 CRASH_DUMP
 * 配置且 size 非零时工作；若区间已被其他 reservation 覆盖则告警并拒绝，
 * 否则调用 memblock_reserve() 从早期可分配内存中摘除。
 * 返回 void；不映射、不取得内容 ownership。成功不代表数据已校验，只保证
 * 后续分配器不会覆盖供 dump kernel 读取的 ELF core header。
 */
static void __init fdt_reserve_elfcorehdr(void)
{
	if (!IS_ENABLED(CONFIG_CRASH_DUMP) || !elfcorehdr_size)
		return;

	if (memblock_is_region_reserved(elfcorehdr_addr, elfcorehdr_size)) {
		/* 重叠可能表示错误布局，不能把既有 reservation 当成本函数成功。 */
		pr_warn("elfcorehdr is overlapped\n");
		return;
	}

	memblock_reserve(elfcorehdr_addr, elfcorehdr_size);

	pr_info("Reserving %llu KiB of memory at 0x%llx for elfcorehdr\n",
		elfcorehdr_size >> 10, elfcorehdr_addr);
}

/**
 * early_init_fdt_scan_reserved_mem() - create reserved memory regions
 *
 * This function grabs memory from early allocator for device exclusive use
 * defined in device tree structures. It should be called by arch specific code
 * once the early allocator (i.e. memblock) has been fully activated.
 */
/*
 * 补充说明：early_init_fdt_scan_reserved_mem - 把 DT 声明的保留区提交给 memblock。
 *
 * 体系结构在 memblock 可用、普通页分配前调用。无入参/直接返回值；缺少 FDT
 * 安全返回。依次保留 elfcorehdr、解析 /reserved-memory，再遍历 FDT header
 * 的 /memreserve/ 表。base/size 单位为物理字节，blob 只借用。
 * 失败的 memreserve 索引会终止该表扫描并记录错误；已成功提交的 reservation
 * 不回滚，因为它们仍是固件明确要求不可分配的区域。
 */
void __init early_init_fdt_scan_reserved_mem(void)
{
	/* n 是 memreserve 索引；base/size 接收 big-endian 条目解码后的物理区间。 */
	int n;
	int res;
	u64 base, size;

	if (!initial_boot_params)
		return;

	fdt_reserve_elfcorehdr();
	/* reserved-memory 子系统处理子节点、动态分配和设备专属保留语义。 */
	fdt_scan_reserved_mem();

	/* Process header /memreserve/ fields */
	/*
	 * 原注释含义：header reservation map 以 size==0 结束；每个有效条目直接
	 * 交给 memblock，重叠合并规则由 memblock 负责。
	 */
	for (n = 0; ; n++) {
		res = fdt_get_mem_rsv(initial_boot_params, n, &base, &size);
		if (res) {
			pr_err("Invalid memory reservation block index %d\n", n);
			break;
		}
		if (!size)
			break;
		memblock_reserve(base, size);
	}
}

/**
 * early_init_fdt_reserve_self() - reserve the memory used by the FDT blob
 */
/*
 * 补充说明：early_init_fdt_reserve_self - 防止 memblock 分配覆盖当前 DTB。
 *
 * 无入参；initial_boot_params 是已验证且可由 __pa() 转换的长期借用地址。
 * 成功把 [DTB physical, physical+totalsize) 标为 reserved；不复制 blob，
 * 因而 unflatten 后 property 对原始 name/value 的借用仍然有效。
 * 缺少 FDT 时无操作，返回 void；调用时机由体系结构保证 memblock 已激活。
 */
void __init early_init_fdt_reserve_self(void)
{
	if (!initial_boot_params)
		return;

	/* Reserve the dtb region */
	/* 原注释含义：保留完整 totalsize，而非只保留 header 或 structure block。 */
	memblock_reserve(__pa(initial_boot_params),
			 fdt_totalsize(initial_boot_params));
}

/**
 * of_scan_flat_dt - scan flattened tree blob and call callback on each.
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan the flattened device-tree, it is
 * used to extract the memory information at boot before we can
 * unflatten the tree
 */
/*
 * 补充说明：of_scan_flat_dt - 以深度优先顺序同步回调每个扁平节点。
 *
 * @it 是非 NULL 借用回调，收到 node offset、借用名称、深度和 @data；@data
 * ownership 不变。只在 initial_boot_params 生命周期内调用，回调不得保存
 * pathp 裸指针到 blob 失效之后。无 FDT 返回 0；回调非零会停止扫描并原样
 * 返回；libfdt 的正常结束也返回当前 rc。本 helper 不展开节点、不加锁。
 */
int __init of_scan_flat_dt(int (*it)(unsigned long node,
				     const char *uname, int depth,
				     void *data),
			   void *data)
{
	const void *blob = initial_boot_params;
	const char *pathp;
	int offset, rc = 0, depth = -1;

	if (!blob)
		return 0;

	/* rc 同时是回调停止标志，保证非零返回后不再访问后续节点。 */
	for (offset = fdt_next_node(blob, -1, &depth);
	     offset >= 0 && depth >= 0 && !rc;
	     offset = fdt_next_node(blob, offset, &depth)) {

		pathp = fdt_get_name(blob, offset, NULL);
		rc = it(offset, pathp, depth, data);
	}
	return rc;
}

/**
 * of_scan_flat_dt_subnodes - scan sub-nodes of a node call callback on each.
 * @parent: parent node
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan sub-nodes of a node.
 */
/*
 * 补充说明：of_scan_flat_dt_subnodes - 同步遍历指定父节点的直接子节点。
 *
 * @parent 是 initial_boot_params 中的节点 offset；@it 接收子节点 offset、
 * 借用名称和 @data，不包含深度；@data 不转移 ownership。首个非零回调结果
 * 立即返回，否则返回 0。宏由 libfdt 驱动，只访问一层，不递归、不分配。
 */
int __init of_scan_flat_dt_subnodes(unsigned long parent,
				    int (*it)(unsigned long node,
					      const char *uname,
					      void *data),
				    void *data)
{
	const void *blob = initial_boot_params;
	int node;

	fdt_for_each_subnode(node, blob, parent) {
		/* pathp 只借用到同步回调返回；rc 决定是否继续下一个 sibling。 */
		const char *pathp;
		int rc;

		pathp = fdt_get_name(blob, node, NULL);
		rc = it(node, pathp, data);
		/* 同步停止协议允许调用者用返回值完成“找到即结束”的扫描。 */
		if (rc)
			return rc;
	}
	return 0;
}

/**
 * of_get_flat_dt_subnode_by_name - get the subnode by given name
 *
 * @node: the parent node
 * @uname: the name of subnode
 * @return offset of the subnode, or -FDT_ERR_NOTFOUND if there is none
 */

/*
 * 补充说明：@node 是父 offset，@uname 是借用且 NUL 结尾的精确子节点名。
 * 返回匹配 offset，找不到返回 -FDT_ERR_NOTFOUND，其他负值来自 libfdt；
 * 函数只读 initial_boot_params，返回的整数不持有任何节点引用。
 */
int __init of_get_flat_dt_subnode_by_name(unsigned long node, const char *uname)
{
	return fdt_subnode_offset(initial_boot_params, node, uname);
}

/*
 * of_get_flat_dt_root - find the root node in the flat blob
 */
/*
 * 原注释含义：返回扁平 blob 根节点 offset。FDT 格式固定根 offset 为 0，
 * 因此无需查表；返回值只在 initial_boot_params 的 libfdt API 中使用。
 */
unsigned long __init of_get_flat_dt_root(void)
{
	return 0;
}

/*
 * of_get_flat_dt_prop - Given a node in the flat blob, return the property ptr
 *
 * This function can be used within scan_flattened_dt callback to get
 * access to properties
 */
/*
 * 补充说明：of_get_flat_dt_prop - 借出全局 FDT 某节点的属性字节。
 *
 * @node 是有效 offset，@name 是借用属性名，@size 可空输出属性字节长度。
 * 成功返回直接指向 initial_boot_params 的只读借用指针；失败返回 NULL，
 * libfdt 可通过 size 写负错误。不得释放或越过 blob 生命周期保存该指针。
 */
const void *__init of_get_flat_dt_prop(unsigned long node, const char *name,
				       int *size)
{
	return fdt_getprop(initial_boot_params, node, name, size);
}

/*
 * of_flat_dt_get_addr_size_prop - 校验并借出 {address,size} 元组数组。
 *
 * @node/@name 定位属性；@entries 是非 NULL 输出元组数。每个元组宽度由根
 * #address-cells + #size-cells 决定，单位转换为字节。属性缺失或长度不能被
 * tuple 宽度整除时写 0 并返回 NULL；成功返回 big-endian cell 借用指针。
 * 不解码数值、不转移 ownership，调用者须在 blob 生命周期内使用。
 */
const __be32 *__init of_flat_dt_get_addr_size_prop(unsigned long node,
						   const char *name,
						   int *entries)
{
	const __be32 *prop;
	int len, elen = (dt_root_addr_cells + dt_root_size_cells) * sizeof(__be32);

	prop = of_get_flat_dt_prop(node, name, &len);
	/* 模运算同时拒绝截断尾部，确保后续按 entry_cells 推进不会越界。 */
	if (!prop || len % elen) {
		*entries = 0;
		return NULL;
	}

	*entries = len / elen;
	return prop;
}

/*
 * of_flat_dt_get_addr_size - 解码恰好一个 {address,size} 元组。
 *
 * @node/@name 定位属性；@addr/@size 是非 NULL 的 u64 输出，失败保持原值。
 * 只有属性存在且 entries==1 才调用解码 helper；成功写物理地址和字节长度并
 * 返回 true，多条/缺失/畸形返回 false。无分配和 ownership 变化。
 */
bool __init of_flat_dt_get_addr_size(unsigned long node, const char *name,
				     u64 *addr, u64 *size)
{
	const __be32 *prop;
	int entries;

	prop = of_flat_dt_get_addr_size_prop(node, name, &entries);
	if (!prop || entries != 1)
		return false;

	of_flat_dt_read_addr_size(prop, 0, addr, size);
	return true;
}

/*
 * of_flat_dt_read_addr_size - 按索引解码一个地址/长度元组。
 *
 * @prop 是已由 of_flat_dt_get_addr_size_prop() 校验的 big-endian cell 数组；
 * @entry_index 从 0 开始且必须小于 entries；@addr/@size 为非 NULL 输出。
 * 函数不做边界检查，先按根 tuple 宽度做 cell 指针算术，再分别消费 address
 * 与 size cells。返回 void，不保存 prop、不改变 blob。
 */
void __init of_flat_dt_read_addr_size(const __be32 *prop, int entry_index,
				      u64 *addr, u64 *size)
{
	int entry_cells = dt_root_addr_cells + dt_root_size_cells;
	/* 指针加法单位是 __be32 cell，不是字节。 */
	prop += entry_cells * entry_index;

	*addr = dt_mem_next_cell(dt_root_addr_cells, &prop);
	*size = dt_mem_next_cell(dt_root_size_cells, &prop);
}

/**
 * of_fdt_is_compatible - Return true if given node from the given blob has
 * compat in its compatible list
 * @blob: A device tree blob
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 *
 * Return: a non-zero value on match with smaller values returned for more
 * specific compatible values.
 */
/*
 * 补充说明：of_fdt_is_compatible - 在任意 blob 的 compatible 列表中评分匹配。
 *
 * @blob/@compat 为借用字符串数据，@node 是 blob 内 offset。按 property 顺序
 * 扫描 NUL 字符串列表；首项匹配返回 1，越靠后数值越大、优先级越低，完全
 * 不匹配返回 0。返回分数而非布尔值供 machine match 选择最具体项。
 */
static int of_fdt_is_compatible(const void *blob,
			      unsigned long node, const char *compat)
{
	const char *cp;
	int idx = 0, score = 0;

	while ((cp = fdt_stringlist_get(blob, node, "compatible", idx++, NULL))) {
		/* score 在比较前递增，使第一个 compatible 得到最高优先级 1。 */
		score++;
		if (of_compat_cmp(cp, compat, strlen(compat)) == 0)
			return score;
	}

	return 0;
}

/**
 * of_flat_dt_is_compatible - Return true if given node has compat in compatible list
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 */
/*
 * 补充说明：这是全局 initial_boot_params 的 init-only 包装；参数和评分语义
 * 与 of_fdt_is_compatible() 相同，不取得 blob 或字符串 ownership。
 */
int __init of_flat_dt_is_compatible(unsigned long node, const char *compat)
{
	return of_fdt_is_compatible(initial_boot_params, node, compat);
}

/*
 * of_flat_dt_match - Return true if node matches a list of compatible values
 */
/*
 * 原注释中的“true”实际是兼容性分数：@compat 是 NULL 结尾字符串指针数组，
 * 函数对每个候选取非零最小值，0 表示全部不匹配。数组和字符串均为借用，
 * 启动期只读；选择最小分数保证节点 compatible 列表中更靠前者获胜。
 */
static int __init of_flat_dt_match(unsigned long node, const char *const *compat)
{
	unsigned int tmp, score = 0;

	if (!compat)
		return 0;

	while (*compat) {
		tmp = of_fdt_is_compatible(initial_boot_params, node, *compat);
		if (tmp && (score == 0 || (tmp < score)))
			score = tmp;
		compat++;
	}

	return score;
}

/*
 * of_get_flat_dt_phandle - Given a node in the flat blob, return the phandle
 */
/*
 * 原注释含义：从 initial_boot_params 的 @node 返回规范化 phandle；0 表示
 * 缺失或无有效 phandle。返回标量，不建立 device_node 引用、无副作用。
 */
uint32_t __init of_get_flat_dt_phandle(unsigned long node)
{
	return fdt_get_phandle(initial_boot_params, node);
}

/*
 * of_flat_dt_get_machine_name - 获取用于启动日志的机器型号字符串。
 *
 * 无入参；先借用根节点 model 首字符串，缺失则回退 compatible 首项。
 * 返回指向 FDT 的可空借用指针，调用者不得释放或超出 blob 生命周期保存。
 */
const char * __init of_flat_dt_get_machine_name(void)
{
	const char *name;
	unsigned long dt_root = of_get_flat_dt_root();

	name = fdt_stringlist_get(initial_boot_params, dt_root, "model", 0, NULL);
	if (!name)
		name = fdt_stringlist_get(initial_boot_params, dt_root,
					  "compatible", 0, NULL);
	return name;
}

/**
 * of_flat_dt_match_machine - Iterate match tables to find matching machine.
 *
 * @default_match: A machine specific ptr to return in case of no match.
 * @get_next_compat: callback function to return next compatible match table.
 *
 * Iterate through machine match tables to find the best match for the machine
 * compatible string in the FDT.
 */
/*
 * 补充说明：of_flat_dt_match_machine - 在体系结构 machine 表中选最佳 FDT 匹配。
 *
 * @default_match 是无匹配时返回的借用候选，可空；@get_next_compat 是同步
 * 迭代回调，每次返回候选私有 data 并通过输出参数给出 NULL 结尾 compatible
 * 表，返回 NULL 结束。函数对根节点取最小非零 score；平分时保留先出现项。
 * 成功返回候选 data 借用指针并记录 model；最终仍无候选时打印固件全部
 * compatible 并返回 NULL。无分配、无 ownership 转移，启动期串行。
 */
const void * __init of_flat_dt_match_machine(const void *default_match,
		const void * (*get_next_compat)(const char * const**))
{
	/* best_score 以最大无符号值为哨兵，任何非零真实分数都可替换默认。 */
	const void *data = NULL;
	const void *best_data = default_match;
	const char *const *compat;
	unsigned long dt_root;
	unsigned int best_score = ~1, score = 0;

	dt_root = of_get_flat_dt_root();
	while ((data = get_next_compat(&compat))) {
		/* data 与 compat 仅在本轮借用；best_data 保存稳定的 machine 表指针。 */
		score = of_flat_dt_match(dt_root, compat);
		if (score > 0 && score < best_score) {
			best_data = data;
			best_score = score;
		}
	}
	if (!best_data) {
		/* 失败诊断逐项读取根 compatible，字符串仍由 FDT 持有。 */
		const char *prop;
		int idx = 0, size;

		pr_err("\n unrecognized device tree list:\n[ ");

		while ((prop = fdt_stringlist_get(initial_boot_params, dt_root,
						  "compatible", idx++, &size)))
			pr_err("'%s' ", prop);
		pr_err("]\n\n");
		return NULL;
	}

	pr_info("Machine model: %s\n", of_flat_dt_get_machine_name());

	return best_data;
}

/*
 * __early_init_dt_declare_initrd - 按体系结构能力发布 initrd 虚拟地址。
 *
 * @start/@end 是从 /chosen 解码的物理字节边界，要求 start<=end。通用平台在
 * 此处用 __va() 写 initrd_start/end，并允许 initrd 位于内核起点以下；
 * arm64 与 64-bit RISC-V 的这一阶段尚不能/不应通用转换，改由体系结构稍后
 * 消费 phys_initrd_start/size。返回 void，不保留参数或分配内存。
 */
static void __early_init_dt_declare_initrd(unsigned long start,
					   unsigned long end)
{
	/*
	 * __va() is not yet available this early on some platforms. In that
	 * case, the platform uses phys_initrd_start/phys_initrd_size instead
	 * and does the VA conversion itself.
	 */
	/*
	 * 原注释含义：部分平台此时 __va() 尚不可用，它们只使用物理 initrd
	 * 全局量并自行转换。条件明确排除 arm64 和 64-bit RISC-V。
	 */
	if (!IS_ENABLED(CONFIG_ARM64) &&
	    !(IS_ENABLED(CONFIG_RISCV) && IS_ENABLED(CONFIG_64BIT))) {
		initrd_start = (unsigned long)__va(start);
		initrd_end = (unsigned long)__va(end);
		initrd_below_start_ok = 1;
	}
}

/**
 * early_init_dt_check_for_initrd - Decode initrd location from flat tree
 * @node: reference to node containing initrd location ('chosen')
 */
/*
 * 补充说明：early_init_dt_check_for_initrd - 从 /chosen 发布 initrd 物理区间。
 *
 * @node 是 initial_boot_params 中 chosen 节点 offset。仅配置
 * BLK_DEV_INITRD 时读取 linux,initrd-start/end；property 是 big-endian
 * 整数，长度以字节返回并按 4 转为 cell 数。任一属性缺失或 start>end 时
 * 无副作用返回。成功写 phys_initrd_start/size，并按体系结构选择是否同步
 * 写虚拟 initrd_start/end；不保留 property 指针、不负责 memblock 保留。
 */
static void __init early_init_dt_check_for_initrd(unsigned long node)
{
	/* start/end 是物理字节边界；prop 仅在每次 of_read_number() 前短暂借用。 */
	u64 start, end;
	int len;
	const __be32 *prop;

	/* 配置门与两项属性读取都发生在发布任何 initrd 全局之前。 */
	if (!IS_ENABLED(CONFIG_BLK_DEV_INITRD))
		return;

	pr_debug("Looking for initrd properties... ");

	prop = of_get_flat_dt_prop(node, "linux,initrd-start", &len);
	if (!prop)
		return;
	start = of_read_number(prop, len/4);

	/* start 成功并不构成部分发布；必须继续取得 end 才能形成完整区间。 */
	prop = of_get_flat_dt_prop(node, "linux,initrd-end", &len);
	if (!prop)
		return;
	end = of_read_number(prop, len/4);
	/* 允许空区间 start==end；反向区间无法形成有效 size，直接拒绝。 */
	if (start > end)
		return;

	__early_init_dt_declare_initrd(start, end);
	phys_initrd_start = start;
	phys_initrd_size = end - start;

	pr_debug("initrd_start=0x%llx  initrd_end=0x%llx\n", start, end);
}

/**
 * early_init_dt_check_for_elfcorehdr - Decode elfcorehdr location from flat
 * tree
 * @node: reference to node containing elfcorehdr location ('chosen')
 */
/*
 * 补充说明：early_init_dt_check_for_elfcorehdr - 解码 dump kernel 的 core header。
 *
 * @node 是 chosen offset。仅 CRASH_DUMP 配置读取 linux,elfcorehdr，且属性
 * 必须恰好一个 {address,size} 元组；成功写全局物理地址/字节数，稍后由
 * fdt_reserve_elfcorehdr() 提交 memblock reservation。失败静默保持旧值，
 * 无映射和 ownership 转移。
 */
static void __init early_init_dt_check_for_elfcorehdr(unsigned long node)
{
	if (!IS_ENABLED(CONFIG_CRASH_DUMP))
		return;

	pr_debug("Looking for elfcorehdr property... ");

	if (!of_flat_dt_get_addr_size(node, "linux,elfcorehdr",
				      &elfcorehdr_addr, &elfcorehdr_size))
		return;

	pr_debug("elfcorehdr_start=0x%llx elfcorehdr_size=0x%llx\n",
		 elfcorehdr_addr, elfcorehdr_size);
}

/*
 * early_init_dt_check_for_dmcryptkeys - 提取 crash dump 解密密钥区并擦除 DT 属性。
 *
 * @node 是 chosen offset。仅 CRASH_DM_CRYPT 配置读取 linux,dmcryptkeys 的
 * 起始物理地址（使用根 address cell 宽度），写 dm_crypt_keys_addr。
 * property 缺失时无副作用；成功后原地删除该属性，防止后续普通 OF 树或原始
 * FDT 接口再次暴露敏感位置。fdt_delprop() 结果未传播，且修改会使旧 CRC
 * 失配，从而阻止未同步更新时创建 /sys/firmware/fdt。
 */
static void __init early_init_dt_check_for_dmcryptkeys(unsigned long node)
{
	const char *prop_name = "linux,dmcryptkeys";
	const __be32 *prop;

	if (!IS_ENABLED(CONFIG_CRASH_DM_CRYPT))
		return;

	pr_debug("Looking for dmcryptkeys property... ");

	prop = of_get_flat_dt_prop(node, prop_name, NULL);
	if (!prop)
		return;

	dm_crypt_keys_addr = dt_mem_next_cell(dt_root_addr_cells, &prop);

	/* Property only accessible to crash dump kernel */
	/* 原注释含义：该属性只能由 crash dump kernel 在此阶段消费，随后必须擦除。 */
	fdt_delprop(initial_boot_params, node, prop_name);
}

/*
 * 最近一次早期扫描找到的 /chosen offset。初值用负 libfdt 错误编码存入
 * unsigned long，读取者强转 long 判断；仅 boot CPU 在 __init 阶段写读。
 */
static unsigned long chosen_node_offset = -FDT_ERR_NOTFOUND;

/*
 * The main usage of linux,usable-memory-range is for crash dump kernel.
 * Originally, the number of usable-memory regions is one. Now there may
 * be two regions, low region and high region.
 * To make compatibility with existing user-space and older kdump, the low
 * region is always the last range of linux,usable-memory-range if exist.
 */
/*
 * 原注释含义：linux,usable-memory-range 主要服务 crash kernel；旧接口只有
 * 一个范围，现在最多表达 low/high 两段。为兼容旧用户态与 kdump，若存在
 * low range，它固定放在属性最后。实现最多接收两项：先用第一项 cap 当前
 * memblock，再把后续 low range 加回，从而得到离散可用集合。
 */
#define MAX_USABLE_RANGES		2

/**
 * early_init_dt_check_for_usable_mem_range - Decode usable memory range
 * location from flat tree
 */
/*
 * 补充说明：early_init_dt_check_for_usable_mem_range - 用 chosen 属性裁剪内存。
 *
 * 无入参；依赖 chosen_node_offset 和已建立的 memblock.memory。属性缺失或
 * chosen 不存在时无操作。最多解码两个物理 {base,size} 元组到栈上快照；
 * 首项调用 memblock_cap_memory_range() 删除范围外内存，其余非空项再 add。
 * 返回 void；这是不可由本函数回滚的全局可用内存发布，必须早于普通分配器。
 */
void __init early_init_dt_check_for_usable_mem_range(void)
{
	/* rgn 零初始化让不足两项时后续 size==0 自然终止；len 实际保存 entry 数。 */
	struct memblock_region rgn[MAX_USABLE_RANGES] = {0};
	const __be32 *prop;
	int len, i;
	u64 base, size;
	unsigned long node = chosen_node_offset;

	/* chosen 的负哨兵表示前序扫描未找到属性容器，memblock 尚不改变。 */
	if ((long)node < 0)
		return;

	pr_debug("Looking for usable-memory-range property... ");

	prop = of_flat_dt_get_addr_size_prop(node, "linux,usable-memory-range",
					     &len);
	if (!prop)
		return;

	len = min(len, MAX_USABLE_RANGES);

	/* 解码阶段尚未改变 memblock，所有 tuple 先形成一致的本地快照。 */
	for (i = 0; i < len; i++) {
		of_flat_dt_read_addr_size(prop, i, &base, &size);
		rgn[i].base = base;
		rgn[i].size = size;

		pr_debug("cap_mem_regions[%d]: base=%pa, size=%pa\n",
			 i, &rgn[i].base, &rgn[i].size);
	}

	/*
	 * 提交阶段：cap 首项后再 add 后续项，保留属性表达的离散高/低区间；
	 * 顺序不可交换，否则 cap 会再次删除刚加入的低端区域。
	 */
	memblock_cap_memory_range(rgn[0].base, rgn[0].size);
	for (i = 1; i < MAX_USABLE_RANGES && rgn[i].size; i++)
		memblock_add(rgn[i].base, rgn[i].size);
}

/**
 * early_init_dt_check_kho - Decode info required for kexec handover from DT
 */
/*
 * 补充说明：early_init_dt_check_kho - 向 kexec handover 发布 FDT 与 scratch 区。
 *
 * 无入参；仅 KEXEC_HANDOVER 配置且 chosen 有效时工作。要求 linux,kho-fdt
 * 和 linux,kho-scratch 都各自恰好一个地址/长度元组；任一缺失均不发布部分
 * 状态。成功调用 kho_populate() 交出两个物理区间供 KHO 子系统校验和管理，
 * 本函数不映射或释放它们。
 */
static void __init early_init_dt_check_kho(void)
{
	unsigned long node = chosen_node_offset;
	u64 fdt_start, fdt_size, scratch_start, scratch_size;

	/* 配置关闭或 chosen 缺失时，KHO 子系统完全看不到半初始化状态。 */
	if (!IS_ENABLED(CONFIG_KEXEC_HANDOVER) || (long)node < 0)
		return;

	if (!of_flat_dt_get_addr_size(node, "linux,kho-fdt",
				      &fdt_start, &fdt_size))
		return;

	if (!of_flat_dt_get_addr_size(node, "linux,kho-scratch",
				      &scratch_start, &scratch_size))
		return;

	/* 两组输入均已完整解码，此处是 KHO 可观察的单一提交点。 */
	kho_populate(fdt_start, fdt_size, scratch_start, scratch_size);
}

#ifdef CONFIG_SERIAL_EARLYCON

/*
 * early_init_dt_scan_chosen_stdout - 按 /chosen stdout-path 建立 earlycon。
 *
 * 无入参；借用 initial_boot_params 和链接器 earlycon 匹配表。依次兼容
 * /chosen@0、linux,stdout-path，拆分首个 ':' 后的串口 options，再按路径
 * 找设备节点并尝试 compatible 驱动。返回 0 表示已设置、已存在，或路径所指
 * 节点缺失但已记录告警；-ENOENT 表示 chosen/stdout 属性缺失；-ENODEV 表示
 * 找到节点但无 earlycon 驱动成功。同步启动期执行，不保存 blob 字符串。
 */
int __init early_init_dt_scan_chosen_stdout(void)
{
	/*
	 * 变量地图：p 是完整 stdout-path 借用字符串；q 定位 ':'/NUL；options
	 * 借用冒号后内容；l 在路径解析前变为不含 options 的名称字节数。
	 */
	int offset;
	const char *p, *q, *options = NULL;
	int l;
	const struct earlycon_id *match;
	const void *fdt = initial_boot_params;
	int ret;

	/* 节点定位阶段兼容规范路径与旧 /chosen@0；两者都不存在是明确缺失。 */
	offset = fdt_path_offset(fdt, "/chosen");
	if (offset < 0)
		offset = fdt_path_offset(fdt, "/chosen@0");
	if (offset < 0)
		return -ENOENT;

	/* 属性定位阶段优先标准 stdout-path，再回退 Linux 旧名称。 */
	p = fdt_stringlist_get(fdt, offset, "stdout-path", 0, &l);
	if (!p)
		p = fdt_stringlist_get(fdt, offset, "linux,stdout-path", 0, &l);
	if (!p || !l)
		return -ENOENT;

	q = strchrnul(p, ':');
	if (*q != '\0')
		options = q + 1;
	l = q - p;

	/* Get the node specified by stdout-path */
	/* 原注释含义：只用冒号前路径定位 console 节点，options 单独传给驱动。 */
	offset = fdt_path_offset_namelen(fdt, p, l);
	if (offset < 0) {
		pr_warn("earlycon: stdout-path %.*s not found\n", l, p);
		return 0;
	}

	for (match = __earlycon_table; match < __earlycon_table_end; match++) {
		/* 链接器表空 compatible 槽不可参与 libfdt 匹配。 */
		if (!match->compatible[0])
			continue;

		if (fdt_node_check_compatible(fdt, offset, match->compatible))
			continue;

		ret = of_setup_earlycon(match, offset, options);
		/* EALREADY 与新建成功对调用者都表示无需继续尝试其他驱动。 */
		if (!ret || ret == -EALREADY)
			return 0;
	}
	return -ENODEV;
}
#endif

/*
 * early_init_dt_scan_root - fetch the top level address and size cells
 */
/*
 * 原注释含义：early_init_dt_scan_root - 读取根节点地址/长度 cell 宽度。
 *
 * 无入参；initial_boot_params 必须已校验。根不存在返回 -ENODEV；否则先发布
 * OF 默认值，再用 #size-cells/#address-cells 的单个 big-endian u32 覆盖。
 * 属性缺失会 WARN 但仍返回 0 并保留默认值。结果写 __initdata 全局，决定
 * 此后 reg、initrd、crash 属性的 cell 指针步长和数值解码。
 */
int __init early_init_dt_scan_root(void)
{
	const __be32 *prop;
	const void *fdt = initial_boot_params;
	int node = fdt_path_offset(fdt, "/");

	if (node < 0)
		return -ENODEV;

	dt_root_size_cells = OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
	dt_root_addr_cells = OF_ROOT_NODE_ADDR_CELLS_DEFAULT;

	/* 先建立默认不变量，畸形/缺失属性不会留下未初始化 cell 宽度。 */
	prop = of_get_flat_dt_prop(node, "#size-cells", NULL);
	if (!WARN(!prop, "No '#size-cells' in root node\n"))
		dt_root_size_cells = be32_to_cpup(prop);
	pr_debug("dt_root_size_cells = %x\n", dt_root_size_cells);

	prop = of_get_flat_dt_prop(node, "#address-cells", NULL);
	if (!WARN(!prop, "No '#address-cells' in root node\n"))
		dt_root_addr_cells = be32_to_cpup(prop);
	pr_debug("dt_root_addr_cells = %x\n", dt_root_addr_cells);

	return 0;
}

/*
 * dt_mem_next_cell - 从 big-endian cell 流读取一个整数并推进游标。
 *
 * @s 是要消费的 32-bit cell 数；@cellp 是非 NULL 输入输出借用游标。
 * 成功把 *cellp 前移 s 个 __be32，并返回最多 64-bit 的组合值。无边界检查，
 * 调用者必须先按属性长度验证；s/指针不合法会越界读取。
 */
u64 __init dt_mem_next_cell(int s, const __be32 **cellp)
{
	const __be32 *p = *cellp;

	/* 先保存当前起点，再按 cell 单位发布下一字段位置。 */
	*cellp = p + s;
	return of_read_number(p, s);
}

/*
 * early_init_dt_scan_memory - Look for and parse memory nodes
 */
/*
 * 原注释含义：early_init_dt_scan_memory - 查找可用 memory 节点并登记 memblock。
 *
 * 无入参；遍历根直接子节点，只处理 device_type="memory" 且 status 可用者。
 * 优先采用 linux,usable-memory，缺失时用 reg；每个非零元组交给体系结构
 * early_init_dt_add_memory_arch() 对齐/裁剪并发布。hotpluggable 属性存在时，
 * 还给相同区间设置 memblock HOTPLUG 标志。
 * 返回 1 表示至少尝试添加过一个非零内存元组，0 表示未发现；单项 mark
 * hotplug 失败仅告警，不撤销已添加内存。启动期串行，不保存 property 指针。
 */
int __init early_init_dt_scan_memory(void)
{
	int node, found_memory = 0;
	const void *fdt = initial_boot_params;

	fdt_for_each_subnode(node, fdt, 0) {
		/* type/reg 都是 blob 借用；l 是经过校验的 {addr,size} entry 数。 */
		const char *type = fdt_stringlist_get(fdt, node,
						      "device_type", 0, NULL);
		const __be32 *reg;
		int i, l;
		bool hotpluggable;

		/* We are scanning "memory" nodes only */
		/* 原注释含义：本遍扫描刻意忽略 CPU、reserved-memory 等其他根节点。 */
		if (type == NULL || strcmp(type, "memory") != 0)
			continue;

		if (!of_fdt_device_is_available(fdt, node))
			continue;

		reg = of_flat_dt_get_addr_size_prop(node, "linux,usable-memory", &l);
		/* usable-memory 是固件/启动约束后的子集，存在时优先于物理 reg。 */
		if (reg == NULL)
			reg = of_flat_dt_get_addr_size_prop(node, "reg", &l);
		if (reg == NULL)
			continue;

		/* 只按属性是否存在标记 hotplug；空属性本身即可表达该布尔语义。 */
		hotpluggable = of_get_flat_dt_prop(node, "hotpluggable", NULL);

		pr_debug("memory scan node %s, reg {addr,size} entries %d,\n",
			 fdt_get_name(fdt, node, NULL), l);

		/* 每个 tuple 独立解码和发布，坏的零长度项不阻止同节点其他区间。 */
		for (i = 0; i < l; i++) {
			u64 base, size;

			of_flat_dt_read_addr_size(reg, i, &base, &size);

			if (size == 0)
				continue;
			pr_debug(" - %llx, %llx\n", base, size);

			early_init_dt_add_memory_arch(base, size);

			/* 标志表示存在有效描述，不代表弱架构 hook 一定成功添加全部区间。 */
			found_memory = 1;

			if (!hotpluggable)
				continue;

			if (memblock_mark_hotplug(base, size))
				/* add 已提交，HOTPLUG 元数据失败不适合回滚整段 RAM。 */
				pr_warn("failed to mark hotplug range 0x%llx - 0x%llx\n",
					base, base + size);
		}
	}
	return found_memory;
}

/*
 * early_init_dt_scan_chosen - 提取 /chosen 启动元数据并合成最终内核命令行。
 *
 * @cmdline 是非 NULL、至少 COMMAND_LINE_SIZE 的可写输出缓冲区，ownership
 * 留给调用者。函数兼容 /chosen@0；节点缺失仍进入 CONFIG_CMDLINE 策略。
 * 找到节点后发布 chosen offset，依次处理 initrd、elfcorehdr、dm-crypt keys，
 * 把 rng-seed 混入早期熵池并从 FDT 擦除，再借用 bootargs 复制进 cmdline。
 * 最后按 EXTEND/FORCE/default 配置追加、覆盖或兜底。当前所有路径返回 0；
 * 副作用包括多个启动全局、随机池、FDT 内容/CRC 和命令行。
 */
int __init early_init_dt_scan_chosen(char *cmdline)
{
	/* rng_seed/p 都借用 blob；l 是对应属性字节数，不能跨 FDT 修改长期保存。 */
	int l, node;
	const char *p;
	const void *rng_seed;
	const void *fdt = initial_boot_params;

	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		node = fdt_path_offset(fdt, "/chosen@0");
	if (node < 0)
		/* Handle the cmdline config options even if no /chosen node */
		/* 原注释含义：没有 chosen 也必须执行编译期命令行的 FORCE/default 规则。 */
		goto handle_cmdline;

	/* 发布给 usable-memory 和 KHO 后续阶段；此后负哨兵被有效 offset 替换。 */
	chosen_node_offset = node;

	/* 三个 helper 独立容错，缺少任一属性不阻止其余 chosen 数据处理。 */
	early_init_dt_check_for_initrd(node);
	early_init_dt_check_for_elfcorehdr(node);
	early_init_dt_check_for_dmcryptkeys(node);

	rng_seed = of_get_flat_dt_prop(node, "rng-seed", &l);
	if (rng_seed && l > 0) {
		/* 先把全部 seed 字节交给随机子系统，再破坏 DT 中的可重复读取入口。 */
		add_bootloader_randomness(rng_seed, l);

		/* try to clear seed so it won't be found. */
		/*
		 * 原注释含义：尽力把属性改成 NOP，防止 unflatten/sysfs 或后续扫描者
		 * 再取得同一启动种子；失败未传播，熵已先被消费。
		 */
		fdt_nop_property(initial_boot_params, node, "rng-seed");

		/* update CRC check value */
		/* 原注释含义：主动修改 blob 后同步完整 CRC，使合法擦除不阻止 sysfs 发布。 */
		of_fdt_crc32 = crc32_be(~0, initial_boot_params,
				fdt_totalsize(initial_boot_params));
	}

	/* Retrieve command line */
	/* 原注释含义：bootargs 是外部输入，按属性长度和目标容量较小值截断复制。 */
	p = of_get_flat_dt_prop(node, "bootargs", &l);
	if (p != NULL && l > 0)
		strscpy(cmdline, p, min(l, COMMAND_LINE_SIZE));

handle_cmdline:
	/*
	 * CONFIG_CMDLINE is meant to be a default in case nothing else
	 * managed to set the command line, unless CONFIG_CMDLINE_FORCE
	 * is set in which case we override whatever was found earlier.
	 */
	/*
	 * 原注释含义：CONFIG_CMDLINE 默认只在固件未给参数时兜底；FORCE 明确覆盖
	 * 先前结果。EXTEND 则保留 bootargs 并追加一个空格和编译期字符串。
	 * 所有写入都以 COMMAND_LINE_SIZE 为界，截断由字符串 helper 处理。
	 */
#ifdef CONFIG_CMDLINE
#if defined(CONFIG_CMDLINE_EXTEND)
	strlcat(cmdline, " ", COMMAND_LINE_SIZE);
	strlcat(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#elif defined(CONFIG_CMDLINE_FORCE)
	strscpy(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#else
	/* No arguments from boot loader, use kernel's  cmdl*/
	/* 原注释含义：bootloader 未提供任何字符时才使用内建命令行。 */
	if (!((char *)cmdline)[0])
		strscpy(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#endif
#endif /* CONFIG_CMDLINE */

	pr_debug("Command line is: %s\n", (char *)cmdline);

	return 0;
}

#ifndef MIN_MEMBLOCK_ADDR
/*
 * 体系结构可覆盖的物理内存可接受边界。默认最小值是线性映射起点对应物理
 * 地址，最大值覆盖 phys_addr_t；弱 memory hook 用它裁剪固件 reg 区间。
 */
#define MIN_MEMBLOCK_ADDR	__pa(PAGE_OFFSET)
#endif
#ifndef MAX_MEMBLOCK_ADDR
#define MAX_MEMBLOCK_ADDR	((phys_addr_t)~0)
#endif

/*
 * early_init_dt_add_memory_arch - 默认体系结构 hook，规范化 RAM 后加入 memblock。
 *
 * @base 是物理字节起点，@size 是字节长度。弱符号允许体系结构替换；默认实现
 * 先确保首个向上对齐页仍完整包含在区间，随后首地址向上、末尾向下页对齐，
 * 再与 MIN/MAX_MEMBLOCK_ADDR 取交集，最后调用 memblock_add()。
 * 无完整可接受页时告警并返回；成功无直接返回值，memblock.memory 是可观察
 * 副作用。启动 CPU 串行，不映射 RAM；底层 add 结果未向上传播。
 */
void __init __weak early_init_dt_add_memory_arch(u64 base, u64 size)
{
	const u64 phys_offset = MIN_MEMBLOCK_ADDR;

	/*
	 * 若从 base 到下一页边界都放不下，向上对齐后区间为空；提前拒绝也避免
	 * 后续 size 减法下溢。
	 */
	if (size < PAGE_SIZE - (base & ~PAGE_MASK)) {
		pr_warn("Ignoring memory block 0x%llx - 0x%llx\n",
			base, base + size);
		return;
	}

	if (!PAGE_ALIGNED(base)) {
		/* 去掉不完整首页，保持原半开区间末端不变。 */
		size -= PAGE_SIZE - (base & ~PAGE_MASK);
		base = PAGE_ALIGN(base);
	}
	/* 去掉不完整尾页，memblock 只接收完整页。 */
	size &= PAGE_MASK;

	/* 起点已超过体系结构可寻址上限时，整个区间均不可用。 */
	if (base > MAX_MEMBLOCK_ADDR) {
		pr_warn("Ignoring memory block 0x%llx - 0x%llx\n",
			base, base + size);
		return;
	}

	if (base + size - 1 > MAX_MEMBLOCK_ADDR) {
		/* 尾端越界时保留上限以内前缀，+1 把闭区间上限转回字节长度。 */
		pr_warn("Ignoring memory range 0x%llx - 0x%llx\n",
			((u64)MAX_MEMBLOCK_ADDR) + 1, base + size);
		size = MAX_MEMBLOCK_ADDR - base + 1;
	}

	if (base + size < phys_offset) {
		/* 整段位于最低可映射物理地址以下，没有可加入的交集。 */
		pr_warn("Ignoring memory block 0x%llx - 0x%llx\n",
			base, base + size);
		return;
	}
	if (base < phys_offset) {
		/* 与下限部分重叠时裁掉低端前缀，仍保持原区间末端。 */
		pr_warn("Ignoring memory range 0x%llx - 0x%llx\n",
			base, phys_offset);
		size -= phys_offset - base;
		base = phys_offset;
	}
	/* 发布点：从此早期分配器可在该物理区间分配，保留区会在后续另行摘除。 */
	memblock_add(base, size);
}

/*
 * early_init_dt_alloc_memory_arch - 从 memblock 为展开树分配永久启动内存。
 *
 * @size/@align 单位为字节；用于 copy/unflatten/alias 扫描。返回持有的虚拟
 * 地址且不会 NULL：分配失败由 memblock_alloc_or_panic() 终止启动。内存不在
 * 本文件释放，随内核长期存活并承载全局 OF 树。
 */
static void * __init early_init_dt_alloc_memory_arch(u64 size, u64 align)
{
	return memblock_alloc_or_panic(size, align);
}

/*
 * early_init_dt_verify - 校验 DTB 并发布全局早期扁平树状态。
 *
 * @dt_virt 是 DTB 虚拟借用地址，可空；@dt_phys 是对应物理起点。NULL 或
 * libfdt header 校验失败返回 false，所有全局保持未发布。成功写
 * initial_boot_params/pa，计算 totalsize CRC，并初始化根 cell 宽度，返回
 * true。函数不复制/保留物理内存，调用者稍后必须 reserve 或 copy blob。
 */
bool __init early_init_dt_verify(void *dt_virt, phys_addr_t dt_phys)
{
	if (!dt_virt)
		return false;

	/* check device tree validity */
	/* 原注释含义：发布任何全局指针之前先验证 magic、版本、offset 和大小。 */
	if (fdt_check_header(dt_virt))
		return false;

	/* Setup flat device-tree pointer */
	/*
	 * 原注释含义：这是全局发布点。CRC 覆盖完整 totalsize，pa 保留给不能从
	 * 虚拟地址反推物理地址的体系结构阶段。
	 */
	initial_boot_params = dt_virt;
	initial_boot_params_pa = dt_phys;
	of_fdt_crc32 = crc32_be(~0, initial_boot_params,
				fdt_totalsize(initial_boot_params));

	/* Initialize {size,address}-cells info */
	/* 原注释含义：所有地址/长度属性解码前必须先建立根 cell 宽度。 */
	early_init_dt_scan_root();

	return true;
}


/*
 * early_init_dt_scan_nodes - 按依赖顺序扫描 chosen、memory、裁剪和 KHO。
 *
 * 无入参/返回值；initial_boot_params 已验证、根 cell 已初始化，memblock 可被
 * memory hook 更新。chosen 先提供命令行和全局 offset；memory 建立基础 RAM；
 * usable-memory-range 再裁剪该集合；KHO 最后消费 chosen 物理区间。
 * chosen 扫描异常仅告警并继续，其他 helper 自行容错；副作用均为启动全局。
 */
void __init early_init_dt_scan_nodes(void)
{
	int rc;

	/* Retrieve various information from the /chosen node */
	/* 原注释含义：先取得启动参数和后续 chosen 属性定位信息。 */
	rc = early_init_dt_scan_chosen(boot_command_line);
	if (rc)
		pr_warn("No chosen node found, continuing without\n");

	/* Setup memory, calling early_init_dt_add_memory_arch */
	/* 原注释含义：把 memory reg 通过可覆写的架构 hook 发布给 memblock。 */
	early_init_dt_scan_memory();

	/* Handle linux,usable-memory-range property */
	/* 原注释含义：基础 RAM 建立后再按 crash-kernel 可用范围做全局裁剪。 */
	early_init_dt_check_for_usable_mem_range();

	/* Handle kexec handover */
	/* 原注释含义：最后把 chosen 中两块 KHO 内存交给 handover 子系统。 */
	early_init_dt_check_kho();
}

/*
 * early_init_dt_scan - 校验并完成体系结构早期 FDT 必需信息扫描。
 *
 * @dt_virt/@dt_phys 契约同 verify。校验失败返回 false 且不扫描节点；成功按
 * 固定顺序处理 chosen/memory 等并返回 true。true 表示 DTB 可用且扫描已
 * 执行，不保证每个可选属性存在。blob ownership 仍归固件/体系结构。
 */
bool __init early_init_dt_scan(void *dt_virt, phys_addr_t dt_phys)
{
	bool status;

	status = early_init_dt_verify(dt_virt, dt_phys);
	if (!status)
		return false;

	early_init_dt_scan_nodes();
	return true;
}

/*
 * copy_device_tree - 把可短期 FDT 复制到 memblock 长期内存。
 *
 * @fdt 是已校验 blob 借用地址。按 totalsize 分配并完整 memcpy；对齐取
 * FDT v17 header 大小的向上 2 次幂，满足 libfdt/体系结构访问要求。
 * 返回持有的新 blob；分配 helper 默认 panic，因此正常不返回 NULL，但仍保留
 * 条件复制以支持可替换实现。调用者负责更新 initial_boot_params。
 */
static void *__init copy_device_tree(void *fdt)
{
	int size;
	void *dt;

	size = fdt_totalsize(fdt);
	dt = early_init_dt_alloc_memory_arch(size,
					     roundup_pow_of_two(FDT_V17_SIZE));

	if (dt)
		/* totalsize 覆盖 header、reserve map、structure 与 strings 全部块。 */
		memcpy(dt, fdt, size);

	return dt;
}

/**
 * unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 */
/*
 * 补充说明：unflatten_device_tree - 建立并发布启动期全局 OF 节点树。
 *
 * 无入参；通常借用已 reserve 的 initial_boot_params。先保存静态
 * reserved-memory 描述；若固件无 DT，则校验链接器内建空根的边界并复制。
 * 随后用 memblock allocator 两遍展开到 of_root，扫描 /chosen 与 /aliases
 * 全局指针，最后运行 overlay base unittest。
 * 返回 void；展开失败由底层保持 of_root 不发布，但本包装无错误返回。
 * 节点/property 块和其借用 FDT 均按启动全局生命周期保留。
 */
void __init unflatten_device_tree(void)
{
	void *fdt = initial_boot_params;

	/* Save the statically-placed regions in the reserved_mem array */
	/*
	 * 原注释含义：在节点树展开前，把静态 reserved-memory 信息保存进
	 * reserved_mem 数组；这是晚期初始化设备保留区的输入快照。
	 */
	fdt_scan_reserved_mem_late();

	/* Populate an empty root node when bootloader doesn't provide one */
	/* 原注释含义：没有固件 DT 也构造空 of_root，保证通用 OF API 有根节点。 */
	if (!fdt) {
		fdt = (void *) __dtb_empty_root_begin;
		/* fdt_totalsize() will be used for copy size */
		/*
		 * 原注释含义：copy 将信任 totalsize，必须先证明它没有越过链接器
		 * 提供的内建区间，防止越界读取内核镜像。
		 */
		if (fdt_totalsize(fdt) >
		    __dtb_empty_root_end - __dtb_empty_root_begin) {
			pr_err("invalid size in dtb_empty_root\n");
			return;
		}
		of_fdt_crc32 = crc32_be(~0, fdt, fdt_totalsize(fdt));
		fdt = copy_device_tree(fdt);
	}

	__unflatten_device_tree(fdt, NULL, &of_root,
				early_init_dt_alloc_memory_arch, false);

	/* Get pointer to "/chosen" and "/aliases" nodes for use everywhere */
	/* 原注释含义：树已构造后发布常用节点和 alias 映射，供所有后续 OF 用户读取。 */
	of_alias_scan(early_init_dt_alloc_memory_arch);

	unittest_unflatten_overlay_base();
}

/**
 * unflatten_and_copy_device_tree - copy and create tree of device_nodes from flat blob
 *
 * Copies and unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used. This should only be used when the FDT memory has not been
 * reserved such is the case when the FDT is built-in to the kernel init
 * section. If the FDT memory is reserved already then unflatten_device_tree
 * should be used instead.
 */
/*
 * 补充说明：unflatten_and_copy_device_tree - 先延长 DTB 生命周期再发布 OF 树。
 *
 * 无入参/返回值。适用于 FDT 位于内核 __init 等会回收区域、尚未 memblock
 * reserve 的场景；若 initial_boot_params 存在，先复制并把全局指针改向长期
 * memblock，再复用 unflatten_device_tree()。原注释强调：已保留的 DTB 应
 * 直接调用非 copy 版本，避免额外内存与复制成本。
 */
void __init unflatten_and_copy_device_tree(void)
{
	/* 指针重定向是 ownership/lifetime 边界，后续 property 将借用新 blob。 */
	if (initial_boot_params)
		initial_boot_params = copy_device_tree(initial_boot_params);

	unflatten_device_tree();
}

#ifdef CONFIG_SYSFS
/*
 * of_fdt_raw_init - 校验启动 DT 未意外变化后发布 /sys/firmware/fdt。
 *
 * late_initcall 在 sysfs/firmware_kobj 可用后执行。无 FDT 或 CRC 不匹配时
 * 返回 0，选择不创建文件而非令启动失败；匹配时把只读 admin bin_attribute
 * 的 private/size 指向长期 initial_boot_params，并返回 sysfs 创建结果。
 * 属性对象 __ro_after_init，用户态只读；blob ownership 不转移给 sysfs。
 */
static int __init of_fdt_raw_init(void)
{
	/* 静态属性在本函数配置后冻结，读取回调通过 private 借用完整 blob。 */
	static __ro_after_init BIN_ATTR_SIMPLE_ADMIN_RO(fdt);

	if (!initial_boot_params)
		return 0;

	if (of_fdt_crc32 != crc32_be(~0, initial_boot_params,
				     fdt_totalsize(initial_boot_params))) {
		/*
		 * CRC 失配可能来自意外写入或未同步的敏感属性擦除；拒绝导出避免用户态
		 * 看到与早期验证对象不同的原始树。
		 */
		pr_warn("not creating '/sys/firmware/fdt': CRC check failed\n");
		return 0;
	}
	bin_attr_fdt.private = initial_boot_params;
	bin_attr_fdt.size = fdt_totalsize(initial_boot_params);
	/* sysfs 成功后持有属性注册，不持有/释放 DTB 内存。 */
	return sysfs_create_bin_file(firmware_kobj, &bin_attr_fdt);
}
late_initcall(of_fdt_raw_init);
#endif

#endif /* CONFIG_OF_EARLY_FLATTREE */
