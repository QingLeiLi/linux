// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 ioremap 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。该标识只说明新增中文注释的来源。
 *
 * 【背景：物理地址不能直接当指针】
 * 设备寄存器、PCI BAR、固件共享页等对象由物理地址描述，但 CPU 只能通过当前页表中的
 * 虚拟地址访问它们。更重要的是，ARM64 页表属性决定该访问属于 Device-nGnRE、
 * Device-nGnRnE、Normal Non-Cacheable 或普通缓存内存；属性会影响乱序、合并、推测和
 * cache 一致性。ioremap 的工作不只是“换一个地址”，而是建立具有正确内存类型的映射。
 *
 * 同一物理 RAM 若已通过 linear map 以 Normal Cacheable 属性映射，再用 ioremap 建立
 * Device/不同缓存属性别名，可能违反 ARM 架构的内存类型别名约束，导致不可预测行为或
 * cache 不一致。因此 ARM64 明确拒绝普通 ioremap 从 System RAM 起始 PFN 建立映射；
 * 需要访问 RAM 的调用者应使用 linear map，或通过 memremap() 的受控语义获取地址。
 *
 * 【主调用链】
 *   ioremap()/ioremap_wc()/ioremap_np()/ioremap_prot()
 *     -> __ioremap_prot(phys, size, pgprot)
 *        -> 校验 PHYS_MASK
 *        -> 拒绝从普通 RAM PFN 开始的属性别名
 *        -> 可选 ioremap_prot_hook 调整/拒绝机密计算映射
 *        -> generic_ioremap_prot()
 *           -> 对齐物理区间
 *           -> 分配 VM_IOREMAP 虚拟区域
 *           -> ioremap_page_range() 建页表
 *
 *   setup_arch()
 *     -> early_fixmap_init()           先建立 fixmap 页表
 *     -> early_ioremap_init()
 *        -> early_ioremap_setup()      初始化 boot-time fixmap 槽位元数据
 *
 *   memremap(MEMREMAP_WB)
 *     -> arch_memremap_can_ram_remap()
 *        -> 确认该 PFN 确实属于 ARM64 linear map 可访问 RAM
 *        -> 允许通用层直接返回 __va(offset)，避免创建第二个缓存别名
 *
 * 【职责边界】
 * 本文件负责 ARM64 特有的准入策略和早期入口包装；vmalloc 地址分配、页表回滚以及
 * iounmap/vunmap 生命周期由 mm/ioremap.c 等通用代码负责。这里没有 ARM64 专用 iounmap。
 *
 * 【并发、所有权与方案权衡】
 * ioremap_prot_hook 是只能注册一次、没有注销路径的全局策略回调，注册必须发生在并发
 * ioremap 之前，且回调代码生命周期必须覆盖系统运行期。普通 ioremap 可能分配内存并
 * 睡眠，不适合原子上下文；early_ioremap 使用有限 fixmap 槽位解决分配器未就绪问题。
 * ARM64 前置检查减少危险属性别名并允许 Realm/pKVM 修改共享属性，代价是比通用实现
 * 更严格，调用者不能把 ioremap 当作映射任意物理内存的万能接口。
 */

#include <linux/mm.h>
#include <linux/io.h>

static ioremap_prot_hook_t ioremap_prot_hook;

/*
 * ioremap_prot_hook 是全局、静态存储期的可选策略函数指针。其签名接收物理字节区间和
 * pgprot 输出指针：回调可原地增加 encrypted/decrypted 等平台属性，返回 0 接受映射，
 * 返回非零拒绝。典型注册者是 ARM Realm RSI 或 pKVM MMIO guard。初值 NULL 表示没有
 * 附加策略；对象无锁、无引用计数、无注销接口，依赖早期一次注册后只读使用的协议。
 */

/*
 * arm64_ioremap_prot_hook_register - 安装唯一的 ARM64 ioremap 策略回调
 *
 * 调用关系：机密计算平台初始化在普通设备大量执行 ioremap 前调用；之后每次
 * __ioremap_prot() 在建立页表前调用已注册回调。
 *
 * 入参：@hook 是借用的函数指针，调用者必须保证目标代码此后永久可执行，不能来自会
 * 卸载的短生命周期模块；当前调用者传非 NULL。本函数只保存指针，不取得模块引用。
 *
 * 返回：首次注册返回 0；已有回调时 WARN_ON 并返回 -EBUSY，保持旧回调不变。没有替换
 * 和注销路径，因为两个独立回调若以未知顺序修改同一 pgprot，会使安全策略不可证明。
 *
 * 并发：没有锁或原子发布；契约要求启动期串行注册，且在并发 ioremap 读该指针前完成。
 */
int arm64_ioremap_prot_hook_register(ioremap_prot_hook_t hook)
{
	if (WARN_ON(ioremap_prot_hook))
		return -EBUSY;

	ioremap_prot_hook = hook;
	return 0;
}

