// SPDX-License-Identifier: GPL-2.0
/*
 * EFI Memory Map 公共映射生命周期学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件只负责把固件已经交接的 EFI Memory Map 物理区间映射到内核虚拟
 * 地址空间，并把映射及描述符元数据安装到全局 efi.memmap；它不向固件获取
 * Memory Map，不校验每个 EFI descriptor，也不负责为 Runtime Services
 * 建立最终虚拟地址映射。
 *
 * 主生命周期：
 *
 *   EFI/FDT/boot params 提供物理地址和描述符格式
 *       -> efi_memmap_init_early()
 *       -> early_memremap() 占用临时 early-mapping/fixmap 资源
 *       -> efi.memmap 发布，早期初始化代码遍历 descriptors
 *       -> efi_memmap_unmap() 归还临时虚拟映射
 *       -> efi_memmap_init_late()
 *       -> memremap(MEMREMAP_WB) 建立运行期持久映射
 *
 * efi.memmap 是一个全局“映射快照”：map/map_end 描述当前虚拟窗口，
 * phys_map 和 descriptor 元数据在 unmap 后仍保留，以便 late 阶段按相同
 * 格式重新映射。unmap 只撤销内核虚拟映射，不释放固件 Memory Map 的物理
 * 存储。
 *
 * 所有入口都带 __init，调用者在启动期串行执行，因而这里没有运行期并发
 * 读写协议；set_bit()/clear_bit() 表达有效状态，不是为并发发布提供的内存
 * 屏障。方案以两次映射换取资源适配：早期无需 vmalloc，后期则不长期占用
 * 稀缺的 early mapping 槽位。
 */
/*
 * Common EFI memory map functions.
 */
/*
 * 本文件提供体系结构共用的 EFI Memory Map 映射、解除映射和 early/late
 * 切换函数；各体系结构仍负责决定何时调用以及如何使用其中的 descriptors。
 */

/* 让本文件的诊断统一带有 "efi: " 前缀，便于在启动日志中识别子系统来源。 */
#define pr_fmt(fmt) "efi: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/efi.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/slab.h>

#include <asm/early_ioremap.h>
#include <asm/efi.h>

/**
 * __efi_memmap_init - Common code for mapping the EFI memory map
 * @data: EFI memory map data
 *
 * This function takes care of figuring out which function to use to
 * map the EFI memory map in efi.memmap based on how far into the boot
 * we are.
 *
 * During bootup EFI_MEMMAP_LATE in data->flags should be clear since we
 * only have access to the early_memremap*() functions as the vmalloc
 * space isn't setup.  Once the kernel is fully booted we can fallback
 * to the more robust memremap*() API.
 *
 * Returns: zero on success, a negative error code on failure.
 */
/*
 * __efi_memmap_init() - 按调用阶段建立映射并整体替换全局映射快照。
 *
 * 调用关系：efi_memmap_init_early()/efi_memmap_init_late() 以及 x86 的
 * 安装路径准备 data 后调用本函数；成功后 for_each_efi_memory_desc()、
 * efi_mem_desc_lookup() 等消费者通过 efi.memmap 读取同一组 descriptors。
 *
 * @data：借用的只读映射描述，指针不可为 NULL。phys_map 是固件 Memory Map
 * 的物理首地址；size 和 desc_size 以字节为单位；desc_version 标识 EFI
 * descriptor ABI；flags 中 EFI_MEMMAP_LATE 决定映射 API。调用者必须保证
 * size/desc_size 合法、desc_size 非零且物理区间在当前阶段仍有效。本函数
 * 不取得 data 的所有权，也不修改其字段。
 *
 * 入口位于内核初始化上下文，不持有 efi.memmap 专用锁。early 分支可在
 * vmalloc 尚不可用时执行；late 分支依赖普通内存映射设施已经建立，不能当作
 * 原子上下文 helper 使用。
 *
 * 成功返回 0：全局 efi.memmap 被完整快照覆盖并设置 EFI_MEMMAP 有效位，
 * 新映射的解除责任转给 efi_memmap_unmap()。映射失败返回 -ENOMEM，尚未
 * 改动全局快照和有效位，调用者无需为本次尝试解除映射。
 */
