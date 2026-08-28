/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 中文学习注释由 OpenAI Codex（GPT-5，2026-08-27）生成。
 *
 * 文件地图：本头文件连接 DAMON 的地址空间后端与 ops-common.c 中的公共机制。
 * vaddr/paddr 后端先取得带引用的 folio，再借助这里的接口完成访问位采样、
 * DAMOS 冷热评分、folio 属性过滤和 NUMA 迁移；策略配置与区域统计仍由
 * DAMON core 持有，本层不创建 damon_ctx、damon_region 或 damos。
 *
 * 生命周期与并发：damon_get_folio() 是 folio 引用的取得边界，调用者必须
 * folio_put()；页表接口要求调用者稳定相应 PTE/PMD，folio 级接口内部只尝试
 * 取得 folio 锁，竞争时允许保守地跳过本轮采样。迁移接口接管调用者构造的
 * 隔离 folio 链表并在返回前迁移或放回 LRU。这样的尽力而为语义降低监控对
 * 被监控负载的扰动，代价是单轮观测和动作可能因并发变化而漏报或少执行。
 */
/*
 * Common Code for Data Access Monitoring
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/*
 * DAMON（Data Access MONitor）不同地址空间实现共享的代码声明。
 * 作者与许可证信息保持上游原文；这些接口把页表访问状态、folio 状态和
 * DAMOS 策略连接起来，避免 vaddr/paddr 后端各自复制同一套机制。
 */

#include <linux/damon.h>

/*
 * damon_get_folio() - 把 PFN 转成可安全短期使用的在线 LRU folio。
 *
 * 业务背景：paddr 采样、过滤和迁移只有物理页框号；本函数是进入 folio
 * 生命周期的共同入口，并在页下线、复合页转换或 LRU 状态并发变化时拒绝
 * 返回不稳定对象。调用链通常是 DAMON vaddr/paddr 后端 → 本函数 →
 * folio 状态检查，之后由调用者执行采样或动作。
 * 入参：pfn 是待查询的物理页框号，按 PAGE_SIZE 为单位；纯输入，可指向
 * 离线、非 LRU 或正在改变复合页归属的页框，不转移任何所有权。
 * 出参/返回：成功返回 folio 头页的持有引用，调用者必须 folio_put()；
 * NULL 表示页框不在线、引用无法取得，或复查时 folio/LRU 身份已不稳定。
 * 注意事项：返回引用只保证对象内存存活，不冻结映射、LRU 或访问状态；
 * 本函数不持有页锁，调用者若修改需要稳定的 folio 状态仍须使用相应同步。
 */
struct folio *damon_get_folio(unsigned long pfn);

/*
 * damon_ptep_mkold() - 清除一个 PTE 的访问证据并启动下一采样周期。
 *
 * 业务背景：vaddr 页表遍历或 folio 反向映射遍历在采样开始时调用；它同时
 * 清 CPU young 位和 MMU notifier 所代表设备侧访问状态，并把对应 folio 标成
 * idle，使稍后的 damon_folio_young() 能判断区间内是否发生过新访问。
 * 入参：pte 是 addr 对应的叶 PTE 借用指针；vma 是拥有该映射的借用 VMA；
 * addr 是 VMA 内页对齐虚拟地址，单位为字节。三个输入必须描述同一映射，
 * 所有权均不转移。
 * 出参/返回：无直接返回值；可清除页表/MMU-notifier 的 young 状态，并设置
 * folio 的 young/idle 标志；无法取得稳定在线 LRU folio 时不产生 folio 动作。
 * 注意事项：调用者必须按页表遍历协议稳定 PTE（常见路径持有页表锁）；
 * 函数本身不睡眠获取 folio 锁，但会触发体系结构页表和 notifier 操作。
 */
void damon_ptep_mkold(pte_t *pte, struct vm_area_struct *vma, unsigned long addr);

