// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 KASLR 状态确认与发布学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。该标识只说明新增中文注释的来源，
 * 不属于上游作者或版权信息。
 *
 * 【背景与要解决的问题】
 * KASLR（Kernel Address Space Layout Randomization）让每次启动时的内核虚拟基址
 * 发生变化，使攻击者不能稳定预测内核代码、只读数据和全局对象的位置。它增加的是
 * 地址猜测成本，不替代页权限、PAN、BTI、PAC、CFI 等防护；种子缺失或用户显式传入
 * nokaslr 时，内核必须诚实报告“未启用”，不能仅凭镜像地址非零偏移作出误判。
 *
 * 【职责边界】
 * 本文件不是 ARM64 KASLR 随机偏移的计算和映射实现。真正的早期过程发生在位置无关
 * 的启动代码中：
 *
 *   early_map_kernel()
 *     -> init_feature_override()       提前解析 nokaslr 等软件特性覆盖项
 *     -> kaslr_early_init()
 *          -> 从 DT /chosen/kaslr-seed 取种子，必要时尝试 RNDR
 *          -> 将种子映射为高位随机偏移；失败或禁用时返回 0
 *     -> 低于 MIN_KIMG_ALIGN 的位取自镜像物理放置地址
 *     -> map_kernel() 按最终偏移建立早期内核页表
 *
 * 本文件位于上述映射已经生效之后，只完成状态确认和运行期发布：
 *
 *   start_kernel() -> setup_arch() -> kaslr_init()
 *                                  -> __kaslr_is_enabled = true/false
 *   后续代码       -> kaslr_enabled() 读取这个最终布尔结论
 *
 * 【关键表示】
 * kaslr_offset() = &_text - KIMAGE_VADDR，单位是字节，表示本次内核映像相对默认虚拟
 * 基址的位移。ARM64 为保留早期 2 MiB block mapping 能力，把偏移低于
 * MIN_KIMG_ALIGN（2 MiB）的部分取自镜像物理放置位置，而把其余高位取自随机种子。
 * 因此“最终偏移小于 MIN_KIMG_ALIGN”可作为“没有随机种子贡献高位”的判据。
 *
 * 【状态生命周期与并发】
 * __kaslr_is_enabled 在静态初始化时为 false，只允许启动 CPU 的 __init 路径在
 * kaslr_init() 中把它单向置为 true；完成内核初始化后 __ro_after_init 使所在内存只读。
 * 运行期读者无需锁，因为状态在 SMP 正常运行前已经确定，之后不会再变化。
 *
 * 【方案优劣】
 * 用一个最终布尔状态把复杂的早期随机化过程与运行期查询解耦，读取成本很低，也避免
 * 后续代码重复解释偏移来源。代价是布尔值只能回答“是否真正随机化”，不能表达种子
 * 来源、熵质量或具体偏移；需要具体位移的诊断代码仍应调用 kaslr_offset()。
 */
/*
 * Copyright (C) 2016 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#include <linux/cache.h>
#include <linux/init.h>
#include <linux/printk.h>

#include <asm/cpufeature.h>
#include <asm/memory.h>

bool __ro_after_init __kaslr_is_enabled = false;

/*
 * __kaslr_is_enabled 是 ARM64 运行期的 KASLR 最终状态：false 同时覆盖配置/命令行禁用、
 * 种子缺失以及 kaslr_init() 尚未执行；true 只在确认存在至少 MIN_KIMG_ALIGN 的随机
 * 位移后发布。它不是偏移量，也不持有种子。全局可见是为了让 memory.h 中极轻量的
 * kaslr_enabled() 内联查询无需函数调用；__ro_after_init 防止初始化结束后被意外改写。
 */

/*
 * 修正说明：上文“配置禁用”需要区分编译期与本对象的运行期状态。kaslr.o 只在
 * CONFIG_RANDOMIZE_BASE=y 时链接，因此配置关闭时 __kaslr_is_enabled 根本不存在，
 * <asm/memory.h> 中的 kaslr_enabled() stub 直接返回 false。本变量自身的 false 状态
 * 实际覆盖的是：kaslr_init() 尚未执行、nokaslr 禁用或没有可用随机种子。
 */