int __init __efi_memmap_init(struct efi_memory_map_data *data)
{
	/*
	 * 变量地图：
	 *   map      栈上的候选全局快照；发布前仅当前函数可见。
	 *   phys_map 保存物理首地址，既传给映射 helper，也供 %pa 日志按
	 *            phys_addr_t 的宽度打印。
	 */
	struct efi_memory_map map;
	phys_addr_t phys_map;

	phys_map = data->phys_map;

	/*
	 * 阶段 1：根据生命周期选择虚拟映射机制。
	 *
	 * early_memremap() 使用启动早期可用的临时映射资源；MEMREMAP_WB 表示
	 * late 映射采用 write-back cache 属性。两者都只借用物理区间，不改变
	 * descriptor 内容，也不取得物理内存的释放责任。
	 */
	if (data->flags & EFI_MEMMAP_LATE)
		map.map = memremap(phys_map, data->size, MEMREMAP_WB);
	else
		map.map = early_memremap(phys_map, data->size);

	/*
	 * 映射失败是唯一正常错误出口。候选 map 尚未发布，因此不能调用
	 * efi_memmap_unmap()，也不能设置 EFI_MEMMAP；旧全局快照保持原样。
	 */
	if (!map.map) {
		pr_err("Could not map the memory map! phys_map=%pa, size=0x%lx\n",
			&phys_map, data->size);
		return -ENOMEM;
	}

	/*
	 * 阶段 2：组装遍历所需的完整快照。
	 *
	 * nr_map 是 descriptor 数量而不是字节数。map_end 使用 GNU C 对
	 * void * 的字节步进语义，指向映射区间尾后地址。遍历宏仍按固件给出的
	 * desc_size 前进，不能假定它等于内核结构体 sizeof。
	 */
	map.phys_map = data->phys_map;
	map.nr_map = data->size / data->desc_size;
	map.map_end = map.map + data->size;

	/* descriptor ABI 和映射类型与地址窗口一起构成不可拆分的全局快照。 */
	map.desc_version = data->desc_version;
	map.desc_size = data->desc_size;
	map.flags = data->flags;

	/*
	 * 阶段 3：发布有效状态和快照。
	 *
	 * 当前路径处于串行启动阶段，不存在另一个 CPU 依据有效位并发读取半成品
	 * 的场景；因此这里的 set_bit 只是统一的 EFI feature-state 操作，不应
	 * 被理解为一般并发发布协议。返回后消费者必须同时以 EFI_MEMMAP 有效位
	 * 和 efi.memmap 字段为准。
	 */
	set_bit(EFI_MEMMAP, &efi.flags);

	efi.memmap = map;

	return 0;
}

/**
 * efi_memmap_init_early - Map the EFI memory map data structure
 * @data: EFI memory map data
 *
 * Use early_memremap() to map the passed in EFI memory map and assign
 * it to efi.memmap.
 *
 * Returns: zero on success, a negative error code on failure.
 */
/*
 * efi_memmap_init_early() - 强制按 early 模式安装首次 EFI Memory Map。
 *
 * @data：调用者持有的输入输出描述。除 flags 外，各字段含义和约束与
 * __efi_memmap_init() 相同；本函数把 flags 清零，因此调用后 data 也明确
 * 记录“不是 late mapping”。指针和物理内存 ownership 均不转移。
 *
 * 主要由各体系结构 efi_init() 在 vmalloc 建立前调用。入口为串行启动上下文，
 * 不持锁；成功返回 0 并把解除责任交给 efi_memmap_unmap()，失败透传
 * -ENOMEM。若全局状态已经是 late mapping，WARN_ON 报告违反“生命周期不能
 * 从 late 倒退到 early”的调用错误，但警告本身不会中止后续尝试。
 */
int __init efi_memmap_init_early(struct efi_memory_map_data *data)
{
	/* Cannot go backwards */
	/*
	 * 映射生命周期只能 early -> unmap -> late。观察到 LATE 表示调用顺序
	 * 错误；WARN_ON 保留现场供调试，而不是一个可恢复的输入校验分支。
	 */
	WARN_ON(efi.memmap.flags & EFI_MEMMAP_LATE);

	/* 清除所有模式位，确保公共 helper 只能选择 early_memremap()。 */
	data->flags = 0;
	return __efi_memmap_init(data);
}

/*
 * efi_memmap_unmap() - 撤销当前全局 EFI Memory Map 的虚拟映射。
 *
 * 入参：无。调用者包括 EFI 初始化失败清理、体系结构 runtime 映射切换以及
 * 重建 Memory Map 的路径。入口要求启动期串行执行，不持锁；函数本身没有
 * 可报告的返回值。
 *
 * EFI_MEMMAP 未设置时是幂等空操作。有效时按 flags 与建立映射的 helper
 * 精确配对：early_memunmap() 需要原字节长度，memunmap() 解除 late 映射。
 * 随后 map 置 NULL、有效位清除，阻止后续遍历继续使用失效虚拟地址。
 *
 * 返回后 phys_map、desc_size、desc_version、nr_map 和 flags 有意保留；
 * efi_memmap_init_late() 依靠这些元数据重建等价映射。函数不释放物理 Memory
 * Map，也不清空其中 descriptors。
 */
