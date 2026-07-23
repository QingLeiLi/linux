// SPDX-License-Identifier: GPL-2.0
/*
 * arm64 虚拟/物理地址转换的带诊断入口。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 这里不建立映射，只为 memory.h 中的快速算术转换增加使用范围检查。
 * 线性映射地址和内核镜像符号走不同公式，因此必须使用匹配的 API；
 * 代价是一条仅在显式函数入口存在的检查，nodebug 内联版本保留热路径。
 */
#include <linux/bug.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/mmdebug.h>
#include <linux/mm.h>

#include <asm/memory.h>

/*
 * __virt_to_phys - 将线性映射虚拟地址 x 转成物理地址。
 * x 可带 arm64 top-byte tag，检查时先去 tag；非线性/vmalloc/ioremap 地址
 * 会告警，因为它们不能靠固定偏移转换。返回物理字节地址，不获取页面
 * 引用，也不保证该物理页仍在线；WARN 后仍执行转换以保留诊断场景行为。
 */
phys_addr_t __virt_to_phys(unsigned long x)
{
	WARN(!__is_lm_address(__tag_reset(x)),
	     "virt_to_phys used for non-linear address: %p (%pS)\n",
	      (void *)x,
	      (void *)x);

	return __virt_to_phys_nodebug(x);
}
EXPORT_SYMBOL(__virt_to_phys);

/*
 * __phys_addr_symbol - 将内核镜像中的符号虚拟地址转换为装载物理地址。
 * x 必须落在 KERNEL_START..KERNEL_END；该公式处理内核镜像与线性映射
 * 基址差异，不能用于普通 kmalloc/page_address 地址。越界触发 VIRTUAL_BUG，
 * 成功返回物理字节地址且不改变映射或对象所有权。
 */
phys_addr_t __phys_addr_symbol(unsigned long x)
{
	/*
	 * This is bounds checking against the kernel image only.
	 * __pa_symbol should only be used on kernel symbol addresses.
	 */
	/* 检查的是镜像链接/装载边界，不是在 memblock 中验证物理 RAM。 */
	VIRTUAL_BUG_ON(x < (unsigned long) KERNEL_START ||
		       x > (unsigned long) KERNEL_END);
	return __pa_symbol_nodebug(x);
}
EXPORT_SYMBOL(__phys_addr_symbol);
