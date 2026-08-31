# mm 学习注释进度

规则：单文件按 `doc/linux-kernel-source-learning-methodology.md` 第 17 章闭环；只加注释；`[~]` 未闭环时不开始下一文件。恢复任务先读本文件和 `todo/mm-learning-comments-baseline.md`，目标未变更时复用探查结论。

## 筛选

- 2026-08-27：扫描 190 个 `.c/.h`；密度门禁通过 0，未通过 190。
- 暂不重复：已有中文学习注释的 18 个文件（后续仅复审，不作为首轮目标）。
  `backing-dev.c`、`early_ioremap.c`、`filemap.c`、`hugetlb.c`、`hugetlb_cma.c`、`init-mm.c`、
  `memcontrol.c`、`mempolicy.c`、`mm_init.c`、`rmap.c`、`slab_common.c`、`slub.c`、`vmalloc.c`、
  `vmstat.c`、`kasan/hw_tags.c`、`kasan/sw_tags.c`、`kfence/core.c`、`kfence/report.c`。

## 进度

- [x] 当前文件：无

## 固定顺序

1. 闭环 `mm/gup_test.h`。
2. 构建入口：`mm/Makefile` → `mm/Kconfig` → `mm/Kconfig.debug` → `damon/{Makefile,Kconfig}` → `kasan/Makefile` → `kfence/Makefile` → `kmsan/Makefile`。
3. 按基线表序号 2 起处理零中文 `.c/.h`；最后复审标记 `r` 的 18 个文件。
4. 排除 `.DS_Store`、两份 `.kunitconfig`；Rust 测试不套用 C/H 方法论，另行定标。

## 已完成