void __init efi_memmap_unmap(void)
{
	/* 未发布映射时没有虚拟资源，也不能根据残留元数据推断需要 unmap。 */
	if (!efi_enabled(EFI_MEMMAP))
		return;

	/*
	 * 阶段 1：与创建路径配对解除映射。early 长度由 descriptor 大小乘数量
	 * 还原；调用者应保证最初 size 是完整 descriptor 序列。
	 */
	if (!(efi.memmap.flags & EFI_MEMMAP_LATE)) {
		/* size 的单位是字节，只在 early_memunmap() 调用期间有效。 */
		unsigned long size;

		size = efi.memmap.desc_size * efi.memmap.nr_map;
		early_memunmap(efi.memmap.map, size);
	} else {
		memunmap(efi.memmap.map);
	}

	/*
	 * 阶段 2：使全局状态失效。先完成实际 unmap，再清指针和 feature bit，
	 * 保证函数返回时不存在“标记无效但映射资源仍占用”的清理缺口。
	 */
	efi.memmap.map = NULL;
	clear_bit(EFI_MEMMAP, &efi.flags);
}

/**
 * efi_memmap_init_late - Map efi.memmap with memremap()
 * @addr: Physical address of the new EFI memory map
 * @size: Size in bytes of the new EFI memory map
 *
 * Setup a mapping of the EFI memory map using ioremap_cache(). This
 * function should only be called once the vmalloc space has been
 * setup and is therefore not suitable for calling during early EFI
 * initialise, e.g. in efi_init(). Additionally, it expects
 * efi_memmap_init_early() to have already been called.
 *
 * The reason there are two EFI memmap initialisation
 * (efi_memmap_init_early() and this late version) is because the
 * early EFI memmap should be explicitly unmapped once EFI
 * initialisation is complete as the fixmap space used to map the EFI
 * memmap (via early_memremap()) is a scarce resource.
 *
 * This late mapping is intended to persist for the duration of
 * runtime so that things like efi_mem_desc_lookup() and
 * efi_mem_attributes() always work.
 *
 * Returns: zero on success, a negative error code on failure.
 */
/*
 * 上述英文说明中的 ioremap_cache() 描述的是持久、可缓存映射的目标语义；
 * 当前实现实际通过 __efi_memmap_init() 调用
 * memremap(..., MEMREMAP_WB)，具体落到直接映射还是体系结构缓存映射由
 * memremap 实现和物理区间类型决定。
 *
 * efi_memmap_init_late() - 利用 early 阶段留下的 ABI 元数据建立持久映射。
 *
 * @addr：新 EFI Memory Map 的物理首地址，按 phys_addr_t 表示，不可为无效
 * 区间。@size：映射总字节数，应由完整 descriptors 组成。两者均为纯输入，
 * 本函数不取得物理内存 ownership。
 *
 * 调用位置通常是体系结构准备 EFI Runtime Services 时：
 * efi_memmap_unmap() 已撤销 early 映射，但保留 desc_size/desc_version，
 * 随后本函数重建供运行期查询长期使用的 efi.memmap。入口依赖 vmalloc/
 * memremap 基础设施可用，处于可执行普通映射分配的初始化上下文，不持锁。
 *
 * 成功返回 0，发布 EFI_MEMMAP_LATE 快照，后续由 efi_memmap_unmap() 负责
 * memunmap；失败返回 -ENOMEM，旧的虚拟映射仍为空，描述符元数据继续保留。
 * 两个 WARN_ON 只诊断重复/乱序调用，不替代调用者的生命周期约束。
 */
int __init efi_memmap_init_late(phys_addr_t addr, unsigned long size)
{
	/*
	 * data 是本次 late 映射的候选描述：地址/长度来自调用者，descriptor
	 * ABI 稍后从已消费的 early 快照继承，避免两个阶段各自宣称不同格式。
	 */
	struct efi_memory_map_data data = {
		.phys_map = addr,
		.size = size,
		.flags = EFI_MEMMAP_LATE,
	};

	/* Did we forget to unmap the early EFI memmap? */
	/*
	 * 正常入口 map 必须为 NULL；非 NULL 表示 early 映射仍占用临时槽位，
	 * 或已有映射会被覆盖而泄漏。WARN_ON 把这种调用方错误暴露出来。
	 */
	WARN_ON(efi.memmap.map);

	/* Were we already called? */
	/*
	 * flags 已带 LATE 表示 late 初始化重复执行；即使 map 因其他路径为空，
	 * 生命周期仍不允许把第二个持久映射当作首次安装。
	 */
	WARN_ON(efi.memmap.flags & EFI_MEMMAP_LATE);

	/*
	 * It makes no sense to allow callers to register different
	 * values for the following fields. Copy them out of the
	 * existing early EFI memmap.
	 */
	/*
	 * descriptor 版本和步长由固件提供，early 阶段已确立；late 阶段只改变
	 * 地址窗口的实现，不应改变同一组数据的 ABI。复制残留元数据也说明
	 * efi_memmap_unmap() 为什么不能把整个 efi.memmap 清零。
	 */
	data.desc_version = efi.memmap.desc_version;
	data.desc_size = efi.memmap.desc_size;

	/* 公共 helper 完成映射、快照发布和错误处理，返回值原样交给调用者。 */
	return __efi_memmap_init(&data);
}