/*
 * __ioremap_prot - 校验 ARM64 物理区间和内存属性后建立 I/O 虚拟映射
 *
 * 调用关系：ARM64 的 ioremap、ioremap_wc、ioremap_np、ioremap_prot、ioremap_cache
 * 等包装选择具体 pgprot 后调用；校验通过后交给 generic_ioremap_prot() 完成虚拟区间
 * 分配和页表建立。导出符号允许需要明确属性的内核代码复用该准入路径。
 *
 * 入参：
 *   @phys_addr 目标物理起始地址，单位字节，可含页内偏移；不转移物理资源所有权。
 *   @size      请求覆盖的字节数；零长度和加法回绕最终由通用层拒绝。
 *   @pgprot    目标 ARM64 页表属性，按值传入；本地副本可被安全 hook 原地调整。
 *
 * 变量：last_addr 是包含式区间末地址 phys_addr + size - 1，用于检查最终地址能否由
 * 当前 ARM64 物理地址位宽表示。size==0 时该表达式回绕，后续通用层仍有显式零长度/
 * 回绕检查，不能把本层检查当作完整的区间合法性证明。
 *
 * 返回：成功返回带 __iomem 标注、指向原始页内偏移的虚拟地址；调用者最终以 iounmap()
 * 释放虚拟映射。地址越界、RAM 属性冲突、hook 拒绝、虚拟区间不足或建页表失败均返回
 * NULL；通用层负责回滚已分配 vm_area，本层失败发生在资源分配前，无需清理。
 *
 * 上下文：generic_ioremap_prot() 需要 slab/vmalloc，可能分配和睡眠，不能从原子上下文
 * 调用。hook 也必须遵守同一调用上下文，且不得递归调用 ioremap 形成无限递归。
 */
void __iomem *__ioremap_prot(phys_addr_t phys_addr, size_t size,
			     pgprot_t pgprot)
{
	unsigned long last_addr = phys_addr + size - 1;

	/* Don't allow outside PHYS_MASK */
	/*
	 * 原注释表示任何超出当前 CPU/页表配置可编码物理位宽的末地址都必须拒绝，否则高位
	 * 在 PTE 中截断后可能映射到完全不同的低物理地址。这里只检查包含式末端。
	 */
	if (last_addr & ~PHYS_MASK)
		return NULL;

	/* Don't allow RAM to be mapped. */
	/*
	 * 原注释背后的 ARM64 约束是避免把 linear-map 中的 Normal RAM 再映射成 Device 或
	 * 不同 cache 属性。pfn_is_map_memory() 还会排除整数溢出的伪 PFN，并依据 memblock
	 * 判断起始页是否属于可映射 RAM；WARN_ONCE 帮助定位误用而不让日志持续刷屏。
	 */
	if (WARN_ONCE(pfn_is_map_memory(__phys_to_pfn(phys_addr)),
		      "ioremap attempted on RAM pfn\n"))
		return NULL;

	/*
	 * If a hook is registered (e.g. for confidential computing
	 * purposes), call that now and barf if it fails.
	 */
	/*
	 * 原注释中的 hook 是安全策略提交点：回调看到原始物理区间，并可通过 &pgprot 修改
	 * 本次局部权限副本；非零返回表示平台无法安全共享/保护该区间。unlikely 让未启用
	 * 机密计算的常见路径保持低分支成本，WARN_ON 暴露策略拒绝。
	 */
	if (unlikely(ioremap_prot_hook) &&
	    WARN_ON(ioremap_prot_hook(phys_addr, size, &pgprot))) {
		return NULL;
	}

	/*
	 * 通用层负责页对齐、VM_IOREMAP 地址分配、ioremap_page_range() 和失败回滚；返回值
	 * 会重新加上 phys_addr 的页内 offset，因此调用者访问的第 0 字节仍对应原始物理地址。
	 */
	return generic_ioremap_prot(phys_addr, size, pgprot);
}
EXPORT_SYMBOL(__ioremap_prot);

/*
 * Must be called after early_fixmap_init
 */
/*
 * 原注释给出严格顺序：early_ioremap 用 FIX_BTMAP_* 槽位改写 fixmap PTE，因此必须先由
 * early_fixmap_init() 把静态下级页表接入 init_mm.pgd；颠倒顺序会访问不存在的页表。
 */
/*
 * early_ioremap_init - 初始化 ARM64 启动期临时 I/O 映射槽位
 *
 * 调用关系：setup_arch() 在 early_fixmap_init() 之后调用；本函数只是架构入口，实际由
 * 通用 early_ioremap_setup() 计算每个 boot-time fixmap slot 的固定虚拟基址。
 *
 * 入参/返回：无参数、无返回值；不映射具体物理页、不分配内存。__init 表示正常启动后
 * 包装代码可回收。执行时由启动 CPU 串行调用，不需要锁。
 */
void __init early_ioremap_init(void)
{
	early_ioremap_setup();
}

/*
 * arch_memremap_can_ram_remap - 判断 RAM 请求能否直接复用 ARM64 linear map
 *
 * 调用关系：通用 memremap(MEMREMAP_WB) 在资源属于 System RAM、起始 PFN 有效且非
 * HighMem 后调用；true 时直接返回 __va(offset)，false 时尝试架构 WB 映射 fallback。
 *
 * 入参：@offset 是资源起始物理字节地址；@size 是请求字节数；@flags 是 MEMREMAP_*
 * 策略位。本实现只需确认起始 PFN 是否由 ARM64 memblock linear map 覆盖，因此 size 和
 * flags 有意不参与计算，也不发生所有权转移。
 *
 * 变量/返回：pfn 是由 offset 舍弃页内低位得到的物理页帧号。pfn_is_map_memory() 先
 * 验证 PFN 往返转换不溢出，再查询 memblock；true 表示通用层可以安全使用现有缓存一致
 * linear-map 地址，false 表示不能直接 __va()。函数无副作用、无需锁，也不创建映射。
 */
bool arch_memremap_can_ram_remap(resource_size_t offset, size_t size,
				 unsigned long flags)
{
	unsigned long pfn = PHYS_PFN(offset);

	return pfn_is_map_memory(pfn);
}