/*
 * kaslr_init - 核验早期映射结果并发布“本次启动是否启用 KASLR”
 *
 * 调用关系：由 ARM64 setup_arch() 在早期页表、命令行软件特性覆盖和最终 &_text 地址
 * 已经确定后调用；后续 kaslr_enabled() 以及依赖该策略的架构代码读取发布结果。
 *
 * 入参/返回：无参数、无返回值。可观察输出只有 __kaslr_is_enabled；失败回退保持其
 * 静态初值 false，并输出原因。函数带 __init，初始化结束后代码存储可被回收。
 *
 * 前置条件：只由启动 CPU 在单线程架构初始化阶段调用，不需要锁，也不能在运行期重新
 * 启用 KASLR——此时页表和内核地址早已选定，修改布尔值并不能搬迁已经运行的内核。
 *
 * 核心机制：先尊重 nokaslr，再用 kaslr_offset() 是否至少达到 MIN_KIMG_ALIGN 判断
 * 随机种子是否贡献了高位。顺序体现策略优先级：即使固件提供了种子，用户显式禁用也
 * 必须获胜；种子缺失则降级到固定/物理放置决定的地址，而不是伪称具备随机化保护。
 */
void __init kaslr_init(void)
{
	/*
	 * kaslr_disabled_cmdline() 读取启动早期已解析的 ARM64 软件特性覆盖位。它不在这里
	 * 重新扫描 bootargs，因此必须晚于 early_map_kernel() 中的 init_feature_override()。
	 */
	if (kaslr_disabled_cmdline()) {
		pr_info("KASLR disabled on command line\n");
		return;
	}

	/*
	 * The KASLR offset modulo MIN_KIMG_ALIGN is taken from the physical
	 * placement of the image rather than from the seed, so a displacement
	 * of less than MIN_KIMG_ALIGN means that no seed was provided.
	 */
	/*
	 * 原注释描述的是 ARM64 特有的偏移编码不变量：低 21 位需要匹配物理放置，以便早期
	 * 映射继续使用 2 MiB block descriptor；随机种子只填充按 2 MiB 对齐的高位。因此
	 * kaslr_offset() 即使非零，只要仍小于 2 MiB，也只能说明物理镜像未按默认边界放置，
	 * 不能证明获得了随机种子。这里的比较单位是字节。
	 */
	if (kaslr_offset() < MIN_KIMG_ALIGN) {
		pr_warn("KASLR disabled due to lack of seed\n");
		return;
	}

	/*
	 * 到达这里说明命令行未禁用且偏移含有种子贡献。先记录日志，再把唯一状态从 false
	 * 单向发布为 true；此后初始化收尾会把 __ro_after_init 区域设为只读。
	 */
	pr_info("KASLR enabled\n");
	__kaslr_is_enabled = true;
}

/*
 * parse_nokaslr - 为通用 early_param 框架登记 nokaslr 的已消费占位回调
 *
 * 调用关系：early_param("nokaslr", ...) 把本函数放入早期参数表；通用参数扫描遇到
 * nokaslr 时调用它。但 ARM64 必须在普通 C 启动流程之前决定内核映射位置，所以真正的
 * 语义处理已经由位置无关的 early cpufeature/idreg override 代码完成。
 *
 * 入参：@unused 是 early_param 传入的可选“=值”字符串借用指针；裸 nokaslr 通常没有
 * 值，可为 NULL。本函数有意不读取、不保存它，也不转移所有权。
 *
 * 返回：固定 0，向通用参数解析器表示该参数已被识别且处理成功，避免稍后出现未知参数
 * 行为。函数不修改 KASLR 状态；__init 表示参数解析结束后其代码可回收。
 */
static int __init parse_nokaslr(char *unused)
{
	/* nokaslr param handling is done by early cpufeature code */
	/*
	 * 原注释强调真正处理点不在此处：早期代码把 nokaslr 编码为
	 * ARM64_SW_FEATURE_OVERRIDE_NOKASLR，kaslr_disabled_cmdline() 只读取该既定结果。
	 */
	return 0;
}

/*
 * early_param 在专用链接段中生成静态参数描述项，把字符串 "nokaslr" 与回调关联；
 * 它不是运行期注册，也不会分配对象。描述项只在启动参数解析阶段有效。
 */
early_param("nokaslr", parse_nokaslr);
