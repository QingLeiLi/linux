/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HUGETLB_CMA_H
#define _LINUX_HUGETLB_CMA_H

#ifdef CONFIG_CMA
/*
 * hugetlb_cma_free_frozen_folio() - 把 HugeTLB 的 frozen folio 归还所属 CMA 区。
 * 业务背景：hugetlb 彻底拆除 gigantic folio 后，根据 HugetlbCma 标志选择本入口，
 * 与 hugetlb_cma_alloc_frozen_folio() 形成运行期分配/释放配对。
 * 入参：folio 是不可为 NULL 的拥有指针，须来自 HugeTLB CMA、refcount 已为 0，
 * order/nid 仍可从 folio 读取；调用成功后 ownership 交回 CMA。
 * 出参/返回：无直接返回值；释放 2^folio_order 个连续页，归还失败会 WARN_ON_ONCE。
 * 注意事项：不持有 hugetlb_lock 调用；本函数不容忍普通 buddy/非 CMA folio。
 */
void hugetlb_cma_free_frozen_folio(struct folio *folio);
/*
 * hugetlb_cma_alloc_frozen_folio() - 从 HugeTLB CMA 区分配 frozen compound folio。
 * 业务背景：运行期创建 gigantic HugeTLB 页时先尝试 CMA，以减少长期碎片导致的
 * 连续分配失败；失败后是否退化到 alloc_contig 由 exclusive 策略决定。
 * 入参：order 是以页为单位的二次幂阶数；gfp_mask 中本实现只用 __GFP_THISNODE
 * 控制是否跨节点；nid 是首选有效节点；nodemask 是允许节点的借用掩码，允许回退
 * 时不可为 NULL。所有输入 ownership 不转移。
 * 出参/返回：成功返回 refcount 冻结、带 HugetlbCma 标志的 folio 拥有指针；没有
 * CMA 预留或连续块分配失败返回 NULL，未留下部分分配。
 * 注意事项：CMA 分配使用 GFP_KERNEL，可睡眠；调用者须完成 HugeTLB 初始化，失败
 * 时依据 hugetlb_cma_exclusive_alloc() 决定能否尝试非 CMA 连续分配。
 */
struct folio *hugetlb_cma_alloc_frozen_folio(int order, gfp_t gfp_mask,
				      int nid, nodemask_t *nodemask);
/*
 * hugetlb_cma_alloc_bootmem() - 启动早期从 CMA 预留区切出一个 gigantic 页记录。
 * 业务背景：架构没有专用 gigantic bootmem allocator 且启用 CMA-only 时，
 * alloc_bootmem() 用它在 CMA 激活前预留连续物理内存，稍后汇入 HugeTLB 池。
 * 入参：h 是目标 hstate 的借用指针，决定字节大小；nid 是不可为 NULL 的输入输出
 * 节点指针，进入为首选节点、跨节点成功时改为实际节点；node_exact 禁止跨节点回退。
 * 出参/返回：成功返回位于预留内存中的 huge_bootmem_page 拥有指针，已设置 CMA 标志
 * 和 cma 来源；失败返回 NULL，nid 除成功回退外保持不变。
 * 注意事项：仅 __init 阶段、CMA 尚未激活时可用；调用者继续设置 hstate/list 并负责
 * 后续 memmap 初始化，普通运行期不得保存或调用该接口。
 */
struct huge_bootmem_page *hugetlb_cma_alloc_bootmem(struct hstate *h, int *nid,
						    bool node_exact);
/*
 * hugetlb_cma_exclusive_alloc() - 查询 gigantic 页是否只能从 HugeTLB CMA 分配。
 * 业务背景：CMA 尝试失败后，hugetlb 分配路径据此决定是否禁止 alloc_contig 回退。
 * 入参：无。
 * 出参/返回：返回已验证的 hugetlb_cma_only 策略；true 禁止非 CMA 回退，false 允许。
 * 注意事项：只读 __ro_after_init 状态、不睡眠且不取锁；不会证明当前 CMA 尚有空间。
 */
bool hugetlb_cma_exclusive_alloc(void);
/*
 * hugetlb_cma_total_size() - 返回启动期成功保留的 HugeTLB CMA 总字节数。
 * 业务背景：HugeTLB 初始化用它判断 CMA 分配是否可用，并调整启动分配和 demote 策略。
 * 入参：无。
 * 出参/返回：返回所有节点预留量之和，单位字节；0 表示没有可用 HugeTLB CMA 区。
 * 注意事项：实现位于 __init 段，只能在启动初始化调用；值是容量而非当前空闲量。
 */
unsigned long hugetlb_cma_total_size(void);
/*
 * hugetlb_cma_validate_params() - 在解析完 HugeTLB 参数后修正无效 CMA-only 策略。
 * 业务背景：预留总量为 0 时若仍保持 only=true，gigantic 页会既无 CMA 又禁止回退。
 * 入参：无。
 * 出参/返回：无直接返回值；无预留时把 hugetlb_cma_only 清为 false，其他状态不变。
 * 注意事项：仅启动参数串行解析阶段调用、不睡眠；必须早于依据 exclusive 的分配。
 */