/*
 * damon_pmdp_mkold() - 对透明巨页 PMD 执行与 PTE 版本相同的访问复位。
 *
 * 业务背景：当页表遍历停在 PMD 映射时，DAMON 不能逐 PTE 处理；本接口以
 * HPAGE_PMD_SIZE 范围清 CPU/设备 young 状态，并为覆盖该 PMD 的 folio 建立
 * 下一轮 idle 基线。调用者来自 vaddr 遍历或 folio rmap 路径。
 * 入参：pmd 是 addr 对应 PMD 的借用指针；vma 是所属借用 VMA；addr 是该
 * PMD 映射起始虚拟地址，单位为字节，三者必须对应同一受稳定保护的映射。
 * 出参/返回：无直接返回值；CONFIG_TRANSPARENT_HUGEPAGE 启用时更新访问状态，
 * 关闭时为空操作，不取得也不转移调用者引用。
 * 注意事项：调用者通常持有 PMD 锁；设备侧清除覆盖整个 PMD 范围。配置关闭
 * 时不能把“无返回”误解为已复位访问位，后续也不会从本接口得到采样基线。
 */
void damon_pmdp_mkold(pmd_t *pmd, struct vm_area_struct *vma, unsigned long addr);

/*
 * damon_folio_mkold() - 通过全部反向映射把一个 folio 置为待观察状态。
 *
 * 业务背景：paddr 后端只有 folio，没有现成页表位置；本函数以 rmap 找到每个
 * VMA 映射并委托 PTE/PMD 接口清访问证据。无用户映射或无 mapping 的 folio
 * 可直接设置 idle，不必遍历页表。
 * 入参：folio 是调用者持有引用的借用对象；本函数不消费该引用，调用期间
 * 对象必须存活。
 * 出参/返回：无直接返回值；成功时相应映射进入新采样周期。folio 锁竞争时
 * 直接跳过，调用者不会得到显式失败通知。
 * 注意事项：内部使用 folio_trylock()，以观测精度换取不阻塞业务路径；因此
 * 这是尽力而为操作，调用者不能据此断言所有映射的 young 位都已被清除。
 */
void damon_folio_mkold(struct folio *folio);

/*
 * damon_folio_young() - 汇总 CPU、设备和 idle 状态判断 folio 是否被访问。
 *
 * 业务背景：prepare 阶段由 damon_folio_mkold() 建立基线，check 阶段调用本
 * 函数沿 rmap 查任一映射的新访问；找到一次访问即可提前停止遍历。
 * 入参：folio 是调用者持有引用的借用对象，本函数不取得长期引用也不转移
 * 所有权。
 * 出参/返回：true 表示发现 PTE/PMD young、设备 notifier young，或 folio 已
 * 不再 idle；false 表示未映射 folio 仍 idle、遍历未发现访问，或 folio 锁竞争。
 * 注意事项：false 包含“本轮无法检查”的保守结果，不能当作永久未访问证明；
 * 引用只保证存活，rmap 与页表稳定性由内部遍历和 folio_trylock() 协议提供。
 */
bool damon_folio_young(struct folio *folio);

/*
 * damon_cold_score() - 把区域访问频率和年龄换算为 DAMOS 冷度优先级。
 *
 * 业务背景：PAGEOUT、LRU_DEPRIO 和冷页迁移策略用该分数在配额不足时选择
 * 更值得先处理的区域；它复用 hot score 后取补值。
 * 入参：c 是持有采样/聚合参数的借用上下文；r 是本轮统计区域的借用对象；
 * s 是提供访问频率与年龄权重的借用策略，三者生命周期由 DAMON core 保证。
 * 出参/返回：返回 0..DAMOS_MAX_SCORE；值越大代表按当前权重越冷，无输出参数
 * 且不修改上下文、区域或策略。
 * 注意事项：分数是聚合窗口统计的相对排序依据，不是温度或时间单位；结果
 * 依赖 c->attrs 与 s->quota 权重，只能在同一评分配置下直接比较。
 */
int damon_cold_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s);