- [x] `mm/damon/ops-common.h`：13/111/8.538/2（code/中文/density/max_gap）；仅新增 152 行注释。
- [x] `mm/damon/modules-common.c`：24/38/1.583/6；仅新增 59 行注释。
- [x] `mm/gup_test.h`：32/41/1.281/8；仅新增 63 行注释；第 17 章验收、diff/checkpatch 通过。
- [x] `mm/Makefile`：仅新增 44 行注释；Kbuild 条件、复合目标和子目录入口已覆盖，GNU Make 解析通过。
- [x] `mm/Kconfig`：仅新增 250 行注释；全部 config/menuconfig 实体附近均有中文语义，Kconfig allnoconfig 解析通过。
- [x] `mm/Kconfig.debug`：仅新增 45 行注释；全部 config 实体附近均有中文语义，Kconfig allnoconfig 解析通过。
- [x] `mm/damon/Makefile`：仅新增 7 行注释；共享对象与配置组合关系已覆盖，GNU Make 解析通过。
- [x] `mm/damon/Kconfig`：仅新增 26 行注释；全部配置实体均有中文语义，Kconfig allnoconfig 解析通过。
- [x] `mm/kasan/Makefile`：仅新增 21 行注释；运行时递归防护、模式对象和 KUnit 复合目标已覆盖，GNU Make 解析通过。
- [x] `mm/kfence/Makefile`：仅新增 4 行注释；context analysis、核心对象和测试栈约束已覆盖，GNU Make 解析通过。
- [x] `mm/kmsan/Makefile`：仅新增 10 行注释；运行时递归防护、对象 flags 和测试反向插桩已覆盖，GNU Make 解析通过。
- [x] `mm/page_reporting.h`：33/56/1.697/8；仅新增 82 行注释；第 17 章验收、diff/checkpatch 通过。
- [x] `mm/rodata_test.c`：33/29/0.879/8；仅新增 46 行注释；启动期 rodata 内容、写保护与页边界四阶段自检已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/sysfs-common.h`：38/92/2.421/5；仅新增 126 行注释；共享锁、两类 kobject 生命周期及 sysfs/core 转换接口已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/report_hw_tags.c`：38/54/1.421/8；仅新增 72 行注释；HW_TAGS 坏地址、slab 大小、metadata 行与标签输出契约已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mm_slot.h`：40/49/1.225/8；仅新增 61 行注释；KSM/khugepaged 的哈希/扫描双索引、mm 引用与锁边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/debug_page_alloc.c`：40/53/1.325/5；仅新增 71 行注释；启动参数、static key 与 buddy guard page 设置/合并协议已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/pgalloc-track.h`：42/49/1.167/5；仅新增 57 行注释；四级页表分配、修改位累积与架构同步边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_cma.h`：44/84/1.909/6；仅新增 114 行注释；运行期 frozen folio、启动期 CMA bootmem、exclusive 策略及无 CMA 桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/shuffle.h`：44/70/1.591/6；仅新增 96 行注释；static key 门控、node/zone 洗牌、随机插入及关闭配置桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/vma_internal.h`：44/16/0.364/10；仅新增 31 行注释；vma.c 可替换依赖边界、include 分组、maple tree/mmap_lock 与 TLB 协议已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/bootmem_info.c`：46/49/1.065/6；仅新增 65 行注释；启动页 private 编码、额外引用、section 去重登记与最终归还 buddy 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/debug_page_ref.c`：46/38/0.826/6；仅新增 54 行注释；七类 page_ref trace 包装的事后观测、参数语义与 ownership 边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/numa.c`：46/41/0.891/9；仅新增 55 行注释；在线/离线 NODE_DATA 启动分配、跨节点退化和两个 node-0 回退桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/ioremap.c`：52/41/0.788/7；仅新增 56 行注释；物理区间校验、页对齐、VM_IOREMAP 分配、页表失败回滚与 iounmap 配对已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/report_tags.c`：52/47/0.904/5；仅新增 64 行注释；通用 bug 分类、stack ring 写锁快照、对象生命周期边界和尽力诊断限制已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/tests/lazy_mmu_mode_kunit.c`：52/34/0.654/9；仅新增 48 行注释；enable/disable 与 pause/resume 嵌套状态、KUnit 期望及 suite 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/fail_page_alloc.c`：53/34/0.642/10；仅新增 44 行注释；GFP/阶数过滤、FAULT_NOWARN、通用注入器副作用与 debugfs 控制面已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/failslab.c`：53/34/0.642/7；仅新增 46 行注释；bootstrap cache、NOFAIL/reclaim/cache 过滤、-ENOMEM 契约与 debugfs 错误边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/report_sw_tags.c`：61/51/0.836/9；仅新增 69 行注释；首坏 granule、shadow 分配大小、metadata 行、标签与任务栈报告已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_internal.h`：67/84/1.254/10；仅新增 124 行注释；HugeTLB NUMA 轮转、池出入、批量发布、demote 错误与 sysfs/sysctl 配置契约已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/cma.h`：68/65/0.956/8；仅新增 88 行注释；多区间 union 生命周期、bitmap/锁、状态位、sysfs 计账及关闭配置桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/folio-compat.c`：68/65/0.956/6；仅新增 89 行注释；11 个 page-to-folio 兼容包装的引用、锁、dirty/writeback、page-cache 发布与错误折叠已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_vmemmap.h`：68/76/1.118/7；仅新增 119 行注释；HVO 优化/恢复、批量链表、pre-HVO 初始化、容量判断及关闭配置行为已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kfence/kfence.h`：73/75/1.027/7；仅新增 101 行注释；canary、metadata 状态/锁/RCU、地址映射、错误分类与报告处置边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mmzone.c`：75/97/1.293/8；仅新增 128 行注释；online node/zone 双层遍历、zonelist 策略过滤、lruvec 初始化与 CPUPID 原子交换已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/percpu-km.c`：76/81/1.066/5；仅新增 101 行注释；NOMMU 连续 chunk、整体 backing ownership、创建回滚、发布计数及禁止部分回收已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/msync.c`：77/45/0.584/8；仅新增 56 行注释；参数校验、VMA 洞延迟错误、共享文件同步、解锁 I/O 与游标重建已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_poison.c`：79/71/0.899/8；仅新增 90 行注释；static key、free/alloc poison 配对、KASAN 局部映射、限速损坏诊断及架构空桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/vma_exec.c`：79/60/0.759/6；仅新增 80 行注释；exec 临时栈创建/KSM 回滚、VMA 扩缩、页表搬移与 TLB 旧范围释放已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/mseal.c`：82/69/0.841/8；仅新增 89 行注释；无洞预检、不可逆密封、VMA split/merge、幂等与部分成功边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/interval_tree.c`：84/72/0.857/7；仅新增 101 行注释；file/anon 两类增强红黑区间树、同起点插入、端点快照与锁内借用迭代已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/bpf_memcontrol.c`：86/80/0.930/10；仅新增 102 行注释；BPF kfunc verifier 合约、memcg 引用、跨 controller RCU 查找、统计哨兵及 sleepable flush 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/tests/sysfs-kunit.h`：86/48/0.558/9；仅新增 60 行注释与空行；sysfs/core target 追加、PID 临时引用、KUnit skip 回滚及计数断言边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/memcontrol-v1.h`：90/195/2.167/8；仅新增 289 行注释与空行；v1/v2 遍历、stock 冲刷、local/total 统计、soft-limit/OOM、memsw/kmem/tcpmem 及关闭配置桩已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/zpdesc.h`：90/94/1.044/8；仅新增 135 行注释与空行；struct page 重叠布局、页类型/对象偏移共存、转换宏、folio 锁/引用/highmem 映射及迁移标记已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/kmsan.h`：98/132/1.347/9；仅新增 192 行注释与空行；shadow/origin 元数据、递归门禁、嵌套 IRQ/NMI 退避、origin 链、poison/check/report 与启动/vmalloc metadata 绑定已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/shuffle.c`：101/64/0.634/9；仅新增 93 行注释与空行；启动参数/static key、PFN 合法性、近似 Fisher-Yates、migratetype 链表不变量、zone 锁让出及无锁随机位缓存已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kasan/tags.c`：104/35/0.337/9；仅新增 55 行注释；启动参数、static key、memblock 栈环、并发槽仲裁与 Stack Depot 引用交接已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/sysfs-common.c`：108/41/0.380/8；仅新增 54 行注释；共享互斥边界、range kobject 生命周期、端点属性与 memcg 迭代引用已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_frag_cache.c`：111/47/0.423/10；仅新增 69 行注释；编码页、refill 回退、引用偏置、pfmemalloc 淘汰与 backing 释放已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/memtest.c`：112/45/0.402/10；仅新增 67 行注释；花纹收尾、free-range 裁剪、坏段合并保留与 proc 区分语义已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hugetlb_sysctl.c`：115/37/0.322/10；仅新增 50 行注释；默认 hstate、ctl_table 临时副本、mempolicy、surplus 上限与配置分支已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/vma_init.c`：115/98/0.852/9；仅新增 138 行注释与空行；VMA slab、浅拷贝、PFN 跟踪引用、配置桩及 detached 释放边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/cma_sysfs.c`：117/83/0.709/9；仅新增 120 行注释与空行；CMA 原子计账、只读属性、kobject 包装生命周期及启动失败回滚已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/dmapool_test.c`：122/76/0.623/9；仅新增 97 行注释；DMA 地址配对、批量失败回滚、参数化计时、伪设备与模块资源生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/hwpoison-inject.c`：139/94/0.676/10；仅新增 124 行注释；设备/flags/memcg 过滤、竞态复核、debugfs 权限、软件 poison/unpoison 与 RCU 注销已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/fadvise.c`：140/90/0.643/8；仅新增 123 行注释与空行；advice 分派、范围饱和、预读模式、完整页 DONTNEED、LRU 双阶段失效及原生/compat ABI 已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/maccess.c`：145/99/0.683/10；仅新增 133 行注释；内核/用户无故障复制、对齐宽度降级、部分成功、字符串 NUL 契约与 KMSAN/KASAN 插桩边界已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/kmsan/init.c`：152/98/0.645/9；仅新增 134 行注释；早期范围合并、三块 data/shadow/origin 分组、held-back ownership、降阶回收与运行期开关发布已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/percpu-stats.c`：157/70/0.446/9；仅新增 92 行注释；锁外容量分配/锁内复核、bitmap 段重建、碎片排序、chunk 槽状态与 debugfs 快照生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/damon/tests/vaddr-kunit.h`：160/86/0.537/9；仅新增 109 行注释；maple tree fixture、两大空洞三分区、region 复用/裁剪/替换、skip 回滚与 KUnit suite 生命周期已覆盖，第 17 章验收、diff/checkpatch 通过。
- [x] `mm/page_idle.c`：160/83/0.519/9；仅新增 110 行注释与空行；在线 PFN/folio 引用、PTE/PMD/MMU notifier young 清理、reclaim 补偿、位图 ABI 与 sysfs 发布已覆盖，第 17 章验收、diff/checkpatch 通过。

前批 checkpatch 以 `--strict --ignore LONG_LINE` 执行：中文 UTF-8 字节列宽告警被明确豁免，避免破坏中文语义断句；其余结果为 0 errors、0 warnings、0 checks。

本批五文件同样以补丁模式执行 `--strict --ignore LONG_LINE`，结果为 0 errors、0 warnings、0 checks；追加式审计为 521 行新增、0 行删除，新增行仅含注释或空行。仓库无 `.config`，未执行目标编译。

本批第 17 章内容验收记录：按结构位置核对的函数/配置声明为 `14/7/11/18/4`，均有专属函数头或明确组契约，未发现错位；结构体、字段组、全局量、枚举项、局部变量和全部上游英文注释均已逐项复读，原文删除/改写为 0。路径清单覆盖 NUMA 快慢轮转与 demote 回滚、CMA bitmap 认领/释放及可选计账、folio dirty/writeback/page-cache 发布、HVO 单项/批量恢复与关闭配置、KFENCE 地址失败和 report/handler 分离。复述/推理抽查选取 `demote_pool_huge_page()`、`hugetlb_vmemmap_restore_folios()`、`addr_to_metadata()` + `kfence_report_error()`，可由注释恢复入口锁/ownership、发布点、部分失败、配置差异和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`sysfs-kunit.h`、`memcontrol-v1.h`、`zpdesc.h`、`kmsan.h`、`shuffle.c` 合计新增 769 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `86/48/0.558/9`、`90/195/2.167/8`、`90/94/1.044/8`、`98/132/1.347/9`、`101/64/0.634/9`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按结构位置核对的函数/接口契约为 `3/45/13/24/5`（含 CONFIG 分支桩），结构体/字段、static key、参数操作表、per-CPU 状态、枚举/常量、宏与局部变量均有紧邻用途和生命周期说明；全部非豁免英文注释已逐项翻译并补充，`shuffle.c` 旧“新增子区间”表述另以修正说明限定为当前全 zone 实现。路径清单覆盖 KUnit 分配回滚与部分 target 追加、MEMCG_V1 开关/OOM 锁与中性桩、zpdesc 锁/引用/迁移、KMSAN tracked/untracked 与递归退避、zone 随机候选重试/migratetype 拒绝/锁让出。复述与推理抽查选取 `damon_sysfs_test_add_targets()`、`memcg1_oom_prepare()` 两配置契约、`zpdesc_wait_locked()` 引用边界、`kmsan_in_runtime()`/origin 链、`__shuffle_zone()`，均可由注释恢复调用条件、ownership、并发参与者、失败/退化路径和返回后下一步。

本轮五文件聚合验收：`kasan/tags.c`、`damon/sysfs-common.c`、`page_frag_cache.c`、`memtest.c`、`hugetlb_sysctl.c` 合计新增 295 行注释、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `104/35/0.337/9`、`108/41/0.380/8`、`111/47/0.423/10`、`112/45/0.402/10`、`115/37/0.322/10`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按物理结构复核的函数/接口为 `6/8/9/6/6`，均有专属契约或紧邻的明确组契约；全局状态、编码字段、引用偏置、启动参数、sysctl 表项、CONFIG 分支和原有英文注释均逐项复读。路径清单覆盖 KASAN 栈环写者/报告者互斥与 Stack Depot 引用交接、DAMON range kobject/memcg 迭代、page fragment refill/复用/pfmemalloc 淘汰、memtest 花纹/坏段 reservation/proc 区分，以及 HugeTLB persistent/surplus/mempolicy 提交。复述与推理抽查选取 `save_stack_info()`、`damon_sysfs_memcg_path_to_id()`、`__page_frag_alloc_align()`、`memtest()`、`hugetlb_sysctl_handler_common()`，均可由注释恢复入口约束、ownership/锁、状态发布、失败或退化路径和返回后的下一步，结论均为通过。

本轮五文件聚合验收：`vma_init.c`、`cma_sysfs.c`、`dmapool_test.c`、`hwpoison-inject.c`、`fadvise.c` 合计新增 602 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `115/98/0.852/9`、`117/83/0.709/9`、`122/76/0.623/9`、`139/94/0.676/10`、`140/90/0.643/8`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译。

本轮第 17 章内容验收记录：按结构位置核对的函数/配置实现为 `9/11/6/9/6`（含 PFN 跟踪、MEMCG 与架构 ABI 配置分支），结构体/字段、全局状态、属性表、宏、参数和局部变量均有紧邻用途与有效期说明；全部非豁免英文注释已逐项翻译补充。路径清单覆盖 VMA slab/PFN ctx acquire-release、CMA kobject 发布与逆序回滚、DMA pair 部分分配回滚、hwpoison 竞态预筛与页锁内 RCU 复核，以及 DONTNEED 部分页保留/本地 LRU drain/远端重试。复述与推理抽查选取 `vm_area_dup()`、`cma_sysfs_init()`、`dmapool_test_alloc()`、`hwpoison_inject()`、`generic_fadvise()`，均可由注释恢复入口约束、ownership、同步参与者、失败或成功跳过语义、配置差异和返回后的下一步。

本轮五文件聚合验收：`maccess.c`、`kmsan/init.c`、`percpu-stats.c`、`damon/tests/vaddr-kunit.h`、`page_idle.c` 合计新增 578 行注释与空行、删除 0 行；新增非空行全部位于注释，原代码、预处理行及上游注释文本/顺序/缩进未改。密度门禁依次为 `145/99/0.683/10`、`152/98/0.645/9`、`157/70/0.446/9`、`160/86/0.537/9`、`160/83/0.519/9`，均通过；聚合 `git diff --check` 及 `checkpatch --strict --ignore LONG_LINE_COMMENT` 为 0 errors、0 warnings、0 checks。仓库无 `.config`，未执行目标编译和 KUnit 运行。

本轮第 17 章内容验收记录：按结构位置复核的函数/回调为 `9/9/5/8/6`，另核对 nofault 循环宏、KMSAN 启动账本结构、percpu 输出宏、KUnit case/suite 和 page-idle sysfs 属性实体；全部非豁免英文注释均有紧邻翻译与学习补充，vaddr 测试原英文把最终映射终点写作 305 的不一致已保留原文并以修正说明按代码/断言限定为 330。路径清单覆盖 nofault 的 -ERANGE/-EFAULT 与部分复制、KMSAN 三块分组/降阶回收、percpu 锁外扩容重试、DAMON fixture skip/部分更新销毁、page-idle PFN/folio 竞态复核及 PTE/PMD/MMU notifier/reclaim 补偿。复述与推理抽查选取 `copy_from_kernel_nofault()`、`kmsan_memblock_discard()`、`percpu_stats_show()`、`damon_do_test_apply_three_regions()`、`page_idle_bitmap_read()/write()`，均可由注释恢复入口约束、ownership、锁/睡眠边界、失败/退化路径、发布点与返回后的下一步。

## 待决

- [-] `mm/damon/modules-common.h`：语义补注完成；统计宏为不可拆的 11 行续行，门禁 max_gap=11，未改宏凑数。

## 已探查索引

- 基线版本：HEAD `3d4371ed2b62`；主方法论 1474 行已完整读取；密度命令固定为 `--min-density 0.20 --max-code-gap 10`。
- `ops-common.h`：已读完整实现 `ops-common.c`，调用点 `paddr.c`、`vaddr.c`、`core.c`，以及 `include/linux/{damon,migrate}.h`；已核对 folio 引用、young/idle、过滤、评分和迁移链表契约。
- `modules-common.c/.h`：已读 `core.c` 的 ops/context/target 构造销毁，以及 `reclaim.c`、`lru_sort.c` 的全部宏调用和构造调用；已核对 attrs/quota/watermarks/stat 的单位与权限。
- `gup_test.h`：已读 `gup_test.c:1-375`，并在 `tools/testing/selftests/mm/{gup_test.c,gup_longterm.c,cow.c,hmm-tests.c,uffd-unit-tests.c}` 核对 ABI 使用；9 个 ioctl、两组 flags、两结构体字段及 get/pin、fast/basic、长期 pin、dump、read/stop 的输入输出和释放配对已完成注释。
- 构建元数据：`mm/Kconfig` 的配置值流向 `mm/Makefile`，再由 `source "mm/damon/Kconfig"` 和 `obj-$(CONFIG_DAMON) += damon/` 进入 DAMON；下一次从 `mm/damon/Kconfig` 继续，无需重扫本轮五文件。
- 构建元数据第二批：`mm/damon/Kconfig`、`kasan/Makefile`、`kfence/Makefile`、`kmsan/Makefile` 已闭环；下一轮直接进入基线 `.c/.h`，无需重扫构建入口。
- `page_reporting.h`：已读 `mm/page_reporting.c` 的 order 参数、request/notify、process、register/unregister，`mm/page_alloc.c` 的 free-list 摘除与释放通知调用点，以及 `include/linux/page_reporting.h` 的驱动契约；已核对 static key、RCU 设备生命周期、zone 锁下 PageReported 清理和异步批处理边界。关联实现目前缺少中文学习注释，后续建议分别处理 `mm/page_reporting.c` 与 `include/linux/page_reporting.h`，不在本轮扩展修改。
- 本批五文件：`rodata_test.c` 已核对 `init/main.c::mark_readonly`、`mm/maccess.c::copy_to_kernel_nofault` 与 `include/linux/rodata_test.h`；`damon/sysfs-common.h` 已核对 `sysfs-common.c`、`sysfs-schemes.c` 对应实现和 `sysfs.c` 调用点；`kasan/report_hw_tags.c` 已核对 `report.c` 报告组装及 `kasan.h` 标签/粒度定义；`mm_slot.h` 已核对 `ksm.c`、`khugepaged.c` 注册退出与锁/引用配对；`debug_page_alloc.c` 已核对 `mm_init.c` static key 提交、`page_alloc.c` guard 拆分/合并及 `include/linux/mm.h` 热路径包装。关联实现的学习注释覆盖不一，本批只修改五个基线目标。
- 本批三文件：`pgalloc-track.h` 已核对 `include/linux/{pgtable,pgalloc}.h` 的修改位/同步契约，以及 `vmalloc.c`、`memory.c` 的页表下降调用链；`hugetlb_cma.h` 已核对 `hugetlb_cma.c` 全部公开实现、`cma.c` frozen/early reservation helper 和 `hugetlb.c` 分配释放/启动路径；`shuffle.h` 已核对 `shuffle.c` 实现、`page_alloc.c` 高阶释放、`mm_init.c` 启动调用与 `memory_hotplug.c` 上线调用。关联实现不在本批修改范围。
- 本批十文件：`bootmem_info.c` 已核对 `include/linux/bootmem_info.h` 和 `arch/x86/mm/init_64.c` 的登记/释放配对；`numa.c` 已核对 `drivers/base/arch_numa.c`、`mm/numa_memblks.c` 及热插拔/CXL/DAX 调用点；`ioremap.c` 已核对 `include/asm-generic/io.h`、`arch/arm64/mm/ioremap.c` 和 `mm/vmalloc.c` 的 VM_IOREMAP/页表回滚；两个 KASAN 文件已核对 `kasan/{tags,report}.c` 与 `kasan.h` 的报告组装及 stack ring 并发；lazy-MMU KUnit 已核对 `include/linux/{pgtable,sched}.h` 的计数与架构 enter/leave/flush 契约；两个 fault-injection 文件已核对 `lib/fault-inject.c`、`mm/{page_alloc,slub}.c` 的调用与错误语义。关联实现不在本批修改范围。
- 本批五文件：`hugetlb_internal.h` 已核对 `hugetlb.c` 的池出入/批量发布/demote/store 实现以及 `hugetlb_sysfs.c`、`hugetlb_sysctl.c` 的调用与初始化；`cma.h` 已核对 `cma.c` 的激活、分配、回滚和释放，以及 `cma_sysfs.c`/`cma_debug.c` 调用点；`folio-compat.c` 已核对 `filemap.c`、`page-writeback.c`、`swap.c` 及相关公开声明的 folio 契约；`hugetlb_vmemmap.h` 已核对 `hugetlb_vmemmap.c` 的单项/批量优化恢复和 pre-HVO；`kfence.h` 已核对 `kfence/{core,report}.c` 的状态转换、fault/report 和最终处置。`hugetlb.c`、`filemap.c`、`kfence/{core,report}.c` 在读取范围已有中文学习注释但未在本轮全文件复审；其余关联实现在读取范围缺少系统学习注释，建议后续仍按基线顺序分别补注，本批未越界修改。
- `mmzone.c`：已核对 `include/linux/mmzone.h` 的遍历宏/zonelist/lruvec 声明、`mm/mm_init.c::pgdat_init_internals()`、`mm/memcontrol.c::alloc_mem_cgroup_per_node_info()` 及 CPUPID 的调度/fault/迁移调用点；确认裸 pgdat/zone 游标依赖外层热插拔稳定性，lruvec 在发布前初始化，flags 内嵌 CPUPID 用 cmpxchg 保留并发 flag 更新。关联区域除已注释的 `mm_init.c`/`memcontrol.c` 外覆盖不一，本轮未越界修改。
- `percpu-km.c`：已核对 `mm/percpu.c` 的后端接口 include、创建/销毁/地址反查及 balance/reclaim 调用点，并与 `mm/percpu-vm.c` 的逐页映射后端对照；确认 KM chunk 在 create 时全量 populated、depopulate/TLB flush 为空、最终按原 order 整体释放。两个关联实现当前均缺少系统全文件学习注释，建议后续按基线分别处理，本轮未修改。
- `msync.c`：已核对 `fs/sync.c::vfs_fsync_range()` 与 `include/linux/fs.h` 声明，确认 `datasync=1` 通过文件系统 fsync 回调提交数据及必要元数据；并核对 UAPI MS_* 标志与 mm selftests 调用。`fs/sync.c` 与 UAPI 关联区域缺少本任务级系统学习注释，建议按各自主题另行补注，本轮未修改。
- `page_poison.c`：已核对 `include/linux/mm.h` 的 static-key wrappers、`mm/page_alloc.c` 的 free/alloc 配对、`mm/mm_init.c::mem_debugging_and_hardening_init()` 的开关发布以及 Kconfig 语义；确认 poison 是尽力诊断且不隔离损坏页。`mm_init.c` 关联段已有学习注释，其余关联区域覆盖不一，本轮未修改。
- `vma_exec.c`：已核对 `fs/exec.c::bprm_mm_init()/setup_arg_pages()`、`mm/vma.c::vma_expand()`、`mm/mremap.c::move_page_tables()`、`include/linux/ksm.h::ksm_execve()/ksm_exit()` 与 `mm/vma.h` 声明；确认只适用于未提交 exec mm，页表部分移动失败由整 mm teardown 清理。`fs/exec.c` 有部分学习注释，其余关联实现覆盖不一，本轮未修改。
- `mseal.c`：已核对 `mm/vma.h`/`mm/vma.c::vma_modify_flags()` 的“只准备 split/merge、不提交 flags”契约、`mm/{madvise,mremap}.c` 的 sealed 检查、`include/linux/mm.h` 64 位门控以及 `Documentation/userspace-api/mseal.rst` 的部分成功说明；关联源码缺少全文件系统注释，建议按基线另行处理，本轮未修改。
- `interval_tree.c`：已完整核对 `include/linux/interval_tree_generic.h::INTERVAL_TREE_DEFINE` 生成的 insert/remove/search/iterator 算法，并读取 `mm/vma.c` 的 i_mmap 与 anon_vma pre/post update 锁协议、`mm/rmap.c` AVC 生命周期、`mm/mmap.c::dup_mmap()` 同起点插入；这些关联区域学习注释覆盖不一，建议随各自基线目标补齐，本轮未修改。
- `bpf_memcontrol.c`：已核对 `include/linux/memcontrol.h` 与 `mm/memcontrol.c` 的统计验证/输出/flush 实现、`include/linux/btf.h` flags 及 `kernel/bpf/{btf,verifier}.c` 对 common hook 和 KF_* 的处理；`memcontrol.c` 已有部分学习注释，其余 BPF 关联区域覆盖不一，本轮未修改。
- 本轮五文件：`sysfs-kunit.h` 已核对 `mm/damon/sysfs.c` 的 alloc/add-targets 与 `include/kunit/test.h::kunit_skip`；前者读取区域缺少学习注释，后者部分覆盖。`memcontrol-v1.h` 已核对 `mm/memcontrol-v1.c` 的 soft-limit/OOM/memsw/kmem/tcpmem 实现和 `mm/memcontrol.c` 的 stock、统计、私有 ID、reparent helper；前者读取区域缺失、后者部分覆盖，`include/linux/memcontrol.h` 关联声明缺失。`zpdesc.h` 已核对 `mm/zsmalloc.c` 的分配/链/偏移/迁移、`mm/migrate.c` 的 movable_ops 分派及 `include/linux/{mm_types,page-flags,migrate}.h` 布局/标志，读取区域均缺失。`kmsan.h` 已核对 `mm/kmsan/{core,shadow,report,hooks,init}.c` 与 `include/linux/kmsan.h`，读取区域均缺失。`shuffle.c` 已核对 `mm/shuffle.h`（充分）、`mm/{Kconfig,Makefile}`（充分）及 `mm/{page_alloc,memory_hotplug,mm_init}.c` 调用点（部分覆盖）。缺失/部分覆盖区域建议仍按各自基线或主题顺序补注，本轮未越界修改。
- 本轮五文件：`kasan/tags.c` 已核对 `kasan.h` 的栈环/static-key 声明（缺失）、`common.c` 的 alloc/free 调用与 track 设置（部分覆盖）、`report_tags.c` 的写锁反向扫描（充分）以及 `hw_tags.c`/`sw_tags.c` 初始化入口（部分覆盖）。`damon/sysfs-common.c` 已核对 `sysfs-common.h`（充分）、`sysfs.c`/`sysfs-schemes.c` 的 range 构造、锁和 memcg 过滤调用（缺失）。`page_frag_cache.c` 已核对 `include/linux/{page_frag_cache,mm_types_task}.h` 的编码/包装/结构契约（缺失）、`net/core/skbuff.c` 的串行调用（缺失）和 `page_alloc.c::free_frozen_pages()`（部分覆盖）。`memtest.c` 已核对各架构 early 调用、`include/linux/memblock.h` 声明与关闭配置桩、`fs/proc/meminfo.c` 报告入口（均缺失，`arch/arm64/mm/init.c` 入口附近部分覆盖）。`hugetlb_sysctl.c` 已核对 `hugetlb_internal.h`（充分）、`hugetlb.c` 的池调整/初始化（部分覆盖）、`hugetlb_sysfs.c` 对称接口及 `include/linux/hugetlb.h` movable 门禁（缺失）。本轮只修改五个连续基线目标，关联缺失区域留待其各自顺序。
- 本轮五文件：`vma_init.c` 已核对 `mmap.c`/`nommu.c::mmap_init()`、`mmap.c::dup_mmap()`、`vma.c::__split_vma()/remove_vma()`、`memory.c::pfnmap_track_ctx_release()` 及 `include/linux/{mm,mm_types,mmap_lock,mm_inline}.h`（`include/linux/mm.h` 相关 helper 部分覆盖，其余读取区域缺失）；`cma_sysfs.c` 已核对 `cma.c::__cma_alloc_frozen()/__cma_release_frozen()` 与 `cma.h`（前者缺失，后者充分）；`dmapool_test.c` 已核对 `dmapool.c` create/alloc/free/destroy、`drivers/base/core.c::device_register()/device_del()/put_device()` 及 DMA mask helper（读取区域缺失或仅零散覆盖）；`hwpoison-inject.c` 已核对 `memory-failure.c` 的 RCU filter、`memory_failure()` 与 `unpoison_memory()`（缺失）及 hwpoison 文档；`fadvise.c` 已核对 `truncate.c::mapping_try_invalidate()`、`filemap.c::filemap_flush_range()`、`mm_inline.h::vma_has_recency()`、fd cleanup class、`madvise.c`/`readahead.c`/`f2fs/file.c` 调用点与 UAPI（仅 `filemap.c` 相关函数充分，其余读取区域缺失）。关联缺失源码建议继续按基线或对应子系统主题单独补注，本轮未越界修改。
- `maccess.c`：已核对 `include/linux/uaccess.h` 的公开声明和 `__get_kernel_nofault` 包装，以及 `arch/{mips/mm/maccess.c,arm/mm/fault.c}` 的架构地址许可覆盖；确认通用弱钩子只给出默认许可，架构可拒绝用户区、越界或不安全地址。关联声明和两个架构实现读取区域缺少本任务级系统学习注释，建议按对应主题另行补注，本轮未越界修改。
- `kmsan/init.c`：已核对 `mm/kmsan/shadow.c::kmsan_init_alloc_meta_for_range()/kmsan_setup_meta()`、`mm/mm_init.c::memblock_free_pages()/mm_core_init()` 与 `include/linux/kmsan.h` 声明；确认 false 返回把页块转交 KMSAN 暂扣，true 才允许数据块进入 buddy，且运行期开关在任务状态与遗留页收尾后发布。`mm/mm_init.c` 关联段已有学习注释，`shadow.c` 与公开头读取区域分别为缺失和缺失，本轮未越界修改。
- `percpu-stats.c`：已核对 `mm/percpu-internal.h` 的 `pcpu_chunk`/`percpu_stats` 字段、统计更新钩子及 map-bit helper，`include/linux/percpu.h::pcpu_alloc_info`，并定位 `mm/percpu.c` 的 chunk 槽移动/分配调用；确认所有列表、bitmap 和计数读取由 `pcpu_lock` 稳定，vmalloc/vfree 位于锁外。`percpu-internal.h` 与 `percpu.c` 读取区域缺少系统全文件学习注释，公开头读取区域缺失，本轮未越界修改。
- `damon/tests/vaddr-kunit.h`：已核对 `mm/damon/vaddr.c::__damon_va_three_regions()/__damon_va_init_regions()`、`mm/damon/core.c::damon_set_regions()/damon_new_target()/damon_destroy_target()` 与 `include/kunit/test.h::kunit_skip()`；确认测试头由 vaddr.c 在实现后直接 include，skip 会终止 case，set 只借用 ranges 且 target 销毁覆盖部分更新回滚。`vaddr.c`/`core.c` 读取区域缺少系统全文件学习注释，KUnit 头部分覆盖，本轮未越界修改。
- `page_idle.c`：已核对 `Documentation/admin-guide/mm/idle_page_tracking.rst` 用户 ABI/实现协议、`include/linux/page_idle.h` 的 32 位 page_ext flag 实现、`include/linux/page-flags.h` 的 64 位 folio flag 与 `mm/page_ext.c::page_idle_ops`；确认写 1 是 OR 语义、huge folio 只报告 head、清页表 young 后用 folio young 补偿 reclaim。关联头与 page_ext 读取区域缺少系统全文件学习注释，管理文档不属于源码注释目标，本轮未越界修改。