void hugetlb_cma_validate_params(void);
/*
 * hugetlb_early_cma() - 判断某 hstate 是否应走启动期 CMA gigantic 分配。
 * 业务背景：alloc_bootmem() 在 memblock/架构分配器和 CMA early reservation 之间分派。
 * 入参：h 是不可为 NULL 的只读借用 hstate，调用期间有效，ownership 不改变。
 * 出参/返回：仅“架构无专用 huge bootmem 分配、h 为 gigantic、CMA-only 开启”时
 * 返回 true；其他情况返回 false。
 * 注意事项：仅 __init 阶段查询；true 仍不保证具体节点 CMA 有足够连续空间。
 */
bool hugetlb_early_cma(struct hstate *h);
#else
/*
 * CONFIG_CMA=n 时以下内联桩保留调用点的统一接口：分配/策略查询返回中性失败值，
 * 释放和校验成为空操作，因此不会产生隐藏的内存、引用、锁或日志副作用。
 */
/*
 * hugetlb_cma_free_frozen_folio() - 无 CMA 配置下的空释放桩。
 * 业务背景：让条件无关的 HugeTLB 代码可编译；该配置不应产生 HugetlbCma folio。
 * 入参：folio 是未使用的借用指针，不得依赖本桩释放它。
 * 出参/返回：无直接返回值和副作用，ownership 不改变。
 * 注意事项：若真实 CMA folio 到达此处说明配置/状态不变量已被破坏。
 */
static inline void hugetlb_cma_free_frozen_folio(struct folio *folio)
{
}

/*
 * hugetlb_cma_alloc_frozen_folio() - 无 CMA 配置下报告运行期分配不可用。
 * 业务背景：上层据 NULL 与 exclusive=false 可继续尝试普通连续分配。
 * 入参：order、gfp_mask、nid、nodemask 均仅保持统一签名，不读取、不转移所有权。
 * 出参/返回：始终返回 NULL，无输出参数和部分分配。
 * 注意事项：纯内联、不睡眠、不取锁；调用者不得把 NULL 解释成永久 HugeTLB 失败。
 */
static inline struct folio *hugetlb_cma_alloc_frozen_folio(int order,
		gfp_t gfp_mask,	int nid, nodemask_t *nodemask)
{
	return NULL;
}

/*
 * hugetlb_cma_alloc_bootmem() - 无 CMA 配置下报告 early reservation 不可用。
 * 业务背景：alloc_bootmem() 将继续选择架构或 memblock 启动分配路径。
 * 入参：h、nid、node_exact 只为接口兼容；nid 不会被写回，所有权不变。
 * 出参/返回：始终返回 NULL，无预留和全局状态副作用。
 * 注意事项：纯 __init 调用链的内联桩，不访问可能为空的参数。
 */
static inline
struct huge_bootmem_page *hugetlb_cma_alloc_bootmem(struct hstate *h, int *nid,
						    bool node_exact)
{
	return NULL;
}

/*
 * hugetlb_cma_exclusive_alloc() - 无 CMA 时声明分配并非 CMA-only。
 * 业务背景：允许上层在 CMA 路径缺席时采用普通 gigantic 连续分配。
 * 入参：无。
 * 出参/返回：恒为 false，无副作用。
 * 注意事项：结果描述编译配置，不代表普通连续分配一定成功。
 */
static inline bool hugetlb_cma_exclusive_alloc(void)
{
	return false;
}

/*
 * hugetlb_cma_total_size() - 无 CMA 时提供零容量。
 * 业务背景：让 HugeTLB 初始化跳过所有依赖 CMA 预留的分支。
 * 入参：无。
 * 出参/返回：恒为 0 字节，无副作用。
 * 注意事项：这是配置事实，不是一次会随时间变化的空闲量查询。
 */
static inline unsigned long hugetlb_cma_total_size(void)
{
	return 0;
}

/*
 * hugetlb_cma_validate_params() - 无 CMA 时的空参数校验桩。
 * 业务背景：HugeTLB 参数解析可无条件调用统一接口。
 * 入参：无。
 * 出参/返回：无直接返回值、无全局状态副作用。
 * 注意事项：编译配置已经排除 CMA 参数，无需运行期修正。
 */
static inline void hugetlb_cma_validate_params(void)
{
}

/*
 * hugetlb_early_cma() - 无 CMA 时禁止选择 early CMA 路径。
 * 业务背景：alloc_bootmem() 据 false 转向架构或 memblock 分配。
 * 入参：h 是未读取的借用指针，ownership 不改变。
 * 出参/返回：恒为 false，无输出和副作用。
 * 注意事项：不检查 h 是否 gigantic；配置本身已使 CMA 分支不可达。
 */
static inline bool hugetlb_early_cma(struct hstate *h)
{
	return false;
}
#endif
#endif