/*
 * damon_hot_score() - 计算供 DAMOS 热页优先策略使用的归一化分数。
 *
 * 业务背景：LRU_PRIO 和热页迁移在配额选择阶段调用；实现把区域访问次数与
 * 年龄分别映射为子分数，再按策略 quota 权重合并并截断到统一范围。
 * 入参：c 是借用 DAMON 上下文，提供聚合周期和最大访问次数；r 是借用区域，
 * 提供 nr_accesses 与 age；s 是借用策略，提供两个评分权重，均不转移所有权。
 * 出参/返回：返回 0..DAMOS_MAX_SCORE，值越大代表按当前模型越热；无输出参数
 * 和可观察状态修改。
 * 注意事项：零访问时年龄越大越冷，非零访问时年龄参与热度；两个权重都为
 * 零时保留未加权结果的零值。该启发式分数不承诺未来访问，也不取得锁。
 */
int damon_hot_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s);

/*
 * damos_folio_filter_match() - 判断 folio 是否满足一条 ops 层 DAMOS 过滤器。
 *
 * 业务背景：vaddr/paddr 后端逐 folio 应用策略前调用；它按匿名、active、
 * memcg、young、大小或 unmapped 类型读取相应状态，再结合 filter->matching
 * 得到该过滤器的最终匹配结果。
 * 入参：filter 是策略持有的借用过滤器；folio 是调用者持有引用的借用对象；
 * 两者均不可为 NULL，调用前后所有权不变。
 * 出参/返回：true 表示 folio 当前状态与过滤器期望相符，false 表示不符；
 * YOUNG 类型在命中后还会调用 damon_folio_mkold()，为后续采样重置访问基线。
 * 注意事项：memcg 判断在 RCU 读侧窗口内取得并比较 ID，不把 memcg 指针带出；
 * 状态可在返回后立即变化，且 young 复位受 folio 锁竞争影响，是瞬时观测结果。
 */
bool damos_folio_filter_match(struct damos_filter *filter, struct folio *folio);

/*
 * damon_migrate_pages() - 按来源 NUMA 节点批量迁移一组已隔离 folio。
 *
 * 业务背景：vaddr/paddr 的 MIGRATE_HOT/COLD 动作先把候选 folio 从 LRU 隔离
 * 并串入 folio_list，再由本函数以异步、NOWAIT、忽略 cpuset/mempolicy 的方式
 * 尝试迁往 target_nid，避免 DAMON 自身触发直接回收而放大内存压力。
 * 入参：folio_list 是输入输出链表，元素使用 folio->lru 节点且已从 LRU 隔离；
 * target_nid 是目标 NUMA 节点号，必须处于 [0, MAX_NUMNODES) 且有内存。
 * 出参/返回：返回成功迁移的基础页数量；有效目标下，本函数消费链表并在返回
 * 前把未迁移 folio 放回 LRU。空链表或无效/无内存目标返回 0，链表保持原状。
 * 注意事项：函数可 cond_resched()，只能在可调度上下文调用；迁移是尽力而为，
 * folio 锁竞争、同节点或分配失败都可能少迁移，但不会把失败项遗留为隔离页。
 */
unsigned long damon_migrate_pages(struct list_head *folio_list, int target_nid);

/*
 * damos_ops_has_filter() - 快速判断策略是否含需地址空间后端处理的过滤器。
 *
 * 业务背景：STAT 等动作若没有 ops_filters，无需扫描区域中的每个 folio；
 * vaddr/paddr 后端用本接口建立该快速返回条件。
 * 入参：s 是 DAMON core 持有的借用策略，调用期间其 ops_filters 链表必须按
 * 策略配置/执行协议保持稳定，本函数不取得引用也不修改链表。
 * 出参/返回：存在至少一个 ops 层过滤器返回 true，否则返回 false；无输出
 * 参数和副作用。
 * 注意事项：这里只判断链表是否为空，不验证过滤器类型、顺序或 allow 语义；
 * 后端仍须按顺序调用 damos_folio_filter_match() 并处理默认拒绝策略。
 */
bool damos_ops_has_filter(struct damos *s);
